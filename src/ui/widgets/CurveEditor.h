/*
 * CurveEditor — Premiere Pro–style color curve editor widget.
 *
 * Displays a cubic spline curve with draggable control points.
 * Supports multiple channels (Master, R, G, B) with tab switching.
 * Each curve generates a 256-entry LUT for GPU processing.
 */

#pragma once

#include <QWidget>
#include <QComboBox>
#include <QPointF>
#include <QColor>

#include <array>
#include <vector>

namespace rt {

class CurveEditor : public QWidget
{
    Q_OBJECT

public:
    /// Channel indices.
    enum Channel : int { Master = 0, Red = 1, Green = 2, Blue = 3, ChannelCount = 4 };

    explicit CurveEditor(QWidget* parent = nullptr);
    ~CurveEditor() override = default;

    /// Set the active channel (determines display color and which curve is edited).
    void setActiveChannel(Channel ch);
    [[nodiscard]] Channel activeChannel() const noexcept { return m_activeChannel; }

    /// Get the 256-entry LUT for a channel (output values 0–1).
    [[nodiscard]] std::array<float, 256> lut(Channel ch) const;

    /// Load an existing effect LUT without emitting an edit signal. The exact
    /// LUT remains authoritative until that channel is edited; a compact set
    /// of points is used only for its on-screen representation.
    void setLut(Channel ch, const std::array<float, 256>& lut);

    /// Reset a single channel to linear identity.
    void resetChannel(Channel ch);

    /// Reset all channels to linear identity.
    void resetAll();

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

signals:
    /// Emitted whenever any curve point is moved (for live preview).
    void curveChanged();

    /// Emitted when a drag finishes (for undo).
    void curveCommitted();

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;

private:
    struct ControlPoint {
        float x{0.0f}; // 0–1
        float y{0.0f}; // 0–1
    };

    /// Evaluate a channel's spline at position t (0–1).
    float evaluate(Channel ch, float t) const;

    /// Convert widget coords to normalized coords and back.
    QPointF toWidget(float x, float y) const;
    QPointF fromWidget(const QPointF& pos) const;

    /// Find closest control point index for a position (-1 if none close).
    int hitTest(Channel ch, const QPointF& widgetPos) const;

    // Per-channel control points (always sorted by x).
    std::array<std::vector<ControlPoint>, ChannelCount> m_points;
    std::array<std::array<float, 256>, ChannelCount> m_loadedLuts{};
    std::array<bool, ChannelCount> m_hasLoadedLut{};

    Channel m_activeChannel{Master};

    // Drag state
    int  m_dragIndex{-1};
    bool m_dragging{false};
};

} // namespace rt
