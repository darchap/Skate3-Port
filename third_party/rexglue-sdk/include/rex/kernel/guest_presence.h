#pragma once

#include <cstdint>

// Live guest presence context, published by XGI XUserSetContext (xam). Lives
// in the shared core library so every module (kernel, graphics backends, the
// host app) reads the same state.

namespace rex::kernel::guest_presence {

// Called from the XGI XUserSetContext handler with every context write.
// Only user 0's presence context 0x8001 is tracked; other writes are ignored.
void NotifyContext(uint32_t user_index, uint32_t context_id, uint32_t value);

// Live value of the Skate 3 presence context 0x8001 (1 = in gameplay,
// 0 = frontend / pause menu / loading).
uint32_t GameplayContextValue();

}  // namespace rex::kernel::guest_presence
