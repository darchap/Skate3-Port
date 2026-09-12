#pragma once

#include <atomic>

#include <rex/ui/windowed_app_context.h>

#include <SDL3/SDL_events.h>

namespace rex {
namespace ui {

class SDLWindowedAppContext final : public WindowedAppContext {
 public:
  SDLWindowedAppContext() = default;
  ~SDLWindowedAppContext() override = default;

  void NotifyUILoopOfPendingFunctions() override;
  void PlatformQuitFromUIThread() override;

  int RunMainLoop();

 private:
  void DispatchEvent(const SDL_Event& event);

  std::atomic_bool pending_functions_event_queued_ = false;
};

}  // namespace ui
}  // namespace rex
