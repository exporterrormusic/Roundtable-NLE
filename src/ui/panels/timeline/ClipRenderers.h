#pragma once

#include <cstdint>
#include <memory>

struct CachedFrame;

class TitleClip;
class GraphicClip;

namespace rt {

// CachedFrame actually lives in namespace rt (media/FrameCache.h); forward-
// declare it here so the renderCaptionClip return type resolves to the same
// rt::CachedFrame as its definition (the global `struct CachedFrame;` above is
// only kept for the legacy renderTitle/Graphic signatures).
struct CachedFrame;

class CaptionClip;

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
