/*
 * PixelOps — in-place BGRA pixel fixups applied after decode/convert.
 *
 * Moved out of FrameCache.h (fable_cleanup.txt §3.3) — these are pixel
 * processing, not caching.  Callers: ConvertDecodedFrame (CPU convert
 * path), the prefetch decode workers, and ShotComposer's video preview.
 */

#pragma once

#include <cstddef>
#include <cstdint>

namespace rt {

/// Zero out the RGB channels of fully-transparent (A==0) pixels in-place.
/// FFmpeg sws_scale leaves non-zero RGB in transparent regions; GPU linear
/// texture filtering would bleed those colours into visible edges.
///
/// Uses branchless 32-bit masking: if alpha==0 the entire pixel becomes
/// 0x00000000; otherwise the pixel is kept unchanged.  This eliminates
/// the per-pixel branch that was causing ~4ms stalls on 1080p frames.
inline void clearTransparentPixelRGB(uint8_t* pixels, size_t pixelCount)
{
    auto* p = reinterpret_cast<uint32_t*>(pixels);
    for (size_t i = 0; i < pixelCount; ++i) {
        // BGRA layout: byte 3 is alpha.  Build a mask that is 0xFFFFFFFF
        // when alpha != 0 and 0x00000000 when alpha == 0.
        uint32_t alpha = (p[i] >> 24) & 0xFF;
        // branchless: -!!alpha produces 0xFFFFFFFF when alpha>0, 0 when 0
        uint32_t mask = static_cast<uint32_t>(-static_cast<int32_t>(!!alpha));
        p[i] &= mask;
    }
}

/// Chroma-key green pixels to transparent in-place.
/// Key color is #18FF00 (R=0x18, G=0xFF, B=0x00).
///
/// Uses a green-dominance test rather than per-channel distance so it
/// handles H.264 4:2:0 chroma subsampling gracefully — chroma planes
/// are at 1/4 resolution, so a single chroma sample bleeds across a
/// 2×2 block and produces heavy variance at object edges.
///
/// Two-stage key:
///   1. Hard key:  G dominates both R and B → alpha=0 (fully transparent)
///   2. Spill suppression: G is elevated but not dominant → desaturate G,
///      push R/B toward their original values (reduces green fringe)
inline void chromaKeyInPlace(uint8_t* pixels, size_t pixelCount)
{
    auto* p = reinterpret_cast<uint32_t*>(pixels);
    for (size_t i = 0; i < pixelCount; ++i) {
        const uint32_t px = p[i];
        // BGRA layout
        const int b = static_cast<int>( px        & 0xFF);
        const int g = static_cast<int>((px >>  8) & 0xFF);
        const int r = static_cast<int>((px >> 16) & 0xFF);
        const int a = static_cast<int>((px >> 24) & 0xFF);

        // ── Hard key: green is clearly dominant ──────────────────────
        // H.264 4:2:0 chroma subsampling means the green channel can
        // bleed into the red/blue of edge pixels.  Require G to be
        // substantially ahead of both R and B, with a minimum brightness
        // floor so dark/shadow areas don't false-positive.
        if (g > 80 && g > r + 40 && g > b + 40) {
            p[i] = 0x00000000;  // fully transparent black
            continue;
        }

        // ── Spill suppression: green cast but not dominant ───────────
        // Semi-transparent edge pixels (hair, motion blur) pick up a
        // green tint from the background.  Pull G down toward the
        // average of R and B, weighted by how much G exceeds them.
        if (g > 60 && g > r && g > b && a > 0) {
            const int maxRB = (r > b) ? r : b;
            const int excess = g - maxRB;
            if (excess > 10) {
                // Clamp G down: newG = maxRB + 25% of excess
                const int newG = maxRB + excess / 4;
                const int gClamped = (newG < 0) ? 0 : (newG > 255 ? 255 : newG);
                p[i] = (static_cast<uint32_t>(a) << 24)
                     | (static_cast<uint32_t>(r) << 16)
                     | (static_cast<uint32_t>(gClamped) << 8)
                     | static_cast<uint32_t>(b);
            }
        }
    }
}

} // namespace rt
