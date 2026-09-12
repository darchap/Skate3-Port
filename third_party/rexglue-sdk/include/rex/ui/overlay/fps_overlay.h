/**
 * @file        rex/ui/overlay/fps_overlay.h
 *
 * @brief       Minimal guest FPS readout overlay.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#pragma once
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/presenter.h>

namespace rex::ui {

// Compact corner readout of the guest frame rate measured by the presenter.
// Unlike the interactive overlays, this one does not request continuous
// repaints, so host presents stay paced by guest output refreshes and
// external FPS measurement tools (e.g. RTSS) remain accurate while it's open.
class FpsOverlayDialog : public ImGuiDialog {
 public:
  FpsOverlayDialog(ImGuiDrawer* imgui_drawer, const Presenter* presenter)
      : ImGuiDialog(imgui_drawer), presenter_(presenter) {}

  bool WantsContinuousRepaint() const override { return false; }

 protected:
  void OnDraw(ImGuiIO& io) override;

 private:
  const Presenter* presenter_;
};

}  // namespace rex::ui
