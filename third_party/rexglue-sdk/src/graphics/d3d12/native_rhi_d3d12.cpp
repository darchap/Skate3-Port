// D3D12 implementation of the native-render RHI. A thin passthrough onto the
// D3D12 command processor's deferred command list, barrier queue and
// submission counters; the descriptor-field choices here reproduce exactly
// what the scene renderer set when it talked to D3D12 directly, so migrating
// the scene onto the RHI is behavior-preserving on this backend.

#include <rex/graphics/d3d12/native_rhi_d3d12.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <condition_variable>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

#include <d3dcompiler.h>
#include <dxgi1_4.h>

#include <rex/graphics/d3d12/command_processor.h>
#include <rex/logging.h>
#include <rex/ui/d3d12/d3d12_provider.h>
#include <rex/ui/d3d12/d3d12_util.h>

namespace rex::graphics::d3d12 {
namespace {

using nrhi::Backend;
using nrhi::Format;
using nrhi::HeapKind;
using nrhi::ResourceState;
using nrhi::TextureKind;

// ---- Shader bytecode cache ------------------------------------------------
// D3DCompile of the scene uber shader costs whole seconds and runs on the
// render thread during pipeline creation, which holds the first presented
// frame (a launch black screen). Compiled DXBC is content-addressed on disk
// (see nrhi::SetShaderBytecodeCacheDirectory): the key hashes everything
// that affects codegen, so source edits miss naturally and entries never go
// stale. Every failure path falls back to compiling.

uint64_t Fnv1a64(const void* data, size_t size, uint64_t hash) {
  const uint8_t* p = static_cast<const uint8_t*>(data);
  for (size_t i = 0; i < size; ++i) {
    hash ^= p[i];
    hash *= 1099511628211ull;
  }
  return hash;
}

std::filesystem::path ShaderCachePath(const nrhi::ShaderDesc& desc,
                                      const char* target) {
  const char* dir = nrhi::GetShaderBytecodeCacheDirectory();
  if (dir == nullptr || dir[0] == '\0') {
    return {};
  }
  // Two independent FNV-1a streams form a 128-bit key; the terminating NUL
  // fed with each field keeps concatenations unambiguous.
  uint64_t h1 = 14695981039346656037ull;
  uint64_t h2 = 0x84222325CBF29CE4ull;
  const auto feed_str = [&](const char* s) {
    const size_t n = std::strlen(s) + 1;
    h1 = Fnv1a64(s, n, h1);
    h2 = Fnv1a64(s, n, h2);
  };
  feed_str("nrhi-dxbc-1");  // cache format tag
  feed_str(target);
  feed_str(desc.entry_point != nullptr ? desc.entry_point : "");
  if (desc.macros != nullptr) {
    for (uint32_t i = 0; desc.macros[i].name != nullptr; ++i) {
      feed_str(desc.macros[i].name);
      feed_str(desc.macros[i].value != nullptr ? desc.macros[i].value : "");
    }
  }
  feed_str(desc.hlsl_source != nullptr ? desc.hlsl_source : "");
  char name[48];
  std::snprintf(name, sizeof(name), "%016llx%016llx.dxbc",
                static_cast<unsigned long long>(h1),
                static_cast<unsigned long long>(h2));
  return std::filesystem::path(dir) / name;
}

ID3DBlob* TryLoadCachedBlob(const std::filesystem::path& path) {
  std::ifstream f(path, std::ios::binary | std::ios::ate);
  if (!f.is_open()) {
    return nullptr;
  }
  const std::streamoff size = f.tellg();
  if (size < 8) {
    return nullptr;
  }
  ID3DBlob* blob = nullptr;
  if (FAILED(D3DCreateBlob(static_cast<SIZE_T>(size), &blob))) {
    return nullptr;
  }
  f.seekg(0);
  f.read(static_cast<char*>(blob->GetBufferPointer()),
         static_cast<std::streamsize>(size));
  // The DXBC container magic rejects truncated or foreign files.
  if (!f.good() || std::memcmp(blob->GetBufferPointer(), "DXBC", 4) != 0) {
    blob->Release();
    return nullptr;
  }
  return blob;
}

void StoreCachedBlob(const std::filesystem::path& path, ID3DBlob* blob) {
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  // Write-then-rename so a crash mid-write never leaves a truncated blob
  // under the final name (the loader's DXBC magic check is the second line
  // of defense). The per-process temp name keeps two instances compiling
  // the same shader from interleaving writes.
  std::filesystem::path tmp = path;
  tmp += ".tmp" + std::to_string(GetCurrentProcessId());
  {
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    if (!f.is_open()) {
      return;
    }
    f.write(static_cast<const char*>(blob->GetBufferPointer()),
            static_cast<std::streamsize>(blob->GetBufferSize()));
    if (!f.good()) {
      f.close();
      std::filesystem::remove(tmp, ec);
      return;
    }
  }
  std::filesystem::rename(tmp, path, ec);
  if (ec) {
    std::filesystem::remove(tmp, ec);
  }
}

DXGI_FORMAT ToDxgi(Format format) {
  switch (format) {
    case Format::kR8G8B8A8_UNORM:
      return DXGI_FORMAT_R8G8B8A8_UNORM;
    case Format::kR8G8B8A8_UINT:
      return DXGI_FORMAT_R8G8B8A8_UINT;
    case Format::kR8_UNORM:
      return DXGI_FORMAT_R8_UNORM;
    case Format::kR8G8_UNORM:
      return DXGI_FORMAT_R8G8_UNORM;
    case Format::kB5G6R5_UNORM:
      return DXGI_FORMAT_B5G6R5_UNORM;
    case Format::kR16G16_UNORM:
      return DXGI_FORMAT_R16G16_UNORM;
    case Format::kR10G10B10A2_UNORM:
      return DXGI_FORMAT_R10G10B10A2_UNORM;
    case Format::kR16G16B16A16_FLOAT:
      return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case Format::kR11G11B10_FLOAT:
      return DXGI_FORMAT_R11G11B10_FLOAT;
    case Format::kR32_FLOAT:
      return DXGI_FORMAT_R32_FLOAT;
    case Format::kR32G32_FLOAT:
      return DXGI_FORMAT_R32G32_FLOAT;
    case Format::kR32G32B32_FLOAT:
      return DXGI_FORMAT_R32G32B32_FLOAT;
    case Format::kR32G32B32A32_FLOAT:
      return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case Format::kD32_FLOAT:
      return DXGI_FORMAT_D32_FLOAT;
    case Format::kBC1_UNORM:
      return DXGI_FORMAT_BC1_UNORM;
    case Format::kBC2_UNORM:
      return DXGI_FORMAT_BC2_UNORM;
    case Format::kBC3_UNORM:
      return DXGI_FORMAT_BC3_UNORM;
    case Format::kBC4_UNORM:
      return DXGI_FORMAT_BC4_UNORM;
    case Format::kBC5_UNORM:
      return DXGI_FORMAT_BC5_UNORM;
    default:
      return DXGI_FORMAT_UNKNOWN;
  }
}

Format FromDxgi(DXGI_FORMAT format) {
  switch (format) {
    case DXGI_FORMAT_R8G8B8A8_UNORM:
      return Format::kR8G8B8A8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_UNORM:
      return Format::kR10G10B10A2_UNORM;
    default:
      return Format::kUnknown;
  }
}

D3D12_RESOURCE_STATES ToStates(ResourceState state,
                               D3D12_RESOURCE_STATES guest_output_state) {
  switch (state) {
    case ResourceState::kRenderTarget:
      return D3D12_RESOURCE_STATE_RENDER_TARGET;
    case ResourceState::kDepthWrite:
      return D3D12_RESOURCE_STATE_DEPTH_WRITE;
    case ResourceState::kPixelShaderResource:
      return D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    case ResourceState::kCopySource:
      return D3D12_RESOURCE_STATE_COPY_SOURCE;
    case ResourceState::kCopyDest:
      return D3D12_RESOURCE_STATE_COPY_DEST;
    case ResourceState::kGenericRead:
      return D3D12_RESOURCE_STATE_GENERIC_READ;
    case ResourceState::kGuestOutput:
      return guest_output_state;
    case ResourceState::kCommon:
    default:
      return D3D12_RESOURCE_STATE_COMMON;
  }
}

UINT8 ToShaderComponentMapping(const nrhi::Swizzle swizzle) {
  switch (swizzle) {
    case nrhi::Swizzle::kX:
      return D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_0;
    case nrhi::Swizzle::kY:
      return D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_1;
    case nrhi::Swizzle::kZ:
      return D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_2;
    case nrhi::Swizzle::kW:
      return D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_3;
    case nrhi::Swizzle::kZero:
      return D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_0;
    case nrhi::Swizzle::kOne:
    default:
      return D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_1;
  }
}

D3D12_BLEND ToBlend(nrhi::BlendFactor factor) {
  switch (factor) {
    case nrhi::BlendFactor::kZero:
      return D3D12_BLEND_ZERO;
    case nrhi::BlendFactor::kSrcAlpha:
      return D3D12_BLEND_SRC_ALPHA;
    case nrhi::BlendFactor::kInvSrcAlpha:
      return D3D12_BLEND_INV_SRC_ALPHA;
    case nrhi::BlendFactor::kOne:
    default:
      return D3D12_BLEND_ONE;
  }
}

D3D12_BLEND_OP ToBlendOp(nrhi::BlendOp op) {
  return op == nrhi::BlendOp::kMin ? D3D12_BLEND_OP_MIN : D3D12_BLEND_OP_ADD;
}

D3D12_COMPARISON_FUNC ToCompare(nrhi::CompareFunc func) {
  switch (func) {
    case nrhi::CompareFunc::kLess:
      return D3D12_COMPARISON_FUNC_LESS;
    case nrhi::CompareFunc::kLessEqual:
      return D3D12_COMPARISON_FUNC_LESS_EQUAL;
    case nrhi::CompareFunc::kAlways:
    default:
      return D3D12_COMPARISON_FUNC_ALWAYS;
  }
}

class NrDeviceD3D12;

class NrBufferD3D12 : public nrhi::Buffer {
 public:
  uint64_t size() const override { return size_; }

  ID3D12Resource* resource = nullptr;
  D3D12_GPU_VIRTUAL_ADDRESS gpu_va = 0;
  void* mapping = nullptr;
  uint64_t size_ = 0;
  HeapKind heap = HeapKind::kDefault;
};

class NrTextureD3D12 : public nrhi::Texture {
 public:
  uint32_t width() const override { return desc.width; }
  uint32_t height() const override { return desc.height; }
  Format format() const override { return desc.format; }

  ID3D12Resource* resource = nullptr;
  nrhi::TextureDesc desc;
  DXGI_FORMAT dxgi_format = DXGI_FORMAT_UNKNOWN;
  // RTV/DSV slots in the device's non-shader-visible heaps, ~0u when absent.
  uint32_t rtv_slot = ~0u;
  uint32_t dsv_slot = ~0u;
  // Guest-output wrappers don't own their resource's lifetime semantics the
  // same way (they hold a ref, but are never destroyed via DestroyDeferred).
  bool is_guest_output = false;
};

class NrTextureViewD3D12 : public nrhi::TextureView {
 public:
  // Slot in the device's non-shader-visible staging CBV_SRV_UAV heap; the
  // SRV descriptor is copied from here into shader-visible binding slots.
  uint32_t staging_slot = ~0u;
  NrTextureD3D12* texture = nullptr;
};

class NrBindingLayoutD3D12 : public nrhi::BindingLayout {
 public:
  ID3D12RootSignature* root_signature = nullptr;
};

class NrShaderD3D12 : public nrhi::Shader {
 public:
  ID3DBlob* blob = nullptr;
};

class NrPipelineD3D12 : public nrhi::Pipeline {
 public:
  ID3D12PipelineState* pso = nullptr;
};

// A simple slot allocator over a fixed-size descriptor heap with
// submission-keyed retirement (mirrors the scene renderer's original
// srv_next/srv_free/retired_srv_slots machinery).
struct SlotAllocator {
  uint32_t capacity = 0;
  uint32_t next = 0;
  std::vector<uint32_t> free_list;
  // (first_slot, count, submission)
  std::vector<std::pair<std::pair<uint32_t, uint32_t>, uint64_t>> retired;

  bool Alloc(uint32_t count, uint32_t* out) {
    if (count == 1 && !free_list.empty()) {
      *out = free_list.back();
      free_list.pop_back();
      return true;
    }
    if (next + count <= capacity) {
      *out = next;
      next += count;
      return true;
    }
    return false;
  }
  void Retire(uint32_t first, uint32_t count, uint64_t submission) {
    retired.emplace_back(std::make_pair(first, count), submission);
  }
  void Drain(uint64_t completed) {
    std::erase_if(retired, [&](const auto& entry) {
      if (entry.second < completed) {
        for (uint32_t i = 0; i < entry.first.second; ++i) {
          free_list.push_back(entry.first.first + i);
        }
        return true;
      }
      return false;
    });
  }
};

class NrCmdD3D12 : public nrhi::Cmd {
 public:
  void SetBindingLayout(nrhi::BindingLayout* layout) override;
  void SetPipeline(nrhi::Pipeline* pipeline) override;
  void SetRootConstants(uint32_t param, uint32_t count, const void* values,
                        uint32_t dest_offset_in_values) override;
  void SetConstantBuffer(uint32_t param, nrhi::Buffer* buffer,
                         uint64_t offset) override;
  void SetBufferSrv(uint32_t param, nrhi::Buffer* buffer,
                    uint64_t offset) override;
  void SetTexture(uint32_t param, nrhi::TextureView* view) override;
  void SetTexturePair(uint32_t param, nrhi::TextureView* first,
                      nrhi::TextureView* second) override;
  void SetTextures(uint32_t param, nrhi::TextureView* const* views,
                   uint32_t count) override;
  void SetRenderTargets(nrhi::Texture* color, nrhi::Texture* depth) override;
  void ClearRenderTarget(nrhi::Texture* color, const float color4[4]) override;
  void ClearDepth(nrhi::Texture* depth, float value) override;
  void SetViewport(const nrhi::Viewport& viewport) override;
  void SetScissor(const nrhi::Rect& rect) override;
  void SetVertexBuffer(nrhi::Buffer* buffer, uint64_t offset,
                       uint32_t size_bytes, uint32_t stride) override;
  void SetIndexBuffer(nrhi::Buffer* buffer, uint64_t offset,
                      uint32_t size_bytes) override;
  void SetPrimitiveTopology(nrhi::PrimitiveTopology topology) override;
  void Draw(uint32_t vertex_count, uint32_t start_vertex) override;
  void DrawIndexed(uint32_t index_count, uint32_t start_index,
                   int32_t base_vertex) override;
  void CopyBufferToTexture(nrhi::Texture* dst, uint32_t mip,
                           uint32_t array_slice, nrhi::Buffer* src,
                           uint64_t src_offset, uint32_t row_pitch,
                           uint32_t width, uint32_t height,
                           uint32_t depth) override;
  void CopyTextureToBuffer(nrhi::Buffer* dst, uint64_t dst_offset,
                           uint32_t row_pitch, nrhi::Texture* src,
                           uint32_t mip, uint32_t width,
                           uint32_t height) override;
  void Barrier(nrhi::Texture* texture, ResourceState before,
               ResourceState after) override;
  void FlushBarriers() override;
  void ProfileRegion(nrhi::ProfileStage stage) override;

  // Called at frame begin and on root-signature changes: the fresh deferred
  // command list / new root signature carries no bindings, so the latched
  // tuples must not suppress the first re-bind.
  void ResetFrameState() {
    std::memset(last_table_views_, 0, sizeof(last_table_views_));
    std::memset(last_table_counts_, 0, sizeof(last_table_counts_));
  }

  // Frame begin only (never on root-signature changes): each frame gets a
  // fresh timestamp-query range, so an open profile region must not carry an
  // end query across frames.
  void ResetProfileRegion() { profile_region_query_ = UINT32_MAX; }

  NrDeviceD3D12* device = nullptr;

 private:
  // Start query of the open ProfileRegion span, UINT32_MAX when none. Unlike
  // Vulkan's auto-closing regions, D3D12 buckets are explicit begin/end query
  // pairs; an unended pair would read stale readback memory, so every opened
  // region is closed by the next mark (kTail closes without opening).
  uint32_t profile_region_query_ = UINT32_MAX;

  void BindTextureTable(uint32_t param, NrTextureViewD3D12* const* views, uint32_t count);

  // Last tuple bound per root param: consecutive draws mostly rebind the
  // same views (shadow atlas, cube map, white fallback), skipping both the
  // binding-cache lookup and the root-table re-record. A zero count is the
  // empty/invalid state; real bindings always have count >= 1.
  NrTextureViewD3D12* last_table_views_[nrhi::kMaxBindingParams]
                                       [nrhi::kMaxTextureTableSize] = {};
  uint32_t last_table_counts_[nrhi::kMaxBindingParams] = {};
};

class NrDeviceD3D12 : public nrhi::Device {
 public:
  explicit NrDeviceD3D12(D3D12CommandProcessor* cp) : cp_(cp) {
    const ui::d3d12::D3D12Provider& provider = cp_->GetD3D12Provider();
    device_ = provider.GetDevice();
    cmd_.device = this;

    D3D12_DESCRIPTOR_HEAP_DESC heap_desc{};
    heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap_desc.NumDescriptors = kStagingViews;
    device_->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&staging_heap_));
    heap_desc.NumDescriptors = kShaderVisibleViews;
    heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    device_->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&srv_heap_));
    heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    heap_desc.NumDescriptors = kRtvSlots;
    device_->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&rtv_heap_));
    heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    heap_desc.NumDescriptors = kDsvSlots;
    device_->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&dsv_heap_));

    view_size_ =
        device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    rtv_size_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    dsv_size_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    staging_slots_.capacity = kStagingViews;
    srv_slots_.capacity = kShaderVisibleViews;
    rtv_slots_.capacity = kRtvSlots;
    dsv_slots_.capacity = kDsvSlots;
    // Resolve the device's adapter for the periodic VRAM telemetry
    // (process-local usage vs the OS-granted budget, the same numbers the
    // OS shows per process). Optional: the log line is skipped when any
    // step fails.
    IDXGIFactory4* factory4 = nullptr;
    IDXGIFactory2* factory = provider.GetDXGIFactory();
    if (factory != nullptr &&
        SUCCEEDED(factory->QueryInterface(IID_PPV_ARGS(&factory4)))) {
      factory4->EnumAdapterByLuid(device_->GetAdapterLuid(),
                                  IID_PPV_ARGS(&adapter3_));
      factory4->Release();
    }
    release_thread_ = std::thread([this] { ReleaseThreadMain(); });
  }

  ~NrDeviceD3D12() override {
    // Callers guarantee GPU idle; release everything. The release thread
    // drains its queue before exiting, so every retirement is released
    // before the heaps below go away.
    if (adapter3_ != nullptr) {
      adapter3_->Release();
    }
    FlushDissolvedViews();
    {
      std::lock_guard<std::mutex> lock(mutex_);
      DrainRetired(~0ull);
    }
    {
      std::lock_guard<std::mutex> lock(release_mutex_);
      release_exit_ = true;
    }
    release_cv_.notify_one();
    if (release_thread_.joinable()) {
      release_thread_.join();
    }
    for (auto& entry : guest_outputs_) {
      entry.second->resource->Release();
      delete entry.second;
    }
    if (staging_heap_) staging_heap_->Release();
    if (srv_heap_) srv_heap_->Release();
    if (rtv_heap_) rtv_heap_->Release();
    if (dsv_heap_) dsv_heap_->Release();
  }

  Backend backend() const override { return Backend::kD3D12; }

  nrhi::Buffer* CreateBuffer(const nrhi::BufferDesc& desc) override {
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = desc.size;
    rd.Height = 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    const D3D12_HEAP_PROPERTIES* heap = &ui::d3d12::util::kHeapPropertiesDefault;
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
    if (desc.heap == HeapKind::kUpload) {
      heap = &ui::d3d12::util::kHeapPropertiesUpload;
      state = D3D12_RESOURCE_STATE_GENERIC_READ;
    } else if (desc.heap == HeapKind::kReadback) {
      heap = &ui::d3d12::util::kHeapPropertiesReadback;
      state = D3D12_RESOURCE_STATE_COPY_DEST;
    }
    ID3D12Resource* resource = nullptr;
    if (FAILED(device_->CreateCommittedResource(
            heap, cp_->GetD3D12Provider().GetHeapFlagCreateNotZeroed(), &rd, state, nullptr,
            IID_PPV_ARGS(&resource)))) {
      REXLOG_ERROR("nrhi-d3d12: buffer creation failed ({} bytes)", desc.size);
      return nullptr;
    }
    auto* buffer = new NrBufferD3D12();
    buffer->resource = resource;
    buffer->gpu_va = resource->GetGPUVirtualAddress();
    buffer->size_ = desc.size;
    buffer->heap = desc.heap;
    return buffer;
  }

  nrhi::Texture* CreateTexture(const nrhi::TextureDesc& desc) override {
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = desc.kind == TextureKind::k3D ? D3D12_RESOURCE_DIMENSION_TEXTURE3D
                                                 : D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = desc.width;
    rd.Height = desc.height;
    rd.DepthOrArraySize =
        UINT16(desc.kind == TextureKind::kCube ? 6 : desc.kind == TextureKind::k3D ? desc.depth : 1);
    rd.MipLevels = UINT16(desc.mip_levels);
    rd.Format = ToDxgi(desc.format);
    rd.SampleDesc.Count = desc.sample_count;
    rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    D3D12_CLEAR_VALUE clear{};
    const D3D12_CLEAR_VALUE* clear_ptr = nullptr;
    if (desc.usage & nrhi::kTextureUsageRenderTarget) {
      rd.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
      clear.Format = rd.Format;
      std::memcpy(clear.Color, desc.clear_color, sizeof(clear.Color));
      clear_ptr = &clear;
    }
    if (desc.usage & nrhi::kTextureUsageDepthStencil) {
      // The depth resource is created typeless and viewed as D32_FLOAT /
      // R32_FLOAT; a typed depth SRV read the clear value on some drivers
      // (the photo-editor DoF smear); preserve the proven arrangement.
      rd.Flags |= D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
      if (desc.format == Format::kD32_FLOAT) {
        rd.Format = DXGI_FORMAT_R32_TYPELESS;
      }
      clear.Format = DXGI_FORMAT_D32_FLOAT;
      clear.DepthStencil.Depth = desc.clear_depth;
      clear_ptr = &clear;
    }
    ID3D12Resource* resource = nullptr;
    if (FAILED(device_->CreateCommittedResource(
            &ui::d3d12::util::kHeapPropertiesDefault,
            cp_->GetD3D12Provider().GetHeapFlagCreateNotZeroed(), &rd,
            ToStates(desc.initial_state, guest_output_state_), clear_ptr,
            IID_PPV_ARGS(&resource)))) {
      REXLOG_ERROR("nrhi-d3d12: texture creation failed ({}x{} fmt {})", desc.width,
                   desc.height, uint32_t(desc.format));
      return nullptr;
    }
    auto* texture = new NrTextureD3D12();
    texture->resource = resource;
    texture->desc = desc;
    texture->dxgi_format = ToDxgi(desc.format);
    if (desc.usage & nrhi::kTextureUsageRenderTarget) {
      std::lock_guard<std::mutex> lock(mutex_);
      if (rtv_slots_.Alloc(1, &texture->rtv_slot)) {
        device_->CreateRenderTargetView(resource, nullptr, RtvHandle(texture->rtv_slot));
      }
    }
    if (desc.usage & nrhi::kTextureUsageDepthStencil) {
      std::lock_guard<std::mutex> lock(mutex_);
      if (dsv_slots_.Alloc(1, &texture->dsv_slot)) {
        D3D12_DEPTH_STENCIL_VIEW_DESC dsv{};
        dsv.Format = DXGI_FORMAT_D32_FLOAT;
        dsv.ViewDimension = desc.sample_count > 1 ? D3D12_DSV_DIMENSION_TEXTURE2DMS
                                                  : D3D12_DSV_DIMENSION_TEXTURE2D;
        device_->CreateDepthStencilView(resource, &dsv, DsvHandle(texture->dsv_slot));
      }
    }
    return texture;
  }

  void* Map(nrhi::Buffer* buffer) override {
    auto* b = static_cast<NrBufferD3D12*>(buffer);
    if (b->mapping == nullptr) {
      const D3D12_RANGE no_read{};
      // Readback buffers are read through mapped memory; upload buffers are
      // write-only (pass an empty read range).
      b->resource->Map(0, b->heap == HeapKind::kReadback ? nullptr : &no_read, &b->mapping);
    }
    return b->mapping;
  }

  void Unmap(nrhi::Buffer* buffer) override {
    auto* b = static_cast<NrBufferD3D12*>(buffer);
    if (b->mapping != nullptr) {
      b->resource->Unmap(0, nullptr);
      b->mapping = nullptr;
    }
  }

  void InvalidateForRead(nrhi::Buffer*, uint64_t, uint64_t) override {}

  void DestroyDeferred(nrhi::Buffer* buffer) override {
    if (buffer == nullptr) return;
    auto* b = static_cast<NrBufferD3D12*>(buffer);
    std::lock_guard<std::mutex> lock(mutex_);
    retired_.emplace_back(RetiredObject{b->resource, nullptr, cp_->GetCurrentSubmission()});
    delete b;
  }

  void DestroyDeferred(nrhi::Texture* texture) override {
    if (texture == nullptr) return;
    auto* t = static_cast<NrTextureD3D12*>(texture);
    std::lock_guard<std::mutex> lock(mutex_);
    const uint64_t submission = cp_->GetCurrentSubmission();
    if (t->rtv_slot != ~0u) rtv_slots_.Retire(t->rtv_slot, 1, submission);
    if (t->dsv_slot != ~0u) dsv_slots_.Retire(t->dsv_slot, 1, submission);
    retired_.emplace_back(RetiredObject{t->resource, nullptr, submission});
    delete t;
  }

  void DestroyDeferred(nrhi::TextureView* view) override {
    if (view == nullptr) return;
    // Destruction is batched: the view object stays allocated (so its
    // address cannot be reused while stale binding-cache keys still hold it)
    // and FlushDissolvedViews sweeps the binding cache ONCE per frame for
    // the whole batch instead of once per destroyed view.
    dissolved_views_.push_back(static_cast<NrTextureViewD3D12*>(view));
  }

  // Render thread, once per frame (and at device destruction): retire every
  // shader-visible binding referencing a view destroyed since the last
  // flush, then retire the views themselves.
  void FlushDissolvedViews() {
    if (dissolved_views_.empty()) return;
    std::unordered_set<const NrTextureViewD3D12*> dissolved(dissolved_views_.begin(),
                                                            dissolved_views_.end());
    std::lock_guard<std::mutex> lock(mutex_);
    const uint64_t submission = cp_->GetCurrentSubmission();
    std::erase_if(bindings_, [&](auto& entry) {
      for (uint32_t i = 0; i < entry.first.count; ++i) {
        if (dissolved.count(entry.first.views[i]) != 0) {
          srv_slots_.Retire(entry.second.first_slot, entry.second.count, submission);
          return true;
        }
      }
      return false;
    });
    for (NrTextureViewD3D12* v : dissolved_views_) {
      staging_slots_.Retire(v->staging_slot, 1, submission);
      delete v;
    }
    dissolved_views_.clear();
  }

  void DestroyDeferred(nrhi::Pipeline* pipeline) override {
    if (pipeline == nullptr) return;
    auto* p = static_cast<NrPipelineD3D12*>(pipeline);
    std::lock_guard<std::mutex> lock(mutex_);
    retired_.emplace_back(RetiredObject{nullptr, p->pso, cp_->GetCurrentSubmission()});
    delete p;
  }

  void DestroyDeferred(nrhi::Shader* shader) override {
    if (shader == nullptr) return;
    auto* s = static_cast<NrShaderD3D12*>(shader);
    if (s->blob) s->blob->Release();
    delete s;
  }

  nrhi::TextureView* CreateTextureView(nrhi::Texture* texture,
                                       const nrhi::TextureViewDesc& desc) override {
    auto* t = static_cast<NrTextureD3D12*>(texture);
    uint32_t slot;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!staging_slots_.Alloc(1, &slot)) {
        REXLOG_ERROR("nrhi-d3d12: staging view heap exhausted");
        return nullptr;
      }
    }
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = desc.format != Format::kUnknown ? ToDxgi(desc.format)
                 : t->desc.format == Format::kD32_FLOAT ? DXGI_FORMAT_R32_FLOAT
                                                        : t->dxgi_format;
    srv.Shader4ComponentMapping = D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(
        ToShaderComponentMapping(desc.swizzle[0]), ToShaderComponentMapping(desc.swizzle[1]),
        ToShaderComponentMapping(desc.swizzle[2]), ToShaderComponentMapping(desc.swizzle[3]));
    const uint32_t mips = desc.mip_levels == ~0u
                              ? (t->desc.mip_levels - desc.base_mip)
                              : desc.mip_levels;
    switch (desc.dimension) {
      case nrhi::ViewDimension::k2DMS:
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMS;
        break;
      case nrhi::ViewDimension::kCube:
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
        srv.TextureCube.MostDetailedMip = desc.base_mip;
        srv.TextureCube.MipLevels = mips;
        break;
      case nrhi::ViewDimension::k3D:
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
        srv.Texture3D.MostDetailedMip = desc.base_mip;
        srv.Texture3D.MipLevels = mips;
        break;
      case nrhi::ViewDimension::k2D:
      default:
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Texture2D.MostDetailedMip = desc.base_mip;
        srv.Texture2D.MipLevels = mips;
        break;
    }
    device_->CreateShaderResourceView(t->resource, &srv, StagingHandle(slot));
    auto* view = new NrTextureViewD3D12();
    view->staging_slot = slot;
    view->texture = t;
    return view;
  }

  nrhi::BindingLayout* CreateBindingLayout(const nrhi::BindingLayoutDesc& desc) override {
    D3D12_ROOT_PARAMETER params[nrhi::kMaxBindingParams] = {};
    D3D12_DESCRIPTOR_RANGE ranges[nrhi::kMaxBindingParams] = {};
    for (uint32_t i = 0; i < desc.param_count; ++i) {
      const nrhi::BindingParamDesc& p = desc.params[i];
      D3D12_SHADER_VISIBILITY visibility =
          p.visibility == nrhi::Visibility::kVertex  ? D3D12_SHADER_VISIBILITY_VERTEX
          : p.visibility == nrhi::Visibility::kPixel ? D3D12_SHADER_VISIBILITY_PIXEL
                                                     : D3D12_SHADER_VISIBILITY_ALL;
      params[i].ShaderVisibility = visibility;
      switch (p.kind) {
        case nrhi::BindingParamKind::kConstants:
          params[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
          params[i].Constants.ShaderRegister = p.shader_register;
          params[i].Constants.Num32BitValues = p.count;
          break;
        case nrhi::BindingParamKind::kConstantBuffer:
          params[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
          params[i].Descriptor.ShaderRegister = p.shader_register;
          break;
        case nrhi::BindingParamKind::kBufferSrv:
          params[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
          params[i].Descriptor.ShaderRegister = p.shader_register;
          break;
        case nrhi::BindingParamKind::kTextureTable:
          ranges[i].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
          ranges[i].NumDescriptors = p.count;
          ranges[i].BaseShaderRegister = p.shader_register;
          params[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
          params[i].DescriptorTable.NumDescriptorRanges = 1;
          params[i].DescriptorTable.pDescriptorRanges = &ranges[i];
          break;
      }
    }
    D3D12_STATIC_SAMPLER_DESC samplers[nrhi::kMaxStaticSamplers] = {};
    for (uint32_t i = 0; i < desc.static_sampler_count; ++i) {
      const nrhi::StaticSamplerDesc& s = desc.static_samplers[i];
      samplers[i].Filter = s.filter == nrhi::Filter::kAnisotropic ? D3D12_FILTER_ANISOTROPIC
                           : s.filter == nrhi::Filter::kLinear
                               ? D3D12_FILTER_MIN_MAG_MIP_LINEAR
                               : D3D12_FILTER_MIN_MAG_MIP_POINT;
      samplers[i].MaxAnisotropy = s.max_anisotropy;
      D3D12_TEXTURE_ADDRESS_MODE mode = s.address == nrhi::AddressMode::kWrap
                                            ? D3D12_TEXTURE_ADDRESS_MODE_WRAP
                                            : D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
      samplers[i].AddressU = mode;
      samplers[i].AddressV = mode;
      samplers[i].AddressW = mode;
      samplers[i].MaxLOD = D3D12_FLOAT32_MAX;
      samplers[i].ShaderRegister = s.shader_register;
      samplers[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    }
    D3D12_ROOT_SIGNATURE_DESC rs{};
    rs.NumParameters = desc.param_count;
    rs.pParameters = params;
    rs.NumStaticSamplers = desc.static_sampler_count;
    rs.pStaticSamplers = samplers;
    rs.Flags = desc.allow_input_layout ? D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT
                                       : D3D12_ROOT_SIGNATURE_FLAG_NONE;
    ID3D12RootSignature* root_signature =
        ui::d3d12::util::CreateRootSignature(cp_->GetD3D12Provider(), rs);
    if (root_signature == nullptr) {
      REXLOG_ERROR("nrhi-d3d12: root signature creation failed");
      return nullptr;
    }
    auto* layout = new NrBindingLayoutD3D12();
    layout->root_signature = root_signature;
    return layout;
  }

  nrhi::Shader* CreateShader(const nrhi::ShaderDesc& desc) override {
    D3D_SHADER_MACRO macros[9] = {};
    uint32_t macro_count = 0;
    if (desc.macros != nullptr) {
      while (desc.macros[macro_count].name != nullptr && macro_count < 8) {
        macros[macro_count].Name = desc.macros[macro_count].name;
        macros[macro_count].Definition = desc.macros[macro_count].value;
        ++macro_count;
      }
    }
    const char* target = desc.stage == nrhi::ShaderStage::kVertex ? "vs_5_0" : "ps_5_0";
    const std::filesystem::path cache_path = ShaderCachePath(desc, target);
    ID3DBlob* blob = cache_path.empty() ? nullptr : TryLoadCachedBlob(cache_path);
    if (blob == nullptr) {
      ID3DBlob* errors = nullptr;
      const auto compile_start = std::chrono::steady_clock::now();
      HRESULT hr = D3DCompile(desc.hlsl_source, std::strlen(desc.hlsl_source),
                              desc.name != nullptr ? desc.name : "nrhi_shader",
                              macro_count != 0 ? macros : nullptr, nullptr, desc.entry_point,
                              target, 0, 0, &blob, &errors);
      if (FAILED(hr)) {
        REXLOG_ERROR("nrhi-d3d12: shader compile failed ({} {}): {}",
                     desc.name != nullptr ? desc.name : "?", desc.entry_point,
                     errors != nullptr ? static_cast<const char*>(errors->GetBufferPointer())
                                       : "no error blob");
        if (errors) errors->Release();
        return nullptr;
      }
      if (errors) errors->Release();
      const auto compile_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                  std::chrono::steady_clock::now() - compile_start)
                                  .count();
      if (!cache_path.empty()) {
        StoreCachedBlob(cache_path, blob);
      }
      REXLOG_INFO("nrhi-d3d12: compiled {} {} in {} ms{}",
                  desc.name != nullptr ? desc.name : "?", desc.entry_point, compile_ms,
                  cache_path.empty() ? "" : " (cached to disk)");
    }
    auto* shader = new NrShaderD3D12();
    shader->blob = blob;
    return shader;
  }

  nrhi::Pipeline* CreateGraphicsPipeline(const nrhi::GraphicsPipelineDesc& desc) override {
    auto* layout = static_cast<NrBindingLayoutD3D12*>(desc.layout);
    auto* vs = static_cast<NrShaderD3D12*>(desc.vs);
    auto* ps = static_cast<NrShaderD3D12*>(desc.ps);
    if (layout == nullptr || vs == nullptr || ps == nullptr) return nullptr;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = layout->root_signature;
    pd.VS = {vs->blob->GetBufferPointer(), vs->blob->GetBufferSize()};
    pd.PS = {ps->blob->GetBufferPointer(), ps->blob->GetBufferSize()};
    pd.BlendState.RenderTarget[0].BlendEnable = desc.blend.enable;
    pd.BlendState.RenderTarget[0].SrcBlend = ToBlend(desc.blend.src);
    pd.BlendState.RenderTarget[0].DestBlend = ToBlend(desc.blend.dst);
    pd.BlendState.RenderTarget[0].BlendOp = ToBlendOp(desc.blend.op);
    pd.BlendState.RenderTarget[0].SrcBlendAlpha = ToBlend(desc.blend.src_alpha);
    pd.BlendState.RenderTarget[0].DestBlendAlpha = ToBlend(desc.blend.dst_alpha);
    pd.BlendState.RenderTarget[0].BlendOpAlpha = ToBlendOp(desc.blend.op_alpha);
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask = desc.blend.write_mask;
    pd.SampleMask = UINT_MAX;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = desc.cull == nrhi::CullMode::kFront ? D3D12_CULL_MODE_FRONT
                                  : desc.cull == nrhi::CullMode::kBack ? D3D12_CULL_MODE_BACK
                                                                       : D3D12_CULL_MODE_NONE;
    pd.RasterizerState.DepthClipEnable = desc.depth_clip;
    pd.DepthStencilState.DepthEnable = desc.depth.test_enable;
    pd.DepthStencilState.DepthWriteMask = desc.depth.write_enable
                                              ? D3D12_DEPTH_WRITE_MASK_ALL
                                              : D3D12_DEPTH_WRITE_MASK_ZERO;
    pd.DepthStencilState.DepthFunc = ToCompare(desc.depth.func);
    D3D12_INPUT_ELEMENT_DESC elements[16] = {};
    if (desc.input_elements != nullptr && desc.input_element_count != 0 &&
        desc.input_element_count <= 16) {
      for (uint32_t i = 0; i < desc.input_element_count; ++i) {
        const nrhi::InputElementDesc& e = desc.input_elements[i];
        elements[i].SemanticName = e.semantic_name;
        elements[i].SemanticIndex = e.semantic_index;
        elements[i].Format = ToDxgi(e.format);
        elements[i].AlignedByteOffset = e.byte_offset;
        elements[i].InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
      }
      pd.InputLayout.pInputElementDescs = elements;
      pd.InputLayout.NumElements = desc.input_element_count;
    }
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    if (desc.rtv_format != Format::kUnknown) {
      pd.NumRenderTargets = 1;
      pd.RTVFormats[0] = ToDxgi(desc.rtv_format);
    }
    if (desc.dsv_format != Format::kUnknown) {
      pd.DSVFormat = ToDxgi(desc.dsv_format);
    }
    pd.SampleDesc.Count = desc.sample_count;
    ID3D12PipelineState* pso = nullptr;
    if (FAILED(device_->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pso)))) {
      REXLOG_ERROR("nrhi-d3d12: graphics pipeline creation failed");
      return nullptr;
    }
    auto* pipeline = new NrPipelineD3D12();
    pipeline->pso = pso;
    return pipeline;
  }

  uint32_t GetSupportedSampleCount(Format format, uint32_t desired) override {
    DXGI_FORMAT dxgi = ToDxgi(format);
    uint32_t count = desired;
    while (count > 1) {
      D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS levels{};
      levels.Format = dxgi;
      levels.SampleCount = count;
      if (SUCCEEDED(device_->CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS,
                                                 &levels, sizeof(levels))) &&
          levels.NumQualityLevels > 0) {
        break;
      }
      count >>= 1;
    }
    return std::max(count, 1u);
  }

  uint64_t CurrentSubmission() const override { return cp_->GetCurrentSubmission(); }
  uint64_t CompletedSubmission() const override { return cp_->GetCompletedSubmission(); }

  // --- frame handling (called from the command processor) ---

  nrhi::Cmd* BeginFrame(ID3D12Resource* guest_output_resource, DXGI_FORMAT guest_output_format,
                        D3D12_RESOURCE_STATES guest_output_internal_state, uint32_t width,
                        uint32_t height, nrhi::Texture** guest_output_out) {
    guest_output_state_ = guest_output_internal_state;
    cmd_.ResetFrameState();
    cmd_.ResetProfileRegion();
    const auto maint_t0 = std::chrono::steady_clock::now();
    const size_t dissolved_count = dissolved_views_.size();
    FlushDissolvedViews();
    const auto drain_t0 = std::chrono::steady_clock::now();
    size_t released = 0;
    size_t backlog = 0;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      released = DrainRetired(cp_->GetCompletedSubmission());
      backlog = retired_.size();
    }
    // Periodic VRAM budget line (mirrors the Vulkan backend's): local =
    // dedicated VRAM this process holds, the number that ratchets when a
    // cache retains superseded content across map changes.
    ++frame_index_;
    if (adapter3_ != nullptr && (frame_index_ % 600) == 0) {
      DXGI_QUERY_VIDEO_MEMORY_INFO local{};
      DXGI_QUERY_VIDEO_MEMORY_INFO nonlocal{};
      if (SUCCEEDED(adapter3_->QueryVideoMemoryInfo(
              0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local)) &&
          SUCCEEDED(adapter3_->QueryVideoMemoryInfo(
              0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &nonlocal))) {
        REXLOG_INFO(
            "nrhi-d3d12 mem: local use={}MB budget={}MB | nonlocal use={}MB "
            "budget={}MB | retired={}",
            local.CurrentUsage >> 20, local.Budget >> 20,
            nonlocal.CurrentUsage >> 20, nonlocal.Budget >> 20, backlog);
      }
    }
    // Frame-maintenance attribution (this work runs outside the app's
    // RenderScene timers, so a sustained cost here is otherwise invisible).
    // Throttled 8 per 5 s.
    const auto maint_end = std::chrono::steady_clock::now();
    const int64_t maint_us =
        std::chrono::duration_cast<std::chrono::microseconds>(maint_end - maint_t0).count();
    if (maint_us >= 2000) {
      static std::chrono::steady_clock::time_point s_window_start{};
      static uint32_t s_window_count = 0;
      if (maint_end - s_window_start > std::chrono::seconds(5)) {
        s_window_start = maint_end;
        s_window_count = 0;
      }
      if (++s_window_count <= 8) {
        const int64_t drain_us =
            std::chrono::duration_cast<std::chrono::microseconds>(maint_end - drain_t0).count();
        REXLOG_INFO(
            "nrhi-d3d12: SLOW frame maintenance {}us: dissolve={}us/{} drain={}us/{} backlog={}",
            maint_us, maint_us - drain_us, dissolved_count, drain_us, released, backlog);
      }
    }
    NrTextureD3D12*& wrapper = guest_outputs_[guest_output_resource];
    if (wrapper != nullptr &&
        (wrapper->desc.width != width || wrapper->desc.height != height)) {
      // Same resource pointer, different geometry: a recreated image reusing
      // the address. Retire the stale wrapper's views and slots.
      DestroyGuestOutputWrapper(wrapper);
      wrapper = nullptr;
    }
    if (wrapper == nullptr) {
      wrapper = new NrTextureD3D12();
      guest_output_resource->AddRef();
      wrapper->resource = guest_output_resource;
      wrapper->desc.width = width;
      wrapper->desc.height = height;
      wrapper->desc.format = FromDxgi(guest_output_format);
      wrapper->desc.usage = nrhi::kTextureUsageRenderTarget;
      wrapper->dxgi_format = guest_output_format;
      wrapper->is_guest_output = true;
      std::lock_guard<std::mutex> lock(mutex_);
      if (rtv_slots_.Alloc(1, &wrapper->rtv_slot)) {
        device_->CreateRenderTargetView(guest_output_resource, nullptr,
                                        RtvHandle(wrapper->rtv_slot));
      }
      // The presenter keeps a small guest-output mailbox; evict wrappers of
      // resources it has replaced.
      if (guest_outputs_.size() > 6) {
        for (auto it = guest_outputs_.begin(); it != guest_outputs_.end();) {
          if (it->second != wrapper && it->first != guest_output_resource) {
            DestroyGuestOutputWrapperLocked(it->second);
            it = guest_outputs_.erase(it);
          } else {
            ++it;
          }
        }
      }
    }
    *guest_output_out = wrapper;
    return &cmd_;
  }

  // --- internals shared with NrCmdD3D12 ---

  D3D12_CPU_DESCRIPTOR_HANDLE StagingHandle(uint32_t slot) const {
    D3D12_CPU_DESCRIPTOR_HANDLE handle =
        staging_heap_->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += size_t(slot) * view_size_;
    return handle;
  }
  D3D12_CPU_DESCRIPTOR_HANDLE SrvCpuHandle(uint32_t slot) const {
    D3D12_CPU_DESCRIPTOR_HANDLE handle = srv_heap_->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += size_t(slot) * view_size_;
    return handle;
  }
  D3D12_GPU_DESCRIPTOR_HANDLE SrvGpuHandle(uint32_t slot) const {
    D3D12_GPU_DESCRIPTOR_HANDLE handle = srv_heap_->GetGPUDescriptorHandleForHeapStart();
    handle.ptr += size_t(slot) * view_size_;
    return handle;
  }
  D3D12_CPU_DESCRIPTOR_HANDLE RtvHandle(uint32_t slot) const {
    D3D12_CPU_DESCRIPTOR_HANDLE handle = rtv_heap_->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += size_t(slot) * rtv_size_;
    return handle;
  }
  D3D12_CPU_DESCRIPTOR_HANDLE DsvHandle(uint32_t slot) const {
    D3D12_CPU_DESCRIPTOR_HANDLE handle = dsv_heap_->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += size_t(slot) * dsv_size_;
    return handle;
  }

  // Shader-visible binding for an ordered view tuple: consecutive slots
  // holding copies of the staging descriptors, cached and retired when a
  // participating view is destroyed. Render thread only.
  bool GetBinding(NrTextureViewD3D12* const* views, uint32_t count,
                  D3D12_GPU_DESCRIPTOR_HANDLE* gpu_out) {
    if (count == 0 || count > nrhi::kMaxTextureTableSize || views[0] == nullptr) {
      return false;
    }
    BindingKey key{};
    for (uint32_t i = 0; i < count; ++i) {
      key.views[i] = views[i];
    }
    key.count = count;
    auto it = bindings_.find(key);
    if (it == bindings_.end()) {
      uint32_t first_slot;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!srv_slots_.Alloc(count, &first_slot)) {
          REXLOG_ERROR("nrhi-d3d12: shader-visible view heap exhausted");
          return false;
        }
      }
      for (uint32_t i = 0; i < count; ++i) {
        if (views[i] == nullptr) continue;
        device_->CopyDescriptorsSimple(1, SrvCpuHandle(first_slot + i),
                                       StagingHandle(views[i]->staging_slot),
                                       D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
      }
      it = bindings_.emplace(key, Binding{first_slot, count}).first;
    }
    *gpu_out = SrvGpuHandle(it->second.first_slot);
    return true;
  }

  D3D12CommandProcessor* cp() { return cp_; }
  ID3D12DescriptorHeap* srv_heap() { return srv_heap_; }
  D3D12_RESOURCE_STATES guest_output_state() const { return guest_output_state_; }

 private:
  static constexpr uint32_t kStagingViews = 16384;
  static constexpr uint32_t kShaderVisibleViews = 32768;
  static constexpr uint32_t kRtvSlots = 64;
  static constexpr uint32_t kDsvSlots = 8;

  struct RetiredObject {
    ID3D12Resource* resource;
    ID3D12PipelineState* pso;
    uint64_t submission;
  };
  struct BindingKey {
    NrTextureViewD3D12* views[nrhi::kMaxTextureTableSize];
    uint32_t count;
    bool operator<(const BindingKey& other) const {
      if (count != other.count) return count < other.count;
      for (uint32_t i = 0; i < count; ++i) {
        if (views[i] != other.views[i]) return views[i] < other.views[i];
      }
      return false;
    }
  };
  struct Binding {
    uint32_t first_slot;
    uint32_t count;
  };

  // Hands GPU-completed retirements to the release thread. Committed
  // resource Release calls are kernel-priced (~150 us each measured); a
  // sustained eviction feed released on the render thread was a whole-frame
  // stall, so the render thread only moves pointers. Callers hold mutex_.
  size_t DrainRetired(uint64_t completed) {
    size_t moved = 0;
    {
      std::lock_guard<std::mutex> release_lock(release_mutex_);
      std::erase_if(retired_, [&](const RetiredObject& r) {
        if (r.submission < completed) {
          ++moved;
          release_queue_.push_back(r);
          return true;
        }
        return false;
      });
    }
    if (moved != 0) {
      release_cv_.notify_one();
    }
    staging_slots_.Drain(completed);
    srv_slots_.Drain(completed);
    rtv_slots_.Drain(completed);
    dsv_slots_.Drain(completed);
    return moved;
  }

  void ReleaseThreadMain() {
    std::vector<RetiredObject> batch;
    for (;;) {
      {
        std::unique_lock<std::mutex> lock(release_mutex_);
        release_cv_.wait(lock, [&] { return release_exit_ || !release_queue_.empty(); });
        if (release_queue_.empty() && release_exit_) {
          return;
        }
        batch.swap(release_queue_);
      }
      for (const RetiredObject& r : batch) {
        if (r.resource) r.resource->Release();
        if (r.pso) r.pso->Release();
      }
      batch.clear();
    }
  }

  void DestroyGuestOutputWrapperLocked(NrTextureD3D12* wrapper) {
    const uint64_t submission = cp_->GetCurrentSubmission();
    if (wrapper->rtv_slot != ~0u) rtv_slots_.Retire(wrapper->rtv_slot, 1, submission);
    retired_.emplace_back(RetiredObject{wrapper->resource, nullptr, submission});
    delete wrapper;
  }
  void DestroyGuestOutputWrapper(NrTextureD3D12* wrapper) {
    std::lock_guard<std::mutex> lock(mutex_);
    DestroyGuestOutputWrapperLocked(wrapper);
  }

  D3D12CommandProcessor* cp_;
  ID3D12Device* device_ = nullptr;
  // Adapter interface for the periodic VRAM telemetry; null when the query
  // chain is unavailable (the log line is simply skipped).
  IDXGIAdapter3* adapter3_ = nullptr;
  uint64_t frame_index_ = 0;
  NrCmdD3D12 cmd_;

  ID3D12DescriptorHeap* staging_heap_ = nullptr;
  ID3D12DescriptorHeap* srv_heap_ = nullptr;
  ID3D12DescriptorHeap* rtv_heap_ = nullptr;
  ID3D12DescriptorHeap* dsv_heap_ = nullptr;
  uint32_t view_size_ = 0;
  uint32_t rtv_size_ = 0;
  uint32_t dsv_size_ = 0;

  // mutex_ guards the slot allocators and retired_ (creation and deferred
  // destruction are thread-safe); bindings_ is render-thread-only.
  // release_mutex_ guards release_queue_/release_exit_ and nests inside
  // mutex_ (the release thread never takes mutex_).
  std::mutex mutex_;
  std::thread release_thread_;
  std::mutex release_mutex_;
  std::condition_variable release_cv_;
  std::vector<RetiredObject> release_queue_;
  bool release_exit_ = false;
  SlotAllocator staging_slots_;
  SlotAllocator srv_slots_;
  SlotAllocator rtv_slots_;
  SlotAllocator dsv_slots_;
  std::vector<RetiredObject> retired_;
  std::map<BindingKey, Binding> bindings_;
  // Views destroyed since the last FlushDissolvedViews (render thread only).
  // The objects stay allocated until the flush so stale binding-cache keys
  // can never collide with a newly created view at the same address.
  std::vector<NrTextureViewD3D12*> dissolved_views_;
  std::map<ID3D12Resource*, NrTextureD3D12*> guest_outputs_;
  D3D12_RESOURCE_STATES guest_output_state_ = D3D12_RESOURCE_STATE_COMMON;
};

void NrCmdD3D12::SetBindingLayout(nrhi::BindingLayout* layout) {
  auto* l = static_cast<NrBindingLayoutD3D12*>(layout);
  DeferredCommandList& list = device->cp()->GetDeferredCommandList();
  list.SetDescriptorHeaps(device->srv_heap(), nullptr);
  list.D3DSetGraphicsRootSignature(l->root_signature);
  // Root-signature semantics: all bindings reset.
  ResetFrameState();
}

void NrCmdD3D12::SetPipeline(nrhi::Pipeline* pipeline) {
  device->cp()->GetDeferredCommandList().D3DSetPipelineState(
      static_cast<NrPipelineD3D12*>(pipeline)->pso);
}

void NrCmdD3D12::SetRootConstants(uint32_t param, uint32_t count, const void* values,
                                  uint32_t dest_offset_in_values) {
  device->cp()->GetDeferredCommandList().D3DSetGraphicsRoot32BitConstants(
      param, count, values, dest_offset_in_values);
}

void NrCmdD3D12::SetConstantBuffer(uint32_t param, nrhi::Buffer* buffer, uint64_t offset) {
  device->cp()->GetDeferredCommandList().D3DSetGraphicsRootConstantBufferView(
      param, static_cast<NrBufferD3D12*>(buffer)->gpu_va + offset);
}

void NrCmdD3D12::SetBufferSrv(uint32_t param, nrhi::Buffer* buffer, uint64_t offset) {
  device->cp()->GetDeferredCommandList().D3DSetGraphicsRootShaderResourceView(
      param, static_cast<NrBufferD3D12*>(buffer)->gpu_va + offset);
}

void NrCmdD3D12::BindTextureTable(uint32_t param, NrTextureViewD3D12* const* views,
                                  uint32_t count) {
  if (param >= nrhi::kMaxBindingParams) return;
  if (count == last_table_counts_[param] &&
      std::memcmp(last_table_views_[param], views, count * sizeof(views[0])) == 0) {
    return;  // identical tuple already bound on this root param
  }
  D3D12_GPU_DESCRIPTOR_HANDLE handle;
  if (device->GetBinding(views, count, &handle)) {
    device->cp()->GetDeferredCommandList().D3DSetGraphicsRootDescriptorTable(param, handle);
    std::memcpy(last_table_views_[param], views, count * sizeof(views[0]));
    last_table_counts_[param] = count;
  }
}

void NrCmdD3D12::SetTexture(uint32_t param, nrhi::TextureView* view) {
  NrTextureViewD3D12* views[1] = {static_cast<NrTextureViewD3D12*>(view)};
  BindTextureTable(param, views, 1);
}

void NrCmdD3D12::SetTexturePair(uint32_t param, nrhi::TextureView* first,
                                nrhi::TextureView* second) {
  NrTextureViewD3D12* views[2] = {static_cast<NrTextureViewD3D12*>(first),
                                  static_cast<NrTextureViewD3D12*>(second)};
  BindTextureTable(param, views, 2);
}

void NrCmdD3D12::SetTextures(uint32_t param, nrhi::TextureView* const* views, uint32_t count) {
  NrTextureViewD3D12* typed[nrhi::kMaxTextureTableSize] = {};
  if (count > nrhi::kMaxTextureTableSize) return;
  for (uint32_t i = 0; i < count; ++i) {
    typed[i] = static_cast<NrTextureViewD3D12*>(views[i]);
  }
  BindTextureTable(param, typed, count);
}

void NrCmdD3D12::SetRenderTargets(nrhi::Texture* color, nrhi::Texture* depth) {
  D3D12_CPU_DESCRIPTOR_HANDLE rtv;
  D3D12_CPU_DESCRIPTOR_HANDLE dsv;
  const D3D12_CPU_DESCRIPTOR_HANDLE* rtv_ptr = nullptr;
  const D3D12_CPU_DESCRIPTOR_HANDLE* dsv_ptr = nullptr;
  UINT num_rtvs = 0;
  if (color != nullptr) {
    rtv = device->RtvHandle(static_cast<NrTextureD3D12*>(color)->rtv_slot);
    rtv_ptr = &rtv;
    num_rtvs = 1;
  }
  if (depth != nullptr) {
    dsv = device->DsvHandle(static_cast<NrTextureD3D12*>(depth)->dsv_slot);
    dsv_ptr = &dsv;
  }
  device->cp()->GetDeferredCommandList().D3DOMSetRenderTargets(num_rtvs, rtv_ptr, FALSE,
                                                               dsv_ptr);
}

void NrCmdD3D12::ClearRenderTarget(nrhi::Texture* color, const float color4[4]) {
  device->cp()->GetDeferredCommandList().D3DClearRenderTargetView(
      device->RtvHandle(static_cast<NrTextureD3D12*>(color)->rtv_slot), color4, 0, nullptr);
}

void NrCmdD3D12::ClearDepth(nrhi::Texture* depth, float value) {
  device->cp()->GetDeferredCommandList().D3DClearDepthStencilView(
      device->DsvHandle(static_cast<NrTextureD3D12*>(depth)->dsv_slot),
      D3D12_CLEAR_FLAG_DEPTH, value, 0, 0, nullptr);
}

void NrCmdD3D12::SetViewport(const nrhi::Viewport& viewport) {
  D3D12_VIEWPORT vp;
  vp.TopLeftX = viewport.x;
  vp.TopLeftY = viewport.y;
  vp.Width = viewport.width;
  vp.Height = viewport.height;
  vp.MinDepth = viewport.min_depth;
  vp.MaxDepth = viewport.max_depth;
  device->cp()->GetDeferredCommandList().RSSetViewport(vp);
}

void NrCmdD3D12::SetScissor(const nrhi::Rect& rect) {
  D3D12_RECT r;
  r.left = rect.left;
  r.top = rect.top;
  r.right = rect.right;
  r.bottom = rect.bottom;
  device->cp()->GetDeferredCommandList().RSSetScissorRect(r);
}

void NrCmdD3D12::SetVertexBuffer(nrhi::Buffer* buffer, uint64_t offset, uint32_t size_bytes,
                                 uint32_t stride) {
  D3D12_VERTEX_BUFFER_VIEW view;
  view.BufferLocation = static_cast<NrBufferD3D12*>(buffer)->gpu_va + offset;
  view.SizeInBytes = size_bytes;
  view.StrideInBytes = stride;
  device->cp()->GetDeferredCommandList().D3DIASetVertexBuffers(0, 1, &view);
}

void NrCmdD3D12::SetIndexBuffer(nrhi::Buffer* buffer, uint64_t offset, uint32_t size_bytes) {
  D3D12_INDEX_BUFFER_VIEW view;
  view.BufferLocation = static_cast<NrBufferD3D12*>(buffer)->gpu_va + offset;
  view.SizeInBytes = size_bytes;
  view.Format = DXGI_FORMAT_R16_UINT;
  device->cp()->GetDeferredCommandList().D3DIASetIndexBuffer(&view);
}

void NrCmdD3D12::SetPrimitiveTopology(nrhi::PrimitiveTopology topology) {
  device->cp()->GetDeferredCommandList().D3DIASetPrimitiveTopology(
      topology == nrhi::PrimitiveTopology::kTriangleStrip ? D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP
                                                          : D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
}

void NrCmdD3D12::Draw(uint32_t vertex_count, uint32_t start_vertex) {
  device->cp()->GetDeferredCommandList().D3DDrawInstanced(vertex_count, 1, start_vertex, 0);
}

void NrCmdD3D12::DrawIndexed(uint32_t index_count, uint32_t start_index, int32_t base_vertex) {
  device->cp()->GetDeferredCommandList().D3DDrawIndexedInstanced(index_count, 1, start_index,
                                                                 base_vertex, 0);
}

void NrCmdD3D12::CopyBufferToTexture(nrhi::Texture* dst, uint32_t mip, uint32_t array_slice,
                                     nrhi::Buffer* src, uint64_t src_offset, uint32_t row_pitch,
                                     uint32_t width, uint32_t height, uint32_t depth) {
  auto* t = static_cast<NrTextureD3D12*>(dst);
  D3D12_TEXTURE_COPY_LOCATION dst_loc{};
  dst_loc.pResource = t->resource;
  dst_loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  dst_loc.SubresourceIndex = mip + array_slice * t->desc.mip_levels;
  D3D12_TEXTURE_COPY_LOCATION src_loc{};
  src_loc.pResource = static_cast<NrBufferD3D12*>(src)->resource;
  src_loc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  src_loc.PlacedFootprint.Offset = src_offset;
  src_loc.PlacedFootprint.Footprint.Format = t->dxgi_format;
  src_loc.PlacedFootprint.Footprint.Width = width;
  src_loc.PlacedFootprint.Footprint.Height = height;
  src_loc.PlacedFootprint.Footprint.Depth = depth;
  src_loc.PlacedFootprint.Footprint.RowPitch = row_pitch;
  device->cp()->GetDeferredCommandList().D3DCopyTextureRegion(&dst_loc, 0, 0, 0, &src_loc,
                                                              nullptr);
}

void NrCmdD3D12::CopyTextureToBuffer(nrhi::Buffer* dst, uint64_t dst_offset, uint32_t row_pitch,
                                     nrhi::Texture* src, uint32_t mip, uint32_t width,
                                     uint32_t height) {
  auto* t = static_cast<NrTextureD3D12*>(src);
  D3D12_TEXTURE_COPY_LOCATION src_loc{};
  src_loc.pResource = t->resource;
  src_loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  src_loc.SubresourceIndex = mip;
  D3D12_TEXTURE_COPY_LOCATION dst_loc{};
  dst_loc.pResource = static_cast<NrBufferD3D12*>(dst)->resource;
  dst_loc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  dst_loc.PlacedFootprint.Offset = dst_offset;
  dst_loc.PlacedFootprint.Footprint.Format = t->dxgi_format;
  dst_loc.PlacedFootprint.Footprint.Width = width;
  dst_loc.PlacedFootprint.Footprint.Height = height;
  dst_loc.PlacedFootprint.Footprint.Depth = 1;
  dst_loc.PlacedFootprint.Footprint.RowPitch = row_pitch;
  device->cp()->GetDeferredCommandList().D3DCopyTextureRegion(&dst_loc, 0, 0, 0, &src_loc,
                                                              nullptr);
}

void NrCmdD3D12::Barrier(nrhi::Texture* texture, ResourceState before, ResourceState after) {
  auto* t = static_cast<NrTextureD3D12*>(texture);
  device->cp()->PushTransitionBarrier(t->resource,
                                      ToStates(before, device->guest_output_state()),
                                      ToStates(after, device->guest_output_state()));
}

void NrCmdD3D12::FlushBarriers() { device->cp()->SubmitBarriers(); }

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

void NrCmdD3D12::ProfileRegion(nrhi::ProfileStage stage) {
  if (profile_region_query_ != UINT32_MAX) {
    device->cp()->EndGpuTimestampedDraw(profile_region_query_);
    profile_region_query_ = UINT32_MAX;
  }
  if (stage == nrhi::ProfileStage::kTail) {
    // No frame-end query to pair with on D3D12; the tail lands in the
    // profiler's untimed remainder.
    return;
  }
  profile_region_query_ = device->cp()->BeginGpuTimestampedDraw(ProfileStageBucket(stage));
}

}  // namespace

nrhi::Device* CreateNativeRhiDevice(D3D12CommandProcessor* command_processor) {
  return new NrDeviceD3D12(command_processor);
}

void DestroyNativeRhiDevice(nrhi::Device* device) {
  delete static_cast<NrDeviceD3D12*>(device);
}

nrhi::Cmd* NativeRhiBeginFrame(nrhi::Device* device, ID3D12Resource* guest_output_resource,
                               DXGI_FORMAT guest_output_format,
                               D3D12_RESOURCE_STATES guest_output_internal_state, uint32_t width,
                               uint32_t height, nrhi::Texture** guest_output_out) {
  return static_cast<NrDeviceD3D12*>(device)->BeginFrame(
      guest_output_resource, guest_output_format, guest_output_internal_state, width, height,
      guest_output_out);
}

}  // namespace rex::graphics::d3d12
