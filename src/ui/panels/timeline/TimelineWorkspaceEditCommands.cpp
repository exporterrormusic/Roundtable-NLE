/*
 * TimelineWorkspaceEditCommands.cpp — Editing commands extracted from
 * TimelineWorkspace.cpp.
 *
 * Contains: setInPoint(), setOutPoint(), clearInOut(),
 * syncProgramMonitorInOut(), refreshSequenceTabs(),
 * nestSequence().
 */

#include "panels/timeline/TimelineWorkspace.h"

#include "CompositeService.h"

#include "panels/monitors/ProgramMonitor.h"
#include "widgets/MiniTimeline.h"
#include "panels/timeline/TimelinePanel.h"

#include "command/CommandStack.h"
#include "command/commands/ClipCommands.h"
#include "project/Project.h"
#include "playback/PlaybackController.h"
#include "timeline/EditOperations.h"
#include "timeline/SequenceClip.h"
#include "timeline/Timeline.h"
#include "timeline/Track.h"

#include <QString>

#include <spdlog/spdlog.h>

#include <memory>

namespace rt {

void TimelineWorkspace::setInPoint()
{
    if (!m_timeline || !m_playbackController) return;
    EditOperations::setInPoint(*m_timeline, m_playbackController->currentTick());
    if (m_timelinePanel) m_timelinePanel->updateInOutRange();
    syncProgramMonitorInOut();
}

void TimelineWorkspace::setOutPoint()
{
    if (!m_timeline || !m_playbackController) return;
    EditOperations::setOutPoint(*m_timeline, m_playbackController->currentTick());
    if (m_timelinePanel) m_timelinePanel->updateInOutRange();
    syncProgramMonitorInOut();
}

void TimelineWorkspace::clearInOut()
{
    if (!m_timeline) return;
    EditOperations::clearInOutPoints(*m_timeline);
    if (m_timelinePanel) m_timelinePanel->updateInOutRange();
    syncProgramMonitorInOut();
}

void TimelineWorkspace::syncProgramMonitorInOut()
{
    if (!m_programMonitor || !m_programMonitor->miniTimeline())
        return;
    auto* mt = m_programMonitor->miniTimeline();
    if (m_timeline) {
        int64_t inPt  = m_timeline->inPoint();
        int64_t outPt = m_timeline->outPoint();
        if (inPt < 0 && outPt < 0) {
            mt->clearInOutPoints();
        } else {
            mt->setInPoint(inPt);
            mt->setOutPoint(outPt);
        }
    } else {
        mt->clearInOutPoints();
    }
}

void TimelineWorkspace::openSequenceTab(size_t index)
{
    if (!m_project || index >= m_project->sequenceCount()) return;
    m_openSequenceTabs.insert(index);
}

void TimelineWorkspace::refreshSequenceTabs()
{
    if (!m_sequenceTabBar || !m_project) return;

    // Drop stale entries — indices that no longer exist because their
    // sequence was removed. Without this, the next sequence created at
    // a freed index inherits the "open" flag from its predecessor and
    // gets a tab automatically (user-reported bug after delete+duplicate).
    for (auto it = m_openSequenceTabs.begin(); it != m_openSequenceTabs.end(); ) {
        if (*it >= m_project->sequenceCount())
            it = m_openSequenceTabs.erase(it);
        else
            ++it;
    }

    // Seed the open set with all sequences on first use (project load)
    if (m_openSequenceTabs.empty()) {
        for (size_t i = 0; i < m_project->sequenceCount(); ++i)
            m_openSequenceTabs.insert(i);
    }

    // Compute the desired tab→sequence list from the open set.
    std::vector<size_t> desired;
    desired.reserve(m_openSequenceTabs.size());
    for (size_t i = 0; i < m_project->sequenceCount(); ++i) {
        if (m_openSequenceTabs.find(i) != m_openSequenceTabs.end())
            desired.push_back(i);
    }

    // Resolve the active sequence (may have been closed → fall back to first open).
    size_t activeIdx = m_project->activeSequenceIndex();
    if (m_openSequenceTabs.find(activeIdx) == m_openSequenceTabs.end()) {
        if (!m_openSequenceTabs.empty()) {
            activeIdx = *m_openSequenceTabs.begin();
            m_project->setActiveSequence(activeIdx);
        }
    }

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
            const Timeline* seq = m_project->sequence(desired[t]);
            QString name = seq ? QString::fromStdString(seq->name())
                               : QStringLiteral("Sequence");
            if (m_sequenceTabBar->tabText(static_cast<int>(t)) != name)
                m_sequenceTabBar->setTabText(static_cast<int>(t), name);
        }
        if (desiredActiveTab >= 0 &&
            desiredActiveTab < m_sequenceTabBar->count() &&
            m_sequenceTabBar->currentIndex() != desiredActiveTab) {
            m_sequenceTabBar->setCurrentIndex(desiredActiveTab);
        }
        m_suppressTabChange = false;
        // Force the queued paint to flush now — workaround for a Qt issue
        // where the tab bar's update from setCurrentIndex sits queued behind
        // other events until the next mouse-click flushes it.
        m_sequenceTabBar->repaint();
        return;
    }

    // Full rebuild.
    m_suppressTabChange = true;
    while (m_sequenceTabBar->count() > 0)
        m_sequenceTabBar->removeTab(0);
    m_tabToSeq.clear();

    for (size_t i : desired) {
        const Timeline* seq = m_project->sequence(i);
        m_sequenceTabBar->addTab(seq ? QString::fromStdString(seq->name())
                                     : QStringLiteral("Sequence"));
        m_tabToSeq.push_back(i);
    }

    if (desiredActiveTab >= 0 && desiredActiveTab < m_sequenceTabBar->count())
        m_sequenceTabBar->setCurrentIndex(desiredActiveTab);
    m_suppressTabChange = false;
    m_sequenceTabBar->repaint();
}

void TimelineWorkspace::nestSequence(size_t sequenceIndex, const QString& sequenceName)
{
    if (!m_timeline || !m_project || !m_commandStack) return;

    // Find first targeted video track
    Track* targetTrack = nullptr;
    for (size_t i = 0; i < m_timeline->trackCount(); ++i) {
        Track* t = m_timeline->track(i);
        if (t && t->type() == TrackType::Video && t->isTargeted()) {
            targetTrack = t;
            break;
        }
    }
    if (!targetTrack) return;

    // Get playhead position
    int64_t playhead = m_playbackController ? m_playbackController->currentTick() : 0;

    // Get the nested sequence's duration (fallback to 5 seconds)
    const Timeline* nestedSeq = m_project->sequence(sequenceIndex);
    int64_t dur = nestedSeq ? nestedSeq->duration() : (48000 * 5);
    if (dur <= 0) dur = 48000 * 5;

    auto clip = std::make_unique<SequenceClip>();
    clip->setSequenceIndex(sequenceIndex);
    clip->setSequenceName(sequenceName.toStdString());
    clip->setLabel(sequenceName.toStdString());
    clip->setTimelineIn(playhead);
    clip->setSourceIn(0);
    clip->setDuration(dur);

    auto cmd = std::make_unique<AddClipCommand>(targetTrack, std::move(clip));
    m_commandStack->execute(std::move(cmd));

    if (m_timelinePanel) m_timelinePanel->refreshTrackContents();
    invalidateAudioSources();
    invalidateCompositeCache();
    updateTransformOverlay();
    if (m_programMonitor) m_programMonitor->requestRefresh();
    schedulePostEditWork();
}

// ═════════════════════════════════════════════════════════════════════════════

} // namespace rt
