/*
 * TimelinePanelInternal.h - Shared internal types for TimelinePanel TUs.
 *
 * Contains GhostTrackOverlay and InsertTrackAtCommand, which are
 * implementation-only classes used by both TimelinePanel.cpp and
 * TimelinePanelMouse.cpp.  NOT part of the public API.
 */
#pragma once

#include "timeline/Timeline.h"
#include "timeline/Track.h"
#include "command/Command.h"

#include <QWidget>
#include <QPainter>
#include <QPen>

#include <memory>
#include <string>

namespace rt {

static constexpr const char* kTransitionMimeType = "application/x-roundtable-transition";
static constexpr int kEdgeSnapPixels = 15; // How close cursor must be to a clip edge

// ═════════════════════════════════════════════════════════════════════════════
//  GhostTrackOverlay — transparent overlay widget for drag-to-create previews
// ═════════════════════════════════════════════════════════════════════════════

class GhostTrackOverlay : public QWidget
{
public:
    bool isAbove = true;
    bool reorderMode = false;  // when true, paint as thin insertion bar
    // When true the overlay is positioned over an EXISTING track row
    // (e.g. multi-clip drop on a current track), so the full-rect tint
    // and dashed border are suppressed — only the individual clip
    // outlines are drawn. Premiere doesn't highlight target tracks
    // during drag; it only previews the clips themselves.
    bool onExistingTrack = false;

    // Number of brand-new track rows this ghost represents.  >1 when a
    // multi-stream source (e.g. an 8-track MXF) will spawn several new audio
    // tracks at once — the overlay then draws that many stacked ghost rows.
    int rowCount = 1;

    struct GhostClipPreview {
        int x{0};          // left edge in overlay-local coords
        int width{0};      // width in pixels
        uint32_t color{0}; // RGBA fill color
        QString label;     // clip label text
    };

    void setClipPreviews(const std::vector<GhostClipPreview>& clips)
    {
        m_clipPreviews = clips;
        update();
    }

    explicit GhostTrackOverlay(QWidget* parent)
        : QWidget(parent)
    {
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_NoSystemBackground);
        setAutoFillBackground(false);
        hide();
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);

        if (reorderMode) {
            QColor bar(255, 180, 60, 220);   // bright orange insertion bar
            p.fillRect(rect(), bar);
            QPen pen(QColor(255, 220, 120, 255), 1);
            p.setPen(pen);
            p.drawRect(rect().adjusted(0, 0, -1, -1));
            return;
        }

        // The overlay can represent several stacked new-track rows (multi-
        // stream audio).  Draw each row identically; rowCount==1 is the normal
        // single-track ghost.
        const int rows   = std::max(1, rowCount);
        const int totalH = rect().height();
        constexpr float kPad = 2.0f;

        for (int r = 0; r < rows; ++r) {
            const int y0 = r * totalH / rows;
            const int y1 = (r + 1) * totalH / rows;
            const QRect rowRect(0, y0, rect().width(), y1 - y0);

            // Only paint the track-row tint + dashed border when we're
            // previewing a brand-new track. On an existing track the
            // background must stay clean — Premiere shows only the clip
            // outlines, not a full-row highlight.
            if (!onExistingTrack) {
                QColor ghostColor = isAbove
                    ? QColor(80, 130, 200, 60)    // video = blue tint
                    : QColor(60, 160, 90, 60);    // audio = green tint
                QColor borderColor = isAbove
                    ? QColor(80, 130, 200, 140)
                    : QColor(60, 160, 90, 140);

                p.fillRect(rowRect, ghostColor);

                QPen pen(borderColor, 2, Qt::DashLine);
                p.setPen(pen);
                p.drawRect(rowRect.adjusted(1, 1, -1, -1));
            }

            // Draw clip previews within this ghost row.
            for (const auto& cp : m_clipPreviews) {
                QRectF clipRect(cp.x, rowRect.y() + kPad,
                                cp.width, rowRect.height() - 2.0f * kPad);

                QColor fill((cp.color >> 24) & 0xFF,
                            (cp.color >> 16) & 0xFF,
                            (cp.color >> 8) & 0xFF,
                            160);
                QColor border((cp.color >> 24) & 0xFF,
                              (cp.color >> 16) & 0xFF,
                              (cp.color >> 8) & 0xFF,
                              220);

                p.fillRect(clipRect, fill);
                p.setPen(QPen(border, 1));
                p.drawRect(clipRect);

                if (!cp.label.isEmpty() && cp.width > 20) {
                    p.setPen(QColor(255, 255, 255, 200));
                    QFont f = p.font();
                    f.setPixelSize(10);
                    p.setFont(f);
                    p.drawText(clipRect.adjusted(4, 0, -4, 0),
                               Qt::AlignVCenter | Qt::AlignLeft,
                               p.fontMetrics().elidedText(cp.label, Qt::ElideRight, cp.width - 8));
                }
            }

            // "+ New Track" label only makes sense when previewing a brand-new
            // track row; suppress on existing tracks.
            if (m_clipPreviews.empty() && !onExistingTrack) {
                p.setPen(QColor(200, 200, 200, 180));
                QFont f = p.font();
                f.setPixelSize(12);
                p.setFont(f);
                QString label = isAbove
                    ? QStringLiteral("+ New Video Track")
                    : QStringLiteral("+ New Audio Track");
                p.drawText(rowRect, Qt::AlignCenter, label);
            }
        }
    }

private:
    std::vector<GhostClipPreview> m_clipPreviews;
};

// ═════════════════════════════════════════════════════════════════════════════
//  InsertTrackAtCommand — undoable track insertion for ghost-track drag
//
//  The track has ALREADY been inserted by the caller.  This command just
//  records the index so that undo removes it and redo re-inserts it.
// ═════════════════════════════════════════════════════════════════════════════

class InsertTrackAtCommand : public Command
{
public:
    InsertTrackAtCommand(Timeline* tl, size_t index)
        : m_timeline(tl), m_index(index) {}

    void execute() override
    {
        // On redo: re-insert the track we saved during undo
        if (m_held) {
            m_timeline->insertTrack(m_index, std::move(m_held));
            m_held = nullptr;
        }
        // On first push (via pushWithoutExecute) this is a no-op
        // because the track is already in place.
    }

    void undo() override
    {
        m_held = m_timeline->takeTrack(m_index);
    }

    [[nodiscard]] std::string description() const override { return "Insert ghost track"; }
    [[nodiscard]] int typeId() const override { return -1; }

private:
    Timeline*              m_timeline;
    size_t                 m_index;
    std::unique_ptr<Track> m_held;
};

} // namespace rt
