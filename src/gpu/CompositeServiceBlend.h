/*
 * CompositeServiceBlend.h - CPU pixel blending declarations.
 * Extracted from CompositeServiceFrame.cpp.
 */
#pragma once

#include "timeline/OpacityMask.h"

#include <cstdint>
#include <vector>

namespace rt {

/// Alpha-blend a source BGRA layer onto a destination BGRA buffer
/// with affine transform (position, scale, rotation).
/// Source is fill-to-output (covers entire output, may crop edges) first,
/// then clip transforms applied.
void blitLayerWithTransform(
    uint8_t* dst, uint32_t dstW, uint32_t dstH,
    const uint8_t* src, uint32_t srcW, uint32_t srcH, uint32_t srcStride,
    float opacity,
    float posXPx, float posYPx,     // position offset in output pixels
    float scX, float scY,           // scale multiplier (1.0 = normal)
    float rotDeg,                   // rotation in degrees
    float cropL = 0.0f, float cropR = 0.0f,  // crop percentages (0-100)
    float cropT = 0.0f, float cropB = 0.0f,
    bool containFit = false);       // true = contain fit, false = cover fit

/// Optional affine mapping from normalized mask space (0–1) into the
/// rasterization target's pixel grid:
///   px = m[0]*u + m[1]*v + m[2]
///   py = m[3]*u + m[4]*v + m[5]
/// Runtime clip/effect masks use the default identity mapping because their
/// geometry and raster target are both clip-local. The optional mapping is
/// retained for callers that intentionally rasterize into another grid.
struct MaskRasterTransform
{
    float m[6];
};

/// CPU mask rasterizer — generates an RGBA buffer (all channels equal)
/// where white = opaque, black = transparent.
///
/// Premiere Pro semantics:
///   - Masks combine ADDITIVELY (union): each mask reveals its region.
///   - Feather is a smooth two-sided falloff centered on the (expanded)
///     edge, in target pixels.
///   - Expansion offsets the edge outward (+) or inward (−).
///   - Bezier paths rasterize as true curves (flattened adaptively).
/// Implemented via scanline polygon fill + exact Euclidean distance
/// transform, so feather/expansion behave identically for every shape.
std::vector<uint8_t> rasterizeMasks(const std::vector<MaskRenderState>& masks,
                                    uint32_t w, uint32_t h,
                                    const MaskRasterTransform* frameToTarget = nullptr);

/// FNV-1a hash of the evaluated mask state + target dims — used by the
/// engine to skip re-rasterizing/uploading when nothing changed.
uint64_t hashMaskStates(const std::vector<MaskRenderState>& masks,
                        uint32_t w, uint32_t h,
                        const MaskRasterTransform* frameToTarget = nullptr);

} // namespace rt
