/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#version 460

// Compute shader to downscale scaled resolve buffer data back to 1x resolution.
// Operates on 32x32 tiled data format used by Xbox 360.
// Each thread handles one output pixel (one 32x32 tile = 1024 threads).
//
// Scaled resolve buffer layout (established from the resolve write shaders,
// e.g. resolve_fast_32bpp_1x2xmsaa_scaled, and the scaled texture load
// shaders; they must agree, and both use this):
//
//   scaled_byte_address = guest_unit_byte_address * (scale_x * scale_y)
//                       + (sub_x * scale_y + sub_y) * 16
//                       + byte_within_scaled_unit
//
// where a "unit" is a 16-byte span of the guest tiled layout, holding
// W = 16 >> pixel_size_log2 texels that are consecutive along X for
// 16/32/64bpp (a property of the Xbox 360 tiled address function's low
// 4 bits); sub_x selects among the scale_x duplicated 16-byte blocks the
// unit expands to along X of the SCALED image, and sub_y among the scale_y
// duplicated rows. Note the (x-major, y-minor) sub ordering and the 16-BYTE
// duplication granularity: the scaled image texel for guest texel t of a
// unit, at sub-position (ox, oy) within its scale_x * scale_y block, is
//
//   q = scale_x * t + ox;  sub_x = q / W;  texel_in_scaled_unit = q % W.
//
// (The previous version of this shader assumed per-PIXEL contiguous
// duplication with y-major sub ordering, which sampled alternating wrong
// rows/columns and produced a sawtooth weave in every readback, visible in
// e.g. the Skate 3 photo grab.)
//
// For 8bpp the guest 16-byte unit is not purely consecutive along X (it
// spans an extra Y bit), so the X mapping below is approximate for half of
// the bytes there; no current readback consumer uses 8bpp surfaces.
//
// By default, picks the top-left host pixel of each scale_x * scale_y block.
// When xe_downscale_half_pixel_offset is set, samples from
// (scale_x/2, scale_y/2) within each block to compensate for the D3D9-style
// half-pixel offset becoming a multi-pixel offset at higher resolutions.

layout(local_size_x = 32, local_size_y = 32, local_size_z = 1) in;

layout(push_constant) uniform ResolveDownscaleConstants {
  uint xe_downscale_scale_x;           // 1 to kMaxDrawResolutionScaleAlongAxis
  uint xe_downscale_scale_y;           // 1 to kMaxDrawResolutionScaleAlongAxis
  uint xe_downscale_pixel_size_log2;   // 0=8bit, 1=16bit, 2=32bit, 3=64bit
  uint xe_downscale_tile_count;        // Number of 32x32 tiles to process
  uint xe_downscale_source_offset_bytes;   // Byte offset into source buffer
  // When non-zero, apply half-pixel offset correction by sampling from
  // (scale_x/2, scale_y/2) within each scaled block instead of (0, 0).
  uint xe_downscale_half_pixel_offset;
};

// Source buffer (scaled resolve data)
layout(std430, set = 0, binding = 0) readonly buffer SourceBuffer {
  uint data[];
} xe_resolve_source;

// Destination buffer (1x resolution)
layout(std430, set = 0, binding = 1) writeonly buffer DestBuffer {
  uint data[];
} xe_resolve_dest;

// Groupshared memory for coalescing sub-32-bit writes
shared uint gs_tile_data[32 * 32];

void main() {
  uint tile_index = gl_WorkGroupID.x;

  // Early out if beyond tile count
  if (tile_index >= xe_downscale_tile_count) {
    return;
  }

  uint row = gl_LocalInvocationID.y;
  uint column = gl_LocalInvocationID.x;
  uint pixel_index = row * 32u + column;  // 0-1023

  uint pixel_size = 1u << xe_downscale_pixel_size_log2;
  uint tile_size_1x = 32u * 32u * pixel_size;
  uint scale_xy = xe_downscale_scale_x * xe_downscale_scale_y;

  // Sub-position within each scale_x * scale_y block to sample from.
  uint offset_x = 0u;
  uint offset_y = 0u;
  if (xe_downscale_half_pixel_offset != 0u && scale_xy > 1u) {
    offset_x = xe_downscale_scale_x >> 1u;
    offset_y = xe_downscale_scale_y >> 1u;
  }

  // Guest-relative tiled byte address of this output pixel. Both the source
  // range (scaled by scale_xy) and the destination range start at the same
  // guest address, so all addressing below is range-relative.
  uint guest_byte_offset = tile_index * tile_size_1x + pixel_index * pixel_size;
  // Its 16-byte unit and texel index within the unit.
  uint unit = guest_byte_offset >> 4u;
  uint texel_in_unit = (guest_byte_offset & 15u) >> xe_downscale_pixel_size_log2;
  // Representative scaled texel within the unit's (W * scale_x)-texel span.
  uint unit_texels_log2 = 4u - xe_downscale_pixel_size_log2;  // W = 16 / pixel_size
  uint q = xe_downscale_scale_x * texel_in_unit + offset_x;
  uint sub_x = q >> unit_texels_log2;
  uint texel_in_scaled_unit = q & ((1u << unit_texels_log2) - 1u);

  uint src_byte_offset = xe_downscale_source_offset_bytes +
                         unit * (scale_xy << 4u) +
                         (sub_x * xe_downscale_scale_y + offset_y) * 16u +
                         (texel_in_scaled_unit << xe_downscale_pixel_size_log2);
  uint src_offset = src_byte_offset >> 2u;

  // Destination offset in 1x buffer
  uint dst_byte_offset = guest_byte_offset;
  uint dst_offset = dst_byte_offset >> 2u;

  // Copy pixel based on size
  switch (xe_downscale_pixel_size_log2) {
    case 0u: {  // 8-bit - use groupshared to coalesce 4 bytes into 32-bit writes
      // Load the byte value
      uint src_word = xe_resolve_source.data[src_offset];
      uint byte_val = (src_word >> ((src_byte_offset & 3u) * 8u)) & 0xFFu;

      // Each group of 4 threads packs into one uint
      uint pack_index = pixel_index >> 2u;   // Which uint (0-255)
      uint byte_pos = pixel_index & 3u;      // Which byte in uint (0-3)

      // Pack byte into shared memory
      uint contribution = byte_val << (byte_pos * 8u);
      if (byte_pos == 0u) {
        gs_tile_data[pack_index] = contribution;
      }
      barrier();
      if (byte_pos != 0u) {
        atomicOr(gs_tile_data[pack_index], contribution);
      }
      barrier();

      // First thread of each 4 writes the packed uint
      if (byte_pos == 0u) {
        xe_resolve_dest.data[tile_index * (32u * 32u / 4u) + pack_index] =
            gs_tile_data[pack_index];
      }
      break;
    }
    case 1u: {  // 16-bit - use groupshared to coalesce 2 shorts into 32-bit writes
      // Load the short value
      uint src_word = xe_resolve_source.data[src_offset];
      uint short_val = (src_word >> ((src_byte_offset & 2u) * 8u)) & 0xFFFFu;

      // Each group of 2 threads packs into one uint
      uint pack_index = pixel_index >> 1u;   // Which uint (0-511)
      uint short_pos = pixel_index & 1u;     // Which short in uint (0-1)

      // Pack short into shared memory
      uint contribution = short_val << (short_pos * 16u);
      if (short_pos == 0u) {
        gs_tile_data[pack_index] = contribution;
      }
      barrier();
      if (short_pos != 0u) {
        atomicOr(gs_tile_data[pack_index], contribution);
      }
      barrier();

      // First thread of each 2 writes the packed uint
      if (short_pos == 0u) {
        xe_resolve_dest.data[tile_index * (32u * 32u * 2u / 4u) + pack_index] =
            gs_tile_data[pack_index];
      }
      break;
    }
    case 2u: {  // 32-bit - direct copy
      xe_resolve_dest.data[dst_offset] = xe_resolve_source.data[src_offset];
      break;
    }
    case 3u: {  // 64-bit - direct copy (2 uints)
      xe_resolve_dest.data[dst_offset] = xe_resolve_source.data[src_offset];
      xe_resolve_dest.data[dst_offset + 1u] =
          xe_resolve_source.data[src_offset + 1u];
      break;
    }
  }
}
