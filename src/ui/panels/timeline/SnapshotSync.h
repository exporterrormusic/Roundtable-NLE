/*
 * SnapshotSync — keeps the render snapshot in step with the edited model.
 *
 * Render threads never read the live Project/Timeline: they read the
 * RenderSnapshot that a SnapshotPublisher copies on the UI thread. This
 * module owns that publisher and decides when it publishes:
 *   - markDirty() after an edit, coalesced to one publish per event-loop
 *     turn; a paused Program Monitor is then re-rendered, because an edit
 *     that only invalidated the cache is visible only after the publish;
 *   - every recorded step, undo and redo (CommandStack history listener),
 *     and every committed edit, scoped by the sequence it names
 *     (EditSession commit listener);
 *   - beforeMonitorRefresh(): a monitor refresh publishes first;
 *   - once a second while paused, the copies are checked against the live
 *     model, and an edit that never signalled is logged and published.
 *
 * The compositor reads publisher(), so this object must outlive the
 * CompositeService (the workspace declares it before the service).
 */

#pragma once

#include <QObject>

#include <cstddef>
#include <functional>
#include <memory>

class QTimer;

namespace rt {

class CommandStack;
class PlaybackController;
class Project;
class ProgramMonitor;
class SnapshotPublisher;
class Timeline;
struct RenderSnapshot;

class SnapshotSync : public QObject {
public:
    struct Config {
        std::function<bool()>                destroying;
        std::function<Timeline*()>           timeline;
        std::function<ProgramMonitor*()>     programMonitor;
        std::function<PlaybackController*()> playback;
        /// After any history change: panels let go of clips that are no
        /// longer on the timeline.
        std::function<void()>                revalidatePanelClipBindings;
    };

    explicit SnapshotSync(Config cfg);
    ~SnapshotSync() override;

    [[nodiscard]] SnapshotPublisher* publisher() const noexcept { return m_publisher.get(); }

    /// The project the publisher may read; rebinds and publishes.
    void setProject(Project* project);
    /// Config::timeline changed; rebinds and publishes. A null timeline also
    /// forgets the project: on a project switch setTimeline(new) runs before
    /// setProject(new), while the old project is already destroyed.
    void timelineChanged();
    /// Listen to `stack`'s history and its edit session's commits.
    void setCommandStack(CommandStack* stack);

    /// The edited model changed: publish once this event-loop turn.
    /// `allSequences` for edits that may touch a sequence other than the
    /// active one.
    void markDirty(bool allSequences);
    /// Publish pending model changes now.
    void publishNow();
    [[nodiscard]] bool isDirty() const noexcept;
    /// The snapshot including every edit so far (publishes pending ones).
    [[nodiscard]] std::shared_ptr<const RenderSnapshot> latest();
    /// ProgramMonitor's before-refresh hook: the synchronous render that
    /// follows must see the latest edit.
    void beforeMonitorRefresh();

    /// Workspace teardown: stop the timers and drop the listeners that
    /// capture us (the stack and its edit session outlive the workspace).
    void detach();
    /// The model is about to be destroyed: stop publishing and forget it.
    void shutdown();

private:
    void rebind();
    void verify();
    /// Re-render the paused frame without re-entering beforeMonitorRefresh.
    void refreshMonitor();

    Config m_cfg;
    std::unique_ptr<SnapshotPublisher> m_publisher;
    QTimer* m_publishTimer{nullptr};   // coalesces markDirty()
    QTimer* m_verifyTimer{nullptr};    // idle check for missed signals
    bool m_refreshFromPublish{false};  // refresh issued by the publish itself
    CommandStack* m_commandStack{nullptr};
    size_t m_historyListenerId{0};
    Project* m_project{nullptr};
};

} // namespace rt
