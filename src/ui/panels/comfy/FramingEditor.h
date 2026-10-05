/*
 * FramingEditor — shows a source frame with the 16:9 output box on top.
 * Drag inside the box to move it, drag a corner or use the wheel to zoom.
 * Values are ShotFraming (Premiere-style scale + centre in the source).
 */

#pragma once

#include "ai/ShotPipeline.h"

#include <QImage>
#include <QWidget>

namespace rt {

class FramingEditor : public QWidget
{
    Q_OBJECT
public:
    explicit FramingEditor(QWidget* parent = nullptr);

    /// `frame` may be a downscaled preview; srcW/srcH are the recording's
    /// native size (the framing is defined against those).
    void setSource(const QImage& frame, int srcW, int srcH);
    void setFraming(const ShotFraming& framing);
    [[nodiscard]] const ShotFraming& framing() const noexcept { return m_framing; }

    /// The output frame as it will be exported (green outside the source).
    [[nodiscard]] static QImage renderOutput(const QImage& frame, int srcW, int srcH,
                                             const ShotFraming& framing, QSize outSize);

signals:
    /// Live while dragging.
    void framingChanged(const rt::ShotFraming& framing);
    /// Once when a drag / wheel gesture ends.
    void framingCommitted(const rt::ShotFraming& framing);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;

private:
    [[nodiscard]] QRectF imageRect() const;   ///< where the source is drawn
    [[nodiscard]] QRectF outputRect() const;  ///< output box, widget coords
    void emitChanged(bool committed);

    QImage      m_frame;
    int         m_srcW{3840};
    int         m_srcH{2160};
    ShotFraming m_framing;

    enum class Drag { None, Move, Corner } m_drag{Drag::None};
    QPointF     m_dragStart;
    ShotFraming m_dragStartFraming;
};

} // namespace rt
