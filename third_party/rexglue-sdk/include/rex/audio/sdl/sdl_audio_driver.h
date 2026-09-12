/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#pragma once

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <queue>
#include <stack>

#include <rex/audio/audio_driver.h>
#include <rex/thread.h>

#include <SDL3/SDL.h>

namespace rex::audio::sdl {

class SDLAudioDriver : public AudioDriver {
 public:
  SDLAudioDriver(memory::Memory* memory, rex::thread::Semaphore* semaphore);
  ~SDLAudioDriver() override;

  bool Initialize();
  void SubmitFrame(uint32_t frame_ptr) override;
  void Shutdown();

 protected:
  static void SDLCallback(void* userdata, SDL_AudioStream* stream, int additional_amount,
                          int total_amount);

  // Releases guest frame credits, paced to real time when
  // audio_realtime_credit_pacing is enabled: a misbehaving audio device that
  // drains the stream faster than real time (e.g. a Bluetooth device in a
  // broken state) would otherwise release credits at an unbounded rate,
  // speeding up the guest's whole audio clock and anything paced by it (such
  // as video playback). Called only from the SDL audio thread. Returns the
  // number of credits actually released to the guest.
  uint64_t ReleasePacedCredits(uint32_t new_credits);

  rex::thread::Semaphore* semaphore_ = nullptr;

  SDL_AudioStream* sdl_stream_ = nullptr;
  bool sdl_initialized_ = false;
  uint8_t sdl_device_channels_ = 0;

  static const uint32_t frame_frequency_ = 48000;
  static const uint32_t frame_channels_ = 6;
  static const uint32_t channel_samples_ = 256;
  static const uint32_t frame_samples_ = frame_channels_ * channel_samples_;
  static const uint32_t frame_size_ = sizeof(float) * frame_samples_;
  std::queue<float*> frames_queued_ = {};
  std::stack<float*> frames_unused_ = {};
  std::mutex frames_mutex_ = {};
  // Signaled by SubmitFrame; lets the device callback briefly wait for frames
  // the guest is mixing right now instead of splicing silence when a large
  // device quantum outruns the queue depth.
  std::condition_variable frames_available_cv_ = {};

  // Credit pacing state - only touched on the SDL audio thread. The allowance
  // accumulates with wall time (slightly above real time to tolerate device
  // clock drift) and is capped so pauses don't bank an unbounded burst.
  double pace_allowance_frames_ = 4.0;
  uint64_t pace_last_ns_ = 0;
  uint64_t pace_deferred_credits_ = 0;

  // audio_stats window counters - only touched on the SDL audio thread.
  uint64_t stats_window_start_ns_ = 0;
  uint64_t stats_last_callback_ns_ = 0;
  uint64_t stats_max_callback_gap_ns_ = 0;
  uint32_t stats_callbacks_ = 0;
  uint32_t stats_frames_ = 0;
  uint32_t stats_silence_chunks_ = 0;
  size_t stats_queue_min_ = SIZE_MAX;
  size_t stats_queue_max_ = 0;
};

}  // namespace rex::audio::sdl
