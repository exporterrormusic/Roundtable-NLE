/*
 * KeyframeEditor — graph editor panel for keyframe curves.
 *
 * Displays animated properties as 2D curves (time × value).
 * Supports:
 *  - GPU-style bezier curve rendering via QPainter
 *  - Drag keyframe handles (time + value)
 *  - Bezier tangent handles for curve shape
 *  - Interpolation modes: linear, bezier, hold
 *  - Add / delete keyframes
 *  - Box select + multi-select
 *  - Copy / paste keyframes
 *  - Fit view to selected curves
 *  - Undo / redo through CommandStack
 */

#pragma once

#include <QWidget>
#include <QPointF>
#include <QRectF>
#include <QColor>
#include <QMenu>
#include <QAction>

#include <cstdint>
#include <functional>
#include <set>
#include <string>
#include <vector>

#include "timeline/Keyframe.h"

namespace rt {

// Forward declarations
template <typename T> class KeyframeTrack;
class Clip;
class CommandStack;

// ═══════════════════════════════════════════════════════════════════════════
//  CurveEntry — describes one curve the editor is displaying
// ═══════════════════════════════════════════════════════════════════════════

struct CurveEntry
{
    std::string            name;
    QColor                 color;
    KeyframeTrack<float>*  track{nullptr};
    bool                   visible{true};
};

// ═══════════════════════════════════════════════════════════════════════════
//  SelectedKey — identity of a selected keyframe
// ═══════════════════════════════════════════════════════════════════════════

struct SelectedKey
{
    int curveIndex{-1};
    int keyIndex{-1};

    bool operator<(const SelectedKey& o) const noexcept
    {
        if (curveIndex != o.curveIndex) return curveIndex < o.curveIndex;
        return keyIndex < o.keyIndex;
    }
    bool operator==(const SelectedKey& o) const noexcept
    {
        return curveIndex == o.curveIndex && keyIndex == o.keyIndex;
    }
};

// ═══════════════════════════════════════════════════════════════════════════
//  KeyframeEditor widget
// ═══════════════════════════════════════════════════════════════════════════

class KeyframeEditor : public QWidget
{
    Q_OBJECT

public:
    explicit KeyframeEditor(QWidget* parent = nullptr);
    ~KeyframeEditor() override;

    // ── Clip binding ────────────────────────────────────────────────────
    /// Bind to a clip — populates curves from its keyframeable properties.
    void setClip(Clip* clip);
    void setCurves(std::vector<CurveEntry> curves, Clip* owningClip = nullptr);
    [[nodiscard]] Clip* clip() const noexcept { return m_clip; }

    // ── Command stack ───────────────────────────────────────────────────
    void setCommandStack(CommandStack* stack) noexcept { m_commandStack = stack; }
    [[nodiscard]] CommandStack* commandStack() const noexcept { return m_commandStack; }

    // ── Curves ──────────────────────────────────────────────────────────
    [[nodiscard]] int                       curveCount() const noexcept;
    [[nodiscard]] const CurveEntry&         curve(int i) const;
    [[nodiscard]] const std::vector<CurveEntry>& curves() const noexcept { return m_curves; }
    void setCurveVisible(int index, bool visible);

    // ── Selection ───────────────────────────────────────────────────────
    [[nodiscard]] const std::set<SelectedKey>& selectedKeys() const noexcept { return m_selection; }
    void clearSelection();
    void selectKey(int curveIndex, int keyIndex, bool addToSelection = false);
    void selectAll();
    void boxSelect(const QRectF& graphRect, bool addToSelection = false);

    // ── Keyframe operations ─────────────────────────────────────────────
    void addKeyframe(int curveIndex, int64_t time, float value);
    void deleteSelectedKeyframes();
    /// Set interpolation on all selected keyframes. `interpMode` is an
    /// InterpMode value (0=Linear, 1=Bezier, 2=Hold, 3=AutoBezier,
    /// 4=ContinuousBezier, 5=EaseIn, 6=EaseOut).
    void setInterpolation(int interpMode);

    // ── Clipboard ───────────────────────────────────────────────────────
    void copySelectedKeyframes();
    void pasteKeyframes(int64_t time);
    [[nodiscard]] bool hasClipboardData() const noexcept { return !m_clipboard.empty(); }

    // ── View ────────────────────────────────────────────────────────────
    void fitViewToAll();
    void fitViewToSelection();
    void setViewRange(double timeMin, double timeMax, double valueMin, double valueMax);

    [[nodiscard]] double viewTimeMin()  const noexcept { return m_viewTimeMin; }
    [[nodiscard]] double viewTimeMax()  const noexcept { return m_viewTimeMax; }
    [[nodiscard]] double viewValueMin() const noexcept { return m_viewValueMin; }
    [[nodiscard]] double viewValueMax() const noexcept { return m_viewValueMax; }

    // ── Coordinate conversion ───────────────────────────────────────────
    /// Graph coordinates → widget pixel coordinates (value pane).
    [[nodiscard]] QPointF graphToPixel(double time, double value) const;
    /// Widget pixel coordinates → graph coordinates (value pane).
    [[nodiscard]] QPointF pixelToGraph(double px, double py) const;

    /// Velocity-pane coordinate conversions. Time axis is shared with the
    /// value pane; the value axis is auto-scaled from observed slopes.
    [[nodiscard]] QPointF graphToPixelVelocity(double time, double velocity) const;
    [[nodiscard]] QPointF pixelToGraphVelocity(double px, double py) const;

    // ── Velocity (speed) graph ─────────────────────────────────────────
    /// Show or hide the expandable velocity graph below the value graph,
    /// matching Premiere Pro's "Show Speed Graph" toggle.
    void setShowVelocityGraph(bool on) noexcept;
    [[nodiscard]] bool showVelocityGraph() const noexcept { return m_showVelocityGraph; }

    // ── Accessors for test introspection ────────────────────────────────
    [[nodiscard]] bool isDragging()  const noexcept { return m_dragging; }
    [[nodiscard]] bool isBoxSelecting() const noexcept { return m_boxSelecting; }

signals:
    void keyframeChanged();
    void selectionChanged();
    void viewChanged();

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;

private:
    void setupUI();
    void rebuildCurves();

    // ── Drawing helpers ─────────────────────────────────────────────────
    void drawGrid(QPainter& p);
    void drawCurve(QPainter& p, int curveIdx);
    void drawKeyframeHandles(QPainter& p, int curveIdx);
    void drawBezierHandles(QPainter& p, int curveIdx);
    void drawBoxSelection(QPainter& p);
    void drawVelocityGraph(QPainter& p);

    /// Auto-fit the velocity y-range from the observed slopes; called when
    /// the velocity graph is shown or when keyframes change.
    void recomputeVelocityRange();

    // ── Hit testing ─────────────────────────────────────────────────────
    static constexpr double kHitRadius = 8.0;
    static constexpr double kTangentHitRadius = 6.0;

    struct HitResult
    {
        int  curveIndex{-1};
        int  keyIndex{-1};
        bool isTangentIn{false};
        bool isTangentOut{false};
        bool isVelocityIn{false};   ///< velocity-pane in-handle
        bool isVelocityOut{false};  ///< velocity-pane out-handle
    };
    [[nodiscard]] HitResult hitTest(const QPointF& pos) const;

    // ── Data ────────────────────────────────────────────────────────────
    Clip*                       m_clip{nullptr};
    CommandStack*               m_commandStack{nullptr};
    std::vector<CurveEntry>     m_curves;
    std::set<SelectedKey>       m_selection;

    // Clipboard stores complete keyframes so paste/redo preserve all handles.
    struct ClipboardEntry
    {
        int             curveIndex;
        Keyframe<float> keyframe;  // time is relative to earliest copied key
    };
    std::vector<ClipboardEntry> m_clipboard;

    // View range (in graph coordinates: time in ticks, value is float)
    double m_viewTimeMin{0.0};
    double m_viewTimeMax{48000.0 * 5.0}; // default 5 seconds
    double m_viewValueMin{-1.0};
    double m_viewValueMax{2.0};

    // Interaction state
    bool     m_dragging{false};
    bool     m_boxSelecting{false};
    bool     m_draggingTangent{false};
    bool     m_tangentIsIn{false};   // which tangent handle
    bool     m_velocityDrag{false};  // tangent drag originated in velocity pane
    int      m_dragCurveIdx{-1};
    int      m_dragKeyIdx{-1};
    KeyframeTrack<float>* m_tangentTrack{nullptr};
    Keyframe<float> m_tangentBefore;
    bool     m_hasTangentBefore{false};
    QPointF  m_dragStartPos;
    QPointF  m_boxStart;       // box-select start (pixel)
    QPointF  m_boxCurrent;     // box-select current (pixel)
    QPointF  m_panStart;       // middle-mouse pan

    // ── Context menu ────────────────────────────────────────────────────
    QMenu*   m_contextMenu{nullptr};
    QAction* m_actAddKeyframe{nullptr};
    QAction* m_actDeleteKeyframes{nullptr};
    QAction* m_actLinear{nullptr};
    QAction* m_actBezier{nullptr};
    QAction* m_actHold{nullptr};
    QAction* m_actAutoBezier{nullptr};
    QAction* m_actContinuousBezier{nullptr};
    QAction* m_actEaseIn{nullptr};
    QAction* m_actEaseOut{nullptr};
    QAction* m_actShowVelocity{nullptr};  ///< Toggle Show Velocity Graph
    QAction* m_actCopy{nullptr};
    QAction* m_actPaste{nullptr};
    QAction* m_actFitAll{nullptr};
    QAction* m_actFitSelection{nullptr};
    QAction* m_actSelectAll{nullptr};

    QPointF  m_contextMenuGraphPos; // graph position for context-menu "add keyframe"

    // ── Velocity (speed) graph state ─────────────────────────────────────
    bool   m_showVelocityGraph{false};
    double m_viewVelocityMin{-100.0};
    double m_viewVelocityMax{ 100.0};
};

} // namespace rt
