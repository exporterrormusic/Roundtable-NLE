/*
 * TimelineWorkspacePanelsCreate.cpp — Panel and central widget creation
 * extracted from TimelineWorkspacePanels.cpp::buildPanels().
 *
 * Contains: createPanelWidgets() — creates all dock widgets, tool column,
 * sequence tab bar, toolbar, and timeline panel.
 */

#include "panels/timeline/TimelineWorkspace.h"
#include "PathUtils.h"
#include "panels/timeline/DockLayoutManager.h"
#include "Constants.h"
#include "CompositeService.h"
#include "spine/AnimationVideoCache.h"
#include "ClipRenderers.h"  // src/core/ClipRenderers.h — shared with the gpu module
#include "panels/timeline/DockBehavior.h"
#include "Theme.h"

// Panels
#include "panels/characters/CharactersPanel.h"
#include "panels/library/LibraryPanel.h"
#include "panels/effects/EffectsPanel.h"
#include "panels/project/HistoryPanel.h"
#include "panels/effects/KeyframeEditor.h"
#include "panels/monitors/ScopesPanel.h"
#include "panels/monitors/ProgramMonitor.h"
#include "playback/PlaybackScheduler.h"
#include "panels/project/ProjectBin.h"

#include "widgets/MiniTimeline.h"
#include "panels/properties/PropertiesPanel.h"
#include "panels/effects/EffectControlsPanel.h"
#include "panels/effects/GraphicsEditorPanel.h"
#include "panels/effects/ColorGradingPanel.h"
#include "panels/captions/CaptionsPanel.h"
#include "panels/monitors/SourceMonitor.h"
#include "panels/timeline/TimelinePanel.h"

// Widgets
#include "widgets/DockTitleBar.h"
#include "widgets/ToolButton.h"
#include "widgets/VUMeter.h"
#include "viewport/Viewport.h"
#include "viewport/TransformOverlayWidget.h"

// Core
#include "command/CommandStack.h"
#include "command/LambdaCommand.h"
#include "command/commands/ClipCommands.h"
#include "command/commands/MarkerCommands.h"
#include "project/Project.h"
#include "MainWindow.h"
#include "audio/AudioEngine.h"
#include "audio/AudioFile.h"
#include "audio/AudioPlaybackService.h"
#include "playback/MediaPool.h"
#include "playback/PlaybackController.h"
#include "timeline/AudioClip.h"
#include "timeline/EditOperations.h"
#include "timeline/ImageClip.h"
#include "timeline/SequenceClip.h"
#include "timeline/SpineClip.h"
#include "timeline/TitleClip.h"
#include "timeline/GraphicClip.h"
#include "timeline/GraphicLayer.h"
#include "timeline/Timeline.h"
#include "timeline/MediaRelinker.h"
#include "timeline/Track.h"
#include "timeline/Transition.h"
#include "timeline/VideoClip.h"
#include "command/commands/TransitionCmds.h"
#include "command/commands/EffectCommands.h"

#ifdef ROUNDTABLE_HAS_SPINE
#include "spine/ModelManager.h"
#include "spine/ShotPreset.h"
#include "stb_image.h"
#endif

#include <QDockWidget>
#include <QFileInfo>
#include <QFont>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QFrame>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QContextMenuEvent>
#include <QPainter>
#include <QPointer>
#include <QSettings>
#include <QSet>
#include <QSizePolicy>
#include <QSlider>
#include <QTabWidget>
#include <QToolBar>
#include <QToolButton>
#include <QApplication>
#include <QAbstractSpinBox>
#include <QChildEvent>
#include <QComboBox>
#include <QCursor>
#include <QPlainTextEdit>
#include <QTabBar>
#include <QTextEdit>
#include <QMenu>
#include <QTimer>
#include <QVBoxLayout>
#include <QWindow>

// GPU compositing
#include "GpuContext.h"
#include "Compositor.h"
#include "GpuTextureCache.h"
#include "EffectProcessor.h"
#include "SpineRenderer.h"
#include "TransitionRenderer.h"
#include "vulkan/Texture.h"

#ifdef _WIN32
#include <windows.h>
#include <windowsx.h>
#endif

namespace rt {

namespace {

// Focus-follows-hover for dock panels. When the mouse enters a widget that
// lives inside a QDockWidget, focus the dock's content widget so that
// Qt::WidgetWithChildrenShortcut shortcuts route to the panel under the
// cursor without requiring a click first (Premiere/Resolve behavior).
class HoverFocusFilter : public QObject
{
public:
    using QObject::QObject;
protected:
    bool eventFilter(QObject* watched, QEvent* event) override
    {
        if (event->type() != QEvent::Enter) return false;
        auto* w = qobject_cast<QWidget*>(watched);
        if (!w) return false;

        // Don't retarget mid-drag or while a modal is up.
        if (QApplication::mouseButtons() != Qt::NoButton) return false;
        if (QApplication::activeModalWidget()) return false;

        // Don't steal focus from a user actively typing/editing.
        if (auto* fw = QApplication::focusWidget()) {
            if (qobject_cast<QLineEdit*>(fw)        ||
                qobject_cast<QTextEdit*>(fw)        ||
                qobject_cast<QPlainTextEdit*>(fw)   ||
                qobject_cast<QAbstractSpinBox*>(fw) ||
                qobject_cast<QComboBox*>(fw)) {
                return false;
            }
        }

        // Walk up to the enclosing dock; focus its content widget.
        for (QWidget* p = w; p; p = p->parentWidget()) {
            if (auto* dock = qobject_cast<QDockWidget*>(p)) {
                if (auto* content = dock->widget()) {
                    if (!content->hasFocus())
                        content->setFocus(Qt::MouseFocusReason);
                }
                break;
            }
        }
        return false;  // never consume — let the original target see Enter too
    }
};

// Transport-target selector. Unlike HoverFocusFilter (focus-follows-hover for
// keyboard shortcuts), transport (Space / JKL) must NOT follow the cursor —
// otherwise opening a clip in the Source Monitor never makes it the play
// target, and hovering away after a pause silently re-targets the timeline.
// A mouse press inside the Source Monitor makes it the target; a press inside
// the Program Monitor or the timeline switches back to the timeline; presses
// elsewhere leave the (sticky) choice unchanged.
class TransportFocusFilter : public QObject
{
public:
    explicit TransportFocusFilter(TimelineWorkspace* ws)
        : QObject(ws), m_ws(ws) {}
protected:
    bool eventFilter(QObject* watched, QEvent* event) override
    {
        if (event->type() != QEvent::MouseButtonPress &&
            event->type() != QEvent::MouseButtonDblClick)
            return false;
        auto* w = qobject_cast<QWidget*>(watched);
        if (!w || !m_ws) return false;

        auto inside = [w](QWidget* panel) {
            return panel && (w == panel || panel->isAncestorOf(w));
        };
        if (inside(m_ws->sourceMonitor())) {
            m_ws->setSourceTransportActive(true);
        } else if (inside(m_ws->programMonitor()) ||
                   inside(m_ws->timelinePanel())) {
            m_ws->setSourceTransportActive(false);
        }
        return false;  // never consume
    }
private:
    TimelineWorkspace* m_ws;
};

} // namespace

void TimelineWorkspace::createPanelWidgets()
{
    const auto& tc = Theme::colors();
    const auto& m  = Theme::metrics();

    // -- Hover-focus filter (focus-follows-hover for dock panels) ---------
    static bool s_hoverFocusInstalled = false;
    if (!s_hoverFocusInstalled) {
        qApp->installEventFilter(new HoverFocusFilter(qApp));
        s_hoverFocusInstalled = true;
    }

    // -- Transport-target selector (click-based, NOT hover) ---------------
    qApp->installEventFilter(new TransportFocusFilter(this));

    // -- Floating dock resize filter (Windows only) -----------------------
#ifdef _WIN32
    static bool s_dockResizeFilterInstalled = false;
    if (!s_dockResizeFilterInstalled) {
        QCoreApplication::instance()->installNativeEventFilter(
            new FloatingResizeFallbackFilter());
        s_dockResizeFilterInstalled = true;
    }
#endif

    // -- Helper: create a dock widget and register in the lookup map ------
    auto makeDock = [this](const QString& title, QWidget* widget,
                           QDockWidget::DockWidgetFeatures features =
                               QDockWidget::DockWidgetMovable |
                               QDockWidget::DockWidgetFloatable |
                               QDockWidget::DockWidgetClosable) -> QDockWidget*
    {
        auto* dock = new QDockWidget(title, m_innerMainWindow);
        dock->setFeatures(features);
        dock->setWidget(widget);
        dock->setObjectName(title);
        dock->setTitleBarWidget(new DockTitleBar(dock, title));
        dock->setContentsMargins(4, 0, 4, 4);
        m_dockWidgets.insert(title, dock);

        connect(dock, &QDockWidget::topLevelChanged,
                dock, [dock](bool floating) {
            if (floating) {
                dock->show();
                dock->raise();
#ifdef _WIN32
                QTimer::singleShot(0, dock, [dock]() {
                    if (!dock->isFloating()) return;
                    WId wid = dock->effectiveWinId();
                    if (!wid) return;
                    HWND hwnd = reinterpret_cast<HWND>(wid);
                    HWND topHwnd = GetAncestor(hwnd, GA_ROOT);
                    if (topHwnd) hwnd = topHwnd;

                    QWidget* ownerWidget = QWidget::find(
                        reinterpret_cast<WId>(hwnd));
                    if (!ownerWidget) return;
                    if (qobject_cast<QMainWindow*>(ownerWidget)
                        && !qobject_cast<QDockWidget*>(ownerWidget)
                        && ownerWidget->objectName() != QStringLiteral("EdgeColumn")) {
                        return;
                    }

                    LONG style = GetWindowLong(hwnd, GWL_STYLE);
                    style &= ~WS_CAPTION;
                    style |= WS_THICKFRAME;
                    SetWindowLong(hwnd, GWL_STYLE, style);
                    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER
                                 | SWP_FRAMECHANGED);
                    if (auto* dw = qobject_cast<QDockWidget*>(ownerWidget)) {
                        auto f = dw->features();
                        dw->setFeatures(f ^ QDockWidget::DockWidgetClosable);
                        dw->setFeatures(f);
                    }
                });
                installDockResizeSubclass(dock);
#endif
            }
        });

        return dock;
    };

    // =====================================================================
    //  TOP ROW PANELS -- each in its own dock widget
    // =====================================================================

    // -- Project Bin ------------------------------------------------------
    m_projectBin = new ProjectBin(this);
    m_projectBin->setMinimumWidth(180);
    if (m_mediaPool)
        m_projectBin->setMediaPool(m_mediaPool);
    if (m_mediaSourceService)
        m_projectBin->setMediaSourceService(m_mediaSourceService);
    connect(m_projectBin, &ProjectBin::projectCreated,
            this, [this](Project* project) {
        if (m_destroying.load(std::memory_order_acquire)) return;
        m_projectBin->setProject(project);
        setProject(project);
        emit autoProjectCreated(project);
    });
    makeDock("Project Bin", m_projectBin);

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

    // Spine character animation dropped from the Library: parse the URI
    // ("spine:CharName|outfit|stanceInt|animName"), prefer a cached video
    // (animVideoCache → loadClip) so it plays exactly like the timeline
    // shows it, and fall back to live spine rendering via loadSpineClip().
    connect(m_sourceMonitor, &SourceMonitor::spineDropReceived,
            this, [this](const QString& uri) {
        if (m_destroying.load(std::memory_order_acquire)) return;
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
    });

    connect(m_sourceMonitor, &SourceMonitor::sequenceDropReceived,
            this, [this](size_t seqIdx) {
        if (m_destroying.load(std::memory_order_acquire)) return;
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
        double  fps  = 24.0;

        SourceMonitor::SequenceFrameProvider provider =
            [this, seqIdx](int64_t tick, uint32_t w, uint32_t h, bool scrub)
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
            (void)w; (void)h;
            uint32_t outW = 1920, outH = 1080;
            if (m_project) {
                auto res = m_project->settings().resolution();
                if (res.width  > 0) outW = static_cast<uint32_t>(res.width);
                if (res.height > 0) outH = static_cast<uint32_t>(res.height);
                if (outW > 1920) { outH = outH * 1920 / outW; outW = 1920; }
                if (outH > 1080) { outW = outW * 1080 / outH; outH = 1080; }
            }
            if (outW == 0) outW = 640;
            if (outH == 0) outH = 360;

            // Bind the composite service to the inner sequence for this
            // frame, composite, then restore. Force CPU display mode so
            // compositeFrame does the GPU→CPU readback INLINE while the
            // composite mutex is still held — otherwise the lazyReadback
            // path races with the FrameProducer's next composite (which
            // can resize/recycle the readback staging buffer and the
            // deferred map returns false, leaving the source monitor black).
            //
            // KNOWN LIMITATION: shares the program monitor's CompositeService,
            // so m_lastGoodComposite is briefly overwritten. Clean per-
            // monitor compositing is a bigger refactor.
            const bool wasGpuMode = m_compositeService->gpuDisplayMode();
            m_compositeService->setGpuDisplayMode(false);
            m_compositeService->setTimeline(innerTimeline);
            auto frame = compositeFrame(tick, outW, outH, scrub);
            m_compositeService->setTimeline(m_timeline);
            m_compositeService->setGpuDisplayMode(wasGpuMode);
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
                                       std::move(provider),
                                       std::move(timelineGetter));
    });

    connect(m_sourceMonitor, &SourceMonitor::playbackStarted,
            this, [this]() {
        if (m_destroying.load(std::memory_order_acquire)) return;
        if (m_playbackController &&
            m_playbackController->state() != PlayState::Stopped &&
            m_playbackController->state() != PlayState::Paused) {
            m_playbackController->pause();
        }
        invalidateAudioSources();
    });

    // Scrubbing the Source Monitor mini-timeline pauses main timeline
    // playback so only one monitor is actively driving audio/video.
    connect(m_sourceMonitor, &SourceMonitor::scrubbed,
            this, [this]() {
        if (m_destroying.load(std::memory_order_acquire)) return;
        if (m_playbackController &&
            m_playbackController->state() != PlayState::Stopped &&
            m_playbackController->state() != PlayState::Paused) {
            m_playbackController->pause();
            invalidateAudioSources();
        }
    });

    makeDock("Source Monitor", m_sourceMonitor);

    // -- Program Monitor --------------------------------------------------
    m_programMonitor = new ProgramMonitor(this);
    m_programMonitor->setMinimumWidth(200);
    if (m_timeline) m_programMonitor->setTimeline(m_timeline);
    if (m_playbackController) m_programMonitor->setController(m_playbackController);
    makeDock("Program Monitor", m_programMonitor);

    // -- Properties -------------------------------------------------------
    m_propertiesPanel = new PropertiesPanel(this);
    if (m_commandStack) m_propertiesPanel->setCommandStack(m_commandStack);
    if (m_timeline) m_propertiesPanel->setTimeline(m_timeline);
    if (m_modelManager) m_propertiesPanel->setModelManager(m_modelManager);
    if (m_shotPresetManager) m_propertiesPanel->setShotPresetManager(m_shotPresetManager);
#ifdef ROUNDTABLE_HAS_SPINE
    m_propertiesPanel->setAnimationNamesProvider(
        [this](const std::string& charName, const std::string& outfit, int stance)
            -> std::vector<std::string> {
        if (m_destroying.load(std::memory_order_acquire)) return {};
        const std::string key = charName + "|" + outfit + "|" + std::to_string(stance);
        if (!m_compositeService) return {};
        {
            auto* cached = m_compositeService->findAnimNames(key);
            if (cached) return *cached;
        }
        auto sharedData = m_compositeService->findSpineSharedData(key);
        for (auto& [cid, state] : m_compositeService->spineCache()) {
            if (state && state->shared && state->shared == sharedData
                && state->engine.isLoaded()) {
                auto anims = state->engine.animation().listAnimations();
                std::vector<std::string> names;
                names.reserve(anims.size());
                for (const auto& info : anims) {
                    if (info.name == "talk_end" || info.name == "talk_start")
                        continue;
                    names.push_back(info.name);
                }
                m_compositeService->storeAnimNames(key, names);
                return names;
            }
        }
        if (sharedData && !sharedData->skelBytes.empty() && !sharedData->atlasText.empty()) {
            SpineEngine tmpEngine;
            if (tmpEngine.loadSkeletonFromBuffers(
                    sharedData->skelBytes, sharedData->atlasText,
                    sharedData->atlasDir,
                    sharedData->skelPath, sharedData->atlasPath)) {
                auto anims = tmpEngine.animation().listAnimations();
                std::vector<std::string> names;
                names.reserve(anims.size());
                for (const auto& info : anims) {
                    if (info.name == "talk_end" || info.name == "talk_start")
                        continue;
                    names.push_back(info.name);
                }
                m_compositeService->storeAnimNames(key, names);
                return names;
            }
        }
        return {};
    });
#endif
    m_propertiesPanel->setVideoAnimNamesProvider(
        [](const std::string& charName, const std::string& outfit)
            -> std::vector<std::string> {
        namespace fs = std::filesystem;
        std::vector<std::string> names;
        static const char* fmtDirs[] = {"H264_Green", "H264_Blue", "H264_Custom", "ProRes"};
        fs::path dir;
        for (const auto* fmt : fmtDirs) {
            auto candidate = fs::path("assets/converted") / fmt / charName / outfit;
            std::error_code ec2;
            if (fs::exists(candidate, ec2)) { dir = candidate; break; }
        }
        if (dir.empty()) dir = fs::path("assets/converted") / "H264_Green" / charName / outfit;
        std::error_code ec;
        for (auto& entry : fs::directory_iterator(dir, ec)) {
            if (!entry.is_regular_file()) continue;
            std::string stem = pathToUtf8(entry.path().stem());
            if (stem.size() > 5 && stem.substr(stem.size() - 5) == "_talk")
                continue;
            names.push_back(stem);
        }
        std::sort(names.begin(), names.end());
        return names;
    });
    makeDock("Properties", m_propertiesPanel);

    // -- Effect Controls --------------------------------------------------
    m_effectControlsPanel = new EffectControlsPanel(this);
    if (m_commandStack) m_effectControlsPanel->setCommandStack(m_commandStack);
    if (m_timeline) m_effectControlsPanel->setTimeline(m_timeline);
    makeDock("Effect Controls", m_effectControlsPanel);

    // -- Color Correction -------------------------------------------------
    m_ColorGradingPanel = new ColorGradingPanel(this);
    if (m_commandStack) m_ColorGradingPanel->setCommandStack(m_commandStack);
    if (m_timeline) m_ColorGradingPanel->setTimeline(m_timeline);
    makeDock("Color Correction", m_ColorGradingPanel);

    // -- Effects ----------------------------------------------------------
    m_effectsPanel = new EffectsPanel(this);
    if (m_commandStack) m_effectsPanel->setCommandStack(m_commandStack);
    makeDock("Effects", m_effectsPanel);

    // -- History ----------------------------------------------------------
    m_historyPanel = new HistoryPanel(this);
    if (m_commandStack) m_historyPanel->setCommandStack(m_commandStack);
    makeDock("History", m_historyPanel);

    // -- Scopes -----------------------------------------------------------
    m_scopesPanel = new ScopesPanel(this);
    makeDock("Scopes", m_scopesPanel);

    // -- Graphics Editor --------------------------------------------------
    m_GraphicsEditorPanel = new GraphicsEditorPanel(this);
    if (m_commandStack) m_GraphicsEditorPanel->setCommandStack(m_commandStack);
    if (m_timeline) m_GraphicsEditorPanel->setTimeline(m_timeline);
    makeDock("Graphics Editor", m_GraphicsEditorPanel);

    // -- Captions ---------------------------------------------------------
    m_captionsPanel = new CaptionsPanel(this);
    if (m_commandStack) m_captionsPanel->setCommandStack(m_commandStack);
    if (m_timeline) m_captionsPanel->setTimeline(m_timeline);
    makeDock("Captions", m_captionsPanel);

    // -- Library ----------------------------------------------------------
    m_libraryPanel = new LibraryPanel(this);
    m_charactersPanel = m_libraryPanel->charactersPanel();
    m_charactersPanel->setModelManager(m_modelManager);
#ifdef ROUNDTABLE_HAS_SPINE
    if (m_compositeService)
        m_charactersPanel->setAnimVideoCache(m_compositeService->animVideoCache());
    if (m_compositeService && m_compositeService->animVideoCache()) {
        m_compositeService->animVideoCache()->setCompletionCallback(
            [panel = m_charactersPanel](const std::string&, const std::string&,
                                         const std::string&, bool) {
                QMetaObject::invokeMethod(panel, &CharactersPanel::refresh,
                                          Qt::QueuedConnection);
            });
    }
#endif
    m_charactersPanel->setMediaPool(m_mediaPool);
    m_libraryPanel->refreshCurrentTab();
    connect(m_libraryPanel, &LibraryPanel::mediaRelinkRequested,
            this, [this](const QString& oldPath, const QString& newPath) {
        if (m_destroying.load(std::memory_order_acquire)) return;
        const std::string oldStd = oldPath.toStdString();
        const std::string newStd = newPath.toStdString();
        int n = 0;
        if (m_timeline)
            n += MediaRelinker::relinkPath(m_timeline, oldStd, newStd);
        if (m_shotPresetManager)
            n += MediaRelinker::relinkPresetBackground(m_shotPresetManager, oldStd, newStd);
        if (n > 0) {
            invalidateCompositeCache();
            if (m_programMonitor) m_programMonitor->requestRefresh();
            if (m_timelinePanel) m_timelinePanel->update();
        }
    });
    connect(m_libraryPanel, &LibraryPanel::loadInSourceMonitor,
            this, [this](const QString& filePath) {
        if (m_destroying.load(std::memory_order_acquire)) return;
        if (!m_sourceMonitor || !m_mediaPool || filePath.isEmpty()) return;
        const auto handle = m_mediaPool->open(filePath.toStdString());
        if (handle == 0) return;
        m_sourceMonitor->loadClip(handle, m_mediaPool);
        // Make the Source Monitor the transport target (next Space plays it).
        setSourceTransportActive(true);
    });
    makeDock("Library", m_libraryPanel);

    // -- Audio Meters (VU Meter) ------------------------------------------
    auto* meterContainer = new QWidget;
    meterContainer->setMinimumWidth(30);
    meterContainer->setMaximumWidth(200);
    meterContainer->setStyleSheet(QStringLiteral("background: %1;")
        .arg(Theme::hex(tc.surface0)));
    auto* meterLayout = new QVBoxLayout(meterContainer);
    meterLayout->setContentsMargins(m.spacingXs, m.spacingMd, m.spacingXs, m.spacingMd);
    meterLayout->setSpacing(m.spacingXs);

    m_timelineVUMeter = new VUMeter(meterContainer);
    m_timelineVUMeter->setChannelCount(2);
    m_timelineVUMeter->setOrientation(VUMeter::Orientation::Vertical);
    m_timelineVUMeter->setScaleVisible(true);
    m_timelineVUMeter->setPeakHoldEnabled(true);
    m_timelineVUMeter->setMinimumHeight(100);
    meterLayout->addWidget(m_timelineVUMeter, 1);

    auto* dbLabel = new QLabel(QStringLiteral("dB"));
    dbLabel->setAlignment(Qt::AlignCenter);
    dbLabel->setStyleSheet(QStringLiteral("color: %1; font-size: 9px; border: none;")
        .arg(Theme::hex(tc.textTertiary)));
    meterLayout->addWidget(dbLabel);

    auto* dockAudioMeters = makeDock("Audio Meters", meterContainer);
    dockAudioMeters->setMinimumWidth(60);

    // =====================================================================
    //  CENTRAL WIDGET: Tool Column + Toolbar + Timeline Panel
    // =====================================================================
    auto* centralContainer = new QWidget;
    centralContainer->setMinimumHeight(200);
    auto* centralLayout = new QHBoxLayout(centralContainer);
    centralLayout->setContentsMargins(0, 0, 0, 0);
    centralLayout->setSpacing(0);

    // -- Left: Vertical Tool Column (Premiere Pro style) ------------------
    auto* toolColumn = new QWidget;
    toolColumn->setMinimumWidth(48);
    toolColumn->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
    toolColumn->setStyleSheet(QStringLiteral(
        "QWidget { background: %1; }")
        .arg(Theme::hex(tc.surface0)));
    auto* toolColumnLayout = new QVBoxLayout(toolColumn);
    toolColumnLayout->setContentsMargins(4, 6, 4, 0);
    toolColumnLayout->setSpacing(0);
    toolColumnLayout->setAlignment(Qt::AlignHCenter | Qt::AlignTop);

    auto* btnSelection = new ToolButton(ToolButton::Selection);
    btnSelection->setFixedSize(40, 34);
    btnSelection->setToolTip(QStringLiteral("Selection Tool (A)"));
    btnSelection->setChecked(true);

    auto* btnRazor = new ToolButton(ToolButton::Razor);
    btnRazor->setFixedSize(40, 34);
    btnRazor->setToolTip(QStringLiteral("Razor Tool (B)"));

    auto* btnRipple = new ToolButton(ToolButton::Ripple);
    btnRipple->setFixedSize(40, 34);
    btnRipple->setToolTip(QStringLiteral("Ripple Edit Tool (N)"));

    auto* btnRolling = new ToolButton(ToolButton::Rolling);
    btnRolling->setFixedSize(40, 34);
    btnRolling->setToolTip(QStringLiteral("Rolling Edit Tool (R)"));

    auto* btnSlip = new ToolButton(ToolButton::Slip);
    btnSlip->setFixedSize(40, 34);
    btnSlip->setToolTip(QStringLiteral("Slip Tool (S)"));

    auto* btnSlide = new ToolButton(ToolButton::Slide);
    btnSlide->setFixedSize(40, 34);
    btnSlide->setToolTip(QStringLiteral("Slide Tool (U)"));

    auto* btnText = new ToolButton(ToolButton::Text);
    btnText->setFixedSize(40, 34);
    btnText->setToolTip(QStringLiteral("Text Tool (T)"));

    auto* btnZoom = new ToolButton(ToolButton::Zoom);
    btnZoom->setFixedSize(40, 34);
    btnZoom->setToolTip(QStringLiteral("Zoom Tool (Z) — Click: Zoom In, Alt+Click: Zoom Out"));

    auto addToolSep = [&]() {
        auto* sep = new QFrame;
        sep->setFrameShape(QFrame::HLine);
        sep->setFrameShadow(QFrame::Plain);
        sep->setFixedHeight(1);
        sep->setStyleSheet(QStringLiteral(
            "QFrame { color: %1; }")
            .arg(Theme::hex(tc.border)));
        toolColumnLayout->addSpacing(3);
        toolColumnLayout->addWidget(sep);
        toolColumnLayout->addSpacing(3);
    };

    toolColumnLayout->addWidget(btnSelection);
    addToolSep();
    toolColumnLayout->addWidget(btnRazor);
    addToolSep();
    toolColumnLayout->addWidget(btnRipple);
    addToolSep();
    toolColumnLayout->addWidget(btnRolling);
    addToolSep();
    toolColumnLayout->addWidget(btnSlip);
    addToolSep();
    toolColumnLayout->addWidget(btnSlide);
    addToolSep();
    toolColumnLayout->addWidget(btnText);
    addToolSep();
    toolColumnLayout->addWidget(btnZoom);
    toolColumnLayout->addStretch();

    // Store tool buttons for sync with shortcuts
    m_toolButtons[0] = btnSelection;
    m_toolButtons[1] = btnRipple;
    m_toolButtons[2] = btnRolling;
    m_toolButtons[3] = btnRazor;
    m_toolButtons[4] = btnSlip;
    m_toolButtons[5] = btnSlide;
    m_toolButtons[6] = btnText;
    m_toolButtons[7] = btnZoom;

    // -- Center: Toolbar + Timeline Panel ---------------------------------
    auto* centerContainer = new QWidget;
    auto* centerLayout = new QVBoxLayout(centerContainer);
    centerLayout->setContentsMargins(0, 0, 0, 0);
    centerLayout->setSpacing(0);

    // -- Sequence tab bar row ---------------------------------------------
    m_sequenceTabBar = new QTabBar;
    m_sequenceTabBar->setTabsClosable(true);
    m_sequenceTabBar->setExpanding(false);
    m_sequenceTabBar->setMovable(true);
    m_sequenceTabBar->setDrawBase(false);
    m_sequenceTabBar->setDocumentMode(true);
    m_sequenceTabBar->setStyleSheet(QStringLiteral(
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
    m_sequenceTabBar->addTab(QStringLiteral("Sequence 1"));
    connect(m_sequenceTabBar, &QTabBar::currentChanged, this, [this](int index) {
        if (m_destroying.load(std::memory_order_acquire)) return;
        if (m_suppressTabChange || index < 0) return;
        if (index >= static_cast<int>(m_tabToSeq.size())) return;
        emit sequenceTabChanged(m_tabToSeq[static_cast<size_t>(index)]);
    });
    connect(m_sequenceTabBar, &QTabBar::tabCloseRequested, this, [this](int tabIdx) {
        if (m_destroying.load(std::memory_order_acquire)) return;
        if (tabIdx < 0 || tabIdx >= static_cast<int>(m_tabToSeq.size())) return;
        if (!m_project) return;

        size_t seqIdx = m_tabToSeq[static_cast<size_t>(tabIdx)];

        // Don't close the last open tab
        if (m_openSequenceTabs.size() <= 1) return;

        // Remove from open set
        m_openSequenceTabs.erase(seqIdx);

        // If closing the active sequence, switch to the first remaining one.
        // Don't call setActiveSequence here — let switchSequence() handle
        // all the timeline/playback/UI wiring.
        // IMPORTANT: emit sequenceTabChanged BEFORE refreshSequenceTabs so
        // switchSequence() sees a different old-vs-new active index and
        // takes the full refresh path (setTimeline, rebuildTracks,
        // pm->refresh).  If refreshSequenceTabs runs first it silently
        // changes the project's active to newActive, causing switchSequence
        // to hit the early-return "already active" path which skips all
        // UI refresh — leaving the timeline/program monitor stuck on the
        // closed sequence until the user clicks another tab.
        if (m_project->activeSequenceIndex() == seqIdx) {
            size_t newActive = *m_openSequenceTabs.begin();
            emit sequenceTabChanged(newActive);
        } else {
            refreshSequenceTabs();
        }
    });
    m_sequenceTabBar->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_sequenceTabBar, &QWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        if (m_destroying.load(std::memory_order_acquire)) return;
        int tabIdx = m_sequenceTabBar->tabAt(pos);
        if (tabIdx < 0 || tabIdx >= static_cast<int>(m_tabToSeq.size()) || !m_project) return;

        size_t seqIdx = m_tabToSeq[static_cast<size_t>(tabIdx)];

        QMenu menu(m_sequenceTabBar);
        QAction* renameAction = menu.addAction("Rename Sequence...");
        QAction* settingsAction = menu.addAction("Sequence Settings...");
        QAction* dupAction    = menu.addAction("Duplicate Sequence");
        menu.addSeparator();
        QAction* closeAction  = menu.addAction("Close");
        closeAction->setEnabled(m_openSequenceTabs.size() > 1);
        QAction* closeOthersAction = menu.addAction("Close Other Tabs");
        closeOthersAction->setEnabled(m_openSequenceTabs.size() > 1);

        QAction* chosen = menu.exec(m_sequenceTabBar->mapToGlobal(pos));
        if (chosen == renameAction) {
            emit sequenceTabRenameRequested(seqIdx);
        } else if (chosen == settingsAction) {
            emit sequenceTabSettingsRequested(seqIdx);
        } else if (chosen == dupAction) {
            emit sequenceTabDuplicateRequested(seqIdx);
        } else if (chosen == closeAction) {
            // Don't close the last open tab
            if (m_openSequenceTabs.size() <= 1) return;
            m_openSequenceTabs.erase(seqIdx);
            // IMPORTANT: when closing the active sequence, emit
            // sequenceTabChanged BEFORE refreshSequenceTabs (see the
            // tabCloseRequested handler above for the full rationale).
            // refreshSequenceTabs silently flips the backend's active to
            // newActive; if it runs first, switchSequence() then hits its
            // "already active" early-return and skips the timeline rebuild,
            // leaving the panel stuck on the just-closed sequence.
            if (m_project->activeSequenceIndex() == seqIdx) {
                size_t newActive = *m_openSequenceTabs.begin();
                emit sequenceTabChanged(newActive);
            } else {
                refreshSequenceTabs();
            }
        } else if (chosen == closeOthersAction) {
            // Close every other open sequence, keeping only the right-clicked
            // one (Premiere's "Close Other Tabs"). The kept tab becomes active.
            if (m_openSequenceTabs.size() <= 1) return;
            const bool keptWasActive = (m_project->activeSequenceIndex() == seqIdx);
            m_openSequenceTabs.clear();
            m_openSequenceTabs.insert(seqIdx);
            // Same ordering hazard as Close above: if the kept tab wasn't the
            // active one, emit BEFORE refreshing so switchSequence() takes the
            // full refresh path instead of the "already active" early-return.
            if (!keptWasActive) {
                emit sequenceTabChanged(seqIdx);
            } else {
                refreshSequenceTabs();
            }
        }
    });
    centerLayout->addWidget(m_sequenceTabBar);

    // -- Timeline toolbar row ---------------------------------------------
    auto* toolbar = new QWidget;
    toolbar->setFixedHeight(30);
    toolbar->setStyleSheet(QStringLiteral(
        "QWidget#timelineToolbar { background: %1; border-top: 1px solid %2; "
        "border-bottom: 1px solid %3; }")
        .arg(Theme::hex(tc.surface2))
        .arg(Theme::hex(tc.surface3))
        .arg(Theme::hex(tc.border)));
    toolbar->setObjectName(QStringLiteral("timelineToolbar"));

    auto* toolbarLayout = new QHBoxLayout(toolbar);
    toolbarLayout->setContentsMargins(6, 0, 6, 0);
    toolbarLayout->setSpacing(2);

    m_timelineTimecode = new QLabel(QStringLiteral("00;00;00;00"));
    m_timelineTimecode->setAlignment(Qt::AlignCenter);
    m_timelineTimecode->setFixedWidth(100);
    m_timelineTimecode->setFixedHeight(22);
    m_timelineTimecode->setStyleSheet(QStringLiteral(
        "QLabel { font-family: 'Consolas', monospace; font-size: 12px; "
        "font-weight: bold; color: %1; background: %2; "
        "padding: 1px 4px; border-radius: 2px; }")
        .arg(Theme::hex(tc.textBright))
        .arg(Theme::hex(tc.surface0)));
    toolbarLayout->addWidget(m_timelineTimecode);
    toolbarLayout->addSpacing(8);

    QString tbIconStyle = QStringLiteral(
        "QToolButton { background: transparent; color: %1; border: none; "
        "padding: 2px; font-size: 14px; min-width: 24px; min-height: 24px; }"
        "QToolButton:hover { background: %2; border-radius: 3px; }"
        "QToolButton:checked { color: %3; background: %4; border-radius: 3px; }")
        .arg(Theme::hex(tc.textSecondary))
        .arg(Theme::hex(tc.surface3))
        .arg(Theme::hex(tc.accent))
        .arg(Theme::hex(tc.accentSubtle));

    auto* btnNest = new QToolButton;
    btnNest->setText(QStringLiteral("\u29C9"));
    btnNest->setToolTip(tr("Insert or overwrite sequences as nests or individual clips"));
    btnNest->setCheckable(true);
    btnNest->setStyleSheet(tbIconStyle);
    toolbarLayout->addWidget(btnNest);

    auto* btnSnap = new QToolButton;
    btnSnap->setText(QStringLiteral("\U0001F9F2"));
    btnSnap->setToolTip(tr("Snap in Timeline (N)"));
    btnSnap->setCheckable(true);
    btnSnap->setChecked(true);
    btnSnap->setStyleSheet(tbIconStyle);
    toolbarLayout->addWidget(btnSnap);

    auto* btnLinked = new QToolButton;
    btnLinked->setText(QStringLiteral("\U0001F517"));
    btnLinked->setToolTip(tr("Linked Selection"));
    btnLinked->setCheckable(true);
    btnLinked->setChecked(true);
    btnLinked->setStyleSheet(tbIconStyle);
    toolbarLayout->addWidget(btnLinked);

    auto* btnMarker = new QToolButton;
    btnMarker->setText(QStringLiteral("\u2666"));
    btnMarker->setToolTip(tr("Add Marker (M)"));
    btnMarker->setStyleSheet(tbIconStyle);
    toolbarLayout->addWidget(btnMarker);

    auto* btnCC = new QToolButton;
    btnCC->setText(QStringLiteral("CC"));
    btnCC->setToolTip(tr("Caption Track Options"));
    btnCC->setStyleSheet(QStringLiteral(
        "QToolButton { background: transparent; color: %1; border: 1px solid %1; "
        "padding: 1px 3px; font-size: 11px; font-weight: bold; min-width: 22px; "
        "min-height: 16px; border-radius: 2px; }"
        "QToolButton:hover { background: %2; color: %3; border-color: %3; }")
        .arg(Theme::hex(tc.textSecondary))
        .arg(Theme::hex(tc.surface3))
        .arg(Theme::hex(tc.textPrimary)));
    toolbarLayout->addWidget(btnCC);

    auto* spacer = new QWidget;
    spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    toolbarLayout->addWidget(spacer);

    auto* zoomLabel = new QLabel(QStringLiteral("\u2212"));
    zoomLabel->setStyleSheet(QStringLiteral("color: %1; font-size: 12px; padding: 0 2px;")
        .arg(Theme::hex(tc.textTertiary)));
    toolbarLayout->addWidget(zoomLabel);

    auto* zoomSlider = new QSlider(Qt::Horizontal);
    zoomSlider->setRange(10, 500);
    zoomSlider->setValue(100);
    zoomSlider->setFixedWidth(120);
    zoomSlider->setToolTip("Timeline Zoom");
    zoomSlider->setStyleSheet(QStringLiteral(
        "QSlider::groove:horizontal { background: %1; height: 4px; border-radius: 2px; }"
        "QSlider::handle:horizontal { background: %2; width: 10px; margin: -3px 0; border-radius: 5px; }"
        "QSlider::handle:horizontal:hover { background: %3; }")
        .arg(Theme::hex(tc.border))
        .arg(Theme::hex(tc.textSecondary))
        .arg(Theme::hex(tc.textPrimary)));
    toolbarLayout->addWidget(zoomSlider);

    auto* zoomPlusLabel = new QLabel(QStringLiteral("+"));
    zoomPlusLabel->setStyleSheet(QStringLiteral("color: %1; font-size: 12px; padding: 0 2px;")
        .arg(Theme::hex(tc.textTertiary)));
    toolbarLayout->addWidget(zoomPlusLabel);

    centerLayout->addWidget(toolbar);

    // -- Timeline Panel ----------------------------------------------------
    m_timelinePanel = new TimelinePanel(this);
    if (m_timeline) m_timelinePanel->setTimeline(m_timeline);
    if (m_compositeService) m_timelinePanel->setCompositeService(m_compositeService.get());
    if (m_commandStack) m_timelinePanel->setCommandStack(m_commandStack);
    if (m_shortcutManager) m_timelinePanel->setShortcutManager(m_shortcutManager);
    if (m_mediaPool) m_timelinePanel->setMediaPool(m_mediaPool);
#ifdef ROUNDTABLE_HAS_SPINE
    if (m_compositeService && m_compositeService->animVideoCache())
        m_timelinePanel->setAnimVideoCache(m_compositeService->animVideoCache());
#endif
    centerLayout->addWidget(m_timelinePanel, 1);

    // Connect zoom slider to timeline layout engine
    connect(zoomSlider, &QSlider::valueChanged, this, [this](int value) {
        if (m_destroying.load(std::memory_order_acquire)) return;
        if (m_timelinePanel) {
            m_timelinePanel->layoutEngine().setPixelsPerSecond(static_cast<double>(value));
            m_timelinePanel->update();
            for (auto* child : m_timelinePanel->findChildren<QWidget*>())
                child->update();
        }
    });

    // Connect linked selection toggle
    connect(btnLinked, &QToolButton::toggled, this, [this](bool checked) {
        if (m_destroying.load(std::memory_order_acquire)) return;
        if (m_timelinePanel) {
            m_timelinePanel->setLinkedSelectionEnabled(checked);
        }
    });

    // Connect Add Marker button
    connect(btnMarker, &QToolButton::clicked, this, [this]() {
        if (m_destroying.load(std::memory_order_acquire)) return;
        if (m_timeline && m_playbackController && m_commandStack) {
            int64_t tick = m_playbackController->currentTick();
            m_commandStack->execute(
                std::make_unique<AddMarkerCommand>(m_timeline, tick, "Marker"));
            if (m_timelinePanel) m_timelinePanel->update();
        }
    });

    // Connect Nest Sequences toggle
    connect(btnNest, &QToolButton::toggled, this, [this, btnNest](bool checked) {
        if (m_destroying.load(std::memory_order_acquire)) return;
        btnNest->setToolTip(checked
            ? tr("Insert sequences as nested clips")
            : tr("Insert sequences as individual clips"));
        if (m_timelinePanel)
            m_timelinePanel->setNestSequencesEnabled(checked);
    });

    // Connect Caption Track Options button
    connect(btnCC, &QToolButton::clicked, this, [this, btnCC]() {
        if (m_destroying.load(std::memory_order_acquire)) return;
        QMenu menu(btnCC);
        menu.setStyleSheet(QStringLiteral(
            "QMenu { background: %1; color: %2; border: 1px solid %3; }"
            "QMenu::item:selected { background: %4; }")
            .arg(Theme::hex(Theme::colors().surface1),
                 Theme::hex(Theme::colors().textPrimary),
                 Theme::hex(Theme::colors().border),
                 Theme::hex(Theme::colors().accent)));
        // The caption track is created from the Captions panel
        // ("Add Captions to Timeline"); this button only toggles its
        // visibility. Disable the toggles when no caption track exists.
        const bool hasCaptionTrack = m_timeline && m_timeline->captionTrack();
        QAction* aHide = menu.addAction(tr("Hide Captions Track"), this, [this]() {
            if (m_destroying.load(std::memory_order_acquire)) return;
            if (m_timelinePanel) m_timelinePanel->setCaptionTrackVisible(false);
            invalidateCompositeCache();
            if (m_programMonitor) m_programMonitor->requestRefresh();
        });
        QAction* aShow = menu.addAction(tr("Show Captions Track"), this, [this]() {
            if (m_destroying.load(std::memory_order_acquire)) return;
            if (m_timelinePanel) m_timelinePanel->setCaptionTrackVisible(true);
            invalidateCompositeCache();
            if (m_programMonitor) m_programMonitor->requestRefresh();
        });
        aHide->setEnabled(hasCaptionTrack);
        aShow->setEnabled(hasCaptionTrack);
        menu.exec(btnCC->mapToGlobal(QPoint(0, btnCC->height())));
    });

    centralLayout->addWidget(centerContainer);

    // Wrap tool column in its own dock widget
    auto* dockTools = makeDock("Tools", toolColumn,
        QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable);

    m_innerMainWindow->setCentralWidget(centralContainer);
}

} // namespace rt
