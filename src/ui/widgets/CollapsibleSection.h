/*
 * CollapsibleSection.h — Premiere-style disclosure section.
 *
 * A titled header bar (chevron + title, plus an optional blue ToggleSwitch on
 * the right) over a collapsible content area. Clicking the header (or the
 * chevron) expands/collapses; the optional toggle enables/disables the content
 * (greyed out when off, matching Premiere's per-section enable switches).
 *
 * No Q_OBJECT of its own — consumers connect to the inner widgets:
 *   auto* sec = new CollapsibleSection("VIDEO", true);  // true = with toggle
 *   sec->contentLayout()->addLayout(form);
 *   connect(sec->toggle(), &QAbstractButton::toggled, this, &Foo::onVideo);
 */

#pragma once

#include "Theme.h"
#include "ToggleSwitch.h"

#include <QEvent>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLayout>
#include <QMouseEvent>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWidget>

namespace rt {

class CollapsibleSection : public QWidget
{
public:
    explicit CollapsibleSection(const QString& title, bool withToggle = false,
                                QWidget* parent = nullptr)
        : QWidget(parent)
    {
        const auto& tc = Theme::colors();
        const auto& m  = Theme::metrics();

        auto* outer = new QVBoxLayout(this);
        outer->setContentsMargins(0, 0, 0, 0);
        outer->setSpacing(0);

        // ── Header bar ──────────────────────────────────────────────────
        m_header = new QFrame(this);
        m_header->setObjectName(QStringLiteral("SectionHeader"));
        m_header->setCursor(Qt::PointingHandCursor);
        m_header->setFixedHeight(m.sectionHeaderHeight);
        m_header->setStyleSheet(QStringLiteral(
            "#SectionHeader { background: %1; border: none;"
            " border-bottom: 1px solid %2; }")
            .arg(Theme::hex(tc.surface2)).arg(Theme::hex(tc.border)));
        auto* hh = new QHBoxLayout(m_header);
        hh->setContentsMargins(m.spacingMd, 0, m.spacingMd, 0);
        hh->setSpacing(m.spacingSm);

        m_arrow = new QToolButton(m_header);
        m_arrow->setArrowType(Qt::DownArrow);
        m_arrow->setAutoRaise(true);
        m_arrow->setFocusPolicy(Qt::NoFocus);
        m_arrow->setCursor(Qt::PointingHandCursor);
        m_arrow->setStyleSheet(QStringLiteral(
            "QToolButton { border: none; background: transparent; }"));
        hh->addWidget(m_arrow);

        auto* titleLbl = new QLabel(title, m_header);
        titleLbl->setStyleSheet(QStringLiteral("color: %1; font-weight: 600;")
                                    .arg(Theme::hex(tc.textBright)));
        hh->addWidget(titleLbl);
        hh->addStretch();

        if (withToggle) {
            m_toggle = new ToggleSwitch(m_header);
            hh->addWidget(m_toggle);
        }

        outer->addWidget(m_header);

        // ── Content area ────────────────────────────────────────────────
        m_content = new QWidget(this);
        m_content->setObjectName(QStringLiteral("SectionContent"));
        m_content->setStyleSheet(QStringLiteral(
            "#SectionContent { background: %1; border: none;"
            " border-bottom: 1px solid %2; }")
            .arg(Theme::hex(tc.surface1)).arg(Theme::hex(tc.border)));
        m_contentLayout = new QVBoxLayout(m_content);
        m_contentLayout->setContentsMargins(m.spacingLg, m.spacingMd,
                                            m.spacingLg, m.spacingMd);
        m_contentLayout->setSpacing(m.spacingSm);
        outer->addWidget(m_content);

        // Chevron + header click both toggle expansion.
        connect(m_arrow, &QToolButton::clicked, this,
                [this] { setExpanded(!m_expanded); });
        m_header->installEventFilter(this);

        // The toggle greys out the content when off (kept visible, like Premiere).
        if (m_toggle) {
            connect(m_toggle, &QAbstractButton::toggled, this,
                    [this](bool on) { m_content->setEnabled(on); });
            m_content->setEnabled(m_toggle->isChecked());
        }
    }

    /// Layout that consumers add their controls to.
    [[nodiscard]] QVBoxLayout* contentLayout() const { return m_contentLayout; }
    void addContentWidget(QWidget* w) { m_contentLayout->addWidget(w); }
    void addContentLayout(QLayout* l) { m_contentLayout->addLayout(l); }

    /// Inner enable switch (nullptr if the section was created without one).
    [[nodiscard]] ToggleSwitch* toggle() const { return m_toggle; }

    [[nodiscard]] bool isExpanded() const { return m_expanded; }
    void setExpanded(bool on)
    {
        m_expanded = on;
        m_content->setVisible(on);
        if (m_arrow)
            m_arrow->setArrowType(on ? Qt::DownArrow : Qt::RightArrow);
    }

protected:
    bool eventFilter(QObject* watched, QEvent* ev) override
    {
        if (watched == m_header && ev->type() == QEvent::MouseButtonRelease) {
            auto* me = static_cast<QMouseEvent*>(ev);
            // Let clicks on the toggle switch fall through to the switch.
            if (m_toggle && m_toggle->geometry().contains(me->position().toPoint()))
                return false;
            if (me->button() == Qt::LeftButton) {
                setExpanded(!m_expanded);
                return true;
            }
        }
        return QWidget::eventFilter(watched, ev);
    }

private:
    QFrame*       m_header{nullptr};
    QToolButton*  m_arrow{nullptr};
    ToggleSwitch* m_toggle{nullptr};
    QWidget*      m_content{nullptr};
    QVBoxLayout*  m_contentLayout{nullptr};
    bool          m_expanded{true};
};

} // namespace rt
