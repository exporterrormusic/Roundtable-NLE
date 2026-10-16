/*
 * VideoFrameView — paints one video frame aspect-fit on black.
 *
 * Optional hover scrub (Premiere bin style): with it enabled, moving the
 * mouse across the view emits hoverScrubbed(fraction 0..1) and draws a
 * position marker; leaving emits hoverEnded() and the poster frame returns.
 * A left click emits clicked().
 */

#pragma once

#include <QColor>
#include <QImage>
#include <QWidget>

namespace rt {

class VideoFrameView : public QWidget
{
    Q_OBJECT

public:
    explicit VideoFrameView(QWidget* parent = nullptr);

    /// Frame shown whenever no live/scrub frame is on screen.
    void setPoster(const QImage& image);
    /// Show a live frame (playback / scrub).
    void setFrame(const QImage& image);
    /// Show the poster again, retaining the live frame until a poster arrives.
    void showPoster();
    /// Clear both images when switching to a different source.
    void clear();

    void setHoverScrubEnabled(bool enabled);
    /// Draw a 2 px outline (e.g. "selected"); invalid colour = none.
    void setOutline(const QColor& color);
    [[nodiscard]] bool isHoverScrubbing() const noexcept { return m_hoverFraction >= 0.0; }

    /// Frame-request size in device pixels (sharp on HiDPI).
    [[nodiscard]] QSize requestSize() const;

signals:
    void hoverScrubbed(double fraction);
    void hoverEnded();
    void clicked();

protected:
    void paintEvent(QPaintEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void leaveEvent(QEvent* event) override;

private:
    QImage m_poster;
    QImage m_frame;
    bool   m_showPoster{true};
    bool   m_hoverScrub{false};
    QColor m_outline;
    bool   m_pressed{false};
    double m_hoverFraction{-1.0};
};

} // namespace rt
