/**
 * @file        ui/overlay/fps_overlay.cpp
 *
 * @brief       Minimal guest FPS readout overlay. See fps_overlay.h for
 *              details.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#include <rex/ui/overlay/fps_overlay.h>

#include <algorithm>

#include <imgui.h>

namespace rex::ui {

void FpsOverlayDialog::OnDraw(ImGuiIO& io) {
  Presenter::GuestFrameStats stats;
  if (presenter_) {
    stats = presenter_->GetGuestFrameStats();
  }

  constexpr float kMargin = 10.0f;
  ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - kMargin, kMargin), ImGuiCond_Always,
                          ImVec2(1.0f, 0.0f));
  ImGui::SetNextWindowBgAlpha(0.4f);
  ImGui::PushFont(nullptr, 20.0f);
  if (ImGui::Begin("##fps_overlay", nullptr,
                   ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                       ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoSavedSettings |
                       ImGuiWindowFlags_NoFocusOnAppearing |
                       ImGuiWindowFlags_AlwaysAutoResize)) {
    if (stats.frame_count > 0 && stats.fps > 0.0) {
      ImGui::Text("%.0f FPS", stats.fps);
      ImGui::PushFont(nullptr, 14.0f);
      ImGui::Text("%.2f ms", stats.frame_time_ms);
      // Time the GPU emulation thread spent blocked on host GPU fences last
      // frame: near the frame time = GPU-bound, near zero = thread-bound.
      ImGui::Text("wait %.2f ms", stats.wait_ms);
      if (stats.gpu_ms > 0.0) {
        // Device-timeline span of a recent frame (first to last command,
        // including idle gaps between submissions).
        ImGui::Text("gpu %.2f ms", stats.gpu_ms);
        double gpu_known_ms = stats.gpu_draw_ms + stats.gpu_resolve_ms + stats.gpu_dump_ms;
        if (gpu_known_ms > 0.0) {
          // "other" covers everything not in a bucket: render target
          // ownership transfers, barriers, and idle gaps.
          ImGui::Text("draw %.2f  res %.2f", stats.gpu_draw_ms, stats.gpu_resolve_ms);
          ImGui::Text("dump %.2f  other %.2f", stats.gpu_dump_ms,
                      std::max(0.0, stats.gpu_ms - gpu_known_ms));
        }
      }
      ImGui::PopFont();
    } else {
      ImGui::TextUnformatted("-- FPS");
    }
  }
  ImGui::End();
  ImGui::PopFont();
}

}  // namespace rex::ui
