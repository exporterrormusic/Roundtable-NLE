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
#include "panels/export/ExportRenderExecutor.h"
#include "panels/export/ExportRenderSession.h"
#include "panels/project/ProjectPanel.h"
#include "panels/properties/PropertiesPanel.h"
#include "panels/project/ProjectBin.h"
#include "panels/timeline/DockBehavior.h"
#include "panels/timeline/TimelinePanel.h"
#include "panels/timeline/TimelineWorkspace.h"
#include "cache/FrameCache.h"
#include "CompositeService.h"
#include "GpuContext.h"
#include "GpuWorkSubmission.h"
#include "Nv12Converter.h"
#include "RenderGpuResources.h"
#include "SpineRenderer.h"

#include <QApplication>
#include <QDir>
#include <QDockWidget>
#include <QEventLoop>
#include <QLineEdit>
#include <QMainWindow>
#include <QMenuBar>
#include <QSettings>
#include <QSplitter>
#include <QTabBar>
#include <QThread>
#include <QTimer>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

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

TEST_F(MainWindowTest, DockPanelChromeTracksOneActivePanel)
{
    qApp->setStyleSheet(Theme::stylesheet());

    QMainWindow host;
    host.setDocumentMode(true); // Matches the production MainWindow setting.

    QWidget nestedTabsHost(&host);
    auto* unrelatedTabs = new QTabBar(&nestedTabsHost);
    unrelatedTabs->setDocumentMode(true);
    unrelatedTabs->addTab(QStringLiteral("Unrelated"));

    auto* firstDock = new QDockWidget(QStringLiteral("First"), &host);
    auto* firstEditor = new QLineEdit(firstDock);
    firstDock->setWidget(firstEditor);
    host.addDockWidget(Qt::LeftDockWidgetArea, firstDock);

    auto* secondDock = new QDockWidget(QStringLiteral("Second"), &host);
    auto* secondEditor = new QLineEdit(secondDock);
    secondDock->setWidget(secondEditor);
    host.addDockWidget(Qt::LeftDockWidgetArea, secondDock);
    host.tabifyDockWidget(firstDock, secondDock);

    auto* thirdDock = new QDockWidget(QStringLiteral("Third"), &host);
    auto* thirdEditor = new QLineEdit(thirdDock);
    thirdDock->setWidget(thirdEditor);
    host.addDockWidget(Qt::RightDockWidgetArea, thirdDock);

    DockTabBarWatcher watcher(&host, &host);
    host.installEventFilter(&watcher);

    host.resize(720, 420);
    host.show();
    secondDock->raise();
    secondEditor->setFocus(Qt::OtherFocusReason);
    {
        QEventLoop loop;
        QTimer::singleShot(10, &loop, &QEventLoop::quit);
        loop.exec();
    }

    QTabBar* dockTabs = nullptr;
    for (auto* tabs : host.findChildren<QTabBar*>()) {
        if (tabs->property("roundtableDockTabBar").toBool()) {
            dockTabs = tabs;
            break;
        }
    }
    ASSERT_NE(dockTabs, nullptr);
    EXPECT_FALSE(unrelatedTabs->property("roundtableDockTabBar").toBool());
    EXPECT_EQ(dockTabs->height(), Theme::metrics().panelHeaderHeight);
    EXPECT_TRUE(secondDock->property("panelFocused").toBool());
    EXPECT_FALSE(firstDock->property("panelFocused").toBool());
    EXPECT_FALSE(thirdDock->property("panelFocused").toBool());
    EXPECT_TRUE(dockTabs->property("panelBarActive").toBool());

    thirdEditor->setFocus(Qt::OtherFocusReason);
    {
        QEventLoop loop;
        QTimer::singleShot(10, &loop, &QEventLoop::quit);
        loop.exec();
    }

    EXPECT_TRUE(thirdDock->property("panelFocused").toBool());
    EXPECT_FALSE(firstDock->property("panelFocused").toBool());
    EXPECT_FALSE(secondDock->property("panelFocused").toBool());
    EXPECT_FALSE(dockTabs->property("panelBarActive").toBool());
}

TEST_F(MainWindowTest, ProductionTimelineUsesOneActiveDockTabGroup)
{
    qApp->setStyleSheet(Theme::stylesheet());

    TimelineWorkspace workspace;
    workspace.buildPanels();

    // Saved layouts can move Captions out of the right-side group and tab it
    // with Source Monitor after the watcher has already been constructed.
    auto* sourceDock = workspace.dockForPanel(QStringLiteral("Source Monitor"));
    auto* captionsDock = workspace.dockForPanel(QStringLiteral("Captions"));
    ASSERT_NE(sourceDock, nullptr);
    ASSERT_NE(captionsDock, nullptr);
    auto* dockHost = qobject_cast<QMainWindow*>(sourceDock->parentWidget());
    ASSERT_NE(dockHost, nullptr);
    dockHost->tabifyDockWidget(sourceDock, captionsDock);

    workspace.resize(1600, 900);
    workspace.show();

    sourceDock->raise();
    sourceDock->setFocusPolicy(Qt::StrongFocus);
    sourceDock->setFocus(Qt::OtherFocusReason);
    {
        QEventLoop loop;
        QTimer::singleShot(20, &loop, &QEventLoop::quit);
        loop.exec();
    }

    QTabBar* sourceTabs = nullptr;
    int activeGroups = 0;
    int dockTabBars = 0;
    for (auto* tabs : workspace.findChildren<QTabBar*>()) {
        if (!tabs->property("roundtableDockTabBar").toBool()) continue;
        ++dockTabBars;
        if (tabs->property("panelBarActive").toBool()) ++activeGroups;
        for (int index = 0; index < tabs->count(); ++index) {
            if (tabs->tabText(index) == QStringLiteral("Source Monitor"))
                sourceTabs = tabs;
        }
    }

    EXPECT_GE(dockTabBars, 2);
    ASSERT_NE(sourceTabs, nullptr);
    EXPECT_EQ(sourceTabs->height(), Theme::metrics().panelHeaderHeight);
    EXPECT_TRUE(sourceTabs->property("panelBarActive").toBool());
    EXPECT_EQ(activeGroups, 1);
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

TEST_F(MainWindowTest, IsolatedRenderResourcesDoNotAliasLiveGpuHelpers)
{
    App app;
    ASSERT_TRUE(app.init());

    auto& gpu = GpuContext::get();
    ASSERT_TRUE(gpu.isInitialized());

    RenderGpuResources isolated;
    ASSERT_TRUE(isolated.init());
    EXPECT_NE(isolated.graphicsCommandPool().handle(),
              gpu.graphicsCmdPool().handle());

    auto sharedCompositor = gpu.compositor(64, 36);
    auto isolatedCompositor = isolated.compositor(64, 36);
    ASSERT_NE(sharedCompositor, nullptr);
    ASSERT_NE(isolatedCompositor, nullptr);
    EXPECT_NE(static_cast<void*>(sharedCompositor.get()),
              static_cast<void*>(isolatedCompositor.get()));

    auto sharedEffects = gpu.effectProcessor(64, 36);
    auto isolatedEffects = isolated.effectProcessor(64, 36);
    ASSERT_NE(sharedEffects, nullptr);
    ASSERT_NE(isolatedEffects, nullptr);
    EXPECT_NE(sharedEffects, isolatedEffects);

    // A resolution change must select a new helper generation instead of
    // resizing the old generation in place. The old pointers remain valid for
    // submissions and presented frames that still reference their resources.
    const uint64_t waitsBeforeGenerationSwitch =
        gpu.scheduler().deviceWaitIdleCalls();
    auto sharedCompositor2 = gpu.compositor(96, 54);
    auto isolatedCompositor2 = isolated.compositor(96, 54);
    ASSERT_NE(sharedCompositor2, nullptr);
    ASSERT_NE(isolatedCompositor2, nullptr);
    EXPECT_NE(sharedCompositor2, sharedCompositor);
    EXPECT_NE(isolatedCompositor2, isolatedCompositor);
    EXPECT_EQ(gpu.compositor(64, 36), sharedCompositor);
    EXPECT_EQ(isolated.compositor(64, 36), isolatedCompositor);

    auto sharedTransition = gpu.transitionRenderer(64, 36);
    auto sharedTransition2 = gpu.transitionRenderer(96, 54);
    auto isolatedTransition = isolated.transitionRenderer(64, 36);
    auto isolatedTransition2 = isolated.transitionRenderer(96, 54);
    ASSERT_NE(sharedTransition, nullptr);
    ASSERT_NE(sharedTransition2, nullptr);
    ASSERT_NE(isolatedTransition, nullptr);
    ASSERT_NE(isolatedTransition2, nullptr);
    EXPECT_NE(sharedTransition2, sharedTransition);
    EXPECT_NE(isolatedTransition2, isolatedTransition);
    EXPECT_EQ(gpu.transitionRenderer(64, 36), sharedTransition);
    EXPECT_EQ(isolated.transitionRenderer(64, 36), isolatedTransition);

    auto sharedSpine = gpu.spineRenderer(64, 36);
    auto sharedSpine2 = gpu.spineRenderer(96, 54);
    auto isolatedSpine = isolated.spineRenderer(64, 36);
    auto isolatedSpine2 = isolated.spineRenderer(96, 54);
    ASSERT_NE(sharedSpine, nullptr);
    ASSERT_NE(sharedSpine2, nullptr);
    ASSERT_NE(isolatedSpine, nullptr);
    ASSERT_NE(isolatedSpine2, nullptr);
    EXPECT_NE(sharedSpine2, sharedSpine);
    EXPECT_NE(isolatedSpine2, isolatedSpine);
    EXPECT_EQ(gpu.spineRenderer(64, 36), sharedSpine);
    EXPECT_EQ(isolated.spineRenderer(64, 36), isolatedSpine);

    // Simultaneously active characters at one resolution must own distinct
    // framebuffers and atlas sets. Re-requesting the same exact identity must
    // reuse its renderer so export does not churn atlases every frame.
    auto sharedSpineAlice = gpu.spineRenderer(64, 36, "Alice|Default|0");
    auto sharedSpineBob = gpu.spineRenderer(64, 36, "Bob|Default|0");
    auto isolatedSpineAlice =
        isolated.spineRenderer(64, 36, "Alice|Default|0");
    auto isolatedSpineBob =
        isolated.spineRenderer(64, 36, "Bob|Default|0");
    ASSERT_NE(sharedSpineAlice, nullptr);
    ASSERT_NE(sharedSpineBob, nullptr);
    ASSERT_NE(isolatedSpineAlice, nullptr);
    ASSERT_NE(isolatedSpineBob, nullptr);
    EXPECT_NE(sharedSpineAlice, sharedSpineBob);
    EXPECT_NE(isolatedSpineAlice, isolatedSpineBob);
    EXPECT_EQ(gpu.spineRenderer(64, 36, "Alice|Default|0"),
              sharedSpineAlice);
    EXPECT_EQ(isolated.spineRenderer(64, 36, "Bob|Default|0"),
              isolatedSpineBob);
    EXPECT_NE(sharedSpineAlice->outputDescriptorInfo().imageView,
              sharedSpineBob->outputDescriptorInfo().imageView);
    EXPECT_NE(isolatedSpineAlice->outputDescriptorInfo().imageView,
              isolatedSpineBob->outputDescriptorInfo().imageView);

    // Spine and the compositor share the graphics queue, so framebuffer
    // reuse is ordered by GPU submissions and must never drain a queue on
    // the calling thread. Two cycles also exercise completed command-buffer
    // reclamation before the second allocation.
    const uint64_t queueWaitsBeforeSpine =
        gpu.scheduler().queueWaitIdleCalls();
    for (int frame = 0; frame < 2; ++frame) {
        ASSERT_TRUE(sharedSpine->beginFrame());
        ASSERT_TRUE(sharedSpine->endFrame());
        ASSERT_TRUE(sharedSpine->waitForFrame());
    }
    ASSERT_TRUE(sharedSpineAlice->beginFrame());
    ASSERT_TRUE(sharedSpineAlice->endFrame());
    ASSERT_TRUE(sharedSpineAlice->waitForFrame());
    ASSERT_TRUE(sharedSpineBob->beginFrame());
    ASSERT_TRUE(sharedSpineBob->endFrame());
    ASSERT_TRUE(sharedSpineBob->waitForFrame());
    EXPECT_EQ(gpu.scheduler().queueWaitIdleCalls(),
              queueWaitsBeforeSpine);

    auto nv12A = gpu.nv12Converter(64, 36, 32, 18);
    auto nv12B = gpu.nv12Converter(96, 54, 32, 18);
    ASSERT_NE(nv12A, nullptr);
    ASSERT_NE(nv12B, nullptr);
    EXPECT_NE(nv12A, nv12B);
    EXPECT_EQ(gpu.nv12Converter(64, 36, 32, 18), nv12A);
    EXPECT_EQ(nv12A->outputWidth(), 32u);
    EXPECT_EQ(nv12A->outputHeight(), 18u);

    std::vector<uint8_t> yPlane(64u * 36u, 128u);
    std::vector<uint8_t> uvPlane(64u * 18u, 128u);
    std::vector<uint8_t> converted;
    ASSERT_TRUE(nv12A->convertAndReadbackNV12Scaled(
        yPlane.data(), 64, uvPlane.data(), 64,
        64, 36, 32, 18, converted));
    EXPECT_EQ(converted.size(), 32u * 18u * 4u);
    EXPECT_EQ(gpu.scheduler().deviceWaitIdleCalls(),
              waitsBeforeGenerationSwitch);

    std::weak_ptr<int> submissionLease;
    {
        GpuWorkSubmission ownedSubmission;
        ASSERT_TRUE(ownedSubmission.init(
            gpu.vkDevice(), isolated.graphicsCommandPool().handle()));
        ASSERT_TRUE(ownedSubmission.beginRecording());
        auto lease = std::make_shared<int>(42);
        submissionLease = lease;
        ownedSubmission.retainForCurrentSlot(lease);
        lease.reset();
        EXPECT_FALSE(submissionLease.expired());
        ASSERT_TRUE(ownedSubmission.endRecording());
        ASSERT_TRUE(ownedSubmission.submit(gpu.graphicsQueue()));
        EXPECT_TRUE(ownedSubmission.waitForAll(5'000'000'000ull));
        // waitForAll establishes safety but the slot releases at its explicit
        // reuse/destruction boundary, not from an unrelated waiter.
        EXPECT_FALSE(submissionLease.expired());
    }
    EXPECT_TRUE(submissionLease.expired());

    // Resource-bundle command pools are session-owned. Drop caller leases
    // before explicitly ending that session, matching production shutdown.
    isolatedSpine2.reset();
    isolatedSpine.reset();
    isolatedTransition2.reset();
    isolatedTransition.reset();
    isolatedEffects.reset();
    isolatedCompositor2.reset();
    isolatedCompositor.reset();

    const uint64_t waitsBeforeScopedShutdown =
        gpu.scheduler().deviceWaitIdleCalls();
    isolated.shutdown(GpuTeardownMode::SessionScoped);
    EXPECT_EQ(gpu.scheduler().deviceWaitIdleCalls(),
              waitsBeforeScopedShutdown);

    {
        CompositeService exportService(
            CompositeService::GpuResourceMode::Isolated);
        exportService.shutdown();
    }
    EXPECT_EQ(gpu.scheduler().deviceWaitIdleCalls(),
              waitsBeforeScopedShutdown);
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

TEST_F(MainWindowTest, ExportQueueHasResizableVerticalPane)
{
    ExportPanel panel;

    auto* splitter = panel.queueSplitter();
    ASSERT_NE(splitter, nullptr);
    ASSERT_NE(panel.jobList(), nullptr);
    EXPECT_EQ(splitter->orientation(), Qt::Vertical);
    EXPECT_EQ(splitter->count(), 2);
    EXPECT_EQ(splitter->widget(1), panel.jobList());
    EXPECT_FALSE(splitter->childrenCollapsible());
    EXPECT_FALSE(splitter->isCollapsible(1));
    EXPECT_EQ(panel.jobList()->maximumHeight(), QWIDGETSIZE_MAX);
    EXPECT_EQ(panel.jobList()->sizePolicy().verticalPolicy(), QSizePolicy::Expanding);
}

TEST_F(MainWindowTest, ExportQueueRowsReserveSpaceForCompletedControls)
{
    ExportPanel panel;
    auto project = Project::createNew("Queue Row Layout");
    ASSERT_NE(project, nullptr);
    panel.setProject(project.get());
    panel.setTimeline(project->timeline());
    panel.setExportFrameCallback([](
        const std::shared_ptr<const ExportRenderSnapshot>&,
        int64_t, uint32_t, uint32_t, bool, bool) -> std::shared_ptr<CachedFrame> {
            return {};
        });

    for (int i = 0; i < 3; ++i) {
        panel.outputPath()->setText(
            QDir::temp().filePath(QStringLiteral("queue_row_%1.mp4").arg(i)));
        ASSERT_TRUE(QMetaObject::invokeMethod(
            &panel, "onAddToQueue", Qt::DirectConnection));
    }

    panel.resize(1200, 900);
    panel.show();
    QApplication::processEvents();

    ASSERT_EQ(panel.jobList()->count(), 3);
    int previousBottom = -1;
    for (int i = 0; i < panel.jobList()->count(); ++i) {
        auto* item = panel.jobList()->item(i);
        auto* rowWidget = panel.jobList()->itemWidget(item);
        ASSERT_NE(item, nullptr);
        ASSERT_NE(rowWidget, nullptr);

        auto* reveal = rowWidget->findChild<QPushButton*>(QStringLiteral("JobReveal"));
        ASSERT_NE(reveal, nullptr);
        reveal->show(); // Simulate the control appearing when the job is Done.
        QApplication::processEvents();

        const QRect rect = panel.jobList()->visualItemRect(item);
        EXPECT_GT(rect.top(), previousBottom);
        EXPECT_GE(item->sizeHint().height(), rowWidget->minimumHeight());
        EXPECT_GE(item->sizeHint().height(),
                  Theme::metrics().controlHeightSm + 2 * Theme::metrics().spacingSm);
        previousBottom = rect.bottom();
    }
}

TEST_F(MainWindowTest, ExportExecutorMarshalsAndPipelinesFrames)
{
    ExportRenderExecutor executor;
    executor.beginRun();
    const auto snapshot = std::make_shared<ExportRenderSnapshot>();

    auto pumpUntilReady = [](auto& future) {
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::seconds(2);
        while (future.wait_for(std::chrono::milliseconds(0)) !=
                   std::future_status::ready &&
               std::chrono::steady_clock::now() < deadline) {
            QApplication::processEvents(QEventLoop::AllEvents, 10);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return future.wait_for(std::chrono::milliseconds(0)) ==
               std::future_status::ready;
    };

    bool preflightOnRenderThread = false;
    auto preflightFuture = std::async(std::launch::async, [&] {
        return executor.preflight(snapshot,
            [&](const std::shared_ptr<const ExportRenderSnapshot>&) {
                preflightOnRenderThread =
                    QThread::currentThread() == executor.executionThread();
                RenderPreflightResult result;
                result.status = RenderResultStatus::Ready;
                return result;
            });
    });
    ASSERT_TRUE(pumpUntilReady(preflightFuture));
    EXPECT_EQ(preflightFuture.get().status, RenderResultStatus::Ready);
    EXPECT_TRUE(preflightOnRenderThread);
    EXPECT_NE(executor.executionThread(), QApplication::instance()->thread());

    std::vector<int64_t> renderedTicks;
    const ExportRenderExecutor::RenderCallback renderCallback =
        [&](const std::shared_ptr<const ExportRenderSnapshot>&,
            int64_t tick, uint32_t width, uint32_t height,
            bool, bool preserveAlpha) {
            EXPECT_EQ(QThread::currentThread(), executor.executionThread());
            renderedTicks.push_back(tick);
            RenderResult result;
            result.timelineTick = tick;
            result.status = RenderResultStatus::Ready;
            result.diagnostics.status = result.status;
            result.frame = std::make_shared<CachedFrame>();
            result.frame->width = width;
            result.frame->height = height;
            result.frame->stride = width * 4;
            result.frame->pixels.resize(
                static_cast<size_t>(result.frame->stride) * height, 0x7f);
            result.frame->preservesAlpha = preserveAlpha;
            return result;
        };

    auto firstFuture = std::async(std::launch::async, [&] {
        return executor.render(snapshot, 0, 1600, 2, 2,
                               true, true, renderCallback);
    });
    ASSERT_TRUE(pumpUntilReady(firstFuture));
    const auto first = firstFuture.get();
    ASSERT_EQ(first.status, RenderResultStatus::Ready);
    ASSERT_NE(first.frame, nullptr);
    EXPECT_EQ(first.timelineTick, 0);
    EXPECT_TRUE(first.frame->preservesAlpha);

    auto secondFuture = std::async(std::launch::async, [&] {
        return executor.render(snapshot, 1600, -1, 2, 2,
                               true, true, renderCallback);
    });
    ASSERT_TRUE(pumpUntilReady(secondFuture));
    const auto second = secondFuture.get();
    ASSERT_EQ(second.status, RenderResultStatus::Ready);
    ASSERT_NE(second.frame, nullptr);
    EXPECT_EQ(second.timelineTick, 1600);
    EXPECT_TRUE(second.frame->preservesAlpha);
    EXPECT_EQ(renderedTicks, (std::vector<int64_t>{0, 1600}));

    bool storedOnRenderThread = false;
    EXPECT_TRUE(executor.storeFrame(
        snapshot, 1600, second.frame,
        [&](const std::shared_ptr<const ExportRenderSnapshot>&,
            int64_t storedTick, const std::shared_ptr<CachedFrame>& stored) {
            storedOnRenderThread =
                QThread::currentThread() == executor.executionThread();
            EXPECT_EQ(storedTick, 1600);
            EXPECT_EQ(stored, second.frame);
        }));
    EXPECT_TRUE(storedOnRenderThread);

    executor.discardPendingWork();

    // A completed/canceled executor must be reusable for the next queued run.
    executor.beginRun();
    auto repeatedRun = std::async(std::launch::async, [&] {
        return executor.render(snapshot, 0, -1, 2, 2,
                               true, true, renderCallback);
    });
    ASSERT_TRUE(pumpUntilReady(repeatedRun));
    EXPECT_EQ(repeatedRun.get().status, RenderResultStatus::Ready);
    EXPECT_EQ(renderedTicks, (std::vector<int64_t>{0, 1600, 0}));
    executor.discardPendingWork();
}

TEST_F(MainWindowTest, ExportExecutorStartsNextCompositeBeforeNextWorkerCall)
{
    ExportRenderExecutor executor;
    executor.beginRun();
    const auto snapshot = std::make_shared<ExportRenderSnapshot>();

    std::mutex mutex;
    std::condition_variable cv;
    bool secondEntered = false;
    bool releaseSecond = false;

    const ExportRenderExecutor::RenderCallback renderCallback =
        [&](const std::shared_ptr<const ExportRenderSnapshot>&,
            int64_t tick, uint32_t width, uint32_t height, bool, bool) {
            if (tick == 1600) {
                std::unique_lock lock(mutex);
                secondEntered = true;
                cv.notify_all();
                cv.wait_for(lock, std::chrono::seconds(2),
                            [&] { return releaseSecond; });
            }
            RenderResult result;
            result.timelineTick = tick;
            result.status = RenderResultStatus::Ready;
            result.diagnostics.status = result.status;
            result.frame = std::make_shared<CachedFrame>();
            result.frame->width = width;
            result.frame->height = height;
            result.frame->stride = width * 4;
            result.frame->pixels.resize(
                static_cast<size_t>(result.frame->stride) * height);
            return result;
        };

    auto first = std::async(std::launch::async, [&] {
        return executor.render(snapshot, 0, 1600, 2, 2,
                               true, false, renderCallback);
    });
    ASSERT_EQ(first.wait_for(std::chrono::seconds(2)),
              std::future_status::ready);
    EXPECT_EQ(first.get().timelineTick, 0);

    bool enteredBeforeSecondCall = false;
    {
        std::unique_lock lock(mutex);
        enteredBeforeSecondCall = cv.wait_for(
            lock, std::chrono::seconds(2), [&] { return secondEntered; });
        releaseSecond = true;
    }
    cv.notify_all();
    EXPECT_TRUE(enteredBeforeSecondCall)
        << "frame N+1 was not pre-submitted while frame N was available to encode";

    auto second = std::async(std::launch::async, [&] {
        return executor.render(snapshot, 1600, -1, 2, 2,
                               true, false, renderCallback);
    });
    ASSERT_EQ(second.wait_for(std::chrono::seconds(2)),
              std::future_status::ready);
    EXPECT_EQ(second.get().timelineTick, 1600);

    executor.discardPendingWork();
}

TEST_F(MainWindowTest, ExportExecutorStopCancelsPendingRenderDispatch)
{
    ExportRenderExecutor executor;
    executor.beginRun();
    const auto snapshot = std::make_shared<ExportRenderSnapshot>();
    std::atomic<bool> releaseBlocker{false};
    std::promise<void> blockerEnteredPromise;
    auto blockerEntered = blockerEnteredPromise.get_future();

    auto activeRender = std::async(std::launch::async, [&] {
        return executor.render(
            snapshot, 0, -1, 2, 2, true, false,
            [&](const std::shared_ptr<const ExportRenderSnapshot>&,
                int64_t tick, uint32_t width, uint32_t height, bool, bool) {
                blockerEnteredPromise.set_value();
                const auto deadline = std::chrono::steady_clock::now() +
                                      std::chrono::seconds(2);
                while (!releaseBlocker.load(std::memory_order_acquire) &&
                       std::chrono::steady_clock::now() < deadline) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                RenderResult result;
                result.timelineTick = tick;
                result.status = RenderResultStatus::Ready;
                result.diagnostics.status = result.status;
                result.frame = std::make_shared<CachedFrame>();
                result.frame->width = width;
                result.frame->height = height;
                result.frame->stride = width * 4;
                result.frame->pixels.resize(
                    static_cast<size_t>(result.frame->stride) * height);
                return result;
            });
    });

    if (blockerEntered.wait_for(std::chrono::seconds(1)) !=
        std::future_status::ready) {
        releaseBlocker.store(true, std::memory_order_release);
        executor.requestStop();
        executor.discardPendingWork();
        FAIL() << "render-thread blocker did not start";
    }

    int callbackCalls = 0;

    auto pending = std::async(std::launch::async, [&] {
        return executor.preflight(snapshot,
            [&](const std::shared_ptr<const ExportRenderSnapshot>&) {
                ++callbackCalls;
                RenderPreflightResult result;
                result.status = RenderResultStatus::Ready;
                return result;
            });
    });

    // The render thread is occupied, so this preflight remains queued while
    // its queue worker waits. Cancellation must wake both waiting callers
    // without requiring the queued callbacks to execute.
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    executor.requestStop();
    ASSERT_EQ(pending.wait_for(std::chrono::milliseconds(500)),
              std::future_status::ready);
    EXPECT_EQ(pending.get().status, RenderResultStatus::Canceled);
    ASSERT_EQ(activeRender.wait_for(std::chrono::milliseconds(500)),
              std::future_status::ready);
    EXPECT_EQ(activeRender.get().status, RenderResultStatus::Canceled);
    releaseBlocker.store(true, std::memory_order_release);
    executor.discardPendingWork();
    EXPECT_EQ(callbackCalls, 0);
}

TEST_F(MainWindowTest, ExportExecutorOwnsProductionSessionUntilThreadJoin)
{
    auto project = std::shared_ptr<Project>(
        Project::createNew("Thread-owned export session").release());
    ASSERT_NE(project, nullptr);
    auto snapshot = std::make_shared<ExportRenderSnapshot>();
    snapshot->project = project;
    snapshot->timeline = std::shared_ptr<const Timeline>(
        project, project->sequence(0));
    snapshot->editVersion = 1;
    snapshot->rangeStartTick = 0;
    snapshot->rangeEndTick = 1600;

    auto session = std::make_shared<ExportRenderSession>(
        ExportRenderSession::Dependencies{});
    std::weak_ptr<ExportRenderSession> weakSession = session;

    ExportRenderExecutor executor;
    executor.beginRun(session);
    session.reset();

    auto preflight = std::async(std::launch::async, [&] {
        return executor.preflight(snapshot, {});
    });
    ASSERT_EQ(preflight.wait_for(std::chrono::seconds(2)),
              std::future_status::ready);
    EXPECT_EQ(preflight.get().status, RenderResultStatus::Ready);

    auto render = std::async(std::launch::async, [&] {
        return executor.render(snapshot, 0, -1, 2, 2,
                               true, false, {});
    });
    ASSERT_EQ(render.wait_for(std::chrono::seconds(2)),
              std::future_status::ready);
    const auto result = render.get();
    ASSERT_TRUE(result.isComplete());
    ASSERT_NE(result.frame, nullptr);
    EXPECT_TRUE(executor.storeFrame(snapshot, 0, result.frame, {}));

    EXPECT_FALSE(weakSession.expired());
    executor.discardPendingWork();
    EXPECT_TRUE(weakSession.expired());
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
    EXPECT_EQ(mw->dockCount(), 16);
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
