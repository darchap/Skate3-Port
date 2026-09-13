/**
 * @file        ui/overlay/simple_settings_overlay.cpp
 *
 * @brief       Curated user-facing settings overlay.
 *
 * Fullscreen, console-style settings screen: category rail on the left, a
 * column of setting rows with left/right value steppers in the middle, a
 * description panel on the right and a button legend along the bottom.
 * Fully navigable with a controller (dpad/left stick, A/B/X/Y, LB/RB),
 * keyboard (arrows/Enter/Esc/Q/E/R/F) and mouse.
 */
#include <rex/ui/overlay/simple_settings_overlay.h>

#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <imgui.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/audio/audio_driver.h>
#include <rex/graphics/native_guest_renderer.h>
#include <rex/platform.h>
#include <rex/ui/presenter.h>
#include <rex/ui/window.h>
#include <toml++/toml.hpp>

namespace rex::ui {
namespace {

#if REX_PLATFORM_ANDROID
constexpr std::array<int32_t, 1> kResolutionScales = {1};
constexpr std::array<const char*, 1> kAndroidQualityProfileLabels = {"Custom"};
// Custom-profile 3D scene target sizes (16:9, even dimensions; the renderer
// masks with & ~1u). The height is what the scene-cap cvar stores; widths
// are derived from the 16:9 output shape at render time.
constexpr std::array<const char*, 8> kAndroidSceneResLabels = {
    "512 x 288 (Performance)", "640 x 360",  "768 x 432",
    "854 x 480",               "960 x 540",  "1088 x 612",
    "1152 x 648",              "1280 x 720 (Quality)"};
constexpr std::array<int32_t, 8> kAndroidSceneResHeights = {
    288, 360, 432, 480, 540, 612, 648, 720};
constexpr std::array<int32_t, 8> kAndroidSceneResWidths = {
    512, 640, 768, 854, 960, 1088, 1152, 1280};
#else
constexpr std::array<int32_t, 3> kResolutionScales = {1, 2, 3};
constexpr std::array<const char*, 3> kResolutionLabels = {"720p (1x)", "1440p (2x)",
                                                          "2160p (3x)"};
#endif
constexpr std::array<const char*, 3> kAspectRatioLabels = {
    "16:9 (Safe)", "4:3 Native", "Ultrawide (Experimental)"};
constexpr std::array<double, 6> kFrameCapRates = {60.0, 90.0, 120.0, 144.0, 165.0, 240.0};
constexpr std::array<const char*, 7> kFrameCapLabels = {"Unlimited", "60 FPS",  "90 FPS",
                                                        "120 FPS",   "144 FPS", "165 FPS",
                                                        "240 FPS"};
constexpr std::array<std::string_view, 7> kCoreSimpleSettingsCvars = {
    "resolution_scale",
    "draw_resolution_scale_x",
    "draw_resolution_scale_y",
    "fullscreen",
    "vsync",
    "mnk_mode",
    "mnk_capture_mouse"};
// Optional cvars persisted when the host defines them (HasCvar-gated: app
// cvars like the native-renderer knobs don't exist in every embedder, and
// backend/platform cvars don't exist in every build).
constexpr std::array<std::string_view, 38> kOptionalSimpleSettingsCvars = {
    "skate3_native_render_scene_handheld_potato",
    "skate3_native_render_scene_vegetation",
    "skate3_native_render_scene_ambient_npcs",
    "skate3_native_render_scene_movable_props",
    "skate3_native_render_scene_clutter_detail",
    "skate3_native_render_scene_lightmaps",
    "skate3_native_render_scene_macro",
    "skate3_native_render_scene_decals",
    "skate3_android_scene_width_cap",
    "skate3_android_scene_height_cap",
    "skate3_native_render_scene",
    "skate3_native_render_scene_msaa",
    "skate3_native_render_scene_shadows",
    "skate3_native_render_scene_shadow_tile",
    "skate3_native_render_scene_shadow_static_casters",
    "skate3_native_render_scene_shadow_static_size",
    "skate3_native_render_scene_shadow_pcss",
    "skate3_native_render_scene_ssao",
    "skate3_native_render_scene_bloom",
    "skate3_native_render_scene_shafts",
    "skate3_native_render_scene_haze",
    "skate3_native_render_mode_indicator",
    "skate3_draw_distance_scale",
    "skate3_lod_distance_scale",
    "skate3_draw_distance_stream_probe",
    "show_fps_counter",
    "monitor",
    "mnk_sensitivity",
    "hid_rumble_enabled",
    "menu_chord",
    "input_backend",
    "audio_mute",
    "audio_device_sample_frames",
    "user_language",
    "skate3_penguin_mod",
    "skate3_display_aspect_mode",
    "skate3_android_quality_profile",
    "skate3_demo_path"};

// MSAA sample counts for the native scene renderer.
constexpr std::array<const char*, 4> kMsaaLabels = {"Off", "2x", "4x", "8x"};
constexpr std::array<int32_t, 4> kMsaaSamples = {1, 2, 4, 8};

// Shadow quality: dynamic-shadow toggle + cascade tile resolution. The
// game's own atlas is three 512 tiles ("Console"); tile 0 = auto (512 x
// the output resolution scale, the emulated renderer's effective raster).
constexpr std::array<const char*, 6> kShadowQualityLabels = {
    "Off", "Auto", "512 (console)", "1024", "1536", "2048"};
constexpr std::array<int32_t, 6> kShadowQualityTiles = {0, 0, 512, 1024, 1536, 2048};

// Enhanced (static sun) shadow map resolution per cascade tile.
constexpr std::array<const char*, 4> kStaticShadowResLabels = {"1024", "2048",
                                                               "4096", "8192"};
constexpr std::array<int32_t, 4> kStaticShadowResSizes = {1024, 2048, 4096,
                                                          8192};

// Draw distance: one multiplier applied to both the world detail cull and
// the character LOD switch ranges (paired hot cvars in the app layer).
#if REX_PLATFORM_ANDROID
constexpr std::array<const char*, 4> kDrawDistanceLabels = {
    "Handheld Ultra", "Handheld Fast", "Original", "2x"};
constexpr std::array<double, 4> kDrawDistanceScales = {0.5, 0.65, 1.0, 2.0};
#else
constexpr std::array<const char*, 4> kDrawDistanceLabels = {"Original", "2x",
                                                            "3x", "5x"};
constexpr std::array<double, 4> kDrawDistanceScales = {1.0, 2.0, 3.0, 5.0};
#endif

// World streaming: pre-load radius in metres for neighbouring world cells
// (0 = the game's own cell-boundary streaming).
constexpr std::array<const char*, 4> kStreamProbeLabels = {
    "Original", "Near (100 m)", "Extended (200 m)", "Far (300 m)"};
constexpr std::array<double, 4> kStreamProbeRadii = {0.0, 100.0, 200.0, 300.0};

// Audio device buffer sizes in sample frames (0 = backend default).
constexpr std::array<const char*, 4> kAudioBufferLabels = {
    "Auto", "512 (low latency)", "1024", "2048 (most stable)"};
constexpr std::array<int32_t, 4> kAudioBufferFrames = {0, 512, 1024, 2048};

// Xbox 360 XLanguage ids 1..12 (user_language cvar).
constexpr std::array<const char*, 12> kLanguageLabels = {
    "English",    "Japanese",           "German",     "French",
    "Spanish",    "Italian",            "Korean",     "Traditional Chinese",
    "Portuguese", "Simplified Chinese", "Polish",     "Russian"};

// Settings-menu controller chord presets (menu_chord cvar spec strings).
// First entry matches the cvar default (the chord reset target).
constexpr std::array<const char*, 4> kMenuChordSpecs = {"rb+start", "lb+rb+start", "back+start",
                                                        "l3+r3"};
constexpr std::array<const char*, 4> kMenuChordLabels = {"RB + Start", "LB + RB + Start",
                                                         "Back + Start", "L3 + R3"};

constexpr std::array<const char*, 5> kMonitorLabels = {"Auto", "Monitor 1", "Monitor 2",
                                                       "Monitor 3", "Monitor 4"};

// X_INPUT_GAMEPAD_* button bits, mirrored locally to keep the UI overlay
// decoupled from the kernel input headers.
constexpr uint16_t kPadDpadUp = 0x0001;
constexpr uint16_t kPadDpadDown = 0x0002;
constexpr uint16_t kPadDpadLeft = 0x0004;
constexpr uint16_t kPadDpadRight = 0x0008;
constexpr uint16_t kPadLShoulder = 0x0100;
constexpr uint16_t kPadRShoulder = 0x0200;
constexpr uint16_t kPadA = 0x1000;
constexpr uint16_t kPadB = 0x2000;
constexpr uint16_t kPadX = 0x4000;
constexpr uint16_t kPadY = 0x8000;

// Categories shown in the left rail.
struct CategoryInfo {
  const char* name;
  const char* desc;
};
constexpr std::array<CategoryInfo, 6> kCategories = {{
    {"Video", "Display, resolution, framerate and renderer quality settings."},
    {"Controls", "Controller, mouse and keyboard input settings."},
    {"Audio", "Sound output settings."},
    {"Profile", "Local player profile and sign-in."},
    {"Mods", "Optional characters and gameplay modifications."},
    {"System", "Game language, pending changes and closing the settings."},
}};

// Navigation repeat pacing (seconds).
constexpr float kRepeatDelay = 0.42f;
constexpr float kRepeatRate = 0.11f;

// Menu-only scale on top of the viewport scale: the rail/content/description
// columns and title shrink together while the footer legend stays at the
// base size.
constexpr float kMenuScale = 0.92f;

// The content view never grows past the classic 12 row slots, so the smaller
// menu buys breathing room rather than extra rows.
constexpr int kMaxViewRows = 12;

// ---- Palette -------------------------------------------------------------
// Frosted-panel color scheme (iterated against
// real gameplay captures): teal frosted side panels with white text over the
// blurred scene, WHITE setting rows with near-black text, black focused row
// with a lime ring, lime section/description bars, magenta steppers/slider.

constexpr ImU32 kColSelFill = IM_COL32(10, 12, 12, 255);
constexpr ImU32 kColSelText = IM_COL32(250, 252, 252, 255);
// Rows are OPAQUE (no scene bleed-through) but tuned to the tone the
// raw white would render (~235 vs 252) so they don't read as
// too bright or too contrasty.
constexpr ImU32 kColPanel = IM_COL32(235, 236, 236, 255);
constexpr ImU32 kColPanelHover = IM_COL32(248, 250, 250, 255);
constexpr ImU32 kColPanelBorder = IM_COL32(0, 0, 0, 26);
constexpr ImU32 kColRowText = IM_COL32(15, 17, 18, 255);
constexpr ImU32 kColRailPanel = IM_COL32(14, 56, 53, 140);
constexpr ImU32 kColRailPanelHover = IM_COL32(24, 90, 86, 166);
constexpr ImU32 kColRailBorder = IM_COL32(255, 255, 255, 26);
constexpr ImU32 kColText = IM_COL32(235, 242, 241, 255);
constexpr ImU32 kColTextDim = IM_COL32(224, 235, 234, 191);
constexpr ImU32 kColTextFaint = IM_COL32(150, 158, 158, 255);
constexpr ImU32 kColAccent = IM_COL32(213, 235, 10, 255);
constexpr ImU32 kColAccentDark = IM_COL32(13, 15, 5, 255);
constexpr ImU32 kColInteract = IM_COL32(230, 0, 120, 255);
constexpr ImU32 kColInteractHover = IM_COL32(255, 25, 145, 255);
constexpr ImU32 kColDanger = IM_COL32(233, 88, 76, 255);
constexpr ImU32 kColWarn = IM_COL32(245, 197, 87, 255);
constexpr ImU32 kColDescPanel = IM_COL32(11, 46, 43, 140);
constexpr ImU32 kColChevDisabled = IM_COL32(0, 0, 0, 56);
// The disabled chevron on the FOCUSED (black) row needs a light variant -
// black-alpha disappears there.
// White-25% disc - keeps the disabled stepper
// visible on the focused black row.
constexpr ImU32 kColChevDisabledOnDark = IM_COL32(255, 255, 255, 64);
constexpr ImU32 kColLegendChip = IM_COL32(238, 240, 240, 255);
constexpr ImU32 kColLegendText = IM_COL32(15, 18, 20, 255);
constexpr ImU32 kColLegendLabel = IM_COL32(228, 236, 235, 255);
constexpr ImU32 kColWhite = IM_COL32(255, 255, 255, 255);

bool HasCvar(std::string_view name) {
  return rex::cvar::GetFlagInfo(name) != nullptr;
}

void ApplyNativeDisplayAspectMode(int mode) {
  mode = std::clamp(mode, 0, 2);
  if (mode == 0) {
    rex::graphics::SetNativeGuestOutputAspect(0.0);
    return;
  }
  if (mode == 1) {
    rex::graphics::SetNativeGuestOutputAspect(4.0 / 3.0);
    return;
  }
  const ImVec2 display = ImGui::GetIO().DisplaySize;
  const double host_aspect = display.y > 0.0f ? double(display.x) / double(display.y) : 0.0;
  rex::graphics::SetNativeGuestOutputAspect(
      host_aspect > (16.0 / 9.0) ? host_aspect : (21.0 / 9.0));
}

std::vector<std::string_view> GetSimpleSettingsCvars() {
  std::vector<std::string_view> cvars;
  cvars.reserve(32);
  cvars.insert(cvars.end(), kCoreSimpleSettingsCvars.begin(), kCoreSimpleSettingsCvars.end());
  for (std::string_view name : kOptionalSimpleSettingsCvars) {
    if (HasCvar(name)) {
      cvars.push_back(name);
    }
  }
  // Literal names rather than the registry's string to keep the string_views
  // pointing at static storage.
  const std::string device_cvar = GetGraphicsDeviceList().cvar_name;
  if (device_cvar == "d3d12_adapter" && HasCvar("d3d12_adapter")) {
    cvars.push_back("d3d12_adapter");
  } else if (device_cvar == "vulkan_device" && HasCvar("vulkan_device")) {
    cvars.push_back("vulkan_device");
  }
  // Persist the graphics API choice only on builds where it can be chosen.
  if (HasCvar("gpu_backend") && HasCvar("d3d12_adapter") && HasCvar("vulkan_device")) {
    cvars.push_back("gpu_backend");
  }
  if (HasCvar("skate3_ultrawide")) {
    cvars.push_back("skate3_ultrawide");
  }
  if (HasCvar("skate3_field_of_view")) {
    cvars.push_back("skate3_field_of_view");
  }
  if (HasCvar("skate3_guest_fps_cap")) {
    cvars.push_back("skate3_guest_fps_cap");
  }
  if (HasCvar("skate3_guest_fps_cap_auto")) {
    cvars.push_back("skate3_guest_fps_cap_auto");
  }
  if (HasCvar("d3d12_present_frame_limiter")) {
    cvars.push_back("d3d12_present_frame_limiter");
  }
  if (HasCvar("d3d12_present_frame_limiter_fps")) {
    cvars.push_back("d3d12_present_frame_limiter_fps");
  }
  if (HasCvar("d3d12_allow_variable_refresh_rate_and_tearing")) {
    cvars.push_back("d3d12_allow_variable_refresh_rate_and_tearing");
  }
  if (HasCvar("vulkan_allow_present_mode_immediate")) {
    cvars.push_back("vulkan_allow_present_mode_immediate");
  }
  if (HasCvar("vulkan_allow_present_mode_mailbox")) {
    cvars.push_back("vulkan_allow_present_mode_mailbox");
  }
  if (HasCvar("vulkan_allow_present_mode_fifo_relaxed")) {
    cvars.push_back("vulkan_allow_present_mode_fifo_relaxed");
  }
  return cvars;
}

// The picker lists devices directly (no separate automatic entry):
// automatic selection (cvar -1) reads as the device the backend reported
// actually picking.
int DeviceIndexFromCvar(const GraphicsDeviceList& device_list) {
  if (device_list.device_names.empty() || !HasCvar(device_list.cvar_name)) {
    return 0;
  }
  int32_t current = rex::cvar::Query<int32_t>(device_list.cvar_name);
  if (current >= 0 && current < static_cast<int32_t>(device_list.device_names.size())) {
    return current;
  }
  return std::clamp(device_list.active_index, 0,
                    static_cast<int32_t>(device_list.device_names.size()) - 1);
}

int ResolutionIndexFromScale(int32_t scale) {
  int best = 0;
  int32_t best_delta = 1000;
  for (int i = 0; i < static_cast<int>(kResolutionScales.size()); ++i) {
    int32_t delta = kResolutionScales[i] > scale ? kResolutionScales[i] - scale
                                                 : scale - kResolutionScales[i];
    if (delta < best_delta) {
      best = i;
      best_delta = delta;
    }
  }
  return best;
}

int ResolutionIndexFromCvar() {
  return ResolutionIndexFromScale(rex::cvar::Query<int32_t>("resolution_scale"));
}

#if REX_PLATFORM_ANDROID
int AndroidQualityProfileIndexFromCvar() {
  return HasCvar("skate3_android_quality_profile")
             ? std::clamp(
                   rex::cvar::Query<int32_t>("skate3_android_quality_profile"),
                   0, static_cast<int>(kAndroidQualityProfileLabels.size()) - 1)
             : 0;
}

// Nearest option to the custom-profile height cap (0 = the 720 fallback).
int AndroidSceneResIndexFromCvar() {
  if (!HasCvar("skate3_android_scene_height_cap")) {
    return static_cast<int>(kAndroidSceneResHeights.size()) - 1;
  }
  int32_t height = rex::cvar::Query<int32_t>("skate3_android_scene_height_cap");
  if (height <= 0) {
    height = 720;
  }
  int best = 0;
  int32_t best_delta = 100000;
  for (int i = 0; i < static_cast<int>(kAndroidSceneResHeights.size()); ++i) {
    const int32_t delta = std::abs(kAndroidSceneResHeights[i] - height);
    if (delta < best_delta) {
      best = i;
      best_delta = delta;
    }
  }
  return best;
}
#endif

bool HasHostFrameCapCvars() {
  // The host present limiter is implemented by the D3D12 presenter; on builds
  // with both backends compiled in its cvars exist even while Vulkan renders,
  // so gate on the backend actually running.
  return HasCvar("d3d12_present_frame_limiter") && HasCvar("d3d12_present_frame_limiter_fps") &&
         GetGraphicsDeviceList().cvar_name != "vulkan_device";
}

// The guest-side cap paces the game's render loop at the swap boundary, so
// both content production and present cadence land on an even beat (the VRR
// case the host present limiter can't fix: it only delays presents of frames
// whose content was already produced on an irregular schedule). It works on
// any backend, but only when the native-render hook layer that implements it
// is active; otherwise fall back to the host present limiter.
bool UseGuestFrameCap() {
  return HasCvar("skate3_guest_fps_cap") && HasCvar("skate3_native_render") &&
         rex::cvar::Query<bool>("skate3_native_render");
}

bool HasFrameCapControl() {
  return UseGuestFrameCap() || HasHostFrameCapCvars();
}

// The refresh-derived automatic cap rides the guest pacer only (the host
// present limiter has no auto mode). Option-list order: [Auto,] <rates...>,
// Unlimited.
bool FrameCapHasAuto() {
  return UseGuestFrameCap() && HasCvar("skate3_guest_fps_cap_auto");
}

int FrameCapRateBaseIndex() { return FrameCapHasAuto() ? 1 : 0; }

int FrameCapUnlimitedIndex() {
  return FrameCapRateBaseIndex() + static_cast<int>(kFrameCapRates.size());
}

int FrameCapIndexFromRate(double rate) {
  if (rate < 1.0) {
    return FrameCapUnlimitedIndex();
  }
  const int base = FrameCapRateBaseIndex();
  int best = base;
  double best_delta = 1000.0;
  for (int i = 0; i < static_cast<int>(kFrameCapRates.size()); ++i) {
    double delta = std::abs(kFrameCapRates[i] - rate);
    if (delta < best_delta) {
      best = base + i;
      best_delta = delta;
    }
  }
  return best;
}

int FrameCapIndexFromCvar() {
  if (FrameCapHasAuto() && rex::cvar::Query<bool>("skate3_guest_fps_cap_auto")) {
    return 0;
  }
  double current = 0.0;
  if (UseGuestFrameCap()) {
    current = rex::cvar::Query<double>("skate3_guest_fps_cap");
  } else if (HasHostFrameCapCvars() && rex::cvar::Query<bool>("d3d12_present_frame_limiter")) {
    current = rex::cvar::Query<double>("d3d12_present_frame_limiter_fps");
  }
  return FrameCapIndexFromRate(current);
}

bool HasFieldOfViewCvar() {
  return HasCvar("skate3_field_of_view");
}

float FieldOfViewFromCvar() {
  if (!HasFieldOfViewCvar()) {
    return 60.0f;
  }
  return float(std::clamp(rex::cvar::Query<double>("skate3_field_of_view"), 40.0, 120.0));
}

bool HasMsaaCvar() { return HasCvar("skate3_native_render_scene_msaa"); }

int MsaaIndexFromSamples(int32_t samples) {
  int best = 0;
  for (int i = 0; i < static_cast<int>(kMsaaSamples.size()); ++i) {
    if (samples >= kMsaaSamples[i]) {
      best = i;
    }
  }
  return best;
}

int MsaaIndexFromCvar() {
  if (!HasMsaaCvar()) {
    return 2;
  }
  return MsaaIndexFromSamples(rex::cvar::Query<int32_t>("skate3_native_render_scene_msaa"));
}

bool HasShadowQualityCvars() {
  return HasCvar("skate3_native_render_scene_shadows") &&
         HasCvar("skate3_native_render_scene_shadow_tile");
}

int ShadowQualityIndexFrom(bool enabled, int32_t tile) {
  if (!enabled) {
    return 0;
  }
  if (tile <= 0) {
    return 1;  // auto
  }
  if (tile <= 512) {
    return 2;
  }
  if (tile <= 1024) {
    return 3;
  }
  return tile <= 1536 ? 4 : 5;
}

int ShadowQualityIndexFromCvar() {
  if (!HasShadowQualityCvars()) {
    return 1;
  }
  return ShadowQualityIndexFrom(
      rex::cvar::Query<bool>("skate3_native_render_scene_shadows"),
      rex::cvar::Query<int32_t>("skate3_native_render_scene_shadow_tile"));
}

bool HasStaticShadowCvars() {
  return HasCvar("skate3_native_render_scene_shadow_static_casters") &&
         HasCvar("skate3_native_render_scene_shadow_static_size");
}

int StaticShadowResIndexFrom(int32_t size) {
  int best = 0;
  for (int i = 0; i < static_cast<int>(kStaticShadowResSizes.size()); ++i) {
    if (size >= kStaticShadowResSizes[i]) {
      best = i;
    }
  }
  return best;
}

int StaticShadowResIndexFromCvar() {
  if (!HasStaticShadowCvars()) {
    return 2;
  }
  return StaticShadowResIndexFrom(
      rex::cvar::Query<int32_t>("skate3_native_render_scene_shadow_static_size"));
}

bool HasDrawDistanceCvars() {
  return HasCvar("skate3_draw_distance_scale") &&
         HasCvar("skate3_lod_distance_scale");
}

template <size_t N>
int NearestValueIndex(const std::array<double, N>& values, double value) {
  int best = 0;
  double best_err = std::abs(value - values[0]);
  for (size_t i = 1; i < N; ++i) {
    const double err = std::abs(value - values[i]);
    if (err < best_err) {
      best = static_cast<int>(i);
      best_err = err;
    }
  }
  return best;
}

int DrawDistanceIndexFromCvar() {
  if (!HasDrawDistanceCvars()) {
    return 0;
  }
  return NearestValueIndex(kDrawDistanceScales,
                           rex::cvar::Query<double>("skate3_draw_distance_scale"));
}

int StreamProbeIndexFromCvar() {
  if (!HasCvar("skate3_draw_distance_stream_probe")) {
    return 0;
  }
  return NearestValueIndex(
      kStreamProbeRadii,
      rex::cvar::Query<double>("skate3_draw_distance_stream_probe"));
}

// The native/emulated renderer choice only means anything while the native
// hook layer (skate3_native_render, restart-scoped) is active.
bool HasRendererChoice() {
  return HasCvar("skate3_native_render_scene") && HasCvar("skate3_native_render") &&
         rex::cvar::Query<bool>("skate3_native_render");
}

int MonitorIndexFromCvar() {
  if (!HasCvar("monitor")) {
    return 0;
  }
  return std::clamp(rex::cvar::Query<int32_t>("monitor"), 0,
                    static_cast<int>(kMonitorLabels.size()) - 1);
}

int AudioBufferIndexFromFrames(int32_t frames) {
  if (frames <= 0) {
    return 0;
  }
  if (frames <= 512) {
    return 1;
  }
  return frames <= 1024 ? 2 : 3;
}

int AudioBufferIndexFromCvar() {
  if (!HasCvar("audio_device_sample_frames")) {
    return 0;
  }
  return AudioBufferIndexFromFrames(rex::cvar::Query<int32_t>("audio_device_sample_frames"));
}

int LanguageIndexFromCvar() {
  if (!HasCvar("user_language")) {
    return 0;
  }
  // XLanguage ids are 1-based (1 = English .. 12 = Russian).
  return std::clamp(static_cast<int>(rex::cvar::Query<uint32_t>("user_language")), 1,
                    static_cast<int>(kLanguageLabels.size())) -
         1;
}

// Preset index for the current menu_chord spec; kMenuChordSpecs.size() means
// "custom" (a hand-edited spec the presets don't cover - kept as an extra
// option so opening the menu never silently rewrites it).
int MenuChordIndexFromCvar(std::string* custom_spec) {
  std::string current = rex::cvar::Query<std::string>("menu_chord");
  for (int i = 0; i < static_cast<int>(kMenuChordSpecs.size()); ++i) {
    if (current == kMenuChordSpecs[i]) {
      return i;
    }
  }
  if (custom_spec) {
    *custom_spec = std::move(current);
  }
  return static_cast<int>(kMenuChordSpecs.size());
}

// ---- CVar default parsing (Y / R "reset to default") ----------------------

std::optional<std::string> CvarDefault(std::string_view name) {
  const auto* info = rex::cvar::GetFlagInfo(name);
  if (!info) {
    return std::nullopt;
  }
  return info->default_value;
}

bool CvarDefaultBool(std::string_view name, bool fallback) {
  auto value = CvarDefault(name);
  if (!value) {
    return fallback;
  }
  return *value == "true" || *value == "1";
}

double CvarDefaultDouble(std::string_view name, double fallback) {
  auto value = CvarDefault(name);
  if (!value || value->empty()) {
    return fallback;
  }
  return std::strtod(value->c_str(), nullptr);
}

void CopyToBuffer(char* buffer, size_t buffer_size, const std::string& value) {
  size_t count = std::min(value.size(), buffer_size - 1);
  std::copy_n(value.data(), count, buffer);
  buffer[count] = '\0';
}

void SetBoolCvar(std::string_view name, bool value) {
  rex::cvar::SetFlagByName(name, value ? "true" : "false");
}

// The Controller Backend row is Windows-only: every other platform always
// runs the SDL input driver, so there is nothing to choose.
bool HasInputBackendChoice() {
#if REX_PLATFORM_WIN32
  return HasCvar("input_backend");
#else
  return false;
#endif
}

// 0 = XInput, 1 = SDL.
int InputBackendIndexFromCvar() {
  return rex::cvar::Query<std::string>("input_backend") == "sdl" ? 1 : 0;
}

// The Graphics API row only exists on builds with both backends compiled in;
// each backend's UI provider registers its device cvar, so their joint
// presence identifies such a build.
bool HasGraphicsApiChoice() {
  return HasCvar("gpu_backend") && HasCvar("d3d12_adapter") && HasCvar("vulkan_device");
}

// 0 = Direct3D 12, 1 = Vulkan. "auto" reads as the backend actually running,
// so the row reflects reality even after a startup fallback.
int GraphicsApiIndexFromCvar() {
  const std::string value = rex::cvar::Query<std::string>("gpu_backend");
  if (value == "vulkan") {
    return 1;
  }
  if (value == "d3d12") {
    return 0;
  }
  return GetGraphicsDeviceList().cvar_name == "vulkan_device" ? 1 : 0;
}

bool TearingFromCvar() {
  if (HasCvar("d3d12_allow_variable_refresh_rate_and_tearing")) {
    return rex::cvar::Query<bool>("d3d12_allow_variable_refresh_rate_and_tearing");
  }
  if (HasCvar("vulkan_allow_present_mode_immediate")) {
    return rex::cvar::Query<bool>("vulkan_allow_present_mode_immediate");
  }
  return false;
}

bool TearingDefault() {
  if (HasCvar("d3d12_allow_variable_refresh_rate_and_tearing")) {
    return CvarDefaultBool("d3d12_allow_variable_refresh_rate_and_tearing", true);
  }
  return CvarDefaultBool("vulkan_allow_present_mode_immediate", true);
}

void SetTearingCvars(bool value) {
  if (HasCvar("d3d12_allow_variable_refresh_rate_and_tearing")) {
    SetBoolCvar("d3d12_allow_variable_refresh_rate_and_tearing", value);
  }
  if (HasCvar("vulkan_allow_present_mode_immediate")) {
    SetBoolCvar("vulkan_allow_present_mode_immediate", value);
  }
  if (HasCvar("vulkan_allow_present_mode_mailbox")) {
    SetBoolCvar("vulkan_allow_present_mode_mailbox", value);
  }
  if (HasCvar("vulkan_allow_present_mode_fifo_relaxed")) {
    SetBoolCvar("vulkan_allow_present_mode_fifo_relaxed", value);
  }
}

// ---- Small draw helpers ----------------------------------------------------

// Snap to whole pixels. Fractional origins under bilinear filtering smear
// glyph edges and turn 1px rules into soft 2px lines - browsers pixel-snap
// text and borders, so snapping to whole pixels keeps them crisp.
float Snap(float value) {
  return std::floor(value + 0.5f);
}

std::string TruncateToWidth(ImFont* font, float size, float max_width, std::string text) {
  if (font->CalcTextSizeA(size, FLT_MAX, 0.0f, text.c_str()).x <= max_width) {
    return text;
  }
  while (!text.empty() &&
         font->CalcTextSizeA(size, FLT_MAX, 0.0f, (text + "...").c_str()).x > max_width) {
    text.pop_back();
  }
  return text + "...";
}

void AddTextCentered(ImDrawList* dl, ImFont* font, float size, ImVec2 center, ImU32 col,
                     const char* text) {
  ImVec2 extent = font->CalcTextSizeA(size, FLT_MAX, 0.0f, text);
  dl->AddText(font, size,
              ImVec2(Snap(center.x - extent.x * 0.5f), Snap(center.y - extent.y * 0.5f)), col,
              text);
}

// Optical centering for short glyph labels (pad button circles, key chips):
// AddTextCentered centers the line box, but a capital's visual ink sits
// asymmetrically inside it (descender slack below, bearings on the sides), so
// the letter lands ~1px down-right of a small circle's center. Vertical
// centers the CAP-HEIGHT BAND taken from 'H'
// rather than each glyph's own ink: per-glyph raster boxes round
// independently (A has a pointed-apex overshoot, Y doesn't), which lands
// adjacent buttons on different subpixel rows - the eye catches that
// inconsistency more than any absolute offset. One shared band = identical
// placement for every label, overshoots hanging evenly, which is also what
// the browser's hinted rasterizer converges to. Horizontal centers the ink
// for single glyphs (circles) or the advance box when ink_x is false -
// multi-glyph chip labels match the browser's text-box centering that way.
// ASCII labels only.
void AddTextCenteredCap(ImDrawList* dl, ImFont* font, float size, ImVec2 center, ImU32 col,
                        const char* text, bool ink_x) {
  ImFontBaked* baked = font->GetFontBaked(size);
  float pen = 0.0f;
  float min_x = FLT_MAX, max_x = -FLT_MAX;
  for (const char* p = text; *p; ++p) {
    const ImFontGlyph* glyph = baked->FindGlyph(ImWchar(uint8_t(*p)));
    if (!glyph) {
      continue;
    }
    if (glyph->Visible) {
      min_x = std::min(min_x, pen + glyph->X0);
      max_x = std::max(max_x, pen + glyph->X1);
    }
    pen += glyph->AdvanceX;
  }
  if (min_x > max_x) {
    return;  // no visible ink
  }
  float band_top = 0.0f;
  float band_bottom = size;
  if (const ImFontGlyph* cap = baked->FindGlyphNoFallback(ImWchar('H'))) {
    band_top = cap->Y0;
    band_bottom = cap->Y1;
  }
  const float x = ink_x ? center.x - (min_x + max_x) * 0.5f : center.x - pen * 0.5f;
  // Round-half-DOWN for the vertical position: the cap band regularly has a
  // half-integer center (e.g. 18px band in a 52px circle), and Snap's
  // round-half-up resolved every tie downward - measured exactly 1px low
  // versus the browser, which resolves the same tie upward (the typographic
  // convention: when a glyph can't center exactly, err high).
  const float y = center.y - (band_top + band_bottom) * 0.5f;
  dl->AddText(font, size, ImVec2(Snap(x), std::ceil(y - 0.5f)), col, text);
}

void AddTextVCentered(ImDrawList* dl, ImFont* font, float size, float x, float center_y,
                      ImU32 col, const char* text) {
  ImVec2 extent = font->CalcTextSizeA(size, FLT_MAX, 0.0f, text);
  dl->AddText(font, size, ImVec2(Snap(x), Snap(center_y - extent.y * 0.5f)), col, text);
}

// Justified paragraph (browser text-align: justify): wraps words to wrap_w
// and stretches the word gaps of every full line so both edges align; the
// last line of the paragraph stays ragged-left. Returns the height consumed.
float AddTextJustified(ImDrawList* dl, ImFont* font, float size, ImVec2 pos, float wrap_w,
                       ImU32 col, const char* text) {
  std::vector<std::string_view> words;
  for (const char* p = text; *p;) {
    while (*p == ' ') {
      ++p;
    }
    const char* start = p;
    while (*p && *p != ' ') {
      ++p;
    }
    if (p > start) {
      words.emplace_back(start, size_t(p - start));
    }
  }
  if (words.empty()) {
    return 0.0f;
  }
  const float space_w = font->CalcTextSizeA(size, FLT_MAX, 0.0f, " ").x;
  auto word_w = [&](std::string_view w) {
    return font->CalcTextSizeA(size, FLT_MAX, 0.0f, w.data(), w.data() + w.size()).x;
  };
  float y = pos.y;
  size_t i = 0;
  while (i < words.size()) {
    float line_w = word_w(words[i]);
    size_t j = i + 1;
    while (j < words.size()) {
      const float w = word_w(words[j]);
      if (line_w + space_w + w > wrap_w) {
        break;
      }
      line_w += space_w + w;
      ++j;
    }
    const bool last_line = j == words.size();
    float gap = space_w;
    if (!last_line && j - i > 1) {
      const float words_only = line_w - space_w * float(j - i - 1);
      const float stretched = (wrap_w - words_only) / float(j - i - 1);
      // Conditional justify: only stretch when the gaps stay modest -
      // beyond ~1.6x a normal space the stretched line reads worse than a
      // ragged one (the "big spaces" objection to naive justification).
      if (stretched <= space_w * 1.6f) {
        gap = stretched;
      }
    }
    float x = pos.x;
    for (size_t k = i; k < j; ++k) {
      dl->AddText(font, size, ImVec2(Snap(x), Snap(y)), col, words[k].data(),
                  words[k].data() + words[k].size());
      x += word_w(words[k]) + gap;
    }
    y += size;
    i = j;
  }
  return y - pos.y;
}

constexpr float kPi = 3.14159265358979323846f;

// Rounded ring band around [p_min,p_max] spanning inset e0..e1 outside the
// rect. The browser rasterizes its stacked box-shadows with pixel-hard
// straight edges (a 4px lime band is EXACTLY 4 full-coverage rows) and
// antialiases only the corner arcs. ImGui's stroked AddRect can't do that:
// PathRect offsets the centerline by 0.5px and thick AA polylines feather
// 1px on both sides, so every horizontal band edge straddled two rows across
// the full row width - measured as 5 soft rows spanning the whole focused
// row (web: zero). Straight runs are therefore drawn as unrounded
// AddRectFilled (PrimRect - no AA fringe) on snapped coordinates; only the
// corner quadrants go through the AA arc stroker.
void DrawHardRingBand(ImDrawList* dl, ImVec2 p_min, ImVec2 p_max, float e0, float e1,
                      float corner_r, ImU32 col) {
  const float x0 = Snap(p_min.x), y0 = Snap(p_min.y);
  const float x1 = Snap(p_max.x), y1 = Snap(p_max.y);
  const float rr = Snap(corner_r);
  const float o0 = Snap(e0), o1 = Snap(e1);
  const float a0 = x0 + rr, a1 = x1 - rr;  // straight-run x span
  const float b0 = y0 + rr, b1 = y1 - rr;  // straight-run y span
  dl->AddRectFilled(ImVec2(a0, y0 - o1), ImVec2(a1, y0 - o0), col);  // top
  dl->AddRectFilled(ImVec2(a0, y1 + o0), ImVec2(a1, y1 + o1), col);  // bottom
  dl->AddRectFilled(ImVec2(x0 - o1, b0), ImVec2(x0 - o0, b1), col);  // left
  dl->AddRectFilled(ImVec2(x1 + o0, b0), ImVec2(x1 + o1, b1), col);  // right
  // Corner quadrants: annulus arcs, stroke centered mid-band.
  const float rm = rr + (o0 + o1) * 0.5f;
  const float t = o1 - o0;
  const struct {
    ImVec2 c;
    float ang0;
  } corners[4] = {
      {ImVec2(a0, b0), kPi},
      {ImVec2(a1, b0), kPi * 1.5f},
      {ImVec2(a1, b1), 0.0f},
      {ImVec2(a0, b1), kPi * 0.5f},
  };
  for (const auto& c : corners) {
    dl->PathArcTo(c.c, rm, c.ang0, c.ang0 + kPi * 0.5f);
    dl->PathStroke(col, 0, t);
  }
}

// Rounded fill with pixel-hard straight edges (browser border-radius
// behavior: only the corner arcs antialias). PathFillConvex would feather
// every edge 1px, leaving a soft seam where the fill meets the lime band.
void DrawHardRoundedFill(ImDrawList* dl, ImVec2 p_min, ImVec2 p_max, float corner_r, ImU32 col) {
  const float x0 = Snap(p_min.x), y0 = Snap(p_min.y);
  const float x1 = Snap(p_max.x), y1 = Snap(p_max.y);
  const float rr = Snap(corner_r);
  const float a0 = x0 + rr, a1 = x1 - rr;
  const float b0 = y0 + rr, b1 = y1 - rr;
  dl->AddRectFilled(ImVec2(x0, b0), ImVec2(x1, b1), col);  // full-width middle
  dl->AddRectFilled(ImVec2(a0, y0), ImVec2(a1, b0), col);  // top strip
  dl->AddRectFilled(ImVec2(a0, b1), ImVec2(a1, y1), col);  // bottom strip
  const struct {
    ImVec2 c;
    float ang0;
  } corners[4] = {
      {ImVec2(a0, b0), kPi},
      {ImVec2(a1, b0), kPi * 1.5f},
      {ImVec2(a1, b1), 0.0f},
      {ImVec2(a0, b1), kPi * 0.5f},
  };
  for (const auto& c : corners) {
    dl->PathLineTo(c.c);
    dl->PathArcTo(c.c, rr, c.ang0, c.ang0 + kPi * 0.5f);
    dl->PathFillConvex(col);
  }
}

// Focused-item highlight: rounded black fill with a lime ring and a thicker
// black outer edge, both drawn OUTSIDE the box (stacked
// box-shadows). Total ring = 6*s beyond the rect on every side; callers keep
// a matching margin in their clip rects.
void DrawFocusHighlight(ImDrawList* dl, ImVec2 p_min, ImVec2 p_max, float s) {
  const float radius = 6.0f * s;
  DrawHardRoundedFill(dl, p_min, p_max, radius, kColSelFill);
  // Lime band: box edge -> +2*s (its corner arc rides at radius + 1*s).
  DrawHardRingBand(dl, p_min, p_max, 0.0f, 2.0f * s, radius, kColAccent);
  // Black outer edge: +2*s -> +6*s (corner arc at radius + 4*s).
  DrawHardRingBand(dl, p_min, p_max, 2.0f * s, 6.0f * s, radius, kColSelFill);
}

// Stepper chevron: a triangle in a circle.
void DrawChevron(ImDrawList* dl, ImVec2 center, float radius, bool points_left, ImU32 circle_col,
                 ImU32 tri_col, bool filled) {
  center = ImVec2(Snap(center.x), Snap(center.y));
  if (filled) {
    dl->AddCircleFilled(center, radius, circle_col, 24);
  } else {
    dl->AddCircle(center, radius, circle_col, 24, 1.5f);
  }
  // Tip-anchored triangle (tip reaches further from center than the back
  // edge). A bbox-centered variant was tried and looked worse in playtest -
  // keep this geometry.
  float t = radius * 0.42f;
  float dir = points_left ? -1.0f : 1.0f;
  ImVec2 tip(center.x + dir * t, center.y);
  ImVec2 a(center.x - dir * t * 0.6f, center.y - t);
  ImVec2 b(center.x - dir * t * 0.6f, center.y + t);
  dl->AddTriangleFilled(tip, a, b, tri_col);
}

struct LegendGlyph {
  const char* glyph;
  const char* label;
  bool circle;  // pad face button -> circle, everything else -> key chip
};

}  // namespace

void SaveSimpleSettingsConfig(const std::filesystem::path& config_path) {
  rex::cvar::SaveConfigValues(config_path, GetSimpleSettingsCvars());
}

void EnsureSimpleSettingsConfig(const std::filesystem::path& config_path) {
  bool should_save = !std::filesystem::exists(config_path);
  if (!should_save) {
    try {
      auto config = toml::parse_file(config_path.string());
      for (std::string_view name : GetSimpleSettingsCvars()) {
        if (!config.contains(name)) {
          should_save = true;
          break;
        }
      }
    } catch (const toml::parse_error&) {
      should_save = true;
    }
  }

  if (should_save) {
    SaveSimpleSettingsConfig(config_path);
  }
}

// One setting row (or section header) in the content column.
struct SimpleSettingsDialog::RowSpec {
  enum Kind { kHeader, kEnum, kSlider, kAction, kText };
  Kind kind = kEnum;
  const char* label = nullptr;
  const char* desc = nullptr;
  std::string desc_extra;       // dynamic addendum shown under desc
  const char* value_note = nullptr;  // short warning shown in the description
  bool enabled = true;
  bool danger = false;
  // kEnum (options step left/right; either index or flag backs the value)
  std::vector<std::string> options;
  int* index = nullptr;
  bool* flag = nullptr;
  std::function<void(int)> on_enum_change;
  // kSlider
  float* value = nullptr;
  float min = 0.0f;
  float max = 1.0f;
  float step = 1.0f;
  const char* fmt = "%.0f";
  std::function<void()> on_value_change;
  // kAction
  std::function<void()> action;
  // kText
  char* text_buf = nullptr;
  size_t text_buf_size = 0;
  // Reset-to-default handler (Y / R), unset when the row has no default.
  std::function<void()> reset;
};

// Per-frame navigation intents merged from pad and keyboard.
struct SimpleSettingsDialog::NavIntents {
  int move_x = 0;
  int move_y = 0;
  bool select = false;
  bool back = false;
  bool category_prev = false;
  bool category_next = false;
  bool reset_default = false;
  bool apply_restart = false;
};

SimpleSettingsDialog::SimpleSettingsDialog(ImGuiDrawer* drawer, std::filesystem::path config_path,
                                           LoadProfilesCallback load_profiles,
                                           SaveProfileCallback save_profile,
                                           CloseSettingsCallback close_settings,
                                           CloseGameCallback close_game,
                                           RestartGameCallback restart_game,
                                           PollGamepadCallback poll_gamepad)
    : ImGuiDialog(drawer),
      config_path_(std::move(config_path)),
      load_profiles_(std::move(load_profiles)),
      save_profile_(std::move(save_profile)),
      close_settings_(std::move(close_settings)),
      close_game_(std::move(close_game)),
      restart_game_(std::move(restart_game)),
      poll_gamepad_(std::move(poll_gamepad)) {
  ReloadProfiles();
  LoadSettingsFromCvars();
  SetDrawActive(false);
}

SimpleSettingsDialog::~SimpleSettingsDialog() = default;

void SimpleSettingsDialog::Show() {
  visible_ = true;
  SetDrawActive(true);
  ReloadProfiles();
  LoadSettingsFromCvars();
  zone_ = FocusZone::kRail;
  rail_sel_ = category_;
  row_index_ = 0;
  content_scroll_ = 0.0f;
  content_scroll_anim_ = 0.0f;
  editing_text_ = false;
  highlight_anim_y_ = -1.0f;
  rail_anim_y_ = -1.0f;
  prev_pad_buttons_ = 0xFFFF;  // swallow buttons already held at open
  just_shown_ = true;          // swallow the stale cursor delta too
}

void SimpleSettingsDialog::LoadSettingsFromCvars() {
  device_list_ = GetGraphicsDeviceList();
  graphics_api_index_ = HasGraphicsApiChoice() ? GraphicsApiIndexFromCvar() : 0;
  device_index_ = DeviceIndexFromCvar(device_list_);
  resolution_scale_index_ = ResolutionIndexFromCvar();
#if REX_PLATFORM_ANDROID
  android_quality_profile_index_ = AndroidQualityProfileIndexFromCvar();
  android_scene_res_index_ = AndroidSceneResIndexFromCvar();
  world_detail_full_ =
      HasCvar("skate3_native_render_scene_handheld_potato") &&
      !rex::cvar::Query<bool>("skate3_native_render_scene_handheld_potato");
  vegetation_ = !HasCvar("skate3_native_render_scene_vegetation") ||
                rex::cvar::Query<bool>("skate3_native_render_scene_vegetation");
  ambient_npcs_ = !HasCvar("skate3_native_render_scene_ambient_npcs") ||
                  rex::cvar::Query<bool>("skate3_native_render_scene_ambient_npcs");
  movable_props_ = !HasCvar("skate3_native_render_scene_movable_props") ||
                   rex::cvar::Query<bool>("skate3_native_render_scene_movable_props");
  clutter_detail_ = !HasCvar("skate3_native_render_scene_clutter_detail") ||
                    rex::cvar::Query<bool>("skate3_native_render_scene_clutter_detail");
  world_texture_layers_ =
      (HasCvar("skate3_native_render_scene_lightmaps") &&
       rex::cvar::Query<bool>("skate3_native_render_scene_lightmaps")) ||
      (HasCvar("skate3_native_render_scene_macro") &&
       rex::cvar::Query<bool>("skate3_native_render_scene_macro")) ||
      (HasCvar("skate3_native_render_scene_decals") &&
       rex::cvar::Query<bool>("skate3_native_render_scene_decals"));
#endif
  frame_cap_index_ = FrameCapIndexFromCvar();
  if (HasCvar("skate3_display_aspect_mode")) {
    aspect_ratio_index_ =
        std::clamp(rex::cvar::Query<int32_t>("skate3_display_aspect_mode"), 0, 2);
  } else {
    aspect_ratio_index_ =
        HasCvar("skate3_ultrawide") && rex::cvar::Query<bool>("skate3_ultrawide") ? 2 : 0;
  }
  msaa_index_ = MsaaIndexFromCvar();
  shadow_quality_index_ = ShadowQualityIndexFromCvar();
  static_shadow_res_index_ = StaticShadowResIndexFromCvar();
  monitor_index_ = MonitorIndexFromCvar();
  audio_buffer_index_ = AudioBufferIndexFromCvar();
  language_index_ = LanguageIndexFromCvar();
  field_of_view_ = FieldOfViewFromCvar();
  fullscreen_ = rex::cvar::Query<bool>("fullscreen");
  vsync_ = rex::cvar::Query<bool>("vsync");
  tearing_ = TearingFromCvar();
  mnk_mode_ = rex::cvar::Query<bool>("mnk_mode");
  mnk_capture_mouse_ = rex::cvar::Query<bool>("mnk_capture_mouse");
  renderer_native_ =
      HasCvar("skate3_native_render_scene") && rex::cvar::Query<bool>("skate3_native_render_scene");
  ssao_ = HasCvar("skate3_native_render_scene_ssao") &&
          rex::cvar::Query<bool>("skate3_native_render_scene_ssao");
  static_shadows_ =
      HasCvar("skate3_native_render_scene_shadow_static_casters") &&
      rex::cvar::Query<bool>("skate3_native_render_scene_shadow_static_casters");
  shadow_pcss_ = HasCvar("skate3_native_render_scene_shadow_pcss") &&
                 rex::cvar::Query<bool>("skate3_native_render_scene_shadow_pcss");
  bloom_ = HasCvar("skate3_native_render_scene_bloom") &&
           rex::cvar::Query<bool>("skate3_native_render_scene_bloom");
  // The volumetric row drives the shafts + haze pair; either one on shows
  // as On so toggling Off always disables both.
  volumetrics_ = (HasCvar("skate3_native_render_scene_shafts") &&
                  rex::cvar::Query<bool>("skate3_native_render_scene_shafts")) ||
                 (HasCvar("skate3_native_render_scene_haze") &&
                  rex::cvar::Query<bool>("skate3_native_render_scene_haze"));
  draw_distance_index_ = DrawDistanceIndexFromCvar();
  stream_probe_index_ = StreamProbeIndexFromCvar();
  mode_indicator_ = HasCvar("skate3_native_render_mode_indicator") &&
                    rex::cvar::Query<bool>("skate3_native_render_mode_indicator");
  fps_counter_ = HasCvar("show_fps_counter") && rex::cvar::Query<bool>("show_fps_counter");
  audio_mute_ = HasCvar("audio_mute") && rex::cvar::Query<bool>("audio_mute");
  rumble_ = HasCvar("hid_rumble_enabled") && rex::cvar::Query<bool>("hid_rumble_enabled");
  penguin_mod_ = HasCvar("skate3_penguin_mod") &&
                 rex::cvar::Query<bool>("skate3_penguin_mod");
  auto_boot_ = HasCvar("skate3_demo_path") && rex::cvar::Query<bool>("skate3_demo_path");
  mnk_sensitivity_ = HasCvar("mnk_sensitivity")
                         ? float(std::clamp(rex::cvar::Query<double>("mnk_sensitivity"), 0.1, 5.0))
                         : 1.0f;
  chord_custom_.clear();
  chord_index_ = HasCvar("menu_chord") ? MenuChordIndexFromCvar(&chord_custom_) : 0;
  input_backend_index_ = HasInputBackendChoice() ? InputBackendIndexFromCvar() : 0;
}

bool SimpleSettingsDialog::HasSettingsChanges() const {
  return
#if REX_PLATFORM_ANDROID
         (HasCvar("skate3_android_quality_profile") &&
          android_quality_profile_index_ != AndroidQualityProfileIndexFromCvar()) ||
#endif
         (HasGraphicsApiChoice() && graphics_api_index_ != GraphicsApiIndexFromCvar()) ||
         device_index_ != DeviceIndexFromCvar(device_list_) ||
         resolution_scale_index_ != ResolutionIndexFromCvar() ||
         frame_cap_index_ != FrameCapIndexFromCvar() ||
         (HasMsaaCvar() && msaa_index_ != MsaaIndexFromCvar()) ||
         (HasShadowQualityCvars() && shadow_quality_index_ != ShadowQualityIndexFromCvar()) ||
         (HasStaticShadowCvars() &&
          static_shadow_res_index_ != StaticShadowResIndexFromCvar()) ||
         (HasCvar("monitor") && monitor_index_ != MonitorIndexFromCvar()) ||
         (HasCvar("audio_device_sample_frames") &&
          audio_buffer_index_ != AudioBufferIndexFromCvar()) ||
         (HasCvar("user_language") && language_index_ != LanguageIndexFromCvar()) ||
         fullscreen_ != rex::cvar::Query<bool>("fullscreen") ||
         vsync_ != rex::cvar::Query<bool>("vsync") ||
         tearing_ != TearingFromCvar() ||
         mnk_mode_ != rex::cvar::Query<bool>("mnk_mode") ||
         mnk_capture_mouse_ != rex::cvar::Query<bool>("mnk_capture_mouse");
}

void SimpleSettingsDialog::Toggle() {
  if (visible_) {
    Hide();
    return;
  }
  Show();
}

void SimpleSettingsDialog::Hide() {
  if (!visible_) {
    return;
  }
  visible_ = false;
  editing_text_ = false;
  SetDrawActive(false);
  if (close_settings_) {
    close_settings_();
  }
}

void SimpleSettingsDialog::NavigateBack() {
  if (!visible_) {
    return;
  }
  if (editing_text_) {
    editing_text_ = false;
    return;
  }
  if (zone_ == FocusZone::kContent) {
    zone_ = FocusZone::kRail;
    return;
  }
  Hide();
}

void SimpleSettingsDialog::ReloadProfiles() {
  profiles_ = load_profiles_ ? load_profiles_() : SimpleProfileState{};
  if (profiles_.profiles.empty()) {
    profiles_.profiles.push_back({"default", "Player"});
    profiles_.selected_index = 0;
  }
  profiles_.selected_index =
      std::clamp(profiles_.selected_index, 0, static_cast<int>(profiles_.profiles.size()) - 1);
  CopyToBuffer(gamertag_buf_, sizeof(gamertag_buf_),
               profiles_.profiles[profiles_.selected_index].gamertag);
  profile_signed_in_ = profiles_.profiles[profiles_.selected_index].signed_in;
}

void SimpleSettingsDialog::SaveVideo() {
  resolution_scale_index_ =
      std::clamp(resolution_scale_index_, 0, static_cast<int>(kResolutionScales.size()) - 1);
#if REX_PLATFORM_ANDROID
  android_quality_profile_index_ = std::clamp(
      android_quality_profile_index_, 0,
      static_cast<int>(kAndroidQualityProfileLabels.size()) - 1);
  if (HasCvar("skate3_android_quality_profile")) {
    rex::cvar::SetFlagByName("skate3_android_quality_profile",
                             std::to_string(android_quality_profile_index_));
  }
#endif
  frame_cap_index_ = std::clamp(frame_cap_index_, 0, FrameCapUnlimitedIndex());
  aspect_ratio_index_ =
      std::clamp(aspect_ratio_index_, 0, static_cast<int>(kAspectRatioLabels.size()) - 1);
  field_of_view_ = std::clamp(field_of_view_, 40.0f, 120.0f);
  graphics_api_index_ = std::clamp(graphics_api_index_, 0, 1);
  // Written only when the selection changed, so an untouched row keeps the
  // cvar on "auto".
  if (HasGraphicsApiChoice() && graphics_api_index_ != GraphicsApiIndexFromCvar()) {
    rex::cvar::SetFlagByName("gpu_backend", graphics_api_index_ == 1 ? "vulkan" : "d3d12");
  }
  if (!device_list_.device_names.empty() && HasCvar(device_list_.cvar_name)) {
    device_index_ = std::clamp(device_index_, 0,
                               static_cast<int>(device_list_.device_names.size()) - 1);
    // Selecting the device automatic selection already picked keeps the cvar
    // automatic, so new hardware still re-picks the best adapter.
    rex::cvar::SetFlagByName(device_list_.cvar_name,
                             device_index_ == device_list_.active_index
                                 ? "-1"
                                 : std::to_string(device_index_));
  }
  const auto scale = std::to_string(kResolutionScales[resolution_scale_index_]);
  rex::cvar::SetFlagByName("resolution_scale", scale);
  rex::cvar::SetFlagByName("draw_resolution_scale_x", scale);
  rex::cvar::SetFlagByName("draw_resolution_scale_y", scale);
  if (UseGuestFrameCap()) {
    const int rate_base = FrameCapRateBaseIndex();
    if (FrameCapHasAuto()) {
      // Auto rides its own flag; the pacer derives the rate from the display
      // refresh live (window moves between monitors keep working).
      SetBoolCvar("skate3_guest_fps_cap_auto", frame_cap_index_ == 0);
    }
    const double cap_fps =
        frame_cap_index_ >= rate_base && frame_cap_index_ < FrameCapUnlimitedIndex()
            ? kFrameCapRates[frame_cap_index_ - rate_base]
            : 0.0;
    rex::cvar::SetFlagByName("skate3_guest_fps_cap", std::to_string(cap_fps));
    // Never run both pacers: the host limiter waits on the paint thread
    // without backpressuring the guest, so presents drop frames on an
    // irregular beat: the judder the guest cap exists to remove.
    if (HasHostFrameCapCvars()) {
      SetBoolCvar("d3d12_present_frame_limiter", false);
    }
  } else if (HasHostFrameCapCvars()) {
    const bool capped = frame_cap_index_ < FrameCapUnlimitedIndex();
    SetBoolCvar("d3d12_present_frame_limiter", capped);
    if (capped) {
      rex::cvar::SetFlagByName(
          "d3d12_present_frame_limiter_fps",
          std::to_string(kFrameCapRates[frame_cap_index_ - FrameCapRateBaseIndex()]));
    }
  }
  SetBoolCvar("fullscreen", fullscreen_);
  if (HasCvar("skate3_display_aspect_mode")) {
    rex::cvar::SetFlagByName("skate3_display_aspect_mode",
                             std::to_string(aspect_ratio_index_));
    ApplyNativeDisplayAspectMode(aspect_ratio_index_);
  }
  if (HasCvar("skate3_ultrawide")) {
    // Keep the legacy flag synchronized for old configs and command lines.
    SetBoolCvar("skate3_ultrawide", aspect_ratio_index_ == 2);
  }
  if (HasFieldOfViewCvar()) {
    rex::cvar::SetFlagByName("skate3_field_of_view", std::to_string(field_of_view_));
  }
  if (HasMsaaCvar()) {
    msaa_index_ = std::clamp(msaa_index_, 0, static_cast<int>(kMsaaSamples.size()) - 1);
    rex::cvar::SetFlagByName("skate3_native_render_scene_msaa",
                             std::to_string(kMsaaSamples[msaa_index_]));
  }
  if (HasShadowQualityCvars()) {
    shadow_quality_index_ =
        std::clamp(shadow_quality_index_, 0, static_cast<int>(kShadowQualityTiles.size()) - 1);
    SetBoolCvar("skate3_native_render_scene_shadows", shadow_quality_index_ != 0);
    if (shadow_quality_index_ != 0) {
      rex::cvar::SetFlagByName("skate3_native_render_scene_shadow_tile",
                               std::to_string(kShadowQualityTiles[shadow_quality_index_]));
    }
  }
  if (HasStaticShadowCvars()) {
    static_shadow_res_index_ = std::clamp(
        static_shadow_res_index_, 0,
        static_cast<int>(kStaticShadowResSizes.size()) - 1);
    rex::cvar::SetFlagByName(
        "skate3_native_render_scene_shadow_static_size",
        std::to_string(kStaticShadowResSizes[static_shadow_res_index_]));
  }
  if (HasCvar("monitor")) {
    monitor_index_ = std::clamp(monitor_index_, 0, static_cast<int>(kMonitorLabels.size()) - 1);
    rex::cvar::SetFlagByName("monitor", std::to_string(monitor_index_));
  }
  if (HasCvar("audio_device_sample_frames")) {
    audio_buffer_index_ =
        std::clamp(audio_buffer_index_, 0, static_cast<int>(kAudioBufferFrames.size()) - 1);
    rex::cvar::SetFlagByName("audio_device_sample_frames",
                             std::to_string(kAudioBufferFrames[audio_buffer_index_]));
  }
  if (HasCvar("user_language")) {
    language_index_ =
        std::clamp(language_index_, 0, static_cast<int>(kLanguageLabels.size()) - 1);
    rex::cvar::SetFlagByName("user_language", std::to_string(language_index_ + 1));
  }
  SetBoolCvar("vsync", vsync_);
  SetTearingCvars(tearing_);
  SetBoolCvar("mnk_mode", mnk_mode_);
  SetBoolCvar("mnk_capture_mouse", mnk_capture_mouse_);
  SaveSimpleSettingsConfig(config_path_);
}

void SimpleSettingsDialog::SaveProfile() {
  if (save_profile_) {
    save_profile_(profiles_.selected_index, gamertag_buf_, profile_signed_in_);
  }
  ReloadProfiles();
}

void SimpleSettingsDialog::ApplyAndRestart() {
  SaveVideo();
  if (restart_game_) {
    restart_game_();
  }
}

void SimpleSettingsDialog::BuildRows(std::vector<RowSpec>& rows, int category) {
  rows.clear();
  const bool pending = HasSettingsChanges();

  // Section bars only SEPARATE groups - a category's first group
  // starts directly with its rows, so a leading header is dropped.
  auto header = [&rows](const char* label) {
    if (rows.empty()) {
      return;
    }
    RowSpec row;
    row.kind = RowSpec::kHeader;
    row.label = label;
    rows.push_back(std::move(row));
  };

  switch (category) {
    case 0: {  // Video
      header("Display");
      if (HasGraphicsApiChoice()) {
        RowSpec row;
        row.kind = RowSpec::kEnum;
        row.label = "Graphics API";
        row.desc =
            "Which graphics API the game renders through. Both perform "
            "similarly - try the other one if you run into driver issues. "
            "Applied with Apply & Restart.";
        row.options = {"DirectX 12", "Vulkan"};
        row.index = &graphics_api_index_;
        row.reset = [this] {
          // The build default ("auto") prefers DirectX 12 on Windows.
          graphics_api_index_ = 0;
        };
        rows.push_back(std::move(row));
      }
      if (!device_list_.device_names.empty() && HasCvar(device_list_.cvar_name)) {
        RowSpec row;
        row.kind = RowSpec::kEnum;
        row.label = "Graphics Device";
        row.desc =
            "Select which GPU renders the game. The highest-performance "
            "adapter is selected automatically.";
        // Index-prefixed labels: virtual display drivers (Parsec etc.) make
        // the same physical GPU enumerate several times, and the index is
        // what the device cvar and the startup log speak in. No separate
        // automatic entry; the row shows the adapter automatic selection
        // resolved to, and re-selecting it keeps the choice automatic.
        for (size_t i = 0; i < device_list_.device_names.size(); ++i) {
          row.options.push_back(std::to_string(i) + ": " + device_list_.device_names[i]);
        }
        row.index = &device_index_;
        row.reset = [this] {
          device_index_ = std::max(device_list_.active_index, 0);
        };
        rows.push_back(std::move(row));
      }
      {
        // Read-only: where frames are actually displayed, so the difference
        // from Render Scale (the internal render size) is visible in the
        // menu. Single option = the steppers grey out on their own; the row
        // stays focusable so its description shows.
        RowSpec row;
        row.kind = RowSpec::kEnum;
        row.label = "Output Resolution";
        row.desc =
            "The resolution frames are displayed at (informational): your "
            "monitor's native resolution in fullscreen, the window size when "
            "windowed. Rendering happens at the Render Scale setting and is "
            "scaled to this.";
        const ImVec2 out_size = ImGui::GetIO().DisplaySize;
        row.options.push_back(std::to_string(int(out_size.x)) + " x " +
                              std::to_string(int(out_size.y)));
        rows.push_back(std::move(row));
      }
#if REX_PLATFORM_ANDROID
      if (HasCvar("skate3_android_quality_profile")) {
        RowSpec row;
        row.kind = RowSpec::kEnum;
        row.label = "Android Device Profile";
        row.desc =
            "Custom: every graphics option below is yours and is kept across "
            "restarts. Requires Apply & Restart.";
        for (const char* label : kAndroidQualityProfileLabels) {
          row.options.push_back(label);
        }
        row.index = &android_quality_profile_index_;
        row.reset = [this] { android_quality_profile_index_ = 0; };
        rows.push_back(std::move(row));
      }
#endif
      {
        RowSpec row;
        row.kind = RowSpec::kEnum;
#if REX_PLATFORM_ANDROID
        row.label = "3D Scene Resolution";
        // A real selector driving the hot-reload scene-cap cvars; the
        // renderer rebuilds its targets on the next frame.
        row.desc =
            "Internal 3D resolution. Menus and the HUD remain at the full "
            "output resolution. Applies immediately; lower is faster. "
            "960 x 540 is a good middle ground.";
        for (const char* label : kAndroidSceneResLabels) {
          row.options.push_back(label);
        }
        row.index = &android_scene_res_index_;
        row.on_enum_change = [this](int value) {
          android_scene_res_index_ = std::clamp(
              value, 0, static_cast<int>(kAndroidSceneResHeights.size()) - 1);
          rex::cvar::SetFlagByName(
              "skate3_android_scene_height_cap",
              std::to_string(kAndroidSceneResHeights[android_scene_res_index_]));
          rex::cvar::SetFlagByName(
              "skate3_android_scene_width_cap",
              std::to_string(kAndroidSceneResWidths[android_scene_res_index_]));
          SaveSimpleSettingsConfig(config_path_);
        };
        row.reset = [this] {
          android_scene_res_index_ = 4;  // 960 x 540
          rex::cvar::SetFlagByName("skate3_android_scene_height_cap", "540");
          rex::cvar::SetFlagByName("skate3_android_scene_width_cap", "960");
          SaveSimpleSettingsConfig(config_path_);
        };
#else
        row.label = "Render Scale";
        row.desc =
            "Internal 3D render resolution, as a multiple of the game's native 720p. "
            "The image is always displayed at your monitor's native resolution - "
            "higher settings supersample down to it.";
        for (const char* label : kResolutionLabels) {
          row.options.push_back(label);
        }
        row.index = &resolution_scale_index_;
        row.reset = [this] {
          resolution_scale_index_ = ResolutionIndexFromScale(
              int32_t(CvarDefaultDouble("resolution_scale", 3.0)));
        };
#endif
        rows.push_back(std::move(row));
      }
      if (HasFrameCapControl()) {
        RowSpec row;
        row.kind = RowSpec::kEnum;
        row.label = "Framerate Cap";
        row.desc =
            "Limit how fast the game runs. A steady cap slightly below your display's "
            "refresh rate gives the smoothest pacing on variable-refresh displays; "
            "Auto tracks the current display and does exactly that. Frames above the "
            "refresh rate cannot be shown and only make steady motion judder.";
        if (FrameCapHasAuto()) {
          // Same policy the guest pacer applies (see Window::AutoFrameCapHz).
          const float auto_cap =
              rex::ui::Window::AutoFrameCapHz(rex::ui::Window::CachedDisplayRefreshHz());
          row.options.push_back(
              auto_cap > 0.0f
                  ? "Auto (" + std::to_string(int(auto_cap)) + " FPS)"
                  : std::string("Auto"));
        }
        for (size_t i = 1; i < kFrameCapLabels.size(); ++i) {
          row.options.push_back(kFrameCapLabels[i]);
        }
        row.options.push_back(kFrameCapLabels[0]);
        row.index = &frame_cap_index_;
        row.reset = [this] {
          if (FrameCapHasAuto() &&
              CvarDefaultBool("skate3_guest_fps_cap_auto", false)) {
            frame_cap_index_ = 0;
            return;
          }
          double rate = 0.0;
          if (UseGuestFrameCap()) {
            rate = CvarDefaultDouble("skate3_guest_fps_cap", 0.0);
          } else if (CvarDefaultBool("d3d12_present_frame_limiter", false)) {
            rate = CvarDefaultDouble("d3d12_present_frame_limiter_fps", 0.0);
          }
          frame_cap_index_ = FrameCapIndexFromRate(rate);
        };
        rows.push_back(std::move(row));
      }
      if (HasCvar("skate3_display_aspect_mode") || HasCvar("skate3_ultrawide")) {
        RowSpec row;
        row.kind = RowSpec::kEnum;
        row.label = "Aspect Ratio";
        row.desc =
            "4:3 Native fills handheld screens such as the RG406V at 960 x 720 "
            "without stretching the world. Ultrawide expands to the host display. "
            "Both require the native renderer and apply immediately.";
        row.value_note = "16:9 remains the safe default";
        for (const char* label : kAspectRatioLabels) {
          row.options.push_back(label);
        }
        row.index = &aspect_ratio_index_;
        row.on_enum_change = [this](int value) {
          aspect_ratio_index_ = std::clamp(value, 0, 2);
          if (HasCvar("skate3_display_aspect_mode")) {
            rex::cvar::SetFlagByName("skate3_display_aspect_mode",
                                     std::to_string(aspect_ratio_index_));
          }
          if (HasCvar("skate3_ultrawide")) {
            SetBoolCvar("skate3_ultrawide", aspect_ratio_index_ == 2);
          }
          ApplyNativeDisplayAspectMode(aspect_ratio_index_);
          SaveSimpleSettingsConfig(config_path_);
        };
        row.reset = [this] {
          aspect_ratio_index_ = HasCvar("skate3_display_aspect_mode")
                                    ? int(CvarDefaultDouble(
                                          "skate3_display_aspect_mode", 0.0))
                                    : (CvarDefaultBool("skate3_ultrawide", false) ? 2 : 0);
        };
        rows.push_back(std::move(row));
      }
      {
        RowSpec row;
        row.kind = RowSpec::kEnum;
        row.label = "Fullscreen";
        row.desc = "Borderless fullscreen presentation instead of a window.";
        row.options = {"Off", "On"};
        row.flag = &fullscreen_;
        row.reset = [this] { fullscreen_ = CvarDefaultBool("fullscreen", true); };
        rows.push_back(std::move(row));
      }
      if (HasCvar("monitor")) {
        RowSpec row;
        row.kind = RowSpec::kEnum;
        row.label = "Monitor";
        row.desc = "Which display the game window opens on. Auto uses the primary display.";
        for (const char* label : kMonitorLabels) {
          row.options.push_back(label);
        }
        row.index = &monitor_index_;
        row.reset = [this] {
          monitor_index_ = int(CvarDefaultDouble("monitor", 0.0));
        };
        rows.push_back(std::move(row));
      }
      {
        RowSpec row;
        row.kind = RowSpec::kEnum;
        row.label = "V-Sync";
        row.desc =
            "Waits for the display before presenting frames. Adds latency and can cause "
            "judder with the frame pacing this port uses.";
        row.value_note = "Not recommended";
        row.options = {"Off", "On"};
        row.flag = &vsync_;
        row.reset = [this] { vsync_ = CvarDefaultBool("vsync", false); };
        rows.push_back(std::move(row));
      }
      {
        RowSpec row;
        row.kind = RowSpec::kEnum;
        row.label = "VRR / Tearing";
        row.desc =
            "Allows uncapped presentation with variable-refresh-rate displays "
            "(G-Sync / FreeSync). Recommended on.";
        row.options = {"Off", "On"};
        row.flag = &tearing_;
        row.reset = [this] { tearing_ = TearingDefault(); };
        rows.push_back(std::move(row));
      }
      if (HasRendererChoice() || HasMsaaCvar() || HasShadowQualityCvars() ||
          HasCvar("skate3_native_render_scene_ssao") || HasDrawDistanceCvars()) {
        header("Graphics");
      }
      if (HasRendererChoice()) {
        RowSpec row;
        row.kind = RowSpec::kEnum;
        row.label = "Renderer";
        row.desc =
            "Native renders the game directly on your GPU (much faster); Emulated "
            "replays the console GPU exactly. Switches live, no restart needed.";
        row.options = {"Emulated", "Native"};
        row.flag = &renderer_native_;
        row.on_enum_change = [this](int value) {
          SetBoolCvar("skate3_native_render_scene", value != 0);
          SaveSimpleSettingsConfig(config_path_);
        };
        row.reset = [this] {
          renderer_native_ = CvarDefaultBool("skate3_native_render_scene", true);
          SetBoolCvar("skate3_native_render_scene", renderer_native_);
          SaveSimpleSettingsConfig(config_path_);
        };
        rows.push_back(std::move(row));
      }
      if (HasMsaaCvar()) {
        RowSpec row;
        row.kind = RowSpec::kEnum;
        row.label = "Anti-Aliasing (MSAA)";
        row.desc =
            "Multisampling for the native renderer. Smooths distant thin geometry "
            "(railings, wires) that shimmers otherwise.";
        for (const char* label : kMsaaLabels) {
          row.options.push_back(label);
        }
        row.index = &msaa_index_;
        row.reset = [this] {
          msaa_index_ =
              MsaaIndexFromSamples(int32_t(CvarDefaultDouble("skate3_native_render_scene_msaa", 4.0)));
        };
        rows.push_back(std::move(row));
      }
      if (HasShadowQualityCvars()) {
        RowSpec row;
        row.kind = RowSpec::kEnum;
        row.label = "Shadow Quality";
        row.desc =
            "Dynamic character/prop shadow resolution in the native renderer. "
            "Auto follows the Render Scale setting; 512 matches the original "
            "game's shadow maps.";
        for (size_t i = 0; i < kShadowQualityLabels.size(); ++i) {
          if (i == 1) {
            // Auto = the game's 512 tiles at the render resolution scale;
            // the renderer sizes the atlas from its scaled output (see the
            // shadow_tile cvar), so the label follows the Render Scale
            // row's STAGED selection: both apply on restart, and changing
            // the scale updates what Auto reads as immediately.
            const int scale_index = std::clamp(
                resolution_scale_index_, 0,
                static_cast<int>(kResolutionScales.size()) - 1);
            const uint32_t auto_tile = std::min(
                512u * uint32_t(kResolutionScales[scale_index]), 4096u);
            row.options.push_back("Auto (" + std::to_string(auto_tile) + ")");
          } else {
            row.options.push_back(kShadowQualityLabels[i]);
          }
        }
        row.index = &shadow_quality_index_;
        row.reset = [this] {
          shadow_quality_index_ = ShadowQualityIndexFrom(
              CvarDefaultBool("skate3_native_render_scene_shadows", true),
              int32_t(CvarDefaultDouble("skate3_native_render_scene_shadow_tile", 0.0)));
        };
        rows.push_back(std::move(row));
      }
      if (HasCvar("skate3_native_render_scene_shadow_static_casters")) {
        RowSpec row;
        row.kind = RowSpec::kEnum;
        row.label = "Enhanced Shadows";
        row.desc =
            "Real-time sun shadows cast by buildings, trees and props from a "
            "dedicated sun-aligned shadow map; the original game only bakes "
            "these into the world. Also shades the volumetric sun shafts. "
            "Applies immediately.";
        row.options = {"Off", "On"};
        row.flag = &static_shadows_;
        row.on_enum_change = [this](int value) {
          SetBoolCvar("skate3_native_render_scene_shadow_static_casters",
                      value != 0);
          SaveSimpleSettingsConfig(config_path_);
        };
        row.reset = [this] {
          static_shadows_ = CvarDefaultBool(
              "skate3_native_render_scene_shadow_static_casters", true);
          SetBoolCvar("skate3_native_render_scene_shadow_static_casters",
                      static_shadows_);
          SaveSimpleSettingsConfig(config_path_);
        };
        rows.push_back(std::move(row));
      }
      if (HasStaticShadowCvars()) {
        RowSpec row;
        row.kind = RowSpec::kEnum;
        row.label = "Enhanced Shadow Resolution";
        row.desc =
            "Resolution per cascade of the enhanced shadow map. Higher is "
            "sharper at distance but uses more video memory (roughly 50 MB "
            "at 2048, 200 MB at 4096, 800 MB at 8192).";
        for (const char* label : kStaticShadowResLabels) {
          row.options.push_back(label);
        }
        row.index = &static_shadow_res_index_;
        row.reset = [this] {
          static_shadow_res_index_ = StaticShadowResIndexFrom(int32_t(
              CvarDefaultDouble("skate3_native_render_scene_shadow_static_size",
                                4096.0)));
        };
        rows.push_back(std::move(row));
      }
      if (HasCvar("skate3_native_render_scene_shadow_pcss")) {
        RowSpec row;
        row.kind = RowSpec::kEnum;
        row.label = "PCSS Shadows";
        row.desc =
            "Contact-hardening soft shadows (PCSS): crisp where a shadow "
            "meets its caster, progressively softer with distance, following "
            "the sun's angular size. Off uses fixed-width filtering. The most "
            "expensive shadow option at high render scales. Applies "
            "immediately.";
        row.options = {"Off", "On"};
        row.flag = &shadow_pcss_;
        row.on_enum_change = [this](int value) {
          SetBoolCvar("skate3_native_render_scene_shadow_pcss", value != 0);
          SaveSimpleSettingsConfig(config_path_);
        };
        row.reset = [this] {
          shadow_pcss_ =
              CvarDefaultBool("skate3_native_render_scene_shadow_pcss", true);
          SetBoolCvar("skate3_native_render_scene_shadow_pcss", shadow_pcss_);
          SaveSimpleSettingsConfig(config_path_);
        };
        rows.push_back(std::move(row));
      }
      if (HasCvar("skate3_native_render_scene_ssao")) {
        RowSpec row;
        row.kind = RowSpec::kEnum;
        row.label = "Ambient Occlusion";
        row.desc =
            "Ground-truth ambient occlusion (GTAO): soft contact shading "
            "where surfaces meet (under ledges, rails, vehicles, the "
            "skater). Native renderer only; applies immediately.";
        row.options = {"Off", "On"};
        row.flag = &ssao_;
        row.on_enum_change = [this](int value) {
          SetBoolCvar("skate3_native_render_scene_ssao", value != 0);
          SaveSimpleSettingsConfig(config_path_);
        };
        row.reset = [this] {
          ssao_ = CvarDefaultBool("skate3_native_render_scene_ssao", true);
          SetBoolCvar("skate3_native_render_scene_ssao", ssao_);
          SaveSimpleSettingsConfig(config_path_);
        };
        rows.push_back(std::move(row));
      }
      if (HasCvar("skate3_native_render_scene_bloom")) {
        RowSpec row;
        row.kind = RowSpec::kEnum;
        row.label = "Bloom";
        row.desc =
            "Glow around bright light sources (lamps, neon, the sun and "
            "sky glare), driven by real scene brightness. Most visible at "
            "night. Applies immediately.";
        row.options = {"Off", "On"};
        row.flag = &bloom_;
        row.on_enum_change = [this](int value) {
          SetBoolCvar("skate3_native_render_scene_bloom", value != 0);
          SaveSimpleSettingsConfig(config_path_);
        };
        row.reset = [this] {
          bloom_ = CvarDefaultBool("skate3_native_render_scene_bloom", true);
          SetBoolCvar("skate3_native_render_scene_bloom", bloom_);
          SaveSimpleSettingsConfig(config_path_);
        };
        rows.push_back(std::move(row));
      }
      if (HasCvar("skate3_native_render_scene_shafts") ||
          HasCvar("skate3_native_render_scene_haze")) {
        RowSpec row;
        row.kind = RowSpec::kEnum;
        row.label = "Volumetric Lighting";
        row.desc =
            "Sun shafts through buildings and trees plus directional "
            "atmospheric haze, both following the time of day. Costs a "
            "little performance. Applies immediately.";
        row.options = {"Off", "On"};
        row.flag = &volumetrics_;
        row.on_enum_change = [this](int value) {
          if (HasCvar("skate3_native_render_scene_shafts")) {
            SetBoolCvar("skate3_native_render_scene_shafts", value != 0);
          }
          if (HasCvar("skate3_native_render_scene_haze")) {
            SetBoolCvar("skate3_native_render_scene_haze", value != 0);
          }
          SaveSimpleSettingsConfig(config_path_);
        };
        row.reset = [this] {
          volumetrics_ =
              CvarDefaultBool("skate3_native_render_scene_shafts", true) ||
              CvarDefaultBool("skate3_native_render_scene_haze", true);
          if (HasCvar("skate3_native_render_scene_shafts")) {
            SetBoolCvar("skate3_native_render_scene_shafts", volumetrics_);
          }
          if (HasCvar("skate3_native_render_scene_haze")) {
            SetBoolCvar("skate3_native_render_scene_haze", volumetrics_);
          }
          SaveSimpleSettingsConfig(config_path_);
        };
        rows.push_back(std::move(row));
      }
#if REX_PLATFORM_ANDROID
      header("Low-End Devices");
      // Remaining lean shortcuts with no row of their own yet: coalesced
      // draw islands, single-pass hair, skipped shadow/outline/spline
      // pipelines and cheaper water probes.
      if (HasCvar("skate3_native_render_scene_handheld_potato")) {
        RowSpec row;
        row.kind = RowSpec::kEnum;
        row.label = "World Detail";
        row.desc =
            "Simplified enables the remaining handheld shortcuts: merged draw "
            "batches, single-pass hair, cheaper water probes, and no shadow, "
            "outline or spline pipelines, so shadow settings only take effect "
            "on Full. The content rows below are independent of this switch.";
        row.options = {"Simplified (fast)", "Full"};
        row.flag = &world_detail_full_;
        row.on_enum_change = [this](int value) {
          world_detail_full_ = value != 0;
          SetBoolCvar("skate3_native_render_scene_handheld_potato",
                      !world_detail_full_);
          SaveSimpleSettingsConfig(config_path_);
        };
        row.reset = [this] {
          world_detail_full_ = true;
          SetBoolCvar("skate3_native_render_scene_handheld_potato", false);
          SaveSimpleSettingsConfig(config_path_);
        };
        rows.push_back(std::move(row));
      }
      if (HasCvar("skate3_native_render_scene_vegetation")) {
        RowSpec row;
        row.kind = RowSpec::kEnum;
        row.label = "Vegetation & Foliage";
        row.desc =
            "Trees, grass, shrubs and other alpha-tested cards. Off removes "
            "them at scene capture: far fewer draw calls and no card shimmer "
            "at low scene resolutions. Content already removed comes back as "
            "areas re-stream while you skate. Applies immediately.";
        row.options = {"Off", "On"};
        row.flag = &vegetation_;
        row.on_enum_change = [this](int value) {
          vegetation_ = value != 0;
          SetBoolCvar("skate3_native_render_scene_vegetation", vegetation_);
          SaveSimpleSettingsConfig(config_path_);
        };
        row.reset = [this] {
          vegetation_ = true;
          SetBoolCvar("skate3_native_render_scene_vegetation", true);
          SaveSimpleSettingsConfig(config_path_);
        };
        rows.push_back(std::move(row));
      }
      if (HasCvar("skate3_native_render_scene_ambient_npcs")) {
        RowSpec row;
        row.kind = RowSpec::kEnum;
        row.label = "Pedestrians & Traffic";
        row.desc =
            "Ambient pedestrians, traffic, their hair and what they carry. Off "
            "removes them from the picture and skips their per-frame capture "
            "cost; it also removes movable street props (same object family), "
            "so the Movable Props row has no effect while this is Off. The "
            "player and other skaters are never affected; the simulation and "
            "its sounds keep running. Applies immediately.";
        row.options = {"Off", "On"};
        row.flag = &ambient_npcs_;
        row.on_enum_change = [this](int value) {
          ambient_npcs_ = value != 0;
          SetBoolCvar("skate3_native_render_scene_ambient_npcs", ambient_npcs_);
          SaveSimpleSettingsConfig(config_path_);
        };
        row.reset = [this] {
          ambient_npcs_ = true;
          SetBoolCvar("skate3_native_render_scene_ambient_npcs", true);
          SaveSimpleSettingsConfig(config_path_);
        };
        rows.push_back(std::move(row));
      }
      if (HasCvar("skate3_native_render_scene_movable_props")) {
        RowSpec row;
        row.kind = RowSpec::kEnum;
        row.label = "Movable Props";
        row.desc =
            "Benches, cones, bins and other pushable street clutter. Off "
            "removes them at scene capture; gameplay objects stay. Applies "
            "immediately.";
        row.options = {"Off", "On"};
        row.flag = &movable_props_;
        row.on_enum_change = [this](int value) {
          movable_props_ = value != 0;
          SetBoolCvar("skate3_native_render_scene_movable_props", movable_props_);
          SaveSimpleSettingsConfig(config_path_);
        };
        row.reset = [this] {
          movable_props_ = true;
          SetBoolCvar("skate3_native_render_scene_movable_props", true);
          SaveSimpleSettingsConfig(config_path_);
        };
        rows.push_back(std::move(row));
      }
      if (HasCvar("skate3_native_render_scene_clutter_detail")) {
        RowSpec row;
        row.kind = RowSpec::kEnum;
        row.label = "Clutter Detail";
        row.desc =
            "Tiny static trim and clutter. Reduced drops small props whose "
            "whole footprint is a few pixels, and more of them with distance. "
            "Rails, ledges and large surfaces are always kept. Applies "
            "immediately.";
        row.options = {"Reduced", "Full"};
        row.flag = &clutter_detail_;
        row.on_enum_change = [this](int value) {
          clutter_detail_ = value != 0;
          SetBoolCvar("skate3_native_render_scene_clutter_detail", clutter_detail_);
          SaveSimpleSettingsConfig(config_path_);
        };
        row.reset = [this] {
          clutter_detail_ = true;
          SetBoolCvar("skate3_native_render_scene_clutter_detail", true);
          SaveSimpleSettingsConfig(config_path_);
        };
        rows.push_back(std::move(row));
      }
      if (HasCvar("skate3_native_render_scene_lightmaps")) {
        RowSpec row;
        row.kind = RowSpec::kEnum;
        row.label = "World Texture Layers";
        row.desc =
            "Secondary world material layers: baked lighting and shadows, "
            "large-scale grime and weathering, graffiti and decal art. Base "
            "Color Only keeps just the base textures. Applies immediately.";
        row.options = {"Base Color Only", "Full"};
        row.flag = &world_texture_layers_;
        row.on_enum_change = [this](int value) {
          world_texture_layers_ = value != 0;
          SetBoolCvar("skate3_native_render_scene_lightmaps", world_texture_layers_);
          SetBoolCvar("skate3_native_render_scene_macro", world_texture_layers_);
          SetBoolCvar("skate3_native_render_scene_decals", world_texture_layers_);
          SaveSimpleSettingsConfig(config_path_);
        };
        row.reset = [this] {
          world_texture_layers_ = true;
          SetBoolCvar("skate3_native_render_scene_lightmaps", true);
          SetBoolCvar("skate3_native_render_scene_macro", true);
          SetBoolCvar("skate3_native_render_scene_decals", true);
          SaveSimpleSettingsConfig(config_path_);
        };
        rows.push_back(std::move(row));
      }
#endif
      if (HasDrawDistanceCvars()) {
        RowSpec row;
        row.kind = RowSpec::kEnum;
        row.label = "Draw Distance";
        row.desc =
            "How far away small world objects, foliage and character detail "
            "stay visible, as a multiple of the original console distances. "
            "Higher settings draw more of the world and cost some "
            "performance. Applies immediately.";
        for (const char* label : kDrawDistanceLabels) {
          row.options.push_back(label);
        }
        row.index = &draw_distance_index_;
        row.on_enum_change = [this](int value) {
          value = std::clamp(value, 0,
                             static_cast<int>(kDrawDistanceScales.size()) - 1);
          const std::string scale = std::to_string(kDrawDistanceScales[value]);
          rex::cvar::SetFlagByName("skate3_draw_distance_scale", scale);
          rex::cvar::SetFlagByName("skate3_lod_distance_scale", scale);
          SaveSimpleSettingsConfig(config_path_);
        };
        row.reset = [this] {
          draw_distance_index_ = NearestValueIndex(
              kDrawDistanceScales,
              CvarDefaultDouble("skate3_draw_distance_scale", 2.0));
          const std::string scale =
              std::to_string(kDrawDistanceScales[draw_distance_index_]);
          rex::cvar::SetFlagByName("skate3_draw_distance_scale", scale);
          rex::cvar::SetFlagByName("skate3_lod_distance_scale", scale);
          SaveSimpleSettingsConfig(config_path_);
        };
        rows.push_back(std::move(row));
      }
      if (HasCvar("skate3_draw_distance_stream_probe")) {
        RowSpec row;
        row.kind = RowSpec::kEnum;
        row.label = "World Streaming";
        row.desc =
            "Load neighbouring world areas before they come into view, so "
            "buildings and trees stream in far away instead of appearing "
            "nearby. Costs a little memory; Original is the console "
            "behavior. Applies immediately.";
        for (const char* label : kStreamProbeLabels) {
          row.options.push_back(label);
        }
        row.index = &stream_probe_index_;
        row.on_enum_change = [this](int value) {
          value = std::clamp(value, 0,
                             static_cast<int>(kStreamProbeRadii.size()) - 1);
          rex::cvar::SetFlagByName("skate3_draw_distance_stream_probe",
                                   std::to_string(kStreamProbeRadii[value]));
          SaveSimpleSettingsConfig(config_path_);
        };
        row.reset = [this] {
          stream_probe_index_ = NearestValueIndex(
              kStreamProbeRadii,
              CvarDefaultDouble("skate3_draw_distance_stream_probe", 0.0));
          rex::cvar::SetFlagByName(
              "skate3_draw_distance_stream_probe",
              std::to_string(kStreamProbeRadii[stream_probe_index_]));
          SaveSimpleSettingsConfig(config_path_);
        };
        rows.push_back(std::move(row));
      }
      if (HasFieldOfViewCvar()) {
        header("Gameplay");
        RowSpec row;
        row.kind = RowSpec::kSlider;
        row.label = "Field of View";
        row.desc = "Camera field of view in degrees. Applies immediately, no restart needed.";
        row.value = &field_of_view_;
        row.min = 40.0f;
        row.max = 120.0f;
        row.step = 2.0f;
        row.fmt = "%.0f";
        row.on_value_change = [this] {
          field_of_view_ = std::clamp(field_of_view_, 40.0f, 120.0f);
          rex::cvar::SetFlagByName("skate3_field_of_view", std::to_string(field_of_view_));
        };
        row.reset = [this] {
          field_of_view_ = float(CvarDefaultDouble("skate3_field_of_view", 60.0));
          rex::cvar::SetFlagByName("skate3_field_of_view", std::to_string(field_of_view_));
          SaveSimpleSettingsConfig(config_path_);
        };
        rows.push_back(std::move(row));
      }
      if (HasCvar("show_fps_counter") || HasCvar("skate3_native_render_mode_indicator")) {
        header("Interface");
      }
      if (HasCvar("show_fps_counter")) {
        RowSpec row;
        row.kind = RowSpec::kEnum;
        row.label = "FPS Counter";
        row.desc = "Show the framerate overlay in the corner. Applies immediately.";
        row.options = {"Off", "On"};
        row.flag = &fps_counter_;
        row.on_enum_change = [this](int value) {
          SetBoolCvar("show_fps_counter", value != 0);
          SaveSimpleSettingsConfig(config_path_);
        };
        row.reset = [this] {
          fps_counter_ = CvarDefaultBool("show_fps_counter", false);
          SetBoolCvar("show_fps_counter", fps_counter_);
          SaveSimpleSettingsConfig(config_path_);
        };
        rows.push_back(std::move(row));
      }
      if (HasCvar("skate3_native_render_mode_indicator")) {
        RowSpec row;
        row.kind = RowSpec::kEnum;
        row.label = "Renderer Indicator";
        row.desc =
            "Small corner readout of which renderer produced the last frame "
            "(NATIVE or EMULATED). Always shown while the Renderer setting "
            "is Emulated. Applies immediately.";
        row.options = {"Off", "On"};
        row.flag = &mode_indicator_;
        row.on_enum_change = [this](int value) {
          SetBoolCvar("skate3_native_render_mode_indicator", value != 0);
          SaveSimpleSettingsConfig(config_path_);
        };
        row.reset = [this] {
          mode_indicator_ = CvarDefaultBool("skate3_native_render_mode_indicator", false);
          SetBoolCvar("skate3_native_render_mode_indicator", mode_indicator_);
          SaveSimpleSettingsConfig(config_path_);
        };
        rows.push_back(std::move(row));
      }
      break;
    }
    case 1: {  // Controls
      header("Mouse & Keyboard");
      {
        RowSpec row;
        row.kind = RowSpec::kEnum;
        row.label = "Mouse & Keyboard Mode";
        row.desc = "Emulates a controller from mouse and keyboard input.";
        row.options = {"Off", "On"};
        row.flag = &mnk_mode_;
        row.reset = [this] { mnk_mode_ = CvarDefaultBool("mnk_mode", false); };
        rows.push_back(std::move(row));
      }
      {
        RowSpec row;
        row.kind = RowSpec::kEnum;
        row.label = "Capture Mouse";
        row.desc =
            "Locks the mouse cursor to the game window while Mouse & Keyboard Mode is "
            "active.";
        row.options = {"Off", "On"};
        row.flag = &mnk_capture_mouse_;
        row.reset = [this] {
          mnk_capture_mouse_ = CvarDefaultBool("mnk_capture_mouse", true);
        };
        rows.push_back(std::move(row));
      }
      if (HasCvar("mnk_sensitivity")) {
        RowSpec row;
        row.kind = RowSpec::kSlider;
        row.label = "Mouse Sensitivity";
        row.desc =
            "Mouse-look speed while Mouse & Keyboard Mode is active. Applies "
            "immediately.";
        row.value = &mnk_sensitivity_;
        row.min = 0.1f;
        row.max = 5.0f;
        row.step = 0.1f;
        row.fmt = "%.1f";
        row.on_value_change = [this] {
          mnk_sensitivity_ = std::clamp(mnk_sensitivity_, 0.1f, 5.0f);
          rex::cvar::SetFlagByName("mnk_sensitivity", std::to_string(mnk_sensitivity_));
        };
        row.reset = [this] {
          mnk_sensitivity_ = float(CvarDefaultDouble("mnk_sensitivity", 1.0));
          rex::cvar::SetFlagByName("mnk_sensitivity", std::to_string(mnk_sensitivity_));
          SaveSimpleSettingsConfig(config_path_);
        };
        rows.push_back(std::move(row));
      }
      if (HasInputBackendChoice() || HasCvar("hid_rumble_enabled") || HasCvar("menu_chord")) {
        header("Controller");
      }
      if (HasInputBackendChoice()) {
        RowSpec row;
        row.kind = RowSpec::kEnum;
        row.label = "Controller Backend";
        row.desc =
            "Which API reads controllers. XInput supports Xbox controllers; "
            "SDL also supports PlayStation, Switch and most generic "
            "controllers without extra software. Applies after restarting "
            "the game.";
        row.value_note = "Restart to apply";
        row.options = {"XInput", "SDL"};
        row.index = &input_backend_index_;
        row.on_enum_change = [this](int value) {
          rex::cvar::SetFlagByName("input_backend", value == 1 ? "sdl" : "xinput");
          SaveSimpleSettingsConfig(config_path_);
        };
        row.reset = [this] {
          input_backend_index_ = 0;
          rex::cvar::SetFlagByName("input_backend", "xinput");
          SaveSimpleSettingsConfig(config_path_);
        };
        rows.push_back(std::move(row));
      }
      if (HasCvar("hid_rumble_enabled")) {
        RowSpec row;
        row.kind = RowSpec::kEnum;
        row.label = "Vibration";
        row.desc = "Controller rumble. Applies immediately.";
        row.options = {"Off", "On"};
        row.flag = &rumble_;
        row.on_enum_change = [this](int value) {
          SetBoolCvar("hid_rumble_enabled", value != 0);
          SaveSimpleSettingsConfig(config_path_);
        };
        row.reset = [this] {
          rumble_ = CvarDefaultBool("hid_rumble_enabled", true);
          SetBoolCvar("hid_rumble_enabled", rumble_);
          SaveSimpleSettingsConfig(config_path_);
        };
        rows.push_back(std::move(row));
      }
      if (HasCvar("menu_chord")) {
        RowSpec row;
        row.kind = RowSpec::kEnum;
        row.label = "Settings Menu Shortcut";
        row.desc =
            "Controller button chord that opens this settings menu. Applies "
            "immediately.";
        for (const char* label : kMenuChordLabels) {
          row.options.push_back(label);
        }
        if (!chord_custom_.empty()) {
          row.options.push_back("Custom: " + chord_custom_);
        }
        row.index = &chord_index_;
        row.on_enum_change = [this](int value) {
          if (value < static_cast<int>(kMenuChordSpecs.size())) {
            rex::cvar::SetFlagByName("menu_chord", kMenuChordSpecs[value]);
          } else if (!chord_custom_.empty()) {
            rex::cvar::SetFlagByName("menu_chord", chord_custom_);
          }
          SaveSimpleSettingsConfig(config_path_);
        };
        row.reset = [this] {
          chord_index_ = 0;
          rex::cvar::SetFlagByName("menu_chord", kMenuChordSpecs[0]);
          SaveSimpleSettingsConfig(config_path_);
        };
        rows.push_back(std::move(row));
      }
      break;
    }
    case 2: {  // Audio
      header("Output");
      if (HasCvar("audio_mute")) {
        RowSpec row;
        row.kind = RowSpec::kEnum;
        row.label = "Mute";
        row.desc = "Silence all game audio. Applies immediately.";
        row.options = {"Off", "On"};
        row.flag = &audio_mute_;
        row.on_enum_change = [this](int value) {
          SetBoolCvar("audio_mute", value != 0);
          SaveSimpleSettingsConfig(config_path_);
        };
        row.reset = [this] {
          audio_mute_ = CvarDefaultBool("audio_mute", false);
          SetBoolCvar("audio_mute", audio_mute_);
          SaveSimpleSettingsConfig(config_path_);
        };
        rows.push_back(std::move(row));
      }
      if (HasCvar("audio_device_sample_frames")) {
        RowSpec row;
        row.kind = RowSpec::kEnum;
        row.label = "Audio Buffer Size";
        row.desc =
            "Audio device buffer length. Larger buffers add a little latency but "
            "fix crackling on machines with scheduling hiccups.";
        for (size_t i = 0; i < kAudioBufferLabels.size(); ++i) {
          if (i == 0) {
            // The backend publishes the buffer it chose when running in auto
            // mode; 0 while unknown (explicit size active, or no device yet).
            const int32_t auto_frames = rex::audio::AutoDeviceSampleFrames();
            row.options.push_back(auto_frames > 0
                                      ? "Auto (" + std::to_string(auto_frames) + ")"
                                      : std::string("Auto"));
          } else {
            row.options.push_back(kAudioBufferLabels[i]);
          }
        }
        row.index = &audio_buffer_index_;
        row.reset = [this] {
          audio_buffer_index_ = AudioBufferIndexFromFrames(
              int32_t(CvarDefaultDouble("audio_device_sample_frames", 0.0)));
        };
        rows.push_back(std::move(row));
      }
      break;
    }
    case 3: {  // Profile
      header("Local Profile");
      {
        RowSpec row;
        row.kind = RowSpec::kEnum;
        row.label = "Profile";
        row.desc = "Select the local player profile used for saves.";
        for (const auto& profile : profiles_.profiles) {
          row.options.push_back(profile.gamertag);
        }
        row.index = &profiles_.selected_index;
        row.on_enum_change = [this](int index) {
          index = std::clamp(index, 0, static_cast<int>(profiles_.profiles.size()) - 1);
          CopyToBuffer(gamertag_buf_, sizeof(gamertag_buf_),
                       profiles_.profiles[index].gamertag);
          profile_signed_in_ = profiles_.profiles[index].signed_in;
        };
        rows.push_back(std::move(row));
      }
      {
        RowSpec row;
        row.kind = RowSpec::kText;
        row.label = "Gamertag";
        row.desc = "Display name for this profile. Select to edit with the keyboard.";
        row.text_buf = gamertag_buf_;
        row.text_buf_size = sizeof(gamertag_buf_);
        rows.push_back(std::move(row));
      }
      {
        RowSpec row;
        row.kind = RowSpec::kEnum;
        row.label = "Local Sign-in";
        row.desc = "Whether this profile is signed in when the game boots.";
        row.options = {"Signed out", "Signed in"};
        row.flag = &profile_signed_in_;
        rows.push_back(std::move(row));
      }
      {
        RowSpec row;
        row.kind = RowSpec::kAction;
        row.label = "Save Profile";
        row.desc = "Save the profile changes above and apply them to the running game.";
        row.action = [this] { SaveProfile(); };
        rows.push_back(std::move(row));
      }
      break;
    }
    case 4: {  // Mods
      if (HasCvar("skate3_penguin_mod")) {
        RowSpec row;
        row.kind = RowSpec::kEnum;
        row.label = "Playable Character";
        row.desc =
            "Switch live between the original create-a-skater and the optional "
            "Seiyu Paradise Penguin Mod. Seiyu requires base.obj and "
            "texture_diffuse.png to be installed from the Android launcher. "
            "Applies immediately.";
        row.options = {"Original Skater", "Seiyu Paradise Penguin"};
        row.flag = &penguin_mod_;
        row.on_enum_change = [this](int value) {
          SetBoolCvar("skate3_penguin_mod", value != 0);
          SaveSimpleSettingsConfig(config_path_);
        };
        row.reset = [this] {
          penguin_mod_ = CvarDefaultBool("skate3_penguin_mod", false);
          SetBoolCvar("skate3_penguin_mod", penguin_mod_);
          SaveSimpleSettingsConfig(config_path_);
        };
        rows.push_back(std::move(row));
      }
      break;
    }
    case 5: {  // System
      if (HasCvar("user_language")) {
        RowSpec row;
        row.kind = RowSpec::kEnum;
        row.label = "Game Language";
        row.desc =
            "Language the game boots in. Skate 3 reads this once at startup, so "
            "changing it needs Apply & Restart.";
        for (const char* label : kLanguageLabels) {
          row.options.push_back(label);
        }
        row.index = &language_index_;
        row.reset = [this] {
          language_index_ =
              std::clamp(int(CvarDefaultDouble("user_language", 1.0)), 1,
                         static_cast<int>(kLanguageLabels.size())) -
              1;
        };
        rows.push_back(std::move(row));
      }
      if (HasCvar("skate3_demo_path")) {
        RowSpec row;
        row.kind = RowSpec::kEnum;
        row.label = "Auto-Boot to Gameplay";
        row.desc =
            "Skip the intro movie, the press-start screen and the profile "
            "prompts on the next launch and drop straight into the world with "
            "your save loaded. The game presses the buttons itself, then stops "
            "sending them the moment gameplay starts. Takes effect next launch.";
        row.value_note = "Applies from the next launch";
        row.options = {"Off", "On"};
        row.flag = &auto_boot_;
        row.on_enum_change = [this](int value) {
          SetBoolCvar("skate3_demo_path", value != 0);
          SaveSimpleSettingsConfig(config_path_);
        };
        row.reset = [this] {
          auto_boot_ = CvarDefaultBool("skate3_demo_path", false);
          SetBoolCvar("skate3_demo_path", auto_boot_);
          SaveSimpleSettingsConfig(config_path_);
        };
        rows.push_back(std::move(row));
      }
      header("Session");
      {
        RowSpec row;
        row.kind = RowSpec::kAction;
        row.label = "Apply & Restart";
        row.desc = pending
                       ? "Save the pending changes and restart the game to apply them."
                       : "No pending changes. Adjust a setting first, then apply it here.";
        row.enabled = pending;
        row.action = [this] { ApplyAndRestart(); };
        rows.push_back(std::move(row));
      }
      {
        RowSpec row;
        row.kind = RowSpec::kAction;
        row.label = "Revert Changes";
        row.desc = "Discard the pending changes and go back to the current settings.";
        row.enabled = pending;
        row.action = [this] { LoadSettingsFromCvars(); };
        rows.push_back(std::move(row));
      }
      {
        RowSpec row;
        row.kind = RowSpec::kAction;
        row.label = "Close Settings";
        row.desc = "Return to the game.";
        row.action = [this] { Hide(); };
        rows.push_back(std::move(row));
      }
      break;
    }
    default:
      break;
  }
}

SimpleSettingsDialog::NavIntents SimpleSettingsDialog::GatherInput(ImGuiIO& io) {
  NavIntents in;

  // ---- Gamepad ----
  SimpleSettingsGamepad pad;
  if (poll_gamepad_) {
    pad = poll_gamepad_();
  }
  // prev_pad_buttons_ starts 0xFFFF on Show so buttons held while opening
  // (e.g. the Back+Start chord) don't fire actions on the first frame.
  const uint16_t pressed = pad.buttons & ~prev_pad_buttons_;
  prev_pad_buttons_ = pad.buttons;

  int dir_x = 0;
  int dir_y = 0;
  if (pad.connected) {
    if (pad.buttons & kPadDpadLeft) {
      dir_x -= 1;
    }
    if (pad.buttons & kPadDpadRight) {
      dir_x += 1;
    }
    if (pad.buttons & kPadDpadUp) {
      dir_y -= 1;
    }
    if (pad.buttons & kPadDpadDown) {
      dir_y += 1;
    }
    constexpr int16_t kDeadzone = 12000;
    if (dir_x == 0) {
      dir_x = pad.thumb_lx <= -kDeadzone ? -1 : (pad.thumb_lx >= kDeadzone ? 1 : 0);
    }
    if (dir_y == 0) {
      // Stick up is positive Y in XInput; menu "up" is -1.
      dir_y = pad.thumb_ly >= kDeadzone ? -1 : (pad.thumb_ly <= -kDeadzone ? 1 : 0);
    }
  }

  auto repeat_axis = [&io](int dir, int& held, float& timer) -> int {
    if (dir == 0) {
      held = 0;
      timer = 0.0f;
      return 0;
    }
    if (dir != held) {
      held = dir;
      timer = 0.0f;
      return dir;  // initial press
    }
    timer += io.DeltaTime;
    if (timer >= kRepeatDelay) {
      timer -= kRepeatRate;
      return dir;
    }
    return 0;
  };
  in.move_x = repeat_axis(dir_x, held_dir_x_, repeat_timer_x_);
  in.move_y = repeat_axis(dir_y, held_dir_y_, repeat_timer_y_);

  if (pressed & kPadA) {
    in.select = true;
  }
  if (pressed & kPadB) {
    in.back = true;
  }
  if (pressed & kPadY) {
    in.reset_default = true;
  }
  if (pressed & kPadX) {
    in.apply_restart = true;
  }
  if (pressed & kPadLShoulder) {
    in.category_prev = true;
  }
  if (pressed & kPadRShoulder) {
    in.category_next = true;
  }
  if (pressed != 0 || dir_x != 0 || dir_y != 0) {
    pad_active_ = true;
  }

  // ---- Keyboard (Escape arrives via the keybind -> NavigateBack) ----
  if (!editing_text_) {
    bool kb = false;
    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow, true)) {
      in.move_y -= 1;
      kb = true;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow, true)) {
      in.move_y += 1;
      kb = true;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow, true)) {
      in.move_x -= 1;
      kb = true;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_RightArrow, true)) {
      in.move_x += 1;
      kb = true;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) ||
        ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false) ||
        ImGui::IsKeyPressed(ImGuiKey_Space, false)) {
      in.select = true;
      kb = true;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Q, false)) {
      in.category_prev = true;
      kb = true;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_E, false)) {
      in.category_next = true;
      kb = true;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_R, false)) {
      in.reset_default = true;
      kb = true;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_F, false)) {
      in.apply_restart = true;
      kb = true;
    }
    if (kb) {
      pad_active_ = false;
    }
  }

  // Mouse motion switches the legend back to keyboard/mouse.
  if (std::abs(io.MousePos.x - mouse_x_) > 2.0f || std::abs(io.MousePos.y - mouse_y_) > 2.0f) {
    if (mouse_x_ >= 0.0f) {
      pad_active_ = false;
    }
  }

  return in;
}

void SimpleSettingsDialog::OnDraw(ImGuiIO& io) {
  if (!visible_) {
    return;
  }

  ImFont* font = imgui_drawer()->ui_font() ? imgui_drawer()->ui_font() : ImGui::GetFont();
  ImFont* bold = imgui_drawer()->ui_font_semibold() ? imgui_drawer()->ui_font_semibold() : font;
  // Coverage-thinned variants for dark text on light panels (rows, section
  // bars, legend chips) - matches the browser's per-polarity text gamma.
  ImFont* bold_ol = imgui_drawer()->ui_font_semibold_on_light()
                        ? imgui_drawer()->ui_font_semibold_on_light()
                        : bold;

  // Layout frame: the letterboxed game image when the presenter reports one,
  // the full window otherwise (boot, no guest output yet). On non-16:9
  // displays the game letterboxes to 16:9 while the window is taller/wider -
  // scaling and anchoring the menu to the window made it read visibly larger
  // than on a 16:9 screen (user report, 16:10 Mac). On 16:9 displays the
  // rectangle equals the window and nothing changes. The blur/backdrop wash
  // still covers the whole window.
  ImVec2 frame_pos(0.0f, 0.0f);
  ImVec2 frame_size = io.DisplaySize;
  if (Presenter* presenter = imgui_drawer()->presenter()) {
    const Presenter::GuestOutputPaintRect rect = presenter->GetLastGuestOutputPaintRect();
    if (rect.width > 0 && rect.height > 0) {
      frame_pos = ImVec2(float(rect.x), float(rect.y));
      frame_size = ImVec2(float(rect.width), float(rect.height));
    }
  }
  const float frame_bottom = frame_pos.y + frame_size.y;

  // Uniform scale: design metrics are authored for 1080p. The menu proper
  // renders at a slightly reduced scale; the footer legend
  // keeps the base viewport scale.
  const float base_s = std::clamp(frame_size.y / 1080.0f, 0.6f, 3.0f);
  const float s = base_s * kMenuScale;

  // Quantize font sizes so the EM (size * upm / (hhea ascent - descent) -
  // the browser's font-size) lands on whole pixels. A fractional em renders
  // measurably wider letter spacing than the browser reference, which snaps
  // css font sizes to the device grid (+1.3% run width at 4K, 370px vs
  // 365px on "Vertical Synchronisation").
  constexpr float kEmPerSize = 2048.0f / 2478.0f;  // Inter upm / (asc - desc)
  auto font_px = [](float size) {
    const float em_quantized = std::round(size * kEmPerSize) / kEmPerSize;
    // Snap to the 1/64px grid GetFontBaked quantizes to (rexglue imgui
    // patch), so the baked size equals the drawn size EXACTLY and RenderText
    // never rescales glyph quads. Stock imgui rounded bakes to whole pixels:
    // the em-quantized 37.51 request baked at 38 and every glyph was
    // GPU-bilinear-minified by 0.987 - the measured ~0.4px vertical smear
    // that kept the menu slightly softer than intended. The 1/64
    // snap moves the em by <0.02% - spacing shift is sub-0.1px per run.
    return std::round(em_quantized * 64.0f) / 64.0f;
  };

  if (just_shown_) {
    // Ignore the pre-open cursor position delta (opening warps/reveals the
    // cursor): it otherwise reads as motion and steals focus from the rail
    // to whatever row happens to sit under the cursor.
    mouse_x_ = io.MousePos.x;
    mouse_y_ = io.MousePos.y;
    just_shown_ = false;
  }

  // ---- Input ----
  NavIntents in = GatherInput(io);
  if (editing_text_) {
    // The text field owns navigation; pad B cancels the edit.
    if (in.back) {
      editing_text_ = false;
    }
    in = NavIntents{};
  }

  const int category_count = static_cast<int>(kCategories.size());
  // The rail holds the categories plus a pinned Close Game entry at the
  // bottom (rail_sel_ == category_count).
  const int quit_rail_index = category_count;
  if (in.category_prev || in.category_next) {
    category_ = (category_ + (in.category_next ? 1 : category_count - 1)) % category_count;
    rail_sel_ = category_;
    row_index_ = 0;
    content_scroll_ = 0.0f;
    content_scroll_anim_ = 0.0f;
    highlight_anim_y_ = -1.0f;
  }
  if (zone_ == FocusZone::kRail && in.move_y != 0) {
    rail_sel_ = std::clamp(rail_sel_ + in.move_y, 0, quit_rail_index);
    if (rail_sel_ < category_count && rail_sel_ != category_) {
      category_ = rail_sel_;
      row_index_ = 0;
      content_scroll_ = 0.0f;
      content_scroll_anim_ = 0.0f;
      highlight_anim_y_ = -1.0f;
    }
  }

  std::vector<RowSpec> rows;
  BuildRows(rows, category_);

  // Selectable row indices (headers and disabled rows are skipped by nav).
  std::vector<int> selectable;
  selectable.reserve(rows.size());
  for (int i = 0; i < static_cast<int>(rows.size()); ++i) {
    if (rows[i].kind != RowSpec::kHeader && rows[i].enabled) {
      selectable.push_back(i);
    }
  }
  auto clamp_focus_to_selectable = [&]() {
    if (selectable.empty()) {
      row_index_ = -1;
      return;
    }
    int best = selectable[0];
    int best_delta = 1 << 30;
    for (int idx : selectable) {
      int delta = std::abs(idx - row_index_);
      if (delta < best_delta) {
        best = idx;
        best_delta = delta;
      }
    }
    row_index_ = best;
  };
  clamp_focus_to_selectable();

  auto enum_value = [](const RowSpec& row) -> int {
    return row.flag ? (*row.flag ? 1 : 0) : (row.index ? *row.index : 0);
  };
  auto set_enum_value = [](RowSpec& row, int value) {
    if (row.flag) {
      *row.flag = value != 0;
    } else if (row.index) {
      *row.index = value;
    }
    if (row.on_enum_change) {
      row.on_enum_change(value);
    }
  };
  // Range semantics: directional stepping (dpad/arrows/chevrons)
  // CLAMPS at the ends - the matching chevron greys out there - while
  // activation (A/Enter/row click) cycles with wrap-around.
  auto step_row = [&](RowSpec& row, int dir) {
    if (row.kind == RowSpec::kEnum) {
      int count = static_cast<int>(row.options.size());
      if (count <= 0) {
        return;
      }
      int value = std::clamp(enum_value(row), 0, count - 1);
      set_enum_value(row, std::clamp(value + dir, 0, count - 1));
    } else if (row.kind == RowSpec::kSlider && row.value) {
      *row.value = std::clamp(*row.value + dir * row.step, row.min, row.max);
      if (row.on_value_change) {
        row.on_value_change();
      }
      SaveSimpleSettingsConfig(config_path_);
    }
  };
  auto activate_row = [&](RowSpec& row) {
    if (!row.enabled) {
      return;
    }
    switch (row.kind) {
      case RowSpec::kEnum: {
        int count = static_cast<int>(row.options.size());
        if (count > 0) {
          int value = std::clamp(enum_value(row), 0, count - 1);
          set_enum_value(row, (value + 1) % count);
        }
        break;
      }
      case RowSpec::kAction:
        if (row.action) {
          row.action();
        }
        break;
      case RowSpec::kText:
        editing_text_ = true;
        text_edit_focus_pending_ = true;
        break;
      default:
        break;
    }
  };

  // ---- Apply navigation intents ----
  bool row_activated_this_frame = false;
  if (zone_ == FocusZone::kRail) {
    if (rail_sel_ == quit_rail_index) {
      if (in.select && close_game_) {
        Hide();
        close_game_();
      }
    } else if ((in.select || in.move_x > 0) && !selectable.empty()) {
      zone_ = FocusZone::kContent;
      row_index_ = selectable[0];
    }
    if (in.back) {
      Hide();
    }
  } else {
    if (in.move_y != 0 && !selectable.empty()) {
      int pos = 0;
      for (int i = 0; i < static_cast<int>(selectable.size()); ++i) {
        if (selectable[i] == row_index_) {
          pos = i;
          break;
        }
      }
      pos = std::clamp(pos + in.move_y, 0, static_cast<int>(selectable.size()) - 1);
      row_index_ = selectable[pos];
    }
    if (in.move_x != 0 && row_index_ >= 0) {
      step_row(rows[row_index_], in.move_x);
    }
    if (in.select && row_index_ >= 0) {
      activate_row(rows[row_index_]);
      row_activated_this_frame = true;
    }
    if (in.back) {
      zone_ = FocusZone::kRail;
    }
  }
  if (in.reset_default && zone_ == FocusZone::kContent && row_index_ >= 0 &&
      rows[row_index_].reset) {
    rows[row_index_].reset();
  }
  if (in.apply_restart && HasSettingsChanges()) {
    ApplyAndRestart();
  }
  if (!visible_) {
    return;  // an action above closed the dialog
  }
  // Actions can invalidate row layout (pending-state rows); rebuild.
  if (row_activated_this_frame || in.reset_default || in.apply_restart) {
    BuildRows(rows, category_);
    selectable.clear();
    for (int i = 0; i < static_cast<int>(rows.size()); ++i) {
      if (rows[i].kind != RowSpec::kHeader && rows[i].enabled) {
        selectable.push_back(i);
      }
    }
    clamp_focus_to_selectable();
  }

  const bool pending = HasSettingsChanges();

  // ---- Layout ----
  // Positional metrics are snapped to whole pixels (see Snap).
  const float margin_x = Snap(std::max(56.0f * s, frame_size.x * 0.05f));
  const float title_size = font_px(42.0f * s);
  // Rail and description columns are equal-width.
  const float rail_w = Snap(300.0f * s);
  const float desc_w = Snap(300.0f * s);
  const float col_gap = Snap(14.0f * s);
  const float footer_h = Snap(96.0f * base_s);
  const float content_bottom = Snap(frame_bottom - footer_h - 18.0f * s);
  float content_w = frame_size.x - 2.0f * margin_x - rail_w - desc_w - 2.0f * col_gap;
  content_w = Snap(std::clamp(content_w, 300.0f * s, 800.0f * s));
  // The column widths are capped, so on wide displays the block would hug the
  // left margin - center the whole menu instead.
  const float menu_total_w = rail_w + content_w + desc_w + 2.0f * col_gap;
  const float rail_x =
      Snap(frame_pos.x + std::max(margin_x, (frame_size.x - menu_total_w) * 0.5f));
  const float content_x = rail_x + rail_w + col_gap;
  const float desc_x = content_x + content_w + col_gap;
  const float menu_right = desc_x + desc_w;

  const float row_h = Snap(52.0f * s);
  const float header_h = row_h;  // section bars match setting rows
  // Gap is 1 design px tighter than the 6*s focus ring, so the ring slightly
  // overlaps neighbours - it draws above them.
  const float row_gap = Snap(5.0f * s);
  const float rail_item_h = Snap(52.0f * s);
  const float label_size = font_px(22.0f * s);
  const float value_size = font_px(22.0f * s);
  const float desc_size = font_px(20.0f * s);
  // The content view holds an exact whole number of row slots: the
  // bottom edge pulls UP to the last full slot, and the scroll target locks
  // to the same pitch, so a resting list always shows complete rows evenly.
  // One slot fewer than fits: playtest verdict - the list otherwise runs too
  // close to the footer. Capacity is measured from the classic flow anchor
  // (title at 13% of the frame) so the slot count keeps its tuned value...
  const float row_pitch = row_h + row_gap;
  const float columns_y_flow = Snap(Snap(frame_pos.y + frame_size.y * 0.13f) + 74.0f * s);
  const int view_rows = std::clamp(
      static_cast<int>((content_bottom - columns_y_flow + row_gap) / row_pitch) - 1, 1,
      kMaxViewRows);
  const float content_view_h = view_rows * row_pitch - row_gap;
  // ...but the block itself is re-anchored so those slots sit vertically
  // CENTERED in the frame (the flow layout read visibly low). The title rides
  // 74*s above the block, the footer stays put; clamps keep the title
  // on-screen and the block clear of the footer.
  const float columns_y = Snap(std::clamp(
      frame_pos.y + (frame_size.y - content_view_h) * 0.5f,
      std::min(frame_pos.y + 98.0f * s, columns_y_flow), content_bottom - content_view_h));
  const float title_y = Snap(columns_y - 74.0f * s);
  const float content_view_bottom = columns_y + content_view_h;
  // Description panel height - also anchors Close Game's bottom edge. When
  // the category rail is long enough to push Close Game below the panel's
  // tuned 320*s height, the panel stretches so its bottom stays level with
  // the button's (the two columns read as one aligned baseline).
  const float rail_close_bottom =
      Snap(columns_y + category_count * (rail_item_h + row_gap) - row_gap +
           45.0f * s) +
      rail_item_h;
  const float desc_panel_h = Snap(std::min(
      content_bottom - columns_y,
      std::max(320.0f * s, rail_close_bottom - columns_y)));

  const ImVec2 mouse = io.MousePos;
  const bool mouse_moved =
      std::abs(mouse.x - mouse_x_) > 2.0f || std::abs(mouse.y - mouse_y_) > 2.0f;
  mouse_x_ = mouse.x;
  mouse_y_ = mouse.y;
  const bool clicked = ImGui::IsMouseClicked(0);
  auto mouse_in = [&mouse](float x0, float y0, float x1, float y1) {
    return mouse.x >= x0 && mouse.x < x1 && mouse.y >= y0 && mouse.y < y1;
  };

  // ---- Window ----
  ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
  ImGui::SetNextWindowSize(io.DisplaySize);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
  if (!ImGui::Begin("##skate3_settings_overlay", nullptr,
                    ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                        ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoCollapse |
                        ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoSavedSettings |
                        ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoBringToFrontOnFocus)) {
    ImGui::End();
    ImGui::PopStyleVar(2);
    return;
  }
  ImDrawList* dl = ImGui::GetWindowDrawList();

  // ---- Backdrop: uniform slightly-dark tint over the whole screen. The
  // native renderer blurs the finished frame while the menu is open
  // (SetSettingsMenuBlur), so the tint lands on a frosted scene.
  dl->AddRectFilled(ImVec2(0.0f, 0.0f), io.DisplaySize, IM_COL32(6, 9, 11, 133));

  // ---- Title ----
  dl->AddText(bold, title_size, ImVec2(Snap(rail_x), title_y), kColText, "Settings");
  if (pending) {
    const char* chip_text = "RESTART REQUIRED TO APPLY";
    float chip_size = font_px(14.0f * s);
    ImVec2 extent = bold->CalcTextSizeA(chip_size, FLT_MAX, 0.0f, chip_text);
    float chip_pad = 10.0f * s;
    float chip_x1 = menu_right;
    float chip_x0 = Snap(chip_x1 - extent.x - 2.0f * chip_pad);
    float chip_y0 = Snap(title_y + title_size * 0.5f - extent.y * 0.5f - 6.0f * s);
    float chip_y1 = Snap(chip_y0 + extent.y + 12.0f * s);
    dl->AddRect(ImVec2(chip_x0, chip_y0), ImVec2(chip_x1, chip_y1), kColWarn, 0.0f, 0,
                1.5f);
    dl->AddText(bold, chip_size, ImVec2(Snap(chip_x0 + chip_pad), Snap(chip_y0 + 6.0f * s)),
                kColWarn, chip_text);
  }
  // ---- Left rail ----
  float rail_focus_target = -1.0f;
  for (int i = 0; i < category_count; ++i) {
    float y0 = columns_y + i * (rail_item_h + row_gap);
    float y1 = y0 + rail_item_h;
    bool is_current = category_ == i;
    bool hovered = mouse_in(rail_x, y0, rail_x + rail_w, y1);
    if (hovered && clicked) {
      category_ = i;
      rail_sel_ = i;
      zone_ = FocusZone::kRail;
      row_index_ = 0;
      content_scroll_ = 0.0f;
      content_scroll_anim_ = 0.0f;
      is_current = true;
    }
    if (is_current && zone_ == FocusZone::kRail && rail_sel_ == i) {
      rail_focus_target = y0;
    }
    // The rounded focus fill covers this item while rail-focused; skip the
    // square panel underneath so its corners don't poke past the radius -
    // but only once the sliding fill (last frame's position: the anim
    // updates just below this loop) has ARRIVED. Skipping - or painting the
    // black selected fill - the moment rail_sel_ changes blanked the target
    // item out from under the still-traveling animation; in flight it keeps
    // its normal teal panel and the fill visibly brings the selection over.
    bool rail_focused = zone_ == FocusZone::kRail && rail_sel_ == i;
    bool fill_arrived = rail_anim_y_ >= 0.0f && std::abs(rail_anim_y_ - y0) < 0.5f;
    if (!(rail_focused && fill_arrived)) {
      ImU32 bg = (is_current && !rail_focused)
                     ? kColSelFill
                     : (hovered ? kColRailPanelHover : kColRailPanel);
      dl->AddRectFilled(ImVec2(rail_x, y0), ImVec2(rail_x + rail_w, y1), bg, 0.0f);
      dl->AddRect(ImVec2(rail_x, y0), ImVec2(rail_x + rail_w, y1), kColRailBorder, 0.0f);
    }
  }
  // Focused rail item: sliding black fill with the lime focus ring, drawn
  // over the panels, then labels.
  if (rail_focus_target >= 0.0f) {
    if (rail_anim_y_ < 0.0f || std::abs(rail_anim_y_ - rail_focus_target) > 160.0f * s) {
      rail_anim_y_ = rail_focus_target;
    }
    rail_anim_y_ += (rail_focus_target - rail_anim_y_) * std::min(1.0f, io.DeltaTime * 22.0f);
    if (std::abs(rail_anim_y_ - rail_focus_target) < 0.5f) {
      rail_anim_y_ = rail_focus_target;  // settle exactly so arrival tests are crisp
    }
    const float fill_y = Snap(rail_anim_y_);
    DrawFocusHighlight(dl, ImVec2(rail_x, fill_y),
                       ImVec2(rail_x + rail_w, fill_y + rail_item_h), s);
  } else {
    rail_anim_y_ = -1.0f;
  }
  for (int i = 0; i < category_count; ++i) {
    float y0 = columns_y + i * (rail_item_h + row_gap);
    bool is_current = category_ == i;
    // Focused text styling waits for the sliding fill to mostly cover the
    // item (same anti-vanish rule as the content rows).
    bool focused = is_current && zone_ == FocusZone::kRail && rail_sel_ == i &&
                   rail_anim_y_ >= 0.0f && std::abs(rail_anim_y_ - y0) < rail_item_h * 0.5f;
    ImU32 text_col = focused ? kColSelText : (is_current ? kColText : kColTextDim);
    AddTextVCentered(dl, bold, label_size, rail_x + 20.0f * s,
                     y0 + rail_item_h * 0.5f, text_col, kCategories[i].name);
    if (is_current && zone_ == FocusZone::kContent) {
      // Arrow hinting that focus lives in the content column.
      float ax = rail_x + rail_w - 18.0f * s;
      float ay = y0 + rail_item_h * 0.5f;
      dl->AddTriangleFilled(ImVec2(ax, ay - 6.0f * s), ImVec2(ax, ay + 6.0f * s),
                            ImVec2(ax + 8.0f * s, ay), kColAccent);
    }
  }

  // Close Game entry: below the categories, set apart by an extra gap.
  {
    // Bottom edge aligned with the description panel's bottom, but
    // never closer to the last category than the 45*s gap the 4-category
    // layout had - with more categories the rail wins and pushes it down.
    const float rail_bottom = columns_y + category_count * (rail_item_h + row_gap) - row_gap;
    float y0 = Snap(std::max(columns_y + desc_panel_h - rail_item_h,
                             rail_bottom + 45.0f * s));
    float y1 = y0 + rail_item_h;
    bool focused = zone_ == FocusZone::kRail && rail_sel_ == quit_rail_index;
    bool hovered = mouse_in(rail_x, y0, rail_x + rail_w, y1);
    if (hovered && clicked && close_game_) {
      Hide();
      close_game_();
    }
    ImU32 bg = focused ? kColDanger : (hovered ? kColRailPanelHover : kColRailPanel);
    dl->AddRectFilled(ImVec2(rail_x, y0), ImVec2(rail_x + rail_w, y1), bg, 0.0f);
    dl->AddRect(ImVec2(rail_x, y0), ImVec2(rail_x + rail_w, y1),
                focused ? kColDanger : kColRailBorder, 0.0f);
    AddTextVCentered(dl, bold, label_size, rail_x + 20.0f * s, (y0 + y1) * 0.5f,
                     focused ? IM_COL32(255, 250, 249, 255) : kColDanger, "Close Game");
  }

  // ---- Content column ----
  // Row geometry (in unscrolled space).
  std::vector<float> row_y(rows.size());
  std::vector<float> row_hgt(rows.size());
  {
    float y = 0.0f;
    for (int i = 0; i < static_cast<int>(rows.size()); ++i) {
      row_y[i] = y;
      row_hgt[i] = rows[i].kind == RowSpec::kHeader ? header_h : row_h;
      y += row_hgt[i] + row_gap;
    }
  }
  const float rows_total_h = rows.empty() ? 0.0f : row_y.back() + row_hgt.back();
  const float max_scroll = std::max(0.0f, rows_total_h - content_view_h);

  // Wheel scroll steps whole rows while over the content column. High-
  // resolution wheels and touchpads report fractional deltas per event, so
  // accumulate them into whole notches first - scaling each tiny delta by
  // the row pitch and then locking to row boundaries would round every one
  // of them straight back to the current position.
  if (mouse_in(content_x, columns_y, content_x + content_w, content_view_bottom)) {
    wheel_accum_ += io.MouseWheel;
    const int notches = static_cast<int>(std::trunc(wheel_accum_));
    if (notches != 0) {
      content_scroll_ -= notches * row_pitch;
      wheel_accum_ -= static_cast<float>(notches);
    }
  } else {
    wheel_accum_ = 0.0f;
  }
  // Keep the focused row in view when navigating.
  if (zone_ == FocusZone::kContent && row_index_ >= 0 && (in.move_y != 0 || in.select)) {
    float y0 = row_y[row_index_];
    float y1 = y0 + row_hgt[row_index_];
    if (y0 - content_scroll_ < 0.0f) {
      content_scroll_ = y0;
    } else if (y1 - content_scroll_ > content_view_h) {
      content_scroll_ = y1 - content_view_h;
    }
  }
  // Lock the resting position onto whole-row boundaries (rows and the view
  // share the same pitch, so max_scroll is itself a pitch multiple).
  content_scroll_ = std::clamp(content_scroll_, 0.0f, max_scroll);
  content_scroll_ =
      std::clamp(std::round(content_scroll_ / row_pitch) * row_pitch, 0.0f, max_scroll);
  // The drawn offset chases the locked target so scrolling slides smoothly
  // instead of rows popping between aligned positions.
  if (content_scroll_anim_ < 0.0f) {
    content_scroll_anim_ = content_scroll_;
  }
  content_scroll_anim_ +=
      (content_scroll_ - content_scroll_anim_) * std::min(1.0f, io.DeltaTime * 16.0f);
  if (std::abs(content_scroll_anim_ - content_scroll_) < 0.5f) {
    content_scroll_anim_ = content_scroll_;  // settle exactly on the boundary
  }
  const float scroll = Snap(content_scroll_anim_);

  // Clip window padded by the focus-ring extent (6*s) so the ring can sit
  // fully OUTSIDE the focused row without being cut off at the edges.
  const float ring_pad = 6.0f * s;
  dl->PushClipRect(ImVec2(content_x - ring_pad, columns_y - ring_pad),
                   ImVec2(content_x + content_w + ring_pad + 1.0f,
                          content_view_bottom + ring_pad),
                   true);

  // Sliding selection highlight (white fill under the focused row).
  const bool content_focus = zone_ == FocusZone::kContent && row_index_ >= 0;
  if (content_focus) {
    float target = row_y[row_index_];
    if (highlight_anim_y_ < 0.0f || std::abs(highlight_anim_y_ - target) > 160.0f * s) {
      highlight_anim_y_ = target;
    }
    highlight_anim_y_ += (target - highlight_anim_y_) * std::min(1.0f, io.DeltaTime * 22.0f);
    if (std::abs(highlight_anim_y_ - target) < 0.5f) {
      highlight_anim_y_ = target;  // settle exactly so arrival tests are crisp
    }
  } else {
    highlight_anim_y_ = -1.0f;
  }

  const float value_w = std::min(390.0f * s, content_w * 0.5f);
  const float chevron_r = 13.0f * s;

  // Rows draw whenever they touch the view: at rest the pitch-locked scroll
  // means nothing is ever cut, and mid-animation the edge rows slide in and
  // out under the clip rect instead of popping.
  auto row_in_view = [&](float y0, float y1) {
    return y1 >= columns_y && y0 <= content_view_bottom;
  };

  // Backgrounds first, then per-row content in TWO phases around the sliding
  // highlight: non-focused content below it, the focused row's content above
  // it - the ring overlaps neighbours (gap is 1 design px tighter than the
  // ring) and must read as the forefront element.
  for (int i = 0; i < static_cast<int>(rows.size()); ++i) {
    const RowSpec& row = rows[i];
    if (row.kind == RowSpec::kHeader) {
      continue;
    }
    float y0 = columns_y + Snap(row_y[i]) - scroll;
    float y1 = y0 + row_hgt[i];
    if (!row_in_view(y0, y1)) {
      continue;
    }
    bool hovered = mouse_in(content_x, y0, content_x + content_w, y1);
    if (hovered && mouse_moved && row.enabled) {
      zone_ = FocusZone::kContent;
      row_index_ = i;
    }
    // The rounded highlight covers the focused row's panel entirely; skip it
    // so its square corners don't poke past the radius - but only once the
    // sliding fill has ARRIVED. Skipping the moment row_index_ changes
    // blanked the target row out from under the still-traveling animation.
    if (content_focus && row_index_ == i &&
        std::abs(highlight_anim_y_ - row_y[i]) < 0.5f) {
      continue;
    }
    ImU32 bg = hovered && row.enabled ? kColPanelHover : kColPanel;
    dl->AddRectFilled(ImVec2(content_x, y0), ImVec2(content_x + content_w, y1), bg, 0.0f);
    dl->AddRect(ImVec2(content_x, y0), ImVec2(content_x + content_w, y1), kColPanelBorder,
                0.0f);
  }

  auto draw_row_content = [&](int i) {
    RowSpec& row = rows[i];
    float y0 = columns_y + Snap(row_y[i]) - scroll;
    float y1 = y0 + row_hgt[i];
    if (!row_in_view(y0, y1)) {
      return;
    }
    float cy = (y0 + y1) * 0.5f;

    if (row.kind == RowSpec::kHeader) {
      // Section header: solid accent bar spanning the content
      // column, dark bold text, label column-aligned with the row labels.
      dl->AddRectFilled(ImVec2(content_x, y0), ImVec2(content_x + content_w, y1), kColAccent,
                        0.0f);
      AddTextVCentered(dl, bold_ol, label_size, content_x + 18.0f * s, cy, kColAccentDark,
                       row.label);
      return;
    }

    // Focused STYLING (white text, polarity-matched weight) waits until the
    // sliding fill mostly covers the row: flipping on row_index_ alone drew
    // white-on-white while the black fill was still in flight.
    const bool focused = content_focus && row_index_ == i &&
                         std::abs(highlight_anim_y_ - row_y[i]) < row_hgt[i] * 0.5f;
    const bool hovered = mouse_in(content_x, y0, content_x + content_w, y1);
    ImFont* row_bold = focused ? bold : bold_ol;  // polarity-matched weight
    ImU32 label_col = focused ? kColSelText
                              : (!row.enabled ? kColTextFaint
                                              : (row.danger ? kColDanger : kColRowText));
    ImU32 value_col = focused ? kColSelText : (row.enabled ? kColRowText : kColTextFaint);

    // Label.
    AddTextVCentered(dl, row_bold, label_size, content_x + 18.0f * s, cy, label_col, row.label);

    // Value area on the right side of the row.
    float vx1 = content_x + content_w - 12.0f * s;
    float vx0 = content_x + content_w - value_w;

    switch (row.kind) {
      case RowSpec::kEnum: {
        int count = static_cast<int>(row.options.size());
        int value = count > 0 ? std::clamp(enum_value(row), 0, count - 1) : 0;
        ImVec2 left_center(vx0 + chevron_r, cy);
        ImVec2 right_center(vx1 - chevron_r, cy);
        // Magenta filled stepper circles with white triangles;
        // a circle greys out (and stops responding) at its end of the range.
        const bool can_left = row.enabled && value > 0;
        const bool can_right = row.enabled && value < count - 1;
        // Hit areas are tracked separately from enablement: a click on a
        // greyed-out stepper must be swallowed, not fall through to the
        // row-body wrap-around cycle below.
        const bool left_hit =
            mouse_in(left_center.x - chevron_r - 4.0f, cy - chevron_r - 4.0f,
                     left_center.x + chevron_r + 4.0f, cy + chevron_r + 4.0f);
        const bool right_hit =
            mouse_in(right_center.x - chevron_r - 4.0f, cy - chevron_r - 4.0f,
                     right_center.x + chevron_r + 4.0f, cy + chevron_r + 4.0f);
        bool left_hover = can_left && left_hit;
        bool right_hover = can_right && right_hit;
        const ImU32 chev_disabled = focused ? kColChevDisabledOnDark : kColChevDisabled;
        DrawChevron(dl, left_center, chevron_r, true,
                    can_left ? (left_hover ? kColInteractHover : kColInteract)
                             : chev_disabled,
                    kColWhite, true);
        DrawChevron(dl, right_center, chevron_r, false,
                    can_right ? (right_hover ? kColInteractHover : kColInteract)
                              : chev_disabled,
                    kColWhite, true);
        if (count > 0) {
          float text_max_w = (right_center.x - chevron_r) - (left_center.x + chevron_r) -
                             16.0f * s;
          // Free-form values (GPU adapter names) can outgrow the slot: step
          // the value text down toward 18 design px first (each candidate
          // em-quantized like every other size), only then ellipsize.
          float fit_size = value_size;
          for (float design = 21.0f;
               design >= 18.0f &&
               row_bold->CalcTextSizeA(fit_size, FLT_MAX, 0.0f, row.options[value].c_str()).x >
                   text_max_w;
               design -= 1.0f) {
            fit_size = font_px(design * s);
          }
          std::string text =
              TruncateToWidth(row_bold, fit_size, text_max_w, row.options[value]);
          AddTextCentered(dl, row_bold, fit_size,
                          ImVec2((left_center.x + right_center.x) * 0.5f, cy), value_col,
                          text.c_str());
        }
        if (clicked && row.enabled) {
          if (left_hit) {
            if (can_left) {
              step_row(row, -1);
            }
          } else if (right_hit) {
            if (can_right) {
              step_row(row, 1);
            }
          } else if (hovered) {
            activate_row(row);  // row-body click cycles with wrap-around
          }
        }
        break;
      }
      case RowSpec::kSlider: {
        float number_w = 52.0f * s;
        float track_x0 = vx0 + 6.0f * s;
        float track_x1 = vx1 - number_w;
        float t = row.max > row.min ? (*row.value - row.min) / (row.max - row.min) : 0.0f;
        float knob_x = track_x0 + t * (track_x1 - track_x0);
        ImU32 track_col = focused ? IM_COL32(255, 255, 255, 77) : IM_COL32(0, 0, 0, 56);
        dl->AddRectFilled(ImVec2(track_x0, cy - 2.0f * s), ImVec2(track_x1, cy + 2.0f * s),
                          track_col, 0.0f);
        dl->AddRectFilled(ImVec2(track_x0, cy - 2.0f * s), ImVec2(knob_x, cy + 2.0f * s),
                          kColInteract, 0.0f);
        dl->AddCircleFilled(ImVec2(Snap(knob_x), Snap(cy)), 8.0f * s, kColInteract, 24);
        char value_text[32];
        std::snprintf(value_text, sizeof(value_text), row.fmt, double(*row.value));
        ImVec2 extent = row_bold->CalcTextSizeA(value_size, FLT_MAX, 0.0f, value_text);
        dl->AddText(row_bold, value_size, ImVec2(Snap(vx1 - extent.x), Snap(cy - extent.y * 0.5f)),
                    value_col, value_text);
        // Mouse drag on the track.
        ImGui::SetCursorScreenPos(ImVec2(track_x0 - 10.0f * s, cy - 14.0f * s));
        char slider_id[48];
        std::snprintf(slider_id, sizeof(slider_id), "##slider_%d_%d", category_, i);
        ImGui::InvisibleButton(slider_id,
                               ImVec2(track_x1 - track_x0 + 20.0f * s, 28.0f * s));
        if (ImGui::IsItemActive() && row.enabled) {
          float nt = std::clamp((mouse.x - track_x0) / std::max(1.0f, track_x1 - track_x0),
                                0.0f, 1.0f);
          float new_value = row.min + nt * (row.max - row.min);
          // Quantize to step for tidy values.
          new_value = row.min + std::round((new_value - row.min) / row.step) * row.step;
          new_value = std::clamp(new_value, row.min, row.max);
          if (new_value != *row.value) {
            *row.value = new_value;
            if (row.on_value_change) {
              row.on_value_change();
            }
          }
          zone_ = FocusZone::kContent;
          row_index_ = i;
        }
        if (ImGui::IsItemDeactivated()) {
          SaveSimpleSettingsConfig(config_path_);
        }
        break;
      }
      case RowSpec::kAction: {
        // No right-side 'Select' / '-' hint: the footer legend covers
        // activation, and disabled rows read from their dim label alone.
        if (clicked && hovered && row.enabled && !row_activated_this_frame) {
          activate_row(row);
          row_activated_this_frame = true;
        }
        break;
      }
      case RowSpec::kText: {
        const bool editing_this = editing_text_ && focused;
        if (editing_this) {
          ImGui::SetCursorScreenPos(ImVec2(vx0 + 6.0f * s, cy - 14.0f * s));
          ImGui::SetNextItemWidth(vx1 - vx0 - 12.0f * s);
          // value_size is already physical; divide out the global widget-font
          // DPI scale (it exists for logical-unit dialogs, see ImGuiDrawer).
          ImGui::PushFont(font, value_size / std::max(0.01f, ImGui::GetStyle().FontScaleDpi));
          // Edit field sits on the BLACK focused row: light text, light frame.
          ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(1, 1, 1, 0.15f));
          ImGui::PushStyleColor(ImGuiCol_Text,
                                ImVec4(0.98f, 0.99f, 0.99f, 1.0f));
          if (text_edit_focus_pending_) {
            ImGui::SetKeyboardFocusHere();
            text_edit_focus_pending_ = false;
          }
          bool committed = ImGui::InputText("##settings_text_edit", row.text_buf,
                                            row.text_buf_size,
                                            ImGuiInputTextFlags_EnterReturnsTrue);
          if (committed || (!ImGui::IsItemActive() && !text_edit_focus_pending_ &&
                            ImGui::IsItemDeactivated())) {
            editing_text_ = false;
          }
          ImGui::PopStyleColor(2);
          ImGui::PopFont();
        } else {
          const char* text = row.text_buf && row.text_buf[0] ? row.text_buf : "-";
          ImVec2 extent = row_bold->CalcTextSizeA(value_size, FLT_MAX, 0.0f, text);
          dl->AddText(row_bold, value_size, ImVec2(Snap(vx1 - extent.x), Snap(cy - extent.y * 0.5f)),
                      value_col, text);
          if (clicked && hovered && row.enabled) {
            zone_ = FocusZone::kContent;
            row_index_ = i;
            editing_text_ = true;
            text_edit_focus_pending_ = true;
          }
        }
        break;
      }
      default:
        break;
    }
  };
  // Phase 1: everything except the focused row.
  for (int i = 0; i < static_cast<int>(rows.size()); ++i) {
    if (!(content_focus && row_index_ == i)) {
      draw_row_content(i);
    }
  }
  // Phase 2: the sliding highlight above neighbours...
  if (content_focus && highlight_anim_y_ >= 0.0f) {
    float y0 = columns_y + Snap(highlight_anim_y_) - scroll;
    DrawFocusHighlight(dl, ImVec2(content_x, y0), ImVec2(content_x + content_w, y0 + row_h),
                       s);
  }
  // ...phase 3: the focused row's own content above the highlight.
  if (content_focus && row_index_ >= 0 && row_index_ < static_cast<int>(rows.size())) {
    draw_row_content(row_index_);
  }
  dl->PopClipRect();

  // Scrollbar hint when the list overflows.
  if (max_scroll > 0.0f) {
    float bar_x = content_x + content_w + 4.0f * s;
    float track_h = content_view_h;
    float bar_h = std::max(24.0f * s, track_h * (content_view_h / rows_total_h));
    float bar_y = columns_y + (track_h - bar_h) * (content_scroll_anim_ / max_scroll);
    dl->AddRectFilled(ImVec2(bar_x, columns_y), ImVec2(bar_x + 3.0f * s, columns_y + track_h),
                      IM_COL32(255, 255, 255, 18), 0.0f);
    dl->AddRectFilled(ImVec2(bar_x, bar_y), ImVec2(bar_x + 3.0f * s, bar_y + bar_h),
                      IM_COL32(255, 255, 255, 90), 0.0f);
  }

  // ---- Description panel ----
  {
    const float panel_h = desc_panel_h;
    const float header_bar_h = row_h;  // header bar matches a settings row
    dl->AddRectFilled(ImVec2(desc_x, columns_y), ImVec2(desc_x + desc_w, columns_y + header_bar_h),
                      kColAccent, 0.0f);
    AddTextVCentered(dl, bold_ol, label_size, desc_x + 18.0f * s, columns_y + header_bar_h * 0.5f,
                     kColAccentDark, "Description");
    dl->AddRectFilled(ImVec2(desc_x, columns_y + header_bar_h),
                      ImVec2(desc_x + desc_w, columns_y + panel_h), kColDescPanel, 0.0f);
    dl->AddRect(ImVec2(desc_x, columns_y), ImVec2(desc_x + desc_w, columns_y + panel_h),
                kColRailBorder, 0.0f);

    const char* desc_text = kCategories[category_].desc;
    if (zone_ == FocusZone::kRail && rail_sel_ == quit_rail_index) {
      desc_text = "Quit to the desktop. Unsaved game progress is lost.";
    }
    const char* note_text = nullptr;
    const std::string* extra_text = nullptr;
    if (zone_ == FocusZone::kContent && row_index_ >= 0 &&
        row_index_ < static_cast<int>(rows.size())) {
      const RowSpec& row = rows[row_index_];
      if (row.desc) {
        desc_text = row.desc;
      }
      note_text = row.value_note;
      if (!row.desc_extra.empty()) {
        extra_text = &row.desc_extra;
      }
    }
    float text_x = Snap(desc_x + 14.0f * s);
    float text_y = Snap(columns_y + header_bar_h + 14.0f * s);
    float wrap_w = desc_w - 28.0f * s;
    text_y = Snap(text_y +
                  AddTextJustified(dl, font, desc_size, ImVec2(text_x, text_y), wrap_w,
                                   kColText, desc_text) +
                  10.0f * s);
    if (extra_text) {
      text_y = Snap(text_y +
                    AddTextJustified(dl, font, desc_size, ImVec2(text_x, text_y), wrap_w,
                                     kColTextDim, extra_text->c_str()) +
                    10.0f * s);
    }
    if (note_text) {
      text_y = Snap(text_y +
                    AddTextJustified(dl, bold, desc_size, ImVec2(text_x, text_y), wrap_w,
                                     kColWarn, note_text) +
                    10.0f * s);
    }
    if (pending) {
      AddTextJustified(dl, font, desc_size, ImVec2(text_x, text_y), wrap_w, kColWarn,
                       "Changes are applied after a restart. Use Apply & Restart when ready.");
    }
  }

  // ---- Footer button legend (base viewport scale, not the menu scale) ----
  {
    const float fs = base_s;
    float legend_y = Snap(frame_bottom - footer_h + 14.0f * fs);
    std::vector<LegendGlyph> glyphs;
    if (pad_active_) {
      glyphs.push_back({"A", "Select", true});
      glyphs.push_back({"B", "Back", true});
      glyphs.push_back({"Y", "Reset to Default", true});
      glyphs.push_back({"LB / RB", "Category", false});
      if (pending) {
        glyphs.push_back({"X", "Apply & Restart", true});
      }
    } else {
      glyphs.push_back({"Enter", "Select", false});
      glyphs.push_back({"Esc", "Back", false});
      glyphs.push_back({"R", "Reset to Default", false});
      glyphs.push_back({"Q / E", "Category", false});
      if (pending) {
        glyphs.push_back({"F", "Apply & Restart", false});
      }
    }
    float x = rail_x;
    float glyph_size = font_px(15.0f * fs);
    float label_text_size = font_px(16.0f * fs);
    float chip_h = Snap(26.0f * fs);
    for (const LegendGlyph& glyph : glyphs) {
      ImVec2 glyph_extent = bold->CalcTextSizeA(glyph_size, FLT_MAX, 0.0f, glyph.glyph);
      float cy = legend_y + chip_h * 0.5f;
      x = Snap(x);
      if (glyph.circle) {
        float r = chip_h * 0.5f;
        dl->AddCircleFilled(ImVec2(x + r, cy), r, kColLegendChip, 28);
        AddTextCenteredCap(dl, bold_ol, glyph_size, ImVec2(x + r, cy), kColLegendText,
                           glyph.glyph, /*ink_x=*/true);
        x += 2.0f * r + 8.0f * fs;
      } else {
        float chip_w = Snap(glyph_extent.x + 18.0f * fs);
        // Hard straight edges + AA corners, like the browser's border-radius
        // (rounded AddRectFilled feathers every edge 1px).
        DrawHardRoundedFill(dl, ImVec2(x, legend_y), ImVec2(x + chip_w, legend_y + chip_h),
                            4.0f * fs, kColLegendChip);
        AddTextCenteredCap(dl, bold_ol, glyph_size, ImVec2(x + chip_w * 0.5f, cy),
                           kColLegendText, glyph.glyph, /*ink_x=*/false);
        x += chip_w + 8.0f * fs;
      }
      ImVec2 label_extent = bold->CalcTextSizeA(label_text_size, FLT_MAX, 0.0f, glyph.label);
      dl->AddText(bold, label_text_size, ImVec2(Snap(x), Snap(cy - label_extent.y * 0.5f)),
                  kColLegendLabel, glyph.label);
      x += label_extent.x + 26.0f * fs;
    }
  }

  ImGui::End();
  ImGui::PopStyleVar(2);
}

}  // namespace rex::ui
