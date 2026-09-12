/**
 * @file        ui/rex_app.cpp
 * @brief       ReXApp implementation, compiled as part of the consumer executable
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/rex_app.h>

#include <rex/chrono/clock.h>
#include <rex/cvar.h>
#include <rex/ui/flags.h>
#include <rex/kernel/crt/heap.h>
#include <rex/filesystem.h>
#include <rex/logging/sink.h>
#include <rex/logging.h>
#include <rex/platform.h>
#include <rex/ui/overlay/console_overlay.h>
#include <rex/ui/overlay/debug_overlay.h>
#include <rex/ui/overlay/fps_overlay.h>
#include <rex/ui/overlay/settings_overlay.h>
#include <rex/graphics/graphics_system.h>
#include <rex/graphics/native_rhi.h>
#if REX_HAS_VULKAN
#include <rex/graphics/vulkan/graphics_system.h>
#endif
#if REX_HAS_D3D12
#include <rex/graphics/d3d12/graphics_system.h>
#endif
#include <rex/audio/audio_system.h>
#include <rex/audio/sdl/sdl_audio_system.h>
#include <rex/input/input_system.h>
#include <rex/kernel/init.h>
#include <rex/system.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xthread.h>
#include <rex/ui/graphics_provider.h>
#include <rex/ui/keybinds.h>
#include <rex/version.h>

#if REX_PLATFORM_LINUX && !REX_PLATFORM_ANDROID
#include <gnu/libc-version.h>
#endif
#if REX_PLATFORM_LINUX
#include <sys/utsname.h>
#endif

#if REX_PLATFORM_WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

#include <fmt/format.h>
#include <imgui.h>
#include <toml++/toml.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace rex {

namespace {

constexpr bool kBlockShaderStorageStartup =
#if REX_PLATFORM_WIN32
    true;
#else
    false;
#endif

#if REX_PLATFORM_LINUX
std::string Trim(std::string value) {
  const auto first = value.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) {
    return "";
  }
  const auto last = value.find_last_not_of(" \t\r\n");
  return value.substr(first, last - first + 1);
}

std::string UnquoteOsReleaseValue(std::string value) {
  value = Trim(std::move(value));
  if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
    std::string unquoted;
    unquoted.reserve(value.size() - 2);
    bool escaped = false;
    for (size_t i = 1; i + 1 < value.size(); ++i) {
      const char ch = value[i];
      if (escaped) {
        unquoted.push_back(ch);
        escaped = false;
      } else if (ch == '\\') {
        escaped = true;
      } else {
        unquoted.push_back(ch);
      }
    }
    return unquoted;
  }
  return value;
}

std::optional<std::string> ReadOsReleaseValue(std::string_view key) {
  std::ifstream file("/etc/os-release");
  std::string line;
  while (std::getline(file, line)) {
    const auto equals = line.find('=');
    if (equals == std::string::npos) {
      continue;
    }
    if (std::string_view(line.data(), equals) == key) {
      return UnquoteOsReleaseValue(line.substr(equals + 1));
    }
  }
  return std::nullopt;
}

std::string TruncateForLog(std::string value) {
  constexpr size_t kMaxLength = 512;
  if (value.size() <= kMaxLength) {
    return value;
  }
  value.resize(kMaxLength);
  value += "...";
  return value;
}

void LogEnvIfSet(const char* name) {
  const char* value = std::getenv(name);
  if (value && *value) {
    REXLOG_INFO("  {}={}", name, TruncateForLog(value));
  }
}

void LogLinuxRuntimeDiagnostics() {
  REXLOG_INFO("Linux runtime diagnostics:");

  const auto pretty_name = ReadOsReleaseValue("PRETTY_NAME");
  const auto id = ReadOsReleaseValue("ID");
  const auto version_id = ReadOsReleaseValue("VERSION_ID");
  if (pretty_name) {
    REXLOG_INFO("  OS: {}", *pretty_name);
  }
  if (id || version_id) {
    REXLOG_INFO("  OS ID: {} {}", id.value_or("unknown"), version_id.value_or(""));
  }

  utsname uts = {};
  if (uname(&uts) == 0) {
    REXLOG_INFO("  Kernel: {} {} {}", uts.sysname, uts.release, uts.machine);
  }
#if !REX_PLATFORM_ANDROID
  REXLOG_INFO("  glibc: {}", gnu_get_libc_version());
#else
  REXLOG_INFO("  libc: Android bionic");
#endif

#if defined(__clang__)
  REXLOG_INFO("  Compiler: clang {}", __clang_version__);
#elif defined(__GNUC__)
  REXLOG_INFO("  Compiler: GCC {}.{}.{}", __GNUC__, __GNUC_MINOR__, __GNUC_PATCHLEVEL__);
#endif

  REXLOG_INFO("Linux session environment:");
  LogEnvIfSet("XDG_SESSION_TYPE");
  LogEnvIfSet("XDG_CURRENT_DESKTOP");
  LogEnvIfSet("DESKTOP_SESSION");
  LogEnvIfSet("DISPLAY");
  LogEnvIfSet("WAYLAND_DISPLAY");
  LogEnvIfSet("GDK_BACKEND");
  LogEnvIfSet("SDL_VIDEODRIVER");
  LogEnvIfSet("LD_LIBRARY_PATH");

  REXLOG_INFO("Steam runtime environment:");
  LogEnvIfSet("SteamAppId");
  LogEnvIfSet("SteamGameId");
  LogEnvIfSet("STEAM_COMPAT_APP_ID");
  LogEnvIfSet("STEAM_COMPAT_CLIENT_INSTALL_PATH");
  LogEnvIfSet("STEAM_COMPAT_DATA_PATH");
  LogEnvIfSet("STEAM_RUNTIME");
  LogEnvIfSet("STEAM_RUNTIME_LIBRARY_PATH");
  LogEnvIfSet("STEAM_RUNTIME_HEAVY");
  LogEnvIfSet("PRESSURE_VESSEL_RUNTIME");
  LogEnvIfSet("PRESSURE_VESSEL_APP_ID");
  LogEnvIfSet("container");

  REXLOG_INFO("Steam Deck / gamescope environment:");
  LogEnvIfSet("SteamDeck");
  LogEnvIfSet("STEAMOS");
  LogEnvIfSet("GAMESCOPE_WAYLAND_DISPLAY");
  LogEnvIfSet("GAMESCOPE_EXTERNAL_OVERLAY");
  LogEnvIfSet("ENABLE_GAMESCOPE_WSI");
  LogEnvIfSet("MESA_VK_WSI_PRESENT_MODE");
  LogEnvIfSet("RADV_PERFTEST");
  LogEnvIfSet("VK_ICD_FILENAMES");
  LogEnvIfSet("VK_DRIVER_FILES");
}

#endif

// Once a close is requested the process must terminate even if a teardown
// step deadlocks (a wedged GPU worker or guest thread otherwise leaves a
// not-responding process the user has to kill): give the orderly shutdown a
// grace window, then force the exit.
void StartForcedExitWatchdog(const char* reason) {
  std::thread([reason]() {
    std::this_thread::sleep_for(std::chrono::seconds(10));
    // The teardown this guards can wedge with the process heap lock orphaned
    // (a guest thread suspended or terminated mid-allocation), so the kill
    // path must not allocate or run lock-taking shutdown: logging and
    // std::_Exit are both off-limits on Windows (ExitProcess acquires the
    // heap lock inside RtlExitUserProcess, deadlocking the watchdog itself).
    // TerminateProcess skips user-mode cleanup entirely; on POSIX std::_Exit
    // is a plain exit_group with no lock use. The marker is emitted through
    // heap-free primitives only.
    char msg[160];
    std::snprintf(msg, sizeof(msg), "%s watchdog terminating process after shutdown timeout\n",
                  reason);
#if REX_PLATFORM_WIN32
    OutputDebugStringA(msg);
    HANDLE log_handle = GetStdHandle(STD_ERROR_HANDLE);
    if (log_handle != INVALID_HANDLE_VALUE && log_handle != nullptr) {
      DWORD written = 0;
      WriteFile(log_handle, msg, DWORD(std::strlen(msg)), &written, nullptr);
    }
    TerminateProcess(GetCurrentProcess(), EXIT_SUCCESS);
#else
    ssize_t written = write(STDERR_FILENO, msg, std::strlen(msg));
    (void)written;
    std::_Exit(EXIT_SUCCESS);
#endif
  }).detach();
}

}  // namespace

REXCVAR_DEFINE_BOOL(advanced_settings_overlay_enabled, false, "UI/Advanced",
                    "Enable the developer cvar browser on F4")
    .debug_only();

REXCVAR_DEFINE_STRING(gpu_backend, "auto", "GPU",
                      "Graphics API used for rendering (auto, d3d12, vulkan). Auto prefers "
                      "Direct3D 12 when available. Applied at startup; when the selected API "
                      "fails to initialize, any other compiled-in backend is tried.")
    .allowed({"auto", "d3d12", "vulkan"})
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_BOOL(show_fps_counter, false, "UI", "Show the guest FPS counter overlay")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

// --- ReXApp ---

ReXApp::~ReXApp() = default;

ReXApp::ReXApp(ui::WindowedAppContext& ctx, std::string_view name, PPCImageInfo ppc_info,
               std::string_view usage)
    : WindowedApp(ctx, name, usage), ppc_info_(ppc_info) {}

std::string_view ReXApp::GetBuildTitle() const {
  return REXGLUE_BUILD_TITLE;
}

std::string_view ReXApp::GetBuildStamp() const {
  return REXGLUE_BUILD_STAMP;
}

std::string ReXApp::GetWindowTitle() const {
  return std::string(GetName()) + " " + std::string(GetBuildTitle());
}

bool ReXApp::OnInitialize() {
  if (!SetupEnvironment())
    return false;
  if (!SetupPresentation())
    return false;

  auto paths = OnFinalizePaths(resolved_defaults_, MakeResumeCallback());
  if (!paths) {
    // Async: consumer will invoke resume when ready. OnInitialize returns
    // true so the event loop keeps pumping (wizard dialogs render).
    return true;
  }

#if REX_PLATFORM_MAC
  // Let the platform event loop run once before runtime setup. SDL creates the
  // Cocoa window synchronously, but the app may not visibly activate if guest
  // initialization starts before the first event-loop pass.
  app_context().CallInUIThreadDeferred([this, paths = std::move(*paths)]() mutable {
    if (shutting_down_.load(std::memory_order_acquire))
      return;
    if (!ConstructRuntime(paths)) {
      app_context().QuitFromUIThread();
      return;
    }
    LaunchModule();
  });
#else
  if (!ConstructRuntime(*paths))
    return false;
  LaunchModule();
#endif
  return true;
}

bool ReXApp::SetupEnvironment() {
  // App root, not executable folder: inside a macOS .app bundle the config
  // and logs stay next to the bundle, where users can find them.
#if REX_PLATFORM_ANDROID
  // /proc/self/exe is /system/bin/app_process64 on Android, which is
  // read-only and not the application's root. The activity supplies
  // XDG_DATA_HOME as its private files directory before SDL_main starts.
  auto exe_dir = rex::filesystem::GetUserFolder();
#else
  auto exe_dir = rex::filesystem::GetAppRootFolder();
#endif
  auto config_path = exe_dir / (std::string(GetName()) + ".toml");

  // Load config before resolving cvar-backed paths such as game_data_root.
  if (std::filesystem::exists(config_path))
    rex::cvar::LoadConfig(config_path);

  toml::table config_table;
  bool has_config_table = false;
  if (std::filesystem::exists(config_path)) {
    try {
      config_table = toml::parse_file(config_path.string());
      has_config_table = true;
    } catch (const toml::parse_error&) {
      has_config_table = false;
    }
  }

  auto config_string = [&](std::string_view key) -> std::optional<std::string> {
    if (!has_config_table) {
      return std::nullopt;
    }
    if (auto value = config_table[key].value<std::string>()) {
      return *value;
    }
    return std::nullopt;
  };

  std::filesystem::path game_dir;
  if (auto config_value = config_string("game_data_root")) {
    game_dir = *config_value;
  }
  std::string game_data_cvar = REXCVAR_GET(game_data_root);
  if (!game_data_cvar.empty()) {
    game_dir = game_data_cvar;
  }

  // User data: cvar override, or platform user directory
  std::filesystem::path user_dir;
  if (auto config_value = config_string("user_data_root")) {
    user_dir = *config_value;
  }
  std::string user_data_cvar = REXCVAR_GET(user_data_root);
  if (!user_data_cvar.empty()) {
    user_dir = user_data_cvar;
  }
  if (user_dir.empty()) {
    user_dir = rex::filesystem::GetUserFolder() / GetName();
  }

  // Update data: cvar override, or empty (opt-in)
  std::filesystem::path update_dir;
  if (auto config_value = config_string("update_data_root")) {
    update_dir = *config_value;
  }
  std::string update_data_cvar = REXCVAR_GET(update_data_root);
  if (!update_data_cvar.empty()) {
    update_dir = update_data_cvar;
  }

  // Cache: cvar override, or user_dir/cache
  std::filesystem::path cache_dir;
  if (auto config_value = config_string("cache_path")) {
    cache_dir = *config_value;
  }
  std::string cache_path_cvar = REXCVAR_GET(cache_path);
  if (!cache_path_cvar.empty()) {
    cache_dir = cache_path_cvar;
  }
  if (cache_dir.empty()) {
    cache_dir = user_dir / "cache";
  }

  PathConfig path_config{game_dir, user_dir, update_dir, cache_dir, config_path};
  OnConfigurePaths(path_config);
  game_data_root_ = path_config.game_data_root;
  user_data_root_ = path_config.user_data_root;
  update_data_root_ = path_config.update_data_root;
  cache_root_ = path_config.cache_root;
  config_path_ = path_config.config_path;
  resolved_defaults_ = std::move(path_config);

  // Native-render RHI shader bytecode cache: the D3D12 backend runtime-
  // compiles its HLSL, and the largest scene shader costs seconds on the
  // render thread at first-frame pipeline creation; cached bytecode turns
  // every launch after the first into a file read. Must be set before the
  // graphics system creates its first shader.
  if (!cache_root_.empty()) {
    graphics::nrhi::SetShaderBytecodeCacheDirectory(
        (cache_root_ / "nrhi_shaders").string().c_str());
  }

  // Late-phase logging
  std::string log_file_cvar = REXCVAR_GET(log_file);
  std::string log_level_str = REXCVAR_GET(log_level);
  if (REXCVAR_GET(log_verbose) && log_level_str == "info")
    log_level_str = "trace";

  auto category_levels = rex::ParseCategoryLevelsFromConfig(config_path_);
  auto log_config = rex::BuildLogConfig(log_file_cvar.empty() ? nullptr : log_file_cvar.c_str(),
                                        log_level_str, category_levels);
  if (log_file_cvar.empty()) {
    log_config.app_name = std::string(GetName());
    log_config.log_dir = (exe_dir / "logs").string();
  }

  rex::InitLogging(log_config);
  rex::RegisterLogLevelCallback();

  log_sink_ = std::make_shared<rex::LogCaptureSink>();
  rex::AddSink(log_sink_);

  OnPostInitLogging();

  if (std::filesystem::exists(config_path_))
    REXLOG_INFO("Loaded config: {}", config_path_.filename().string());

  // Include the build title so support logs identify the exact build.
  if (auto build_title = GetBuildTitle(); !build_title.empty()) {
    REXLOG_INFO("{} starting {}", GetName(), build_title);
  } else {
    REXLOG_INFO("{} starting", GetName());
  }
  if (!game_data_root_.empty()) {
    REXLOG_INFO("  Game directory: {}", game_data_root_.string());
  }
  if (!user_data_root_.empty()) {
    REXLOG_INFO("  User data:      {}", user_data_root_.string());
  }
  if (!update_data_root_.empty()) {
    REXLOG_INFO("  Update data:    {}", update_data_root_.string());
  }
  REXLOG_INFO("  Cache root:     {}", cache_root_.string());
#if REX_PLATFORM_LINUX
  LogLinuxRuntimeDiagnostics();
#endif

  return true;
}

bool ReXApp::ConstructRuntime(const PathConfig& paths) {
  if (paths.game_data_root.empty()) {
    auto msg = std::string("--game_data_root was not provided.");
    REXLOG_ERROR("{}", msg);
    rex::ShowSimpleMessageBox(rex::SimpleMessageBoxType::Error, msg);
    return false;
  }
  if (!std::filesystem::is_directory(paths.game_data_root)) {
    auto msg = fmt::format("--game_data_root does not exist: {}", paths.game_data_root.string());
    REXLOG_ERROR("{}", msg);
    rex::ShowSimpleMessageBox(rex::SimpleMessageBoxType::Error, msg);
    return false;
  }

  runtime_ = std::make_unique<rex::Runtime>(paths.game_data_root, paths.user_data_root,
                                            paths.update_data_root, paths.cache_root);
  runtime_->set_app_context(&app_context());
  config_.config_path = paths.config_path;

  // Recompiled guest code is linked into the host executable, while the kernel
  // imports live in rexruntime. Keep the exe-side clock state in sync with
  // Runtime::Setup so mftb and KeQueryPerformanceFrequency agree.
  rex::chrono::Clock::set_guest_tick_frequency(50000000);
  rex::chrono::Clock::set_guest_system_time_base(rex::chrono::Clock::QueryHostSystemTime());
  rex::chrono::Clock::set_guest_time_scalar(1.0);
  auto guest_tick_ratio = rex::chrono::Clock::guest_tick_ratio();
  REXLOG_INFO("Host guest clock initialized: frequency={} ratio={}/{}",
              rex::chrono::Clock::guest_tick_frequency(), guest_tick_ratio.first,
              guest_tick_ratio.second);

  // Window and ImGui drawer already exist from SetupPresentation; publish them
  // to the runtime before Setup so hooks and native rendering see them.
  if (window_) {
    runtime_->set_display_window(window_.get());
  }
  if (imgui_drawer_) {
    runtime_->set_imgui_drawer(imgui_drawer_.get());
  }

  auto status = runtime_->Setup(ppc_info_, std::move(config_));
  if (XFAILED(status)) {
    REXLOG_ERROR("Runtime setup failed: {:08X}", status);
    return false;
  }

  if (window_ && runtime_->input_system()) {
    static_cast<rex::input::InputSystem*>(runtime_->input_system())->AttachWindow(window_.get());
  }

  if (ppc_info_.register_modules) {
    ppc_info_.register_modules(runtime_->kernel_state());
  }

  if (imgui_drawer_) {
    auto* input_sys = static_cast<rex::input::InputSystem*>(runtime_->input_system());
    if (input_sys) {
      input_sys->SetActiveCallback([this]() {
        if (!imgui_drawer_->HasDialogs()) {
          return true;
        }
        const auto& io = imgui_drawer_->GetIO();
        return !io.WantCaptureMouse && !io.WantCaptureKeyboard;
      });
    }
  }

  std::string xex_image = "game:\\default.xex";
  OnLoadXexImage(xex_image);

  // Mirrors the game:\ / d:\ -> game_data_root mapping in Runtime::SetupVfs.
  {
    constexpr std::string_view kGameDevice = "game:\\";
    constexpr std::string_view kDDevice = "d:\\";
    std::string_view tail = xex_image;
    if (tail.starts_with(kGameDevice)) {
      tail.remove_prefix(kGameDevice.size());
    } else if (tail.starts_with(kDDevice)) {
      tail.remove_prefix(kDDevice.size());
    }
    std::string host_tail{tail};
    std::replace(host_tail.begin(), host_tail.end(), '\\', '/');
    auto xex_host = paths.game_data_root / host_tail;
    if (!std::filesystem::is_regular_file(xex_host)) {
      auto msg = fmt::format("Entrypoint XEX not found: {}", xex_host.string());
      REXLOG_ERROR("{}", msg);
      rex::ShowSimpleMessageBox(rex::SimpleMessageBoxType::Error, msg);
      return false;
    }
  }

  status = runtime_->LoadXexImage(xex_image);
  if (XFAILED(status)) {
    auto msg = fmt::format("Failed to load XEX ({}): {:08X}", xex_image, status);
    REXLOG_ERROR("{}", msg);
    rex::ShowSimpleMessageBox(rex::SimpleMessageBoxType::Error, msg);
    return false;
  }

  OnPostLoadXexImage();

  if (ppc_info_.rexcrt_heap) {
    if (!rex::kernel::crt::InitHeap(REXCVAR_GET(rexcrt_heap_size_mb), runtime_->memory())) {
      REXLOG_ERROR("Failed to initialize rexcrt heap");
      return false;
    }
  }

  OnPostSetup();

  return true;
}

bool ReXApp::SetupPresentation() {
  // Candidate graphics backends in preference order. The gpu_backend cvar
  // (already loaded from the config/settings files by SetupEnvironment)
  // reorders the list at runtime; "auto" keeps Direct3D 12 first when it is
  // compiled in.
  struct GraphicsCandidate {
    const char* id;    // gpu_backend cvar value
    const char* name;  // log-facing name
    std::unique_ptr<rex::system::IGraphicsSystem> (*make)();
  };
  std::vector<GraphicsCandidate> candidates;
#if REX_HAS_D3D12
  candidates.push_back({"d3d12", "Direct3D 12", []() -> std::unique_ptr<rex::system::IGraphicsSystem> {
                          return std::make_unique<rex::graphics::d3d12::D3D12GraphicsSystem>();
                        }});
#endif
#if REX_HAS_VULKAN
  candidates.push_back({"vulkan", "Vulkan", []() -> std::unique_ptr<rex::system::IGraphicsSystem> {
                          return std::make_unique<rex::graphics::vulkan::VulkanGraphicsSystem>();
                        }});
#endif
  const std::string requested = REXCVAR_GET(gpu_backend);
  if (requested != "auto" && !candidates.empty()) {
    bool available = false;
    for (size_t i = 0; i < candidates.size(); ++i) {
      if (requested == candidates[i].id) {
        GraphicsCandidate wanted = candidates[i];
        candidates.erase(candidates.begin() + i);
        candidates.insert(candidates.begin(), wanted);
        available = true;
        break;
      }
    }
    if (!available) {
      REXLOG_WARN("gpu_backend={} is not available in this build; using {}", requested,
                  candidates[0].id);
    }
  }

  size_t candidate_index = 0;
  if (!candidates.empty()) {
    config_.graphics = candidates[candidate_index].make();
  }
  config_.audio_factory = REX_AUDIO_BACKEND(rex::audio::sdl::SDLAudioSystem);
  config_.input_factory = REX_INPUT_BACKEND(rex::input::CreateDefaultInputSystem);
  config_.kernel_init = rex::kernel::InitializeKernel;

  rex::system::IGraphicsSystem* selected_graphics = config_.graphics.get();
  OnPreSetup(config_);
  // A consumer may substitute its own graphics system in OnPreSetup; the
  // candidate fallback below only applies to the stock selection.
  const bool stock_graphics = config_.graphics.get() == selected_graphics;

  if (config_.graphics) {
    for (;;) {
      X_STATUS status = config_.graphics->SetupPresentation(&app_context());
      if (!XFAILED(status)) {
        if (stock_graphics && !candidates.empty()) {
          REXLOG_INFO("Graphics backend: {} (gpu_backend={})",
                      candidates[candidate_index].name, requested);
        }
        break;
      }
      REXLOG_ERROR("Graphics presentation setup failed: {:08X}", status);
      if (!stock_graphics || candidate_index + 1 >= candidates.size()) {
        return false;
      }
      ++candidate_index;
      REXLOG_WARN("Trying the {} graphics backend instead", candidates[candidate_index].name);
      config_.graphics = candidates[candidate_index].make();
    }
  }

  // Create window
  window_ = rex::ui::Window::Create(app_context(), GetName(), 1280, 720);
  if (!window_) {
    REXLOG_ERROR("Failed to create window");
    return false;
  }

  window_->SetTitle(GetWindowTitle());

  window_->AddListener(this);
  window_->AddInputListener(this, 0);

  if (REXCVAR_GET(fullscreen)) {
    window_->SetFullscreen(true);
  }
  window_->Open();

  auto* graphics_system = static_cast<rex::graphics::GraphicsSystem*>(config_.graphics.get());
  if (graphics_system && graphics_system->presenter()) {
    auto* presenter = graphics_system->presenter();
    auto* provider = graphics_system->provider();
    if (provider) {
      immediate_drawer_ = provider->CreateImmediateDrawer();
      if (immediate_drawer_) {
        immediate_drawer_->SetPresenter(presenter);
        imgui_drawer_ = std::make_unique<rex::ui::ImGuiDrawer>(
            window_.get(), 64, [this](ImFontAtlas* atlas) { OnConfigureFonts(atlas); });
        imgui_drawer_->SetPresenterAndImmediateDrawer(presenter, immediate_drawer_.get());
        if (!frame_stats_provider_) {
          frame_stats_provider_ = [presenter]() {
            ui::Presenter::GuestFrameStats stats = presenter->GetGuestFrameStats();
            return ui::FrameStats{stats.frame_time_ms, stats.fps, stats.frame_count};
          };
        }
        auto update_guest_frame_stats_enabled = [this, presenter]() {
          presenter->SetGuestFrameStatsEnabled(fps_overlay_ != nullptr || debug_overlay_ != nullptr);
        };
        rex::ui::RegisterBind("bind_fps_counter", "F2", "Toggle FPS counter", [this, presenter] {
          if (fps_overlay_) {
            fps_overlay_.reset();
          } else {
            fps_overlay_ =
                std::make_unique<ui::FpsOverlayDialog>(imgui_drawer_.get(), presenter);
          }
          REXCVAR_SET(show_fps_counter, fps_overlay_ != nullptr);
          presenter->SetGuestFrameStatsEnabled(fps_overlay_ != nullptr || debug_overlay_ != nullptr);
          if (!config_path_.empty()) {
            rex::cvar::SaveConfigValues(config_path_, {"show_fps_counter"});
          }
        });
        if (REXCVAR_GET(show_fps_counter)) {
          fps_overlay_ = std::make_unique<ui::FpsOverlayDialog>(imgui_drawer_.get(), presenter);
        }
        // The settings menu (and console/config reload) write show_fps_counter
        // through cvar::SetFlagByName - without a change callback the value
        // only stuck on disk and the overlay object never followed (the menu
        // toggle looked dead). Add/RemoveDialog are safe mid-draw: the drawer
        // iterates by index and adjusts it on removal. The F2 bind above
        // already syncs the overlay itself before setting the cvar, so this
        // callback no-ops for that path.
        rex::cvar::RegisterChangeCallback(
            "show_fps_counter", [this, presenter](std::string_view, std::string_view) {
              const bool want = REXCVAR_GET(show_fps_counter);
              if (want == (fps_overlay_ != nullptr)) {
                return;
              }
              if (want) {
                fps_overlay_ =
                    std::make_unique<ui::FpsOverlayDialog>(imgui_drawer_.get(), presenter);
              } else {
                fps_overlay_.reset();
              }
              presenter->SetGuestFrameStatsEnabled(fps_overlay_ != nullptr ||
                                                   debug_overlay_ != nullptr);
            });
        rex::ui::RegisterBind("bind_debug_overlay", "F3", "Toggle debug overlay", [this] {
          if (debug_overlay_) {
            debug_overlay_.reset();
          } else {
            debug_overlay_ = std::make_unique<ui::DebugOverlayDialog>(
                imgui_drawer_.get(), frame_stats_provider_, GetBuildStamp());
          }
          auto* graphics_system = static_cast<rex::graphics::GraphicsSystem*>(config_.graphics.get());
          if (graphics_system && graphics_system->presenter()) {
            graphics_system->presenter()->SetGuestFrameStatsEnabled(
                fps_overlay_ != nullptr || debug_overlay_ != nullptr);
          }
        });
        update_guest_frame_stats_enabled();
        rex::ui::RegisterBind("bind_console", "Backtick", "Toggle console overlay", [this] {
          if (console_overlay_) {
            console_overlay_.reset();
          } else {
            console_overlay_ = std::make_unique<ui::ConsoleDialog>(imgui_drawer_.get(), log_sink_);
          }
        });
        rex::ui::RegisterBind("bind_settings", "F4", "Toggle settings overlay", [this] {
          if (!REXCVAR_GET(advanced_settings_overlay_enabled)) {
            return;
          }
          if (settings_overlay_) {
            settings_overlay_.reset();
          } else {
            settings_overlay_ =
                std::make_unique<ui::SettingsDialog>(imgui_drawer_.get(), config_path_);
          }
        });

        OnCreateDialogs(imgui_drawer_.get());
      }
    }
    window_->SetPresenter(presenter);
  }

  return true;
}

void ReXApp::LaunchModule() {
  app_context().CallInUIThreadDeferred([this]() {
    OnPreLaunchModule();

    auto main_thread = runtime_->PrepareModuleLaunch();
    if (!main_thread) {
      REXLOG_ERROR("Failed to launch module");
      app_context().QuitFromUIThread();
      return;
    }
#if REX_PLATFORM_MAC
    main_thread_ = main_thread;
#endif

    auto* graphics_system =
        static_cast<rex::graphics::GraphicsSystem*>(runtime_->graphics_system());
    if (graphics_system && !runtime_->cache_root().empty()) {
      uint32_t title_id = runtime_->kernel_state()->title_id();
      if (title_id != 0) {
        REXLOG_INFO("Initializing shader storage for title {:08X}...", title_id);
        graphics_system->InitializeShaderStorage(runtime_->cache_root(), title_id,
                                                kBlockShaderStorageStartup);
      }
    }

    OnPostLaunchModule(main_thread.get());
    main_thread->Resume();

    module_thread_ = std::thread([this, main_thread = std::move(main_thread)]() mutable {
      main_thread->Wait(0, 0, 0, nullptr);
      OnGuestThreadExit(main_thread.get());
      REXLOG_INFO("Execution complete");
      if (!shutting_down_.load(std::memory_order_acquire)) {
        app_context().CallInUIThread([this]() { app_context().QuitFromUIThread(); });
      }
    });
  });
}

std::function<void(PathConfig)> ReXApp::MakeResumeCallback() {
  return [this](PathConfig paths) {
    if (shutting_down_.load(std::memory_order_acquire))
      return;
    if (!ConstructRuntime(std::move(paths))) {
      app_context().QuitFromUIThread();
      return;
    }
    LaunchModule();
  };
}

void ReXApp::OnKeyDown(ui::KeyEvent& e) {
  rex::ui::ProcessKeyEvent(e);
}

void ReXApp::OnClosing(ui::UIEvent& e) {
  (void)e;
  REXLOG_INFO("Window closing, shutting down...");
  StartForcedExitWatchdog("Window close");
  shutting_down_.store(true, std::memory_order_release);
#if REX_PLATFORM_MAC
  if (main_thread_ && main_thread_->is_running()) {
    main_thread_->Terminate(0);
  }
#endif
  if (runtime_ && runtime_->kernel_state()) {
    runtime_->kernel_state()->TerminateTitle();
  }
  app_context().QuitFromUIThread();
}

void ReXApp::OnDestroy() {
  // Notify subclass before cleanup
  OnShutdown();

#if REX_PLATFORM_MAC
  shutting_down_.store(true, std::memory_order_release);
  if (main_thread_ && main_thread_->is_running()) {
    main_thread_->Terminate(0);
  }
#endif

  // Unregister overlay keybinds before destroying dialogs
  rex::ui::UnregisterBind("bind_fps_counter");
  rex::ui::UnregisterBind("bind_debug_overlay");
  rex::ui::UnregisterBind("bind_console");
  rex::ui::UnregisterBind("bind_settings");
  // The show_fps_counter callback captures `this` - drop it before teardown.
  rex::cvar::UnregisterChangeCallbacks("show_fps_counter");

  // ImGui cleanup (reverse of setup)
  fps_overlay_.reset();
  settings_overlay_.reset();
  console_overlay_.reset();
  debug_overlay_.reset();
  if (imgui_drawer_) {
    imgui_drawer_->SetPresenterAndImmediateDrawer(nullptr, nullptr);
    imgui_drawer_.reset();
  }
  if (immediate_drawer_) {
    immediate_drawer_->SetPresenter(nullptr);
    immediate_drawer_.reset();
  }
  if (runtime_) {
    runtime_->set_display_window(nullptr);
    runtime_->set_imgui_drawer(nullptr);
  }
  // Window/runtime cleanup
  if (window_) {
    window_->SetPresenter(nullptr);
  }
  if (module_thread_.joinable()) {
    module_thread_.join();
  }
#if REX_PLATFORM_MAC
  main_thread_ = nullptr;
#endif
  // Input drivers may still be registered as window listeners. Detach them
  // before destroying the SDL window so their backend state is released in
  // listener order.
  if (runtime_ && runtime_->input_system()) {
    static_cast<rex::input::InputSystem*>(runtime_->input_system())->Shutdown();
  }
  if (window_) {
    window_->RemoveInputListener(this);
    window_->RemoveListener(this);
  }
  window_.reset();
  runtime_.reset();
}

void ReXApp::SetGuestFrameStats(ui::DebugOverlayDialog::FrameStatsProvider provider) {
  frame_stats_provider_ = provider;
  if (debug_overlay_) {
    debug_overlay_->SetStatsProvider(provider);
  }
}

}  // namespace rex
