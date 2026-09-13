/**
 * Android app background state, set from the Java main thread on Activity
 * pause and cleared by the runtime on foreground. Readable from any thread.
 * Defined out-of-line in the runtime library on purpose: an inline variable
 * would give the game library its own copy and the presenter would never see
 * the flag.
 */

#pragma once

namespace rex::ui {

#if defined(__GNUC__) || defined(__clang__)
#define REX_UI_LIFECYCLE_API __attribute__((visibility("default")))
#else
#define REX_UI_LIFECYCLE_API
#endif

REX_UI_LIFECYCLE_API void SetAppBackgrounded(bool backgrounded);
REX_UI_LIFECYCLE_API bool IsAppBackgrounded();

// Set from Java once a resume has a surface. SDL drops a pause+resume pair
// that lands before its first pump; the UI loop consumes this and synthesises
// the foreground when the flag is still set.
REX_UI_LIFECYCLE_API void RequestAppForeground();
REX_UI_LIFECYCLE_API bool ConsumeAppForegroundRequest();

}  // namespace rex::ui
