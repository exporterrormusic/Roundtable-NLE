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
#include <optional>
#include <set>
#include <unordered_map>
#include <vector>

namespace rt {

namespace {

OpacityMask* resolveMaskById(Clip* clip, quint64 effectId, uint64_t maskId)
{
    if (!clip) return nullptr;
    std::vector<OpacityMask>* masks = nullptr;
    if (effectId == 0) {
        masks = &clip->masks();
    } else if (Effect* fx = clip->effects().effectById(effectId)) {
        masks = &fx->masks();
    }
    if (!masks) return nullptr;
    auto it = std::find_if(masks->begin(), masks->end(),
        [maskId](const OpacityMask& mask) { return mask.maskId == maskId; });
    return it == masks->end() ? nullptr : &*it;
}

// Most Effect Controls tracks have stable addresses for the life of a clip.
// Mask scalar tracks do not: they are members of OpacityMask values stored in
// a vector, so inserting/removing a mask can relocate them. Commands must keep
// a semantic address for those tracks instead of retaining a dangling pointer.
struct StableScalarTrackRef
{
    KeyframeTrack<float>* direct{nullptr};
    quint64 effectId{0};
    uint64_t maskId{0};
    uint8_t maskField{0xFF}; // 0 feather, 1 opacity, 2 expansion

    [[nodiscard]] KeyframeTrack<float>* resolve(Clip* clip) const
    {
        if (maskField == 0xFF) return direct;
        OpacityMask* mask = resolveMaskById(clip, effectId, maskId);
        if (!mask) return nullptr;
        switch (maskField) {
        case 0: return &mask->feather;
        case 1: return &mask->maskOpacity;
        case 2: return &mask->expansion;
        default: return nullptr;
        }
    }
};

StableScalarTrackRef stableTrackRef(Clip* clip, KeyframeTrack<float>* track)
{
    if (!clip || !track) return {track};
    auto inspect = [track](std::vector<OpacityMask>& masks, quint64 effectId)
            -> std::optional<StableScalarTrackRef> {
        for (auto& mask : masks) {
            if (track == &mask.feather)
                return StableScalarTrackRef{nullptr, effectId, mask.maskId, 0};
            if (track == &mask.maskOpacity)
                return StableScalarTrackRef{nullptr, effectId, mask.maskId, 1};
            if (track == &mask.expansion)
                return StableScalarTrackRef{nullptr, effectId, mask.maskId, 2};
        }
        return std::nullopt;
    };

    if (auto ref = inspect(clip->masks(), 0)) return *ref;
    auto& effects = clip->effects();
    for (size_t i = 0; i < effects.effectCount(); ++i) {
        Effect& effect = effects.effect(i);
        if (auto ref = inspect(effect.masks(), effect.id())) return *ref;
    }
    return {track};
}

} // namespace

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
    if (m_clip != clip) {
        m_selectedKeys.clear();
        m_selectedMaskKeys.clear();
        clearKfClipboard();
        m_dragEntries.clear();
        m_maskDragEntries.clear();
        m_dragTrackSnap.clear();
        m_dragMaskSnap.clear();
    }
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
    // Rows are rebuilt after structural model changes. Scalar selections and
    // clipboard entries use live track pointers while the widget is open, so
    // retire them before accepting the replacement row set. Mask Path entries
    // carry stable IDs and are pruned separately in setMaskPathLanes().
    m_selectedKeys.clear();
    m_preMarqueeSelection.clear();
    m_dragEntries.clear();
    m_dragTrackSnap.clear();
    m_kfClipboard.clear();
    m_rows = rows;
    update();
}

void KeyframeTimeline::setMaskPathLanes(const std::vector<MaskPathLane>& lanes)
{
    m_maskPathLanes = lanes;

    // A rebuild may remove/reorder masks. Discard selection entries that no
    // longer resolve instead of leaving invisible stale keys selected.
    for (auto it = m_selectedMaskKeys.begin(); it != m_selectedMaskKeys.end(); ) {
        const OpacityMask* mask = resolveMaskPath(it->id);
        if (!laneFor(it->id) || !mask || !mask->hasPathKeyAt(it->time))
            it = m_selectedMaskKeys.erase(it);
        else
            ++it;
    }
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

int KeyframeTimeline::rowY(const QWidget* row) const
{
    return row ? row->pos().y() + kRowHeight / 2 - m_scrollOffsetY : -1;
}

OpacityMask* KeyframeTimeline::resolveMaskPath(const MaskPathId& id) const
{
    return resolveMaskById(m_clip, id.effectId, id.maskId);
}

const KeyframeTimeline::MaskPathLane*
KeyframeTimeline::laneFor(const MaskPathId& id) const
{
    for (const auto& lane : m_maskPathLanes) {
        if (lane.effectId == id.effectId && lane.maskId == id.maskId)
            return &lane;
    }
    return nullptr;
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
    for (const auto& lane : m_maskPathLanes) {
        if (!lane.row || !lane.row->isVisible()) continue;
        int y = rowY(lane.row) + kRowHeight / 2;
        if (y > kRulerHeight && y < height())
            p.drawLine(0, y, width(), y);
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
            InterpMode interp{InterpMode::Linear};
            bool       selected{false};
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

    // Mask Path values are whole geometries, not floats, but their temporal
    // keys behave like every other Effect Controls lane. They use the linear
    // diamond icon because MaskGeometry currently interpolates linearly.
    for (const auto& lane : m_maskPathLanes) {
        if (!lane.row || !lane.row->isVisible()) continue;
        const MaskPathId id{lane.effectId, lane.maskId};
        const OpacityMask* mask = resolveMaskPath(id);
        if (!mask || !mask->pathAnimated) continue;
        const int y = rowY(lane.row);
        if (y < kRulerHeight || y > height()) continue;

        for (const auto& kf : mask->pathKeys) {
            const int x = tickToX(kf.time);
            constexpr int d = kDiamondRadius;
            const bool selected = m_selectedMaskKeys.count({id, kf.time}) > 0;
            p.setBrush(selected ? tc.accent : tc.textTertiary);
            QPolygonF diamond;
            diamond << QPointF(x, y - d) << QPointF(x + d, y)
                    << QPointF(x, y + d) << QPointF(x - d, y);
            p.drawPolygon(diamond);
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
        if (!hit.valid()) return;
        const int64_t kfTime = hit.time;

        // Selection: right-click on an un-selected diamond replaces selection
        // with just that one; right-click on a selected one keeps multi-select
        // so the action applies to the whole group (matches Premiere).
        if (hit.isMaskPath()) {
            const auto& lane = m_maskPathLanes[static_cast<size_t>(hit.maskLane)];
            SelMaskKey clickedKey{{lane.effectId, lane.maskId}, kfTime};
            if (m_selectedMaskKeys.count(clickedKey) == 0) {
                m_selectedKeys.clear();
                m_selectedMaskKeys.clear();
                m_selectedMaskKeys.insert(clickedKey);
            }
        } else {
            SelKey clickedKey{hit.track, kfTime};
            if (m_selectedKeys.count(clickedKey) == 0) {
                m_selectedKeys.clear();
                m_selectedMaskKeys.clear();
                m_selectedKeys.insert(clickedKey);
            }
        }
        update();

        // Snapshot the current selection now — the menu's nested event loop
        // can fire focus events that drop m_selectedKeys before the chosen
        // action runs, so the lambdas need their own copy to act on.
        const auto keysSnap = m_selectedKeys;

        QMenu menu(this);
        QMenu* interpSub = keysSnap.empty()
            ? nullptr : menu.addMenu(QStringLiteral("Temporal Interpolation"));
        auto addInterpAction = [&](const QString& label, InterpMode mode) {
            if (!interpSub) return;
            QAction* a = interpSub->addAction(label);
            connect(a, &QAction::triggered, this, [this, mode, keysSnap]() {
                const bool batch = m_commandStack && keysSnap.size() > 1;
                if (batch)
                    m_commandStack->beginMacro("Set Keyframe Interpolation");
                for (const auto& sk : keysSnap) {
                    if (!sk.track) continue;
                    auto* track = sk.track;
                    auto key = std::find_if(
                        track->keyframes().begin(), track->keyframes().end(),
                        [&sk](const Keyframe<float>& value) {
                            return value.time == sk.time;
                        });
                    if (key == track->keyframes().end()) continue;
                    const InterpMode oldMode = key->interp;
                    const StableScalarTrackRef ref = stableTrackRef(m_clip, track);
                    Clip* clip = m_clip;
                    auto applyMode = [ref, clip, time = sk.time](InterpMode value) {
                        auto* resolved = ref.resolve(clip);
                        if (!resolved) return;
                        for (size_t i = 0; i < resolved->keyframeCount(); ++i) {
                            if (resolved->keyframe(i).time == time) {
                                resolved->keyframe(i).interp = value;
                                break;
                            }
                        }
                    };
                    if (m_commandStack) {
                        m_commandStack->execute(std::make_unique<LambdaCommand>(
                            "Set Keyframe Interpolation",
                            [applyMode, mode]() { applyMode(mode); },
                            [applyMode, oldMode]() { applyMode(oldMode); }));
                    } else {
                        applyMode(mode);
                    }
                }
                if (batch)
                    m_commandStack->endMacro();
                emit keyframeChanged();
                update();
            });
        };
        addInterpAction(QStringLiteral("Linear"),            InterpMode::Linear);
        addInterpAction(QStringLiteral("Bezier"),            InterpMode::Bezier);
        addInterpAction(QStringLiteral("Auto Bezier"),       InterpMode::AutoBezier);
        addInterpAction(QStringLiteral("Continuous Bezier"), InterpMode::ContinuousBezier);
        addInterpAction(QStringLiteral("Hold"),              InterpMode::Hold);
        if (interpSub) interpSub->addSeparator();
        addInterpAction(QStringLiteral("Ease In"),           InterpMode::EaseIn);
        addInterpAction(QStringLiteral("Ease Out"),          InterpMode::EaseOut);

        if (interpSub) menu.addSeparator();
        QAction* del = menu.addAction(QStringLiteral("Delete"));
        connect(del, &QAction::triggered, this, [this]() {
            deleteSelectedKeyframes("Delete Keyframes");
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
        if (hit.isMaskPath()) {
            const auto& lane = m_maskPathLanes[static_cast<size_t>(hit.maskLane)];
            SelMaskKey key{{lane.effectId, lane.maskId}, hit.time};
            const bool alreadySelected = m_selectedMaskKeys.count(key) > 0;
            if (shift) {
                if (alreadySelected) m_selectedMaskKeys.erase(key);
                else                 m_selectedMaskKeys.insert(key);
            } else if (!alreadySelected) {
                m_selectedKeys.clear();
                m_selectedMaskKeys.clear();
                m_selectedMaskKeys.insert(key);
            }
            beginSelectionDrag(xToTick(event->pos().x()));
            setFocus();
            update();
            return;
        }
        if (hit.track) {
            const int64_t hitTime = hit.time;

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
                    m_selectedMaskKeys.clear();
                    for (const auto& s : siblings) m_selectedKeys.insert(s);
                }
                // else: already selected, keep multi-selection for group drag
            }

            // Start dragging the selection
            beginSelectionDrag(xToTick(event->pos().x()));
            // Snapshot every track touched by the drag so each move can
            // restore non-dragged keyframes that were overwritten when the
            // dragged keyframe momentarily collided with them.
            setFocus();
            update();
            return;
        }

        // Click on empty area → start marquee
        if (!shift) {
            m_selectedKeys.clear();
            m_selectedMaskKeys.clear();
        }
        m_preMarqueeSelection = m_selectedKeys;
        m_preMarqueeMaskSelection = m_selectedMaskKeys;
        m_marqueeActive = true;
        m_marqueeOrigin = event->pos();
        m_marqueeCurrent = event->pos();
        m_draggingSelection = false;
        setFocus();
        update();
    }
}

void KeyframeTimeline::beginSelectionDrag(int64_t anchorTick)
{
    m_dragAnchorTick = anchorTick;
    m_dragEntries.clear();
    m_maskDragEntries.clear();
    m_dragTrackSnap.clear();
    m_dragMaskSnap.clear();

    for (const auto& sk : m_selectedKeys) {
        if (!sk.track) continue;
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
    for (const auto& sk : m_selectedMaskKeys) {
        const OpacityMask* mask = resolveMaskPath(sk.id);
        if (!mask) continue;
        auto it = std::find_if(mask->pathKeys.begin(), mask->pathKeys.end(),
            [&sk](const MaskPathKeyframe& kf) { return kf.time == sk.time; });
        if (it != mask->pathKeys.end())
            m_maskDragEntries.push_back({sk.id, it->time, it->time, it->geometry});
    }

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
    for (const auto& entry : m_maskDragEntries) {
        if (m_dragMaskSnap.count(entry.id)) continue;
        if (const OpacityMask* mask = resolveMaskPath(entry.id))
            m_dragMaskSnap[entry.id] = mask->pathKeys;
    }

    m_draggingSelection = !m_dragEntries.empty() || !m_maskDragEntries.empty();
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
        m_selectedMaskKeys = m_preMarqueeMaskSelection;
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
        for (const auto& lane : m_maskPathLanes) {
            if (!lane.row || !lane.row->isVisible()) continue;
            const MaskPathId id{lane.effectId, lane.maskId};
            const OpacityMask* mask = resolveMaskPath(id);
            if (!mask || !mask->pathAnimated) continue;
            const int y = rowY(lane.row);
            for (const auto& kf : mask->pathKeys) {
                if (hitRect.contains(tickToX(kf.time), y))
                    m_selectedMaskKeys.insert({id, kf.time});
            }
        }
        update();
        return;
    }

    if (m_draggingSelection &&
        (!m_dragEntries.empty() || !m_maskDragEntries.empty())) {
        int64_t currentTick = xToTick(event->pos().x());
        int64_t delta = currentTick - m_dragAnchorTick;
        const int64_t maxTime = m_clip
            ? std::max<int64_t>(0, m_clip->duration())
            : std::numeric_limits<int64_t>::max();

        // Premiere-style magnetism (hold Shift to disable). Snap delta so
        // the "leader" entry (the keyframe nearest the cursor when the drag
        // began — i.e. the first entry the drag was anchored to) lands on
        // a sibling keyframe in another track, or on the playhead. We snap
        // the GROUP delta rather than each keyframe individually so a
        // multi-select drag keeps its relative spacing.
        const bool snapEnabled = !(event->modifiers() & Qt::ShiftModifier);
        if (snapEnabled) {
            // Build a set of dragged (track, origTime) so we don't snap to
            // ourselves or to siblings within the drag group.
            std::set<std::pair<KeyframeTrack<float>*, int64_t>> dragged;
            for (const auto& e : m_dragEntries)
                dragged.insert({e.track, e.origTime});
            std::set<SelMaskKey> draggedMasks;
            for (const auto& e : m_maskDragEntries)
                draggedMasks.insert({e.id, e.origTime});

            // Candidate snap targets: every non-dragged keyframe on any
            // visible row, plus the playhead.
            std::vector<int64_t> targets;
            for (const auto* row : m_rows) {
                if (!row->isVisible()) continue;
                for (auto* tr : row->allTracks()) {
                    if (!tr) continue;
                    for (size_t i = 0; i < tr->keyframeCount(); ++i) {
                        int64_t t = tr->keyframe(i).time;
                        if (!dragged.count({tr, t}))
                            targets.push_back(t);
                    }
                }
            }
            for (const auto& lane : m_maskPathLanes) {
                if (!lane.row || !lane.row->isVisible()) continue;
                const MaskPathId id{lane.effectId, lane.maskId};
                const OpacityMask* mask = resolveMaskPath(id);
                if (!mask) continue;
                for (const auto& kf : mask->pathKeys)
                    if (!draggedMasks.count({id, kf.time}))
                        targets.push_back(kf.time);
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
            for (const auto& e : m_maskDragEntries) {
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

        // Clamp the delta once for the complete selection. Clamping every
        // key independently makes several keys collapse onto tick 0 or the
        // clip end and silently replaces geometry/values there.
        int64_t earliest = std::numeric_limits<int64_t>::max();
        int64_t latest = std::numeric_limits<int64_t>::min();
        for (const auto& entry : m_dragEntries) {
            earliest = std::min(earliest, entry.origTime);
            latest = std::max(latest, entry.origTime);
        }
        for (const auto& entry : m_maskDragEntries) {
            earliest = std::min(earliest, entry.origTime);
            latest = std::max(latest, entry.origTime);
        }
        if (earliest != std::numeric_limits<int64_t>::max()) {
            delta = std::clamp(delta, -earliest, maxTime - latest);
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
        for (const auto& [id, snap] : m_dragMaskSnap) {
            if (OpacityMask* mask = resolveMaskPath(id))
                mask->pathKeys = snap;
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
        std::map<MaskPathId, std::set<int64_t>> nonDragMaskTimes;
        for (const auto& [id, snap] : m_dragMaskSnap) {
            auto& times = nonDragMaskTimes[id];
            for (const auto& kf : snap) times.insert(kf.time);
        }
        for (const auto& entry : m_maskDragEntries) {
            nonDragMaskTimes[entry.id].erase(entry.origTime);
            if (OpacityMask* mask = resolveMaskPath(entry.id))
                mask->removePathKeyAtTime(entry.origTime);
        }

        // Reinsert at new positions. The group delta is already clamped to
        // [0, clipDuration]; collision resolution also reserves each chosen
        // destination so two selected keys can never replace one another.
        m_selectedKeys.clear();
        m_selectedMaskKeys.clear();
        const int64_t shiftDir = (delta >= 0) ? +1 : -1;
        auto availableTime = [maxTime, shiftDir](
                                 std::set<int64_t>& occupied,
                                 int64_t desired) {
            if (occupied.count(desired) == 0) return desired;

            int64_t candidate = desired;
            for (size_t step = 0; step <= occupied.size(); ++step) {
                if ((shiftDir > 0 && candidate >= maxTime)
                    || (shiftDir < 0 && candidate <= 0))
                    break;
                candidate += shiftDir;
                if (occupied.count(candidate) == 0) return candidate;
            }

            candidate = desired;
            for (size_t step = 0; step <= occupied.size(); ++step) {
                if ((shiftDir > 0 && candidate <= 0)
                    || (shiftDir < 0 && candidate >= maxTime))
                    break;
                candidate -= shiftDir;
                if (occupied.count(candidate) == 0) return candidate;
            }
            return desired;
        };
        for (auto& entry : m_dragEntries) {
            const int64_t desired = entry.origTime + delta;
            auto& occupied = nonDragTimes[entry.track];
            const int64_t newTime = availableTime(occupied, desired);
            entry.track->addKeyframe(newTime, entry.value, entry.interp);
            occupied.insert(newTime);
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
        for (auto& entry : m_maskDragEntries) {
            OpacityMask* mask = resolveMaskPath(entry.id);
            if (!mask) continue;
            auto& occupied = nonDragMaskTimes[entry.id];
            const int64_t desired = entry.origTime + delta;
            const int64_t newTime = availableTime(occupied, desired);
            mask->addPathKey(newTime, entry.geometry);
            occupied.insert(newTime);
            entry.currentTime = newTime;
            m_selectedMaskKeys.insert({entry.id, newTime});
        }

        // Emit live during the drag so the Effect Controls spinboxes and
        // the Program Monitor track the new keyframe positions in real
        // time — otherwise the displayed value stays at the pre-drag
        // evaluation until the user releases.
        emit keyframeChanged();
        if (!m_maskDragEntries.empty()) emit maskPathChanged();
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
        m_selectedMaskKeys = m_preMarqueeMaskSelection;
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
        for (const auto& lane : m_maskPathLanes) {
            if (!lane.row || !lane.row->isVisible()) continue;
            const MaskPathId id{lane.effectId, lane.maskId};
            const OpacityMask* mask = resolveMaskPath(id);
            if (!mask || !mask->pathAnimated) continue;
            const int y = rowY(lane.row);
            for (const auto& kf : mask->pathKeys) {
                if (hitRect.contains(tickToX(kf.time), y))
                    m_selectedMaskKeys.insert({id, kf.time});
            }
        }
    }

    if (m_draggingSelection &&
        (!m_dragEntries.empty() || !m_maskDragEntries.empty())) {
        // Check if any keyframe actually moved
        bool moved = false;
        for (const auto& entry : m_dragEntries) {
            if (entry.currentTime != entry.origTime) { moved = true; break; }
        }
        if (!moved) {
            for (const auto& entry : m_maskDragEntries) {
                if (entry.currentTime != entry.origTime) { moved = true; break; }
            }
        }
        if (moved && m_commandStack) {
            // Build undo/redo data: snapshot original and final positions
            struct MoveInfo {
                StableScalarTrackRef track;
                int64_t origTime;
                int64_t newTime;
                float value;
                InterpMode interp;
                float biX, biY, boX, boY;
            };
            auto moves = std::make_shared<std::vector<MoveInfo>>();
            for (const auto& entry : m_dragEntries) {
                moves->push_back({stableTrackRef(m_clip, entry.track),
                                  entry.origTime, entry.currentTime,
                                  entry.value, entry.interp,
                                  entry.biX, entry.biY, entry.boX, entry.boY});
            }
            struct MaskMoveState {
                quint64 effectId;
                uint64_t maskId;
                std::vector<MaskPathKeyframe> before;
                std::vector<MaskPathKeyframe> after;
            };
            auto maskMoves = std::make_shared<std::vector<MaskMoveState>>();
            for (const auto& [id, before] : m_dragMaskSnap) {
                if (const OpacityMask* mask = resolveMaskPath(id))
                    maskMoves->push_back(
                        {id.effectId, id.maskId, before, mask->pathKeys});
            }
            Clip* clip = m_clip;
            m_commandStack->pushWithoutExecute(
                std::make_unique<LambdaCommand>(
                    "Move Keyframes",
                    [moves, maskMoves, clip]() {
                        // Redo: move from orig → new
                        for (auto& m : *moves) {
                            auto* track = m.track.resolve(clip);
                            if (!track) continue;
                            track->removeKeyframeAtTime(m.origTime);
                            track->addKeyframe(m.newTime, m.value, m.interp);
                            for (size_t i = 0; i < track->keyframeCount(); ++i) {
                                if (track->keyframe(i).time == m.newTime) {
                                    auto& kf = track->keyframe(i);
                                    kf.bezierInX = m.biX;  kf.bezierInY = m.biY;
                                    kf.bezierOutX = m.boX; kf.bezierOutY = m.boY;
                                    break;
                                }
                            }
                        }
                        for (const auto& state : *maskMoves) {
                            if (OpacityMask* mask = resolveMaskById(
                                    clip, state.effectId, state.maskId))
                                mask->pathKeys = state.after;
                        }
                    },
                    [moves, maskMoves, clip]() {
                        // Undo: move from new → orig
                        for (auto& m : *moves) {
                            auto* track = m.track.resolve(clip);
                            if (!track) continue;
                            track->removeKeyframeAtTime(m.newTime);
                            track->addKeyframe(m.origTime, m.value, m.interp);
                            for (size_t i = 0; i < track->keyframeCount(); ++i) {
                                if (track->keyframe(i).time == m.origTime) {
                                    auto& kf = track->keyframe(i);
                                    kf.bezierInX = m.biX;  kf.bezierInY = m.biY;
                                    kf.bezierOutX = m.boX; kf.bezierOutY = m.boY;
                                    break;
                                }
                            }
                        }
                        for (const auto& state : *maskMoves) {
                            if (OpacityMask* mask = resolveMaskById(
                                    clip, state.effectId, state.maskId))
                                mask->pathKeys = state.before;
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
    m_maskDragEntries.clear();
    m_dragTrackSnap.clear();
    m_dragMaskSnap.clear();
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

void KeyframeTimeline::deleteSelectedKeyframes(const char* description)
{
    struct ScalarSaved {
        StableScalarTrackRef track;
        Keyframe<float> keyframe;
    };
    struct MaskSaved {
        quint64 effectId;
        uint64_t maskId;
        std::vector<MaskPathKeyframe> before;
        std::vector<MaskPathKeyframe> after;
    };

    auto scalarSaved = std::make_shared<std::vector<ScalarSaved>>();
    for (const auto& sk : m_selectedKeys) {
        if (!sk.track) continue;
        for (size_t i = 0; i < sk.track->keyframeCount(); ++i) {
            if (sk.track->keyframe(i).time == sk.time) {
                scalarSaved->push_back(
                    {stableTrackRef(m_clip, sk.track), sk.track->keyframe(i)});
                break;
            }
        }
    }

    std::map<MaskPathId, std::vector<MaskPathKeyframe>> maskBefore;
    for (const auto& sk : m_selectedMaskKeys) {
        if (const OpacityMask* mask = resolveMaskPath(sk.id))
            maskBefore.try_emplace(sk.id, mask->pathKeys);
    }

    for (auto it = m_selectedKeys.rbegin(); it != m_selectedKeys.rend(); ++it)
        if (it->track) it->track->removeKeyframeAtTime(it->time);
    for (const auto& sk : m_selectedMaskKeys)
        if (OpacityMask* mask = resolveMaskPath(sk.id))
            mask->removePathKeyAtTime(sk.time);

    auto maskSaved = std::make_shared<std::vector<MaskSaved>>();
    for (const auto& [id, before] : maskBefore) {
        if (const OpacityMask* mask = resolveMaskPath(id))
            maskSaved->push_back(
                {id.effectId, id.maskId, before, mask->pathKeys});
    }

    if (scalarSaved->empty() && maskSaved->empty()) {
        m_selectedKeys.clear();
        m_selectedMaskKeys.clear();
        update();
        return;
    }

    if (m_commandStack) {
        Clip* clip = m_clip;
        m_commandStack->pushWithoutExecute(std::make_unique<LambdaCommand>(
            description,
            [scalarSaved, maskSaved, clip]() {
                for (auto it = scalarSaved->rbegin(); it != scalarSaved->rend(); ++it) {
                    if (auto* track = it->track.resolve(clip))
                        track->removeKeyframeAtTime(it->keyframe.time);
                }
                for (const auto& state : *maskSaved) {
                    if (OpacityMask* mask = resolveMaskById(
                            clip, state.effectId, state.maskId))
                        mask->pathKeys = state.after;
                }
            },
            [scalarSaved, maskSaved, clip]() {
                for (const auto& state : *maskSaved) {
                    if (OpacityMask* mask = resolveMaskById(
                            clip, state.effectId, state.maskId))
                        mask->pathKeys = state.before;
                }
                for (const auto& saved : *scalarSaved) {
                    if (auto* track = saved.track.resolve(clip))
                        track->restoreKeyframe(saved.keyframe);
                }
            }));
    }

    m_selectedKeys.clear();
    m_selectedMaskKeys.clear();
    update();
    emit keyframeChanged();
    if (!maskSaved->empty()) emit maskPathChanged();
}

// ═════════════════════════════════════════════════════════════════════════════
//  Keyframe clipboard (Premiere-style copy/paste across properties)
// ═════════════════════════════════════════════════════════════════════════════

void KeyframeTimeline::copySelectedKeyframes()
{
    m_kfClipboard.clear();
    m_maskKfClipboard.clear();
    if (m_selectedKeys.empty() && m_selectedMaskKeys.empty()) return;

    // Find earliest time for relative offsets
    int64_t earliest = std::numeric_limits<int64_t>::max();
    for (const auto& sk : m_selectedKeys) {
        if (!sk.track) continue;
        earliest = std::min(earliest, sk.time);
    }
    for (const auto& sk : m_selectedMaskKeys)
        earliest = std::min(earliest, sk.time);

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
    for (const auto& sk : m_selectedMaskKeys) {
        const OpacityMask* mask = resolveMaskPath(sk.id);
        if (!mask) continue;
        auto it = std::find_if(mask->pathKeys.begin(), mask->pathKeys.end(),
            [&sk](const MaskPathKeyframe& kf) { return kf.time == sk.time; });
        if (it != mask->pathKeys.end())
            m_maskKfClipboard.push_back(
                {sk.id, it->time - earliest, it->geometry});
    }
}

void KeyframeTimeline::cutSelectedKeyframes()
{
    copySelectedKeyframes();
    deleteSelectedKeyframes("Cut Keyframes");
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
    if (m_kfClipboard.empty() && m_maskKfClipboard.empty()) return;

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
    std::set<MaskPathId> validMaskPaths;
    for (const auto& lane : m_maskPathLanes)
        validMaskPaths.insert({lane.effectId, lane.maskId});

    const int64_t pasteTime = m_clip
        ? (m_playheadTick - m_clip->timelineIn())
        : m_playheadTick;

    const bool mixedPaste = m_commandStack &&
        !m_kfClipboard.empty() && !m_maskKfClipboard.empty();
    if (mixedPaste) m_commandStack->beginMacro("Paste Keyframes");

    if (m_commandStack && !m_kfClipboard.empty()) {
        struct StablePasteEntry {
            StableScalarTrackRef track;
            KfClipboardEntry value;
        };
        auto entries = std::make_shared<std::vector<StablePasteEntry>>();
        for (const auto& entry : m_kfClipboard) {
            if (entry.track && validTracks.count(entry.track) != 0)
                entries->push_back({stableTrackRef(m_clip, entry.track), entry});
        }
        struct PasteUndo {
            StableScalarTrackRef track;
            int64_t time;
            std::optional<Keyframe<float>> previous;
        };
        auto addedSnap = std::make_shared<std::vector<PasteUndo>>();
        Clip* clip = m_clip;
        m_commandStack->execute(
            std::make_unique<LambdaCommand>(
                "Paste Keyframes",
                [entries, pasteTime, addedSnap, clip]() {
                    // Redo: add all pasted keyframes
                    const bool captureUndo = addedSnap->empty();
                    for (const auto& stable : *entries) {
                        auto* track = stable.track.resolve(clip);
                        if (!track) continue;
                        const auto& e = stable.value;
                        int64_t t = pasteTime + e.relativeTime;
                        auto interp = static_cast<InterpMode>(e.interp);
                        if (captureUndo) {
                            std::optional<Keyframe<float>> previous;
                            for (const auto& key : track->keyframes()) {
                                if (key.time == t) {
                                    previous = key;
                                    break;
                                }
                            }
                            addedSnap->push_back({stable.track, t, previous});
                        }
                        track->addKeyframe(t, e.value, interp);
                        // Restore bezier/spatial handles
                        for (size_t i = 0; i < track->keyframeCount(); ++i) {
                            if (track->keyframe(i).time == t) {
                                auto& kf = track->keyframe(i);
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
                },
                [addedSnap, clip]() {
                    // Undo restores a key that paste replaced, if any.
                    for (auto it = addedSnap->rbegin(); it != addedSnap->rend(); ++it) {
                        auto* track = it->track.resolve(clip);
                        if (!track) continue;
                        track->removeKeyframeAtTime(it->time);
                        if (it->previous) track->restoreKeyframe(*it->previous);
                    }
                }));
    } else if (!m_kfClipboard.empty()) {
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

    struct MaskPasteState {
        quint64 effectId;
        uint64_t maskId;
        bool beforeAnimated;
        bool afterAnimated;
        std::vector<MaskPathKeyframe> before;
        std::vector<MaskPathKeyframe> after;
    };
    auto maskStates = std::make_shared<std::vector<MaskPasteState>>();
    std::map<MaskPathId, size_t> stateIndex;
    for (const auto& entry : m_maskKfClipboard) {
        if (validMaskPaths.count(entry.id) == 0) continue;
        OpacityMask* mask = resolveMaskPath(entry.id);
        if (!mask) continue;

        auto [it, inserted] = stateIndex.emplace(entry.id, maskStates->size());
        if (inserted) {
            maskStates->push_back({entry.id.effectId, entry.id.maskId,
                                   mask->pathAnimated, true,
                                   mask->pathKeys, mask->pathKeys});
        }
        auto& state = (*maskStates)[it->second];
        const int64_t rawTime = pasteTime + entry.relativeTime;
        const int64_t t = std::clamp<int64_t>(
            rawTime, 0, m_clip ? std::max<int64_t>(0, m_clip->duration()) : INT64_MAX);
        auto keyIt = std::lower_bound(
            state.after.begin(), state.after.end(), t,
            [](const MaskPathKeyframe& kf, int64_t time) { return kf.time < time; });
        if (keyIt != state.after.end() && keyIt->time == t)
            keyIt->geometry = entry.geometry;
        else
            state.after.insert(keyIt, MaskPathKeyframe{t, entry.geometry});
    }

    if (!maskStates->empty()) {
        Clip* clip = m_clip;
        auto applyStates = [clip](const std::vector<MaskPasteState>& states,
                                  bool after) {
            for (const auto& state : states) {
                OpacityMask* mask = resolveMaskById(
                    clip, state.effectId, state.maskId);
                if (!mask) continue;
                mask->pathAnimated = after ? state.afterAnimated : state.beforeAnimated;
                mask->pathKeys = after ? state.after : state.before;
            }
        };
        if (m_commandStack) {
            m_commandStack->execute(std::make_unique<LambdaCommand>(
                "Paste Mask Path Keyframes",
                [maskStates, applyStates]() { applyStates(*maskStates, true); },
                [maskStates, applyStates]() { applyStates(*maskStates, false); }));
        } else {
            applyStates(*maskStates, true);
        }
    }
    if (mixedPaste) m_commandStack->endMacro();

    // Select the freshly pasted keyframes
    m_selectedKeys.clear();
    m_selectedMaskKeys.clear();
    for (auto& e : m_kfClipboard) {
        if (!e.track || validTracks.count(e.track) == 0) continue;
        int64_t t = pasteTime + e.relativeTime;
        m_selectedKeys.insert({e.track, t});
    }
    for (const auto& e : m_maskKfClipboard) {
        if (validMaskPaths.count(e.id) == 0) continue;
        const int64_t rawTime = pasteTime + e.relativeTime;
        const int64_t t = std::clamp<int64_t>(
            rawTime, 0, m_clip ? std::max<int64_t>(0, m_clip->duration()) : INT64_MAX);
        m_selectedMaskKeys.insert({e.id, t});
    }

    update();
    emit keyframeChanged();
    if (!maskStates->empty()) emit maskPathChanged();
}

void KeyframeTimeline::keyPressEvent(QKeyEvent* event)
{
    // ── Ctrl+C / Ctrl+X / Ctrl+V: keyframe clipboard ──────────────────
    if (event->modifiers() & Qt::ControlModifier) {
        if (event->key() == Qt::Key_C && hasSelectedKeyframes()) {
            copySelectedKeyframes();
            event->accept();
            return;
        }
        if (event->key() == Qt::Key_X && hasSelectedKeyframes()) {
            cutSelectedKeyframes();
            event->accept();
            return;
        }
        if (event->key() == Qt::Key_V && hasKfClipboardData()) {
            pasteKeyframes();
            event->accept();
            return;
        }
    }

    if ((event->key() == Qt::Key_Delete || event->key() == Qt::Key_Backspace)
        && hasSelectedKeyframes()) {
        deleteSelectedKeyframes("Delete Keyframes");
        event->accept();
        return;
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
    for (size_t li = 0; li < m_maskPathLanes.size(); ++li) {
        const auto& lane = m_maskPathLanes[li];
        if (!lane.row || !lane.row->isVisible()) continue;
        const MaskPathId id{lane.effectId, lane.maskId};
        const OpacityMask* mask = resolveMaskPath(id);
        if (!mask || !mask->pathAnimated) continue;
        const int y = rowY(lane.row);
        if (y < kRulerHeight || y > height()) continue;
        for (size_t i = 0; i < mask->pathKeys.size(); ++i) {
            const int x = tickToX(mask->pathKeys[i].time);
            if (std::abs(pos.x() - x) + std::abs(pos.y() - y)
                    <= kDiamondRadius + 5) {
                return {nullptr, i, static_cast<int>(li), mask->pathKeys[i].time};
            }
        }
    }

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
                    return {track, i, -1, track->keyframe(i).time};
                }
            }
        }
    }
    return {};
}

} // namespace rt
