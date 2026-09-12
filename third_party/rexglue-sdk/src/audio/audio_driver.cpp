/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <rex/audio/audio_driver.h>

#include <atomic>

namespace rex::audio {

namespace {
std::atomic<int32_t> g_auto_device_sample_frames{0};
}  // namespace

int32_t AutoDeviceSampleFrames() {
  return g_auto_device_sample_frames.load(std::memory_order_relaxed);
}

void SetAutoDeviceSampleFrames(int32_t sample_frames) {
  g_auto_device_sample_frames.store(sample_frames, std::memory_order_relaxed);
}

AudioDriver::AudioDriver(memory::Memory* memory) : memory_(memory) {}

AudioDriver::~AudioDriver() = default;

}  // namespace rex::audio
