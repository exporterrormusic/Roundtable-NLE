/*
 * TrackHeader.cpp - Track header widget extracted from TimelinePanel.cpp.
 */

#include "panels/timeline/TimelinePanel.h"
#include "Theme.h"
#include "timeline/Track.h"
#include "panels/timeline/TimelinePanelInternal.h"

#include "spdlog/spdlog.h"

#include <QPainter>
#include <QMouseEvent>
#include <QContextMenuEvent>
#include <QMenu>
#include <QInputDialog>
#include <QLineEdit>
#include <QColorDialog>
#include <QToolTip>
#include <QApplication>

#include <algorithm>
#include <cmath>

namespace rt {

// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
//  TrackHeader
// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•

TrackHeader::TrackHeader(QWidget* parent)
    : QWidget(parent)
{
}

TrackHeader::~TrackHeader()
{
    spdlog::info("[LIFECYCLE] TrackHeader destructor entered: this={} m_trackIndex={} m_track={} (class={})", (void*)this, m_trackIndex, (void*)m_track, metaObject() ? metaObject()->className() : "null");
}

void TrackHeader::setTrack(const Track* track, size_t index)
{
    m_track = track;
    m_trackIndex = index;
    update();
}

void TrackHeader::setHeight(float h)
{
    m_height = h;
    setFixedHeight(static_cast<int>(h));
    update();
}

QSize TrackHeader::sizeHint() const
{
    // Use actual width if already laid out, otherwise fall back to default
    int w = width() > 0 ? width() : kHeaderWidth;
    return {w, static_cast<int>(m_height)};
}

void TrackHeader::paintEvent(QPaintEvent* event)
{
    static thread_local int s_paintDepth = 0;
    if (++s_paintDepth > 5) {
        --s_paintDepth;
        QWidget::paintEvent(event);
        return;
    }

    if (!m_track) { --s_paintDepth; return; }

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    int w = width();
    int h = height();

    const auto& tc = Theme::colors();

    // ── Divider: flat near-black bar, no label ────────────────────────
    // Dividers are pure visual separators — no labels, ever. Even if a
    // stale Track::name() leaked through from an older project file
    // (e.g. "V3" before the isDivider flag was persisted), we don't
    // render it.
    if (m_track->isDivider()) {
        const QColor divBg = (m_track->color() != 0)
                                 ? QColor::fromRgba(m_track->color())
                                 : QColor(10, 10, 12);   // near-black default
        painter.fillRect(rect(), divBg);

        // Side/top/bottom borders (match other tracks).
        painter.setPen(tc.trackDivider);
        painter.drawLine(w - 1, 0, w - 1, h);
        painter.drawLine(0, 0, w, 0);
        painter.drawLine(0, h - 1, w, h - 1);

        --s_paintDepth;
        return;
    }

    // Background
    QColor bg = (m_track->type() == TrackType::Video)
                    ? tc.trackBg : tc.trackBgAlt;
    if (m_track->isCaptionTrack()) {
        const QColor a = tc.accent;  // match the content-area caption tint
        bg = QColor((bg.red()   * 65 + a.red()   * 35) / 100,
                    (bg.green() * 65 + a.green() * 35) / 100,
                    (bg.blue()  * 65 + a.blue()  * 35) / 100);
    }
    painter.fillRect(rect(), bg);

    // Right border
    painter.setPen(tc.trackDivider);
    painter.drawLine(w - 1, 0, w - 1, h);

    // Bottom border — thick accent under the caption track to set it apart.
    if (m_track->isCaptionTrack())
        painter.setPen(QPen(tc.accent, 2));
    painter.drawLine(0, h - 1, w, h - 1);
    painter.setPen(tc.trackDivider);

    // Targeting indicator (left edge patch)
    {
        QRect tgtRect = targetButtonRect();
        bool targeted = m_track->isTargeted();
        QColor patchColor;
        if (m_track->color() != 0) {
            patchColor = targeted ? QColor::fromRgba(m_track->color())
                                  : QColor::fromRgba(m_track->color()).darker(200);
        } else if (m_track->type() == TrackType::Video) {
            patchColor = targeted ? tc.accent : tc.surface2;
        } else {
            patchColor = targeted ? tc.success : tc.surface2;
        }
        painter.setPen(Qt::NoPen);
        painter.setBrush(patchColor);
        painter.drawRect(tgtRect);
    }

    // Track label - centered horizontally, vertically centered with buttons as a group
    painter.setPen(tc.textPrimary);
    QFont labelFont("Segoe UI", 10, QFont::Bold);
    painter.setFont(labelFont);
    int labelLeft = 14;
    // When buttons are beside label (short track), leave room on right
    bool shortTrack = (h < 22 + 20 + 4);
    int labelRight = shortTrack ? std::max(4, lockButtonRect().x() - 2) : (w - 4);

    // Calculate vertical offset to center the label+button block
    int yOff = contentYOffset();
    int labelH = shortTrack ? h : std::min(h, 22);
    int labelY = shortTrack ? 0 : yOff;
    painter.drawText(QRect(labelLeft, labelY, labelRight - labelLeft, labelH),
                     Qt::AlignVCenter | Qt::AlignHCenter,
                     QString::fromStdString(m_track->name()));

    // Buttons - always drawn (wrapping layout handles fit)
    {
        QFont btnFont("Segoe UI", 8, QFont::DemiBold);
        painter.setFont(btnFont);

        auto drawButton = [&](const QRect& r, const QString& text, bool active, QColor activeColor)
        {
            if (r.right() > w || r.bottom() > h) return; // skip if outside widget
            painter.setPen(Qt::NoPen);
            painter.setBrush(active ? activeColor : tc.surface3);
            painter.drawRoundedRect(r, 3, 3);
            painter.setPen(active ? tc.textBright : tc.textDisabled);
            painter.drawText(r, Qt::AlignCenter, text);
        };

        QString muteLabel = (m_track->type() == TrackType::Video)
                             ? QStringLiteral("V")
                             : QStringLiteral("M");
        drawButton(lockButtonRect(),     QStringLiteral("L"),  m_track->isLocked(),    tc.error);
        drawButton(muteButtonRect(),     muteLabel,            m_track->isMuted(),     tc.warning);
        drawButton(soloButtonRect(),     QStringLiteral("S"),  m_track->isSoloed(),    tc.success);
        drawButton(syncLockButtonRect(), QStringLiteral("SL"), m_track->isSyncLocked(), tc.accent);
    }

    --s_paintDepth;
}

bool TrackHeader::event(QEvent* e)
{
    if (e->type() == QEvent::ToolTip && m_track) {
        auto* he = static_cast<QHelpEvent*>(e);
        QPoint pos = he->pos();
        QString tip;
        if (lockButtonRect().contains(pos))
            tip = m_track->isLocked() ? "Unlock Track" : "Lock Track";
        else if (muteButtonRect().contains(pos))
            tip = m_track->isMuted() ? (m_track->type() == TrackType::Video ? "Show Track" : "Unmute Track")
                                     : (m_track->type() == TrackType::Video ? "Hide Track" : "Mute Track");
        else if (soloButtonRect().contains(pos))
            tip = m_track->isSoloed() ? "Unsolo Track" : "Solo Track";
        else if (syncLockButtonRect().contains(pos))
            tip = m_track->isSyncLocked() ? "Disable Sync Lock" : "Enable Sync Lock";
        else if (targetButtonRect().contains(pos))
            tip = m_track->isTargeted() ? "Untarget Track" : "Target Track";

        if (!tip.isEmpty()) {
            QToolTip::showText(he->globalPos(), tip, this);
            return true;
        }
        QToolTip::hideText();
        e->ignore();
        return true;
    }
    return QWidget::event(e);
}

void TrackHeader::resizeEvent(QResizeEvent* event)
{
    static thread_local bool s_inResize = false;
    if (s_inResize) {
        QWidget::resizeEvent(event);
        return;
    }
    s_inResize = true;

    // Button positions are computed dynamically from width()/height(),
    // so any size change must trigger a repaint to reflow the layout.
    QWidget::resizeEvent(event);
    update();

    s_inResize = false;
}

void TrackHeader::mousePressEvent(QMouseEvent* event)
{
    if (!m_track || event->button() != Qt::LeftButton) return;

    QPoint pos = event->pos();

    // Check if click is on the bottom resize grip.
    // Dividers are too short (≈15 px) for a grip zone to be meaningful —
    // any click would land in it and turn reorder drags into resize drags,
    // which is why dragging a divider was growing it instead of moving it.
    if (!m_track->isDivider() && pos.y() >= height() - kResizeGripHeight) {
        m_resizeDragging = true;
        m_resizeDragStartY = event->globalPosition().toPoint().y();
        m_resizeDragStartHeight = m_height;
        setCursor(Qt::SplitVCursor);
        event->accept();
        return;
    }

    // Dividers: any press (not resize) begins reorder drag — EXCEPT for
    // the permanent V/A boundary divider, which is locked in place.
    if (m_track->isDivider()) {
        if (m_track->isPermanentDivider()) {
            event->accept();
            return;
        }
        m_reorderPressed = true;
        m_reorderActive = false;
        m_reorderPressGlobal = event->globalPosition().toPoint();
        event->accept();
        return;
    }

    if (targetButtonRect().contains(pos))
        emit targetToggled(m_trackIndex, !m_track->isTargeted());
    else if (lockButtonRect().contains(pos))
        emit lockToggled(m_trackIndex, !m_track->isLocked());
    else if (muteButtonRect().contains(pos))
        emit muteToggled(m_trackIndex, !m_track->isMuted());
    else if (soloButtonRect().contains(pos))
        emit soloToggled(m_trackIndex, !m_track->isSoloed());
    else if (syncLockButtonRect().contains(pos))
        emit syncLockToggled(m_trackIndex, !m_track->isSyncLocked());
    else if (m_track->isCaptionTrack()) {
        // The caption (subtitle) track is pinned at the top and cannot be
        // reordered — swallow the press without arming a drag.
        event->accept();
    }
    else {
        // Click landed on the label / blank area → arm a reorder drag.
        m_reorderPressed = true;
        m_reorderActive = false;
        m_reorderPressGlobal = event->globalPosition().toPoint();
        event->accept();
    }
}

void TrackHeader::mouseMoveEvent(QMouseEvent* event)
{
    if (m_resizeDragging) {
        int dy = event->globalPosition().toPoint().y() - m_resizeDragStartY;
        float newHeight = std::clamp(m_resizeDragStartHeight + static_cast<float>(dy),
                                     kMinTrackHeight, kMaxTrackHeight);
        m_height = newHeight;
        setFixedHeight(static_cast<int>(newHeight));
        emit heightChanged(m_trackIndex, newHeight);
        event->accept();
        return;
    }

    // Promote pending press into an active reorder drag once past threshold
    if (m_reorderPressed) {
        QPoint gp = event->globalPosition().toPoint();
        if (!m_reorderActive) {
            if ((gp - m_reorderPressGlobal).manhattanLength()
                    >= QApplication::startDragDistance()) {
                m_reorderActive = true;
                setCursor(Qt::ClosedHandCursor);
                emit reorderDragStarted(m_trackIndex);
            }
        }
        if (m_reorderActive) {
            emit reorderDragMoved(m_trackIndex, gp);
            event->accept();
            return;
        }
    }

    // Show resize cursor when hovering near bottom edge
    QPoint pos = event->pos();
    if (!m_track || !m_track->isDivider()) {
        if (pos.y() >= height() - kResizeGripHeight) {
            setCursor(Qt::SplitVCursor);
        } else {
            setCursor(Qt::ArrowCursor);
        }
    } else {
        setCursor(Qt::OpenHandCursor);
    }
}

void TrackHeader::mouseReleaseEvent(QMouseEvent* event)
{
    if (m_resizeDragging && event->button() == Qt::LeftButton) {
        m_resizeDragging = false;
        setCursor(Qt::ArrowCursor);
        event->accept();
        return;
    }

    if (m_reorderPressed && event->button() == Qt::LeftButton) {
        bool wasActive = m_reorderActive;
        QPoint gp = event->globalPosition().toPoint();
        m_reorderPressed = false;
        m_reorderActive = false;
        setCursor(Qt::ArrowCursor);
        if (wasActive) {
            emit reorderDragFinished(m_trackIndex, gp, true);
            event->accept();
            return;
        }
    }
}

void TrackHeader::mouseDoubleClickEvent(QMouseEvent* event)
{
    if (!m_track) return;
    if (m_track->isDivider()) return;  // dividers have no collapse/buttons

    QPoint pos = event->pos();

    // If double-click is on a button, treat as a second press instead of collapse
    if (targetButtonRect().contains(pos) ||
        lockButtonRect().contains(pos) ||
        muteButtonRect().contains(pos) ||
        soloButtonRect().contains(pos) ||
        syncLockButtonRect().contains(pos))
    {
        mousePressEvent(event);
        return;
    }

    emit collapseToggled(m_trackIndex, !m_track->isCollapsed());
}

void TrackHeader::contextMenuEvent(QContextMenuEvent* event)
{
    if (!m_track) return;

    // Dividers: minimal menu — Add/Delete only. The permanent V/A
    // boundary divider hides "Delete Divider" (it's auto-managed and
    // can't be removed by hand).
    if (m_track->isDivider()) {
        QMenu dm(this);
        QMenu* addMenu = dm.addMenu("Add Track");
        QAction* aV  = addMenu->addAction("Add Video Track");
        QAction* aA  = addMenu->addAction("Add Audio Track");
        QAction* aD  = addMenu->addAction("Add Divider");
        QAction* aDel = m_track->isPermanentDivider()
                            ? nullptr
                            : dm.addAction("Delete Divider");
        QAction* chosen = dm.exec(event->globalPos());
        if      (chosen == aV)   emit addTrackRequested(true, true, m_trackIndex);
        else if (chosen == aA)   emit addTrackRequested(false, true, m_trackIndex);
        else if (chosen == aD)   emit addDividerRequested(true, m_trackIndex);
        else if (aDel && chosen == aDel) emit deleteTrackRequested(m_trackIndex);
        return;
    }

    QMenu menu(this);

    QAction* renameAction = menu.addAction("Rename Track...");
    QAction* collapseAction = menu.addAction(
        m_track->isCollapsed() ? "Expand Track" : "Collapse Track");

    menu.addSeparator();

    QMenu* addMenu = menu.addMenu("Add Track");
    QAction* addVideoTrack = addMenu->addAction("Add Video Track");
    QAction* addAudioTrack = addMenu->addAction("Add Audio Track");
    QAction* addDividerTrack = addMenu->addAction("Add Divider");

    // The caption track is pinned/auto-managed — it can't be deleted by hand.
    QAction* deleteAction = m_track->isCaptionTrack()
                                ? nullptr
                                : menu.addAction("Delete Track");

    menu.addSeparator();

    QMenu* sizeMenu = menu.addMenu("Track Size");
    QAction* sizeSmall  = sizeMenu->addAction("Small (30)");
    QAction* sizeMedium = sizeMenu->addAction("Medium (60)");
    QAction* sizeLarge  = sizeMenu->addAction("Large (100)");
    QAction* sizeXL     = sizeMenu->addAction("Extra Large (150)");

    menu.addSeparator();

    QMenu* colorMenu = menu.addMenu("Track Color");
    QAction* colorMango   = colorMenu->addAction("Mango");
    QAction* colorForest  = colorMenu->addAction("Forest");
    QAction* colorCerulean = colorMenu->addAction("Cerulean");
    QAction* colorIris    = colorMenu->addAction("Iris");
    QAction* colorRose    = colorMenu->addAction("Rose");
    QAction* colorCustom  = colorMenu->addAction("Custom...");
    colorMenu->addSeparator();
    QAction* colorClear   = colorMenu->addAction("Clear Color");

    QAction* chosen = menu.exec(event->globalPos());
    if (chosen == renameAction) {
        bool ok = false;
        QString newName = QInputDialog::getText(
            this, "Rename Track",
            "Track name:",
            QLineEdit::Normal,
            QString::fromStdString(m_track->name()),
            &ok);
        if (ok && !newName.isEmpty()) {
            emit trackRenamed(m_trackIndex, newName);
        }
    }
    else if (chosen == addVideoTrack) { emit addTrackRequested(true, true, m_trackIndex); }
    else if (chosen == addAudioTrack) { emit addTrackRequested(false, true, m_trackIndex); }
    else if (chosen == addDividerTrack) { emit addDividerRequested(true, m_trackIndex); }
    else if (deleteAction && chosen == deleteAction)  { emit deleteTrackRequested(m_trackIndex); }
    else if (chosen == collapseAction) { emit collapseToggled(m_trackIndex, !m_track->isCollapsed()); }
    else if (chosen == sizeSmall)  { emit trackSizePresetRequested(m_trackIndex, 30.0f); }
    else if (chosen == sizeMedium) { emit trackSizePresetRequested(m_trackIndex, 60.0f); }
    else if (chosen == sizeLarge)  { emit trackSizePresetRequested(m_trackIndex, 100.0f); }
    else if (chosen == sizeXL)     { emit trackSizePresetRequested(m_trackIndex, 150.0f); }
    else if (chosen == colorMango)   { const_cast<Track*>(m_track)->setColor(0xFFFF9800); update(); }
    else if (chosen == colorForest)  { const_cast<Track*>(m_track)->setColor(0xFF4CAF50); update(); }
    else if (chosen == colorCerulean){ const_cast<Track*>(m_track)->setColor(0xFF2196F3); update(); }
    else if (chosen == colorIris)    { const_cast<Track*>(m_track)->setColor(0xFF9C27B0); update(); }
    else if (chosen == colorRose)    { const_cast<Track*>(m_track)->setColor(0xFFE91E63); update(); }
    else if (chosen == colorCustom) {
        QColor initial = m_track->color() != 0 ? QColor::fromRgba(m_track->color()) : Qt::white;
        QColor picked = QColorDialog::getColor(initial, this, "Track Color");
        if (picked.isValid()) {
            const_cast<Track*>(m_track)->setColor(picked.rgba());
            update();
        }
    }
    else if (chosen == colorClear)   { const_cast<Track*>(m_track)->setColor(0); update(); }
}

QRect TrackHeader::targetButtonRect() const
{
    return QRect(0, 0, 10, height());
}

// Calculate total content height (label + button rows) and vertical offset to center it.
int TrackHeader::contentYOffset() const
{
    static constexpr int labelH = 22;
    static constexpr int bh = 20;
    static constexpr int gapY = 2;
    static constexpr int numButtons = 4;
    static constexpr int bw = 26;
    static constexpr int gapX = 3;
    static constexpr int leftPad = 12;

    int w = width();
    int h = height();

    if (h < labelH + bh + 4) return 0; // short track, no offset

    int availW = w - leftPad - 4;
    int colsPerRow = std::max(1, (availW + gapX) / (bw + gapX));
    if (colsPerRow > numButtons) colsPerRow = numButtons;
    int numRows = (numButtons + colsPerRow - 1) / colsPerRow;

    int contentH = labelH + numRows * bh + (numRows - 1) * gapY;
    return std::max(0, (h - contentH) / 2);
}

// Button layout: wraps to multiple rows when width is narrow.
// Buttons are centered horizontally within the available width.
// When track is short, buttons sit beside the label instead of below.
QRect TrackHeader::buttonRect(int index) const
{
    static constexpr int bw = 26;
    static constexpr int bh = 20;
    static constexpr int gapX = 3;
    static constexpr int gapY = 2;
    static constexpr int labelH = 22;
    static constexpr int numButtons = 4;
    static constexpr int leftPad = 12; // left padding past target indicator

    int w = width();
    int h = height();

    // Available width for buttons (after target indicator + padding)
    int availW = w - leftPad - 4; // 4px right padding

    // If track is short (collapsed or small), put buttons to the right of the label
    if (h < labelH + bh + 4) {
        int totalBtnW = numButtons * bw + (numButtons - 1) * gapX;
        int startX = w - totalBtnW - 4;
        if (startX < leftPad + 20) startX = leftPad + 20;
        int y = std::max(0, (h - bh) / 2);
        return QRect(startX + index * (bw + gapX), y, bw, bh);
    }

    // Normal: buttons below the label, wrapping to rows, centered
    int colsPerRow = std::max(1, (availW + gapX) / (bw + gapX));
    if (colsPerRow > numButtons) colsPerRow = numButtons;

    int row = index / colsPerRow;
    int col = index % colsPerRow;

    // How many buttons in this row?
    int buttonsThisRow = std::min(colsPerRow, numButtons - row * colsPerRow);
    int rowWidth = buttonsThisRow * bw + (buttonsThisRow - 1) * gapX;

    // Center the row within available width
    int rowStartX = leftPad + (availW - rowWidth) / 2;

    // Vertical offset to center the whole label+buttons block
    int yOff = contentYOffset();

    int x = rowStartX + col * (bw + gapX);
    int y = yOff + labelH + row * (bh + gapY);

    return QRect(x, y, bw, bh);
}

QRect TrackHeader::lockButtonRect() const     { return buttonRect(0); }
QRect TrackHeader::muteButtonRect() const     { return buttonRect(1); }
QRect TrackHeader::soloButtonRect() const     { return buttonRect(2); }
QRect TrackHeader::syncLockButtonRect() const { return buttonRect(3); }

} // namespace rt
