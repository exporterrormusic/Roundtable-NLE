/*
 * MainWindowUIWorkspace.cpp — Panel accessors, workspace persistence, and
 * status bar extracted from MainWindowUI.cpp.
 *
 * Contains: panel accessors (timelinePanel, sourceMonitor, etc.),
 * applyDefaultLayout(), saveWorkspace(), restoreWorkspace(),
 * saveWorkspaceToFile(), restoreWorkspaceFromFile(),
 * restoreWorkspacePreset(), setupStatusBar(), showBusyIndicator(),
 * hideBusyIndicator().
 */

#include "MainWindow.h"

#include "panels/characters/CharacterShotPanel.h"
#include "panels/timeline/TimelineWorkspace.h"
#include "Settings.h"

#include <QDataStream>
#include <QFile>
#include <QLabel>
#include <QProgressBar>
#include <QStatusBar>
#include <QTemporaryDir>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>

#include <spdlog/spdlog.h>

namespace rt {

// ═════════════════════════════════════════════════════════════════════════════
// Panel accessors (delegate through TimelineWorkspace)
// ═════════════════════════════════════════════════════════════════════════════

TimelinePanel*   MainWindow::timelinePanel()   const noexcept { return m_timelineWorkspace ? m_timelineWorkspace->timelinePanel()   : nullptr; }
SourceMonitor*   MainWindow::sourceMonitor()   const noexcept { return m_timelineWorkspace ? m_timelineWorkspace->sourceMonitor()   : nullptr; }
ProgramMonitor*  MainWindow::programMonitor()  const noexcept { return m_timelineWorkspace ? m_timelineWorkspace->programMonitor()  : nullptr; }
ProjectBin*      MainWindow::projectBin()      const noexcept { return m_timelineWorkspace ? m_timelineWorkspace->projectBin()      : nullptr; }
PropertiesPanel* MainWindow::propertiesPanel() const noexcept { return m_timelineWorkspace ? m_timelineWorkspace->propertiesPanel() : nullptr; }
EffectControlsPanel* MainWindow::effectControlsPanel() const noexcept { return m_timelineWorkspace ? m_timelineWorkspace->effectControlsPanel() : nullptr; }
EffectsPanel*    MainWindow::effectsPanel()    const noexcept { return m_timelineWorkspace ? m_timelineWorkspace->effectsPanel()    : nullptr; }
AudioMixer*      MainWindow::audioMixer()      const noexcept { return m_timelineWorkspace ? m_timelineWorkspace->audioMixer()      : nullptr; }
KeyframeEditor*  MainWindow::keyframeEditor()  const noexcept { return m_timelineWorkspace ? m_timelineWorkspace->keyframeEditor()  : nullptr; }
HistoryPanel*    MainWindow::historyPanel()    const noexcept { return m_timelineWorkspace ? m_timelineWorkspace->historyPanel()    : nullptr; }

CharacterBrowser* MainWindow::characterBrowser() const noexcept { return m_characterShotPanel ? m_characterShotPanel->characterBrowser() : nullptr; }
ShotComposer*     MainWindow::shotComposer()     const noexcept { return m_characterShotPanel ? m_characterShotPanel->shotComposer()     : nullptr; }

QDockWidget* MainWindow::dockForPanel(const QString& panelName) const
{
    return m_timelineWorkspace ? m_timelineWorkspace->dockForPanel(panelName) : nullptr;
}

int MainWindow::dockCount() const noexcept
{
    return m_timelineWorkspace ? m_timelineWorkspace->dockCount() : 0;
}

// ═════════════════════════════════════════════════════════════════════════════
// Workspace
// ═════════════════════════════════════════════════════════════════════════════

void MainWindow::applyDefaultLayout()
{
    // Restore window geometry from the last session so the window appears
    // at the correct position/size immediately.
    auto settings = rt::appSettings();
    settings.beginGroup("workspace/last_session");

    QByteArray geo = settings.value("geometry").toByteArray();
    bool savedCollapsed = settings.value("navCollapsed", false).toBool();
    if (!geo.isEmpty()) {
        restoreGeometry(geo);
    }

    settings.endGroup();

    // ── Load the canonical default layout ───────────────────────────────
    // Always set the dock layout to the USE_AS_DEFAULT preset (if it exists)
    // or the bundled/programmatic default.  The last_session dock layout is
    // intentionally NOT restored here — it may contain corrupt closedDocks
    // data from previous sessions.  The canonical default (USE_AS_DEFAULT)
    // should be the baseline for the "no project" state.  When a project is
    // opened later, its own saved workspace (project/xxx) overrides this.
    if (m_timelineWorkspace)
        m_timelineWorkspace->resetToDefaultDockLayout();

    // Always start on the PROJECTS page
    setCurrentPage(Page::Projects);

    // Defer sidebar collapse until after the window is shown and laid out.
    if (savedCollapsed != m_navCollapsed) {
        QTimer::singleShot(0, this, [this]() {
            toggleNavRail();
        });
    }
}

void MainWindow::saveWorkspace(const QString& name)
{
    auto settings = rt::appSettings();
    settings.beginGroup("workspace/" + name);
    settings.setValue("geometry", saveGeometry());
    settings.setValue("activePage", static_cast<int>(currentPage()));
    settings.setValue("navCollapsed", m_navCollapsed);

    // Save the Timeline workspace dock layout (panel positions, sizes, tab order)
    if (m_timelineWorkspace)
        m_timelineWorkspace->saveDockLayout(settings);

    settings.endGroup();
    spdlog::info("Workspace '{}' saved", name.toStdString());
}

bool MainWindow::restoreWorkspace(const QString& name)
{
    auto settings = rt::appSettings();
    settings.beginGroup("workspace/" + name);

    QByteArray geo = settings.value("geometry").toByteArray();

    if (geo.isEmpty()) {
        settings.endGroup();
        spdlog::warn("No saved workspace '{}'", name.toStdString());
        return false;
    }

    restoreGeometry(geo);
    // Restore the last active page so the user returns to where they left off.
    int savedPage = settings.value("activePage",
                                    static_cast<int>(Page::Projects)).toInt();
    if (savedPage < 0 || savedPage > static_cast<int>(Page::Export))
        savedPage = static_cast<int>(Page::Projects);
    setCurrentPage(static_cast<Page>(savedPage));

    // Restore sidebar collapsed/expanded state
    bool savedCollapsed = settings.value("navCollapsed", false).toBool();
    if (savedCollapsed != m_navCollapsed)
        toggleNavRail();

    // NOTE: The Timeline dock layout is intentionally NOT restored here.
    // The canonical default layout (USE_AS_DEFAULT preset) is always
    // applied via resetToDefaultDockLayout() / applyDefaultLayout().
    // Restoring a saved workspace's dock layout would override the
    // user's preferred default arrangement with stale saved data.
    // Project-specific dock layouts are not used — the panel arrangement
    // is global, set by the USE_AS_DEFAULT preset for all projects.

    settings.endGroup();
    spdlog::info("Workspace '{}' restored (dock layout skipped, using USE_AS_DEFAULT)", name.toStdString());
    return true;
}

void MainWindow::saveWorkspaceToFile(const QString& filePath)
{
    QFile file(filePath);
    if (!file.open(QIODevice::WriteOnly)) {
        spdlog::warn("saveWorkspaceToFile: cannot write {}", filePath.toStdString());
        return;
    }

    QDataStream stream(&file);
    stream.setVersion(QDataStream::Qt_6_5);

    // Serialize the workspace to a temporary QSettings (IniFormat),
    // then write and read back as a single QByteArray blob.
    {
        QTemporaryDir tmpDir;
        QString iniPath = tmpDir.filePath("workspace.ini");
        QSettings tmpSettings(iniPath, QSettings::IniFormat);
        tmpSettings.beginGroup("workspace/last_session");
        tmpSettings.setValue("geometry", saveGeometry());
        tmpSettings.setValue("activePage", static_cast<qint32>(currentPage()));
        tmpSettings.setValue("navCollapsed", m_navCollapsed);
        if (m_timelineWorkspace)
            m_timelineWorkspace->saveDockLayout(tmpSettings);
        tmpSettings.endGroup();
        tmpSettings.sync();

        // Read the ini file back into a byte array
        QFile iniFile(iniPath);
        if (iniFile.open(QIODevice::ReadOnly)) {
            QByteArray fileBytes = iniFile.readAll();
            stream << fileBytes;
        }
    }

    file.close();
    spdlog::info("Workspace layout saved to {}", filePath.toStdString());
}

bool MainWindow::restoreWorkspaceFromFile(const QString& filePath)
{
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly))
        return false;

    QDataStream stream(&file);
    stream.setVersion(QDataStream::Qt_6_5);

    QByteArray iniData;
    stream >> iniData;
    if (iniData.isEmpty()) {
        file.close();
        return false;
    }
    file.close();

    // Write the ini data to a temp file and load via QSettings(IniFormat)
    QTemporaryDir tmpDir;
    QString iniPath = tmpDir.filePath("workspace.ini");
    {
        QFile iniFile(iniPath);
        if (!iniFile.open(QIODevice::WriteOnly)) return false;
        iniFile.write(iniData);
        iniFile.close();
    }

    QSettings fileSettings(iniPath, QSettings::IniFormat);
    fileSettings.beginGroup("workspace/last_session");

    QByteArray geo = fileSettings.value("geometry").toByteArray();
    if (geo.isEmpty()) {
        fileSettings.endGroup();
        return false;
    }

    restoreGeometry(geo);

    // Read the saved active page (default to Projects for backward compat)
    int savedPage = fileSettings.value("activePage",
                                        static_cast<int>(Page::Projects)).toInt();
    setCurrentPage(static_cast<Page>(savedPage));

    bool savedCollapsed = fileSettings.value("navCollapsed", false).toBool();
    if (savedCollapsed != m_navCollapsed)
        toggleNavRail();

    // Restore dock layout (deferred if widget not yet visible)
    if (m_timelineWorkspace)
        m_timelineWorkspace->restoreDockLayout(fileSettings);

    fileSettings.endGroup();
    spdlog::info("Workspace layout restored from {}", filePath.toStdString());
    return true;
}

void MainWindow::restoreWorkspacePreset(const QString& presetName)
{
    if (!m_timelineWorkspace) return;
    auto s = rt::appSettings();
    s.beginGroup("WorkspacePresets/" + presetName);
    if (s.childKeys().isEmpty() && s.childGroups().isEmpty()) {
        spdlog::warn("Workspace preset '{}' not found", presetName.toStdString());
        s.endGroup();
        return;
    }
    m_timelineWorkspace->restoreDockLayout(s);
    s.endGroup();
    spdlog::info("Workspace preset '{}' restored", presetName.toStdString());
}

// ═════════════════════════════════════════════════════════════════════════════
// Status bar
// ═════════════════════════════════════════════════════════════════════════════

void MainWindow::setupStatusBar()
{
    auto* sb = statusBar();
    sb->showMessage("Ready");

    // Busy spinner (indeterminate progress bar) — hidden by default
    m_busySpinner = new QProgressBar(sb);
    m_busySpinner->setRange(0, 0);          // indeterminate
    m_busySpinner->setFixedSize(120, 14);
    m_busySpinner->setTextVisible(false);
    m_busySpinner->setVisible(false);

    m_busyLabel = new QLabel(sb);
    m_busyLabel->setStyleSheet("color: palette(text); font-size: 11px;");
    m_busyLabel->setVisible(false);

    sb->addPermanentWidget(m_busyLabel);
    sb->addPermanentWidget(m_busySpinner);
}

void MainWindow::showBusyIndicator(const QString& message)
{
    if (m_busyLabel)   { m_busyLabel->setText(message); m_busyLabel->setVisible(true); }
    if (m_busySpinner) { m_busySpinner->setVisible(true); }
}

void MainWindow::hideBusyIndicator()
{
    if (m_busySpinner) m_busySpinner->setVisible(false);
    if (m_busyLabel)   m_busyLabel->setVisible(false);
    statusBar()->showMessage("Ready");
}

// ═════════════════════════════════════════════════════════════════════════════
// Project-open input lock (full-window non-interactive overlay)
// ═════════════════════════════════════════════════════════════════════════════

void MainWindow::engageLoadingOverlay(const QString& message)
{
    m_projectLoading.store(true, std::memory_order_release);
    showBusyIndicator(message);

    // Lazily create the overlay.  It is a top child of the window: being
    // opaque-to-mouse and raised, it eats clicks destined for the panels
    // below.  Keyboard shortcuts that travel through MainWindow's qApp event
    // filter are separately swallowed there while m_projectLoading is set.
    if (!m_loadingOverlay) {
        m_loadingOverlay = new QWidget(this);
        m_loadingOverlay->setObjectName("projectLoadingOverlay");
        // Plain QWidget honors a stylesheet background only with styled-bg set.
        m_loadingOverlay->setAttribute(Qt::WA_StyledBackground, true);
        m_loadingOverlay->setStyleSheet(
            "#projectLoadingOverlay { background: rgba(0,0,0,150); }");
        // Receive (and discard) mouse events instead of forwarding them.
        m_loadingOverlay->setAttribute(Qt::WA_NoMousePropagation, true);

        m_loadingOverlayLabel = new QLabel(m_loadingOverlay);
        m_loadingOverlayLabel->setAlignment(Qt::AlignCenter);
        m_loadingOverlayLabel->setStyleSheet(
            "color: white; font-size: 15px; font-weight: 600; "
            "background: transparent;");

        m_loadingOverlayBar = new QProgressBar(m_loadingOverlay);
        m_loadingOverlayBar->setFixedWidth(320);
        m_loadingOverlayBar->setTextVisible(false);

        // Center the label + bar in the overlay.  The label spans the full
        // overlay width (text centered via its own alignment) so longer
        // strings like "Loading media… 100 / 111" are never clipped.
        auto* lay = new QVBoxLayout(m_loadingOverlay);
        lay->addStretch(1);
        lay->addWidget(m_loadingOverlayLabel);
        lay->addSpacing(12);
        lay->addWidget(m_loadingOverlayBar, 0, Qt::AlignHCenter);
        lay->addStretch(1);

        // Smoothing timer: eases the bar toward m_loadingBarTarget so the
        // bursty (6-thread) warmup progress fills smoothly instead of jumping.
        m_loadingBarTimer = new QTimer(this);
        m_loadingBarTimer->setInterval(16);  // ~60 fps
        connect(m_loadingBarTimer, &QTimer::timeout, this, [this]() {
            if (!m_loadingOverlayBar || m_loadingOverlayBar->maximum() <= 0) return;
            const int cur = m_loadingOverlayBar->value();
            if (cur < m_loadingBarTarget) {
                const int step = std::max(1, (m_loadingBarTarget - cur) / 4);
                m_loadingOverlayBar->setValue(cur + step);
            }
            if (m_loadingOverlayLabel)
                m_loadingOverlayLabel->setText(
                    tr("Loading media… %1 / %2")
                        .arg(m_loadingOverlayBar->value()).arg(m_loadingBarTotal));
        });
    }

    if (m_loadingOverlayLabel) m_loadingOverlayLabel->setText(message);
    // Start indeterminate: during the off-thread parse we don't yet know how
    // many media handles there are.  The warmup phase switches it to a real
    // 0..total bar via setLoadingProgress().
    m_loadingBarTarget = 0;
    m_loadingBarTotal  = 0;
    if (m_loadingOverlayBar) m_loadingOverlayBar->setRange(0, 0);
    m_loadingOverlay->setGeometry(rect());
    m_loadingOverlay->show();
    m_loadingOverlay->raise();

    // Watchdog: never leave the UI permanently locked if the warmup-finished
    // signal somehow doesn't arrive (early-return in preOpenVideoMedia, a
    // crashed worker, etc.).  30s is far longer than any real open.
    if (!m_loadingWatchdog) {
        m_loadingWatchdog = new QTimer(this);
        m_loadingWatchdog->setSingleShot(true);
        connect(m_loadingWatchdog, &QTimer::timeout, this, [this]() {
            if (m_projectLoading.load(std::memory_order_acquire)) {
                spdlog::warn("[ASYNC-OPEN] loading-overlay watchdog fired — "
                             "releasing input lock after timeout");
                disengageLoadingOverlay();
            }
        });
    }
    m_loadingWatchdog->start(30000);
}

void MainWindow::disengageLoadingOverlay()
{
    if (m_loadingWatchdog) m_loadingWatchdog->stop();
    if (m_loadingBarTimer) m_loadingBarTimer->stop();
    m_projectLoading.store(false, std::memory_order_release);
    if (m_loadingOverlay) m_loadingOverlay->hide();
    hideBusyIndicator();
}

void MainWindow::onBackgroundWarmupFinished()
{
    // Only meaningful while an open is pending on the lock; ordinary
    // post-edit warmups also emit this and should be ignored here.
    if (m_projectLoading.load(std::memory_order_acquire))
        disengageLoadingOverlay();
}

void MainWindow::setLoadingProgress(int done, int total)
{
    // Ignore stray progress from post-edit warmups when no open is pending.
    if (!m_projectLoading.load(std::memory_order_acquire)) return;
    if (!m_loadingOverlayBar || total <= 0) return;

    // First determinate update of this open (engage left it indeterminate at
    // range 0..0): switch to a real 0..total range and reset to empty so the
    // smoothing eases up from zero, not a stale prior-open value.
    if (m_loadingOverlayBar->maximum() != total) {
        m_loadingOverlayBar->setRange(0, total);
        m_loadingOverlayBar->reset();
        m_loadingBarTarget = 0;
    }
    m_loadingBarTotal = total;
    // Events arrive in bursts from several warmup threads (and can land
    // slightly out of order); track a monotonic target and let the smoothing
    // timer ease the visible bar toward it for a continuous fill.
    if (done > m_loadingBarTarget) m_loadingBarTarget = done;
    if (m_loadingBarTimer && !m_loadingBarTimer->isActive())
        m_loadingBarTimer->start();
}

// ═════════════════════════════════════════════════════════════════════════════

} // namespace rt
