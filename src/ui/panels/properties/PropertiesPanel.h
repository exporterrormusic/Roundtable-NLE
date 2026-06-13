/*
 * PropertiesPanel — Context-sensitive clip property editor.
 *
 * Step 17: Properties Panel
 *
 * Shows editable properties for the currently selected clip(s).
 * Adapts layout based on clip type:
 *
 *   Spine clip:  character, outfit, stance, animation, talking, speed, transform
 *   Video clip:  source file, speed, volume, transform, opacity
 *   Audio clip:  volume, pan, mute, fade in/out, speed
 *   Title clip:  text, font, size, bold/italic, alignment, colors, transform
 *
 * All property changes go through the Command system for undo/redo.
 * Uses ScrubbySpinBox for numeric values (Premiere-style drag adjustment).
 *
 * Layout:
 *   ┌─────────────────────────────────────────────┐
 *   │  Properties — [Clip Name] [Type badge]      │
 *   ├─────────────────────────────────────────────┤
 *   │  ▸ Identity                                  │
 *   │    Label:  [──────────────]                  │
 *   │    Enabled: ☑                                │
 *   ├─────────────────────────────────────────────┤
 *   │  ▸ Type-specific section                     │
 *   │    (varies by clip type)                     │
 *   ├─────────────────────────────────────────────┤
 *   │  ▸ Transform                                 │
 *   │    Position X: [←─ 0.000 ─→]                │
 *   │    Position Y: [←─ 0.000 ─→]                │
 *   │    Scale X:    [←─ 1.000 ─→]                │
 *   │    Scale Y:    [←─ 1.000 ─→]                │
 *   │    Rotation:   [←─ 0.000 ─→]                │
 *   │    Opacity:    [←─ 1.000 ─→]                │
 *   └─────────────────────────────────────────────┘
 */

#pragma once

#include <QCheckBox>
#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>
#include <QWidget>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

class QGroupBox;
class QTimer;
class QFontComboBox;
class QPlainTextEdit;

namespace rt {

class Clip;
class SpineClip;
class PngPuppetClip;
class VideoClip;
class AudioClip;
class TitleClip;
class CaptionClip;
class ScrubbySpinBox;
class AudioFxSection;
class CommandStack;
class ModelManager;
class ShotPresetManager;
class Timeline;
class Track;

/// Properties Panel — context-sensitive clip property editor.
class PropertiesPanel : public QWidget
{
    Q_OBJECT

public:
    explicit PropertiesPanel(QWidget* parent = nullptr);
    ~PropertiesPanel() override;

protected:
    bool eventFilter(QObject* obj, QEvent* event) override;

public:
    // ── Clip binding ────────────────────────────────────────────────────

    /// Set the clip to display/edit properties for.
    /// Pass nullptr to clear.
    void setClip(Clip* clip, Track* track = nullptr);

    /// Set a transition to display/edit properties for.
    void setTransition(Track* track, size_t transitionIndex);

    /// Get the currently bound clip.
    [[nodiscard]] Clip* clip() const noexcept { return m_clip; }

    /// Focus and select the graphic "Text:" field so the user can
    /// immediately type to change a text layer's content (used by the
    /// double-click-to-edit-text flow). No-op if the field is hidden.
    void focusGraphicTextField();

    /// Get the track containing the clip (for command creation).
    [[nodiscard]] Track* track() const noexcept { return m_track; }

    /// Refresh all widgets from the current clip's values.
    void refresh();

    /// Refresh only the effects section (after drag-drop add).
    void refreshEffects();

    /// Clear the panel (no clip selected).
    void clearClip();

    /// Set multi-selection: shows shot section if all clips share a group.
    void setMultiSelection(const std::vector<Clip*>& clips);

    // ── Command system ──────────────────────────────────────────────────

    /// Set the command stack for undo/redo support.
    void setCommandStack(CommandStack* stack) noexcept { m_commandStack = stack; }
    [[nodiscard]] CommandStack* commandStack() const noexcept { return m_commandStack; }

    /// Set the shot preset manager for shot switching support.
    void setShotPresetManager(ShotPresetManager* mgr) noexcept { m_shotManager = mgr; }

    /// Refresh the shot dropdown (when COMPOSE saves a new/updated shot).
    void refreshShotDropdown();

    /// Set the timeline for shot switching (needed to find group clips).
    void setTimeline(Timeline* tl) noexcept { m_timeline = tl; }

    /// Set the model manager for character/outfit/stance dropdowns.
    void setModelManager(ModelManager* mgr) noexcept { m_modelManager = mgr; }

    /// Callback that returns animation names for a character identity.
    using AnimNamesProvider = std::function<std::vector<std::string>(
        const std::string& characterName, const std::string& outfit, int stance)>;
    void setAnimationNamesProvider(AnimNamesProvider provider) {
        m_animNamesProvider = std::move(provider);
    }

    /// Callback that returns animation names for a video character (from AnimationVideoCache).
    using VideoAnimNamesProvider = std::function<std::vector<std::string>(
        const std::string& characterName, const std::string& outfit)>;
    void setVideoAnimNamesProvider(VideoAnimNamesProvider provider) {
        m_videoAnimNamesProvider = std::move(provider);
    }

    // ── Accessors for test introspection ────────────────────────────────

    [[nodiscard]] QLineEdit* labelEdit() const noexcept { return m_labelEdit; }
    [[nodiscard]] QCheckBox* enabledCheck() const noexcept { return m_enabledCheck; }
    [[nodiscard]] ScrubbySpinBox* speedSpin() const noexcept { return m_speedSpin; }

    // Transform section
    [[nodiscard]] ScrubbySpinBox* posXSpin()    const noexcept { return m_posXSpin; }
    [[nodiscard]] ScrubbySpinBox* posYSpin()    const noexcept { return m_posYSpin; }
    [[nodiscard]] ScrubbySpinBox* scaleXSpin()  const noexcept { return m_scaleXSpin; }
    [[nodiscard]] ScrubbySpinBox* scaleYSpin()  const noexcept { return m_scaleYSpin; }
    [[nodiscard]] ScrubbySpinBox* rotationSpin() const noexcept { return m_rotationSpin; }
    [[nodiscard]] ScrubbySpinBox* opacitySpin() const noexcept { return m_opacitySpin; }
    [[nodiscard]] QCheckBox*      flipHCheck()  const noexcept { return m_flipHCheck; }
    [[nodiscard]] QCheckBox*      flipVCheck()  const noexcept { return m_flipVCheck; }

    // Spine-specific
    [[nodiscard]] QComboBox*  characterCombo()    const noexcept { return m_characterCombo; }
    [[nodiscard]] QComboBox*  outfitCombo()       const noexcept { return m_outfitCombo; }
    [[nodiscard]] QComboBox*  stanceCombo()       const noexcept { return m_stanceCombo; }
    [[nodiscard]] QComboBox*  animationCombo()    const noexcept { return m_animationCombo; }
    [[nodiscard]] QCheckBox*  loopingCheck()      const noexcept { return m_loopingCheck; }
    [[nodiscard]] QCheckBox*  talkingCheck()      const noexcept { return m_talkingCheck; }
    [[nodiscard]] ScrubbySpinBox* animSpeedSpin() const noexcept { return m_animSpeedSpin; }
    [[nodiscard]] QCheckBox*  continuityCheck()   const noexcept { return m_continuityCheck; }

    // Video-specific
    [[nodiscard]] QLabel*         mediaPathLabel() const noexcept { return m_mediaPathLabel; }
    [[nodiscard]] ScrubbySpinBox* volumeSpin()     const noexcept { return m_volumeSpin; }

    // Audio-specific
    [[nodiscard]] ScrubbySpinBox* audioVolumeSpin()  const noexcept { return m_audioVolumeSpin; }
    [[nodiscard]] ScrubbySpinBox* panSpin()          const noexcept { return m_panSpin; }
    [[nodiscard]] ScrubbySpinBox* fadeInSpin()       const noexcept { return m_fadeInSpin; }
    [[nodiscard]] ScrubbySpinBox* fadeOutSpin()      const noexcept { return m_fadeOutSpin; }
    [[nodiscard]] QComboBox*      audioStreamCombo() const noexcept { return m_audioStreamCombo; }

    // Title-specific
    [[nodiscard]] QLineEdit*      textEdit()         const noexcept { return m_textEdit; }
    [[nodiscard]] QLineEdit*      fontFamilyEdit()   const noexcept { return m_fontFamilyEdit; }
    [[nodiscard]] ScrubbySpinBox* fontSizeSpin()     const noexcept { return m_fontSizeSpin; }
    [[nodiscard]] QCheckBox*      boldCheck()        const noexcept { return m_boldCheck; }
    [[nodiscard]] QCheckBox*      italicCheck()      const noexcept { return m_italicCheck; }
    [[nodiscard]] QComboBox*      alignCombo()       const noexcept { return m_alignCombo; }

    // Section widgets (for reparenting into separate tabs)
    [[nodiscard]] QWidget* transformSection() const noexcept { return m_transformSection; }
    [[nodiscard]] QWidget* shotSection()      const noexcept { return m_shotSection; }

    QSize sizeHint() const override;

signals:
    /// Emitted when any property changes via the panel.
    void propertyChanged();

    /// Emitted specifically when an audio clip's chosen audio stream changes,
    /// so the timeline can re-decode that clip's waveform (the generic
    /// propertyChanged fires on every volume/pan nudge and must NOT trigger a
    /// waveform re-decode). Carries the affected clip id.
    void audioStreamChanged(quint64 clipId);

    /// Emitted when the bound clip changes (or is cleared).
    void clipChanged(Clip* clip);

    /// Emitted when a shot switch or rebuild is needed.
    void shotSwitchRequested(uint64_t groupId, const std::string& newShotName);

private:
    void setupUI();
    void setupIdentitySection(QWidget* container);
    void setupTransformSection(QWidget* container);
    void setupCharacterSection(QWidget* container);
    void setupAnimationSection(QWidget* container);
    void setupSpineSection(QWidget* container);
    void setupPuppetSection(QWidget* container);
    void setupVideoSection(QWidget* container);
    void setupAudioSection(QWidget* container);
    void setupTitleSection(QWidget* container);
    void setupGraphicSection(QWidget* container);
    void setupCaptionSection(QWidget* container);
    void setupShotSection(QWidget* container);
    void setupEffectsSection(QWidget* container);
    void setupTransitionSection(QWidget* container);

    void showSectionsForType();
    void populateFromClip();
    // Per-type section populate helpers, dispatched from populateFromClip()
    // (PngPuppet and Caption have theirs in their own TUs: populateFromPuppet
    // / populateFromCaption).
    void populateFromSpine();
    void populateFromVideo();
    void populateFromAudio();
    void populateFromTitle();
    void populateFromGraphic();
    void populateFromTransition();
    void updateShotSection();
    void onShotChanged(const std::string& newShotName);

    /// Repopulate the Shot dropdown honoring the Show/Character navigation
    /// filters. Selects `selectName` if present (else the clip's current shot).
    void populateShotCombo(const QString& selectName);

    // Spine dropdown population
    void populateCharacterDropdown();
    void populateOutfitDropdown();
    void populateStanceDropdown();
    void populateAnimationDropdown();

    // Helpers
    ScrubbySpinBox* createScrubby(double min, double max, double step,
                                   int decimals, const QString& suffix = {});
    void makeCollapsible(QGroupBox* box);

    /// Return the playhead tick relative to the current clip's timeline-in,
    /// or 0 if no timeline/clip is set.  Used to write keyframes at the
    /// correct time (instead of always at time 0).
    [[nodiscard]] int64_t clipRelativeTick() const noexcept;

    // ── Apply property changes ──────────────────────────────────────────
    void applyLabel();
    void applyEnabled();
    void applySpeed();
    /// Commit a single transform field (the spinbox that emitted
    /// valueCommitted) as one undoable command.  Per-field so undoing a
    /// scale change never clobbers an unrelated flip/position/etc.
    void applyTransform(ScrubbySpinBox* src, double oldUi, double newUi);
    /// Live preview during scrub drag — writes current spinbox values to
    /// clip properties without creating undo commands.
    void applyTransformLive();

    void applySpineCharacter();
    void applySpineOutfit();
    void applySpineStance();
    void applySpineAnimation();
    void applySpineLooping();
    void applySpineTalking();
    void applySpineAnimSpeed();
    void applySpineContinuity();

    // PNG puppet
    void populateFromPuppet();
    void applyPuppetTalking();
    void applyPuppetOutfit();
    void applyPuppetAction();
    void applyPuppetFloat(const char* cmdName,
                          const std::function<void(PngPuppetClip*, float)>& setter,
                          float newVal, float oldVal);

    void applyVideoVolume();

    void applyAudioVolume();
    void applyAudioPan();
    void applyAudioFadeIn();
    void applyAudioFadeOut();
    void applyAudioStream();

    void applyTitleText();
    void applyTitleFontFamily();
    void applyTitleFontSize();
    void applyTitleBold();
    void applyTitleItalic();
    void applyTitleAlign();

    void applyGfxText();
    void applyGfxFontFamily();
    void applyGfxFontSize();
    void applyGfxFontWeight();
    void applyGfxItalic();
    void applyGfxAllCaps();
    void applyGfxAlign();
    void applyGfxFillColor();
    void applyGfxStrokeEnabled();
    void applyGfxStrokeWidth();
    void applyGfxStrokeColor();
    void applyGfxShadowEnabled();

    // Caption styling — applies to ALL selected caption clips at once.
    void applyCaptionText();
    void applyCaptionSpeaker();
    void applyCaptionFontFamily();
    void applyCaptionFontSize();
    void applyCaptionPosition();
    void applyCaptionTextColor();
    void applyCaptionBgColor();
    void populateFromCaption();
    // Universal text appearance presets — shared by Caption, Title, and
    // Graphic text sections. Apply/save dispatch on the current clip type.
    void applyTextPreset(int index);
    void saveTextPresetAs();
    void loadTextPresets();
    void saveTextPresetsToDisk() const;
    void rebuildPresetCombos();
    /// Collect every selected caption clip (single or multi-selection) so a
    /// style change can be applied to all of them in one undoable command.
    std::vector<CaptionClip*> captionTargets() const;

    void applyTransitionType();
    void applyTransitionDuration();
    void applyTransitionSoftness();
    void applyTransitionAlignment();

    // State
    Clip*              m_clip{nullptr};
    /// Full multi-clip selection (empty for single-clip / transition / empty
    /// states). Retained so the Shot dropdown can apply across every selected
    /// clip rather than just the single representative `m_clip`.
    std::vector<Clip*> m_multiSelection;
    SpineClip*         m_spineClip{nullptr};
    Track*             m_track{nullptr};
    size_t             m_transitionIndex{SIZE_MAX};
    CommandStack*      m_commandStack{nullptr};
    ModelManager*      m_modelManager{nullptr};
    ShotPresetManager* m_shotManager{nullptr};
    Timeline*          m_timeline{nullptr};
    bool               m_updating{false};
    AnimNamesProvider  m_animNamesProvider;
    VideoAnimNamesProvider m_videoAnimNamesProvider;

    // Header
    QLabel*       m_headerLabel{nullptr};
    QLabel*       m_typeLabel{nullptr};

    // Search + empty state + status
    QLineEdit*    m_searchField{nullptr};
    QTimer*       m_searchDebounce{nullptr};
    QLabel*       m_emptyLabel{nullptr};
    QLabel*       m_statusLabel{nullptr};
    QScrollArea*  m_scrollArea{nullptr};
    QWidget*      m_scrollContainer{nullptr};

    // Identity section
    QWidget*      m_identitySection{nullptr};
    QLineEdit*    m_labelEdit{nullptr};
    QCheckBox*    m_enabledCheck{nullptr};
    ScrubbySpinBox* m_speedSpin{nullptr};

    // Transform section
    QWidget*        m_transformSection{nullptr};
    ScrubbySpinBox* m_posXSpin{nullptr};
    ScrubbySpinBox* m_posYSpin{nullptr};
    ScrubbySpinBox* m_scaleXSpin{nullptr};
    ScrubbySpinBox* m_scaleYSpin{nullptr};
    QCheckBox*      m_flipHCheck{nullptr};   // negates scaleX
    QCheckBox*      m_flipVCheck{nullptr};   // negates scaleY
    ScrubbySpinBox* m_rotationSpin{nullptr};
    ScrubbySpinBox* m_opacitySpin{nullptr};
    // Crop spinboxes (percentage 0-100)
    QWidget*        m_cropGroup{nullptr};
    ScrubbySpinBox* m_tfCropLeftSpin{nullptr};
    ScrubbySpinBox* m_tfCropRightSpin{nullptr};
    ScrubbySpinBox* m_tfCropTopSpin{nullptr};
    ScrubbySpinBox* m_tfCropBottomSpin{nullptr};

    // Character section (Spine clips)
    QGroupBox*      m_characterSection{nullptr};
    QComboBox*      m_characterCombo{nullptr};
    QComboBox*      m_outfitCombo{nullptr};
    QComboBox*      m_stanceCombo{nullptr};

    // Animation section (Spine clips)
    QGroupBox*      m_animationSection{nullptr};
    QComboBox*      m_animationCombo{nullptr};
    QCheckBox*      m_loopingCheck{nullptr};
    QCheckBox*      m_talkingCheck{nullptr};
    ScrubbySpinBox* m_animSpeedSpin{nullptr};
    QCheckBox*      m_continuityCheck{nullptr};

    // Legacy spine section pointer (hidden, kept for backward compat)
    QWidget*        m_spineSection{nullptr};

    // PNG puppet section
    QGroupBox*      m_puppetSection{nullptr};
    PngPuppetClip*  m_puppetClip{nullptr};
    QComboBox*      m_puppetOutfitCombo{nullptr};
    QComboBox*      m_puppetActionCombo{nullptr};
    QCheckBox*      m_puppetTalkingCheck{nullptr};
    ScrubbySpinBox* m_puppetTalkSwapSpin{nullptr};
    ScrubbySpinBox* m_puppetBlinkIntervalSpin{nullptr};
    ScrubbySpinBox* m_puppetBlinkDurSpin{nullptr};
    ScrubbySpinBox* m_puppetBreathAmpSpin{nullptr};
    ScrubbySpinBox* m_puppetBreathSpeedSpin{nullptr};
    ScrubbySpinBox* m_puppetSwaySpin{nullptr};

    // Video section
    QWidget*        m_videoSection{nullptr};
    QLabel*         m_mediaPathLabel{nullptr};
    ScrubbySpinBox* m_volumeSpin{nullptr};

    // Audio section
    QWidget*        m_audioSection{nullptr};
    QComboBox*      m_audioStreamCombo{nullptr};
    ScrubbySpinBox* m_audioVolumeSpin{nullptr};
    ScrubbySpinBox* m_panSpin{nullptr};
    ScrubbySpinBox* m_fadeInSpin{nullptr};
    ScrubbySpinBox* m_fadeOutSpin{nullptr};

    // Audio effect chain editor (EQ / Dynamics) — shown for audio clips
    AudioFxSection* m_audioFxSection{nullptr};

    // Title section
    QWidget*        m_titleSection{nullptr};
    QLineEdit*      m_textEdit{nullptr};
    QLineEdit*      m_fontFamilyEdit{nullptr};
    ScrubbySpinBox* m_fontSizeSpin{nullptr};
    QCheckBox*      m_boldCheck{nullptr};
    QCheckBox*      m_italicCheck{nullptr};
    QComboBox*      m_alignCombo{nullptr};
    QComboBox*      m_titlePresetCombo{nullptr};
    QPushButton*    m_titleSavePresetBtn{nullptr};

    // Graphic section
    QWidget*        m_graphicSection{nullptr};
    QLineEdit*      m_gfxTextEdit{nullptr};
    QLineEdit*      m_gfxFontFamilyEdit{nullptr};
    ScrubbySpinBox* m_gfxFontSizeSpin{nullptr};
    ScrubbySpinBox* m_gfxFontWeightSpin{nullptr};
    QCheckBox*      m_gfxItalicCheck{nullptr};
    QCheckBox*      m_gfxAllCapsCheck{nullptr};
    QComboBox*      m_gfxAlignCombo{nullptr};
    QPushButton*    m_gfxFillColorBtn{nullptr};
    QCheckBox*      m_gfxStrokeCheck{nullptr};
    ScrubbySpinBox* m_gfxStrokeWidthSpin{nullptr};
    QPushButton*    m_gfxStrokeColorBtn{nullptr};
    QCheckBox*      m_gfxShadowCheck{nullptr};
    QComboBox*      m_gfxPresetCombo{nullptr};
    QPushButton*    m_gfxSavePresetBtn{nullptr};

    // Caption section (shown for CaptionClip — single or multi-selection)
    QWidget*        m_captionSection{nullptr};
    QPlainTextEdit* m_capTextEdit{nullptr};
    QLineEdit*      m_capSpeakerEdit{nullptr};
    QFontComboBox*  m_capFontCombo{nullptr};
    ScrubbySpinBox* m_capFontSizeSpin{nullptr};
    QComboBox*      m_capPositionCombo{nullptr};
    QPushButton*    m_capTextColorBtn{nullptr};
    QPushButton*    m_capBgColorBtn{nullptr};
    QComboBox*      m_capPresetCombo{nullptr};
    QPushButton*    m_capSavePresetBtn{nullptr};

    /// A saved text appearance, shared across caption/title/graphic text.
    /// Each clip type applies the subset of fields it supports.
    struct TextStylePreset {
        std::string name;
        std::string fontFamily{"Arial"};
        float       fontSize{32.0f};
        uint32_t    textColor{0xFFFFFFFFu};
        uint32_t    bgColor{0xCC000000u};
        int         position{0};   // caption only
        bool        bold{false};
        bool        italic{false};
        int         alignment{1};  // 0=left 1=center 2=right
    };
    std::vector<TextStylePreset> m_textPresets;

    // Transition section
    QWidget*        m_transitionSection{nullptr};
    QComboBox*      m_transTypeCombo{nullptr};
    ScrubbySpinBox* m_transDurationSpin{nullptr};
    ScrubbySpinBox* m_transSoftnessSpin{nullptr};
    QComboBox*      m_transAlignCombo{nullptr};
    QLabel*         m_transClipALabel{nullptr};
    QLabel*         m_transClipBLabel{nullptr};

    // Shot section (shown when clip belongs to a shot group)
    QWidget*        m_shotSection{nullptr};
    QComboBox*      m_shotShowCombo{nullptr};   ///< Navigation filter: show
    QComboBox*      m_shotCharCombo{nullptr};   ///< Navigation filter: character
    QComboBox*      m_shotCombo{nullptr};
    QLabel*         m_shotInfoLabel{nullptr};

    // Effects section (Premiere Pro–style collapsible effect controls)
    QWidget*        m_effectsSection{nullptr};
    QVBoxLayout*    m_fxParamsLayout{nullptr};   ///< Dynamic layout for per-effect groups
    QListWidget*    m_fxList{nullptr};            // kept for programmatic access
    QPushButton*    m_fxRemoveBtn{nullptr};

    std::atomic<bool> m_destroying{false};
};

} // namespace rt
