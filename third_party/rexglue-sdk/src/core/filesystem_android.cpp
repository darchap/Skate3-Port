#include <rex/filesystem.h>

#if REX_PLATFORM_ANDROID
#include <fcntl.h>
#include <unistd.h>

namespace rex::filesystem {
bool IsAndroidContentUri(const std::string_view source) {
  return source.starts_with("content://");
}

int OpenAndroidContentFileDescriptor(const std::string_view uri, const char* mode) {
  if (IsAndroidContentUri(uri)) return -1;  // SAF/JNI bridge is a later enhancement.
  const int flags = mode && mode[0] == 'w' ? (O_WRONLY | O_CREAT | O_TRUNC) : O_RDONLY;
  return open(std::string(uri).c_str(), flags, 0666);
}
}
#endif
