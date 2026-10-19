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
 *   - recorded edits and undo reaching the render snapshot, including an
 *     edit to a sequence that isn't open and a change that never signalled.
 */

#include <gtest/gtest.h>

#include "command/CommandStack.h"
#include "command/commands/ClipCommands.h"
#include "edit/EditSession.h"
#include "panels/effects/ColorGradingPanel.h"
#include "panels/effects/EffectControlsPanel.h"
#include "panels/effects/GraphicsEditorPanel.h"
#include "panels/monitors/ProgramMonitor.h"
#include "panels/properties/PropertiesPanel.h"
#include "panels/timeline/TimelinePanel.h"
#include "panels/timeline/TimelineWorkspace.h"
#include "playback/EngineContracts.h"
#include "playback/PlaybackController.h"
#include "project/Project.h"
#include "spine/ShotPreset.h"
#include "timeline/AudioClip.h"
#include "timeline/EditOperations.h"
#include "timeline/GraphicClip.h"
#include "timeline/Timeline.h"
#include "timeline/Track.h"
#include "timeline/VideoClip.h"
#include "widgets/MiniTimeline.h"

#include <QApplication>
#include <QDockWidget>
#include <QElapsedTimer>
#include <QKeyEvent>
#include <QLabel>
#include <QMainWindow>
#include <QStringList>
#include <QTabBar>
#include <QToolButton>

#include <algorithm>
#include <filesystem>
#include <string>
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

TEST(WorkspaceWiring, FocusingAClipSelectsItAndBindsEffectControls)
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

namespace {
// A Graphic clip right after the rig's video clip on V1 (clip index 1).
rt::Clip* addGraphicClip(Rig& rig)
{
    auto g = std::make_unique<rt::GraphicClip>();
    g->setTimelineIn(48000);
    g->setDuration(48000);
    return rig.v1->addClip(std::move(g));
}

// The front tab of its group. Tabs behind it still report isVisible().
bool dockInFront(const Rig& rig, const QString& name)
{
    QDockWidget* dock = rig.ws->dockForPanel(name);
    return dock && !dock->visibleRegion().isEmpty();
}
} // namespace

// A click emits selectionChanged, then clipSelected; the S-split re-focus
// emits clipSelected alone. As in Premiere, every clip panel follows the
// focused clip, even one whose dock is closed (opened later, it shows the
// current clip), and the editor for the clip's kind comes to the front.
TEST(WorkspaceWiring, FocusingAClipBindsEveryPanelEvenClosedOnes)
{
    Rig rig;
    rt::Clip* graphic = addGraphicClip(rig);
    auto* panel = rig.ws->timelinePanel();
    ASSERT_TRUE(dockInFront(rig, QStringLiteral("Effect Controls")));

    emit panel->clipSelected(rig.v1Index(), 1);
    QApplication::processEvents();
    EXPECT_EQ(rig.ws->selection().clip, graphic);
    EXPECT_EQ(rig.ws->selection().clipIdx, 1u);
    EXPECT_EQ(rig.ws->graphicsEditorPanel()->clip(), graphic);
    EXPECT_EQ(rig.ws->effectControlsPanel()->clip(), graphic);
    EXPECT_EQ(rig.ws->propertiesPanel()->clip(), graphic);
    EXPECT_EQ(rig.ws->colorGradingPanel()->clip(), graphic);
    EXPECT_TRUE(dockInFront(rig, QStringLiteral("Graphics Editor")));

    for (const char* name : {"Effect Controls", "Properties", "Color Correction"})
        rig.ws->dockForPanel(QString::fromLatin1(name))->close();
    QApplication::processEvents();
    emit panel->clipSelected(rig.v1Index(), 0);
    QApplication::processEvents();
    EXPECT_EQ(rig.ws->selection().clip, rig.clip);
    EXPECT_EQ(rig.ws->effectControlsPanel()->clip(), rig.clip);
    EXPECT_EQ(rig.ws->propertiesPanel()->clip(), rig.clip);
    EXPECT_EQ(rig.ws->colorGradingPanel()->clip(), rig.clip);
    // The raise re-opens Effect Controls; binding alone opens nothing.
    EXPECT_TRUE(rig.ws->dockForPanel(QStringLiteral("Effect Controls"))->isVisible());
    EXPECT_FALSE(rig.ws->dockForPanel(QStringLiteral("Properties"))->isVisible());
}

// The clip context menu's "Properties...": like Premiere's clip Properties,
// it shows that clip's properties, whatever kind of clip it is.
TEST(WorkspaceWiring, ClipPropertiesBringsThePropertiesPanelToTheFront)
{
    Rig rig;
    auto* panel = rig.ws->timelinePanel();
    ASSERT_TRUE(dockInFront(rig, QStringLiteral("Effect Controls")));

    emit panel->clipPropertiesRequested(rig.v1Index(), 0);
    QApplication::processEvents();
    EXPECT_EQ(rig.ws->selection().clip, rig.clip);
    EXPECT_EQ(rig.ws->propertiesPanel()->clip(), rig.clip);
    EXPECT_TRUE(dockInFront(rig, QStringLiteral("Properties")));
}

TEST(WorkspaceWiring, SelectingOneClipBindsEveryPanelAndRaisesItsEditor)
{
    Rig rig;
    rt::Clip* graphic = addGraphicClip(rig);
    auto* panel = rig.ws->timelinePanel();

    panel->selection().selectClip(rt::ClipRef{rig.v1Index(), graphic->id()});
    emit panel->selectionChanged();
    QApplication::processEvents();
    EXPECT_EQ(rig.ws->selection().clip, graphic);
    EXPECT_EQ(rig.ws->selection().clipIdx, 1u);
    EXPECT_EQ(rig.ws->effectControlsPanel()->clip(), graphic);
    EXPECT_EQ(rig.ws->graphicsEditorPanel()->clip(), graphic);
    EXPECT_EQ(rig.ws->colorGradingPanel()->clip(), graphic);
    EXPECT_EQ(rig.ws->propertiesPanel()->clip(), graphic);
    EXPECT_TRUE(dockInFront(rig, QStringLiteral("Graphics Editor")));

    panel->selection().clear();
    panel->selection().selectClip(rt::ClipRef{rig.v1Index(), rig.clip->id()});
    emit panel->selectionChanged();
    QApplication::processEvents();
    EXPECT_EQ(rig.ws->effectControlsPanel()->clip(), rig.clip);
    EXPECT_EQ(rig.ws->propertiesPanel()->clip(), rig.clip);
    EXPECT_TRUE(dockInFront(rig, QStringLiteral("Effect Controls")));
}

TEST(WorkspaceWiring, UndoingTheSelectedClipsAdditionReleasesEveryClipPanel)
{
    Rig rig;
    auto g = std::make_unique<rt::GraphicClip>();
    g->setTimelineIn(48000);
    g->setDuration(48000);
    rt::Clip* graphic = g.get();
    ASSERT_TRUE(rig.edits.execute(
        *rig.seq0, std::make_unique<rt::AddClipCommand>(rig.v1, std::move(g))));
    auto* panel = rig.ws->timelinePanel();
    panel->selection().selectClip(rt::ClipRef{rig.v1Index(), graphic->id()});
    emit panel->selectionChanged();
    QApplication::processEvents();
    ASSERT_EQ(rig.ws->graphicsEditorPanel()->clip(), graphic);

    rig.stack.undo();
    rig.ws->refreshAfterUndoRedo();
    QApplication::processEvents();
    EXPECT_EQ(rig.ws->selection().clip, nullptr);
    EXPECT_EQ(rig.ws->effectControlsPanel()->clip(), nullptr);
    EXPECT_EQ(rig.ws->graphicsEditorPanel()->clip(), nullptr);
    EXPECT_EQ(rig.ws->colorGradingPanel()->clip(), nullptr);
    EXPECT_EQ(rig.ws->propertiesPanel()->clip(), nullptr);
}

// The snap toggle used to be connected to the toolbar's FIRST tool button
// (Nest): the Snap button did nothing and Nest off turned snapping off.
TEST(WorkspaceWiring, ToolbarSnapButtonTogglesSnappingAndFollowsTheNKey)
{
    Rig rig;
    auto* toolbar = rig.ws->findChild<QWidget*>(QStringLiteral("timelineToolbar"));
    ASSERT_NE(toolbar, nullptr);
    QToolButton* snap = nullptr;
    QToolButton* nest = nullptr;
    for (QToolButton* b : toolbar->findChildren<QToolButton*>()) {
        if (b->toolTip().startsWith(QStringLiteral("Snap"))) snap = b;
        if (b->toolTip().startsWith(QStringLiteral("Insert or overwrite"))) nest = b;
    }
    ASSERT_NE(snap, nullptr);
    ASSERT_NE(nest, nullptr);
    rt::TimelinePanel* panel = rig.ws->timelinePanel();
    ASSERT_TRUE(panel->snapEngine().isEnabled());

    snap->click();
    EXPECT_FALSE(snap->isChecked());
    EXPECT_FALSE(panel->snapEngine().isEnabled());
    snap->click();
    EXPECT_TRUE(panel->snapEngine().isEnabled());

    nest->click();
    EXPECT_TRUE(panel->nestSequencesEnabled());
    EXPECT_TRUE(panel->snapEngine().isEnabled());
    nest->click();
    EXPECT_FALSE(panel->nestSequencesEnabled());
    EXPECT_TRUE(panel->snapEngine().isEnabled());

    // N (Toggle Snapping) switches it off; the button follows.
    QKeyEvent press(QEvent::KeyPress, Qt::Key_N, Qt::NoModifier);
    QApplication::sendEvent(rig.ws.get(), &press);
    EXPECT_FALSE(panel->snapEngine().isEnabled());
    EXPECT_FALSE(snap->isChecked());
    QKeyEvent pressAgain(QEvent::KeyPress, Qt::Key_N, Qt::NoModifier);
    QApplication::sendEvent(rig.ws.get(), &pressAgain);
    EXPECT_TRUE(panel->snapEngine().isEnabled());
    EXPECT_TRUE(snap->isChecked());
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

TEST(WorkspaceWiring, AnEditToTheOtherSequenceReachesItsSnapshotCopy)
{
    Rig rig;
    rt::Track* other = rig.seq1->addVideoTrack("V1");
    auto c = std::make_unique<rt::VideoClip>();
    c->setDuration(48000);
    rt::Clip* otherClip = other->addClip(std::move(c));
    rig.ws->markSnapshotDirty(/*allSequences=*/true);
    auto clipsIn = [](const rt::RenderSnapshot& snap, size_t index) {
        size_t n = 0;
        const rt::Timeline* seq = snap.sequenceAt(index);
        for (size_t i = 0; seq && i < seq->trackCount(); ++i)
            n += seq->track(i)->clipCount();
        return n;
    };
    auto before = rig.ws->publishedSnapshot();
    ASSERT_TRUE(before);
    ASSERT_EQ(before->sequences.size(), 2u);
    EXPECT_EQ(clipsIn(*before, 1), 1u);

    // Sequence 01 stays open; the edit names Sequence 02.
    size_t otherIndex = 0;
    while (otherIndex < rig.seq1->trackCount() && rig.seq1->track(otherIndex) != other)
        ++otherIndex;
    rt::SelectionSet selection;
    selection.selectClip(rt::ClipRef{otherIndex, otherClip->id()});
    ASSERT_TRUE(rig.edits.execute(
        *rig.seq1, rt::EditOperations::deleteSelection(*rig.seq1, selection)));
    auto afterDelete = rig.ws->publishedSnapshot();
    ASSERT_TRUE(afterDelete);
    EXPECT_EQ(clipsIn(*afterDelete, 1), 0u);
    EXPECT_EQ(clipsIn(*afterDelete, 0), 1u);

    rig.stack.undo();
    auto afterUndo = rig.ws->publishedSnapshot();
    ASSERT_TRUE(afterUndo);
    EXPECT_EQ(clipsIn(*afterUndo, 1), 1u);
}

TEST(WorkspaceWiring, TheIdleCheckPublishesAChangeThatDidNotSignal)
{
    Rig rig;
    auto publishedDuration = [&rig] {
        auto snap = rig.ws->publishedSnapshot();
        if (!snap || !snap->timeline) return int64_t{-1};
        const rt::Track* track = snap->timeline->track(rig.v1Index());
        return track && track->clipCount() ? track->clip(0)->duration() : int64_t{-1};
    };
    ASSERT_EQ(publishedDuration(), 48000);

    rig.clip->setDuration(24000);   // a plain setter: nothing marks it dirty
    EXPECT_EQ(publishedDuration(), 48000);

    // The verify timer runs once a second while paused.
    QElapsedTimer waited;
    waited.start();
    while (publishedDuration() != 24000 && waited.elapsed() < 3000)
        QApplication::processEvents(QEventLoop::AllEvents, 50);
    EXPECT_EQ(publishedDuration(), 24000);
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

namespace {

struct GroupView
{
    std::vector<std::string> visual;   // "<type>:<shotName>" per visual member
    std::vector<uint64_t> visualIds;
    std::vector<std::string> other;    // shot names of non-visual members
};

GroupView groupMembers(const rt::Timeline& timeline, uint64_t groupId)
{
    GroupView view;
    for (size_t ti = 0; ti < timeline.trackCount(); ++ti) {
        const rt::Track* track = timeline.track(ti);
        for (size_t ci = 0; ci < track->clipCount(); ++ci) {
            const rt::Clip* c = track->clip(ci);
            if (c->groupId() != groupId) continue;
            if (c->isVisual()) {
                view.visual.push_back(std::to_string(static_cast<int>(c->clipType())) +
                                      ":" + c->shotName());
                view.visualIds.push_back(c->id());
            } else {
                view.other.push_back(c->shotName());
            }
        }
    }
    return view;
}

} // namespace

TEST(WorkspaceWiring, ShotSwitchReplacesTheGroupAndUndoRestoresIt)
{
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() /
        ("rt_ws_shot_switch_" + std::to_string(QCoreApplication::applicationPid()));
    fs::remove_all(dir);
    fs::create_directories(dir);
    rt::ShotPresetManager presets;
    presets.scan(dir);
    ASSERT_TRUE(presets.save(rt::ShotPreset::createDefault("Rapi")));

    Rig rig;
    rig.ws->setShotPresetManager(&presets);
    rig.clip->setGroupId(7);
    rig.clip->setShotName("Old Shot");
    rt::Track* a1 = rig.seq0->addAudioTrack("A1");
    auto audio = std::make_unique<rt::AudioClip>();
    audio->setDuration(48000);
    audio->setGroupId(7);
    audio->setShotName("Old Shot");
    a1->addClip(std::move(audio));
    const uint64_t oldId = rig.clip->id();
    const GroupView before = groupMembers(*rig.seq0, 7);

    emit rig.ws->propertiesPanel()->shotSwitchRequested(7, "Rapi - Default");
    QApplication::processEvents();

    const GroupView after = groupMembers(*rig.seq0, 7);
    ASSERT_EQ(after.visual.size(), 1u);
    EXPECT_EQ(after.visual.front(),
              std::to_string(static_cast<int>(rt::ClipType::Spine)) + ":Rapi - Default");
    EXPECT_NE(after.visualIds.front(), oldId);
    EXPECT_EQ(after.other, (std::vector<std::string>{"Rapi - Default"}));
    // The Properties panel follows the new shot's first visual clip.
    ASSERT_NE(rig.ws->propertiesPanel()->clip(), nullptr);
    EXPECT_EQ(rig.ws->propertiesPanel()->clip()->id(), after.visualIds.front());
    EXPECT_EQ(rig.ws->selection().clip, rig.ws->propertiesPanel()->clip());

    ASSERT_TRUE(rig.stack.undo());
    const GroupView undone = groupMembers(*rig.seq0, 7);
    EXPECT_EQ(undone.visual, before.visual);
    EXPECT_EQ(undone.visualIds, (std::vector<uint64_t>{oldId}));
    EXPECT_EQ(undone.other, (std::vector<std::string>{"Old Shot"}));

    ASSERT_TRUE(rig.stack.redo());
    const GroupView redone = groupMembers(*rig.seq0, 7);
    EXPECT_EQ(redone.visual, after.visual);
    EXPECT_EQ(redone.visualIds, after.visualIds);

    rig.ws.reset();
    fs::remove_all(dir);
}

namespace {

// One token per track, top to bottom: "<V|A><clip types and layer ids>".
std::string trackLayout(const rt::Timeline& timeline)
{
    std::string out;
    for (size_t ti = 0; ti < timeline.trackCount(); ++ti) {
        const rt::Track* track = timeline.track(ti);
        if (!out.empty()) out += " | ";
        out += track->type() == rt::TrackType::Video ? "V[" : "A[";
        for (size_t ci = 0; ci < track->clipCount(); ++ci) {
            const rt::Clip* c = track->clip(ci);
            if (ci) out += ",";
            out += std::to_string(static_cast<int>(c->clipType())) + ":" + c->layerId();
        }
        out += "]";
    }
    return out;
}

} // namespace

TEST(WorkspaceWiring, ShotSwitchToABiggerShotAddsTracksAndUndoRemovesThem)
{
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() /
        ("rt_ws_shot_switch_big_" + std::to_string(QCoreApplication::applicationPid()));
    fs::remove_all(dir);
    fs::create_directories(dir);
    rt::ShotPresetManager presets;
    presets.scan(dir);
    rt::ShotPreset big("Big");
    rt::BackgroundState bg;
    bg.path = "C:/missing/stage.png";
    bg.posX = 0.25f;
    big.addBackground(bg);
    rt::CharacterState a;
    a.characterName = "Rapi";
    a.animation = "idle";
    a.flipX = true;
    big.addCharacter(a);
    rt::CharacterState b;
    b.characterName = "Anis";
    big.addCharacter(b);
    rt::CharacterState hidden;
    hidden.characterName = "Hidden";
    hidden.visible = false;
    big.addCharacter(hidden);
    ASSERT_TRUE(presets.save(big));

    Rig rig;
    rig.ws->setShotPresetManager(&presets);
    rig.clip->setGroupId(9);
    // A clip on another track overlapping the group keeps that track busy.
    rt::Track* v2 = rig.seq0->addVideoTrack("V2");
    auto busy = std::make_unique<rt::VideoClip>();
    busy->setDuration(48000);
    busy->setLayerId("busy");
    v2->addClip(std::move(busy));
    const std::string before = trackLayout(*rig.seq0);

    emit rig.ws->propertiesPanel()->shotSwitchRequested(9, "Big");
    QApplication::processEvents();
    const std::string after = trackLayout(*rig.seq0);
    // The preset lists the background first, so it is the FRONT layer.
    EXPECT_EQ(after, "V[1:background_0] | V[0:char_0] | V[0:char_1] | V[1:busy] | V[] | A[]");
    std::vector<std::string> labels;
    for (size_t ti = 0; ti < rig.seq0->trackCount(); ++ti)
        for (size_t ci = 0; ci < rig.seq0->track(ti)->clipCount(); ++ci) {
            rt::Clip* c = rig.seq0->track(ti)->clip(ci);
            if (c->groupId() == 9)
                labels.push_back(c->label() + "@" + std::to_string(c->timelineIn()) + "+" +
                                 std::to_string(c->duration()) + " x" +
                                 std::to_string(c->positionX().defaultValue()) + " s" +
                                 std::to_string(c->scaleX().defaultValue()));
        }
    std::string joined;
    for (const auto& l : labels) joined += l + ";";
    EXPECT_EQ(joined,
              "stage@0+48000 x-480.000000 s1.000000;"
              "Rapi - idle@0+48000 x0.000000 s-1.000000;"
              "Anis - idle@0+48000 x0.000000 s1.000000;");

    ASSERT_TRUE(rig.stack.undo());
    EXPECT_EQ(trackLayout(*rig.seq0), before);
    ASSERT_TRUE(rig.stack.redo());
    EXPECT_EQ(trackLayout(*rig.seq0), after);

    rig.ws.reset();
    fs::remove_all(dir);
}

TEST(WorkspaceWiring, ShotSwitchUndoAffectsItsOwnSequenceAfterSwitchingSequences)
{
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() /
        ("rt_ws_shot_switch_seq_" + std::to_string(QCoreApplication::applicationPid()));
    fs::remove_all(dir);
    fs::create_directories(dir);
    rt::ShotPresetManager presets;
    presets.scan(dir);
    ASSERT_TRUE(presets.save(rt::ShotPreset::createDefault("Rapi")));

    Rig rig;
    rig.ws->setShotPresetManager(&presets);
    rig.clip->setGroupId(7);
    const uint64_t oldId = rig.clip->id();
    const std::string seq0Before = trackLayout(*rig.seq0);

    emit rig.ws->propertiesPanel()->shotSwitchRequested(7, "Rapi - Default");
    QApplication::processEvents();
    const std::string seq0After = trackLayout(*rig.seq0);
    ASSERT_NE(seq0After, seq0Before);

    // The user opens the other sequence, then undoes.
    rig.project.setActiveSequence(1);
    rig.ws->setTimeline(rig.seq1);
    const std::string seq1Before = trackLayout(*rig.seq1);

    ASSERT_TRUE(rig.stack.undo());
    EXPECT_EQ(trackLayout(*rig.seq1), seq1Before);
    EXPECT_EQ(trackLayout(*rig.seq0), seq0Before);
    EXPECT_EQ(groupMembers(*rig.seq0, 7).visualIds, (std::vector<uint64_t>{oldId}));
    // The open sequence's panels are not pointed at the other sequence's clip.
    const rt::Clip* bound = rig.ws->propertiesPanel()->clip();
    EXPECT_TRUE(bound == nullptr || bound->id() != oldId);

    ASSERT_TRUE(rig.stack.redo());
    EXPECT_EQ(trackLayout(*rig.seq1), seq1Before);
    EXPECT_EQ(trackLayout(*rig.seq0), seq0After);

    rig.ws.reset();
    fs::remove_all(dir);
}
