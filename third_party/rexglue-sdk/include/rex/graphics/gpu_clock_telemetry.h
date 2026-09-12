/**
 * @file        graphics/gpu_clock_telemetry.h
 * @brief       Host GPU clock/power-state sampling for performance attribution.
 */

#pragma once

#include <cstdint>

namespace rex::graphics {

// Snapshot of the host GPU's clock/power state, read from the vendor's
// management library (NVML for NVIDIA). Lets logs distinguish "the workload
// grew" from "the same workload executes slower" (all GPU timestamp buckets
// inflating together = a core/memory clock drop the driver never recovered
// from).
struct GpuClockSample {
  bool valid = false;
  uint32_t sm_mhz = 0;
  uint32_t mem_mhz = 0;
  uint32_t gpu_util_percent = 0;
  uint32_t mem_util_percent = 0;
  uint32_t performance_state = 0;  // P-state number; 0 = full performance.
  uint64_t throttle_reasons = 0;   // NVML clocks-throttle-reasons bitmask.
};

// Samples the first vendor GPU. Loads the management library on the first
// call; returns valid=false forever if it is unavailable (non-NVIDIA host,
// library missing). Cheap enough to call a few times per second.
GpuClockSample SampleGpuClocks();

}  // namespace rex::graphics
