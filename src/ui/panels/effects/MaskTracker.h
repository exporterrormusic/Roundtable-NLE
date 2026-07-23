/*
 * MaskTracker — Premiere Pro–style "track mask forward/backward".
 *
 * Steps through the clip's source video one frame at a time, follows the
 * mask's content by normalized-cross-correlation template matching on the
 * luma plane, and writes one Mask Path keyframe per frame (translation
 * tracking — Premiere's default mask-tracking mode).
 *
 * Runs a modal, cancellable QProgressDialog loop on the calling (GUI)
 * thread; decoding uses a private software decoder so it never disturbs
 * playback's decoder cache. Cancel keeps the keyframes tracked so far
 * (Premiere behaviour). The caller wraps the whole run in one undo command.
 */

#pragma once

#include "timeline/OpacityMask.h"

#include <cstdint>
#include <filesystem>

class QWidget;

namespace rt {

struct MaskTrackParams
{
    std::filesystem::path mediaPath;

    // Clip → source time mapping
    int64_t sourceInTicks{0};      ///< clip->sourceIn()
    double  speed{1.0};            ///< clip->speed()
    int64_t clipDurationTicks{0};  ///< clip->duration()
    int64_t startLocalTick{0};     ///< clip-local playhead tick
    bool    forward{true};

    // Sequence resolution + the clip's transform at the start time (output
    // px / degrees, same units the compositor uses). These are only needed
    // for projects whose masks use the legacy sequence-frame coordinate
    // space. Source-local masks track directly in decoded source pixels.
    uint32_t outW{1920};
    uint32_t outH{1080};
    float posX{0.0f}, posY{0.0f};
    float scaleX{1.0f}, scaleY{1.0f};
    float rotation{0.0f};
    float anchorX{0.0f}, anchorY{0.0f};
};

namespace MaskTracker {

namespace detail {

/// Affine mapping between mask coordinates and native decoded source pixels.
/// Kept public in the detail namespace so the coordinate-space contract can
/// be covered without opening or decoding media in a unit test.
struct CoordinateMapping
{
    float sourceXFromU{0.0f};
    float sourceXFromV{0.0f};
    float sourceXOffset{0.0f};
    float sourceYFromU{0.0f};
    float sourceYFromV{0.0f};
    float sourceYOffset{0.0f};
    float determinant{0.0f};

    [[nodiscard]] bool isValid() const noexcept;
    void toSourcePixels(float u, float v, float& x, float& y) const noexcept;
    void sourceDeltaToMask(float dx, float dy,
                           float& du, float& dv) const noexcept;
    [[nodiscard]] float pixelsPerMaskU() const noexcept;
    [[nodiscard]] float pixelsPerMaskV() const noexcept;
};

/// SourceLocal ignores clip transform and display rotation because its UVs
/// already address the decoder's native pixel orientation. Legacy masks are
/// converted from sequence-frame UV through the compositor transform.
[[nodiscard]] CoordinateMapping buildCoordinateMapping(
    const MaskTrackParams& params,
    MaskCoordinateSpace coordinateSpace,
    uint32_t sourceWidth,
    uint32_t sourceHeight,
    int sourceRotationDegrees);

} // namespace detail

/// Track the mask through the source video, adding Mask Path keyframes to
/// `mask` (turns the Mask Path stopwatch on). Returns the number of
/// keyframes written (0 = failed or nothing to do).
int track(const MaskTrackParams& params, OpacityMask& mask, QWidget* parent);

} // namespace MaskTracker
} // namespace rt
