// JNI bridge: mirrors the Activity's paused state into the runtime's
// background flag. Atomic stores only, safe at any point of the lifecycle.

#if defined(__ANDROID__)

#include <android/log.h>
#include <jni.h>

#include <rex/ui/android_background_state.h>

extern "C" JNIEXPORT void JNICALL
Java_io_skate3port_game_Skate3Activity_nativeSetBackgrounded(JNIEnv*, jclass,
                                                             jboolean backgrounded) {
  __android_log_print(ANDROID_LOG_INFO, "Skate3Lifecycle", "nativeSetBackgrounded(%d)",
                      backgrounded != JNI_FALSE);
  rex::ui::SetAppBackgrounded(backgrounded != JNI_FALSE);
}

extern "C" JNIEXPORT void JNICALL
Java_io_skate3port_game_Skate3Activity_nativeNotifyResumed(JNIEnv*, jclass) {
  __android_log_print(ANDROID_LOG_INFO, "Skate3Lifecycle", "nativeNotifyResumed");
  rex::ui::RequestAppForeground();
}

#endif  // defined(__ANDROID__)
