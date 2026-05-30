/*
 * EffectControlsPanelUI.cpp -- EffectControlsPanel ctor, createScrubby,
 * setupUI, and drag/drop. (PropertyRow -> ...PropertyRow.cpp;
 *  KeyframeTimeline -> ...KeyframeTimeline.cpp)
 */

#include "panels/effects/EffectControlsPanel.h"
#include "widgets/ScrubbySpinBox.h"
#include "Theme.h"

#include "timeline/Clip.h"
#include "timeline/KeyframeTrack.h"
#include "command/CommandStack.h"
#include "command/LambdaCommand.h"
#include "command/commands/KeyframeCmds.h"
#include "command/commands/EffectCommands.h"
#include "effects/Effect.h"

#include <QFrame>
#include <QGridLayout>
#include <QPushButton>
#include <QPainter>
#include <QPainterPath>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QMimeData>
#include <QScrollBar>
#include <QSplitter>
#include <QApplication>
#include <QMenu>
#include <QAction>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <map>
#include <set>
#include <unordered_map>
#include <vector>

namespace rt {

// ═════════════════════════════════════════════════════════════════════════════
//  EffectControlsPanel
// ═════════════════════════════════════════════════════════════════════════════

EffectControlsPanel::EffectControlsPanel(QWidget* parent)
    : QWidget(parent)
{
    setFocusPolicy(Qt::ClickFocus);
    setAcceptDrops(true);
    setupUI();
}

EffectControlsPanel::~EffectControlsPanel() = default;

ScrubbySpinBox* EffectControlsPanel::createScrubby(double min, double max,
                                                    double step, int decimals,
                                                    const QString& suffix)
{
    const auto& tc = Theme::colors();
    auto* spin = new ScrubbySpinBox(this);
    spin->setRange(min, max);
    spin->setScrubStep(step);
    spin->setSingleStep(step);
    spin->setDecimals(decimals);
    spin->setFixedHeight(22);
    if (!suffix.isEmpty())
        spin->setSuffix(suffix);

    // Blue values like Premiere Pro
    spin->setStyleSheet(QStringLiteral(
        "QDoubleSpinBox { color: %1; background: transparent; border: none;"
        "  font-size: 12px; padding: 0 2px; }"
        "QDoubleSpinBox::up-button, QDoubleSpinBox::down-button { width: 0; }")
        .arg(Theme::hex(tc.accent)));
    return spin;
}

void EffectControlsPanel::setupUI()
{
    const auto& tc = Theme::colors();
    const auto& m  = Theme::metrics();

    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(0);

    // ── Header row (clip name + colored type badge) ─────────────────────
    auto* headerBar = new QWidget(this);
    headerBar->setFixedHeight(28);
    headerBar->setStyleSheet(QStringLiteral("background: %1; border-bottom: 1px solid %2;")
        .arg(Theme::hex(tc.surface2), Theme::hex(tc.border)));
    auto* headerLayout = new QHBoxLayout(headerBar);
    headerLayout->setContentsMargins(m.spacingMd, 0, m.spacingMd, 0);
    headerLayout->setSpacing(m.spacingMd);

    m_clipNameLabel = new QLabel("No clip selected", headerBar);
    m_clipNameLabel->setStyleSheet(QStringLiteral(
        "QLabel { color: %1; font-size: 12px; font-weight: bold; background: transparent; }")
        .arg(Theme::hex(tc.textPrimary)));
    headerLayout->addWidget(m_clipNameLabel);

    headerLayout->addStretch();

    m_clipTypeLabel = new QLabel(headerBar);
    m_clipTypeLabel->setStyleSheet(QStringLiteral(
        "QLabel { color: %1; font-size: 11px; font-weight: bold; "
        "background: %2; border-radius: 3px; padding: 1px 6px; }")
        .arg(Theme::hex(tc.textPrimary), Theme::hex(tc.accentDim)));
    headerLayout->addWidget(m_clipTypeLabel);

    mainLayout->addWidget(headerBar);

    // ── Search / filter bar ─────────────────────────────────────────────
    auto* searchBar = new QWidget(this);
    searchBar->setFixedHeight(26);
    searchBar->setStyleSheet(QStringLiteral("background: %1; border-bottom: 1px solid %2;")
        .arg(Theme::hex(tc.surface1), Theme::hex(tc.border)));
    auto* searchLayout = new QHBoxLayout(searchBar);
    searchLayout->setContentsMargins(m.spacingMd, 2, m.spacingMd, 2);
    searchLayout->setSpacing(m.spacingXs);

    m_searchField = new QLineEdit(searchBar);
    m_searchField->setPlaceholderText(QStringLiteral("\U0001F50D Filter properties\u2026"));
    m_searchField->setClearButtonEnabled(true);
    m_searchField->setFixedHeight(20);
    m_searchField->setStyleSheet(
        QStringLiteral("QLineEdit { background: %1; color: %2; border: 1px solid %3; "
                       "border-radius: %4px; padding: 1px 4px; font-size: 12px; }"
                       "QLineEdit:focus { border: 1px solid %5; }")
            .arg(Theme::hex(tc.inputBg), Theme::hex(tc.text),
                 Theme::hex(tc.controlBorder),
                 QString::number(m.radiusSm),
                 Theme::hex(tc.accent)));
    searchLayout->addWidget(m_searchField, 1);

    connect(m_searchField, &QLineEdit::textChanged, this, [this](const QString& text) {
        QString filter = text.trimmed().toLower();
        for (auto& sec : m_sectionArrows) {
            bool sectionMatch = filter.isEmpty()
                || sec.title.toLower().contains(filter);
            // Show/hide child rows based on filter match
            for (auto* child : sec.children) {
                auto* row = qobject_cast<PropertyRow*>(child);
                if (row) {
                    bool match = filter.isEmpty()
                        || row->propertyName().toLower().contains(filter)
                        || sectionMatch;
                    // Scale Width stays hidden while Uniform Scale is on,
                    // even when the filter would otherwise show it.
                    if (row == m_scaleWRow && m_uniformScaleCheck
                            && m_uniformScaleCheck->isChecked()) {
                        match = false;
                    }
                    row->setVisible(match);
                } else {
                    child->setVisible(sectionMatch || filter.isEmpty());
                }
            }
            // Show section header if any child is visible or filter matches section name
            bool anyVisible = false;
            for (auto* child : sec.children)
                if (child->isVisible()) { anyVisible = true; break; }
            sec.header->setVisible(anyVisible || sectionMatch);
        }
    });

    mainLayout->addWidget(searchBar);

    // ── Splitter: left (property tree) | right (keyframe timeline) ──────
    m_splitter = new QSplitter(Qt::Horizontal, this);
    m_splitter->setChildrenCollapsible(false);
    m_splitter->setHandleWidth(2);
    m_splitter->setStyleSheet(QStringLiteral(
        "QSplitter::handle { background: %1; }"
        "QSplitter::handle:hover { background: %2; }")
        .arg(Theme::hex(tc.border), Theme::hex(tc.accent)));

    // -- Left: scroll area with property sections ────────────────────────
    m_scrollArea = new QScrollArea;
    m_scrollArea->setWidgetResizable(true);
    m_scrollArea->setMinimumWidth(380);
    m_scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_scrollArea->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_scrollArea->setStyleSheet(QStringLiteral(
        "QScrollArea { background: %1; border: none; }")
        .arg(Theme::hex(tc.surface1)));

    m_propContainer = new QWidget;
    m_propLayout = new QVBoxLayout(m_propContainer);
    m_propLayout->setContentsMargins(0, 0, 0, 0);
    m_propLayout->setSpacing(0);
    m_scrollArea->setWidget(m_propContainer);

    // -- Right: keyframe timeline ────────────────────────────────────────
    m_kfTimeline = new KeyframeTimeline;
    m_kfTimeline->setCommandStack(m_commandStack);

    m_splitter->addWidget(m_scrollArea);
    m_splitter->addWidget(m_kfTimeline);
    m_splitter->setStretchFactor(0, 2);  // property tree gets more space
    m_splitter->setStretchFactor(1, 3);

    // Sync scroll position to keyframe timeline
    connect(m_scrollArea->verticalScrollBar(), &QScrollBar::valueChanged,
            m_kfTimeline, &KeyframeTimeline::setScrollOffset);

    // Mini-timeline scrub → seek the playback controller
    connect(m_kfTimeline, &KeyframeTimeline::playheadScrubbed,
            this, &EffectControlsPanel::seekRequested);

    // Keyframe moved or deleted in the mini-timeline. Re-evaluate every
    // bound track at the playhead so spinbox values reflect the keyframes'
    // new positions live, not just on release.
    connect(m_kfTimeline, &KeyframeTimeline::keyframeChanged,
            this, [this]() {
        syncValuesFromClip();
        emit propertyChanged();
    });

    // ── Empty state label ───────────────────────────────────────────────
    m_emptyLabel = new QLabel(tr("Select a clip to view properties"), this);
    m_emptyLabel->setAlignment(Qt::AlignCenter);
    m_emptyLabel->setStyleSheet(QStringLiteral(
        "QLabel { color: %1; font-size: 13px; background: %2; border: none; }")
        .arg(Theme::hex(tc.textDisabled), Theme::hex(tc.surface1)));

    // Stack splitter + empty label — show one at a time
    m_splitterContainer = new QWidget(this);
    auto* stackLayout = new QVBoxLayout(m_splitterContainer);
    stackLayout->setContentsMargins(0, 0, 0, 0);
    stackLayout->setSpacing(0);
    stackLayout->addWidget(m_splitter, 1);
    stackLayout->addWidget(m_emptyLabel, 1);
    m_splitter->hide();
    m_emptyLabel->show();

    mainLayout->addWidget(m_splitterContainer, 1);

    // ── Footer (live timecode display) ──────────────────────────────────
    auto* footer = new QWidget(this);
    footer->setFixedHeight(22);
    footer->setStyleSheet(QStringLiteral("background: %1; border-top: 1px solid %2;")
        .arg(Theme::hex(tc.surface2), Theme::hex(tc.border)));
    auto* footerLayout = new QHBoxLayout(footer);
    footerLayout->setContentsMargins(m.spacingMd, 0, m.spacingMd, 0);
    footerLayout->setSpacing(0);

    m_footerTimecodeLabel = new QLabel("00:00:00;00", footer);
    m_footerTimecodeLabel->setStyleSheet(QStringLiteral(
        "QLabel { color: %1; font-size: 11px; font-family: Consolas; background: transparent; }")
        .arg(Theme::hex(tc.textSecondary)));
    footerLayout->addWidget(m_footerTimecodeLabel);
    footerLayout->addStretch();

    // "Remove All Keyframes" — clears every keyframe on the clip AND on all
    // of a GraphicClip's text/shape layers in one undoable step. This is the
    // only way to reach keyframes on a graphic layer that isn't the currently
    // selected one (Effect Controls binds to a single layer at a time).
    auto* removeAllKfBtn = new QPushButton(tr("Remove All Keyframes"), footer);
    removeAllKfBtn->setCursor(Qt::PointingHandCursor);
    removeAllKfBtn->setToolTip(tr("Clear every keyframe on this clip and all of its layers"));
    removeAllKfBtn->setStyleSheet(QStringLiteral(
        "QPushButton { color: %1; font-size: 11px; background: transparent; border: none; padding: 0 2px; }"
        "QPushButton:hover { color: %2; }")
        .arg(Theme::hex(tc.textSecondary), Theme::hex(tc.textBright)));
    connect(removeAllKfBtn, &QPushButton::clicked,
            this, &EffectControlsPanel::removeAllKeyframes);
    footerLayout->addWidget(removeAllKfBtn);

    mainLayout->addWidget(footer);
}

// ═════════════════════════════════════════════════════════════════════════════
//  Drag & Drop — accept effects dragged from the Effects panel
// ═════════════════════════════════════════════════════════════════════════════

void EffectControlsPanel::dragEnterEvent(QDragEnterEvent* event)
{
    if (event->mimeData()->hasFormat(QStringLiteral("application/x-roundtable-effect")) && m_clip) {
        event->acceptProposedAction();
    }
}

void EffectControlsPanel::dragMoveEvent(QDragMoveEvent* event)
{
    if (event->mimeData()->hasFormat(QStringLiteral("application/x-roundtable-effect")) && m_clip) {
        event->acceptProposedAction();
    }
}

void EffectControlsPanel::dropEvent(QDropEvent* event)
{
    if (!m_clip || !m_commandStack || !m_timeline) return;
    if (!event->mimeData()->hasFormat(QStringLiteral("application/x-roundtable-effect"))) return;

    QByteArray effectData = event->mimeData()->data(QStringLiteral("application/x-roundtable-effect"));
    bool ok = false;
    int effectType = effectData.toInt(&ok);
    if (!ok) return;

    auto type = static_cast<EffectType>(effectType);
    auto& stack = m_clip->effects();

    m_commandStack->execute(
        std::make_unique<AddEffectCommand>(&stack, type));

    refresh();
    emit propertyChanged();

    spdlog::info("EffectControlsPanel: added effect '{}' via drop",
                 effectTypeName(type));
    event->acceptProposedAction();
}

} // namespace rt
