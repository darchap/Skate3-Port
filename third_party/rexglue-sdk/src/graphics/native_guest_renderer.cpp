#include <rex/graphics/native_guest_renderer.h>

#include <algorithm>
#include <atomic>
#include <string>

#include <rex/cvar.h>

// Defined here (shared TU) rather than in a backend's command_processor.cpp
// so both D3D12 and Vulkan see the same definition.
REXCVAR_DEFINE_BOOL(native_render_suppress_emulated_draws, true, "GPU",
                    "While the registered native guest-output renderer is actively "
                    "replacing frames, skip emulated draw and resolve execution in the "
                    "command processor (PM4 parsing, fences, queries and memexport draws "
                    "still run). Menus/pause yield to the emulated path and execute "
                    "normally.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_INT32(native_render_suppress_mode, 2, "GPU",
                     "Which emulated passes to suppress while the native guest-output "
                     "renderer is active. 0 = framebuffer-sized passes only (surface "
                     "pitch >= 1280); the game's whole postfx chain then still executes "
                     "at 1152x640 x resolution scale EVERY frame and paces the pipeline "
                     "(~half the achievable frame rate). 1 = suppress everything (perf "
                     "probing; mid-gameplay lightmap page composition breaks). 2 = "
                     "suppress everything EXCEPT the memory-composition passes whose "
                     "outputs the native renderer samples from guest memory: lightmap "
                     "page composition (pitch 1024) and small composite surfaces (pitch "
                     "<= 512, CAS outfit pieces). Menus/pause/loading always render "
                     "fully (the native renderer yields there), so shop/outfit "
                     "composition is unaffected by any mode. 3 = portrait-window mode: "
                     "like 0 the sub-framebuffer RTT passes execute (one-shot frontend "
                     "portrait renders, census pitches 560-1200), but the 1152-wide "
                     "main scene + postfx band stays suppressed; that band is the "
                     "whole-pipeline cost at scaled resolutions and portraits never "
                     "need it.")
    .range(0, 3)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(native_render_suppress_exempt_depth_only, true, "GPU",
                    "Within the suppression-EXEMPT passes (see "
                    "native_render_suppress_mode), also skip draws with no pixel shader "
                    "(depth/stencil-only: shadow-map casters, z-prepasses). Their output "
                    "feeds only the suppressed scene passes; the native renderer builds "
                    "its own shadows, and on Vulkan this stream is the dominant "
                    "remaining emulated GPU cost (2-12 ms/frame at 3x, the bimodal-FPS "
                    "slow state). Disable if lightmap-page or CAS composition content "
                    "regresses (a composition pass depth/stencil-testing against its own "
                    "no-PS lay-down would need this off).")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace rex::graphics {
namespace {

std::atomic<NativeGuestOutputRenderer> g_renderer{nullptr};
std::atomic<void*> g_renderer_user_data{nullptr};
// True while the registered renderer actually replaced the last presented
// frame (false when it yields: menus, early-outs, no renderer).
std::atomic<bool> g_native_output_active{false};
std::atomic<NativeGuestOutputPostProcessor> g_post_processor{nullptr};
std::atomic<void*> g_post_processor_user_data{nullptr};
std::atomic<bool> g_post_process_requested{false};

}  // namespace

void SetNativeGuestOutputRenderer(NativeGuestOutputRenderer renderer, void* user_data) {
  g_renderer_user_data.store(user_data, std::memory_order_release);
  g_renderer.store(renderer, std::memory_order_release);
}

bool TryRenderNativeGuestOutput(const NativeGuestOutputRenderContext& context) {
  NativeGuestOutputRenderer renderer = g_renderer.load(std::memory_order_acquire);
  if (renderer == nullptr) {
    g_native_output_active.store(false, std::memory_order_relaxed);
    return false;
  }
  void* user_data = g_renderer_user_data.load(std::memory_order_acquire);
  const bool rendered = renderer(context, user_data);
  g_native_output_active.store(rendered, std::memory_order_relaxed);
  return rendered;
}

bool HasNativeGuestOutputRenderer() {
  return g_renderer.load(std::memory_order_acquire) != nullptr;
}

void SetNativeGuestOutputPostProcessor(NativeGuestOutputPostProcessor post_processor,
                                       void* user_data) {
  g_post_processor_user_data.store(user_data, std::memory_order_release);
  g_post_processor.store(post_processor, std::memory_order_release);
}

bool HasNativeGuestOutputPostProcessor() {
  return g_post_processor.load(std::memory_order_acquire) != nullptr;
}

void InvokeNativeGuestOutputPostProcessor(const NativeGuestOutputRenderContext& context) {
  NativeGuestOutputPostProcessor post_processor =
      g_post_processor.load(std::memory_order_acquire);
  if (post_processor == nullptr) {
    return;
  }
  post_processor(context, g_post_processor_user_data.load(std::memory_order_acquire));
}

void RequestNativeGuestOutputPostProcess(bool requested) {
  g_post_process_requested.store(requested, std::memory_order_release);
}

bool IsNativeGuestOutputPostProcessRequested() {
  return g_post_process_requested.load(std::memory_order_acquire);
}

bool IsNativeGuestOutputActive() {
  return g_native_output_active.load(std::memory_order_relaxed);
}

namespace {
std::atomic<double> g_output_aspect{0.0};
std::atomic<bool> g_output_aspect_enabled{true};
}  // namespace

void SetNativeGuestOutputAspect(double aspect) {
  g_output_aspect.store(aspect > 0.0 ? aspect : 0.0, std::memory_order_relaxed);
}

double GetNativeGuestOutputAspect() {
  return g_output_aspect.load(std::memory_order_relaxed);
}

void SetNativeGuestOutputAspectEnabled(bool enabled) {
  g_output_aspect_enabled.store(enabled, std::memory_order_relaxed);
}

bool ApplyNativeGuestOutputAspect(uint32_t& guest_output_width, uint32_t guest_output_height,
                                  uint32_t& display_width, uint32_t& display_height) {
  const double aspect = g_output_aspect.load(std::memory_order_relaxed);
  if (aspect <= 0.0 || !guest_output_width || !guest_output_height ||
      !g_output_aspect_enabled.load(std::memory_order_relaxed) ||
      !HasNativeGuestOutputRenderer() ||
      !g_native_output_active.load(std::memory_order_relaxed)) {
    return false;
  }
  // Even width keeps every half-resolution derived target (bloom, SSAO,
  // menu snapshots) an exact divide.
  uint32_t aspect_width =
      uint32_t(std::clamp(aspect * double(guest_output_height) + 0.5, 1.0, 16384.0)) & ~1u;
  if (aspect_width == guest_output_width) {
    return false;
  }
  guest_output_width = aspect_width;
  // The display aspect is the actual pixel aspect: the presenter maps the
  // native image edge-to-edge on a matching window.
  display_width = guest_output_width;
  display_height = guest_output_height;
  return true;
}

bool ShouldSuppressEmulatedDraws() {
  return REXCVAR_GET(native_render_suppress_emulated_draws) &&
         g_native_output_active.load(std::memory_order_relaxed);
}

bool ShouldSuppressExemptDepthOnlyDraws() {
  return REXCVAR_GET(native_render_suppress_exempt_depth_only);
}

bool ShouldSuppressPassAtPitch(uint32_t surface_pitch) {
  switch (REXCVAR_GET(native_render_suppress_mode)) {
    case 0:
      return surface_pitch >= 1280;
    case 1:
      return true;
    case 3:
      // Portrait-window mode: the one-shot frontend portrait RTTs (Skate 3
      // census during the window: 1200/800/640/600/560 + small mips)
      // execute, while the 1152-wide main scene/postfx band, the
      // whole-pipeline cost at scaled resolutions, stays suppressed like
      // the framebuffer.
      return surface_pitch >= 1280 || surface_pitch == 1152;
    default:
      // Execute only the memory-composition passes the native renderer
      // samples: lightmap page composition (1024) and small composite
      // surfaces (<= 512, CAS outfit pieces). The <= 512 window also lets a
      // few tiny postfx pyramid mips through, negligible, and safer than
      // guessing which small surfaces matter.
      return !(surface_pitch == 1024 || surface_pitch <= 512);
  }
}

}  // namespace rex::graphics

namespace rex::graphics::nrhi {
namespace {

// Set once during app startup before any CreateShader (see native_rhi.h);
// lives in this shared TU so every backend reads the same value.
std::string g_shader_bytecode_cache_dir;

}  // namespace

void SetShaderBytecodeCacheDirectory(const char* path) {
  g_shader_bytecode_cache_dir = path != nullptr ? path : "";
}

const char* GetShaderBytecodeCacheDirectory() {
  return g_shader_bytecode_cache_dir.c_str();
}

}  // namespace rex::graphics::nrhi
