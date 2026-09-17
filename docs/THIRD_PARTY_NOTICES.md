# Third-party notices

This project vendors and links the following third-party software. Each remains
under its own licence; the notices below are reproduced in summary, with the
full texts available at the paths and links given.

## rexglue SDK

BSD-3-Clause. Full text: `third_party/rexglue-sdk/LICENSE`.
Source: https://github.com/rexglue/rexglue-sdk

Portions are derived from the **Xenia** project (BSD-3-Clause),
https://github.com/xenia-project/xenia — the recompiler and runtime build on its
work.

## libadrenotools

Full text: `third_party/libadrenotools/LICENSE`.
Source: https://github.com/bylaws/libadrenotools

## SDL

zlib licence. Vendored inside the rexglue SDK.
Source: https://github.com/libsdl-org/SDL

## FFmpeg

**LGPL-2.1-or-later.** Vendored inside the rexglue SDK and used for Xbox 360 XMA
audio decoding (`src/audio/xma_context.cpp`, `xma_decoder.cpp`).
Source: https://ffmpeg.org/ — full text at
`third_party/rexglue-sdk/thirdparty/FFmpeg/LICENSE.md`

FFmpeg is currently linked statically into `librexruntime.so`. Anyone wishing to
relink against a different FFmpeg build should contact the maintainer.

## Vulkan

Vulkan headers and loader components under their respective licences.
https://www.vulkan.org/

---

Retail game code, assets and title-update data are not included in this
repository and are not covered by any of the above. See the Legal section of the
main README.
