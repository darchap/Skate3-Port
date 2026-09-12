/**
 * @file        graphics/nvidia_app_profile.h
 * @brief       NVIDIA driver application-profile setup (power management mode).
 */

#pragma once

namespace rex::graphics {

// Ensures the NVIDIA driver's application profile for the current executable
// requests "Prefer maximum performance" power management. Without it the
// driver parks the GPU in a low P-state during load-screen lulls and, because
// a paced frame loop never pushes utilization over the boost threshold, can
// keep it parked through gameplay (the sticky 40-80 fps state; memory clock
// halves or worse while all GPU timestamp buckets inflate together).
//
// Windows + NVIDIA only; a no-op everywhere else. Never fatal - failures are
// logged and swallowed. Controlled by the nvidia_prefer_max_performance cvar.
void ApplyNvidiaMaxPerformanceProfile();

}  // namespace rex::graphics
