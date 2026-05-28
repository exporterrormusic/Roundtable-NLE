/*
 * EffectControlsPanelTree.cpp -- clearPropertyTree, buildPropertyTree,
 * and populateFromClip.
 *
 * Split from EffectControlsPanel.cpp for maintainability.
 */

#include "panels/effects/EffectControlsPanel.h"
#include "widgets/ScrubbySpinBox.h"
#include "Theme.h"

#include "timeline/Clip.h"
#include "timeline/AudioClip.h"
#include "timeline/VideoClip.h"
#include "timeline/SpineClip.h"
#include "timeline/KeyframeTrack.h"
#include "timeline/OpacityMask.h"
#include "command/CommandStack.h"
#include "command/LambdaCommand.h"
#include "command/commands/EffectCommands.h"
#include "command/commands/KeyframeCmds.h"
#include "effects/Effect.h"
#include "effects/EffectStack.h"
#include "effects/ChromaKey.h"
#include "effects/Ots.h"

#include <QFrame>
#include <QGridLayout>
#include <QMenu>
#include <QColorDialog>
#include <QComboBox>
#include <QPushButton>

#include <cmath>

namespace rt {

namespace {
// Match EffectControlsPanel.cpp: UI is dB, track stores linear gain.
constexpr float kAudioVolumeMinDb = -60.0f;
inline float gainToDb(float gain) noexcept {
    if (gain <= 0.0f) return kAudioVolumeMinDb;
    float db = 20.0f * std::log10(gain);
    return db < kAudioVolumeMinDb ? kAudioVolumeMinDb : db;
}
} // namespace

void EffectControlsPanel::clearPropertyTree()
{
    // Preserve collapse state before destroying widgets
    for (auto& sec : m_sectionArrows) {
        if (!sec.title.isEmpty())
            m_sectionCollapsed[sec.title] =
                (sec.arrow->text() == QStringLiteral("\u25B6"));
    }

    m_propertyRows.clear();
    m_sectionArrows.clear();
    m_selectedEffectIndex = -1;
    m_effectHeaders.clear();

    // Remove all widgets from m_propLayout.
    //
    // Do NOT call setParent(nullptr) here. On Windows, reparenting a QWidget
    // to nullptr immediately makes it a top-level widget — Qt realises its
    // native HWND, and the OS briefly shows it as a tiny blank window at the
    // Win32 default position. deleteLater() then destroys it on the next
    // event-loop tick, but the window has already flashed on-screen. After
    // a property-heavy refresh (e.g. undo of a ghost-track-drop that had a
    // clip selected) this produces a storm of blank popup windows.
    //
    // takeAt() has already removed the widget from the layout; hiding it
    // suppresses paint until deleteLater() runs.
    while (m_propLayout->count() > 0) {
        auto* item = m_propLayout->takeAt(0);
        if (item->widget()) {
            item->widget()->hide();
            item->widget()->deleteLater();
        }
        delete item;
    }

    m_motionSection = nullptr;
    m_cropSection = nullptr;
    m_opacitySection = nullptr;
    m_timeRemapSection = nullptr;
    m_effectsContainer = nullptr;
    m_posXSpin = nullptr;
    m_posYSpin = nullptr;
    m_scaleSpin = nullptr;
    m_scaleWSpin = nullptr;
    m_uniformScaleCheck = nullptr;
    // Transform-row member ptrs — used by setSelectedGraphicLayer to
    // retarget rows between clip-level and layer-level transforms
    // without rebuilding the tree. Must be nulled here so a stray
    // post-clear access doesn't dereference a dangling widget.
    m_posRow = nullptr;
    m_scaleRow = nullptr;
    m_scaleWRow = nullptr;
    m_rotationRow = nullptr;
    m_anchorRow = nullptr;
    m_opacityRow = nullptr;
    m_rotationSpin = nullptr;
    m_anchorXSpin = nullptr;
    m_anchorYSpin = nullptr;
    m_antiFlickerSpin = nullptr;
    m_cropLeftSpin = nullptr;
    m_cropTopSpin = nullptr;
    m_cropRightSpin = nullptr;
    m_cropBottomSpin = nullptr;
    m_opacitySpin = nullptr;
    m_blendModeCombo = nullptr;
    m_speedSpin = nullptr;
    m_panSpin = nullptr;
    m_audioVolumeSpin = nullptr;
}


void EffectControlsPanel::buildPropertyTree()
{
    if (!m_clip) return;

    const auto& tc = Theme::colors();
    int rowIdx = 0;

    // â”€â”€ Section header helper â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    auto makeSectionHeader = [&](const QString& title,
                                 bool hasFxToggle = true) -> QWidget* {
        auto* header = new QWidget(m_propContainer);
        header->setFixedHeight(28);
        header->setCursor(Qt::PointingHandCursor);
        header->setStyleSheet(QStringLiteral(
            "background: %1; border-bottom: 1px solid %2;")
            .arg(Theme::hex(tc.surface2), Theme::hex(tc.border)));

        auto* hl = new QHBoxLayout(header);
        hl->setContentsMargins(6, 0, 6, 0);
        hl->setSpacing(6);

        auto* arrow = new QToolButton(header);
        arrow->setText(QStringLiteral("\u25BC"));  // â–¼
        arrow->setFixedSize(16, 20);
        arrow->setStyleSheet(QStringLiteral(
            "QToolButton { color: %1; font-size: 11px; background: transparent; border: none; padding: 0; }"
            "QToolButton:hover { color: %2; }")
            .arg(Theme::hex(tc.textTertiary), Theme::hex(tc.textPrimary)));
        hl->addWidget(arrow);

        if (hasFxToggle) {
            auto* fxLabel = new QLabel("fx", header);
            fxLabel->setFixedWidth(14);
            fxLabel->setStyleSheet(QStringLiteral(
                "color: %1; font-size: 11px; font-weight: bold; font-style: italic; background: transparent;")
                .arg(Theme::hex(tc.accent)));
            hl->addWidget(fxLabel);
        }

        auto* titleLabel = new QLabel(title, header);
        titleLabel->setStyleSheet(QStringLiteral(
            "color: %1; font-size: 12px; font-weight: bold; background: transparent;")
            .arg(Theme::hex(tc.textPrimary)));
        hl->addWidget(titleLabel);
        hl->addStretch();

        // Reset button (↺) — resets all properties in this section to defaults
        auto* resetBtn = new QToolButton(header);
        resetBtn->setText(QStringLiteral("\u21BA"));  // ↺
        resetBtn->setFixedSize(18, 20);
        resetBtn->setToolTip(QStringLiteral("Reset %1 to defaults").arg(title));
        resetBtn->setStyleSheet(QStringLiteral(
            "QToolButton { color: %1; font-size: 12px; background: transparent; border: none; padding: 0; }"
            "QToolButton:hover { color: %2; }")
            .arg(Theme::hex(tc.textTertiary), Theme::hex(tc.warning)));
        hl->addWidget(resetBtn);

        // Collect child widgets added after this header until next header
        // We'll wire up the click in a post-pass below.
        m_sectionArrows.push_back({header, arrow, {}, title, resetBtn});

        return header;
    };

    // â”€â”€ Row builder helper â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    auto makeRow = [&](const QString& name,
                       KeyframeTrack<float>* track) -> PropertyRow* {
        auto* row = new PropertyRow(name, track, m_propContainer);
        row->setRowIndex(rowIdx++);
        m_propertyRows.push_back(row);

        // Wire keyframe signals
        connect(row, &PropertyRow::addKeyframeRequested,
                this, &EffectControlsPanel::onAddKeyframe);
        connect(row, &PropertyRow::deleteKeyframeRequested,
                this, &EffectControlsPanel::onDeleteKeyframe);
        connect(row, &PropertyRow::goToPrevKeyframe,
                this, &EffectControlsPanel::onGoToPrevKeyframe);
        connect(row, &PropertyRow::goToNextKeyframe,
                this, &EffectControlsPanel::onGoToNextKeyframe);
        connect(row, &PropertyRow::resetRequested,
                this, [this, row]() { resetPropertyRow(row); });
        connect(row, &PropertyRow::keyframingToggled,
                this, [this](KeyframeTrack<float>* trk, bool enabled) {
            if (!trk || !m_clip) return;
            int64_t t = clipRelativeTick();

            // Determine if we should also toggle the companion scale track
            KeyframeTrack<float>* companion = nullptr;
            if (m_uniformScaleCheck && m_uniformScaleCheck->isChecked()) {
                if (trk == effScaleX())
                    companion = effScaleY();
                else if (trk == effScaleY())
                    companion = effScaleX();
            }

            if (enabled) {
                // Toggling ON: create a keyframe at the current playhead
                float val = trk->evaluate(t);
                if (m_commandStack) {
                    m_commandStack->execute(
                        std::make_unique<AddKeyframeCommand>(trk, t, val));
                } else {
                    trk->addKeyframe(t, val);
                }
                if (companion) {
                    float cVal = companion->evaluate(t);
                    if (m_commandStack) {
                        m_commandStack->execute(
                            std::make_unique<AddKeyframeCommand>(companion, t, cVal));
                    } else {
                        companion->addKeyframe(t, cVal);
                    }
                    // Sync companion row stopwatch
                    if (m_scaleWRow && companion == effScaleY()) {
                        m_scaleWRow->stopwatchButton()->blockSignals(true);
                        m_scaleWRow->stopwatchButton()->setChecked(true);
                        m_scaleWRow->stopwatchButton()->blockSignals(false);
                    }
                }
            } else {
                // Toggling OFF: freeze current value as default, remove all keyframes
                float val = trk->evaluate(t);
                auto savedKfs = std::make_shared<std::vector<Keyframe<float>>>(
                    trk->keyframes().begin(), trk->keyframes().end());
                float oldDefault = trk->defaultValue();
                while (trk->keyframeCount() > 0)
                    trk->removeKeyframe(trk->keyframeCount() - 1);
                trk->setDefaultValue(val);

                // Also disable companion
                std::shared_ptr<std::vector<Keyframe<float>>> companionSavedKfs;
                float companionOldDefault = 0.0f;
                float companionNewDefault = 0.0f;
                if (companion) {
                    companionNewDefault = companion->evaluate(t);
                    companionSavedKfs = std::make_shared<std::vector<Keyframe<float>>>(
                        companion->keyframes().begin(), companion->keyframes().end());
                    companionOldDefault = companion->defaultValue();
                    while (companion->keyframeCount() > 0)
                        companion->removeKeyframe(companion->keyframeCount() - 1);
                    companion->setDefaultValue(companionNewDefault);
                    // Sync companion row stopwatch
                    if (m_scaleWRow && companion == effScaleY()) {
                        m_scaleWRow->stopwatchButton()->blockSignals(true);
                        m_scaleWRow->stopwatchButton()->setChecked(false);
                        m_scaleWRow->stopwatchButton()->blockSignals(false);
                    }
                }

                if (m_commandStack) {
                    float newDefault = val;
                    m_commandStack->pushWithoutExecute(
                        std::make_unique<LambdaCommand>(
                            "Disable Keyframing",
                            [trk, newDefault, companion, companionNewDefault]() {
                                while (trk->keyframeCount() > 0)
                                    trk->removeKeyframe(trk->keyframeCount() - 1);
                                trk->setDefaultValue(newDefault);
                                if (companion) {
                                    while (companion->keyframeCount() > 0)
                                        companion->removeKeyframe(companion->keyframeCount() - 1);
                                    companion->setDefaultValue(companionNewDefault);
                                }
                            },
                            [trk, savedKfs, oldDefault, companion, companionSavedKfs, companionOldDefault]() {
                                while (trk->keyframeCount() > 0)
                                    trk->removeKeyframe(trk->keyframeCount() - 1);
                                trk->setDefaultValue(oldDefault);
                                for (const auto& kf : *savedKfs)
                                    trk->restoreKeyframe(kf);
                                if (companion && companionSavedKfs) {
                                    while (companion->keyframeCount() > 0)
                                        companion->removeKeyframe(companion->keyframeCount() - 1);
                                    companion->setDefaultValue(companionOldDefault);
                                    for (const auto& kf : *companionSavedKfs)
                                        companion->restoreKeyframe(kf);
                                }
                            }));
                }
            }
            m_kfTimeline->update();
            emit propertyChanged();
        });

        return row;
    };

    // Audio clips only show Speed â€” no visual transform
    bool isAudio = (m_clip->clipType() == ClipType::Audio);

    // â•”â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
    // â•‘  VIDEO / VISUAL PROPERTIES â€” Motion, Crop, Opacity
    // â•šâ•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•

    if (!isAudio) {
        // "Video" header label
        auto* videoLabel = new QLabel("Video", m_propContainer);
        videoLabel->setFixedHeight(24);
        videoLabel->setStyleSheet(QStringLiteral(
            "background: %1; color: %2; font-size: 11px; padding-left: 8px; "
            "border-bottom: 1px solid %3;")
            .arg(Theme::hex(tc.surface3), Theme::hex(tc.textSecondary),
                 Theme::hex(tc.border)));
        m_propLayout->addWidget(videoLabel);

        // â”€â”€ Motion section â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
        m_motionSection = makeSectionHeader("Motion");
        m_propLayout->addWidget(m_motionSection);

        // Position — bound to the active source (clip OR selected layer).
        // Position is a compound 2D property: X and Y are independent
        // KeyframeTracks but the user expects a single "Position keyframe"
        // to capture both. Binding effPosY() as an extra track on the row
        // makes the +keyframe button, delete, prev/next, and diamond drag
        // act on both tracks together.
        m_posRow = makeRow("Position", effPosX());
        m_posRow->addExtraTrack(effPosY());
        m_posXSpin = createScrubby(-10000, 10000, 1.0, 1);
        m_posYSpin = createScrubby(-10000, 10000, 1.0, 1);
        m_posRow->addValuePair(m_posXSpin, m_posYSpin);
        m_propLayout->addWidget(m_posRow);

        // Scale — when Uniform Scale is on, bind scaleY as an extra so the
        // row's diamond, prev/next nav, and timeline drag all act on BOTH
        // axes (otherwise scaleX animates while scaleY stays static).
        m_scaleRow = makeRow("Scale", effScaleX());
        m_scaleRow->addExtraTrack(effScaleY());
        m_scaleSpin = createScrubby(0, 1000, 0.1, 1);
        m_scaleRow->addValueWidget(m_scaleSpin);
        m_propLayout->addWidget(m_scaleRow);

        // Scale Width (hidden when Uniform Scale is on)
        m_scaleWRow = makeRow("Scale Width", effScaleY());
        m_scaleWSpin = createScrubby(0, 1000, 0.1, 1);
        m_scaleWRow->addValueWidget(m_scaleWSpin);
        m_propLayout->addWidget(m_scaleWRow);
        m_scaleWRow->setVisible(false);

        // Uniform Scale checkbox (no keyframe track â€” just a toggle)
        m_uniformScaleCheck = new QCheckBox("Uniform Scale", m_propContainer);
        m_uniformScaleCheck->setChecked(true);
        m_uniformScaleCheck->setFixedHeight(28);
        m_uniformScaleCheck->setStyleSheet(QStringLiteral(
            "QCheckBox { color: %1; font-size: 12px; padding-left: 36px; background: transparent; }"
            "QCheckBox::indicator { width: 14px; height: 14px; }")
            .arg(Theme::hex(tc.textPrimary)));
        connect(m_uniformScaleCheck, &QCheckBox::toggled, this, [this](bool uniform) {
            if (m_scaleWRow) m_scaleWRow->setVisible(!uniform);
            if (m_scaleRow) {
                // Re-attach scaleY as the Scale row's compound sibling when
                // uniform turns on, detach when it turns off.
                if (uniform) m_scaleRow->addExtraTrack(effScaleY());
                else         m_scaleRow->removeExtraTrack(effScaleY());
            }
            if (uniform && m_scaleSpin && m_scaleWSpin) {
                m_scaleWSpin->setValue(m_scaleSpin->value());
            }
            if (m_kfTimeline) m_kfTimeline->update();
        });
        m_propLayout->addWidget(m_uniformScaleCheck);

        // Rotation
        m_rotationRow = makeRow("Rotation", effRotation());
        m_rotationSpin = createScrubby(-3600, 3600, 0.1, 1);
        m_rotationRow->addValueWidget(m_rotationSpin);
        m_propLayout->addWidget(m_rotationRow);

        // Anchor Point — clip-level (or selected graphic layer) anchorX/Y
        // track. The keyframe nav buttons / stopwatch operate on anchorX
        // as the row's primary track (Y follows via the same write path).
        m_anchorRow = makeRow("Anchor Point", effAnchorX());
        m_anchorRow->addExtraTrack(effAnchorY());
        m_anchorXSpin = createScrubby(-10000, 10000, 1.0, 1);
        m_anchorYSpin = createScrubby(-10000, 10000, 1.0, 1);
        m_anchorRow->addValuePair(m_anchorXSpin, m_anchorYSpin);
        m_propLayout->addWidget(m_anchorRow);

        // Anti-flicker Filter
        auto* antiFlickerRow = makeRow("Anti-flicker Filter", nullptr);
        m_antiFlickerSpin = createScrubby(0, 100, 0.01, 2);
        antiFlickerRow->addValueWidget(m_antiFlickerSpin);
        m_propLayout->addWidget(antiFlickerRow);

        // â”€â”€ Crop section â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
        m_cropSection = makeSectionHeader("Crop", false);
        m_propLayout->addWidget(m_cropSection);

        auto* cropLRow = makeRow("Crop Left", nullptr);
        m_cropLeftSpin = createScrubby(0, 100, 0.5, 1, " %");
        cropLRow->addValueWidget(m_cropLeftSpin);
        m_propLayout->addWidget(cropLRow);

        auto* cropTRow = makeRow("Crop Top", nullptr);
        m_cropTopSpin = createScrubby(0, 100, 0.5, 1, " %");
        cropTRow->addValueWidget(m_cropTopSpin);
        m_propLayout->addWidget(cropTRow);

        auto* cropRRow = makeRow("Crop Right", nullptr);
        m_cropRightSpin = createScrubby(0, 100, 0.5, 1, " %");
        cropRRow->addValueWidget(m_cropRightSpin);
        m_propLayout->addWidget(cropRRow);

        auto* cropBRow = makeRow("Crop Bottom", nullptr);
        m_cropBottomSpin = createScrubby(0, 100, 0.5, 1, " %");
        cropBRow->addValueWidget(m_cropBottomSpin);
        m_propLayout->addWidget(cropBRow);

        // â”€â”€ Opacity section â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
        m_opacitySection = makeSectionHeader("Opacity");
        // Add mask shape buttons inline with the Opacity header
        {
            auto* headerLayout = qobject_cast<QHBoxLayout*>(m_opacitySection->layout());
            if (headerLayout) {
                // Remove the stretch spacer (second-to-last item; the last item is the reset button).
                // We'll re-insert the stretch between the mask buttons and the reset button.
                auto* stretchItem = headerLayout->takeAt(headerLayout->count() - 2);

                auto makeMaskBtn = [&](const QString& text, const QString& tip,
                                       uint8_t shapeType) -> QToolButton* {
                    auto* btn = new QToolButton(m_opacitySection);
                    btn->setText(text);
                    btn->setToolTip(tip);
                    btn->setFixedSize(22, 20);
                    btn->setStyleSheet(QStringLiteral(
                        "QToolButton { color: %1; font-size: 12px; background: transparent; border: 1px solid %2; border-radius: 2px; padding: 0; }"
                        "QToolButton:hover { background: %3; border-color: %4; }")
                        .arg(Theme::hex(tc.textSecondary), Theme::hex(tc.border),
                             Theme::hex(tc.surface3), Theme::hex(tc.accent)));
                    connect(btn, &QToolButton::clicked, this, [this, shapeType]() {
                        addMask(shapeType);
                    });
                    return btn;
                };

                // Insert mask buttons before the reset button (which is now the last item)
                headerLayout->insertWidget(headerLayout->count() - 1,
                    makeMaskBtn(QStringLiteral("\u25CB"), tr("Create Ellipse Mask"), 0));
                headerLayout->insertWidget(headerLayout->count() - 1,
                    makeMaskBtn(QStringLiteral("\u25A1"), tr("Create Rectangle Mask"), 1));
                headerLayout->insertWidget(headerLayout->count() - 1,
                    makeMaskBtn(QStringLiteral("\u270E"), tr("Create Free Draw Bezier Mask"), 2));
                // Re-insert stretch before the reset button
                headerLayout->insertItem(headerLayout->count() - 1, stretchItem);
            }
        }
        m_propLayout->addWidget(m_opacitySection);

        m_opacityRow = makeRow("Opacity", effOpacity());
        m_opacitySpin = createScrubby(0, 100, 0.1, 1, " %");
        m_opacityRow->addValueWidget(m_opacitySpin);
        m_propLayout->addWidget(m_opacityRow);

        m_blendModeCombo = new QComboBox(m_propContainer);
        m_blendModeCombo->setFixedHeight(22);
        m_blendModeCombo->addItems({"Normal", "Multiply", "Screen", "Add",
                                    "Overlay", "Soft Light", "Hard Light",
                                    "Difference", "Color Dodge", "Color Burn"});
        auto* blendWidget = new QWidget(m_propContainer);
        blendWidget->setFixedHeight(28);
        auto* blendLayout = new QHBoxLayout(blendWidget);
        blendLayout->setContentsMargins(36, 2, 6, 2);
        blendLayout->setSpacing(6);
        auto* blendLabel = new QLabel("Blend Mode", blendWidget);
        blendLabel->setStyleSheet(QStringLiteral("color: %1; font-size: 12px; background: transparent;")
            .arg(Theme::hex(tc.textPrimary)));
        blendLayout->addWidget(blendLabel);
        blendLayout->addWidget(m_blendModeCombo, 1);
        m_propLayout->addWidget(blendWidget);

        // Set current blend mode from clip
        m_blendModeCombo->setCurrentIndex(m_clip->blendMode());

        // Wire combo to clip's blendMode
        connect(m_blendModeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
                this, [this](int index) {
            if (m_clip) {
                m_clip->setBlendMode(index);
                emit propertyChanged();
            }
        });

        // â”€â”€ Mask sub-sections (below blend mode, still in Opacity section) â”€â”€
        buildMaskUI(rowIdx);
    }

    // â•”â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
    // â•‘  AUDIO PROPERTIES â€” Volume, Pan
    // â•šâ•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•

    if (isAudio) {
        auto* audioClip = dynamic_cast<AudioClip*>(m_clip);

        auto* audioLabel = new QLabel("Audio", m_propContainer);
        audioLabel->setFixedHeight(24);
        audioLabel->setStyleSheet(QStringLiteral(
            "background: %1; color: %2; font-size: 11px; padding-left: 8px; "
            "border-bottom: 1px solid %3;")
            .arg(Theme::hex(tc.surface3), Theme::hex(tc.textSecondary),
                 Theme::hex(tc.border)));
        m_propLayout->addWidget(audioLabel);

        if (audioClip) {
            // â”€â”€ Volume â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
            auto* volRow = makeRow("Volume", &audioClip->volume());
            // Volume displayed in dB (range -60..+12), stored as linear gain.
            m_audioVolumeSpin = createScrubby(-60.0, 12.0, 0.1, 1, " dB");
            volRow->addValueWidget(m_audioVolumeSpin);
            m_propLayout->addWidget(volRow);

            // â”€â”€ Pan â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
            auto* panRow = makeRow("Pan", &audioClip->pan());
            m_panSpin = createScrubby(-100, 100, 1.0, 1);
            panRow->addValueWidget(m_panSpin);
            m_propLayout->addWidget(panRow);
        }
    }

    // â”€â”€ Time Remapping section â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    m_timeRemapSection = makeSectionHeader("Time Remapping");
    m_propLayout->addWidget(m_timeRemapSection);

    auto* speedRow = makeRow("Speed", &m_clip->speedRamp());
    m_speedSpin = createScrubby(1, 10000, 1.0, 1, " %");
    speedRow->addValueWidget(m_speedSpin);
    m_propLayout->addWidget(speedRow);

    // â”€â”€ Applied Effects section â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    if (m_clip->effects().effectCount() > 0) {
        for (size_t i = 0; i < m_clip->effects().effectCount(); ++i) {
            auto& fx = m_clip->effects().effect(i);

            // Build a custom header with delete button and selection support
            auto* fxHeader = new QWidget(m_propContainer);
            fxHeader->setFixedHeight(28);
            fxHeader->setCursor(Qt::PointingHandCursor);
            fxHeader->setStyleSheet(QStringLiteral(
                "background: %1; border-bottom: 1px solid %2;")
                .arg(Theme::hex(tc.surface2), Theme::hex(tc.border)));
            fxHeader->setContextMenuPolicy(Qt::CustomContextMenu);

            auto* hl = new QHBoxLayout(fxHeader);
            hl->setContentsMargins(6, 0, 6, 0);
            hl->setSpacing(6);

            auto* arrow = new QToolButton(fxHeader);
            arrow->setText(QStringLiteral("\u25BC"));
            arrow->setFixedSize(16, 20);
            arrow->setStyleSheet(QStringLiteral(
                "QToolButton { color: %1; font-size: 11px; background: transparent; border: none; padding: 0; }"
                "QToolButton:hover { color: %2; }")
                .arg(Theme::hex(tc.textTertiary), Theme::hex(tc.textPrimary)));
            hl->addWidget(arrow);

            auto* fxLabel = new QToolButton(fxHeader);
            fxLabel->setText(QStringLiteral("fx"));
            fxLabel->setCheckable(true);
            fxLabel->setChecked(fx.isEnabled());
            fxLabel->setFixedSize(18, 18);
            fxLabel->setStyleSheet(QStringLiteral(
                "QToolButton { color: %1; font-size: 11px; font-weight: bold; font-style: italic; "
                "background: transparent; border: none; padding: 0; }"
                "QToolButton:!checked { color: %2; }")
                .arg(Theme::hex(tc.accent), Theme::hex(tc.textTertiary)));
            hl->addWidget(fxLabel);

            auto* titleLabel = new QLabel(QString::fromUtf8(fx.name()), fxHeader);
            titleLabel->setStyleSheet(QStringLiteral(
                "color: %1; font-size: 12px; font-weight: bold; background: transparent;")
                .arg(Theme::hex(tc.textPrimary)));
            hl->addWidget(titleLabel);
            hl->addStretch();

            // Wire fx toggle button
            uint64_t fxId = fx.id();
            connect(fxLabel, &QToolButton::toggled, this, [this, fxId, titleLabel, tc](bool checked) {
                if (!m_clip || !m_commandStack) return;
                m_commandStack->execute(
                    std::make_unique<SetEffectEnabledCommand>(&m_clip->effects(), fxId, checked));
                // Dim the title when disabled
                titleLabel->setStyleSheet(QStringLiteral(
                    "color: %1; font-size: 12px; font-weight: bold; background: transparent;")
                    .arg(Theme::hex(checked ? tc.textPrimary : tc.textTertiary)));
                emit propertyChanged();
            });

            // Delete button
            auto* deleteBtn = new QToolButton(fxHeader);
            deleteBtn->setText(QStringLiteral("\u2715")); // âœ•
            deleteBtn->setFixedSize(20, 20);
            deleteBtn->setStyleSheet(QStringLiteral(
                "QToolButton { color: %1; font-size: 12px; background: transparent; border: none; padding: 0; }"
                "QToolButton:hover { color: %2; }")
                .arg(Theme::hex(tc.textTertiary), Theme::hex(tc.error)));
            hl->addWidget(deleteBtn);

            // "Save as Default" button -- only for OTS effects (per-side JSON).
            if (fx.effectType() == EffectType::OtsLeft ||
                fx.effectType() == EffectType::OtsRight)
            {
                auto* saveDefaultBtn = new QToolButton(fxHeader);
                saveDefaultBtn->setText(QStringLiteral("\u2B07"));
                saveDefaultBtn->setFixedSize(20, 20);
                saveDefaultBtn->setToolTip(QStringLiteral(
                    "Save current %1 settings as the default")
                    .arg(QString::fromUtf8(fx.name())));
                saveDefaultBtn->setStyleSheet(QStringLiteral(
                    "QToolButton { color: %1; font-size: 12px; background: transparent; border: none; padding: 0; }"
                    "QToolButton:hover { color: %2; }")
                    .arg(Theme::hex(tc.textTertiary), Theme::hex(tc.accent)));
                // Insert before the delete button so layout reads:
                //   [arrow] [fx] [title] ...stretch... [save] [delete]
                hl->insertWidget(hl->count() - 1, saveDefaultBtn);

                Effect* fxPtr = &fx;
                connect(saveDefaultBtn, &QToolButton::clicked, this,
                        [this, fxPtr]() {
                    if (auto* ots = dynamic_cast<Ots*>(fxPtr)) {
                        ots->saveAsDefault();
                    }
                });
            }

            m_sectionArrows.push_back({fxHeader, arrow, {}});
            m_effectHeaders.push_back(fxHeader);
            m_propLayout->addWidget(fxHeader);

            // Wire delete button
            size_t effectIdx = i;
            connect(deleteBtn, &QToolButton::clicked, this, [this, effectIdx]() {
                deleteEffect(effectIdx);
            });

            // Wire header click for selection
            connect(fxHeader, &QWidget::customContextMenuRequested,
                    this, [this, effectIdx, fxHeader](const QPoint& pos) {
                m_selectedEffectIndex = static_cast<int>(effectIdx);
                QMenu menu(fxHeader);
                menu.addAction("Remove Effect", this, [this, effectIdx]() {
                    deleteEffect(effectIdx);
                });
                menu.exec(fxHeader->mapToGlobal(pos));
            });

            // Left-click on header selects the effect
            fxHeader->installEventFilter(this);

            // â”€â”€ Ultra Key gets grouped sub-sections â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
            if (fx.effectType() == EffectType::ChromaKey) {
                buildUltraKeyUI(fx, effectIdx, rowIdx);
            } else if (fx.effectType() == EffectType::LUT) {
                buildLUTUI(fx, effectIdx, rowIdx);
            } else if (fx.effectType() == EffectType::Letterbox) {
                buildLetterboxUI(fx, effectIdx, rowIdx);
            } else if (fx.effectType() == EffectType::FlipHorizontal ||
                       fx.effectType() == EffectType::FlipVertical) {
                // Flip has no user-editable parameters (the axis is fixed
                // by the effect type) — like Premiere Pro's Flip effects.
                // The hidden "Axis" param still rides along to the shader
                // via evalAllParams(); we just don't render a row for it.
            } else {
                // Generic effect: flat parameter rows
                buildGenericEffectUI(fx, effectIdx, rowIdx);
            }
        }
    }

    // Stretch at bottom
    m_propLayout->addStretch();

    // Default collapsed sections: only Motion and Opacity start expanded
    if (m_sectionCollapsed.find(QStringLiteral("Crop")) == m_sectionCollapsed.end())
        m_sectionCollapsed[QStringLiteral("Crop")] = true;
    if (m_sectionCollapsed.find(QStringLiteral("Time Remapping")) == m_sectionCollapsed.end())
        m_sectionCollapsed[QStringLiteral("Time Remapping")] = true;

    // â”€â”€ Wire up collapsible section arrows â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    // Walk the layout to find which children belong to each section header.
    // Each section runs from the header to just before the next header or end.
    {
        // Build index of header positions
        std::vector<int> headerIndices;
        for (int li = 0; li < m_propLayout->count(); ++li) {
            auto* w = m_propLayout->itemAt(li)->widget();
            if (!w) continue;
            for (auto& sec : m_sectionArrows) {
                if (sec.header == w) {
                    headerIndices.push_back(li);
                    break;
                }
            }
        }

        // For each section, collect widgets from (headerIndex+1) up to next header
        for (size_t si = 0; si < m_sectionArrows.size(); ++si) {
            int startIdx = (si < headerIndices.size()) ? headerIndices[si] + 1 : 0;
            int endIdx = (si + 1 < headerIndices.size())
                             ? headerIndices[si + 1]
                             : m_propLayout->count();

            // But stop if we hit a non-section header widget that belongs to a different section group
            // (e.g. the "Video" label). Just collect until the next section header.
            for (int li = startIdx; li < endIdx; ++li) {
                auto* w = m_propLayout->itemAt(li)->widget();
                if (!w) continue;
                // Skip if it's another section header (shouldn't happen due to endIdx, but safety)
                bool isHeader = false;
                for (auto& other : m_sectionArrows) {
                    if (other.header == w) { isHeader = true; break; }
                }
                if (!isHeader) {
                    m_sectionArrows[si].children.push_back(w);
                }
            }

            // Connect arrow click to toggle visibility
            auto& sec = m_sectionArrows[si];

            // Restore saved collapse state, or fall back to arrow default
            bool shouldCollapse;
            auto it = m_sectionCollapsed.find(sec.title);
            if (it != m_sectionCollapsed.end())
                shouldCollapse = it->second;
            else
                shouldCollapse = (sec.arrow->text() == QStringLiteral("\u25B6"));

            // Scale Width hides while Uniform Scale is checked. Section
            // expansion would otherwise blanket-show every child and
            // override that hide.
            auto uniformHidesScaleW = [this](QWidget* child) {
                return child == m_scaleWRow && m_uniformScaleCheck
                       && m_uniformScaleCheck->isChecked();
            };

            if (shouldCollapse) {
                sec.arrow->setText(QStringLiteral("\u25B6"));
                for (auto* child : sec.children)
                    child->setVisible(false);
            } else {
                sec.arrow->setText(QStringLiteral("\u25BC"));
                for (auto* child : sec.children)
                    child->setVisible(!uniformHidesScaleW(child));
            }

            connect(sec.arrow, &QToolButton::clicked, this, [this, &sec]() {
                bool collapsed = !sec.children.empty() && sec.children[0]->isVisible();
                for (auto* child : sec.children) {
                    bool show = !collapsed;
                    if (show && child == m_scaleWRow && m_uniformScaleCheck
                            && m_uniformScaleCheck->isChecked()) {
                        show = false;
                    }
                    child->setVisible(show);
                }
                sec.arrow->setText(collapsed ? QStringLiteral("\u25B6")   // â–¶ collapsed
                                             : QStringLiteral("\u25BC")); // â–¼ expanded
            });

            // Wire reset button to reset all scrubby spinboxes in this section
            if (sec.resetBtn) {
                connect(sec.resetBtn, &QToolButton::clicked, this, [this, &sec]() {
                    // Opacity resets to 100% (fully opaque), not 0
                    if (sec.title == QStringLiteral("Opacity")) {
                        if (m_opacitySpin) {
                            m_opacitySpin->setValue(100.0);
                        }
                    } else {
                        for (auto* child : sec.children) {
                            auto* row = qobject_cast<PropertyRow*>(child);
                            if (!row) continue;
                            // Find all ScrubbySpinBox children and reset to default
                            auto spins = row->findChildren<ScrubbySpinBox*>();
                            for (auto* spin : spins) {
                                spin->setValue(spin->minimum() == 0.0 ? 0.0 : spin->value());
                            }
                        }
                    }
                    // Apply as a transform change
                    applyTransformLive();
                    emit propertyChanged();
                });
            }
        }
    }

    // Connect transform spins â€” live preview during scrub, undo on commit
    auto connectTransform = [this](ScrubbySpinBox* spin) {
        if (!spin) return;
        connect(spin, &ScrubbySpinBox::valueScrubbed,
                this, [this](double) { applyTransformLive(); });
        connect(spin, &ScrubbySpinBox::valueCommitted,
                this, &EffectControlsPanel::commitTransform);
        // Note: editingFinished is NOT connected here. Both scrub and
        // typed entry go through valueScrubbed â†’ applyTransformLive
        // then valueCommitted â†’ commitTransform for undo.
    };
    connectTransform(m_posXSpin);
    connectTransform(m_posYSpin);
    connectTransform(m_scaleSpin);
    connectTransform(m_scaleWSpin);
    connectTransform(m_rotationSpin);
    connectTransform(m_opacitySpin);
    connectTransform(m_cropLeftSpin);
    connectTransform(m_cropTopSpin);
    connectTransform(m_cropRightSpin);
    connectTransform(m_cropBottomSpin);
    connectTransform(m_anchorXSpin);
    connectTransform(m_anchorYSpin);
    connectTransform(m_antiFlickerSpin);
    connectTransform(m_speedSpin);
    connectTransform(m_panSpin);
    connectTransform(m_audioVolumeSpin);
}


void EffectControlsPanel::populateFromClip()
{
    if (!m_clip) return;
    m_updating = true;

    int64_t t = clipRelativeTick();

    // Position — clip-level is stored REF-1920 and shown ×seqW/1920;
    // graphic-layer is stored project-px (already seq-px) and shown as-is.
    // posDisplayFactor{X,Y}() returns the right multiplier for each source.
    const double posFx = posDisplayFactorX();
    const double posFy = posDisplayFactorY();
    if (auto* trk = effPosX();    m_posXSpin && trk) m_posXSpin->setValue(trk->evaluate(t) * posFx);
    if (auto* trk = effPosY();    m_posYSpin && trk) m_posYSpin->setValue(trk->evaluate(t) * posFy);

    // Scale — Premiere-style native-pixel percentage.  Stored values are
    // cover-fit multipliers (1.0 = fill frame), but we want the displayed
    // number to read 100% only when the source is rendered 1:1 (sharp).
    // Multiply by coverFit so: 1080p source in 1080p seq → 100% (native),
    // 4K source in 1080p → 50% (downscaled), 500-px source → 384% (upscaled
    // and the user can see at a glance that it'll be soft).  For clip kinds
    // without a meaningful native-pixel size (characters / spine / title /
    // graphic) coverFitForCurrentClip() returns 1.0 — unchanged display.
    const double sf = coverFitForCurrentClip();
    if (auto* trk = effScaleX();  m_scaleSpin  && trk) m_scaleSpin->setValue(trk->evaluate(t) * sf * 100.0);
    if (auto* trk = effScaleY();  m_scaleWSpin && trk) m_scaleWSpin->setValue(trk->evaluate(t) * sf * 100.0);

    // Rotation (degrees)
    if (auto* trk = effRotation(); m_rotationSpin && trk) m_rotationSpin->setValue(trk->evaluate(t));

    // Opacity (percentage)
    if (auto* trk = effOpacity();  m_opacitySpin  && trk) m_opacitySpin->setValue(trk->evaluate(t) * 100.0);

    // Anchor point — same display convention as Position. Clip-level
    // anchor is REF-1920 so multiply by seqW/1920; graphic-layer anchor
    // is project-px (already seq-px) so factor is 1.0. posDisplayFactor*
    // returns the right multiplier for whichever source is active.
    if (auto* trk = effAnchorX(); m_anchorXSpin && trk) m_anchorXSpin->setValue(trk->evaluate(t) * posFx);
    if (auto* trk = effAnchorY(); m_anchorYSpin && trk) m_anchorYSpin->setValue(trk->evaluate(t) * posFy);

    // Crop (percentage 0–100) — stored on the clip, not a keyframe track.
    // Only VideoClip / SpineClip carry crop; other clip types leave the
    // spins at 0 (the compositor ignores crop for them anyway).
    if (m_cropLeftSpin && m_cropRightSpin && m_cropTopSpin && m_cropBottomSpin) {
        float cl = 0, cr = 0, ct = 0, cb = 0;
        if (auto* vc = dynamic_cast<VideoClip*>(m_clip)) {
            cl = vc->cropLeft(); cr = vc->cropRight();
            ct = vc->cropTop();  cb = vc->cropBottom();
        } else if (auto* sc = dynamic_cast<SpineClip*>(m_clip)) {
            cl = sc->cropLeft(); cr = sc->cropRight();
            ct = sc->cropTop();  cb = sc->cropBottom();
        }
        m_cropLeftSpin->setValue(static_cast<double>(cl));
        m_cropRightSpin->setValue(static_cast<double>(cr));
        m_cropTopSpin->setValue(static_cast<double>(ct));
        m_cropBottomSpin->setValue(static_cast<double>(cb));
    }

    // Speed (percentage)
    if (m_speedSpin) m_speedSpin->setValue(m_clip->speed() * 100.0);

    // Audio â€” Pan and Volume (via AudioClip)
    if (m_panSpin || m_audioVolumeSpin) {
        auto* audioClip = dynamic_cast<AudioClip*>(m_clip);
        if (audioClip) {
            if (m_audioVolumeSpin) m_audioVolumeSpin->setValue(gainToDb(audioClip->volume().evaluate(t)));
            if (m_panSpin)        m_panSpin->setValue(audioClip->pan().evaluate(t) * 100.0);
        }
    }

    m_updating = false;
}

// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
//  wireEffectParam â€” connect a ScrubbySpinBox to an effect parameter

} // namespace rt
