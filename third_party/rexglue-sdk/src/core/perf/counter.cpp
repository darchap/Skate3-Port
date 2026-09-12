/**
 * @file        core/perf/counter.cpp
 * @brief       Performance counter registry implementation
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#include <rex/perf/counter.h>

#include <rex/cvar.h>
#include <rex/logging.h>

#include <array>
#include <atomic>
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <mutex>
#include <unordered_map>

REXCVAR_DEFINE_STRING(perf_log_csv, "", "Perf",
                      "Path to write per-frame CSV log (empty = disabled)");
REXCVAR_DEFINE_BOOL(perf_diagnostics, false, "Perf",
                    "Keep heavyweight draw diagnostics enabled outside explicit captures")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(perf_draw_fingerprints, false, "Perf",
                    "Collect per-frame draw fingerprints outside explicit captures")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(perf_capture_frames, 120, "Perf",
                     "Number of frames to collect for F8 performance captures")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace rex::perf {

namespace {

constexpr size_t kNumCounters = static_cast<size_t>(CounterId::kCount);

std::array<std::atomic<int64_t>, kNumCounters> g_counters{};
std::array<std::atomic<int64_t>, kNumCounters> g_snapshot{};

struct DrawFingerprintKey {
  DrawFingerprint fingerprint;

  bool operator==(const DrawFingerprintKey& other) const {
    return fingerprint.bucket == other.fingerprint.bucket &&
           fingerprint.vertex_shader_hash == other.fingerprint.vertex_shader_hash &&
           fingerprint.pixel_shader_hash == other.fingerprint.pixel_shader_hash &&
           fingerprint.primitive_type == other.fingerprint.primitive_type &&
           fingerprint.vertex_count == other.fingerprint.vertex_count &&
           fingerprint.primitive_count == other.fingerprint.primitive_count &&
           std::equal(std::begin(fingerprint.vertex_fetch_address),
                      std::end(fingerprint.vertex_fetch_address),
                      std::begin(other.fingerprint.vertex_fetch_address)) &&
           std::equal(std::begin(fingerprint.vertex_fetch_size),
                      std::end(fingerprint.vertex_fetch_size),
                      std::begin(other.fingerprint.vertex_fetch_size));
  }
};

struct DrawFingerprintKeyHash {
  size_t operator()(const DrawFingerprintKey& key) const {
    size_t hash = 1469598103934665603ull;
    auto add = [&hash](uint64_t value) {
      for (size_t i = 0; i < sizeof(value); ++i) {
        hash ^= uint8_t(value >> (i * 8));
        hash *= 1099511628211ull;
      }
    };
    add(uint64_t(key.fingerprint.bucket));
    add(key.fingerprint.vertex_shader_hash);
    add(key.fingerprint.pixel_shader_hash);
    add(key.fingerprint.primitive_type);
    add(key.fingerprint.vertex_count);
    add(key.fingerprint.primitive_count);
    for (size_t i = 0; i < DrawFingerprint::kVertexFetchCount; ++i) {
      add(key.fingerprint.vertex_fetch_address[i]);
      add(key.fingerprint.vertex_fetch_size[i]);
    }
    return hash;
  }
};

std::mutex g_draw_fingerprints_mutex;
std::unordered_map<DrawFingerprintKey, DrawFingerprintEntry, DrawFingerprintKeyHash>
    g_draw_fingerprints_live;
std::vector<DrawFingerprintEntry> g_draw_fingerprints_snapshot;

struct CaptureState {
  bool active = false;
  uint32_t record_frames_remaining = 0;
  uint32_t drain_frames_remaining = 0;
  uint32_t captured_frames = 0;
  std::filesystem::path counters_path;
  std::filesystem::path fingerprints_path;
  std::array<int64_t, kNumCounters> counter_totals = {};
  std::unordered_map<DrawFingerprintKey, DrawFingerprintEntry, DrawFingerprintKeyHash>
      draw_fingerprints;
};

std::mutex g_capture_mutex;
CaptureState g_capture;
std::atomic<bool> g_capture_recording = false;
constexpr uint32_t kCaptureDrainFrames = 8;

bool IsDelayedGpuTimingCounter(size_t counter_index) {
  auto id = static_cast<CounterId>(counter_index);
  return id == CounterId::kGpuMainUs || id == CounterId::kGpuDepthUs ||
         id == CounterId::kGpuCopyUs || id == CounterId::kGpuMemexportUs ||
         id == CounterId::kGpuNoPixelShaderUs ||
         id == CounterId::kGpuCopyDumpUs ||
         id == CounterId::kGpuCopyResolveUs ||
         id == CounterId::kGpuCopyResolveFast32Us ||
         id == CounterId::kGpuCopyResolveFull32Us ||
         id == CounterId::kGpuResolveDownscaleUs ||
         id == CounterId::kGpuTimestampedDraws ||
         id == CounterId::kGpuCommandProcessorFrameUs ||
         id == CounterId::kGpuTimestampedFrames ||
         id == CounterId::kGpuBarrierUs || id == CounterId::kGpuClearUs ||
         id == CounterId::kGpuCopyBufferUs || id == CounterId::kGpuCopyTextureUs ||
         id == CounterId::kGpuCopyResourceUs || id == CounterId::kGpuDispatchUs ||
         id == CounterId::kGpuResolveQueryUs;
}

constexpr const char* kCounterNames[] = {
    "frame_time_us",
    "fps",
    "guest_draw_packets",
    "draw_calls",
    "command_buffer_stalls",
    "vertices_processed",
    "primitives_processed",
    "draw_main_calls",
    "draw_main_vertices",
    "draw_main_primitives",
    "draw_depth_calls",
    "draw_depth_vertices",
    "draw_depth_primitives",
    "draw_copy_calls",
    "draw_copy_vertices",
    "draw_copy_primitives",
    "draw_memexport_calls",
    "draw_memexport_vertices",
    "draw_memexport_primitives",
    "draw_no_pixel_shader_calls",
    "draw_no_pixel_shader_vertices",
    "draw_no_pixel_shader_primitives",
    "draw_stage_total_us",
    "draw_stage_primitive_us",
    "draw_stage_render_target_us",
    "draw_stage_pipeline_us",
    "draw_stage_texture_us",
    "draw_stage_fixed_function_us",
    "draw_stage_bindings_us",
    "draw_stage_vertex_buffers_us",
    "draw_stage_barriers_us",
    "draw_stage_submit_us",
    "draw_stage_system_constants_us",
    "draw_sampler_fast_path_draws",
    "draw_texture_fast_path_draws",
    "draw_stage_analyze_us",
    "draw_stage_translate_us",
    "draw_stage_samplers_us",
    "draw_stage_pre_draw_us",
    "gpu_main_us",
    "gpu_depth_us",
    "gpu_copy_us",
    "gpu_memexport_us",
    "gpu_no_pixel_shader_us",
    "gpu_copy_dump_us",
    "gpu_copy_resolve_us",
    "gpu_copy_resolve_fast32_us",
    "gpu_copy_resolve_full32_us",
    "gpu_resolve_downscale_us",
    "gpu_timestamped_draws",
    "gpu_command_processor_frame_us",
    "gpu_timestamped_frames",
    "gpu_barrier_us",
    "gpu_clear_us",
    "gpu_copy_buffer_us",
    "gpu_copy_texture_us",
    "gpu_copy_resource_us",
    "gpu_dispatch_us",
    "gpu_resolve_query_us",
    "cpu_primary_buffer_us",
    "cpu_indirect_buffer_us",
    "cpu_guest_wait_us",
    "cpu_swap_us",
    "cpu_d3d12_begin_submission_us",
    "cpu_d3d12_begin_submission_fence_us",
    "cpu_d3d12_begin_submission_frame_open_us",
    "cpu_d3d12_begin_submission_open_us",
    "cpu_d3d12_process_gpu_timestamps_us",
    "cpu_d3d12_end_submission_us",
    "cpu_d3d12_check_fence_us",
    "cpu_d3d12_fence_wait_us",
    "cpu_d3d12_fence_reclaim_us",
    "cpu_d3d12_end_frame_us",
    "cpu_d3d12_pipeline_end_us",
    "cpu_d3d12_submit_barriers_us",
    "cpu_d3d12_deferred_execute_us",
    "cpu_d3d12_command_list_close_us",
    "cpu_d3d12_execute_command_lists_us",
    "cpu_d3d12_signal_us",
    "cpu_d3d12_paint_total_us",
    "cpu_d3d12_paint_consume_us",
    "cpu_d3d12_paint_record_us",
    "cpu_d3d12_paint_ui_us",
    "cpu_d3d12_present_wait_us",
    "cpu_d3d12_present_us",
    "gpu_thread_fence_wait_us",
    "texture_request_us",
    "texture_request_calls",
    "texture_fetches_requested",
    "texture_bindings_changed",
    "texture_pending_loads",
    "texture_pending_ranges",
    "texture_pending_bytes",
    "texture_shared_memory_request_us",
    "texture_commit_load_us",
    "texture_loads_committed",
    "texture_load_bytes",
    "texture_load_backend_us",
    "texture_scaled_resolve_commit_us",
    "d3d12_barriers_queued",
    "d3d12_barriers_submitted",
    "d3d12_transition_barriers",
    "d3d12_aliasing_barriers",
    "d3d12_uav_barriers",
    "d3d12_rt_transitions",
    "d3d12_depth_transitions",
    "d3d12_copy_transitions",
    "d3d12_srv_transitions",
    "d3d12_uav_transitions",
    "d3d12_present_transitions",
    "deferred_commands",
    "deferred_clear_count",
    "deferred_clear_us",
    "deferred_barrier_count",
    "deferred_barrier_us",
    "deferred_copy_buffer_count",
    "deferred_copy_buffer_bytes",
    "deferred_copy_buffer_us",
    "deferred_copy_texture_count",
    "deferred_copy_texture_us",
    "deferred_copy_resource_count",
    "deferred_copy_resource_us",
    "deferred_dispatch_count",
    "deferred_dispatch_us",
    "deferred_draw_count",
    "deferred_draw_us",
    "deferred_query_count",
    "deferred_query_us",
    "deferred_resolve_query_count",
    "deferred_resolve_query_us",
    "deferred_state_count",
    "deferred_state_us",
    "rt_resolve_calls",
    "rt_resolve_us",
    "rt_resolve_direct_calls",
    "rt_resolve_direct_us",
    "rt_resolve_transfer_clear_us",
    "rt_resolve_readback_calls",
    "rt_resolve_readback_us",
    "rt_dump_calls",
    "rt_dump_rectangles",
    "rt_dump_dispatches",
    "rt_dump_groups",
    "rt_resolve_copy_dispatches",
    "rt_resolve_copy_groups",
    "rt_resolve_scaled_bytes",
    "rt_scaled_resolve_write_barriers",
    "rt_scaled_resolve_write_barrier_overlaps",
    "rt_scaled_resolve_write_barrier_skipped",
    "rt_scaled_resolve_write_barrier_bytes",
    "rt_resolve_copy_fast32_1x2x_dispatches",
    "rt_resolve_copy_fast32_1x2x_groups",
    "rt_resolve_copy_fast32_4x_dispatches",
    "rt_resolve_copy_fast32_4x_groups",
    "rt_resolve_copy_fast64_1x2x_dispatches",
    "rt_resolve_copy_fast64_1x2x_groups",
    "rt_resolve_copy_fast64_4x_dispatches",
    "rt_resolve_copy_fast64_4x_groups",
    "rt_resolve_copy_full8_dispatches",
    "rt_resolve_copy_full8_groups",
    "rt_resolve_copy_full16_dispatches",
    "rt_resolve_copy_full16_groups",
    "rt_resolve_copy_full32_dispatches",
    "rt_resolve_copy_full32_groups",
    "rt_resolve_copy_full64_dispatches",
    "rt_resolve_copy_full64_groups",
    "rt_resolve_copy_full128_dispatches",
    "rt_resolve_copy_full128_groups",
    "draw_no_pixel_depth_test_calls",
    "draw_no_pixel_depth_write_calls",
    "xma_frames_decoded",
    "audio_frame_latency_us",
    "buffer_queue_depth",
    "functions_dispatched",
    "interrupt_dispatches",
    "active_threads",
    "apc_queue_depth",
    "critical_region_contentions",
    "texture_cache_hits",
    "texture_cache_misses",
    "pipeline_cache_hits",
    "pipeline_cache_misses",
};
static_assert(std::size(kCounterNames) == kNumCounters, "kCounterNames must match CounterId enum");

// Gauge counters are snapshotted but NOT zeroed each frame.
// Accumulators (everything else) are zeroed after snapshot.
constexpr bool kIsGauge[] = {
    false,  // kFrameTimeUs       (set each frame)
    false,  // kFps               (set each frame)
    false,  // kGuestDrawPackets
    false,  // kDrawCalls
    false,  // kCommandBufferStalls
    false,  // kVerticesProcessed
    false,  // kPrimitivesProcessed
    false,  // kDrawMainCalls
    false,  // kDrawMainVertices
    false,  // kDrawMainPrimitives
    false,  // kDrawDepthCalls
    false,  // kDrawDepthVertices
    false,  // kDrawDepthPrimitives
    false,  // kDrawCopyCalls
    false,  // kDrawCopyVertices
    false,  // kDrawCopyPrimitives
    false,  // kDrawMemexportCalls
    false,  // kDrawMemexportVertices
    false,  // kDrawMemexportPrimitives
    false,  // kDrawNoPixelShaderCalls
    false,  // kDrawNoPixelShaderVertices
    false,  // kDrawNoPixelShaderPrimitives
    false,  // kDrawStageTotalUs
    false,  // kDrawStagePrimitiveUs
    false,  // kDrawStageRenderTargetUs
    false,  // kDrawStagePipelineUs
    false,  // kDrawStageTextureUs
    false,  // kDrawStageFixedFunctionUs
    false,  // kDrawStageBindingsUs
    false,  // kDrawStageVertexBuffersUs
    false,  // kDrawStageBarriersUs
    false,  // kDrawStageSubmitUs
    false,  // kDrawStageSystemConstantsUs
    false,  // kDrawSamplerFastPathDraws
    false,  // kDrawTextureFastPathDraws
    false,  // kDrawStageAnalyzeUs
    false,  // kDrawStageTranslateUs
    false,  // kDrawStageSamplersUs
    false,  // kDrawStagePreDrawUs
    false,  // kGpuMainUs
    false,  // kGpuDepthUs
    false,  // kGpuCopyUs
    false,  // kGpuMemexportUs
    false,  // kGpuNoPixelShaderUs
    false,  // kGpuCopyDumpUs
    false,  // kGpuCopyResolveUs
    false,  // kGpuCopyResolveFast32Us
    false,  // kGpuCopyResolveFull32Us
    false,  // kGpuResolveDownscaleUs
    false,  // kGpuTimestampedDraws
    false,  // kGpuCommandProcessorFrameUs
    false,  // kGpuTimestampedFrames
    false,  // kGpuBarrierUs
    false,  // kGpuClearUs
    false,  // kGpuCopyBufferUs
    false,  // kGpuCopyTextureUs
    false,  // kGpuCopyResourceUs
    false,  // kGpuDispatchUs
    false,  // kGpuResolveQueryUs
    false,  // kCpuPrimaryBufferUs
    false,  // kCpuIndirectBufferUs
    false,  // kCpuGuestWaitUs
    false,  // kCpuSwapUs
    false,  // kCpuD3D12BeginSubmissionUs
    false,  // kCpuD3D12BeginSubmissionFenceUs
    false,  // kCpuD3D12BeginSubmissionFrameOpenUs
    false,  // kCpuD3D12BeginSubmissionOpenUs
    false,  // kCpuD3D12ProcessGpuTimestampsUs
    false,  // kCpuD3D12EndSubmissionUs
    false,  // kCpuD3D12CheckFenceUs
    false,  // kCpuD3D12FenceWaitUs
    false,  // kCpuD3D12FenceReclaimUs
    false,  // kCpuD3D12EndFrameUs
    false,  // kCpuD3D12PipelineEndUs
    false,  // kCpuD3D12SubmitBarriersUs
    false,  // kCpuD3D12DeferredExecuteUs
    false,  // kCpuD3D12CommandListCloseUs
    false,  // kCpuD3D12ExecuteCommandListsUs
    false,  // kCpuD3D12SignalUs
    false,  // kCpuD3D12PaintTotalUs
    false,  // kCpuD3D12PaintConsumeUs
    false,  // kCpuD3D12PaintRecordUs
    false,  // kCpuD3D12PaintUiUs
    false,  // kCpuD3D12PresentWaitUs
    false,  // kCpuD3D12PresentUs
    false,  // kGpuThreadFenceWaitUs
    false,  // kTextureRequestUs
    false,  // kTextureRequestCalls
    false,  // kTextureFetchesRequested
    false,  // kTextureBindingsChanged
    false,  // kTexturePendingLoads
    false,  // kTexturePendingRanges
    false,  // kTexturePendingBytes
    false,  // kTextureSharedMemoryRequestUs
    false,  // kTextureCommitLoadUs
    false,  // kTextureLoadsCommitted
    false,  // kTextureLoadBytes
    false,  // kTextureLoadBackendUs
    false,  // kTextureScaledResolveCommitUs
    false,  // kD3D12BarriersQueued
    false,  // kD3D12BarriersSubmitted
    false,  // kD3D12TransitionBarriers
    false,  // kD3D12AliasingBarriers
    false,  // kD3D12UavBarriers
    false,  // kD3D12RtTransitions
    false,  // kD3D12DepthTransitions
    false,  // kD3D12CopyTransitions
    false,  // kD3D12SrvTransitions
    false,  // kD3D12UavTransitions
    false,  // kD3D12PresentTransitions
    false,  // kDeferredCommands
    false,  // kDeferredClearCount
    false,  // kDeferredClearUs
    false,  // kDeferredBarrierCount
    false,  // kDeferredBarrierUs
    false,  // kDeferredCopyBufferCount
    false,  // kDeferredCopyBufferBytes
    false,  // kDeferredCopyBufferUs
    false,  // kDeferredCopyTextureCount
    false,  // kDeferredCopyTextureUs
    false,  // kDeferredCopyResourceCount
    false,  // kDeferredCopyResourceUs
    false,  // kDeferredDispatchCount
    false,  // kDeferredDispatchUs
    false,  // kDeferredDrawCount
    false,  // kDeferredDrawUs
    false,  // kDeferredQueryCount
    false,  // kDeferredQueryUs
    false,  // kDeferredResolveQueryCount
    false,  // kDeferredResolveQueryUs
    false,  // kDeferredStateCount
    false,  // kDeferredStateUs
    false,  // kRtResolveCalls
    false,  // kRtResolveUs
    false,  // kRtResolveDirectCalls
    false,  // kRtResolveDirectUs
    false,  // kRtResolveTransferClearUs
    false,  // kRtResolveReadbackCalls
    false,  // kRtResolveReadbackUs
    false,  // kRtDumpCalls
    false,  // kRtDumpRectangles
    false,  // kRtDumpDispatches
    false,  // kRtDumpGroups
    false,  // kRtResolveCopyDispatches
    false,  // kRtResolveCopyGroups
    false,  // kRtResolveScaledBytes
    false,  // kRtScaledResolveWriteBarriers
    false,  // kRtScaledResolveWriteBarrierOverlaps
    false,  // kRtScaledResolveWriteBarrierSkipped
    false,  // kRtScaledResolveWriteBarrierBytes
    false,  // kRtResolveCopyFast32bpp1x2xMsaaDispatches
    false,  // kRtResolveCopyFast32bpp1x2xMsaaGroups
    false,  // kRtResolveCopyFast32bpp4xMsaaDispatches
    false,  // kRtResolveCopyFast32bpp4xMsaaGroups
    false,  // kRtResolveCopyFast64bpp1x2xMsaaDispatches
    false,  // kRtResolveCopyFast64bpp1x2xMsaaGroups
    false,  // kRtResolveCopyFast64bpp4xMsaaDispatches
    false,  // kRtResolveCopyFast64bpp4xMsaaGroups
    false,  // kRtResolveCopyFull8bppDispatches
    false,  // kRtResolveCopyFull8bppGroups
    false,  // kRtResolveCopyFull16bppDispatches
    false,  // kRtResolveCopyFull16bppGroups
    false,  // kRtResolveCopyFull32bppDispatches
    false,  // kRtResolveCopyFull32bppGroups
    false,  // kRtResolveCopyFull64bppDispatches
    false,  // kRtResolveCopyFull64bppGroups
    false,  // kRtResolveCopyFull128bppDispatches
    false,  // kRtResolveCopyFull128bppGroups
    false,  // kDrawNoPixelDepthTestCalls
    false,  // kDrawNoPixelDepthWriteCalls
    false,  // kXmaFramesDecoded
    false,  // kAudioFrameLatencyUs
    false,  // kBufferQueueDepth  (set each frame)
    false,  // kFunctionsDispatched
    false,  // kInterruptDispatches
    true,   // kActiveThreads     (inc/dec over lifetime)
    false,  // kApcQueueDepth
    true,   // kCriticalRegionContentions (running total)
    false,  // kTextureCacheHits
    false,  // kTextureCacheMisses
    false,  // kPipelineCacheHits
    false,  // kPipelineCacheMisses
};
static_assert(std::size(kIsGauge) == kNumCounters, "kIsGauge must match CounterId enum");

// CSV state
std::FILE* g_csv_file = nullptr;
std::string g_csv_path;
int g_csv_frame_count = 0;

}  // anonymous namespace

// DrawBucketName is defined inline in counter.h.

const char* CounterName(CounterId id) {
  auto idx = static_cast<size_t>(id);
  if (idx < kNumCounters)
    return kCounterNames[idx];
  return "unknown";
}

void AddFingerprintEntry(
    std::unordered_map<DrawFingerprintKey, DrawFingerprintEntry, DrawFingerprintKeyHash>& target,
    const DrawFingerprintEntry& source) {
  DrawFingerprintKey key{source.fingerprint};
  auto& entry = target[key];
  if (!entry.draw_count) {
    entry.fingerprint = source.fingerprint;
  }
  entry.draw_count += source.draw_count;
  entry.vertices += source.vertices;
  entry.primitives += source.primitives;
}

std::optional<std::filesystem::path> SaveDrawFingerprintEntries(
    std::vector<DrawFingerprintEntry> entries, const std::filesystem::path& path) {
  std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) {
    if (a.draw_count != b.draw_count) {
      return a.draw_count > b.draw_count;
    }
    return a.vertices > b.vertices;
  });

  std::ofstream out(path, std::ios::out | std::ios::trunc);
  if (!out) {
    return std::nullopt;
  }

  out << "bucket,draws,vertices,primitives,vs_hash,ps_hash,prim_type,draw_vertices,"
         "draw_primitives,vfetch0_addr,vfetch0_size,vfetch1_addr,vfetch1_size,vfetch2_addr,"
         "vfetch2_size,vfetch3_addr,vfetch3_size\n";
  for (const auto& entry : entries) {
    const auto& fp = entry.fingerprint;
    out << DrawBucketName(fp.bucket) << ',' << entry.draw_count << ',' << entry.vertices << ','
        << entry.primitives << ',';
    out << std::hex << fp.vertex_shader_hash << ',' << fp.pixel_shader_hash << std::dec << ',';
    out << fp.primitive_type << ',' << fp.vertex_count << ',' << fp.primitive_count;
    for (size_t i = 0; i < DrawFingerprint::kVertexFetchCount; ++i) {
      out << ',' << fp.vertex_fetch_address[i] << ',' << fp.vertex_fetch_size[i];
    }
    out << '\n';
  }
  return path;
}

std::optional<std::filesystem::path> SaveCaptureCounters(const CaptureState& capture) {
  std::ofstream out(capture.counters_path, std::ios::out | std::ios::trunc);
  if (!out) {
    return std::nullopt;
  }

  const uint32_t frame_count = std::max(capture.captured_frames, uint32_t(1));
  out << "counter,total,avg_per_frame\n";
  for (uint16_t i = 0; i < static_cast<uint16_t>(CounterId::kCount); ++i) {
    auto id = static_cast<CounterId>(i);
    const int64_t total = capture.counter_totals[i];
    out << CounterName(id) << ',' << total << ','
        << (static_cast<double>(total) / static_cast<double>(frame_count)) << '\n';
  }
  return capture.counters_path;
}

void SetCounter(CounterId id, int64_t value) {
  g_counters[static_cast<size_t>(id)].store(value, std::memory_order_relaxed);
}

void IncrementCounter(CounterId id, int64_t delta) {
  g_counters[static_cast<size_t>(id)].fetch_add(delta, std::memory_order_relaxed);
}

void RecordDrawBucket(DrawBucket bucket, int64_t vertices, int64_t primitives) {
  CounterId calls_id;
  CounterId vertices_id;
  CounterId primitives_id;
  switch (bucket) {
    case DrawBucket::kMainColorDepth:
      calls_id = CounterId::kDrawMainCalls;
      vertices_id = CounterId::kDrawMainVertices;
      primitives_id = CounterId::kDrawMainPrimitives;
      break;
    case DrawBucket::kDepthOnly:
      calls_id = CounterId::kDrawDepthCalls;
      vertices_id = CounterId::kDrawDepthVertices;
      primitives_id = CounterId::kDrawDepthPrimitives;
      break;
    case DrawBucket::kCopyResolve:
      calls_id = CounterId::kDrawCopyCalls;
      vertices_id = CounterId::kDrawCopyVertices;
      primitives_id = CounterId::kDrawCopyPrimitives;
      break;
    case DrawBucket::kMemexport:
      calls_id = CounterId::kDrawMemexportCalls;
      vertices_id = CounterId::kDrawMemexportVertices;
      primitives_id = CounterId::kDrawMemexportPrimitives;
      break;
    case DrawBucket::kNoPixelShader:
      calls_id = CounterId::kDrawNoPixelShaderCalls;
      vertices_id = CounterId::kDrawNoPixelShaderVertices;
      primitives_id = CounterId::kDrawNoPixelShaderPrimitives;
      break;
    default:
      return;
  }
  IncrementCounter(calls_id);
  IncrementCounter(vertices_id, vertices);
  IncrementCounter(primitives_id, primitives);
}

void RecordDrawFingerprint(const DrawFingerprint& fingerprint) {
  if (!ShouldCollectDrawFingerprints()) {
    return;
  }
  std::lock_guard lock(g_draw_fingerprints_mutex);
  DrawFingerprintKey key{fingerprint};
  auto& entry = g_draw_fingerprints_live[key];
  if (!entry.draw_count) {
    entry.fingerprint = fingerprint;
  }
  ++entry.draw_count;
  entry.vertices += fingerprint.vertex_count;
  entry.primitives += fingerprint.primitive_count;
}

std::vector<DrawFingerprintEntry> GetDrawFingerprintSnapshot() {
  std::lock_guard lock(g_draw_fingerprints_mutex);
  return g_draw_fingerprints_snapshot;
}

std::optional<std::filesystem::path> SaveDrawFingerprintSnapshot(const std::filesystem::path& path) {
  return SaveDrawFingerprintEntries(GetDrawFingerprintSnapshot(), path);
}

std::optional<std::filesystem::path> SaveCounterSnapshot(const std::filesystem::path& path) {
  std::ofstream out(path, std::ios::out | std::ios::trunc);
  if (!out) {
    return std::nullopt;
  }

  out << "counter,value\n";
  for (uint16_t i = 0; i < static_cast<uint16_t>(CounterId::kCount); ++i) {
    auto id = static_cast<CounterId>(i);
    out << CounterName(id) << ',' << GetSnapshotCounter(id) << '\n';
  }
  return path;
}

bool StartCapture(const std::filesystem::path& counters_path,
                  const std::filesystem::path& fingerprints_path) {
  std::lock_guard lock(g_capture_mutex);
  if (g_capture.active) {
    return false;
  }

  CaptureState capture;
  capture.active = true;
  capture.record_frames_remaining =
      static_cast<uint32_t>(std::max(INT32_C(1), REXCVAR_GET(perf_capture_frames)));
  capture.drain_frames_remaining = kCaptureDrainFrames;
  capture.counters_path = counters_path;
  capture.fingerprints_path = fingerprints_path;
  g_capture = std::move(capture);
  g_capture_recording.store(true, std::memory_order_release);
  return true;
}

bool IsCaptureRecording() {
  return g_capture_recording.load(std::memory_order_acquire);
}

bool ShouldCollectDrawDiagnostics() {
  return REXCVAR_GET(perf_diagnostics) || IsCaptureRecording();
}

bool ShouldCollectDrawFingerprints() {
  return REXCVAR_GET(perf_draw_fingerprints) || ShouldCollectDrawDiagnostics();
}

int64_t GetCounter(CounterId id) {
  return g_counters[static_cast<size_t>(id)].load(std::memory_order_relaxed);
}

void ResetFrameCounters() {
  for (size_t i = 0; i < kNumCounters; ++i) {
    if (kIsGauge[i]) {
      // Gauges: snapshot the current value, don't zero
      g_snapshot[i].store(g_counters[i].load(std::memory_order_relaxed), std::memory_order_relaxed);
    } else {
      // Accumulators: snapshot and zero for next frame
      g_snapshot[i].store(g_counters[i].exchange(0, std::memory_order_relaxed),
                          std::memory_order_relaxed);
    }
  }

  std::lock_guard lock(g_draw_fingerprints_mutex);
  g_draw_fingerprints_snapshot.clear();
  g_draw_fingerprints_snapshot.reserve(g_draw_fingerprints_live.size());
  for (const auto& [_, entry] : g_draw_fingerprints_live) {
    g_draw_fingerprints_snapshot.push_back(entry);
  }
  g_draw_fingerprints_live.clear();

  CaptureState capture_to_save;
  bool save_capture = false;
  {
    std::lock_guard capture_lock(g_capture_mutex);
    if (g_capture.active) {
      const bool recording = g_capture.record_frames_remaining != 0;
      if (recording) {
        for (size_t i = 0; i < kNumCounters; ++i) {
          g_capture.counter_totals[i] += g_snapshot[i].load(std::memory_order_relaxed);
        }
      } else if (g_capture.drain_frames_remaining != 0) {
        for (size_t i = 0; i < kNumCounters; ++i) {
          if (IsDelayedGpuTimingCounter(i)) {
            g_capture.counter_totals[i] += g_snapshot[i].load(std::memory_order_relaxed);
          }
        }
      }
      if (recording) {
        for (const auto& entry : g_draw_fingerprints_snapshot) {
          AddFingerprintEntry(g_capture.draw_fingerprints, entry);
        }
        ++g_capture.captured_frames;
        --g_capture.record_frames_remaining;
        if (g_capture.record_frames_remaining == 0) {
          g_capture_recording.store(false, std::memory_order_release);
        }
      } else if (g_capture.drain_frames_remaining != 0) {
        --g_capture.drain_frames_remaining;
      }
      if (g_capture.record_frames_remaining == 0 && g_capture.drain_frames_remaining == 0) {
        capture_to_save = std::move(g_capture);
        g_capture = {};
        g_capture_recording.store(false, std::memory_order_release);
        save_capture = true;
      }
    }
  }

  if (save_capture) {
    auto counters_path = SaveCaptureCounters(capture_to_save);
    std::vector<DrawFingerprintEntry> entries;
    entries.reserve(capture_to_save.draw_fingerprints.size());
    for (const auto& [_, entry] : capture_to_save.draw_fingerprints) {
      entries.push_back(entry);
    }
    auto fingerprints_path =
        SaveDrawFingerprintEntries(std::move(entries), capture_to_save.fingerprints_path);
    if (counters_path) {
      REXLOG_INFO("Saved perf capture counter log to {}", counters_path->string());
    } else {
      REXLOG_WARN("Failed to save perf capture counter log to {}",
                  capture_to_save.counters_path.string());
    }
    if (fingerprints_path) {
      REXLOG_INFO("Saved perf capture draw fingerprint log to {}", fingerprints_path->string());
    } else {
      REXLOG_WARN("Failed to save perf capture draw fingerprint log to {}",
                  capture_to_save.fingerprints_path.string());
    }
  }
}

int64_t GetSnapshotCounter(CounterId id) {
  return g_snapshot[static_cast<size_t>(id)].load(std::memory_order_relaxed);
}

void Init() {
  for (auto& c : g_counters)
    c.store(0, std::memory_order_relaxed);
  for (auto& s : g_snapshot)
    s.store(0, std::memory_order_relaxed);
}

void SetCsvLogPath(const std::string& path) {
  if (g_csv_file) {
    std::fflush(g_csv_file);
    std::fclose(g_csv_file);
    g_csv_file = nullptr;
  }
  g_csv_path = path;
  g_csv_frame_count = 0;

  if (path.empty())
    return;

  g_csv_file = std::fopen(path.c_str(), "w");
  if (!g_csv_file) {
    REXLOG_WARN("perf: failed to open CSV log: {}", path);
    g_csv_path.clear();
    return;
  }

  // Write header
  for (size_t i = 0; i < kNumCounters; ++i) {
    if (i > 0)
      std::fputc(',', g_csv_file);
    std::fputs(kCounterNames[i], g_csv_file);
  }
  std::fputc('\n', g_csv_file);
}

void WriteCsvFrame() {
  if (!g_csv_file)
    return;

  for (size_t i = 0; i < kNumCounters; ++i) {
    if (i > 0)
      std::fputc(',', g_csv_file);
    std::fprintf(g_csv_file, "%lld",
                 static_cast<long long>(g_snapshot[i].load(std::memory_order_relaxed)));
  }
  std::fputc('\n', g_csv_file);

  if (++g_csv_frame_count % 60 == 0) {
    std::fflush(g_csv_file);
  }
}

void FlushCsv() {
  if (g_csv_file) {
    std::fflush(g_csv_file);
    std::fclose(g_csv_file);
    g_csv_file = nullptr;
  }
  g_csv_path.clear();
}

}  // namespace rex::perf
