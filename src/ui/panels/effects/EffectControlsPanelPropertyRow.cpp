/*
 * EffectControlsPanelPropertyRow.cpp
 * PropertyRow keyframe-property row widget + stopwatch icon helper.
 * Extracted from EffectControlsPanelUI.cpp (behavior-preserving).
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

// ── Stopwatch icon helper ───────────────────────────────────────────────────
static QPixmap createStopwatchPixmap(const QColor& color, int sz = 14)
{
    QPixmap pix(sz, sz);
    pix.fill(Qt::transparent);
    QPainter p(&pix);
    p.setRenderHint(QPainter::Antialiasing);

    QPen pen(color, 1.3);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);

    // Clock face circle
    qreal cx = sz * 0.5, cy = sz * 0.57;
    qreal r  = sz * 0.33;
    p.drawEllipse(QPointF(cx, cy), r, r);

    // Minute hand (pointing up)
    p.drawLine(QPointF(cx, cy), QPointF(cx, cy - r * 0.72));
    // Hour hand (pointing right)
    p.drawLine(QPointF(cx, cy), QPointF(cx + r * 0.55, cy));

    // Top stem
    p.drawLine(QPointF(cx, cy - r - 0.5), QPointF(cx, cy - r - sz * 0.14));
    // Top button bar
    qreal bw = sz * 0.14;
    p.drawLine(QPointF(cx - bw, cy - r - sz * 0.14),
               QPointF(cx + bw, cy - r - sz * 0.14));

    // Side push-button (upper-right)
    qreal bx = cx + r * 0.65, by = cy - r * 0.65;
    p.drawLine(QPointF(bx, by), QPointF(bx + sz * 0.1, by - sz * 0.1));

    return pix;
}

// ═════════════════════════════════════════════════════════════════════════════
//  PropertyRow
// ═════════════════════════════════════════════════════════════════════════════

PropertyRow::PropertyRow(const QString& name, KeyframeTrack<float>* track,
                         QWidget* parent)
    : QWidget(parent), m_name(name), m_track(track)
{
    buildUI();
    setFixedHeight(28);
    setAttribute(Qt::WA_Hover, true);
    const auto& tc = Theme::colors();
    setStyleSheet(QStringLiteral(
        "PropertyRow { background: transparent; }"
        "PropertyRow:hover { background: %1; }")
        .arg(Theme::hex(tc.controlBgHover)));
}

void PropertyRow::setTrack(KeyframeTrack<float>* track)
{
    m_track = track;
    syncStopwatchState();
}

void PropertyRow::setCurveExpanded(bool expanded)
{
    if (m_expandBtn) m_expandBtn->setChecked(expanded);
}

void PropertyRow::addExtraTrack(KeyframeTrack<float>* track)
{
    if (!track) return;
    for (auto* t : m_extraTracks) if (t == track) return;
    m_extraTracks.push_back(track);
    syncStopwatchState();
}

void PropertyRow::removeExtraTrack(KeyframeTrack<float>* track)
{
    if (!track) return;
    m_extraTracks.erase(
        std::remove(m_extraTracks.begin(), m_extraTracks.end(), track),
        m_extraTracks.end());
    syncStopwatchState();
}

void PropertyRow::clearExtraTracks()
{
    m_extraTracks.clear();
    syncStopwatchState();
}

std::vector<KeyframeTrack<float>*> PropertyRow::allTracks() const
{
    std::vector<KeyframeTrack<float>*> out;
    if (m_track) out.push_back(m_track);
    for (auto* t : m_extraTracks) if (t) out.push_back(t);
    return out;
}

QString PropertyRow::propertyName() const { return m_name; }

void PropertyRow::syncStopwatchState()
{
    if (!m_stopwatch) return;
    const auto tracks = allTracks();
    const bool animated = std::any_of(
        tracks.begin(), tracks.end(), [](const auto* track) {
            return track && track->keyframeCount() > 0;
        });
    m_stopwatch->blockSignals(true);
    m_stopwatch->setEnabled(!tracks.empty());
    m_stopwatch->setChecked(animated);
    m_stopwatch->blockSignals(false);
}

void PropertyRow::configureStopwatchButton(QToolButton* button)
{
    if (!button) return;
    const auto& tc = Theme::colors();
    button->setText({});
    button->setFixedSize(18, 18);
    button->setCheckable(true);
    QIcon icon;
    icon.addPixmap(createStopwatchPixmap(tc.textTertiary),
                   QIcon::Normal, QIcon::Off);
    icon.addPixmap(createStopwatchPixmap(tc.accent),
                   QIcon::Normal, QIcon::On);
    button->setIcon(icon);
    button->setIconSize(QSize(14, 14));
    button->setStyleSheet(QStringLiteral(
        "QToolButton { background: transparent; border: none; padding: 0; }"));
}

void PropertyRow::buildUI()
{
    const auto& tc = Theme::colors();

    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(8, 2, 6, 2);
    layout->setSpacing(4);

    // Expand arrow (placeholder — expands to show bezier controls)
    m_expandBtn = new QToolButton(this);
    m_expandBtn->setText(QStringLiteral(">")); // > chevron like Premiere Pro
    m_expandBtn->setFixedSize(14, 18);
    m_expandBtn->setCheckable(true);
    m_expandBtn->setStyleSheet(QStringLiteral(
        "QToolButton { background: transparent; color: %1; border: none; font-size: %3px; padding: 0; }"
        "QToolButton:checked { color: %2; }")
        .arg(Theme::hex(tc.textTertiary), Theme::hex(tc.textPrimary))
        .arg(Theme::typography().sizeXxs));
    layout->addWidget(m_expandBtn);
    m_expandBtn->setToolTip(tr("Show keyframe value and velocity graphs"));
    connect(m_expandBtn, &QToolButton::toggled, this, [this](bool expanded) {
        m_expandBtn->setText(expanded ? QStringLiteral("v") : QStringLiteral(">"));
        emit curveEditorRequested(this, expanded);
    });

    // Stopwatch / clock icon (keyframe toggle)
    m_stopwatch = new QToolButton(this);
    configureStopwatchButton(m_stopwatch);
    m_stopwatch->setToolTip(tr("Toggle animation (keyframing)"));
    if (m_track) {
        // If track has any keyframes, keyframing is "on"
        m_stopwatch->setChecked(m_track->keyframeCount() > 0);
    }
    connect(m_stopwatch, &QToolButton::toggled, this, [this](bool on) {
        emit keyframingToggled(m_track, on);
    });
    layout->addWidget(m_stopwatch);

    // Property name
    m_nameLabel = new QLabel(m_name, this);
    m_nameLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
    m_nameLabel->setMinimumWidth(90);
    m_nameLabel->setStyleSheet(QStringLiteral(
        "QLabel { color: %1; font-size: %2px; background: transparent; padding-left: 2px; }")
        .arg(Theme::hex(tc.textPrimary))
        .arg(Theme::typography().sizeXs));
    layout->addWidget(m_nameLabel);

    // Value area (populated by addValueWidget / addValuePair)
    m_valueLayout = new QHBoxLayout;
    m_valueLayout->setContentsMargins(0, 0, 0, 0);
    m_valueLayout->setSpacing(6);
    layout->addLayout(m_valueLayout, 1);

    // Keyframe navigation: ◀ ◆ ▶
    auto kfBtnStyle = QStringLiteral(
        "QToolButton { background: transparent; color: %1; border: none; font-size: %4px; padding: 0; }"
        "QToolButton:hover { color: %2; }"
        "QToolButton:disabled { color: %3; }")
        .arg(Theme::hex(tc.textSecondary), Theme::hex(tc.textPrimary),
             Theme::hex(tc.textTertiary))
        .arg(Theme::typography().sizeXxs);

    m_prevKfBtn = new QToolButton(this);
    m_prevKfBtn->setText(QStringLiteral("\u25C0")); // ◀
    m_prevKfBtn->setFixedSize(16, 22);
    m_prevKfBtn->setToolTip(tr("Go to previous keyframe"));
    m_prevKfBtn->setStyleSheet(kfBtnStyle);
    connect(m_prevKfBtn, &QToolButton::clicked, this, [this]() {
        emit goToPrevKeyframe(m_track);
    });

    m_addKfBtn = new QToolButton(this);
    m_addKfBtn->setText(QStringLiteral("\u25C6")); // ◆
    m_addKfBtn->setFixedSize(16, 22);
    m_addKfBtn->setToolTip(tr("Add / remove keyframe"));
    m_addKfBtn->setStyleSheet(kfBtnStyle);
    connect(m_addKfBtn, &QToolButton::clicked, this, [this]() {
        // Do not capture the time (or the bound tracks) during the last UI
        // refresh. Playback updates are intentionally throttled, and graphic
        // layer selection can retarget a row between paints. Resolve both at
        // click time so the diamond always operates on what the user sees now.
        const int64_t time = m_timeProvider ? m_timeProvider()
                                             : m_displayedTime;
        const auto tracks = allTracks();
        bool atKeyframe = false;
        for (auto* tr : tracks) {
            if (tr && tr->hasKeyframeAt(time)) {
                atKeyframe = true;
                break;
            }
        }
        // One visible diamond click must become one history entry. The panel
        // expands the primary track into any compound siblings and records
        // the whole edit atomically.
        if (!m_track) return;
        if (atKeyframe)
            emit deleteKeyframeRequested(m_track, time);
        else
            emit addKeyframeRequested(m_track, time);
    });

    m_nextKfBtn = new QToolButton(this);
    m_nextKfBtn->setText(QStringLiteral("\u25B6")); // ▶
    m_nextKfBtn->setFixedSize(16, 22);
    m_nextKfBtn->setToolTip(tr("Go to next keyframe"));
    m_nextKfBtn->setStyleSheet(kfBtnStyle);
    connect(m_nextKfBtn, &QToolButton::clicked, this, [this]() {
        emit goToNextKeyframe(m_track);
    });

    // Per-attribute reset (↺) — Premiere Pro-style: restores this property
    // to its factory default and clears its keyframes (undoable). Sits just
    // left of the keyframe-nav arrows so every row exposes it consistently.
    m_resetBtn = new QToolButton(this);
    m_resetBtn->setText(QStringLiteral("↺")); // ↺
    m_resetBtn->setFixedSize(16, 22);
    m_resetBtn->setToolTip(tr("Reset %1 to default").arg(m_name));
    m_resetBtn->setStyleSheet(QStringLiteral(
        "QToolButton { background: transparent; color: %1; border: none; font-size: %3px; padding: 0; }"
        "QToolButton:hover { color: %2; }")
        .arg(Theme::hex(tc.textSecondary), Theme::hex(tc.warning))
        .arg(Theme::typography().sizeXs));
    connect(m_resetBtn, &QToolButton::clicked, this, [this]() {
        emit resetRequested();
    });
    layout->addWidget(m_resetBtn);

    layout->addWidget(m_prevKfBtn);
    layout->addWidget(m_addKfBtn);
    layout->addWidget(m_nextKfBtn);

    // Hide kf nav if no track; show stopwatch but disabled for non-keyframeable
    bool hasTrack = (m_track != nullptr);
    m_stopwatch->setVisible(true);
    m_stopwatch->setEnabled(hasTrack);
    if (!hasTrack) m_stopwatch->setIconSize(QSize(14, 14));  // ensure icon shows
    m_prevKfBtn->setVisible(hasTrack);
    m_addKfBtn->setVisible(hasTrack);
    m_nextKfBtn->setVisible(hasTrack);
}

void PropertyRow::addValueWidget(ScrubbySpinBox* spin)
{
    m_valueLayout->addWidget(spin, 1);
}

void PropertyRow::addValuePair(ScrubbySpinBox* spinA, ScrubbySpinBox* spinB)
{
    m_valueLayout->addWidget(spinA, 1);
    m_valueLayout->addWidget(spinB, 1);
}

void PropertyRow::addCustomWidget(QWidget* widget)
{
    m_valueLayout->addWidget(widget, 1);
}

void PropertyRow::updateForTime(int64_t time)
{
    if (!m_track) return;

    m_displayedTime = time;
    syncStopwatchState();

    const auto tracks = allTracks();

    // "At a keyframe" if ANY bound track has a keyframe at this time.
    bool atKeyframe = false;
    for (auto* tr : tracks) {
        for (size_t i = 0; i < tr->keyframeCount(); ++i) {
            if (tr->keyframe(i).time == time) { atKeyframe = true; break; }
        }
        if (atKeyframe) break;
    }

    // Update diamond button style based on whether we're at a keyframe
    const auto& tc = Theme::colors();
    if (atKeyframe) {
        m_addKfBtn->setStyleSheet(QStringLiteral(
            "QToolButton { background: transparent; color: %1; border: none; font-size: 9px; padding: 0; }"
            "QToolButton:hover { color: %2; }")
            .arg(Theme::hex(tc.accent), Theme::hex(tc.accentHover)));
    } else {
        m_addKfBtn->setStyleSheet(QStringLiteral(
            "QToolButton { background: transparent; color: %1; border: none; font-size: 9px; padding: 0; }"
            "QToolButton:hover { color: %2; }")
            .arg(Theme::hex(tc.textSecondary), Theme::hex(tc.textPrimary)));
    }

    // Enable/disable prev/next based on whether keyframes exist in that
    // direction on ANY bound track.
    bool hasPrev = false, hasNext = false;
    for (auto* tr : tracks) {
        for (size_t i = 0; i < tr->keyframeCount(); ++i) {
            if (tr->keyframe(i).time < time) hasPrev = true;
            if (tr->keyframe(i).time > time) hasNext = true;
        }
    }
    m_prevKfBtn->setEnabled(hasPrev);
    m_nextKfBtn->setEnabled(hasNext);
}


} // namespace rt
