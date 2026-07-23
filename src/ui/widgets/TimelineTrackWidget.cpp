/*
 * TimelineTrackWidget.cpp — Track clip area renderer.
 * Step 12: Timeline Panel — Core UI
 */

#include "widgets/TimelineTrackWidget.h"
#include "widgets/TimelineClipWidget.h"
#include "Theme.h"

#include "timeline/TimelineLayoutEngine.h"
#include "timeline/Track.h"
#include "timeline/Clip.h"
#include "timeline/AudioClip.h"
#include "timeline/VideoClip.h"
#include "timeline/Transition.h"

#ifdef ROUNDTABLE_HAS_SPINE
#include "timeline/SpineClip.h"
#include "spine/AnimationVideoCache.h"
#endif

#include <QPainter>
#include <QMouseEvent>
#include <QToolTip>
#include <QHelpEvent>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <set>

namespace rt {


TimelineTrackWidget::TimelineTrackWidget(QWidget* parent)
    : QWidget(parent)
{
    setMouseTracking(true);
}

TimelineTrackWidget::~TimelineTrackWidget()
{
    m_destroying.store(true);
    spdlog::info("[LIFECYCLE] TimelineTrackWidget destructor entered: this={} (class={})", (void*)this, metaObject() ? metaObject()->className() : "null");
}

void TimelineTrackWidget::setLayoutEngine(const TimelineLayoutEngine* engine)
{
    m_engine = engine;
    update();
}

void TimelineTrackWidget::setTrack(const Track* track, size_t trackIndex)
{
    // If the widget is being repointed to a different Track (e.g. after
    // rebuildTracks reused this widget for a different row), drag state
    // from the previous track no longer applies. Without clearing it, a
    // widget that was the drag source can be reassigned to the freshly-
    // created top track after a ghost-track drop and keep skipping the
    // moved clip in its paintEvent — the clip stays invisible even though
    // the program monitor renders it (it's in the data model on the new
    // track). Clearing here is safe because an in-progress drag re-applies
    // the state on every drag-move tick.
    if (m_track != track) {
        m_draggedSet.clear();
        m_ghostDragActive = false;
    }
    m_track = track;
    m_trackIndex = trackIndex;
    update();
}

void TimelineTrackWidget::setSelectedClips(const std::vector<size_t>& indices)
{
    m_selectedClips = indices;
    m_selectedSet.clear();
    m_selectedSet.insert(indices.begin(), indices.end());
    update();
}

void TimelineTrackWidget::setSelectedTransition(size_t transitionIndex)
{
    m_selectedTransitionIndex = transitionIndex;
    update();
}

void TimelineTrackWidget::setGapHighlight(int64_t startTick, int64_t endTick)
{
    if (m_gapHighlightStart == startTick && m_gapHighlightEnd == endTick)
        return;
    m_gapHighlightStart = startTick;
    m_gapHighlightEnd = endTick;
    update();
}

void TimelineTrackWidget::setMediaDragPreview(int64_t tick, int64_t duration, bool isAudio)
{
    if (m_dragPreviewTick == tick && m_dragPreviewDuration == duration
        && m_dragPreviewIsAudio == isAudio)
        return;
    m_dragPreviewTick = tick;
    m_dragPreviewDuration = duration;
    m_dragPreviewIsAudio = isAudio;
    spdlog::debug("GHOST-CLIP: setMediaDragPreview track={} tick={} dur={} audio={} engine={}",
                  m_trackIndex, tick, duration, isAudio, (m_engine ? "yes" : "NULL"));
    update();
}

void TimelineTrackWidget::clearMediaDragPreview()
{
    if (m_dragPreviewTick < 0) return;
    spdlog::debug("GHOST-CLIP: clearMediaDragPreview track={}", m_trackIndex);
    m_dragPreviewTick = -1;
    m_dragPreviewDuration = 0;
    m_dragPreviewIsAudio = false;
    update();
}

void TimelineTrackWidget::setDraggedClips(const std::vector<size_t>& indices)
{
    m_draggedClips = indices;
    m_draggedSet.clear();
    m_draggedSet.insert(indices.begin(), indices.end());
    update();
}

void TimelineTrackWidget::setPlayheadTick(int64_t tick)
{
    if (m_playheadTick == tick) return;  // avoid redundant repaint
    m_playheadTick = tick;

    // Full repaint — the 5px strip optimization caused the first clip
    // to visually disappear because Qt's QPainter clip region and
    // backing-store management don't reliably preserve untouched areas
    // across coalesced partial updates.
    update();
}

void TimelineTrackWidget::setPlayheadTickNoRepaint(int64_t tick)
{
    m_playheadTick = tick;
}

void TimelineTrackWidget::setSnapIndicatorTick(int64_t tick)
{
    m_snapIndicatorTick = tick;
    update();
}

void TimelineTrackWidget::setHoverEdgeTick(int64_t tick)
{
    m_hoverEdgeTick = tick;
    update();
}

void TimelineTrackWidget::setEditPointTick(int64_t tick, EditPointSide side)
{
    if (m_editPointTick == tick && m_editPointSide == side) return;
    m_editPointTick = tick;
    m_editPointSide = side;
    update();
}

void TimelineTrackWidget::setRazorTick(int64_t tick)
{
    if (m_razorTick != tick) {
        m_razorTick = tick;
        update();
    }
}

void TimelineTrackWidget::setWaveformCache(
    const std::unordered_map<uint64_t, std::vector<float>>* cache)
{
    m_waveformCache = cache;
}

double TimelineTrackWidget::waveformClipFractionAtPixel(
    double pixelX, double clipLeft, double clipWidth) noexcept
{
    if (!(clipWidth > 0.0)) return 0.0;
    return std::clamp((pixelX - clipLeft) / clipWidth, 0.0, 1.0);
}

void TimelineTrackWidget::setThumbnailCache(
    const std::unordered_map<uint64_t, QPixmap>* cache)
{
    m_thumbnailCache = cache;
}

void TimelineTrackWidget::setAnimVideoCache(const AnimationVideoCache* cache)
{
    m_animVideoCache = cache;
}

bool TimelineTrackWidget::event(QEvent* event)
{
    if (event->type() == QEvent::ToolTip && m_engine && m_track) {
        auto* helpEvent = static_cast<QHelpEvent*>(event);
        QPoint pos = helpEvent->pos();

        // Find clip under mouse
        for (size_t i = 0; i < m_track->clipCount(); ++i) {
            const Clip* clip = m_track->clip(i);
            if (!clip) continue;
            auto layoutRect = m_engine->clipRect(
                clip->timelineIn(), clip->duration(),
                0.0f, static_cast<float>(height()));
            QRectF qRect(layoutRect.x, layoutRect.y, layoutRect.width, layoutRect.height);
            if (qRect.contains(pos)) {
                // Format timecode
                auto formatTime = [](int64_t ticks) -> QString {
                    double secs = static_cast<double>(ticks) / 48000.0;
                    int m = static_cast<int>(secs) / 60;
                    int s = static_cast<int>(secs) % 60;
                    int f = static_cast<int>((secs - static_cast<int>(secs)) * 30);
                    return QString("%1:%2:%3")
                        .arg(m, 2, 10, QChar('0'))
                        .arg(s, 2, 10, QChar('0'))
                        .arg(f, 2, 10, QChar('0'));
                };

                QString name = clip->label().empty()
                    ? TimelineClipWidget::typeName(clip->clipType())
                    : QString::fromStdString(clip->label());
                QString tip = QString("<b>%1</b><br>"
                                      "In: %2 &nbsp; Out: %3<br>"
                                      "Duration: %4<br>"
                                      "Speed: %5x")
                    .arg(name)
                    .arg(formatTime(clip->timelineIn()))
                    .arg(formatTime(clip->timelineOut()))
                    .arg(formatTime(clip->duration()))
                    .arg(clip->speed(), 0, 'f', 2);

                QToolTip::showText(helpEvent->globalPos(), tip, this);
                return true;
            }
        }
        QToolTip::hideText();
        return true;
    }
    return QWidget::event(event);
}

void TimelineTrackWidget::paintEvent(QPaintEvent* event)
{
    static thread_local int s_paintDepth = 0;
    if (++s_paintDepth > 5) {
        --s_paintDepth;
        QWidget::paintEvent(event);
        return;
    }

    // Unconditional one-shot diagnostic for every track widget
    static std::set<std::string> s_loggedTracks;
    if (m_track && s_loggedTracks.find(m_track->name()) == s_loggedTracks.end()) {
        s_loggedTracks.insert(m_track->name());
        spdlog::info("PAINT-FIRST: track='{}' visible={} size={}x{} pos=({},{}) "
                     "parent={} parentVisible={} clips={} engine={}",
                     m_track->name(), isVisible(), width(), height(),
                     x(), y(),
                     (parentWidget() ? "yes" : "no"),
                     (parentWidget() ? parentWidget()->isVisible() : false),
                     m_track->clipCount(),
                     (m_engine ? "yes" : "no"));
    }

    if (!m_engine || !m_track) { --s_paintDepth; return; }

    auto paintT0 = std::chrono::steady_clock::now();

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    const auto& tc = Theme::colors();
    const QRect dirtyRect = event->rect();

    // ── Divider track: flat near-black separator bar, no clips ──────────
    // Matches the divider styling on the header side so the separator
    // spans the full timeline width as its own row.
    if (m_track->isDivider()) {
        const QColor divBg = (m_track->color() != 0)
                                 ? QColor::fromRgba(m_track->color())
                                 : QColor(10, 10, 12);   // near-black default
        painter.fillRect(rect(), divBg);
        painter.setPen(tc.trackDivider);
        painter.drawLine(0, 0, width(), 0);
        painter.drawLine(0, height() - 1, width(), height() - 1);
        --s_paintDepth;
        return;
    }

    // Track background
    QColor bgColor = (m_track->type() == TrackType::Video)
                         ? tc.trackBg : tc.trackBgAlt;
    // Caption (subtitle) track gets a clearly distinct tint (blended ~35%
    // toward the accent) so it never looks like just another video track.
    if (m_track->isCaptionTrack()) {
        const QColor a = tc.accent;
        bgColor = QColor((bgColor.red()   * 65 + a.red()   * 35) / 100,
                         (bgColor.green() * 65 + a.green() * 35) / 100,
                         (bgColor.blue()  * 65 + a.blue()  * 35) / 100);
    }
    painter.fillRect(dirtyRect, bgColor);

    // Bottom border (only if dirty rect touches the bottom edge)
    if (dirtyRect.bottom() >= height() - 2) {
        // Caption track gets a thick accent separator so it reads as a
        // distinct, pinned region clearly divided from the video tracks below.
        if (m_track->isCaptionTrack()) {
            painter.setPen(QPen(tc.accent, 2));
            painter.drawLine(dirtyRect.left(), height() - 1,
                             dirtyRect.right(), height() - 1);
        } else {
            painter.setPen(tc.trackDivider);
            painter.drawLine(dirtyRect.left(), height() - 1,
                             dirtyRect.right(), height() - 1);
        }
    }

    // Locked overlay — drawn after clips (see below)
    // (intentionally empty here)

    // Muted overlay
    if (m_track->isMuted())
    {
        painter.fillRect(rect(), QColor(0, 0, 0, 40));
    }

    // Draw only visible clips that intersect the dirty region.
    // Clips are stored sorted by timelineIn, so we use binary search to
    // skip clips entirely before the viewport, then early-exit once
    // a clip starts past the right edge.
    const size_t clipCount = m_track->clipCount();
    if (clipCount > 0 && m_engine) {
        // Convert dirty region left edge to timeline tick for binary search.
        int64_t leftTick = m_engine->pixelXToTime(dirtyRect.left());

        // Binary search: find first clip whose timelineOut >= leftTick.
        size_t lo = 0, hi = clipCount;
        while (lo < hi) {
            size_t mid = lo + (hi - lo) / 2;
            const auto* c = m_track->clip(mid);
            if (c && c->timelineOut() < leftTick)
                lo = mid + 1;
            else
                hi = mid;
        }

        for (size_t i = lo; i < clipCount; ++i)
        {
            const auto* clip = m_track->clip(i);
            if (!clip) continue;

            double clipLeft = m_engine->timeToPixelX(clip->timelineIn());
            if (clipLeft > dirtyRect.right() + 1) break;

            // Skip dragged clips on the first pass — they're re-painted
            // last so they always appear ABOVE any clip they overlap.
            if (m_draggedSet.count(i) > 0) continue;
            paintClip(painter, i);
        }

        // Second pass: dragged clips on top so the move preview is
        // visible above the clip(s) it would overwrite.
        // When ghost-dragging, hide them entirely (ghost overlay shows them).
        if (!m_draggedSet.empty() && !m_ghostDragActive) {
            for (size_t i = lo; i < clipCount; ++i) {
                if (m_draggedSet.count(i) == 0) continue;
                const auto* clip = m_track->clip(i);
                if (!clip) continue;
                double clipLeft = m_engine->timeToPixelX(clip->timelineIn());
                if (clipLeft > dirtyRect.right() + 1) continue;
                paintClip(painter, i);
            }
        }
    }

    // ── Gap selection highlight ─────────────────────────────────────────
    if (m_gapHighlightStart >= 0 && m_gapHighlightEnd > m_gapHighlightStart && m_engine)
    {
        double gapLeft  = m_engine->timeToPixelX(m_gapHighlightStart);
        double gapRight = m_engine->timeToPixelX(m_gapHighlightEnd);
        QRectF gapRect(gapLeft, 0, gapRight - gapLeft, height());

        // Semi-transparent accent fill
        QColor gapFill = tc.accent;
        gapFill.setAlpha(50);
        painter.fillRect(gapRect, gapFill);

        // Border
        QPen gapPen(tc.accent, 1.5);
        gapPen.setStyle(Qt::DashLine);
        painter.setPen(gapPen);
        painter.setBrush(Qt::NoBrush);
        painter.drawRect(gapRect);
    }

    // ── Media drag preview (semi-transparent ghost clip — Premiere Pro style) ──
    if (m_dragPreviewTick >= 0 && m_dragPreviewDuration > 0 && m_engine)
    {
        auto lr = m_engine->clipRect(
            m_dragPreviewTick, m_dragPreviewDuration,
            0.0f, static_cast<float>(height()));
        QRectF ghostRect(lr.x, lr.y, lr.width, lr.height);

        // Rate-limited diagnostic (every 300ms)
        {
            static thread_local auto s_lastGhostLog = std::chrono::steady_clock::now();
            auto now = std::chrono::steady_clock::now();
            if (now - s_lastGhostLog > std::chrono::milliseconds(300)) {
                s_lastGhostLog = now;
                spdlog::debug("GHOST-CLIP: paint track={} tick={} dur={} ghostRect=({:.0f},{:.0f},{:.0f},{:.0f}) "
                              "widgetW={} visible={}",
                              m_trackIndex, m_dragPreviewTick, m_dragPreviewDuration,
                              ghostRect.x(), ghostRect.y(), ghostRect.width(), ghostRect.height(),
                              width(), (ghostRect.right() >= 0 && ghostRect.left() <= width()));
            }
        }

        if (ghostRect.right() >= 0 && ghostRect.left() <= width()) {
            painter.save();
            painter.setOpacity(0.45);

            // Use clip type color
            QColor fillCol = m_dragPreviewIsAudio ? tc.clipAudio : tc.clipVideo;
            QColor borderCol = fillCol.lighter(130);

            // Rounded rect fill
            painter.setPen(Qt::NoPen);
            painter.setBrush(fillCol);
            painter.drawRoundedRect(ghostRect, 3, 3);

            // Border
            painter.setOpacity(0.7);
            QPen borderPen(borderCol, 1.5);
            painter.setPen(borderPen);
            painter.setBrush(Qt::NoBrush);
            painter.drawRoundedRect(ghostRect.adjusted(0.5, 0.5, -0.5, -0.5), 3, 3);

            // White snap line at the left edge (drop position indicator)
            painter.setOpacity(0.9);
            painter.setPen(QPen(tc.textBright, 1.5));
            painter.drawLine(QPointF(ghostRect.left(), 0),
                             QPointF(ghostRect.left(), height()));

            painter.restore();
        }
    }

    // ── Effect-drop highlight (darken clip + border + "fx" badge) ───────
    if (m_effectHighlightClipId != 0)
    {
        for (size_t i = 0; i < m_track->clipCount(); ++i)
        {
            const Clip* clip = m_track->clip(i);
            if (!clip || clip->id() != m_effectHighlightClipId) continue;

            auto lr = m_engine->clipRect(
                clip->timelineIn(), clip->duration(),
                0.0f, static_cast<float>(height()));
            QRectF cr(lr.x, lr.y, lr.width, lr.height);

            // Same visible-neighbourhood clamp as paintClip — unclamped
            // coordinates on a project-long clip exceed the raster engine's
            // device-space limit and the overlay silently vanishes.
            if (cr.left() < -64.0)           cr.setLeft(-64.0);
            if (cr.right() > width() + 64.0) cr.setRight(width() + 64.0);

            // Semi-transparent dark overlay (slight darkening)
            painter.fillRect(cr, QColor(0, 0, 0, 55));

            // Bright accent border (2 px)
            QPen borderPen(tc.accent, 2.0);
            painter.setPen(borderPen);
            painter.setBrush(Qt::NoBrush);
            painter.drawRect(cr.adjusted(1, 1, -1, -1));

            // "fx" badge in top-left corner
            painter.setPen(tc.textBright);
            static const QFont fxFont("Segoe UI", 7, QFont::Bold);
            painter.setFont(fxFont);
            painter.drawText(cr.adjusted(4, 2, 0, 0),
                             Qt::AlignTop | Qt::AlignLeft, "fx");
            break;
        }
    }

    // ── Transition drop-zone edge highlight (bright pulsing line + glow) ──
    if (m_transitionDropEdgeTick >= 0)
    {
        double edgeX = m_engine->timeToPixelX(m_transitionDropEdgeTick);
        if (edgeX >= -10 && edgeX <= width() + 10)
        {
            // Glow behind (wider, semi-transparent)
            QColor glowCol = tc.accent; glowCol.setAlpha(60);
            painter.setPen(Qt::NoPen);
            painter.setBrush(glowCol);
            painter.drawRect(QRectF(edgeX - 6, 0, 12, height()));

            // Bright center line
            QColor lineCol = tc.accent; lineCol.setAlpha(240);
            painter.setPen(QPen(lineCol, 2.5));
            painter.drawLine(QPointF(edgeX, 0), QPointF(edgeX, height()));

            // Small transition icon (two overlapping triangles) at center
            double cy = height() / 2.0;
            painter.setBrush(lineCol);
            painter.setPen(Qt::NoPen);
            // Left triangle (outgoing)
            QPointF leftTri[3] = {
                QPointF(edgeX - 8, cy - 6),
                QPointF(edgeX - 2, cy),
                QPointF(edgeX - 8, cy + 6)
            };
            painter.drawPolygon(leftTri, 3);
            // Right triangle (incoming)
            QPointF rightTri[3] = {
                QPointF(edgeX + 8, cy - 6),
                QPointF(edgeX + 2, cy),
                QPointF(edgeX + 8, cy + 6)
            };
            painter.drawPolygon(rightTri, 3);
        }
    }

    // Playhead line is drawn by the PlayheadOverlay widget in TimelinePanel
    // to avoid triggering full track repaints on every tick.

    // ── Snap indicator line (white dashed line at snap target) ──────────
    if (m_snapIndicatorTick >= 0)
    {
        double snapX = m_engine->timeToPixelX(m_snapIndicatorTick);
        if (snapX >= 0 && snapX <= width())
        {
            QColor snapCol = tc.textBright; snapCol.setAlpha(200);
            QPen snapPen(snapCol, 1.0, Qt::DashLine);
            painter.setPen(snapPen);
            painter.drawLine(QPointF(snapX, 0),
                             QPointF(snapX, height()));
        }
    }

    // ── Hover edge highlight (yellow line on hovered clip edge) ─────────
    if (m_hoverEdgeTick >= 0)
    {
        double edgeX = m_engine->timeToPixelX(m_hoverEdgeTick);
        if (edgeX >= 0 && edgeX <= width())
        {
            QColor edgeCol = tc.clipSelected; edgeCol.setAlpha(220);
            painter.setPen(QPen(edgeCol, 2.5));
            painter.drawLine(QPointF(edgeX, 0),
                             QPointF(edgeX, height()));
        }
    }

    // ── Edit-point selection (Premiere-style brackets at a clip edge) ─────
    // Both:     facing brackets at a connected seam between touching clips.
    // HeadOnly: single right-opening bracket on a clip's head (no left nb).
    // TailOnly: single left-opening bracket on a clip's tail (no right nb).
    if (m_editPointTick >= 0)
    {
        double cutX = m_engine->timeToPixelX(m_editPointTick);
        if (cutX >= -16 && cutX <= width() + 16)
        {
            const double topY = 2.0;
            const double botY = height() - 2.0;
            const double armW = 6.0;          // horizontal arm of each bracket
            const double pen  = 2.0;

            QColor col = tc.clipSelected;
            col.setAlpha(240);
            painter.setPen(QPen(col, pen));
            painter.setBrush(Qt::NoBrush);

            const bool drawLeftBracket =
                m_editPointSide != EditPointSide::HeadOnly; // omit on head-only
            const bool drawRightBracket =
                m_editPointSide != EditPointSide::TailOnly; // omit on tail-only

            if (drawLeftBracket) {
                // Left bracket (belongs to the LEFT clip's tail) — opens →
                painter.drawLine(QPointF(cutX - armW, topY),
                                 QPointF(cutX,        topY));
                painter.drawLine(QPointF(cutX,        topY),
                                 QPointF(cutX,        botY));
                painter.drawLine(QPointF(cutX - armW, botY),
                                 QPointF(cutX,        botY));
            }
            if (drawRightBracket) {
                // Right bracket (belongs to the RIGHT clip's head) — opens ←
                painter.drawLine(QPointF(cutX,        topY),
                                 QPointF(cutX + armW, topY));
                painter.drawLine(QPointF(cutX,        topY),
                                 QPointF(cutX,        botY));
                painter.drawLine(QPointF(cutX,        botY),
                                 QPointF(cutX + armW, botY));
            }

            // Thin highlight line right at the seam for clarity
            QColor seam = col; seam.setAlpha(140);
            painter.setPen(QPen(seam, 1.0));
            painter.drawLine(QPointF(cutX, topY), QPointF(cutX, botY));
        }
    }

    // ── Razor / Blade indicator (red vertical line — Premiere Pro style) ──
    if (m_razorTick >= 0)
    {
        double rzX = m_engine->timeToPixelX(m_razorTick);
        if (rzX >= 0 && rzX <= width())
        {
            QColor rzCol = tc.error; rzCol.setAlpha(230);
            painter.setPen(QPen(rzCol, 1.5));
            painter.drawLine(QPointF(rzX, 0),
                             QPointF(rzX, height()));
            // Small triangular arrow at top
            painter.setBrush(rzCol);
            painter.setPen(Qt::NoPen);
            QPointF tri[3] = {
                QPointF(rzX - 4, 0),
                QPointF(rzX + 4, 0),
                QPointF(rzX, 6)
            };
            painter.drawPolygon(tri, 3);
        }
    }

    // ── In/Out point overlay (gray-out outside range + bracket markers) ──
    if (m_inPoint >= 0 || m_outPoint >= 0)
    {
        double inX  = (m_inPoint >= 0)  ? m_engine->timeToPixelX(m_inPoint)  : 0.0;
        double outX = (m_outPoint >= 0) ? m_engine->timeToPixelX(m_outPoint) : static_cast<double>(width());

        QColor grayOverlay(0, 0, 0, 100);

        // Gray out region before in-point
        if (m_inPoint >= 0 && inX > 0)
            painter.fillRect(QRectF(0, 0, inX, height()), grayOverlay);

        // Gray out region after out-point
        if (m_outPoint >= 0 && outX < width())
            painter.fillRect(QRectF(outX, 0, width() - outX, height()), grayOverlay);

        // In-point — thin 1px accent line (Premiere style, no brackets)
        if (m_inPoint >= 0)
        {
            int ix = static_cast<int>(inX);
            painter.setPen(QPen(tc.accent, 1.0));
            painter.drawLine(ix, 0, ix, height());
        }

        // Out-point — thin 1px accent line
        if (m_outPoint >= 0)
        {
            int ox = static_cast<int>(outX);
            painter.setPen(QPen(tc.accent, 1.0));
            painter.drawLine(ox, 0, ox, height());
        }
    }

    // ── Locked-track hatching overlay (Premiere Pro style) ──────────
    if (m_track->isLocked())
    {
        painter.save();
        // Semi-transparent dark fill
        painter.fillRect(rect(), QColor(0, 0, 0, 50));
        // Diagonal hatch lines
        QPen hatchPen(QColor(180, 180, 180, 70), 1);
        painter.setPen(hatchPen);
        painter.setClipRect(dirtyRect);
        const int spacing = 8;
        int w = width();
        int h = height();
        // Draw lines from bottom-left to upper-right
        for (int d = -h; d < w + h; d += spacing) {
            painter.drawLine(d, h, d + h, 0);
        }
        painter.restore();
    }

    // ── Paint perf logging ──────────────────────────────────────────────
    {
        double paintMs = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - paintT0).count();
        if (paintMs > 0.5) {
            spdlog::info("[PERF] TrackWidget::paintEvent: {:.1f}ms  clips={} dirty={}x{}  track={}",
                         paintMs, m_track->clipCount(),
                         dirtyRect.width(), dirtyRect.height(),
                         m_track->name());
        }
    }

    --s_paintDepth;
}

void TimelineTrackWidget::paintClip(QPainter& painter, size_t clipIndex)
{
    const auto& tc = Theme::colors();
    const Clip* clip = m_track->clip(clipIndex);
    if (!clip) return;

    auto layoutRect = m_engine->clipRect(
        clip->timelineIn(),
        clip->duration(),
        0.0f,  // Y relative to this widget
        static_cast<float>(height()));

    // Skip if not visible
    if (layoutRect.x + layoutRect.width < 0 || layoutRect.x > width())
        return;

    QRectF qRect(layoutRect.x, layoutRect.y, layoutRect.width, layoutRect.height);

    // Clamp to the widget's visible neighbourhood.  At deep zoom a
    // project-long clip is millions of pixels wide; unclamped coordinates
    // exceed the Qt raster engine's ±32767 DEVICE-space limit (which shrinks
    // further with display scaling) and primitives are silently dropped —
    // and the offline cross-hatch loop below would iterate width/12 times.
    // The painter clips to the widget, so this clamp is visually exact.
    // (TimelineClipWidget::paint clamps the body the same way internally.)
    constexpr double kPaintMargin = 64.0;
    const bool rightEdgeOffscreen = qRect.right() > width() + kPaintMargin;
    if (qRect.left() < -kPaintMargin)  qRect.setLeft(-kPaintMargin);
    if (rightEdgeOffscreen)            qRect.setRight(width() + kPaintMargin);

    bool selected = m_selectedSet.count(clipIndex) > 0;

    bool dragging = m_draggedSet.count(clipIndex) > 0;

    // Dragged clips: force selected border + semi-transparent (Premiere Pro style)
    if (dragging) {
        selected = true;
        painter.setOpacity(0.55);
    }

    auto style = TimelineClipWidget::defaultStyle(clip->clipType());

    // ── Distinguish video subtypes (Premiere-style color coding) ────────
    //   - Still images (.png/.jpg/...) → purple (matches Project Bin)
    //   - Video characters (e.g. Wells)  → orange
    //   - Other videos                  → default blue
    if (clip->clipType() == ClipType::Video) {
        auto* vc = static_cast<const VideoClip*>(clip);
        if (vc->isVideoCharacter()) {
            style.fillColor   = tc.clipCharacter;
            style.borderColor = tc.clipCharacter.lighter(130);
        } else {
            const std::string& mp = vc->mediaPath();
            auto dot = mp.find_last_of('.');
            if (dot != std::string::npos) {
                std::string ext = mp.substr(dot + 1);
                for (auto& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                if (ext == "png"  || ext == "jpg"  || ext == "jpeg" || ext == "bmp" ||
                    ext == "gif"  || ext == "tif"  || ext == "tiff" || ext == "webp" ||
                    ext == "tga"  || ext == "dds")
                {
                    style.fillColor   = tc.clipImage;
                    style.borderColor = tc.clipImage.lighter(130);
                }
            }
        }
    }

    // Use per-clip custom label color if set (non-default)
    if (clip->color() != 0xFF888888) {
        QColor customColor = QColor::fromRgba(clip->color());
        style.fillColor = customColor;
        style.borderColor = customColor.lighter(140);
    }

    QString label = TimelineClipWidget::displayLabel(
        clip->clipType(), QString::fromStdString(clip->label()));

    TimelineClipWidget::paint(painter, qRect, style, label, selected, clip->isEnabled());

    // ── Per-clip render status bar (thin line at top — Premiere Pro style) ──
    // Yellow=needs render, Green=rendered, Red=error
    if (qRect.width() > 6) {
        QColor statusColor;
        switch (clip->renderStatus()) {
        case 1:  statusColor = tc.success; break;
        case 2:  statusColor = tc.error;   break;
        default: statusColor = tc.warning; break;
        }
        statusColor.setAlpha(180);
        painter.setPen(Qt::NoPen);
        painter.setBrush(statusColor);
        painter.drawRect(QRectF(qRect.left() + 1, qRect.top() + 1, qRect.width() - 2, 2));
    }

    // (Removed the Spine "VID"/"LIVE" badge: it flagged whether a converted
    // AnimationVideoCache file existed on disk, but Spine clips always render
    // live from skeleton+atlas and never use that cache for compositing/export
    // — so the badge only created confusion that characters were "converted".)

    // ── Offline media indicator (red "OFFLINE" badge — Premiere Pro style) ──
    if (clip->isOffline() && qRect.width() >= 40 && qRect.height() >= 16) {
        painter.save();

        // Darken entire clip with red tint
        QColor offlineOverlay = tc.error; offlineOverlay.setAlpha(40);
        painter.fillRect(qRect, offlineOverlay);

        // Red diagonal cross-hatch pattern
        QPen crossPen(tc.error, 1.0);
        crossPen.setStyle(Qt::DashLine);
        painter.setPen(crossPen);
        painter.setOpacity(0.3);
        for (double dx = -qRect.height(); dx < qRect.width(); dx += 12) {
            painter.drawLine(QPointF(qRect.left() + dx, qRect.bottom()),
                             QPointF(qRect.left() + dx + qRect.height(), qRect.top()));
        }
        painter.setOpacity(1.0);

        // "OFFLINE" badge centered in clip
        static const QFont offlineFont("Segoe UI", 8, QFont::Bold);
        painter.setFont(offlineFont);
        QFontMetrics fm(offlineFont);
        int textW = fm.horizontalAdvance("OFFLINE");
        int badgeW = textW + 10;
        int badgeH = fm.height() + 4;
        double bx = qRect.center().x() - badgeW / 2.0;
        double by = qRect.center().y() - badgeH / 2.0;

        QColor badgeBg = tc.error; badgeBg.setAlpha(200);
        painter.setPen(Qt::NoPen);
        painter.setBrush(badgeBg);
        painter.drawRoundedRect(QRectF(bx, by, badgeW, badgeH), 3, 3);
        painter.setPen(tc.textBright);
        painter.drawText(QRectF(bx, by, badgeW, badgeH), Qt::AlignCenter, "OFFLINE");

        painter.restore();
    }

    // ── Draw clip badges (FX, speed, keyframes) ──────────────────────────
    if (!rightEdgeOffscreen && qRect.width() > 30 && qRect.height() > 16)
    {
        static const QFont badgeFont("Segoe UI", 6, QFont::Bold);
        painter.setFont(badgeFont);
        double bx = qRect.right() - 4;
        constexpr double bw = 16, bh = 10, gap = 2;

        // Speed badge: show if speed != 1.0
        if (std::abs(clip->speed() - 1.0) > 0.01) {
            bx -= bw;
            QRectF badge(bx, qRect.bottom() - bh - 2, bw, bh);
            painter.setPen(Qt::NoPen);
            QColor speedBadge = tc.warning; speedBadge.setAlpha(200);
            painter.setBrush(speedBadge);
            painter.drawRoundedRect(badge, 2, 2);
            painter.setPen(tc.textBright);
            QString speedTxt = QString::number(clip->speed(), 'f', 1) + "x";
            painter.drawText(badge, Qt::AlignCenter, speedTxt);
            bx -= gap;
        }

        // FX badge: show if clip has effects
        if (!clip->effects().isEmpty()) {
            bx -= bw;
            QRectF badge(bx, qRect.bottom() - bh - 2, bw, bh);
            painter.setPen(Qt::NoPen);
            QColor fxBadge = tc.clipTitle; fxBadge.setAlpha(200);
            painter.setBrush(fxBadge);
            painter.drawRoundedRect(badge, 2, 2);
            painter.setPen(tc.textBright);
            painter.drawText(badge, Qt::AlignCenter, "FX");
            bx -= gap;
        }

        // Keyframe badge: show if any keyframetrack is animated (>1 keyframe)
        {
            // Use const_cast workaround since accessors are non-const
            Clip* mutableClip = const_cast<Clip*>(clip);
            bool hasKF = !mutableClip->opacity().isStatic()
                      || !mutableClip->positionX().isStatic()
                      || !mutableClip->positionY().isStatic()
                      || !mutableClip->scaleX().isStatic()
                      || !mutableClip->scaleY().isStatic()
                      || !mutableClip->rotation().isStatic();
            if (hasKF) {
                bx -= bw;
                QRectF badge(bx, qRect.bottom() - bh - 2, bw, bh);
                painter.setPen(Qt::NoPen);
                QColor kfBadge = tc.accent; kfBadge.setAlpha(200);
                painter.setBrush(kfBadge);
                painter.drawRoundedRect(badge, 2, 2);
                painter.setPen(tc.textBright);
                painter.drawText(badge, Qt::AlignCenter, QStringLiteral("\u25C6"));  // diamond
            }
        }
    }

    // ── Draw transition indicators (diagonal gradient overlays) ─────────
    const uint64_t clipId = clip->id();
    for (size_t trI = 0; trI < m_track->transitionCount(); ++trI) {
        const Transition* trans = m_track->transition(trI);
        if (!trans) continue;

        // Only draw if this transition involves this clip
        bool isLeft  = (trans->leftClipId == clipId);
        bool isRight = (trans->rightClipId == clipId);
        if (!isLeft && !isRight) continue;

        int64_t tStart, tEnd;
        trans->getRange(tStart, tEnd);

        double txLeft  = m_engine->timeToPixelX(tStart);
        double txRight = m_engine->timeToPixelX(tEnd);

        // Clamp to clip rect
        txLeft  = std::max(txLeft,  qRect.left());
        txRight = std::min(txRight, qRect.right());
        if (txRight <= txLeft) continue;

        QRectF transRect(txLeft, qRect.top() + 1, txRight - txLeft, qRect.height() - 2);

        painter.save();
        painter.setClipRect(qRect);

        bool transSelected = (trI == m_selectedTransitionIndex);

        // Semi-transparent diagonal gradient overlay
        QColor transColor = tc.transition; transColor.setAlpha(80);
        if (trans->type == TransitionType::CrossDissolve) {
            transColor = tc.accent; transColor.setAlpha(80);
        }

        // Selected transition: brighter fill + selection border
        if (transSelected) {
            QColor selFill = transColor.lighter(160);
            selFill.setAlpha(140);
            painter.fillRect(transRect, selFill);

            // Outer glow
            QColor glowColor = tc.accent; glowColor.setAlpha(90);
            painter.setPen(QPen(glowColor, 4.0));
            painter.setBrush(Qt::NoBrush);
            painter.drawRect(transRect.adjusted(-1, -1, 1, 1));

            // Inner border
            painter.setPen(QPen(tc.accent, 1.5));
            painter.setBrush(Qt::NoBrush);
            painter.drawRect(transRect);
        } else {
            painter.fillRect(transRect, transColor);
        }

        // Diagonal line to indicate the transition ramp
        painter.setPen(QPen(transColor.lighter(180), 1.5));
        if (isLeft) {
            // Outgoing: diagonal from top-left to bottom-right
            painter.drawLine(QPointF(transRect.left(), transRect.top()),
                             QPointF(transRect.right(), transRect.bottom()));
        } else {
            // Incoming: diagonal from bottom-left to top-right
            painter.drawLine(QPointF(transRect.left(), transRect.bottom()),
                             QPointF(transRect.right(), transRect.top()));
        }

        // ── Draw drag handle on the adjustable edge ────────────────────
        // For fade-in: handle on the right edge (end of transition)
        // For fade-out: handle on the left edge (start of transition)
        // For cross-dissolve: handles on both edges
        {
            constexpr double hW = 4.0;  // handle half-width
            constexpr double hH = 10.0; // handle half-height
            double cy = transRect.center().y();
            QColor handleCol = tc.textBright; handleCol.setAlpha(200);
            painter.setPen(Qt::NoPen);
            painter.setBrush(handleCol);

            bool drawLeft  = (trans->rightClipId == 0)          // fade-out
                          || (trans->leftClipId != 0 && trans->rightClipId != 0); // cross-dissolve
            bool drawRight = (trans->leftClipId == 0)           // fade-in
                          || (trans->leftClipId != 0 && trans->rightClipId != 0); // cross-dissolve

            if (drawLeft && isLeft) {
                // handle on left side of transition rect (only paint on the left clip pass)
                QRectF h(transRect.left() - hW, cy - hH, hW * 2, hH * 2);
                painter.drawRoundedRect(h, 2, 2);
            }
            if (drawRight && isRight) {
                // handle on right side of transition rect (only paint on the right clip pass)
                QRectF h(transRect.right() - hW, cy - hH, hW * 2, hH * 2);
                painter.drawRoundedRect(h, 2, 2);
            }
        }

        painter.restore();
    }

    // ── Draw waveform for audio clips ───────────────────────────────────
    // Peaks are generated for the ENTIRE source file. During rendering we
    // map the clip's visible source window into the peak array so trimming,
    // speed changes, and viewport clipping all reveal the correct audio.
    // qRect is deliberately clamped above for safe Qt painting, so waveform
    // time mapping must use the original, unclipped layoutRect geometry.
    if (clip->isAudio() && m_waveformCache && layoutRect.width > 4)
    {
        auto it = m_waveformCache->find(clip->id());
        if (it != m_waveformCache->end())
        {
            const auto& peaks = it->second;
            if (!peaks.empty())
            {
                painter.save();
                painter.setClipRect(qRect.adjusted(1, 1, -1, -1));

                const double centerY = qRect.center().y();
                const double halfH   = qRect.height() * 0.40; // 80% of clip height
                const int peakCount  = static_cast<int>(peaks.size());

                const double clipLeft = layoutRect.x;
                const double clipWidth = layoutRect.width;
                const double clipRight = clipLeft + clipWidth;
                const int firstPixel = std::max(
                    0, static_cast<int>(std::floor(std::max(clipLeft, 0.0))));
                const int pastLastPixel = std::min(
                    width(), static_cast<int>(std::ceil(
                                 std::min(clipRight, static_cast<double>(width())))));
                const int visiblePixelCount = std::max(0, pastLastPixel - firstPixel);

                // Map the clip's source window into peak indices.
                // Peaks cover the entire file at 480-frame windows (48 kHz).
                // Match playback's pointwise source mapping, including reverse
                // playback and animated speed ramps.
                constexpr int kPeakWindowFrames = 480;
                const int64_t srcIn = clip->sourceIn();
                const int64_t dur   = clip->duration();
                auto sourcePeakAtPixel = [&](double pixelX) {
                    const double fraction = waveformClipFractionAtPixel(
                        pixelX, clipLeft, clipWidth);
                    const int64_t localTick = std::clamp<int64_t>(
                        static_cast<int64_t>(std::llround(fraction * dur)), 0, dur);
                    const double sourceTick = static_cast<double>(srcIn) +
                        static_cast<double>(localTick) * clip->effectiveSpeed(localTick);
                    return sourceTick / kPeakWindowFrames;
                };

                QColor wfCol = tc.waveformFg; wfCol.setAlpha(180);
                painter.setPen(QPen(wfCol, 1.0));

                // Batch all waveform lines into a single drawLines() call
                // instead of per-pixel drawLine() for ~5x paint speedup.
                QVector<QLineF> wfLines;
                wfLines.reserve(visiblePixelCount);

                for (int x = firstPixel; x < pastLastPixel; ++x)
                {
                    const double peakA = sourcePeakAtPixel(static_cast<double>(x));
                    const double peakB = sourcePeakAtPixel(static_cast<double>(x + 1));
                    const double lowPeak = std::min(peakA, peakB);
                    const double highPeak = std::max(peakA, peakB);
                    int p0 = static_cast<int>(std::clamp(
                        std::floor(lowPeak), 0.0, static_cast<double>(peakCount - 1)));
                    int p1 = static_cast<int>(std::clamp(
                        std::ceil(highPeak), 1.0, static_cast<double>(peakCount)));
                    if (p1 <= p0) p1 = std::min(p0 + 1, peakCount);

                    float maxPeak = 0.0f;
                    for (int pi = p0; pi < p1 && pi < peakCount; ++pi)
                        maxPeak = std::max(maxPeak, peaks[pi]);

                    double amp = static_cast<double>(maxPeak) * halfH;
                    if (amp < 0.5) continue; // skip tiny values

                    const double xPos = static_cast<double>(x);
                    wfLines.append(QLineF(xPos, centerY - amp, xPos, centerY + amp));
                }

                if (!wfLines.isEmpty())
                    painter.drawLines(wfLines);

                painter.restore();
            }
        }
    }

    // ── Draw keyframe overlay (opacity curve for video, volume for audio) ──
    // Premiere Pro-style rubber-band line showing animated property values.
    {
        Clip* mutableClip = const_cast<Clip*>(clip);
        KeyframeTrack<float>* kfTrack = nullptr;
        float maxVal = 1.0f;
        QColor curveColor;

        if (clip->isAudio()) {
            auto* audioClip = dynamic_cast<const AudioClip*>(clip);
            if (audioClip) {
                kfTrack = &const_cast<AudioClip*>(audioClip)->volume();
                maxVal = 2.0f;
                curveColor = tc.success;
            }
        } else {
            kfTrack = &mutableClip->opacity();
            curveColor = tc.accent;
        }

        if (kfTrack && !kfTrack->isStatic() && qRect.width() > 20 && qRect.height() > 16) {
            painter.save();
            painter.setClipRect(qRect.adjusted(1, 1, -1, -1));

            int64_t dur   = clip->duration();
            int pixW = static_cast<int>(qRect.width());

            // Build polyline by evaluating the keyframe track at each pixel column
            QVector<QPointF> points;
            points.reserve(pixW / 2 + 1);
            for (int px = 0; px <= pixW; px += 2) {
                double t = static_cast<double>(px) / pixW;
                int64_t time = static_cast<int64_t>(t * dur);
                float val = std::clamp(kfTrack->evaluate(time), 0.0f, maxVal);
                double y = qRect.bottom() - 4.0 - (val / maxVal) * (qRect.height() - 8.0);
                points.append(QPointF(qRect.left() + px, y));
            }

            // Draw the curve
            curveColor.setAlpha(180);
            painter.setPen(QPen(curveColor, 1.5));
            painter.setBrush(Qt::NoBrush);
            painter.drawPolyline(points.data(), points.size());

            // Draw keyframe diamonds at each keyframe position
            curveColor.setAlpha(230);
            painter.setPen(Qt::NoPen);
            painter.setBrush(curveColor);
            for (size_t ki = 0; ki < kfTrack->keyframeCount(); ++ki) {
                const auto& kf = kfTrack->keyframe(ki);
                if (kf.time >= 0 && kf.time < dur) {
                    double xPos = qRect.left() + (static_cast<double>(kf.time) / dur) * qRect.width();
                    float val = std::clamp(kf.value, 0.0f, maxVal);
                    double yPos = qRect.bottom() - 4.0 - (val / maxVal) * (qRect.height() - 8.0);
                    QPointF diamond[4] = {
                        QPointF(xPos, yPos - 3),
                        QPointF(xPos + 3, yPos),
                        QPointF(xPos, yPos + 3),
                        QPointF(xPos - 3, yPos)
                    };
                    painter.drawPolygon(diamond, 4);
                }
            }

            painter.restore();
        }
    }

    // Restore full opacity after all clip content is drawn
    if (dragging)
        painter.setOpacity(1.0);
}

void TimelineTrackWidget::mousePressEvent(QMouseEvent* event)
{
    if (!m_engine || !m_track) return;

    if (event->button() == Qt::LeftButton)
    {
        // Check if a clip was clicked
        for (size_t i = 0; i < m_track->clipCount(); ++i)
        {
            const Clip* clip = m_track->clip(i);
            if (!clip) continue;

            auto layoutRect = m_engine->clipRect(
                clip->timelineIn(),
                clip->duration(),
                0.0f,
                static_cast<float>(height()));

            QRectF qRect(layoutRect.x, layoutRect.y, layoutRect.width, layoutRect.height);
            if (qRect.contains(event->pos()))
            {
                bool shift = event->modifiers() & Qt::ShiftModifier;
                emit clipClicked(m_trackIndex, i, shift);
                return;
            }
        }

        // Clicked on background
        int64_t tick = m_engine->pixelXToTime(event->pos().x());
        emit trackBackgroundClicked(m_trackIndex, tick);
    }
}

void TimelineTrackWidget::setInOutPoints(int64_t inPoint, int64_t outPoint)
{
    if (m_inPoint != inPoint || m_outPoint != outPoint) {
        m_inPoint  = inPoint;
        m_outPoint = outPoint;
        update();
    }
}

void TimelineTrackWidget::setEffectHighlightClipId(uint64_t clipId)
{
    if (m_effectHighlightClipId != clipId) {
        m_effectHighlightClipId = clipId;
        update();
    }
}

void TimelineTrackWidget::clearEffectHighlight()
{
    setEffectHighlightClipId(0);
}

void TimelineTrackWidget::setTransitionDropEdgeTick(int64_t tick)
{
    if (m_transitionDropEdgeTick != tick) {
        m_transitionDropEdgeTick = tick;
        update();
    }
}

void TimelineTrackWidget::clearTransitionDropEdge()
{
    setTransitionDropEdgeTick(-1);
}

} // namespace rt
