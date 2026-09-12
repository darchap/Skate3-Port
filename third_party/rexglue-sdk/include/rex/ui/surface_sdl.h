#pragma once

#include <rex/ui/surface.h>

#include <SDL3/SDL_metal.h>
#include <SDL3/SDL_video.h>

namespace rex {
namespace ui {

class SDLMetalViewSurface final : public Surface {
 public:
  explicit SDLMetalViewSurface(SDL_Window* window, SDL_MetalView metal_view,
                               void* metal_layer)
      : window_(window), metal_view_(metal_view), metal_layer_(metal_layer) {}

  TypeIndex GetType() const override { return kTypeIndex_SDLMetalView; }
  SDL_MetalView metal_view() const { return metal_view_; }
  void* metal_layer() const { return metal_layer_; }

 protected:
  bool GetSizeImpl(uint32_t& width_out, uint32_t& height_out) const override;

 private:
  SDL_Window* window_;
  SDL_MetalView metal_view_;
  void* metal_layer_;
};

}  // namespace ui
}  // namespace rex
