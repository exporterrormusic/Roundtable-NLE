#include "panels/timeline/SnapshotSync.h"

#include "command/CommandStack.h"
#include "edit/EditSession.h"
#include "panels/monitors/ProgramMonitor.h"
#include "playback/PlaybackController.h"
#include "playback/SnapshotPublisher.h"
#include "timeline/Timeline.h"

#include <QTimer>

#include <spdlog/spdlog.h>

#include <chrono>
#include <string>

namespace rt {

SnapshotSync::SnapshotSync(Config cfg)
    : m_cfg(std::move(cfg))
    , m_publisher(std::make_unique<SnapshotPublisher>())
{
    m_publishTimer = new QTimer(this);
    m_publishTimer->setSingleShot(true);
    m_publishTimer->setInterval(0);
    connect(m_publishTimer, &QTimer::timeout, this, [this] {
        const bool wasDirty = m_publisher->isDirty();
        publishNow();
        // An edit that only invalidated the cache (no requestRefresh) is
        // visible only after this publish: re-render the paused frame.
        PlaybackController* playback = m_cfg.playback();
        if (wasDirty && playback && !playback->isPlaying())
            refreshMonitor();
    });
    m_verifyTimer = new QTimer(this);
    m_verifyTimer->setInterval(1000);
    connect(m_verifyTimer, &QTimer::timeout, this, [this] { verify(); });
    m_verifyTimer->start();
}

SnapshotSync::~SnapshotSync() = default;

void SnapshotSync::setProject(Project* project)
{
    m_project = project;
    rebind();
}

void SnapshotSync::timelineChanged()
{
    if (!m_cfg.timeline())
        m_project = nullptr;
    rebind();
}

void SnapshotSync::setCommandStack(CommandStack* stack)
{
    if (m_commandStack && m_historyListenerId)
        m_commandStack->removeHistoryListener(m_historyListenerId);
    m_historyListenerId = 0;
    m_commandStack = stack;
    // Every undo/redo — whichever panel, shortcut or History jump drove it —
    // and every recorded step must reach the render snapshot, even when the
    // caller skips refreshAfterUndoRedo (AudioSync's own Ctrl+Z, History).
    if (stack) {
        m_historyListenerId = stack->addHistoryListener(
            [this](CommandStack::HistoryEvent event) {
                markDirty(event != CommandStack::HistoryEvent::Recorded);
                // A step leaving history (trimmed, cleared) frees the clips
                // it kept; a step undone may remove a bound clip.
                m_cfg.revalidatePanelClipBindings();
            });
    }
    // Each recorded edit (and its undo/redo) names the sequence it changed.
    if (EditSession* edits = stack ? stack->editSession() : nullptr) {
        edits->setCommitListener([this](uint64_t sequenceId) {
            const Timeline* timeline = m_cfg.timeline();
            markDirty(!timeline || timeline->id() != sequenceId);
        });
    }
}

void SnapshotSync::rebind()
{
    m_publisher->bind(m_project, m_cfg.timeline());
    publishNow();
}

void SnapshotSync::markDirty(bool allSequences)
{
    if (m_cfg.destroying()) return;
    m_publisher->markDirty(allSequences
        ? SnapshotPublisher::DirtyScope::AllSequences
        : SnapshotPublisher::DirtyScope::ActiveSequence);
    if (!m_publishTimer->isActive())
        m_publishTimer->start();
}

void SnapshotSync::publishNow()
{
    if (m_cfg.destroying()) return;
    if (!m_publisher->publish()) return;

    const auto stats = m_publisher->lastPublishStats();
    spdlog::debug("[SNAPSHOT] v{} published in {:.2f} ms ({} sequence(s) copied, "
                  "{} shared, {} clips)", stats.editVersion, stats.cloneMs,
                  stats.sequencesCloned, stats.sequencesShared, stats.clipsCloned);
    // A copy this slow per edit would make dragging feel heavy; surface it
    // (throttled) so it can be measured on real projects.
    constexpr double kSlowPublishMs = 8.0;
    static auto s_lastSlowLog = std::chrono::steady_clock::time_point{};
    const auto now = std::chrono::steady_clock::now();
    if (stats.cloneMs > kSlowPublishMs && now - s_lastSlowLog > std::chrono::seconds(10)) {
        s_lastSlowLog = now;
        spdlog::warn("[SNAPSHOT] slow publish: {:.1f} ms for {} clips in {} sequence(s)",
                     stats.cloneMs, stats.clipsCloned, stats.sequencesCloned);
    }
}

bool SnapshotSync::isDirty() const noexcept
{
    return m_publisher->isDirty();
}

std::shared_ptr<const RenderSnapshot> SnapshotSync::latest()
{
    if (m_cfg.destroying()) return nullptr;
    publishNow();
    return m_publisher->latest();
}

void SnapshotSync::beforeMonitorRefresh()
{
    if (m_cfg.destroying() || m_refreshFromPublish)
        return;
    m_publisher->markDirty(SnapshotPublisher::DirtyScope::ActiveSequence);
    publishNow();
}

void SnapshotSync::refreshMonitor()
{
    ProgramMonitor* monitor = m_cfg.programMonitor();
    if (!monitor) return;
    m_refreshFromPublish = true;
    monitor->requestRefresh();
    m_refreshFromPublish = false;
}

void SnapshotSync::verify()
{
    if (m_cfg.destroying()) return;
    // Free copies render/export threads have finished with, even when no
    // edit triggers a publish (e.g. after an export job completes).
    m_publisher->collectRetired();
    if (!m_cfg.timeline()) return;
    if (PlaybackController* playback = m_cfg.playback(); playback && playback->isPlaying())
        return;

    std::string detail;
    if (m_publisher->verifySequences(&detail)) return;

    // An edit changed the model without invalidating the composite cache or
    // refreshing the monitor. Log it so the call site can be fixed, and show
    // the edit now.
    spdlog::warn("[SNAPSHOT] stale render copy (an edit did not signal): {}", detail);
    publishNow();
    refreshMonitor();
}

void SnapshotSync::detach()
{
    m_publishTimer->stop();
    m_verifyTimer->stop();
    if (EditSession* edits = m_commandStack ? m_commandStack->editSession() : nullptr)
        edits->setCommitListener({});
    if (m_commandStack && m_historyListenerId)
        m_commandStack->removeHistoryListener(m_historyListenerId);
    m_historyListenerId = 0;
    m_commandStack = nullptr;
}

void SnapshotSync::shutdown()
{
    m_publishTimer->stop();
    m_verifyTimer->stop();
    m_project = nullptr;
    m_publisher->bind(nullptr, nullptr);
}

} // namespace rt
