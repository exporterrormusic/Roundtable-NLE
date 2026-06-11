/*
 * Viewport — GPU-accelerated viewport widget for displaying frames.
 *
 * Step 11 (header) / Step 15 (used by monitors)
 *
 * Provides a QWidget that displays decoded video frames as BGRA pixel data.
 * In the future this will be backed by Vulkan; for now it uses QPainter
 * software blitting so the monitor panels can be developed and tested.
 *
 * Features:
 *   - Display CachedFrame pixel data
 *   - Fit / fill / 1:1 zoom modes
 *   - Overlay text (timecode, safe areas, etc.)
 *   - Mouse interaction for scrubbing / transform gizmo
 *   - Aspect-ratio-correct letterboxing
 *   - Transform overlay with bounding box + corner handles for clip position/scale
 */

#pragma once

#include <QImage>
#include <QWidget>
#include <QCursor>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "viewport/OverlayMath.h"

namespace rt {

struct CachedFrame;  // from cache/FrameCache.h

/// How the frame is fitted into the viewport area.
enum class ViewportFitMode : uint8_t
{
    Fit,    ///< Scale to fit entirely within the widget (letterbox/pillarbox)
    Fill,   ///< Scale to fill the widget (may crop edges)
    Actual  ///< 1:1 pixel mapping (may need scroll if frame > widget)
};

/// Viewport display widget — shows a single video frame with optional overlay.
class Viewport : public QWidget
{
    Q_OBJECT

public:
    explicit Viewport(QWidget* parent = nullptr);
    ~Viewport() override;

    // ── Frame display ───────────────────────────────────────────────────

    /// Display a frame from the frame cache (BGRA pixel data).
    void displayFrame(const CachedFrame& frame);

    /// Display a cached frame with zero-copy (wraps pixel data directly).
    /// The shared_ptr keeps the pixel data alive until the next frame.
    void displayFrame(std::shared_ptr<CachedFrame> frame);

    /// Display raw BGRA pixel data.
    void displayRaw(const uint8_t* bgraData, uint32_t width, uint32_t height, uint32_t stride = 0);

    /// Clear the viewport to black.
    void clearFrame();

    /// Is a frame currently displayed?
    [[nodiscard]] bool hasFrame() const noexcept { return m_hasFrame; }

    /// Get the size of the currently displayed frame.
    [[nodiscard]] uint32_t frameWidth() const noexcept { return m_frameWidth; }
    [[nodiscard]] uint32_t frameHeight() const noexcept { return m_frameHeight; }

    // ── Fit mode ────────────────────────────────────────────────────────

    void setFitMode(ViewportFitMode mode);
    [[nodiscard]] ViewportFitMode fitMode() const noexcept { return m_fitMode; }

    // ── Overlay ─────────────────────────────────────────────────────────

    /// Set overlay text (e.g. timecode) drawn in top-left corner.
    void setOverlayText(const QString& text);

    /// Enable/disable safe area guides.
    void setSafeAreasVisible(bool visible);
    [[nodiscard]] bool safeAreasVisible() const noexcept { return m_showSafeAreas; }

    // ── Viewport zoom & pan ─────────────────────────────────────────────

    /// Current view zoom level (1.0 = 100%).
    [[nodiscard]] float viewZoom() const noexcept { return m_viewZoom; }

    /// Set zoom level programmatically (centered, no pan offset).
    void setViewZoom(float zoom);

    /// Reset zoom to 1.0 and pan to (0,0).
    void resetZoomPan();

    // ── Transform overlay ───────────────────────────────────────────────

    /// Show/update the transform overlay for the selected clip.
    void setTransformOverlay(const TransformOverlayInfo& info);

    /// Hide the transform overlay.
    void clearTransformOverlay();

    // ── Coordinate mapping ──────────────────────────────────────────────

    /// Map a widget-space point to frame-space coordinates.
    /// Returns (-1,-1) if the point is outside the frame area.
    [[nodiscard]] QPointF widgetToFrame(const QPointF& widgetPos) const;

    /// Map a frame-space point to widget-space coordinates.
    [[nodiscard]] QPointF frameToWidget(const QPointF& framePos) const;

    /// Get the rectangle in widget space where the frame is drawn.
    [[nodiscard]] QRectF frameRect() const;

    // ── Size hint ───────────────────────────────────────────────────────

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

signals:
    /// Emitted when the user clicks inside the frame area.
    void frameClicked(QPointF framePos, Qt::MouseButton button);

    /// Emitted when the user drags inside the frame area.
    void frameDragged(QPointF framePos, Qt::MouseButtons buttons);

    /// Emitted when the mouse moves inside the frame area.
    void frameHovered(QPointF framePos);

    /// Emitted when the user drags the transform overlay to change position.
    /// Values are in reference resolution (1920×1080) pixel offsets from center.
    void transformPositionChanged(float posX, float posY);

    /// Emitted when the user drag-scales via corner handles.
    /// Values are scale multipliers.
    void transformScaleChanged(float scaleX, float scaleY);

    /// Emitted when the user drag-rotates via the corner rotation zone.
    /// Value is in degrees.
    void transformRotationChanged(float rotation);

    /// Emitted when the transform drag completes (for undo recording).
    /// Carries old and new values so the receiver can build an undo command.
    void transformDragFinished(float oldPosX, float oldPosY, float oldScX, float oldScY, float oldRot,
                               float newPosX, float newPosY, float newScX, float newScY, float newRot);

    /// Emitted when the viewport zoom level changes.
    void viewZoomChanged(float zoom);

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;

private:
    void recalcLayout();

    // ── Transform overlay helpers ───────────────────────────────────────
    /// Compute the 4 widget-space corners of the selected clip's bounding box.
    void computeOverlayCorners(QPointF corners[4]) const;
    /// Invalidate cached corners (called when overlay params or layout change).
    void invalidateCornerCache();
    /// Get cached corners (computes if stale).
    const QPointF* getCachedCorners() const;
    /// Test if widget point is near a corner handle; returns handle index 0-3 or -1.
    int hitTestHandle(const QPointF& widgetPos) const;
    /// Test if widget point is inside the overlay bounding box.
    bool hitTestBody(const QPointF& widgetPos) const;
    /// Draw the transform overlay (bounding box + handles).
    void drawTransformOverlay(QPainter& painter);

    QImage           m_image;          ///< Current frame as QImage
    std::shared_ptr<CachedFrame> m_frameRef;  ///< Keeps zero-copy frame alive
    std::vector<uint8_t> m_rawBuffer;      ///< Owned buffer for displayRaw (keeps QImage data alive)
    bool             m_hasFrame{false};
    uint32_t         m_frameWidth{0};
    uint32_t         m_frameHeight{0};

    ViewportFitMode  m_fitMode{ViewportFitMode::Fit};
    QRectF           m_drawRect;       ///< Where the frame is drawn in widget space

    QString          m_overlayText;
    bool             m_showSafeAreas{false};

    // ── Viewport zoom & pan ──────────────────────────────────────────────
    float            m_viewZoom{1.0f};
    float            m_viewPanX{0.0f};   ///< Pan offset in widget pixels
    float            m_viewPanY{0.0f};
    bool             m_panning{false};
    QPointF          m_panStartPos;

    // ── Transform overlay state ─────────────────────────────────────────
    TransformOverlayInfo m_transformOverlay;

    // Cached corners: valid while overlay params and drawRect unchanged.
    // m_cornerCacheDirty is set to true whenever overlay params or layout change.
    mutable QPointF    m_cachedCorners[4]{QPointF(0, 0)};
    mutable bool       m_cornerCacheDirty{true};

    enum class DragMode : uint8_t { None, MoveBody, ScaleCorner, RotateCorner };
    DragMode        m_dragMode{DragMode::None};
    int             m_dragHandle{-1};       ///< Corner index 0-3 for scale drag
    QPointF         m_dragStartWidget;      ///< Widget position at drag start
    float           m_dragStartPosX{0.0f};  ///< posX at drag start
    float           m_dragStartPosY{0.0f};  ///< posY at drag start
    float           m_dragStartScX{1.0f};   ///< scaleX at drag start
    float           m_dragStartScY{1.0f};   ///< scaleY at drag start
    float           m_dragStartRot{0.0f};   ///< rotation (deg) at drag start
    float           m_dragStartAngle{0.0f}; ///< angle center→mouse at drag start

    /// Corner index (0-3) if the point is in the rotation zone just
    /// OUTSIDE a corner handle (Premiere-style), else -1.
    [[nodiscard]] int hitTestRotate(const QPointF& widgetPos) const;

    /// Premiere-style curved-arrow rotation cursor.
    [[nodiscard]] static QCursor rotateCursor();
};

} // namespace rt
