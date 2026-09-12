/**
 * @file        ui/overlay/debug_overlay.cpp
 *
 * @brief       Debug overlay implementation. See debug_overlay.h for details.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#include <rex/ui/overlay/debug_overlay.h>
#include <rex/version.h>
#include <imgui.h>
#ifdef REXGLUE_ENABLE_PERF_COUNTERS
#include <rex/perf/counter.h>
#include <cinttypes>
#endif

namespace rex::ui {

#ifdef REXGLUE_ENABLE_PERF_COUNTERS
namespace {

int64_t Counter(rex::perf::CounterId id) {
  return rex::perf::GetSnapshotCounter(id);
}

void DrawBucketRow(const char* label, rex::perf::CounterId calls, rex::perf::CounterId vertices,
                   rex::perf::CounterId primitives) {
  ImGui::Text("%-6s %6" PRId64 " %9" PRId64 " %9" PRId64, label, Counter(calls),
              Counter(vertices), Counter(primitives));
}

float CounterMs(rex::perf::CounterId id) {
  return static_cast<float>(Counter(id)) / 1000.0f;
}

}  // namespace
#endif

DebugOverlayDialog::DebugOverlayDialog(ImGuiDrawer* imgui_drawer, FrameStatsProvider stats_provider,
                                       std::string_view build_stamp)
    : ImGuiDialog(imgui_drawer),
      stats_provider_(std::move(stats_provider)),
      build_stamp_(build_stamp.empty() ? REXGLUE_BUILD_STAMP : std::string(build_stamp)) {}

DebugOverlayDialog::~DebugOverlayDialog() {}

void DebugOverlayDialog::OnDraw(ImGuiIO& io) {
  ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_FirstUseEver);
#ifdef REXGLUE_ENABLE_PERF_COUNTERS
  ImGui::SetNextWindowSize(ImVec2(620, 760), ImGuiCond_FirstUseEver);
#else
  ImGui::SetNextWindowSize(ImVec2(360, 80), ImGuiCond_FirstUseEver);
#endif
  ImGui::SetNextWindowBgAlpha(0.5f);
  ImGui::PushFont(nullptr, 17.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.0f, 8.0f));
  if (ImGui::Begin("Debug##overlay", nullptr, ImGuiWindowFlags_NoCollapse)) {
    if (stats_provider_) {
      auto stats = stats_provider_();
      if (stats.frame_count > 0) {
        ImGui::Text("Guest: %.1f FPS (%.2f ms)", stats.fps, stats.frame_time_ms);
      }
    }
#ifdef REXGLUE_ENABLE_PERF_COUNTERS
    ImGui::Separator();

    // Frame time graph
    auto ft_us = Counter(rex::perf::CounterId::kFrameTimeUs);
    float ft_ms = static_cast<float>(ft_us) / 1000.0f;
    frame_time_history_[frame_history_idx_] = ft_ms;
    frame_history_idx_ = (frame_history_idx_ + 1) % kFrameHistorySize;
    ImGui::PlotLines("##ft", frame_time_history_.data(), kFrameHistorySize,
                     static_cast<int>(frame_history_idx_), "Frame (ms)", 0.0f, 50.0f,
                     ImVec2(340, 48));

    // GPU
    ImGui::Text("PreDraw: %" PRId64 "  Submit: %" PRId64 "  Stalls: %" PRId64,
                Counter(rex::perf::CounterId::kGuestDrawPackets),
                Counter(rex::perf::CounterId::kDrawCalls),
                Counter(rex::perf::CounterId::kCommandBufferStalls));
    ImGui::Text("Verts: %" PRId64 "  Prims: %" PRId64,
                Counter(rex::perf::CounterId::kVerticesProcessed),
                Counter(rex::perf::CounterId::kPrimitivesProcessed));

    ImGui::SeparatorText("Draw Buckets");
    ImGui::TextUnformatted("Bucket  Draws     Verts     Prims");
    DrawBucketRow("Main", rex::perf::CounterId::kDrawMainCalls,
                  rex::perf::CounterId::kDrawMainVertices,
                  rex::perf::CounterId::kDrawMainPrimitives);
    DrawBucketRow("Depth", rex::perf::CounterId::kDrawDepthCalls,
                  rex::perf::CounterId::kDrawDepthVertices,
                  rex::perf::CounterId::kDrawDepthPrimitives);
    DrawBucketRow("Copy", rex::perf::CounterId::kDrawCopyCalls,
                  rex::perf::CounterId::kDrawCopyVertices,
                  rex::perf::CounterId::kDrawCopyPrimitives);
    DrawBucketRow("MemExp", rex::perf::CounterId::kDrawMemexportCalls,
                  rex::perf::CounterId::kDrawMemexportVertices,
                  rex::perf::CounterId::kDrawMemexportPrimitives);
    DrawBucketRow("NoPS", rex::perf::CounterId::kDrawNoPixelShaderCalls,
                  rex::perf::CounterId::kDrawNoPixelShaderVertices,
                  rex::perf::CounterId::kDrawNoPixelShaderPrimitives);

    ImGui::SeparatorText("D3D12 Draw CPU ms");
    ImGui::Text("Total: %.3f  Prim: %.3f  RT: %.3f",
                CounterMs(rex::perf::CounterId::kDrawStageTotalUs),
                CounterMs(rex::perf::CounterId::kDrawStagePrimitiveUs),
                CounterMs(rex::perf::CounterId::kDrawStageRenderTargetUs));
    ImGui::Text("Pipe: %.3f  Tex: %.3f  Fixed: %.3f",
                CounterMs(rex::perf::CounterId::kDrawStagePipelineUs),
                CounterMs(rex::perf::CounterId::kDrawStageTextureUs),
                CounterMs(rex::perf::CounterId::kDrawStageFixedFunctionUs));
    ImGui::Text("Bind: %.3f  VB: %.3f  Bar: %.3f  Sub: %.3f",
                CounterMs(rex::perf::CounterId::kDrawStageBindingsUs),
                CounterMs(rex::perf::CounterId::kDrawStageVertexBuffersUs),
                CounterMs(rex::perf::CounterId::kDrawStageBarriersUs),
                CounterMs(rex::perf::CounterId::kDrawStageSubmitUs));

    ImGui::SeparatorText("D3D12 GPU ms");
    ImGui::Text("Main: %.3f  Depth: %.3f  NoPS: %.3f",
                CounterMs(rex::perf::CounterId::kGpuMainUs),
                CounterMs(rex::perf::CounterId::kGpuDepthUs),
                CounterMs(rex::perf::CounterId::kGpuNoPixelShaderUs));
    ImGui::Text("Copy: %.3f  MemExp: %.3f  Timed: %" PRId64,
                CounterMs(rex::perf::CounterId::kGpuCopyUs),
                CounterMs(rex::perf::CounterId::kGpuMemexportUs),
                Counter(rex::perf::CounterId::kGpuTimestampedDraws));
    ImGui::Text("Frame: %.3f  Frames: %" PRId64,
                CounterMs(rex::perf::CounterId::kGpuCommandProcessorFrameUs),
                Counter(rex::perf::CounterId::kGpuTimestampedFrames));

    ImGui::SeparatorText("CPU Phase ms");
    ImGui::Text("PM4 Primary: %.3f  Indirect: %.3f",
                CounterMs(rex::perf::CounterId::kCpuPrimaryBufferUs),
                CounterMs(rex::perf::CounterId::kCpuIndirectBufferUs));
    ImGui::Text("BeginSub: %.3f  EndSub: %.3f  Deferred: %.3f  Exec: %.3f",
                CounterMs(rex::perf::CounterId::kCpuD3D12BeginSubmissionUs),
                CounterMs(rex::perf::CounterId::kCpuD3D12EndSubmissionUs),
                CounterMs(rex::perf::CounterId::kCpuD3D12DeferredExecuteUs),
                CounterMs(rex::perf::CounterId::kCpuD3D12ExecuteCommandListsUs));
    ImGui::Text("Paint: %.3f  Consume: %.3f  Record: %.3f  UI: %.3f",
                CounterMs(rex::perf::CounterId::kCpuD3D12PaintTotalUs),
                CounterMs(rex::perf::CounterId::kCpuD3D12PaintConsumeUs),
                CounterMs(rex::perf::CounterId::kCpuD3D12PaintRecordUs),
                CounterMs(rex::perf::CounterId::kCpuD3D12PaintUiUs));
    ImGui::Text("PresentWait: %.3f  Present: %.3f",
                CounterMs(rex::perf::CounterId::kCpuD3D12PresentWaitUs),
                CounterMs(rex::perf::CounterId::kCpuD3D12PresentUs));

    ImGui::SeparatorText("Texture Streaming");
    ImGui::Text("Req: %" PRId64 "  Fetches: %" PRId64 "  Changed: %" PRId64 "  CPU: %.3f",
                Counter(rex::perf::CounterId::kTextureRequestCalls),
                Counter(rex::perf::CounterId::kTextureFetchesRequested),
                Counter(rex::perf::CounterId::kTextureBindingsChanged),
                CounterMs(rex::perf::CounterId::kTextureRequestUs));
    ImGui::Text("Loads: %" PRId64 "  Ranges: %" PRId64 "  Bytes: %.2f MB",
                Counter(rex::perf::CounterId::kTexturePendingLoads),
                Counter(rex::perf::CounterId::kTexturePendingRanges),
                static_cast<double>(Counter(rex::perf::CounterId::kTexturePendingBytes)) /
                    (1024.0 * 1024.0));
    ImGui::Text("SMem: %.3f  Commit: %.3f  Backend: %.3f  Scaled: %.3f",
                CounterMs(rex::perf::CounterId::kTextureSharedMemoryRequestUs),
                CounterMs(rex::perf::CounterId::kTextureCommitLoadUs),
                CounterMs(rex::perf::CounterId::kTextureLoadBackendUs),
                CounterMs(rex::perf::CounterId::kTextureScaledResolveCommitUs));
    ImGui::Text("Committed: %" PRId64 "  LoadBytes: %.2f MB",
                Counter(rex::perf::CounterId::kTextureLoadsCommitted),
                static_cast<double>(Counter(rex::perf::CounterId::kTextureLoadBytes)) /
                    (1024.0 * 1024.0));

    // Audio
    ImGui::Text("XMA: %" PRId64 "  Lat: %.1fms  BufQ: %" PRId64,
                Counter(rex::perf::CounterId::kXmaFramesDecoded),
                static_cast<float>(Counter(rex::perf::CounterId::kAudioFrameLatencyUs)) / 1000.0f,
                Counter(rex::perf::CounterId::kBufferQueueDepth));

    // Dispatch
    ImGui::Text("Dispatch: %" PRId64 "  IRQ: %" PRId64,
                Counter(rex::perf::CounterId::kFunctionsDispatched),
                Counter(rex::perf::CounterId::kInterruptDispatches));

    // Threading
    ImGui::Text("Threads: %" PRId64 "  APC: %" PRId64 "  Contention: %" PRId64,
                Counter(rex::perf::CounterId::kActiveThreads),
                Counter(rex::perf::CounterId::kApcQueueDepth),
                Counter(rex::perf::CounterId::kCriticalRegionContentions));

    // Caches
    auto tex_h = Counter(rex::perf::CounterId::kTextureCacheHits);
    auto tex_m = Counter(rex::perf::CounterId::kTextureCacheMisses);
    auto pip_h = Counter(rex::perf::CounterId::kPipelineCacheHits);
    auto pip_m = Counter(rex::perf::CounterId::kPipelineCacheMisses);
    ImGui::Text("TexCache: %" PRId64 "/%" PRId64 "  PipeCache: %" PRId64 "/%" PRId64, tex_h,
                tex_h + tex_m, pip_h, pip_h + pip_m);
#else
    ImGui::TextUnformatted("Perf counters are disabled in this build.");
#endif
  }
  ImGui::End();
  ImGui::PopStyleVar();
  ImGui::PopFont();

  // Build stamp watermark -- centered near bottom of screen
  const char* build_stamp = build_stamp_.c_str();
  auto text_size = ImGui::CalcTextSize(build_stamp);
  float padding = ImGui::GetStyle().WindowPadding.x * 2.0f;
  float bottom_offset = io.DisplaySize.y * 0.03f;
  ImGui::SetNextWindowPos(ImVec2((io.DisplaySize.x - text_size.x - padding) * 0.5f,
                                 io.DisplaySize.y - text_size.y - bottom_offset));
  ImGui::SetNextWindowSize(ImVec2(0, 0));
  ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 0.5f));
  if (ImGui::Begin("##watermark", nullptr,
                       ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground |
                       ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                       ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::TextUnformatted(build_stamp);
  }
  ImGui::End();
  ImGui::PopStyleColor();
}

}  // namespace rex::ui
