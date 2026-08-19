/*
 * DockTitleBar.cpp — Custom dock title bar implementation.
 */

#include "widgets/DockTitleBar.h"
#include "Theme.h"

#include <QApplication>
#include <QContextMenuEvent>
#include <QDockWidget>
#include <QEvent>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QLabel>
#include <QMainWindow>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QResizeEvent>
#include <QStyle>
#include <QTabBar>

namespace rt {

DockTitleBar::DockTitleBar(QDockWidget* dock, const QString& title,
                           QWidget* parent)
    : QWidget(parent ? parent : dock)
    , m_dock(dock)
{
    buildUI();
    setTitle(title);
    applyTheme();

    // Repaint title bar accent when focus moves between panels
    connect(qApp, &QApplication::focusChanged, this, [this](QWidget*, QWidget* now) {
        if (!m_dock) return;
        bool focused = now && m_dock->isAncestorOf(now);
        bool prev = m_dock->property("panelFocused").toBool();
        if (focused != prev) {
            m_dock->setProperty("panelFocused", focused);
            m_dock->style()->unpolish(m_dock);
            m_dock->style()->polish(m_dock);
            update();
        }
    });

    // Re-check tabbed state when dock floats/docks
    connect(m_dock, &QDockWidget::topLevelChanged,
            this, &DockTitleBar::updateVisibility);
}

void DockTitleBar::buildUI()
{
    const auto& m = Theme::metrics();

    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(10, 0, 6, 0);
    layout->setSpacing(4);

    // ── Title label ─────────────────────────────────────────────────
    m_titleLabel = new QLabel(this);
    m_titleLabel->setAlignment(Qt::AlignVCenter | Qt::AlignLeft);
    m_titleLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
    m_titleLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    layout->addWidget(m_titleLabel);

    setFixedHeight(m.panelHeaderHeight);
}

void DockTitleBar::updateTitleLayout()
{
    if (!m_titleLabel) return;
    // No-op: title alignment is set in buildUI/setTitle.
}

void DockTitleBar::resizeEvent(QResizeEvent* event)
{
    static thread_local bool s_inResize = false;
    if (s_inResize) {
        QWidget::resizeEvent(event);
        return;
    }
    s_inResize = true;

    QWidget::resizeEvent(event);

    // A tabbed dock uses QMainWindow's tab bar as its complete header.
    // Do not let resize handling restore height to the zero-height shim.
    if (m_tabbed) {
        s_inResize = false;
        return;
    }

    // For "Audio Meters", dynamically switch between single-line and stacked
    // based on available width to prevent text from being cut off.
    if (m_isAudioMeters && m_titleLabel) {
        QFontMetrics fm(m_titleLabel->font());
        int singleWidth = fm.horizontalAdvance(QStringLiteral("Audio Meters"));
        // Account for layout margins (10 left + 6 right = 16) and some padding
        int available = this->width() - 20;

        bool shouldStack = singleWidth > available;

        if (shouldStack) {
            m_titleLabel->setText(QStringLiteral("Audio\nMeters"));
            m_titleLabel->setAlignment(Qt::AlignCenter);
            setFixedHeight(Theme::metrics().panelHeaderHeight + 14);
        } else {
            m_titleLabel->setText(QStringLiteral("Audio Meters"));
            m_titleLabel->setAlignment(Qt::AlignVCenter | Qt::AlignCenter);
            setFixedHeight(Theme::metrics().panelHeaderHeight);
        }
        updateGeometry();
    }

    s_inResize = false;
}

void DockTitleBar::setTitle(const QString& title)
{
    if (!m_titleLabel) return;

    if (title == QStringLiteral("Audio Meters")) {
        m_isAudioMeters = true;
        m_titleLabel->setText(title);
        m_titleLabel->setAlignment(Qt::AlignVCenter | Qt::AlignCenter);
        setFixedHeight(Theme::metrics().panelHeaderHeight);
        // resizeEvent will handle switching to stacked if needed
    } else {
        m_isAudioMeters = false;
        m_titleLabel->setText(title);
        m_titleLabel->setAlignment(Qt::AlignVCenter | Qt::AlignLeft);
        setFixedHeight(Theme::metrics().panelHeaderHeight);
    }
}

QSize DockTitleBar::sizeHint() const
{
    if (m_tabbed)
        return {0, 0};
    return {200, Theme::metrics().panelHeaderHeight};
}

QSize DockTitleBar::minimumSizeHint() const
{
    if (m_tabbed)
        return {0, 0};
    // For "Audio Meters", ensure minimum width accommodates stacked text
    if (m_isAudioMeters && m_titleLabel) {
        QFontMetrics fm(m_titleLabel->font());
        int stackedWidth = qMax(fm.horizontalAdvance(QStringLiteral("Audio")),
                                 fm.horizontalAdvance(QStringLiteral("Meters")));
        // Account for layout margins (10 left + 6 right = 16) and some padding
        return {stackedWidth + 24, Theme::metrics().panelHeaderHeight};
    }
    return {10, Theme::metrics().panelHeaderHeight};
}

void DockTitleBar::applyTheme()
{
    const auto& tc = Theme::colors();
    const auto& ty = Theme::typography();

    m_titleLabel->setStyleSheet(
        QStringLiteral("QLabel { color: %1; font-size: %2px; font-weight: 600;"
                        " letter-spacing: 0.5px; background: transparent; }")
            .arg(Theme::rgb(tc.dockTitleText))
            .arg(ty.sizeCaption));
}

void DockTitleBar::paintEvent(QPaintEvent* event)
{
    static thread_local int s_paintDepth = 0;
    if (++s_paintDepth > 5) {
        --s_paintDepth;
        QWidget::paintEvent(event);
        return;
    }
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, false);
    const auto& tc = Theme::colors();

    // Background — subtle gradient for depth
    p.fillRect(rect(), tc.dockTitleBg);

    // Bottom separator
    p.setPen(tc.border);
    p.drawLine(0, height() - 1, width(), height() - 1);

    // Top accent line when dock contains focus
    if (m_dock && m_dock->property("panelFocused").toBool()) {
        p.setPen(Qt::NoPen);
        p.setBrush(tc.accent);
        p.drawRect(0, height() - 2, width(), 2);
    }

    --s_paintDepth;
}

void DockTitleBar::mouseDoubleClickEvent(QMouseEvent* event)
{
    // Only toggle floating when double-clicking the title label
    if (m_dock && event->button() == Qt::LeftButton &&
        m_titleLabel && m_titleLabel->geometry().contains(event->pos())) {
        m_dock->setFloating(!m_dock->isFloating());
    }
}

void DockTitleBar::mousePressEvent(QMouseEvent* event)
{
    // Only allow drag-to-move when grabbing the title label (Premiere Pro
    // behavior).  Clicks elsewhere on the title bar are consumed so
    // QDockWidget never starts its internal drag handler.
    if (m_titleLabel && m_titleLabel->geometry().contains(event->pos())) {
        event->ignore();  // propagate to QDockWidget → drag
    } else {
        event->accept();  // consume → no drag
    }
}

void DockTitleBar::mouseMoveEvent(QMouseEvent* event)
{
    event->ignore();
}

void DockTitleBar::mouseReleaseEvent(QMouseEvent* event)
{
    event->ignore();
}

void DockTitleBar::contextMenuEvent(QContextMenuEvent* event)
{
    if (!m_dock) return;
    QMenu menu(this);
    bool floating = m_dock->isFloating();
    menu.addAction(floating ? tr("Dock Panel") : tr("Float Panel"), [this]() {
        m_dock->setFloating(!m_dock->isFloating());
    });
    menu.addAction(tr("Close Panel"), [this]() {
        m_dock->close();
    });
    menu.exec(event->globalPos());
}

bool DockTitleBar::event(QEvent* event)
{
    if (event->type() == QEvent::Show ||
        event->type() == QEvent::ParentChange ||
        event->type() == QEvent::LayoutRequest) {
        QMetaObject::invokeMethod(this, &DockTitleBar::updateVisibility,
                                   Qt::QueuedConnection);
    }
    return QWidget::event(event);
}

bool DockTitleBar::isTabbed() const
{
    if (!m_dock) return false;

    auto* mainWin = qobject_cast<QMainWindow*>(m_dock->parentWidget());
    if (!mainWin) {
        // Floating docks inside a QDockWidgetGroupWindow
        if (m_dock->isFloating()) {
            auto* tlw = m_dock->window();
            mainWin = qobject_cast<QMainWindow*>(tlw);
        }
        if (!mainWin) return false;
    }

    for (auto* tabBar : mainWin->findChildren<QTabBar*>()) {
        if (!tabBar->isVisible()) continue;
        for (int i = 0; i < tabBar->count(); ++i) {
            if (tabBar->tabText(i) == m_dock->windowTitle())
                return true;
        }
    }
    return false;
}

void DockTitleBar::updateVisibility()
{
    // Re-entrancy guard: prevent infinite recursion if updateGeometry()
    // triggered from within this function causes another layout pass that
    // re-emits LayoutRequest/Show events back to this title bar.
    if (m_updatingVisibility) {
        spdlog::trace("DockTitleBar::updateVisibility: re-entrancy detected, skipping");
        return;
    }
    m_updatingVisibility = true;

    m_tabbed = isTabbed();
    // Qt still reserves a custom title bar's size hint when it is merely
    // hidden. Collapse it to a true zero-height shim so tab content starts
    // directly under the QMainWindow tab row.
    if (m_tabbed) {
        setFixedHeight(0);
        setVisible(false);
    } else {
        setVisible(true);
        setFixedHeight(Theme::metrics().panelHeaderHeight);
    }
    updateGeometry();

    m_updatingVisibility = false;
}

} // namespace rt
