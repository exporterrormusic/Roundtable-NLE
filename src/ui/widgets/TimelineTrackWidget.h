/*
 * TimelineTrackWidget — Renders a single track's clips and header area.
 *
 * Step 12: Draws clips as colored rectangles, handles clip selection,
 * and paints the track background.
 */

#pragma once

#include <QWidget>
#include <QString>
#include <QPixmap>

#include <atomic>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace rt {

class AnimationVideoCache;
class TimelineLayoutEngine;
class Track;

/// Which side(s) of an edit-point bracket selection to draw.
/// Both = two facing brackets at a connected seam between touching clips.
/// HeadOnly = single right-opening bracket on the head of an isolated clip.
/// TailOnly = single left-opening bracket on the tail of an isolated clip.
/// (Underlying type pinned so it can be forward-declared in TimelinePanel.h.)
enum class EditPointSide : int { Both, HeadOnly, TailOnly };

/// Visual representation of a single timeline track (clip area only).
class TimelineTrackWidget : public QWidget
{
    Q_OBJECT

public:
    explicit TimelineTrackWidget(QWidget* parent = nullptr);
    ~TimelineTrackWidget() override;

    /// Set the layout engine.
    void setLayoutEngine(const TimelineLayoutEngine* engine);

    /// Set the data model track (non-owning).
    void setTrack(const Track* track, size_t trackIndex);

    /// Set selected clip indices.
    void setSelectedClips(const std::vector<size_t>& indices);
    void setSelectedTransition(size_t transitionIndex);

    /// Set dragged clip indices (rendered semi-transparent with selected border).
    void setDraggedClips(const std::vector<size_t>& indices);

    /// Set playhead tick so a vertical line is drawn through the track.
    void setPlayheadTick(int64_t tick);

    /// Set playhead tick without scheduling a repaint (caller already invalidated).
    void setPlayheadTickNoRepaint(int64_t tick);

    /// Set snap indicator tick (-1 to hide). Draws a white line when snapping.
    void setSnapIndicatorTick(int64_t tick);

    /// Set hover edge highlight. edgeTick = the tick position of the hovered edge, -1 to clear.
    void setHoverEdgeTick(int64_t tick);

    /// Set / clear the Premiere-style edit-point selection.
    /// Pass -1 to clear. With side=Both the widget draws facing brackets
    /// at the connected cut. With HeadOnly/TailOnly only the inward-facing
    /// bracket is drawn — used when clicking the outer edge of an isolated
    /// clip so the user sees the edge as selected (not the whole clip).
    void setEditPointTick(int64_t tick, EditPointSide side = EditPointSide::Both);

    /// Set razor position tick (-1 to hide). Draws a vertical red line through the clip.
    void setRazorTick(int64_t tick);

    /// Set pointer to waveform peak cache (owned by TimelinePanel).
    void setWaveformCache(const std::unordered_map<uint64_t, std::vector<float>>* cache);

    /// Convert a viewport pixel to its normalized position in the full,
    /// unclipped timeline clip. Exposed for regression testing of waveform
    /// mapping when a clip extends beyond the viewport.
    [[nodiscard]] static double waveformClipFractionAtPixel(
        double pixelX, double clipLeft, double clipWidth) noexcept;

    /// Set in/out point range (ticks). Negative means unset.
    void setInOutPoints(int64_t inPoint, int64_t outPoint);

    /// Get the track index.
    size_t trackIndex() const noexcept { return m_trackIndex; }


    /// Set pointer to thumbnail cache (clipId → QPixmap, owned externally).
    void setThumbnailCache(const std::unordered_map<uint64_t, QPixmap>* cache);

    /// Set pointer to animation video cache (for cached-clip color override).
    void setAnimVideoCache(const AnimationVideoCache* cache);

    /// When true, dragged clips are hidden entirely (shown in ghost overlay instead).
    void setGhostDragActive(bool active) { m_ghostDragActive = active; update(); }

    /// Set / clear the effect-drop highlight.  When non-zero, the clip with
    /// this ID gets a darkened overlay + border to indicate an effect is about
    /// to be applied on drop.
    void setEffectHighlightClipId(uint64_t clipId);

    /// Set / clear the gap selection highlight.  Draws a translucent rectangle
    /// between startTick and endTick to indicate a selected gap.
    void setGapHighlight(int64_t startTick, int64_t endTick);
    void clearEffectHighlight();

    /// Set / clear transition-drop edge highlight.  When set, a glowing
    /// vertical line is drawn at the given tick to indicate a drop zone.
    void setTransitionDropEdgeTick(int64_t tick);
    void clearTransitionDropEdge();

    /// Set / clear media-drag ghost preview (Premiere Pro-style semi-transparent clip).
    void setMediaDragPreview(int64_t tick, int64_t duration, bool isAudio);
    void clearMediaDragPreview();

    std::atomic<bool> m_destroying{false};

signals:
    /// Emitted when a clip is clicked.
    void clipClicked(size_t trackIndex, size_t clipIndex, bool shiftHeld);

    /// Emitted when the background (no clip) is clicked.
    void trackBackgroundClicked(size_t trackIndex, int64_t timeTick);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    bool event(QEvent* event) override;  // For tooltips

private:
    const TimelineLayoutEngine* m_engine{nullptr};
    const Track*                m_track{nullptr};
    size_t                      m_trackIndex{0};
    std::vector<size_t>         m_selectedClips;
    std::unordered_set<size_t>   m_selectedSet;     // O(1) lookup during paint
    size_t                      m_selectedTransitionIndex{SIZE_MAX};
    std::vector<size_t>         m_draggedClips;
    std::unordered_set<size_t>   m_draggedSet;      // O(1) lookup during paint
    int64_t                     m_playheadTick{0};
    double                      m_lastPaintedPlayheadPx{-1000};
    int64_t                     m_snapIndicatorTick{-1};
    int64_t                     m_hoverEdgeTick{-1};
    int64_t                     m_razorTick{-1};
    int64_t                     m_editPointTick{-1};  ///< "between clips" selection (-1 = none)
    EditPointSide               m_editPointSide{EditPointSide::Both}; ///< which bracket(s) to draw
    const std::unordered_map<uint64_t, std::vector<float>>* m_waveformCache{nullptr};
    const std::unordered_map<uint64_t, QPixmap>* m_thumbnailCache{nullptr};
    const AnimationVideoCache*  m_animVideoCache{nullptr};
    int64_t                     m_inPoint{-1};
    int64_t                     m_outPoint{-1};
    uint64_t                    m_effectHighlightClipId{0};
    int64_t                     m_transitionDropEdgeTick{-1};
    int64_t                     m_gapHighlightStart{-1};
    int64_t                     m_gapHighlightEnd{-1};

    // Media drag preview (ghost clip)
    int64_t                     m_dragPreviewTick{-1};
    int64_t                     m_dragPreviewDuration{0};
    bool                        m_dragPreviewIsAudio{false};

    bool                        m_ghostDragActive{false};

    void paintClip(class QPainter& painter, size_t clipIndex);
};

} // namespace rt
