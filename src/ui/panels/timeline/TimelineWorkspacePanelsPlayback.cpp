/*
 * TimelineWorkspacePanelsPlayback.cpp — Playback signal wiring
 * extracted from TimelineWorkspacePanels.cpp::buildPanels().
 *
 * Contains: wirePlaybackSignals() — connects PlaybackController <-> Timeline
 * <-> Monitor scrub/position/state/composite/scopes wiring.
 */

#include "panels/timeline/TimelineWorkspace.h"

#include "CompositeService.h"
#include "playback/PlaybackScheduler.h"
#include "audio/AudioEngine.h"
#include "audio/AudioPlaybackService.h"
#include "playback/PlaybackController.h"
#include "panels/effects/EffectControlsPanel.h"
#include "panels/monitors/ScopesPanel.h"
#include "panels/monitors/ProgramMonitor.h"
#include "panels/monitors/SourceMonitor.h"
#include "panels/timeline/TimelinePanel.h"
#include "widgets/MiniTimeline.h"
#include "timeline/EditOperations.h"
#include "timeline/Timeline.h"
#include <QDockWidget>
#include <QTimer>

#include <spdlog/spdlog.h>

#include <chrono>

namespace rt {

void TimelineWorkspace::wirePlaybackSignals()
{
    if (!m_playbackController) return;

    // Scrubbing: user drags playhead on timeline ruler -> seek + audio scrub
    connect(m_timelinePanel, &TimelinePanel::playheadMoved,
            this, [this](int64_t tick) {
        auto scrubWireT0 = std::chrono::steady_clock::now();

        // Update the timeline toolbar timecode display
        if (m_timelineTimecode && m_timelinePanel) {
            auto tc = m_timelinePanel->layoutEngine().formatTimecode(tick);
            m_timelineTimecode->setText(QString::fromStdString(tc));
        }
        if (m_playbackController->isPlaying()) {
            m_playbackController->pause();
        }
        if (m_sourceMonitor && m_sourceMonitor->controller()
            && m_sourceMonitor->controller()->state() != PlayState::Stopped
            && m_sourceMonitor->controller()->state() != PlayState::Paused) {
            m_sourceMonitor->controller()->pause();
        }
        m_playbackController->seekTo(tick);
        ensureAudioSourcesLoaded();
        if (m_audioEngine && !m_playbackController->isPlaying()) {
            const uint32_t sr = m_audioEngine->sampleRate();
            const int64_t frame = static_cast<int64_t>(
                static_cast<double>(tick) / 48000.0 * sr);
            m_audioEngine->scrub(frame);
            if (m_meterTimer && !m_meterTimer->isActive())
                m_meterTimer->start();
        }
        if (m_programMonitor)
            m_programMonitor->notifyScrub();

        double scrubWireMs = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - scrubWireT0).count();
        if (scrubWireMs > 6.0) {
            spdlog::info("[PERF] scrubWiring: {:.1f}ms  tick={}", scrubWireMs, tick);
        }
    });

    // Scrubbing from ProgramMonitor mini-timeline
    connect(m_programMonitor, &ProgramMonitor::scrubbed,
            this, [this](int64_t tick) {
        m_playbackController->seekTo(tick);
        ensureAudioSourcesLoaded();
        if (m_audioEngine && !m_playbackController->isPlaying()) {
            const uint32_t sr = m_audioEngine->sampleRate();
            const int64_t frame = static_cast<int64_t>(
                static_cast<double>(tick) / 48000.0 * sr);
            m_audioEngine->scrub(frame);
            if (m_meterTimer && !m_meterTimer->isActive())
                m_meterTimer->start();
        }
    });

    // ProgramMonitor MiniTimeline in/out point changes -> Timeline
    connect(m_programMonitor->miniTimeline(), &MiniTimeline::inPointChanged,
            this, [this](int64_t tick) {
        if (m_timeline) {
            if (tick >= 0)
                EditOperations::setInPoint(*m_timeline, tick);
            else
                m_timeline->setInPoint(-1);
            if (m_timelinePanel) m_timelinePanel->updateInOutRange();
        }
    });
    connect(m_programMonitor->miniTimeline(), &MiniTimeline::outPointChanged,
            this, [this](int64_t tick) {
        if (m_timeline) {
            if (tick >= 0)
                EditOperations::setOutPoint(*m_timeline, tick);
            else
                m_timeline->setOutPoint(-1);
            if (m_timelinePanel) m_timelinePanel->updateInOutRange();
        }
    });

    // I/O key presses inside ProgramMonitor -> set in/out on timeline
    connect(m_programMonitor, &ProgramMonitor::inPointRequested,
            this, [this]() {
        if (m_timeline && m_playbackController) {
            EditOperations::setInPoint(*m_timeline, m_playbackController->currentTick());
            if (m_timelinePanel) m_timelinePanel->updateInOutRange();
            syncProgramMonitorInOut();
        }
    });
    connect(m_programMonitor, &ProgramMonitor::outPointRequested,
            this, [this]() {
        if (m_timeline && m_playbackController) {
            EditOperations::setOutPoint(*m_timeline, m_playbackController->currentTick());
            if (m_timelinePanel) m_timelinePanel->updateInOutRange();
            syncProgramMonitorInOut();
        }
    });

    // During playback: controller updates playhead position in timeline panel
    m_playbackController->onPositionChanged = [this](int64_t tick) {
        static int s_posThrottle = 0;
        const bool playing = m_playbackController &&
                             m_playbackController->isPlaying();
        const bool doUpdate = !playing || (++s_posThrottle % 3 == 0);

        if (doUpdate) {
            if (m_timelinePanel)
                m_timelinePanel->setPlayheadPosition(tick);
            if (m_effectControlsPanel)
                m_effectControlsPanel->setPlayheadTick(tick);
            if (m_timelineTimecode && m_timelinePanel) {
                auto tc = m_timelinePanel->layoutEngine().formatTimecode(tick);
                m_timelineTimecode->setText(QString::fromStdString(tc));
            }
            // Re-evaluate the transform overlay at the new playhead so any
            // animated scale/position/rotation keyframes update the program
            // monitor box in real time. Without this, the box stayed at the
            // value sampled at selection time and only refreshed when the
            // user deselected + reselected the clip.
            if (m_selection.clip)
                updateTransformOverlay();
        }
        // Keyboard transport (arrow keys, Home/End, edit-point nav) and any
        // other controller-driven seek while paused must re-composite the
        // Program Monitor — same mechanism the ruler-scrub path uses. During
        // playback the monitor is driven by its own pipeline, so skip it.
        if (!playing && m_programMonitor)
            m_programMonitor->notifyScrub();
        scheduleAudioPlaybackWindowRefresh();
    };

    // State change: load audio sources BEFORE playback starts
    m_playbackController->onStateChanged = [this](PlayState state) {
        if (state == PlayState::Playing || state == PlayState::Shuttling) {
            if (m_meterTimer && !m_meterTimer->isActive())
                m_meterTimer->start();
            if (m_sourceMonitor && m_sourceMonitor->controller()
                && m_sourceMonitor->controller()->state() != PlayState::Stopped
                && m_sourceMonitor->controller()->state() != PlayState::Paused) {
                m_sourceMonitor->controller()->pause();
            }
            if (m_audioPlayback && m_audioPlayback->needsPlaybackWindowRefresh()) {
                scheduleAudioPlaybackWindowRefresh();
            } else {
                warmAudioCacheAsync();
            }
        }
        if (m_programMonitor) {
            if (auto* pl = m_programMonitor->pipeline())
                pl->notifyStateChange();
        }
    };

    m_playbackController->onPlayStarting = [this](int64_t startTick) {
        if (m_programMonitor) {
            if (isBackgroundWarmupActive()) {
                spdlog::info("playback start deferred — background warmup still in progress");
                QTimer::singleShot(100, this, [this, startTick]() {
                    if (!isBackgroundWarmupActive() && m_programMonitor) {
                        prewarmPlaybackResources(startTick,
                                                 m_programMonitor->compositeWidth(),
                                                 m_programMonitor->compositeHeight());
                        m_programMonitor->requestPlaybackPreroll(startTick);
                    }
                });
                return;
            }
            prewarmPlaybackResources(startTick,
                                     m_programMonitor->compositeWidth(),
                                     m_programMonitor->compositeHeight());
            m_programMonitor->requestPlaybackPreroll(startTick);
        }
    };

    // Set composite callback
    m_programMonitor->setCompositeCallback(
        [this](int64_t tick, uint32_t w, uint32_t h, bool scrubMode) -> std::shared_ptr<CachedFrame> {
            return compositeFrame(tick, w, h, scrubMode);
        });

    // Wire playback-resolution dropdown
    if (m_compositeService) {
        m_programMonitor->setPlaybackTierCallback(
            [this](int divisor) {
                if (!m_compositeService) return;
                ResolutionTier tier = ResolutionTier::Full;
                if      (divisor >= 4) tier = ResolutionTier::Quarter;
                else if (divisor >= 2) tier = ResolutionTier::Half;
                m_compositeService->setPlaybackTier(tier);
            });
    }

    // Wire VRAM pressure query
    if (m_compositeService) {
        m_programMonitor->setVramQueryCallback([this]() -> int {
            return m_compositeService ? m_compositeService->vramUsagePercent() : 0;
        });
    }

    // Feed compositor output to Scopes
    if (m_scopesPanel) {
        connect(m_programMonitor, &ProgramMonitor::frameDisplayed,
                this, [this](int64_t /*tick*/) {
            auto* dockScopes = dockForPanel("Scopes");
            if (dockScopes && !dockScopes->isVisible()) return;

            auto frame = m_programMonitor->lastDisplayedFrame();
            if (!frame) return;
            if (!frame->ensurePixels()) return;
            if (frame->pixels.empty() || frame->width == 0 || frame->height == 0) return;
            m_scopesPanel->feedFrame(frame->pixels.data(),
                                     static_cast<int>(frame->width),
                                     static_cast<int>(frame->height));
        });
    }

    // Start the 60 fps polling timer
    m_programMonitor->startPolling();
}

} // namespace rt
