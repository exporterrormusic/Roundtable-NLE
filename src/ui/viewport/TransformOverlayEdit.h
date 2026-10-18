/*
 * TransformOverlayEdit.h — which transform a Program Monitor gesture edits.
 *
 * A whole GraphicClip shown with a tight content box edits the OUTER clip
 * transform (clipPosX/clipScaleX/...); everything else edits posX/scaleX/...
 * These read and write whichever one applies. Internal to the overlay.
 */
#pragma once

#include "viewport/OverlayMath.h"

#include <algorithm>
#include <cmath>

namespace rt::overlay_edit {

/// Mask handle ids at or above this encode a Bezier tangent handle:
/// base + vertex * 2 + (0 = in, 1 = out).
inline constexpr int kMaskTangentHandleBase = 10000;

inline bool editsOuterClipTransform(const TransformOverlayInfo& info) noexcept
{
    return info.useContentRect && info.editOuterClipTransform;
}

inline float editPositionX(const TransformOverlayInfo& info) noexcept
{
    return editsOuterClipTransform(info) ? info.clipPosX : info.posX;
}

inline float editPositionY(const TransformOverlayInfo& info) noexcept
{
    return editsOuterClipTransform(info) ? info.clipPosY : info.posY;
}

inline float editScaleX(const TransformOverlayInfo& info) noexcept
{
    return editsOuterClipTransform(info) ? info.clipScaleX : info.scaleX;
}

inline float editScaleY(const TransformOverlayInfo& info) noexcept
{
    return editsOuterClipTransform(info) ? info.clipScaleY : info.scaleY;
}

inline float editRotation(const TransformOverlayInfo& info) noexcept
{
    return editsOuterClipTransform(info) ? info.clipRotation : info.rotation;
}

inline void setEditPosition(TransformOverlayInfo& info, float x, float y) noexcept
{
    if (editsOuterClipTransform(info)) {
        info.clipPosX = x;
        info.clipPosY = y;
    } else {
        info.posX = x;
        info.posY = y;
    }
}

inline void setEditScale(TransformOverlayInfo& info, float x, float y) noexcept
{
    if (editsOuterClipTransform(info)) {
        info.clipScaleX = x;
        info.clipScaleY = y;
    } else {
        info.scaleX = x;
        info.scaleY = y;
    }
}

inline void setEditRotation(TransformOverlayInfo& info, float value) noexcept
{
    if (editsOuterClipTransform(info))
        info.clipRotation = value;
    else
        info.rotation = value;
}

inline float scaleWithOriginalSign(float start, float ratio) noexcept
{
    const float magnitude = std::max(0.01f, std::abs(start) * ratio);
    return std::signbit(start) ? -magnitude : magnitude;
}

} // namespace rt::overlay_edit
