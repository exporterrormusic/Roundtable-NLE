/*
 * TimelineWorkspacePanelsCreate.cpp — panel construction entry point.
 *
 * createPanelWidgets() installs the app-wide focus filters, then builds
 * every dock in a fixed order (the default layout and the saved dock state
 * depend on it). Each area lives in its own file:
 *   createBinDocks, createLibraryDock   TimelineWorkspacePanelsMedia.cpp
 *   createMonitorDocks                  TimelineWorkspacePanelsMonitors.cpp
 *   createClipPanelDocks                TimelineWorkspacePanelsClip.cpp
 *   createCentralWidget (tool column,   TimelineWorkspacePanelsCentral.cpp
 *     sequence tabs, toolbar, timeline panel)
 * makeDock() and the Audio Meters dock are here.
 */

#include "panels/timeline/TimelineWorkspace.h"

#include "Theme.h"
#include "panels/monitors/ProgramMonitor.h"
#include "panels/monitors/SourceMonitor.h"
#include "panels/timeline/DockBehavior.h"
#include "panels/timeline/TimelinePanel.h"
#include "widgets/DockTitleBar.h"
#include "widgets/VUMeter.h"

#include <QAbstractSpinBox>
#include <QApplication>
#include <QComboBox>
#include <QDockWidget>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QPlainTextEdit>
#include <QTextEdit>
#include <QTimer>
#include <QVBoxLayout>

#ifdef _WIN32
#include <windows.h>
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

        // Don't steal focus from a user actively typing/editing (a hidden
        // field — e.g. in a closed floating dock — is not being edited).
        if (auto* fw = QApplication::focusWidget(); fw && fw->isVisible()) {
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

    // Top row: each panel in its own dock, in this order.
    createBinDocks();
    createMonitorDocks();
    createClipPanelDocks();
    createLibraryDock();
    createAudioMetersDock();

    // Tool column + sequence tabs + toolbar + timeline panel.
    createCentralWidget();
}

QDockWidget* TimelineWorkspace::makeDock(const QString& title, QWidget* widget,
                                         bool closable)
{
    QDockWidget::DockWidgetFeatures features =
        QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable;
    if (closable)
        features |= QDockWidget::DockWidgetClosable;
    auto* dock = new QDockWidget(title, m_innerMainWindow);
    dock->setFeatures(features);
    dock->setWidget(widget);
    dock->setObjectName(title);
    dock->setTitleBarWidget(new DockTitleBar(dock, title));
    dock->setContentsMargins(0, 0, 0, 0);
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
}

void TimelineWorkspace::createAudioMetersDock()
{
    const auto& tc = Theme::colors();
    const auto& m  = Theme::metrics();

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
}

} // namespace rt
