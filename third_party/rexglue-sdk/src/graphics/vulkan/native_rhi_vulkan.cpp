// Vulkan implementation of the native-render RHI. Built on the Vulkan
// command processor's deferred command buffer, barrier queue and submission
// counters. The D3D12 backend is a passthrough; this backend owns all the
// translation:
//  - explicit resource states -> (stage mask, access mask, image layout),
//    with the current layout tracked per texture (first-use transitions come
//    from UNDEFINED and discard, exactly what D3D12 initial states allowed);
//  - render targets -> classic render pass + framebuffer caches with lazy
//    begin at the first draw and end on target changes / copies / barrier
//    flushes (no dynamic-rendering dependency - one tested path);
//  - the binding model -> the frozen Vulkan set/binding plan:
//    set 0 = buffer params in
//    param order as dynamic-offset UBO/SSBO descriptors + immutable
//    samplers, sets 1..N = one per texture-table param;
//  - 32-bit root constants -> a CPU shadow block copied per draw into a
//    256-aligned slice of an internal per-frame ring UBO bound with a
//    dynamic offset;
//  - D3D top-left raster space -> negative-height viewport (core 1.1
//    maintenance1 behavior, the backend targets 1.3) with front face
//    CLOCKWISE, so the geometry->pixel mapping and screen-space winding
//    match D3D12 with unchanged shaders.
//
// Threading: Create*/Map/Unmap/InvalidateForRead/DestroyDeferred are
// thread-safe (VMA in thread-safe mode + a mutex over the backend's own
// bookkeeping); views, binding layouts, pipelines, shaders and all Cmd
// methods are render-thread-only per the interface contract (no locks
// taken on the caches they touch).

#include <rex/graphics/vulkan/native_rhi_vulkan.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include <rex/graphics/vulkan/command_processor.h>
#include <rex/logging.h>
#include <rex/ui/vulkan/mem_alloc.h>
#include <rex/ui/vulkan/presenter.h>
#include <rex/ui/vulkan/util.h>

namespace rex::graphics::vulkan {
namespace {

using nrhi::Backend;
using nrhi::Format;
using nrhi::HeapKind;
using nrhi::ResourceState;
using nrhi::TextureKind;

// ---------------------------------------------------------------------------
// Format / state / enum translation.
// ---------------------------------------------------------------------------

VkFormat ToVkFormat(Format format) {
  switch (format) {
    case Format::kR8G8B8A8_UNORM:
      return VK_FORMAT_R8G8B8A8_UNORM;
    case Format::kR8G8B8A8_UINT:
      return VK_FORMAT_R8G8B8A8_UINT;
    case Format::kR8_UNORM:
      return VK_FORMAT_R8_UNORM;
    case Format::kR8G8_UNORM:
      return VK_FORMAT_R8G8_UNORM;
    case Format::kB5G6R5_UNORM:
      // Same bit layout as DXGI_FORMAT_B5G6R5_UNORM (G in the middle, R in
      // the top bits of the packed 16).
      return VK_FORMAT_R5G6B5_UNORM_PACK16;
    case Format::kR16G16_UNORM:
      return VK_FORMAT_R16G16_UNORM;
    case Format::kR10G10B10A2_UNORM:
      // Identical bit layout to DXGI_FORMAT_R10G10B10A2_UNORM.
      return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    case Format::kR16G16B16A16_FLOAT:
      return VK_FORMAT_R16G16B16A16_SFLOAT;
    case Format::kR11G11B10_FLOAT:
      // Identical bit layout to DXGI_FORMAT_R11G11B10_FLOAT (R in the low
      // bits of the packed 32).
      return VK_FORMAT_B10G11R11_UFLOAT_PACK32;
    case Format::kR32_FLOAT:
      return VK_FORMAT_R32_SFLOAT;
    case Format::kR32G32_FLOAT:
      return VK_FORMAT_R32G32_SFLOAT;
    case Format::kR32G32B32_FLOAT:
      return VK_FORMAT_R32G32B32_SFLOAT;
    case Format::kR32G32B32A32_FLOAT:
      return VK_FORMAT_R32G32B32A32_SFLOAT;
    case Format::kD32_FLOAT:
      return VK_FORMAT_D32_SFLOAT;
    case Format::kBC1_UNORM:
      return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
    case Format::kBC2_UNORM:
      return VK_FORMAT_BC2_UNORM_BLOCK;
    case Format::kBC3_UNORM:
      return VK_FORMAT_BC3_UNORM_BLOCK;
    case Format::kBC4_UNORM:
      return VK_FORMAT_BC4_UNORM_BLOCK;
    case Format::kBC5_UNORM:
      return VK_FORMAT_BC5_UNORM_BLOCK;
    default:
      return VK_FORMAT_UNDEFINED;
  }
}

// Bytes per block (BC) / per texel (uncompressed) - the copy math table.
uint32_t FormatBytesPerBlock(Format format) {
  switch (format) {
    case Format::kBC1_UNORM:
    case Format::kBC4_UNORM:
      return 8;
    case Format::kBC2_UNORM:
    case Format::kBC3_UNORM:
    case Format::kBC5_UNORM:
      return 16;
    case Format::kR8_UNORM:
      return 1;
    case Format::kR8G8_UNORM:
    case Format::kB5G6R5_UNORM:
      return 2;
    case Format::kR8G8B8A8_UNORM:
    case Format::kR8G8B8A8_UINT:
    case Format::kR16G16_UNORM:
    case Format::kR10G10B10A2_UNORM:
    case Format::kR32_FLOAT:
    case Format::kD32_FLOAT:
      return 4;
    case Format::kR11G11B10_FLOAT:
      return 4;
    case Format::kR16G16B16A16_FLOAT:
    case Format::kR32G32_FLOAT:
      return 8;
    case Format::kR32G32B32_FLOAT:
      return 12;
    case Format::kR32G32B32A32_FLOAT:
      return 16;
    default:
      return 4;
  }
}

VkImageAspectFlags FormatAspect(Format format) {
  return format == Format::kD32_FLOAT ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
}

struct StateInfo {
  VkPipelineStageFlags stage_mask;
  VkAccessFlags access_mask;
  VkImageLayout layout;
};

StateInfo ToStateInfo(ResourceState state) {
  switch (state) {
    case ResourceState::kRenderTarget:
      return {VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
              VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
              VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    case ResourceState::kDepthWrite:
      return {VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
              VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                  VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
              VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    case ResourceState::kPixelShaderResource:
      return {VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT,
              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    case ResourceState::kCopySource:
      return {VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT,
              VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL};
    case ResourceState::kCopyDest:
      return {VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL};
    case ResourceState::kGenericRead:
      return {VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
              VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    case ResourceState::kGuestOutput:
      // The presenter's internal steady state of the guest output image.
      return {ui::vulkan::VulkanPresenter::kGuestOutputInternalStageMask,
              ui::vulkan::VulkanPresenter::kGuestOutputInternalAccessMask,
              ui::vulkan::VulkanPresenter::kGuestOutputInternalLayout};
    case ResourceState::kCommon:
    default:
      return {VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, VK_IMAGE_LAYOUT_GENERAL};
  }
}

VkComponentSwizzle ToVkSwizzle(nrhi::Swizzle swizzle) {
  switch (swizzle) {
    case nrhi::Swizzle::kX:
      return VK_COMPONENT_SWIZZLE_R;
    case nrhi::Swizzle::kY:
      return VK_COMPONENT_SWIZZLE_G;
    case nrhi::Swizzle::kZ:
      return VK_COMPONENT_SWIZZLE_B;
    case nrhi::Swizzle::kW:
      return VK_COMPONENT_SWIZZLE_A;
    case nrhi::Swizzle::kZero:
      return VK_COMPONENT_SWIZZLE_ZERO;
    case nrhi::Swizzle::kOne:
    default:
      return VK_COMPONENT_SWIZZLE_ONE;
  }
}

VkBlendFactor ToVkBlend(nrhi::BlendFactor factor) {
  switch (factor) {
    case nrhi::BlendFactor::kZero:
      return VK_BLEND_FACTOR_ZERO;
    case nrhi::BlendFactor::kSrcAlpha:
      return VK_BLEND_FACTOR_SRC_ALPHA;
    case nrhi::BlendFactor::kInvSrcAlpha:
      return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case nrhi::BlendFactor::kOne:
    default:
      return VK_BLEND_FACTOR_ONE;
  }
}

VkBlendOp ToVkBlendOp(nrhi::BlendOp op) {
  return op == nrhi::BlendOp::kMin ? VK_BLEND_OP_MIN : VK_BLEND_OP_ADD;
}

VkCompareOp ToVkCompare(nrhi::CompareFunc func) {
  switch (func) {
    case nrhi::CompareFunc::kLess:
      return VK_COMPARE_OP_LESS;
    case nrhi::CompareFunc::kLessEqual:
      return VK_COMPARE_OP_LESS_OR_EQUAL;
    case nrhi::CompareFunc::kAlways:
    default:
      return VK_COMPARE_OP_ALWAYS;
  }
}

// ---------------------------------------------------------------------------
// Handle types.
// ---------------------------------------------------------------------------

class NrDeviceVulkan;

class NrBufferVulkan : public nrhi::Buffer {
 public:
  uint64_t size() const override { return size_; }

  VkBuffer buffer = VK_NULL_HANDLE;
  VmaAllocation allocation = VK_NULL_HANDLE;
  void* mapping = nullptr;
  uint64_t size_ = 0;  // the app-requested size (kFull allocations are padded)
  HeapKind heap = HeapKind::kDefault;
  nrhi::BufferBindClass bind_class = nrhi::BufferBindClass::kFull;
  bool device_local = false;  // memory-type DEVICE_LOCAL (diagnostics)
};

// Live-buffer placement totals (bimodal-FPS diagnosis): bytes of nrhi
// buffers currently alive per (heap kind, device-local?) bucket. Updated at
// create/DestroyDeferred, reported in the periodic "nrhi-vulkan mem" line.
std::atomic<uint64_t> g_buf_bytes_upload_dl{0};
std::atomic<uint64_t> g_buf_bytes_upload_host{0};
std::atomic<uint64_t> g_buf_bytes_default_dl{0};
std::atomic<uint64_t> g_buf_bytes_default_host{0};

class NrTextureVulkan : public nrhi::Texture {
 public:
  uint32_t width() const override { return desc.width; }
  uint32_t height() const override { return desc.height; }
  Format format() const override { return desc.format; }

  VkImage image = VK_NULL_HANDLE;
  VmaAllocation allocation = VK_NULL_HANDLE;  // VK_NULL_HANDLE for guest-output wrappers
  nrhi::TextureDesc desc;
  VkFormat vk_format = VK_FORMAT_UNDEFINED;
  VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
  // The current image layout; starts UNDEFINED so the first Barrier()
  // transitions from UNDEFINED and discards, matching D3D12 initial-state
  // semantics. Updated when barriers are queued and by internal transitions.
  VkImageLayout current_layout = VK_IMAGE_LAYOUT_UNDEFINED;
  // Mip-0 attachment view for RT/DS usage (identity swizzle - swizzled views
  // are for sampling only), created lazily; for guest-output wrappers this
  // is the presenter's own view and is not owned.
  VkImageView attachment_view = VK_NULL_HANDLE;
  bool attachment_view_owned = false;
  bool is_guest_output = false;
  // Latched pending clear, consumed as loadOp CLEAR at the next render pass
  // begin targeting this texture (or flushed as an empty CLEAR pass).
  bool pending_clear = false;
  float clear_color[4] = {};
  float clear_depth = 1.0f;

  VkImageSubresourceRange WholeRange() const {
    return ui::vulkan::util::InitializeSubresourceRange(aspect);
  }
};

class NrTextureViewVulkan : public nrhi::TextureView {
 public:
  VkImageView view = VK_NULL_HANDLE;
  NrTextureVulkan* texture = nullptr;
};

class NrBindingLayoutVulkan : public nrhi::BindingLayout {
 public:
  struct ParamInfo {
    nrhi::BindingParamKind kind = nrhi::BindingParamKind::kTextureTable;
    // Buffer kinds: binding index in set 0 (== index among buffer-kind
    // params in param order, per the frozen derivation rule).
    uint32_t set0_binding = 0;
    VkDescriptorType descriptor_type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    // Table kinds: the descriptor set index (1 + index among table params in
    // param order) and the declared table size.
    uint32_t set_index = 0;
    uint32_t table_index = 0;  // index among table params
    uint32_t table_size = 0;
  };

  VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
  VkDescriptorSetLayout set0_layout = VK_NULL_HANDLE;
  std::vector<VkDescriptorSetLayout> table_layouts;  // one per kTextureTable param
  std::vector<VkSampler> immutable_samplers;         // owned
  ParamInfo params[nrhi::kMaxBindingParams] = {};
  uint32_t param_count = 0;
  uint32_t buffer_binding_count = 0;  // dynamic-offset bindings in set 0
  uint32_t table_count = 0;
  int32_t constants_param = -1;       // param index of the kConstants param
  uint32_t constants_size_bytes = 0;  // count * 4 from the desc
};

class NrShaderVulkan : public nrhi::Shader {
 public:
  VkShaderModule module = VK_NULL_HANDLE;
  nrhi::ShaderStage stage = nrhi::ShaderStage::kVertex;
  // dxc -spirv keeps the HLSL entry-point name as the SPIR-V OpEntryPoint
  // name (unlike glslang's "main"); pipelines must pass it as pName.
  std::string entry_point = "main";
};

class NrPipelineVulkan : public nrhi::Pipeline {
 public:
  VkPipeline list_pipeline = VK_NULL_HANDLE;
  // TRIANGLE_STRIP twin, created lazily on the first strip draw with this
  // pipeline bound. Requires the shader modules (below) to still be alive at
  // that point - the scene keeps its shaders for the process lifetime,
  // mirroring how D3D12 baked the blobs into the PSO at creation.
  VkPipeline strip_pipeline = VK_NULL_HANDLE;

  // Everything needed to build the strip twin.
  VkShaderModule vs_module = VK_NULL_HANDLE;
  VkShaderModule ps_module = VK_NULL_HANDLE;
  // dxc-produced SPIR-V keeps the HLSL entry-point names.
  std::string vs_entry = "main";
  std::string ps_entry = "main";
  VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
  std::vector<nrhi::InputElementDesc> input_elements;
  uint32_t vertex_stride = 0;
  nrhi::BlendStateDesc blend;
  nrhi::DepthStateDesc depth;
  nrhi::CullMode cull = nrhi::CullMode::kNone;
  bool depth_clip = true;
  Format rtv_format = Format::kUnknown;
  Format dsv_format = Format::kUnknown;
  uint32_t sample_count = 1;
};

// ---------------------------------------------------------------------------
// Frame command recording.
// ---------------------------------------------------------------------------

// Per-frame render-thread CPU accounting for the backend's own work (the
// app's timers cover RenderScene minus this layer; sustained multi-ms frames
// here showed up as the "render=13ms, items=0.6ms" gap). Reported from
// EndFrame when a frame exceeds the threshold, throttled.
struct NrFrameProf {
  struct Bucket {
    uint64_t us = 0;
    uint32_t count = 0;
  };
  Bucket pass_open;
  Bucket flush_barriers;
  Bucket copies;
  Bucket table_miss;
  Bucket set0_miss;
  Bucket view_destroy;
  Bucket view_create;
  Bucket const_slice;
  Bucket pipeline_build;
  Bucket drain;
  void Reset() { *this = NrFrameProf{}; }
  uint64_t Total() const {
    return pass_open.us + flush_barriers.us + copies.us + table_miss.us + set0_miss.us +
           view_destroy.us + view_create.us + const_slice.us + pipeline_build.us + drain.us;
  }
};

class NrProfScope {
 public:
  NrProfScope(NrFrameProf::Bucket& bucket)
      : bucket_(bucket), start_(std::chrono::steady_clock::now()) {}
  ~NrProfScope() {
    bucket_.us += uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
                               std::chrono::steady_clock::now() - start_)
                               .count());
    ++bucket_.count;
  }

 private:
  NrFrameProf::Bucket& bucket_;
  std::chrono::steady_clock::time_point start_;
};

class NrCmdVulkan : public nrhi::Cmd {
 public:
  void SetBindingLayout(nrhi::BindingLayout* layout) override;
  void SetPipeline(nrhi::Pipeline* pipeline) override;
  void SetRootConstants(uint32_t param, uint32_t count, const void* values,
                        uint32_t dest_offset_in_values) override;
  void SetConstantBuffer(uint32_t param, nrhi::Buffer* buffer, uint64_t offset) override;
  void SetBufferSrv(uint32_t param, nrhi::Buffer* buffer, uint64_t offset) override;
  void SetTexture(uint32_t param, nrhi::TextureView* view) override;
  void SetTexturePair(uint32_t param, nrhi::TextureView* first,
                      nrhi::TextureView* second) override;
  void SetTextures(uint32_t param, nrhi::TextureView* const* views, uint32_t count) override;
  void SetRenderTargets(nrhi::Texture* color, nrhi::Texture* depth) override;
  void ClearRenderTarget(nrhi::Texture* color, const float color4[4]) override;
  void ClearDepth(nrhi::Texture* depth, float value) override;
  void SetViewport(const nrhi::Viewport& viewport) override;
  void SetScissor(const nrhi::Rect& rect) override;
  void SetVertexBuffer(nrhi::Buffer* buffer, uint64_t offset, uint32_t size_bytes,
                       uint32_t stride) override;
  void SetIndexBuffer(nrhi::Buffer* buffer, uint64_t offset, uint32_t size_bytes) override;
  void SetPrimitiveTopology(nrhi::PrimitiveTopology topology) override;
  void Draw(uint32_t vertex_count, uint32_t start_vertex) override;
  void DrawIndexed(uint32_t index_count, uint32_t start_index, int32_t base_vertex) override;
  void CopyBufferToTexture(nrhi::Texture* dst, uint32_t mip, uint32_t array_slice,
                           nrhi::Buffer* src, uint64_t src_offset, uint32_t row_pitch,
                           uint32_t width, uint32_t height, uint32_t depth) override;
  void CopyTextureToBuffer(nrhi::Buffer* dst, uint64_t dst_offset, uint32_t row_pitch,
                           nrhi::Texture* src, uint32_t mip, uint32_t width,
                           uint32_t height) override;
  void Barrier(nrhi::Texture* texture, ResourceState before, ResourceState after) override;
  void FlushBarriers() override;
  void ProfileRegion(nrhi::ProfileStage stage) override;

  // --- frame handling (driven by the device) ---
  void ResetFrameState();
  void EndFrame();

  NrDeviceVulkan* device = nullptr;

 private:
  friend class NrDeviceVulkan;

  bool EnsureRenderPassOpen();
  void EndRenderPassIfOpen();
  bool EnsureDrawState();
  void LatchPendingClear(NrTextureVulkan* texture, const float* color4, float depth);

  // Latched state (D3D12 command-list semantics).
  NrBindingLayoutVulkan* layout_ = nullptr;
  NrPipelineVulkan* pipeline_ = nullptr;
  VkPipeline bound_pipeline_ = VK_NULL_HANDLE;
  nrhi::PrimitiveTopology topology_ = nrhi::PrimitiveTopology::kTriangleList;

  // Root constants: CPU shadow block, copied to a ring UBO slice when dirty.
  uint32_t constants_shadow_[64] = {};
  bool constants_dirty_ = false;
  uint32_t constants_ring_offset_ = ~0u;  // dynamic offset of the current slice

  // Set 0 buffer bindings, by set-0 binding index.
  NrBufferVulkan* set0_buffers_[nrhi::kMaxBindingParams] = {};
  uint32_t set0_offsets_[nrhi::kMaxBindingParams] = {};
  bool set0_rebind_needed_ = true;
  VkDescriptorSet set0_bound_ = VK_NULL_HANDLE;

  // Texture tables, by table index within the layout.
  NrTextureViewVulkan* table_views_[nrhi::kMaxBindingParams][nrhi::kMaxTextureTableSize] = {};
  bool table_dirty_[nrhi::kMaxBindingParams] = {};

  // Render target state.
  NrTextureVulkan* rt_color_ = nullptr;
  NrTextureVulkan* rt_depth_ = nullptr;
  bool render_pass_open_ = false;
};

// ---------------------------------------------------------------------------
// Device.
// ---------------------------------------------------------------------------

class NrDeviceVulkan : public nrhi::Device {
 public:
  // Every kUpload/kDefault buffer allocation (and the ring UBO) is padded by
  // this much slack: dynamic-offset descriptors are written with fixed
  // ranges (UBO 16384, SSBO 65536), so an offset near the end of the app's
  // logical buffer must still leave the (offset + range) window in-bounds.
  static constexpr uint64_t kBufferPadding = 65536;
  static constexpr VkDeviceSize kUboBindRange = 16384;   // minimum maxUniformBufferRange
  static constexpr VkDeviceSize kSsboBindRange = 65536;  // covered by kBufferPadding

  static constexpr uint32_t kRingRegions = 8;
  static constexpr uint32_t kRingRegionSize = 2 * 1024 * 1024;
  // Mirrors VulkanCommandProcessor::kMaxFramesInFlight (private there): the
  // CP fences frame N so that frame N - kCpFramesInFlight has fully executed
  // before N records.
  static constexpr uint32_t kCpFramesInFlight = 3;
  // Ring regions provably idle while a frame records, usable as overflow
  // space for frames whose constants exceed one region. Of the kRingRegions
  // regions, the recording frame's own region, the kCpFramesInFlight - 1
  // regions behind it (the GPU may not have read them yet) and the
  // kCpFramesInFlight - 1 regions ahead of it (the next frames record into
  // them before this frame executes) are live; the rest belong to frames the
  // in-flight fence has already retired.
  static constexpr uint32_t kRingOverflowRegions =
      kRingRegions + 1 - 2 * kCpFramesInFlight;

  explicit NrDeviceVulkan(VulkanCommandProcessor* cp)
      : cp_(cp), vulkan_device_(cp->GetVulkanDevice()) {
    cmd_.device = this;
    // Thread-safe allocator: nrhi creation APIs are called from decode
    // worker threads concurrently with the render thread.
    allocator_ = ui::vulkan::CreateVmaAllocator(vulkan_device_, /*externally_synchronized=*/false);

    // The internal root-constant ring UBO: kRingRegions regions cycled by
    // frame index (deeper than the CP's 3 frames in flight), persistently
    // mapped, host-coherent. +kBufferPadding for the fixed descriptor range.
    VkBufferCreateInfo ring_info = {};
    ring_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    ring_info.size = VkDeviceSize(kRingRegions) * kRingRegionSize + kBufferPadding;
    // STORAGE too: unbound kBufferSrv params fall back to this buffer so
    // their STORAGE_BUFFER_DYNAMIC descriptors are always valid.
    ring_info.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    ring_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VmaAllocationCreateInfo ring_alloc_info = {};
    ring_alloc_info.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT |
                            VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
    ring_alloc_info.usage = VMA_MEMORY_USAGE_AUTO;
    ring_alloc_info.requiredFlags =
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    VmaAllocationInfo ring_result_info = {};
    if (allocator_ == VK_NULL_HANDLE ||
        vmaCreateBuffer(allocator_, &ring_info, &ring_alloc_info, &ring_buffer_,
                        &ring_allocation_, &ring_result_info) != VK_SUCCESS) {
      REXLOG_ERROR("nrhi-vulkan: root-constant ring UBO creation failed");
      ring_buffer_ = VK_NULL_HANDLE;
    } else {
      ring_mapping_ = static_cast<uint8_t*>(ring_result_info.pMappedData);
    }
  }

  ~NrDeviceVulkan() override {
    // Callers guarantee GPU idle; release everything immediately.
    const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device_->functions();
    const VkDevice device = vulkan_device_->device();
    FlushDissolvedViews();
    DrainRetired(~0ull);
    for (auto& entry : set0_sets_) {
      dfn.vkFreeDescriptorSets(device, entry.second.pool, 1, &entry.second.set);
    }
    for (auto& entry : table_sets_) {
      dfn.vkFreeDescriptorSets(device, entry.second.pool, 1, &entry.second.set);
    }
    for (auto& entry : framebuffers_) {
      dfn.vkDestroyFramebuffer(device, entry.second, nullptr);
    }
    for (auto& entry : render_passes_) {
      dfn.vkDestroyRenderPass(device, entry.second, nullptr);
    }
    for (VkDescriptorPool pool : descriptor_pools_) {
      dfn.vkDestroyDescriptorPool(device, pool, nullptr);
    }
    // App-held objects that were never DestroyDeferred'ed (the scene keeps
    // its caches for the whole session and gets no teardown notification):
    // release them here so no child object outlives vkDestroyDevice.
    // Framebuffers referencing leftover views were destroyed above; order
    // below: views before their images.
    if (!live_buffers_.empty() || !live_textures_.empty() || !live_views_.empty() ||
        !live_pipelines_.empty() || !live_shaders_.empty()) {
      REXLOG_INFO(
          "nrhi-vulkan: releasing app-held objects at device destruction "
          "(buffers={} textures={} views={} pipelines={} shaders={})",
          live_buffers_.size(), live_textures_.size(), live_views_.size(),
          live_pipelines_.size(), live_shaders_.size());
    }
    for (NrTextureViewVulkan* v : live_views_) {
      if (v->view != VK_NULL_HANDLE) {
        dfn.vkDestroyImageView(device, v->view, nullptr);
      }
      delete v;
    }
    live_views_.clear();
    for (NrPipelineVulkan* p : live_pipelines_) {
      if (p->list_pipeline != VK_NULL_HANDLE) {
        dfn.vkDestroyPipeline(device, p->list_pipeline, nullptr);
      }
      if (p->strip_pipeline != VK_NULL_HANDLE) {
        dfn.vkDestroyPipeline(device, p->strip_pipeline, nullptr);
      }
      delete p;
    }
    live_pipelines_.clear();
    for (NrShaderVulkan* s : live_shaders_) {
      if (s->module != VK_NULL_HANDLE) {
        dfn.vkDestroyShaderModule(device, s->module, nullptr);
      }
      delete s;
    }
    live_shaders_.clear();
    for (NrTextureVulkan* t : live_textures_) {
      if (t->attachment_view_owned && t->attachment_view != VK_NULL_HANDLE) {
        dfn.vkDestroyImageView(device, t->attachment_view, nullptr);
      }
      if (t->image != VK_NULL_HANDLE && t->allocation != VK_NULL_HANDLE) {
        vmaDestroyImage(allocator_, t->image, t->allocation);
      }
      delete t;
    }
    live_textures_.clear();
    for (NrBufferVulkan* b : live_buffers_) {
      if (b->buffer != VK_NULL_HANDLE) {
        vmaDestroyBuffer(allocator_, b->buffer, b->allocation);
      }
      delete b;
    }
    live_buffers_.clear();
    for (NrBindingLayoutVulkan* layout : layouts_) {
      for (VkSampler sampler : layout->immutable_samplers) {
        dfn.vkDestroySampler(device, sampler, nullptr);
      }
      for (VkDescriptorSetLayout set_layout : layout->table_layouts) {
        dfn.vkDestroyDescriptorSetLayout(device, set_layout, nullptr);
      }
      if (layout->set0_layout != VK_NULL_HANDLE) {
        dfn.vkDestroyDescriptorSetLayout(device, layout->set0_layout, nullptr);
      }
      if (layout->pipeline_layout != VK_NULL_HANDLE) {
        dfn.vkDestroyPipelineLayout(device, layout->pipeline_layout, nullptr);
      }
      delete layout;
    }
    for (auto& entry : guest_outputs_) {
      delete entry.second;  // image + view owned by the presenter
    }
    if (white_view_ != VK_NULL_HANDLE) {
      dfn.vkDestroyImageView(device, white_view_, nullptr);
    }
    if (white_image_ != VK_NULL_HANDLE) {
      vmaDestroyImage(allocator_, white_image_, white_allocation_);
    }
    if (white_staging_ != VK_NULL_HANDLE) {
      vmaDestroyBuffer(allocator_, white_staging_, white_staging_allocation_);
    }
    if (ring_buffer_ != VK_NULL_HANDLE) {
      vmaDestroyBuffer(allocator_, ring_buffer_, ring_allocation_);
    }
    if (allocator_ != VK_NULL_HANDLE) {
      vmaDestroyAllocator(allocator_);
    }
  }

  Backend backend() const override { return Backend::kVulkan; }

  // --- thread-safe creation / mapping ---

  nrhi::Buffer* CreateBuffer(const nrhi::BufferDesc& desc) override {
    VkBufferCreateInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VmaAllocationCreateInfo alloc_info = {};
    // The descriptor-window padding only matters for buffers that can be
    // bound through dynamic-offset UBO/SSBO descriptors; narrower bind
    // classes skip it (and the descriptor usage bits).
    const bool descriptor_bindable = desc.bind_class == nrhi::BufferBindClass::kFull;
    const uint64_t padding = descriptor_bindable ? kBufferPadding : 0;
    VkBufferUsageFlags app_usage;
    switch (desc.bind_class) {
      case nrhi::BufferBindClass::kCopySrc:
        app_usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        break;
      case nrhi::BufferBindClass::kVertexIndex:
        app_usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                    VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
        break;
      case nrhi::BufferBindClass::kFull:
      default:
        app_usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                    VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        break;
    }
    switch (desc.heap) {
      case HeapKind::kUpload:
        info.size = desc.size + padding;
        info.usage = app_usage;
        alloc_info.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT |
                           VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
        alloc_info.usage = VMA_MEMORY_USAGE_AUTO;
        // Coherent is required, not merely preferred: the app writes upload
        // memory with no flush machinery (write-combined D3D12 semantics).
        // Every conformant implementation has a HOST_VISIBLE|HOST_COHERENT
        // type.
        alloc_info.requiredFlags =
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        break;
      case HeapKind::kReadback:
        info.size = desc.size;
        info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        alloc_info.flags =
            VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT;
        alloc_info.usage = VMA_MEMORY_USAGE_AUTO;
        alloc_info.requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
        alloc_info.preferredFlags = VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
        break;
      case HeapKind::kDefault:
      default:
        info.size = desc.size + padding;
        info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | app_usage;
        alloc_info.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        break;
    }
    // Unpadded classes could otherwise reach vkCreateBuffer with size 0
    // (invalid); the padded classes never could.
    info.size = std::max<VkDeviceSize>(info.size, 4);
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    VmaAllocationInfo result_info = {};
    if (vmaCreateBuffer(allocator_, &info, &alloc_info, &buffer, &allocation, &result_info) !=
        VK_SUCCESS) {
      REXLOG_ERROR("nrhi-vulkan: buffer creation failed ({} bytes, heap {})", desc.size,
                   uint32_t(desc.heap));
      return nullptr;
    }
    VkMemoryPropertyFlags mem_flags = 0;
    vmaGetAllocationMemoryProperties(allocator_, allocation, &mem_flags);
    const bool mem_dl = (mem_flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0;
    if (desc.heap == HeapKind::kDefault && !mem_dl) {
      REXLOG_WARN("nrhi-vulkan: DEFAULT-heap buffer ({} bytes) landed in HOST memory (flags {:#x})",
                  desc.size, uint32_t(mem_flags));
    }
    if (desc.heap == HeapKind::kUpload) {
      (mem_dl ? g_buf_bytes_upload_dl : g_buf_bytes_upload_host)
          .fetch_add(desc.size, std::memory_order_relaxed);
    } else if (desc.heap == HeapKind::kDefault) {
      (mem_dl ? g_buf_bytes_default_dl : g_buf_bytes_default_host)
          .fetch_add(desc.size, std::memory_order_relaxed);
    }
    auto* b = new NrBufferVulkan();
    b->buffer = buffer;
    b->allocation = allocation;
    b->mapping = result_info.pMappedData;  // persistent map for host heaps
    b->size_ = desc.size;
    b->heap = desc.heap;
    b->bind_class = desc.bind_class;
    b->device_local = mem_dl;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      live_buffers_.insert(b);
    }
    return b;
  }

  nrhi::Texture* CreateTexture(const nrhi::TextureDesc& desc) override {
    VkImageCreateInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.imageType = desc.kind == TextureKind::k3D ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
    info.format = ToVkFormat(desc.format);
    info.extent.width = desc.width;
    info.extent.height = desc.height;
    info.extent.depth = desc.kind == TextureKind::k3D ? desc.depth : 1;
    info.mipLevels = desc.mip_levels;
    info.arrayLayers = desc.kind == TextureKind::kCube ? 6 : 1;
    info.samples = VkSampleCountFlagBits(desc.sample_count);
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (desc.usage & nrhi::kTextureUsageRenderTarget) {
      info.usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    }
    if (desc.usage & nrhi::kTextureUsageDepthStencil) {
      info.usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    }
    if (desc.usage & nrhi::kTextureUsageCopySource) {
      info.usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    }
    if (desc.kind == TextureKind::kCube) {
      info.flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    }
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VmaAllocationCreateInfo alloc_info = {};
    alloc_info.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    VkImage image = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    if (vmaCreateImage(allocator_, &info, &alloc_info, &image, &allocation, nullptr) !=
        VK_SUCCESS) {
      REXLOG_ERROR("nrhi-vulkan: texture creation failed ({}x{} fmt {})", desc.width, desc.height,
                   uint32_t(desc.format));
      return nullptr;
    }
    {
      VkMemoryPropertyFlags mem_flags = 0;
      vmaGetAllocationMemoryProperties(allocator_, allocation, &mem_flags);
      if (!(mem_flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
        REXLOG_WARN(
            "nrhi-vulkan: texture ({}x{} fmt {} samples {}) landed in HOST memory (flags {:#x})",
            desc.width, desc.height, uint32_t(desc.format), desc.sample_count,
            uint32_t(mem_flags));
      }
    }
    auto* t = new NrTextureVulkan();
    t->image = image;
    t->allocation = allocation;
    t->desc = desc;
    t->vk_format = info.format;
    t->aspect = FormatAspect(desc.format);
    t->attachment_view_owned = true;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      live_textures_.insert(t);
    }
    // current_layout starts UNDEFINED regardless of desc.initial_state: the
    // first transition (explicit Barrier or the internal ensure-layout on
    // first attachment/copy use) comes from UNDEFINED and discards.
    return t;
  }

  void* Map(nrhi::Buffer* buffer) override {
    // Host-heap buffers are persistently mapped at creation.
    return static_cast<NrBufferVulkan*>(buffer)->mapping;
  }

  void Unmap(nrhi::Buffer*) override {
    // Persistent mapping - nothing to do.
  }

  void InvalidateForRead(nrhi::Buffer* buffer, uint64_t offset, uint64_t size) override {
    auto* b = static_cast<NrBufferVulkan*>(buffer);
    vmaInvalidateAllocation(allocator_, b->allocation, offset, size);
  }

  // --- deferred destruction ---

  void DestroyDeferred(nrhi::Buffer* buffer) override {
    if (buffer == nullptr) return;
    auto* b = static_cast<NrBufferVulkan*>(buffer);
    // Retire cached set-0 descriptor sets whose buffer tuple references this
    // buffer. set0_sets_ is render-thread-only, like the D3D12 backend's
    // bindings_: buffer destruction of bound buffers happens on the render
    // thread. Non-kFull buffers can never appear in a descriptor tuple, so
    // their destruction (the mesh/staging churn path) skips the scan.
    const uint64_t submission = cp_->GetCurrentSubmission();
    if (b->bind_class == nrhi::BufferBindClass::kFull) {
      for (auto it = set0_sets_.begin(); it != set0_sets_.end();) {
        bool references = false;
        for (uint32_t i = 0; i < it->first.count; ++i) {
          if (it->first.buffers[i] == b->buffer) {
            references = true;
            break;
          }
        }
        if (references) {
          std::lock_guard<std::mutex> lock(mutex_);
          RetireDescriptorSetLocked(it->second, submission);
          it = set0_sets_.erase(it);
        } else {
          ++it;
        }
      }
    }
    if (b->heap == HeapKind::kUpload) {
      (b->device_local ? g_buf_bytes_upload_dl : g_buf_bytes_upload_host)
          .fetch_sub(b->size_, std::memory_order_relaxed);
    } else if (b->heap == HeapKind::kDefault) {
      (b->device_local ? g_buf_bytes_default_dl : g_buf_bytes_default_host)
          .fetch_sub(b->size_, std::memory_order_relaxed);
    }
    std::lock_guard<std::mutex> lock(mutex_);
    live_buffers_.erase(b);
    RetiredObject r;
    r.submission = submission;
    r.buffer = b->buffer;
    r.buffer_allocation = b->allocation;
    retired_.push_back(r);
    delete b;
  }

  void DestroyDeferred(nrhi::Texture* texture) override {
    if (texture == nullptr) return;
    auto* t = static_cast<NrTextureVulkan*>(texture);
    if (t->is_guest_output) return;  // wrappers are owned by the device cache
    RetireFramebuffersForView(t->attachment_view);
    if (cmd_.rt_color_ == t) cmd_.rt_color_ = nullptr;
    if (cmd_.rt_depth_ == t) cmd_.rt_depth_ = nullptr;
    std::erase(pending_clear_textures_, t);
    std::lock_guard<std::mutex> lock(mutex_);
    live_textures_.erase(t);
    RetiredObject r;
    r.submission = cp_->GetCurrentSubmission();
    r.image = t->image;
    r.image_allocation = t->allocation;
    if (t->attachment_view_owned) {
      r.view = t->attachment_view;
    }
    retired_.push_back(r);
    delete t;
  }

  void DestroyDeferred(nrhi::TextureView* view) override {
    if (view == nullptr) return;
    // Destruction is batched: the view object stays allocated (so its
    // address cannot be reused by a new view while stale cache keys still
    // hold it) and FlushDissolvedViews sweeps the descriptor-set cache ONCE
    // per frame for the whole batch. The eviction sweeps retire ~1500 views
    // in a single frame; a per-view cache scan was a measured 10-15 ms
    // recurring hitch.
    auto* v = static_cast<NrTextureViewVulkan*>(view);
    {
      std::lock_guard<std::mutex> lock(mutex_);
      live_views_.erase(v);
    }
    dissolved_views_.push_back(v);
  }

  // Render thread, once per frame (and at device destruction): retire every
  // cached texture descriptor set referencing a view destroyed since the
  // last flush, then retire the views themselves.
  void FlushDissolvedViews() {
    if (dissolved_views_.empty()) return;
    NrProfScope prof_scope(prof_.view_destroy);
    const uint64_t submission = cp_->GetCurrentSubmission();
    std::unordered_set<const NrTextureViewVulkan*> dissolved(dissolved_views_.begin(),
                                                             dissolved_views_.end());
    for (auto it = table_sets_.begin(); it != table_sets_.end();) {
      bool references = false;
      for (uint32_t i = 0; i < it->first.count; ++i) {
        if (dissolved.count(it->first.views[i]) != 0) {
          references = true;
          break;
        }
      }
      if (references) {
        std::lock_guard<std::mutex> lock(mutex_);
        RetireDescriptorSetLocked(it->second, submission);
        it = table_sets_.erase(it);
      } else {
        ++it;
      }
    }
    std::lock_guard<std::mutex> lock(mutex_);
    for (NrTextureViewVulkan* v : dissolved_views_) {
      RetiredObject r;
      r.submission = submission;
      r.view = v->view;
      retired_.push_back(r);
      delete v;
    }
    dissolved_views_.clear();
  }

  void DestroyDeferred(nrhi::Pipeline* pipeline) override {
    if (pipeline == nullptr) return;
    auto* p = static_cast<NrPipelineVulkan*>(pipeline);
    std::lock_guard<std::mutex> lock(mutex_);
    live_pipelines_.erase(p);
    RetiredObject r;
    r.submission = cp_->GetCurrentSubmission();
    r.pipelines[0] = p->list_pipeline;
    r.pipelines[1] = p->strip_pipeline;
    retired_.push_back(r);
    delete p;
  }

  void DestroyDeferred(nrhi::Shader* shader) override {
    if (shader == nullptr) return;
    auto* s = static_cast<NrShaderVulkan*>(shader);
    std::lock_guard<std::mutex> lock(mutex_);
    live_shaders_.erase(s);
    RetiredObject r;
    r.submission = cp_->GetCurrentSubmission();
    r.shader_module = s->module;
    retired_.push_back(r);
    delete s;
  }

  // --- render-thread-only creation ---

  nrhi::TextureView* CreateTextureView(nrhi::Texture* texture,
                                       const nrhi::TextureViewDesc& desc) override {
    NrProfScope prof_scope(prof_.view_create);
    auto* t = static_cast<NrTextureVulkan*>(texture);
    VkImageViewCreateInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    info.image = t->image;
    switch (desc.dimension) {
      case nrhi::ViewDimension::kCube:
        info.viewType = VK_IMAGE_VIEW_TYPE_CUBE;
        break;
      case nrhi::ViewDimension::k3D:
        info.viewType = VK_IMAGE_VIEW_TYPE_3D;
        break;
      case nrhi::ViewDimension::k2DMS:
      case nrhi::ViewDimension::k2D:
      default:
        info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        break;
    }
    // kUnknown = the texture's own format. D32 depth is sampled through the
    // depth aspect of the D32_SFLOAT image (the shader reads .r - the
    // R32_FLOAT-cast semantics of the D3D12 arrangement).
    info.format = desc.format != Format::kUnknown ? ToVkFormat(desc.format) : t->vk_format;
    info.components.r = ToVkSwizzle(desc.swizzle[0]);
    info.components.g = ToVkSwizzle(desc.swizzle[1]);
    info.components.b = ToVkSwizzle(desc.swizzle[2]);
    info.components.a = ToVkSwizzle(desc.swizzle[3]);
    info.subresourceRange.aspectMask = t->aspect;
    info.subresourceRange.baseMipLevel = desc.base_mip;
    info.subresourceRange.levelCount =
        desc.mip_levels == ~0u ? VK_REMAINING_MIP_LEVELS : desc.mip_levels;
    info.subresourceRange.baseArrayLayer = 0;
    info.subresourceRange.layerCount = VK_REMAINING_ARRAY_LAYERS;
    const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device_->functions();
    VkImageView view = VK_NULL_HANDLE;
    if (dfn.vkCreateImageView(vulkan_device_->device(), &info, nullptr, &view) != VK_SUCCESS) {
      REXLOG_ERROR("nrhi-vulkan: texture view creation failed");
      return nullptr;
    }
    auto* v = new NrTextureViewVulkan();
    v->view = view;
    v->texture = t;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      live_views_.insert(v);
    }
    return v;
  }

  nrhi::BindingLayout* CreateBindingLayout(const nrhi::BindingLayoutDesc& desc) override {
    const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device_->functions();
    const VkDevice device = vulkan_device_->device();
    const ui::vulkan::VulkanDevice::Properties& props = vulkan_device_->properties();

    auto* layout = new NrBindingLayoutVulkan();
    layout->param_count = desc.param_count;

    // Immutable samplers from the static sampler descs.
    std::vector<VkSampler> samplers;
    for (uint32_t i = 0; i < desc.static_sampler_count; ++i) {
      const nrhi::StaticSamplerDesc& s = desc.static_samplers[i];
      VkSamplerCreateInfo sampler_info = {};
      sampler_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
      const bool linear = s.filter != nrhi::Filter::kPoint;
      sampler_info.magFilter = linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
      sampler_info.minFilter = sampler_info.magFilter;
      sampler_info.mipmapMode =
          linear ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
      VkSamplerAddressMode address = s.address == nrhi::AddressMode::kWrap
                                         ? VK_SAMPLER_ADDRESS_MODE_REPEAT
                                         : VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
      sampler_info.addressModeU = address;
      sampler_info.addressModeV = address;
      sampler_info.addressModeW = address;
      if (s.filter == nrhi::Filter::kAnisotropic && props.samplerAnisotropy) {
        sampler_info.anisotropyEnable = VK_TRUE;
        sampler_info.maxAnisotropy =
            std::min(float(std::max(s.max_anisotropy, 1u)), props.maxSamplerAnisotropy);
      }
      sampler_info.minLod = 0.0f;
      sampler_info.maxLod = VK_LOD_CLAMP_NONE;
      VkSampler sampler = VK_NULL_HANDLE;
      if (dfn.vkCreateSampler(device, &sampler_info, nullptr, &sampler) != VK_SUCCESS) {
        REXLOG_ERROR("nrhi-vulkan: immutable sampler creation failed");
      }
      samplers.push_back(sampler);
    }
    layout->immutable_samplers = samplers;

    // Set 0: buffer params in param order (dynamic-offset descriptors), then
    // the static samplers in declaration order (immutable). All graphics
    // stage flags everywhere - the frozen derivation rule.
    const VkShaderStageFlags kAllGraphics = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    std::vector<VkDescriptorSetLayoutBinding> set0_bindings;
    uint32_t table_count = 0;
    for (uint32_t i = 0; i < desc.param_count; ++i) {
      const nrhi::BindingParamDesc& p = desc.params[i];
      NrBindingLayoutVulkan::ParamInfo& info = layout->params[i];
      info.kind = p.kind;
      switch (p.kind) {
        case nrhi::BindingParamKind::kConstants:
          layout->constants_param = int32_t(i);
          layout->constants_size_bytes = p.count * sizeof(uint32_t);
          [[fallthrough]];
        case nrhi::BindingParamKind::kConstantBuffer:
        case nrhi::BindingParamKind::kBufferSrv: {
          info.set0_binding = uint32_t(set0_bindings.size());
          info.descriptor_type = p.kind == nrhi::BindingParamKind::kBufferSrv
                                     ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC
                                     : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
          VkDescriptorSetLayoutBinding binding = {};
          binding.binding = info.set0_binding;
          binding.descriptorType = info.descriptor_type;
          binding.descriptorCount = 1;
          binding.stageFlags = kAllGraphics;
          set0_bindings.push_back(binding);
          break;
        }
        case nrhi::BindingParamKind::kTextureTable: {
          info.table_index = table_count;
          info.set_index = 1 + table_count;
          info.table_size = p.count;
          ++table_count;
          break;
        }
      }
    }
    layout->buffer_binding_count = uint32_t(set0_bindings.size());
    layout->table_count = table_count;
    for (uint32_t i = 0; i < desc.static_sampler_count; ++i) {
      VkDescriptorSetLayoutBinding binding = {};
      binding.binding = layout->buffer_binding_count + i;
      binding.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
      binding.descriptorCount = 1;
      binding.stageFlags = kAllGraphics;
      binding.pImmutableSamplers = &layout->immutable_samplers[i];
      set0_bindings.push_back(binding);
    }

    VkDescriptorSetLayoutCreateInfo set_layout_info = {};
    set_layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    set_layout_info.bindingCount = uint32_t(set0_bindings.size());
    set_layout_info.pBindings = set0_bindings.data();
    if (dfn.vkCreateDescriptorSetLayout(device, &set_layout_info, nullptr, &layout->set0_layout) !=
        VK_SUCCESS) {
      REXLOG_ERROR("nrhi-vulkan: set-0 descriptor set layout creation failed");
    }

    // Sets 1..N: one per kTextureTable param, SAMPLED_IMAGE bindings
    // 0..count-1.
    for (uint32_t i = 0; i < desc.param_count; ++i) {
      const nrhi::BindingParamDesc& p = desc.params[i];
      if (p.kind != nrhi::BindingParamKind::kTextureTable) continue;
      std::vector<VkDescriptorSetLayoutBinding> table_bindings(p.count);
      for (uint32_t j = 0; j < p.count; ++j) {
        table_bindings[j] = {};
        table_bindings[j].binding = j;
        table_bindings[j].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        table_bindings[j].descriptorCount = 1;
        table_bindings[j].stageFlags = kAllGraphics;
      }
      VkDescriptorSetLayoutCreateInfo table_layout_info = {};
      table_layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
      table_layout_info.bindingCount = p.count;
      table_layout_info.pBindings = table_bindings.data();
      VkDescriptorSetLayout table_layout = VK_NULL_HANDLE;
      if (dfn.vkCreateDescriptorSetLayout(device, &table_layout_info, nullptr, &table_layout) !=
          VK_SUCCESS) {
        REXLOG_ERROR("nrhi-vulkan: texture-table descriptor set layout creation failed");
      }
      layout->table_layouts.push_back(table_layout);
    }

    std::vector<VkDescriptorSetLayout> all_layouts;
    all_layouts.push_back(layout->set0_layout);
    for (VkDescriptorSetLayout l : layout->table_layouts) {
      all_layouts.push_back(l);
    }
    VkPipelineLayoutCreateInfo pipeline_layout_info = {};
    pipeline_layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipeline_layout_info.setLayoutCount = uint32_t(all_layouts.size());
    pipeline_layout_info.pSetLayouts = all_layouts.data();
    if (dfn.vkCreatePipelineLayout(device, &pipeline_layout_info, nullptr,
                                   &layout->pipeline_layout) != VK_SUCCESS) {
      REXLOG_ERROR("nrhi-vulkan: pipeline layout creation failed");
      layouts_.push_back(layout);
      return nullptr;
    }
    layouts_.push_back(layout);
    return layout;
  }

  nrhi::Shader* CreateShader(const nrhi::ShaderDesc& desc) override {
    if (desc.spirv == nullptr || desc.spirv_size_bytes == 0) {
      REXLOG_ERROR("nrhi-vulkan: no SPIR-V blob for {}:{}",
                   desc.name != nullptr ? desc.name : "?",
                   desc.entry_point != nullptr ? desc.entry_point : "?");
      return nullptr;
    }
    VkShaderModule module =
        ui::vulkan::util::CreateShaderModule(vulkan_device_, desc.spirv, desc.spirv_size_bytes);
    if (module == VK_NULL_HANDLE) {
      REXLOG_ERROR("nrhi-vulkan: shader module creation failed ({}:{})",
                   desc.name != nullptr ? desc.name : "?",
                   desc.entry_point != nullptr ? desc.entry_point : "?");
      return nullptr;
    }
    auto* shader = new NrShaderVulkan();
    shader->module = module;
    shader->stage = desc.stage;
    if (desc.entry_point != nullptr) {
      shader->entry_point = desc.entry_point;
    }
    {
      std::lock_guard<std::mutex> lock(mutex_);
      live_shaders_.insert(shader);
    }
    return shader;
  }

  nrhi::Pipeline* CreateGraphicsPipeline(const nrhi::GraphicsPipelineDesc& desc) override {
    auto* layout = static_cast<NrBindingLayoutVulkan*>(desc.layout);
    auto* vs = static_cast<NrShaderVulkan*>(desc.vs);
    auto* ps = static_cast<NrShaderVulkan*>(desc.ps);
    if (layout == nullptr || vs == nullptr || ps == nullptr) return nullptr;
    auto* pipeline = new NrPipelineVulkan();
    pipeline->vs_module = vs->module;
    pipeline->ps_module = ps->module;
    pipeline->vs_entry = vs->entry_point;
    pipeline->ps_entry = ps->entry_point;
    pipeline->pipeline_layout = layout->pipeline_layout;
    if (desc.input_elements != nullptr && desc.input_element_count != 0) {
      pipeline->input_elements.assign(desc.input_elements,
                                      desc.input_elements + desc.input_element_count);
    }
    pipeline->vertex_stride = desc.vertex_stride;
    pipeline->blend = desc.blend;
    pipeline->depth = desc.depth;
    pipeline->cull = desc.cull;
    pipeline->depth_clip = desc.depth_clip;
    pipeline->rtv_format = desc.rtv_format;
    pipeline->dsv_format = desc.dsv_format;
    pipeline->sample_count = desc.sample_count;
    pipeline->list_pipeline = BuildPipeline(*pipeline, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
    if (pipeline->list_pipeline == VK_NULL_HANDLE) {
      REXLOG_ERROR("nrhi-vulkan: graphics pipeline creation failed");
      delete pipeline;
      return nullptr;
    }
    // Build the strip twin before the scene releases its shader modules.
    // Deferring this until the first draw caused the RG406V loading path to
    // remain on a black frame, so keep the proven eager lifetime here.
    pipeline->strip_pipeline = BuildPipeline(*pipeline, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP);
    if (pipeline->strip_pipeline == VK_NULL_HANDLE) {
      REXLOG_ERROR("nrhi-vulkan: strip pipeline creation failed");
      DestroyDeferred(static_cast<nrhi::Pipeline*>(pipeline));
      return nullptr;
    }
    {
      std::lock_guard<std::mutex> lock(mutex_);
      live_pipelines_.insert(pipeline);
    }
    return pipeline;
  }

  uint32_t GetSupportedSampleCount(Format format, uint32_t desired) override {
    (void)format;  // color RT formats only; the device-wide framebuffer +
                   // sampled-image color masks are the portable bound.
    const ui::vulkan::VulkanDevice::Properties& props = vulkan_device_->properties();
    const VkSampleCountFlags mask =
        props.framebufferColorSampleCounts & props.sampledImageColorSampleCounts;
    uint32_t count = desired;
    while (count > 1 && !(mask & count)) {
      count >>= 1;
    }
    return std::max(count, 1u);
  }

  uint64_t CurrentSubmission() const override { return cp_->GetCurrentSubmission(); }
  uint64_t CompletedSubmission() const override { return cp_->GetCompletedSubmission(); }

  // --- frame handling (called from the command processor) ---

  nrhi::Cmd* BeginFrame(VkImage guest_output_image, VkImageView guest_output_image_view,
                        bool guest_output_ever_written, uint32_t width, uint32_t height,
                        nrhi::Texture** guest_output_out) {
    prof_.Reset();
    FlushDissolvedViews();
    {
      std::lock_guard<std::mutex> lock(mutex_);
      // Bounded per frame: the eviction sweeps retire thousands of objects
      // at once, and destroying them all in one frame is its own hitch (the
      // remainder drains over the following frames). 256 objects measured
      // ~5 ms worst case through the allocator.
      DrainRetired(cp_->GetCompletedSubmission(), 256);
    }
    ++frame_index_;
    if ((frame_index_ % 600) == 0) {
      // Periodic VRAM budget attribution (bimodal-FPS diagnosis): log every
      // heap's VMA usage vs the driver-reported budget, flagging device-local.
      VmaBudget budgets[VK_MAX_MEMORY_HEAPS] = {};
      vmaGetHeapBudgets(allocator_, budgets);
      const VkPhysicalDeviceMemoryProperties* mem_props = nullptr;
      vmaGetMemoryProperties(allocator_, &mem_props);
      char line[512];
      size_t off = 0;
      for (uint32_t i = 0; i < mem_props->memoryHeapCount && off + 64 < sizeof(line); ++i) {
        const bool dl = (mem_props->memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0;
        off += std::snprintf(line + off, sizeof(line) - off, "%sheap%u%s use=%lluMB budget=%lluMB",
                             i ? " | " : "", i, dl ? "(DL)" : "",
                             (unsigned long long)(budgets[i].usage >> 20),
                             (unsigned long long)(budgets[i].budget >> 20));
      }
      size_t retired_backlog = 0;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        retired_backlog = retired_.size();
      }
      REXLOG_INFO(
          "nrhi-vulkan mem: {} | bufs upload dl={}MB host={}MB, default dl={}MB host={}MB | "
          "retired={} | ring peak={}KB ovf={}",
          line, g_buf_bytes_upload_dl.load(std::memory_order_relaxed) >> 20,
          g_buf_bytes_upload_host.load(std::memory_order_relaxed) >> 20,
          g_buf_bytes_default_dl.load(std::memory_order_relaxed) >> 20,
          g_buf_bytes_default_host.load(std::memory_order_relaxed) >> 20, retired_backlog,
          ring_peak_bytes_ >> 10, ring_overflow_count_);
    }
    ring_region_base_ = uint32_t(frame_index_ % kRingRegions) * kRingRegionSize;
    ring_region_offset_ = 0;
    ring_overflow_hops_ = 0;
    ring_peak_bytes_ = std::max(ring_peak_bytes_, ring_frame_bytes_);
    ring_frame_bytes_ = 0;
    cmd_.ResetFrameState();
    // Close any emulated render pass and flush the CP's queued barriers: the
    // raw commands recorded below (and by the app callback) go into the same
    // deferred command buffer and must be outside any render pass.
    cp_->SubmitBarriers(true);
    EnsureWhiteTexture();

    NrTextureVulkan*& wrapper = guest_outputs_[guest_output_image];
    if (wrapper != nullptr &&
        (wrapper->desc.width != width || wrapper->desc.height != height ||
         wrapper->attachment_view != guest_output_image_view)) {
      // Same VkImage handle value, different geometry/view: a recreated
      // image reusing the handle. Drop the stale wrapper (image + view are
      // presenter-owned) and its framebuffers.
      DestroyGuestOutputWrapper(wrapper);
      wrapper = nullptr;
    }
    if (wrapper == nullptr) {
      wrapper = new NrTextureVulkan();
      wrapper->image = guest_output_image;
      wrapper->desc.width = width;
      wrapper->desc.height = height;
      wrapper->desc.format = Format::kR10G10B10A2_UNORM;
      wrapper->desc.usage = nrhi::kTextureUsageRenderTarget;
      wrapper->vk_format = ui::vulkan::VulkanPresenter::kGuestOutputFormat;
      wrapper->aspect = VK_IMAGE_ASPECT_COLOR_BIT;
      wrapper->attachment_view = guest_output_image_view;
      wrapper->attachment_view_owned = false;
      wrapper->is_guest_output = true;
      // The presenter keeps a small guest-output mailbox; evict wrappers of
      // images it has replaced.
      if (guest_outputs_.size() > 6) {
        for (auto it = guest_outputs_.begin(); it != guest_outputs_.end();) {
          if (it->second != wrapper && it->first != guest_output_image) {
            DestroyGuestOutputWrapper(it->second);
            it = guest_outputs_.erase(it);
          } else {
            ++it;
          }
        }
      }
    }
    // Acquire contract: UNDEFINED before the image's first ever write, the
    // presenter's internal layout otherwise.
    wrapper->current_layout = guest_output_ever_written
                                  ? ui::vulkan::VulkanPresenter::kGuestOutputInternalLayout
                                  : VK_IMAGE_LAYOUT_UNDEFINED;
    wrapper->pending_clear = false;
    *guest_output_out = wrapper;
    return &cmd_;
  }

  void EndFrame() {
    cmd_.EndFrame();
    // Render-thread CPU attribution for slow frames (throttled 8 per 5 s).
    const uint64_t total_us = prof_.Total();
    if (total_us >= 4000) {
      const auto now = std::chrono::steady_clock::now();
      static std::chrono::steady_clock::time_point s_window_start{};
      static uint32_t s_window_count = 0;
      if (now - s_window_start > std::chrono::seconds(5)) {
        s_window_start = now;
        s_window_count = 0;
      }
      if (++s_window_count <= 8) {
        REXLOG_INFO(
            "nrhi-vulkan: SLOW frame {}us: pass_open={}us/{} flush={}us/{} copies={}us/{} "
            "table_miss={}us/{} set0_miss={}us/{} view_destroy={}us/{} view_create={}us/{} "
            "const={}us/{} pso={}us/{} drain={}us/{}",
            total_us, prof_.pass_open.us, prof_.pass_open.count, prof_.flush_barriers.us,
            prof_.flush_barriers.count, prof_.copies.us, prof_.copies.count, prof_.table_miss.us,
            prof_.table_miss.count, prof_.set0_miss.us, prof_.set0_miss.count,
            prof_.view_destroy.us, prof_.view_destroy.count, prof_.view_create.us,
            prof_.view_create.count, prof_.const_slice.us, prof_.const_slice.count,
            prof_.pipeline_build.us, prof_.pipeline_build.count, prof_.drain.us,
            prof_.drain.count);
      }
    }
  }

  NrFrameProf prof_;

  // --- internals shared with NrCmdVulkan ---

  VulkanCommandProcessor* cp() { return cp_; }
  const ui::vulkan::VulkanDevice* vulkan_device() const { return vulkan_device_; }
  VkBuffer ring_buffer() const { return ring_buffer_; }
  VkImageView white_view() const { return white_view_; }

  // Bump-allocates a 256-aligned slice in the current ring region and copies
  // the shadow block in. Returns the dynamic offset. Render thread only.
  uint32_t AllocateConstantSlice(const void* data, uint32_t size_bytes) {
    if (ring_mapping_ == nullptr) return 0;
    NrProfScope prof_scope(prof_.const_slice);
    uint32_t aligned = (size_bytes + 255u) & ~255u;
    if (ring_region_offset_ + aligned > kRingRegionSize) {
      if (ring_overflow_hops_ < kRingOverflowRegions) {
        // Region exhausted: continue in a region the in-flight fence has
        // already retired (see kRingOverflowRegions). Wrapping to the region
        // start instead silently overwrote the frame's EARLIEST recorded
        // draws - the shadow-atlas caster pass - so oversized frames (static
        // sun-map rebuilds, dense views) dropped every dynamic shadow for
        // exactly one frame: a character/vehicle shadow blink unique to this
        // backend (D3D12 uses true root constants).
        ring_region_base_ =
            uint32_t((frame_index_ + kCpFramesInFlight + ring_overflow_hops_) %
                     kRingRegions) *
            kRingRegionSize;
        ring_region_offset_ = 0;
        ++ring_overflow_hops_;
        ++ring_overflow_count_;
        if (ring_overflow_count_ == 1 || (ring_overflow_count_ & 1023u) == 0) {
          REXLOG_INFO(
              "nrhi-vulkan: root-constant region overflow (> {} bytes this frame), "
              "continuing in idle region (hop {}/{}, n={})",
              kRingRegionSize, ring_overflow_hops_, kRingOverflowRegions,
              ring_overflow_count_);
        }
      } else {
        // Every safe region is full (> kRingRegionSize * (1 +
        // kRingOverflowRegions) bytes in one frame): wrap to the current
        // region's start. A visual glitch beats a crash.
        if (!ring_wrap_logged_) {
          ring_wrap_logged_ = true;
          REXLOG_ERROR(
              "nrhi-vulkan: root-constant ring exhausted past every overflow region "
              "({} bytes/frame), wrapping - constants may glitch this session",
              kRingRegionSize * (1 + kRingOverflowRegions));
        }
        ring_region_offset_ = 0;
      }
    }
    uint32_t offset = ring_region_base_ + ring_region_offset_;
    std::memcpy(ring_mapping_ + offset, data, size_bytes);
    ring_region_offset_ += aligned;
    ring_frame_bytes_ += aligned;
    return offset;
  }

  // Ensures the texture's tracked layout equals the state's layout, pushing
  // an internal transition when the app relied on a D3D12 initial state (or
  // hasn't flushed a queued barrier - the caller submits right after).
  void EnsureLayout(NrTextureVulkan* t, const StateInfo& dst) {
    if (t->current_layout == dst.layout) return;
    const bool undefined = t->current_layout == VK_IMAGE_LAYOUT_UNDEFINED;
    cp_->PushImageMemoryBarrier(
        t->image, t->WholeRange(),
        undefined ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        dst.stage_mask, undefined ? 0 : VK_ACCESS_MEMORY_WRITE_BIT, dst.access_mask,
        t->current_layout, dst.layout);
    t->current_layout = dst.layout;
  }

  // Attachment (identity-swizzle) view of mip 0, created lazily.
  VkImageView GetAttachmentView(NrTextureVulkan* t) {
    if (t->attachment_view != VK_NULL_HANDLE) return t->attachment_view;
    VkImageViewCreateInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    info.image = t->image;
    info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    info.format = t->vk_format;
    info.subresourceRange.aspectMask = t->aspect;
    info.subresourceRange.baseMipLevel = 0;
    info.subresourceRange.levelCount = 1;
    info.subresourceRange.baseArrayLayer = 0;
    info.subresourceRange.layerCount = 1;
    if (vulkan_device_->functions().vkCreateImageView(vulkan_device_->device(), &info, nullptr,
                                                      &t->attachment_view) != VK_SUCCESS) {
      REXLOG_ERROR("nrhi-vulkan: attachment view creation failed");
      t->attachment_view = VK_NULL_HANDLE;
    }
    t->attachment_view_owned = true;
    return t->attachment_view;
  }

  VkRenderPass GetRenderPass(VkFormat color_format, VkFormat depth_format, uint32_t samples,
                             bool color_clear, bool depth_clear) {
    RenderPassKey key{color_format, depth_format, samples, color_clear, depth_clear};
    auto it = render_passes_.find(key);
    if (it != render_passes_.end()) return it->second;

    VkAttachmentDescription attachments[2] = {};
    VkAttachmentReference color_ref = {};
    VkAttachmentReference depth_ref = {};
    uint32_t attachment_count = 0;
    if (color_format != VK_FORMAT_UNDEFINED) {
      VkAttachmentDescription& a = attachments[attachment_count];
      a.format = color_format;
      a.samples = VkSampleCountFlagBits(samples);
      a.loadOp = color_clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
      a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
      a.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
      a.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
      a.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
      a.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
      color_ref.attachment = attachment_count;
      color_ref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
      ++attachment_count;
    }
    if (depth_format != VK_FORMAT_UNDEFINED) {
      VkAttachmentDescription& a = attachments[attachment_count];
      a.format = depth_format;
      a.samples = VkSampleCountFlagBits(samples);
      a.loadOp = depth_clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
      a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
      a.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
      a.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
      a.initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
      a.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
      depth_ref.attachment = attachment_count;
      depth_ref.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
      ++attachment_count;
    }

    VkSubpassDescription subpass = {};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    if (color_format != VK_FORMAT_UNDEFINED) {
      subpass.colorAttachmentCount = 1;
      subpass.pColorAttachments = &color_ref;
    }
    if (depth_format != VK_FORMAT_UNDEFINED) {
      subpass.pDepthStencilAttachment = &depth_ref;
    }

    // Explicit external dependencies ordering attachment access between
    // back-to-back passes on the same target (D3D12 needed no barrier for
    // consecutive passes in the same resource state).
    const VkPipelineStageFlags attachment_stages =
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    const VkAccessFlags attachment_access =
        VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    VkSubpassDependency dependencies[2] = {};
    dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[0].dstSubpass = 0;
    dependencies[0].srcStageMask = attachment_stages;
    dependencies[0].dstStageMask = attachment_stages;
    dependencies[0].srcAccessMask = attachment_access;
    dependencies[0].dstAccessMask = attachment_access;
    dependencies[1].srcSubpass = 0;
    dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[1].srcStageMask = attachment_stages;
    dependencies[1].dstStageMask = attachment_stages;
    dependencies[1].srcAccessMask = attachment_access;
    dependencies[1].dstAccessMask = attachment_access;

    VkRenderPassCreateInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    info.attachmentCount = attachment_count;
    info.pAttachments = attachments;
    info.subpassCount = 1;
    info.pSubpasses = &subpass;
    info.dependencyCount = 2;
    info.pDependencies = dependencies;
    VkRenderPass render_pass = VK_NULL_HANDLE;
    if (vulkan_device_->functions().vkCreateRenderPass(vulkan_device_->device(), &info, nullptr,
                                                       &render_pass) != VK_SUCCESS) {
      REXLOG_ERROR("nrhi-vulkan: render pass creation failed");
      return VK_NULL_HANDLE;
    }
    render_passes_.emplace(key, render_pass);
    return render_pass;
  }

  VkFramebuffer GetFramebuffer(VkRenderPass render_pass, VkImageView color_view,
                               VkImageView depth_view, uint32_t width, uint32_t height) {
    // Framebuffers are compatible across load-op variants of the same
    // attachment set, so the key omits the render pass.
    FramebufferKey key{color_view, depth_view, width, height};
    auto it = framebuffers_.find(key);
    if (it != framebuffers_.end()) return it->second;
    VkImageView attachments[2];
    uint32_t attachment_count = 0;
    if (color_view != VK_NULL_HANDLE) attachments[attachment_count++] = color_view;
    if (depth_view != VK_NULL_HANDLE) attachments[attachment_count++] = depth_view;
    VkFramebufferCreateInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    info.renderPass = render_pass;
    info.attachmentCount = attachment_count;
    info.pAttachments = attachments;
    info.width = width;
    info.height = height;
    info.layers = 1;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    if (vulkan_device_->functions().vkCreateFramebuffer(vulkan_device_->device(), &info, nullptr,
                                                        &framebuffer) != VK_SUCCESS) {
      REXLOG_ERROR("nrhi-vulkan: framebuffer creation failed");
      return VK_NULL_HANDLE;
    }
    framebuffers_.emplace(key, framebuffer);
    return framebuffer;
  }

  // Retires all cached framebuffers referencing the view. Render thread.
  void RetireFramebuffersForView(VkImageView view) {
    if (view == VK_NULL_HANDLE) return;
    const uint64_t submission = cp_->GetCurrentSubmission();
    for (auto it = framebuffers_.begin(); it != framebuffers_.end();) {
      if (it->first.color_view == view || it->first.depth_view == view) {
        std::lock_guard<std::mutex> lock(mutex_);
        RetiredObject r;
        r.submission = submission;
        r.framebuffer = it->second;
        retired_.push_back(r);
        it = framebuffers_.erase(it);
      } else {
        ++it;
      }
    }
  }

  // Cached set-0 descriptor set for (layout, bound buffer tuple). The ring
  // UBO backs the kConstants binding and any never-bound buffer param (so a
  // draw before every SetConstantBuffer still has valid descriptors).
  VkDescriptorSet GetSet0(NrBindingLayoutVulkan* layout, NrBufferVulkan* const* buffers) {
    Set0Key key{};
    key.layout = layout;
    key.count = layout->buffer_binding_count;
    VkBuffer vk_buffers[nrhi::kMaxBindingParams];
    for (uint32_t i = 0; i < key.count; ++i) {
      vk_buffers[i] = buffers[i] != nullptr ? buffers[i]->buffer : ring_buffer_;
      key.buffers[i] = vk_buffers[i];
    }
    auto it = set0_sets_.find(key);
    if (it != set0_sets_.end()) return it->second.set;

    NrProfScope prof_scope(prof_.set0_miss);
    SetEntry entry;
    if (!AllocateDescriptorSet(layout->set0_layout, &entry)) return VK_NULL_HANDLE;
    VkDescriptorBufferInfo buffer_infos[nrhi::kMaxBindingParams];
    VkWriteDescriptorSet writes[nrhi::kMaxBindingParams];
    uint32_t write_count = 0;
    for (uint32_t i = 0; i < layout->param_count; ++i) {
      const NrBindingLayoutVulkan::ParamInfo& p = layout->params[i];
      if (p.kind == nrhi::BindingParamKind::kTextureTable) continue;
      VkDescriptorBufferInfo& bi = buffer_infos[write_count];
      bi.buffer = vk_buffers[p.set0_binding];
      bi.offset = 0;  // the bind offset rides the dynamic offset
      bi.range = p.descriptor_type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC ? kSsboBindRange
                                                                                : kUboBindRange;
      VkWriteDescriptorSet& w = writes[write_count];
      w = {};
      w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      w.dstSet = entry.set;
      w.dstBinding = p.set0_binding;
      w.descriptorCount = 1;
      w.descriptorType = p.descriptor_type;
      w.pBufferInfo = &bi;
      ++write_count;
    }
    vulkan_device_->functions().vkUpdateDescriptorSets(vulkan_device_->device(), write_count,
                                                       writes, 0, nullptr);
    set0_sets_.emplace(key, entry);
    return entry.set;
  }

  // Cached texture-table set for (per-param set layout, ordered view tuple)
  // with white-fallback substitution already applied by the caller.
  VkDescriptorSet GetTableSet(VkDescriptorSetLayout set_layout,
                              NrTextureViewVulkan* const* views, uint32_t count) {
    TableKey key{};
    key.layout = set_layout;
    key.count = count;
    for (uint32_t i = 0; i < count; ++i) {
      key.views[i] = views[i];
    }
    auto it = table_sets_.find(key);
    if (it != table_sets_.end()) return it->second.set;

    NrProfScope prof_scope(prof_.table_miss);
    SetEntry entry;
    if (!AllocateDescriptorSet(set_layout, &entry)) return VK_NULL_HANDLE;
    VkDescriptorImageInfo image_infos[nrhi::kMaxTextureTableSize];
    VkWriteDescriptorSet writes[nrhi::kMaxTextureTableSize];
    for (uint32_t i = 0; i < count; ++i) {
      image_infos[i] = {};
      image_infos[i].imageView = views[i] != nullptr ? views[i]->view : white_view_;
      image_infos[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
      writes[i] = {};
      writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      writes[i].dstSet = entry.set;
      writes[i].dstBinding = i;
      writes[i].descriptorCount = 1;
      writes[i].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
      writes[i].pImageInfo = &image_infos[i];
    }
    vulkan_device_->functions().vkUpdateDescriptorSets(vulkan_device_->device(), count, writes, 0,
                                                       nullptr);
    table_sets_.emplace(key, entry);
    return entry.set;
  }

  // Both topology variants are available before the scene releases the
  // shader modules used to create them.
  VkPipeline GetPipelineVariant(NrPipelineVulkan* p, nrhi::PrimitiveTopology topology) {
    return topology == nrhi::PrimitiveTopology::kTriangleList ? p->list_pipeline
                                                              : p->strip_pipeline;
  }

  std::vector<NrTextureVulkan*>& pending_clear_textures() { return pending_clear_textures_; }

 private:
  struct RetiredObject {
    uint64_t submission = 0;
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation buffer_allocation = VK_NULL_HANDLE;
    VkImage image = VK_NULL_HANDLE;
    VmaAllocation image_allocation = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkPipeline pipelines[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkShaderModule shader_module = VK_NULL_HANDLE;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    VkDescriptorSet descriptor_set = VK_NULL_HANDLE;
    VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
  };

  struct SetEntry {
    VkDescriptorSet set = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
  };

  struct RenderPassKey {
    VkFormat color_format;
    VkFormat depth_format;
    uint32_t samples;
    bool color_clear;
    bool depth_clear;
    bool operator<(const RenderPassKey& o) const {
      if (color_format != o.color_format) return color_format < o.color_format;
      if (depth_format != o.depth_format) return depth_format < o.depth_format;
      if (samples != o.samples) return samples < o.samples;
      if (color_clear != o.color_clear) return color_clear < o.color_clear;
      return depth_clear < o.depth_clear;
    }
  };

  struct FramebufferKey {
    VkImageView color_view;
    VkImageView depth_view;
    uint32_t width;
    uint32_t height;
    bool operator<(const FramebufferKey& o) const {
      if (color_view != o.color_view) return color_view < o.color_view;
      if (depth_view != o.depth_view) return depth_view < o.depth_view;
      if (width != o.width) return width < o.width;
      return height < o.height;
    }
  };

  struct Set0Key {
    const NrBindingLayoutVulkan* layout;
    VkBuffer buffers[nrhi::kMaxBindingParams];
    uint32_t count;
    bool operator<(const Set0Key& o) const {
      if (layout != o.layout) return layout < o.layout;
      if (count != o.count) return count < o.count;
      for (uint32_t i = 0; i < count; ++i) {
        if (buffers[i] != o.buffers[i]) return buffers[i] < o.buffers[i];
      }
      return false;
    }
  };

  struct TableKey {
    VkDescriptorSetLayout layout;
    NrTextureViewVulkan* views[nrhi::kMaxTextureTableSize];
    uint32_t count;
    bool operator<(const TableKey& o) const {
      if (layout != o.layout) return layout < o.layout;
      if (count != o.count) return count < o.count;
      for (uint32_t i = 0; i < count; ++i) {
        if (views[i] != o.views[i]) return views[i] < o.views[i];
      }
      return false;
    }
  };

  bool AllocateDescriptorSet(VkDescriptorSetLayout layout, SetEntry* out) {
    const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device_->functions();
    const VkDevice device = vulkan_device_->device();
    for (int attempt = 0; attempt < 2; ++attempt) {
      if (!descriptor_pools_.empty()) {
        VkDescriptorSetAllocateInfo info = {};
        info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        info.descriptorPool = descriptor_pools_.back();
        info.descriptorSetCount = 1;
        info.pSetLayouts = &layout;
        VkDescriptorSet set = VK_NULL_HANDLE;
        if (dfn.vkAllocateDescriptorSets(device, &info, &set) == VK_SUCCESS) {
          out->set = set;
          out->pool = descriptor_pools_.back();
          return true;
        }
      }
      // Grow: sets are freed individually on retirement, so pools need the
      // free-descriptor-set flag. Immutable-sampler bindings still consume
      // SAMPLER pool capacity.
      VkDescriptorPoolSize sizes[4] = {
          {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 4096},
          {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 1024},
          {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 16384},
          {VK_DESCRIPTOR_TYPE_SAMPLER, 2048},
      };
      VkDescriptorPoolCreateInfo pool_info = {};
      pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
      pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
      pool_info.maxSets = 4096;
      pool_info.poolSizeCount = 4;
      pool_info.pPoolSizes = sizes;
      VkDescriptorPool pool = VK_NULL_HANDLE;
      if (dfn.vkCreateDescriptorPool(device, &pool_info, nullptr, &pool) != VK_SUCCESS) {
        REXLOG_ERROR("nrhi-vulkan: descriptor pool creation failed");
        return false;
      }
      descriptor_pools_.push_back(pool);
    }
    REXLOG_ERROR("nrhi-vulkan: descriptor set allocation failed");
    return false;
  }

  void RetireDescriptorSetLocked(const SetEntry& entry, uint64_t submission) {
    RetiredObject r;
    r.submission = submission;
    r.descriptor_set = entry.set;
    r.descriptor_pool = entry.pool;
    retired_.push_back(r);
  }

  void DrainRetired(uint64_t completed, size_t max_objects = SIZE_MAX) {
    NrProfScope prof_scope(prof_.drain);
    const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device_->functions();
    const VkDevice device = vulkan_device_->device();
    size_t destroyed = 0;
    std::erase_if(retired_, [&](const RetiredObject& r) {
      if (r.submission >= completed || destroyed >= max_objects) return false;
      ++destroyed;
      if (r.framebuffer != VK_NULL_HANDLE) dfn.vkDestroyFramebuffer(device, r.framebuffer, nullptr);
      if (r.view != VK_NULL_HANDLE) dfn.vkDestroyImageView(device, r.view, nullptr);
      for (VkPipeline pipeline : r.pipelines) {
        if (pipeline != VK_NULL_HANDLE) dfn.vkDestroyPipeline(device, pipeline, nullptr);
      }
      if (r.shader_module != VK_NULL_HANDLE) {
        dfn.vkDestroyShaderModule(device, r.shader_module, nullptr);
      }
      if (r.descriptor_set != VK_NULL_HANDLE) {
        dfn.vkFreeDescriptorSets(device, r.descriptor_pool, 1, &r.descriptor_set);
      }
      if (r.image != VK_NULL_HANDLE) vmaDestroyImage(allocator_, r.image, r.image_allocation);
      if (r.buffer != VK_NULL_HANDLE) vmaDestroyBuffer(allocator_, r.buffer, r.buffer_allocation);
      return true;
    });
  }

  void DestroyGuestOutputWrapper(NrTextureVulkan* wrapper) {
    RetireFramebuffersForView(wrapper->attachment_view);
    if (cmd_.rt_color_ == wrapper) cmd_.rt_color_ = nullptr;
    std::erase(pending_clear_textures_, wrapper);
    delete wrapper;  // image + view are presenter-owned
  }

  // 1x1 white RGBA8 fallback for unbound texture-table tail entries. The
  // upload is recorded into the deferred command buffer on the first
  // BeginFrame (a submission is guaranteed open there), through the CP
  // barrier queue - the simplest arrangement that needs no extra queue
  // submission machinery.
  void EnsureWhiteTexture() {
    if (white_view_ != VK_NULL_HANDLE) return;
    VkImageCreateInfo image_info = {};
    image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = VK_FORMAT_R8G8B8A8_UNORM;
    image_info.extent = {1, 1, 1};
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VmaAllocationCreateInfo alloc_info = {};
    alloc_info.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    if (vmaCreateImage(allocator_, &image_info, &alloc_info, &white_image_, &white_allocation_,
                       nullptr) != VK_SUCCESS) {
      REXLOG_ERROR("nrhi-vulkan: white fallback image creation failed");
      return;
    }

    VkBufferCreateInfo staging_info = {};
    staging_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    staging_info.size = 4;
    staging_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    staging_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VmaAllocationCreateInfo staging_alloc_info = {};
    staging_alloc_info.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT |
                               VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
    staging_alloc_info.usage = VMA_MEMORY_USAGE_AUTO;
    staging_alloc_info.requiredFlags =
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    VmaAllocationInfo staging_result = {};
    if (vmaCreateBuffer(allocator_, &staging_info, &staging_alloc_info, &white_staging_,
                        &white_staging_allocation_, &staging_result) != VK_SUCCESS) {
      REXLOG_ERROR("nrhi-vulkan: white fallback staging buffer creation failed");
      return;
    }
    const uint32_t white = 0xFFFFFFFFu;
    std::memcpy(staging_result.pMappedData, &white, sizeof(white));

    VkImageSubresourceRange range = ui::vulkan::util::InitializeSubresourceRange();
    cp_->PushImageMemoryBarrier(white_image_, range, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                VK_PIPELINE_STAGE_TRANSFER_BIT, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                                VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    cp_->SubmitBarriers(true);
    VkBufferImageCopy region = {};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = {1, 1, 1};
    cp_->deferred_command_buffer().CmdVkCopyBufferToImage(
        white_staging_, white_image_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    cp_->PushImageMemoryBarrier(white_image_, range, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    cp_->SubmitBarriers(true);

    VkImageViewCreateInfo view_info = {};
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.image = white_image_;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = VK_FORMAT_R8G8B8A8_UNORM;
    view_info.subresourceRange = range;
    if (vulkan_device_->functions().vkCreateImageView(vulkan_device_->device(), &view_info,
                                                      nullptr, &white_view_) != VK_SUCCESS) {
      REXLOG_ERROR("nrhi-vulkan: white fallback view creation failed");
      white_view_ = VK_NULL_HANDLE;
    }
  }

  VkPipeline BuildPipeline(const NrPipelineVulkan& p, VkPrimitiveTopology topology) {
    NrProfScope prof_scope(prof_.pipeline_build);
    const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device_->functions();
    const ui::vulkan::VulkanDevice::Properties& props = vulkan_device_->properties();

    VkPipelineShaderStageCreateInfo stages[2] = {};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = p.vs_module;
    stages[0].pName = p.vs_entry.c_str();
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = p.ps_module;
    stages[1].pName = p.ps_entry.c_str();

    VkVertexInputBindingDescription binding = {};
    VkVertexInputAttributeDescription attributes[16] = {};
    VkPipelineVertexInputStateCreateInfo vertex_input = {};
    vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    if (!p.input_elements.empty() && p.input_elements.size() <= 16) {
      binding.binding = 0;
      binding.stride = p.vertex_stride;
      binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
      for (size_t i = 0; i < p.input_elements.size(); ++i) {
        const nrhi::InputElementDesc& e = p.input_elements[i];
        attributes[i].location = e.location;
        attributes[i].binding = 0;
        attributes[i].format = ToVkFormat(e.format);
        attributes[i].offset = e.byte_offset;
      }
      vertex_input.vertexBindingDescriptionCount = 1;
      vertex_input.pVertexBindingDescriptions = &binding;
      vertex_input.vertexAttributeDescriptionCount = uint32_t(p.input_elements.size());
      vertex_input.pVertexAttributeDescriptions = attributes;
    }

    VkPipelineInputAssemblyStateCreateInfo input_assembly = {};
    input_assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    input_assembly.topology = topology;

    VkPipelineViewportStateCreateInfo viewport_state = {};
    viewport_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport_state.viewportCount = 1;
    viewport_state.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo raster = {};
    raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    // depthClampEnable requires the depthClamp feature; the SDK device
    // enables it when supported (Properties::depthClamp). If unsupported,
    // fall back to clipping and note it once.
    if (!p.depth_clip) {
      if (props.depthClamp) {
        raster.depthClampEnable = VK_TRUE;
      } else {
        static bool depth_clamp_unsupported_logged = false;
        if (!depth_clamp_unsupported_logged) {
          depth_clamp_unsupported_logged = true;
          REXLOG_ERROR(
              "nrhi-vulkan: pipeline wants depth clamp (depth_clip=false) but the device has no "
              "depthClamp feature; using depth clipping");
        }
      }
    }
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = p.cull == nrhi::CullMode::kFront   ? VK_CULL_MODE_FRONT_BIT
                      : p.cull == nrhi::CullMode::kBack ? VK_CULL_MODE_BACK_BIT
                                                        : VK_CULL_MODE_NONE;
    // Matches D3D FrontCounterClockwise=FALSE under the negative-viewport
    // y-flip: the geometry->pixel mapping equals D3D, so the screen-space
    // winding is unchanged.
    raster.frontFace = VK_FRONT_FACE_CLOCKWISE;
    raster.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo multisample = {};
    multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VkSampleCountFlagBits(p.sample_count);

    VkPipelineDepthStencilStateCreateInfo depth_stencil = {};
    depth_stencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depth_stencil.depthTestEnable = p.depth.test_enable;
    depth_stencil.depthWriteEnable = p.depth.write_enable;
    depth_stencil.depthCompareOp = ToVkCompare(p.depth.func);

    VkPipelineColorBlendAttachmentState blend_attachment = {};
    blend_attachment.blendEnable = p.blend.enable;
    blend_attachment.srcColorBlendFactor = ToVkBlend(p.blend.src);
    blend_attachment.dstColorBlendFactor = ToVkBlend(p.blend.dst);
    blend_attachment.colorBlendOp = ToVkBlendOp(p.blend.op);
    blend_attachment.srcAlphaBlendFactor = ToVkBlend(p.blend.src_alpha);
    blend_attachment.dstAlphaBlendFactor = ToVkBlend(p.blend.dst_alpha);
    blend_attachment.alphaBlendOp = ToVkBlendOp(p.blend.op_alpha);
    blend_attachment.colorWriteMask = p.blend.write_mask;  // RGBA bits match VkColorComponentFlags
    VkPipelineColorBlendStateCreateInfo blend = {};
    blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    if (p.rtv_format != Format::kUnknown) {
      blend.attachmentCount = 1;
      blend.pAttachments = &blend_attachment;
    }

    const VkDynamicState dynamic_states[2] = {VK_DYNAMIC_STATE_VIEWPORT,
                                              VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic = {};
    dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dynamic_states;

    VkRenderPass render_pass = GetRenderPass(
        p.rtv_format != Format::kUnknown ? ToVkFormat(p.rtv_format) : VK_FORMAT_UNDEFINED,
        p.dsv_format != Format::kUnknown ? ToVkFormat(p.dsv_format) : VK_FORMAT_UNDEFINED,
        p.sample_count, false, false);
    if (render_pass == VK_NULL_HANDLE) return VK_NULL_HANDLE;

    VkGraphicsPipelineCreateInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    info.stageCount = 2;
    info.pStages = stages;
    info.pVertexInputState = &vertex_input;
    info.pInputAssemblyState = &input_assembly;
    info.pViewportState = &viewport_state;
    info.pRasterizationState = &raster;
    info.pMultisampleState = &multisample;
    info.pDepthStencilState = &depth_stencil;
    info.pColorBlendState = &blend;
    info.pDynamicState = &dynamic;
    info.layout = p.pipeline_layout;
    info.renderPass = render_pass;
    info.subpass = 0;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkResult result = dfn.vkCreateGraphicsPipelines(vulkan_device_->device(), VK_NULL_HANDLE, 1,
                                                    &info, nullptr, &pipeline);
    if (result != VK_SUCCESS) {
      REXLOG_ERROR("nrhi-vulkan: vkCreateGraphicsPipelines failed ({}, vs '{}' ps '{}')",
                   int32_t(result), p.vs_entry, p.ps_entry);
      return VK_NULL_HANDLE;
    }
    return pipeline;
  }

  friend class NrCmdVulkan;

  VulkanCommandProcessor* cp_;
  const ui::vulkan::VulkanDevice* vulkan_device_;
  NrCmdVulkan cmd_;

  VmaAllocator allocator_ = VK_NULL_HANDLE;

  // Root-constant ring UBO.
  VkBuffer ring_buffer_ = VK_NULL_HANDLE;
  VmaAllocation ring_allocation_ = VK_NULL_HANDLE;
  uint8_t* ring_mapping_ = nullptr;
  uint64_t frame_index_ = 0;
  uint32_t ring_region_base_ = 0;
  uint32_t ring_region_offset_ = 0;
  // Overflow regions consumed by the recording frame / total overflow events
  // this session (see AllocateConstantSlice).
  uint32_t ring_overflow_hops_ = 0;
  uint64_t ring_overflow_count_ = 0;
  // Constant bytes allocated by the recording frame and the session peak,
  // reported with the periodic memory log to keep region sizing honest.
  uint32_t ring_frame_bytes_ = 0;
  uint32_t ring_peak_bytes_ = 0;
  bool ring_wrap_logged_ = false;

  // White 1x1 fallback.
  VkImage white_image_ = VK_NULL_HANDLE;
  VmaAllocation white_allocation_ = VK_NULL_HANDLE;
  VkImageView white_view_ = VK_NULL_HANDLE;
  VkBuffer white_staging_ = VK_NULL_HANDLE;
  VmaAllocation white_staging_allocation_ = VK_NULL_HANDLE;

  // mutex_ guards retired_ and the live_* sets (creation and deferred
  // destruction are thread-safe); the caches below are render-thread-only.
  std::mutex mutex_;
  std::vector<RetiredObject> retired_;
  // Every app-created object still alive (inserted at Create*, erased at
  // DestroyDeferred). The app holds its caches (meshes, texture store,
  // rings, PSOs) across the whole session and has no device-teardown
  // notification, so whatever is still here at device destruction is
  // released by the destructor sweep; child objects must not outlive
  // vkDestroyDevice (VUID-vkDestroyDevice-device-05137).
  std::unordered_set<NrBufferVulkan*> live_buffers_;
  std::unordered_set<NrTextureVulkan*> live_textures_;
  std::unordered_set<NrTextureViewVulkan*> live_views_;
  std::unordered_set<NrPipelineVulkan*> live_pipelines_;
  std::unordered_set<NrShaderVulkan*> live_shaders_;

  // Views destroyed since the last FlushDissolvedViews (render thread only).
  // The objects stay allocated until the flush so stale descriptor-set cache
  // keys can never collide with a newly created view at the same address.
  std::vector<NrTextureViewVulkan*> dissolved_views_;

  std::map<RenderPassKey, VkRenderPass> render_passes_;
  std::map<FramebufferKey, VkFramebuffer> framebuffers_;
  std::vector<VkDescriptorPool> descriptor_pools_;
  std::map<Set0Key, SetEntry> set0_sets_;
  std::map<TableKey, SetEntry> table_sets_;
  std::vector<NrBindingLayoutVulkan*> layouts_;  // owned; no destroy API on the interface
  std::map<VkImage, NrTextureVulkan*> guest_outputs_;
  std::vector<NrTextureVulkan*> pending_clear_textures_;
};

// ---------------------------------------------------------------------------
// NrCmdVulkan implementation.
// ---------------------------------------------------------------------------

void NrCmdVulkan::ResetFrameState() {
  layout_ = nullptr;
  pipeline_ = nullptr;
  bound_pipeline_ = VK_NULL_HANDLE;
  topology_ = nrhi::PrimitiveTopology::kTriangleList;
  std::memset(constants_shadow_, 0, sizeof(constants_shadow_));
  constants_dirty_ = false;
  constants_ring_offset_ = ~0u;
  std::memset(set0_buffers_, 0, sizeof(set0_buffers_));
  std::memset(set0_offsets_, 0, sizeof(set0_offsets_));
  set0_rebind_needed_ = true;
  set0_bound_ = VK_NULL_HANDLE;
  std::memset(table_views_, 0, sizeof(table_views_));
  std::memset(table_dirty_, 0, sizeof(table_dirty_));
  rt_color_ = nullptr;
  rt_depth_ = nullptr;
  if (render_pass_open_) {
    // Should have been closed by the previous EndFrame - close in the
    // current command stream is impossible (different submission), so just
    // drop the flag.
    REXLOG_ERROR("nrhi-vulkan: render pass leaked across frames (state dropped)");
    render_pass_open_ = false;
  }
}

void NrCmdVulkan::EndFrame() {
  EndRenderPassIfOpen();
  // Flush pending clears no draw consumed (D3D12 clears were immediate -
  // preserve the observable "cleared even without a subsequent draw"
  // behavior).
  auto& pending = device->pending_clear_textures();
  while (!pending.empty()) {
    NrTextureVulkan* t = pending.back();
    if (!t->pending_clear) {
      pending.pop_back();
      continue;
    }
    // FlushPendingClear via Barrier-style path: an empty CLEAR pass.
    const bool is_depth = t->aspect == VK_IMAGE_ASPECT_DEPTH_BIT;
    device->EnsureLayout(t, ToStateInfo(is_depth ? ResourceState::kDepthWrite
                                                 : ResourceState::kRenderTarget));
    device->cp()->SubmitBarriers(true);
    VkRenderPass render_pass = device->GetRenderPass(
        is_depth ? VK_FORMAT_UNDEFINED : t->vk_format, is_depth ? t->vk_format : VK_FORMAT_UNDEFINED,
        t->desc.sample_count, !is_depth, is_depth);
    VkImageView view = device->GetAttachmentView(t);
    if (render_pass != VK_NULL_HANDLE && view != VK_NULL_HANDLE) {
      VkFramebuffer framebuffer =
          device->GetFramebuffer(render_pass, is_depth ? VK_NULL_HANDLE : view,
                                 is_depth ? view : VK_NULL_HANDLE, t->desc.width, t->desc.height);
      if (framebuffer != VK_NULL_HANDLE) {
        VkClearValue clear_value;
        if (is_depth) {
          clear_value.depthStencil = {t->clear_depth, 0};
        } else {
          std::memcpy(clear_value.color.float32, t->clear_color, sizeof(t->clear_color));
        }
        VkRenderPassBeginInfo begin_info = {};
        begin_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        begin_info.renderPass = render_pass;
        begin_info.framebuffer = framebuffer;
        begin_info.renderArea.extent = {t->desc.width, t->desc.height};
        begin_info.clearValueCount = 1;
        begin_info.pClearValues = &clear_value;
        DeferredCommandBuffer& cmd = device->cp()->deferred_command_buffer();
        cmd.CmdVkBeginRenderPass(&begin_info, VK_SUBPASS_CONTENTS_INLINE);
        cmd.CmdVkEndRenderPass();
      }
    }
    t->pending_clear = false;
    pending.pop_back();
  }
  // Submit remaining queued barriers (including the app's release of the
  // guest output back to kGuestOutput, if it did not flush explicitly).
  device->cp()->SubmitBarriers(true);
}

void NrCmdVulkan::EndRenderPassIfOpen() {
  if (!render_pass_open_) return;
  device->cp()->deferred_command_buffer().CmdVkEndRenderPass();
  render_pass_open_ = false;
}

bool NrCmdVulkan::EnsureRenderPassOpen() {
  if (render_pass_open_) return true;
  NrProfScope prof_scope(device->prof_.pass_open);
  if (rt_color_ == nullptr && rt_depth_ == nullptr) {
    static bool no_targets_logged = false;
    if (!no_targets_logged) {
      no_targets_logged = true;
      REXLOG_ERROR("nrhi-vulkan: draw with no render targets latched");
    }
    return false;
  }

  // Bring attachments to attachment layout (covers D3D12 initial-state
  // reliance) and record every queued barrier before the pass begins.
  if (rt_color_ != nullptr) {
    device->EnsureLayout(rt_color_, ToStateInfo(ResourceState::kRenderTarget));
  }
  if (rt_depth_ != nullptr) {
    device->EnsureLayout(rt_depth_, ToStateInfo(ResourceState::kDepthWrite));
  }
  device->cp()->SubmitBarriers(true);

  const bool color_clear = rt_color_ != nullptr && rt_color_->pending_clear;
  const bool depth_clear = rt_depth_ != nullptr && rt_depth_->pending_clear;
  const uint32_t samples = rt_color_ != nullptr ? rt_color_->desc.sample_count
                                                : rt_depth_->desc.sample_count;
  VkRenderPass render_pass = device->GetRenderPass(
      rt_color_ != nullptr ? rt_color_->vk_format : VK_FORMAT_UNDEFINED,
      rt_depth_ != nullptr ? rt_depth_->vk_format : VK_FORMAT_UNDEFINED, samples, color_clear,
      depth_clear);
  if (render_pass == VK_NULL_HANDLE) return false;
  VkImageView color_view =
      rt_color_ != nullptr ? device->GetAttachmentView(rt_color_) : VK_NULL_HANDLE;
  VkImageView depth_view =
      rt_depth_ != nullptr ? device->GetAttachmentView(rt_depth_) : VK_NULL_HANDLE;
  const uint32_t width = rt_color_ != nullptr ? rt_color_->desc.width : rt_depth_->desc.width;
  const uint32_t height = rt_color_ != nullptr ? rt_color_->desc.height : rt_depth_->desc.height;
  VkFramebuffer framebuffer =
      device->GetFramebuffer(render_pass, color_view, depth_view, width, height);
  if (framebuffer == VK_NULL_HANDLE) return false;

  VkClearValue clear_values[2];
  uint32_t clear_value_count = 0;
  if (rt_color_ != nullptr) {
    std::memcpy(clear_values[clear_value_count].color.float32, rt_color_->clear_color,
                sizeof(rt_color_->clear_color));
    ++clear_value_count;
  }
  if (rt_depth_ != nullptr) {
    clear_values[clear_value_count].depthStencil = {rt_depth_->clear_depth, 0};
    ++clear_value_count;
  }
  if (color_clear) {
    rt_color_->pending_clear = false;
    std::erase(device->pending_clear_textures(), rt_color_);
  }
  if (depth_clear) {
    rt_depth_->pending_clear = false;
    std::erase(device->pending_clear_textures(), rt_depth_);
  }

  // Draws require the render area to be the full target extent.
  VkRenderPassBeginInfo begin_info = {};
  begin_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
  begin_info.renderPass = render_pass;
  begin_info.framebuffer = framebuffer;
  begin_info.renderArea.extent = {width, height};
  begin_info.clearValueCount = clear_value_count;
  begin_info.pClearValues = clear_values;
  device->cp()->deferred_command_buffer().CmdVkBeginRenderPass(&begin_info,
                                                               VK_SUBPASS_CONTENTS_INLINE);
  render_pass_open_ = true;
  return true;
}

bool NrCmdVulkan::EnsureDrawState() {
  if (layout_ == nullptr || pipeline_ == nullptr) return false;
  if (!EnsureRenderPassOpen()) return false;
  DeferredCommandBuffer& cmd = device->cp()->deferred_command_buffer();

  VkPipeline variant = device->GetPipelineVariant(pipeline_, topology_);
  if (variant == VK_NULL_HANDLE) return false;
  if (variant != bound_pipeline_) {
    cmd.CmdVkBindPipeline(VK_PIPELINE_BIND_POINT_GRAPHICS, variant);
    bound_pipeline_ = variant;
  }

  // Root constants: copy the shadow block into a fresh ring slice when
  // dirty; the slice offset rides the kConstants dynamic offset.
  if (layout_->constants_param >= 0 &&
      (constants_dirty_ || constants_ring_offset_ == ~0u)) {
    constants_ring_offset_ = device->AllocateConstantSlice(
        constants_shadow_, std::max(layout_->constants_size_bytes, 4u));
    constants_dirty_ = false;
    set0_rebind_needed_ = true;
  }

  if (set0_rebind_needed_) {
    // Bound even with zero buffer params - the immutable samplers live in
    // set 0 too.
    VkDescriptorSet set0 = device->GetSet0(layout_, set0_buffers_);
    if (set0 != VK_NULL_HANDLE) {
      uint32_t dynamic_offsets[nrhi::kMaxBindingParams];
      for (uint32_t i = 0; i < layout_->param_count; ++i) {
        const NrBindingLayoutVulkan::ParamInfo& p = layout_->params[i];
        if (p.kind == nrhi::BindingParamKind::kTextureTable) continue;
        dynamic_offsets[p.set0_binding] =
            p.kind == nrhi::BindingParamKind::kConstants
                ? (constants_ring_offset_ != ~0u ? constants_ring_offset_ : 0)
                : set0_offsets_[p.set0_binding];
      }
      cmd.CmdVkBindDescriptorSets(VK_PIPELINE_BIND_POINT_GRAPHICS, layout_->pipeline_layout, 0, 1,
                                  &set0, layout_->buffer_binding_count, dynamic_offsets);
      set0_bound_ = set0;
      set0_rebind_needed_ = false;
    }
  }

  for (uint32_t i = 0; i < layout_->param_count; ++i) {
    const NrBindingLayoutVulkan::ParamInfo& p = layout_->params[i];
    if (p.kind != nrhi::BindingParamKind::kTextureTable) continue;
    if (!table_dirty_[p.table_index]) continue;
    VkDescriptorSet set = device->GetTableSet(layout_->table_layouts[p.table_index],
                                              table_views_[p.table_index], p.table_size);
    if (set != VK_NULL_HANDLE) {
      cmd.CmdVkBindDescriptorSets(VK_PIPELINE_BIND_POINT_GRAPHICS, layout_->pipeline_layout,
                                  p.set_index, 1, &set, 0, nullptr);
      table_dirty_[p.table_index] = false;
    }
  }
  return true;
}

void NrCmdVulkan::SetBindingLayout(nrhi::BindingLayout* layout) {
  layout_ = static_cast<NrBindingLayoutVulkan*>(layout);
  // D3D12 root-signature semantics: all root/binding state resets.
  std::memset(constants_shadow_, 0, sizeof(constants_shadow_));
  constants_dirty_ = true;
  constants_ring_offset_ = ~0u;
  std::memset(set0_buffers_, 0, sizeof(set0_buffers_));
  std::memset(set0_offsets_, 0, sizeof(set0_offsets_));
  set0_rebind_needed_ = true;
  set0_bound_ = VK_NULL_HANDLE;
  std::memset(table_views_, 0, sizeof(table_views_));
  if (layout_ != nullptr) {
    for (uint32_t i = 0; i < layout_->table_count; ++i) {
      table_dirty_[i] = true;
    }
  }
}

void NrCmdVulkan::SetPipeline(nrhi::Pipeline* pipeline) {
  pipeline_ = static_cast<NrPipelineVulkan*>(pipeline);
}

void NrCmdVulkan::SetRootConstants(uint32_t param, uint32_t count, const void* values,
                                   uint32_t dest_offset_in_values) {
  (void)param;
  if (dest_offset_in_values + count > 64) return;
  std::memcpy(constants_shadow_ + dest_offset_in_values, values, count * sizeof(uint32_t));
  constants_dirty_ = true;
}

void NrCmdVulkan::SetConstantBuffer(uint32_t param, nrhi::Buffer* buffer, uint64_t offset) {
  if (layout_ == nullptr || param >= layout_->param_count) return;
  const NrBindingLayoutVulkan::ParamInfo& p = layout_->params[param];
  set0_buffers_[p.set0_binding] = static_cast<NrBufferVulkan*>(buffer);
  set0_offsets_[p.set0_binding] = uint32_t(offset);
  set0_rebind_needed_ = true;
}

void NrCmdVulkan::SetBufferSrv(uint32_t param, nrhi::Buffer* buffer, uint64_t offset) {
  SetConstantBuffer(param, buffer, offset);
}

void NrCmdVulkan::SetTexture(uint32_t param, nrhi::TextureView* view) {
  SetTextures(param, &view, 1);
}

void NrCmdVulkan::SetTexturePair(uint32_t param, nrhi::TextureView* first,
                                 nrhi::TextureView* second) {
  nrhi::TextureView* views[2] = {first, second};
  SetTextures(param, views, 2);
}

void NrCmdVulkan::SetTextures(uint32_t param, nrhi::TextureView* const* views, uint32_t count) {
  if (layout_ == nullptr || param >= layout_->param_count) return;
  const NrBindingLayoutVulkan::ParamInfo& p = layout_->params[param];
  if (p.kind != nrhi::BindingParamKind::kTextureTable) return;
  // Consecutive draws mostly rebind the same tuple (shadow atlas, cube map,
  // white fallback); leaving the table clean skips the per-draw descriptor
  // set lookup and rebind entirely.
  bool changed = false;
  for (uint32_t i = 0; i < p.table_size; ++i) {
    // Missing tail entries (table declares N, bound M<N) and null entries
    // fall back to the backend's 1x1 white.
    NrTextureViewVulkan* view =
        i < count ? static_cast<NrTextureViewVulkan*>(views[i]) : nullptr;
    if (table_views_[p.table_index][i] != view) {
      table_views_[p.table_index][i] = view;
      changed = true;
    }
  }
  if (changed) {
    table_dirty_[p.table_index] = true;
  }
}

void NrCmdVulkan::SetRenderTargets(nrhi::Texture* color, nrhi::Texture* depth) {
  EndRenderPassIfOpen();
  rt_color_ = static_cast<NrTextureVulkan*>(color);
  rt_depth_ = static_cast<NrTextureVulkan*>(depth);
}

void NrCmdVulkan::LatchPendingClear(NrTextureVulkan* t, const float* color4, float depth) {
  if (color4 != nullptr) {
    std::memcpy(t->clear_color, color4, sizeof(t->clear_color));
  } else {
    t->clear_depth = depth;
  }
  if (!t->pending_clear) {
    t->pending_clear = true;
    device->pending_clear_textures().push_back(t);
  }
}

void NrCmdVulkan::ClearRenderTarget(nrhi::Texture* color, const float color4[4]) {
  auto* t = static_cast<NrTextureVulkan*>(color);
  if (render_pass_open_ && t == rt_color_) {
    VkClearAttachment attachment = {};
    attachment.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    attachment.colorAttachment = 0;
    std::memcpy(attachment.clearValue.color.float32, color4, sizeof(float) * 4);
    VkClearRect rect = {};
    rect.rect.extent = {t->desc.width, t->desc.height};
    rect.layerCount = 1;
    device->cp()->deferred_command_buffer().CmdVkClearAttachments(1, &attachment, 1, &rect);
    return;
  }
  LatchPendingClear(t, color4, 0.0f);
}

void NrCmdVulkan::ClearDepth(nrhi::Texture* depth, float value) {
  auto* t = static_cast<NrTextureVulkan*>(depth);
  if (render_pass_open_ && t == rt_depth_) {
    VkClearAttachment attachment = {};
    attachment.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    attachment.clearValue.depthStencil = {value, 0};
    VkClearRect rect = {};
    rect.rect.extent = {t->desc.width, t->desc.height};
    rect.layerCount = 1;
    device->cp()->deferred_command_buffer().CmdVkClearAttachments(1, &attachment, 1, &rect);
    return;
  }
  LatchPendingClear(t, nullptr, value);
}

void NrCmdVulkan::SetViewport(const nrhi::Viewport& viewport) {
  // D3D top-left origin -> negative-height Vulkan viewport (core 1.1
  // maintenance1 behavior): unchanged shaders, unchanged pixel math.
  VkViewport vk_viewport;
  vk_viewport.x = viewport.x;
  vk_viewport.y = viewport.y + viewport.height;
  vk_viewport.width = viewport.width;
  vk_viewport.height = -viewport.height;
  vk_viewport.minDepth = viewport.min_depth;
  vk_viewport.maxDepth = viewport.max_depth;
  device->cp()->deferred_command_buffer().CmdVkSetViewport(0, 1, &vk_viewport);
}

void NrCmdVulkan::SetScissor(const nrhi::Rect& rect) {
  VkRect2D scissor;
  scissor.offset.x = rect.left;
  scissor.offset.y = rect.top;
  scissor.extent.width = uint32_t(std::max(rect.right - rect.left, 0));
  scissor.extent.height = uint32_t(std::max(rect.bottom - rect.top, 0));
  device->cp()->deferred_command_buffer().CmdVkSetScissor(0, 1, &scissor);
}

void NrCmdVulkan::SetVertexBuffer(nrhi::Buffer* buffer, uint64_t offset, uint32_t size_bytes,
                                  uint32_t stride) {
  (void)size_bytes;  // the pipeline's vertex stride governs fetch; Vulkan
                     // binds (buffer, offset) only
  (void)stride;
  VkBuffer vk_buffer = static_cast<NrBufferVulkan*>(buffer)->buffer;
  VkDeviceSize vk_offset = offset;
  device->cp()->deferred_command_buffer().CmdVkBindVertexBuffers(0, 1, &vk_buffer, &vk_offset);
}

void NrCmdVulkan::SetIndexBuffer(nrhi::Buffer* buffer, uint64_t offset, uint32_t size_bytes) {
  (void)size_bytes;
  device->cp()->deferred_command_buffer().CmdVkBindIndexBuffer(
      static_cast<NrBufferVulkan*>(buffer)->buffer, offset, VK_INDEX_TYPE_UINT16);
}

void NrCmdVulkan::SetPrimitiveTopology(nrhi::PrimitiveTopology topology) {
  topology_ = topology;
}

void NrCmdVulkan::Draw(uint32_t vertex_count, uint32_t start_vertex) {
  if (!EnsureDrawState()) return;
  device->cp()->deferred_command_buffer().CmdVkDraw(vertex_count, 1, start_vertex, 0);
}

void NrCmdVulkan::DrawIndexed(uint32_t index_count, uint32_t start_index, int32_t base_vertex) {
  if (!EnsureDrawState()) return;
  device->cp()->deferred_command_buffer().CmdVkDrawIndexed(index_count, 1, start_index,
                                                           base_vertex, 0);
}

void NrCmdVulkan::CopyBufferToTexture(nrhi::Texture* dst, uint32_t mip, uint32_t array_slice,
                                      nrhi::Buffer* src, uint64_t src_offset, uint32_t row_pitch,
                                      uint32_t width, uint32_t height, uint32_t depth) {
  NrProfScope prof_scope(device->prof_.copies);
  auto* t = static_cast<NrTextureVulkan*>(dst);
  auto* b = static_cast<NrBufferVulkan*>(src);
  EndRenderPassIfOpen();
  // Cover the D3D12 initial-state reliance (created-in-COPY_DEST textures
  // are copied into without an explicit barrier), then record every queued
  // barrier so the copy is ordered after them.
  device->EnsureLayout(t, ToStateInfo(ResourceState::kCopyDest));
  device->cp()->SubmitBarriers(true);

  const uint32_t block_width = nrhi::FormatBlockWidth(t->desc.format);
  const uint32_t bytes_per_block = FormatBytesPerBlock(t->desc.format);
  // The given width/height are the D3D12 block-aligned footprint dims;
  // Vulkan wants the actual (unaligned) extent for small BC mips - clamp
  // against the mip chain dims from the texture desc.
  const uint32_t mip_width = std::max(t->desc.width >> mip, 1u);
  const uint32_t mip_height = std::max(t->desc.height >> mip, 1u);
  VkBufferImageCopy region = {};
  region.bufferOffset = src_offset;
  // bufferRowLength is in TEXELS (a block-multiple for BC).
  region.bufferRowLength = row_pitch / bytes_per_block * block_width;
  region.bufferImageHeight = height;  // texels, block-aligned as given
  region.imageSubresource.aspectMask = t->aspect;
  region.imageSubresource.mipLevel = mip;
  region.imageSubresource.baseArrayLayer = array_slice;
  region.imageSubresource.layerCount = 1;
  region.imageExtent.width = std::min(width, mip_width);
  region.imageExtent.height = std::min(height, mip_height);
  region.imageExtent.depth = depth;
  device->cp()->deferred_command_buffer().CmdVkCopyBufferToImage(
      b->buffer, t->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
}

void NrCmdVulkan::CopyTextureToBuffer(nrhi::Buffer* dst, uint64_t dst_offset, uint32_t row_pitch,
                                      nrhi::Texture* src, uint32_t mip, uint32_t width,
                                      uint32_t height) {
  NrProfScope prof_scope(device->prof_.copies);
  auto* t = static_cast<NrTextureVulkan*>(src);
  auto* b = static_cast<NrBufferVulkan*>(dst);
  EndRenderPassIfOpen();
  device->EnsureLayout(t, ToStateInfo(ResourceState::kCopySource));
  device->cp()->SubmitBarriers(true);

  const uint32_t block_width = nrhi::FormatBlockWidth(t->desc.format);
  const uint32_t bytes_per_block = FormatBytesPerBlock(t->desc.format);
  VkBufferImageCopy region = {};
  region.bufferOffset = dst_offset;
  region.bufferRowLength = row_pitch / bytes_per_block * block_width;
  region.bufferImageHeight = height;
  region.imageSubresource.aspectMask = t->aspect;
  region.imageSubresource.mipLevel = mip;
  region.imageSubresource.baseArrayLayer = 0;
  region.imageSubresource.layerCount = 1;
  region.imageExtent.width = width;
  region.imageExtent.height = height;
  region.imageExtent.depth = 1;
  device->cp()->deferred_command_buffer().CmdVkCopyImageToBuffer(
      t->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, b->buffer, 1, &region);
  // Order the transfer write against mapped host reads: the readback gate is
  // the completed-submission counter, the memory dependency is this barrier
  // (submitted with the next barrier flush / frame end).
  device->cp()->PushBufferMemoryBarrier(b->buffer, dst_offset, uint64_t(row_pitch) * height,
                                        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                                        VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT);
}

void NrCmdVulkan::Barrier(nrhi::Texture* texture, ResourceState before, ResourceState after) {
  auto* t = static_cast<NrTextureVulkan*>(texture);
  const StateInfo dst = ToStateInfo(after);
  // A latched clear must not be lost when the texture leaves its attachment
  // state without a draw having consumed it ("clear then barrier"): flush it
  // as an empty CLEAR pass now, before the transition is queued.
  if (t->pending_clear && after != ResourceState::kRenderTarget &&
      after != ResourceState::kDepthWrite) {
    EndRenderPassIfOpen();
    const bool is_depth = t->aspect == VK_IMAGE_ASPECT_DEPTH_BIT;
    device->EnsureLayout(t, ToStateInfo(is_depth ? ResourceState::kDepthWrite
                                                 : ResourceState::kRenderTarget));
    device->cp()->SubmitBarriers(true);
    VkRenderPass render_pass = device->GetRenderPass(
        is_depth ? VK_FORMAT_UNDEFINED : t->vk_format,
        is_depth ? t->vk_format : VK_FORMAT_UNDEFINED, t->desc.sample_count, !is_depth, is_depth);
    VkImageView view = device->GetAttachmentView(t);
    if (render_pass != VK_NULL_HANDLE && view != VK_NULL_HANDLE) {
      VkFramebuffer framebuffer =
          device->GetFramebuffer(render_pass, is_depth ? VK_NULL_HANDLE : view,
                                 is_depth ? view : VK_NULL_HANDLE, t->desc.width, t->desc.height);
      if (framebuffer != VK_NULL_HANDLE) {
        VkClearValue clear_value;
        if (is_depth) {
          clear_value.depthStencil = {t->clear_depth, 0};
        } else {
          std::memcpy(clear_value.color.float32, t->clear_color, sizeof(t->clear_color));
        }
        VkRenderPassBeginInfo begin_info = {};
        begin_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        begin_info.renderPass = render_pass;
        begin_info.framebuffer = framebuffer;
        begin_info.renderArea.extent = {t->desc.width, t->desc.height};
        begin_info.clearValueCount = 1;
        begin_info.pClearValues = &clear_value;
        DeferredCommandBuffer& cmd = device->cp()->deferred_command_buffer();
        cmd.CmdVkBeginRenderPass(&begin_info, VK_SUBPASS_CONTENTS_INLINE);
        cmd.CmdVkEndRenderPass();
      }
    }
    t->pending_clear = false;
    std::erase(device->pending_clear_textures(), t);
  }
  if (t->current_layout == dst.layout) {
    // Layout already matches (e.g. the internal ensure-layout ran) - still
    // queue an execution/memory dependency between the states.
    const StateInfo src = ToStateInfo(before);
    device->cp()->PushImageMemoryBarrier(t->image, t->WholeRange(), src.stage_mask, dst.stage_mask,
                                         src.access_mask, dst.access_mask, dst.layout, dst.layout);
    return;
  }
  const StateInfo src = ToStateInfo(before);
  const bool undefined = t->current_layout == VK_IMAGE_LAYOUT_UNDEFINED;
  device->cp()->PushImageMemoryBarrier(
      t->image, t->WholeRange(),
      undefined ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : src.stage_mask, dst.stage_mask,
      undefined ? 0 : src.access_mask, dst.access_mask,
      // The tracked layout is the authoritative oldLayout: first-use
      // transitions come from UNDEFINED and discard.
      t->current_layout, dst.layout);
  t->current_layout = dst.layout;
}

void NrCmdVulkan::FlushBarriers() {
  NrProfScope prof_scope(device->prof_.flush_barriers);
  // My render passes live in the same deferred command buffer the CP records
  // barriers into - close mine BEFORE the barrier commands are recorded (the
  // CP's SubmitBarriers only closes its own tracked pass).
  EndRenderPassIfOpen();
  device->cp()->SubmitBarriers(true);
}

rex::perf::DrawBucket ProfileStageBucket(nrhi::ProfileStage stage) {
  switch (stage) {
    case nrhi::ProfileStage::kCommit:
      return rex::perf::DrawBucket::kNativeCommit;
    case nrhi::ProfileStage::kShadow:
      return rex::perf::DrawBucket::kNativeShadow;
    case nrhi::ProfileStage::kStaticSun:
      return rex::perf::DrawBucket::kNativeSun;
    case nrhi::ProfileStage::kMain:
      return rex::perf::DrawBucket::kNativeMain;
    case nrhi::ProfileStage::kResolve:
      return rex::perf::DrawBucket::kNativeResolve;
    case nrhi::ProfileStage::kAmbientOcclusion:
      return rex::perf::DrawBucket::kNativeAo;
    case nrhi::ProfileStage::kSsr:
      return rex::perf::DrawBucket::kNativeSsr;
    case nrhi::ProfileStage::kVolumetrics:
      return rex::perf::DrawBucket::kNativeVol;
    case nrhi::ProfileStage::kBloom:
      return rex::perf::DrawBucket::kNativeBloom;
    case nrhi::ProfileStage::k2d:
      return rex::perf::DrawBucket::kNative2d;
    case nrhi::ProfileStage::kTail:
      return rex::perf::DrawBucket::kNativeTail;
  }
  return rex::perf::DrawBucket::kNativeScene;
}

void NrCmdVulkan::ProfileRegion(nrhi::ProfileStage stage) {
  // Region semantics: beginning a bucket closes the active one, and the CP
  // closes the last bucket at frame end - so the kTail mark records the
  // present/submission tail as its own span.
  device->cp()->BeginGpuTimestampedRegion(ProfileStageBucket(stage));
}

}  // namespace

// ---------------------------------------------------------------------------
// Public factory / per-frame entry points.
// ---------------------------------------------------------------------------

nrhi::Device* CreateNativeRhiDevice(VulkanCommandProcessor* command_processor) {
  return new NrDeviceVulkan(command_processor);
}

void DestroyNativeRhiDevice(nrhi::Device* device) {
  delete static_cast<NrDeviceVulkan*>(device);
}

nrhi::Cmd* NativeRhiBeginFrame(nrhi::Device* device, VkImage guest_output_image,
                               VkImageView guest_output_image_view,
                               bool guest_output_ever_written_previously, uint32_t width,
                               uint32_t height, nrhi::Texture** guest_output_out) {
  return static_cast<NrDeviceVulkan*>(device)->BeginFrame(
      guest_output_image, guest_output_image_view, guest_output_ever_written_previously, width,
      height, guest_output_out);
}

void NativeRhiEndFrame(nrhi::Device* device) {
  static_cast<NrDeviceVulkan*>(device)->EndFrame();
}

}  // namespace rex::graphics::vulkan
