/*
 * TimelineWorkspacePanelsMonitors.cpp — the Source and Program Monitor
 * docks, and what the Source Monitor does with a dropped Spine animation
 * or sequence. Part of createPanelWidgets().
 */

#include "panels/timeline/TimelineWorkspace.h"

#include "CompositeService.h"
#include "Constants.h"
#include "command/CommandStack.h"
#include "command/LambdaCommand.h"
#include "command/commands/MarkerCommands.h"
#include "edit/EditSession.h"
#include "timeline/EditOperations.h"
#include "panels/timeline/TimelinePanel.h"
#include "playback/PlaybackController.h"
#include "widgets/DockTitleBar.h"
#include "Settings.h"
#include "cache/FrameCache.h"
#include "panels/monitors/ProgramMonitor.h"
#include "panels/monitors/SourceMonitor.h"
#include "playback/EngineContracts.h"
#include "playback/MediaPool.h"
#include "project/Project.h"
#include "spine/AnimationVideoCache.h"
#include "timeline/SpineClip.h"
#include "timeline/Timeline.h"

#include <QDockWidget>
#include <QPointer>
#include <QStringList>
#include <QTimer>

#include <algorithm>
#include <memory>
#include <string>

namespace rt {

void TimelineWorkspace::createMonitorDocks()
{
    // -- Source Monitor ---------------------------------------------------
    m_sourceMonitor = new SourceMonitor(this);
    m_sourceMonitor->setMinimumWidth(200);
    if (m_mediaPool)
        m_sourceMonitor->setMediaPool(m_mediaPool);
    if (m_mediaSourceService)
        m_sourceMonitor->setMediaSourceService(m_mediaSourceService);
    if (m_audioEngine)
        m_sourceMonitor->setAudioEngine(m_audioEngine);

    connect(m_sourceMonitor, &SourceMonitor::dropReceived,
            this, [this](uint64_t handle) {
        if (m_destroying.load(std::memory_order_acquire)) return;
        if (m_mediaPool && m_sourceMonitor)
            m_sourceMonitor->loadClip(handle, m_mediaPool);
    });

    connect(m_sourceMonitor, &SourceMonitor::spineDropReceived,
            this, [this](const QString& uri) {
        if (m_destroying.load(std::memory_order_acquire)) return;
        loadSpineInSourceMonitor(uri);
    });

    connect(m_sourceMonitor, &SourceMonitor::sequenceDropReceived,
            this, [this](size_t seqIdx) {
        if (m_destroying.load(std::memory_order_acquire)) return;
        loadSequenceInSourceMonitor(seqIdx);
    });

    connect(m_sourceMonitor, &SourceMonitor::playbackStarted,
            this, [this]() {
        if (m_destroying.load(std::memory_order_acquire)) return;
        setSourceTransportActive(true);
        invalidateAudioSources();
    });

    // Scrubbing the Source Monitor mini-timeline pauses main timeline
    // playback so only one monitor is actively driving audio/video.
    connect(m_sourceMonitor, &SourceMonitor::scrubbed,
            this, [this]() {
        if (m_destroying.load(std::memory_order_acquire)) return;
        setSourceTransportActive(true);
    });

    // The tab reads "Source: <clip>" (Premiere), and the clip's markers
    // come from the project, whenever the loaded clip changes.
    connect(m_sourceMonitor, &SourceMonitor::clipChanged,
            this, [this]() {
        if (m_destroying.load(std::memory_order_acquire)) return;
        refreshSourceMonitorMarkers();
        if (QDockWidget* dock = m_dockWidgets.value(QStringLiteral("Source Monitor"))) {
            const QString title = m_sourceMonitor->panelTitle();
            dock->setWindowTitle(title);
            if (auto* bar = qobject_cast<DockTitleBar*>(dock->titleBarWidget()))
                bar->setTitle(title);
        }
    });

    // Clip markers: Add Marker (button or M) and the ruler menu.
    connect(m_sourceMonitor, &SourceMonitor::addMarkerRequested,
            this, [this](int64_t tick) {
        editSourceClipMarkers(tr("Add Marker"), [tick](std::vector<Marker>& markers) {
            const bool taken = std::any_of(markers.begin(), markers.end(),
                                           [tick](const Marker& m) { return m.time == tick; });
            if (!taken) markers.push_back({tick, "Marker"});
        });
    });
    connect(m_sourceMonitor, &SourceMonitor::clearMarkerRequested,
            this, [this](int64_t tick) {
        editSourceClipMarkers(tr("Clear Marker"), [tick](std::vector<Marker>& markers) {
            std::erase_if(markers, [tick](const Marker& m) { return m.time == tick; });
        });
    });
    connect(m_sourceMonitor, &SourceMonitor::clearAllMarkersRequested,
            this, [this]() {
        editSourceClipMarkers(tr("Clear All Markers"),
                              [](std::vector<Marker>& markers) { markers.clear(); });
    });
    m_sourceMonitor->setShortcutManager(m_shortcutManager);

    makeDock("Source Monitor", m_sourceMonitor);

    // -- Program Monitor --------------------------------------------------
    m_programMonitor = new ProgramMonitor(this);
    m_programMonitor->setMinimumWidth(200);
    m_programMonitor->setMinimumHeight(190);
    if (m_timeline) m_programMonitor->setTimeline(m_timeline);
    if (m_playbackController) m_programMonitor->setController(m_playbackController);
    connect(m_programMonitor, &ProgramMonitor::exportFrameRequested,
            this, &TimelineWorkspace::exportCurrentFrame);
    connect(m_programMonitor, &ProgramMonitor::addMarkerRequested,
            this, &TimelineWorkspace::addSequenceMarkerAtPlayhead);
    connect(m_programMonitor, &ProgramMonitor::liftRequested,
            this, &TimelineWorkspace::liftInOutRange);
    connect(m_programMonitor, &ProgramMonitor::extractRequested,
            this, &TimelineWorkspace::extractInOutRange);
    m_programMonitor->setShortcutManager(m_shortcutManager);
    auto* programMonitorDock = makeDock("Program Monitor", m_programMonitor);
    connect(programMonitorDock, &QDockWidget::topLevelChanged,
            m_programMonitor, [monitor = m_programMonitor](bool) {
        // Native viewport geometry settles after Qt completes the dock move.
        QTimer::singleShot(0, monitor, [monitor]() {
            monitor->resetViewState();
        });
    });
}

// ── Monitor button-row edits (also the M, ; and ' keys) ─────────────────────

void TimelineWorkspace::addSequenceMarkerAtPlayhead()
{
    if (!m_timeline || !m_playbackController || !m_commandStack) return;
    const int64_t tick = m_playbackController->currentTick();
    executeTimelineEdit(m_commandStack, m_timeline,
                        std::make_unique<AddMarkerCommand>(m_timeline, tick, "Marker"));
    if (m_timelinePanel) m_timelinePanel->update();
}

void TimelineWorkspace::liftInOutRange()
{
    if (!m_timeline || !m_commandStack) return;
    const int64_t inPt = m_timeline->inPoint();
    const int64_t outPt = m_timeline->outPoint();
    if (inPt < 0 || outPt < 0 || outPt <= inPt) return;
    if (auto cmd = EditOperations::liftInOut(*m_timeline, inPt, outPt)) {
        executeTimelineEdit(m_commandStack, m_timeline, std::move(cmd));
        applyEditImpact();
    }
}

void TimelineWorkspace::extractInOutRange()
{
    if (!m_timeline || !m_commandStack) return;
    const int64_t inPt = m_timeline->inPoint();
    const int64_t outPt = m_timeline->outPoint();
    if (inPt < 0 || outPt < 0 || outPt <= inPt) return;
    if (auto cmd = EditOperations::extractInOut(*m_timeline, inPt, outPt)) {
        executeTimelineEdit(m_commandStack, m_timeline, std::move(cmd));
        applyEditImpact();
    }
}

// ── Source Monitor clip markers ─────────────────────────────────────────────

void TimelineWorkspace::refreshSourceMonitorMarkers()
{
    if (!m_sourceMonitor) return;
    const auto& media = m_sourceMonitor->mediaPath();
    m_sourceMonitor->setClipMarkers(m_project && !media.empty()
                                        ? m_project->sourceMarkersFor(media)
                                        : std::vector<Marker>{});
}

void TimelineWorkspace::editSourceClipMarkers(
    const QString& description, const std::function<void(std::vector<Marker>&)>& edit)
{
    if (!m_project || !m_commandStack || !m_sourceMonitor) return;
    const std::filesystem::path media = m_sourceMonitor->mediaPath();
    if (media.empty()) return;

    const std::vector<Marker> before = m_project->sourceMarkersFor(media);
    std::vector<Marker> after = before;
    edit(after);
    if (after.size() == before.size()) return;   // nothing added or removed

    // Not a timeline edit, so the stack is called directly. The project is
    // resolved when the step runs (the stack is cleared when projects change).
    QPointer<TimelineWorkspace> self(this);
    auto apply = [self, media](const std::vector<Marker>& markers) {
        if (!self || !self->m_project) return;
        self->m_project->setSourceMarkersFor(media, markers);
        self->refreshSourceMonitorMarkers();
    };
    m_commandStack->execute(std::make_unique<LambdaCommand>(
        description.toStdString(),
        [apply, after]() { apply(after); },
        [apply, before]() { apply(before); }));
}

// Spine character animation dropped from the Library: parse the URI
// ("spine:CharName|outfit|stanceInt|animName"), prefer a cached video
// (animVideoCache → loadClip) so it plays exactly like the timeline
// shows it, and fall back to live spine rendering via loadSpineClip().
void TimelineWorkspace::loadSpineInSourceMonitor(const QString& uri)
{
    if (!m_sourceMonitor) return;
    // Strip "spine:" prefix
    QString payload = uri.startsWith(QStringLiteral("spine:"))
                         ? uri.mid(6) : uri;
    QStringList parts = payload.split('|');
    if (parts.size() < 4) return;
    const std::string charName  = parts[0].toStdString();
    const std::string outfit    = parts[1].toStdString();
    const int         stanceInt = parts[2].toInt();
    const std::string animName  = parts[3].toStdString();

#ifdef ROUNDTABLE_HAS_SPINE
    // Try the pre-rendered animation video first — same path the
    // double-click-on-timeline-clip handler uses, so the user sees an
    // identical preview from either entry point.
    if (m_compositeService && m_compositeService->animVideoCache()) {
        uint64_t handle = m_compositeService->animVideoCache()->getMediaHandle(
            charName, outfit, animName);
        if (handle != 0 && m_mediaPool) {
            m_sourceMonitor->loadClip(handle, m_mediaPool);
            return;
        }
    }
    // No cached video — build a transient SpineClip and let
    // CompositeService render it live for preview.
    auto sc = std::make_unique<SpineClip>(charName, outfit);
    CharacterStance stance = CharacterStance::Default;
    if (stanceInt == 1) stance = CharacterStance::Aim;
    else if (stanceInt == 2) stance = CharacterStance::Cover;
    sc->setStance(stance);
    sc->setAnimationName(animName);
    sc->setLooping(true);
    sc->setLabel(charName + " - " + animName);
    if (m_compositeService) {
        // loadSpineClip renders ONE preview frame inline and does
        // not retain the pointer, so a local unique_ptr is fine.
        m_sourceMonitor->loadSpineClip(sc.get(),
                                       m_compositeService.get());
    }
#else
    (void)charName; (void)outfit; (void)stanceInt; (void)animName;
#endif
}

void TimelineWorkspace::loadSequenceInSourceMonitor(size_t seqIdx)
{
    if (!m_project || !m_sourceMonitor) return;
    if (seqIdx >= m_project->sequenceCount()) return;

    auto* seq = m_project->sequence(seqIdx);
    if (!seq) return;

    QString name = QString::fromStdString(seq->name());
    int64_t dur  = seq->duration();
    // Empty sequence: still give the source monitor a usable timeline
    // length so the user can scrub a black preview (Premiere does this
    // — an empty sequence still has its nominal length).
    if (dur <= 0) dur = kTicksPerSecond * 60;
    double fps = seq->settings().frameRate();
    if (!(fps > 0.0)) fps = 24.0;
    const auto seqResolution = seq->settings().resolution();
    const uint32_t seqWidth = seqResolution.width > 0
        ? static_cast<uint32_t>(seqResolution.width) : 1920u;
    const uint32_t seqHeight = seqResolution.height > 0
        ? static_cast<uint32_t>(seqResolution.height) : 1080u;

    SourceMonitor::SequenceFrameProvider provider =
        [this, seqIdx](int64_t tick, uint32_t w, uint32_t h, bool scrub,
                       ResolutionTier tier, bool still)
            -> std::shared_ptr<CachedFrame>
    {
        if (m_destroying.load(std::memory_order_acquire)) return nullptr;
        if (!m_project || seqIdx >= m_project->sequenceCount())
            return nullptr;
        auto* innerTimeline = m_project->sequence(seqIdx);
        if (!innerTimeline) return nullptr;
        if (!m_compositeService) return nullptr;

        // Composite at the PROJECT resolution, not the (small) source
        // monitor viewport size. Compositing into a tiny canvas made
        // characters look low-res no matter the decode tier — the
        // whole frame was just small. The source viewport downscales
        // the full-res frame for display, preserving detail. Capped at
        // 1920×1080 so the preview composite stays cheap.
        const uint32_t outW = std::max(w, 64u);
        const uint32_t outH = std::max(h, 36u);

        // Target the inner sequence through this request without rebinding
        // the program monitor's compositor. Force CPU display mode so
        // compositeFrame does the GPU→CPU readback INLINE while the
        // composite mutex is still held — otherwise the lazyReadback
        // path races with the FrameProducer's next composite (which
        // can resize/recycle the readback staging buffer and the
        // deferred map returns false, leaving the source monitor black).
        RenderRequest request;
        request.type = RenderRequestType::SourceMonitor;
        switch (tier) {
        case ResolutionTier::Full: request.quality = RenderQuality::Full; break;
        case ResolutionTier::Half: request.quality = RenderQuality::Half; break;
        case ResolutionTier::Quarter: request.quality = RenderQuality::Quarter; break;
        }
        request.exactness = defaultExactnessFor(request.type);
        request.timelineTick = tick;
        request.outputWidth = outW;
        request.outputHeight = outH;
        request.scrubMode = scrub;
        request.stillFrame = still;
        request.preferGpuOutput = false;
        request.targetSequenceIndex = seqIdx;
        request.caller = "TimelineWorkspace::sequenceSourceMonitor";
        // Use the isolated recursive path so this secondary consumer does
        // not populate the program monitor's LRU or last-good frame.
        auto frame = m_compositeService->compositeFrame(
            request, /*isNestedRecursion=*/true);
        if (frame) frame->ensurePixels();  // belt + suspenders
        return frame;
    };

    // Timeline getter — resolved lazily so the source monitor never
    // dereferences a stale Timeline* when the project changes.
    SourceMonitor::SequenceTimelineGetter timelineGetter =
        [this, seqIdx]() -> Timeline* {
        if (m_destroying.load(std::memory_order_acquire)) return nullptr;
        if (!m_project || seqIdx >= m_project->sequenceCount())
            return nullptr;
        return m_project->sequence(seqIdx);
    };

    m_sourceMonitor->loadSequence(seqIdx, name, dur, fps,
                                   seqWidth, seqHeight,
                                   std::move(provider),
                                   std::move(timelineGetter));
}

} // namespace rt
