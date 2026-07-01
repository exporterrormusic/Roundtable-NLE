/*
 * MainWindowProjectSet.cpp — setCurrentProject() extracted from
 * MainWindowProject.cpp.
 *
 * Contains: setCurrentProject() — the main project-lifecycle handler that
 * stops old project, wires new project to all subsystems (timeline,
 * playback controller, export panel, project bin, etc.).
 */

#include "ProjectController.h"
#include "MainWindow.h"

#include "panels/audio/AudioSync.h"
#include "panels/export/ExportPanel.h"
#include "panels/project/ProjectPanel.h"
#include "panels/project/ProjectBin.h"
#include "panels/timeline/TimelineWorkspace.h"
#include "panels/timeline/TimelinePanel.h"
#include "panels/monitors/ProgramMonitor.h"
#include "panels/monitors/SourceMonitor.h"
#include "panels/effects/EffectControlsPanel.h"

#include "command/CommandStack.h"
#include "audio/AudioEngine.h"
#include "playback/MediaPool.h"
#include "playback/PlaybackController.h"
#include "playback/PlaybackScheduler.h"
#include "timeline/Timeline.h"
#include "timeline/Track.h"
#include "timeline/Clip.h"
#include "timeline/SpineClip.h"
#include "timeline/VideoClip.h"

#include "project/Project.h"
#include "project/ProjectSerializer.h"

#include <QApplication>
#include <QDir>
#include <QMessageBox>
#include <QStatusBar>

#include <spdlog/spdlog.h>

#include <chrono>
#include <filesystem>

namespace rt {

void ProjectController::setCurrentProject(std::unique_ptr<Project> project)
{
    // ── Pre-move cleanup: stop all background operations BEFORE destroying
    // the old project.  The old project's timeline is about to be destroyed
    // via std::move, but raw pointers (m_mw->timeline(), etc.)  and background
    // threads (FrameProducer, audio playback) may still reference it.
    // Without this, a concurrent composite or audio callback reads a
    // dangling timeline pointer → crash during project switching.
    if (m_mw->currentProject()) {
        // 1. Stop audio playback / transport
        if (m_mw->playbackController() && m_mw->playbackController()->isPlaying())
            m_mw->playbackController()->stop();

        // 2. Stop the async composite pipeline (FrameProducer thread)
        if (m_mw->timelineWorkspace()) {
            if (auto* pm = m_mw->timelineWorkspace()->programMonitor()) {
                pm->stopPolling();
                if (auto* pl = pm->pipeline())
                    pl->stop();
            }
        }
        // Also stop source monitor pipeline
        if (m_mw->timelineWorkspace()) {
            if (auto* sm = m_mw->timelineWorkspace()->sourceMonitor()) {
                if (auto* ctrl = sm->controller()) {
                    if (ctrl->isPlaying()) ctrl->stop();
                }
            }
        }

        // 3. Disconnect old timeline from composite service so the next
        //    poll-timer tick or producer thread doesn't access destroyed data
        if (m_mw->timelineWorkspace()) {
            if (auto* pm = m_mw->timelineWorkspace()->programMonitor())
                pm->setCompositeCallback(nullptr);
            m_mw->timelineWorkspace()->setTimeline(nullptr);
        }
        if (m_mw->playbackController())
            m_mw->playbackController()->setTimeline(nullptr);
        // Also disconnect the export panel — otherwise when it is later
        // wired to the new project's timeline, ExportPanel::setTimeline
        // will call removeObserver on the already-destroyed old timeline.
        if (m_mw->exportPanel())
            m_mw->exportPanel()->setTimeline(nullptr);
        m_mw->setTimeline(nullptr);
    }

    // Now safe to destroy the old project and set up the new one
    m_mw->adoptCurrentProject(std::move(project));
    m_lastSavedAudioSyncBlob = m_mw->currentProject() ? m_mw->currentProject()->audioSyncBlob()
                                               : std::vector<uint8_t>{};

    if (m_mw->currentProject()) {
        // Reset per-project panels so old state doesn't leak into the new project.
        // Scripts, audio, clips, and sessions from the previous project are cleared.
        if (m_mw->audioSync()) {
            m_mw->audioSync()->resetForNewProject();
            // Per-show default shots: tell AudioSync which show this project
            // belongs to so resolveDefaultShot() picks show-specific defaults.
            m_mw->audioSync()->setCurrentShow(m_mw->currentProject()->show());
        }

        // Release all media from the old project and clear the frame cache
        if (m_mw->mediaPool()) {
            m_mw->mediaPool()->closeAll();
            m_mw->mediaPool()->clearFailedPaths();  // retry files that failed transiently
        }

        // Clear undo/redo history so old commands don't apply to the new project
        if (m_mw->commandStack())
            m_mw->commandStack()->clear();

        QString name = QString::fromStdString(m_mw->currentProject()->name());
        if (m_mw->projectPanel())
            m_mw->projectPanel()->setCurrentProjectName(name);
        if (auto* bin = m_mw->projectBin())
            bin->setProjectName(name);
        m_mw->setWindowTitle(QString("ROUNDTABLE NLE %1 \u2014 %2").arg(ROUNDTABLE_VERSION).arg(name));

        // Wire the project's timeline to ALL consumers so every subsystem
        // reads/writes the same timeline.  Without this, export writes to
        // the App's default timeline while playback reads from the project's
        // timeline, resulting in 0 audio sources loaded.
        if (m_mw->currentProject()->timeline()) {
            Timeline* projTimeline = m_mw->currentProject()->timeline();

            // Update MainWindow's own pointer (used by export handler)
            m_mw->setTimeline(projTimeline);

            // Update workspace (TimelinePanel + compositeFrame + loadAudioSources)
            if (m_mw->timelineWorkspace()) {
                m_mw->timelineWorkspace()->setTimeline(projTimeline);
                // MUST set project BEFORE the export preview callback fires,
                // otherwise CompositeService::m_project is null and nested
                // SequenceClips silently render as blank (no effects, no
                // inner timeline, and z-ordering is broken).
                m_mw->timelineWorkspace()->setProject(m_mw->currentProject());
            }

            // Update PlaybackController (transport / duration queries)
            if (m_mw->playbackController())
                m_mw->playbackController()->setTimeline(projTimeline);

            // Update ExportPanel
            if (m_mw->exportPanel()) {
                m_mw->exportPanel()->setTimeline(projTimeline);
                m_mw->exportPanel()->setProject(m_mw->currentProject());
                if (m_mw->playbackController()) m_mw->exportPanel()->setPlaybackController(m_mw->playbackController());
                if (m_mw->audioEngine()) m_mw->exportPanel()->setAudioEngine(m_mw->audioEngine());
                m_mw->exportPanel()->setPreviewCallback(
                    [this](int64_t tick, uint32_t w, uint32_t h, bool scrub)
                        -> std::shared_ptr<CachedFrame> {
                        if (m_mw->isDestroying()) return nullptr;
                        if (m_mw->timelineWorkspace()) {
                            m_mw->timelineWorkspace()->setForceFullResolution(true);
                            auto result = m_mw->timelineWorkspace()->compositeFrame(tick, w, h, scrub);
                            m_mw->timelineWorkspace()->setForceFullResolution(false);
                            return result;
                        }
                        return nullptr;
                    });
            }

            spdlog::info("setCurrentProject: all subsystems wired to project timeline (tracks={})",
                         projTimeline->trackCount());

            // Wire project to ProjectBin for sequence management.
            // IMPORTANT: clearAll() must come BEFORE setProject() so that
            // syncListView() (called inside setProject) rebuilds from an
            // empty grid, not from the previous project's stale items.
            // Also force a tree/sync refresh in both view modes since
            // setProject() only calls syncListView() when in list view.
            if (auto* bin = m_mw->projectBin()) {
                bin->clearAll();
                bin->setCommandStack(m_mw->commandStack());

                // Restore bin media files from saved state only.
                auto savedFiles = m_mw->currentProject()->binFiles(); // copy
                spdlog::info("setCurrentProject: project binFiles={} binFolders={}",
                             savedFiles.size(), m_mw->currentProject()->binFolders().size());

                bin->setProject(m_mw->currentProject());

                // Force-sync both views so the tree and grid are rebuilt
                // from the new project regardless of current view mode.
                bin->refreshAllViews();

                // Build folder state once (shared by both restore paths).
                const auto& projFolders = m_mw->currentProject()->binFolders();
                std::vector<BinFolderState> uiFolders;
                uiFolders.reserve(projFolders.size());
                for (const auto& pf : projFolders) {
                    BinFolderState bf;
                    bf.name      = pf.name;
                    bf.expanded  = pf.expanded;
                    bf.childKeys = pf.childKeys;
                    uiFolders.push_back(std::move(bf));
                }

                const auto& projItems = m_mw->currentProject()->binItems();
                if (!projItems.empty()) {
                    // v14+: rich per-instance restore (footage
                    // reference-duplicates + folder membership survive).
                    bin->restoreBinModel(projItems, uiFolders);
                } else {
                    // Legacy (pre-v14) projects: flat path list.
                    if (!savedFiles.empty())
                        bin->addFiles(savedFiles);
                    if (!uiFolders.empty())
                        bin->restoreBinFolders(uiFolders);
                }
                spdlog::info("setCurrentProject: restored {} items / {} files, {} folders",
                             projItems.size(), savedFiles.size(),
                             projFolders.size());
            }

            // Wire project to TimelineWorkspace for sequence tabs
            if (m_mw->timelineWorkspace())
                m_mw->timelineWorkspace()->setProject(m_mw->currentProject());

            // ── Apply project framerate to all consumers ────────────────
            // Without this, the PlaybackController stays at its default
            // (24fps) and the ProgramMonitor gates compositing to 24fps
            // regardless of what the project actually specifies.
            {
                double fps = m_mw->currentProject()->settings().frameRate();
                if (fps < 1.0) fps = 60.0; // sane default

                if (m_mw->playbackController())
                    m_mw->playbackController()->setFrameRate(fps);

                if (m_mw->timelineWorkspace() && m_mw->timelineWorkspace()->timelinePanel())
                    m_mw->timelineWorkspace()->timelinePanel()->setFrameRate(fps);

                spdlog::info("setCurrentProject: applied project framerate {:.1f} fps", fps);
            }

            // Push sequence resolution to Effect Controls so the Position
            // display converts internal REF-1920 px ↔ sequence pixels.
            if (auto* ec = m_mw->effectControlsPanel()) {
                const auto& res = m_mw->currentProject()->settings().resolution();
                ec->setSequenceResolution(res.width, res.height);
            }

            // ── Migrate stale normalized positions from old exports ─────
            // Old export code stored normalized 0–1 values directly as pixel
            // offsets.  Detect this (both posX/posY in (0.01,0.99)) and
            // convert to real pixel offsets.
            for (size_t ti = 0; ti < projTimeline->trackCount(); ++ti) {
                Track* track = projTimeline->track(ti);
                if (!track || track->type() != TrackType::Video) continue;

                for (size_t ci = 0; ci < track->clipCount(); ++ci) {
                    Clip* clip = track->clip(ci);
                    if (!clip) continue;

                    // Only migrate SpineClip / VideoClip on video tracks
                    if (clip->clipType() != ClipType::Spine &&
                        clip->clipType() != ClipType::Video)
                        continue;

                    float px = clip->positionX().evaluate(0);
                    float py = clip->positionY().evaluate(0);

                    // Heuristic: if both are in (0.01, 0.99), they are normalized
                    // coordinates from the old export (pixel offsets are typically
                    // hundreds of pixels, well outside this range).
                    if (px > 0.01f && px < 0.99f && py > 0.01f && py < 0.99f) {
                        float newPx = (px - 0.5f) * 1920.0f;
                        float newPy = (py - 0.5f) * 1080.0f;
                        clip->positionX().addKeyframe(0, newPx);
                        clip->positionY().addKeyframe(0, newPy);
                        spdlog::info("  Migrated clip '{}' position: ({:.3f},{:.3f}) → ({:.1f},{:.1f})",
                                     clip->label(), px, py, newPx, newPy);
                    }
                }
            }
        }

        // Wire the App-level CommandStack to mark the project modified
        // whenever any command is executed/undone/redone.  The Project has
        // its own CommandStack with a change callback, but UI panels use
        // the App-level one — so without this, isModified() stays false
        // and auto-save never triggers.
        if (m_mw->commandStack()) {
            m_mw->commandStack()->setChangeCallback([this]() {
                if (m_mw->isDestroying()) return;
                if (m_mw->currentProject())
                    m_mw->currentProject()->setModified(true);
            });
        }

        // Restart the auto-save countdown so a freshly-opened project gets a
        // full interval before its first auto-save.  Without this the timer
        // free-runs from app launch, so opening a project late in a session
        // could auto-save seconds later — which then triggers the recovery
        // prompt on the next launch even for a quick "open to test" session.
        m_mw->resetAutoSaveTimer();
    } else {
        if (m_mw->projectPanel())
            m_mw->projectPanel()->setCurrentProjectName({});
        if (auto* bin = m_mw->projectBin())
            bin->setProjectName({});
        m_mw->setWindowTitle(QString("ROUNDTABLE NLE %1").arg(ROUNDTABLE_VERSION));
    }
}

// ═════════════════════════════════════════════════════════════════════════════

} // namespace rt
