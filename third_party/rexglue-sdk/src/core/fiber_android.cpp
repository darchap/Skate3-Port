#include <rex/platform.h>
#if REX_PLATFORM_ANDROID

#include <rex/thread/fiber.h>
#include <cassert>

namespace rex::thread {
thread_local Fiber* Fiber::tls_current_ = nullptr;

// AAPCS64 context: x19-x28, frame pointer, link register, stack pointer,
// and the callee-saved SIMD registers d8-d15.
extern "C" __attribute__((naked)) void rex_android_fiber_switch(void** from, void* to) {
  __asm__ volatile(
      "sub sp, sp, #176\n"
      "stp x19, x20, [sp, #0]\n" "stp x21, x22, [sp, #16]\n"
      "stp x23, x24, [sp, #32]\n" "stp x25, x26, [sp, #48]\n"
      "stp x27, x28, [sp, #64]\n" "stp x29, x30, [sp, #80]\n"
      "stp d8, d9, [sp, #96]\n" "stp d10, d11, [sp, #112]\n"
      "stp d12, d13, [sp, #128]\n" "stp d14, d15, [sp, #144]\n"
      "mov x2, sp\n" "str x2, [x0]\n" "mov sp, x1\n"
      "ldp x19, x20, [sp, #0]\n" "ldp x21, x22, [sp, #16]\n"
      "ldp x23, x24, [sp, #32]\n" "ldp x25, x26, [sp, #48]\n"
      "ldp x27, x28, [sp, #64]\n" "ldp x29, x30, [sp, #80]\n"
      "ldp d8, d9, [sp, #96]\n" "ldp d10, d11, [sp, #112]\n"
      "ldp d12, d13, [sp, #128]\n" "ldp d14, d15, [sp, #144]\n"
      "add sp, sp, #176\n" "ret\n");
}

Fiber* Fiber::ConvertCurrentThread() {
  auto* f = new Fiber(); f->is_thread_fiber_ = true; tls_current_ = f; return f;
}
Fiber* Fiber::Create(size_t stack_size, void (*entry)(void*), void* arg) {
  auto* f = new Fiber(); f->entry_ = entry; f->arg_ = arg; f->stack_.resize(stack_size + 16);
  uintptr_t top = (reinterpret_cast<uintptr_t>(f->stack_.data() + f->stack_.size()) & ~uintptr_t(15));
  top -= 176; auto* ctx = reinterpret_cast<uint64_t*>(top); for (int i = 0; i < 22; ++i) ctx[i] = 0;
  ctx[11] = reinterpret_cast<uint64_t>(&Fiber::Trampoline); f->sp_ = reinterpret_cast<void*>(top);
  return f;
}
void Fiber::Trampoline() { Fiber* f = tls_current_; f->entry_(f->arg_); __builtin_trap(); }
void Fiber::SwitchTo(Fiber* target) { Fiber* from = tls_current_; tls_current_ = target; rex_android_fiber_switch(&from->sp_, target->sp_); }
void Fiber::Destroy() { if (is_thread_fiber_) tls_current_ = nullptr; else assert(this != tls_current_); delete this; }
}
#endif
