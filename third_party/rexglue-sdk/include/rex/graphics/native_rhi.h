#pragma once

// Native-render RHI: the backend abstraction the app-side native scene
// renderer draws through. One implementation per SDK graphics backend
// (d3d12/native_rhi_d3d12.cpp, vulkan/native_rhi_vulkan.cpp), each built on
// that backend's command processor (deferred command list, barrier queue,
// submission counters).
//
// The semantics deliberately mirror the D3D12 model the scene renderer was
// written against: explicit resource states, latched command state, a
// binding model of root constants + texture tables + direct buffer binds;
// so the D3D12 backend is a thin passthrough and the Vulkan backend owns all
// translation (render-pass scoping, descriptor sets, layout transitions,
// the y-flip).
//
// Threading contract:
//  - Create*/Destroy*/Map/Unmap/InvalidateForRead on Device are thread-safe
//    (async decode workers create and fill resources off-thread).
//  - CreateTextureView / CreateGraphicsPipeline / CreateBindingLayout /
//    CreateShader and every Cmd method are render-thread-only.
//  - Cmd is only valid inside the native guest-output render callback.
//
// Alignment contract: buffer-bind offsets (SetConstantBuffer/SetBufferSrv)
// and copy row pitches must be kRowPitchAlignment-aligned. Upload memory is
// write-combined: never read it back; readback memory must be read through
// InvalidateForRead.

#include <cstddef>
#include <cstdint>

namespace rex::graphics::nrhi {

inline constexpr uint32_t kRowPitchAlignment = 256;
inline constexpr uint32_t kBufferOffsetAlignment = 256;

enum class Backend : uint32_t {
  kD3D12,
  kVulkan,
};

// Texture, view and vertex formats: exactly the set the scene renderer
// uses. kR10G10B10A2 is the guest-output format on both backends (identical
// bit layout: DXGI_FORMAT_R10G10B10A2_UNORM / VK_FORMAT_A2B10G10R10_UNORM_PACK32).
enum class Format : uint32_t {
  kUnknown = 0,
  kR8G8B8A8_UNORM,
  kR8G8B8A8_UINT,   // vertex blend indices
  kR8_UNORM,
  kR8G8_UNORM,
  kB5G6R5_UNORM,
  kR16G16_UNORM,
  kR10G10B10A2_UNORM,
  kR32_FLOAT,
  kR32G32_FLOAT,
  kR32G32B32_FLOAT,     // vertex only
  kR32G32B32A32_FLOAT,  // vertex only
  kD32_FLOAT,
  kBC1_UNORM,
  kBC2_UNORM,
  kBC3_UNORM,
  kBC4_UNORM,
  kBC5_UNORM,
  // Float color formats (HDR render targets), appended so existing values
  // stay stable across exe/dll pairs. Both are color-renderable and
  // blendable on every target backend (D3D12 FL11+, Vulkan core, MoltenVK);
  // kR11G11B10_FLOAT has no alpha channel and stores no sign bits.
  kR16G16B16A16_FLOAT,
  kR11G11B10_FLOAT,
};

// Block dimensions/bytes for the copy math (1x1 for uncompressed).
inline uint32_t FormatBlockWidth(Format format) {
  switch (format) {
    case Format::kBC1_UNORM:
    case Format::kBC2_UNORM:
    case Format::kBC3_UNORM:
    case Format::kBC4_UNORM:
    case Format::kBC5_UNORM:
      return 4;
    default:
      return 1;
  }
}

// D3D12-style explicit resource states; the Vulkan backend maps each to a
// (stage mask, access mask, image layout) triple.
enum class ResourceState : uint32_t {
  kCommon = 0,
  kRenderTarget,
  kDepthWrite,
  kPixelShaderResource,
  kCopySource,
  kCopyDest,
  kGenericRead,
  // The presenter-owned steady state of the guest output image (D3D12:
  // the presenter's internal state constant; Vulkan: SHADER_READ_ONLY_OPTIMAL
  // for the paint pass). The guest output texture must be returned to this
  // state before the render callback returns.
  kGuestOutput,
};

enum class HeapKind : uint32_t {
  kDefault = 0,
  kUpload,
  kReadback,
};

enum class TextureKind : uint32_t {
  k2D = 0,
  kCube,
  k3D,
};

// Usage flags beyond "sampled + upload destination" (always granted).
enum TextureUsage : uint32_t {
  kTextureUsageNone = 0,
  kTextureUsageRenderTarget = 1u << 0,
  kTextureUsageDepthStencil = 1u << 1,
  kTextureUsageCopySource = 1u << 2,  // readback source
};

enum class ViewDimension : uint32_t {
  k2D = 0,
  k2DMS,
  kCube,
  k3D,
};

// Per-channel view swizzle (Xenos fetch-constant swizzles).
enum class Swizzle : uint8_t {
  kX = 0,
  kY,
  kZ,
  kW,
  kZero,
  kOne,
};

enum class CompareFunc : uint32_t {
  kAlways = 0,
  kLess,
  kLessEqual,
};

enum class CullMode : uint32_t {
  kNone = 0,
  kFront,
  kBack,
};

enum class BlendFactor : uint32_t {
  kZero = 0,
  kOne,
  kSrcAlpha,
  kInvSrcAlpha,
};

enum class BlendOp : uint32_t {
  kAdd = 0,
  kMin,
};

enum class PrimitiveTopology : uint32_t {
  kTriangleList = 0,
  kTriangleStrip,
};

enum class Filter : uint32_t {
  kPoint = 0,
  kLinear,
  kAnisotropic,
};

enum class AddressMode : uint32_t {
  kWrap = 0,
  kClamp,
};

enum class ShaderStage : uint32_t {
  kVertex = 0,
  kPixel,
};

// Shader visibility of a binding parameter (D3D12 root-parameter visibility /
// Vulkan stage flags).
enum class Visibility : uint32_t {
  kAll = 0,
  kVertex,
  kPixel,
};

// ---------------------------------------------------------------------------
// Opaque handles. Created and destroyed only through Device; destruction is
// always deferred against the submission timeline.
// ---------------------------------------------------------------------------

class Buffer {
 public:
  virtual uint64_t size() const = 0;

 protected:
  virtual ~Buffer() = default;
};

class Texture {
 public:
  virtual uint32_t width() const = 0;
  virtual uint32_t height() const = 0;
  virtual Format format() const = 0;

 protected:
  virtual ~Texture() = default;
};

class TextureView {
 protected:
  virtual ~TextureView() = default;
};

class BindingLayout {
 protected:
  virtual ~BindingLayout() = default;
};

class Shader {
 protected:
  virtual ~Shader() = default;
};

class Pipeline {
 protected:
  virtual ~Pipeline() = default;
};

// ---------------------------------------------------------------------------
// Descriptors.
// ---------------------------------------------------------------------------

// Binding classes a buffer will be used with. kFull is the historical
// default (constant buffer / buffer SRV / vertex / index / copy source).
// Narrower classes let backends drop the descriptor-window padding that only
// dynamic-offset constant/SRV binds need, and request narrower usage.
enum class BufferBindClass : uint32_t {
  kFull,
  kVertexIndex,  // vertex/index binds and copy source only
  kCopySrc,      // copy source only (texture upload staging)
};

struct BufferDesc {
  uint64_t size = 0;
  HeapKind heap = HeapKind::kDefault;
  BufferBindClass bind_class = BufferBindClass::kFull;
};

struct TextureDesc {
  TextureKind kind = TextureKind::k2D;
  uint32_t width = 1;
  uint32_t height = 1;
  uint32_t depth = 1;       // k3D only
  uint32_t mip_levels = 1;
  uint32_t sample_count = 1;
  Format format = Format::kUnknown;
  uint32_t usage = kTextureUsageNone;  // TextureUsage bits
  ResourceState initial_state = ResourceState::kCopyDest;
  // Optimized clear value for render/depth targets (D3D12 fast clears).
  float clear_color[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  float clear_depth = 1.0f;
};

struct TextureViewDesc {
  ViewDimension dimension = ViewDimension::k2D;
  // kUnknown = the texture's own format (D32_FLOAT textures are viewed as
  // R32_FLOAT automatically).
  Format format = Format::kUnknown;
  uint32_t base_mip = 0;
  uint32_t mip_levels = ~0u;  // ~0u = all remaining
  Swizzle swizzle[4] = {Swizzle::kX, Swizzle::kY, Swizzle::kZ, Swizzle::kW};
};

enum class BindingParamKind : uint32_t {
  // 32-bit root constants (register b<slot>). One per layout at most. On
  // Vulkan this is backed by an internal per-draw ring UBO.
  kConstants = 0,
  // Constant buffer bound by (buffer, offset).
  kConstantBuffer,
  // Read-only structured/raw buffer bound by (buffer, offset).
  kBufferSrv,
  // Table of 1..8 sampled textures at consecutive t-registers, bound with
  // SetTexture / SetTexturePair / SetTextures.
  kTextureTable,
};

inline constexpr uint32_t kMaxTextureTableSize = 8;

struct BindingParamDesc {
  BindingParamKind kind = BindingParamKind::kTextureTable;
  uint32_t shader_register = 0;   // b/t register of the first entry
  uint32_t count = 1;             // kConstants: number of 32-bit values;
                                  // kTextureTable: 1 or 2 textures
  Visibility visibility = Visibility::kAll;
};

struct StaticSamplerDesc {
  uint32_t shader_register = 0;  // s register
  Filter filter = Filter::kLinear;
  AddressMode address = AddressMode::kClamp;
  uint32_t max_anisotropy = 1;
};

inline constexpr uint32_t kMaxBindingParams = 12;
inline constexpr uint32_t kMaxStaticSamplers = 4;

struct BindingLayoutDesc {
  BindingParamDesc params[kMaxBindingParams] = {};
  uint32_t param_count = 0;
  StaticSamplerDesc static_samplers[kMaxStaticSamplers] = {};
  uint32_t static_sampler_count = 0;
  // D3D12: whether the root signature allows an input-assembler layout.
  bool allow_input_layout = true;
};

struct ShaderMacro {
  const char* name = nullptr;
  const char* value = nullptr;
};

struct ShaderDesc {
  ShaderStage stage = ShaderStage::kVertex;
  // Debug identity, used in compile-error logs.
  const char* name = nullptr;
  // HLSL source + entry point; the D3D12 backend compiles this at runtime
  // (vs_5_0 / ps_5_0), exactly as the scene renderer always has.
  const char* hlsl_source = nullptr;
  const char* entry_point = nullptr;
  const ShaderMacro* macros = nullptr;  // terminated by a {nullptr, nullptr}
  // Prebuilt SPIR-V for the same (source, entry, macros), generated
  // offline by the app's shader script; required by the Vulkan backend.
  const uint32_t* spirv = nullptr;
  size_t spirv_size_bytes = 0;
};

struct InputElementDesc {
  // D3D12 semantic binding.
  const char* semantic_name = nullptr;
  uint32_t semantic_index = 0;
  // Vulkan input location: the element's declaration order in the VS input
  // struct (dxc assigns locations in that order).
  uint32_t location = 0;
  Format format = Format::kUnknown;
  uint32_t byte_offset = 0;
};

struct BlendStateDesc {
  bool enable = false;
  BlendFactor src = BlendFactor::kOne;
  BlendFactor dst = BlendFactor::kZero;
  BlendOp op = BlendOp::kAdd;
  BlendFactor src_alpha = BlendFactor::kOne;
  BlendFactor dst_alpha = BlendFactor::kZero;
  BlendOp op_alpha = BlendOp::kAdd;
  uint8_t write_mask = 0xF;  // RGBA bits 0..3
};

struct DepthStateDesc {
  bool test_enable = false;
  bool write_enable = false;
  CompareFunc func = CompareFunc::kLess;
};

struct GraphicsPipelineDesc {
  BindingLayout* layout = nullptr;
  Shader* vs = nullptr;
  Shader* ps = nullptr;
  const InputElementDesc* input_elements = nullptr;  // nullptr = SV_VertexID
  uint32_t input_element_count = 0;
  uint32_t vertex_stride = 0;
  BlendStateDesc blend;
  DepthStateDesc depth;
  CullMode cull = CullMode::kNone;
  bool depth_clip = true;
  Format rtv_format = Format::kUnknown;  // kUnknown = no color target
  Format dsv_format = Format::kUnknown;  // kUnknown = no depth target
  uint32_t sample_count = 1;
};

struct Viewport {
  float x = 0.0f;
  float y = 0.0f;
  float width = 0.0f;
  float height = 0.0f;
  float min_depth = 0.0f;
  float max_depth = 1.0f;
};

struct Rect {
  int32_t left = 0;
  int32_t top = 0;
  int32_t right = 0;
  int32_t bottom = 0;
};

// GPU-time attribution stages for ProfileRegion: the renderer marks the start
// of each of its passes and the command processor's timestamp-bucket profiler
// (d3d12_gpu_timestamp_buckets / vulkan_gpu_timestamp_buckets) reports the
// per-stage GPU spans. kTail marks the end of the last stage; on Vulkan the
// span from kTail to frame end is reported as its own bucket, on D3D12 it
// stays in the profiler's untimed remainder.
enum class ProfileStage : uint8_t {
  kCommit,     // content-store commits, decode uploads, eviction
  kShadow,     // dynamic caster shadow atlas + blur
  kStaticSun,  // static sun-shadow cascade renders
  kMain,       // main scene pass (opaque + transparent)
  kResolve,    // MSAA resolve + outline composite
  kAmbientOcclusion,
  kSsr,
  kVolumetrics,
  kBloom,  // bloom pyramid + tonemap
  k2d,     // photo grab, blur chains, 2D overlay replay
  kTail,   // after the last stage: backend/present tail
};

// ---------------------------------------------------------------------------
// Command recording. Wraps the command processor's deferred command list for
// the current frame; valid only inside the render callback, render thread
// only. State latches like a D3D12 command list; SetBindingLayout resets
// root-constant and binding state.
// ---------------------------------------------------------------------------

class Cmd {
 public:
  virtual void SetBindingLayout(BindingLayout* layout) = 0;
  virtual void SetPipeline(Pipeline* pipeline) = 0;

  // kConstants param. Values persist across pipeline changes within the
  // current layout (D3D12 root-constant semantics).
  virtual void SetRootConstants(uint32_t param, uint32_t count,
                                const void* values,
                                uint32_t dest_offset_in_values) = 0;
  // offset must be kBufferOffsetAlignment-aligned.
  virtual void SetConstantBuffer(uint32_t param, Buffer* buffer,
                                 uint64_t offset) = 0;
  virtual void SetBufferSrv(uint32_t param, Buffer* buffer,
                            uint64_t offset) = 0;
  virtual void SetTexture(uint32_t param, TextureView* view) = 0;
  // Multi-entry kTextureTable params. Binding fewer views than the table
  // declares leaves the tail entries at a backend fallback (white);
  // shaders only sample declared-but-unbound slots behind flags (the old
  // D3D12 "dangling t5" arrangement, made explicit).
  virtual void SetTexturePair(uint32_t param, TextureView* first,
                              TextureView* second) = 0;
  virtual void SetTextures(uint32_t param, TextureView* const* views,
                           uint32_t count) = 0;

  // Mip 0 of each. color/depth may each be nullptr.
  virtual void SetRenderTargets(Texture* color, Texture* depth) = 0;
  virtual void ClearRenderTarget(Texture* color, const float color4[4]) = 0;
  virtual void ClearDepth(Texture* depth, float value) = 0;

  virtual void SetViewport(const Viewport& viewport) = 0;
  virtual void SetScissor(const Rect& rect) = 0;

  virtual void SetVertexBuffer(Buffer* buffer, uint64_t offset,
                               uint32_t size_bytes, uint32_t stride) = 0;
  // 16-bit indices (the only index format the scene uses).
  virtual void SetIndexBuffer(Buffer* buffer, uint64_t offset,
                              uint32_t size_bytes) = 0;
  virtual void SetPrimitiveTopology(PrimitiveTopology topology) = 0;

  virtual void Draw(uint32_t vertex_count, uint32_t start_vertex) = 0;
  virtual void DrawIndexed(uint32_t index_count, uint32_t start_index,
                           int32_t base_vertex) = 0;

  // Copies. row_pitch must be kRowPitchAlignment-aligned. width/height are
  // the explicit footprint dims in texels (block-aligned for BC formats,
  // exactly as the D3D12 placed footprints were built); depth is 1 except
  // for k3D uploads. array_slice selects the cube face for kCube textures.
  virtual void CopyBufferToTexture(Texture* dst, uint32_t mip,
                                   uint32_t array_slice, Buffer* src,
                                   uint64_t src_offset, uint32_t row_pitch,
                                   uint32_t width, uint32_t height,
                                   uint32_t depth) = 0;
  virtual void CopyTextureToBuffer(Buffer* dst, uint64_t dst_offset,
                                   uint32_t row_pitch, Texture* src,
                                   uint32_t mip, uint32_t width,
                                   uint32_t height) = 0;

  // Queues a transition barrier; FlushBarriers() submits queued barriers
  // (and, on Vulkan, ends any open render pass).
  virtual void Barrier(Texture* texture, ResourceState before,
                       ResourceState after) = 0;
  virtual void FlushBarriers() = 0;

  // Marks the start of a renderer pass for GPU-time attribution (closing the
  // previously marked stage). No-op while the backend's timestamp-bucket
  // profiler is disabled; see ProfileStage for the reporting semantics.
  virtual void ProfileRegion(ProfileStage stage) = 0;

 protected:
  virtual ~Cmd() = default;
};

// ---------------------------------------------------------------------------
// Device: creation, destruction (always deferred), submission timeline.
// Owned by the command processor; outlives every frame's Cmd.
// ---------------------------------------------------------------------------

class Device {
 public:
  virtual Backend backend() const = 0;

  // --- thread-safe ---
  virtual Buffer* CreateBuffer(const BufferDesc& desc) = 0;
  virtual Texture* CreateTexture(const TextureDesc& desc) = 0;
  // Persistent mapping is allowed (and the norm). Upload memory is
  // write-combined: write-only, no reads.
  virtual void* Map(Buffer* buffer) = 0;
  virtual void Unmap(Buffer* buffer) = 0;
  // Make GPU writes visible before reading mapped readback memory
  // (no-op on D3D12, cache invalidate on non-coherent Vulkan memory).
  virtual void InvalidateForRead(Buffer* buffer, uint64_t offset,
                                 uint64_t size) = 0;

  // Deferred destruction: the object dies when the current submission
  // completes. Views/bindings referencing a destroyed object are retired by
  // the backend. Null is allowed (no-op).
  virtual void DestroyDeferred(Buffer* buffer) = 0;
  virtual void DestroyDeferred(Texture* texture) = 0;
  virtual void DestroyDeferred(TextureView* view) = 0;
  virtual void DestroyDeferred(Pipeline* pipeline) = 0;
  virtual void DestroyDeferred(Shader* shader) = 0;

  // --- render-thread-only ---
  virtual TextureView* CreateTextureView(Texture* texture,
                                         const TextureViewDesc& desc) = 0;
  virtual BindingLayout* CreateBindingLayout(const BindingLayoutDesc& desc) = 0;
  // Returns nullptr on compile failure (details logged).
  virtual Shader* CreateShader(const ShaderDesc& desc) = 0;
  virtual Pipeline* CreateGraphicsPipeline(const GraphicsPipelineDesc& desc) = 0;

  // Largest supported sample count <= desired for a render-target format.
  virtual uint32_t GetSupportedSampleCount(Format format,
                                           uint32_t desired) = 0;

  // The command processor's monotonic submission counters: the basis of all
  // lifetime/readback gating.
  virtual uint64_t CurrentSubmission() const = 0;
  virtual uint64_t CompletedSubmission() const = 0;

 protected:
  virtual ~Device() = default;
};

// Optional on-disk cache directory for runtime-compiled shader bytecode
// (the D3D12 backend's D3DCompile output; backends consuming prebuilt
// shaders ignore it). Entries are keyed by a content hash of the stage,
// entry point, macros and full source text, so source edits miss naturally
// and stale bytecode is never served. Set once during app startup, before
// the first CreateShader; empty (the default) disables caching. A missing
// or unwritable directory only costs compile time, never correctness.
void SetShaderBytecodeCacheDirectory(const char* path);
// The configured directory, or an empty string when caching is disabled.
const char* GetShaderBytecodeCacheDirectory();

}  // namespace rex::graphics::nrhi
