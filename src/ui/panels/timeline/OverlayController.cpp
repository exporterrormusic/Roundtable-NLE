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
#include <QTimer>

#include <spdlog/spdlog.h>

#include <filesystem>
namespace rt {

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

    QString text = QString::fromStdString(tl->text());
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
    const QRectF textBounds =
        fm.boundingRect(textRect, hAlign | vAlign | Qt::TextWordWrap, text);

    // Premiere-style breathing room so the corner handles don't sit on the ink.
    const float horizPad = tl->fontSize() * 0.45f;
    const float vertPad  = tl->fontSize() * 0.40f;
    info.useContentRect = true;
    info.contentL = static_cast<float>(textBounds.left())   - horizPad;
    info.contentT = static_cast<float>(textBounds.top())    - vertPad;
    info.contentR = static_cast<float>(textBounds.right())  + horizPad;
    info.contentB = static_cast<float>(textBounds.bottom()) + vertPad;
    info.contentCanvasW = static_cast<float>(outW);
    info.contentCanvasH = static_cast<float>(outH);
}

void OverlayController::updateTransformOverlay()
{
    if (!m_ws->programMonitor() || !m_ws->programMonitor()->viewport()) return;
    auto* vp = m_ws->programMonitor()->viewport();

    if (!m_ws->selection().clip || !m_ws->timeline()) {
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

    // Evaluate at the current playhead position relative to clip start
    const int64_t relTick = m_ws->playbackController()
        ? std::max(int64_t{0}, m_ws->playbackController()->currentTick() - m_ws->selection().clip->timelineIn())
        : int64_t{0};

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
                measureTextContentRect(static_cast<TextLayer*>(layer), relTick, info);
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
                }
            }
        }
    }

    if ((info.srcW == 0 || info.srcH == 0) && dynamic_cast<VideoClip*>(m_ws->selection().clip)) {
        auto* videoClip = dynamic_cast<VideoClip*>(m_ws->selection().clip);
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

        // Pass mask data for overlay drawing
        if (m_ws->selection().clip && m_ws->selection().clip->maskCount() > 0)
            overlay->setMasks(&m_ws->selection().clip->masks());
        else
            overlay->setMasks(nullptr);

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


void OverlayController::graphicCanvasRes(uint32_t& w, uint32_t& h) const
{
    // Mirror renderGraphicClip()'s reference: the project/sequence
    // resolution. Fall back to the monitor preview res, then 1920×1080.
    w = 0;
    h = 0;
    if (m_ws->project()) {
        const auto& res = m_ws->project()->settings().resolution();
        w = res.width;
        h = res.height;
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
                this, [this, ov2, currentTextLayer](float, float) {
            if (m_ws->isDestroying()) return;

            // Caption clip selected → edit the caption's text in place,
            // just like a graphic text layer.
            if (m_ws->selection().clip && m_ws->selection().clip->isCaption()) {
                auto* cc = static_cast<CaptionClip*>(m_ws->selection().clip);
                m_preEditOriginalText = cc->text();
                m_inlineTextEditActive = true;
                updateTransformOverlay();
                cc->setText(std::string{});           // hide while editing
                m_ws->invalidateCompositeCache();
                if (m_ws->programMonitor()) m_ws->programMonitor()->requestRefresh();
                QColor textColor = QColor::fromRgba(cc->textColor());
                ov2->beginInlineTextEdit(
                    QString::fromStdString(m_preEditOriginalText),
                    QString::fromStdString(cc->fontFamily()),
                    cc->fontSize(),
                    static_cast<int>(QFont::Bold),  // captions render bold
                    /*italic*/false,
                    textColor,
                    /*hStretch*/1.0f,
                    Qt::AlignHCenter);
                return;
            }

            TextLayer* tl = currentTextLayer();
            if (!tl) return;

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
            m_inlineTextEditActive = true;

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

            // Now clear the text for compositing so it doesn't show behind
            // the editor box (Premiere Pro).
            tl->setText(std::string{});
            m_ws->invalidateCompositeCache();
            if (m_ws->programMonitor()) m_ws->programMonitor()->requestRefresh();

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
            if (m_ws->selection().clip && m_ws->playbackController()) {
                const int64_t localTick = std::max<int64_t>(
                    0, m_ws->playbackController()->currentTick() - m_ws->selection().clip->timelineIn());
                scaleX = tl->transform().scaleX.evaluate(localTick);
                scaleY = tl->transform().scaleY.evaluate(localTick);
                if (!std::isfinite(scaleX) || scaleX <= 0.0f) scaleX = 1.0f;
                if (!std::isfinite(scaleY) || scaleY <= 0.0f) scaleY = 1.0f;
            }
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
            ov2->beginInlineTextEdit(
                QString::fromStdString(m_preEditOriginalText),
                QString::fromStdString(tl->fontFamily()),
                tl->fontSize() * scaleY,
                tl->fontWeight(),
                tl->isItalic(),
                textColor,
                scaleX / scaleY,
                hAlignFlag);
        });

        connect(ov2, &TransformOverlayWidget::inlineTextCommitted,
                this, [this, currentTextLayer](const QString& newText) {
            if (m_ws->isDestroying()) return;

            // Caption clip: commit the edited text back to the caption.
            if (m_ws->selection().clip && m_ws->selection().clip->isCaption()) {
                auto* cc = static_cast<CaptionClip*>(m_ws->selection().clip);
                const std::string newVal = newText.toStdString();
                const std::string oldVal = m_preEditOriginalText;
                m_inlineTextEditActive = false;
                m_preEditOriginalText.clear();
                auto capRefresh = [this]() {
                    m_ws->invalidateCompositeCache();
                    if (m_ws->programMonitor()) m_ws->programMonitor()->requestRefresh();
                    scheduleOverlayRefresh();
                    if (m_ws->timelinePanel()) m_ws->timelinePanel()->refreshTrackContents();
                    if (m_ws->captionsPanel()) m_ws->captionsPanel()->refresh();
                };
                if (newVal == oldVal) { cc->setText(oldVal); capRefresh(); return; }
                if (m_ws->commandStack()) {
                    m_ws->commandStack()->execute(std::make_unique<LambdaCommand>(
                        "Edit Caption Text",
                        [cc, newVal, capRefresh]() { cc->setText(newVal); capRefresh(); },
                        [cc, oldVal, capRefresh]() { cc->setText(oldVal); capRefresh(); }));
                } else { cc->setText(newVal); capRefresh(); }
                return;
            }

            TextLayer* tl = currentTextLayer();
            if (!tl) {
                m_inlineTextEditActive = false;
                return;
            }
            const std::string newVal = newText.toStdString();
            const std::string oldVal = m_preEditOriginalText;
            const bool wasActive = m_inlineTextEditActive;
            m_inlineTextEditActive = false;
            m_preEditOriginalText.clear();

            auto refresh = [this]() {
                if (m_ws->graphicsEditorPanel()) m_ws->graphicsEditorPanel()->refresh();
                m_ws->invalidateCompositeCache();
                if (m_ws->programMonitor()) m_ws->programMonitor()->requestRefresh();
                scheduleOverlayRefresh();
            };

            // If nothing changed (e.g. cancel/commit unchanged), restore
            // the original text without making an undo entry.
            if (newVal == oldVal) {
                if (wasActive) {
                    tl->setText(oldVal);
                    refresh();
                }
                return;
            }

            // Route through the command stack so Ctrl+Z reverts to the
            // pre-edit text. The layer is currently "" (cleared on begin),
            // so execute() sets it to newVal and undo restores oldVal.
            if (m_ws->commandStack()) {
                TextLayer* target = tl;
                m_ws->commandStack()->execute(std::make_unique<LambdaCommand>(
                    "Edit Text",
                    [target, newVal, refresh]() {
                        target->setText(newVal);
                        refresh();
                    },
                    [target, oldVal, refresh]() {
                        target->setText(oldVal);
                        refresh();
                    }));
            } else {
                tl->setText(newVal);
                refresh();
            }
        });
    }
}


} // namespace rt
