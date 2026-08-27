/*
 * test_main_window.cpp — Unit tests for Step 26: Main Window & Workspace
 *
 * Tests:
 *   - Theme colors and palette
 *   - App initialization lifecycle
 *   - MainWindow construction and panel creation
 *   - Menu bar structure
 *   - Toolbar structure
 *   - Dock widget management
 *   - Workspace save/restore
 *   - Full-screen toggle
 */

#include <gtest/gtest.h>

#include "App.h"
#include "MainWindow.h"
#include "Theme.h"
#include "ShortcutManager.h"

#include "command/CommandStack.h"
#include "project/Project.h"
#include "project/ProjectSerializer.h"
#include "timeline/Timeline.h"

// Panels (for type checking)
#include "panels/characters/CharacterBrowser.h"
#include "panels/effects/EffectControlsPanel.h"
#include "panels/export/ExportPanel.h"
#include "panels/project/ProjectPanel.h"
#include "panels/properties/PropertiesPanel.h"
#include "panels/project/ProjectBin.h"
#include "panels/timeline/TimelinePanel.h"
#include "panels/timeline/TimelineWorkspace.h"

#include <QApplication>
#include <QDockWidget>
#include <QMenuBar>
#include <QSettings>
#include <QTabBar>

#include <memory>

// ═════════════════════════════════════════════════════════════════════════════
// QApplication fixture
// ═════════════════════════════════════════════════════════════════════════════

namespace {

int    g_argc = 1;
char   g_arg0[] = "test_main_window";
char*  g_argv[] = {g_arg0, nullptr};

class MainWindowTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        if (!QApplication::instance()) {
            m_app = std::make_unique<QApplication>(g_argc, g_argv);
        }
        // Headless UI tests must never block on the first-use personal-model
        // acknowledgement dialog.
        QSettings().setValue(
            QStringLiteral("transcription/crisperWhisperPersonalAccepted"), true);
    }

    std::unique_ptr<QApplication> m_app;
};

} // anonymous namespace

using namespace rt;

// ═════════════════════════════════════════════════════════════════════════════
// Theme tests
// ═════════════════════════════════════════════════════════════════════════════

TEST(ThemeTest, ColorsNotNull)
{
    const auto& c = Theme::colors();
    EXPECT_TRUE(c.window.isValid());
    EXPECT_TRUE(c.text.isValid());
    EXPECT_TRUE(c.base.isValid());
    EXPECT_TRUE(c.highlight.isValid());
}

TEST(ThemeTest, WindowIsDark)
{
    const auto& c = Theme::colors();
    // Dark theme window should have low luminance
    EXPECT_LT(c.window.red(), 60);
    EXPECT_LT(c.window.green(), 60);
    EXPECT_LT(c.window.blue(), 60);
}

TEST(ThemeTest, TextIsBright)
{
    const auto& c = Theme::colors();
    EXPECT_GT(c.text.red(), 150);
    EXPECT_GT(c.text.green(), 150);
    EXPECT_GT(c.text.blue(), 150);
}

TEST(ThemeTest, PaletteValid)
{
    QPalette p = Theme::palette();
    EXPECT_EQ(p.color(QPalette::Window), Theme::colors().window);
    EXPECT_EQ(p.color(QPalette::Text), Theme::colors().text);
    EXPECT_EQ(p.color(QPalette::Highlight), Theme::colors().highlight);
}

TEST(ThemeTest, StylesheetNotEmpty)
{
    QString ss = Theme::stylesheet();
    EXPECT_FALSE(ss.isEmpty());
    EXPECT_TRUE(ss.contains("QDockWidget"));
    EXPECT_TRUE(ss.contains("QMenuBar"));
    EXPECT_TRUE(ss.contains("QScrollBar"));
    EXPECT_TRUE(ss.contains("QToolButton"));
    EXPECT_TRUE(ss.contains("QStatusBar"));
    EXPECT_TRUE(ss.contains("QTabBar"));
}

TEST(ThemeTest, TimelineColors)
{
    const auto& c = Theme::colors();
    // Playhead should be blueish (Premiere-style)
    EXPECT_GT(c.playhead.blue(), 200);
    // Clip colors should be distinguishable
    EXPECT_NE(c.clipVideo, c.clipAudio);
    EXPECT_NE(c.clipAudio, c.clipTitle);
    EXPECT_NE(c.clipTitle, c.clipSpine);
}

TEST(ThemeTest, DisabledPaletteIsDimmer)
{
    QPalette p = Theme::palette();
    QColor normalText = p.color(QPalette::Active, QPalette::Text);
    QColor disabledText = p.color(QPalette::Disabled, QPalette::Text);
    // Disabled text should be dimmer (lower value)
    EXPECT_LT(disabledText.lightness(), normalText.lightness());
}

TEST(ThemeTest, AccentColorExists)
{
    const auto& c = Theme::colors();
    EXPECT_TRUE(c.accent.isValid());
    // Accent should be blueish
    EXPECT_GT(c.accent.blue(), c.accent.red());
}

// ═════════════════════════════════════════════════════════════════════════════
// App lifecycle
// ═════════════════════════════════════════════════════════════════════════════

TEST_F(MainWindowTest, AppInitBasic)
{
    App app;
    EXPECT_FALSE(app.isInitialized());
    EXPECT_TRUE(app.init());
    EXPECT_TRUE(app.isInitialized());
}

TEST_F(MainWindowTest, AppSubsystemsCreated)
{
    App app;
    app.init();
    EXPECT_NE(app.timeline(), nullptr);
    EXPECT_NE(app.commandStack(), nullptr);
    EXPECT_NE(app.shortcutManager(), nullptr);
    EXPECT_NE(app.audioEngine(), nullptr);
}

TEST_F(MainWindowTest, AppInstance)
{
    App app;
    EXPECT_EQ(App::instance(), &app);
}

TEST_F(MainWindowTest, AppDoubleInit)
{
    App app;
    EXPECT_TRUE(app.init());
    EXPECT_TRUE(app.init()); // Idempotent
}

TEST_F(MainWindowTest, AppCreateMainWindow)
{
    App app;
    app.init();
    EXPECT_TRUE(app.createMainWindow());
    EXPECT_NE(app.mainWindow(), nullptr);
}

TEST_F(MainWindowTest, AppMainWindowBeforeInit)
{
    App app;
    EXPECT_FALSE(app.createMainWindow()); // Should fail
}

// ═════════════════════════════════════════════════════════════════════════════
// MainWindow construction
// ═════════════════════════════════════════════════════════════════════════════

TEST_F(MainWindowTest, ConstructDefault)
{
    MainWindow mw;
    // Version-agnostic: the title embeds ROUNDTABLE_VERSION, which bumps
    // per release (a hardcoded "v2.0" rotted when the version moved on).
    EXPECT_TRUE(mw.windowTitle().startsWith("ROUNDTABLE NLE"));
    EXPECT_GE(mw.minimumWidth(), 1280);
    EXPECT_GE(mw.minimumHeight(), 720);
}

TEST_F(MainWindowTest, NoPanelsBeforeBuild)
{
    MainWindow mw;
    EXPECT_EQ(mw.timelinePanel(), nullptr);
    EXPECT_EQ(mw.projectBin(), nullptr);
    EXPECT_EQ(mw.dockCount(), 0);
}

TEST_F(MainWindowTest, BuildPanelsCreatesAllPanels)
{
    MainWindow mw;
    Timeline tl;
    CommandStack cs;

    mw.setTimeline(&tl);
    mw.setCommandStack(&cs);
    mw.buildPanels();

    EXPECT_NE(mw.timelinePanel(), nullptr);
    EXPECT_NE(mw.sourceMonitor(), nullptr);
    EXPECT_NE(mw.programMonitor(), nullptr);
    EXPECT_NE(mw.projectBin(), nullptr);
    EXPECT_NE(mw.propertiesPanel(), nullptr);
    EXPECT_NE(mw.effectsPanel(), nullptr);
}

TEST_F(MainWindowTest, BuildPanelsIdempotent)
{
    MainWindow mw;
    mw.buildPanels();
    int count = mw.dockCount();
    mw.buildPanels(); // Should not create duplicates
    EXPECT_EQ(mw.dockCount(), count);
}

TEST_F(MainWindowTest, ClosingSequenceTabImmediatelyUpdatesSavedProjectState)
{
    auto project = Project::createNew("Sequence Tab State");
    project->addSequence("Sequence 2");
    project->addSequence("Sequence 3");
    project->setOpenSequenceIndices({0, 1, 2});

    TimelineWorkspace workspace;
    workspace.buildPanels();
    workspace.setProject(project.get());

    auto* tabs = workspace.findChild<QTabBar*>(QStringLiteral("SequenceTabBar"));
    ASSERT_NE(tabs, nullptr);
    ASSERT_EQ(tabs->count(), 3);

    tabs->tabCloseRequested(1);

    EXPECT_EQ(tabs->count(), 2);
    EXPECT_EQ(project->openSequenceIndices(), (std::vector<size_t>{0, 2}));

    ProjectSerializer serializer;
    auto loaded = serializer.deserialize(serializer.serialize(*project));
    ASSERT_NE(loaded, nullptr);
    EXPECT_EQ(loaded->openSequenceIndices(), (std::vector<size_t>{0, 2}));
}

TEST_F(MainWindowTest, DockCount)
{
    MainWindow mw;
    mw.buildPanels();
    // 16 dock widgets in the dockable layout. Update this count when a
    // dock panel is added/removed — it catches accidentally dropped docks.
    EXPECT_EQ(mw.dockCount(), 16);
}

// ═════════════════════════════════════════════════════════════════════════════
// Dock widget management
// ═════════════════════════════════════════════════════════════════════════════

TEST_F(MainWindowTest, DockForPanelFound)
{
    MainWindow mw;
    mw.buildPanels();

    // Dockable layout: dockForPanel returns the QDockWidget for named panels
    EXPECT_NE(mw.dockForPanel("Project Bin"), nullptr);
    EXPECT_NE(mw.dockForPanel("Source Monitor"), nullptr);
    EXPECT_NE(mw.dockForPanel("Program Monitor"), nullptr);
    EXPECT_NE(mw.dockForPanel("Effect Controls"), nullptr);
    EXPECT_NE(mw.timelinePanel(), nullptr);
    EXPECT_NE(mw.propertiesPanel(), nullptr);
    EXPECT_NE(mw.projectBin(), nullptr);
}

TEST_F(MainWindowTest, DockForPanelNotFound)
{
    MainWindow mw;
    mw.buildPanels();
    EXPECT_EQ(mw.dockForPanel("Nonexistent"), nullptr);
}

TEST_F(MainWindowTest, DockContainsPanelWidget)
{
    MainWindow mw;
    mw.buildPanels();

    // Dock widgets contain the actual panel widgets
    auto* dock = mw.dockForPanel("Project Bin");
    ASSERT_NE(dock, nullptr);
    EXPECT_EQ(dock->widget(), mw.projectBin());

    auto* dockProps = mw.dockForPanel("Effect Controls");
    ASSERT_NE(dockProps, nullptr);
    EXPECT_EQ(dockProps->widget(), qobject_cast<QWidget*>(mw.effectControlsPanel()));
}

TEST_F(MainWindowTest, PanelsAccessible)
{
    MainWindow mw;
    mw.buildPanels();

    // All panels should be accessible
    EXPECT_NE(mw.sourceMonitor(), nullptr);
    EXPECT_NE(mw.programMonitor(), nullptr);
    EXPECT_NE(mw.effectsPanel(), nullptr);
    EXPECT_NE(mw.historyPanel(), nullptr);
}

// ═════════════════════════════════════════════════════════════════════════════
// Menu bar
// ═════════════════════════════════════════════════════════════════════════════

TEST_F(MainWindowTest, MenuBarCreated)
{
    MainWindow mw;
    mw.buildPanels();
    mw.buildMenuBar();

    auto* mb = mw.menuBar();
    EXPECT_NE(mb, nullptr);

    auto actions = mb->actions();
    EXPECT_GE(actions.size(), 5); // File, Edit, View, Timeline, Audio, Help
}

TEST_F(MainWindowTest, MenuBarHasFileMenu)
{
    MainWindow mw;
    mw.buildMenuBar();

    auto menus = mw.menuBar()->actions();
    bool found = false;
    for (auto* a : menus) {
        if (a->text().contains("File")) { found = true; break; }
    }
    EXPECT_TRUE(found) << "File menu not found";
}

TEST_F(MainWindowTest, MenuBarHasEditMenu)
{
    MainWindow mw;
    mw.buildMenuBar();

    auto menus = mw.menuBar()->actions();
    bool found = false;
    for (auto* a : menus) {
        if (a->text().contains("Edit")) { found = true; break; }
    }
    EXPECT_TRUE(found) << "Edit menu not found";
}

// ═════════════════════════════════════════════════════════════════════════════
// Workspace
// ═════════════════════════════════════════════════════════════════════════════

TEST_F(MainWindowTest, FullScreenToggle)
{
    MainWindow mw;
    EXPECT_FALSE(mw.isFullScreenPreview());
}

TEST_F(MainWindowTest, WorkspaceDefaultLayout)
{
    MainWindow mw;
    mw.buildPanels();
    mw.applyDefaultLayout();

    // Default layout starts on Projects page
    EXPECT_EQ(mw.currentPage(), Page::Projects);
}

TEST_F(MainWindowTest, PageNavigation)
{
    MainWindow mw;
    mw.buildPanels();

    mw.setCurrentPage(Page::Projects);
    EXPECT_EQ(mw.currentPage(), Page::Projects);

    mw.setCurrentPage(Page::Audio);
    EXPECT_EQ(mw.currentPage(), Page::Audio);

    mw.setCurrentPage(Page::Characters);
    EXPECT_EQ(mw.currentPage(), Page::Characters);

    mw.setCurrentPage(Page::Export);
    EXPECT_EQ(mw.currentPage(), Page::Export);

    mw.setCurrentPage(Page::Timeline);
    EXPECT_EQ(mw.currentPage(), Page::Timeline);
}

TEST_F(MainWindowTest, PageCount)
{
    MainWindow mw;
    EXPECT_EQ(mw.pageCount(), 5);
}

TEST_F(MainWindowTest, WorkspaceSaveRestore)
{
    MainWindow mw;
    mw.buildPanels();
    mw.applyDefaultLayout();
    mw.saveWorkspace("test_layout");

    bool ok = mw.restoreWorkspace("test_layout");
    EXPECT_TRUE(ok);
}

TEST_F(MainWindowTest, WorkspaceRestoreNonexistent)
{
    MainWindow mw;
    EXPECT_FALSE(mw.restoreWorkspace("does_not_exist_xyz"));
}

// ═════════════════════════════════════════════════════════════════════════════
// Undo/Redo integration
// ═════════════════════════════════════════════════════════════════════════════

TEST_F(MainWindowTest, UndoRedoWithCommandStack)
{
    MainWindow mw;
    CommandStack stack;
    mw.setCommandStack(&stack);

    // No crash when undo with empty stack
    mw.menuBar(); // Ensure menu constructed
    // Direct call should be safe
    EXPECT_FALSE(stack.canUndo());
    EXPECT_FALSE(stack.canRedo());
}

// ═════════════════════════════════════════════════════════════════════════════
// App + MainWindow integration
// ═════════════════════════════════════════════════════════════════════════════

TEST_F(MainWindowTest, AppFullIntegration)
{
    App app;
    EXPECT_TRUE(app.init());
    EXPECT_TRUE(app.createMainWindow());

    auto* mw = app.mainWindow();
    ASSERT_NE(mw, nullptr);
    EXPECT_EQ(mw->dockCount(), 15);
    EXPECT_NE(mw->timelinePanel(), nullptr);
    EXPECT_NE(mw->exportPanel(), nullptr);
    EXPECT_NE(mw->projectPanel(), nullptr);
    EXPECT_EQ(mw->pageCount(), 5);
}

// ═════════════════════════════════════════════════════════════════════════════
// StatusBar
// ═════════════════════════════════════════════════════════════════════════════

TEST_F(MainWindowTest, StatusBarExists)
{
    MainWindow mw;
    EXPECT_NE(mw.statusBar(), nullptr);
}

// ═════════════════════════════════════════════════════════════════════════════
// Dock nesting enabled
// ═════════════════════════════════════════════════════════════════════════════

TEST_F(MainWindowTest, CharacterBrowserAccessor)
{
    MainWindow mw;
    mw.buildPanels();
    EXPECT_NE(mw.characterBrowser(), nullptr);
}

TEST_F(MainWindowTest, ProjectPanelAccessor)
{
    MainWindow mw;
    mw.buildPanels();
    EXPECT_NE(mw.projectPanel(), nullptr);
}
