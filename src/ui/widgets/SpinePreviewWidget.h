/*
 * SpinePreviewWidget — QPainter-based animated Spine character preview.
 *
 * Renders one or more Spine characters (from SpineEngine::extractMeshes())
 * composited together with background images, using QPainter and a
 * software triangle rasteriser.  A 60fps QTimer drives animation updates.
 *
 * In multi-layer mode (ShotComposer), supports:
 *   - Photoshop-style transform overlay (bounding box + corner handles)
 *   - Mouse drag to reposition characters
 *   - Corner-drag to scale characters
 *   - Mouse-wheel zoom / middle-button pan
 */

#pragma once

#ifdef ROUNDTABLE_HAS_SPINE

#include <QElapsedTimer>
#include <QImage>
#include <QPixmap>
#include <QSet>
#include <QTimer>
#include <QWidget>

#include <functional>
#include <memory>
#include <vector>

namespace rt {

class SpineEngine;

/// A layer for multi-layer compositing in the preview.
/// Can be either a character (Spine engine) or a background/video image.
struct PreviewCharLayer
{
    SpineEngine*        engine   = nullptr;
    std::vector<QImage> textures;           ///< One QImage per atlas page
    float posX     = 0.5f;   ///< Horizontal position (0–1, 0.5 = center)
    float posY     = 0.75f;  ///< Vertical position (0–1, 0.75 = default)
    float scale    = 1.0f;   ///< Scale multiplier
    float rotation = 0.0f;   ///< Rotation in degrees
    bool  flipX    = false;  ///< Horizontal flip
    bool  flipY    = false;  ///< Vertical flip
    float opacity  = 1.0f;   ///< Opacity (0–1)
    bool  visible  = true;
    int   layerIndex = -1;   ///< Index in ShotPreset::layerOrder() (for selection)

    // ── Crop (percentage 0–100) ─────────────────────────────────────────
    float cropLeft   = 0.0f;
    float cropRight  = 0.0f;
    float cropTop    = 0.0f;
    float cropBottom = 0.0f;
    float blur       = 0.0f;   ///< Gaussian blur radius (0–100)
    // cached bounds for this character's skeleton
    bool  boundsCached = false;
    float boundsX = 0, boundsY = 0, boundsW = 0, boundsH = 0;

    // ── Background layer support ────────────────────────────────────────
    bool   isBackground = false;    ///< If true, draw backgroundImage instead of Spine
    bool   isVideoCharacter = false; ///< If true, use character-style sizing (fit to 85% height)
    QImage backgroundImage;         ///< Image to draw for background layers
    // Scaled cache (avoid rescaling every frame)
    QImage scaledBgCache;
    int    scaledBgCacheW = 0;
    int    scaledBgCacheH = 0;
    // Blurred cache (avoid re-blurring every frame for static images)
    QImage blurredBgCache;
    float  blurredBgBlurVal = -1.0f;  ///< blur value used to produce blurredBgCache

    // ── Video playback ───────────────────────────────────────────────
    /// Callback that advances video by dt seconds and returns the new frame.
    /// If set, called every timer tick. Return null QImage to keep current frame.
    std::function<QImage(float dt)> videoFrameProvider;

    /// Optional per-frame "breathing" transform for PNG puppets, mirroring the
    /// timeline's PngPuppetClip breathing.  Writes an additive position offset in
    /// REF (1920x1080) pixels, a scale multiplier, and an additive rotation in
    /// degrees.  Queried at paint time (after videoFrameProvider has advanced the
    /// puppet's clock for this tick).
    std::function<void(float& dxRef, float& dyRef, float& scaleMul, float& rotDeg)>
        breathProvider;
};

class SpinePreviewWidget : public QWidget
{
    Q_OBJECT

public:
    explicit SpinePreviewWidget(QWidget* parent = nullptr);
    ~SpinePreviewWidget() override;

    // ── Single-engine API (backward compat for Characters tab) ──────────

    void setSpineEngine(SpineEngine* engine);
    void loadTextures();

    // ── Multi-layer API (for ShotComposer) ──────────────────────────────

    void setCharacterLayers(std::vector<PreviewCharLayer> layers);
    void clearCharacterLayers();

    void startAnimation();
    void stopAnimation();
    [[nodiscard]] bool isAnimating() const noexcept { return m_timer.isActive(); }

    /// Set the selected layer index (for transform overlay highlight).
    void setSelectedLayer(int layerIndex);

    /// Set the set of selected layer indices (for multi-layer transform).
    void setSelectedLayers(const QSet<int>& layerIndices);

    /// Set the camera transform (zoom, panX, panY) from the shot preset.
    /// This sets the viewport zoom/pan programmatically (same as mouse wheel/drag).
    void setCameraTransform(float zoom, float panX = 0.0f, float panY = 0.0f);

    /// Get the current viewport zoom level.
    [[nodiscard]] float viewZoom() const noexcept { return m_viewZoom; }
    /// Get the current viewport horizontal pan offset (in pixels).
    [[nodiscard]] float viewPanX() const noexcept { return m_viewPanX; }
    /// Get the current viewport vertical pan offset (in pixels).
    [[nodiscard]] float viewPanY() const noexcept { return m_viewPanY; }

    /// Reset viewport zoom/pan to 1:1 default.
    void resetViewport();

    // ── Appearance ──────────────────────────────────────────────────────

    void setBackgroundColor(const QColor& color);
    void setBackgroundImage(const QImage& image);
    void clearBackgroundImage();

    /// Toggle safe area guides (action-safe 90% + title-safe 80%).
    void setSafeAreasVisible(bool visible);
    [[nodiscard]] bool safeAreasVisible() const noexcept { return m_showSafeAreas; }

    /// Show a semi-transparent drag overlay thumbnail at a given position.
    void setDragOverlay(const QPixmap& pixmap, const QPoint& pos);
    /// Clear the drag overlay.
    void clearDragOverlay();

signals:
    /// Emitted when the user drags/scales a character in the viewport.
    /// posX, posY are normalized 0–1; scale is a multiplier.
    void layerTransformChanged(int layerIndex, float posX, float posY, float scale);

    /// Emitted when the user ALT+drags an edge to crop.
    void layerCropChanged(int layerIndex, float cropLeft, float cropRight, float cropTop, float cropBottom);

    /// Emitted when a drag/scale operation begins (for undo snapshots).
    void dragStarted();

    /// Emitted when the user clicks directly on a layer in the preview.
    /// ShotComposer uses this to synchronize the layer-list selection.
    void layerClicked(int layerIndex);

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;

private:
    void onTimerTick();
    void renderSingleEngine(QPainter& painter);
    void renderMultiLayer(QPainter& painter);
    void drawTransformOverlay(QPainter& painter);

    // ── Hit testing / geometry helpers for overlay ───────────────────────

    /// Compute the screen-space transform of the 16:9 logical canvas
    /// (origin = top-left in widget pixels, w/h = on-screen size).
    /// This is the SINGLE source of truth for the canvas: content rendering,
    /// the overlay frame border, hit-testing and dragging must all use it so
    /// the shot frame and its contents stay locked together at every zoom.
    void computeCanvasTransform(float& originX, float& originY,
                                float& w, float& h) const;

    /// Get the screen-space bounding rect for a character layer.
    QRect layerScreenRect(const PreviewCharLayer& layer) const;

    /// -1 = no hit, otherwise index into m_layers
    int hitTestLayer(const QPoint& pos) const;

    /// Returns handle name: "", "tl", "tr", "bl", "br"
    QString hitTestHandle(const QPoint& pos) const;

    /// Returns edge name for crop: "", "left", "right", "top", "bottom"
    QString hitTestEdge(const QPoint& pos) const;

    // ── Single-engine state (backward compat) ───────────────────────────
    SpineEngine*           m_engine{nullptr};
    std::vector<QImage>    m_textures;
    bool  m_boundsCached{false};
    float m_cachedBoundsX{0}, m_cachedBoundsY{0};
    float m_cachedBoundsW{0}, m_cachedBoundsH{0};

    // ── Multi-layer state ───────────────────────────────────────────────
    std::vector<PreviewCharLayer> m_layers;
    int   m_selectedLayerIdx{-1};   ///< layerIndex of the selected layer
    QSet<int> m_selectedLayerIndices;   ///< All selected layer indices (for multi-transform)

    /// Initial transform state for each selected layer at drag/scale start.
    /// Key = index into m_layers array.
    struct DragInitState { float posX; float posY; float scale; };
    std::vector<DragInitState> m_dragInitStates;
    float m_groupCenterX{0.0f};     ///< Centre of selected group at drag start (normalised)
    float m_groupCenterY{0.0f};

    // ── Viewport zoom & pan ─────────────────────────────────────────────
    float m_viewZoom{1.0f};
    float m_viewPanX{0.0f};         ///< Pixels
    float m_viewPanY{0.0f};         ///< Pixels

    // ── Drag state ──────────────────────────────────────────────────────
    bool    m_dragging{false};
    bool    m_scaling{false};
    bool    m_panning{false};
    bool    m_cropping{false};           ///< ALT+drag crop from edge
    QString m_scaleHandle;               ///< "tl","tr","bl","br"
    QString m_cropEdge;                  ///< "left","right","top","bottom"
    int     m_dragLayerIdx{-1};          ///< index into m_layers
    QPoint  m_dragStartPos;
    float   m_dragInitPosX{0}, m_dragInitPosY{0}, m_dragInitScale{0};
    float   m_cropInitLeft{0}, m_cropInitRight{0};
    float   m_cropInitTop{0}, m_cropInitBottom{0};

    // ── Drag overlay (semi-transparent preview during drag-and-drop) ───
    QPixmap m_dragOverlayPixmap;     ///< Thumbnail to show during drag
    QPoint  m_dragOverlayPos;        ///< Widget-local position to draw at
    bool    m_dragOverlayVisible{false};

    // ── Common ──────────────────────────────────────────────────────────
    QTimer                 m_timer;
    QColor                 m_bgColor{30, 30, 38};
    QElapsedTimer          m_elapsed;
    QImage                 m_backBuffer;
    QImage                 m_bgImage;

    bool                   m_showSafeAreas{false};

    static constexpr int HANDLE_SIZE = 10;
};

} // namespace rt

#endif // ROUNDTABLE_HAS_SPINE
