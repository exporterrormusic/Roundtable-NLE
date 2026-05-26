/*
 * EffectControlsPanelUI.cpp -- PropertyRow / KeyframeTimeline widgets,
 * EffectControlsPanel constructor, createScrubby, and setupUI.
 *
 * Split from EffectControlsPanel.cpp for maintainability.
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

void PropertyRow::setTrack(KeyframeTrack<float>* track) { m_track = track; }

void PropertyRow::addExtraTrack(KeyframeTrack<float>* track)
{
    if (track) m_extraTracks.push_back(track);
}

std::vector<KeyframeTrack<float>*> PropertyRow::allTracks() const
{
    std::vector<KeyframeTrack<float>*> out;
    if (m_track) out.push_back(m_track);
    for (auto* t : m_extraTracks) if (t) out.push_back(t);
    return out;
}

QString PropertyRow::propertyName() const { return m_name; }

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
        "QToolButton { background: transparent; color: %1; border: none; font-size: 11px; padding: 0; }"
        "QToolButton:checked { color: %2; }")
        .arg(Theme::hex(tc.textTertiary), Theme::hex(tc.textPrimary)));
    layout->addWidget(m_expandBtn);

    // Stopwatch / clock icon (keyframe toggle)
    m_stopwatch = new QToolButton(this);
    m_stopwatch->setFixedSize(18, 18);
    m_stopwatch->setCheckable(true);
    m_stopwatch->setToolTip(tr("Toggle animation (keyframing)"));
    {
        QIcon icon;
        icon.addPixmap(createStopwatchPixmap(tc.textTertiary), QIcon::Normal, QIcon::Off);
        icon.addPixmap(createStopwatchPixmap(tc.accent), QIcon::Normal, QIcon::On);
        m_stopwatch->setIcon(icon);
        m_stopwatch->setIconSize(QSize(14, 14));
    }
    m_stopwatch->setStyleSheet(QStringLiteral(
        "QToolButton { background: transparent; border: none; padding: 0; }"));
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
    m_nameLabel->setMinimumWidth(90);
    m_nameLabel->setStyleSheet(QStringLiteral(
        "QLabel { color: %1; font-size: 12px; background: transparent; padding-left: 2px; }")
        .arg(Theme::hex(tc.textPrimary)));
    layout->addWidget(m_nameLabel);

    // Value area (populated by addValueWidget / addValuePair)
    m_valueLayout = new QHBoxLayout;
    m_valueLayout->setContentsMargins(0, 0, 0, 0);
    m_valueLayout->setSpacing(6);
    layout->addLayout(m_valueLayout, 1);

    // Keyframe navigation: ◀ ◆ ▶
    auto kfBtnStyle = QStringLiteral(
        "QToolButton { background: transparent; color: %1; border: none; font-size: 11px; padding: 0; }"
        "QToolButton:hover { color: %2; }"
        "QToolButton:disabled { color: %3; }")
        .arg(Theme::hex(tc.textSecondary), Theme::hex(tc.textPrimary),
             Theme::hex(tc.textTertiary));

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
        "QToolButton { background: transparent; color: %1; border: none; font-size: 12px; padding: 0; }"
        "QToolButton:hover { color: %2; }")
        .arg(Theme::hex(tc.textSecondary), Theme::hex(tc.warning)));
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
        // Click = delete keyframe on every bound track at this time
        m_addKfBtn->disconnect();
        connect(m_addKfBtn, &QToolButton::clicked, this, [this, time, tracks]() {
            for (auto* tr : tracks)
                emit deleteKeyframeRequested(tr, time);
        });
    } else {
        m_addKfBtn->setStyleSheet(QStringLiteral(
            "QToolButton { background: transparent; color: %1; border: none; font-size: 9px; padding: 0; }"
            "QToolButton:hover { color: %2; }")
            .arg(Theme::hex(tc.textSecondary), Theme::hex(tc.textPrimary)));
        // Click = add keyframe on every bound track at this time
        m_addKfBtn->disconnect();
        connect(m_addKfBtn, &QToolButton::clicked, this, [this, time, tracks]() {
            for (auto* tr : tracks)
                emit addKeyframeRequested(tr, time);
        });
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

// ═════════════════════════════════════════════════════════════════════════════
//  KeyframeTimeline — mini-timeline with diamonds
// ═════════════════════════════════════════════════════════════════════════════

KeyframeTimeline::KeyframeTimeline(QWidget* parent)
    : QWidget(parent)
{
    setMinimumWidth(100);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setMouseTracking(true);
    setFocusPolicy(Qt::ClickFocus);

    setStyleSheet(QStringLiteral("background: %1;")
        .arg(Theme::hex(Theme::colors().surface0)));
}

void KeyframeTimeline::setClip(Clip* clip)
{
    m_clip = clip;
    if (clip) {
        m_viewStart = 0;
        m_viewEnd = clip->duration();
        if (m_viewEnd <= 0) m_viewEnd = 48000 * 10;
    }
    update();
}

void KeyframeTimeline::setPropertyRows(const std::vector<PropertyRow*>& rows)
{
    m_rows = rows;
    update();
}

void KeyframeTimeline::setScrollOffset(int y)
{
    m_scrollOffsetY = y;
    update();
}

void KeyframeTimeline::setPlayheadTick(int64_t tick)
{
    m_playheadTick = tick;
    update();
}

void KeyframeTimeline::setViewRange(int64_t startTick, int64_t endTick)
{
    m_viewStart = startTick;
    m_viewEnd = endTick;
    update();
}

int KeyframeTimeline::tickToX(int64_t tick) const
{
    if (m_viewEnd <= m_viewStart) return 0;
    double ratio = static_cast<double>(tick - m_viewStart) /
                   static_cast<double>(m_viewEnd - m_viewStart);
    return static_cast<int>(ratio * (width() - 1));
}

int64_t KeyframeTimeline::xToTick(int x) const
{
    if (width() <= 1) return m_viewStart;
    double ratio = static_cast<double>(x) / static_cast<double>(width() - 1);
    return m_viewStart + static_cast<int64_t>(ratio * (m_viewEnd - m_viewStart));
}

int KeyframeTimeline::rowY(int rowIndex) const
{
    return kRulerHeight + kClipBarHeight + (rowIndex * kRowHeight) + kRowHeight / 2
           - m_scrollOffsetY;
}

void KeyframeTimeline::paintEvent(QPaintEvent* /*event*/)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    const auto& tc = Theme::colors();

    // Background
    p.fillRect(rect(), tc.surface0);

    drawRuler(p);
    drawClipBar(p);
    drawKeyframeDiamonds(p);
    drawPlayhead(p);
}

void KeyframeTimeline::drawRuler(QPainter& p)
{
    const auto& tc = Theme::colors();

    // Ruler background
    QRect rulerRect(0, 0, width(), kRulerHeight);
    p.fillRect(rulerRect, tc.surface2);

    // Bottom line
    p.setPen(tc.border);
    p.drawLine(0, kRulerHeight - 1, width(), kRulerHeight - 1);

    // Time labels
    p.setPen(tc.textTertiary);
    QFont rulerFont;
    rulerFont.setPixelSize(9);
    p.setFont(rulerFont);

    int64_t range = m_viewEnd - m_viewStart;
    if (range <= 0) return;

    // Determine step size (aim for ~80px spacing between labels)
    int labelCount = std::max(1, width() / 80);
    int64_t step = range / labelCount;
    // Round step to nice timecode values
    if (step < 4800)       step = 4800;   // 0.1s
    else if (step < 24000) step = 24000;  // 0.5s
    else if (step < 48000) step = 48000;  // 1s
    else                   step = (step / 48000 + 1) * 48000;

    for (int64_t t = (m_viewStart / step) * step; t <= m_viewEnd; t += step) {
        if (t < m_viewStart) continue;
        int x = tickToX(t);

        // Tick mark
        p.drawLine(x, kRulerHeight - 6, x, kRulerHeight - 1);

        // Timecode label (HH:MM:SS;FF @ 30fps)
        int64_t totalFrames = t / 1600; // 48000/30 = 1600 ticks per frame
        int frames  = totalFrames % 30;
        int seconds = (totalFrames / 30) % 60;
        int minutes = (totalFrames / 30 / 60) % 60;
        QString tc_str = QStringLiteral("%1:%2:%3;%4")
            .arg(0, 2, 10, QChar('0'))
            .arg(minutes, 2, 10, QChar('0'))
            .arg(seconds, 2, 10, QChar('0'))
            .arg(frames, 2, 10, QChar('0'));

        p.drawText(x + 2, kRulerHeight - 8, tc_str);
    }
}

void KeyframeTimeline::drawClipBar(QPainter& p)
{
    if (!m_clip) return;

    const auto& tc = Theme::colors();

    int y = kRulerHeight + 2;
    int x0 = tickToX(0);
    int x1 = tickToX(m_clip->duration());
    int barW = std::max(1, x1 - x0);

    // Clip bar (magenta/pink like Premiere Pro)
    QRect barRect(x0, y, barW, kClipBarHeight - 4);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(200, 70, 200, 180));
    p.drawRect(barRect);

    // Clip name inside bar
    p.setPen(tc.textPrimary);
    QFont barFont;
    barFont.setPixelSize(9);
    p.setFont(barFont);
    p.drawText(barRect.adjusted(4, 0, -4, 0), Qt::AlignVCenter | Qt::AlignLeft,
               QString::fromStdString(m_clip->label()));
}

void KeyframeTimeline::drawKeyframeDiamonds(QPainter& p)
{
    if (!m_clip) return;

    const auto& tc = Theme::colors();

    // Build visible-row-index map
    std::unordered_map<const PropertyRow*, int> visIdx;
    int vi = 0;
    for (const auto* row : m_rows) {
        if (!row->isVisible()) continue;
        visIdx[row] = vi++;
    }

    // Draw horizontal divider lines for each visible property row
    p.setPen(QPen(tc.border, 1.0));
    for (const auto* row : m_rows) {
        auto it = visIdx.find(row);
        if (it == visIdx.end()) continue;
        int y = rowY(it->second) + kRowHeight / 2;
        if (y > kRulerHeight && y < height()) {
            p.drawLine(0, y, width(), y);
        }
    }

    // Draw keyframe diamonds
    p.setPen(Qt::NoPen);

    for (const auto* row : m_rows) {
        auto it = visIdx.find(row);
        if (it == visIdx.end()) continue;
        const auto rowTracks = row->allTracks();
        if (rowTracks.empty()) continue;

        int y = rowY(it->second);
        if (y < kRulerHeight || y > height()) continue;

        // Union the keyframe times across every track bound to this row so
        // compound properties like Position (X+Y) render a single diamond
        // per time. The "representative" InterpMode for icon selection
        // comes from the first track that has a keyframe at that time.
        struct DiamondInfo {
            InterpMode interp;
            bool       selected;
        };
        std::map<int64_t, DiamondInfo> diamonds;
        for (auto* track : rowTracks) {
            if (!track) continue;
            for (size_t i = 0; i < track->keyframeCount(); ++i) {
                const auto& kf = track->keyframe(i);
                int64_t t = kf.time;
                auto& d = diamonds[t];
                d.interp = kf.interp;  // last write wins; first track usually primary
                if (m_selectedKeys.count({track, t}) > 0)
                    d.selected = true;
            }
        }
        for (auto& [t, info] : diamonds) {
            int x = tickToX(t);
            constexpr int d = kDiamondRadius;
            const bool selected = info.selected;
            // Premiere convention: unselected = neutral grey, selected =
            // the accent color so the user can see exactly which keyframes
            // will move.
            p.setBrush(selected ? tc.accent : tc.textTertiary);

            // Premiere-style per-mode keyframe icon:
            //   Linear            → diamond
            //   Hold              → square (step)
            //   Bezier / Auto /   → circle
            //   Continuous / Ease
            switch (info.interp) {
            case InterpMode::Linear: {
                QPolygonF diamond;
                diamond << QPointF(x, y - d) << QPointF(x + d, y)
                        << QPointF(x, y + d) << QPointF(x - d, y);
                p.drawPolygon(diamond);
                break;
            }
            case InterpMode::Hold: {
                p.drawRect(QRectF(x - d + 0.5, y - d + 0.5, 2.0 * d - 1.0, 2.0 * d - 1.0));
                break;
            }
            case InterpMode::Bezier:
            case InterpMode::AutoBezier:
            case InterpMode::ContinuousBezier:
            case InterpMode::EaseIn:
            case InterpMode::EaseOut:
            default: {
                p.drawEllipse(QPointF(x, y), static_cast<double>(d), static_cast<double>(d));
                break;
            }
            }
        }
    }

    // Draw marquee rubber-band
    if (m_marqueeActive) {
        QRect rect = QRect(m_marqueeOrigin, m_marqueeCurrent).normalized();
        p.setPen(QPen(tc.accent, 1.0, Qt::DashLine));
        QColor fill = tc.accent;
        fill.setAlpha(30);
        p.setBrush(fill);
        p.drawRect(rect);
    }
}

void KeyframeTimeline::drawPlayhead(QPainter& p)
{
    const auto& tc = Theme::colors();

    // Clip-relative playhead
    int64_t relTick = m_playheadTick;
    if (m_clip) {
        relTick = m_playheadTick - m_clip->timelineIn();
    }

    int x = tickToX(relTick);
    if (x < 0 || x > width()) return;

    // Thin vertical line (Premiere Pro blue)
    p.setPen(QPen(tc.playhead, 1.0));
    p.drawLine(x, 0, x, height());

    // Small triangle at top
    QPolygonF tri;
    tri << QPointF(x - 4, 0) << QPointF(x + 4, 0) << QPointF(x, 6);
    p.setPen(Qt::NoPen);
    p.setBrush(tc.playhead);
    p.drawPolygon(tri);
}

void KeyframeTimeline::mousePressEvent(QMouseEvent* event)
{
    if (event->button() == Qt::RightButton) {
        // Premiere-style right-click menu on a keyframe diamond.
        auto hit = hitTestKeyframe(event->pos());
        if (!hit.track) return;
        const int64_t kfTime = hit.track->keyframe(hit.index).time;

        // Selection: right-click on an un-selected diamond replaces selection
        // with just that one; right-click on a selected one keeps multi-select
        // so the action applies to the whole group (matches Premiere).
        SelKey clickedKey{hit.track, kfTime};
        if (m_selectedKeys.count(clickedKey) == 0) {
            m_selectedKeys.clear();
            m_selectedKeys.insert(clickedKey);
            update();
        }

        // Snapshot the current selection now — the menu's nested event loop
        // can fire focus events that drop m_selectedKeys before the chosen
        // action runs, so the lambdas need their own copy to act on.
        const auto keysSnap = m_selectedKeys;

        QMenu menu(this);
        QMenu* interpSub = menu.addMenu(QStringLiteral("Temporal Interpolation"));
        auto addInterpAction = [&](const QString& label, InterpMode mode) {
            QAction* a = interpSub->addAction(label);
            connect(a, &QAction::triggered, this, [this, mode, keysSnap]() {
                for (const auto& sk : keysSnap) {
                    if (!sk.track) continue;
                    if (m_commandStack)
                        m_commandStack->execute(
                            std::make_unique<SetKeyframeInterpCommand>(sk.track, sk.time, mode));
                    else {
                        for (size_t i = 0; i < sk.track->keyframeCount(); ++i)
                            if (sk.track->keyframe(i).time == sk.time)
                                sk.track->keyframe(i).interp = mode;
                    }
                }
                emit keyframeChanged();
                update();
            });
        };
        addInterpAction(QStringLiteral("Linear"),            InterpMode::Linear);
        addInterpAction(QStringLiteral("Bezier"),            InterpMode::Bezier);
        addInterpAction(QStringLiteral("Auto Bezier"),       InterpMode::AutoBezier);
        addInterpAction(QStringLiteral("Continuous Bezier"), InterpMode::ContinuousBezier);
        addInterpAction(QStringLiteral("Hold"),              InterpMode::Hold);
        interpSub->addSeparator();
        addInterpAction(QStringLiteral("Ease In"),           InterpMode::EaseIn);
        addInterpAction(QStringLiteral("Ease Out"),          InterpMode::EaseOut);

        menu.addSeparator();
        QAction* del = menu.addAction(QStringLiteral("Delete"));
        connect(del, &QAction::triggered, this, [this, keysSnap]() {
            for (const auto& sk : keysSnap) {
                if (!sk.track) continue;
                if (m_commandStack)
                    m_commandStack->execute(
                        std::make_unique<RemoveKeyframeCommand>(sk.track, sk.time));
                else
                    sk.track->removeKeyframeAtTime(sk.time);
            }
            m_selectedKeys.clear();
            emit keyframeChanged();
            update();
        });

        menu.exec(event->globalPosition().toPoint());
        return;
    }
    if (event->button() == Qt::LeftButton) {
        // Ruler / clip-bar area → scrub, clamped to clip edges
        if (event->pos().y() < kRulerHeight + kClipBarHeight) {
            m_scrubbing = true;
            int64_t tick = xToTick(event->pos().x());
            if (m_clip) {
                int64_t clipDur = m_clip->duration();
                if (tick < 0) tick = 0;
                if (tick > clipDur) tick = clipDur;
                tick += m_clip->timelineIn();
            }
            emit playheadScrubbed(tick);
            return;
        }

        bool shift = event->modifiers() & Qt::ShiftModifier;

        // Hit-test keyframe diamonds
        auto hit = hitTestKeyframe(event->pos());
        if (hit.track) {
            const int64_t hitTime = hit.track->keyframe(hit.index).time;

            // Compound rows (Position = X + Y) render ONE diamond per time
            // across every bound track. Expand the selection so clicking
            // that diamond grabs every sibling keyframe at the same time —
            // otherwise dragging would move X but leave Y behind, which
            // is exactly the "doesn't seem to save both x and y" symptom.
            std::vector<SelKey> siblings;
            siblings.push_back({hit.track, hitTime});
            for (const auto* row : m_rows) {
                if (!row->isVisible()) continue;
                bool rowOwnsHit = false;
                for (auto* tr : row->allTracks())
                    if (tr == hit.track) { rowOwnsHit = true; break; }
                if (!rowOwnsHit) continue;
                for (auto* tr : row->allTracks()) {
                    if (!tr || tr == hit.track) continue;
                    for (size_t i = 0; i < tr->keyframeCount(); ++i) {
                        if (tr->keyframe(i).time == hitTime) {
                            siblings.push_back({tr, hitTime});
                            break;
                        }
                    }
                }
            }

            SelKey key = siblings.front();
            bool alreadySelected = m_selectedKeys.count(key) > 0;

            if (shift) {
                // Toggle selection of this keyframe (and its siblings)
                if (alreadySelected) {
                    for (const auto& s : siblings) m_selectedKeys.erase(s);
                } else {
                    for (const auto& s : siblings) m_selectedKeys.insert(s);
                }
            } else {
                if (!alreadySelected) {
                    // Click on unselected keyframe → select this + siblings
                    m_selectedKeys.clear();
                    for (const auto& s : siblings) m_selectedKeys.insert(s);
                }
                // else: already selected, keep multi-selection for group drag
            }

            // Start dragging the selection
            m_draggingSelection = true;
            m_dragAnchorTick = xToTick(event->pos().x());
            m_dragEntries.clear();
            for (const auto& sk : m_selectedKeys) {
                for (size_t i = 0; i < sk.track->keyframeCount(); ++i) {
                    const auto& kf = sk.track->keyframe(i);
                    if (kf.time == sk.time) {
                        m_dragEntries.push_back({sk.track, kf.time, kf.time,
                                                 kf.value, kf.interp,
                                                 kf.bezierInX, kf.bezierInY,
                                                 kf.bezierOutX, kf.bezierOutY});
                        break;
                    }
                }
            }
            setFocus();
            update();
            return;
        }

        // Click on empty area → start marquee
        if (!shift) m_selectedKeys.clear();
        m_preMarqueeSelection = m_selectedKeys;
        m_marqueeActive = true;
        m_marqueeOrigin = event->pos();
        m_marqueeCurrent = event->pos();
        m_draggingSelection = false;
        setFocus();
        update();
    }
}

void KeyframeTimeline::mouseMoveEvent(QMouseEvent* event)
{
    if (m_scrubbing) {
        int64_t tick = xToTick(event->pos().x());
        if (m_clip) {
            int64_t clipDur = m_clip->duration();
            if (tick < 0) tick = 0;
            if (tick > clipDur) tick = clipDur;
            tick += m_clip->timelineIn();
        }
        emit playheadScrubbed(tick);
        return;
    }

    if (m_marqueeActive) {
        m_marqueeCurrent = event->pos();
        // Build selection from keyframes inside the marquee rect, inflated
        // by the diamond's visual radius so a keyframe whose CENTER is
        // just past the marquee edge (common when the user drags to the
        // very last frame and can't push the cursor past the widget's
        // right edge) still gets selected. Without this, edge keyframes
        // flickered blue during the drag but were dropped on release.
        QRect rect = QRect(m_marqueeOrigin, m_marqueeCurrent).normalized();
        QRect hitRect = rect.adjusted(-kDiamondRadius, -kDiamondRadius,
                                       kDiamondRadius,  kDiamondRadius);
        m_selectedKeys = m_preMarqueeSelection; // start from pre-existing
        int vi = 0;
        for (const auto* row : m_rows) {
            if (!row->isVisible()) continue;
            const auto rowTracks = row->allTracks();
            if (rowTracks.empty()) { ++vi; continue; }
            int y = rowY(vi);
            for (auto* track : rowTracks) {
                if (!track) continue;
                for (size_t i = 0; i < track->keyframeCount(); ++i) {
                    int x = tickToX(track->keyframe(i).time);
                    if (hitRect.contains(x, y))
                        m_selectedKeys.insert({track, track->keyframe(i).time});
                }
            }
            ++vi;
        }
        update();
        return;
    }

    if (m_draggingSelection && !m_dragEntries.empty()) {
        int64_t currentTick = xToTick(event->pos().x());
        int64_t delta = currentTick - m_dragAnchorTick;

        // Premiere-style magnetism (hold Shift to disable). Snap delta so
        // the "leader" entry (the keyframe nearest the cursor when the drag
        // began — i.e. the first entry the drag was anchored to) lands on
        // a sibling keyframe in another track, or on the playhead. We snap
        // the GROUP delta rather than each keyframe individually so a
        // multi-select drag keeps its relative spacing.
        const bool snapEnabled = !(event->modifiers() & Qt::ShiftModifier);
        if (snapEnabled && !m_dragEntries.empty()) {
            // Build a set of dragged (track, origTime) so we don't snap to
            // ourselves or to siblings within the drag group.
            std::set<std::pair<KeyframeTrack<float>*, int64_t>> dragged;
            for (const auto& e : m_dragEntries)
                dragged.insert({e.track, e.origTime});

            // Candidate snap targets: every non-dragged keyframe on any
            // visible row, plus the playhead.
            std::vector<int64_t> targets;
            for (const auto* row : m_rows) {
                if (!row->isVisible()) continue;
                auto* tr = row->track();
                if (!tr) continue;
                for (size_t i = 0; i < tr->keyframeCount(); ++i) {
                    int64_t t = tr->keyframe(i).time;
                    if (!dragged.count({tr, t}))
                        targets.push_back(t);
                }
            }
            if (m_clip) targets.push_back(m_playheadTick - m_clip->timelineIn());

            // ~6 px snap radius — wide enough to feel sticky, narrow enough
            // to not interfere with fine placement. Convert to ticks via
            // the local ruler scale (1 px ≈ tickRangePerPx).
            const double pxPerTick = (m_clip && m_clip->duration() > 0)
                ? (static_cast<double>(width()) / static_cast<double>(m_clip->duration()))
                : 1.0;
            const int64_t snapRadiusTicks = pxPerTick > 0.0
                ? static_cast<int64_t>(6.0 / pxPerTick)
                : 0;

            // Find the best snap by checking what each dragged keyframe
            // would land on after applying `delta`. The smallest distance
            // wins; that snap then biases the group delta.
            int64_t bestDelta   = delta;
            int64_t bestDist    = snapRadiusTicks + 1;  // sentinel
            for (const auto& e : m_dragEntries) {
                int64_t projected = e.origTime + delta;
                for (int64_t tgt : targets) {
                    int64_t d = std::abs(projected - tgt);
                    if (d <= snapRadiusTicks && d < bestDist) {
                        bestDist  = d;
                        bestDelta = tgt - e.origTime;
                    }
                }
            }
            delta = bestDelta;
        }

        // Remove all dragged keyframes from their current positions
        for (auto& entry : m_dragEntries) {
            entry.track->removeKeyframeAtTime(entry.currentTime);
        }

        // Reinsert at new positions, clamped to [0, clipDuration]
        m_selectedKeys.clear();
        int64_t maxTime = m_clip ? m_clip->duration() : INT64_MAX;
        for (auto& entry : m_dragEntries) {
            int64_t newTime = std::clamp(entry.origTime + delta, int64_t(0), maxTime);
            entry.track->addKeyframe(newTime, entry.value, entry.interp);
            // Restore bezier handles
            for (size_t i = 0; i < entry.track->keyframeCount(); ++i) {
                if (entry.track->keyframe(i).time == newTime) {
                    auto& kf = entry.track->keyframe(i);
                    kf.bezierInX = entry.biX;
                    kf.bezierInY = entry.biY;
                    kf.bezierOutX = entry.boX;
                    kf.bezierOutY = entry.boY;
                    break;
                }
            }
            entry.currentTime = newTime;
            m_selectedKeys.insert({entry.track, newTime});
        }

        update();
        return;
    }
}

void KeyframeTimeline::mouseReleaseEvent(QMouseEvent* /*event*/)
{
    if (m_draggingSelection && !m_dragEntries.empty()) {
        // Check if any keyframe actually moved
        bool moved = false;
        for (const auto& entry : m_dragEntries) {
            if (entry.currentTime != entry.origTime) { moved = true; break; }
        }
        if (moved && m_commandStack) {
            // Build undo/redo data: snapshot original and final positions
            struct MoveInfo {
                KeyframeTrack<float>* track;
                int64_t origTime;
                int64_t newTime;
                float value;
                InterpMode interp;
                float biX, biY, boX, boY;
            };
            auto moves = std::make_shared<std::vector<MoveInfo>>();
            for (const auto& entry : m_dragEntries) {
                moves->push_back({entry.track, entry.origTime, entry.currentTime,
                                  entry.value, entry.interp,
                                  entry.biX, entry.biY, entry.boX, entry.boY});
            }
            m_commandStack->pushWithoutExecute(
                std::make_unique<LambdaCommand>(
                    "Move Keyframes",
                    [moves]() {
                        // Redo: move from orig → new
                        for (auto& m : *moves) {
                            m.track->removeKeyframeAtTime(m.origTime);
                            m.track->addKeyframe(m.newTime, m.value, m.interp);
                            for (size_t i = 0; i < m.track->keyframeCount(); ++i) {
                                if (m.track->keyframe(i).time == m.newTime) {
                                    auto& kf = m.track->keyframe(i);
                                    kf.bezierInX = m.biX;  kf.bezierInY = m.biY;
                                    kf.bezierOutX = m.boX; kf.bezierOutY = m.boY;
                                    break;
                                }
                            }
                        }
                    },
                    [moves]() {
                        // Undo: move from new → orig
                        for (auto& m : *moves) {
                            m.track->removeKeyframeAtTime(m.newTime);
                            m.track->addKeyframe(m.origTime, m.value, m.interp);
                            for (size_t i = 0; i < m.track->keyframeCount(); ++i) {
                                if (m.track->keyframe(i).time == m.origTime) {
                                    auto& kf = m.track->keyframe(i);
                                    kf.bezierInX = m.biX;  kf.bezierInY = m.biY;
                                    kf.bezierOutX = m.boX; kf.bezierOutY = m.boY;
                                    break;
                                }
                            }
                        }
                    }));
            emit keyframeChanged();
        } else if (moved) {
            emit keyframeChanged();
        }
    }
    m_scrubbing = false;
    m_draggingSelection = false;
    m_dragEntries.clear();
    const bool wasMarquee = m_marqueeActive;
    m_marqueeActive = false;
    if (wasMarquee) {
        // Repaint to erase the marquee rubber-band; otherwise it stays on
        // screen until something else triggers a repaint.
        update();
    }
}

void KeyframeTimeline::focusOutEvent(QFocusEvent* event)
{
    // Premiere-style: clicking anywhere else in the Effect Controls panel
    // (a spinbox, a section header, the property tree background, etc.)
    // moves keyboard focus away from the mini-timeline, which is when we
    // drop any keyframe selection. Skip clearing on popup activations (the
    // right-click context menu) and on window deactivation so a returning
    // user isn't surprised by losing selection when they re-focus the app.
    const bool popupActive = (QApplication::activePopupWidget() != nullptr);
    if (event->reason() != Qt::PopupFocusReason &&
        event->reason() != Qt::ActiveWindowFocusReason &&
        !popupActive &&
        !m_selectedKeys.empty()) {
        m_selectedKeys.clear();
        update();
    }
    QWidget::focusOutEvent(event);
}

void KeyframeTimeline::keyPressEvent(QKeyEvent* event)
{
    if ((event->key() == Qt::Key_Delete || event->key() == Qt::Key_Backspace)
        && !m_selectedKeys.empty()) {
        if (m_commandStack) {
            // Save all selected keyframes for undo
            struct KfInfo {
                KeyframeTrack<float>* track;
                Keyframe<float> kf;
            };
            auto saved = std::make_shared<std::vector<KfInfo>>();
            for (const auto& sk : m_selectedKeys) {
                for (size_t i = 0; i < sk.track->keyframeCount(); ++i) {
                    if (sk.track->keyframe(i).time == sk.time) {
                        saved->push_back({sk.track, sk.track->keyframe(i)});
                        break;
                    }
                }
            }
            // Remove all selected
            for (auto it = m_selectedKeys.rbegin(); it != m_selectedKeys.rend(); ++it) {
                it->track->removeKeyframeAtTime(it->time);
            }
            m_commandStack->pushWithoutExecute(
                std::make_unique<LambdaCommand>(
                    "Delete Keyframes",
                    [saved]() {
                        // Redo: remove them again
                        for (auto it = saved->rbegin(); it != saved->rend(); ++it)
                            it->track->removeKeyframeAtTime(it->kf.time);
                    },
                    [saved]() {
                        // Undo: re-add all saved keyframes
                        for (auto& info : *saved) {
                            info.track->addKeyframe(info.kf.time, info.kf.value, info.kf.interp);
                            for (size_t i = 0; i < info.track->keyframeCount(); ++i) {
                                if (info.track->keyframe(i).time == info.kf.time) {
                                    auto& kf = info.track->keyframe(i);
                                    kf.bezierInX = info.kf.bezierInX;
                                    kf.bezierInY = info.kf.bezierInY;
                                    kf.bezierOutX = info.kf.bezierOutX;
                                    kf.bezierOutY = info.kf.bezierOutY;
                                    break;
                                }
                            }
                        }
                    }));
        } else {
            for (auto it = m_selectedKeys.rbegin(); it != m_selectedKeys.rend(); ++it) {
                it->track->removeKeyframeAtTime(it->time);
            }
        }
        m_selectedKeys.clear();
        update();
        emit keyframeChanged();
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}

KeyframeTimeline::HitResult KeyframeTimeline::hitTestKeyframe(const QPoint& pos) const
{
    // Build visible-row-index map
    std::unordered_map<const PropertyRow*, int> visIdx;
    int vi = 0;
    for (const auto* row : m_rows) {
        if (!row->isVisible()) continue;
        visIdx[row] = vi++;
    }

    for (const auto* row : m_rows) {
        auto it = visIdx.find(row);
        if (it == visIdx.end()) continue;
        const auto rowTracks = row->allTracks();
        if (rowTracks.empty()) continue;

        int y = rowY(it->second);
        if (y < kRulerHeight || y > height()) continue;

        // Look across every track bound to this row — a compound row like
        // Position renders ONE diamond at each time covering both X and Y;
        // returning either track is fine, mousePressEvent expands the
        // selection to all sibling keyframes at the same time below.
        for (auto* track : rowTracks) {
            if (!track) continue;
            for (size_t i = 0; i < track->keyframeCount(); ++i) {
                int x = tickToX(track->keyframe(i).time);
                int dx = pos.x() - x;
                int dy = pos.y() - y;
                // Manhattan distance check (diamond shape). Use a generous
                // 5px slop so near-misses on the small diamond grab it
                // instead of falling through to the marquee/empty-area path,
                // which would let the user accidentally start a marquee on
                // top of an existing keyframe they meant to drag.
                if (std::abs(dx) + std::abs(dy) <= kDiamondRadius + 5) {
                    return {track, i};
                }
            }
        }
    }
    return {};
}

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

    // Keyframe moved or deleted in the mini-timeline
    connect(m_kfTimeline, &KeyframeTimeline::keyframeChanged,
            this, &EffectControlsPanel::propertyChanged);

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
