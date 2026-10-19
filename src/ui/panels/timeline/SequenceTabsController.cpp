#include "panels/timeline/SequenceTabsController.h"

#include "Theme.h"
#include "project/Project.h"
#include "timeline/Timeline.h"

#include <QAction>
#include <QMenu>
#include <QString>
#include <QTabBar>

namespace rt {

QTabBar* SequenceTabsController::createTabBar()
{
    const auto& tc = Theme::colors();
    m_tabBar = new QTabBar;
    m_tabBar->setObjectName(QStringLiteral("SequenceTabBar"));
    m_tabBar->setTabsClosable(true);
    m_tabBar->setExpanding(false);
    m_tabBar->setMovable(true);
    m_tabBar->setDrawBase(false);
    m_tabBar->setDocumentMode(true);
    m_tabBar->setStyleSheet(QStringLiteral(
        "QTabBar { background: %1; }"
        "QTabBar::tab { background: %1; color: %2; padding: 4px 12px; "
        "border: none; border-right: 1px solid %3; font-size: 12px; }"
        "QTabBar::tab:selected { background: %4; color: %5; font-weight: bold; }"
        "QTabBar::tab:hover { background: %6; }"
        "QTabBar::close-button { image: none; width: 12px; height: 12px; "
        "subcontrol-position: right; padding: 2px; }"
        "QTabBar::close-button:hover { background: %7; border-radius: 3px; }")
        .arg(Theme::hex(tc.surface1))
        .arg(Theme::hex(tc.textTertiary))
        .arg(Theme::hex(tc.border))
        .arg(Theme::hex(tc.surface3))
        .arg(Theme::hex(tc.textPrimary))
        .arg(Theme::hex(tc.surface2))
        .arg(Theme::hex(tc.error)));
    m_tabBar->addTab(QStringLiteral("Sequence 1"));
    connect(m_tabBar, &QTabBar::currentChanged, this, [this](int index) {
        if (m_cfg.destroying()) return;
        if (m_suppressTabChange || index < 0) return;
        if (index >= static_cast<int>(m_tabToSeq.size())) return;
        m_cfg.switchRequested(m_tabToSeq[static_cast<size_t>(index)]);
    });
    connect(m_tabBar, &QTabBar::tabCloseRequested, this, [this](int tabIdx) {
        if (m_cfg.destroying()) return;
        if (tabIdx < 0 || tabIdx >= static_cast<int>(m_tabToSeq.size())) return;
        if (!m_cfg.project()) return;
        closeSequence(m_tabToSeq[static_cast<size_t>(tabIdx)]);
    });
    m_tabBar->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_tabBar, &QWidget::customContextMenuRequested, this,
            [this](const QPoint& pos) { showContextMenu(pos); });
    return m_tabBar;
}

void SequenceTabsController::showContextMenu(const QPoint& pos)
{
    if (m_cfg.destroying()) return;
    int tabIdx = m_tabBar->tabAt(pos);
    if (tabIdx < 0 || tabIdx >= static_cast<int>(m_tabToSeq.size()) || !m_cfg.project())
        return;

    size_t seqIdx = m_tabToSeq[static_cast<size_t>(tabIdx)];

    QMenu menu(m_tabBar);
    QAction* renameAction = menu.addAction("Rename Sequence...");
    QAction* settingsAction = menu.addAction("Sequence Settings...");
    QAction* dupAction    = menu.addAction("Duplicate Sequence");
    menu.addSeparator();
    QAction* closeAction  = menu.addAction("Close");
    closeAction->setEnabled(m_openSequenceTabs.size() > 1);
    QAction* closeOthersAction = menu.addAction("Close Other Tabs");
    closeOthersAction->setEnabled(m_openSequenceTabs.size() > 1);

    QAction* chosen = menu.exec(m_tabBar->mapToGlobal(pos));
    if (chosen == renameAction) {
        m_cfg.renameRequested(seqIdx);
    } else if (chosen == settingsAction) {
        m_cfg.settingsRequested(seqIdx);
    } else if (chosen == dupAction) {
        m_cfg.duplicateRequested(seqIdx);
    } else if (chosen == closeAction) {
        closeSequence(seqIdx);
    } else if (chosen == closeOthersAction) {
        closeOthers(seqIdx);
    }
}

void SequenceTabsController::closeSequence(size_t seqIdx)
{
    Project* project = m_cfg.project();
    // Don't close the last open tab
    if (!project || m_openSequenceTabs.size() <= 1) return;

    m_openSequenceTabs.erase(seqIdx);
    syncToProject();

    // If closing the active sequence, switch to the first remaining one.
    // Don't call setActiveSequence here — let switchSequence() handle
    // all the timeline/playback/UI wiring.
    // IMPORTANT: request the switch BEFORE refresh() so switchSequence()
    // sees a different old-vs-new active index and takes the full refresh
    // path (setTimeline, rebuildTracks, pm->refresh). If refresh() runs
    // first it silently changes the project's active to newActive, causing
    // switchSequence to hit the early-return "already active" path which
    // skips all UI refresh — leaving the timeline/program monitor stuck on
    // the closed sequence until the user clicks another tab.
    if (project->activeSequenceIndex() == seqIdx) {
        size_t newActive = *m_openSequenceTabs.begin();
        m_cfg.switchRequested(newActive);
    } else {
        refresh();
    }
}

void SequenceTabsController::closeOthers(size_t seqIdx)
{
    // Close every other open sequence, keeping only the right-clicked
    // one (Premiere's "Close Other Tabs"). The kept tab becomes active.
    Project* project = m_cfg.project();
    if (!project || m_openSequenceTabs.size() <= 1) return;
    const bool keptWasActive = (project->activeSequenceIndex() == seqIdx);
    m_openSequenceTabs.clear();
    m_openSequenceTabs.insert(seqIdx);
    syncToProject();
    // Same ordering hazard as closeSequence(): if the kept tab wasn't the
    // active one, request the switch BEFORE refreshing so switchSequence()
    // takes the full refresh path instead of the "already active" return.
    if (!keptWasActive)
        m_cfg.switchRequested(seqIdx);
    else
        refresh();
}

void SequenceTabsController::projectChanged(Project* project)
{
    m_openSequenceTabs.clear();
    m_tabToSeq.clear();
    if (project) {
        for (size_t index : project->openSequenceIndices())
            m_openSequenceTabs.insert(index);
    }
}

void SequenceTabsController::open(size_t index)
{
    Project* project = m_cfg.project();
    if (!project || index >= project->sequenceCount()) return;
    m_openSequenceTabs.insert(index);
    syncToProject();
}

void SequenceTabsController::syncToProject()
{
    Project* project = m_cfg.project();
    if (!project) return;
    project->setOpenSequenceIndices(
        std::vector<size_t>(m_openSequenceTabs.begin(), m_openSequenceTabs.end()));
}

void SequenceTabsController::refresh()
{
    Project* project = m_cfg.project();
    if (!m_tabBar || !project) return;
    m_cfg.sequencesChanged();

    // Drop stale entries — indices that no longer exist because their
    // sequence was removed. Without this, the next sequence created at
    // a freed index inherits the "open" flag from its predecessor and
    // gets a tab automatically (user-reported bug after delete+duplicate).
    for (auto it = m_openSequenceTabs.begin(); it != m_openSequenceTabs.end(); ) {
        if (*it >= project->sequenceCount())
            it = m_openSequenceTabs.erase(it);
        else
            ++it;
    }

    // Defensive fallback for a new/legacy project with no usable saved state.
    if (m_openSequenceTabs.empty()) {
        for (size_t i = 0; i < project->sequenceCount(); ++i)
            m_openSequenceTabs.insert(i);
    }

    // Compute the desired tab→sequence list from the open set.
    std::vector<size_t> desired;
    desired.reserve(m_openSequenceTabs.size());
    for (size_t i = 0; i < project->sequenceCount(); ++i) {
        if (m_openSequenceTabs.find(i) != m_openSequenceTabs.end())
            desired.push_back(i);
    }

    // Resolve the active sequence (may have been closed → fall back to first open).
    size_t activeIdx = project->activeSequenceIndex();
    if (m_openSequenceTabs.find(activeIdx) == m_openSequenceTabs.end()) {
        if (!m_openSequenceTabs.empty()) {
            activeIdx = *m_openSequenceTabs.begin();
            project->setActiveSequence(activeIdx);
        }
    }

    // Keep the model in sync with tab open/close actions. ProjectSerializer
    // persists this state, and Project marks a genuine change dirty so a
    // normal save/autosave captures it.
    syncToProject();

    int desiredActiveTab = -1;
    for (size_t t = 0; t < desired.size(); ++t) {
        if (desired[t] == activeIdx) { desiredActiveTab = static_cast<int>(t); break; }
    }

    // Fast path: tab set is already correct — just sync the current tab
    // (and tab labels in case a sequence was renamed). Avoids the flicker
    // of tearing down and rebuilding every tab.
    if (desired == m_tabToSeq) {
        m_suppressTabChange = true;
        for (size_t t = 0; t < desired.size(); ++t) {
            const Timeline* seq = project->sequence(desired[t]);
            QString name = seq ? QString::fromStdString(seq->name())
                               : QStringLiteral("Sequence");
            if (m_tabBar->tabText(static_cast<int>(t)) != name)
                m_tabBar->setTabText(static_cast<int>(t), name);
        }
        if (desiredActiveTab >= 0 &&
            desiredActiveTab < m_tabBar->count() &&
            m_tabBar->currentIndex() != desiredActiveTab) {
            m_tabBar->setCurrentIndex(desiredActiveTab);
        }
        m_suppressTabChange = false;
        // Force the queued paint to flush now — workaround for a Qt issue
        // where the tab bar's update from setCurrentIndex sits queued behind
        // other events until the next mouse-click flushes it.
        m_tabBar->repaint();
        return;
    }

    // Full rebuild.
    m_suppressTabChange = true;
    while (m_tabBar->count() > 0)
        m_tabBar->removeTab(0);
    m_tabToSeq.clear();

    for (size_t i : desired) {
        const Timeline* seq = project->sequence(i);
        m_tabBar->addTab(seq ? QString::fromStdString(seq->name())
                             : QStringLiteral("Sequence"));
        m_tabToSeq.push_back(i);
    }

    if (desiredActiveTab >= 0 && desiredActiveTab < m_tabBar->count())
        m_tabBar->setCurrentIndex(desiredActiveTab);
    m_suppressTabChange = false;
    m_tabBar->repaint();
}

} // namespace rt
