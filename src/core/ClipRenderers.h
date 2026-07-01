#pragma once

#include <cstdint>
#include <memory>

struct CachedFrame;

class TitleClip;
class GraphicClip;

namespace rt {

// CachedFrame actually lives in namespace rt (cache/FrameCache.h); forward-
// declare it here so the renderCaptionClip return type resolves to the same
// rt::CachedFrame as its definition (the global `struct CachedFrame;` above is
// only kept for the legacy renderTitle/Graphic signatures).
struct CachedFrame;

class CaptionClip;
class PngPuppetClip;
class TierListClip;

/// CPU-render a PngPuppetClip's currently-selected face (1 of 4 PNGs, chosen
/// deterministically from time) to a native-resolution BGRA CachedFrame.  The
/// compositor contain-fits + 0.85×-scales it like any other character.  `tick`
/// is the GLOBAL timeline tick so talk/blink stay continuous across cuts.
std::shared_ptr<CachedFrame> renderPngPuppetClip(PngPuppetClip* clip, int64_t tick,
                                                 uint32_t outW, uint32_t outH);

/// CPU-render a TierListClip (ranking board) to a full-frame BGRA CachedFrame.
/// Replays the clip's timed events at `tick` (clip-local) to lay out the grid,
/// placed entries, and the centred spotlight.  refW/refH unused (fractional layout).
std::shared_ptr<CachedFrame> renderTierListClip(TierListClip* clip, int64_t tick,
                                                uint32_t outW, uint32_t outH,
                                                uint32_t refW = 0, uint32_t refH = 0);

/// CPU-render a CaptionClip as a burned-in (open-caption) subtitle overlay to a
/// transparent BGRA CachedFrame using QPainter: centered text over a rounded
/// background box, vertically placed by the clip's CaptionPosition. refW/refH
/// (project resolution) keep font metrics proportional at reduced render res.
std::shared_ptr<CachedFrame> renderCaptionClip(CaptionClip* clip, int64_t tick,
                                               uint32_t outW, uint32_t outH,
                                               uint32_t refW = 0, uint32_t refH = 0);

/// CPU-render a TitleClip to a BGRA CachedFrame using QPainter.
std::shared_ptr<CachedFrame> renderTitleClip(TitleClip* clip, int64_t tick,
                                             uint32_t outW, uint32_t outH);

/// CPU-render a GraphicClip (multi-layer text/shape container) to a BGRA CachedFrame.
/// When refW/refH are non-zero, font sizes and pixel-based metrics are scaled by
/// outW/refW so that text maintains the same visual proportion at reduced resolution.
std::shared_ptr<CachedFrame> renderGraphicClip(GraphicClip* clip, int64_t tick,
                                               uint32_t outW, uint32_t outH,
                                               uint32_t refW = 0, uint32_t refH = 0);

} // namespace rt
