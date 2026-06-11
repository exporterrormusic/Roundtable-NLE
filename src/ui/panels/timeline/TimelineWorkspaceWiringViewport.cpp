// TimelineWorkspaceWiringViewport.cpp
// Software-viewport transform, overlay tool-change, and program-monitor
// content-refresh signal wiring for TimelineWorkspace.
// Extracted from TimelineWorkspacePanelsWiringClipSelection.cpp for file size.

#include <volk.h>

#include <cmath>
#include <map>
#include <set>

#include "panels/timeline/TimelineWorkspace.h"
#include "ClipRenderers.h"  // src/core/ClipRenderers.h — shared with the gpu module
#include "CompositeService.h"
#include "spine/AnimationVideoCache.h"
#include "Theme.h"

// ShotPanel removed � character/shot controls merged into PropertiesPanel
#include "panels/effects/EffectsPanel.h"
#include "panels/effects/KeyframeEditor.h"
#include "panels/monitors/ProgramMonitor.h"
#include "panels/project/ProjectBin.h"
#include "panels/properties/PropertiesPanel.h"
#include "panels/effects/EffectControlsPanel.h"
#include "panels/effects/GraphicsEditorPanel.h"
#include "panels/effects/ColorGradingPanel.h"
#include "panels/monitors/SourceMonitor.h"
#include "panels/timeline/TimelinePanel.h"

#include "widgets/MiniTimeline.h"
#include "widgets/DockTitleBar.h"
#include "widgets/VUMeter.h"
#include "viewport/Viewport.h"
#include "viewport/TransformOverlayWidget.h"

#include "command/CommandStack.h"
#include "command/CompoundCommand.h"
#include "command/LambdaCommand.h"
#include "command/commands/ClipCommands.h"
#include "command/commands/TrackCommands.h"
#include "command/commands/MarkerCommands.h"
#include "command/commands/TransitionCmds.h"
#include "command/commands/EffectCommands.h"
#include "project/Project.h"
#include "MainWindow.h"
#include "audio/AudioEngine.h"
#include "playback/MediaPool.h"
#include "playback/PlaybackController.h"
#include "timeline/AudioClip.h"
#include "timeline/EditOperations.h"
#include "timeline/ImageClip.h"
#include "timeline/OpacityMask.h"
#include "timeline/SequenceClip.h"
#include "timeline/SpineClip.h"
#include "timeline/TitleClip.h"
#include "timeline/VideoClip.h"
#include "timeline/GraphicClip.h"
#include "timeline/GraphicLayer.h"
#include "timeline/CaptionClip.h"
#include "panels/captions/CaptionsPanel.h"
#include "timeline/Timeline.h"
#include "timeline/Track.h"
#include "timeline/Transition.h"
#include "timeline/VideoClip.h"

#include "effects/ChromaKey.h"
#include "cache/FrameCache.h"
#include "audio/AudioPlaybackService.h"

#include "panels/characters/ShotComposerInternal.h"
#include "spine/ShotPreset.h"

#ifdef ROUNDTABLE_HAS_SPINE
#include "spine/ModelManager.h"
#endif

#include <QDockWidget>
#include <QFileInfo>
#include <QImage>
#include <QPainter>
#include <QTimer>
#include <spdlog/spdlog.h>

namespace rt {

void TimelineWorkspace::wireViewportTransformSignals()
{
    // -- Software Viewport transform signals: route to the SAME onOverlay*
    //    handlers the GPU TransformOverlayWidget uses (defined in
    //    TimelineWorkspaceWiringTransformOverlay.cpp).  These used to be
    //    duplicated lambda bodies that drifted apart — the Viewport copy
    //    was missing group-move support.  Keep them unified.
    if (m_programMonitor && m_programMonitor->viewport()) {
        auto* vp = m_programMonitor->viewport();
        connect(vp, &Viewport::transformPositionChanged,
                this, &TimelineWorkspace::onOverlayPositionChanged);
        connect(vp, &Viewport::transformScaleChanged,
                this, &TimelineWorkspace::onOverlayScaleChanged);
        connect(vp, &Viewport::transformRotationChanged,
                this, &TimelineWorkspace::onOverlayRotationChanged);
        connect(vp, &Viewport::transformDragFinished,
                this, &TimelineWorkspace::onOverlayDragFinished);
    }
}

void TimelineWorkspace::wireOverlayToolSignals()
{
    // -- Forward tool changes to TransformOverlayWidget -------------------
    if (m_timelinePanel && m_programMonitor && m_programMonitor->transformOverlay()) {
        auto* ov2 = m_programMonitor->transformOverlay();
        connect(m_timelinePanel, &TimelinePanel::toolChanged,
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
            if (!m_GraphicsEditorPanel) return nullptr;
            GraphicLayer* gl = m_GraphicsEditorPanel->selectedLayer();
            if (!gl || gl->layerType() != GraphicLayerType::Text) return nullptr;
            return static_cast<TextLayer*>(gl);
        };

        connect(ov2, &TransformOverlayWidget::textEditRequested,
                this, [this, ov2, currentTextLayer](float, float) {
            if (m_destroying.load(std::memory_order_acquire)) return;

            // Caption clip selected → edit the caption's text in place,
            // just like a graphic text layer.
            if (m_selectedClip && m_selectedClip->clipType() == ClipType::Caption) {
                auto* cc = static_cast<CaptionClip*>(m_selectedClip);
                m_preEditOriginalText = cc->text();
                m_inlineTextEditActive = true;
                updateTransformOverlay();
                cc->setText(std::string{});           // hide while editing
                invalidateCompositeCache();
                if (m_programMonitor) m_programMonitor->requestRefresh();
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

            // Sync m_selectedGraphicLayerIdx to the layer we're about to
            // edit. updateTransformOverlay()'s per-layer branch is gated
            // on this index (>= 0); when it's -1 the overlay falls back
            // to a full-frame box, whose centroid is the frame center —
            // making the editor appear dead-center on the first edit of a
            // freshly-created text clip (the GraphicsEditorPanel's
            // layerSelected signal hasn't propagated yet for a brand-new
            // clip auto-selected by setClip). selectedLayer() is the
            // authoritative source, so derive the index directly.
            if (m_selectedClip &&
                m_selectedClip->clipType() == ClipType::Graphic) {
                auto* gc = static_cast<GraphicClip*>(m_selectedClip);
                size_t idx = gc->findLayerIndex(tl->layerId());
                if (idx != SIZE_MAX)
                    m_selectedGraphicLayerIdx = static_cast<int>(idx);
            }

            // Force a synchronous overlay update with the current text bounds.
            updateTransformOverlay();

            // Now clear the text for compositing so it doesn't show behind
            // the editor box (Premiere Pro).
            tl->setText(std::string{});
            invalidateCompositeCache();
            if (m_programMonitor) m_programMonitor->requestRefresh();

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
            if (m_selectedClip && m_playbackController) {
                const int64_t localTick = std::max<int64_t>(
                    0, m_playbackController->currentTick() - m_selectedClip->timelineIn());
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
            if (m_destroying.load(std::memory_order_acquire)) return;

            // Caption clip: commit the edited text back to the caption.
            if (m_selectedClip && m_selectedClip->clipType() == ClipType::Caption) {
                auto* cc = static_cast<CaptionClip*>(m_selectedClip);
                const std::string newVal = newText.toStdString();
                const std::string oldVal = m_preEditOriginalText;
                m_inlineTextEditActive = false;
                m_preEditOriginalText.clear();
                auto capRefresh = [this]() {
                    invalidateCompositeCache();
                    if (m_programMonitor) m_programMonitor->requestRefresh();
                    scheduleOverlayRefresh();
                    if (m_timelinePanel) m_timelinePanel->refreshTrackContents();
                    if (m_captionsPanel) m_captionsPanel->refresh();
                };
                if (newVal == oldVal) { cc->setText(oldVal); capRefresh(); return; }
                if (m_commandStack) {
                    m_commandStack->execute(std::make_unique<LambdaCommand>(
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
                if (m_GraphicsEditorPanel) m_GraphicsEditorPanel->refresh();
                invalidateCompositeCache();
                if (m_programMonitor) m_programMonitor->requestRefresh();
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
            if (m_commandStack) {
                TextLayer* target = tl;
                m_commandStack->execute(std::make_unique<LambdaCommand>(
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

void TimelineWorkspace::wireTimelineContentSignals()
{
    // Refresh Program Monitor whenever timeline content changes
    if (m_timelinePanel && m_programMonitor) {
        connect(m_timelinePanel, &TimelinePanel::contentChanged,
                this, [this]() {
            if (m_destroying.load(std::memory_order_acquire)) return;
            invalidateCompositeCache();
            if (m_programMonitor) m_programMonitor->requestRefresh();
            // Defer spine warm-up + audio reload to next event-loop iteration
            // so razor splits don't block the UI thread.
            invalidateAudioSources();
            schedulePostEditWork();
        });

        // Also refresh when a new clip is created via tools (Text tool, etc.)
        connect(m_timelinePanel, &TimelinePanel::clipCreated,
                this, [this]() {
            if (m_destroying.load(std::memory_order_acquire)) return;
            invalidateCompositeCache();
            if (m_programMonitor) m_programMonitor->requestRefresh();
        });
    }

    // Refresh Program Monitor when its dock becomes visible again
    // (e.g. after switching tabs away and back). Without this the
    // viewport can show stale content from whatever was rendered at
    // that screen location while tabs were switched.
    auto* dockProgramMonitor = dockForPanel(QStringLiteral("Program Monitor"));
    if (dockProgramMonitor && m_programMonitor) {
        connect(dockProgramMonitor, &QDockWidget::visibilityChanged,
                this, [this](bool visible) {
            if (m_destroying.load(std::memory_order_acquire)) return;
            if (visible && m_programMonitor) {
                // Flush composite cache so we re-render from current state
                invalidateCompositeCache();
                m_programMonitor->requestRefresh();
            }
        });
    }
}

} // namespace rt
