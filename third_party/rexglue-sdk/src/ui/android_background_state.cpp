#include <rex/ui/android_background_state.h>

#include <atomic>

#include <rex/logging.h>

namespace rex::ui {

namespace {
// One definition for the whole process; see the header.
std::atomic<bool> g_app_backgrounded{false};
std::atomic<bool> g_app_foreground_requested{false};
}  // namespace

void SetAppBackgrounded(bool backgrounded) {
  const bool previous = g_app_backgrounded.exchange(backgrounded, std::memory_order_release);
  if (previous != backgrounded) {
    REXLOG_INFO("App background state: {}", backgrounded ? "backgrounded" : "foregrounded");
  }
}

bool IsAppBackgrounded() {
  return g_app_backgrounded.load(std::memory_order_acquire);
}

void RequestAppForeground() {
  g_app_foreground_requested.store(true, std::memory_order_release);
}

bool ConsumeAppForegroundRequest() {
  return g_app_foreground_requested.exchange(false, std::memory_order_acq_rel);
}

}  // namespace rex::ui
