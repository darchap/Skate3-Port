#include <rex/ui/surface_sdl.h>

namespace rex {
namespace ui {

bool SDLMetalViewSurface::GetSizeImpl(uint32_t& width_out, uint32_t& height_out) const {
  int width = 0;
  int height = 0;
  if (!window_ || !SDL_GetWindowSizeInPixels(window_, &width, &height) || width <= 0 ||
      height <= 0) {
    width_out = 0;
    height_out = 0;
    return false;
  }
  width_out = uint32_t(width);
  height_out = uint32_t(height);
  return true;
}

}  // namespace ui
}  // namespace rex
