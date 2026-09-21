/**
******************************************************************************
* Xenia : Xbox 360 Emulator Research Project                                 *
******************************************************************************
* Copyright 2021 Ben Vanik. All rights reserved.                             *
* Released under the BSD license - see LICENSE in the root for more details. *
******************************************************************************
*
* @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
*/

#include <algorithm>
#include <cstring>
#include <tuple>

#include <rex/audio/xma/context.h>
#include <rex/audio/xma/decoder.h>
#include <rex/audio/xma/helpers.h>
#include <rex/cvar.h>
#include <rex/dbg.h>
#include <rex/logging.h>
#include <rex/memory/ring_buffer.h>
#include <rex/platform.h>
#include <rex/stream.h>

extern "C" {
#if REX_COMPILER_MSVC
#pragma warning(push)
#pragma warning(disable : 4101 4244 5033)
#endif
#include "libavcodec/avcodec.h"
#include "libavutil/error.h"
#if REX_COMPILER_MSVC
#pragma warning(pop)
#endif
}  // extern "C"

// Credits for most of this code goes to:
// https://github.com/koolkdev/libertyv/blob/master/libav_wrapper/xma2dec.c

REXCVAR_DEFINE_BOOL(xma_use_old_decoder, true, "Audio",
                    "Use the xenia-edge old XMA decoder path. This avoids Skate 3 one-shot SFX "
                    "crackle caused by the current subframe/frame assembly path.");

namespace rex::audio {

using stream::BitStream;

const uint32_t XmaContext::kBitsPerPacketHeader;
const uint32_t XmaContext::kOutputMaxSizeBytes;

XmaContext::XmaContext()
    : work_completion_event_(rex::thread::Event::CreateAutoResetEvent(false)) {}

XmaContext::~XmaContext() {
  if (av_context_) {
    avcodec_free_context(&av_context_);
  }
  if (av_frame_) {
    av_frame_free(&av_frame_);
  }
}

int XmaContext::Setup(uint32_t id, memory::Memory* memory, uint32_t guest_ptr) {
  id_ = id;
  memory_ = memory;
  guest_ptr_ = guest_ptr;

  // Allocate ffmpeg stuff:
  av_packet_ = av_packet_alloc();
  assert_not_null(av_packet_);
  av_packet_->buf = av_buffer_alloc(128 * 1024);

  // find the XMA2 audio decoder
  av_codec_ = avcodec_find_decoder(AV_CODEC_ID_XMAFRAMES);
  if (!av_codec_) {
    REXAPU_ERROR("XmaContext {}: Codec not found", id);
    return 1;
  }

  av_context_ = avcodec_alloc_context3(av_codec_);
  if (!av_context_) {
    REXAPU_ERROR("XmaContext {}: Couldn't allocate context", id);
    return 1;
  }

  // Initialize these to 0. They'll actually be set later.
  av_context_->channels = 0;
  av_context_->sample_rate = 0;

  av_frame_ = av_frame_alloc();
  if (!av_frame_) {
    REXAPU_ERROR("XmaContext {}: Couldn't allocate frame", id);
    return 1;
  }

  // FYI: We're purposely not opening the codec here. That is done later.
  return 0;
}

bool XmaContext::Work() {
  // NOTE: do NOT decode ahead of the kick ("eager decode" on cleared
  // contexts was tried and increased mix clicks ~60%): the game commits
  // input_buffer_*_valid BEFORE writing the final input_buffer_read_offset
  // (mid-stream seek values), so decoding early uses a stale offset and
  // plays the wrong region. The kick is the only safe decode trigger.
  if (!is_allocated() || !is_enabled()) {
    return false;
  }

  std::lock_guard<std::mutex> lock(lock_);
  if (REXCVAR_GET(xma_use_old_decoder)) {
    return WorkOldFrameDecoder();
  }

  // Free-running semantics: real XMA hardware keeps an enabled context
  // decoding whenever output space frees up, without further kicks - the
  // enable persists until XMADisableContext/lock. Clearing the enable per
  // Work() made decode strictly kick-driven; when the game consumed output
  // after its kick, the refill waited for the next kick and the voice fetch
  // could find the ring empty -> 5.3ms per-voice dropouts (audible clicks).
  // The worker now polls enabled contexts on a short cadence (see
  // WorkerThreadMain), so keep is_enabled set and report whether progress
  // was made: returning false on no-progress keeps the worker from spinning.
  auto context_ptr = memory()->TranslateVirtual(guest_ptr());
  XMA_CONTEXT_DATA data(context_ptr);
  const XMA_CONTEXT_DATA initial_data = data;

  if (!data.output_buffer_valid) {
    return false;
  }

  memory::RingBuffer output_rb = PrepareOutputRingBuffer(&data);

  // Consume-only context: no input, just drain remaining subframes.
  if (data.IsConsumeOnlyContext()) {
    if (current_frame_remaining_subframes_ == 0) {
      return false;
    }
    Consume(&output_rb, &data);
    data.output_buffer_write_offset = output_rb.write_offset() / kOutputBytesPerBlock;
    StoreContextMerged(data, initial_data, context_ptr);
    return true;
  }

  // Minimum free blocks needed before attempting a decode.
  // Use subframe_decode_count (clamped to 1) instead of full frame size.
  const uint32_t effective_sdc = std::max(static_cast<uint32_t>(1), data.subframe_decode_count);
  const int32_t minimum_subframe_decode_count =
      static_cast<int32_t>(effective_sdc) + data.output_buffer_padding;

  if (minimum_subframe_decode_count > remaining_subframe_blocks_in_output_buffer_) {
    StoreContextMerged(data, initial_data, context_ptr);
    return false;
  }

  while (remaining_subframe_blocks_in_output_buffer_ >= minimum_subframe_decode_count) {
    Decode(&data);
    Consume(&output_rb, &data);

    if (!data.IsAnyInputBufferValid() || data.error_status == 4) {
      break;
    }
  }

  data.output_buffer_write_offset = output_rb.write_offset() / kOutputBytesPerBlock;

  if (output_rb.empty()) {
    data.output_buffer_valid = 0;
  }

  StoreContextMerged(data, initial_data, context_ptr);
  return true;
}

void XmaContext::Enable() {
  std::lock_guard<std::mutex> lock(lock_);
  set_is_enabled(true);
}

bool XmaContext::Block(bool poll) {
  if (!lock_.try_lock()) {
    if (poll) {
      return false;
    }
    lock_.lock();
  }
  lock_.unlock();
  return true;
}

void XmaContext::Clear() {
  std::lock_guard<std::mutex> lock(lock_);
  REXAPU_NOISY_DEBUG("XmaContext: reset context {}", id());

  auto context_ptr = memory()->TranslateVirtual(guest_ptr());
  XMA_CONTEXT_DATA data(context_ptr);
  ClearLocked(&data);
  data.Store(context_ptr);
}

void XmaContext::ClearLocked(XMA_CONTEXT_DATA* data) {
  // The game clears a context to rewind it or start a new sound on it (Skate 3
  // keeps all contexts allocated and multiplexes voices via clear + re-kick).
  // The XMA bitstream is overlap-add: the ffmpeg decoder carries the previous
  // frame's spectral tail, and PrepareDecoder only reopens the codec when the
  // sample rate or channel count changes. Without a flush, the first frame of
  // the next sound on this context gets overlap-added with the tail of the
  // previous one - audible as crunchy/echo-like corruption on one-shot SFX.
  if (av_context_ && avcodec_is_open(av_context_)) {
    avcodec_flush_buffers(av_context_);
  }

  data->input_buffer_0_valid = 0;
  data->input_buffer_1_valid = 0;
  data->output_buffer_valid = 0;

  data->input_buffer_read_offset =
      REXCVAR_GET(xma_use_old_decoder) ? 0 : kBitsPerPacketHeader;
  data->output_buffer_read_offset = 0;
  data->output_buffer_write_offset = 0;
  // NOTE: do NOT zero the output ring contents here. The game clears
  // contexts for reuse while the previous voice's ring tail is still being
  // played out by the mixer fetch - wiping it audibly truncates live audio
  // (measured: mix clicks doubled with a memset here).

  current_frame_remaining_subframes_ = 0;
  loop_frame_output_limit_ = 0;
  loop_start_skip_pending_ = false;
  old_packets_skip_ = 0;
  old_is_stream_done_ = false;
  old_split_frame_len_ = 0;
  old_split_frame_len_partial_ = 0;
  old_split_frame_padding_start_ = 0;
  xma_frame_.fill(0);
}

void XmaContext::Disable() {
  std::lock_guard<std::mutex> lock(lock_);
  set_is_enabled(false);
}

void XmaContext::Release() {
  std::lock_guard<std::mutex> lock(lock_);
  assert_true(is_allocated());

  set_is_allocated(false);
  auto context_ptr = memory()->TranslateVirtual(guest_ptr());
  std::memset(context_ptr, 0, sizeof(XMA_CONTEXT_DATA));
}

void XmaContext::SwapInputBuffer(XMA_CONTEXT_DATA* data) {
  if (data->current_buffer == 0) {
    data->input_buffer_0_valid = 0;
  } else {
    data->input_buffer_1_valid = 0;
  }
  data->current_buffer ^= 1;
  data->input_buffer_read_offset = kBitsPerPacketHeader;
}

void XmaContext::UpdateLoopStatus(XMA_CONTEXT_DATA* data) {
  if (data->loop_count == 0) {
    return;
  }

  const uint32_t loop_start = std::max(kBitsPerPacketHeader, data->loop_start);
  const uint32_t loop_end = std::max(kBitsPerPacketHeader, data->loop_end);

  if (data->input_buffer_read_offset != loop_end) {
    return;
  }

  data->input_buffer_read_offset = loop_start;
  loop_start_skip_pending_ = true;

  if (data->loop_count < 255) {
    data->loop_count--;
  }
}

int XmaContext::GetSampleRate(int id) {
  return kIdToSampleRate[std::min(id, 3)];
}

int16_t XmaContext::GetPacketNumber(size_t size, size_t bit_offset) {
  if (bit_offset < kBitsPerPacketHeader) {
    assert_always();
    return -1;
  }
  if (bit_offset >= (size << 3)) {
    assert_always();
    return -1;
  }
  size_t byte_offset = bit_offset >> 3;
  size_t packet_number = byte_offset / kBytesPerPacket;
  return static_cast<int16_t>(packet_number);
}

uint32_t XmaContext::GetCurrentInputBufferSize(XMA_CONTEXT_DATA* data) {
  return data->GetCurrentInputBufferPacketCount() * kBytesPerPacket;
}

uint8_t* XmaContext::GetCurrentInputBuffer(XMA_CONTEXT_DATA* data) {
  return memory()->TranslatePhysical(data->GetCurrentInputBufferAddress());
}

uint32_t XmaContext::GetAmountOfBitsToRead(uint32_t remaining_stream_bits, uint32_t frame_size) {
  return std::min(remaining_stream_bits, frame_size);
}

kPacketHandle XmaContext::GetPacketHandle(XMA_CONTEXT_DATA* data, uint32_t buffer_index,
                                          uint32_t packet_index,
                                          uint32_t current_input_packet_count) {
  kPacketHandle result{};
  const bool is_packet_in_next_buffer = packet_index >= current_input_packet_count;
  if (is_packet_in_next_buffer) {
    buffer_index ^= 1;
    packet_index -= current_input_packet_count;
  }

  if (!data->IsInputBufferValid(static_cast<uint8_t>(buffer_index))) {
    return result;
  }

  const uint32_t buffer_address = data->GetInputBufferAddress(static_cast<uint8_t>(buffer_index));
  if (!buffer_address) {
    REXAPU_ERROR("XmaContext {}: Buffer marked valid but has null pointer!", id());
    return result;
  }

  const uint32_t packet_count =
      data->GetInputBufferPacketCount(static_cast<uint8_t>(buffer_index));
  if (packet_index >= packet_count) {
    REXAPU_ERROR("XmaContext {}: Packet {} is outside buffer {} packet count {}", id(),
                 packet_index, buffer_index, packet_count);
    return result;
  }

  result.buffer_index_ = buffer_index;
  result.packet_index_ = packet_index;
  result.is_valid_ = true;
  return result;
}

const uint8_t* XmaContext::GetNextPacket(XMA_CONTEXT_DATA* data, uint32_t next_packet_index,
                                         uint32_t current_input_packet_count) {
  const kPacketHandle packet =
      GetPacketHandle(data, data->current_buffer, next_packet_index, current_input_packet_count);
  if (!packet.is_valid_) {
    return nullptr;
  }

  const uint32_t buffer_address =
      data->GetInputBufferAddress(static_cast<uint8_t>(packet.buffer_index_));
  return memory()->TranslatePhysical(buffer_address) + packet.packet_index_ * kBytesPerPacket;
}

uint32_t XmaContext::GetNextPacketReadOffset(uint8_t* buffer, uint32_t next_packet_index,
                                             uint32_t current_input_packet_count) {
  while (next_packet_index < current_input_packet_count) {
    uint8_t* next_packet = buffer + (next_packet_index * kBytesPerPacket);
    const uint32_t packet_frame_offset = xma::GetPacketFrameOffset(next_packet);

    if (packet_frame_offset <= kMaxFrameSizeinBits) {
      return (next_packet_index * kBitsPerPacket) + packet_frame_offset;
    }
    next_packet_index++;
  }

  return kBitsPerPacketHeader;
}

uint32_t XmaContext::GetNextPacketReadOffset(XMA_CONTEXT_DATA* data, uint32_t next_packet_index,
                                             uint32_t current_input_packet_count) {
  const kPacketHandle packet =
      GetPacketHandle(data, data->current_buffer, next_packet_index, current_input_packet_count);
  if (!packet.is_valid_) {
    return kBitsPerPacketHeader;
  }

  const uint32_t buffer_address =
      data->GetInputBufferAddress(static_cast<uint8_t>(packet.buffer_index_));
  return GetNextPacketReadOffset(memory()->TranslatePhysical(buffer_address), packet.packet_index_,
                                 data->GetInputBufferPacketCount(
                                     static_cast<uint8_t>(packet.buffer_index_)));
}

memory::RingBuffer XmaContext::PrepareOutputRingBuffer(XMA_CONTEXT_DATA* data) {
  const uint32_t output_capacity = data->output_buffer_block_count * kOutputBytesPerBlock;
  const uint32_t output_read_offset = data->output_buffer_read_offset * kOutputBytesPerBlock;
  const uint32_t output_write_offset = data->output_buffer_write_offset * kOutputBytesPerBlock;

  if (output_capacity > kOutputMaxSizeBytes) {
    REXAPU_WARN(
        "XmaContext {}: Output buffer exceeds expected size! "
        "(Actual: {} Max: {})",
        id(), output_capacity, kOutputMaxSizeBytes);
  }

  uint8_t* output_buffer = memory()->TranslatePhysical(data->output_buffer_ptr);

  memory::RingBuffer output_rb(output_buffer, output_capacity);
  output_rb.set_read_offset(output_read_offset);
  output_rb.set_write_offset(output_write_offset);
  remaining_subframe_blocks_in_output_buffer_ =
      static_cast<int32_t>(output_rb.write_count()) / kOutputBytesPerBlock;

  return output_rb;
}

kPacketInfo XmaContext::GetPacketInfo(uint8_t* packet, uint32_t frame_offset) {
  kPacketInfo packet_info = {};

  const uint32_t first_frame_offset = xma::GetPacketFrameOffset(packet);
  BitStream stream(packet, kBitsPerPacket);
  stream.SetOffset(first_frame_offset);

  if (frame_offset < first_frame_offset) {
    packet_info.current_frame_ = 0;
    packet_info.current_frame_size_ = first_frame_offset - frame_offset;
  }

  while (true) {
    if (stream.BitsRemaining() < kBitsPerFrameHeader) {
      // A frame whose 15-bit size header straddles the packet boundary still
      // starts in this packet (we only get here following a set continuation
      // bit or the packet's declared first-frame offset). Count it, so the
      // frame before it is not misclassified as last-in-packet - otherwise
      // the advance logic jumps to the next packet's declared offset and
      // silently drops the straddling frame (audible as a click/crunch in
      // one-shot SFX). current_frame_size_ stays 0 for it, which routes
      // Decode() into the existing split-header path.
      if (stream.BitsRemaining() > 0) {
        if (stream.offset_bits() == frame_offset) {
          packet_info.current_frame_ = packet_info.frame_count_;
        }
        packet_info.frame_count_++;
      }
      break;
    }

    const uint64_t frame_size = stream.Peek(kBitsPerFrameHeader);
    if (frame_size == 0 || frame_size == xma::kMaxFrameLength) {
      break;
    }

    if (stream.offset_bits() == frame_offset) {
      packet_info.current_frame_ = packet_info.frame_count_;
      packet_info.current_frame_size_ = static_cast<uint32_t>(frame_size);
    }

    packet_info.frame_count_++;

    if (frame_size > stream.BitsRemaining()) {
      break;
    }

    stream.Advance(frame_size - 1);

    if (stream.Read(1) == 0) {
      break;
    }
  }

  if (xma::IsPacketXma2Type(packet)) {
    const uint8_t xma2_frame_count = xma::GetPacketFrameCount(packet);
    if (xma2_frame_count > packet_info.frame_count_) {
      if (packet_info.current_frame_size_ == 0) {
        packet_info.current_frame_ = packet_info.frame_count_;
      }
      packet_info.frame_count_ = xma2_frame_count;
    }
  }
  return packet_info;
}

void XmaContext::StoreContextMerged(const XMA_CONTEXT_DATA& data,
                                    const XMA_CONTEXT_DATA& initial_data, uint8_t* context_ptr) {
  // The game reads and writes this context concurrently (valid flags, input
  // pointers, output read offset - especially during load-time decode
  // storms). The old whole-struct read-modify-write had a microsecond window
  // where any concurrent game write was silently clobbered, which made the
  // decoder consume stale/wrong buffers and bake garbage frames into sounds
  // the game pre-decodes and caches at level load. Update only decoder-owned
  // bitfields, with a per-dword CAS so game-owned neighbor bits survive.
  // Note: output_buffer_read_offset (dword 9) is game-owned and deliberately
  // never written here.
  const uint32_t* d = reinterpret_cast<const uint32_t*>(&data);

  const auto cas_update = [&](size_t dword_idx, uint32_t mask, uint32_t value) {
    uint32_t* p = reinterpret_cast<uint32_t*>(context_ptr) + dword_idx;
#if REX_PLATFORM_ANDROID
    uint32_t old_be = __atomic_load_n(p, __ATOMIC_RELAXED);
#else
    std::atomic_ref<uint32_t> ref(*p);
    uint32_t old_be = ref.load(std::memory_order_relaxed);
#endif
    while (true) {
      const uint32_t old_host = rex::byte_swap(old_be);
      const uint32_t new_host = (old_host & ~mask) | (value & mask);
      const uint32_t new_be = rex::byte_swap(new_host);
      if (new_be == old_be) {
        return;
      }
      // release: decoded PCM written to the output ring must be visible
      // before the write offset advances.
#if REX_PLATFORM_ANDROID
      if (__atomic_compare_exchange_n(p, &old_be, new_be, true, __ATOMIC_RELEASE,
                                      __ATOMIC_RELAXED)) {
#else
      if (ref.compare_exchange_weak(old_be, new_be, std::memory_order_release,
                                    std::memory_order_relaxed)) {
#endif
        return;
      }
    }
  };

  // DWORD 0: loop_count (bits 12-19), output_buffer_write_offset (27-31);
  // input valid flags (20, 21) are clear-only.
  uint32_t mask0 = 0x000FF000u | 0xF8000000u;
  if (initial_data.input_buffer_0_valid && !data.input_buffer_0_valid) {
    mask0 |= 1u << 20;
  }
  if (initial_data.input_buffer_1_valid && !data.input_buffer_1_valid) {
    mask0 |= 1u << 21;
  }
  cas_update(0, mask0, d[0] & mask0);

  // DWORD 1: output_buffer_valid (bit 31) is clear-only.
  if (initial_data.output_buffer_valid && !data.output_buffer_valid) {
    cas_update(1, 0x80000000u, 0);
  }

  // DWORD 2: input_buffer_read_offset (0-25) + error_status (26-30).
  cas_update(2, 0x7FFFFFFFu, d[2] & 0x7FFFFFFFu);

  // DWORD 3: parser_error_status (26-30) + parser_error_set (31). loop_start shares
  // this dword and is game-owned (XMASetLoopData), so mask to the error bits only.
  cas_update(3, 0xFC000000u, d[3] & 0xFC000000u);

  // DWORD 4: current_buffer (bit 31).
  cas_update(4, 0x80000000u, d[4] & 0x80000000u);
}

void XmaContext::Consume(memory::RingBuffer* output_rb, const XMA_CONTEXT_DATA* data) {
  if (!current_frame_remaining_subframes_) {
    return;
  }

  if (loop_frame_output_limit_ > 0) {
    const uint8_t total_subframes = (kBytesPerFrameChannel / kOutputBytesPerBlock)
                                    << data->is_stereo;
    const uint8_t consumed = total_subframes - current_frame_remaining_subframes_;
    if (consumed >= loop_frame_output_limit_) {
      remaining_subframe_blocks_in_output_buffer_ -= data->output_buffer_padding;
      current_frame_remaining_subframes_ = 0;
      loop_frame_output_limit_ = 0;
      return;
    }
  }

  const uint8_t effective_sdc = std::max(static_cast<uint32_t>(1), data->subframe_decode_count);
  int8_t subframes_to_write = std::min(static_cast<int8_t>(current_frame_remaining_subframes_),
                                       static_cast<int8_t>(effective_sdc));

  if (loop_frame_output_limit_ > 0) {
    const uint8_t total_subframes = (kBytesPerFrameChannel / kOutputBytesPerBlock)
                                    << data->is_stereo;
    const uint8_t consumed = total_subframes - current_frame_remaining_subframes_;
    const int8_t remaining_until_limit = static_cast<int8_t>(loop_frame_output_limit_ - consumed);
    if (subframes_to_write > remaining_until_limit) {
      subframes_to_write = remaining_until_limit;
    }
  }

  const int8_t raw_frame_read_offset =
      ((kBytesPerFrameChannel / kOutputBytesPerBlock) << data->is_stereo) -
      current_frame_remaining_subframes_;

  output_rb->Write(raw_frame_.data() + (kOutputBytesPerBlock * raw_frame_read_offset),
                   subframes_to_write * kOutputBytesPerBlock);

  const int8_t headroom = (current_frame_remaining_subframes_ - subframes_to_write == 0)
                              ? data->output_buffer_padding
                              : 0;

  remaining_subframe_blocks_in_output_buffer_ -= subframes_to_write + headroom;
  current_frame_remaining_subframes_ -= subframes_to_write;
}

int XmaContext::PrepareDecoder(int sample_rate, bool is_two_channel) {
  sample_rate = GetSampleRate(sample_rate);

  uint32_t channels = is_two_channel ? 2 : 1;
  if (av_context_->sample_rate != sample_rate ||
      av_context_->channels != static_cast<int>(channels)) {
    REXAPU_NOISY_DEBUG("XmaContext {}: Codec reinit: rate {} -> {}, channels {} -> {}", id(),
                       av_context_->sample_rate, sample_rate, av_context_->channels, channels);
    avcodec_free_context(&av_context_);
    av_context_ = avcodec_alloc_context3(av_codec_);

    av_context_->sample_rate = sample_rate;
    av_context_->channels = channels;
    av_context_->flags2 |= AV_CODEC_FLAG2_SKIP_MANUAL;

    if (avcodec_open2(av_context_, av_codec_, NULL) < 0) {
      REXAPU_ERROR("XmaContext: Failed to reopen FFmpeg context");
      return -1;
    }
    return 1;
  }
  return 0;
}

void XmaContext::PreparePacket(uint32_t frame_size, uint32_t frame_padding) {
  av_packet_->data = xma_frame_.data();
  av_packet_->size = static_cast<int>(1 + ((frame_padding + frame_size) / 8) +
                                      (((frame_padding + frame_size) % 8) ? 1 : 0));

  auto padding_end = av_packet_->size * 8 - (8 + frame_padding + frame_size);
  assert_true(padding_end < 8);
  xma_frame_[0] = ((frame_padding & 7) << 5) | ((padding_end & 7) << 2);
}

bool XmaContext::DecodePacket(AVCodecContext* av_context, const AVPacket* av_packet,
                              AVFrame* av_frame) {
  auto ret = avcodec_send_packet(av_context, av_packet);
  if (ret < 0) {
    char errbuf[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(ret, errbuf, sizeof(errbuf));
    REXAPU_ERROR("XmaContext {}: Error sending packet for decoding: {} ({})", id(), errbuf, ret);
    return false;
  }
  ret = avcodec_receive_frame(av_context, av_frame);

  if (ret == AVERROR(EAGAIN)) {
    return false;
  }
  if (ret < 0) {
    char errbuf[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(ret, errbuf, sizeof(errbuf));
    REXAPU_ERROR("XmaContext {}: Error during decoding: {} ({})", id(), errbuf, ret);
    return false;
  }
  return true;
}

void XmaContext::Decode(XMA_CONTEXT_DATA* data) {
  SCOPE_profile_cpu_f("apu");

  if (!data->IsAnyInputBufferValid()) {
    return;
  }

  if (current_frame_remaining_subframes_ > 0) {
    return;
  }

  if (!data->IsCurrentInputBufferValid()) {
    SwapInputBuffer(data);
    if (!data->IsCurrentInputBufferValid()) {
      return;
    }
  }

  uint8_t* current_input_buffer = GetCurrentInputBuffer(data);

  input_buffer_.fill(0);

  // Detect loop end frame before UpdateLoopStatus resets the offset.
  bool is_loop_end_frame = false;
  if (data->loop_count > 0) {
    const uint32_t loop_end = std::max(kBitsPerPacketHeader, data->loop_end);
    is_loop_end_frame = (data->input_buffer_read_offset == loop_end);
  }

  UpdateLoopStatus(data);

  if (!data->output_buffer_block_count) {
    REXAPU_ERROR("XmaContext {}: Error - Received 0 for output_buffer_block_count!", id());
    return;
  }

  if (data->input_buffer_read_offset < kBitsPerPacketHeader) {
    data->input_buffer_read_offset = kBitsPerPacketHeader;
  }

  const uint32_t current_input_size = GetCurrentInputBufferSize(data);
  const uint32_t current_input_packet_count = current_input_size / kBytesPerPacket;

  const int16_t packet_index = GetPacketNumber(current_input_size, data->input_buffer_read_offset);

  if (packet_index == -1) {
    REXAPU_ERROR("XmaContext {}: Invalid packet index. Input read offset: {}", id(),
                 static_cast<uint32_t>(data->input_buffer_read_offset));
    return;
  }

  uint8_t* packet = current_input_buffer + (packet_index * kBytesPerPacket);
  // Work on a stable copy: the game's streamer can refill this buffer while
  // we are mid-decode, and slicing decisions made from one read tearing into
  // another produce garbage frames with no error status.
  std::memcpy(packet_snapshot_.data(), packet, kBytesPerPacket);
  packet = packet_snapshot_.data();

  const uint32_t packet_first_frame_offset = xma::GetPacketFrameOffset(packet);
  uint32_t relative_offset = data->input_buffer_read_offset % kBitsPerPacket;

  if (relative_offset < packet_first_frame_offset) {
    data->input_buffer_read_offset = (packet_index * kBitsPerPacket) + packet_first_frame_offset;
    relative_offset = packet_first_frame_offset;
  }

  const uint8_t skip_count = xma::GetPacketSkipCount(packet);

  // Full packet skip (0xFF) -- no new frames begin in this packet.
  if (skip_count == 0xFF) {
    const uint32_t next_packet_index_skip = packet_index + 1;
    uint32_t next_input_offset =
        GetNextPacketReadOffset(data, next_packet_index_skip, current_input_packet_count);
    if (next_packet_index_skip >= current_input_packet_count ||
        next_input_offset == kBitsPerPacketHeader) {
      SwapInputBuffer(data);
    }
    data->input_buffer_read_offset = next_input_offset;
    return;
  }

  kPacketInfo packet_info = GetPacketInfo(packet, relative_offset);
  const uint32_t packet_to_skip = skip_count + 1;
  const uint32_t next_packet_index = packet_index + packet_to_skip;

  // Frame header split across packet boundary.
  if (packet_info.current_frame_size_ == 0) {
    const uint8_t* next_packet = GetNextPacket(data, next_packet_index, current_input_packet_count);
    if (!next_packet) {
      SwapInputBuffer(data);
      return;
    }
    std::memcpy(input_buffer_.data(), packet + kBytesPerPacketHeader, kBytesPerPacketData);
    std::memcpy(input_buffer_.data() + kBytesPerPacketData, next_packet + kBytesPerPacketHeader,
                kBytesPerPacketData);

    BitStream combined(input_buffer_.data(), (kBitsPerPacket - kBitsPerPacketHeader) * 2);
    combined.SetOffset(relative_offset - kBitsPerPacketHeader);

    uint64_t frame_size = combined.Peek(kBitsPerFrameHeader);
    if (frame_size == xma::kMaxFrameLength) {
      data->error_status = 4;
      return;
    }
    packet_info.current_frame_size_ = static_cast<uint32_t>(frame_size);
  }

  BitStream stream(current_input_buffer, (packet_index + 1) * kBitsPerPacket);
  stream.SetOffset(data->input_buffer_read_offset);

  const uint64_t bits_to_copy = GetAmountOfBitsToRead(static_cast<uint32_t>(stream.BitsRemaining()),
                                                      packet_info.current_frame_size_);

  if (bits_to_copy == 0) {
    REXAPU_ERROR("XmaContext {}: There are no bits to copy!", id());
    SwapInputBuffer(data);
    return;
  }

  if (packet_info.isLastFrameInPacket()) {
    if (stream.BitsRemaining() < packet_info.current_frame_size_) {
      const uint8_t* next_packet =
          GetNextPacket(data, next_packet_index, current_input_packet_count);
      if (!next_packet) {
        data->error_status = 4;
        return;
      }
      std::memcpy(input_buffer_.data() + kBytesPerPacketData, next_packet + kBytesPerPacketHeader,
                  kBytesPerPacketData);
    }
  }

  std::memcpy(input_buffer_.data(), packet + kBytesPerPacketHeader, kBytesPerPacketData);

  stream = BitStream(input_buffer_.data(), (kBitsPerPacket - kBitsPerPacketHeader) * 2);
  stream.SetOffset(relative_offset - kBitsPerPacketHeader);

  xma_frame_.fill(0);

  const uint32_t padding_start =
      static_cast<uint8_t>(stream.Copy(xma_frame_.data() + 1, packet_info.current_frame_size_));

  raw_frame_.fill(0);

  PrepareDecoder(data->sample_rate, bool(data->is_stereo));
  PreparePacket(packet_info.current_frame_size_, padding_start);
  if (DecodePacket(av_context_, av_packet_, av_frame_)) {
    ConvertFrame(reinterpret_cast<const uint8_t**>(&av_frame_->data), bool(data->is_stereo),
                 raw_frame_.data());
    current_frame_remaining_subframes_ = 4 << data->is_stereo;

    // Loop end: limit output to subframes 0..loop_subframe_end.
    if (is_loop_end_frame) {
      loop_frame_output_limit_ = (data->loop_subframe_end + 1) << data->is_stereo;
    } else {
      loop_frame_output_limit_ = 0;
    }

    // Loop start: skip leading subframes per loop_subframe_skip.
    if (loop_start_skip_pending_) {
      const uint8_t skip = data->loop_subframe_skip << data->is_stereo;
      if (skip < current_frame_remaining_subframes_) {
        current_frame_remaining_subframes_ -= skip;
      }
      loop_start_skip_pending_ = false;
    }
  }

  // Compute where to go next.
  if (!packet_info.isLastFrameInPacket()) {
    const uint32_t next_frame_offset =
        (data->input_buffer_read_offset + bits_to_copy) % kBitsPerPacket;
    data->input_buffer_read_offset = (packet_index * kBitsPerPacket) + next_frame_offset;
    return;
  }

  uint32_t next_input_offset =
      GetNextPacketReadOffset(data, next_packet_index, current_input_packet_count);

  if (next_packet_index >= current_input_packet_count ||
      next_input_offset == kBitsPerPacketHeader) {
    SwapInputBuffer(data);
  }

  if (next_input_offset == kBitsPerPacketHeader) {
    if (data->IsAnyInputBufferValid()) {
      next_input_offset = xma::GetPacketFrameOffset(
          memory()->TranslatePhysical(data->GetCurrentInputBufferAddress()));

      if (next_input_offset > kMaxFrameSizeinBits) {
        SwapInputBuffer(data);
        return;
      }
    }
  }
  data->input_buffer_read_offset = next_input_offset;
}

bool XmaContext::WorkOldFrameDecoder() {
  set_is_enabled(false);

  auto context_ptr = memory()->TranslateVirtual(guest_ptr());
  XMA_CONTEXT_DATA data(context_ptr);
  const XMA_CONTEXT_DATA initial_data = data;
  DecodeOldFrame(&data);
  StoreContextMerged(data, initial_data, context_ptr);
  return true;
}

bool XmaContext::TrySetupNextLoopOld(XMA_CONTEXT_DATA* data,
                                     bool ignore_input_buffer_offset) {
  if (data->loop_count > 0 && data->loop_start < data->loop_end &&
      (ignore_input_buffer_offset || data->input_buffer_read_offset >= data->loop_end)) {
    // loop_start is a stream offset; with single-packet double-buffering it is
    // routinely past the mapped buffer, and applying it trips the range check.
    const uint32_t packet_count = data->current_buffer ? data->input_buffer_1_packet_count
                                                       : data->input_buffer_0_packet_count;
    const uint32_t size_bits = packet_count * kBytesPerPacket * 8;
    if (size_bits && data->loop_start >= size_bits) {
      return false;
    }
    data->input_buffer_read_offset = data->loop_start;
    if (data->loop_count < 255) {
      data->loop_count--;
    }
    return true;
  }
  return false;
}

bool XmaContext::ValidFrameOffsetOld(uint8_t* block, size_t size_bytes,
                                     size_t frame_offset_bits) {
  const int packet_num = GetFramePacketNumberOld(block, size_bytes, frame_offset_bits);
  if (packet_num < 0) {
    return false;
  }

  uint8_t* packet = block + (packet_num * kBytesPerPacket);
  const size_t relative_offset_bits = frame_offset_bits % kBitsPerPacket;
  const uint32_t first_frame_offset = xma::GetPacketFrameOffset(packet);
  if (first_frame_offset > kBitsPerPacket) {
    return false;
  }

  BitStream stream(packet, kBitsPerPacket);
  stream.SetOffset(first_frame_offset);
  while (true) {
    if (stream.offset_bits() == relative_offset_bits) {
      return true;
    }
    if (stream.BitsRemaining() < kBitsPerFrameHeader) {
      return false;
    }

    const uint64_t size = stream.Read(kBitsPerFrameHeader);
    if (size == xma::kMaxFrameLength || size < kBitsPerFrameHeader + 1 ||
        (size - kBitsPerFrameHeader) > stream.BitsRemaining()) {
      return false;
    }

    stream.Advance(size - (kBitsPerFrameHeader + 1));
    if (stream.Read(1) == 0) {
      break;
    }
  }
  return false;
}

size_t XmaContext::GetNextFrameOld(uint8_t* block, size_t size_bytes, size_t bit_offset) {
  const int packet_idx = GetFramePacketNumberOld(block, size_bytes, bit_offset);
  if (packet_idx < 0) {
    return 0;
  }

  BitStream stream(block, size_bytes * 8);
  stream.SetOffset(bit_offset);
  if (stream.BitsRemaining() < kBitsPerFrameHeader) {
    return 0;
  }

  const uint64_t len = stream.Read(kBitsPerFrameHeader);
  if (len >= xma::kMaxFrameLength || len < kBitsPerFrameHeader + 1 ||
      (len - kBitsPerFrameHeader) > stream.BitsRemaining()) {
    return 0;
  }

  stream.Advance(len - (kBitsPerFrameHeader + 1));
  if (stream.Read(1) == 0) {
    return 0;
  }

  bit_offset += len;
  if (packet_idx < GetFramePacketNumberOld(block, size_bytes, bit_offset)) {
    return 0;
  }
  return bit_offset;
}

int XmaContext::GetFramePacketNumberOld(uint8_t* block, size_t size_bytes,
                                        size_t bit_offset) {
  (void)block;
  const size_t size_bits = size_bytes * 8;
  if (bit_offset >= size_bits) {
    return -1;
  }
  return static_cast<int>((bit_offset >> 3) / kBytesPerPacket);
}

std::tuple<int, int> XmaContext::GetFrameNumberOld(uint8_t* block, size_t size_bytes,
                                                   size_t bit_offset) {
  const int packet_idx = GetFramePacketNumberOld(block, size_bytes, bit_offset);
  if (packet_idx < 0 || (static_cast<size_t>(packet_idx) + 1) * kBytesPerPacket > size_bytes) {
    return {packet_idx, -2};
  }
  if (bit_offset == 0) {
    return {packet_idx, -1};
  }

  uint8_t* packet = block + (packet_idx * kBytesPerPacket);
  const uint32_t first_frame_offset = xma::GetPacketFrameOffset(packet);
  if (first_frame_offset > kBitsPerPacket) {
    return {packet_idx, -2};
  }

  BitStream stream(block, size_bytes * 8);
  stream.SetOffset(packet_idx * kBitsPerPacket + first_frame_offset);

  int frame_idx = 0;
  while (true) {
    if (stream.BitsRemaining() < kBitsPerFrameHeader || stream.offset_bits() == bit_offset) {
      break;
    }

    const uint64_t size = stream.Read(kBitsPerFrameHeader);
    if (size == xma::kMaxFrameLength || size < kBitsPerFrameHeader + 1 ||
        (size - kBitsPerFrameHeader) > stream.BitsRemaining()) {
      break;
    }

    stream.Advance(size - (kBitsPerFrameHeader + 1));
    if (stream.Read(1) == 0) {
      break;
    }
    frame_idx++;
  }
  return {packet_idx, frame_idx};
}

std::tuple<int, bool> XmaContext::GetPacketFrameCountOld(uint8_t* packet) {
  const uint32_t first_frame_offset = xma::GetPacketFrameOffset(packet);
  if (first_frame_offset > kBitsPerPacket - kBitsPerPacketHeader) {
    return {0, false};
  }

  BitStream stream(packet, kBitsPerPacket);
  stream.SetOffset(first_frame_offset);
  int frame_count = 0;
  while (true) {
    if (stream.BitsRemaining() < kBitsPerFrameHeader) {
      return {frame_count, false};
    }

    frame_count++;
    const uint64_t size = stream.Read(kBitsPerFrameHeader);
    if (size == xma::kMaxFrameLength || size < kBitsPerFrameHeader + 1 ||
        (size - kBitsPerFrameHeader) > stream.BitsRemaining()) {
      return {frame_count, true};
    }

    stream.Advance(size - (kBitsPerFrameHeader + 1));
    if (stream.Read(1) == 0 || !stream.BitsRemaining()) {
      return {frame_count, false};
    }
  }
}

uint32_t XmaContext::GetPacketFirstFrameOffsetOld(const XMA_CONTEXT_DATA* data) {
  uint8_t* current_input_buffer =
      data->IsCurrentInputBufferValid()
          ? memory()->TranslatePhysical(data->GetCurrentInputBufferAddress())
          : nullptr;
  return current_input_buffer ? xma::GetPacketFrameOffset(current_input_buffer)
                              : kBitsPerPacketHeader;
}

void XmaContext::DecodeOldFrame(XMA_CONTEXT_DATA* data) {
  SCOPE_profile_cpu_f("apu");

  if (!data->output_buffer_valid || !data->IsAnyInputBufferValid()) {
    return;
  }

  uint8_t* in0 = data->input_buffer_0_valid
                     ? memory()->TranslatePhysical(data->input_buffer_0_ptr)
                     : nullptr;
  uint8_t* in1 = data->input_buffer_1_valid
                     ? memory()->TranslatePhysical(data->input_buffer_1_ptr)
                     : nullptr;
  uint8_t* current_input_buffer = data->current_buffer ? in1 : in0;
  if (!current_input_buffer) {
    REXAPU_ERROR("XmaContext {} old: input buffer pointer is invalid", id());
    return;
  }

  if (!data->output_buffer_block_count) {
    REXAPU_ERROR("XmaContext {} old: output_buffer_block_count is 0", id());
    return;
  }

  if (old_is_stream_done_) {
    old_is_stream_done_ = false;
    old_packets_skip_ = 0;
    SwapInputBuffer(data);
    return;
  }

  const size_t input_buffer_0_size = data->input_buffer_0_packet_count * kBytesPerPacket;
  const size_t input_buffer_1_size = data->input_buffer_1_packet_count * kBytesPerPacket;
  const size_t current_input_size = data->current_buffer ? input_buffer_1_size
                                                        : input_buffer_0_size;
  const size_t current_input_packet_count = current_input_size / kBytesPerPacket;
  const bool is_streaming =
      data->input_buffer_0_packet_count == 1 && data->input_buffer_1_packet_count == 1;

  uint8_t* output_buffer = memory()->TranslatePhysical(data->output_buffer_ptr);
  const uint32_t output_capacity = data->output_buffer_block_count * kOutputBytesPerBlock;
  const uint32_t output_read_offset = data->output_buffer_read_offset * kOutputBytesPerBlock;
  const uint32_t output_write_offset = data->output_buffer_write_offset * kOutputBytesPerBlock;

  memory::RingBuffer output_rb(output_buffer, output_capacity);
  output_rb.set_read_offset(output_read_offset);
  output_rb.set_write_offset(output_write_offset);

  const size_t frame_byte_count = size_t(kBytesPerFrameChannel) << data->is_stereo;
  size_t output_remaining_bytes = output_rb.write_count();
  output_remaining_bytes -= output_remaining_bytes % frame_byte_count;

  while (output_remaining_bytes > 0) {
    if (!data->IsAnyInputBufferValid()) {
      break;
    }

    bool reuse_input_buffer = TrySetupNextLoopOld(data, false);
    int packet_idx = -1;
    int frame_idx = -1;
    int frame_count = 0;
    uint8_t* packet = nullptr;
    bool frame_last_split = false;

    BitStream stream(current_input_buffer, current_input_size * 8);
    stream.SetOffset(data->input_buffer_read_offset);

    // >= not >: an offset exactly on the end is a consumed buffer; > let it reach a
    // branch that bailed without swapping and stalled the voice.
    const uint32_t input_size_bits = current_input_size * 8;
    if (data->input_buffer_read_offset >= input_size_bits) {
      REXAPU_NOISY_DEBUG("XmaContext {} old: input buffer consumed ({} >= {}), swapping", id(),
                         uint32_t(data->input_buffer_read_offset), input_size_bits);
      SwapInputBuffer(data);
      if (is_streaming) {
        data->input_buffer_read_offset = GetPacketFirstFrameOffsetOld(data);
      }
      return;
    }

    if (old_packets_skip_ > 0) {
      packet_idx =
          GetFramePacketNumberOld(current_input_buffer, current_input_size,
                                  data->input_buffer_read_offset);
      if (packet_idx < 0) {
        return;
      }
      while (old_packets_skip_ > 0) {
        old_packets_skip_--;
        packet_idx++;
        if (static_cast<size_t>(packet_idx) >= current_input_packet_count) {
          if (!reuse_input_buffer) {
            reuse_input_buffer = TrySetupNextLoopOld(data, true);
          }
          if (!reuse_input_buffer) {
            if (is_streaming) {
              SwapInputBuffer(data);
            } else {
              old_is_stream_done_ = true;
            }
          }
          return;
        }
      }
      data->input_buffer_read_offset = packet_idx * kBitsPerPacket;
    }

    if (old_split_frame_len_) {
      packet_idx =
          GetFramePacketNumberOld(current_input_buffer, current_input_size,
                                  data->input_buffer_read_offset);
      if (packet_idx < 0) {
        return;
      }
      packet = current_input_buffer + packet_idx * kBytesPerPacket;
      std::tie(frame_count, frame_last_split) = GetPacketFrameCountOld(packet);
      frame_idx = -1;

      stream = BitStream(current_input_buffer, (packet_idx + 1) * kBitsPerPacket);
      stream.SetOffset(packet_idx * kBitsPerPacket + kBitsPerPacketHeader);

      if (old_split_frame_len_ > xma::kMaxFrameLength) {
        const auto offset = stream.offset_bits();
        stream.Copy(xma_frame_.data() + 1 +
                        ((old_split_frame_len_partial_ + old_split_frame_padding_start_) / 8),
                    kBitsPerFrameHeader - old_split_frame_len_partial_);
        stream.SetOffset(offset);
        BitStream slen(xma_frame_.data() + 1,
                       kBitsPerFrameHeader + old_split_frame_padding_start_);
        slen.Advance(old_split_frame_padding_start_);
        old_split_frame_len_ = static_cast<uint32_t>(slen.Read(kBitsPerFrameHeader));
      }

      stream.Copy(xma_frame_.data() + 1 +
                      ((old_split_frame_len_partial_ + old_split_frame_padding_start_) / 8),
                  old_split_frame_len_ - old_split_frame_len_partial_);
    } else {
      if (data->input_buffer_read_offset % kBitsPerPacket == 0) {
        const int packet_number =
            GetFramePacketNumberOld(current_input_buffer, current_input_size,
                                    data->input_buffer_read_offset);
        if (packet_number < 0) {
          return;
        }

        const uint32_t first_frame_offset =
            xma::GetPacketFrameOffset(current_input_buffer + kBytesPerPacket * packet_number);
        if (first_frame_offset > kBitsPerPacket) {
          SwapInputBuffer(data);
          return;
        }
        data->input_buffer_read_offset += first_frame_offset;
      }

      if (!ValidFrameOffsetOld(current_input_buffer, current_input_size,
                               data->input_buffer_read_offset)) {
        REXAPU_NOISY_DEBUG("XmaContext {} old: invalid read offset {}", id(),
                           uint32_t(data->input_buffer_read_offset));
        SwapInputBuffer(data);
        return;
      }

      std::tie(packet_idx, frame_idx) =
          GetFrameNumberOld(current_input_buffer, current_input_size,
                            data->input_buffer_read_offset);
      if (packet_idx < 0 || frame_idx < 0) {
        return;
      }
      packet = current_input_buffer + packet_idx * kBytesPerPacket;
      std::tie(frame_count, frame_last_split) = GetPacketFrameCountOld(packet);
      if (frame_count < 0) {
        return;
      }

      PrepareDecoder(data->sample_rate, bool(data->is_stereo));

      bool frame_is_split = frame_last_split && (frame_idx >= frame_count - 1);
      stream = BitStream(current_input_buffer, (packet_idx + 1) * kBitsPerPacket);
      stream.SetOffset(data->input_buffer_read_offset);
      old_split_frame_len_partial_ = static_cast<uint32_t>(stream.BitsRemaining());
      if (old_split_frame_len_partial_ >= kBitsPerFrameHeader) {
        old_split_frame_len_ = static_cast<uint32_t>(stream.Peek(kBitsPerFrameHeader));
      } else {
        old_split_frame_len_ = xma::kMaxFrameLength + 1;
      }

      // A frame that does not fit the packet remainder is split; the frame-count
      // scan misses a header that straddles the packet boundary.
      if (old_split_frame_len_ > old_split_frame_len_partial_) {
        frame_is_split = true;
      }

      xma_frame_.fill(0);
      const uint32_t bits_to_copy = std::min(old_split_frame_len_, old_split_frame_len_partial_);
      old_split_frame_padding_start_ =
          static_cast<uint8_t>(stream.Copy(xma_frame_.data() + 1, bits_to_copy));

      if (frame_is_split) {
        old_packets_skip_ = xma::GetPacketSkipCount(packet) + 1;
        while (old_packets_skip_ > 0) {
          old_packets_skip_--;
          packet_idx++;
          if (static_cast<size_t>(packet_idx) >= current_input_packet_count) {
            if (!reuse_input_buffer) {
              reuse_input_buffer = TrySetupNextLoopOld(data, true);
            }
            if (!reuse_input_buffer) {
              if (is_streaming) {
                SwapInputBuffer(data);
              } else {
                old_is_stream_done_ = true;
              }
            }
            return;
          }
        }
        data->input_buffer_read_offset = packet_idx * kBitsPerPacket;
        continue;
      }
    }

    av_packet_->data = xma_frame_.data();
    av_packet_->size = static_cast<int>(
        1 + ((old_split_frame_padding_start_ + old_split_frame_len_) / 8) +
        (((old_split_frame_padding_start_ + old_split_frame_len_) % 8) ? 1 : 0));
    const auto padding_end =
        av_packet_->size * 8 - (8 + old_split_frame_padding_start_ + old_split_frame_len_);
    if (padding_end >= 8) {
      return;
    }
    xma_frame_[0] =
        ((old_split_frame_padding_start_ & 7) << 5) | ((padding_end & 7) << 2);

    old_split_frame_len_ = 0;
    old_split_frame_len_partial_ = 0;
    old_split_frame_padding_start_ = 0;

    int ret = avcodec_send_packet(av_context_, av_packet_);
    if (ret < 0) {
      return;
    }
    ret = avcodec_receive_frame(av_context_, av_frame_);
    if (ret == AVERROR(EAGAIN)) {
      return;
    }
    if (ret < 0) {
      char errbuf[AV_ERROR_MAX_STRING_SIZE];
      av_strerror(ret, errbuf, sizeof(errbuf));
      REXAPU_ERROR("XmaContext {} old: decoding failed: {} ({})", id(), errbuf, ret);
      data->parser_error_status = 4;
      SwapInputBuffer(data);
      return;
    }

    ConvertFrame(reinterpret_cast<const uint8_t**>(&av_frame_->data), bool(data->is_stereo),
                 raw_frame_.data());

    output_rb.Write(raw_frame_.data(), frame_byte_count);
    output_remaining_bytes -= frame_byte_count;
    data->output_buffer_write_offset = output_rb.write_offset() / kOutputBytesPerBlock;

    uint32_t offset = std::max(kBitsPerPacketHeader, data->input_buffer_read_offset);
    offset = static_cast<uint32_t>(
        GetNextFrameOld(current_input_buffer, current_input_size, offset));

    if (frame_idx + 1 >= frame_count) {
      old_packets_skip_ = xma::GetPacketSkipCount(packet) + 1;
      while (old_packets_skip_ > 0) {
        old_packets_skip_--;
        packet_idx++;
        if (static_cast<size_t>(packet_idx) >= current_input_packet_count) {
          if (!reuse_input_buffer) {
            reuse_input_buffer = TrySetupNextLoopOld(data, true);
          }
          if (!reuse_input_buffer) {
            if (is_streaming) {
              SwapInputBuffer(data);
              data->input_buffer_read_offset = GetPacketFirstFrameOffsetOld(data);
            } else {
              old_is_stream_done_ = true;
            }
            if (output_rb.write_offset() == output_rb.read_offset()) {
              data->output_buffer_valid = 0;
            }
          }
          return;
        }
      }
      packet = current_input_buffer + packet_idx * kBytesPerPacket;
      offset = xma::GetPacketFrameOffset(packet) + packet_idx * kBitsPerPacket;
    }

    if (offset == 0 || frame_idx == -1) {
      if (static_cast<size_t>(packet_idx) >= current_input_packet_count) {
        if (!reuse_input_buffer) {
          reuse_input_buffer = TrySetupNextLoopOld(data, true);
        }
        if (!reuse_input_buffer) {
          if (is_streaming) {
            SwapInputBuffer(data);
          } else {
            old_is_stream_done_ = true;
          }
        }
        break;
      }
      offset = xma::GetPacketFrameOffset(packet) + packet_idx * kBitsPerPacket;
    }

    if (offset <= data->input_buffer_read_offset) {
      REXAPU_NOISY_DEBUG("XmaContext {} old: non-advancing offset {} <= {}", id(), offset,
                         uint32_t(data->input_buffer_read_offset));
      return;
    }

    // GetNextFrameOld can return one packet header past the buffer end; storing
    // that trips the range check next call and swaps mid-stream.
    if (offset >= current_input_size * 8) {
      if (!reuse_input_buffer) {
        reuse_input_buffer = TrySetupNextLoopOld(data, true);
      }
      if (!reuse_input_buffer) {
        if (is_streaming) {
          SwapInputBuffer(data);
          data->input_buffer_read_offset = GetPacketFirstFrameOffsetOld(data);
        } else {
          old_is_stream_done_ = true;
        }
      }
      break;
    }

    data->input_buffer_read_offset = offset;
  }

  if (output_rb.write_offset() == output_rb.read_offset()) {
    data->output_buffer_valid = 0;
  }
}

void XmaContext::ConvertFrame(const uint8_t** samples, bool is_two_channel,
                              uint8_t* output_buffer) {
  // Loop through every sample, convert and drop it into the output array.
  // If more than one channel, we need to interleave the samples from each
  // channel next to each other. Always saturate because FFmpeg output is
  // not limited to [-1, 1] (for example 1.095 as seen in 5454082B).
  constexpr float scale = (1 << 15) - 1;
  auto out = reinterpret_cast<int16_t*>(output_buffer);

  // For testing of vectorized versions, stereo audio is common in 4D5307E6,
  // since the first menu frame; the intro cutscene also has more than 2
  // channels.
#if REX_ARCH_AMD64
  static_assert(kSamplesPerFrame % 8 == 0);
  const auto in_channel_0 = reinterpret_cast<const float*>(samples[0]);
  const __m128 scale_mm = _mm_set1_ps(scale);
  if (is_two_channel) {
    const auto in_channel_1 = reinterpret_cast<const float*>(samples[1]);
    const __m128i shufmask = _mm_set_epi8(14, 15, 6, 7, 12, 13, 4, 5, 10, 11, 2, 3, 8, 9, 0, 1);
    for (uint32_t i = 0; i < kSamplesPerFrame; i += 4) {
      // Load 8 samples, 4 for each channel.
      __m128 in_mm0 = _mm_loadu_ps(&in_channel_0[i]);
      __m128 in_mm1 = _mm_loadu_ps(&in_channel_1[i]);
      // Rescale.
      in_mm0 = _mm_mul_ps(in_mm0, scale_mm);
      in_mm1 = _mm_mul_ps(in_mm1, scale_mm);
      // Cast to int32.
      __m128i out_mm0 = _mm_cvtps_epi32(in_mm0);
      __m128i out_mm1 = _mm_cvtps_epi32(in_mm1);
      // Saturated cast and pack to int16.
      __m128i out_mm = _mm_packs_epi32(out_mm0, out_mm1);
      // Interleave channels and byte swap.
      out_mm = _mm_shuffle_epi8(out_mm, shufmask);
      // Store, as [out + i * 4] movdqu.
      _mm_storeu_si128(reinterpret_cast<__m128i*>(&out[i * 2]), out_mm);
    }
  } else {
    const __m128i shufmask = _mm_set_epi8(14, 15, 12, 13, 10, 11, 8, 9, 6, 7, 4, 5, 2, 3, 0, 1);
    for (uint32_t i = 0; i < kSamplesPerFrame; i += 8) {
      // Load 8 samples, as [in_channel_0 + i * 4] and
      // [in_channel_0 + i * 4 + 16] movups.
      __m128 in_mm0 = _mm_loadu_ps(&in_channel_0[i]);
      __m128 in_mm1 = _mm_loadu_ps(&in_channel_0[i + 4]);
      // Rescale.
      in_mm0 = _mm_mul_ps(in_mm0, scale_mm);
      in_mm1 = _mm_mul_ps(in_mm1, scale_mm);
      // Cast to int32.
      __m128i out_mm0 = _mm_cvtps_epi32(in_mm0);
      __m128i out_mm1 = _mm_cvtps_epi32(in_mm1);
      // Saturated cast and pack to int16.
      __m128i out_mm = _mm_packs_epi32(out_mm0, out_mm1);
      // Byte swap.
      out_mm = _mm_shuffle_epi8(out_mm, shufmask);
      // Store, as [out + i * 2] movdqu.
      _mm_storeu_si128(reinterpret_cast<__m128i*>(&out[i]), out_mm);
    }
  }
#else
  uint32_t o = 0;
  for (uint32_t i = 0; i < kSamplesPerFrame; i++) {
    for (uint32_t j = 0; j <= uint32_t(is_two_channel); j++) {
      // Select the appropriate array based on the current channel.
      auto in = reinterpret_cast<const float*>(samples[j]);

      // Raw samples sometimes aren't within [-1, 1]
      float scaled_sample = rex::clamp_float(in[i], -1.0f, 1.0f) * scale;

      // Convert the sample and output it in big endian.
      auto sample = static_cast<int16_t>(scaled_sample);
      out[o++] = rex::byte_swap(sample);
    }
  }
#endif
}

}  // namespace rex::audio
