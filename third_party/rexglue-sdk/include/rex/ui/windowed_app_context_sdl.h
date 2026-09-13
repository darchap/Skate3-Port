#pragma once

#include <atomic>
#include <mutex>
#include <vector>

#include <rex/ui/windowed_app_context.h>

#include <SDL3/SDL_events.h>

namespace rex {
namespace ui {

class SDLWindowedAppContext final : public WindowedAppContext {
 public:
  // Installs the lifecycle event watch for the lifetime of the context.
  SDLWindowedAppContext();
  ~SDLWindowedAppContext() override;

  void NotifyUILoopOfPendingFunctions() override;
  void PlatformQuitFromUIThread() override;

  int RunMainLoop();

 private:
  void DispatchEvent(const SDL_Event& event);

  // SDL never queues app lifecycle events; only an event watch sees them. The
  // watch just records the type: SDL holds its watcher lock during the call and
  // other threads' SDL_PushEvent block on it, so handling runs later on the UI
  // thread in DrainLifecycleEvents.
  static bool SDLCALL LifecycleEventWatch(void* userdata, SDL_Event* event);
  void DrainLifecycleEvents();
  void HandleLifecycleEvent(uint32_t event_type);

  std::mutex pending_lifecycle_mutex_;
  std::vector<uint32_t> pending_lifecycle_events_;
  std::atomic_bool pending_functions_event_queued_ = false;
};

}  // namespace ui
}  // namespace rex
