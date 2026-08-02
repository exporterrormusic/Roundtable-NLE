// EssentialGraphicsProperties.cpp - Property binding (extracted from GraphicsEditorPanel.cpp).

#include "panels/effects/GraphicsEditorPanel.h"
#include "widgets/ScrubbySpinBox.h"
#include "Theme.h"
#include "timeline/GraphicClip.h"
#include "timeline/GraphicLayer.h"
#include "command/CommandStack.h"
#include "command/LambdaCommand.h"
#include "viewport/TransformOverlayWidget.h"
#include <QColorDialog>
#include <QLineEdit>
#include <QFontDatabase>
#include <QSignalBlocker>
#include <algorithm>
#include <cmath>
#include <spdlog/spdlog.h>

namespace rt {

void GraphicsEditorPanel::setInlineTextSelectionFormat(
    const QString& family, float pointSize, int weight, bool italic,
    bool allCaps, bool smallCaps, float tracking, float baselineShift,
    float leading, uint32_t mixedFlags)
{
    if (!m_monitorTextEditing) return;
    m_updating = true;
    const auto mixed = [mixedFlags](InlineTextMixedFlag flag) {
        return (mixedFlags & static_cast<uint32_t>(flag)) != 0;
    };
    const auto showMixedValue = [](ScrubbySpinBox* spin, bool isMixed,
                                   const QString& description) {
        if (!spin) return;
        spin->setToolTip(isMixed ? description : QString());
        if (QLineEdit* edit = spin->findChild<QLineEdit*>()) {
            edit->setPlaceholderText(isMixed ? QStringLiteral("\u2014")
                                             : QString());
            if (isMixed) edit->clear();
            else edit->setText(spin->locale().toString(
                spin->value(), 'f', spin->decimals()) + spin->suffix());
        }
    };
    if (m_fontCombo) {
        m_fontCombo->blockSignals(true);
        if (mixed(InlineMixedFamily)) {
            m_fontCombo->setCurrentIndex(-1);
        } else {
            const int index = m_fontCombo->findText(family);
            if (index >= 0) m_fontCombo->setCurrentIndex(index);
        }
        m_fontCombo->setToolTip(mixed(InlineMixedFamily)
            ? tr("Mixed font families in selection") : QString());
        m_fontCombo->blockSignals(false);
    }
    if (m_boldBtn) {
        m_boldBtn->blockSignals(true);
        m_boldBtn->setChecked(weight >= 700);
        m_boldBtn->setText(mixed(InlineMixedWeight)
            ? QStringLiteral("B\u2014") : QStringLiteral("B"));
        m_boldBtn->setToolTip(mixed(InlineMixedWeight)
            ? tr("Mixed font weights in selection") : tr("Bold"));
        m_boldBtn->blockSignals(false);
    }
    if (m_italicBtn) {
        m_italicBtn->blockSignals(true);
        m_italicBtn->setChecked(italic);
        m_italicBtn->setText(mixed(InlineMixedItalic)
            ? QStringLiteral("I\u2014") : QStringLiteral("I"));
        m_italicBtn->setToolTip(mixed(InlineMixedItalic)
            ? tr("Mixed italic styles in selection") : tr("Italic"));
        m_italicBtn->blockSignals(false);
    }
    if (m_allCapsBtn) {
        m_allCapsBtn->blockSignals(true);
        m_allCapsBtn->setChecked(allCaps);
        m_allCapsBtn->setText(mixed(InlineMixedCapitalization)
            ? QStringLiteral("TT\u2014") : QStringLiteral("TT"));
        m_allCapsBtn->setToolTip(mixed(InlineMixedCapitalization)
            ? tr("Mixed capitalization in selection") : tr("All Caps"));
        m_allCapsBtn->blockSignals(false);
    }
    if (m_smallCapsBtn) {
        m_smallCapsBtn->blockSignals(true);
        m_smallCapsBtn->setChecked(smallCaps);
        m_smallCapsBtn->setText(mixed(InlineMixedCapitalization)
            ? QStringLiteral("T\u1D04\u2014") : QStringLiteral("T\u1D04"));
        m_smallCapsBtn->setToolTip(mixed(InlineMixedCapitalization)
            ? tr("Mixed capitalization in selection") : tr("Small Caps"));
        m_smallCapsBtn->blockSignals(false);
    }
    if (m_fontSizeSpin) {
        m_fontSizeSpin->blockSignals(true);
        m_fontSizeSpin->setValue(pointSize);
        m_fontSizeSpin->blockSignals(false);
        showMixedValue(m_fontSizeSpin, mixed(InlineMixedSize),
                       tr("Mixed font sizes in selection"));
    }
    if (m_fontSizeSlider) {
        m_fontSizeSlider->blockSignals(true);
        m_fontSizeSlider->setValue(static_cast<int>(std::round(pointSize)));
        m_fontSizeSlider->blockSignals(false);
    }
    if (m_trackingSpin) {
        m_trackingSpin->blockSignals(true);
        m_trackingSpin->setValue(tracking);
        m_trackingSpin->blockSignals(false);
        showMixedValue(m_trackingSpin, mixed(InlineMixedTracking),
                       tr("Mixed tracking in selection"));
    }
    if (m_baselineShiftSpin) {
        m_baselineShiftSpin->blockSignals(true);
        m_baselineShiftSpin->setValue(baselineShift);
        m_baselineShiftSpin->blockSignals(false);
        showMixedValue(m_baselineShiftSpin, mixed(InlineMixedBaseline),
                       tr("Mixed baseline shifts in selection"));
    }
    if (m_leadingSpin) {
        m_leadingSpin->blockSignals(true);
        m_leadingSpin->setValue(leading);
        m_leadingSpin->blockSignals(false);
        showMixedValue(m_leadingSpin, mixed(InlineMixedLeading),
                       tr("Mixed leading in selection"));
    }
    m_updating = false;
}

void GraphicsEditorPanel::setInlineTextAdvancedFormat(
    const QString& fontStyle, float kerning, float tabWidth, float tsume,
    bool fauxBold, bool fauxItalic, bool underline, bool superscript,
    bool subscript, uint32_t mixedFlags)
{
    if (!m_monitorTextEditing) return;
    m_updating = true;
    auto setButton = [](QToolButton* button, bool checked, bool mixed) {
        if (!button) return;
        QSignalBlocker blocker(button);
        button->setChecked(checked);
        button->setProperty("mixedTextFormat", mixed);
        button->setToolTip(mixed ? QObject::tr("Mixed values in selection")
                                 : button->toolTip());
    };
    if (m_fontStyleCombo) {
        QSignalBlocker blocker(m_fontStyleCombo);
        const bool mixed = mixedFlags & InlineMixedFontStyle;
        const int index = m_fontStyleCombo->findText(fontStyle);
        m_fontStyleCombo->setCurrentIndex(mixed ? -1 : index);
    }
    auto setSpin = [](ScrubbySpinBox* spin, float value, bool mixed) {
        if (!spin) return;
        QSignalBlocker blocker(spin);
        spin->setValue(value);
        if (QLineEdit* edit = spin->findChild<QLineEdit*>()) {
            edit->setPlaceholderText(mixed ? QStringLiteral("\u2014") : QString());
            if (mixed) edit->clear();
        }
    };
    setSpin(m_kerningSpin, kerning, mixedFlags & InlineMixedKerning);
    setSpin(m_tabWidthSpin, tabWidth, mixedFlags & InlineMixedTabWidth);
    setSpin(m_tsumeSpin, tsume, mixedFlags & InlineMixedTsume);
    setButton(m_fauxBoldBtn, fauxBold, mixedFlags & InlineMixedFauxStyle);
    setButton(m_fauxItalicBtn, fauxItalic, mixedFlags & InlineMixedFauxStyle);
    setButton(m_underlineBtn, underline, mixedFlags & InlineMixedDecoration);
    setButton(m_superscriptBtn, superscript, mixedFlags & InlineMixedScript);
    setButton(m_subscriptBtn, subscript, mixedFlags & InlineMixedScript);
    m_updating = false;
}

void GraphicsEditorPanel::setInlineTextSelectionAppearance(
    bool fillEnabled, uint32_t fillColor,
    bool strokeEnabled, uint32_t strokeColor, float strokeWidth,
    int strokePosition, bool shadowEnabled, uint32_t shadowColor,
    float shadowDistance, float shadowAngle, float shadowSoftness,
    float shadowOpacity, bool backgroundEnabled,
    uint32_t backgroundColor, float backgroundPadding, uint32_t mixedFlags)
{
    if (!m_monitorTextEditing) return;
    m_updating = true;
    auto setCheck = [](QCheckBox* check, bool checked, bool mixed) {
        if (!check) return;
        QSignalBlocker blocker(check);
        check->setTristate(mixed);
        check->setCheckState(mixed ? Qt::PartiallyChecked
                                   : checked ? Qt::Checked : Qt::Unchecked);
    };
    auto setColor = [](QPushButton* button, uint32_t argb) {
        if (!button) return;
        button->setProperty("appearanceColor", argb);
        const QColor color = QColor::fromRgba(argb);
        button->setStyleSheet(QStringLiteral(
            "background: %1; border: 1px solid %2;")
            .arg(color.name(QColor::HexArgb),
                 Theme::hex(Theme::colors().border)));
    };
    setCheck(m_fillCheck, fillEnabled, mixedFlags & InlineMixedFill);
    setColor(m_fillColorBtn, fillColor);
    setCheck(m_strokeCheck, strokeEnabled, mixedFlags & InlineMixedStroke);
    setColor(m_strokeColorBtn, strokeColor);
    if (m_strokeColorBtn) m_strokeColorBtn->setEnabled(strokeEnabled);
    if (m_strokeWidthSpin) m_strokeWidthSpin->setValue(strokeWidth);
    if (m_strokeOpacitySpin) {
        m_strokeOpacitySpin->setEnabled(strokeEnabled);
        m_strokeOpacitySpin->setValue(
            ((strokeColor >> 24) & 0xFF) * 100.0 / 255.0);
    }
    if (m_strokePosCombo) m_strokePosCombo->setCurrentIndex(strokePosition);
    setCheck(m_shadowCheck, shadowEnabled, mixedFlags & InlineMixedShadow);
    setColor(m_shadowColorBtn, shadowColor);
    if (m_shadowColorBtn) m_shadowColorBtn->setEnabled(shadowEnabled);
    auto setShadowSpin = [shadowEnabled](ScrubbySpinBox* spin, double value) {
        if (!spin) return;
        QSignalBlocker blocker(spin);
        spin->setValue(value);
        spin->setEnabled(shadowEnabled);
    };
    setShadowSpin(m_shadowDistanceSpin, shadowDistance);
    setShadowSpin(m_shadowAngleSpin, shadowAngle);
    setShadowSpin(m_shadowSoftnessSpin, shadowSoftness);
    setShadowSpin(m_shadowOpacitySpin, shadowOpacity * 100.0);
    setCheck(m_backgroundCheck, backgroundEnabled,
             mixedFlags & InlineMixedBackground);
    setColor(m_backgroundColorBtn, backgroundColor);
    if (m_backgroundColorBtn) m_backgroundColorBtn->setEnabled(backgroundEnabled);
    if (m_backgroundPaddingSpin) {
        m_backgroundPaddingSpin->setEnabled(backgroundEnabled);
        m_backgroundPaddingSpin->setValue(backgroundPadding);
    }
    if (m_backgroundOpacitySpin) {
        m_backgroundOpacitySpin->setEnabled(backgroundEnabled);
        m_backgroundOpacitySpin->setValue(
            ((backgroundColor >> 24) & 0xFF) * 100.0 / 255.0);
    }
    m_updating = false;
}

void GraphicsEditorPanel::setInlineParagraphFormat(
    int alignment, bool rightToLeft, uint32_t mixedFlags)
{
    if (!m_monitorTextEditing) return;
    m_updating = true;
    const bool mixed = mixedFlags & InlineMixedParagraph;
    for (auto* button : {m_alignLeftBtn, m_alignCenterBtn,
                         m_alignRightBtn, m_alignJustifyBtn}) {
        if (!button) continue;
        QSignalBlocker blocker(button);
        const bool selected = !mixed && ((button == m_alignLeftBtn
            && alignment == static_cast<int>(GTextAlign::Left))
            || (button == m_alignCenterBtn
                && alignment == static_cast<int>(GTextAlign::Center))
            || (button == m_alignRightBtn
                && alignment == static_cast<int>(GTextAlign::Right))
            || (button == m_alignJustifyBtn
                && alignment == static_cast<int>(GTextAlign::Justify)));
        button->setChecked(selected);
    }
    if (m_rtlBtn) {
        QSignalBlocker blocker(m_rtlBtn);
        m_rtlBtn->setChecked(rightToLeft);
        m_rtlBtn->setProperty("mixedTextFormat",
                              bool(mixedFlags & InlineMixedDirection));
    }
    m_updating = false;
}

void GraphicsEditorPanel::populateFromLayer()
{
 if (!m_selectedLayer) return;
 m_updating = true;

 // ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ Text properties ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬
 if (m_selectedLayer->layerType() == GraphicLayerType::Text) {
 auto* tl = static_cast<TextLayer*>(m_selectedLayer);

 // Clear any mixed-selection presentation left by the monitor editor.
 if (m_fontCombo) m_fontCombo->setToolTip(QString());
 if (m_boldBtn) { m_boldBtn->setText(QStringLiteral("B")); m_boldBtn->setToolTip(tr("Bold")); }
 if (m_italicBtn) { m_italicBtn->setText(QStringLiteral("I")); m_italicBtn->setToolTip(tr("Italic")); }
 if (m_allCapsBtn) { m_allCapsBtn->setText(QStringLiteral("TT")); m_allCapsBtn->setToolTip(tr("All Caps")); }
 if (m_smallCapsBtn) { m_smallCapsBtn->setText(QStringLiteral("T\u1D04")); m_smallCapsBtn->setToolTip(tr("Small Caps")); }

 // Text content is edited inline via QLineEdit in the layer list.
 // The list item widgets handle their own population during rebuildLayerList().

 if (m_fontCombo) {
 m_fontCombo->blockSignals(true);
 int idx = m_fontCombo->findText(QString::fromStdString(tl->fontFamily()));
 if (idx >= 0) m_fontCombo->setCurrentIndex(idx);
 m_fontCombo->blockSignals(false);
 }
 if (m_fontStyleCombo) {
 m_fontStyleCombo->blockSignals(true);
 m_fontStyleCombo->clear();
 m_fontStyleCombo->addItems(QFontDatabase::styles(
     QString::fromStdString(tl->fontFamily())));
 int styleIndex = m_fontStyleCombo->findText(
     QString::fromStdString(tl->fontStyle()));
 if (styleIndex >= 0) m_fontStyleCombo->setCurrentIndex(styleIndex);
 m_fontStyleCombo->blockSignals(false);
 }
 if (m_boldBtn) {
 m_boldBtn->blockSignals(true);
 m_boldBtn->setChecked(tl->fontWeight() >= 700);
 m_boldBtn->blockSignals(false);
 }
 if (m_italicBtn) {
 m_italicBtn->blockSignals(true);
 m_italicBtn->setChecked(tl->isItalic());
 m_italicBtn->blockSignals(false);
 }
 if (m_allCapsBtn) {
 m_allCapsBtn->blockSignals(true);
 m_allCapsBtn->setChecked(tl->allCaps());
 m_allCapsBtn->blockSignals(false);
 }
 if (m_smallCapsBtn) {
 m_smallCapsBtn->blockSignals(true);
 m_smallCapsBtn->setChecked(tl->smallCaps());
 m_smallCapsBtn->blockSignals(false);
 }
 auto setChecked = [](QToolButton* button, bool checked) {
     if (!button) return;
     QSignalBlocker blocker(button);
     button->setChecked(checked);
 };
 setChecked(m_fauxBoldBtn, tl->fauxBold());
 setChecked(m_fauxItalicBtn, tl->fauxItalic());
 setChecked(m_underlineBtn, tl->underline());
 setChecked(m_superscriptBtn, tl->superscript());
 setChecked(m_subscriptBtn, tl->subscript());
 setChecked(m_rtlBtn, tl->rightToLeft());
 if (m_fontSizeSpin) m_fontSizeSpin->setValue(tl->fontSize());
 if (m_fontSizeSlider) m_fontSizeSlider->setValue(static_cast<int>(tl->fontSize()));

 // Alignment
 if (m_alignLeftBtn) {
 m_alignLeftBtn->blockSignals(true);
 m_alignLeftBtn->setChecked(tl->alignment() == GTextAlign::Left);
 m_alignLeftBtn->blockSignals(false);
 }
 if (m_alignCenterBtn) {
 m_alignCenterBtn->blockSignals(true);
 m_alignCenterBtn->setChecked(tl->alignment() == GTextAlign::Center);
 m_alignCenterBtn->blockSignals(false);
 }
 if (m_alignRightBtn) {
 m_alignRightBtn->blockSignals(true);
 m_alignRightBtn->setChecked(tl->alignment() == GTextAlign::Right);
 m_alignRightBtn->blockSignals(false);
 }
 if (m_alignJustifyBtn) {
 m_alignJustifyBtn->blockSignals(true);
 m_alignJustifyBtn->setChecked(tl->alignment() == GTextAlign::Justify);
 m_alignJustifyBtn->blockSignals(false);
 }
 // Vertical alignment
 if (m_valignTopBtn) {
 m_valignTopBtn->blockSignals(true);
 m_valignTopBtn->setChecked(tl->vAlignment() == GTextVAlign::Top);
 m_valignTopBtn->blockSignals(false);
 }
 if (m_valignMiddleBtn) {
 m_valignMiddleBtn->blockSignals(true);
 m_valignMiddleBtn->setChecked(tl->vAlignment() == GTextVAlign::Middle);
 m_valignMiddleBtn->blockSignals(false);
 }
 if (m_valignBottomBtn) {
 m_valignBottomBtn->blockSignals(true);
 m_valignBottomBtn->setChecked(tl->vAlignment() == GTextVAlign::Bottom);
 m_valignBottomBtn->blockSignals(false);
 }

 // Spacing
 if (m_trackingSpin) m_trackingSpin->setValue(tl->tracking().evaluate(0));
 if (m_leadingSpin) m_leadingSpin->setValue(tl->leading().evaluate(0));
 if (m_baselineShiftSpin) m_baselineShiftSpin->setValue(tl->baselineShift().evaluate(0));
 if (m_kerningSpin) m_kerningSpin->setValue(tl->kerning());
 if (m_tabWidthSpin) m_tabWidthSpin->setValue(tl->tabWidth());
 if (m_tsumeSpin) m_tsumeSpin->setValue(tl->tsume());

 // Paragraph box
 if (m_wrapCheck) {
 m_wrapCheck->blockSignals(true);
 m_wrapCheck->setChecked(tl->useParagraphBox());
 m_wrapCheck->blockSignals(false);
 }
 if (m_wrapWidthSpin) {
 m_wrapWidthSpin->setEnabled(tl->useParagraphBox());
 m_wrapWidthSpin->setValue(tl->boxWidth() > 1.0f ? tl->boxWidth() : 800.0);
 }
 if (m_wrapHeightSpin) {
 m_wrapHeightSpin->setEnabled(tl->useParagraphBox());
 m_wrapHeightSpin->setValue(tl->boxHeight() > 1.0f ? tl->boxHeight() : 300.0);
 }
 for (ScrubbySpinBox* spin : {m_fontSizeSpin, m_trackingSpin,
                              m_leadingSpin, m_baselineShiftSpin,
                              m_kerningSpin, m_tabWidthSpin, m_tsumeSpin}) {
     if (!spin) continue;
     spin->setToolTip(QString());
     if (QLineEdit* edit = spin->findChild<QLineEdit*>()) {
         edit->setPlaceholderText(QString());
         edit->setText(spin->locale().toString(
             spin->value(), 'f', spin->decimals()) + spin->suffix());
     }
 }
 }

 // ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ Appearance ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬
 const auto& app = m_selectedLayer->appearance();

 // Fill
 if (m_fillCheck) {
 bool hasFill = !app.fills.empty() && app.fills[0].enabled;
 m_fillCheck->blockSignals(true);
 m_fillCheck->setChecked(hasFill);
 m_fillCheck->blockSignals(false);
 }
 if (m_fillColorBtn) {
 uint32_t fc = 0xFFFFFFFF;
 if (!app.fills.empty()) fc = app.fills[0].color;
  QColor fillC((fc >> 16) & 0xFF, (fc >> 8) & 0xFF, fc & 0xFF, (fc >> 24) & 0xFF);
  m_fillColorBtn->setProperty("appearanceColor", fc);
 m_fillColorBtn->setStyleSheet(
 QStringLiteral("background: %1; border: 1px solid %2;")
 .arg(fillC.name(), Theme::hex(Theme::colors().border)));
 }

 // Stroke
 if (m_strokeCheck) {
 bool hasStroke = !app.strokes.empty() && app.strokes[0].enabled;
 uint32_t sc = !app.strokes.empty() ? app.strokes[0].color : 0xFF000000;
 m_strokeCheck->blockSignals(true);
 m_strokeCheck->setChecked(hasStroke);
 m_strokeCheck->blockSignals(false);
 if (m_strokeWidthSpin) {
 m_strokeWidthSpin->setEnabled(hasStroke);
 m_strokeWidthSpin->setValue(hasStroke ? app.strokes[0].width : 2.0);
 }
 if (m_strokeColorBtn) {
 m_strokeColorBtn->setEnabled(hasStroke);
  QColor strokeC((sc >> 16) & 0xFF, (sc >> 8) & 0xFF, sc & 0xFF);
  m_strokeColorBtn->setProperty("appearanceColor", sc);
 m_strokeColorBtn->setStyleSheet(
 QStringLiteral("background: %1; border: 1px solid %2;")
 .arg(strokeC.name(), Theme::hex(Theme::colors().border)));
  }
  if (m_strokeOpacitySpin) {
  m_strokeOpacitySpin->setEnabled(hasStroke);
  const double opacity = app.strokes.empty()
      ? ((sc >> 24) & 0xFF) * 100.0 / 255.0
      : app.strokes[0].opacity * 100.0;
  m_strokeOpacitySpin->setValue(opacity);
  }
 if (m_strokePosCombo) {
 m_strokePosCombo->setEnabled(hasStroke);
 if (hasStroke) {
 m_strokePosCombo->blockSignals(true);
 m_strokePosCombo->setCurrentIndex(static_cast<int>(app.strokes[0].position));
 m_strokePosCombo->blockSignals(false);
 }
 }
 }

 // Shadow
 if (m_shadowCheck) {
 bool hasShadow = !app.shadows.empty() && app.shadows[0].enabled;
 m_shadowCheck->blockSignals(true);
 m_shadowCheck->setChecked(hasShadow);
 m_shadowCheck->blockSignals(false);
 if (m_shadowColorBtn) {
 m_shadowColorBtn->setEnabled(hasShadow);
 uint32_t shc = app.shadows.empty() ? 0xFF000000 : app.shadows[0].color;
  QColor shadowC((shc >> 16) & 0xFF, (shc >> 8) & 0xFF, shc & 0xFF, (shc >> 24) & 0xFF);
  m_shadowColorBtn->setProperty("appearanceColor", shc);
 m_shadowColorBtn->setStyleSheet(
 QStringLiteral("background: %1; border: 1px solid %2;")
 .arg(shadowC.name(), Theme::hex(Theme::colors().border)));
 }
 const ShadowEntry shadow = app.shadows.empty() ? ShadowEntry{}
                                                : app.shadows[0];
 auto setShadowSpin = [hasShadow](ScrubbySpinBox* spin, double value) {
     if (!spin) return;
     spin->setEnabled(hasShadow);
     spin->setValue(value);
 };
 setShadowSpin(m_shadowDistanceSpin, shadow.distance);
 setShadowSpin(m_shadowAngleSpin, shadow.angle);
 setShadowSpin(m_shadowSoftnessSpin, shadow.softness);
 setShadowSpin(m_shadowOpacitySpin, shadow.opacity * 100.0);
 }

 // ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ Layer transform ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬
 if (m_selectedLayer->layerType() == GraphicLayerType::Text) {
 const auto* textLayer = static_cast<TextLayer*>(m_selectedLayer);
 if (m_backgroundCheck) {
     QSignalBlocker blocker(m_backgroundCheck);
     m_backgroundCheck->setChecked(textLayer->backgroundEnabled());
 }
 if (m_backgroundColorBtn) {
     m_backgroundColorBtn->setEnabled(textLayer->backgroundEnabled());
      const QColor color = QColor::fromRgba(textLayer->backgroundColor());
      m_backgroundColorBtn->setProperty("appearanceColor",
                                        textLayer->backgroundColor());
     m_backgroundColorBtn->setStyleSheet(QStringLiteral(
         "background: %1; border: 1px solid %2;")
         .arg(color.name(QColor::HexArgb),
              Theme::hex(Theme::colors().border)));
 }
  if (m_backgroundPaddingSpin) {
     m_backgroundPaddingSpin->setEnabled(textLayer->backgroundEnabled());
     m_backgroundPaddingSpin->setValue(textLayer->backgroundPadding());
  }
  if (m_backgroundOpacitySpin) {
      m_backgroundOpacitySpin->setEnabled(textLayer->backgroundEnabled());
      m_backgroundOpacitySpin->setValue(
          ((textLayer->backgroundColor() >> 24) & 0xFF) * 100.0 / 255.0);
  }
 if (m_maskWithTextCheck) {
     QSignalBlocker blocker(m_maskWithTextCheck);
     m_maskWithTextCheck->setChecked(textLayer->maskWithText());
 }
 }
 const auto& xf = m_selectedLayer->transform();
 if (m_posXSpin) m_posXSpin->setValue(xf.posX.evaluate(0));
 if (m_posYSpin) m_posYSpin->setValue(xf.posY.evaluate(0));
 if (m_anchorXSpin) m_anchorXSpin->setValue(xf.anchorX.evaluate(0));
 if (m_anchorYSpin) m_anchorYSpin->setValue(xf.anchorY.evaluate(0));
 if (m_scaleXSpin) m_scaleXSpin->setValue(xf.scaleX.evaluate(0) * 100.0);
 if (m_scaleYSpin) m_scaleYSpin->setValue(xf.scaleY.evaluate(0) * 100.0);
 if (m_rotationSpin) m_rotationSpin->setValue(xf.rotation.evaluate(0));
 if (m_opacitySpin) m_opacitySpin->setValue(xf.opacity.evaluate(0) * 100.0);

 m_updating = false;
}

// ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬
// Apply text properties to the selected TextLayer
// ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬

void GraphicsEditorPanel::applyTextProperties()
{
 if (!m_selectedLayer || m_updating) return;
 if (m_selectedLayer->layerType() != GraphicLayerType::Text) return;
 auto* tl = static_cast<TextLayer*>(m_selectedLayer);

 // Text content is set directly by the inline QLineEdit in the layer list;
 // no need to read from a separate text edit here.

 // While the monitor owns the live rich-text document, character controls
 // are applied directly to the caret/selection.  Paragraph controls can still
 // update the layer, but must not flatten a mixed character selection.
 if (!m_monitorTextEditing) {
 if (m_fontCombo)
 tl->setFontFamily(m_fontCombo->currentText().toStdString());

 if (m_fontSizeSpin)
 tl->setFontSize(static_cast<float>(m_fontSizeSpin->value()));

 if (m_italicBtn) tl->setItalic(m_italicBtn->isChecked());
 if (m_allCapsBtn) tl->setAllCaps(m_allCapsBtn->isChecked());
 if (m_smallCapsBtn) tl->setSmallCaps(m_smallCapsBtn->isChecked());
 if (m_fontStyleCombo)
 tl->setFontStyleForAll(m_fontStyleCombo->currentText().toStdString());
 if (m_kerningSpin)
 tl->setKerningForAll(static_cast<float>(m_kerningSpin->value()));
 if (m_tabWidthSpin)
 tl->setTabWidthForAll(static_cast<float>(m_tabWidthSpin->value()));
 if (m_tsumeSpin)
 tl->setTsumeForAll(static_cast<float>(m_tsumeSpin->value()));
 tl->setFauxStylesForAll(m_fauxBoldBtn && m_fauxBoldBtn->isChecked(),
                         m_fauxItalicBtn && m_fauxItalicBtn->isChecked());
 tl->setUnderlineForAll(m_underlineBtn && m_underlineBtn->isChecked());
 tl->setScriptForAll(m_superscriptBtn && m_superscriptBtn->isChecked(),
                     m_subscriptBtn && m_subscriptBtn->isChecked());
 tl->setRightToLeft(m_rtlBtn && m_rtlBtn->isChecked());
 }

 // Alignment
 if (m_alignLeftBtn && m_alignLeftBtn->isChecked())
 tl->setAlignment(GTextAlign::Left);
 else if (m_alignCenterBtn && m_alignCenterBtn->isChecked())
 tl->setAlignment(GTextAlign::Center);
 else if (m_alignRightBtn && m_alignRightBtn->isChecked())
 tl->setAlignment(GTextAlign::Right);
 else if (m_alignJustifyBtn && m_alignJustifyBtn->isChecked())
 tl->setAlignment(GTextAlign::Justify);

 // Vertical alignment
 if (m_valignTopBtn && m_valignTopBtn->isChecked())
 tl->setVAlignment(GTextVAlign::Top);
 else if (m_valignMiddleBtn && m_valignMiddleBtn->isChecked())
 tl->setVAlignment(GTextVAlign::Middle);
 else if (m_valignBottomBtn && m_valignBottomBtn->isChecked())
 tl->setVAlignment(GTextVAlign::Bottom);

 // Spacing
 if (!m_monitorTextEditing && m_trackingSpin)
 tl->setTrackingForAll(static_cast<float>(m_trackingSpin->value()));
 if (!m_monitorTextEditing && m_leadingSpin)
 tl->setLeadingForAll(static_cast<float>(m_leadingSpin->value()));
 if (!m_monitorTextEditing && m_baselineShiftSpin)
 tl->setBaselineShiftForAll(static_cast<float>(m_baselineShiftSpin->value()));

 // Paragraph box
 if (m_wrapCheck) tl->setUseParagraphBox(m_wrapCheck->isChecked());
 if (m_wrapWidthSpin) tl->setBoxWidth(static_cast<float>(m_wrapWidthSpin->value()));
 if (m_wrapHeightSpin) tl->setBoxHeight(static_cast<float>(m_wrapHeightSpin->value()));

 m_layerEditDirty = true;
 emit propertyChanged();
}

// ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬
// Apply appearance to the selected layer
// ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬

static uint32_t colorFromButton(QPushButton* btn)
{
 if (!btn) return 0xFF000000;
 const QVariant stored = btn->property("appearanceColor");
 if (stored.isValid()) return stored.toUInt();
 QString ss = btn->styleSheet();
 int bgIdx = ss.indexOf("background:");
 if (bgIdx < 0) return 0xFF000000;
 int start = bgIdx + 11;
 int end = ss.indexOf(';', start);
 QColor c(ss.mid(start, end - start).trimmed());
 if (!c.isValid()) return 0xFF000000;
 return (static_cast<uint32_t>(c.alpha()) << 24) |
 (static_cast<uint32_t>(c.red()) << 16) |
 (static_cast<uint32_t>(c.green()) << 8) |
 static_cast<uint32_t>(c.blue());
}

static uint32_t colorWithOpacity(uint32_t color, ScrubbySpinBox* opacity)
{
 if (!opacity) return color;
 const auto alpha = static_cast<uint32_t>(std::clamp(
     qRound(opacity->value() * 2.55), 0, 255));
 return (color & 0x00FFFFFFu) | (alpha << 24);
}

void GraphicsEditorPanel::applyAppearance()
{
 if (!m_selectedLayer || m_updating) return;

 const bool fillOn = m_fillCheck && m_fillCheck->isChecked();
 const uint32_t fillColor = colorFromButton(m_fillColorBtn);
 const bool strokeOn = m_strokeCheck && m_strokeCheck->isChecked();
 const uint32_t strokeColor = colorWithOpacity(
     colorFromButton(m_strokeColorBtn), m_strokeOpacitySpin);
 const float strokeWidth = m_strokeWidthSpin
     ? static_cast<float>(m_strokeWidthSpin->value()) : 2.0f;
 const StrokePosition strokePosition = m_strokePosCombo
     ? static_cast<StrokePosition>(m_strokePosCombo->currentIndex())
     : StrokePosition::Center;
 const bool shadowOn = m_shadowCheck && m_shadowCheck->isChecked();
 const uint32_t shadowColor = colorFromButton(m_shadowColorBtn);
 const float shadowDistance = m_shadowDistanceSpin
     ? static_cast<float>(m_shadowDistanceSpin->value()) : 4.0f;
 const float shadowAngle = m_shadowAngleSpin
     ? static_cast<float>(m_shadowAngleSpin->value()) : 135.0f;
 const float shadowSoftness = m_shadowSoftnessSpin
     ? static_cast<float>(m_shadowSoftnessSpin->value()) : 4.0f;
 const float shadowOpacity = m_shadowOpacitySpin
     ? static_cast<float>(m_shadowOpacitySpin->value() / 100.0) : 0.6f;
 const bool backgroundOn = m_backgroundCheck && m_backgroundCheck->isChecked();
 const uint32_t backgroundColor = colorWithOpacity(
     colorFromButton(m_backgroundColorBtn), m_backgroundOpacitySpin);
 const float backgroundSize = m_backgroundPaddingSpin
     ? static_cast<float>(m_backgroundPaddingSpin->value()) : 4.0f;

 if (m_monitorTextEditing
     && m_selectedLayer->layerType() == GraphicLayerType::Text) {
     emit inlineFillRequested(fillOn, fillColor);
     emit inlineStrokeRequested(strokeOn, strokeColor, strokeWidth,
                                static_cast<int>(strokePosition));
     emit inlineShadowRequested(shadowOn, shadowColor, shadowDistance,
                                shadowAngle, shadowSoftness, shadowOpacity);
     emit inlineBackgroundRequested(backgroundOn, backgroundColor,
                                    backgroundSize);
     return;
 }

 auto& app = m_selectedLayer->appearance();
 if (app.fills.empty()) app.fills.push_back({fillColor, 1.0f, fillOn});
 else {
     app.fills[0].color = fillColor;
     app.fills[0].enabled = fillOn;
 }
 const float strokeOpacity = m_strokeOpacitySpin
     ? static_cast<float>(m_strokeOpacitySpin->value() / 100.0) : 1.0f;
 if (app.strokes.empty())
     app.strokes.push_back(
         {strokeColor, strokeWidth, strokePosition, strokeOpacity, strokeOn});
 else {
     auto& stroke = app.strokes[0];
     stroke.color = strokeColor;
     stroke.width = strokeWidth;
     stroke.position = strokePosition;
     stroke.opacity = strokeOpacity;
     stroke.enabled = strokeOn;
 }
 if (app.shadows.empty())
     app.shadows.push_back({shadowColor, shadowDistance, shadowAngle,
                            shadowSoftness, shadowOpacity, shadowOn});
 else {
     auto& shadow = app.shadows[0];
     shadow.color = shadowColor;
     shadow.distance = shadowDistance;
     shadow.angle = shadowAngle;
     shadow.softness = shadowSoftness;
     shadow.opacity = shadowOpacity;
     shadow.enabled = shadowOn;
 }

 if (m_selectedLayer->layerType() == GraphicLayerType::Text) {
     auto* text = static_cast<TextLayer*>(m_selectedLayer);
     text->setFillForAll(fillOn, fillColor);
     text->setStrokeForAll(strokeOn, strokeColor, strokeWidth, strokePosition);
     if (!text->appearance().strokes.empty())
         text->appearance().strokes[0].opacity = strokeOpacity;
     text->setShadowForAll(shadowOn, shadowColor, shadowDistance, shadowAngle,
                           shadowSoftness, shadowOpacity);
     text->setBackgroundForAll(backgroundOn, backgroundColor, backgroundSize);
     if (m_maskWithTextCheck)
         text->setMaskWithText(m_maskWithTextCheck->isChecked());
 }

 m_layerEditDirty = true;
 emit propertyChanged();
}

// ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬
// Apply layer transform
// ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬

void GraphicsEditorPanel::applyLayerTransform()
{
 if (!m_selectedLayer || m_updating) return;
 auto& xf = m_selectedLayer->transform();

 // writeValue() changes the default for a static track and only creates a
 // keyframe when animation is already enabled. The old addKeyframe(0, ...)
 // calls silently animated every Graphics-editor adjustment at time zero.
 if (m_posXSpin) xf.posX.writeValue(0, static_cast<float>(m_posXSpin->value()));
 if (m_posYSpin) xf.posY.writeValue(0, static_cast<float>(m_posYSpin->value()));
 if (m_anchorXSpin) xf.anchorX.writeValue(0, static_cast<float>(m_anchorXSpin->value()));
 if (m_anchorYSpin) xf.anchorY.writeValue(0, static_cast<float>(m_anchorYSpin->value()));

 float scaleX = m_scaleXSpin ? static_cast<float>(m_scaleXSpin->value() / 100.0) : xf.scaleX.evaluate(0);
 float scaleY = (m_uniformScaleCheck && m_uniformScaleCheck->isChecked())
 ? scaleX
 : (m_scaleYSpin ? static_cast<float>(m_scaleYSpin->value() / 100.0) : xf.scaleY.evaluate(0));
 xf.scaleX.writeValue(0, scaleX);
 xf.scaleY.writeValue(0, scaleY);

 if (m_rotationSpin) xf.rotation.writeValue(0, static_cast<float>(m_rotationSpin->value()));
 if (m_opacitySpin) xf.opacity.writeValue(0, static_cast<float>(m_opacitySpin->value() / 100.0));

 m_layerEditDirty = true;
 emit propertyChanged();
}

// ── Undo: snapshot on selection, push a restore-state command on commit ──────
void GraphicsEditorPanel::captureEditBaseline()
{
    if (m_selectedLayer) {
        m_editBaseline   = m_selectedLayer->clone();
        m_editBaselineId = m_selectedLayer->layerId();
    } else {
        m_editBaseline.reset();
        m_editBaselineId = 0;
    }
    m_layerEditDirty = false;
}

void GraphicsEditorPanel::commitLayerEdit()
{
    if (!m_layerEditDirty) return;
    m_layerEditDirty = false;

    // If we can't form a real before/after command, just re-baseline.
    if (!m_editBaseline || !m_graphicClip || !m_selectedLayer || !m_commandStack ||
        m_selectedLayer->layerId() != m_editBaselineId) {
        captureEditBaseline();
        return;
    }

    const uint64_t id = m_editBaselineId;
    auto oldState = std::shared_ptr<GraphicLayer>(m_editBaseline->clone().release());
    auto newState = std::shared_ptr<GraphicLayer>(m_selectedLayer->clone().release());

    auto* gc = m_graphicClip;
    // Restore a snapshot IN PLACE onto the layer (found by its stable id), so
    // the layer object + the panel's selection survive undo/redo.
    auto restore = [gc, id, this](const std::shared_ptr<GraphicLayer>& src) {
        GraphicLayer* target = nullptr;
        for (size_t i = 0; i < gc->layerCount(); ++i) {
            auto* l = gc->layer(i);
            if (l && l->layerId() == id) { target = l; break; }
        }
        if (!target) return;
        target->assignStateFrom(*src);
        if (m_selectedLayer && m_selectedLayer->layerId() == id) {
            m_updating = true;
            populateFromLayer();
            m_updating = false;
        }
        emit propertyChanged();
    };

    // The edit was already applied live, so record without re-executing.
    m_commandStack->pushWithoutExecute(std::make_unique<LambdaCommand>(
        "Edit Graphic Layer",
        [restore, newState]() { restore(newState); },
        [restore, oldState]() { restore(oldState); }));

    // The next gesture diffs against the now-current state.
    captureEditBaseline();
}

// ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬
// Copy / Paste layers
// ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬

void GraphicsEditorPanel::copySelectedLayer()
{
 if (!m_selectedLayer) return;
 m_copiedLayer = m_selectedLayer->clone();
 spdlog::info("[EssentialGraphics] Layer copied: {}", m_selectedLayer->name());
}

void GraphicsEditorPanel::pasteLayer()
{
 if (!m_copiedLayer || !m_graphicClip) return;

 auto cloned = m_copiedLayer->clone();
 // Offset position slightly so the paste is visually distinct
 auto& xf = cloned->transform();
 float px = xf.posX.evaluate(0);
 float py = xf.posY.evaluate(0);
 xf.posX.addKeyframe(0, px + 20.0f);
 xf.posY.addKeyframe(0, py + 20.0f);

 auto* gc = m_graphicClip; // capture raw ptr for lambdas
 m_graphicClip->addLayer(std::move(cloned));

 // The new layer is at the end of the stack (highest index).
 size_t newIdx = gc->layerCount() - 1;

 if (m_commandStack) {
 auto cmd = std::make_unique<LambdaCommand>(
 "Paste Layer",
 /*execute (re-do)*/ [gc, this]() {
 auto redoClone = m_copiedLayer->clone();
 auto& rxf = redoClone->transform();
 float rpx = rxf.posX.evaluate(0);
 float rpy = rxf.posY.evaluate(0);
 rxf.posX.addKeyframe(0, rpx + 20.0f);
 rxf.posY.addKeyframe(0, rpy + 20.0f);
 gc->addLayer(std::move(redoClone));
 rebuildLayerList();
 m_layerList->setCurrentRow(0);
 emit propertyChanged();
 },
 /*undo*/ [gc, newIdx, this]() {
 gc->removeLayer(gc->layerCount() - 1);
 rebuildLayerList();
 if (gc->layerCount() > 0)
 m_layerList->setCurrentRow(0);
 emit propertyChanged();
 });
 m_commandStack->pushWithoutExecute(std::move(cmd));
 }

 // Rebuild list and select the new layer
 rebuildLayerList();
 // New layer is at the top of the stack ÃƒÂ¢Ã¢â‚¬Â Ã¢â‚¬â„¢ row 0
 m_layerList->setCurrentRow(0);

 emit propertyChanged();
 spdlog::info("[EssentialGraphics] Layer pasted");
}

void GraphicsEditorPanel::deleteSelectedLayer()
{
 if (!m_selectedLayer || !m_graphicClip) return;
 if (m_graphicClip->layerCount() <= 1) return; // keep at least one

 size_t delIdx = static_cast<size_t>(m_selectedLayerIdx);
 auto* gc = m_graphicClip;

 auto removed = gc->removeLayer(delIdx);
 if (!removed) return;

 if (m_commandStack) {
 auto shared = std::shared_ptr<GraphicLayer>(removed.release());
 auto cmd = std::make_unique<LambdaCommand>(
 "Delete Layer",
 /*execute (re-do)*/ [gc, delIdx, shared, this]() {
 gc->removeLayer(delIdx);
 m_selectedLayer = nullptr;
 m_selectedLayerIdx = -1;
 rebuildLayerList();
 if (gc->layerCount() > 0)
 m_layerList->setCurrentRow(0);
 emit propertyChanged();
 },
 /*undo*/ [gc, delIdx, shared, this]() {
 gc->insertLayer(delIdx, shared->clone());
 rebuildLayerList();
 int listRow = static_cast<int>(gc->layerCount()) - 1
 - static_cast<int>(delIdx);
 m_layerList->setCurrentRow(listRow);
 emit propertyChanged();
 });
 m_commandStack->pushWithoutExecute(std::move(cmd));
 }

 m_selectedLayer = nullptr;
 m_selectedLayerIdx = -1;
 rebuildLayerList();
 if (m_graphicClip->layerCount() > 0)
 m_layerList->setCurrentRow(0);
 emit propertyChanged();
 spdlog::info("[EssentialGraphics] Layer deleted");
}


} // namespace rt
