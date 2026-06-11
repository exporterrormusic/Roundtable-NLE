// TimelineWorkspacePanelsWiring.cpp - Signal wiring for TimelineWorkspace.
// Split from TimelineWorkspacePanels.cpp for maintainability.

#include <volk.h>

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
#include "command/LambdaCommand.h"
#include "command/commands/ClipCommands.h"
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
#include <QMessageBox>
#include <QPainter>
#include <QTimer>
#include <spdlog/spdlog.h>

namespace rt {



void TimelineWorkspace::wirePanelSignals()
{
    // =====================================================================
    //  VU METER POLLING -- feed AudioEngine::meter() -> timeline VU meter
    // =====================================================================
    m_meterTimer = new QTimer(this);
    m_meterTimer->setInterval(33); // ~30 Hz
    connect(m_meterTimer, &QTimer::timeout, this, [this]() {
        if (m_destroying.load(std::memory_order_acquire)) return;
        if (!m_audioEngine || !m_timelineVUMeter) return;
        auto state = m_audioEngine->transportState();
        if (state != TransportState::Playing && state != TransportState::Scrubbing) {
            // Decay to silence when not playing
            float curL = m_timelineVUMeter->level(0);
            float curR = m_timelineVUMeter->level(1);
            if (curL < 0.001f && curR < 0.001f) {
                // Fully decayed � stop polling until next play/scrub
                m_timelineVUMeter->setLevel(0, 0.0f);
                m_timelineVUMeter->setLevel(1, 0.0f);
                m_meterTimer->stop();
                return;
            }
            constexpr float decay = 0.85f;
            m_timelineVUMeter->setLevel(0, curL * decay < 0.001f ? 0.0f : curL * decay);
            m_timelineVUMeter->setLevel(1, curR * decay < 0.001f ? 0.0f : curR * decay);
            return;
        }
        auto m = m_audioEngine->meter();
        m_timelineVUMeter->setLevel(0, m.peakL);
        m_timelineVUMeter->setLevel(1, m.peakR);
    });
    // Don't start yet � started on play/scrub via onStateChanged

    // =====================================================================
    wireClipSelectionSignals();
    // -- ShotPanel wiring removed: character/shot controls merged into PropertiesPanel --

    
    wireMediaDropSignals();
    wireNestSignals();

    wireEffectDropSignals();

    wireTrackSignals();

    // =====================================================================
    //  EFFECTS PANEL -> REFRESH EFFECT CONTROLS WHEN EFFECTS CHANGE
    // =====================================================================
    if (m_effectsPanel && m_effectControlsPanel) {
        auto refreshAfterEffectChange = [this]() {
            if (m_destroying.load(std::memory_order_acquire)) return;
            if (m_effectControlsPanel) m_effectControlsPanel->refresh();
            invalidateCompositeCache();
            if (m_programMonitor) m_programMonitor->requestRefresh();
        };
        connect(m_effectsPanel, &EffectsPanel::effectAdded,
                this, refreshAfterEffectChange);
        connect(m_effectsPanel, &EffectsPanel::effectRemoved,
                this, refreshAfterEffectChange);
        connect(m_effectsPanel, &EffectsPanel::effectMoved,
                this, refreshAfterEffectChange);
    }

    m_panelsBuilt = true;
}

} // namespace rt

