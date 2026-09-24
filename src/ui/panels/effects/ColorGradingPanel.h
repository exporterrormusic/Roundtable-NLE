/*
 * ColorGradingPanel — –style dedicated color grading panel.
 *
 * Color grading panel layout:
 * 1. Header: "Color Panel" dropdown + fx toggle
 * 2. Edit / Settings tab bar
 * 3. Collapsible sections:
 * - Basic Correction (toggle)
 * - Input LUT (combo)
 * - Color: Temperature, Tint sliders
 * - Light: Exposure, Contrast, Highlights, Shadows, Whites, Blacks
 * - Intensity / Saturation
 * - Creative (toggle)
 * - Faded Film, Sharpen, Vibrance, Saturation
 * - Curves (toggle — placeholder)
 * - Color Wheels & Match (toggle — placeholder)
 * - HSL Secondary (toggle — placeholder)
 * - Vignette (toggle)
 * - Amount, Midpoint, Roundness, Feather
 *
 * Automatically creates a ColorGrading effect on the selected clip
 * when any slider is adjusted .
 *
 * Binds to the same effect instance that appears in EffectControlsPanel.
 */

#pragma once

#include <QWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QComboBox>
#include <QToolButton>
#include <QScrollArea>
#include <QCheckBox>

#include "effects/ColorGrading.h"

#include <optional>
#include <vector>

namespace rt {

class Clip;
class Track;
class CommandStack;
class Timeline;
class ScrubbySpinBox;
class ColorWheelWidget;
class CurveEditor;

class ColorGradingPanel : public QWidget
{
 Q_OBJECT

public:
 explicit ColorGradingPanel(QWidget* parent = nullptr);
 ~ColorGradingPanel() override = default;

 // ── Dependency injection ────────────────────────────────────────────
 void setCommandStack(CommandStack* stack) noexcept { m_commandStack = stack; }
 void setTimeline(Timeline* tl) noexcept { m_timeline = tl; }

 // ── Clip binding ────────────────────────────────────────────────────
 void setClip(Clip* clip, Track* track = nullptr);
 void clearClip();

 [[nodiscard]] Clip* clip() const noexcept { return m_clip; }

signals:
 void propertyChanged();

private:
 [[nodiscard]] bool canMutateBoundClip() const noexcept;
 void setupUI();
 void populateFromEffect();
 void applyToEffect();
 void applyCurvesToEffect();
 void ensureColorGradingEffect();

 ScrubbySpinBox* makeSlider(const QString& label, double min, double max,
 double defaultVal, int decimals,
 const QString& suffix, QVBoxLayout* parent);
 void pushUndoCommand(ScrubbySpinBox* changedSpin, double oldVal);
 void pushUndoState(ColorGrading::State before, const char* description);

 // ── Section header with collapsible arrow + enable toggle ───────────
 struct SectionInfo {
 QWidget* header{nullptr};
 QToolButton* arrow{nullptr};
 QToolButton* enableBtn{nullptr};
 std::vector<QWidget*> children;
 bool enabled{true};
 };
 QWidget* makeSectionHeader(const QString& title, bool hasToggle, QVBoxLayout* parent);
 void wireSectionCollapse();

 // ── State ───────────────────────────────────────────────────────────
 Clip* m_clip{nullptr};
 Track* m_track{nullptr};
 CommandStack* m_commandStack{nullptr};
 Timeline* m_timeline{nullptr};
 ColorGrading* m_effect{nullptr}; ///< Currently bound effect instance
 bool m_updating{false}; ///< Guard against feedback loops
 std::optional<ColorGrading::State> m_curveUndoBefore;

 // ── Layout ──────────────────────────────────────────────────────────
 QLabel* m_clipLabel{nullptr}; QLabel* m_typeBadge{nullptr};
 QLabel* m_emptyLabel{nullptr};
 QLabel* m_statusLabel{nullptr}; QScrollArea* m_scrollArea{nullptr};
 QToolButton* m_fxButton{nullptr};
 QWidget* m_scrollContent{nullptr};
 QVBoxLayout* m_mainLayout{nullptr};

 // ── Basic Correction ────────────────────────────────────────────────
 ScrubbySpinBox* m_temperatureSpin{nullptr};
 ScrubbySpinBox* m_tintSpin{nullptr};
 ScrubbySpinBox* m_exposureSpin{nullptr};
 ScrubbySpinBox* m_contrastSpin{nullptr};
 ScrubbySpinBox* m_highlightsSpin{nullptr};
 ScrubbySpinBox* m_shadowsSpin{nullptr};
 ScrubbySpinBox* m_whitesSpin{nullptr};
 ScrubbySpinBox* m_blacksSpin{nullptr};
 ScrubbySpinBox* m_saturationSpin{nullptr};

 // ── Creative ────────────────────────────────────────────────────────
 ScrubbySpinBox* m_fadedFilmSpin{nullptr};
 ScrubbySpinBox* m_sharpenSpin{nullptr};
 ScrubbySpinBox* m_vibranceSpin{nullptr};
 ScrubbySpinBox* m_creativeSatSpin{nullptr};

 // ── Curves ────────────────────────────────────────────────────────
 CurveEditor* m_curveEditor{nullptr};
 QComboBox* m_curveChannelCombo{nullptr};

 // ── Color Wheels ────────────────────────────────────────────────────
 ColorWheelWidget* m_shadowWheel{nullptr};
 ColorWheelWidget* m_midtoneWheel{nullptr};
 ColorWheelWidget* m_highlightWheel{nullptr};

 // ── HSL Secondary ───────────────────────────────────────────────────
 ScrubbySpinBox* m_hslHueCenterSpin{nullptr};
 ScrubbySpinBox* m_hslHueWidthSpin{nullptr};
 ScrubbySpinBox* m_hslSatMinSpin{nullptr};
 ScrubbySpinBox* m_hslSatMaxSpin{nullptr};
 ScrubbySpinBox* m_hslLumMinSpin{nullptr};
 ScrubbySpinBox* m_hslLumMaxSpin{nullptr};
 ScrubbySpinBox* m_hslHueShiftSpin{nullptr};
 ScrubbySpinBox* m_hslSatAdjustSpin{nullptr};
 ScrubbySpinBox* m_hslLumAdjustSpin{nullptr};

 // ── Vignette ────────────────────────────────────────────────────────
 ScrubbySpinBox* m_vigAmountSpin{nullptr};
 ScrubbySpinBox* m_vigMidpointSpin{nullptr};
 ScrubbySpinBox* m_vigRoundnessSpin{nullptr};
 ScrubbySpinBox* m_vigFeatherSpin{nullptr};

 // ── All sections (for collapsible wiring) ───────────────────────────
 std::vector<SectionInfo> m_sections;
};

} // namespace rt
