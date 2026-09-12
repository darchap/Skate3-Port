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

#pragma once

#include <rex/kernel.h>
#include <rex/memory.h>

namespace rex::audio {

// Device buffer length (sample frames) the backend chose when
// audio_device_sample_frames ran at 0 (= backend default). 0 while unknown:
// before device init, on init failure, or when an explicit size was
// requested (the device then just reflects the request). UI consumers use
// this to display what "auto" resolves to on this machine; drivers publish
// it at device init.
int32_t AutoDeviceSampleFrames();
void SetAutoDeviceSampleFrames(int32_t sample_frames);

class AudioDriver {
 public:
  explicit AudioDriver(memory::Memory* memory);
  virtual ~AudioDriver();

  virtual void SubmitFrame(uint32_t samples_ptr) = 0;

 protected:
  inline uint8_t* TranslatePhysical(uint32_t guest_address) const {
    return memory_->TranslatePhysical(guest_address);
  }

  memory::Memory* memory_ = nullptr;
};

}  // namespace rex::audio
