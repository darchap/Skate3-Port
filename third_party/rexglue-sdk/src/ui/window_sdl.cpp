#include <rex/ui/window_sdl.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <unordered_map>

#include <rex/assert.h>
#include <rex/cvar.h>
#include <rex/graphics/flags.h>
#include <rex/graphics/video_mode_util.h>
#include <rex/logging.h>
#include <rex/platform.h>
#include <rex/ui/surface_sdl.h>
#if REX_PLATFORM_ANDROID
#include <rex/ui/surface_android.h>
#include <SDL3/SDL_properties.h>
#endif
#include <rex/ui/virtual_key.h>

#include <SDL3/SDL_hints.h>
#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_mouse.h>
#include <SDL3/SDL_scancode.h>
#include <SDL3/SDL_timer.h>

namespace {

constexpr uint32_t kDefaultDpi = 96;
constexpr uint32_t kDefaultCursorAutoHideMilliseconds = 1000;
constexpr uint32_t kCursorAutoHideEvent = SDL_EVENT_USER + 1;

// Enabled on macOS: without a high-DPI backing the Metal drawable stays at
// logical point size and CoreAnimation upscales the whole window to the Retina
// panel, blurring everything (the settings UI most visibly). Internal 3D render
// cost is governed separately by the resolution-scale cvars, so this only makes
// the final composite and UI render at native panel resolution.
REXCVAR_DEFINE_BOOL(sdl_high_pixel_density, true, "UI/SDL",
                    "Request high-DPI backing pixels for SDL windows")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

std::unordered_map<SDL_WindowID, rex::ui::SDLWindow*>& WindowMap() {
  static std::unordered_map<SDL_WindowID, rex::ui::SDLWindow*> windows;
  return windows;
}

Uint32 SDLCALL AutoHideCursorTimerCallback(void* userdata, SDL_TimerID timer_id,
                                           Uint32 interval) {
  (void)interval;
  SDL_Event event = {};
  event.type = kCursorAutoHideEvent;
  event.user.data1 = userdata;
  event.user.data2 = reinterpret_cast<void*>(uintptr_t(timer_id));
  SDL_PushEvent(&event);
  return 0;
}

uint32_t ResolveWindowWidth(uint32_t requested_width) {
  if (REXCVAR_GET(window_width) > 0) {
    return uint32_t(REXCVAR_GET(window_width));
  }
  if (!rex::cvar::HasNonDefaultValue("window_width")) {
    if (rex::cvar::HasNonDefaultValue("video_mode_width") && REXCVAR_GET(video_mode_width) > 0) {
      return uint32_t(std::clamp(REXCVAR_GET(video_mode_width), 1, 8192));
    }
    int32_t preset_width = 0;
    int32_t preset_height = 0;
    if (rex::graphics::video_mode_util::TryGetResolutionPresetFromCVar(preset_width,
                                                                       preset_height)) {
      return uint32_t(std::clamp(preset_width, 1, 8192));
    }
  }
  return requested_width;
}

uint32_t ResolveWindowHeight(uint32_t requested_height) {
  if (REXCVAR_GET(window_height) > 0) {
    return uint32_t(REXCVAR_GET(window_height));
  }
  if (!rex::cvar::HasNonDefaultValue("window_height")) {
    if (rex::cvar::HasNonDefaultValue("video_mode_height") && REXCVAR_GET(video_mode_height) > 0) {
      return uint32_t(std::clamp(REXCVAR_GET(video_mode_height), 1, 8192));
    }
    int32_t preset_width = 0;
    int32_t preset_height = 0;
    if (rex::graphics::video_mode_util::TryGetResolutionPresetFromCVar(preset_width,
                                                                       preset_height)) {
      return uint32_t(std::clamp(preset_height, 1, 8192));
    }
  }
  return requested_height;
}

using rex::ui::VirtualKey;

VirtualKey TranslateSDLScancode(SDL_Scancode scancode) {
  switch (scancode) {
    case SDL_SCANCODE_F1:
      return VirtualKey::kF1;
    case SDL_SCANCODE_F2:
      return VirtualKey::kF2;
    case SDL_SCANCODE_F3:
      return VirtualKey::kF3;
    case SDL_SCANCODE_F4:
      return VirtualKey::kF4;
    case SDL_SCANCODE_F5:
      return VirtualKey::kF5;
    case SDL_SCANCODE_F6:
      return VirtualKey::kF6;
    case SDL_SCANCODE_F7:
      return VirtualKey::kF7;
    case SDL_SCANCODE_F8:
      return VirtualKey::kF8;
    case SDL_SCANCODE_F9:
      return VirtualKey::kF9;
    case SDL_SCANCODE_F10:
      return VirtualKey::kF10;
    case SDL_SCANCODE_F11:
      return VirtualKey::kF11;
    case SDL_SCANCODE_F12:
      return VirtualKey::kF12;
    case SDL_SCANCODE_ESCAPE:
      return VirtualKey::kEscape;
    case SDL_SCANCODE_RETURN:
    case SDL_SCANCODE_KP_ENTER:
      return VirtualKey::kReturn;
    case SDL_SCANCODE_SPACE:
      return VirtualKey::kSpace;
    case SDL_SCANCODE_TAB:
      return VirtualKey::kTab;
    case SDL_SCANCODE_BACKSPACE:
      return VirtualKey::kBack;
    case SDL_SCANCODE_DELETE:
      return VirtualKey::kDelete;
    case SDL_SCANCODE_INSERT:
      return VirtualKey::kInsert;
    case SDL_SCANCODE_HOME:
      return VirtualKey::kHome;
    case SDL_SCANCODE_END:
      return VirtualKey::kEnd;
    case SDL_SCANCODE_PAGEUP:
      return VirtualKey::kPrior;
    case SDL_SCANCODE_PAGEDOWN:
      return VirtualKey::kNext;
    case SDL_SCANCODE_LEFT:
      return VirtualKey::kLeft;
    case SDL_SCANCODE_RIGHT:
      return VirtualKey::kRight;
    case SDL_SCANCODE_UP:
      return VirtualKey::kUp;
    case SDL_SCANCODE_DOWN:
      return VirtualKey::kDown;
    case SDL_SCANCODE_LSHIFT:
      return VirtualKey::kLShift;
    case SDL_SCANCODE_RSHIFT:
      return VirtualKey::kRShift;
    case SDL_SCANCODE_LCTRL:
      return VirtualKey::kLControl;
    case SDL_SCANCODE_RCTRL:
      return VirtualKey::kRControl;
    case SDL_SCANCODE_LALT:
      return VirtualKey::kLMenu;
    case SDL_SCANCODE_RALT:
      return VirtualKey::kRMenu;
    case SDL_SCANCODE_LGUI:
      return VirtualKey::kLWin;
    case SDL_SCANCODE_RGUI:
      return VirtualKey::kRWin;
    case SDL_SCANCODE_APPLICATION:
      return VirtualKey::kApps;
    case SDL_SCANCODE_CAPSLOCK:
      return VirtualKey::kCapital;
    case SDL_SCANCODE_NUMLOCKCLEAR:
      return VirtualKey::kNumLock;
    case SDL_SCANCODE_SCROLLLOCK:
      return VirtualKey::kScroll;
    case SDL_SCANCODE_PRINTSCREEN:
      return VirtualKey::kSnapshot;
    case SDL_SCANCODE_PAUSE:
      return VirtualKey::kPause;
    case SDL_SCANCODE_0:
      return VirtualKey::k0;
    case SDL_SCANCODE_1:
      return VirtualKey::k1;
    case SDL_SCANCODE_2:
      return VirtualKey::k2;
    case SDL_SCANCODE_3:
      return VirtualKey::k3;
    case SDL_SCANCODE_4:
      return VirtualKey::k4;
    case SDL_SCANCODE_5:
      return VirtualKey::k5;
    case SDL_SCANCODE_6:
      return VirtualKey::k6;
    case SDL_SCANCODE_7:
      return VirtualKey::k7;
    case SDL_SCANCODE_8:
      return VirtualKey::k8;
    case SDL_SCANCODE_9:
      return VirtualKey::k9;
    case SDL_SCANCODE_A:
      return VirtualKey::kA;
    case SDL_SCANCODE_B:
      return VirtualKey::kB;
    case SDL_SCANCODE_C:
      return VirtualKey::kC;
    case SDL_SCANCODE_D:
      return VirtualKey::kD;
    case SDL_SCANCODE_E:
      return VirtualKey::kE;
    case SDL_SCANCODE_F:
      return VirtualKey::kF;
    case SDL_SCANCODE_G:
      return VirtualKey::kG;
    case SDL_SCANCODE_H:
      return VirtualKey::kH;
    case SDL_SCANCODE_I:
      return VirtualKey::kI;
    case SDL_SCANCODE_J:
      return VirtualKey::kJ;
    case SDL_SCANCODE_K:
      return VirtualKey::kK;
    case SDL_SCANCODE_L:
      return VirtualKey::kL;
    case SDL_SCANCODE_M:
      return VirtualKey::kM;
    case SDL_SCANCODE_N:
      return VirtualKey::kN;
    case SDL_SCANCODE_O:
      return VirtualKey::kO;
    case SDL_SCANCODE_P:
      return VirtualKey::kP;
    case SDL_SCANCODE_Q:
      return VirtualKey::kQ;
    case SDL_SCANCODE_R:
      return VirtualKey::kR;
    case SDL_SCANCODE_S:
      return VirtualKey::kS;
    case SDL_SCANCODE_T:
      return VirtualKey::kT;
    case SDL_SCANCODE_U:
      return VirtualKey::kU;
    case SDL_SCANCODE_V:
      return VirtualKey::kV;
    case SDL_SCANCODE_W:
      return VirtualKey::kW;
    case SDL_SCANCODE_X:
      return VirtualKey::kX;
    case SDL_SCANCODE_Y:
      return VirtualKey::kY;
    case SDL_SCANCODE_Z:
      return VirtualKey::kZ;
    case SDL_SCANCODE_KP_0:
      return VirtualKey::kNumpad0;
    case SDL_SCANCODE_KP_1:
      return VirtualKey::kNumpad1;
    case SDL_SCANCODE_KP_2:
      return VirtualKey::kNumpad2;
    case SDL_SCANCODE_KP_3:
      return VirtualKey::kNumpad3;
    case SDL_SCANCODE_KP_4:
      return VirtualKey::kNumpad4;
    case SDL_SCANCODE_KP_5:
      return VirtualKey::kNumpad5;
    case SDL_SCANCODE_KP_6:
      return VirtualKey::kNumpad6;
    case SDL_SCANCODE_KP_7:
      return VirtualKey::kNumpad7;
    case SDL_SCANCODE_KP_8:
      return VirtualKey::kNumpad8;
    case SDL_SCANCODE_KP_9:
      return VirtualKey::kNumpad9;
    case SDL_SCANCODE_KP_PLUS:
      return VirtualKey::kAdd;
    case SDL_SCANCODE_KP_MINUS:
      return VirtualKey::kSubtract;
    case SDL_SCANCODE_KP_MULTIPLY:
      return VirtualKey::kMultiply;
    case SDL_SCANCODE_KP_DIVIDE:
      return VirtualKey::kDivide;
    case SDL_SCANCODE_KP_DECIMAL:
      return VirtualKey::kDecimal;
    case SDL_SCANCODE_GRAVE:
      return VirtualKey::kOem3;
    case SDL_SCANCODE_MINUS:
      return VirtualKey::kOemMinus;
    case SDL_SCANCODE_EQUALS:
      return VirtualKey::kOemPlus;
    case SDL_SCANCODE_COMMA:
      return VirtualKey::kOemComma;
    case SDL_SCANCODE_PERIOD:
      return VirtualKey::kOemPeriod;
    case SDL_SCANCODE_SEMICOLON:
      return VirtualKey::kOem1;
    case SDL_SCANCODE_SLASH:
      return VirtualKey::kOem2;
    case SDL_SCANCODE_BACKSLASH:
      return VirtualKey::kOem5;
    case SDL_SCANCODE_LEFTBRACKET:
      return VirtualKey::kOem4;
    case SDL_SCANCODE_RIGHTBRACKET:
      return VirtualKey::kOem6;
    case SDL_SCANCODE_APOSTROPHE:
      return VirtualKey::kOem7;
    default:
      return VirtualKey::kNone;
  }
}

rex::ui::MouseEvent::Button TranslateMouseButton(uint8_t button) {
  switch (button) {
    case SDL_BUTTON_LEFT:
      return rex::ui::MouseEvent::Button::kLeft;
    case SDL_BUTTON_RIGHT:
      return rex::ui::MouseEvent::Button::kRight;
    case SDL_BUTTON_MIDDLE:
      return rex::ui::MouseEvent::Button::kMiddle;
    case SDL_BUTTON_X1:
      return rex::ui::MouseEvent::Button::kX1;
    case SDL_BUTTON_X2:
      return rex::ui::MouseEvent::Button::kX2;
    default:
      return rex::ui::MouseEvent::Button::kNone;
  }
}

}  // namespace

namespace rex {
namespace ui {

std::unique_ptr<Window> Window::Create(WindowedAppContext& app_context,
                                       const std::string_view title, uint32_t desired_logical_width,
                                       uint32_t desired_logical_height) {
  desired_logical_width = ResolveWindowWidth(desired_logical_width);
  desired_logical_height = ResolveWindowHeight(desired_logical_height);
  return std::make_unique<SDLWindow>(app_context, title, desired_logical_width,
                                     desired_logical_height);
}

SDLWindow::SDLWindow(WindowedAppContext& app_context, const std::string_view title,
                     uint32_t desired_logical_width, uint32_t desired_logical_height)
    : Window(app_context, title, desired_logical_width, desired_logical_height) {}

SDLWindow::~SDLWindow() {
  EnterDestructor();
  RemoveCursorAutoHideTimer();
  if (window_) {
    WindowMap().erase(SDL_GetWindowID(window_));
  }
  if (metal_view_) {
    SDL_Metal_DestroyView(metal_view_);
    metal_view_ = nullptr;
    metal_layer_ = nullptr;
  }
  if (window_) {
    SDL_DestroyWindow(window_);
    window_ = nullptr;
  }
}

void SDLWindow::HandleSDLEvent(const SDL_Event& event) {
  SDL_WindowID window_id = 0;
  switch (event.type) {
    case SDL_EVENT_WINDOW_SHOWN:
    case SDL_EVENT_WINDOW_HIDDEN:
    case SDL_EVENT_WINDOW_EXPOSED:
    case SDL_EVENT_WINDOW_MOVED:
    case SDL_EVENT_WINDOW_RESIZED:
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
    case SDL_EVENT_WINDOW_METAL_VIEW_RESIZED:
    case SDL_EVENT_WINDOW_FOCUS_GAINED:
    case SDL_EVENT_WINDOW_FOCUS_LOST:
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
    case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED:
    case SDL_EVENT_WINDOW_ENTER_FULLSCREEN:
    case SDL_EVENT_WINDOW_LEAVE_FULLSCREEN:
      window_id = event.window.windowID;
      break;
    case kCursorAutoHideEvent:
      window_id = SDL_WindowID(reinterpret_cast<uintptr_t>(event.user.data1));
      break;
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
      window_id = event.key.windowID;
      break;
    case SDL_EVENT_TEXT_INPUT:
      window_id = event.text.windowID;
      break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
      window_id = event.button.windowID;
      break;
    case SDL_EVENT_MOUSE_MOTION:
      window_id = event.motion.windowID;
      break;
    case SDL_EVENT_MOUSE_WHEEL:
      window_id = event.wheel.windowID;
      break;
    default:
      return;
  }

  auto it = WindowMap().find(window_id);
  if (it != WindowMap().end()) {
    it->second->HandleEvent(event);
  }
}

uint32_t SDLWindow::GetLatestDpiImpl() const { return dpi_ ? dpi_ : kDefaultDpi; }

float SDLWindow::QueryDisplayRefreshHzImpl() const {
  if (!window_) {
    return 0.0f;
  }
  const SDL_DisplayID display = SDL_GetDisplayForWindow(window_);
  if (!display) {
    return 0.0f;
  }
  const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(display);
  return mode != nullptr && mode->refresh_rate > 0.0f ? mode->refresh_rate : 0.0f;
}

bool SDLWindow::OpenImpl() {
  SDL_SetHint(SDL_HINT_MOUSE_FOCUS_CLICKTHROUGH, "1");
  SDL_WindowFlags flags = SDL_WINDOW_RESIZABLE;
  if (REXCVAR_GET(sdl_high_pixel_density)) {
    flags |= SDL_WINDOW_HIGH_PIXEL_DENSITY;
  }
  if (IsFullscreen()) {
    flags |= SDL_WINDOW_FULLSCREEN;
  }

  window_ = SDL_CreateWindow(GetTitle().c_str(), int(GetDesiredLogicalWidth()),
                             int(GetDesiredLogicalHeight()), flags);
  if (!window_) {
    REXLOG_ERROR("SDLWindow: Failed to create window: {}", SDL_GetError());
    return false;
  }

  WindowMap().emplace(SDL_GetWindowID(window_), this);

#if !REX_PLATFORM_ANDROID
  metal_view_ = SDL_Metal_CreateView(window_);
  if (!metal_view_) {
    REXLOG_ERROR("SDLWindow: Failed to create Metal view: {}", SDL_GetError());
    WindowMap().erase(SDL_GetWindowID(window_));
    SDL_DestroyWindow(window_);
    window_ = nullptr;
    return false;
  }
  metal_layer_ = SDL_Metal_GetLayer(metal_view_);
  if (!metal_layer_) {
    REXLOG_ERROR("SDLWindow: Failed to get Metal layer: {}", SDL_GetError());
    SDL_Metal_DestroyView(metal_view_);
    metal_view_ = nullptr;
    WindowMap().erase(SDL_GetWindowID(window_));
    SDL_DestroyWindow(window_);
    window_ = nullptr;
    return false;
  }
#endif

  dpi_ = QueryDpi();
  SDL_ShowWindow(window_);
  if (IsMouseCaptureRequested()) {
    SDL_SetWindowMouseGrab(window_, true);
  }
  ApplyNewCursorVisibility(CursorVisibility::kVisible);

  WindowDestructionReceiver destruction_receiver(this);
  HandleSizeUpdate(destruction_receiver);
  if (destruction_receiver.IsWindowDestroyedOrClosed()) {
    return true;
  }
  OnFocusUpdate((SDL_GetWindowFlags(window_) & SDL_WINDOW_INPUT_FOCUS) != 0, destruction_receiver);
  return true;
}

void SDLWindow::RequestCloseImpl() {
  WindowDestructionReceiver destruction_receiver(this);
  OnBeforeClose(destruction_receiver);
  if (!destruction_receiver.IsWindowDestroyed()) {
    RemoveCursorAutoHideTimer();
    if (metal_view_) {
      SDL_Metal_DestroyView(metal_view_);
      metal_view_ = nullptr;
      metal_layer_ = nullptr;
    }
    if (window_) {
      WindowMap().erase(SDL_GetWindowID(window_));
      SDL_DestroyWindow(window_);
      window_ = nullptr;
    }
    OnAfterClose();
  }
}

void SDLWindow::ApplyNewFullscreen() {
  if (window_) {
    SDL_SetWindowFullscreen(window_, IsFullscreen());
  }
}

void SDLWindow::ApplyNewTitle() {
  if (window_) {
    SDL_SetWindowTitle(window_, GetTitle().c_str());
  }
}

void SDLWindow::ApplyNewMouseCapture() {
  if (window_) {
    SDL_SetWindowMouseGrab(window_, true);
  }
}

void SDLWindow::ApplyNewMouseRelease() {
  if (window_) {
    SDL_SetWindowMouseGrab(window_, false);
  }
}

void SDLWindow::ApplyNewCursorVisibility(CursorVisibility old_cursor_visibility) {
  (void)old_cursor_visibility;
  RemoveCursorAutoHideTimer();
  switch (GetCursorVisibility()) {
    case CursorVisibility::kVisible:
      cursor_currently_auto_hidden_ = false;
      SDL_ShowCursor();
      break;
    case CursorVisibility::kAutoHidden:
      cursor_currently_auto_hidden_ = true;
      SDL_HideCursor();
      break;
    case CursorVisibility::kHidden:
      cursor_currently_auto_hidden_ = true;
      SDL_HideCursor();
      break;
  }
}

void SDLWindow::FocusImpl() {
  if (window_) {
    SDL_RaiseWindow(window_);
  }
}

std::unique_ptr<Surface> SDLWindow::CreateSurfaceImpl(Surface::TypeFlags allowed_types) {
#if REX_PLATFORM_ANDROID
  if (!(allowed_types & Surface::kTypeFlag_AndroidNativeWindow)) return nullptr;
  auto* native_window = static_cast<ANativeWindow*>(SDL_GetPointerProperty(
      SDL_GetWindowProperties(window_), SDL_PROP_WINDOW_ANDROID_WINDOW_POINTER, nullptr));
  return native_window ? std::make_unique<AndroidNativeWindowSurface>(native_window) : nullptr;
#else
  if (!(allowed_types & Surface::kTypeFlag_SDLMetalView)) {
    return nullptr;
  }
  return std::make_unique<SDLMetalViewSurface>(window_, metal_view_, metal_layer_);
#endif
}

void SDLWindow::RequestPaintImpl() {
  bool expected = false;
  if (!paint_event_queued_.compare_exchange_strong(expected, true, std::memory_order_acq_rel,
                                                   std::memory_order_acquire)) {
    return;
  }

  SDL_Event event = {};
  event.type = SDL_EVENT_WINDOW_EXPOSED;
  event.window.windowID = SDL_GetWindowID(window_);
  if (!SDL_PushEvent(&event)) {
    paint_event_queued_.store(false, std::memory_order_release);
  }
}

void SDLWindow::HandleEvent(const SDL_Event& event) {
  WindowDestructionReceiver destruction_receiver(this);
  switch (event.type) {
    case SDL_EVENT_WINDOW_EXPOSED:
      paint_event_queued_.store(false, std::memory_order_release);
      OnPaint();
      break;
    case SDL_EVENT_WINDOW_RESIZED:
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
    case SDL_EVENT_WINDOW_METAL_VIEW_RESIZED:
    case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED:
#if REX_PLATFORM_ANDROID
      // A resize accompanies every new ANativeWindow; rebuild the presenter surface
      // if the foreground event arrived before the surface existed.
      if (presenter() && !HasPresenterSurface()) {
        NotifySurfaceChanged(true);
      }
#endif
      HandleSizeUpdate(destruction_receiver);
      break;
    case SDL_EVENT_WINDOW_FOCUS_GAINED:
      OnFocusUpdate(true, destruction_receiver);
      break;
    case SDL_EVENT_WINDOW_FOCUS_LOST:
      OnFocusUpdate(false, destruction_receiver);
      break;
    case SDL_EVENT_WINDOW_ENTER_FULLSCREEN:
      OnDesiredFullscreenUpdate(true);
      break;
    case SDL_EVENT_WINDOW_LEAVE_FULLSCREEN:
      OnDesiredFullscreenUpdate(false);
      break;
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
#if REX_PLATFORM_MAC
      app_context().QuitFromUIThread();
#else
      RequestCloseImpl();
      app_context().RequestDeferredQuit();
#endif
      break;
    case kCursorAutoHideEvent:
      if (GetCursorVisibility() == CursorVisibility::kAutoHidden &&
          SDL_TimerID(reinterpret_cast<uintptr_t>(event.user.data2)) == cursor_auto_hide_timer_) {
        cursor_auto_hide_timer_ = 0;
        cursor_currently_auto_hidden_ = true;
        SDL_HideCursor();
      }
      break;
    case SDL_EVENT_KEY_DOWN:
      HandleKey(event.key, true, destruction_receiver);
      break;
    case SDL_EVENT_KEY_UP:
      HandleKey(event.key, false, destruction_receiver);
      break;
    case SDL_EVENT_TEXT_INPUT:
      HandleTextInput(event.text, destruction_receiver);
      break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
      HandleMouseButton(event.button, true, destruction_receiver);
      break;
    case SDL_EVENT_MOUSE_BUTTON_UP:
      HandleMouseButton(event.button, false, destruction_receiver);
      break;
    case SDL_EVENT_MOUSE_MOTION:
      HandleMouseMotion(event.motion, destruction_receiver);
      break;
    case SDL_EVENT_MOUSE_WHEEL:
      HandleMouseWheel(event.wheel, destruction_receiver);
      break;
    default:
      break;
  }
}

void SDLWindow::HandleSizeUpdate(WindowDestructionReceiver& destruction_receiver) {
  dpi_ = QueryDpi();
  int width = 0;
  int height = 0;
  if (!SDL_GetWindowSizeInPixels(window_, &width, &height)) {
    return;
  }
  if (!IsFullscreen()) {
    int logical_width = 0;
    int logical_height = 0;
    if (SDL_GetWindowSize(window_, &logical_width, &logical_height)) {
      OnDesiredLogicalSizeUpdate(uint32_t(std::max(logical_width, 0)),
                                 uint32_t(std::max(logical_height, 0)));
    }
  }
  OnActualSizeUpdate(uint32_t(std::max(width, 0)), uint32_t(std::max(height, 0)),
                     destruction_receiver);
}

void SDLWindow::HandleKey(const SDL_KeyboardEvent& event, bool down,
                          WindowDestructionReceiver& destruction_receiver) {
  SDL_Keymod mod = SDL_GetModState();
  KeyEvent e(this, TranslateSDLScancode(event.scancode), event.repeat ? 2 : 1, !down,
             (mod & SDL_KMOD_SHIFT) != 0, (mod & SDL_KMOD_CTRL) != 0,
             (mod & SDL_KMOD_ALT) != 0, (mod & SDL_KMOD_GUI) != 0);
  if (down) {
    OnKeyDown(e, destruction_receiver);
  } else {
    OnKeyUp(e, destruction_receiver);
  }
}

void SDLWindow::HandleMouseButton(const SDL_MouseButtonEvent& event, bool down,
                                  WindowDestructionReceiver& destruction_receiver) {
  RevealAutoHiddenCursor();
  int32_t physical_x = 0;
  int32_t physical_y = 0;
  WindowPointToPhysical(event.x, event.y, physical_x, physical_y);
  MouseEvent e(this, TranslateMouseButton(event.button), physical_x, physical_y);
  if (down) {
    OnMouseDown(e, destruction_receiver);
  } else {
    OnMouseUp(e, destruction_receiver);
  }
}

void SDLWindow::HandleMouseMotion(const SDL_MouseMotionEvent& event,
                                  WindowDestructionReceiver& destruction_receiver) {
  RevealAutoHiddenCursor();
  int32_t physical_x = 0;
  int32_t physical_y = 0;
  WindowPointToPhysical(event.x, event.y, physical_x, physical_y);
  MouseEvent e(this, MouseEvent::Button::kNone, physical_x, physical_y);
  OnMouseMove(e, destruction_receiver);
}

void SDLWindow::HandleTextInput(const SDL_TextInputEvent& event,
                                WindowDestructionReceiver& destruction_receiver) {
  if (!event.text) {
    return;
  }
  SDL_Keymod mod = SDL_GetModState();
  const uint8_t* s = reinterpret_cast<const uint8_t*>(event.text);
  while (*s) {
    // Decode one UTF-8 codepoint.
    uint32_t codepoint = 0;
    uint32_t continuation_count = 0;
    uint8_t lead = *s++;
    if (lead < 0x80) {
      codepoint = lead;
    } else if ((lead & 0xE0) == 0xC0) {
      codepoint = lead & 0x1F;
      continuation_count = 1;
    } else if ((lead & 0xF0) == 0xE0) {
      codepoint = lead & 0x0F;
      continuation_count = 2;
    } else if ((lead & 0xF8) == 0xF0) {
      codepoint = lead & 0x07;
      continuation_count = 3;
    } else {
      // Stray continuation or invalid lead byte - skip.
      continue;
    }
    bool valid = true;
    for (uint32_t i = 0; i < continuation_count; ++i) {
      uint8_t continuation = *s;
      if ((continuation & 0xC0) != 0x80) {
        valid = false;
        break;
      }
      codepoint = (codepoint << 6) | (continuation & 0x3F);
      ++s;
    }
    if (!valid || !codepoint) {
      continue;
    }
    // Mirrors the Win32 WM_CHAR path - the character is carried in the
    // KeyEvent's virtual key field.
    KeyEvent e(this, VirtualKey(codepoint), 1, false, (mod & SDL_KMOD_SHIFT) != 0,
               (mod & SDL_KMOD_CTRL) != 0, (mod & SDL_KMOD_ALT) != 0, (mod & SDL_KMOD_GUI) != 0);
    OnKeyChar(e, destruction_receiver);
    if (destruction_receiver.IsWindowDestroyedOrClosed()) {
      return;
    }
  }
}

void SDLWindow::SetTextInputActive(bool active) {
  if (!window_) {
    return;
  }
  if (active) {
    SDL_StartTextInput(window_);
  } else {
    SDL_StopTextInput(window_);
  }
}

void SDLWindow::HandleMouseWheel(const SDL_MouseWheelEvent& event,
                                 WindowDestructionReceiver& destruction_receiver) {
  RevealAutoHiddenCursor();
  int32_t physical_x = 0;
  int32_t physical_y = 0;
  WindowPointToPhysical(event.mouse_x, event.mouse_y, physical_x, physical_y);
  MouseEvent e(this, MouseEvent::Button::kNone, physical_x, physical_y,
               int32_t(std::lround(event.x * double(MouseEvent::kScrollPerDetent))),
               int32_t(std::lround(event.y * double(MouseEvent::kScrollPerDetent))));
  OnMouseWheel(e, destruction_receiver);
}

void SDLWindow::RevealAutoHiddenCursor() {
  if (GetCursorVisibility() != CursorVisibility::kAutoHidden) {
    return;
  }
  if (cursor_currently_auto_hidden_) {
    cursor_currently_auto_hidden_ = false;
    SDL_ShowCursor();
  }
  SetCursorAutoHideTimer();
}

void SDLWindow::SetCursorAutoHideTimer() {
  RemoveCursorAutoHideTimer();
  if (!window_) {
    return;
  }
  cursor_auto_hide_timer_ =
      SDL_AddTimer(kDefaultCursorAutoHideMilliseconds, AutoHideCursorTimerCallback,
                   reinterpret_cast<void*>(uintptr_t(SDL_GetWindowID(window_))));
}

void SDLWindow::RemoveCursorAutoHideTimer() {
  if (!cursor_auto_hide_timer_) {
    return;
  }
  SDL_RemoveTimer(cursor_auto_hide_timer_);
  cursor_auto_hide_timer_ = 0;
}

void SDLWindow::WindowPointToPhysical(float x, float y, int32_t& physical_x,
                                      int32_t& physical_y) const {
  int logical_width = 0;
  int logical_height = 0;
  int physical_width = 0;
  int physical_height = 0;
  if (window_ && SDL_GetWindowSize(window_, &logical_width, &logical_height) &&
      SDL_GetWindowSizeInPixels(window_, &physical_width, &physical_height) &&
      logical_width > 0 && logical_height > 0 && physical_width > 0 && physical_height > 0) {
    physical_x = int32_t(std::lround(double(x) * double(physical_width) / double(logical_width)));
    physical_y = int32_t(std::lround(double(y) * double(physical_height) / double(logical_height)));
    return;
  }

  physical_x = int32_t(std::lround(x));
  physical_y = int32_t(std::lround(y));
}

uint32_t SDLWindow::QueryDpi() const {
  if (!window_) {
    return kDefaultDpi;
  }
  float scale = SDL_GetWindowDisplayScale(window_);
  if (scale <= 0.0f) {
    scale = 1.0f;
  }
  return std::clamp(uint32_t(std::lround(double(kDefaultDpi) * scale)), uint32_t(48),
                    uint32_t(384));
}

std::unique_ptr<ui::MenuItem> MenuItem::Create(Type type, const std::string& text,
                                               const std::string& hotkey,
                                               std::function<void()> callback) {
  return std::make_unique<SDLMenuItem>(type, text, hotkey, std::move(callback));
}

}  // namespace ui
}  // namespace rex
