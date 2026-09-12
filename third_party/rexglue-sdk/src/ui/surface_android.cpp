#include <rex/ui/surface_android.h>
namespace rex::ui {
bool AndroidNativeWindowSurface::GetSizeImpl(uint32_t& width, uint32_t& height) const {
  const int w = ANativeWindow_getWidth(window_), h = ANativeWindow_getHeight(window_);
  width = w > 0 ? uint32_t(w) : 0; height = h > 0 ? uint32_t(h) : 0;
  return width && height;
}
}
