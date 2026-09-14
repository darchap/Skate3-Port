
#pragma once

namespace rex::ui {

class AppLifecycleListener {
 public:
  virtual ~AppLifecycleListener() = default;
  virtual void OnAppEnterBackground() {}
  virtual void OnAppEnterForeground() {}
  virtual void OnAppTerminating() {}
  virtual void OnAppLowMemory() {}
};

}  // namespace rex::ui
