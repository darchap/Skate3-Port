#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/platform.h>
#include <rex/ui/windowed_app.h>
#include <rex/ui/windowed_app_context_sdl.h>

#include <SDL3/SDL.h>
#if REX_PLATFORM_ANDROID
#include <SDL3/SDL_main.h>
#endif

int main(int argc, char** argv) {
  auto remaining = rex::cvar::Init(argc, argv);
  rex::cvar::ApplyEnvironment();
  rex::InitLoggingEarly();

#if REX_PLATFORM_ANDROID
  // SDL_Init(SDL_INIT_VIDEO) starts Android's platform plumbing before the
  // input driver is constructed. Disable desktop-style HIDAPI discovery here
  // so the handheld's Android InputDevice gamepad is the only scanner ever
  // initialized; setting this later leaves HIDAPI_JoystickDetect in every
  // event pump even though no HID controller is used.
  SDL_SetHintWithPriority(SDL_HINT_JOYSTICK_HIDAPI, "0", SDL_HINT_OVERRIDE);
  // Keep the UI thread pumping while backgrounded: a parked UI thread deadlocks
  // every CallInUIThreadSynchronous. Pausing is done by the lifecycle listener.
  SDL_SetHintWithPriority(SDL_HINT_ANDROID_BLOCK_ON_PAUSE, "0", SDL_HINT_OVERRIDE);
#endif

#if REX_PLATFORM_MAC
  // Use native macOS fullscreen so the menu bar and app switching behave like
  // other Mac apps. MoltenVK presentation is paced separately on macOS.
  SDL_SetHint(SDL_HINT_VIDEO_MAC_FULLSCREEN_SPACES, "1");
  SDL_SetHint(SDL_HINT_VIDEO_MAC_FULLSCREEN_MENU_VISIBILITY, "1");
#endif

  if (!SDL_Init(SDL_INIT_VIDEO)) {
    std::fprintf(stderr, "Failed to initialize SDL video: %s\n", SDL_GetError());
    return EXIT_FAILURE;
  }

  int result = EXIT_FAILURE;
  {
    rex::ui::SDLWindowedAppContext app_context;
    std::unique_ptr<rex::ui::WindowedApp> app = rex::ui::GetWindowedAppCreator()(app_context);

    const auto& option_names = app->GetPositionalOptions();
    std::map<std::string, std::string> parsed;
    size_t count = std::min(remaining.size(), option_names.size());
    for (size_t i = 0; i < count; ++i) {
      parsed[option_names[i]] = remaining[i];
    }
    app->SetParsedArguments(std::move(parsed));

    bool initialized = app->OnInitialize();
    result = initialized ? app_context.RunMainLoop() : EXIT_FAILURE;
#if REX_PLATFORM_MAC
    // Skip app/runtime teardown entirely: guest threads cannot be reliably
    // stopped on Darwin (pthread_cancel only lands at cancellation points,
    // never in CPU-bound recompiled code), so the destructor chain races
    // still-running guest threads over freed kernel objects and turns a
    // normal quit into the macOS crash-reporter dialog. Flush the logs and
    // leave; the OS reclaims everything else. Flush-only on purpose -
    // ShutdownLogging destroys sinks a straggler thread may still be using.
    rex::FlushLogging();
    std::_Exit(result);
#else
    app->InvokeOnDestroy();
#endif
  }

  rex::ShutdownLogging();
  SDL_Quit();
  return result;
}
