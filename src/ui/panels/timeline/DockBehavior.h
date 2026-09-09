#pragma once
/*
 * DockBehavior.h — Dock tab, resize, edge-column, and drag behavior classes.
 *
 * Class declarations only. Implementations in DockBehavior.cpp.
 *
 * These classes customize Qt's dock widget system to match Premiere Pro's
 * panel behavior: no tab eliding, tab tear-off, wide resize borders on
 * floating docks, and full-height edge columns via QSplitter.
 */

#include <QObject>
#include <QAbstractNativeEventFilter>
#include <QWidget>
#include <QPointer>
#include <QList>
#include <QSet>

class QMainWindow;
class QTabBar;
class QDockWidget;
class QSplitter;
class QTimer;

namespace rt { class TimelineWorkspace; }

// ═════════════════════════════════════════════════════════════════════════════
//  DockTabBarWatcher — forces no-elide, scroll-buttons on dock tab bars
// ═════════════════════════════════════════════════════════════════════════════
class DockTabBarWatcher : public QObject
{
    Q_OBJECT
public:
    explicit DockTabBarWatcher(QMainWindow* host, QObject* parent = nullptr);
    void setWorkspace(rt::TimelineWorkspace* ws);
    void setDragFilter(QObject* df);
    void watchTabBar(QTabBar* tabBar);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void forceSettings(QTabBar* tabBar);
    void setupTabBar(QTabBar* tabBar);
    void showTabContextMenu(QTabBar* tabBar, int tabIdx, const QPoint& globalPos);
    void updateFocusedPanel(QWidget* focusedWidget);
    void setActiveDock(QDockWidget* dock);
    bool isDockTabBar(QTabBar* tabBar) const;
    QDockWidget* dockForTab(QTabBar* tabBar, int tabIdx) const;
    QList<QDockWidget*> registeredDocks() const;
    void refreshTabBarState(QTabBar* tabBar);

    QMainWindow*              m_host{nullptr};
    rt::TimelineWorkspace*    m_workspace{nullptr};
    QObject*                  m_dragFilter{nullptr};
    QPointer<QDockWidget>      m_activeDock;
    bool                      m_configuring{false};
};

// ═════════════════════════════════════════════════════════════════════════════
//  DockTabDragFilter — enables dragging tabs out of tab groups
// ═════════════════════════════════════════════════════════════════════════════
class DockEdgeDragWatcher;  // forward declaration

class DockTabDragFilter : public QObject
{
    Q_OBJECT
public:
    explicit DockTabDragFilter(QMainWindow* dockHost, QObject* parent = nullptr);
    void setEdgeDragWatcher(DockEdgeDragWatcher* watcher) { m_edgeWatcher = watcher; }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    QDockWidget* dockForTabIndex(QTabBar* tabBar, int index) const;
    void notifyEdgeWatcher(QDockWidget* dock);

    QMainWindow*          m_dockHost{nullptr};
    DockEdgeDragWatcher*  m_edgeWatcher{nullptr};
    int                   m_pressedTab{-1};
    QPoint                m_pressPos;
    bool                  m_dragging{false};
    bool                  m_detached{false};
};

// ═════════════════════════════════════════════════════════════════════════════
//  FloatingResizeFallbackFilter — wide resize grab zone on floating docks
// ═════════════════════════════════════════════════════════════════════════════
#ifdef _WIN32
/// Install a Windows subclass on a floating dock's HWND to widen resize borders.
/// Called from TimelineWorkspacePanels.cpp when a dock is floated.
void installDockResizeSubclass(QDockWidget* dock);

class FloatingResizeFallbackFilter : public QAbstractNativeEventFilter
{
public:
    bool nativeEventFilter(const QByteArray& eventType,
                           void* message, qintptr* result) override;
};
#endif

// ═════════════════════════════════════════════════════════════════════════════
//  DockEdgeOverlay — semi-transparent drop zone preview overlay
// ═════════════════════════════════════════════════════════════════════════════
class DockEdgeOverlay : public QWidget
{
    Q_OBJECT
public:
    explicit DockEdgeOverlay(QWidget* parent);
    void showZone(const QRect& zone, bool isTab = false);
    void hideOverlay();

protected:
    void paintEvent(QPaintEvent*) override;

private:
    QPointer<QWidget> m_anchor;
    bool m_isTab{false};
};

// ═════════════════════════════════════════════════════════════════════════════
//  EdgeColumnGuard — auto-cleanup for edge-column QMainWindows
// ═════════════════════════════════════════════════════════════════════════════
class EdgeColumnGuard : public QObject
{
    Q_OBJECT
public:
    EdgeColumnGuard(QMainWindow* edgeCol, QMainWindow* host);
    static EdgeColumnGuard* forColumn(QMainWindow* edgeCol);
    void watchDock(QDockWidget* dock);

private:
    void onDockFloated(QPointer<QDockWidget> dock);
    void checkEmpty();

    QPointer<QMainWindow> m_edgeCol;
    QPointer<QMainWindow> m_host;
    QSet<QDockWidget*>    m_watched;
};

// ═════════════════════════════════════════════════════════════════════════════
//  DockEdgeDragWatcher — detects floating docks near workspace edges
// ═════════════════════════════════════════════════════════════════════════════
class DockEdgeDragWatcher : public QObject
{
    Q_OBJECT
public:
    explicit DockEdgeDragWatcher(QMainWindow* host, QSplitter* edgeSplitter,
                                 QObject* parent = nullptr);
    void beginExternalDrag(QDockWidget* dock);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    enum class Edge { None, Left, Right };
    enum class DropMode
        { None, NewColumn, TabWith, SplitAbove, SplitBelow, SplitLeft, SplitRight };

    void commitEdgeDrop();
    void commitInnerDrop();
    void poll();
    QDockWidget* findDraggedDock() const;
    QMainWindow* findEdgeColumn(bool isLeft) const;
    QRect computeColumnDropZone(QMainWindow* edgeCol, bool isLeft);
    void updateEdgeHighlight();
    void clearInner();
    void updateInnerHighlight();
    void setEdge(Edge edge, QRect zone = QRect());
    void endTracking();

    QMainWindow*                m_host;
    QSplitter*                  m_splitter;
    DockEdgeOverlay*            m_overlay;
    QTimer*                     m_pollTimer;
    QPointer<QDockWidget>       m_draggedDock;
    QPointer<QDockWidget>       m_pressTargetDock;
    Edge                        m_activeEdge{Edge::None};
    DropMode                    m_dropMode{DropMode::None};
    QPointer<QDockWidget>       m_dropTarget;
    QPointer<QDockWidget>       m_innerTarget;
    DropMode                    m_innerMode{DropMode::None};
    bool                        m_dragConfirmed{false};
    bool                        m_committing{false};
    QPoint                      m_initialCursorPos;
    QPoint                      m_dragOffset;
    int                         m_previewWidth{200};
};
