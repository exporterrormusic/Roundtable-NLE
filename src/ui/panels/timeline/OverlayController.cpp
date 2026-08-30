/*
 * OverlayController.cpp - Program Monitor transform-overlay sync + wiring.
 * updateTransformOverlay() / scheduleOverlayRefresh() keep the drag-handle
 * overlay in step with the selected clip / graphic layer; the wire* methods
 * connect both overlay widgets and the inline-text editor.  Drag/click
 * handlers live in OverlayControllerHandlers.cpp.
 */
#include "panels/timeline/OverlayController.h"

#include "panels/timeline/TimelineWorkspace.h"
#include "CompositeService.h"
#include "panels/monitors/ProgramMonitor.h"
#include "panels/timeline/TimelinePanel.h"
#include "panels/properties/PropertiesPanel.h"
#include "panels/effects/EffectControlsPanel.h"
#include "panels/effects/GraphicsEditorPanel.h"
#include "panels/effects/ColorGradingPanel.h"
#include "viewport/Viewport.h"
#include "viewport/TransformOverlayWidget.h"
#include "command/CommandStack.h"
#include "command/LambdaCommand.h"
#include "timeline/Timeline.h"
#include "timeline/Track.h"
#include "timeline/VideoClip.h"
#include "timeline/ImageClip.h"
#include "timeline/SpineClip.h"
#include "timeline/PngPuppetClip.h"
#include "timeline/GraphicClip.h"
#include "timeline/Position2D.h"
#include "timeline/GraphicLayer.h"
#include "timeline/CaptionClip.h"
#include "ClipRenderers.h"
#include "panels/captions/CaptionsPanel.h"
#include "project/Project.h"
#include "playback/MediaPool.h"
#include "playback/PlaybackController.h"
#include <QFileInfo>
#include <QImage>
#include <QFontMetrics>
#include <algorithm>
#include <unordered_map>
#include <functional>

#ifdef ROUNDTABLE_HAS_SPINE
#include "spine/ShotPreset.h"
#endif

#include "effects/Blur.h"

#include <QFont>
#include <QFontMetrics>
#include <QImage>
#include <QPainter>
#include <QPolygonF>
#include <QTimer>

#include <spdlog/spdlog.h>

#include <filesystem>
namespace rt {

bool OverlayController::selectedTrackIsEditable() const noexcept
{
    if (!m_ws || !m_ws->timeline() || !m_ws->selection().clip)
        return false;
    const size_t trackIndex = m_ws->selection().trackIdx;
    if (trackIndex >= m_ws->timeline()->trackCount())
        return false;
    const Track* track = m_ws->timeline()->track(trackIndex);
    return track && !track->isLocked();
}

// Transform overlay - update the Program Monitor handle overlay for the
// currently selected clip so the user can drag-to-move / drag-to-scale.

void OverlayController::scheduleOverlayRefresh()
{
    // Bump generation so stale timer callbacks are ignored.
    uint32_t gen = ++m_overlayRefreshGen;

    // Also force a recomposite so the displayed frame matches compositeWidth.
    m_ws->invalidateCompositeCache();
    if (m_ws->programMonitor()) m_ws->programMonitor()->requestRefresh();

    // Schedule deferred overlay updates at increasing intervals.
    // By the time the later callbacks fire, the pipeline will have
    // composited and displayed the new frame, so srcWidth() is current.
    for (int delayMs : {0, 50, 150}) {
        QTimer::singleShot(delayMs, this, [this, gen]() {
            if (gen == m_overlayRefreshGen)
                updateTransformOverlay();
        });
    }
}

void OverlayController::measureTextContentRect(TextLayer* tl, int64_t relTick,
                                               TransformOverlayInfo& info) const
{
    if (!tl) return;

    QFont font(QString::fromStdString(tl->fontFamily()),
               static_cast<int>(tl->fontSize()));
    font.setWeight(static_cast<QFont::Weight>(tl->fontWeight()));
    font.setItalic(tl->isItalic());
    float tracking = tl->tracking().evaluate(relTick);
    font.setLetterSpacing(QFont::AbsoluteSpacing, static_cast<qreal>(tracking));

    const QString sourceText = QString::fromStdString(tl->text());
    QString text = sourceText;
    // Measure the ALL-CAPS extent for both All Caps and Small Caps: small caps
    // draws originally-lowercase glyphs SMALLER, so the all-caps box is always
    // >= the small-caps box and fully contains it (a slightly generous, never
    // too-small draggable region — never smaller than the visible glyphs).
    if (tl->allCaps() || tl->smallCaps()) text = text.toUpper();

    // Project/sequence resolution — the SAME space renderGraphicClip()
    // composites in (NOT the monitor preview res), so the box doesn't drift.
    uint32_t outW = 0, outH = 0;
    graphicCanvasRes(outW, outH);
    if (outW == 0 || outH == 0) return;   // leave useContentRect false → fallback box

    // Same alignment as renderGraphicClip().
    int hAlign = Qt::AlignHCenter;
    switch (tl->alignment()) {
        case GTextAlign::Left:    hAlign = Qt::AlignLeft;    break;
        case GTextAlign::Center:  hAlign = Qt::AlignHCenter; break;
        case GTextAlign::Right:   hAlign = Qt::AlignRight;   break;
        case GTextAlign::Justify: hAlign = Qt::AlignJustify; break;
    }
    int vAlign = Qt::AlignVCenter;
    switch (tl->vAlignment()) {
        case GTextVAlign::Top:    vAlign = Qt::AlignTop;     break;
        case GTextVAlign::Middle: vAlign = Qt::AlignVCenter; break;
        case GTextVAlign::Bottom: vAlign = Qt::AlignBottom;  break;
    }

    // Very large rect, anchored so the active alignment edge sits at the canvas
    // center — MUST match renderGraphicClip()'s rect so the box tracks the
    // rendered pixels for non-center alignment too (center is unchanged).
    const double bigW = static_cast<double>(outW) * 10.0;
    const double bigH = static_cast<double>(outH) * 10.0;
    const double cxD = static_cast<double>(outW) * 0.5;
    const double cyD = static_cast<double>(outH) * 0.5;
    const double rectX = (hAlign == Qt::AlignLeft)  ? cxD
                       : (hAlign == Qt::AlignRight) ? cxD - bigW
                       :                              cxD - bigW * 0.5;
    const double rectY = (vAlign == Qt::AlignTop)    ? cyD
                       : (vAlign == Qt::AlignBottom) ? cyD - bigH
                       :                               cyD - bigH * 0.5;
    QRectF textRect(rectX, rectY, bigW, bigH);

    // Measure the text bounds with QFontMetricsF (same layout flags as
    // renderGraphicClip's drawText) instead of allocating a full project-res
    // QImage + QPainter every call — this runs per displayed frame while a text
    // layer is selected, so the old ~33 MB (4K) alloc/free per frame was a real
    // cost. boundingRect() returns the identical rect drawText would occupy.
    const QFontMetricsF fm(font);
    QRectF textBounds;
    QRectF textLayoutBounds;
    const bool needsStyledLayout = !tl->styleRuns().empty()
        || !tl->paragraphStyles().empty() || !tl->fontStyle().empty()
        || tl->kerning() != 0.0f || tl->tsume() != 0.0f
        || tl->tabWidth() != 48.0f || sourceText.contains(QChar('\t'))
        || tl->fauxBold() || tl->fauxItalic() || tl->underline()
        || tl->superscript() || tl->subscript() || tl->rightToLeft();
    // Always ask the renderer-owned layout path for both ink and logical line
    // geometry. Even an unstyled title needs the latter for its caret origin;
    // the padded transform box is deliberately wider/taller than the text.
    const auto styled = measureGraphicTextLayout(
        tl, relTick, cxD, cyD, hAlign, vAlign);
    bool usedRendererLayout = false;
    if (styled.valid) {
        textBounds = QRectF(QPointF(styled.left, styled.top),
                            QPointF(styled.right, styled.bottom));
        usedRendererLayout = true;
    }
    if (styled.layoutValid) {
        textLayoutBounds = QRectF(
            QPointF(styled.layoutLeft, styled.layoutTop),
            QPointF(styled.layoutRight, styled.layoutBottom));
    }
    info.textCarets.clear();
    info.textCarets.reserve(styled.carets.size());
    for (const auto& caret : styled.carets) {
        info.textCarets.push_back({
            static_cast<float>(caret.x), static_cast<float>(caret.top),
            static_cast<float>(caret.bottom), caret.valid});
    }
    if (textBounds.isEmpty()) {
        textBounds = fm.boundingRect(
            textRect, hAlign | vAlign | Qt::TextWordWrap, text);
    }
    if (!usedRendererLayout && !needsStyledLayout && !tl->useParagraphBox()) {
        // Point text keeps its first line anchored and grows downward. Qt's
        // AlignVCenter bounds center the entire multiline block, so translate
        // the measured bounds by half of the additional line height to match
        // the renderer and the live monitor editor.
        const int extraLines = std::max(
            0, static_cast<int>(text.count(QChar('\n'))));
        const qreal lineSpacing = fm.lineSpacing()
            + static_cast<qreal>(tl->leading().evaluate(relTick));
        textBounds.translate(0.0, lineSpacing * extraLines * 0.5);
    }

    // Premiere-style breathing room so the corner handles don't sit on the ink.
    float largestFontSize = tl->fontSize();
    for (const auto& run : tl->styleRuns())
        largestFontSize = std::max(largestFontSize, run.fontSize);
    const float horizPad = largestFontSize * 0.45f;
    const float vertPad  = largestFontSize * 0.40f;
    info.useContentRect = true;
    info.contentL = static_cast<float>(textBounds.left())   - horizPad;
    info.contentT = static_cast<float>(textBounds.top())    - vertPad;
    info.contentR = static_cast<float>(textBounds.right())  + horizPad;
    info.contentB = static_cast<float>(textBounds.bottom()) + vertPad;
    info.contentCanvasW = static_cast<float>(outW);
    info.contentCanvasH = static_cast<float>(outH);
    if (textLayoutBounds.isValid() && !textLayoutBounds.isEmpty()) {
        info.useTextLayoutRect = true;
        info.textLayoutL = static_cast<float>(textLayoutBounds.left());
        info.textLayoutT = static_cast<float>(textLayoutBounds.top());
        info.textLayoutR = static_cast<float>(textLayoutBounds.right());
        info.textLayoutB = static_cast<float>(textLayoutBounds.bottom());
    }
}

bool OverlayController::selectTextLayerAt(float frameX, float frameY)
{
    if (!m_ws || !m_ws->timeline() || !m_ws->graphicsEditorPanel())
        return false;

    uint32_t canvasW = 0, canvasH = 0;
    graphicCanvasRes(canvasW, canvasH);
    if (canvasW == 0 || canvasH == 0) return false;

    // textEditRequested is already expressed in this sequence-canvas space.
    // ProgramMonitor::compositeWidth() describes the most recent preview
    // frame and may lag or intentionally differ from the active sequence.
    const float hitX = frameX;
    const float hitY = frameY;

    const int64_t playheadTick = m_ws->playbackController()
        ? m_ws->playbackController()->currentTick() : 0;
    struct Hit {
        GraphicClip* clip{nullptr};
        Track* track{nullptr};
        size_t trackIndex{0};
        size_t clipIndex{0};
        int layerIndex{-1};
    };

    auto hitInClip = [&](GraphicClip* clip, Track* track,
                         size_t trackIndex, size_t clipIndex) -> Hit {
        if (!clip || !track || track->isLocked()
            || playheadTick < clip->timelineIn()
            || playheadTick >= clip->timelineOut()) {
            return {};
        }

        const int64_t localTick = playheadTick - clip->timelineIn();
        const float cx = static_cast<float>(canvasW) * 0.5f;
        const float cy = static_cast<float>(canvasH) * 0.5f;
        const auto clipPosition = evaluatePosition2D(
            clip->positionX(), clip->positionY(), localTick);
        const float clipPosX = clipPosition.first
            * (static_cast<float>(canvasW) / 1920.0f);
        const float clipPosY = clipPosition.second
            * (static_cast<float>(canvasH) / 1080.0f);
        const float clipScaleX = clip->scaleX().evaluate(localTick);
        const float clipScaleY = clip->scaleY().evaluate(localTick);
        const float clipRotation = clip->rotation().evaluate(localTick)
            * 3.14159265358979f / 180.0f;
        const float clipCos = std::cos(clipRotation);
        const float clipSin = std::sin(clipRotation);
        const float clipAnchorX = clip->anchorX().evaluate(localTick)
            * (static_cast<float>(canvasW) / 1920.0f);
        const float clipAnchorY = clip->anchorY().evaluate(localTick)
            * (static_cast<float>(canvasH) / 1080.0f);

        for (int i = static_cast<int>(clip->layerCount()) - 1; i >= 0; --i) {
            auto* layer = clip->layer(static_cast<size_t>(i));
            if (!layer || layer->layerType() != GraphicLayerType::Text
                || !layer->isVisible() || layer->isLocked()) {
                continue;
            }

            auto* text = static_cast<TextLayer*>(layer);
            TransformOverlayInfo bounds;
            measureTextContentRect(text, localTick, bounds);
            if (!bounds.useContentRect) continue;

            const auto& transform = text->transform();
            const float posX = transform.posX.evaluate(localTick);
            const float posY = transform.posY.evaluate(localTick);
            const float scaleX = transform.scaleX.evaluate(localTick);
            const float scaleY = transform.scaleY.evaluate(localTick);
            const float rotation = transform.rotation.evaluate(localTick)
                * 3.14159265358979f / 180.0f;
            const float cosR = std::cos(rotation);
            const float sinR = std::sin(rotation);
            const float anchorX = transform.anchorX.evaluate(localTick);
            const float anchorY = transform.anchorY.evaluate(localTick);

            auto mapPoint = [&](float x, float y) -> QPointF {
                const float dx = scaleX * (x - cx - anchorX);
                const float dy = scaleY * (y - cy - anchorY);
                const float layerX = dx * cosR - dy * sinR
                    + cx + anchorX + posX;
                const float layerY = dx * sinR + dy * cosR
                    + cy + anchorY + posY;

                const float outerX = (layerX - cx - clipAnchorX) * clipScaleX;
                const float outerY = (layerY - cy - clipAnchorY) * clipScaleY;
                return QPointF(
                    outerX * clipCos - outerY * clipSin
                        + cx + clipPosX + clipAnchorX,
                    outerX * clipSin + outerY * clipCos
                        + cy + clipPosY + clipAnchorY);
            };

            QPolygonF polygon;
            polygon << mapPoint(bounds.contentL, bounds.contentT)
                    << mapPoint(bounds.contentR, bounds.contentT)
                    << mapPoint(bounds.contentR, bounds.contentB)
                    << mapPoint(bounds.contentL, bounds.contentB);
            if (polygon.containsPoint(QPointF(hitX, hitY), Qt::WindingFill))
                return {clip, track, trackIndex, clipIndex, i};
        }
        return {};
    };

    Hit hit;
    // Preserve the existing preference for the selected graphic clip, but
    // independently resolve the layer as text instead of trusting the single
    // click that precedes a double-click.
    if (m_ws->selection().clip
        && m_ws->selection().clip->clipType() == ClipType::Graphic
        && m_ws->selection().trackIdx < m_ws->timeline()->trackCount()) {
        Track* track = m_ws->timeline()->track(m_ws->selection().trackIdx);
        if (track) {
            hit = hitInClip(static_cast<GraphicClip*>(m_ws->selection().clip),
                            track, m_ws->selection().trackIdx,
                            m_ws->selection().clipIdx);
        }
    }

    if (!hit.clip) {
        for (size_t ti = 0; ti < m_ws->timeline()->trackCount() && !hit.clip;
             ++ti) {
            Track* track = m_ws->timeline()->track(ti);
            if (!track || track->type() != TrackType::Video
                || track->isLocked()) {
                continue;
            }
            for (size_t ci = 0; ci < track->clipCount(); ++ci) {
                Clip* candidate = track->clip(ci);
                if (!candidate || candidate == m_ws->selection().clip
                    || candidate->clipType() != ClipType::Graphic) {
                    continue;
                }
                hit = hitInClip(static_cast<GraphicClip*>(candidate), track,
                                ti, ci);
                if (hit.clip) break;
            }
        }
    }
    if (!hit.clip) {
        spdlog::warn("[INLINE-TEXT] hit-test miss canvas=({}, {}) tick={} size={}x{}",
                     hitX, hitY, playheadTick, canvasW, canvasH);
        return false;
    }

    spdlog::warn("[INLINE-TEXT] hit-test selected clip={} layerIndex={} track={} canvas=({}, {})",
                 hit.clip->id(), hit.layerIndex, hit.trackIndex, hitX, hitY);

    m_ws->selection().clip = hit.clip;
    m_ws->selection().trackIdx = hit.trackIndex;
    m_ws->selection().clipIdx = hit.clipIndex;
    m_ws->selection().graphicLayerIdx = hit.layerIndex;
    if (m_ws->effectControlsPanel())
        m_ws->effectControlsPanel()->setClip(hit.clip, hit.track);
    m_ws->graphicsEditorPanel()->setClip(hit.clip, hit.track);
    m_ws->graphicsEditorPanel()->selectLayerByStackIndex(hit.layerIndex);
    if (m_ws->colorGradingPanel())
        m_ws->colorGradingPanel()->setClip(hit.clip, hit.track);
    if (m_ws->propertiesPanel())
        m_ws->propertiesPanel()->setClip(hit.clip, hit.track);
    if (m_ws->timelinePanel()) {
        m_ws->timelinePanel()->selection().selectClip(
            ClipRef{hit.trackIndex, hit.clip->id()}, false);
    }
    updateTransformOverlay();
    return true;
}

void OverlayController::updateTransformOverlay()
{
    if (!m_ws->programMonitor() || !m_ws->programMonitor()->viewport()) return;
    auto* vp = m_ws->programMonitor()->viewport();

    if (!m_ws->selection().clip || !m_ws->timeline()
            || !selectedTrackIsEditable()) {
        vp->clearTransformOverlay();
        if (m_ws->programMonitor()->transformOverlay())
            m_ws->programMonitor()->transformOverlay()->clearTransformOverlay();
        return;
    }

    // Hide overlay when playhead is outside the clip's time range.
    int64_t clipEnd = m_ws->selection().clip->timelineOut();
    int64_t clipIn = m_ws->selection().clip->timelineIn();
    if (m_ws->playbackController()) {
        int64_t curTick = m_ws->playbackController()->currentTick();
        if (curTick < clipIn || curTick > clipEnd) {
            vp->clearTransformOverlay();
            if (m_ws->programMonitor()->transformOverlay())
                m_ws->programMonitor()->transformOverlay()->clearTransformOverlay();
            return;
        }
    }

    TransformOverlayInfo info;
    info.visible  = true;
    info.editOuterClipTransform =
        !(m_ws->selection().clip->clipType() == ClipType::Graphic
          && m_ws->selection().graphicLayerIdx >= 0);

    // Evaluate at the current playhead position relative to clip start
    const int64_t relTick = m_ws->playbackController()
        ? std::max(int64_t{0}, m_ws->playbackController()->currentTick() - m_ws->selection().clip->timelineIn())
        : int64_t{0};

    const auto sequenceResolution = m_ws->timeline()->settings().resolution();
    auto migrateSelectedMasks = [&](uint32_t sourceW, uint32_t sourceH,
                                    int sourceRotation = 0) {
        if (sourceW == 0 || sourceH == 0 ||
            sequenceResolution.width == 0 || sequenceResolution.height == 0)
            return;
        const int migrated =
            m_ws->selection().clip->migrateLegacyMasksToSourceLocal(
                sequenceResolution.width, sequenceResolution.height,
                sourceW, sourceH, sourceRotation);
        if (migrated > 0) m_ws->invalidateCompositeCache();
    };
    if (auto* video = dynamic_cast<VideoClip*>(m_ws->selection().clip)) {
        if (video->sourceMetadataAuthoritative())
            migrateSelectedMasks(video->sourceWidth(), video->sourceHeight(),
                                 video->sourceRotation());
    } else if (auto* image = dynamic_cast<ImageClip*>(m_ws->selection().clip)) {
        migrateSelectedMasks(image->sourceWidth(), image->sourceHeight());
    }

    // Per-layer transform for GraphicClip: when a specific layer is selected
    // in Essential Graphics, show the overlay sized around that layer only.
    if (m_ws->selection().clip->clipType() == ClipType::Graphic && m_ws->selection().graphicLayerIdx >= 0) {
        auto* gc = static_cast<GraphicClip*>(m_ws->selection().clip);
        if (m_ws->selection().graphicLayerIdx < static_cast<int>(gc->layerCount())) {
            auto* layer = gc->layer(static_cast<size_t>(m_ws->selection().graphicLayerIdx));
            const auto& xf = layer->transform();
            info.posX     = xf.posX.evaluate(relTick);
            info.posY     = xf.posY.evaluate(relTick);
            info.scaleX   = xf.scaleX.evaluate(relTick);
            info.scaleY   = xf.scaleY.evaluate(relTick);
            info.rotation = xf.rotation.evaluate(relTick);
            info.anchorX  = xf.anchorX.evaluate(relTick);
            info.anchorY  = xf.anchorY.evaluate(relTick);

            // Content-rect mode: the overlay will apply the EXACT same
            // QPainter transform that renderGraphicClip() uses, so the
            // bounding box matches the rendered content pixel-perfectly.
            // posX/posY/scale/rotation are the layer transform values;
            // contentL/T/R/B are the pre-transform canvas-space bounds.
            info.useContentRect = true;

            if (layer->layerType() == GraphicLayerType::Text) {
                auto* textLayer = static_cast<TextLayer*>(layer);
                measureTextContentRect(textLayer, relTick, info);

                // Text is laid out around an alignment origin on the full
                // sequence canvas, while a new layer's anchor tracks start at
                // (0, 0), i.e. that canvas origin.  For left/right alignment
                // and for the font's asymmetric ascent/descent, that is not
                // the visual center of the text.  Seed untouched, static text
                // anchors from the measured content center.  Restrict this
                // migration to unrotated, unscaled layers so existing visuals
                // can never jump when an older project is opened.
                auto& anchorX = textLayer->transform().anchorX;
                auto& anchorY = textLayer->transform().anchorY;
                const bool untouchedAnchor = anchorX.isStatic()
                    && anchorY.isStatic()
                    && std::abs(anchorX.defaultValue()) < 1e-4f
                    && std::abs(anchorY.defaultValue()) < 1e-4f;
                const bool unchangedPivotTransform =
                    textLayer->transform().scaleX.isStatic()
                    && textLayer->transform().scaleY.isStatic()
                    && textLayer->transform().rotation.isStatic()
                    && std::abs(info.scaleX - 1.0f) < 1e-4f
                    && std::abs(info.scaleY - 1.0f) < 1e-4f
                    && std::abs(info.rotation) < 1e-4f;
                if (untouchedAnchor && unchangedPivotTransform
                    && info.contentCanvasW > 0.0f
                    && info.contentCanvasH > 0.0f) {
                    const float centeredAnchorX =
                        (info.contentL + info.contentR) * 0.5f
                        - info.contentCanvasW * 0.5f;
                    const float centeredAnchorY =
                        (info.contentT + info.contentB) * 0.5f
                        - info.contentCanvasH * 0.5f;
                    anchorX.setDefaultValue(centeredAnchorX);
                    anchorY.setDefaultValue(centeredAnchorY);
                    info.anchorX = centeredAnchorX;
                    info.anchorY = centeredAnchorY;
                }
            } else {
                auto* sl = static_cast<ShapeLayer*>(layer);
                float sw = sl->shapeWidth();
                float sh = sl->shapeHeight();
                // Project/sequence resolution — same space renderGraphicClip
                // composites shapes in (NOT the monitor preview res).
                uint32_t outW = 0, outH = 0;
                graphicCanvasRes(outW, outH);
                // Shapes are centered in the canvas
                float cx = static_cast<float>(outW) * 0.5f;
                float cy = static_cast<float>(outH) * 0.5f;
                info.contentL = cx - sw * 0.5f;
                info.contentT = cy - sh * 0.5f;
                info.contentR = cx + sw * 0.5f;
                info.contentB = cy + sh * 0.5f;
                info.contentCanvasW = static_cast<float>(outW);
                info.contentCanvasH = static_cast<float>(outH);
            }

            // Clip-level (outer) transform — applied by compositor on top of layer transform.
            {
                auto p2 = evaluatePosition2D(m_ws->selection().clip->positionX(),
                                             m_ws->selection().clip->positionY(), relTick);
                info.clipPosX = p2.first;
                info.clipPosY = p2.second;
            }
            info.clipScaleX   = m_ws->selection().clip->scaleX().evaluate(relTick);
            info.clipScaleY   = m_ws->selection().clip->scaleY().evaluate(relTick);
            info.clipRotation = m_ws->selection().clip->rotation().evaluate(relTick);
            info.clipAnchorX  = m_ws->selection().clip->anchorX().evaluate(relTick);
            info.clipAnchorY  = m_ws->selection().clip->anchorY().evaluate(relTick);
        }
    } else {
        {
            auto p2 = evaluatePosition2D(m_ws->selection().clip->positionX(),
                                         m_ws->selection().clip->positionY(), relTick);
            info.posX = p2.first;
            info.posY = p2.second;
        }
        info.scaleX   = m_ws->selection().clip->scaleX().evaluate(relTick);
        info.scaleY   = m_ws->selection().clip->scaleY().evaluate(relTick);
        info.rotation = m_ws->selection().clip->rotation().evaluate(relTick);
        info.anchorX  = m_ws->selection().clip->anchorX().evaluate(relTick);
        info.anchorY  = m_ws->selection().clip->anchorY().evaluate(relTick);

        // Whole-clip GraphicClip (no layer focused in Essential Graphics):
        // still size the box around the rendered TEXT so the whole glyph
        // extent is draggable — not the full canvas. The text layer carries
        // its own transform on top of the clip transform, so move the clip
        // transform into the clip-level fields and bake the layer transform
        // into posX/scale (content-rect space), exactly like the focused-layer
        // branch above. Falls back to the canvas-sized box when the clip has
        // no text layer.
        if (m_ws->selection().clip->clipType() == ClipType::Graphic) {
            auto* gc = static_cast<GraphicClip*>(m_ws->selection().clip);
            TextLayer* firstText = nullptr;
            for (size_t i = 0; i < gc->layerCount(); ++i) {
                auto* l = gc->layer(i);
                if (l && l->isVisible() && l->layerType() == GraphicLayerType::Text) {
                    firstText = static_cast<TextLayer*>(l);
                    break;
                }
            }
            if (firstText) {
                const auto& xf = firstText->transform();
                info.clipPosX    = info.posX;
                info.clipPosY    = info.posY;
                info.clipScaleX  = info.scaleX;
                info.clipScaleY  = info.scaleY;
                info.clipRotation = info.rotation;
                info.clipAnchorX = info.anchorX;
                info.clipAnchorY = info.anchorY;
                info.posX     = xf.posX.evaluate(relTick);
                info.posY     = xf.posY.evaluate(relTick);
                info.scaleX   = xf.scaleX.evaluate(relTick);
                info.scaleY   = xf.scaleY.evaluate(relTick);
                info.rotation = xf.rotation.evaluate(relTick);
                info.anchorX  = xf.anchorX.evaluate(relTick);
                info.anchorY  = xf.anchorY.evaluate(relTick);
                measureTextContentRect(firstText, relTick, info);
            }
        }
    }

    // Determine source dimensions for the bounding box.
    // For VideoClip / SpineClipÃƒÂ¢Ã¢â‚¬Â Ã¢â‚¬â„¢video fallback, look up the media info.
    // For GraphicClip, use the output resolution (graphics fill the canvas).
    // For GraphicClip without per-layer bounding box, use output resolution
    if (m_ws->selection().clip->clipType() == ClipType::Graphic && info.srcW == 0 && info.srcH == 0) {
        info.srcW = m_ws->programMonitor()->outputWidth();
        info.srcH = m_ws->programMonitor()->outputHeight();
    }

    if ((info.srcW == 0 || info.srcH == 0) && dynamic_cast<ImageClip*>(m_ws->selection().clip)) {
        auto* imageClip = dynamic_cast<ImageClip*>(m_ws->selection().clip);
        // Use stored source dimensions first
        if (imageClip->sourceWidth() > 0 && imageClip->sourceHeight() > 0) {
            info.srcW = imageClip->sourceWidth();
            info.srcH = imageClip->sourceHeight();
        } else if (m_ws->mediaPool()) {
            uint64_t handle = m_ws->compositeService()->findMediaHandle(imageClip->mediaPath());
            if (handle == 0) {
                handle = m_ws->mediaPool()->open(imageClip->mediaPath());
                if (handle != 0)
                    m_ws->compositeService()->registerMediaHandle(imageClip->mediaPath(), handle);
            }
            if (handle != 0) {
                const auto* mi = m_ws->mediaPool()->getInfo(handle);
                if (mi) {
                    info.srcW = mi->width;
                    info.srcH = mi->height;
                    info.srcRotation = mi->rotation;
                    imageClip->setSourceResolution(mi->width, mi->height);
                    migrateSelectedMasks(mi->width, mi->height);
                }
            }
        }
    }

    if ((info.srcW == 0 || info.srcH == 0) && dynamic_cast<VideoClip*>(m_ws->selection().clip)) {
        auto* videoClip = dynamic_cast<VideoClip*>(m_ws->selection().clip);
        info.srcRotation = videoClip->sourceRotation();
        if (m_ws->mediaPool()) {
            // Try cached handle first
            uint64_t handle = m_ws->compositeService()->findMediaHandle(videoClip->mediaPath());
            if (handle == 0) {
                // Media not opened yet ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â open it now so we get correct dimensions.
                // compositeFrame() will find this handle on its next pass.
                handle = m_ws->mediaPool()->open(videoClip->mediaPath());
                if (handle == 0) {
                    // Try alternate extension (.webm ÃƒÂ¢Ã¢â‚¬Â Ã¢â‚¬Â .mov)
                    namespace fs = std::filesystem;
                    fs::path vidPath(videoClip->mediaPath());
                    fs::path alt = vidPath;
                    if (vidPath.extension() == ".webm") alt.replace_extension(".mov");
                    else if (vidPath.extension() == ".mov") alt.replace_extension(".webm");
                    if (alt != vidPath) {
                        handle = m_ws->mediaPool()->open(alt);
                        if (handle == 0) {
                            fs::path candidate = fs::path("assets") / "videos" / alt.filename();
                            if (fs::exists(candidate))
                                handle = m_ws->mediaPool()->open(candidate);
                        }
                    }
                    if (handle == 0) {
                        fs::path candidate = fs::path("assets") / "videos" / vidPath.filename();
                        if (fs::exists(candidate))
                            handle = m_ws->mediaPool()->open(candidate);
                    }
                }
                if (handle != 0)
                    m_ws->compositeService()->registerMediaHandle(videoClip->mediaPath(), handle);
            }
            if (handle != 0) {
                const auto* mi = m_ws->mediaPool()->getInfo(handle);
                if (mi) {
                    info.srcW = mi->width;
                    info.srcH = mi->height;
                    info.srcRotation = mi->rotation;
                    videoClip->setSourceMetadata(
                        mi->width, mi->height, mi->rotation);
                    migrateSelectedMasks(
                        mi->width, mi->height, mi->rotation);
                }
            }
        }
    }
#ifdef ROUNDTABLE_HAS_SPINE
    // For SpineClip: use shared spine bounds for the overlay size.
    if ((info.srcW == 0 || info.srcH == 0) && dynamic_cast<SpineClip*>(m_ws->selection().clip)) {
        auto* spineClip = static_cast<SpineClip*>(m_ws->selection().clip);
        if (m_ws->compositeService()) {
            const auto* shared = m_ws->compositeService()->getSpineSharedDataForOverlay(
                spineClip->characterName(), spineClip->outfit(),
                static_cast<int>(spineClip->stance()));
            // Match the renderer: it frames the character to THIS animation's
            // box (setup pose ∪ the animation's envelope), so the overlay must
            // use the same bounds or it won't line up with the visible
            // character (especially for taller "action" poses).
            float boundsW = shared ? shared->stableBoundsW : 0.0f;
            float boundsH = shared ? shared->stableBoundsH : 0.0f;
            if (shared) {
                auto abIt = shared->animBounds.find(spineClip->animationName());
                if (abIt != shared->animBounds.end()) {
                    boundsW = abIt->second.w;
                    boundsH = abIt->second.h;
                }
            }
            if (shared && boundsW > 1.0f && boundsH > 1.0f) {
                // Scale: same as compositor: fit height with 0.9 padding
                float refH = 1080.0f;
                float fitZoom = (refH / boundsH) * 0.9f;
                info.srcW = static_cast<uint32_t>(boundsW * fitZoom);
                info.srcH = static_cast<uint32_t>(boundsH * fitZoom);
            }
        }
    }
#endif

    // For PngPuppetClip: probe the resting face PNG to get the character's
    // native dimensions so the transform bounding box has the correct
    // (typically tall/portrait) aspect ratio instead of defaulting to the
    // 16:9 canvas shape.  Match the compositor's contain-fit + 0.85×
    // compose-scale so the box aligns with the visible character.
    // Dimensions are cached per path to avoid repeated disk I/O.
    if ((info.srcW == 0 || info.srcH == 0) && dynamic_cast<PngPuppetClip*>(m_ws->selection().clip)) {
        auto* puppetClip = static_cast<PngPuppetClip*>(m_ws->selection().clip);
        std::string idlePath = puppetClip->facePath(PngPuppetClip::MouthClosedEyesOpen);
        if (!idlePath.empty()) {
            static std::unordered_map<std::string, std::pair<uint32_t, uint32_t>> s_dimCache;
            auto it = s_dimCache.find(idlePath);
            uint32_t imgW = 0, imgH = 0;
            if (it != s_dimCache.end()) {
                imgW = it->second.first;
                imgH = it->second.second;
            } else {
                QImage img(QString::fromStdString(idlePath));
                if (!img.isNull() && img.width() > 0 && img.height() > 0) {
                    imgW = static_cast<uint32_t>(img.width());
                    imgH = static_cast<uint32_t>(img.height());
                    s_dimCache[idlePath] = {imgW, imgH};
                }
            }
            if (imgW > 0 && imgH > 0) {
                // Same pre-scale as Spine: fit height × compose factor so
                // the overlay matches the compositor's final character size.
                constexpr float refH = 1080.0f;
                constexpr float kComposeFit = 0.85f;
                float fitZoom = (refH / static_cast<float>(imgH)) * kComposeFit;
                info.srcW = static_cast<uint32_t>(static_cast<float>(imgW) * fitZoom);
                info.srcH = static_cast<uint32_t>(static_cast<float>(imgH) * fitZoom);
            }
        }
    }

    // Fallback: if we still don't have dimensions, use the viewport's
    // frame dimensions (the composite output).  This at least makes the
    // bounding box match the visible canvas rather than an arbitrary 16:9.
    if (info.srcW == 0 || info.srcH == 0) {
        if (vp->frameWidth() > 0 && vp->frameHeight() > 0) {
            info.srcW = vp->frameWidth();
            info.srcH = vp->frameHeight();
        } else {
            info.srcW = 1920;
            info.srcH = 1080;
        }
    }

    // Packed-alpha: visible area is top half (same as compositor).
    // Apply this BEFORE pushing to either overlay so both the software
    // viewport and the GPU TransformOverlayWidget receive the same srcH —
    // previously the software path got the un-adjusted (full packed)
    // height, producing a 2×-tall bounding box for packed-alpha clips.
    if (info.srcW > 0 && info.srcH > 0 && !info.directSize && !info.useContentRect) {
        if (auto* vc = dynamic_cast<VideoClip*>(m_ws->selection().clip)) {
            if (m_ws->mediaPool()) {
                uint64_t h = m_ws->compositeService()->findMediaHandle(vc->mediaPath());
                if (h != 0) {
                    const auto* mi2 = m_ws->mediaPool()->getInfo(h);
                    if (mi2 && mi2->packedAlpha) {
                        if (info.srcH > 1) info.srcH /= 2;
                    }
                }
            }
        }
    }

    // Characters (SpineClip, PngPuppetClip, and VideoClip flagged as
    // character) are composited with CONTAIN-fit, not cover-fit (see
    // CompositeServiceLayerBuild.cpp where layer.containFit = true is
    // set for isVideoCharClip || isPreRenderedSpine || isPuppetClip).
    // The overlay must match that fit mode or the bounding box is
    // grossly oversized (cover-fit overflows for portrait sources,
    // ~2× too big visually).
    {
        bool isCharacter = false;
#ifdef ROUNDTABLE_HAS_SPINE
        if (dynamic_cast<SpineClip*>(m_ws->selection().clip))
            isCharacter = true;
#endif
        if (dynamic_cast<PngPuppetClip*>(m_ws->selection().clip))
            isCharacter = true;
        if (!isCharacter) {
            if (auto* vc = dynamic_cast<VideoClip*>(m_ws->selection().clip))
                if (vc->isVideoCharacter())
                    isCharacter = true;
        }
        if (isCharacter)
            info.containFit = true;
    }

    // Crop overlay — draggable edge handles for crop-capable clips (Video /
    // Spine) so the user can see what's cropped and crop/uncrop directly in
    // the Program Monitor. Same `info` feeds both the software viewport and
    // the GPU overlay widget below.
    if (m_ws->selection().clip->supportsCrop()) {
        info.cropEnabled = true;
        info.cropL = m_ws->selection().clip->cropLeft();
        info.cropR = m_ws->selection().clip->cropRight();
        info.cropT = m_ws->selection().clip->cropTop();
        info.cropB = m_ws->selection().clip->cropBottom();
    }

    vp->setTransformOverlay(info);

    // Build outline-only sibling overlays for every other layer that's
    // multi-selected in the Essential Graphics list. They share the
    // clip-level transform with the focused layer; only the per-layer
    // transform + content rect differ. The overlay widget draws these
    // as thin dashed rectangles (no handles) so the user can see at a
    // glance which layers will travel together in a group-move drag.
    std::vector<TransformOverlayInfo> secondaries;
    if (m_ws->selection().clip
        && m_ws->selection().clip->clipType() == ClipType::Graphic
        && m_ws->selection().graphicLayerIdxs.size() > 1)
    {
        auto* gc = static_cast<GraphicClip*>(m_ws->selection().clip);
        secondaries.reserve(m_ws->selection().graphicLayerIdxs.size());
        for (int idx : m_ws->selection().graphicLayerIdxs) {
            if (idx == m_ws->selection().graphicLayerIdx) continue;
            if (idx < 0 || idx >= static_cast<int>(gc->layerCount())) continue;
            auto* layer = gc->layer(static_cast<size_t>(idx));
            if (!layer) continue;

            TransformOverlayInfo si;
            si.visible = true;
            si.useContentRect = true;

            const auto& xf = layer->transform();
            si.posX     = xf.posX.evaluate(relTick);
            si.posY     = xf.posY.evaluate(relTick);
            si.scaleX   = xf.scaleX.evaluate(relTick);
            si.scaleY   = xf.scaleY.evaluate(relTick);
            si.rotation = xf.rotation.evaluate(relTick);
            si.anchorX  = xf.anchorX.evaluate(relTick);
            si.anchorY  = xf.anchorY.evaluate(relTick);

            // Shared clip-level (outer) transform.
            si.clipPosX     = info.clipPosX;
            si.clipPosY     = info.clipPosY;
            si.clipScaleX   = info.clipScaleX;
            si.clipScaleY   = info.clipScaleY;
            si.clipRotation = info.clipRotation;
            si.clipAnchorX  = info.clipAnchorX;
            si.clipAnchorY  = info.clipAnchorY;

            uint32_t outW = 0, outH = 0;
            graphicCanvasRes(outW, outH);
            si.contentCanvasW = static_cast<float>(outW);
            si.contentCanvasH = static_cast<float>(outH);

            if (layer->layerType() == GraphicLayerType::Text) {
                auto* tl = static_cast<TextLayer*>(layer);
                QFont font(QString::fromStdString(tl->fontFamily()),
                           static_cast<int>(tl->fontSize()));
                font.setWeight(static_cast<QFont::Weight>(tl->fontWeight()));
                font.setItalic(tl->isItalic());
                float tracking = tl->tracking().evaluate(relTick);
                font.setLetterSpacing(QFont::AbsoluteSpacing,
                                      static_cast<qreal>(tracking));
                QString text = QString::fromStdString(tl->text());
                if (tl->allCaps()) text = text.toUpper();

                double bigW = static_cast<double>(outW) * 10.0;
                double bigH = static_cast<double>(outH) * 10.0;
                QRectF textRect(-bigW * 0.5 + static_cast<double>(outW) * 0.5,
                                -bigH * 0.5 + static_cast<double>(outH) * 0.5,
                                bigW, bigH);
                int hAlign = Qt::AlignHCenter;
                switch (tl->alignment()) {
                    case GTextAlign::Left:    hAlign = Qt::AlignLeft;    break;
                    case GTextAlign::Center:  hAlign = Qt::AlignHCenter; break;
                    case GTextAlign::Right:   hAlign = Qt::AlignRight;   break;
                    case GTextAlign::Justify: hAlign = Qt::AlignJustify; break;
                }
                int vAlign = Qt::AlignVCenter;
                switch (tl->vAlignment()) {
                    case GTextVAlign::Top:    vAlign = Qt::AlignTop;     break;
                    case GTextVAlign::Middle: vAlign = Qt::AlignVCenter; break;
                    case GTextVAlign::Bottom: vAlign = Qt::AlignBottom;  break;
                }
                QImage mc(static_cast<int>(outW), static_cast<int>(outH),
                          QImage::Format_ARGB32_Premultiplied);
                QPainter mp(&mc);
                mp.setRenderHint(QPainter::Antialiasing, true);
                mp.setRenderHint(QPainter::TextAntialiasing, true);
                mp.setFont(font);
                QRectF tb;
                mp.drawText(textRect, hAlign | vAlign | Qt::TextWordWrap,
                            text, &tb);
                mp.end();
                const float horizPad = tl->fontSize() * 0.45f;
                const float vertPad  = tl->fontSize() * 0.40f;
                si.contentL = static_cast<float>(tb.left())   - horizPad;
                si.contentT = static_cast<float>(tb.top())    - vertPad;
                si.contentR = static_cast<float>(tb.right())  + horizPad;
                si.contentB = static_cast<float>(tb.bottom()) + vertPad;
            } else {
                auto* sl = static_cast<ShapeLayer*>(layer);
                float sw = sl->shapeWidth();
                float sh = sl->shapeHeight();
                float cx = static_cast<float>(outW) * 0.5f;
                float cy = static_cast<float>(outH) * 0.5f;
                si.contentL = cx - sw * 0.5f;
                si.contentT = cy - sh * 0.5f;
                si.contentR = cx + sw * 0.5f;
                si.contentB = cy + sh * 0.5f;
            }

            secondaries.push_back(si);
        }
    }

    // Also update the GPU overlay widget (TransformOverlayWidget)
    if (auto* overlay = m_ws->programMonitor()->transformOverlay()) {
        overlay->setTransformOverlay(info);
        overlay->setSecondaryOverlays(secondaries);

        // Masks belong to the clip/effect texture, not to a focused inner
        // Graphic layer. Give the overlay an explicit owner transform so the
        // same local UV path used by the compositor is used for draw/hit-test.
        TransformOverlayInfo maskOwnerInfo = info;
        if (info.useContentRect) {
            uint32_t canvasW = 0, canvasH = 0;
            graphicCanvasRes(canvasW, canvasH);
            maskOwnerInfo = TransformOverlayInfo{};
            maskOwnerInfo.visible = true;
            maskOwnerInfo.srcW = canvasW;
            maskOwnerInfo.srcH = canvasH;
            const auto ownerPos = evaluatePosition2D(
                m_ws->selection().clip->positionX(),
                m_ws->selection().clip->positionY(), relTick);
            maskOwnerInfo.posX = ownerPos.first;
            maskOwnerInfo.posY = ownerPos.second;
            maskOwnerInfo.scaleX =
                m_ws->selection().clip->scaleX().evaluate(relTick);
            maskOwnerInfo.scaleY =
                m_ws->selection().clip->scaleY().evaluate(relTick);
            maskOwnerInfo.rotation =
                m_ws->selection().clip->rotation().evaluate(relTick);
            maskOwnerInfo.anchorX =
                m_ws->selection().clip->anchorX().evaluate(relTick);
            maskOwnerInfo.anchorY =
                m_ws->selection().clip->anchorY().evaluate(relTick);
        }
        overlay->setMaskOwnerOverlay(maskOwnerInfo, !info.useContentRect);

        // Pass mask data for overlay drawing.  The active mask context
        // decides WHICH list the monitor edits: the clip's opacity masks
        // (default) or a specific effect's masks (activated by clicking a
        // mask header under that effect in Effect Controls).
        {
            Clip* selClip = m_ws->selection().clip;
            std::vector<OpacityMask>* maskList = nullptr;
            if (selClip) {
                if (m_activeMaskEffectId != 0) {
                    if (Effect* fx = selClip->effects().effectById(m_activeMaskEffectId)) {
                        maskList = &fx->masks();
                    }
                    if (!maskList)
                        m_activeMaskEffectId = 0;  // effect gone — fall back
                }
                if (!maskList)
                    maskList = &selClip->masks();

                // Clip-local time, same basis the renderer evaluates masks
                // at (playhead − clip start) — used for Mask Path keyframe
                // evaluation and stopwatch-aware writes during drags.
                overlay->setMaskTime(std::max<int64_t>(
                    0, m_ws->playbackController()->currentTick()
                           - selClip->timelineIn()));
            }
            overlay->setMasks(maskList);
        }

        // Pass Position tracks so the overlay can draw the motion path and
        // expose the right-click "Spatial Interpolation" menu on waypoints.
        if (m_ws->selection().clip
            && m_ws->selection().clip->positionX().keyframeCount() >= 2
            && m_ws->selection().clip->positionY().keyframeCount() >= 2)
        {
            overlay->setMotionPathTracks(&m_ws->selection().clip->positionX(),
                                         &m_ws->selection().clip->positionY(),
                                         m_ws->commandStack());
        } else {
            overlay->clearMotionPath();
        }

        // Tell the overlay the PROJECT/sequence resolution — the same
        // space renderGraphicClip composites text in (and the same one
        // the inline text editor scales its font against). Using
        // ProgramMonitor::outputWidth() here gave the preview resolution,
        // which can be lower than the project (e.g. 1920 preview of a 4K
        // project) and made the inline-edit font sized as if the canvas
        // were 1080-tall — visibly different from the rendered text.
        {
            uint32_t w = 0, h = 0;
            graphicCanvasRes(w, h);
            overlay->setSequenceResolution(w, h);
        }
    }
}


void OverlayController::setActiveMaskContext(uint64_t effectId)
{
    // Even an unchanged owner id must rebind the overlay's mask vector. Clip
    // opacity masks use id 0, which is also the default; returning early here
    // meant their first Effect Controls click could select against a null or
    // stale Program Monitor list and silently leave no draggable mask active.
    m_activeMaskEffectId = effectId;
    updateTransformOverlay();
}

void OverlayController::graphicCanvasRes(uint32_t& w, uint32_t& h) const
{
    // Mirror renderGraphicClip()'s reference: the active sequence resolution.
    // A project can contain sequences whose dimensions differ from its
    // defaults, so the timeline must win for overlay hit-testing.
    w = 0;
    h = 0;
    if (m_ws->timeline()) {
        const auto& res = m_ws->timeline()->settings().resolution();
        w = res.width;
        h = res.height;
    }
    if (m_ws->project()) {
        const auto& res = m_ws->project()->settings().resolution();
        if (w == 0) w = res.width;
        if (h == 0) h = res.height;
    }
    if ((w == 0 || h == 0) && m_ws->programMonitor()) {
        w = m_ws->programMonitor()->outputWidth();
        h = m_ws->programMonitor()->outputHeight();
    }
    if (w == 0) w = 1920;
    if (h == 0) h = 1080;
}

void OverlayController::wireViewportTransformSignals()
{
    // -- Software Viewport transform signals: route to the SAME onOverlay*
    //    handlers the GPU TransformOverlayWidget uses (defined in
    //    TimelineWorkspaceWiringTransformOverlay.cpp).  These used to be
    //    duplicated lambda bodies that drifted apart — the Viewport copy
    //    was missing group-move support.  Keep them unified.
    if (m_ws->programMonitor() && m_ws->programMonitor()->viewport()) {
        auto* vp = m_ws->programMonitor()->viewport();
        connect(vp, &Viewport::transformPositionChanged,
                this, &OverlayController::onOverlayPositionChanged);
        connect(vp, &Viewport::transformScaleChanged,
                this, &OverlayController::onOverlayScaleChanged);
        connect(vp, &Viewport::transformRotationChanged,
                this, &OverlayController::onOverlayRotationChanged);
        connect(vp, &Viewport::transformDragFinished,
                this, &OverlayController::onOverlayDragFinished);
    }
}

void OverlayController::wireOverlayToolSignals()
{
    // -- Forward tool changes to TransformOverlayWidget -------------------
    if (m_ws->timelinePanel() && m_ws->programMonitor() && m_ws->programMonitor()->transformOverlay()) {
        auto* ov2 = m_ws->programMonitor()->transformOverlay();
        if (m_ws->graphicsEditorPanel()) {
            auto* graphics = m_ws->graphicsEditorPanel();
            ov2->setInlineTextFormattingWidget(
                graphics->textFormattingWidget());
            connect(graphics, &GraphicsEditorPanel::inlineFontFamilyRequested,
                    ov2, &TransformOverlayWidget::applyInlineTextFontFamily);
            connect(graphics, &GraphicsEditorPanel::inlineFontSizeRequested,
                    ov2, &TransformOverlayWidget::applyInlineTextFontSize);
            connect(graphics, &GraphicsEditorPanel::inlineFontWeightRequested,
                    ov2, &TransformOverlayWidget::applyInlineTextFontWeight);
            connect(graphics, &GraphicsEditorPanel::inlineItalicRequested,
                    ov2, &TransformOverlayWidget::applyInlineTextItalic);
            connect(graphics,
                    &GraphicsEditorPanel::inlineCapitalizationRequested,
                    ov2, &TransformOverlayWidget::applyInlineTextCapitalization);
            connect(graphics, &GraphicsEditorPanel::inlineTrackingRequested,
                    ov2, &TransformOverlayWidget::applyInlineTextTracking);
            connect(graphics,
                    &GraphicsEditorPanel::inlineBaselineShiftRequested,
                    ov2, &TransformOverlayWidget::applyInlineTextBaselineShift);
            connect(graphics, &GraphicsEditorPanel::inlineLeadingRequested,
                    ov2, &TransformOverlayWidget::applyInlineTextLeading);
            connect(graphics, &GraphicsEditorPanel::inlineFontStyleRequested,
                    ov2, &TransformOverlayWidget::applyInlineTextFontStyle);
            connect(graphics, &GraphicsEditorPanel::inlineKerningRequested,
                    ov2, &TransformOverlayWidget::applyInlineTextKerning);
            connect(graphics, &GraphicsEditorPanel::inlineTabWidthRequested,
                    ov2, &TransformOverlayWidget::applyInlineTextTabWidth);
            connect(graphics, &GraphicsEditorPanel::inlineTsumeRequested,
                    ov2, &TransformOverlayWidget::applyInlineTextTsume);
            connect(graphics, &GraphicsEditorPanel::inlineFauxStylesRequested,
                    ov2, &TransformOverlayWidget::applyInlineTextFauxStyles);
            connect(graphics, &GraphicsEditorPanel::inlineUnderlineRequested,
                    ov2, &TransformOverlayWidget::applyInlineTextUnderline);
            connect(graphics, &GraphicsEditorPanel::inlineScriptRequested,
                    ov2, &TransformOverlayWidget::applyInlineTextScript);
            connect(graphics, &GraphicsEditorPanel::inlineFillRequested,
                    ov2, &TransformOverlayWidget::applyInlineTextFill);
            connect(graphics, &GraphicsEditorPanel::inlineStrokeRequested,
                    ov2, &TransformOverlayWidget::applyInlineTextStroke);
            connect(graphics, &GraphicsEditorPanel::inlineShadowRequested,
                    ov2, &TransformOverlayWidget::applyInlineTextShadow);
            connect(graphics, &GraphicsEditorPanel::inlineBackgroundRequested,
                    ov2, &TransformOverlayWidget::applyInlineTextBackground);
            connect(graphics,
                    &GraphicsEditorPanel::inlineParagraphAlignmentRequested,
                    ov2, &TransformOverlayWidget::applyInlineParagraphAlignment);
            connect(graphics,
                    &GraphicsEditorPanel::inlineParagraphDirectionRequested,
                    ov2, &TransformOverlayWidget::applyInlineParagraphDirection);
            connect(ov2,
                    &TransformOverlayWidget::inlineTextSelectionFormatChanged,
                    graphics, &GraphicsEditorPanel::setInlineTextSelectionFormat);
            connect(ov2,
                    &TransformOverlayWidget::inlineTextAdvancedFormatChanged,
                    graphics, &GraphicsEditorPanel::setInlineTextAdvancedFormat);
            connect(ov2,
                    &TransformOverlayWidget::inlineTextSelectionAppearanceChanged,
                    graphics,
                    &GraphicsEditorPanel::setInlineTextSelectionAppearance);
            connect(ov2,
                    &TransformOverlayWidget::inlineParagraphFormatChanged,
                    graphics, &GraphicsEditorPanel::setInlineParagraphFormat);
        }
        if (m_ws->propertiesPanel()) {
            auto* properties = m_ws->propertiesPanel();
            connect(properties, &PropertiesPanel::inlineFontFamilyRequested,
                    ov2, &TransformOverlayWidget::applyInlineTextFontFamily);
            connect(properties, &PropertiesPanel::inlineFontSizeRequested,
                    ov2, &TransformOverlayWidget::applyInlineTextFontSize);
            connect(properties, &PropertiesPanel::inlineFontWeightRequested,
                    ov2, &TransformOverlayWidget::applyInlineTextFontWeight);
            connect(properties, &PropertiesPanel::inlineItalicRequested,
                    ov2, &TransformOverlayWidget::applyInlineTextItalic);
            connect(properties,
                    &PropertiesPanel::inlineCapitalizationRequested,
                    ov2, &TransformOverlayWidget::applyInlineTextCapitalization);
            connect(properties, &PropertiesPanel::inlineLeadingRequested,
                    ov2, &TransformOverlayWidget::applyInlineTextLeading);
            connect(properties, &PropertiesPanel::inlineKerningRequested,
                    ov2, &TransformOverlayWidget::applyInlineTextKerning);
            connect(properties, &PropertiesPanel::inlineFillRequested,
                    ov2, &TransformOverlayWidget::applyInlineTextFill);
            connect(properties, &PropertiesPanel::inlineStrokeRequested,
                    ov2, &TransformOverlayWidget::applyInlineTextStroke);
            connect(properties, &PropertiesPanel::inlineShadowRequested,
                    ov2, &TransformOverlayWidget::applyInlineTextShadow);
            connect(properties,
                    &PropertiesPanel::inlineParagraphAlignmentRequested,
                    ov2, &TransformOverlayWidget::applyInlineParagraphAlignment);
            connect(ov2,
                    &TransformOverlayWidget::inlineTextSelectionFormatChanged,
                    properties,
                    &PropertiesPanel::setInlineTextSelectionFormat);
            connect(ov2,
                    &TransformOverlayWidget::inlineTextAdvancedFormatChanged,
                    properties,
                    &PropertiesPanel::setInlineTextAdvancedFormat);
            connect(ov2,
                    &TransformOverlayWidget::inlineTextSelectionAppearanceChanged,
                    properties,
                    &PropertiesPanel::setInlineTextSelectionAppearance);
            connect(ov2,
                    &TransformOverlayWidget::inlineParagraphFormatChanged,
                    properties, &PropertiesPanel::setInlineParagraphFormat);
        }
        if (m_ws->captionsPanel()) {
            auto* captions = m_ws->captionsPanel();
            connect(captions, &CaptionsPanel::inlineFontFamilyRequested,
                    ov2, &TransformOverlayWidget::applyInlineTextFontFamily);
            connect(captions, &CaptionsPanel::inlineFontSizeRequested,
                    ov2, &TransformOverlayWidget::applyInlineTextFontSize);
            connect(ov2,
                    &TransformOverlayWidget::inlineTextSelectionFormatChanged,
                    captions, &CaptionsPanel::setInlineTextSelectionFormat);
        }
        connect(m_ws->timelinePanel(), &TimelinePanel::toolChanged,
                this, [ov2](EditTool tool) {
            ov2->setEditTool(static_cast<uint8_t>(tool));
        });

        // Double-click a text layer in the Program Monitor → drop an
        // editable text box right on the layer (Premiere Pro). The single
        // click that precedes the double-click already selected the layer
        // and bound it to the panels; the Essential Graphics panel's
        // selectedLayer() is the authoritative source for which layer
        // we're editing.
        auto currentTextLayer = [this]() -> TextLayer* {
            if (!m_ws->graphicsEditorPanel()) return nullptr;
            GraphicLayer* gl = m_ws->graphicsEditorPanel()->selectedLayer();
            if (!gl || gl->layerType() != GraphicLayerType::Text) return nullptr;
            return static_cast<TextLayer*>(gl);
        };

        connect(ov2, &TransformOverlayWidget::textEditRequested,
                this, [this, ov2, currentTextLayer](float frameX, float frameY) {
            if (m_ws->isDestroying()) return;
            spdlog::warn("[INLINE-TEXT] controller request canvas=({}, {})",
                         frameX, frameY);

            // Caption clip selected → edit the caption's text in place,
            // just like a graphic text layer.
            if (m_ws->selection().clip && m_ws->selection().clip->isCaption()) {
                auto* cc = static_cast<CaptionClip*>(m_ws->selection().clip);
                if (m_ws->captionsPanel()) {
                    m_ws->captionsPanel()->setMonitorTextEditing(true);
                    ov2->setInlineTextFormattingWidget(
                        m_ws->captionsPanel()->textFormattingWidget());
                }
                m_preEditOriginalText = cc->text();
                m_preEditOriginalStyles = cc->styleRuns();
                m_preEditOriginalParagraphStyles = cc->paragraphStyles();
                m_preEditClipId = cc->id();
                m_preEditLayerId = 0;
                m_preEditWasCaption = true;
                m_inlineTextEditActive = true;
                updateTransformOverlay();
                QColor textColor = QColor::fromRgba(cc->textColor());
                TextRunAppearance captionAppearance;
                captionAppearance.fillEnabled = true;
                captionAppearance.fillColor = cc->textColor();
                captionAppearance.strokeEnabled = cc->outlineWidth() > 0.0f;
                captionAppearance.strokeColor = cc->outlineColor();
                captionAppearance.strokeWidth = cc->outlineWidth();
                Qt::Alignment captionAlignment = Qt::AlignHCenter;
                if (cc->alignment() == GTextAlign::Left)
                    captionAlignment = Qt::AlignLeft;
                else if (cc->alignment() == GTextAlign::Right)
                    captionAlignment = Qt::AlignRight;
                else if (cc->alignment() == GTextAlign::Justify)
                    captionAlignment = Qt::AlignJustify;
                ov2->beginInlineTextEdit(
                    QString::fromStdString(m_preEditOriginalText),
                    QString::fromStdString(cc->fontFamily()),
                    cc->fontSize(),
                    cc->isBold() ? 700 : 400,
                    /*italic*/false,
                    textColor,
                    /*hStretch*/1.0f,
                    captionAlignment,
                    m_preEditOriginalStyles,
                    1.0f, cc->allCaps(), cc->smallCaps(), cc->tracking(),
                    0.0f, cc->leading(), captionAppearance,
                    m_preEditOriginalParagraphStyles,
                    QString::fromStdString(cc->fontStyle()),
                    0.0f, 48.0f, 0.0f, cc->fauxBold(), cc->fauxItalic(),
                    cc->underline(), cc->superscript(), cc->subscript(),
                    false, true);
                return;
            }

            // Resolve the exact visible text layer at the double-click point.
            // Do not rely on the first click of the gesture: ordinary layer
            // selection can cycle to a shape stacked beneath the text.
            TextLayer* selectedBeforeHitTest = currentTextLayer();
            if (!selectedBeforeHitTest
                && ov2->doubleClickStartedOnSelectedBody()
                && m_ws->selection().clip
                && m_ws->selection().clip->clipType() == ClipType::Graphic) {
                // A timeline-selected graphic can reach this gesture before
                // GraphicsEditorPanel has synchronized selectedLayer(). For a
                // one-title graphic (the common lower-third/timecode case),
                // resolve that unambiguous editable layer directly.
                auto* selectedGraphic = static_cast<GraphicClip*>(
                    m_ws->selection().clip);
                TextLayer* onlyText = nullptr;
                int onlyTextIndex = -1;
                for (size_t i = 0; i < selectedGraphic->layerCount(); ++i) {
                    GraphicLayer* layer = selectedGraphic->layer(i);
                    if (!layer || layer->layerType() != GraphicLayerType::Text
                        || !layer->isVisible() || layer->isLocked()) {
                        continue;
                    }
                    if (onlyText) {
                        onlyText = nullptr;
                        onlyTextIndex = -1;
                        break;
                    }
                    onlyText = static_cast<TextLayer*>(layer);
                    onlyTextIndex = static_cast<int>(i);
                }
                if (onlyText) {
                    selectedBeforeHitTest = onlyText;
                    m_ws->selection().graphicLayerIdx = onlyTextIndex;
                    if (m_ws->graphicsEditorPanel())
                        m_ws->graphicsEditorPanel()->selectLayerByStackIndex(
                            onlyTextIndex);
                    spdlog::warn("[INLINE-TEXT] recovered unsynchronized single text layer={} index={}",
                                 onlyText->layerId(), onlyTextIndex);
                }
            }
            if (!selectTextLayerAt(frameX, frameY)) {
                // A selected text body already proved the gesture target on
                // the first press.  Do not discard its double-click merely
                // because the independent canvas-space hit-test raced a
                // monitor resize or preview-resolution update.
                if (!selectedBeforeHitTest
                    || !ov2->doubleClickStartedOnSelectedBody()) {
                    return;
                }
            }
            TextLayer* tl = currentTextLayer();
            if (!tl) tl = selectedBeforeHitTest;
            if (!tl) {
                spdlog::warn("[INLINE-TEXT] controller stopped: no editable TextLayer");
                return;
            }
            spdlog::warn("[INLINE-TEXT] controller starting editor layer={} textLength={}",
                         tl->layerId(), tl->text().size());
            if (m_ws->graphicsEditorPanel()) {
                ov2->setInlineTextFormattingWidget(
                    m_ws->graphicsEditorPanel()->textFormattingWidget());
            }
            if (m_ws->propertiesPanel()) {
                ov2->addInlineTextFormattingWidget(
                    m_ws->propertiesPanel()->graphicTextFormattingWidget());
            }

            // Pick the first fill colour for the editor text colour.
            QColor textColor(Qt::white);
            const auto& fills = tl->appearance().fills;
            if (!fills.empty())
                textColor = QColor::fromRgba(fills.front().color);

            // Snapshot the current text and update the overlay BEFORE
            // clearing the text, so computeOverlayCorners can measure the
            // content bounds from the original text (otherwise the overlay
            // may have stale/zero bounds, causing beginInlineTextEdit to
            // fall back to a huge default box).
            m_preEditOriginalText = tl->text();
            m_preEditOriginalStyles = tl->styleRuns();
            m_preEditOriginalParagraphStyles = tl->paragraphStyles();
            m_preEditClipId = m_ws->selection().clip
                ? m_ws->selection().clip->id() : 0;
            m_preEditLayerId = tl->layerId();
            m_preEditWasCaption = false;
            m_inlineTextEditActive = true;
            if (m_ws->graphicsEditorPanel())
                m_ws->graphicsEditorPanel()->setMonitorTextEditing(true);
            if (m_ws->propertiesPanel())
                m_ws->propertiesPanel()->setMonitorTextEditing(true);

            // Sync m_ws->selection().graphicLayerIdx to the layer we're about to
            // edit. updateTransformOverlay()'s per-layer branch is gated
            // on this index (>= 0); when it's -1 the overlay falls back
            // to a full-frame box, whose centroid is the frame center —
            // making the editor appear dead-center on the first edit of a
            // freshly-created text clip (the GraphicsEditorPanel's
            // layerSelected signal hasn't propagated yet for a brand-new
            // clip auto-selected by setClip). selectedLayer() is the
            // authoritative source, so derive the index directly.
            if (m_ws->selection().clip &&
                m_ws->selection().clip->clipType() == ClipType::Graphic) {
                auto* gc = static_cast<GraphicClip*>(m_ws->selection().clip);
                size_t idx = gc->findLayerIndex(tl->layerId());
                if (idx != SIZE_MAX)
                    m_ws->selection().graphicLayerIdx = static_cast<int>(idx);
            }

            // Force a synchronous overlay update with the current text bounds.
            updateTransformOverlay();

            // Keep the compositor-rendered title visible. The transparent
            // input control supplies only caret/selection and sends live text
            // below, avoiding a second differently hinted rendering.

            // Pass the layer's font in REFERENCE units. The renderer
            // multiplies the rasterised glyphs by the layer's vertical
            // scale (painter.scale(lsx, lsy) in renderGraphicClip), so a
            // text layer scaled to 2× shows glyphs twice as tall. The
            // inline editor uses a single QFont pointSize, so bake that
            // vertical scale into the effective font size — otherwise the
            // editor renders at the un-scaled size while the surrounding
            // transform box (which already accounts for scale) is much
            // larger.
            float scaleX = 1.0f;
            float scaleY = 1.0f;
            int64_t localTick = 0;
            if (m_ws->selection().clip && m_ws->playbackController()) {
                localTick = std::max<int64_t>(
                    0, m_ws->playbackController()->currentTick() - m_ws->selection().clip->timelineIn());
                scaleX = tl->transform().scaleX.evaluate(localTick);
                scaleY = tl->transform().scaleY.evaluate(localTick);
                // The final composite applies the GraphicClip transform after
                // the text-layer transform. The editor previously included
                // only the inner layer scale, so a clip scaled to 277% snapped
                // much larger on commit while a clip scaled to 78% snapped
                // smaller. Bake both transform levels into the live editor.
                scaleX *= std::abs(
                    m_ws->selection().clip->scaleX().evaluate(localTick));
                scaleY *= std::abs(
                    m_ws->selection().clip->scaleY().evaluate(localTick));
                if (!std::isfinite(scaleX) || scaleX <= 0.0f) scaleX = 1.0f;
                if (!std::isfinite(scaleY) || scaleY <= 0.0f) scaleY = 1.0f;
            }
            spdlog::warn("[INLINE-TEXT] effective scale layer+clip=({}, {})",
                         scaleX, scaleY);
            // Translate the text layer's GTextAlign into a Qt::Alignment
            // flag so the inline editor anchors and aligns its glyphs the
            // same way the renderer does (otherwise center-aligned text
            // jumps to the left edge during edit and snaps back on commit).
            Qt::Alignment hAlignFlag = Qt::AlignHCenter;
            switch (tl->alignment()) {
            case GTextAlign::Left:    hAlignFlag = Qt::AlignLeft;    break;
            case GTextAlign::Right:   hAlignFlag = Qt::AlignRight;   break;
            case GTextAlign::Justify: hAlignFlag = Qt::AlignJustify; break;
            case GTextAlign::Center:
            default:                  hAlignFlag = Qt::AlignHCenter; break;
            }

            // fontSize × scaleY → vertical match. horizontalStretch =
            // scaleX/scaleY → horizontal match for anisotropic scaling.
            TextRunAppearance baseAppearance;
            baseAppearance.fillEnabled = !tl->appearance().fills.empty()
                && tl->appearance().fills.front().enabled;
            if (!tl->appearance().fills.empty())
                baseAppearance.fillColor = tl->appearance().fills.front().color;
            baseAppearance.strokeEnabled = !tl->appearance().strokes.empty()
                && tl->appearance().strokes.front().enabled;
            if (!tl->appearance().strokes.empty()) {
                const auto& stroke = tl->appearance().strokes.front();
                baseAppearance.strokeColor = stroke.color;
                baseAppearance.strokeWidth = stroke.width;
                baseAppearance.strokePosition = stroke.position;
            }
            baseAppearance.shadowEnabled = !tl->appearance().shadows.empty()
                && tl->appearance().shadows.front().enabled;
            if (!tl->appearance().shadows.empty()) {
                const auto& shadow = tl->appearance().shadows.front();
                baseAppearance.shadowColor = shadow.color;
                baseAppearance.shadowDistance = shadow.distance;
                baseAppearance.shadowAngle = shadow.angle;
                baseAppearance.shadowSoftness = shadow.softness;
                baseAppearance.shadowOpacity = shadow.opacity;
            }
            baseAppearance.backgroundEnabled = tl->backgroundEnabled();
            baseAppearance.backgroundColor = tl->backgroundColor();
            baseAppearance.backgroundPadding = tl->backgroundPadding();
            ov2->beginInlineTextEdit(
                QString::fromStdString(m_preEditOriginalText),
                QString::fromStdString(tl->fontFamily()),
                tl->fontSize(),
                tl->fontWeight(),
                tl->isItalic(),
                textColor,
                scaleX / scaleY,
                hAlignFlag,
                m_preEditOriginalStyles,
                scaleY,
                tl->allCaps(),
                tl->smallCaps(),
                tl->tracking().evaluate(localTick),
                tl->baselineShift().evaluate(localTick),
                tl->leading().evaluate(localTick), baseAppearance,
                m_preEditOriginalParagraphStyles,
                QString::fromStdString(tl->fontStyle()), tl->kerning(),
                tl->tabWidth(), tl->tsume(), tl->fauxBold(),
                tl->fauxItalic(), tl->underline(), tl->superscript(),
                tl->subscript(), tl->rightToLeft(), tl->useParagraphBox());
        });

        connect(ov2, &TransformOverlayWidget::inlineTextPreviewChanged,
                this, [this, ov2](const QString& previewText) {
            if (m_ws->isDestroying() || !m_inlineTextEditActive
                || !m_preEditClipId) {
                return;
            }

            Clip* editedClip = nullptr;
            Timeline* timeline = m_ws->timeline();
            if (timeline) {
                for (size_t i = 0; i < timeline->trackCount(); ++i) {
                    Track* track = timeline->track(i);
                    if (!track) continue;
                    const size_t index = track->findClipIndexById(
                        m_preEditClipId);
                    if (index != track->clipCount()) {
                        editedClip = track->clip(index);
                        break;
                    }
                }
            }
            if (!editedClip) return;

            const auto styles = ov2->currentInlineTextStyles();
            const auto paragraphs = ov2->currentInlineParagraphStyles();
            if (m_preEditWasCaption && editedClip->isCaption()) {
                auto* caption = static_cast<CaptionClip*>(editedClip);
                caption->setText(previewText.toStdString());
                caption->setStyleRuns(styles);
                caption->setParagraphStyles(paragraphs);
            } else if (editedClip->clipType() == ClipType::Graphic) {
                GraphicLayer* layer = static_cast<GraphicClip*>(editedClip)
                    ->findLayerById(m_preEditLayerId);
                if (!layer || layer->layerType() != GraphicLayerType::Text)
                    return;
                auto* textLayer = static_cast<TextLayer*>(layer);
                textLayer->setText(previewText.toStdString());
                textLayer->setStyleRuns(styles);
                textLayer->setParagraphStyles(paragraphs);
            } else {
                return;
            }

            // Remeasure from the live model text immediately. While editing,
            // TransformOverlayWidget stores this as the compositor-aligned
            // geometry for its dashed bounds and custom caret.
            updateTransformOverlay();
            m_ws->invalidateCompositeCache();
            if (m_ws->programMonitor())
                m_ws->programMonitor()->requestRefresh();
        });

        connect(ov2, &TransformOverlayWidget::inlineTextCommitted,
                this, [this, ov2](const QString& newText) {
            if (m_ws->isDestroying()) return;
            if (m_ws->graphicsEditorPanel())
                m_ws->graphicsEditorPanel()->setMonitorTextEditing(false);
            if (m_ws->captionsPanel())
                m_ws->captionsPanel()->setMonitorTextEditing(false);
            if (m_ws->propertiesPanel())
                m_ws->propertiesPanel()->setMonitorTextEditing(false);

            // The click that ends editing is also allowed to change the
            // timeline/panel selection. Resolve the edit target from the IDs
            // captured when editing began, never from the current selection.
            // The old selection-based lookup could leave the original layer
            // permanently empty because its text is hidden while the monitor
            // editor is visible.
            const uint64_t editClipId = m_preEditClipId;
            const uint64_t editLayerId = m_preEditLayerId;
            const bool editWasCaption = m_preEditWasCaption;
            auto resolveEditedClip = [this, editClipId]() -> Clip* {
                Timeline* timeline = m_ws->timeline();
                if (!timeline || !editClipId) return nullptr;
                for (size_t i = 0; i < timeline->trackCount(); ++i) {
                    Track* track = timeline->track(i);
                    if (!track) continue;
                    const size_t index = track->findClipIndexById(editClipId);
                    if (index != track->clipCount()) return track->clip(index);
                }
                return nullptr;
            };
            Clip* editedClip = resolveEditedClip();
            auto clearEditIdentity = [this]() {
                m_preEditClipId = 0;
                m_preEditLayerId = 0;
                m_preEditWasCaption = false;
            };

            // Caption clip: commit the edited text back to the caption.
            if (editWasCaption) {
                auto* cc = editedClip && editedClip->isCaption()
                    ? static_cast<CaptionClip*>(editedClip) : nullptr;
                if (!cc) {
                    m_inlineTextEditActive = false;
                    clearEditIdentity();
                    return;
                }
                const std::string newVal = newText.toStdString();
                const std::string oldVal = m_preEditOriginalText;
                const std::vector<TextStyleRun> newStyles =
                    ov2->committedInlineTextStyles();
                const std::vector<TextStyleRun> oldStyles =
                    m_preEditOriginalStyles;
                const std::vector<TextParagraphStyle> newParagraphStyles =
                    ov2->committedInlineParagraphStyles();
                const std::vector<TextParagraphStyle> oldParagraphStyles =
                    m_preEditOriginalParagraphStyles;
                m_inlineTextEditActive = false;
                clearEditIdentity();
                m_preEditOriginalText.clear();
                m_preEditOriginalStyles.clear();
                m_preEditOriginalParagraphStyles.clear();
                auto capRefresh = [this]() {
                    m_ws->invalidateCompositeCache();
                    if (m_ws->programMonitor()) m_ws->programMonitor()->requestRefresh();
                    scheduleOverlayRefresh();
                    if (m_ws->timelinePanel()) m_ws->timelinePanel()->refreshTrackContents();
                    if (m_ws->captionsPanel()) m_ws->captionsPanel()->refresh();
                };
                if (newVal == oldVal && newStyles == oldStyles
                    && newParagraphStyles == oldParagraphStyles) {
                    cc->setText(oldVal);
                    cc->setStyleRuns(oldStyles);
                    cc->setParagraphStyles(oldParagraphStyles);
                    capRefresh();
                    return;
                }
                if (m_ws->commandStack()) {
                    // Capture the clip ID, never the pointer — the caption can
                    // be deleted while this command is still on the stack.
                    const uint64_t capId = cc->id();
                    auto resolve = [this, capId]() -> CaptionClip* {
                        Timeline* tline = m_ws->timeline();
                        if (!tline) return nullptr;
                        for (size_t i = 0; i < tline->trackCount(); ++i) {
                            Track* trk = tline->track(i);
                            if (!trk || !trk->isCaptionTrack()) continue;
                            const size_t idx = trk->findClipIndexById(capId);
                            if (idx == trk->clipCount()) continue;
                            Clip* c = trk->clip(idx);
                            if (c && c->isCaption()) return static_cast<CaptionClip*>(c);
                        }
                        return nullptr;
                    };
                    m_ws->commandStack()->execute(std::make_unique<LambdaCommand>(
                        "Edit Caption Text",
                        [resolve, newVal, newStyles, newParagraphStyles,
                         capRefresh]() {
                            if (auto* c = resolve()) {
                                c->setText(newVal);
                                c->setStyleRuns(newStyles);
                                c->setParagraphStyles(newParagraphStyles);
                                capRefresh();
                            }
                        },
                        [resolve, oldVal, oldStyles, oldParagraphStyles,
                         capRefresh]() {
                            if (auto* c = resolve()) {
                                c->setText(oldVal);
                                c->setStyleRuns(oldStyles);
                                c->setParagraphStyles(oldParagraphStyles);
                                capRefresh();
                            }
                        }));
                } else {
                    cc->setText(newVal);
                    cc->setStyleRuns(newStyles);
                    cc->setParagraphStyles(newParagraphStyles);
                    capRefresh();
                }
                return;
            }

            TextLayer* tl = nullptr;
            if (editedClip && editedClip->clipType() == ClipType::Graphic) {
                auto* layer = static_cast<GraphicClip*>(editedClip)
                    ->findLayerById(editLayerId);
                if (layer && layer->layerType() == GraphicLayerType::Text)
                    tl = static_cast<TextLayer*>(layer);
            }
            if (!tl) {
                m_inlineTextEditActive = false;
                clearEditIdentity();
                return;
            }
            const std::string newVal = newText.toStdString();
            const std::string oldVal = m_preEditOriginalText;
            const std::vector<TextStyleRun> newStyles =
                ov2->committedInlineTextStyles();
            const std::vector<TextStyleRun> oldStyles =
                m_preEditOriginalStyles;
            const std::vector<TextParagraphStyle> newParagraphStyles =
                ov2->committedInlineParagraphStyles();
            const std::vector<TextParagraphStyle> oldParagraphStyles =
                m_preEditOriginalParagraphStyles;
            const bool wasActive = m_inlineTextEditActive;
            m_inlineTextEditActive = false;
            clearEditIdentity();
            m_preEditOriginalText.clear();
            m_preEditOriginalStyles.clear();
            m_preEditOriginalParagraphStyles.clear();

            auto refresh = [this]() {
                if (m_ws->graphicsEditorPanel()) m_ws->graphicsEditorPanel()->refresh();
                m_ws->invalidateCompositeCache();
                if (m_ws->programMonitor()) m_ws->programMonitor()->requestRefresh();
                scheduleOverlayRefresh();
            };

            // If nothing changed (e.g. cancel/commit unchanged), restore
            // the original text without making an undo entry.
            if (newVal == oldVal && newStyles == oldStyles
                && newParagraphStyles == oldParagraphStyles) {
                if (wasActive) {
                    tl->setText(oldVal);
                    tl->setStyleRuns(oldStyles);
                    tl->setParagraphStyles(oldParagraphStyles);
                    refresh();
                }
                return;
            }

            // Route through the command stack so Ctrl+Z reverts to the
            // pre-edit text. The layer is currently "" (cleared on begin),
            // so execute() sets it to newVal and undo restores oldVal.
            if (m_ws->commandStack()) {
                // Capture clip + layer IDs, never the TextLayer pointer — the
                // graphic clip can be deleted while the command is stacked.
                const uint64_t gcId = editClipId;
                const uint64_t layerId = editLayerId;
                auto resolve = [this, gcId, layerId]() -> TextLayer* {
                    Timeline* tline = m_ws->timeline();
                    if (!tline || !gcId) return nullptr;
                    for (size_t i = 0; i < tline->trackCount(); ++i) {
                        Track* trk = tline->track(i);
                        if (!trk) continue;
                        const size_t idx = trk->findClipIndexById(gcId);
                        if (idx == trk->clipCount()) continue;
                        Clip* c = trk->clip(idx);
                        if (!c || c->clipType() != ClipType::Graphic) return nullptr;
                        auto* layer = static_cast<GraphicClip*>(c)->findLayerById(layerId);
                        if (layer && layer->layerType() == GraphicLayerType::Text)
                            return static_cast<TextLayer*>(layer);
                        return nullptr;
                    }
                    return nullptr;
                };
                m_ws->commandStack()->execute(std::make_unique<LambdaCommand>(
                    "Edit Text",
                    [resolve, newVal, newStyles, newParagraphStyles, refresh]() {
                        if (auto* t = resolve()) {
                            t->setText(newVal);
                            t->setStyleRuns(newStyles);
                            t->setParagraphStyles(newParagraphStyles);
                            refresh();
                        }
                    },
                    [resolve, oldVal, oldStyles, oldParagraphStyles, refresh]() {
                        if (auto* t = resolve()) {
                            t->setText(oldVal);
                            t->setStyleRuns(oldStyles);
                            t->setParagraphStyles(oldParagraphStyles);
                            refresh();
                        }
                    }));
            } else {
                tl->setText(newVal);
                tl->setStyleRuns(newStyles);
                tl->setParagraphStyles(newParagraphStyles);
                refresh();
            }
        });
    }
}


} // namespace rt
