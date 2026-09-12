#include <rex/main_android.h>

#if REX_PLATFORM_ANDROID
#include <android/api-level.h>

namespace rex {
int GetAndroidApiLevel() { return android_get_device_api_level(); }
}
#endif
