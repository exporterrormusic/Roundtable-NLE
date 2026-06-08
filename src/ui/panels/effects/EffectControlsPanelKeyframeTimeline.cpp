/*
 * EffectControlsPanelKeyframeTimeline.cpp
 * KeyframeTimeline mini-timeline (diamonds) widget.
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
#include <limits>
#include <map>
#include <set>
#include <unordered_map>
#include <vector>

namespace rt {

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

int KeyframeTimeline::rowY(const PropertyRow* row) const
{
    // Use the PropertyRow's actual Y position within its parent container
    // rather than a calculated rowIndex * kRowHeight. This ensures the
    // keyframe tracks line up with the effect entries even when section
    // headers, checkboxes, and other non-row widgets add vertical space
    // between PropertyRows in the left-side layout.
    //
    // The left scrollArea and this KeyframeTimeline share the same Y=0
    // origin (both are children of the same QSplitter), so the row's
    // container position maps directly to this widget's coordinates
    // after subtracting the synced scroll offset.
    return row->pos().y() + kRowHeight / 2 - m_scrollOffsetY;
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

    // Draw horizontal divider lines for each visible property row
    p.setPen(QPen(tc.border, 1.0));
    for (const auto* row : m_rows) {
        if (!row->isVisible()) continue;
        int y = rowY(row) + kRowHeight / 2;
        if (y > kRulerHeight && y < height()) {
            p.drawLine(0, y, width(), y);
        }
    }

    // Draw keyframe diamonds
    p.setPen(Qt::NoPen);

    for (const auto* row : m_rows) {
        if (!row->isVisible()) continue;
        const auto rowTracks = row->allTracks();
        if (rowTracks.empty()) continue;

        int y = rowY(row);
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
            m_dragTrackSnap.clear();
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
            // Snapshot every track touched by the drag so each move can
            // restore non-dragged keyframes that were overwritten when the
            // dragged keyframe momentarily collided with them.
            for (const auto& entry : m_dragEntries) {
                if (m_dragTrackSnap.count(entry.track)) continue;
                auto& snap = m_dragTrackSnap[entry.track];
                snap.reserve(entry.track->keyframeCount());
                for (size_t i = 0; i < entry.track->keyframeCount(); ++i) {
                    const auto& kf = entry.track->keyframe(i);
                    snap.push_back({kf.time, kf.value, kf.interp,
                                    kf.bezierInX, kf.bezierInY,
                                    kf.bezierOutX, kf.bezierOutY});
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
        for (const auto* row : m_rows) {
            if (!row->isVisible()) continue;
            const auto rowTracks = row->allTracks();
            if (rowTracks.empty()) continue;
            int y = rowY(row);
            for (auto* track : rowTracks) {
                if (!track) continue;
                for (size_t i = 0; i < track->keyframeCount(); ++i) {
                    int x = tickToX(track->keyframe(i).time);
                    if (hitRect.contains(x, y))
                        m_selectedKeys.insert({track, track->keyframe(i).time});
                }
            }
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

        // Restore each affected track from its drag-start snapshot. This
        // un-erases any non-dragged keyframe that a previous move-tick
        // happened to land on (the "drag past = silently destroyed"
        // bug). Then strip the dragged-set's origTimes so we can re-add
        // them at their new positions without leaving duplicates behind.
        for (auto& [trk, snap] : m_dragTrackSnap) {
            while (trk->keyframeCount() > 0)
                trk->removeKeyframe(trk->keyframeCount() - 1);
            for (const auto& s : snap) {
                Keyframe<float> kf;
                kf.time       = s.time;
                kf.value      = s.value;
                kf.interp     = s.interp;
                kf.bezierInX  = s.biX;
                kf.bezierInY  = s.biY;
                kf.bezierOutX = s.boX;
                kf.bezierOutY = s.boY;
                trk->restoreKeyframe(kf);
            }
        }
        // Build the set of non-dragged keyframe times per track. A new-time
        // landing on one of these would silently overwrite it (addKeyframe
        // replaces on collision), so we shift by ±1 tick to skip past it.
        std::unordered_map<KeyframeTrack<float>*, std::set<int64_t>> nonDragTimes;
        for (auto& [trk, snap] : m_dragTrackSnap) {
            auto& set = nonDragTimes[trk];
            for (const auto& s : snap) set.insert(s.time);
        }
        for (const auto& entry : m_dragEntries) {
            auto it = nonDragTimes.find(entry.track);
            if (it != nonDragTimes.end()) it->second.erase(entry.origTime);
            entry.track->removeKeyframeAtTime(entry.origTime);
        }

        // Reinsert at new positions, clamped to [0, clipDuration]
        m_selectedKeys.clear();
        int64_t maxTime = m_clip ? m_clip->duration() : INT64_MAX;
        const int64_t shiftDir = (delta >= 0) ? +1 : -1;
        for (auto& entry : m_dragEntries) {
            int64_t newTime = std::clamp(entry.origTime + delta, int64_t(0), maxTime);
            // Step past any non-dragged keyframe that already occupies
            // newTime so neither keyframe is lost. Bounded to avoid an
            // infinite loop in pathological cases.
            auto& occupied = nonDragTimes[entry.track];
            int guard = 0;
            while (occupied.count(newTime) && guard++ < 100000) {
                int64_t next = newTime + shiftDir;
                if (next < 0 || next > maxTime) break;
                newTime = next;
            }
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

        // Emit live during the drag so the Effect Controls spinboxes and
        // the Program Monitor track the new keyframe positions in real
        // time — otherwise the displayed value stays at the pre-drag
        // evaluation until the user releases.
        emit keyframeChanged();
        update();
        return;
    }
}

void KeyframeTimeline::mouseReleaseEvent(QMouseEvent* event)
{
    // Re-evaluate the marquee with the release position so a fast drag
    // that stopped firing mouseMoveEvent right before release still has
    // the final rect — otherwise edge keyframes that the cursor swept
    // through but didn't have a recent move event for would drop out.
    if (m_marqueeActive && event) {
        m_marqueeCurrent = event->pos();
        QRect rect = QRect(m_marqueeOrigin, m_marqueeCurrent).normalized();
        QRect hitRect = rect.adjusted(-kDiamondRadius, -kDiamondRadius,
                                       kDiamondRadius,  kDiamondRadius);
        m_selectedKeys = m_preMarqueeSelection;
        for (const auto* row : m_rows) {
            if (!row->isVisible()) continue;
            const auto rowTracks = row->allTracks();
            if (rowTracks.empty()) continue;
            int y = rowY(row);
            for (auto* track : rowTracks) {
                if (!track) continue;
                for (size_t i = 0; i < track->keyframeCount(); ++i) {
                    int x = tickToX(track->keyframe(i).time);
                    if (hitRect.contains(x, y))
                        m_selectedKeys.insert({track, track->keyframe(i).time});
                }
            }
        }
    }

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
    m_dragTrackSnap.clear();
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
    // Keep the selection across focus losses. Earlier behavior cleared on
    // focus-out, but that fired right after a marquee drag (Qt routes a
    // focus-out as the press grab releases on edge keyframes — common when
    // dragging to the very first/last frame), making selected diamonds
    // flash blue during the drag and instantly drop on release. Users
    // deselect explicitly by clicking an empty area of the mini-timeline.
    QWidget::focusOutEvent(event);
}

// ═════════════════════════════════════════════════════════════════════════════
//  Keyframe clipboard (Premiere-style copy/paste across properties)
// ═════════════════════════════════════════════════════════════════════════════

void KeyframeTimeline::copySelectedKeyframes()
{
    m_kfClipboard.clear();
    if (m_selectedKeys.empty()) return;

    // Find earliest time for relative offsets
    int64_t earliest = std::numeric_limits<int64_t>::max();
    for (const auto& sk : m_selectedKeys) {
        if (!sk.track) continue;
        earliest = std::min(earliest, sk.time);
    }

    for (const auto& sk : m_selectedKeys) {
        if (!sk.track) continue;
        // Find the keyframe at this time to capture full data
        for (size_t i = 0; i < sk.track->keyframeCount(); ++i) {
            if (sk.track->keyframe(i).time == sk.time) {
                const auto& kf = sk.track->keyframe(i);
                m_kfClipboard.push_back({
                    sk.track,
                    kf.time - earliest,
                    kf.value,
                    static_cast<int>(kf.interp),
                    kf.bezierInX, kf.bezierInY,
                    kf.bezierOutX, kf.bezierOutY,
                    static_cast<int>(kf.spatialInterp),
                    kf.spatialInX, kf.spatialInY,
                    kf.spatialOutX, kf.spatialOutY
                });
                break;
            }
        }
    }
}

void KeyframeTimeline::cutSelectedKeyframes()
{
    copySelectedKeyframes();
    // Delete selected — reuse the same delete logic as keyPressEvent
    if (!m_selectedKeys.empty()) {
        if (m_commandStack) {
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
            for (auto it = m_selectedKeys.rbegin(); it != m_selectedKeys.rend(); ++it) {
                it->track->removeKeyframeAtTime(it->time);
            }
            m_commandStack->pushWithoutExecute(
                std::make_unique<LambdaCommand>(
                    "Cut Keyframes",
                    [saved]() {
                        for (auto it = saved->rbegin(); it != saved->rend(); ++it)
                            it->track->removeKeyframeAtTime(it->kf.time);
                    },
                    [saved]() {
                        for (auto& info : *saved) {
                            info.track->addKeyframe(info.kf.time, info.kf.value, info.kf.interp);
                            for (size_t i = 0; i < info.track->keyframeCount(); ++i) {
                                if (info.track->keyframe(i).time == info.kf.time) {
                                    auto& kf = info.track->keyframe(i);
                                    kf.bezierInX = info.kf.bezierInX;
                                    kf.bezierInY = info.kf.bezierInY;
                                    kf.bezierOutX = info.kf.bezierOutX;
                                    kf.bezierOutY = info.kf.bezierOutY;
                                    kf.spatialInX = info.kf.spatialInX;
                                    kf.spatialInY = info.kf.spatialInY;
                                    kf.spatialOutX = info.kf.spatialOutX;
                                    kf.spatialOutY = info.kf.spatialOutY;
                                    kf.spatialInterp = info.kf.spatialInterp;
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
    }
}

void KeyframeTimeline::pasteKeyframes()
{
    if (m_kfClipboard.empty()) return;

    // Build a set of valid destination tracks currently visible in the panel.
    // We match clipboard entries to destination tracks by pointer identity —
    // the clipboard stores the original track pointers which remain valid
    // as long as the same clip is loaded.
    std::set<KeyframeTrack<float>*> validTracks;
    for (const auto* row : m_rows) {
        for (auto* t : row->allTracks()) {
            if (t) validTracks.insert(t);
        }
    }

    const int64_t pasteTime = m_clip
        ? (m_playheadTick - m_clip->timelineIn())
        : m_playheadTick;

    if (m_commandStack) {
        auto entries = std::make_shared<std::vector<KfClipboardEntry>>(m_kfClipboard);
        auto addedSnap = std::make_shared<std::vector<std::pair<KeyframeTrack<float>*, int64_t>>>();
        m_commandStack->execute(
            std::make_unique<LambdaCommand>(
                "Paste Keyframes",
                [entries, pasteTime, validTracks, addedSnap]() {
                    // Redo: add all pasted keyframes
                    for (auto& e : *entries) {
                        if (!e.track || validTracks.count(e.track) == 0) continue;
                        int64_t t = pasteTime + e.relativeTime;
                        auto interp = static_cast<InterpMode>(e.interp);
                        e.track->addKeyframe(t, e.value, interp);
                        // Restore bezier/spatial handles
                        for (size_t i = 0; i < e.track->keyframeCount(); ++i) {
                            if (e.track->keyframe(i).time == t) {
                                auto& kf = e.track->keyframe(i);
                                kf.bezierInX  = e.bezierInX;
                                kf.bezierInY  = e.bezierInY;
                                kf.bezierOutX = e.bezierOutX;
                                kf.bezierOutY = e.bezierOutY;
                                kf.spatialInterp = static_cast<InterpMode>(e.spatialInterp);
                                kf.spatialInX  = e.spatialInX;
                                kf.spatialInY  = e.spatialInY;
                                kf.spatialOutX = e.spatialOutX;
                                kf.spatialOutY = e.spatialOutY;
                                break;
                            }
                        }
                        addedSnap->push_back({e.track, t});
                    }
                },
                [addedSnap]() {
                    // Undo: remove all pasted keyframes
                    for (auto it = addedSnap->rbegin(); it != addedSnap->rend(); ++it) {
                        it->first->removeKeyframeAtTime(it->second);
                    }
                }));
    } else {
        for (auto& e : m_kfClipboard) {
            if (!e.track || validTracks.count(e.track) == 0) continue;
            int64_t t = pasteTime + e.relativeTime;
            auto interp = static_cast<InterpMode>(e.interp);
            e.track->addKeyframe(t, e.value, interp);
            for (size_t i = 0; i < e.track->keyframeCount(); ++i) {
                if (e.track->keyframe(i).time == t) {
                    auto& kf = e.track->keyframe(i);
                    kf.bezierInX  = e.bezierInX;
                    kf.bezierInY  = e.bezierInY;
                    kf.bezierOutX = e.bezierOutX;
                    kf.bezierOutY = e.bezierOutY;
                    kf.spatialInterp = static_cast<InterpMode>(e.spatialInterp);
                    kf.spatialInX  = e.spatialInX;
                    kf.spatialInY  = e.spatialInY;
                    kf.spatialOutX = e.spatialOutX;
                    kf.spatialOutY = e.spatialOutY;
                    break;
                }
            }
        }
    }

    // Select the freshly pasted keyframes
    m_selectedKeys.clear();
    for (auto& e : m_kfClipboard) {
        if (!e.track || validTracks.count(e.track) == 0) continue;
        int64_t t = pasteTime + e.relativeTime;
        m_selectedKeys.insert({e.track, t});
    }

    update();
    emit keyframeChanged();
}

void KeyframeTimeline::keyPressEvent(QKeyEvent* event)
{
    // ── Ctrl+C / Ctrl+X / Ctrl+V: keyframe clipboard ──────────────────
    if (event->modifiers() & Qt::ControlModifier) {
        if (event->key() == Qt::Key_C && !m_selectedKeys.empty()) {
            copySelectedKeyframes();
            event->accept();
            return;
        }
        if (event->key() == Qt::Key_X && !m_selectedKeys.empty()) {
            cutSelectedKeyframes();
            event->accept();
            return;
        }
        if (event->key() == Qt::Key_V && !m_kfClipboard.empty()) {
            pasteKeyframes();
            event->accept();
            return;
        }
    }

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
    for (const auto* row : m_rows) {
        if (!row->isVisible()) continue;
        const auto rowTracks = row->allTracks();
        if (rowTracks.empty()) continue;

        int y = rowY(row);
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

} // namespace rt
