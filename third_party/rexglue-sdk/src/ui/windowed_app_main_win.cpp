/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2021 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <cstdlib>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/platform.h>
#include <rex/ui/windowed_app.h>
#include <rex/ui/windowed_app_context_win.h>

REXCVAR_DEFINE_BOOL(win32_high_resolution_timer, true, "Win32",
                    "Requests high-resolution timer from the NT kernel");
REXCVAR_DEFINE_BOOL(win32_mmcss, true, "Win32",
                    "Opt in the Multimedia Class Scheduler Service (MMCSS) scheduling for "
                    "prioritized access to CPU resources");

namespace {

void RequestWin32HighResolutionTimer() {
  HMODULE ntdll_module = GetModuleHandleW(L"ntdll.dll");
  if (!ntdll_module) {
    return;
  }

  // Xenia requests the maximum NT timer resolution directly rather than using
  // timeBeginPeriod(1), which is coarser and may override this setting in SDL.
  using NtQueryTimerResolutionFn = LONG(NTAPI*)(PULONG minimum_resolution,
                                                PULONG maximum_resolution,
                                                PULONG current_resolution);
  using NtSetTimerResolutionFn = LONG(NTAPI*)(ULONG desired_resolution,
                                              BOOLEAN set_resolution,
                                              PULONG current_resolution);
  auto nt_query_timer_resolution = reinterpret_cast<NtQueryTimerResolutionFn>(
      GetProcAddress(ntdll_module, "NtQueryTimerResolution"));
  auto nt_set_timer_resolution =
      reinterpret_cast<NtSetTimerResolutionFn>(GetProcAddress(ntdll_module, "NtSetTimerResolution"));
  if (!nt_query_timer_resolution || !nt_set_timer_resolution) {
    return;
  }

  ULONG minimum_resolution = 0;
  ULONG maximum_resolution = 0;
  ULONG current_resolution = 0;
  nt_query_timer_resolution(&minimum_resolution, &maximum_resolution, &current_resolution);
  nt_set_timer_resolution(maximum_resolution, TRUE, &current_resolution);
}

void RequestWin32MMCSS() {
  HMODULE dwmapi_module = LoadLibraryW(L"dwmapi.dll");
  if (!dwmapi_module) {
    return;
  }

  using DwmEnableMMCSSFn = HRESULT(STDAPICALLTYPE*)(BOOL enable_mmcss);
  auto dwm_enable_mmcss =
      reinterpret_cast<DwmEnableMMCSSFn>(GetProcAddress(dwmapi_module, "DwmEnableMMCSS"));
  if (dwm_enable_mmcss) {
    dwm_enable_mmcss(TRUE);
  }
  FreeLibrary(dwmapi_module);
}

// Convert wide argv from CommandLineToArgvW to UTF-8 argc/argv for cvar::Init
std::vector<std::string> WideArgsToUtf8(int argc, wchar_t** wargv) {
  std::vector<std::string> args;
  args.reserve(static_cast<size_t>(argc));
  for (int i = 0; i < argc; ++i) {
    std::wstring wide(wargv[i]);
    if (wide.empty()) {
      args.emplace_back();
      continue;
    }
    int size = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr,
                                   0, nullptr, nullptr);
    std::string utf8(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), utf8.data(), size,
                        nullptr, nullptr);
    args.push_back(std::move(utf8));
  }
  return args;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE hinstance, HINSTANCE hinstance_prev, LPWSTR command_line,
                    int show_cmd) {
  (void)hinstance_prev;
  (void)command_line;

  // Convert wide command line to UTF-8 argc/argv and parse CVARs
  int wargc = 0;
  wchar_t** wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
  auto utf8_args = WideArgsToUtf8(wargc, wargv);
  LocalFree(wargv);

  // Build char* argv for cvar::Init
  std::vector<char*> argv_ptrs;
  argv_ptrs.reserve(utf8_args.size());
  for (auto& s : utf8_args) {
    argv_ptrs.push_back(s.data());
  }
  auto remaining = rex::cvar::Init(static_cast<int>(argv_ptrs.size()), argv_ptrs.data());
  rex::cvar::ApplyEnvironment();
  rex::InitLoggingEarly();

  // Mirrors Xenia's InitializeWin32App timing setup: parse cvars first, then
  // request high-performance timing and compositor scheduling before windows
  // and render/audio/input subsystems start.
  if (REXCVAR_GET(win32_high_resolution_timer)) {
    RequestWin32HighResolutionTimer();
  }
  if (REXCVAR_GET(win32_mmcss)) {
    RequestWin32MMCSS();
  }

  int result;

  {
    rex::ui::Win32WindowedAppContext app_context(hinstance, show_cmd);
    // TODO(Triang3l): Initialize creates a window. Set DPI awareness via the
    // manifest.
    if (!app_context.Initialize()) {
      return EXIT_FAILURE;
    }

    std::unique_ptr<rex::ui::WindowedApp> app = rex::ui::GetWindowedAppCreator()(app_context);

    // Match remaining positional args to app's expected options
    const auto& option_names = app->GetPositionalOptions();
    std::map<std::string, std::string> parsed;
    size_t count = std::min(remaining.size(), option_names.size());
    for (size_t i = 0; i < count; ++i) {
      parsed[option_names[i]] = remaining[i];
    }
    app->SetParsedArguments(std::move(parsed));

    // Initialize COM on the UI thread with the apartment-threaded concurrency
    // model, so dialogs can be used.
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
      return EXIT_FAILURE;
    }

    result = app->OnInitialize() ? app_context.RunMainMessageLoop() : EXIT_FAILURE;

    app->InvokeOnDestroy();
  }

  // TODO: Port ShutdownWin32App from Xenia
  // Logging may still be needed in the destructors.
  // rex::ShutdownWin32App();

  CoUninitialize();

  return result;
}
