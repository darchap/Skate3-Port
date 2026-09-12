#pragma once

#include <memory>
#include <string>
#include <atomic>

#include <rex/ui/menu_item.h>
#include <rex/ui/window.h>

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_metal.h>
#include <SDL3/SDL_timer.h>
#include <SDL3/SDL_video.h>

namespace rex {
namespace ui {

class SDLWindow final : public Window {
  using super = Window;

 public:
  SDLWindow(WindowedAppContext& app_context, const std::string_view title,
            uint32_t desired_logical_width, uint32_t desired_logical_height);
  ~SDLWindow() override;

  static void HandleSDLEvent(const SDL_Event& event);

  void SetTextInputActive(bool active) override;

 protected:
  uint32_t GetLatestDpiImpl() const override;
  float QueryDisplayRefreshHzImpl() const override;
  bool OpenImpl() override;
  void RequestCloseImpl() override;

  void ApplyNewFullscreen() override;
  void ApplyNewTitle() override;
  void ApplyNewMouseCapture() override;
  void ApplyNewMouseRelease() override;
  void ApplyNewCursorVisibility(CursorVisibility old_cursor_visibility) override;
  void FocusImpl() override;

  std::unique_ptr<Surface> CreateSurfaceImpl(Surface::TypeFlags allowed_types) override;
  void RequestPaintImpl() override;

 private:
  void HandleEvent(const SDL_Event& event);
  void HandleSizeUpdate(WindowDestructionReceiver& destruction_receiver);
  void HandleKey(const SDL_KeyboardEvent& event, bool down,
                 WindowDestructionReceiver& destruction_receiver);
  void HandleMouseButton(const SDL_MouseButtonEvent& event, bool down,
                         WindowDestructionReceiver& destruction_receiver);
  void HandleMouseMotion(const SDL_MouseMotionEvent& event,
                         WindowDestructionReceiver& destruction_receiver);
  void HandleMouseWheel(const SDL_MouseWheelEvent& event,
                        WindowDestructionReceiver& destruction_receiver);
  void HandleTextInput(const SDL_TextInputEvent& event,
                       WindowDestructionReceiver& destruction_receiver);
  void RevealAutoHiddenCursor();
  void SetCursorAutoHideTimer();
  void RemoveCursorAutoHideTimer();
  void WindowPointToPhysical(float x, float y, int32_t& physical_x, int32_t& physical_y) const;
  uint32_t QueryDpi() const;

  SDL_Window* window_ = nullptr;
  SDL_MetalView metal_view_ = nullptr;
  void* metal_layer_ = nullptr;
  std::atomic<bool> paint_event_queued_{false};
  SDL_TimerID cursor_auto_hide_timer_ = 0;
  bool cursor_currently_auto_hidden_ = false;
  uint32_t dpi_ = 96;
};

class SDLMenuItem final : public MenuItem {
 public:
  SDLMenuItem(Type type, const std::string& text, const std::string& hotkey,
              std::function<void()> callback)
      : MenuItem(type, text, hotkey, std::move(callback)) {}
};

}  // namespace ui
}  // namespace rex
