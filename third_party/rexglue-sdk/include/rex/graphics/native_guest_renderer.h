#pragma once

#include <cstdint>

#include <rex/graphics/native_rhi.h>

namespace rex::graphics {

enum class NativeGuestOutputBackend : uint32_t {
  kUnknown = 0,
  kD3D12,
  kVulkan,
};

// Per-frame context handed to the registered native guest-output renderer.
// Backend-agnostic: all device access and command recording goes through the
// native-render RHI (rex/graphics/native_rhi.h); the command processor owns
// the nrhi::Device and the per-frame nrhi::Cmd.
struct NativeGuestOutputRenderContext {
  NativeGuestOutputBackend backend = NativeGuestOutputBackend::kUnknown;
  uint32_t guest_output_width = 0;
  uint32_t guest_output_height = 0;
  uint32_t display_width = 0;
  uint32_t display_height = 0;

  // Device-level RHI: resource/pipeline creation, submission counters.
  // Stable across frames for the lifetime of the graphics system.
  nrhi::Device* device = nullptr;
  // Frame-scoped command recording into the command processor's deferred
  // command list. Only valid during the callback.
  nrhi::Cmd* cmd = nullptr;
  // The presenter's guest output image, wrapped as an RHI texture. In
  // nrhi::ResourceState::kGuestOutput at entry (or kCommon the very first
  // frame it exists); must be returned to kGuestOutput before the callback
  // returns true. The pointer is stable while the underlying image is (it
  // changes on output resize).
  nrhi::Texture* guest_output = nullptr;
};

using NativeGuestOutputRenderer = bool (*)(const NativeGuestOutputRenderContext& context,
                                           void* user_data);

void SetNativeGuestOutputRenderer(NativeGuestOutputRenderer renderer, void* user_data);
bool TryRenderNativeGuestOutput(const NativeGuestOutputRenderContext& context);
// Whether a renderer is registered at all; command processors skip RHI
// setup entirely when none is (non-Skate titles / renderer disabled).
bool HasNativeGuestOutputRenderer();

// True while the registered renderer actually replaced the last presented
// frame (false when it yields to the emulated output).
bool IsNativeGuestOutputActive();

// ---- Native guest-output aspect ------------------------------------------
// Exact display aspect the native renderer wants to draw at. 0 = disabled.
// This supports both narrower handheld displays (for example 4:3) and wider
// ultrawide displays. Set at startup or live from settings; consumed by the
// command processors when sizing the guest output for a swap.
void SetNativeGuestOutputAspect(double aspect);
double GetNativeGuestOutputAspect();
// Temporarily keeps the configured native aspect out of emulated fallback
// frames, such as CPU-decoded FMV. The requested aspect remains stored and
// takes effect again as soon as native output is eligible.
void SetNativeGuestOutputAspectEnabled(bool enabled);
// Applies the requested aspect to this swap's guest output and display
// dimensions by sizing guest_output_width to guest_output_height * aspect.
// Only applies while a renderer is registered AND it served the last
// presented frame, so emulated fallback frames return to the frontbuffer
// aspect and use the presenter's letterbox path. Returns whether the output
// aspect changed; when it did and the renderer then yields the frame, the
// refresh callback must return false (keeping the previously presented
// image) instead of running the emulated blit, which writes
// frontbuffer-aspect content.
bool ApplyNativeGuestOutputAspect(uint32_t& guest_output_width, uint32_t guest_output_height,
                                  uint32_t& display_width, uint32_t& display_height);
// native_render_suppress_emulated_draws && IsNativeGuestOutputActive():
// command processors skip emulated draw/resolve execution (memexport draws,
// fences, queries and PM4 parsing still run).
bool ShouldSuppressEmulatedDraws();

// While the native guest-output renderer is active: should the emulated pass
// currently targeting `surface_pitch`-wide surfaces be suppressed? Driven by
// the native_render_suppress_mode cvar; shared by both command processors.
// Draw and resolve suppression must agree: executed passes need their
// resolves, suppressed passes leave garbage EDRAM that must never be copied
// out.
bool ShouldSuppressPassAtPitch(uint32_t surface_pitch);

// Within a suppression-EXEMPT pass: should a draw with no pixel shader
// (depth/stencil-only: shadow casters, z-prepasses) be skipped anyway?
// Driven by native_render_suppress_exempt_depth_only; their output feeds
// only suppressed scene passes (the native renderer shadows itself).
bool ShouldSuppressExemptDepthOnlyDraws();

// ---- Guest-output post-processor ------------------------------------------
// Host effect over the EMULATED guest-output path (e.g. the settings-menu
// backdrop blur): invoked by the command processors at the very end of the
// emulated gamma/FXAA refresh, after the guest output image has been fully
// written, with the same context contract as the renderer callback (the
// image arrives in kGuestOutput state and must be returned to it). When the
// native renderer handled the frame this is NOT invoked; the renderer
// applies its own effects inline. Only invoked while the request flag is set
// (a cheap gate so the common no-effect frame skips the RHI frame setup);
// the post-processor clears the flag itself once its effect has fully
// decayed.
using NativeGuestOutputPostProcessor = void (*)(const NativeGuestOutputRenderContext& context,
                                                void* user_data);
void SetNativeGuestOutputPostProcessor(NativeGuestOutputPostProcessor post_processor,
                                       void* user_data);
bool HasNativeGuestOutputPostProcessor();
void InvokeNativeGuestOutputPostProcessor(const NativeGuestOutputRenderContext& context);
void RequestNativeGuestOutputPostProcess(bool requested);
bool IsNativeGuestOutputPostProcessRequested();

}  // namespace rex::graphics
