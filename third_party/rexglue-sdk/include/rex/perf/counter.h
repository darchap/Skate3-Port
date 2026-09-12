/**
 * @file        perf/counter.h
 * @brief       Performance counter registry and profiler
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#ifdef REXGLUE_ENABLE_PROFILING
#include <tracy/Tracy.hpp>
#endif

#if defined(_M_X64) || defined(_M_IX86)
#include <intrin.h>
#define REX_PERF_CPU_PROFILE_RDTSC 1
#elif defined(__x86_64__) || defined(__i386__)
#include <x86intrin.h>
#define REX_PERF_CPU_PROFILE_RDTSC 1
#endif

namespace rex::perf {

enum class DrawBucket : uint8_t {
  kMainColorDepth,
  kDepthOnly,
  kCopyResolve,
  kMemexport,
  kNoPixelShader,
  kCopyDump,
  kCopyResolveShader,
  kCopyResolveFast32,
  kCopyResolveFull32,
  kResolveDownscale,
  // Guest render pass entry: ending the previous render pass, beginning the
  // new one and replaying deferred resolve clears. Not aggregated into the
  // draw/res/dump FPS overlay lines - shows up in "other".
  kRenderPassEntry,
  // Render target ownership transfers and resolve clears performed in
  // PerformTransfersAndResolveClears.
  kRtTransfer,
  // Texture cache uploads (guest texture data load compute dispatches).
  kTextureUpload,
  // Shared memory (guest memory mirror buffer) uploads.
  kSharedMemoryUpload,
  // From the end of a resolve operation until the next instrumented point -
  // the post-resolve barriers and any idle gap before the next draws.
  kResolveGap,
  // The buffer-to-image copy phase of a texture upload (kTextureUpload then
  // covers only the load compute dispatches and their barriers).
  kTextureUploadCopy,
  // The native guest-output renderer's whole GPU pass (scene + 2D replay +
  // resolve), recorded inside IssueSwap. Runs until frame end.
  kNativeScene,
  // Native-pass stages (nrhi::Cmd::ProfileRegion): successive marks partition
  // the native pass into its renderer-defined stages; when the renderer marks
  // them, they replace kNativeScene in the frame's attribution.
  kNativeCommit,   // content-store commits, decode uploads, eviction
  kNativeShadow,   // dynamic caster shadow atlas + blur
  kNativeSun,      // static sun-shadow cascade renders
  kNativeMain,     // main scene pass (opaque + transparent + SSR G-buffer)
  kNativeResolve,  // MSAA resolve + outline composite
  kNativeAo,       // screen-space ambient occlusion
  kNativeSsr,      // screen-space reflection march + composite
  kNativeVol,      // volumetric sun shafts + haze
  kNativeBloom,    // bloom pyramid + tonemap
  kNative2d,       // photo grab, blur chains, 2D overlay replay
  kNativeTail,     // after the last stage: backend/present tail
  kCount,
};

// Short display name of a draw bucket (for logs and capture CSVs).
inline const char* DrawBucketName(DrawBucket bucket) {
  switch (bucket) {
    case DrawBucket::kMainColorDepth:
      return "Main";
    case DrawBucket::kDepthOnly:
      return "Depth";
    case DrawBucket::kCopyResolve:
      return "Copy";
    case DrawBucket::kMemexport:
      return "MemExp";
    case DrawBucket::kNoPixelShader:
      return "NoPS";
    case DrawBucket::kCopyDump:
      return "CopyDump";
    case DrawBucket::kCopyResolveShader:
      return "CopyResolve";
    case DrawBucket::kCopyResolveFast32:
      return "CopyResolveFast32";
    case DrawBucket::kCopyResolveFull32:
      return "CopyResolveFull32";
    case DrawBucket::kResolveDownscale:
      return "ResolveDownscale";
    case DrawBucket::kRenderPassEntry:
      return "PassEntry";
    case DrawBucket::kRtTransfer:
      return "RtTransfer";
    case DrawBucket::kTextureUpload:
      return "TexUpload";
    case DrawBucket::kSharedMemoryUpload:
      return "SMUpload";
    case DrawBucket::kResolveGap:
      return "ResolveGap";
    case DrawBucket::kTextureUploadCopy:
      return "TexUpCopy";
    case DrawBucket::kNativeScene:
      return "Native";
    case DrawBucket::kNativeCommit:
      return "NatCommit";
    case DrawBucket::kNativeShadow:
      return "NatShadow";
    case DrawBucket::kNativeSun:
      return "NatSun";
    case DrawBucket::kNativeMain:
      return "NatMain";
    case DrawBucket::kNativeResolve:
      return "NatResolve";
    case DrawBucket::kNativeAo:
      return "NatAO";
    case DrawBucket::kNativeSsr:
      return "NatSSR";
    case DrawBucket::kNativeVol:
      return "NatVol";
    case DrawBucket::kNativeBloom:
      return "NatBloom";
    case DrawBucket::kNative2d:
      return "Nat2D";
    case DrawBucket::kNativeTail:
      return "NatTail";
    default:
      return "Unknown";
  }
}

struct DrawFingerprint {
  DrawBucket bucket = DrawBucket::kMainColorDepth;
  uint64_t vertex_shader_hash = 0;
  uint64_t pixel_shader_hash = 0;
  uint32_t primitive_type = 0;
  uint32_t vertex_count = 0;
  uint32_t primitive_count = 0;
  static constexpr size_t kVertexFetchCount = 4;
  uint32_t vertex_fetch_address[kVertexFetchCount] = {};
  uint32_t vertex_fetch_size[kVertexFetchCount] = {};
};

struct DrawFingerprintEntry {
  DrawFingerprint fingerprint;
  uint64_t draw_count = 0;
  uint64_t vertices = 0;
  uint64_t primitives = 0;
};

enum class CounterId : uint16_t {
  // Frame
  kFrameTimeUs,
  kFps,

  // GPU
  kGuestDrawPackets,
  kDrawCalls,
  kCommandBufferStalls,
  kVerticesProcessed,
  kPrimitivesProcessed,
  kDrawMainCalls,
  kDrawMainVertices,
  kDrawMainPrimitives,
  kDrawDepthCalls,
  kDrawDepthVertices,
  kDrawDepthPrimitives,
  kDrawCopyCalls,
  kDrawCopyVertices,
  kDrawCopyPrimitives,
  kDrawMemexportCalls,
  kDrawMemexportVertices,
  kDrawMemexportPrimitives,
  kDrawNoPixelShaderCalls,
  kDrawNoPixelShaderVertices,
  kDrawNoPixelShaderPrimitives,
  kDrawStageTotalUs,
  kDrawStagePrimitiveUs,
  kDrawStageRenderTargetUs,
  kDrawStagePipelineUs,
  kDrawStageTextureUs,
  kDrawStageFixedFunctionUs,
  kDrawStageBindingsUs,
  kDrawStageVertexBuffersUs,
  kDrawStageBarriersUs,
  kDrawStageSubmitUs,
  kDrawStageSystemConstantsUs,
  kDrawSamplerFastPathDraws,
  kDrawTextureFastPathDraws,
  kDrawStageAnalyzeUs,
  kDrawStageTranslateUs,
  kDrawStageSamplersUs,
  kDrawStagePreDrawUs,
  kGpuMainUs,
  kGpuDepthUs,
  kGpuCopyUs,
  kGpuMemexportUs,
  kGpuNoPixelShaderUs,
  kGpuCopyDumpUs,
  kGpuCopyResolveUs,
  kGpuCopyResolveFast32Us,
  kGpuCopyResolveFull32Us,
  kGpuResolveDownscaleUs,
  kGpuTimestampedDraws,
  kGpuCommandProcessorFrameUs,
  kGpuTimestampedFrames,
  kGpuBarrierUs,
  kGpuClearUs,
  kGpuCopyBufferUs,
  kGpuCopyTextureUs,
  kGpuCopyResourceUs,
  kGpuDispatchUs,
  kGpuResolveQueryUs,
  kCpuPrimaryBufferUs,
  kCpuIndirectBufferUs,
  kCpuGuestWaitUs,
  kCpuSwapUs,
  kCpuD3D12BeginSubmissionUs,
  kCpuD3D12BeginSubmissionFenceUs,
  kCpuD3D12BeginSubmissionFrameOpenUs,
  kCpuD3D12BeginSubmissionOpenUs,
  kCpuD3D12ProcessGpuTimestampsUs,
  kCpuD3D12EndSubmissionUs,
  kCpuD3D12CheckFenceUs,
  kCpuD3D12FenceWaitUs,
  kCpuD3D12FenceReclaimUs,
  kCpuD3D12EndFrameUs,
  kCpuD3D12PipelineEndUs,
  kCpuD3D12SubmitBarriersUs,
  kCpuD3D12DeferredExecuteUs,
  kCpuD3D12CommandListCloseUs,
  kCpuD3D12ExecuteCommandListsUs,
  kCpuD3D12SignalUs,
  kCpuD3D12PaintTotalUs,
  kCpuD3D12PaintConsumeUs,
  kCpuD3D12PaintRecordUs,
  kCpuD3D12PaintUiUs,
  kCpuD3D12PresentWaitUs,
  kCpuD3D12PresentUs,
  // Time the GPU emulation thread spent blocked waiting for submission
  // fences (both backends) - high values mean GPU-bound, near zero means the
  // thread itself is the bottleneck.
  kGpuThreadFenceWaitUs,
  kTextureRequestUs,
  kTextureRequestCalls,
  kTextureFetchesRequested,
  kTextureBindingsChanged,
  kTexturePendingLoads,
  kTexturePendingRanges,
  kTexturePendingBytes,
  kTextureSharedMemoryRequestUs,
  kTextureCommitLoadUs,
  kTextureLoadsCommitted,
  kTextureLoadBytes,
  kTextureLoadBackendUs,
  kTextureScaledResolveCommitUs,
  kD3D12BarriersQueued,
  kD3D12BarriersSubmitted,
  kD3D12TransitionBarriers,
  kD3D12AliasingBarriers,
  kD3D12UavBarriers,
  kD3D12RtTransitions,
  kD3D12DepthTransitions,
  kD3D12CopyTransitions,
  kD3D12SrvTransitions,
  kD3D12UavTransitions,
  kD3D12PresentTransitions,
  kDeferredCommands,
  kDeferredClearCount,
  kDeferredClearUs,
  kDeferredBarrierCount,
  kDeferredBarrierUs,
  kDeferredCopyBufferCount,
  kDeferredCopyBufferBytes,
  kDeferredCopyBufferUs,
  kDeferredCopyTextureCount,
  kDeferredCopyTextureUs,
  kDeferredCopyResourceCount,
  kDeferredCopyResourceUs,
  kDeferredDispatchCount,
  kDeferredDispatchUs,
  kDeferredDrawCount,
  kDeferredDrawUs,
  kDeferredQueryCount,
  kDeferredQueryUs,
  kDeferredResolveQueryCount,
  kDeferredResolveQueryUs,
  kDeferredStateCount,
  kDeferredStateUs,
  kRtResolveCalls,
  kRtResolveUs,
  kRtResolveDirectCalls,
  kRtResolveDirectUs,
  kRtResolveTransferClearUs,
  kRtResolveReadbackCalls,
  kRtResolveReadbackUs,
  kRtDumpCalls,
  kRtDumpRectangles,
  kRtDumpDispatches,
  kRtDumpGroups,
  kRtResolveCopyDispatches,
  kRtResolveCopyGroups,
  kRtResolveScaledBytes,
  kRtScaledResolveWriteBarriers,
  kRtScaledResolveWriteBarrierOverlaps,
  kRtScaledResolveWriteBarrierSkipped,
  kRtScaledResolveWriteBarrierBytes,
  kRtResolveCopyFast32bpp1x2xMsaaDispatches,
  kRtResolveCopyFast32bpp1x2xMsaaGroups,
  kRtResolveCopyFast32bpp4xMsaaDispatches,
  kRtResolveCopyFast32bpp4xMsaaGroups,
  kRtResolveCopyFast64bpp1x2xMsaaDispatches,
  kRtResolveCopyFast64bpp1x2xMsaaGroups,
  kRtResolveCopyFast64bpp4xMsaaDispatches,
  kRtResolveCopyFast64bpp4xMsaaGroups,
  kRtResolveCopyFull8bppDispatches,
  kRtResolveCopyFull8bppGroups,
  kRtResolveCopyFull16bppDispatches,
  kRtResolveCopyFull16bppGroups,
  kRtResolveCopyFull32bppDispatches,
  kRtResolveCopyFull32bppGroups,
  kRtResolveCopyFull64bppDispatches,
  kRtResolveCopyFull64bppGroups,
  kRtResolveCopyFull128bppDispatches,
  kRtResolveCopyFull128bppGroups,
  kDrawNoPixelDepthTestCalls,
  kDrawNoPixelDepthWriteCalls,

  // Audio
  kXmaFramesDecoded,
  kAudioFrameLatencyUs,
  kBufferQueueDepth,

  // Dispatch
  kFunctionsDispatched,
  kInterruptDispatches,

  // Threading
  kActiveThreads,
  kApcQueueDepth,
  kCriticalRegionContentions,

  // Caches
  kTextureCacheHits,
  kTextureCacheMisses,
  kPipelineCacheHits,
  kPipelineCacheMisses,

  kCount  // sentinel -- must be last
};

// Lightweight CPU-side scope attribution for perf-counter builds. When enabled
// at runtime (the gpu_cpu_profile cvar), every ScopedCounterTimer scope
// accumulates raw timestamp ticks and a hit count per CounterId; the command
// processor converts ticks to microseconds against a wall-clock calibration
// window and logs the breakdown periodically at frame end. Constraint: each
// CounterId may only be written from one thread (slots are not atomic).
namespace cpu_profile {

inline uint64_t Ticks() {
#ifdef REX_PERF_CPU_PROFILE_RDTSC
  return __rdtsc();
#else
  return uint64_t(std::chrono::steady_clock::now().time_since_epoch().count());
#endif
}

struct Slot {
  uint64_t ticks;
  uint64_t count;
};

inline std::atomic<bool> enabled{false};
inline Slot slots[size_t(CounterId::kCount)]{};

inline void Reset() {
  for (Slot& slot : slots) {
    slot.ticks = 0;
    slot.count = 0;
  }
}

// Count an event (no time attribution) when the CPU profile is armed.
inline void CountEvent(CounterId id) {
#ifdef REXGLUE_ENABLE_PERF_COUNTERS
  if (enabled.load(std::memory_order_relaxed)) {
    ++slots[size_t(id)].count;
  }
#else
  (void)id;
#endif
}

}  // namespace cpu_profile

#ifdef REXGLUE_ENABLE_PERF_COUNTERS

// Returns human-readable name for a counter (e.g. "frame_time_us")
const char* CounterName(CounterId id);

// Set a counter to an absolute value
void SetCounter(CounterId id, int64_t value);

// Atomically add to a counter
void IncrementCounter(CounterId id, int64_t delta = 1);

// Record one draw-like operation in a coarse render bucket for view comparisons.
void RecordDrawBucket(DrawBucket bucket, int64_t vertices, int64_t primitives);

// Aggregate one submitted draw for the last completed frame's fingerprint dump.
void RecordDrawFingerprint(const DrawFingerprint& fingerprint);

// Return a copy of the last completed frame's aggregated fingerprints.
std::vector<DrawFingerprintEntry> GetDrawFingerprintSnapshot();

// Save the last completed frame's aggregated fingerprints. Returns the written
// path, or std::nullopt if the file couldn't be opened.
std::optional<std::filesystem::path> SaveDrawFingerprintSnapshot(const std::filesystem::path& path);

// Save the last completed frame's counter snapshot as name,value CSV rows.
// Returns the written path, or std::nullopt if the file couldn't be opened.
std::optional<std::filesystem::path> SaveCounterSnapshot(const std::filesystem::path& path);

// Start a multi-frame capture. Counter totals and average-per-frame values are
// written after the requested frame count plus a short drain for delayed GPU
// timestamp query results. Returns false if another capture is already active.
bool StartCapture(const std::filesystem::path& counters_path,
                  const std::filesystem::path& fingerprints_path);

bool IsCaptureRecording();

// True when always-on heavyweight draw diagnostics are enabled, or while an
// explicit F8 capture is recording.
bool ShouldCollectDrawDiagnostics();

// True when callsites should build and record per-draw fingerprints.
bool ShouldCollectDrawFingerprints();

class ScopedCounterTimer {
 public:
  explicit ScopedCounterTimer(CounterId counter_id,
                              bool active = ShouldCollectDrawDiagnostics())
      : counter_id_(counter_id), active_(active) {
    if (active_) {
      start_ = Clock::now();
    }
    if (cpu_profile::enabled.load(std::memory_order_relaxed)) {
      cpu_profile_start_ticks_ = cpu_profile::Ticks();
    }
  }

  ~ScopedCounterTimer() {
    Stop();
  }

  void Stop() {
    if (cpu_profile_start_ticks_) {
      cpu_profile::Slot& slot = cpu_profile::slots[size_t(counter_id_)];
      slot.ticks += cpu_profile::Ticks() - cpu_profile_start_ticks_;
      ++slot.count;
      cpu_profile_start_ticks_ = 0;
    }
    if (!active_) {
      return;
    }
    auto elapsed_us =
        std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - start_).count();
    IncrementCounter(counter_id_, elapsed_us);
    active_ = false;
  }

  ScopedCounterTimer(const ScopedCounterTimer&) = delete;
  ScopedCounterTimer& operator=(const ScopedCounterTimer&) = delete;

 private:
  using Clock = std::chrono::steady_clock;
  CounterId counter_id_;
  bool active_;
  Clock::time_point start_;
  uint64_t cpu_profile_start_ticks_ = 0;
};

// Read a counter's current live value
int64_t GetCounter(CounterId id);

// Snapshot current values into the read buffer and zero the live counters.
// Called once per frame by Profiler::Flip().
void ResetFrameCounters();

// Read a counter from the last-frame snapshot (stable between frames).
int64_t GetSnapshotCounter(CounterId id);

// Initialize the counter system (zeroes everything). Safe to call multiple times.
void Init();

// CSV logging
void SetCsvLogPath(const std::string& path);
void WriteCsvFrame();
void FlushCsv();

#else

inline const char* CounterName(CounterId id) {
  (void)id;
  return "";
}

inline void SetCounter(CounterId id, int64_t value) {
  (void)id;
  (void)value;
}

inline void IncrementCounter(CounterId id, int64_t delta = 1) {
  (void)id;
  (void)delta;
}

inline void RecordDrawBucket(DrawBucket bucket, int64_t vertices, int64_t primitives) {
  (void)bucket;
  (void)vertices;
  (void)primitives;
}

inline void RecordDrawFingerprint(const DrawFingerprint& fingerprint) {
  (void)fingerprint;
}

inline std::vector<DrawFingerprintEntry> GetDrawFingerprintSnapshot() {
  return {};
}

inline std::optional<std::filesystem::path> SaveDrawFingerprintSnapshot(
    const std::filesystem::path& path) {
  (void)path;
  return std::nullopt;
}

inline std::optional<std::filesystem::path> SaveCounterSnapshot(
    const std::filesystem::path& path) {
  (void)path;
  return std::nullopt;
}

inline bool StartCapture(const std::filesystem::path& counters_path,
                         const std::filesystem::path& fingerprints_path) {
  (void)counters_path;
  (void)fingerprints_path;
  return false;
}

inline bool IsCaptureRecording() {
  return false;
}

inline bool ShouldCollectDrawDiagnostics() {
  return false;
}

inline bool ShouldCollectDrawFingerprints() {
  return false;
}

class ScopedCounterTimer {
 public:
  explicit ScopedCounterTimer(CounterId counter_id, bool active = false) {
    (void)counter_id;
    (void)active;
  }

  ~ScopedCounterTimer() = default;

  void Stop() {}

  ScopedCounterTimer(const ScopedCounterTimer&) = delete;
  ScopedCounterTimer& operator=(const ScopedCounterTimer&) = delete;
};

inline int64_t GetCounter(CounterId id) {
  (void)id;
  return 0;
}

inline void ResetFrameCounters() {}

inline int64_t GetSnapshotCounter(CounterId id) {
  (void)id;
  return 0;
}

inline void Init() {}

inline void SetCsvLogPath(const std::string& path) {
  (void)path;
}

inline void WriteCsvFrame() {}

inline void FlushCsv() {}

#endif

// Profiler -- coordinates Tracy frame marks and counter snapshots.
// Moved here from rex::debug to consolidate all perf code under rex::perf.
class Profiler {
 public:
  // Call once at runtime startup to enable Tracy's network threads.
  // CLI tools should skip this to avoid socket listeners entirely.
  static void Startup() {
#ifdef REXGLUE_ENABLE_PROFILING
    tracy::StartupProfiler();
#endif
  }
  static void OnThreadEnter(const char* name = nullptr) {
#ifdef REXGLUE_ENABLE_PROFILING
    if (name)
      tracy::SetThreadName(name);
#else
    (void)name;
#endif
  }
  static void OnThreadExit() {}
  static void ThreadEnter(const char* name = nullptr) { OnThreadEnter(name); }
  static void ThreadExit() {}
  static void Flip() {
#ifdef REXGLUE_ENABLE_PROFILING
    FrameMark;
#endif
#ifdef REXGLUE_ENABLE_PERF_COUNTERS
    ResetFrameCounters();
    WriteCsvFrame();
#endif
  }
  static void Flush() {}
  static void Shutdown() {
#ifdef REXGLUE_ENABLE_PROFILING
    tracy::ShutdownProfiler();
#endif
#ifdef REXGLUE_ENABLE_PERF_COUNTERS
    FlushCsv();
#endif
  }
  static bool is_enabled() {
#ifdef REXGLUE_ENABLE_PROFILING
    return tracy::IsProfilerStarted();
#else
    return false;
#endif
  }
};

}  // namespace rex::perf

// Perf counter macros -- compile to no-ops when counters are disabled. CPU
// profile scope timers are perf-build-only so shipped builds do not pay
// per-scope constructor/destructor or atomic-check costs.
#define REX_PERF_CONCAT_INNER(a, b) a##b
#define REX_PERF_CONCAT(a, b) REX_PERF_CONCAT_INNER(a, b)

#ifdef REXGLUE_ENABLE_PERF_COUNTERS

// Generic helpers for easily adding new counters
#define PERF_counter_set(id, value) rex::perf::SetCounter(rex::perf::CounterId::id, value)
#define PERF_counter_inc(id) rex::perf::IncrementCounter(rex::perf::CounterId::id)
#define PERF_counter_add(id, delta) rex::perf::IncrementCounter(rex::perf::CounterId::id, delta)

// Purpose-specific macros so callsites stay clean
#define PROFILE_FRAME_TIME_US(value) PERF_counter_set(kFrameTimeUs, value)
#define PROFILE_FPS(value) PERF_counter_set(kFps, value)
#define PROFILE_FUNCTION_DISPATCHED() PERF_counter_inc(kFunctionsDispatched)
#define PROFILE_INTERRUPT_DISPATCHED() PERF_counter_inc(kInterruptDispatches)
#define PROFILE_XMA_FRAME_DECODED() PERF_counter_inc(kXmaFramesDecoded)
#define PROFILE_GUEST_DRAW_PACKET() PERF_counter_inc(kGuestDrawPackets)
#define PROFILE_DRAW_CALL() PERF_counter_inc(kDrawCalls)
#define PROFILE_VERTICES(n) PERF_counter_add(kVerticesProcessed, n)
#define PROFILE_PRIMITIVES(n) PERF_counter_add(kPrimitivesProcessed, n)
#define PROFILE_DRAW_BUCKET(bucket, vertices, primitives) \
  rex::perf::RecordDrawBucket(bucket, vertices, primitives)
#define PROFILE_DRAW_FINGERPRINT(fingerprint) rex::perf::RecordDrawFingerprint(fingerprint)
#define PROFILE_DRAW_STAGE_US(id, delta) rex::perf::IncrementCounter((id), (delta))
#define PROFILE_GPU_TIMESTAMPED_DRAW() PERF_counter_inc(kGpuTimestampedDraws)
#define PROFILE_SCOPE_COUNTER(id) \
  rex::perf::ScopedCounterTimer REX_PERF_CONCAT(perf_scope_timer_, __LINE__)(rex::perf::CounterId::id)
#define PROFILE_SCOPE_COUNTER_IF(id, active) \
  rex::perf::ScopedCounterTimer REX_PERF_CONCAT(perf_scope_timer_, __LINE__)( \
      rex::perf::CounterId::id, active)
#define PROFILE_CMD_BUFFER_STALL() PERF_counter_inc(kCommandBufferStalls)
#define PROFILE_AUDIO_LATENCY_US(value) PERF_counter_set(kAudioFrameLatencyUs, value)
#define PROFILE_BUFFER_QUEUE_DEPTH(value) PERF_counter_set(kBufferQueueDepth, value)
#define PROFILE_THREAD_CREATED() PERF_counter_inc(kActiveThreads)
#define PROFILE_THREAD_EXITED() PERF_counter_add(kActiveThreads, -1)
#define PROFILE_APC_QUEUE_DEPTH(value) PERF_counter_set(kApcQueueDepth, value)
#define PROFILE_CRITICAL_REGION_CONTENTION() PERF_counter_inc(kCriticalRegionContentions)
#define PROFILE_TEXTURE_CACHE_HIT() PERF_counter_inc(kTextureCacheHits)
#define PROFILE_TEXTURE_CACHE_MISS() PERF_counter_inc(kTextureCacheMisses)
#define PROFILE_PIPELINE_CACHE_HIT() PERF_counter_inc(kPipelineCacheHits)
#define PROFILE_PIPELINE_CACHE_MISS() PERF_counter_inc(kPipelineCacheMisses)

#else

#define PERF_counter_set(id, value)
#define PERF_counter_inc(id)
#define PERF_counter_add(id, delta)

#define PROFILE_FRAME_TIME_US(value)
#define PROFILE_FPS(value)
#define PROFILE_FUNCTION_DISPATCHED()
#define PROFILE_INTERRUPT_DISPATCHED()
#define PROFILE_XMA_FRAME_DECODED()
#define PROFILE_GUEST_DRAW_PACKET()
#define PROFILE_DRAW_CALL()
#define PROFILE_VERTICES(n)
#define PROFILE_PRIMITIVES(n)
#define PROFILE_DRAW_BUCKET(bucket, vertices, primitives)
#define PROFILE_DRAW_FINGERPRINT(fingerprint)
#define PROFILE_DRAW_STAGE_US(id, delta)
#define PROFILE_GPU_TIMESTAMPED_DRAW()
#define PROFILE_SCOPE_COUNTER(id)
#define PROFILE_SCOPE_COUNTER_IF(id, active)
#define PROFILE_CMD_BUFFER_STALL()
#define PROFILE_AUDIO_LATENCY_US(value)
#define PROFILE_BUFFER_QUEUE_DEPTH(value)
#define PROFILE_THREAD_CREATED()
#define PROFILE_THREAD_EXITED()
#define PROFILE_APC_QUEUE_DEPTH(value)
#define PROFILE_CRITICAL_REGION_CONTENTION()
#define PROFILE_TEXTURE_CACHE_HIT()
#define PROFILE_TEXTURE_CACHE_MISS()
#define PROFILE_PIPELINE_CACHE_HIT()
#define PROFILE_PIPELINE_CACHE_MISS()

#endif
