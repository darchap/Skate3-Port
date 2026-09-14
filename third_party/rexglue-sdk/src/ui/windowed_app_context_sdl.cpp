#include <rex/ui/android_background_state.h>
#include <rex/ui/windowed_app_context_sdl.h>
#include <rex/ui/window_sdl.h>

#include <rex/logging.h>

#include <cstdlib>

namespace rex {
namespace ui {

namespace {

constexpr uint32_t kPendingFunctionsEvent = SDL_EVENT_USER;

}  // namespace

void SDLWindowedAppContext::NotifyUILoopOfPendingFunctions() {
  bool expected = false;
  if (!pending_functions_event_queued_.compare_exchange_strong(expected, true)) {
    return;
  }

  SDL_Event event = {};
  event.type = kPendingFunctionsEvent;
  if (!SDL_PushEvent(&event)) {
    pending_functions_event_queued_ = false;
  }
}

void SDLWindowedAppContext::PlatformQuitFromUIThread() {
  SDL_Event event = {};
  event.type = SDL_EVENT_QUIT;
  SDL_PushEvent(&event);
}

SDLWindowedAppContext::SDLWindowedAppContext() {
  SDL_AddEventWatch(LifecycleEventWatch, this);
}

SDLWindowedAppContext::~SDLWindowedAppContext() {
  SDL_RemoveEventWatch(LifecycleEventWatch, this);
}

bool SDLCALL SDLWindowedAppContext::LifecycleEventWatch(void* userdata, SDL_Event* event) {
  if (userdata && event) {
    switch (event->type) {
      case SDL_EVENT_DID_ENTER_BACKGROUND:
      case SDL_EVENT_DID_ENTER_FOREGROUND:
      case SDL_EVENT_TERMINATING:
      case SDL_EVENT_LOW_MEMORY: {
        auto* self = static_cast<SDLWindowedAppContext*>(userdata);
        std::lock_guard<std::mutex> lock(self->pending_lifecycle_mutex_);
        self->pending_lifecycle_events_.push_back(event->type);
        break;
      }
      default:
        break;
    }
  }
  // Lifecycle events are never queued; the return value is irrelevant.
  return true;
}

void SDLWindowedAppContext::DrainLifecycleEvents() {
  std::vector<uint32_t> events;
  {
    std::lock_guard<std::mutex> lock(pending_lifecycle_mutex_);
    events.swap(pending_lifecycle_events_);
  }
  for (uint32_t type : events) {
    HandleLifecycleEvent(type);
  }
#if REX_PLATFORM_ANDROID
  // A resume the Activity reported but SDL never delivered (pause + resume
  // coalesced before the first pump): synthesise the foreground.
  if (ConsumeAppForegroundRequest() && IsAppBackgrounded()) {
    REXLOG_INFO("App foreground synthesised from the Activity resume");
    HandleLifecycleEvent(SDL_EVENT_DID_ENTER_FOREGROUND);
  }
#endif
}

void SDLWindowedAppContext::HandleLifecycleEvent(uint32_t event_type) {
  switch (event_type) {
    case SDL_EVENT_DID_ENTER_BACKGROUND:
      REXLOG_INFO("App entered background");
      if (app_lifecycle_listener_) {
        app_lifecycle_listener_->OnAppEnterBackground();
      }
      break;
    case SDL_EVENT_DID_ENTER_FOREGROUND:
      REXLOG_INFO("App entered foreground");
      if (app_lifecycle_listener_) {
        app_lifecycle_listener_->OnAppEnterForeground();
      } else {
        // No listener yet: clear the flag ourselves or it is never cleared.
        SetAppBackgrounded(false);
      }
      break;
    case SDL_EVENT_TERMINATING:
      REXLOG_INFO("App terminating");
      if (app_lifecycle_listener_) {
        app_lifecycle_listener_->OnAppTerminating();
      }
      break;
    case SDL_EVENT_LOW_MEMORY:
      REXLOG_WARN("App low-memory warning");
      if (app_lifecycle_listener_) {
        app_lifecycle_listener_->OnAppLowMemory();
      }
      break;
    default:
      break;
  }
}

int SDLWindowedAppContext::RunMainLoop() {
  if (HasQuitFromUIThread()) {
    return EXIT_SUCCESS;
  }

  SDL_Event event;
#if REX_PLATFORM_ANDROID
  // SDL's Android joystick backend is updated while pumping the event loop.
  // Keep that update at a deliberate 125 Hz instead of relying on an internal
  // poll-sentinel wake cycle (which made this otherwise-idle thread spin at a
  // full core), while retaining sub-frame controller latency.
  while (!HasQuitFromUIThread()) {
    const bool got_event = SDL_WaitEventTimeout(&event, 8);
    // Handled before the queued event: a foreground precedes its RESIZED.
    DrainLifecycleEvents();
    if (got_event) {
      DispatchEvent(event);
    }
  }
#else
  while (!HasQuitFromUIThread() && SDL_WaitEvent(&event)) {
    DrainLifecycleEvents();
    DispatchEvent(event);
  }

  if (!HasQuitFromUIThread()) {
    REXLOG_WARN("SDL event loop exited unexpectedly: {}", SDL_GetError());
    QuitFromUIThread();
  }
#endif
  DrainLifecycleEvents();
  return EXIT_SUCCESS;
}

void SDLWindowedAppContext::DispatchEvent(const SDL_Event& event) {
  if (event.type == kPendingFunctionsEvent) {
    pending_functions_event_queued_ = false;
    ExecutePendingFunctionsFromUIThread();
    return;
  }

  if (event.type == SDL_EVENT_QUIT) {
    REXLOG_INFO("SDL quit event received");
    QuitFromUIThread();
    return;
  }

  SDLWindow::HandleSDLEvent(event);
}

}  // namespace ui
}  // namespace rex
