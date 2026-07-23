/*
 * OverlayMath.cpp — Shared transform overlay math utilities.
 *
 * Implementation of free functions declared in OverlayMath.h.
 */

#include "viewport/OverlayMath.h"

namespace rt {

// ═════════════════════════════════════════════════════════════════════════════
//  Compute overlay corners
// ═════════════════════════════════════════════════════════════════════════════

void computeOverlayCorners(
    const TransformOverlayInfo& overlay,
    float outW, float outH,
    const QRectF& frameRect,
    const FrameToWidgetFn& toWidget,
    QPointF corners[4])
{
    constexpr float REF_W = 1920.0f;
    constexpr float REF_H = 1080.0f;

    // ── Per-layer content-rect mode ─────────────────────────────────────
    // Replicates renderGraphicClip's QPainter transform exactly:
    //   translate(outW/2 + posX, outH/2 + posY)
    //   rotate(rotation)
    //   scale(scaleX, scaleY)
    //   translate(-outW/2, -outH/2)
    // Content rect corners are in pre-transform canvas coordinates.
    if (overlay.useContentRect &&
        overlay.contentCanvasW > 0.0f && overlay.contentCanvasH > 0.0f &&
        outW > 0.0f && outH > 0.0f && !frameRect.isEmpty())
    {
        float canvasW = overlay.contentCanvasW;
        float canvasH = overlay.contentCanvasH;
        float cx = canvasW * 0.5f;
        float cy = canvasH * 0.5f;
        float radians = overlay.rotation * 3.14159265358979f / 180.0f;
        float cosR = std::cos(radians);
        float sinR = std::sin(radians);

        // Clip-level (outer) transform
        float clipRadians = overlay.clipRotation * 3.14159265358979f / 180.0f;
        float clipCosR = std::cos(clipRadians);
        float clipSinR = std::sin(clipRadians);
        // Clip-level position is in REF-1920 px; scale into canvas space.
        float clipPxX = overlay.clipPosX * (canvasW / REF_W);
        float clipPxY = overlay.clipPosY * (canvasH / REF_H);
        float clipAnchorPxX = overlay.clipAnchorX * (canvasW / REF_W);
        float clipAnchorPxY = overlay.clipAnchorY * (canvasH / REF_H);

        auto fwd = [&](float x, float y) -> QPointF {
            // Layer transform (inner): same as renderGraphicClip QPainter.
            // Subtract the anchor inside the scale/rotate so they pivot
            // around (cx+anchorX, cy+anchorY), then add it back along with
            // the layer position. Anchor=(0,0) → identical to legacy.
            const float lax = overlay.anchorX;
            const float lay = overlay.anchorY;
            float dx = overlay.scaleX * (x - cx - lax);
            float dy = overlay.scaleY * (y - cy - lay);
            float ox = dx * cosR - dy * sinR + cx + lax + overlay.posX;
            float oy = dx * sinR + dy * cosR + cy + lay + overlay.posY;

            // Clip-level transform (outer): compositor blitLayerWithTransform
            float rx = (ox - cx - clipAnchorPxX) * overlay.clipScaleX;
            float ry = (oy - cy - clipAnchorPxY) * overlay.clipScaleY;
            float fx = rx * clipCosR - ry * clipSinR
                     + cx + clipPxX + clipAnchorPxX;
            float fy = rx * clipSinR + ry * clipCosR
                     + cy + clipPxY + clipAnchorPxY;

            // Map using canvas dimensions so coordinates in 1920x1080 space
            // map correctly even if displayed at half resolution.
            double wx = frameRect.x() + (static_cast<double>(fx) / canvasW) * frameRect.width();
            double wy = frameRect.y() + (static_cast<double>(fy) / canvasH) * frameRect.height();
            return QPointF(wx, wy);
        };

        corners[0] = fwd(overlay.contentL, overlay.contentT);
        corners[1] = fwd(overlay.contentR, overlay.contentT);
        corners[2] = fwd(overlay.contentR, overlay.contentB);
        corners[3] = fwd(overlay.contentL, overlay.contentB);
        return;
    }

    // ── Standard mode ───────────────────────────────────────────────────
    if (overlay.srcW == 0 || overlay.srcH == 0 ||
        outW <= 0.0f || outH <= 0.0f || frameRect.isEmpty())
    {
        for (int i = 0; i < 4; ++i) corners[i] = QPointF(0, 0);
        return;
    }

    // Scale positions from reference (1920×1080) to output.
    float posXPx    = overlay.posX    * (outW / REF_W);
    float posYPx    = overlay.posY    * (outH / REF_H);
    float anchorXPx = overlay.anchorX * (outW / REF_W);
    float anchorYPx = overlay.anchorY * (outH / REF_H);

    float cx = outW * 0.5f;
    float cy = outH * 0.5f;

    float fittedW, fittedH, baseOffX, baseOffY;

    if (overlay.directSize) {
        // directSize: srcW/srcH are pixel dims in reference space.
        fittedW = static_cast<float>(overlay.srcW) * (outW / REF_W);
        fittedH = static_cast<float>(overlay.srcH) * (outH / REF_H);
        baseOffX = (outW - fittedW) * 0.5f;
        baseOffY = (outH - fittedH) * 0.5f;
    } else {
        const int srcRotation = ((overlay.srcRotation % 360) + 360) % 360;
        const bool quarterTurn = srcRotation == 90 || srcRotation == 270;
        float srcW = static_cast<float>(
            quarterTurn ? overlay.srcH : overlay.srcW);
        float srcH = static_cast<float>(
            quarterTurn ? overlay.srcW : overlay.srcH);
        float scaleToFitW = outW / srcW;
        float scaleToFitH = outH / srcH;
        float fitScale = overlay.containFit
            ? std::min(scaleToFitW, scaleToFitH)
            : std::max(scaleToFitW, scaleToFitH);

        fittedW = srcW * fitScale;
        fittedH = srcH * fitScale;
        baseOffX = (outW - fittedW) * 0.5f;
        baseOffY = (outH - fittedH) * 0.5f;
    }

    float radians = overlay.rotation * 3.14159265358979f / 180.0f;
    float cosR = std::cos(radians);
    float sinR = std::sin(radians);

    auto forwardXY = [&](float fitX, float fitY) -> QPointF {
        // Subtract anchor inside the scale/rotate so they pivot around
        // (cx + anchor); add anchor + posPx back afterwards. Mirrors the
        // GPU compositor's buildViewportTransform anchor handling and
        // the per-layer (content-rect) overlay branch. anchor=(0,0)
        // collapses to legacy behavior.
        float rx = (fitX - cx + baseOffX - anchorXPx) * overlay.scaleX;
        float ry = (fitY - cy + baseOffY - anchorYPx) * overlay.scaleY;
        float ox = rx * cosR - ry * sinR + cx + posXPx + anchorXPx;
        float oy = rx * sinR + ry * cosR + cy + posYPx + anchorYPx;
        return toWidget(ox, oy);
    };

    corners[0] = forwardXY(0.0f,    0.0f);    // top-left
    corners[1] = forwardXY(fittedW, 0.0f);    // top-right
    corners[2] = forwardXY(fittedW, fittedH); // bottom-right
    corners[3] = forwardXY(0.0f,    fittedH); // bottom-left
}

// ═════════════════════════════════════════════════════════════════════════════
//  Hit testing
// ═════════════════════════════════════════════════════════════════════════════

int hitTestHandle(const QPointF& widgetPos, const QPointF corners[4])
{
    // Generous corner grab zone so the scale cursor is easy to hit.
    // The rotation zone starts exactly where this ends (see hitTestRotate
    // INNER_RADIUS) so the two never overlap.
    constexpr double HANDLE_RADIUS = 18.0;

    // Centroid — for small items the 4 handle circles overlap the body's
    // centre; require the cursor to be radially beyond the corner so the
    // centre/body never matches a handle (otherwise body-drag has a dead
    // zone in the middle).
    const QPointF center(
        (corners[0].x() + corners[1].x() + corners[2].x() + corners[3].x()) * 0.25,
        (corners[0].y() + corners[1].y() + corners[2].y() + corners[3].y()) * 0.25);

    for (int i = 0; i < 4; ++i) {
        double dx = widgetPos.x() - corners[i].x();
        double dy = widgetPos.y() - corners[i].y();
        if (dx * dx + dy * dy > HANDLE_RADIUS * HANDLE_RADIUS) continue;
        const double pcDx = widgetPos.x() - center.x();
        const double pcDy = widgetPos.y() - center.y();
        const double kcDx = corners[i].x() - center.x();
        const double kcDy = corners[i].y() - center.y();
        if ((pcDx * pcDx + pcDy * pcDy) < (kcDx * kcDx + kcDy * kcDy)) continue;
        return i;
    }
    return -1;
}

bool hitTestBody(const QPointF& widgetPos, const QPointF corners[4])
{
    auto cross = [](QPointF a, QPointF b, QPointF p) {
        return (b.x() - a.x()) * (p.y() - a.y()) -
               (b.y() - a.y()) * (p.x() - a.x());
    };

    double c0 = cross(corners[0], corners[1], widgetPos);
    double c1 = cross(corners[1], corners[2], widgetPos);
    double c2 = cross(corners[2], corners[3], widgetPos);
    double c3 = cross(corners[3], corners[0], widgetPos);

    bool allPos = (c0 >= 0) && (c1 >= 0) && (c2 >= 0) && (c3 >= 0);
    bool allNeg = (c0 <= 0) && (c1 <= 0) && (c2 <= 0) && (c3 <= 0);
    return allPos || allNeg;
}

void syncMaskOwnerFromOuterClip(const TransformOverlayInfo& edited,
                                TransformOverlayInfo& maskOwner) noexcept
{
    maskOwner.posX = edited.clipPosX;
    maskOwner.posY = edited.clipPosY;
    maskOwner.scaleX = edited.clipScaleX;
    maskOwner.scaleY = edited.clipScaleY;
    maskOwner.rotation = edited.clipRotation;
    maskOwner.anchorX = edited.clipAnchorX;
    maskOwner.anchorY = edited.clipAnchorY;
}

} // namespace rt
