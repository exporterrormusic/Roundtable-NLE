/*
 * PanelMaximizeController.h — Premiere-style panel maximize/restore (tilde/`).
 *
 * Pressing ` maximizes the panel under the cursor; pressing it again reverts
 * to EXACTLY the prior layout.  Extracted from TimelineWorkspace as part of
 * the god-class decomposition (cleanup audit §3.1).
 *
 * Design (deliberately minimal):
 *   - Maximize never reparents any widget.  It simply hides every sibling
 *     panel/column so the target dock/panel gets all the space.  This is
 *     critical because the Program Monitor hosts a native Vulkan surface;
 *     reparenting it (the old implementation did) destroys + recreates the
 *     native window and the swapchain, which is why the monitor went blank.
 *   - Restore re-applies an EXACT visibility snapshot captured at maximize
 *     time, plus QMainWindow::restoreState() for the inner dock arrangement.
 *     Panels the user had closed stay closed (the old code force-showed
 *     every panel/dock unconditionally — the reported bug).
 */
#pragma once

#include <QByteArray>
#include <QMap>
#include <QString>

#include <functional>
#include <unordered_map>
#include <utility>
#include <vector>

class QDockWidget;
class QMainWindow;
class QSplitter;
class QWidget;

namespace rt {

class PanelMaximizeController {
public:
    /// Collaborator accessors injected from the owning TimelineWorkspace.
    /// innerMainWindow/edgeSplitter/fallbackPanel are accessor functions
    /// because they are created in buildPanels(), after construction; the
    /// dock-widget map is a stable member object so a plain pointer works.
    struct Config {
        std::function<bool()>         ready;            // panels built?
        std::function<QMainWindow*()> innerMainWindow;
        std::function<QSplitter*()>   edgeSplitter;
        const QMap<QString, QDockWidget*>* dockWidgets{nullptr};
        /// Maximizable panel that is NOT a dock (lives in the central
        /// widget) — the timeline panel.
        std::function<QWidget*()>     fallbackPanel;
    };

    explicit PanelMaximizeController(Config cfg) : m_cfg(std::move(cfg)) {}

    /// Maximize the panel under the cursor (focus fallback), or restore the
    /// pre-maximize layout if already maximized.
    void toggle();

    [[nodiscard]] bool isMaximized() const noexcept { return m_maximized; }

    /// Inner-window saveState() snapshot taken at maximize time (empty when
    /// not maximized).  saveDockLayout() persists THIS instead of the live
    /// state, because while maximized the live dock layout is the broken
    /// "everything hidden" arrangement.
    [[nodiscard]] const QByteArray& preMaximizeDockState() const noexcept {
        return m_dockStateBeforeMaximize;
    }

    /// Forget all maximize state WITHOUT restoring — used when the dock
    /// layout is being reset wholesale (resetToDefaultDockLayout).
    void clear();

private:
    Config m_cfg;

    bool m_maximized{false};
    QWidget*     m_maximizedWidget{nullptr};  // the panel shown fullscreen
    QDockWidget* m_maximizedDock{nullptr};    // its owning dock (nullptr = central widget)

    /// Snapshot of the inner QMainWindow dock state taken just before the
    /// panel was maximized — see preMaximizeDockState().
    QByteArray m_dockStateBeforeMaximize;

    /// Exact visibility snapshot captured at maximize time so restore can
    /// revert to PRECISELY the prior layout (panels the user had closed
    /// stay closed).
    std::unordered_map<QDockWidget*, bool> m_dockVisBeforeMax;
    std::vector<std::pair<QMainWindow*, bool>> m_edgeVisBeforeMax;
    /// saveState() for EVERY dock-hosting window (inner + each edge
    /// column).  Each edge column is its own QMainWindow with its own
    /// internal dock heights; restoring only the inner one left the edge
    /// columns' stacked panels relaying to default proportions.
    std::vector<std::pair<QMainWindow*, QByteArray>> m_dockStatesBeforeMax;
    QByteArray m_edgeSplitterStateBeforeMax;
    QWidget*   m_centralBeforeMax{nullptr};
    bool       m_centralVisBeforeMax{true};
};

} // namespace rt
