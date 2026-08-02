/*
 * GraphicsEditorPanel Ã¢â‚¬â€ Ã¢â‚¬â€œstyle Essential Graphics panel.
 *
 * Matches the 2024/2026 layout:
 * 1. Clip title bar
 * 2. Layer list (with eye/lock icons, reorderable)
 * 3. Text section (font, style buttons, size, alignment, spacing)
 * 4. Appearance section (Fill, Stroke, Shadow)
 * 5. Align and Transform section (position, anchor, scale, rotation, opacity)
 *
 * Supports editing the currently-selected layer within a GraphicClip.
 */

#pragma once

#include <QWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QCheckBox>
#include <QComboBox>
#include <QPushButton>
#include <QToolButton>
#include <QPlainTextEdit>
#include <QScrollArea>
#include <QSlider>
#include <QListWidget>
#include <QSplitter>

#include <memory>
#include <vector>

#include "timeline/GraphicLayer.h"

namespace rt {

class Clip;
class Track;
class GraphicClip;
class TextLayer;
class CommandStack;
class Timeline;
class ScrubbySpinBox;

class GraphicsEditorPanel : public QWidget
{
 Q_OBJECT

public:
 explicit GraphicsEditorPanel(QWidget* parent = nullptr);
 ~GraphicsEditorPanel() override = default;

 void setClip(Clip* clip, Track* track = nullptr);
 [[nodiscard]] Clip* clip() const noexcept { return m_clip; }
 [[nodiscard]] GraphicClip* graphicClip() const noexcept { return m_graphicClip; }
 [[nodiscard]] GraphicLayer* selectedLayer() const noexcept { return m_selectedLayer; }
 [[nodiscard]] int selectedLayerIndex() const noexcept { return m_selectedLayerIdx; }

 /// Stack indices of every selected layer in the layer list (focused +
 /// all multi-selected rows). Empty if no clip / no selection. Used by
 /// the timeline workspace to apply group-move deltas in the program
 /// monitor when more than one layer is selected.
 [[nodiscard]] std::vector<int> selectedLayerStackIndices() const;

 void refresh();
 void clearClip();

 /// Programmatically select a layer by stack index (0 = bottom).

 bool eventFilter(QObject* obj, QEvent* event) override;
 void selectLayerByStackIndex(int stackIdx);

 /// Toggle a layer's membership in the multi-selection set without
 /// disturbing the existing selection — used when the user
 /// Shift/Ctrl-clicks a layer in the program monitor. The clicked
 /// layer also becomes the focused row (so the single-layer edit
 /// controls switch to it). No-op if the index is out of range.
 void toggleLayerInSelection(int stackIdx);

 void copySelectedLayer();
 void pasteLayer();
 void deleteSelectedLayer();

 void setCommandStack(CommandStack* stack) noexcept { m_commandStack = stack; }
 void setTimeline(Timeline* tl) noexcept { m_timeline = tl; }

 /// The Program Monitor owns the live QTextDocument while inline editing is
 /// active. Character controls route to that document instead of overwriting
 /// the entire TextLayer, matching Premiere's selected-range behavior.
 void setMonitorTextEditing(bool active) noexcept {
     m_monitorTextEditing = active;
 }
 [[nodiscard]] bool monitorTextEditing() const noexcept {
     return m_monitorTextEditing;
 }
 [[nodiscard]] QWidget* textFormattingWidget() const noexcept {
     return m_editContainer;
 }
 void setInlineTextSelectionFormat(const QString& family, float pointSize,
                                   int weight, bool italic,
                                   bool allCaps, bool smallCaps,
                                   float tracking, float baselineShift,
                                   float leading,
                                   uint32_t mixedFlags);
 void setInlineTextAdvancedFormat(const QString& fontStyle, float kerning,
                                  float tabWidth, float tsume,
                                  bool fauxBold, bool fauxItalic,
                                  bool underline, bool superscript,
                                  bool subscript, uint32_t mixedFlags);
 void setInlineTextSelectionAppearance(
     bool fillEnabled, uint32_t fillColor,
     bool strokeEnabled, uint32_t strokeColor, float strokeWidth,
     int strokePosition, bool shadowEnabled, uint32_t shadowColor,
     float shadowDistance, float shadowAngle, float shadowSoftness,
     float shadowOpacity, bool backgroundEnabled,
     uint32_t backgroundColor, float backgroundPadding,
     uint32_t mixedFlags);
 void setInlineParagraphFormat(int alignment, bool rightToLeft,
                               uint32_t mixedFlags);

signals:
 void propertyChanged();
 void inlineFontFamilyRequested(const QString& family);
 void inlineFontSizeRequested(float pointSize);
 void inlineFontWeightRequested(int weight);
 void inlineItalicRequested(bool italic);
 void inlineCapitalizationRequested(bool allCaps, bool smallCaps);
 void inlineTrackingRequested(float tracking);
 void inlineBaselineShiftRequested(float baselineShift);
 void inlineLeadingRequested(float leading);
 void inlineFontStyleRequested(const QString& styleName);
 void inlineKerningRequested(float kerning);
 void inlineTabWidthRequested(float tabWidth);
 void inlineTsumeRequested(float tsume);
 void inlineFauxStylesRequested(bool fauxBold, bool fauxItalic);
 void inlineUnderlineRequested(bool underline);
 void inlineScriptRequested(bool superscript, bool subscript);
 void inlineFillRequested(bool enabled, uint32_t color);
 void inlineStrokeRequested(bool enabled, uint32_t color, float width,
                            int position);
 void inlineShadowRequested(bool enabled, uint32_t color, float distance,
                            float angle, float softness, float opacity);
 void inlineBackgroundRequested(bool enabled, uint32_t color, float padding);
 void inlineParagraphAlignmentRequested(int alignment);
 void inlineParagraphDirectionRequested(bool rightToLeft);
 /// Emitted when the user selects a different layer in the layer list.
 void layerSelected(GraphicLayer* layer, int layerIndex);

 /// Emitted when the multi-selection set in the layer list changes
 /// (Shift / Ctrl click). Carries the stack indices of every selected
 /// row. The workspace consumes this to group-move all selected layers
 /// when the user drags in the program monitor.
 void layerSelectionSetChanged(const std::vector<int>& stackIdxs);

 /// Emitted when the user double-clicks a layer row — request to jump
 /// into editing that layer's text (Premiere Pro behavior).
 void textEditRequested();

private:
 void setupUI();
 void rebuildLayerList();
 void selectLayer(int index);
 void buildEditControls();
 void clearEditControls();
 void populateFromLayer();
 void applyTextProperties();
 void applyAppearance();
 void applyLayerTransform();

 // Undo support for live property edits: snapshot the selected layer when it
 // becomes the edit target, then on edit-commit push a command that restores
 // the before/after state in place (by stable layer id). captureEditBaseline()
 // is called on selection; commitLayerEdit() from each control's commit signal.
 void captureEditBaseline();
 void commitLayerEdit();

 // Helpers
 ScrubbySpinBox* makeScrubby(double min, double max, double step,
 int decimals, const QString& suffix = {});
 QToolButton* makeStyleButton(const QString& text, const QString& tooltip);

 // Ã¢â€â‚¬Ã¢â€â‚¬ State Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬
 Clip* m_clip{nullptr};
 Track* m_track{nullptr};
 GraphicClip* m_graphicClip{nullptr};
 GraphicLayer* m_selectedLayer{nullptr};
 int m_selectedLayerIdx{-1};
 CommandStack* m_commandStack{nullptr};
 Timeline* m_timeline{nullptr};
 bool m_updating{false};
 std::unique_ptr<GraphicLayer> m_copiedLayer; ///< Clipboard for layer copy/paste

 // Undo edit-gesture state (see captureEditBaseline / commitLayerEdit).
 std::unique_ptr<GraphicLayer> m_editBaseline;   ///< Layer snapshot at gesture start
 uint64_t m_editBaselineId{0};                   ///< Stable id of the snapshotted layer
 bool     m_layerEditDirty{false};               ///< A property changed since the snapshot
 bool     m_monitorTextEditing{false};            ///< Route character controls to monitor selection

 // Ã¢â€â‚¬Ã¢â€â‚¬ Top-level layout Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬
 QLabel* m_clipNameLabel{nullptr};
 QLabel* m_typeBadge{nullptr};
 QLabel* m_emptyLabel{nullptr};
 QLabel* m_statusLabel{nullptr};
 QListWidget* m_layerList{nullptr};
 QSplitter* m_splitter{nullptr};
 QScrollArea* m_scrollArea{nullptr};
 QWidget* m_editContainer{nullptr};
 QVBoxLayout* m_editLayout{nullptr};

 // Ã¢â€â‚¬Ã¢â€â‚¬ Text section Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬
 QWidget* m_textSection{nullptr};
 QPlainTextEdit* m_textContentEdit{nullptr};  // editable text-string box
 QComboBox* m_fontCombo{nullptr};
 QComboBox* m_fontStyleCombo{nullptr};
 QToolButton* m_boldBtn{nullptr};
 QToolButton* m_italicBtn{nullptr};
 QToolButton* m_allCapsBtn{nullptr};
 QToolButton* m_smallCapsBtn{nullptr};
 QToolButton* m_fauxBoldBtn{nullptr};
 QToolButton* m_fauxItalicBtn{nullptr};
 QToolButton* m_underlineBtn{nullptr};
 QToolButton* m_superscriptBtn{nullptr};
 QToolButton* m_subscriptBtn{nullptr};
 QToolButton* m_rtlBtn{nullptr};
 QSlider* m_fontSizeSlider{nullptr};
 ScrubbySpinBox* m_fontSizeSpin{nullptr};

 // Alignment buttons
 QToolButton* m_alignLeftBtn{nullptr};
 QToolButton* m_alignCenterBtn{nullptr};
 QToolButton* m_alignRightBtn{nullptr};
 QToolButton* m_alignJustifyBtn{nullptr};
 QToolButton* m_valignTopBtn{nullptr};
 QToolButton* m_valignMiddleBtn{nullptr};
 QToolButton* m_valignBottomBtn{nullptr};

 // Spacing
 ScrubbySpinBox* m_trackingSpin{nullptr};
 ScrubbySpinBox* m_leadingSpin{nullptr};
 ScrubbySpinBox* m_baselineShiftSpin{nullptr};
 ScrubbySpinBox* m_kerningSpin{nullptr};
 ScrubbySpinBox* m_tabWidthSpin{nullptr};
 ScrubbySpinBox* m_tsumeSpin{nullptr};

 // Paragraph box (word-wrap to a fixed width)
 QCheckBox* m_wrapCheck{nullptr};
 ScrubbySpinBox* m_wrapWidthSpin{nullptr};
 ScrubbySpinBox* m_wrapHeightSpin{nullptr};

 // Ã¢â€â‚¬Ã¢â€â‚¬ Appearance section Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬
 QWidget* m_appearanceSection{nullptr};
 QCheckBox* m_fillCheck{nullptr};
 QPushButton* m_fillColorBtn{nullptr};
 QCheckBox* m_strokeCheck{nullptr};
 QPushButton* m_strokeColorBtn{nullptr};
 ScrubbySpinBox* m_strokeWidthSpin{nullptr};
 ScrubbySpinBox* m_strokeOpacitySpin{nullptr};
 QComboBox* m_strokePosCombo{nullptr};
 QCheckBox* m_shadowCheck{nullptr};
 QPushButton* m_shadowColorBtn{nullptr};
 ScrubbySpinBox* m_shadowDistanceSpin{nullptr};
 ScrubbySpinBox* m_shadowAngleSpin{nullptr};
 ScrubbySpinBox* m_shadowSoftnessSpin{nullptr};
 ScrubbySpinBox* m_shadowOpacitySpin{nullptr};
 QCheckBox* m_backgroundCheck{nullptr};
 QPushButton* m_backgroundColorBtn{nullptr};
 ScrubbySpinBox* m_backgroundPaddingSpin{nullptr};
 ScrubbySpinBox* m_backgroundOpacitySpin{nullptr};
 QCheckBox* m_maskWithTextCheck{nullptr};

 // Ã¢â€â‚¬Ã¢â€â‚¬ Align and Transform section Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬
 QWidget* m_transformSection{nullptr};
 ScrubbySpinBox* m_posXSpin{nullptr};
 ScrubbySpinBox* m_posYSpin{nullptr};
 ScrubbySpinBox* m_anchorXSpin{nullptr};
 ScrubbySpinBox* m_anchorYSpin{nullptr};
 ScrubbySpinBox* m_scaleXSpin{nullptr};
 ScrubbySpinBox* m_scaleYSpin{nullptr};
 QCheckBox* m_uniformScaleCheck{nullptr};
 ScrubbySpinBox* m_rotationSpin{nullptr};
 ScrubbySpinBox* m_opacitySpin{nullptr};

 // Ã¢â€â‚¬Ã¢â€â‚¬ Collapsible sections Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬
 struct SectionInfo {
 QWidget* header{nullptr};
 QToolButton* arrow{nullptr};
 std::vector<QWidget*> children;
 };
 std::vector<SectionInfo> m_sectionArrows;
};

} // namespace rt
