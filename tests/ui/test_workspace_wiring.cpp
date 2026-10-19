/*
 * test_workspace_wiring.cpp — characterization harness for TimelineWorkspace.
 *
 * Stage 11 splits the workspace hub into modules with no user-visible
 * change. These tests pin what the hub does today, through its public
 * surface and the panels' own signals:
 *   - the dock layout after buildPanels() (names, areas, tab groups);
 *   - clip selection reaching the clip panels, and deselection clearing them;
 *   - the playhead reaching playback and the toolbar timecode;
 *   - in/out points reaching the Program Monitor mini-timeline;
 *   - the sequence tab bar following the project's open sequences;
 *   - recorded edits and undo reaching the render snapshot.
 */

#include <gtest/gtest.h>

#include "command/CommandStack.h"
#include "edit/EditSession.h"
#include "panels/effects/ColorGradingPanel.h"
#include "panels/effects/EffectControlsPanel.h"
#include "panels/monitors/ProgramMonitor.h"
#include "panels/properties/PropertiesPanel.h"
#include "panels/timeline/TimelinePanel.h"
#include "panels/timeline/TimelineWorkspace.h"
#include "playback/EngineContracts.h"
#include "playback/PlaybackController.h"
#include "project/Project.h"
#include "timeline/EditOperations.h"
#include "timeline/Timeline.h"
#include "timeline/Track.h"
#include "timeline/VideoClip.h"
#include "widgets/MiniTimeline.h"

#include <QApplication>
#include <QDockWidget>
#include <QLabel>
#include <QMainWindow>
#include <QStringList>
#include <QTabBar>

#include <algorithm>
#include <memory>

namespace {

void ensureApp()
{
    static int argc = 1;
    static const char* argv[] = {"test_workspace_wiring"};
    if (!QApplication::instance())
        new QApplication(argc, const_cast<char**>(argv));
}

// A project with two sequences, a clip on V1 of the first, and a workspace
// built over it the way MainWindow does (dependencies, build, project).
struct Rig
{
    rt::Project project;
    rt::Timeline* seq0{nullptr};
    rt::Timeline* seq1{nullptr};
    rt::Track* v1{nullptr};
    rt::Clip* clip{nullptr};
    rt::CommandStack stack;
    rt::EditSession edits{stack};
    rt::PlaybackController playback;
    std::unique_ptr<rt::TimelineWorkspace> ws;

    Rig()
    {
        ensureApp();
        // A new Project starts with one sequence; use it as the first.
        seq0 = project.sequenceCount() ? project.sequence(0)
                                       : project.addSequence("Sequence 01");
        seq0->setName("Sequence 01");
        seq1 = project.addSequence("Sequence 02");
        project.setActiveSequence(0);
        v1 = seq0->addVideoTrack("V1");
        auto c = std::make_unique<rt::VideoClip>();
        c->setDuration(48000);
        clip = v1->addClip(std::move(c));

        edits.setSequenceResolver([this](uint64_t id) -> rt::Timeline* {
            for (size_t i = 0; i < project.sequenceCount(); ++i)
                if (project.sequence(i)->id() == id) return project.sequence(i);
            return nullptr;
        });
        playback.setTimeline(seq0);

        ws = std::make_unique<rt::TimelineWorkspace>();
        ws->setCommandStack(&stack);
        ws->setPlaybackController(&playback);
        ws->setTimeline(seq0);
        ws->buildPanels();
        ws->setProject(&project);
        ws->resize(1600, 1000);
        ws->show();
        QApplication::processEvents();
    }

    ~Rig() { ws.reset(); }

    size_t v1Index() const
    {
        for (size_t i = 0; i < seq0->trackCount(); ++i)
            if (seq0->track(i) == v1) return i;
        return 0;
    }

    QTabBar* sequenceTabs() const
    {
        return ws->findChild<QTabBar*>(QStringLiteral("SequenceTabBar"));
    }
};

// One line per dock: name, owning main window, area, its tab group.
QString dockLayoutFingerprint(const rt::TimelineWorkspace& ws)
{
    QStringList lines;
    const auto& docks = ws.dockWidgets();
    for (auto it = docks.begin(); it != docks.end(); ++it) {
        QDockWidget* dock = it.value();
        auto* host = qobject_cast<QMainWindow*>(dock->parentWidget());
        QString area = QStringLiteral("-");
        QStringList tabs;
        if (host) {
            area = QString::number(static_cast<int>(host->dockWidgetArea(dock)));
            for (QDockWidget* t : host->tabifiedDockWidgets(dock))
                tabs << t->windowTitle();
            tabs.sort();
        }
        lines << QStringLiteral("%1|%2|%3|%4|%5")
                     .arg(it.key(), host ? host->objectName() : QStringLiteral("?"),
                          area, tabs.join(QLatin1Char(',')),
                          dock->isHidden() ? QStringLiteral("hidden")
                                           : QStringLiteral("shown"));
    }
    return lines.join(QLatin1Char('\n'));
}

} // namespace

TEST(WorkspaceWiring, BuildCreatesTheStockDockLayout)
{
    Rig rig;
    const QString got = dockLayoutFingerprint(*rig.ws);
    // name|host objectName|Qt::DockWidgetArea|tab group|hidden flag.
    // Areas: 1 left, 2 right, 4 top. The Tools column and Audio Meters sit
    // beside the timeline; everything else is the top row.
    const char* expected =
        "Audio Meters||2||shown\n"
        "Captions||4|Color Correction,Effect Controls,Effects,Graphics Editor,History,Library,Properties,Scopes,Tier List|shown\n"
        "Color Correction||4|Captions,Effect Controls,Effects,Graphics Editor,History,Library,Properties,Scopes,Tier List|shown\n"
        "Effect Controls||4|Captions,Color Correction,Effects,Graphics Editor,History,Library,Properties,Scopes,Tier List|shown\n"
        "Effects||4|Captions,Color Correction,Effect Controls,Graphics Editor,History,Library,Properties,Scopes,Tier List|shown\n"
        "Graphics Editor||4|Captions,Color Correction,Effect Controls,Effects,History,Library,Properties,Scopes,Tier List|shown\n"
        "History||4|Captions,Color Correction,Effect Controls,Effects,Graphics Editor,Library,Properties,Scopes,Tier List|shown\n"
        "Library||4|Captions,Color Correction,Effect Controls,Effects,Graphics Editor,History,Properties,Scopes,Tier List|shown\n"
        "Program Monitor||4||shown\n"
        "Project Bin||4||shown\n"
        "Properties||4|Captions,Color Correction,Effect Controls,Effects,Graphics Editor,History,Library,Scopes,Tier List|shown\n"
        "Scopes||4|Captions,Color Correction,Effect Controls,Effects,Graphics Editor,History,Library,Properties,Tier List|shown\n"
        "Source Monitor||4||shown\n"
        "Tier List||4|Captions,Color Correction,Effect Controls,Effects,Graphics Editor,History,Library,Properties,Scopes|shown\n"
        "Tools||1||shown";
    EXPECT_EQ(got.toStdString(), std::string(expected));
}

TEST(WorkspaceWiring, ClickingAClipSelectsItAndBindsTheShownPanels)
{
    Rig rig;
    emit rig.ws->timelinePanel()->clipSelected(rig.v1Index(), 0);
    QApplication::processEvents();

    EXPECT_EQ(rig.ws->selection().clip, rig.clip);
    EXPECT_EQ(rig.ws->selection().trackIdx, rig.v1Index());
    EXPECT_EQ(rig.ws->selection().clipIdx, 0u);
    // Effect Controls is the front tab of its group: bound on click.
    EXPECT_EQ(rig.ws->effectControlsPanel()->clip(), rig.clip);
}

TEST(WorkspaceWiring, SelectingOneClipBindsEveryClipPanelAndDeselectClears)
{
    Rig rig;
    auto* panel = rig.ws->timelinePanel();
    panel->selection().selectClip(rt::ClipRef{rig.v1Index(), rig.clip->id()});
    emit panel->selectionChanged();
    QApplication::processEvents();

    EXPECT_EQ(rig.ws->selection().clip, rig.clip);
    EXPECT_EQ(rig.ws->effectControlsPanel()->clip(), rig.clip);
    EXPECT_EQ(rig.ws->propertiesPanel()->clip(), rig.clip);
    if (rig.ws->colorGradingPanel())
        EXPECT_EQ(rig.ws->colorGradingPanel()->clip(), rig.clip);

    panel->selection().clear();
    emit panel->selectionChanged();
    QApplication::processEvents();

    EXPECT_EQ(rig.ws->selection().clip, nullptr);
    EXPECT_EQ(rig.ws->effectControlsPanel()->clip(), nullptr);
    EXPECT_EQ(rig.ws->propertiesPanel()->clip(), nullptr);
    if (rig.ws->colorGradingPanel())
        EXPECT_EQ(rig.ws->colorGradingPanel()->clip(), nullptr);
}

TEST(WorkspaceWiring, MovingThePlayheadSeeksAndUpdatesTheTimecode)
{
    Rig rig;
    emit rig.ws->timelinePanel()->playheadMoved(48000);
    QApplication::processEvents();

    EXPECT_EQ(rig.playback.currentTick(), 48000);
    const QString expectedTc = QString::fromStdString(
        rig.ws->timelinePanel()->layoutEngine().formatTimecode(48000));
    bool found = false;
    for (QLabel* label : rig.ws->findChildren<QLabel*>())
        if (label->text() == expectedTc) found = true;
    EXPECT_TRUE(found) << "no timecode label reads " << expectedTc.toStdString();
}

TEST(WorkspaceWiring, InAndOutPointsReachTheProgramMonitor)
{
    Rig rig;
    auto* mini = rig.ws->programMonitor()->miniTimeline();
    ASSERT_NE(mini, nullptr);

    rig.playback.seekTo(24000);
    rig.ws->setInPoint();
    rig.playback.seekTo(36000);
    rig.ws->setOutPoint();
    EXPECT_EQ(rig.seq0->inPoint(), 24000);
    EXPECT_EQ(rig.seq0->outPoint(), 36000);
    EXPECT_EQ(mini->inPoint(), rig.seq0->inPoint());
    EXPECT_EQ(mini->outPoint(), rig.seq0->outPoint());

    rig.ws->clearInOut();
    EXPECT_EQ(rig.seq0->inPoint(), -1);
    EXPECT_FALSE(mini->hasInPoint());
    EXPECT_FALSE(mini->hasOutPoint());
}

TEST(WorkspaceWiring, SequenceTabsFollowTheOpenSequences)
{
    Rig rig;
    QTabBar* tabs = rig.sequenceTabs();
    ASSERT_NE(tabs, nullptr);
    // A new project opens only its first sequence.
    ASSERT_EQ(tabs->count(), 1);
    EXPECT_EQ(tabs->tabText(0), QStringLiteral("Sequence 01"));

    rig.ws->openSequenceTab(1);
    rig.ws->refreshSequenceTabs();
    ASSERT_EQ(tabs->count(), 2);
    EXPECT_EQ(tabs->tabText(1), QStringLiteral("Sequence 02"));
    EXPECT_EQ(rig.project.openSequenceIndices(), (std::vector<size_t>{0, 1}));

    std::vector<size_t> changed;
    QObject::connect(rig.ws.get(), &rt::TimelineWorkspace::sequenceTabChanged,
                     [&](size_t index) { changed.push_back(index); });
    tabs->setCurrentIndex(1);
    EXPECT_EQ(changed, (std::vector<size_t>{1}));

    // Renaming a sequence relabels its tab without a rebuild.
    rig.seq1->setName("Renamed");
    rig.ws->refreshSequenceTabs();
    EXPECT_EQ(tabs->tabText(1), QStringLiteral("Renamed"));
}

TEST(WorkspaceWiring, RecordedEditsAndUndoReachTheRenderSnapshot)
{
    Rig rig;
    auto countClips = [](const rt::RenderSnapshot& snap) {
        size_t n = 0;
        for (size_t i = 0; i < snap.timeline->trackCount(); ++i)
            n += snap.timeline->track(i)->clipCount();
        return n;
    };
    auto before = rig.ws->publishedSnapshot();
    ASSERT_TRUE(before && before->timeline);
    EXPECT_EQ(countClips(*before), 1u);

    rt::SelectionSet selection;
    selection.selectClip(rt::ClipRef{rig.v1Index(), rig.clip->id()});
    ASSERT_TRUE(rig.edits.execute(
        *rig.seq0, rt::EditOperations::deleteSelection(*rig.seq0, selection)));
    auto afterDelete = rig.ws->publishedSnapshot();
    ASSERT_TRUE(afterDelete && afterDelete->timeline);
    EXPECT_EQ(countClips(*afterDelete), 0u);

    rig.stack.undo();
    auto afterUndo = rig.ws->publishedSnapshot();
    ASSERT_TRUE(afterUndo && afterUndo->timeline);
    EXPECT_EQ(countClips(*afterUndo), 1u);
}

TEST(WorkspaceWiring, DraggedSequenceTabsStillSwitchToTheirOwnSequence)
{
    Rig rig;
    rig.ws->openSequenceTab(1);
    rig.ws->refreshSequenceTabs();
    QTabBar* tabs = rig.sequenceTabs();
    ASSERT_EQ(tabs->count(), 2);

    // The user drags "Sequence 02" in front of "Sequence 01".
    tabs->moveTab(1, 0);
    ASSERT_EQ(tabs->tabText(0), QStringLiteral("Sequence 02"));

    std::vector<size_t> changed;
    QObject::connect(rig.ws.get(), &rt::TimelineWorkspace::sequenceTabChanged,
                     [&](size_t index) { changed.push_back(index); });
    tabs->setCurrentIndex(0);
    EXPECT_EQ(changed, (std::vector<size_t>{1}));

    // A later refresh (e.g. a rename) keeps the user's order.
    rig.seq1->setName("Renamed");
    rig.ws->refreshSequenceTabs();
    EXPECT_EQ(tabs->tabText(0), QStringLiteral("Renamed"));
    EXPECT_EQ(tabs->tabText(1), QStringLiteral("Sequence 01"));
}
