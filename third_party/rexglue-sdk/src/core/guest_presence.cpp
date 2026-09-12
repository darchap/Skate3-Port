#include <rex/kernel/guest_presence.h>

#include <atomic>

namespace rex::kernel::guest_presence {
namespace {

std::atomic<uint32_t> g_gameplay_context_value{0};

}  // namespace

void NotifyContext(uint32_t user_index, uint32_t context_id, uint32_t value) {
  constexpr uint32_t kGameplayContext = 0x8001;
  if (user_index != 0 || context_id != kGameplayContext) {
    return;
  }
  g_gameplay_context_value.store(value, std::memory_order_relaxed);
}

uint32_t GameplayContextValue() {
  return g_gameplay_context_value.load(std::memory_order_relaxed);
}

}  // namespace rex::kernel::guest_presence
