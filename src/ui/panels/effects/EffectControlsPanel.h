/*
 * EffectControlsPanel — Premiere Pro–style Effect Controls panel.
 *
 * Displays clip properties as collapsible sections with inline keyframe
 * navigation and a synchronized mini-timeline on the right showing
 * keyframe diamonds.
 *
 * Layout (mirrors Premiere Pro "Effect Controls"):
 * ┌──────────────────────────────────────────────────────────┐
 * │  Source: [clip name]         [504317 - clip title]   [□] │
 * ├────────────────────────────┬─────────────────────────────┤
 * │ Video                      │ ▲  ruler (timecodes)        │
 * │ ▸ fx  Motion              │  [clip bar]                  │
 * │   ◉ Position   640  360   │  ◆ ──── ◆                   │
 * │   ◉ Scale      100.0      │  ◆ ─────────── ◆            │
 * │   ☑ Uniform Scale          │                              │
 * │   ◉ Rotation   0.0        │                              │
 * │   ◉ Anchor Pt  640  360   │                              │
 * │   ◉ Anti-flicker 0.00     │                              │
 * │ ▸ Crop                     │                              │
 * │   ◉ Crop Left   0.0 %    │                              │
 * │   ◉ Crop Top    0.0 %    │                              │
 * │   ◉ Crop Right  0.0 %    │                              │
 * │   ◉ Crop Bottom 0.0 %    │                              │
 * │ ▸ fx  Opacity             │                              │
 * │   ◉ Opacity     100.0 %  │  ◆                           │
 * │     Blend Mode  Normal ▼  │                              │
 * │ ▸ Time Remapping           │                              │
 * │   ◉ Speed       100.0 %  │                              │
 * │ (applied effects below)    │                              │
 * ├────────────────────────────┴─────────────────────────────┤
 * │  00:00:04;25                              [filter icons] │
 * └──────────────────────────────────────────────────────────┘
 *
 * Property rows feature:
 *  - ▸ expand arrow (for bezier sub-controls)
 *  - ◉ stopwatch toggle (enable/disable keyframing)
 *  - Property name + scrubby value(s)
 *  - ◀ ◆ ▶ keyframe navigation buttons (prev/add-remove/next)
 */

#pragma once

#include <QWidget>
#include <QSplitter>
#include <QScrollArea>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QCheckBox>
#include <QComboBox>
#include <QLineEdit>
#include <QPushButton>
#include <QToolButton>
#include <QGroupBox>
#include <QKeyEvent>
#include <QTimer>

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace rt {

// Forward declarations
class Clip;
class CommandStack;
class Effect;
class GraphicLayer;
class Timeline;
class Track;
class ScrubbySpinBox;
template <typename T> class KeyframeTrack;
enum class InterpMode : uint8_t;

// ═════════════════════════════════════════════════════════════════════════════
//  PropertyRow — a single animatable property row (stopwatch + value + kf nav)
// ═════════════════════════════════════════════════════════════════════════════

class PropertyRow : public QWidget
{
    Q_OBJECT

public:
    /// Create a property row.
    /// @param name       Display name (e.g. "Position", "Scale")
    /// @param track      Pointer to the KeyframeTrack (for keyframe ops). May be nullptr for non-keyframeable.
    /// @param parent     Parent widget.
    PropertyRow(const QString& name, KeyframeTrack<float>* track,
                QWidget* parent = nullptr);

    void setTrack(KeyframeTrack<float>* track);
    [[nodiscard]] KeyframeTrack<float>* track() const noexcept { return m_track; }

    /// Bind additional tracks to this row. The row's keyframe-button,
    /// stopwatch toggle, and prev/next nav all act on the union; the
    /// KeyframeTimeline draws and drags a single diamond per time across
    /// every bound track. Used for compound properties like Position
    /// (X+Y) where the user expects a single keyframe to capture every
    /// component, not just the primary one.
    void addExtraTrack(KeyframeTrack<float>* track);
    /// Drop a previously-bound extra track. Used by the Scale row to
    /// detach scaleY when the user un-checks Uniform Scale so the diamond
    /// drag stops moving Y alongside X.
    void removeExtraTrack(KeyframeTrack<float>* track);
    void clearExtraTracks();
    [[nodiscard]] const std::vector<KeyframeTrack<float>*>& extraTracks() const noexcept {
        return m_extraTracks;
    }
    /// All tracks bound to this row: primary first, then extras (skipping nulls).
    [[nodiscard]] std::vector<KeyframeTrack<float>*> allTracks() const;

    /// Add a scrubby spinbox as a value field for this property.
    void addValueWidget(ScrubbySpinBox* spin);

    /// Add a pair of scrubby spinboxes (e.g. Position X/Y).
    void addValuePair(ScrubbySpinBox* spinA, ScrubbySpinBox* spinB);

    /// Add any arbitrary widget (e.g. a checkbox or combo).
    void addCustomWidget(QWidget* widget);

    /// Refresh keyframe navigation state for a given playhead time.
    void updateForTime(int64_t time);

    /// The stopwatch toggle button.
    [[nodiscard]] QToolButton* stopwatchButton() const noexcept { return m_stopwatch; }

    /// Row height (for mini-timeline alignment).
    [[nodiscard]] int rowIndex() const noexcept { return m_rowIndex; }
    void setRowIndex(int idx) noexcept { m_rowIndex = idx; }

    /// Property name.
    [[nodiscard]] QString propertyName() const;

signals:
    void addKeyframeRequested(KeyframeTrack<float>* track, int64_t time);
    void deleteKeyframeRequested(KeyframeTrack<float>* track, int64_t time);
    void goToPrevKeyframe(KeyframeTrack<float>* track);
    void goToNextKeyframe(KeyframeTrack<float>* track);
    void keyframingToggled(KeyframeTrack<float>* track, bool enabled);
    /// Premiere-style per-attribute reset: restore this row's value(s) to
    /// their factory default and clear any keyframes (undoable). Handled by
    /// EffectControlsPanel which knows the spin→track mapping.
    void resetRequested();

private:
    void buildUI();

    QString               m_name;
    KeyframeTrack<float>* m_track{nullptr};
    std::vector<KeyframeTrack<float>*> m_extraTracks;
    int                   m_rowIndex{0};

    // Widgets
    QToolButton*  m_expandBtn{nullptr};
    QToolButton*  m_stopwatch{nullptr};
    QLabel*       m_nameLabel{nullptr};
    QHBoxLayout*  m_valueLayout{nullptr};
    QToolButton*  m_prevKfBtn{nullptr};
    QToolButton*  m_addKfBtn{nullptr};
    QToolButton*  m_nextKfBtn{nullptr};
    QToolButton*  m_resetBtn{nullptr};
};

// ═════════════════════════════════════════════════════════════════════════════
//  KeyframeTimeline — mini-timeline widget showing keyframe diamonds
// ═════════════════════════════════════════════════════════════════════════════

class KeyframeTimeline : public QWidget
{
    Q_OBJECT

public:
    explicit KeyframeTimeline(QWidget* parent = nullptr);

    /// Set the clip whose keyframes to display.
    void setClip(Clip* clip);

    /// Set the list of property rows (for vertical alignment).
    void setPropertyRows(const std::vector<PropertyRow*>& rows);

    /// Set the scroll offset (synced with left-side scroll area).
    void setScrollOffset(int y);

    /// Set the current playhead position (clip-relative ticks).
    void setPlayheadTick(int64_t tick);

    /// Set view range (clip-relative ticks).
    void setViewRange(int64_t startTick, int64_t endTick);

    /// Set command stack for undo support.
    void setCommandStack(CommandStack* stack) noexcept { m_commandStack = stack; }

    QSize sizeHint() const override { return {400, 200}; }
    QSize minimumSizeHint() const override { return {100, 50}; }

signals:
    void playheadScrubbed(int64_t tick);
    void keyframeChanged();  // emitted after move/delete of a keyframe

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;  // clears selection
                                                      // when focus leaves to
                                                      // a non-popup widget

private:
    struct HitResult {
        KeyframeTrack<float>* track{nullptr};
        size_t index{0};
    };
    [[nodiscard]] HitResult hitTestKeyframe(const QPoint& pos) const;

    void drawRuler(QPainter& p);
    void drawClipBar(QPainter& p);
    void drawKeyframeDiamonds(QPainter& p);
    void drawPlayhead(QPainter& p);

    [[nodiscard]] int tickToX(int64_t tick) const;
    [[nodiscard]] int64_t xToTick(int x) const;
    [[nodiscard]] int rowY(const PropertyRow* row) const;

    Clip*                      m_clip{nullptr};
    std::vector<PropertyRow*>  m_rows;
    int                        m_scrollOffsetY{0};
    int64_t                    m_playheadTick{0};
    int64_t                    m_viewStart{0};
    int64_t                    m_viewEnd{48000 * 10};  // 10 seconds default
    bool                       m_scrubbing{false};

    // Multi-selection (track + time pairs)
    struct SelKey {
        KeyframeTrack<float>* track{nullptr};
        int64_t time{0};
        bool operator<(const SelKey& o) const {
            if (track != o.track) return track < o.track;
            return time < o.time;
        }
    };
    std::set<SelKey> m_selectedKeys;

    // Marquee rubber-band selection
    bool   m_marqueeActive{false};
    QPoint m_marqueeOrigin;
    QPoint m_marqueeCurrent;
    std::set<SelKey> m_preMarqueeSelection; // keys already selected before marquee

    // Group drag of selected keyframes
    bool    m_draggingSelection{false};
    int64_t m_dragAnchorTick{0};
    struct DragEntry {
        KeyframeTrack<float>* track;
        int64_t origTime;
        int64_t currentTime;
        float value;
        InterpMode interp;
        float biX, biY, boX, boY;
    };
    std::vector<DragEntry> m_dragEntries;

    // Per-track snapshot taken at drag start. Each frame of the drag
    // restores the track from this snapshot, removes the dragged-set's
    // origTimes, then re-inserts the dragged keyframes at their new
    // positions. This guarantees that dragging a keyframe past a
    // non-dragged keyframe never silently destroys the non-dragged one
    // (the snapshot brings it back on the next move).
    struct DragSnapKf {
        int64_t time;
        float value;
        InterpMode interp;
        float biX, biY, boX, boY;
    };
    std::map<KeyframeTrack<float>*, std::vector<DragSnapKf>> m_dragTrackSnap;

    CommandStack*  m_commandStack{nullptr};

    static constexpr int kRulerHeight = 24;
    static constexpr int kRowHeight   = 28;
    static constexpr int kClipBarHeight = 18;
    static constexpr int kDiamondRadius = 5;
};

// ═════════════════════════════════════════════════════════════════════════════
//  EffectControlsPanel — the main panel widget
// ═════════════════════════════════════════════════════════════════════════════

class EffectControlsPanel : public QWidget
{
    Q_OBJECT

public:
    explicit EffectControlsPanel(QWidget* parent = nullptr);
    ~EffectControlsPanel() override;

    // ── Clip binding ────────────────────────────────────────────────────
    void setClip(Clip* clip, Track* track = nullptr);
    [[nodiscard]] Clip* clip() const noexcept { return m_clip; }

    void refresh();
    void clearClip();

    // ── Dependencies ────────────────────────────────────────────────────
    void setCommandStack(CommandStack* stack) noexcept {
        m_commandStack = stack;
        if (m_kfTimeline) m_kfTimeline->setCommandStack(stack);
    }
    void setTimeline(Timeline* tl) noexcept { m_timeline = tl; }

    /// Sequence resolution used to convert the internal REF-1920 Position
    /// values into displayed sequence pixels (Premiere-style Motion).  Stored
    /// values stay REF-1920; only the Effect Controls UI shows seq-px.
    void setSequenceResolution(uint32_t w, uint32_t h) noexcept {
        if (w > 0) m_seqW = w;
        if (h > 0) m_seqH = h;
        if (m_clip) populateFromClip();
    }

    /// Update playhead position (for keyframe nav + mini-timeline).
    void setPlayheadTick(int64_t tick);

    /// Lightweight refresh of the transform spin-box VALUES only (no
    /// property-tree rebuild).  Call this when the clip's transform is
    /// changed externally — e.g. dragging the transform overlay in the
    /// Program Monitor — so the Effect Controls numbers track live.
    void syncValuesFromClip();

    /// Premiere-style per-layer Effect Controls: when a graphic layer
    /// (text / shape inside a GraphicClip) is selected, route the Motion
    /// section's reads AND writes through the LAYER's transform instead
    /// of the clip's. Pass nullptr to fall back to clip-level transforms
    /// (default behavior for video / image clips and for graphic clips
    /// with no layer selected). Triggers a property-tree rebuild when
    /// the source changes so PropertyRows hold the right track pointers.
    void setSelectedGraphicLayer(GraphicLayer* layer);

    /// Get the PropertyRow widgets for test introspection.
    [[nodiscard]] const std::vector<PropertyRow*>& propertyRows() const noexcept { return m_propertyRows; }

    /// Returns true if an applied effect is currently selected.
    [[nodiscard]] bool hasSelectedEffect() const noexcept { return m_selectedEffectIndex >= 0; }

    /// Returns true if an effect has been copied and is ready to paste.
    [[nodiscard]] bool hasCopiedEffect() const noexcept { return m_copiedEffect != nullptr; }

    /// Clear the copied effect clipboard. Call this when the user copies
    /// a clip (or anything else) so that Ctrl+V doesn't keep pasting the
    /// stale effect.
    void clearCopiedEffect() noexcept { m_copiedEffect.reset(); }

    /// Delete the currently selected effect (no-op if none selected).
    void deleteSelectedEffect();

    /// Copy the currently selected effect to internal clipboard.
    void copySelectedEffect();

    /// Paste a previously copied effect onto the current clip.
    void pasteEffect();

protected:
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dragMoveEvent(QDragMoveEvent* event) override;
    void dropEvent(QDropEvent* event) override;

signals:
    void propertyChanged();
    void clipChanged(Clip* clip);
    void seekRequested(int64_t tick);
    void eyedropperRequested(size_t effectIdx);  // request eyedropper for Ultra Key color sampling
    void maskChanged();  // emitted when a mask is added, removed, or modified
    void maskSelected(int maskIndex);  // emitted when user clicks a mask header to select it
    /// Emitted when an audio clip's volume/pan is scrubbed in the panel.
    /// Values are in engine units (linear gain, pan -1..+1). Listeners push
    /// these directly to AudioEngine so playback reflects the change live.
    void audioLevelsChanged(uint64_t clipId, float linearVolume, float pan);

private:
    void setupUI();
    void buildPropertyTree();
    void clearPropertyTree();
    void populateFromClip();
    /// True when one of this panel's spinboxes currently has keyboard
    /// focus — used to avoid clobbering a typed value with a playback
    /// tick's evaluated value before the user commits.
    [[nodiscard]] bool isAnySpinBoxBeingEdited() const;
    void deleteEffect(size_t index);

    /// Build Ultra Key grouped sections (key color, matte gen, cleanup, spill, CC)
    void buildUltraKeyUI(Effect& fx, size_t effectIdx, int& rowIdx);
    /// Build LUT effect UI with file browser for .cube files
    void buildLUTUI(Effect& fx, size_t effectIdx, int& rowIdx);
    /// Build Letterbox effect UI with preset aspect ratio dropdown
    void buildLetterboxUI(Effect& fx, size_t effectIdx, int& rowIdx);
    /// Build generic flat parameter rows for a non-Ultra Key effect
    void buildGenericEffectUI(Effect& fx, size_t effectIdx, int& rowIdx);
    /// Wire a single effect parameter spin box to live preview + undo commit
    void wireEffectParam(ScrubbySpinBox* spin, size_t effectIdx, size_t paramIdx);
    /// Build mask parameter sub-sections for all masks on the current clip
    void buildMaskUI(int& rowIdx);
    /// Add a new mask to the current clip
    void addMask(uint8_t shapeType);

protected:
    void keyPressEvent(QKeyEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

    // ── Apply helpers ───────────────────────────────────────────────────
    struct TransformState {
        float posX, posY, scaleX, scaleY, rotation, opacity;
        double speed;
        float pan{0.0f};
        float volume{1.0f};
    };
    TransformState captureTransformState() const;
    void restoreTransformState(const TransformState& s);
    void applyTransform();
    void applyTransformLive();
    void commitTransform(double oldVal, double newVal);

    /// Premiere-style per-attribute reset. Restores every value spin in the
    /// row to its engine-native factory default and clears that property's
    /// keyframes, as a single undoable command.
    void resetPropertyRow(PropertyRow* row);

    // ── Keyframe operations ─────────────────────────────────────────────
    void onAddKeyframe(KeyframeTrack<float>* track, int64_t time);
    void onDeleteKeyframe(KeyframeTrack<float>* track, int64_t time);
    void onGoToPrevKeyframe(KeyframeTrack<float>* track);
    void onGoToNextKeyframe(KeyframeTrack<float>* track);

    /// Clip-relative playhead tick (for keyframe ops).
    [[nodiscard]] int64_t clipRelativeTick() const noexcept;

    /// Cover-fit factor for the current clip (Premiere-style native-pixel
    /// Scale display).  For normal media (Image / non-character Video /
    /// color matte), returns max(seqW/srcW, seqH/srcH) — the multiplier
    /// the compositor bakes into scale=1.0.  Effect Controls multiplies
    /// the stored scaleX/Y by this factor so a displayed 100% always
    /// means the source is rendered 1:1 (no upscale = sharp), and any
    /// other value tells the user honestly that pixels are being
    /// stretched/shrunk.  Returns 1.0 for clip kinds without meaningful
    /// native pixels (SpineClip, VideoCharacter, TitleClip, GraphicClip)
    /// so their existing fill-model Scale numbers are unaffected.
    [[nodiscard]] double coverFitForCurrentClip() const noexcept;

    ScrubbySpinBox* createScrubby(double min, double max, double step,
                                   int decimals, const QString& suffix = {});

    // ── Transform source indirection (Premiere-style per-layer Motion) ──
    // When m_graphicLayer is non-null, the Motion section binds to that
    // layer's transform tracks; otherwise it binds to the clip's tracks.
    // Defined in EffectControlsPanel.cpp so we can keep GraphicLayer.h out
    // of this header.
    KeyframeTrack<float>* effPosX() noexcept;
    KeyframeTrack<float>* effPosY() noexcept;
    KeyframeTrack<float>* effScaleX() noexcept;
    KeyframeTrack<float>* effScaleY() noexcept;
    KeyframeTrack<float>* effRotation() noexcept;
    KeyframeTrack<float>* effOpacity() noexcept;
    KeyframeTrack<float>* effAnchorX() noexcept;
    KeyframeTrack<float>* effAnchorY() noexcept;
    /// Display-factor for the Position spinboxes (multiply stored→display).
    /// Clip-level position is stored REF-1920 and shown in sequence px;
    /// layer-level position is stored project-px (already sequence-px) so
    /// the factor is 1.0 when a graphic layer drives the source.
    double posDisplayFactorX() const noexcept;
    double posDisplayFactorY() const noexcept;

    // ── State ───────────────────────────────────────────────────────────
    Clip*          m_clip{nullptr};
    Track*         m_track{nullptr};
    /// Non-null when Effect Controls is bound to a graphic layer's
    /// transform (text / shape inside a GraphicClip). See
    /// setSelectedGraphicLayer().
    GraphicLayer*  m_graphicLayer{nullptr};
    CommandStack*  m_commandStack{nullptr};
    Timeline*      m_timeline{nullptr};
    bool           m_updating{false};
    int64_t        m_playheadTick{0};
    int            m_selectedEffectIndex{-1};  // -1 = none selected
    /// Clipboard for copy/paste of a single effect with its settings.
    std::unique_ptr<Effect> m_copiedEffect;
    /// Sequence resolution for Position seq-px display conversion (defaults
    /// to 1920×1080 — same basis as the internal REF representation).
    uint32_t       m_seqW{1920};
    uint32_t       m_seqH{1080};

    // ── UI ──────────────────────────────────────────────────────────────
    QLabel*         m_footerTimecodeLabel{nullptr};
    QLabel*         m_emptyLabel{nullptr};
    QLineEdit*      m_searchField{nullptr};
    QWidget*        m_splitterContainer{nullptr};  // wraps splitter + empty label
    QLabel*         m_clipNameLabel{nullptr};
    QLabel*         m_clipTypeLabel{nullptr};

    QSplitter*      m_splitter{nullptr};
    QScrollArea*    m_scrollArea{nullptr};
    QWidget*        m_propContainer{nullptr};
    QVBoxLayout*    m_propLayout{nullptr};
    KeyframeTimeline* m_kfTimeline{nullptr};

    // ── Sections ────────────────────────────────────────────────────────
    QWidget*        m_motionSection{nullptr};
    QWidget*        m_cropSection{nullptr};
    QWidget*        m_opacitySection{nullptr};
    QWidget*        m_timeRemapSection{nullptr};
    QWidget*        m_effectsContainer{nullptr};

    // ── Effect header tracking (for selection) ──────────────────────────
    std::vector<QWidget*> m_effectHeaders;  // one per applied effect

    // ── Property rows ───────────────────────────────────────────────────
    std::vector<PropertyRow*> m_propertyRows;

    // ── Collapsible section tracking ────────────────────────────────────
    struct SectionInfo {
        QWidget*              header{nullptr};
        QToolButton*          arrow{nullptr};
        std::vector<QWidget*> children;       // rows belonging to this section
        QString               title;          // for preserving collapse state
        QToolButton*          resetBtn{nullptr};
    };
    std::vector<SectionInfo> m_sectionArrows;

    // Persisted collapse state across refresh() — keyed by section title
    std::map<QString, bool> m_sectionCollapsed;

    // ── Value widgets ───────────────────────────────────────────────────
    ScrubbySpinBox* m_posXSpin{nullptr};
    ScrubbySpinBox* m_posYSpin{nullptr};
    ScrubbySpinBox* m_scaleSpin{nullptr};
    ScrubbySpinBox* m_scaleWSpin{nullptr};
    PropertyRow*    m_posRow{nullptr};
    PropertyRow*    m_scaleRow{nullptr};
    PropertyRow*    m_scaleWRow{nullptr};
    PropertyRow*    m_rotationRow{nullptr};
    PropertyRow*    m_anchorRow{nullptr};
    PropertyRow*    m_opacityRow{nullptr};
    QCheckBox*      m_uniformScaleCheck{nullptr};
    ScrubbySpinBox* m_rotationSpin{nullptr};
    ScrubbySpinBox* m_anchorXSpin{nullptr};
    ScrubbySpinBox* m_anchorYSpin{nullptr};
    ScrubbySpinBox* m_antiFlickerSpin{nullptr};
    ScrubbySpinBox* m_cropLeftSpin{nullptr};
    ScrubbySpinBox* m_cropTopSpin{nullptr};
    ScrubbySpinBox* m_cropRightSpin{nullptr};
    ScrubbySpinBox* m_cropBottomSpin{nullptr};
    ScrubbySpinBox* m_opacitySpin{nullptr};
    QComboBox*      m_blendModeCombo{nullptr};
    ScrubbySpinBox* m_speedSpin{nullptr};

    // Audio-only controls
    ScrubbySpinBox* m_panSpin{nullptr};
    ScrubbySpinBox* m_audioVolumeSpin{nullptr};
};

} // namespace rt
