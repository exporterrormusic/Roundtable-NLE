// TimelineWorkspaceWiringViewport.cpp
// Program-monitor content-refresh signal wiring for TimelineWorkspace.
// (Viewport transform + overlay tool wiring moved to OverlayController.)
// Extracted from TimelineWorkspaceWiringClipSelection.cpp for file size.

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
