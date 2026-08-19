/*
 * EffectControlsPanelEffects.cpp - Effect parameter wiring, UltraKey UI, and mask UI.
 * Split from EffectControlsPanelTree.cpp.
 */

#include "panels/effects/EffectControlsPanel.h"
#include "panels/effects/MaskTracker.h"
#include "widgets/ScrubbySpinBox.h"
#include "Theme.h"
#include "PathUtils.h"

#include "timeline/Clip.h"
#include "timeline/AudioClip.h"
#include "timeline/VideoClip.h"
#include "timeline/KeyframeTrack.h"
#include "timeline/OpacityMask.h"
#include "timeline/Timeline.h"
#include "timeline/Track.h"
#include "command/CommandStack.h"
#include "command/LambdaCommand.h"
#include "command/commands/EffectCommands.h"
#include "command/commands/KeyframeCmds.h"
#include "effects/Effect.h"
#include "effects/EffectStack.h"
#include "effects/ChromaKey.h"
#include "effects/LUT.h"
#include "effects/Letterbox.h"
#include "effects/Tint.h"
#include "effects/BeatEffects.h"
#include "audio/AudioFile.h"
#include "audio/BeatDetector.h"
#include "Constants.h"

#include <QFrame>
#include <QGridLayout>
#include <QMenu>
#include <QColorDialog>
#include <QCheckBox>
#include <QComboBox>
#include <QPushButton>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QLabel>
#include <QFileInfo>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>

namespace rt {

// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•

void EffectControlsPanel::wireEffectParam(ScrubbySpinBox* spin,
                                          size_t effectIdx, size_t paramIdx)
{
    // Live preview during scrub
    connect(spin, &ScrubbySpinBox::valueScrubbed,
            this, [this, effectIdx, paramIdx](double val) {
        if (!m_clip || m_updating) return;
        auto& st = m_clip->effects();
        if (effectIdx >= st.effectCount()) return;
        auto& ef = st.effect(effectIdx);
        if (paramIdx >= ef.paramCount()) return;
        ef.param(paramIdx).track.writeValue(clipRelativeTick(), static_cast<float>(val));
        emit propertyChanged();
    });

    // Commit with undo support
    connect(spin, &ScrubbySpinBox::valueCommitted,
            this, [this, effectIdx, paramIdx](double oldVal, double newVal) {
        if (!m_clip || m_updating) return;
        auto& st = m_clip->effects();
        if (effectIdx >= st.effectCount()) return;
        auto& ef = st.effect(effectIdx);
        if (paramIdx >= ef.paramCount()) return;
        auto& trk = ef.param(paramIdx).track;
        int64_t t = clipRelativeTick();
        auto fOld = static_cast<float>(oldVal);
        auto fNew = static_cast<float>(newVal);

        bool createdKF = false;
        if (!trk.isStatic() && trk.keyframeCount() >= 2 && trk.hasKeyframeAt(t)) {
            KeyframeTrack<float> tmp(trk.defaultValue());
            for (const auto& kf : trk.keyframes()) {
                if (kf.time != t) tmp.restoreKeyframe(kf);
            }
            createdKF = (std::abs(tmp.evaluate(t) - fOld) < 0.01f);
        }

        emit propertyChanged();
        if (m_commandStack) {
            auto* stack = &st;
            auto fxId = ef.id();
            auto pi = paramIdx;
            m_commandStack->pushWithoutExecute(
                std::make_unique<LambdaCommand>(
                    "Set Effect Parameter",
                    [stack, fxId, pi, fNew, t]() {
                        if (auto* fx2 = stack->effectById(fxId))
                            if (pi < fx2->paramCount())
                                fx2->param(pi).track.writeValue(t, fNew);
                    },
                    [stack, fxId, pi, fOld, t, createdKF]() {
                        if (auto* fx2 = stack->effectById(fxId))
                            if (pi < fx2->paramCount()) {
                                if (createdKF)
                                    fx2->param(pi).track.removeKeyframeAtTime(t);
                                else
                                    fx2->param(pi).track.writeValue(t, fOld);
                            }
                    }));
        }
    });
}

// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
//  buildGenericEffectUI â€” flat parameter rows for non-Ultra Key effects
// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•

void EffectControlsPanel::buildGenericEffectUI(Effect& fx, size_t effectIdx,
                                               int& rowIdx)
{
    // Row builder helper (same as the local lambda in buildPropertyTree)
    auto makeRow = [&](const QString& name,
                       KeyframeTrack<float>* track) -> PropertyRow* {
        auto* row = new PropertyRow(name, track, m_propContainer);
        row->setRowIndex(rowIdx++);
        row->setTimeProvider([this]() { return clipRelativeTick(); });
        registerPropertyRow(row);
        connect(row, &PropertyRow::addKeyframeRequested,
                this, &EffectControlsPanel::onAddKeyframe);
        connect(row, &PropertyRow::deleteKeyframeRequested,
                this, &EffectControlsPanel::onDeleteKeyframe);
        connect(row, &PropertyRow::goToPrevKeyframe,
                this, &EffectControlsPanel::onGoToPrevKeyframe);
        connect(row, &PropertyRow::goToNextKeyframe,
                this, &EffectControlsPanel::onGoToNextKeyframe);
        return row;
    };

    for (size_t p = 0; p < fx.paramCount(); ++p) {
        auto& param = fx.param(p);
        auto* fxRow = makeRow(QString::fromStdString(param.name), &param.track);
        auto* fxSpin = createScrubby(param.minVal, param.maxVal, 0.01, 2);
        fxSpin->setValue(static_cast<double>(param.track.evaluate(clipRelativeTick())));
        fxRow->addValueWidget(fxSpin);
        m_propLayout->addWidget(fxRow);
        wireEffectParam(fxSpin, effectIdx, p);
    }
}

// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
//  buildUltraKeyUI â€” grouped sections for Ultra Key effect
// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•

void EffectControlsPanel::buildUltraKeyUI(Effect& fx, size_t effectIdx,
                                          int& rowIdx)
{
    const auto& tc = Theme::colors();

    // Row builder (identical to the one above but captured for this scope)
    auto makeRow = [&](const QString& name,
                       KeyframeTrack<float>* track) -> PropertyRow* {
        auto* row = new PropertyRow(name, track, m_propContainer);
        row->setRowIndex(rowIdx++);
        row->setTimeProvider([this]() { return clipRelativeTick(); });
        registerPropertyRow(row);
        connect(row, &PropertyRow::addKeyframeRequested,
                this, &EffectControlsPanel::onAddKeyframe);
        connect(row, &PropertyRow::deleteKeyframeRequested,
                this, &EffectControlsPanel::onDeleteKeyframe);
        connect(row, &PropertyRow::goToPrevKeyframe,
                this, &EffectControlsPanel::onGoToPrevKeyframe);
        connect(row, &PropertyRow::goToNextKeyframe,
                this, &EffectControlsPanel::onGoToNextKeyframe);
        return row;
    };

    // Sub-section header builder (Premiere-style collapsible group)
    auto makeSubHeader = [&](const QString& title) -> QWidget* {
        auto* header = new QWidget(m_propContainer);
        header->setFixedHeight(26);
        header->setCursor(Qt::PointingHandCursor);
        header->setStyleSheet(QStringLiteral(
            "background: %1; border-top: 1px solid %2; border-bottom: 1px solid %2;")
            .arg(Theme::hex(tc.surface2), Theme::hex(tc.border)));
        auto* hl = new QHBoxLayout(header);
        hl->setContentsMargins(24, 0, 6, 0);
        hl->setSpacing(6);

        auto* arrow = new QToolButton(header);
        arrow->setText(QStringLiteral("\u25B6"));  // â–¶ collapsed by default
        arrow->setFixedSize(16, 20);
        arrow->setStyleSheet(QStringLiteral(
            "QToolButton { color: %1; font-size: %3px; background: transparent; border: none; padding: 0; }"
            "QToolButton:hover { color: %2; }")
            .arg(Theme::hex(tc.textSecondary), Theme::hex(tc.textPrimary))
            .arg(Theme::typography().sizeXxs));
        hl->addWidget(arrow);

        auto* lbl = new QLabel(title, header);
        lbl->setStyleSheet(QStringLiteral(
            "color: %1; font-size: %2px; font-weight: bold; background: transparent;")
            .arg(Theme::hex(tc.textPrimary))
            .arg(Theme::typography().sizeXs));
        hl->addWidget(lbl);
        hl->addStretch();

        m_sectionArrows.push_back({header, arrow, {}, title});
        return header;
    };

    // Helper: add a param row and wire it
    auto addParamRow = [&](size_t paramIdx, double step = 1.0, int dec = 1,
                           const QString& suffix = {}) {
        auto& param = fx.param(paramIdx);
        auto* row = makeRow(QString::fromStdString(param.name), &param.track);
        auto* spin = createScrubby(param.minVal, param.maxVal, step, dec, suffix);
        spin->setValue(static_cast<double>(param.track.evaluate(clipRelativeTick())));
        row->addValueWidget(spin);
        m_propLayout->addWidget(row);
        wireEffectParam(spin, effectIdx, paramIdx);
    };

    // â”€â”€ Key Color row with eyedropper button â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    {
        auto* keyColorWidget = new QWidget(m_propContainer);
        keyColorWidget->setFixedHeight(32);
        auto* kcLayout = new QHBoxLayout(keyColorWidget);
        kcLayout->setContentsMargins(20, 2, 6, 2);
        kcLayout->setSpacing(6);

        auto* lbl = new QLabel("Key Color", keyColorWidget);
        lbl->setMinimumWidth(80);
        lbl->setStyleSheet(QStringLiteral(
            "color: %1; font-size: %2px; background: transparent;")
            .arg(Theme::hex(tc.textPrimary))
            .arg(Theme::typography().sizeXs));
        kcLayout->addWidget(lbl);

        // Color swatch (shows current key color)
        auto* swatch = new QPushButton(keyColorWidget);
        swatch->setFixedSize(36, 22);
        float r = fx.param(ChromaKey::KeyColorR).track.evaluate(0);
        float g = fx.param(ChromaKey::KeyColorG).track.evaluate(0);
        float b = fx.param(ChromaKey::KeyColorB).track.evaluate(0);
        QColor keyCol = QColor::fromRgbF(r, g, b);
        swatch->setStyleSheet(QStringLiteral(
            "QPushButton { background: %1; border: 1px solid %2; border-radius: 2px; }"
            "QPushButton:hover { border: 1px solid %3; }")
            .arg(keyCol.name(), Theme::hex(tc.border), Theme::hex(tc.accent)));

        // Click swatch â†’ open color dialog
        connect(swatch, &QPushButton::clicked, this, [this, effectIdx, swatch]() {
            if (!m_clip) return;
            auto& st = m_clip->effects();
            if (effectIdx >= st.effectCount()) return;
            auto& ef = st.effect(effectIdx);
            float cr = ef.param(ChromaKey::KeyColorR).track.evaluate(clipRelativeTick());
            float cg = ef.param(ChromaKey::KeyColorG).track.evaluate(clipRelativeTick());
            float cb = ef.param(ChromaKey::KeyColorB).track.evaluate(clipRelativeTick());
            QColor current = QColor::fromRgbF(cr, cg, cb);
            QColor chosen = QColorDialog::getColor(current, this, "Select Key Color");
            if (chosen.isValid()) {
                int64_t t = clipRelativeTick();
                ef.param(ChromaKey::KeyColorR).track.writeValue(t, static_cast<float>(chosen.redF()));
                ef.param(ChromaKey::KeyColorG).track.writeValue(t, static_cast<float>(chosen.greenF()));
                ef.param(ChromaKey::KeyColorB).track.writeValue(t, static_cast<float>(chosen.blueF()));
                swatch->setStyleSheet(QStringLiteral(
                    "QPushButton { background: %1; border: 1px solid %2; border-radius: 2px; }")
                    .arg(chosen.name(), Theme::hex(Theme::colors().border)));
                emit propertyChanged();
            }
        });
        kcLayout->addWidget(swatch);

        // Eyedropper button
        auto* eyedropBtn = new QToolButton(keyColorWidget);
        eyedropBtn->setText(QStringLiteral("\U0001F4A7")); // ðŸ’§ (droplet)
        eyedropBtn->setToolTip(tr("Pick color from Program Monitor"));
        eyedropBtn->setFixedSize(24, 22);
        eyedropBtn->setStyleSheet(QStringLiteral(
            "QToolButton { background: %1; color: %2; border: 1px solid %3; border-radius: 2px; font-size: %6px; }"
            "QToolButton:hover { background: %4; border: 1px solid %5; }")
            .arg(Theme::hex(tc.surface2), Theme::hex(tc.textPrimary),
                 Theme::hex(tc.border), Theme::hex(tc.surface3),
                 Theme::hex(tc.accent))
            .arg(Theme::typography().sizeXs));
        connect(eyedropBtn, &QToolButton::clicked, this, [this, effectIdx]() {
            emit eyedropperRequested(effectIdx);
        });
        kcLayout->addWidget(eyedropBtn);

        kcLayout->addStretch();
        m_propLayout->addWidget(keyColorWidget);
    }

    // â”€â”€ Output Mode dropdown â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    {
        auto* outputWidget = new QWidget(m_propContainer);
        outputWidget->setFixedHeight(28);
        auto* ol = new QHBoxLayout(outputWidget);
        ol->setContentsMargins(20, 2, 6, 2);
        ol->setSpacing(6);

        auto* lbl = new QLabel("Output", outputWidget);
        lbl->setMinimumWidth(80);
        lbl->setStyleSheet(QStringLiteral("color: %1; font-size: %2px; background: transparent;")
            .arg(Theme::hex(tc.textPrimary))
            .arg(Theme::typography().sizeXs));
        ol->addWidget(lbl);

        auto* combo = new QComboBox(outputWidget);
        combo->addItems({"Composite", "Alpha Matte", "Color Channel",
                         "Original", "Removed Color", "Spill Map"});
        combo->setCurrentIndex(static_cast<int>(fx.param(ChromaKey::OutputMode).track.evaluate(0)));
        combo->setFixedHeight(22);
        ol->addWidget(combo, 1);

        connect(combo, QOverload<int>::of(&QComboBox::currentIndexChanged),
                this, [this, effectIdx](int idx) {
            if (!m_clip || m_updating) return;
            auto& st = m_clip->effects();
            if (effectIdx >= st.effectCount()) return;
            st.effect(effectIdx).param(ChromaKey::OutputMode).track
                .writeValue(clipRelativeTick(), static_cast<float>(idx));
            emit propertyChanged();
        });
        m_propLayout->addWidget(outputWidget);
    }

    // â”€â”€ Setting dropdown (Default / Relaxed / Aggressive / Custom) â”€â”€â”€â”€â”€â”€
    {
        auto* settingWidget = new QWidget(m_propContainer);
        settingWidget->setFixedHeight(28);
        auto* sl = new QHBoxLayout(settingWidget);
        sl->setContentsMargins(20, 2, 6, 2);
        sl->setSpacing(6);

        auto* lbl = new QLabel("Setting", settingWidget);
        lbl->setMinimumWidth(80);
        lbl->setStyleSheet(QStringLiteral("color: %1; font-size: %2px; background: transparent;")
            .arg(Theme::hex(tc.textPrimary))
            .arg(Theme::typography().sizeXs));
        sl->addWidget(lbl);

        auto* combo = new QComboBox(settingWidget);
        combo->addItems({"Default", "Relaxed", "Aggressive", "Custom"});
        combo->setCurrentIndex(static_cast<int>(fx.param(ChromaKey::Setting).track.evaluate(0)));
        combo->setFixedHeight(22);
        sl->addWidget(combo, 1);

        connect(combo, QOverload<int>::of(&QComboBox::currentIndexChanged),
                this, [this, effectIdx](int idx) {
            if (!m_clip || m_updating) return;
            auto& st = m_clip->effects();
            if (effectIdx >= st.effectCount()) return;
            auto& ef = st.effect(effectIdx);
            ef.param(ChromaKey::Setting).track
                .writeValue(clipRelativeTick(), static_cast<float>(idx));
            // Apply preset values
            if (auto* ck = dynamic_cast<ChromaKey*>(&ef))
                ck->applyPreset(idx);
            refresh();
            emit propertyChanged();
        });
        m_propLayout->addWidget(settingWidget);
    }

    // â”€â”€ Matte Generation section â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    auto* matteHeader = makeSubHeader("Matte Generation");
    m_propLayout->addWidget(matteHeader);
    addParamRow(ChromaKey::Transparency);
    addParamRow(ChromaKey::Highlight);
    addParamRow(ChromaKey::Shadow);
    addParamRow(ChromaKey::Tolerance);
    addParamRow(ChromaKey::Pedestal);

    // â”€â”€ Matte Cleanup section â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    auto* cleanupHeader = makeSubHeader("Matte Cleanup");
    m_propLayout->addWidget(cleanupHeader);
    addParamRow(ChromaKey::Choke);
    addParamRow(ChromaKey::Soften);
    addParamRow(ChromaKey::Contrast);
    addParamRow(ChromaKey::MidPoint);

    // â”€â”€ Spill Suppression section â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    auto* spillHeader = makeSubHeader("Spill Suppression");
    m_propLayout->addWidget(spillHeader);
    addParamRow(ChromaKey::Desaturate, 1.0, 1);
    addParamRow(ChromaKey::SpillRange);
    addParamRow(ChromaKey::Spill);
    addParamRow(ChromaKey::Luma);

    // â”€â”€ Color Correction section â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    auto* ccHeader = makeSubHeader("Color Correction");
    m_propLayout->addWidget(ccHeader);
    addParamRow(ChromaKey::Saturation);
    addParamRow(ChromaKey::Hue, 1.0, 1, QStringLiteral("\u00B0")); // Â° symbol
    addParamRow(ChromaKey::Luminance);
}

// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
//  addMask â€” create a new mask and rebuild UI
// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•

std::vector<OpacityMask>* EffectControlsPanel::maskListFor(quint64 effectId) const
{
    if (!m_clip) return nullptr;
    if (effectId == 0) return &m_clip->masks();
    if (Effect* fx = m_clip->effects().effectById(effectId))
        return &fx->masks();
    return nullptr;
}

bool EffectControlsPanel::deleteMask(quint64 effectId, uint64_t maskId)
{
    if (m_track && m_track->isLocked()) return false;
    auto* list = maskListFor(effectId);
    if (!list || !m_clip) return false;
    const auto current = std::find_if(
        list->begin(), list->end(),
        [maskId](const OpacityMask& value) {
            return value.maskId == maskId;
        });
    if (current == list->end()) return false;

    Clip* clip = m_clip;
    const OpacityMask savedMask = *current;
    const size_t restoreIndex = static_cast<size_t>(current - list->begin());
    auto resolveList = [clip, effectId]() -> std::vector<OpacityMask>* {
        if (effectId == 0) return &clip->masks();
        if (Effect* fx = clip->effects().effectById(effectId))
            return &fx->masks();
        return nullptr;
    };

    if (m_commandStack) {
        m_commandStack->execute(std::make_unique<LambdaCommand>(
            "Delete Mask",
            [resolveList, maskId]() {
                auto* masks = resolveList();
                if (!masks) return;
                const auto it = std::find_if(
                    masks->begin(), masks->end(),
                    [maskId](const OpacityMask& value) {
                        return value.maskId == maskId;
                    });
                if (it != masks->end()) masks->erase(it);
            },
            [resolveList, restoreIndex, maskId, savedMask]() {
                auto* masks = resolveList();
                if (!masks) return;
                const bool exists = std::any_of(
                    masks->begin(), masks->end(),
                    [maskId](const OpacityMask& value) {
                        return value.maskId == maskId;
                    });
                if (exists) return;
                const size_t pos = std::min(restoreIndex, masks->size());
                masks->insert(masks->begin() + static_cast<ptrdiff_t>(pos),
                              savedMask);
            }));
    } else {
        list->erase(current);
    }

    m_hasSelectedMask = false;
    m_selectedMaskEffectId = 0;
    m_selectedMaskId = 0;
    refresh();
    emit maskChanged();
    emit propertyChanged();
    return true;
}

void EffectControlsPanel::addMask(uint8_t shapeType, quint64 effectId)
{
    if (m_track && m_track->isLocked()) return;
    // A free-draw mask is created interactively in the Program Monitor.  Do
    // not manufacture the old four-corner placeholder: arm the dedicated Pen
    // Mask tool so the first click starts the real Bezier path instead.
    if (shapeType == static_cast<uint8_t>(MaskShape::FreeDrawBezier)) {
        emit penMaskToolRequested(effectId);
        return;
    }

    auto* maskList = maskListFor(effectId);
    if (!maskList) return;

    OpacityMask mask;
    mask.shape = static_cast<MaskShape>(shapeType);
    mask.name = "Mask " + std::to_string(maskList->size() + 1);

    // Default size: ellipse/rect centered at 50% with 50% coverage
    mask.base.centerX = 0.5f;
    mask.base.centerY = 0.5f;
    mask.base.width   = 0.5f;
    mask.base.height  = 0.5f;

    Clip* clip = m_clip;
    OpacityMask savedMask = mask;
    const uint64_t stableMaskId = savedMask.maskId;
    size_t insertIdx = maskList->size();

    if (m_commandStack) {
        m_commandStack->execute(std::make_unique<LambdaCommand>(
            "Add Mask",
            [clip, effectId, stableMaskId, savedMask]() {
                Clip* c = clip;
                std::vector<OpacityMask>* list = nullptr;
                if (effectId == 0) list = &c->masks();
                else if (Effect* fx = c->effects().effectById(effectId))
                    list = &fx->masks();
                if (list && std::none_of(
                        list->begin(), list->end(),
                        [stableMaskId](const OpacityMask& value) {
                            return value.maskId == stableMaskId;
                        }))
                    list->push_back(OpacityMask(savedMask));
            },
            [clip, effectId, stableMaskId]() {
                Clip* c = clip;
                std::vector<OpacityMask>* list = nullptr;
                if (effectId == 0) list = &c->masks();
                else if (Effect* fx = c->effects().effectById(effectId))
                    list = &fx->masks();
                if (list) {
                    const auto it = std::find_if(
                        list->begin(), list->end(),
                        [stableMaskId](const OpacityMask& value) {
                            return value.maskId == stableMaskId;
                        });
                    if (it != list->end()) list->erase(it);
                }
            }));
    } else {
        maskList->push_back(std::move(mask));
    }
    refresh();
    // Select the new mask in the Program Monitor immediately (Premiere
    // creates the mask "armed" for direct manipulation).
    emit maskSelected(static_cast<int>(insertIdx), effectId);
    emit maskChanged();
    emit propertyChanged();
}

// ═════════════════════════════════════════════════════════════════════════════
//  wireMaskParam — live preview + stopwatch-aware undo for a mask scalar
// ═════════════════════════════════════════════════════════════════════════════

namespace {
KeyframeTrack<float>* maskScalarTrack(OpacityMask& m,
                                      EffectControlsPanel::MaskParam which)
{
    switch (which) {
    case EffectControlsPanel::MaskParam::Feather:   return &m.feather;
    case EffectControlsPanel::MaskParam::Opacity:   return &m.maskOpacity;
    case EffectControlsPanel::MaskParam::Expansion: return &m.expansion;
    }
    return nullptr;
}
} // namespace

void EffectControlsPanel::wireMaskParam(ScrubbySpinBox* spin, quint64 effectId,
                                        size_t maskIdx, MaskParam which,
                                        float scale)
{
    auto* initialList = maskListFor(effectId);
    if (!initialList || maskIdx >= initialList->size()) return;
    const uint64_t stableMaskId = (*initialList)[maskIdx].maskId;
    auto findMask = [stableMaskId](std::vector<OpacityMask>* list)
            -> OpacityMask* {
        if (!list) return nullptr;
        const auto it = std::find_if(list->begin(), list->end(),
            [stableMaskId](const OpacityMask& mask) {
                return mask.maskId == stableMaskId;
            });
        return it == list->end() ? nullptr : &*it;
    };
    // Live preview during scrub
    connect(spin, &ScrubbySpinBox::valueScrubbed,
            this, [this, effectId, findMask, which, scale](double val) {
        if (m_updating) return;
        auto* mask = findMask(maskListFor(effectId));
        if (!mask) return;
        auto* trk = maskScalarTrack(*mask, which);
        if (!trk) return;
        trk->writeValue(clipRelativeTick(), static_cast<float>(val) * scale);
        emit maskChanged();
        emit propertyChanged();
    });

    // Commit with undo support (same stopwatch semantics as effect params)
    connect(spin, &ScrubbySpinBox::valueCommitted,
            this, [this, effectId, stableMaskId, findMask, which, scale](
                      double oldVal, double newVal) {
        if (m_updating) return;
        auto* mask = findMask(maskListFor(effectId));
        if (!mask) return;
        auto* trk = maskScalarTrack(*mask, which);
        if (!trk) return;
        int64_t t = clipRelativeTick();
        auto fOld = static_cast<float>(oldVal) * scale;
        auto fNew = static_cast<float>(newVal) * scale;

        bool createdKF = false;
        if (!trk->isStatic() && trk->keyframeCount() >= 2 && trk->hasKeyframeAt(t)) {
            KeyframeTrack<float> tmp(trk->defaultValue());
            for (const auto& kf : trk->keyframes())
                if (kf.time != t) tmp.restoreKeyframe(kf);
            createdKF = (std::abs(tmp.evaluate(t) - fOld) < 0.01f);
        }

        emit maskChanged();
        emit propertyChanged();
        if (!m_commandStack) return;
        Clip* clip = m_clip;
        auto resolve = [clip, effectId, stableMaskId, which]()
                -> KeyframeTrack<float>* {
            std::vector<OpacityMask>* list = nullptr;
            if (effectId == 0) list = &clip->masks();
            else if (Effect* fx = clip->effects().effectById(effectId))
                list = &fx->masks();
            if (!list) return nullptr;
            const auto it = std::find_if(list->begin(), list->end(),
                [stableMaskId](const OpacityMask& value) {
                    return value.maskId == stableMaskId;
                });
            return it == list->end() ? nullptr : maskScalarTrack(*it, which);
        };
        m_commandStack->pushWithoutExecute(
            std::make_unique<LambdaCommand>(
                "Set Mask Parameter",
                [resolve, fNew, t]() {
                    if (auto* trk2 = resolve()) trk2->writeValue(t, fNew);
                },
                [resolve, fOld, t, createdKF]() {
                    if (auto* trk2 = resolve()) {
                        if (createdKF) trk2->removeKeyframeAtTime(t);
                        else trk2->writeValue(t, fOld);
                    }
                }));
    });
}

// ═════════════════════════════════════════════════════════════════════════════
//  trackMask — Premiere "track mask forward/backward"
// ═════════════════════════════════════════════════════════════════════════════

void EffectControlsPanel::trackMask(quint64 effectId, size_t maskIdx,
                                    bool forward)
{
    auto* list = maskListFor(effectId);
    if (!list || maskIdx >= list->size() || !m_clip) return;

    auto* vc = dynamic_cast<VideoClip*>(m_clip);
    if (!vc || vc->mediaPath().empty()) {
        spdlog::info("[MASK-TRACK] tracking requires a video clip with media");
        return;
    }

    MaskTrackParams p;
    p.mediaPath         = utf8ToPath(vc->mediaPath());
    p.sourceInTicks     = m_clip->sourceIn();
    p.speed             = m_clip->speed();
    p.clipDurationTicks = m_clip->duration();
    p.startLocalTick    = clipRelativeTick();
    p.forward           = forward;

    // Sequence resolution + the clip's transform at the start time — same
    // unit conversions the layer builder applies (REF-1920 position/anchor
    // scaled to output pixels).
    uint32_t outW = 1920, outH = 1080;
    if (m_timeline) {
        const auto& res = m_timeline->settings().resolution();
        if (res.width > 0 && res.height > 0) {
            outW = res.width;
            outH = res.height;
        }
    }
    const float scaleToOutX = static_cast<float>(outW) / 1920.0f;
    const float scaleToOutY = static_cast<float>(outH) / 1080.0f;
    const int64_t t0 = p.startLocalTick;
    p.outW     = outW;
    p.outH     = outH;
    p.posX     = m_clip->positionX().evaluate(t0) * scaleToOutX;
    p.posY     = m_clip->positionY().evaluate(t0) * scaleToOutY;
    p.scaleX   = m_clip->scaleX().evaluate(t0);
    p.scaleY   = m_clip->scaleY().evaluate(t0);
    p.rotation = m_clip->rotation().evaluate(t0);
    p.anchorX  = m_clip->anchorX().evaluate(t0) * scaleToOutX;
    p.anchorY  = m_clip->anchorY().evaluate(t0) * scaleToOutY;

    OpacityMask before = (*list)[maskIdx];
    OpacityMask working = before;
    const uint64_t stableMaskId = before.maskId;

    const int keys = MaskTracker::track(p, working, this);
    if (keys <= 1) {
        spdlog::info("[MASK-TRACK] no keyframes written (keys={})", keys);
        return;
    }

    (*list)[maskIdx] = working;
    OpacityMask after = working;

    if (m_commandStack) {
        Clip* clip = m_clip;
        auto resolveList = [clip, effectId]() -> std::vector<OpacityMask>* {
            if (effectId == 0) return &clip->masks();
            if (Effect* fx = clip->effects().effectById(effectId))
                return &fx->masks();
            return nullptr;
        };
        m_commandStack->pushWithoutExecute(std::make_unique<LambdaCommand>(
            forward ? "Track Mask Forward" : "Track Mask Backward",
            [resolveList, stableMaskId, after]() {
                auto* masks = resolveList();
                if (!masks) return;
                const auto it = std::find_if(
                    masks->begin(), masks->end(),
                    [stableMaskId](const OpacityMask& mask) {
                        return mask.maskId == stableMaskId;
                    });
                if (it != masks->end()) *it = after;
            },
            [resolveList, stableMaskId, before]() {
                auto* masks = resolveList();
                if (!masks) return;
                const auto it = std::find_if(
                    masks->begin(), masks->end(),
                    [stableMaskId](const OpacityMask& mask) {
                        return mask.maskId == stableMaskId;
                    });
                if (it != masks->end()) *it = before;
            }));
    }

    refresh();
    emit maskChanged();
    emit propertyChanged();
    spdlog::info("[MASK-TRACK] wrote {} Mask Path keyframes ({})",
                 keys, forward ? "forward" : "backward");
}

// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
//  buildMaskUI â€” build parameter rows for each mask on the current clip
// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•

void EffectControlsPanel::buildMaskUI(std::vector<OpacityMask>& maskList,
                                      quint64 effectId, int& rowIdx)
{
    if (!m_clip || maskList.empty()) return;

    const auto& tc = Theme::colors();
    const int64_t curT = std::clamp<int64_t>(
        clipRelativeTick(), 0, std::max<int64_t>(0, m_clip->duration()));

    // Row builder for keyframeable mask scalars (same wiring as effect
    // param rows: stopwatch + prev/add/next nav through the shared
    // keyframe handlers).
    auto makeRow = [&](const QString& name, KeyframeTrack<float>* track,
                       MaskParam which, size_t maskIndex) -> PropertyRow* {
        const uint64_t stableMaskId = maskList[maskIndex].maskId;
        auto* row = new PropertyRow(name, track, m_propContainer);
        row->setProperty("maskIndex", static_cast<int>(maskIndex));
        row->setProperty("maskEffectId",
                         QVariant::fromValue<qulonglong>(effectId));
        row->setProperty("maskId",
                         QVariant::fromValue<qulonglong>(stableMaskId));
        row->setRowIndex(rowIdx++);
        row->setTimeProvider([this]() { return clipRelativeTick(); });
        registerPropertyRow(row);
        auto resolveTrack = [clip = m_clip, effectId, stableMaskId, which]()
                -> KeyframeTrack<float>* {
            std::vector<OpacityMask>* list = nullptr;
            if (effectId == 0) {
                list = &clip->masks();
            } else if (Effect* fx = clip->effects().effectById(effectId)) {
                list = &fx->masks();
            }
            if (!list) return nullptr;
            const auto it = std::find_if(list->begin(), list->end(),
                [stableMaskId](const OpacityMask& mask) {
                    return mask.maskId == stableMaskId;
                });
            return it == list->end() ? nullptr : maskScalarTrack(*it, which);
        };
        connect(row, &PropertyRow::addKeyframeRequested,
                this, [this, resolveTrack, row](KeyframeTrack<float>*,
                                                int64_t time) {
            auto* trackNow = resolveTrack();
            if (!trackNow) return;
            const KeyframeTrack<float> before = *trackNow;
            KeyframeTrack<float> after = before;
            after.addKeyframe(time, before.evaluate(time));
            if (m_commandStack) {
                m_commandStack->execute(std::make_unique<LambdaCommand>(
                    "Add Mask Keyframe",
                    [resolveTrack, after]() {
                        if (auto* value = resolveTrack()) *value = after;
                    },
                    [resolveTrack, before]() {
                        if (auto* value = resolveTrack()) *value = before;
                    }));
            } else {
                *trackNow = after;
            }
            row->updateForTime(time);
            if (m_kfTimeline) m_kfTimeline->update();
            emit maskChanged();
            emit propertyChanged();
        });
        connect(row, &PropertyRow::deleteKeyframeRequested,
                this, [this, resolveTrack, row](KeyframeTrack<float>*,
                                                int64_t time) {
            auto* trackNow = resolveTrack();
            if (!trackNow) return;
            const KeyframeTrack<float> before = *trackNow;
            KeyframeTrack<float> after = before;
            after.removeKeyframeAtTime(time);
            if (m_commandStack) {
                m_commandStack->execute(std::make_unique<LambdaCommand>(
                    "Delete Mask Keyframe",
                    [resolveTrack, after]() {
                        if (auto* value = resolveTrack()) *value = after;
                    },
                    [resolveTrack, before]() {
                        if (auto* value = resolveTrack()) *value = before;
                    }));
            } else {
                *trackNow = after;
            }
            row->updateForTime(time);
            if (m_kfTimeline) m_kfTimeline->update();
            emit maskChanged();
            emit propertyChanged();
        });
        connect(row, &PropertyRow::goToPrevKeyframe,
                this, &EffectControlsPanel::onGoToPrevKeyframe);
        connect(row, &PropertyRow::goToNextKeyframe,
                this, &EffectControlsPanel::onGoToNextKeyframe);
        connect(row, &PropertyRow::keyframingToggled,
                this, [this, effectId, stableMaskId, which, row](
                          KeyframeTrack<float>*, bool enabled) {
            if (m_updating || !m_clip) return;
            auto* masks = maskListFor(effectId);
            if (!masks) return;
            auto maskIt = std::find_if(masks->begin(), masks->end(),
                [stableMaskId](const OpacityMask& mask) {
                    return mask.maskId == stableMaskId;
                });
            if (maskIt == masks->end()) return;
            auto* trackNow = maskScalarTrack(*maskIt, which);
            if (!trackNow) return;

            const int64_t t = std::clamp<int64_t>(
                clipRelativeTick(), 0,
                std::max<int64_t>(0, m_clip->duration()));
            const KeyframeTrack<float> before = *trackNow;
            KeyframeTrack<float> after = before;
            if (enabled) {
                after.addKeyframe(t, before.evaluate(t));
            } else {
                const float frozen = before.evaluate(t);
                while (after.keyframeCount() > 0)
                    after.removeKeyframe(after.keyframeCount() - 1);
                after.setDefaultValue(frozen);
            }

            Clip* clip = m_clip;
            auto resolve = [clip, effectId, stableMaskId, which]()
                    -> KeyframeTrack<float>* {
                std::vector<OpacityMask>* list = nullptr;
                if (effectId == 0) {
                    list = &clip->masks();
                } else if (Effect* fx = clip->effects().effectById(effectId)) {
                    list = &fx->masks();
                }
                if (!list) return nullptr;
                const auto it = std::find_if(list->begin(), list->end(),
                    [stableMaskId](const OpacityMask& mask) {
                        return mask.maskId == stableMaskId;
                    });
                return it == list->end() ? nullptr
                                         : maskScalarTrack(*it, which);
            };
            if (m_commandStack) {
                m_commandStack->execute(std::make_unique<LambdaCommand>(
                    enabled ? "Animate Mask Property"
                            : "Stop Animating Mask Property",
                    [resolve, after]() {
                        if (auto* trk = resolve()) *trk = after;
                    },
                    [resolve, before]() {
                        if (auto* trk = resolve()) *trk = before;
                    }));
            } else {
                *trackNow = after;
            }
            row->updateForTime(t);
            emit maskChanged();
            emit propertyChanged();
        });
        connect(row, &PropertyRow::resetRequested,
                this, [this, resolveTrack, row, which]() {
            auto* trackNow = resolveTrack();
            if (!trackNow || !m_clip) return;
            const int64_t t = std::clamp<int64_t>(
                clipRelativeTick(), 0,
                std::max<int64_t>(0, m_clip->duration()));
            const float factory = which == MaskParam::Opacity ? 1.0f : 0.0f;
            const KeyframeTrack<float> before = *trackNow;
            KeyframeTrack<float> after = before;
            if (after.keyframeCount() > 0)
                after.addKeyframe(t, factory);
            else
                after.setDefaultValue(factory);

            if (m_commandStack) {
                m_commandStack->execute(std::make_unique<LambdaCommand>(
                    "Reset Mask Property",
                    [resolveTrack, after]() {
                        if (auto* track = resolveTrack()) *track = after;
                    },
                    [resolveTrack, before]() {
                        if (auto* track = resolveTrack()) *track = before;
                    }));
            } else {
                *trackNow = after;
            }
            row->updateForTime(t);
            if (m_kfTimeline) m_kfTimeline->update();
            emit maskChanged();
            emit propertyChanged();
        });
        return row;
    };

    for (size_t mi = 0; mi < maskList.size(); ++mi) {
        auto& mask = maskList[mi];
        const uint64_t stableMaskId = mask.maskId;

        // â”€â”€ Mask sub-section header â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
        auto* header = new QWidget(m_propContainer);
        header->setObjectName(QStringLiteral("maskHeader"));
        header->setFixedHeight(26);
        header->setCursor(Qt::PointingHandCursor);
        header->setStyleSheet(QStringLiteral(
            "background: %1; border-top: 1px solid %2; border-bottom: 1px solid %2;")
            .arg(Theme::hex(tc.surface2), Theme::hex(tc.border)));
        auto* hl = new QHBoxLayout(header);
        hl->setContentsMargins(24, 0, 6, 0);
        hl->setSpacing(6);

        auto* arrow = new QToolButton(header);
        arrow->setObjectName(QStringLiteral("maskCollapseButton"));
        arrow->setText(QStringLiteral("\u25BC"));
        arrow->setFixedSize(16, 20);
        arrow->setStyleSheet(QStringLiteral(
            "QToolButton { color: %1; font-size: %3px; background: transparent; border: none; padding: 0; }"
            "QToolButton:hover { color: %2; }")
            .arg(Theme::hex(tc.textSecondary), Theme::hex(tc.textPrimary))
            .arg(Theme::typography().sizeXxs));
        hl->addWidget(arrow);

        auto* titleLabel = new QLabel(QString::fromStdString(mask.name), header);
        titleLabel->setObjectName(QStringLiteral("maskTitleLabel"));
        titleLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
        titleLabel->setStyleSheet(QStringLiteral(
            "color: %1; font-size: %2px; font-weight: bold; background: transparent;")
            .arg(Theme::hex(tc.textPrimary))
            .arg(Theme::typography().sizeXs));
        hl->addWidget(titleLabel);
        hl->addStretch();

        // Delete mask button
        auto* deleteBtn = new QToolButton(header);
        deleteBtn->setText(QStringLiteral("\u2715"));
        deleteBtn->setFixedSize(20, 20);
        deleteBtn->setStyleSheet(QStringLiteral(
            "QToolButton { color: %1; font-size: %3px; background: transparent; border: none; padding: 0; }"
            "QToolButton:hover { color: %2; }")
            .arg(Theme::hex(tc.textTertiary), Theme::hex(tc.error))
            .arg(Theme::typography().sizeXs));
        hl->addWidget(deleteBtn);
        connect(deleteBtn, &QToolButton::clicked, this,
                [this, effectId, stableMaskId]() {
            deleteMask(effectId, stableMaskId);
        });

        m_sectionArrows.push_back({header, arrow, {}, QString::fromStdString(mask.name)});
        m_propLayout->addWidget(header);

        // Click on mask header â†’ select this mask for editing in Program Monitor
        header->setProperty("maskIndex", static_cast<int>(mi));
        header->setProperty("maskEffectId", QVariant::fromValue<qulonglong>(effectId));
        header->setProperty("maskId",
                            QVariant::fromValue<qulonglong>(stableMaskId));
        installMaskSelectionFilters(header);

        // ── Mask Path (stopwatch + keyframe nav, Premiere-style) ─────────
        {
            auto* w = new QWidget(m_propContainer);
            w->setObjectName(QStringLiteral("maskPathRow"));
            w->setProperty("maskEffectId", QVariant::fromValue<qulonglong>(effectId));
            w->setProperty("maskIndex", static_cast<qulonglong>(mi));
            w->setProperty("maskId",
                           QVariant::fromValue<qulonglong>(stableMaskId));
            w->setFixedHeight(28);
            auto* lay = new QHBoxLayout(w);
            lay->setContentsMargins(8, 2, 6, 2);
            lay->setSpacing(4);

            // Match PropertyRow's 14x18 expand-control slot. Mask Path has
            // no float curve to expand, but its stopwatch must align exactly.
            auto* leadingSlot = new QWidget(w);
            leadingSlot->setObjectName(QStringLiteral("maskPathLeadingSlot"));
            leadingSlot->setFixedSize(14, 18);
            lay->addWidget(leadingSlot);

            // Stopwatch: toggles Mask Path animation
            auto* stopwatch = new QToolButton(w);
            stopwatch->setObjectName(QStringLiteral("maskPathStopwatch"));
            PropertyRow::configureStopwatchButton(stopwatch);
            stopwatch->setChecked(mask.pathAnimated);
            stopwatch->setToolTip(tr("Toggle animation of Mask Path"));
            lay->addWidget(stopwatch);

            auto* lbl = new QLabel(tr("Mask Path"), w);
            lbl->setObjectName(QStringLiteral("maskPathLabel"));
            lbl->setAttribute(Qt::WA_TransparentForMouseEvents);
            lbl->setMinimumWidth(90);
            lbl->setStyleSheet(QStringLiteral(
                "QLabel { color: %1; font-size: %2px; background: transparent; padding-left: 2px; }")
                .arg(Theme::hex(tc.textPrimary))
                .arg(Theme::typography().sizeXs));
            lay->addWidget(lbl);
            lay->addStretch();

            auto makeNavBtn = [&](const QString& text, const QString& tip) {
                auto* b = new QToolButton(w);
                b->setText(text);
                b->setToolTip(tip);
                b->setFixedSize(18, 20);
                b->setStyleSheet(QStringLiteral(
                    "QToolButton { color: %1; background: transparent; border: none; padding: 0; font-size: %3px; }"
                    "QToolButton:hover { color: %2; }")
                    .arg(Theme::hex(tc.textSecondary), Theme::hex(tc.textPrimary))
                    .arg(Theme::typography().sizeXs));
                lay->addWidget(b);
                return b;
            };
            // Tracking buttons (Premiere: track mask backward / forward)
            auto* trackBackBtn = makeNavBtn(QStringLiteral("«"),
                                            tr("Track selected mask backward"));
            auto* trackFwdBtn  = makeNavBtn(QStringLiteral("»"),
                                            tr("Track selected mask forward"));
            connect(trackBackBtn, &QToolButton::clicked, this,
                    [this, effectId, stableMaskId]() {
                auto* masks = maskListFor(effectId);
                if (!masks) return;
                const auto it = std::find_if(
                    masks->begin(), masks->end(),
                    [stableMaskId](const OpacityMask& value) {
                        return value.maskId == stableMaskId;
                    });
                if (it != masks->end())
                    trackMask(effectId,
                              static_cast<size_t>(it - masks->begin()), false);
            });
            connect(trackFwdBtn, &QToolButton::clicked, this,
                    [this, effectId, stableMaskId]() {
                auto* masks = maskListFor(effectId);
                if (!masks) return;
                const auto it = std::find_if(
                    masks->begin(), masks->end(),
                    [stableMaskId](const OpacityMask& value) {
                        return value.maskId == stableMaskId;
                    });
                if (it != masks->end())
                    trackMask(effectId,
                              static_cast<size_t>(it - masks->begin()), true);
            });

            auto* prevBtn = makeNavBtn(QStringLiteral("◀"), tr("Go to previous Mask Path keyframe"));
            auto* diaBtn  = makeNavBtn(QStringLiteral("◆"), tr("Add/remove Mask Path keyframe at playhead"));
            auto* nextBtn = makeNavBtn(QStringLiteral("▶"), tr("Go to next Mask Path keyframe"));

            diaBtn->setObjectName(QStringLiteral("maskPathKeyButton"));
            diaBtn->setCheckable(true);
            diaBtn->setChecked(mask.pathAnimated && mask.hasPathKeyAt(curT));
            diaBtn->setEnabled(mask.pathAnimated);
            prevBtn->setEnabled(mask.pathAnimated
                                && mask.prevPathKeyTime(curT) != curT);
            nextBtn->setEnabled(mask.pathAnimated
                                && mask.nextPathKeyTime(curT) != curT);
            m_maskPathControls.push_back(
                {effectId, mi, mask.maskId, w,
                 stopwatch, prevBtn, diaBtn, nextBtn});

            connect(stopwatch, &QToolButton::toggled, this,
                    [this, effectId, stableMaskId](bool on) {
                if (m_updating) return;
                auto* list = maskListFor(effectId);
                if (!list) return;
                const auto current = std::find_if(
                    list->begin(), list->end(),
                    [stableMaskId](const OpacityMask& value) {
                        return value.maskId == stableMaskId;
                    });
                if (current == list->end()) return;
                auto& m = *current;
                if (on == m.pathAnimated) return;
                Clip* clip = m_clip;
                OpacityMask before = m;
                OpacityMask after  = m;
                const int64_t t = std::clamp<int64_t>(
                    clipRelativeTick(), 0,
                    std::max<int64_t>(0, m_clip->duration()));
                if (on) {
                    // Premiere: enabling the stopwatch seeds the first
                    // keyframe at the playhead with the current path.
                    after.pathAnimated = true;
                    after.pathKeys.clear();
                    after.addPathKey(t, m.geometryAt(t));
                } else {
                    // Disabling removes all keyframes; the path freezes at
                    // its value at the playhead.
                    after.base = m.geometryAt(t);
                    after.pathKeys.clear();
                    after.pathAnimated = false;
                }
                auto resolveList = [clip, effectId]() -> std::vector<OpacityMask>* {
                    if (effectId == 0) return &clip->masks();
                    if (Effect* fx = clip->effects().effectById(effectId))
                        return &fx->masks();
                    return nullptr;
                };
                if (m_commandStack) {
                    m_commandStack->execute(std::make_unique<LambdaCommand>(
                        on ? "Animate Mask Path" : "Stop Animating Mask Path",
                        [resolveList, stableMaskId, after]() {
                            auto* masks = resolveList();
                            if (!masks) return;
                            const auto it = std::find_if(
                                masks->begin(), masks->end(),
                                [stableMaskId](const OpacityMask& value) {
                                    return value.maskId == stableMaskId;
                                });
                            if (it != masks->end()) *it = after;
                        },
                        [resolveList, stableMaskId, before]() {
                            auto* masks = resolveList();
                            if (!masks) return;
                            const auto it = std::find_if(
                                masks->begin(), masks->end(),
                                [stableMaskId](const OpacityMask& value) {
                                    return value.maskId == stableMaskId;
                                });
                            if (it != masks->end()) *it = before;
                        }));
                } else {
                    m = after;
                }
                emit maskChanged();
                emit propertyChanged();
                updateMaskPathControls(t);
            });

            connect(diaBtn, &QToolButton::clicked, this,
                    [this, effectId, stableMaskId]() {
                auto* list = maskListFor(effectId);
                if (!list) return;
                const auto current = std::find_if(
                    list->begin(), list->end(),
                    [stableMaskId](const OpacityMask& value) {
                        return value.maskId == stableMaskId;
                    });
                if (current == list->end()) return;
                auto& m = *current;
                if (!m.pathAnimated) return;
                Clip* clip = m_clip;
                OpacityMask before = m;
                OpacityMask after  = m;
                const int64_t t = std::clamp<int64_t>(
                    clipRelativeTick(), 0,
                    std::max<int64_t>(0, m_clip->duration()));
                if (m.hasPathKeyAt(t))
                    after.removePathKeyAtTime(t);
                else
                    after.addPathKey(t, m.geometryAt(t));
                auto resolveList = [clip, effectId]() -> std::vector<OpacityMask>* {
                    if (effectId == 0) return &clip->masks();
                    if (Effect* fx = clip->effects().effectById(effectId))
                        return &fx->masks();
                    return nullptr;
                };
                if (m_commandStack) {
                    m_commandStack->execute(std::make_unique<LambdaCommand>(
                        "Toggle Mask Path Keyframe",
                        [resolveList, stableMaskId, after]() {
                            auto* masks = resolveList();
                            if (!masks) return;
                            const auto it = std::find_if(
                                masks->begin(), masks->end(),
                                [stableMaskId](const OpacityMask& value) {
                                    return value.maskId == stableMaskId;
                                });
                            if (it != masks->end()) *it = after;
                        },
                        [resolveList, stableMaskId, before]() {
                            auto* masks = resolveList();
                            if (!masks) return;
                            const auto it = std::find_if(
                                masks->begin(), masks->end(),
                                [stableMaskId](const OpacityMask& value) {
                                    return value.maskId == stableMaskId;
                                });
                            if (it != masks->end()) *it = before;
                        }));
                } else {
                    m = after;
                }
                emit maskChanged();
                emit propertyChanged();
                updateMaskPathControls(t);
            });

            auto seekToKey = [this, effectId, stableMaskId](bool forward) {
                auto* list = maskListFor(effectId);
                if (!list || !m_clip) return;
                const auto current = std::find_if(
                    list->begin(), list->end(),
                    [stableMaskId](const OpacityMask& value) {
                        return value.maskId == stableMaskId;
                    });
                if (current == list->end()) return;
                auto& m = *current;
                const int64_t t = std::clamp<int64_t>(
                    clipRelativeTick(), 0,
                    std::max<int64_t>(0, m_clip->duration()));
                const int64_t target = forward ? m.nextPathKeyTime(t)
                                               : m.prevPathKeyTime(t);
                if (target == t) return;
                emit seekRequested(m_clip->timelineIn() + target);
            };
            connect(prevBtn, &QToolButton::clicked, this,
                    [seekToKey]() { seekToKey(false); });
            connect(nextBtn, &QToolButton::clicked, this,
                    [seekToKey]() { seekToKey(true); });

            installMaskSelectionFilters(w);
            m_propLayout->addWidget(w);
        }

        // ── Mask Feather (keyframeable, px) ──────────────────────────────
        {
            auto* row = makeRow(tr("Mask Feather"), &mask.feather,
                                MaskParam::Feather, mi);
            auto* spin = createScrubby(0, 500, 0.5, 1, " px");
            spin->setValue(static_cast<double>(mask.feather.evaluate(curT)));
            row->addValueWidget(spin);
            installMaskSelectionFilters(row);
            m_propLayout->addWidget(row);
            wireMaskParam(spin, effectId, mi, MaskParam::Feather, 1.0f);
        }

        // ── Mask Opacity (keyframeable, %) ───────────────────────────────
        {
            auto* row = makeRow(tr("Mask Opacity"), &mask.maskOpacity,
                                MaskParam::Opacity, mi);
            auto* spin = createScrubby(0, 100, 0.5, 1, " %");
            spin->setValue(static_cast<double>(mask.maskOpacity.evaluate(curT) * 100.0f));
            row->addValueWidget(spin);
            installMaskSelectionFilters(row);
            m_propLayout->addWidget(row);
            wireMaskParam(spin, effectId, mi, MaskParam::Opacity, 0.01f);
        }

        // ── Mask Expansion (keyframeable, px) ────────────────────────────
        {
            auto* row = makeRow(tr("Mask Expansion"), &mask.expansion,
                                MaskParam::Expansion, mi);
            auto* spin = createScrubby(-500, 500, 0.5, 1, " px");
            spin->setValue(static_cast<double>(mask.expansion.evaluate(curT)));
            row->addValueWidget(spin);
            installMaskSelectionFilters(row);
            m_propLayout->addWidget(row);
            wireMaskParam(spin, effectId, mi, MaskParam::Expansion, 1.0f);
        }

        // ── Inverted checkbox ────────────────────────────────────────────
        {
            auto* chk = new QCheckBox("Inverted", m_propContainer);
            chk->setProperty("maskIndex", static_cast<int>(mi));
            chk->setProperty("maskEffectId",
                             QVariant::fromValue<qulonglong>(effectId));
            chk->setProperty("maskId",
                             QVariant::fromValue<qulonglong>(stableMaskId));
            chk->setChecked(mask.inverted);
            chk->setFixedHeight(28);
            chk->setStyleSheet(QStringLiteral(
                "QCheckBox { color: %1; font-size: %2px; padding-left: 36px; background: transparent; }"
                "QCheckBox::indicator { width: 14px; height: 14px; }")
                .arg(Theme::hex(tc.textPrimary))
                .arg(Theme::typography().sizeXs));
            connect(chk, &QCheckBox::toggled, this,
                    [this, effectId, stableMaskId](bool checked) {
                auto* list = maskListFor(effectId);
                if (!list) return;
                const auto current = std::find_if(
                    list->begin(), list->end(),
                    [stableMaskId](const OpacityMask& value) {
                        return value.maskId == stableMaskId;
                    });
                if (current == list->end()) return;
                Clip* clip = m_clip;
                bool oldVal = current->inverted;
                bool newVal = checked;
                auto resolveList = [clip, effectId]() -> std::vector<OpacityMask>* {
                    if (effectId == 0) return &clip->masks();
                    if (Effect* fx = clip->effects().effectById(effectId))
                        return &fx->masks();
                    return nullptr;
                };
                if (m_commandStack) {
                    m_commandStack->execute(std::make_unique<LambdaCommand>(
                        "Mask Inverted",
                        [resolveList, stableMaskId, newVal]() {
                            auto* masks = resolveList();
                            if (!masks) return;
                            const auto it = std::find_if(
                                masks->begin(), masks->end(),
                                [stableMaskId](const OpacityMask& value) {
                                    return value.maskId == stableMaskId;
                                });
                            if (it != masks->end()) it->inverted = newVal;
                        },
                        [resolveList, stableMaskId, oldVal]() {
                            auto* masks = resolveList();
                            if (!masks) return;
                            const auto it = std::find_if(
                                masks->begin(), masks->end(),
                                [stableMaskId](const OpacityMask& value) {
                                    return value.maskId == stableMaskId;
                                });
                            if (it != masks->end()) it->inverted = oldVal;
                        }));
                } else {
                    current->inverted = checked;
                }
                emit maskChanged();
                emit propertyChanged();
            });
            installMaskSelectionFilters(chk);
            m_propLayout->addWidget(chk);
        }
    }
}

// ═══════════════════════════════════════════════════════════════════════════
//  buildLUTUI — LUT effect with file browser for .cube files
// ═══════════════════════════════════════════════════════════════════════════

void EffectControlsPanel::buildLUTUI(Effect& fx, size_t effectIdx, int& rowIdx)
{
    const auto& tc = Theme::colors();
    auto* lutFx = dynamic_cast<LUT*>(&fx);

    // Row builder helper
    auto makeRow = [&](const QString& name,
                       KeyframeTrack<float>* track) -> PropertyRow* {
        auto* row = new PropertyRow(name, track, m_propContainer);
        row->setRowIndex(rowIdx++);
        row->setTimeProvider([this]() { return clipRelativeTick(); });
        registerPropertyRow(row);
        connect(row, &PropertyRow::addKeyframeRequested,
                this, &EffectControlsPanel::onAddKeyframe);
        connect(row, &PropertyRow::deleteKeyframeRequested,
                this, &EffectControlsPanel::onDeleteKeyframe);
        connect(row, &PropertyRow::goToPrevKeyframe,
                this, &EffectControlsPanel::onGoToPrevKeyframe);
        connect(row, &PropertyRow::goToNextKeyframe,
                this, &EffectControlsPanel::onGoToNextKeyframe);
        return row;
    };

    // ── LUT file path label + load button ─────────────────────────────
    {
        auto* lutWidget = new QWidget(m_propContainer);
        lutWidget->setFixedHeight(28);
        auto* lutLayout = new QHBoxLayout(lutWidget);
        lutLayout->setContentsMargins(36, 2, 6, 2);
        lutLayout->setSpacing(6);

        auto* loadBtn = new QPushButton(QStringLiteral("Load LUT..."), lutWidget);
        loadBtn->setFixedHeight(22);
        loadBtn->setStyleSheet(QStringLiteral(
            "QPushButton { background: %1; color: %2; border: 1px solid %3; "
            "border-radius: %5px; padding: 2px 8px; font-size: %6px; }"
            "QPushButton:hover { background: %4; }")
            .arg(Theme::hex(tc.surface3), Theme::hex(tc.textPrimary),
                 Theme::hex(tc.controlBorder), Theme::hex(tc.controlBgHover))
            .arg(Theme::metrics().radiusSm)
            .arg(Theme::typography().sizeXxs));
        lutLayout->addWidget(loadBtn);

        QString loadedPath;
        if (lutFx && lutFx->hasLUT()) {
            loadedPath = QString::fromStdString(lutFx->lutPath());
        }

        auto* pathLabel = new QLabel(loadedPath.isEmpty()
            ? QStringLiteral("No LUT loaded")
            : QFileInfo(loadedPath).fileName(), lutWidget);
        pathLabel->setStyleSheet(QStringLiteral(
            "color: %1; font-size: %2px; background: transparent;")
            .arg(loadedPath.isEmpty() ? Theme::hex(tc.textTertiary)
                                      : Theme::hex(tc.textSecondary))
            .arg(Theme::typography().sizeXxs));
        pathLabel->setToolTip(loadedPath);
        lutLayout->addWidget(pathLabel, 1);

        m_propLayout->addWidget(lutWidget);

        if (lutFx) {
            connect(loadBtn, &QPushButton::clicked, this, [this, lutFx, pathLabel]() {
                QString filePath = QFileDialog::getOpenFileName(
                    nullptr, QStringLiteral("Load LUT File"),
                    QString(), QStringLiteral("Cube LUT files (*.cube);;All files (*)"));
                if (filePath.isEmpty()) return;

                if (lutFx->loadCubeFile(filePath.toStdString())) {
                    pathLabel->setText(QFileInfo(filePath).fileName());
                    pathLabel->setToolTip(filePath);
                    pathLabel->setStyleSheet(QStringLiteral(
                        "color: %1; font-size: %2px; background: transparent;")
                        .arg(Theme::hex(Theme::colors().textSecondary))
                        .arg(Theme::typography().sizeXxs));
                    emit propertyChanged();
                }
            });
        }
    }

    // ── Intensity parameter ───────────────────────────────────────────
    if (fx.paramCount() > LUT::Param::Intensity) {
        auto& intensityParam = fx.param(LUT::Param::Intensity);
        auto* fxRow = makeRow(QStringLiteral("Intensity"), &intensityParam.track);
        auto* fxSpin = createScrubby(intensityParam.minVal, intensityParam.maxVal,
                                      0.01, 2);
        fxSpin->setValue(static_cast<double>(
            intensityParam.track.evaluate(clipRelativeTick())));
        fxRow->addValueWidget(fxSpin);
        m_propLayout->addWidget(fxRow);
        wireEffectParam(fxSpin, effectIdx, LUT::Param::Intensity);
    }
}

// ═══════════════════════════════════════════════════════════════════════════
//  buildBeatUI — beat-reactive effect: generic params + onset detection
// ═══════════════════════════════════════════════════════════════════════════

void EffectControlsPanel::buildBeatUI(Effect& fx, size_t effectIdx, int& rowIdx)
{
    const auto& tc = Theme::colors();

    // Standard keyframeable params first (Amount, BPM, Offset, Rate, Attack,
    // Decay, Mode). Mode: 0 = manual BPM grid, 1 = detected onsets.
    buildGenericEffectUI(fx, effectIdx, rowIdx);

    // ── Auto-detect section ────────────────────────────────────────────
    auto* box = new QWidget(m_propContainer);
    auto* col = new QVBoxLayout(box);
    col->setContentsMargins(36, 4, 6, 4);
    col->setSpacing(4);

    const QString lblCss = QStringLiteral("color: %1; font-size: %2px; background: transparent;")
        .arg(Theme::hex(tc.textSecondary))
        .arg(Theme::typography().sizeXxs);

    const QString comboCss = QStringLiteral(
        "QComboBox { background: %1; color: %2; border: 1px solid %3; "
        "border-radius: %5px; padding: 1px 4px; font-size: %6px; }"
        "QComboBox QAbstractItemView { background: %1; color: %2; "
        "selection-background-color: %4; }")
        .arg(Theme::hex(tc.inputBg), Theme::hex(tc.textPrimary),
             Theme::hex(tc.controlBorder), Theme::hex(tc.accent))
        .arg(Theme::metrics().radiusSm)
        .arg(Theme::typography().sizeXxs);

    auto* beatFx = static_cast<BeatReactEffect*>(&fx);

    // ── Track picker row ──────────────────────────────────────────────
    auto* trkRow = new QWidget(box);
    auto* trkLay = new QHBoxLayout(trkRow);
    trkLay->setContentsMargins(0, 0, 0, 0);
    trkLay->setSpacing(6);
    auto* trkLabel = new QLabel(QStringLiteral("Track"), trkRow);
    trkLabel->setStyleSheet(lblCss);
    trkLabel->setFixedWidth(36);
    trkLay->addWidget(trkLabel);

    auto* trackCombo = new QComboBox(trkRow);
    trackCombo->setFixedHeight(22);
    trackCombo->setStyleSheet(comboCss);
    trkLay->addWidget(trackCombo, 1);
    col->addWidget(trkRow);

    // ── Clip picker row ───────────────────────────────────────────────
    auto* clipRow = new QWidget(box);
    auto* clipLay = new QHBoxLayout(clipRow);
    clipLay->setContentsMargins(0, 0, 0, 0);
    clipLay->setSpacing(6);
    auto* clipLabel = new QLabel(QStringLiteral("Source"), clipRow);
    clipLabel->setStyleSheet(lblCss);
    clipLabel->setFixedWidth(36);
    clipLay->addWidget(clipLabel);

    auto* clipCombo = new QComboBox(clipRow);
    clipCombo->setFixedHeight(22);
    clipCombo->setStyleSheet(comboCss);
    clipLay->addWidget(clipCombo, 1);
    col->addWidget(clipRow);

    // ── Populate track combo ──────────────────────────────────────────
    // Collect tracks that have at least one audio clip. Store track index
    // as user data so we can look up clips when the track changes.
    trackCombo->addItem(QStringLiteral("(select track)"), QVariant(-1));
    int preselectedTrackIdx = -1;
    if (m_timeline) {
        for (size_t t = 0; t < m_timeline->trackCount(); ++t) {
            auto* tr = m_timeline->track(t);
            if (!tr) continue;
            // Check if this track has any audio clips.
            bool hasAudio = false;
            for (size_t c = 0; c < tr->clipCount(); ++c) {
                auto* cl = tr->clip(c);
                if (cl && cl->isAudio()) { hasAudio = true; break; }
            }
            if (!hasAudio) continue;
            QString trackName = QString::fromStdString(tr->name());
            if (trackName.isEmpty())
                trackName = QStringLiteral("Track %1").arg(t + 1);
            trackCombo->addItem(trackName, QVariant(static_cast<int>(t)));
            // If this track contains the previously-selected audio source,
            // remember it for preselection.
            if (beatFx->audioSourceId() != 0) {
                for (size_t c = 0; c < tr->clipCount(); ++c) {
                    auto* cl = tr->clip(c);
                    if (cl && cl->id() == beatFx->audioSourceId()) {
                        preselectedTrackIdx = static_cast<int>(t);
                        break;
                    }
                }
            }
        }
    }
    if (trackCombo->count() == 1) {
        trackCombo->setItemText(0, QStringLiteral("(no audio tracks)"));
    }
    if (preselectedTrackIdx >= 0) {
        for (int i = 0; i < trackCombo->count(); ++i) {
            if (trackCombo->itemData(i).toInt() == preselectedTrackIdx) {
                trackCombo->setCurrentIndex(i);
                break;
            }
        }
    }

    // ── Helper: populate clip combo for a given track index ───────────
    // (must be a pointer to a shared callable so the connect lambda can
    //  capture it by value; a local std::function works.)
    auto populateClipCombo = std::make_shared<std::function<void(int)>>(
        [this, clipCombo, beatFx](int trackIdx) {
        clipCombo->blockSignals(true);
        clipCombo->clear();
        if (!m_timeline || trackIdx < 0
            || trackIdx >= static_cast<int>(m_timeline->trackCount())) {
            clipCombo->addItem(QStringLiteral("(no clips)"), QVariant(0));
            clipCombo->blockSignals(false);
            return;
        }
        auto* tr = m_timeline->track(static_cast<size_t>(trackIdx));
        if (!tr) {
            clipCombo->addItem(QStringLiteral("(no clips)"), QVariant(0));
            clipCombo->blockSignals(false);
            return;
        }
        bool found = false;
        for (size_t c = 0; c < tr->clipCount(); ++c) {
            auto* cl = tr->clip(c);
            if (!cl || !cl->isAudio()) continue;
            found = true;
            QString lbl = QString::fromStdString(cl->label());
            if (lbl.isEmpty()) lbl = QStringLiteral("Clip %1").arg(c + 1);
            clipCombo->addItem(lbl, QVariant(static_cast<qulonglong>(cl->id())));
            if (cl->id() == beatFx->audioSourceId())
                clipCombo->setCurrentIndex(clipCombo->count() - 1);
        }
        if (!found)
            clipCombo->addItem(QStringLiteral("(no audio clips)"), QVariant(0));
        clipCombo->blockSignals(false);
    });

    // Initial population.
    int initTrack = preselectedTrackIdx >= 0 ? preselectedTrackIdx
        : trackCombo->currentData().toInt();
    (*populateClipCombo)(initTrack);

    // Track selection → repopulate clips.
    connect(trackCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this, trackCombo, populateClipCombo](int) {
        int ti = trackCombo->currentData().toInt();
        (*populateClipCombo)(ti);
    });

    // ── Sensitivity slider ────────────────────────────────────────────
    auto* sensRow = new QWidget(box);
    auto* sensLay = new QHBoxLayout(sensRow);
    sensLay->setContentsMargins(0, 0, 0, 0);
    sensLay->setSpacing(6);
    auto* sensLabel = new QLabel(QStringLiteral("Sens."), sensRow);
    sensLabel->setStyleSheet(lblCss);
    sensLabel->setFixedWidth(36);
    sensLay->addWidget(sensLabel);

    auto* sensSlider = new QSlider(Qt::Horizontal, sensRow);
    sensSlider->setRange(50, 300);  // 0.5 → 3.0
    sensSlider->setValue(120);      // default 1.2 (more sensitive than old 1.4)
    sensSlider->setFixedHeight(20);
    sensSlider->setStyleSheet(QStringLiteral(
        "QSlider::groove:horizontal { background: %1; height: 4px; border-radius: 2px; }"
        "QSlider::handle:horizontal { background: %2; width: 12px; height: 12px; "
        "margin: -4px 0; border-radius: 6px; }"
        "QSlider::sub-page:horizontal { background: %2; border-radius: 2px; }")
        .arg(Theme::hex(tc.surface3), Theme::hex(tc.accent)));
    sensLay->addWidget(sensSlider, 1);

    auto* sensValLabel = new QLabel(QStringLiteral("1.2"), sensRow);
    sensValLabel->setStyleSheet(lblCss);
    sensValLabel->setFixedWidth(24);
    sensValLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    sensLay->addWidget(sensValLabel);
    col->addWidget(sensRow);
    connect(sensSlider, &QSlider::valueChanged, this,
            [sensValLabel](int v) {
        sensValLabel->setText(QString::number(v / 100.0, 'f', 1));
    });

    // Bass-only toggle.
    auto* bassCheck = new QCheckBox(QStringLiteral("Bass / kick only"), box);
    bassCheck->setChecked(true);
    bassCheck->setStyleSheet(lblCss);
    col->addWidget(bassCheck);

    // Detect button + status.
    auto* btnRow = new QWidget(box);
    auto* btnLay = new QHBoxLayout(btnRow);
    btnLay->setContentsMargins(0, 0, 0, 0);
    btnLay->setSpacing(6);
    auto* detectBtn = new QPushButton(QStringLiteral("Detect Beats"), btnRow);
    detectBtn->setFixedHeight(22);
    detectBtn->setStyleSheet(QStringLiteral(
        "QPushButton { background: %1; color: %2; border: 1px solid %3; "
        "border-radius: %5px; padding: 2px 8px; font-size: %6px; }"
        "QPushButton:hover { background: %4; }")
        .arg(Theme::hex(tc.surface3), Theme::hex(tc.textPrimary),
             Theme::hex(tc.controlBorder), Theme::hex(tc.controlBgHover))
        .arg(Theme::metrics().radiusSm)
        .arg(Theme::typography().sizeXxs));
    btnLay->addWidget(detectBtn);

    auto* status = new QLabel(box);
    status->setStyleSheet(lblCss);
    if (!beatFx->beatTimes().empty())
        status->setText(QStringLiteral("%1 beats — Auto mode").arg(beatFx->beatTimes().size()));
    else
        status->setText(QStringLiteral("Mode 0 = manual BPM"));
    btnLay->addWidget(status, 1);
    col->addWidget(btnRow);

    m_propLayout->addWidget(box);

    // ── Detection logic (shared by button + auto-detect) ──────────────
    auto runDetection = [this, effectIdx, clipCombo, bassCheck, sensSlider, status]() {
        if (!m_clip || !m_timeline) return;
        const uint64_t srcId = clipCombo->currentData().toULongLong();
        if (srcId == 0) { status->setText(QStringLiteral("Pick an audio clip")); return; }

        // Locate the source audio clip on the timeline.
        Clip* audio = nullptr;
        for (size_t t = 0; t < m_timeline->trackCount() && !audio; ++t) {
            auto* tr = m_timeline->track(t);
            if (!tr) continue;
            for (size_t c = 0; c < tr->clipCount(); ++c) {
                auto* cl = tr->clip(c);
                if (cl && cl->id() == srcId) { audio = cl; break; }
            }
        }
        if (!audio || !audio->isAudio()) {
            status->setText(QStringLiteral("Source not found"));
            return;
        }
        const std::string path = static_cast<AudioClip*>(audio)->mediaPath();

        AudioFile af;
        if (!af.open(path)) {
            status->setText(QStringLiteral("Can't read audio"));
            spdlog::warn("BeatDetect: failed to open '{}'", path);
            return;
        }
        auto samples = af.readAll();
        const auto& info = af.info();
        if (samples.empty() || info.channels == 0) {
            status->setText(QStringLiteral("Empty audio"));
            return;
        }

        BeatDetectorParams bp;
        bp.bassOnly   = bassCheck->isChecked();
        bp.sensitivity = sensSlider->value() / 100.0f;
        auto onsets = detectBeatsInterleaved(samples.data(), info.frames,
                                             info.channels, info.sampleRate, bp);

        // Map file-time onsets → this (video) clip's local seconds via the
        // timeline positions, so the pulse fires at the right moment.
        std::vector<float> local;
        local.reserve(onsets.size());
        const int64_t aSrcIn = audio->sourceIn();
        const int64_t aTlIn  = audio->timelineIn();
        const int64_t aDur   = audio->duration();
        const int64_t vTlIn  = m_clip->timelineIn();
        const int64_t vDur   = m_clip->duration();
        for (float ta : onsets) {
            const int64_t fileTick = static_cast<int64_t>(ta * kTicksPerSecond);
            if (fileTick < aSrcIn || fileTick > aSrcIn + aDur) continue;
            const int64_t localTick = (aTlIn + (fileTick - aSrcIn)) - vTlIn;
            if (localTick < 0 || localTick > vDur) continue;
            local.push_back(static_cast<float>(ticksToSeconds(localTick)));
        }

        auto& st = m_clip->effects();
        if (effectIdx >= st.effectCount()) return;
        auto* be = static_cast<BeatReactEffect*>(&st.effect(effectIdx));
        be->setAudioSourceId(srcId);
        be->setBeatTimes(std::move(local));
        // Switch to Auto mode so the detected onsets drive the pulse
        // (overrides manual BPM).
        be->param(BeatReactEffect::Mode).track.setDefaultValue(1.0f);

        status->setText(QStringLiteral("%1 beats — Auto mode").arg(be->beatTimes().size()));
        spdlog::info("BeatDetect: {} onsets from '{}' (sens={:.1f}) baked into clip '{}'",
                     be->beatTimes().size(), audio->label(),
                     bp.sensitivity, m_clip->label());
        emit propertyChanged();
    };

    // ── Wire up ───────────────────────────────────────────────────────
    // "Detect Beats" button.
    connect(detectBtn, &QPushButton::clicked, this, runDetection);

    // Auto-detect when a different audio clip is selected — this overrides
    // BPM and switches to Auto (detected) mode immediately, matching the
    // expectation that picking a Beat Source drives the effect.
    connect(clipCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this, runDetection, clipCombo](int) {
        if (clipCombo->currentData().toULongLong() != 0)
            runDetection();
    });
}

// ═══════════════════════════════════════════════════════════════════════════
//  buildLetterboxUI — Letterbox with preset aspect ratio dropdown
// ═══════════════════════════════════════════════════════════════════════════

void EffectControlsPanel::buildTintUI(Effect& fx, size_t effectIdx, int& rowIdx)
{
    const auto& tc = Theme::colors();

    auto addColorRow = [&](const QString& label, size_t firstParam,
                           const QString& objectName) {
        if (fx.paramCount() <= firstParam + 2) return;

        auto* row = new QWidget(m_propContainer);
        row->setFixedHeight(30);
        auto* layout = new QHBoxLayout(row);
        layout->setContentsMargins(36, 2, 6, 2);
        layout->setSpacing(6);

        auto* nameLabel = new QLabel(label, row);
        nameLabel->setStyleSheet(QStringLiteral(
            "color: %1; font-size: %2px; background: transparent;")
            .arg(Theme::hex(tc.textPrimary))
            .arg(Theme::typography().sizeXs));
        layout->addWidget(nameLabel);
        layout->addStretch();

        const int64_t t = clipRelativeTick();
        const QColor initial = QColor::fromRgbF(
            fx.param(firstParam).track.evaluate(t),
            fx.param(firstParam + 1).track.evaluate(t),
            fx.param(firstParam + 2).track.evaluate(t));

        auto* swatch = new QPushButton(row);
        swatch->setObjectName(objectName);
        swatch->setFixedSize(44, 22);
        swatch->setToolTip(tr("Choose %1").arg(label));
        auto setSwatchColor = [swatch](const QColor& color) {
            swatch->setStyleSheet(QStringLiteral(
                "QPushButton { background: %1; border: 1px solid %2; border-radius: 2px; }")
                .arg(color.name(), Theme::hex(Theme::colors().controlBorder)));
        };
        setSwatchColor(initial);
        layout->addWidget(swatch);
        m_propLayout->addWidget(row);

        connect(swatch, &QPushButton::clicked, this,
                [this, effectIdx, firstParam, label, setSwatchColor]() {
            if (!m_clip) return;
            auto& stack = m_clip->effects();
            if (effectIdx >= stack.effectCount()) return;
            auto& effect = stack.effect(effectIdx);
            if (effect.paramCount() <= firstParam + 2) return;

            const int64_t time = clipRelativeTick();
            const QColor current = QColor::fromRgbF(
                effect.param(firstParam).track.evaluate(time),
                effect.param(firstParam + 1).track.evaluate(time),
                effect.param(firstParam + 2).track.evaluate(time));
            const QColor chosen = QColorDialog::getColor(
                current, this, tr("Choose %1").arg(label));
            if (!chosen.isValid()) return;

            std::array<KeyframeTrack<float>, 3> before = {
                effect.param(firstParam).track,
                effect.param(firstParam + 1).track,
                effect.param(firstParam + 2).track
            };
            auto after = before;
            after[0].writeValue(time, static_cast<float>(chosen.redF()));
            after[1].writeValue(time, static_cast<float>(chosen.greenF()));
            after[2].writeValue(time, static_cast<float>(chosen.blueF()));

            auto* stackPtr = &stack;
            const uint64_t effectId = effect.id();
            auto applyTracks = [stackPtr, effectId, firstParam](
                                   const std::array<KeyframeTrack<float>, 3>& tracks) {
                auto* target = stackPtr->effectById(effectId);
                if (!target || target->paramCount() <= firstParam + 2) return;
                for (size_t i = 0; i < tracks.size(); ++i)
                    target->param(firstParam + i).track = tracks[i];
            };

            if (m_commandStack) {
                m_commandStack->execute(std::make_unique<LambdaCommand>(
                    "Set Tint Color",
                    [applyTracks, after]() { applyTracks(after); },
                    [applyTracks, before]() { applyTracks(before); }));
            } else {
                applyTracks(after);
            }

            setSwatchColor(chosen);
            if (m_kfTimeline) m_kfTimeline->update();
            emit propertyChanged();
        });
    };

    addColorRow(QStringLiteral("Map Black To"), Tint::MapBlackR,
                QStringLiteral("tintMapBlackButton"));
    addColorRow(QStringLiteral("Map White To"), Tint::MapWhiteR,
                QStringLiteral("tintMapWhiteButton"));

    if (fx.paramCount() > Tint::AmountToTint) {
        auto& amount = fx.param(Tint::AmountToTint);
        auto* amountRow = new PropertyRow(
            QStringLiteral("Amount to Tint"), &amount.track, m_propContainer);
        amountRow->setRowIndex(rowIdx++);
        amountRow->setTimeProvider([this]() { return clipRelativeTick(); });
        registerPropertyRow(amountRow);
        connect(amountRow, &PropertyRow::addKeyframeRequested,
                this, &EffectControlsPanel::onAddKeyframe);
        connect(amountRow, &PropertyRow::deleteKeyframeRequested,
                this, &EffectControlsPanel::onDeleteKeyframe);
        connect(amountRow, &PropertyRow::goToPrevKeyframe,
                this, &EffectControlsPanel::onGoToPrevKeyframe);
        connect(amountRow, &PropertyRow::goToNextKeyframe,
                this, &EffectControlsPanel::onGoToNextKeyframe);

        auto* spin = createScrubby(0.0, 100.0, 1.0, 1, QStringLiteral(" %"));
        spin->setObjectName(QStringLiteral("tintAmountSpin"));
        spin->setValue(amount.track.evaluate(clipRelativeTick()));
        amountRow->addValueWidget(spin);
        m_propLayout->addWidget(amountRow);
        wireEffectParam(spin, effectIdx, Tint::AmountToTint);
    }
}

void EffectControlsPanel::buildLetterboxUI(Effect& fx, size_t effectIdx, int& rowIdx)
{
    const auto& tc = Theme::colors();

    // Row builder helper
    auto makeRow = [&](const QString& name,
                       KeyframeTrack<float>* track) -> PropertyRow* {
        auto* row = new PropertyRow(name, track, m_propContainer);
        row->setRowIndex(rowIdx++);
        row->setTimeProvider([this]() { return clipRelativeTick(); });
        registerPropertyRow(row);
        connect(row, &PropertyRow::addKeyframeRequested,
                this, &EffectControlsPanel::onAddKeyframe);
        connect(row, &PropertyRow::deleteKeyframeRequested,
                this, &EffectControlsPanel::onDeleteKeyframe);
        connect(row, &PropertyRow::goToPrevKeyframe,
                this, &EffectControlsPanel::onGoToPrevKeyframe);
        connect(row, &PropertyRow::goToNextKeyframe,
                this, &EffectControlsPanel::onGoToNextKeyframe);
        return row;
    };

    // ── Aspect ratio presets ──────────────────────────────────────────
    struct AspectPreset {
        const char* label;
        float width;
        float height;
    };
    static const AspectPreset presets[] = {
        {"2.39:1 (CinemaScope)",  2.39f, 1.0f},
        {"2.35:1 (Classic scope)", 2.35f, 1.0f},
        {"2.76:1 (Ultra Panavision)", 2.76f, 1.0f},
        {"1.85:1 (Academy Flat)", 1.85f, 1.0f},
        {"1.78:1 (16:9)",         1.78f, 1.0f},
        {"1.33:1 (4:3)",          1.33f, 1.0f},
        {"1:1 (Square)",          1.0f,  1.0f},
        {"1.43:1 (IMAX)",         1.43f, 1.0f},
    };
    constexpr int kNumPresets = sizeof(presets) / sizeof(presets[0]);

    // ── Preset combo box ──────────────────────────────────────────────
    {
        auto* presetWidget = new QWidget(m_propContainer);
        presetWidget->setFixedHeight(28);
        auto* presetLayout = new QHBoxLayout(presetWidget);
        presetLayout->setContentsMargins(36, 2, 6, 2);
        presetLayout->setSpacing(6);

        auto* presetLabel = new QLabel(QStringLiteral("Aspect Ratio"), presetWidget);
        presetLabel->setStyleSheet(QStringLiteral(
            "color: %1; font-size: %2px; background: transparent;")
            .arg(Theme::hex(tc.textPrimary))
            .arg(Theme::typography().sizeXs));
        presetLayout->addWidget(presetLabel);

        auto* presetCombo = new QComboBox(presetWidget);
        presetCombo->setFixedHeight(22);
        for (int i = 0; i < kNumPresets; ++i)
            presetCombo->addItem(QString::fromUtf8(presets[i].label));
        presetCombo->setStyleSheet(QStringLiteral(
            "QComboBox { background: %1; color: %2; border: 1px solid %3; "
            "border-radius: %5px; padding: 1px 4px; font-size: %6px; }"
            "QComboBox:hover { border-color: %4; }"
            "QComboBox::drop-down { border: none; width: 16px; }")
            .arg(Theme::hex(tc.inputBg), Theme::hex(tc.textPrimary),
                 Theme::hex(tc.controlBorder), Theme::hex(tc.accent))
            .arg(Theme::metrics().radiusSm)
            .arg(Theme::typography().sizeXxs));
        presetLayout->addWidget(presetCombo, 1);

        m_propLayout->addWidget(presetWidget);

        // Try to detect current preset from param values
        float curW = fx.paramCount() > Letterbox::Param::AspectWidth
            ? fx.param(Letterbox::Param::AspectWidth).track.evaluate(clipRelativeTick())
            : 2.39f;
        float curH = fx.paramCount() > Letterbox::Param::AspectHeight
            ? fx.param(Letterbox::Param::AspectHeight).track.evaluate(clipRelativeTick())
            : 1.0f;
        float curRatio = curW / std::max(curH, 0.001f);

        int bestPreset = 0;
        float bestDist = std::abs(curRatio - presets[0].width / presets[0].height);
        for (int i = 1; i < kNumPresets; ++i) {
            float dist = std::abs(curRatio - presets[i].width / presets[i].height);
            if (dist < bestDist) {
                bestDist = dist;
                bestPreset = i;
            }
        }
        presetCombo->setCurrentIndex(bestPreset);

        // Wire preset selection to update AspectWidth/AspectHeight
        connect(presetCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
                this, [this, &fx, effectIdx, presetCombo](int index) {
            if (!m_clip || m_updating) return;
            if (index < 0 || index >= kNumPresets) return;

            float newW = presets[index].width;
            float newH = presets[index].height;

            auto& st = m_clip->effects();
            if (effectIdx >= st.effectCount()) return;
            auto& ef = st.effect(effectIdx);

            if (ef.paramCount() > Letterbox::Param::AspectWidth)
                ef.param(Letterbox::Param::AspectWidth).track.writeValue(
                    clipRelativeTick(), newW);
            if (ef.paramCount() > Letterbox::Param::AspectHeight)
                ef.param(Letterbox::Param::AspectHeight).track.writeValue(
                    clipRelativeTick(), newH);

            emit propertyChanged();
        });
    }

    // ── Bar Opacity ──────────────────────────────────────────────────
    if (fx.paramCount() > Letterbox::Param::BarOpacity) {
        auto& opParam = fx.param(Letterbox::Param::BarOpacity);
        auto* opRow = makeRow(QStringLiteral("Bar Opacity"), &opParam.track);
        auto* opSpin = createScrubby(opParam.minVal, opParam.maxVal, 0.01, 2);
        opSpin->setValue(static_cast<double>(
            opParam.track.evaluate(clipRelativeTick())));
        opRow->addValueWidget(opSpin);
        m_propLayout->addWidget(opRow);
        wireEffectParam(opSpin, effectIdx, Letterbox::Param::BarOpacity);
    }

    // ── Bar Color (R, G, B) — simplified as a single color swatch ────
    // (hidden for now — bars default to black)
    // ── Feather (hidden for simplicity ─ bars are clean)
}

} // namespace rt
