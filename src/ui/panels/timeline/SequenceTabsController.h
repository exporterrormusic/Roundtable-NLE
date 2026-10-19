/*
 * SequenceTabsController — the TIMELINE page's sequence tab bar
 * (Premiere-style: one tab per open sequence).
 *
 * Owns the tab bar's state: which sequences are open, the tab→sequence
 * map, and the open/close/context-menu behaviour. Keeps Project's
 * openSequenceIndices in step, since the project file persists them.
 * The workspace re-emits the requests (switch, rename, settings,
 * duplicate) as its own signals; MainWindow does the actual switch.
 */

#pragma once

#include <QObject>

#include <cstddef>
#include <functional>
#include <set>
#include <vector>

class QPoint;
class QTabBar;

namespace rt {

class Project;

class SequenceTabsController : public QObject {
public:
    struct Config {
        std::function<Project*()>     project;
        std::function<bool()>         destroying;
        /// Sequences were added, removed, renamed or reordered.
        std::function<void()>         sequencesChanged;
        std::function<void(size_t)>   switchRequested;
        std::function<void(size_t)>   renameRequested;
        std::function<void(size_t)>   settingsRequested;
        std::function<void(size_t)>   duplicateRequested;
    };

    explicit SequenceTabsController(Config cfg) : m_cfg(std::move(cfg)) {}

    /// Build the styled tab bar and connect it. The caller parents it.
    QTabBar* createTabBar();

    /// A different project was set: take its saved open-tab set.
    void projectChanged(Project* project);

    /// Mark a sequence as open. Does nothing if already open.
    void open(size_t index);

    /// Copy the open-tab set into the Project model.
    void syncToProject();

    /// Rebuild the tab bar from the current project.
    void refresh();

private:
    void closeSequence(size_t seqIdx);
    void closeOthers(size_t seqIdx);
    void showContextMenu(const QPoint& pos);

    Config m_cfg;
    QTabBar* m_tabBar{nullptr};
    bool m_suppressTabChange{false};
    /// Sequence indices currently open as tabs. When empty, refresh()
    /// seeds it with all sequences.
    std::set<size_t> m_openSequenceTabs;
    /// Tab index → sequence index, rebuilt by refresh().
    std::vector<size_t> m_tabToSeq;
};

} // namespace rt
