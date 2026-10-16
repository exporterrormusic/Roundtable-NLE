/*
 * ToolGroupButton — one tool-column slot holding several related tools
 * (Premiere's grouped tools, e.g. Pen / Ellipse / Rectangle Mask).
 *
 * Shows the most recently chosen tool. Click activates it; press-and-hold or
 * right-click opens a flyout listing the whole group. A small triangle in the
 * corner marks the slot as a group, as in Premiere's toolbar.
 */

#pragma once

#include "widgets/ToolButton.h"

#include <QContextMenuEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPixmap>
#include <QTimer>

#include <functional>
#include <utility>
#include <vector>

namespace rt {

class ToolGroupButton : public ToolButton
{
public:
    struct Entry {
        ToolButton::Tool icon;
        QString          label;
        int              id;   ///< caller's tool id, passed back on activation
    };

    explicit ToolGroupButton(std::vector<Entry> entries, QWidget* parent = nullptr)
        : ToolButton(entries.front().icon, parent), m_entries(std::move(entries))
    {
        m_holdTimer.setSingleShot(true);
        m_holdTimer.setInterval(350);
        QObject::connect(&m_holdTimer, &QTimer::timeout, this,
                         [this]() { showFlyout(); });
        QObject::connect(this, &QToolButton::clicked, this, [this]() {
            if (m_flyoutOpened) {
                m_flyoutOpened = false;
                return;
            }
            activate(m_current);
        });
        showEntry(0);
    }

    /// Called with an Entry::id whenever the user picks or clicks a tool.
    void setActivateHandler(std::function<void(int)> handler)
    {
        m_onActivate = std::move(handler);
    }

    /// Reflect the active tool: shows and checks the entry with this id, or
    /// unchecks the slot when the id is not part of the group.
    void syncToId(int id)
    {
        for (size_t i = 0; i < m_entries.size(); ++i) {
            if (m_entries[i].id == id) {
                showEntry(i);
                setChecked(true);
                return;
            }
        }
        setChecked(false);
    }

protected:
    void mousePressEvent(QMouseEvent* event) override
    {
        if (event->button() == Qt::LeftButton) {
            m_flyoutOpened = false;
            m_holdTimer.start();
        }
        ToolButton::mousePressEvent(event);
    }

    void mouseReleaseEvent(QMouseEvent* event) override
    {
        m_holdTimer.stop();
        ToolButton::mouseReleaseEvent(event);
    }

    void contextMenuEvent(QContextMenuEvent* event) override
    {
        showFlyout();
        event->accept();
    }

    void paintEvent(QPaintEvent* event) override
    {
        ToolButton::paintEvent(event);
        // Group marker: a small filled triangle in the bottom-right corner.
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(Qt::NoPen);
        p.setBrush(isChecked() ? Theme::colors().textBright
                               : Theme::colors().textSecondary);
        const QPointF br(width() - 5.0, height() - 5.0);
        const QPointF tri[3] = {br, br - QPointF(5.0, 0.0), br - QPointF(0.0, 5.0)};
        p.drawPolygon(tri, 3);
    }

private:
    void showEntry(size_t index)
    {
        if (index >= m_entries.size()) return;
        m_current = index;
        setTool(m_entries[index].icon);
        setToolTip(m_entries[index].label
                   + QStringLiteral("  (hold or right-click for more mask tools)"));
    }

    void activate(size_t index)
    {
        showEntry(index);
        setChecked(true);
        if (m_onActivate) m_onActivate(m_entries[index].id);
    }

    static QIcon iconFor(ToolButton::Tool tool)
    {
        ToolButton swatch(tool);
        swatch.setFixedSize(28, 28);
        QPixmap pixmap(swatch.size());
        pixmap.fill(Qt::transparent);
        swatch.render(&pixmap, QPoint(), QRegion(),
                      QWidget::RenderFlag::DrawChildren);
        return QIcon(pixmap);
    }

    void showFlyout()
    {
        m_holdTimer.stop();
        m_flyoutOpened = true;
        setDown(false);
        QMenu menu(this);
        for (size_t i = 0; i < m_entries.size(); ++i) {
            QAction* action = menu.addAction(iconFor(m_entries[i].icon),
                                             m_entries[i].label);
            action->setCheckable(true);
            action->setChecked(i == m_current && isChecked());
            QObject::connect(action, &QAction::triggered, this,
                             [this, i]() { activate(i); });
        }
        menu.exec(mapToGlobal(QPoint(width(), 0)));
        update();
    }

    std::vector<Entry>        m_entries;
    size_t                    m_current{0};
    QTimer                    m_holdTimer;
    bool                      m_flyoutOpened{false};
    std::function<void(int)>  m_onActivate;
};

} // namespace rt
