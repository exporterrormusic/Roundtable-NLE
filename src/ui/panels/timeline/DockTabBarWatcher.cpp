/*
 * DockTabBarWatcher.cpp — extracted from DockBehavior.cpp.
 *
 * Forces no-elide, scroll-buttons on dock tab bars, enables tab
 * context menus (Close Tab), and watches for newly-added QTabBars
 * in dock containers.
 */

#include "panels/timeline/DockBehavior.h"
#include "panels/timeline/TimelineWorkspace.h"
#include "Theme.h"

#include <QApplication>
#include <QDockWidget>
#include <QMainWindow>
#include <QTabBar>
#include <QMenu>
#include <QMouseEvent>
#include <QPointer>
#include <QCoreApplication>
#include <QChildEvent>
#include <QContextMenuEvent>
#include <QEvent>
#include <QStyle>
#include <QTimer>

#include <spdlog/spdlog.h>

// ═════════════════════════════════════════════════════════════════════════════
//  DockTabBarWatcher
// ═════════════════════════════════════════════════════════════════════════════

DockTabBarWatcher::DockTabBarWatcher(QMainWindow* host, QObject* parent)
    : QObject(parent), m_host(host)
{
    QCoreApplication::instance()->installEventFilter(this);
    connect(qApp, &QApplication::focusChanged, this,
            [this](QWidget*, QWidget* now) { updateFocusedPanel(now); });
    if (m_host) {
        for (auto* tabBar : m_host->findChildren<QTabBar*>())
            watchTabBar(tabBar);
    }
}

void DockTabBarWatcher::setWorkspace(rt::TimelineWorkspace* ws)
{
    m_workspace = ws;
    updateFocusedPanel(QApplication::focusWidget());
}

void DockTabBarWatcher::setDragFilter(QObject* df)
{
    m_dragFilter = df;
    if (!m_host || !m_dragFilter) return;
    for (auto* tabBar : m_host->findChildren<QTabBar*>()) {
        if (tabBar->property("roundtableDockTabBar").toBool())
            tabBar->installEventFilter(m_dragFilter);
    }
}

void DockTabBarWatcher::watchTabBar(QTabBar* tabBar)
{
    if (!isDockTabBar(tabBar)) return;
    forceSettings(tabBar);
    tabBar->installEventFilter(this);
    setupTabBar(tabBar);
    refreshTabBarState(tabBar);
}

bool DockTabBarWatcher::eventFilter(QObject* watched, QEvent* event)
{
    if (event->type() == QEvent::ChildAdded
        && qobject_cast<QMainWindow*>(watched)) {
        auto* ce = static_cast<QChildEvent*>(event);
        if (auto* tabBar = qobject_cast<QTabBar*>(ce->child())) {
            watchTabBar(tabBar);
            QPointer<QTabBar> pendingTabBar(tabBar);
            QTimer::singleShot(0, this, [this, pendingTabBar]() {
                if (pendingTabBar) watchTabBar(pendingTabBar);
            });
        }
    }
    if (watched == m_host
        && (event->type() == QEvent::LayoutRequest
            || event->type() == QEvent::Show)) {
        // restoreState() can reparent an existing, initially-empty QTabBar
        // without delivering another ChildAdded event. Rescan after layout
        // changes so saved and custom workspaces receive the same chrome.
        QPointer<QMainWindow> host(m_host);
        QTimer::singleShot(0, this, [this, host]() {
            if (!host) return;
            for (auto* tabBar : host->findChildren<QTabBar*>())
                watchTabBar(tabBar);
        });
    }
    if (auto* tabBar = qobject_cast<QTabBar*>(watched)) {
        switch (event->type()) {
        case QEvent::Paint:
        case QEvent::LayoutRequest:
        case QEvent::Show:
        case QEvent::Resize:
        case QEvent::StyleChange:
            forceSettings(tabBar);
            refreshTabBarState(tabBar);
            break;
        case QEvent::MouseButtonPress: {
            auto* me = static_cast<QMouseEvent*>(event);
            const int tabIdx = tabBar->tabAt(me->position().toPoint());
            if (tabIdx >= 0)
                setActiveDock(dockForTab(tabBar, tabIdx));
            break;
        }
        case QEvent::ContextMenu: {
            auto* ce = static_cast<QContextMenuEvent*>(event);
            int tabIdx = tabBar->tabAt(tabBar->mapFromGlobal(ce->globalPos()));
            if (tabIdx >= 0) {
                showTabContextMenu(tabBar, tabIdx, ce->globalPos());
                return true;
            }
            break;
        }
        default:
            break;
        }
    }
    return false;
}

void DockTabBarWatcher::showTabContextMenu(QTabBar* tabBar, int tabIdx,
                                            const QPoint& globalPos)
{
    QString tabTitle = tabBar->tabText(tabIdx);
    QDockWidget* foundDock = nullptr;
    if (m_workspace)
        foundDock = m_workspace->dockForPanel(tabTitle);
    if (!foundDock) {
        auto* mw = qobject_cast<QMainWindow*>(tabBar->window());
        if (!mw) mw = m_host;
        if (mw) {
            for (auto* d : mw->findChildren<QDockWidget*>()) {
                if (d->windowTitle() == tabTitle) {
                    foundDock = d;
                    break;
                }
            }
        }
    }
    QMenu menu(tabBar);
    QPointer<QDockWidget> dock = foundDock;
    QPointer<QTabBar> tb = tabBar;
    menu.addAction(QObject::tr("Close Tab"), [dock, tb, tabIdx]() {
        if (dock) {
            dock->close();
        } else if (tb) {
            // Non-dock tab bar (e.g. ProjectBin) — emit the standard
            // tabCloseRequested signal so the owning widget handles it.
            emit tb->tabCloseRequested(tabIdx);
        }
    });
    menu.exec(globalPos);
}

void DockTabBarWatcher::forceSettings(QTabBar* tabBar)
{
    if (m_configuring) return;
    m_configuring = true;
    tabBar->setElideMode(Qt::ElideNone);
    tabBar->setExpanding(false);
    tabBar->setUsesScrollButtons(true);
    tabBar->setMaximumWidth(QWIDGETSIZE_MAX);
    tabBar->setMinimumWidth(0);
    tabBar->setVisible(true);
    m_configuring = false;
}

void DockTabBarWatcher::setupTabBar(QTabBar* tabBar)
{
    if (!tabBar) return;
    if (tabBar->property("_rt_ctx_menu").toBool()) return;
    tabBar->setProperty("_rt_ctx_menu", true);
    tabBar->setProperty("roundtableDockTabBar", true);
    tabBar->setFixedHeight(rt::Theme::metrics().panelHeaderHeight);
    tabBar->setMovable(true);
    tabBar->setTabsClosable(false);
    if (m_dragFilter)
        tabBar->installEventFilter(m_dragFilter);

    connect(tabBar, &QTabBar::currentChanged, this,
            [this, tabBar](int index) {
                if (tabBar->property("panelBarActive").toBool()) {
                    setActiveDock(dockForTab(tabBar, index));
                    return;
                }
                refreshTabBarState(tabBar);
            });

    tabBar->style()->unpolish(tabBar);
    tabBar->style()->polish(tabBar);
}

QList<QDockWidget*> DockTabBarWatcher::registeredDocks() const
{
    if (m_workspace)
        return m_workspace->dockWidgets().values();
    return m_host ? m_host->findChildren<QDockWidget*>() : QList<QDockWidget*>{};
}

bool DockTabBarWatcher::isDockTabBar(QTabBar* tabBar) const
{
    if (!tabBar) return false;

    // Qt creates a QMainWindow's dock tab bars as direct children of that
    // QMainWindow.  Application-owned tabs live inside their panel widgets.
    // Do not use documentMode here: the production main window deliberately
    // enables it for dock tabs as well.
    if (tabBar->parentWidget() == m_host)
        return true;

    // Reparented/floating dock groups are occasionally wrapped by Qt.  In
    // that case, only accept a bar when every tab maps to a known dock.
    if (tabBar->count() == 0) return false;
    for (int index = 0; index < tabBar->count(); ++index) {
        if (!dockForTab(tabBar, index)) return false;
    }
    return true;
}

QDockWidget* DockTabBarWatcher::dockForTab(QTabBar* tabBar, int tabIdx) const
{
    if (!tabBar || tabIdx < 0 || tabIdx >= tabBar->count()) return nullptr;
    const QString title = tabBar->tabText(tabIdx);
    for (auto* dock : registeredDocks()) {
        if (dock && dock->windowTitle() == title)
            return dock;
    }
    return nullptr;
}

void DockTabBarWatcher::updateFocusedPanel(QWidget* focusedWidget)
{
    for (QWidget* widget = focusedWidget; widget; widget = widget->parentWidget()) {
        if (auto* dock = qobject_cast<QDockWidget*>(widget)) {
            setActiveDock(dock);
            return;
        }
        if (auto* tabBar = qobject_cast<QTabBar*>(widget)) {
            if (tabBar->property("roundtableDockTabBar").toBool()) {
                setActiveDock(dockForTab(tabBar, tabBar->currentIndex()));
                return;
            }
        }
    }
}

void DockTabBarWatcher::setActiveDock(QDockWidget* dock)
{
    if (!dock || dock == m_activeDock) return;

    const auto docks = registeredDocks();
    if (!docks.contains(dock)) return;

    m_activeDock = dock;
    for (auto* candidate : docks) {
        if (!candidate) continue;
        const bool active = candidate == m_activeDock;
        if (candidate->property("panelFocused").toBool() != active) {
            candidate->setProperty("panelFocused", active);
            if (candidate->titleBarWidget())
                candidate->titleBarWidget()->update();
        }
    }

    if (m_host) {
        for (auto* tabBar : m_host->findChildren<QTabBar*>())
            refreshTabBarState(tabBar);
    }
    // Edge-column and floating tab groups can be reparented outside m_host.
    for (auto* window : QApplication::topLevelWidgets()) {
        for (auto* tabBar : window->findChildren<QTabBar*>()) {
            if (tabBar->property("roundtableDockTabBar").toBool())
                refreshTabBarState(tabBar);
        }
    }
}

void DockTabBarWatcher::refreshTabBarState(QTabBar* tabBar)
{
    if (!tabBar || !tabBar->property("roundtableDockTabBar").toBool()) return;
    const bool active = dockForTab(tabBar, tabBar->currentIndex()) == m_activeDock;
    if (tabBar->property("panelBarActive").toBool() == active) return;
    tabBar->setProperty("panelBarActive", active);
    tabBar->style()->unpolish(tabBar);
    tabBar->style()->polish(tabBar);
    tabBar->update();
}

// ═════════════════════════════════════════════════════════════════════════════
