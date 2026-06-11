// TimelineWorkspacePanelsWiringTrack.cpp - Track management signal wiring.
// Extracted from TimelineWorkspacePanelsWiring.cpp for maintainability.

#include <volk.h>

#include <map>
#include <set>

#include "panels/timeline/TimelineWorkspace.h"
#include "ClipRenderers.h"  // src/core/ClipRenderers.h — shared with the gpu module
#include "CompositeService.h"
#include "spine/AnimationVideoCache.h"
#include "Theme.h"

// ShotPanel removed — character/shot controls merged into PropertiesPanel
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
#include "command/LambdaCommand.h"
#include "command/commands/ClipCommands.h"
#include "command/commands/MarkerCommands.h"
#include "command/commands/TransitionCmds.h"
#include "command/commands/EffectCommands.h"
#include "project/Project.h"
#include "MainWindow.h"
#include "media/AudioEngine.h"
#include "media/MediaPool.h"
#include "media/PlaybackController.h"
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
#include "timeline/Timeline.h"
#include "timeline/Track.h"
#include "timeline/Transition.h"
#include "timeline/VideoClip.h"

#include "effects/ChromaKey.h"
#include "media/FrameCache.h"
#include "media/AudioPlaybackService.h"

#include "panels/characters/ShotComposerInternal.h"
#include "spine/ShotPreset.h"

#ifdef ROUNDTABLE_HAS_SPINE
#include "spine/ModelManager.h"
#endif

#include <QDockWidget>
#include <QFileInfo>
#include <QImage>
#include <QMessageBox>
#include <QPainter>
#include <QTimer>
#include <spdlog/spdlog.h>

namespace rt {

void TimelineWorkspace::wireTrackSignals()
{
// =====================================================================
    //  TRACK MANAGEMENT -- add/delete tracks from context menus
    // =====================================================================
    if (m_timelinePanel && m_timeline) {
        // Pick a sensible height for a freshly-inserted track. The Track
        // default is 80 px, but the user may have resized existing tracks —
        // matching the neighbour's height avoids a jarring size mismatch
        // when "Add Track Above/Below" pops a new lane in.
        auto resolveNewTrackHeight = [this](size_t nearIndex) {
            if (!m_timeline) return 80.0f;
            auto sample = [this](size_t i) -> float {
                if (i >= m_timeline->trackCount()) return -1.0f;
                Track* t = m_timeline->track(i);
                if (!t || t->isDivider()) return -1.0f;
                float h = t->height();
                return (h >= 1.0f) ? h : -1.0f;
            };
            float h = sample(nearIndex);
            if (h > 0.0f) return h;
            // Fall back to any non-divider neighbour.
            for (size_t i = 0; i < m_timeline->trackCount(); ++i) {
                float hh = sample(i);
                if (hh > 0.0f) return hh;
            }
            return 80.0f;
        };

        // Insert a fresh track at `insertAt` undoably and refresh the
        // dependent UI/audio routing. The redo lambda re-runs the same
        // path; the undo lambda yanks the track back out at the same index.
        auto addTrackUndoable = [this](size_t insertAt, bool video, float height) {
            if (!m_timeline) return;
            if (insertAt > m_timeline->trackCount())
                insertAt = m_timeline->trackCount();

            auto redo = [this, insertAt, video, height]() {
                if (m_destroying.load(std::memory_order_acquire)) return;
                if (!m_timeline) return;
                auto t = std::make_unique<Track>(
                    video ? TrackType::Video : TrackType::Audio, "");
                t->setHeight(height);
                m_timeline->insertTrack(insertAt, std::move(t));
                if (m_timelinePanel) m_timelinePanel->rebuildTracks();
                invalidateAudioSources();
            };
            auto undo = [this, insertAt]() {
                if (m_destroying.load(std::memory_order_acquire)) return;
                if (!m_timeline) return;
                if (insertAt >= m_timeline->trackCount()) return;
                // Clear selection state so we don't dangle a Clip* belonging
                // to the removed track.
                m_selectedClip = nullptr;
                if (m_propertiesPanel) m_propertiesPanel->clearClip();
                m_timeline->removeTrack(insertAt);
                if (m_timelinePanel) m_timelinePanel->rebuildTracks();
                invalidateAudioSources();
            };

            if (m_commandStack) {
                m_commandStack->execute(std::make_unique<LambdaCommand>(
                    video ? "Add Video Track" : "Add Audio Track",
                    std::move(redo), std::move(undo)));
            } else {
                redo();
            }
        };

        connect(m_timelinePanel, &TimelinePanel::addTrackAbove,
                this, [this, addTrackUndoable, resolveNewTrackHeight](size_t nearIndex, bool video) {
            if (m_destroying.load(std::memory_order_acquire)) return;
            if (!m_timeline) return;
            size_t idx = nearIndex < m_timeline->trackCount() ? nearIndex : 0;
            float h = resolveNewTrackHeight(nearIndex);
            addTrackUndoable(idx, video, h);
        });
        connect(m_timelinePanel, &TimelinePanel::addTrackBelow,
                this, [this, addTrackUndoable, resolveNewTrackHeight](size_t nearIndex, bool video) {
            if (m_destroying.load(std::memory_order_acquire)) return;
            if (!m_timeline) return;
            size_t idx = nearIndex < m_timeline->trackCount() ? nearIndex + 1 : m_timeline->trackCount();
            float h = resolveNewTrackHeight(nearIndex);
            addTrackUndoable(idx, video, h);
        });
        connect(m_timelinePanel, &TimelinePanel::deleteTrack,
                this, [this](size_t trackIndex) {
            if (m_destroying.load(std::memory_order_acquire)) return;
            if (!m_timeline || m_timeline->trackCount() <= 1) return;
            if (trackIndex >= m_timeline->trackCount()) return;
            // The permanent V/A divider is auto-managed; if removed it
            // would just be re-created by ensureSectionDivider on the
            // next rebuild. Reject the delete so the result matches the
            // user's intent (no flicker, no recreate).
            if (auto* tr = m_timeline->track(trackIndex);
                    tr && (tr->isPermanentDivider() || tr->isCaptionTrack()))
                return;

            // Clear selection state BEFORE removing the track so we don't
            // hold a dangling Clip* that belonged to the deleted track.
            m_selectedClip = nullptr;
            m_selectedTrackIdx = 0;
            m_selectedClipIdx  = 0;
            m_selectedGraphicLayerIdx = -1;
            if (m_propertiesPanel) m_propertiesPanel->clearClip();

            m_timeline->removeTrack(trackIndex);
            m_timelinePanel->rebuildTracks();
            invalidateAudioSources();
            invalidateCompositeCache();
            if (m_programMonitor) m_programMonitor->requestRefresh();
        });

        connect(m_timelinePanel, &TimelinePanel::clipContextAction,
                this, [this](const QString& action, size_t trackIndex, uint64_t clipId) {
            if (m_destroying.load(std::memory_order_acquire)) return;
            Q_UNUSED(action); Q_UNUSED(trackIndex); Q_UNUSED(clipId);
        });
    }
}

} // namespace rt
