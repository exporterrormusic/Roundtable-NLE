/*
 * PropertiesPanelGraphic.cpp — Graphic clip property application and UI setup.
 * Split from PropertiesPanelApply.cpp + PropertiesPanelUI.cpp.
 *
 * Contains: applyGfxText(), applyGfxFontFamily(), applyGfxFontSize(),
 *           applyGfxFontWeight(), applyGfxLeading(), applyGfxKerning(),
 *           applyGfxItalic(), applyGfxAllCaps(),
 *           applyGfxAlign(), applyGfxFillColor(), applyGfxStrokeEnabled(),
 *           applyGfxStrokeWidth(), applyGfxStrokeColor(),
 *           applyGfxShadowEnabled(), selectedGraphicTextLayer(), setupGraphicSection().
 */

#include "panels/properties/PropertiesPanel.h"
#include "widgets/ScrubbySpinBox.h"
#include "Theme.h"

#include "timeline/Clip.h"
#include "timeline/GraphicClip.h"
#include "command/CommandStack.h"
#include "command/LambdaCommand.h"

#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QComboBox>
#include <QCompleter>
#include <QCheckBox>
#include <QFontDatabase>
#include <QLineEdit>
#include <QPushButton>
#include <QColorDialog>
#include <QPointer>
#include <QSignalBlocker>
#include <QTimer>

#include <cmath>
#include <memory>

namespace rt {

TextLayer* PropertiesPanel::selectedGraphicTextLayer() const noexcept
{
    if (!m_clip || m_clip->clipType() != ClipType::Graphic) return nullptr;
    auto* gc = static_cast<GraphicClip*>(m_clip);
    if (m_gfxSelectedTextLayerId != 0) {
        if (auto* layer = gc->findLayerById(m_gfxSelectedTextLayerId);
            layer && layer->layerType() == GraphicLayerType::Text) {
            return static_cast<TextLayer*>(layer);
        }
    }
    // Match the Graphics Editor's default: the topmost text object is active.
    for (size_t i = gc->layerCount(); i > 0; --i)
        if (gc->layer(i - 1)->layerType() == GraphicLayerType::Text)
            return static_cast<TextLayer*>(gc->layer(i - 1));
    return nullptr;
}

void PropertiesPanel::rebuildGfxTextObjectCombo()
{
    if (!m_gfxTextObjectCombo) return;
    QSignalBlocker blocker(m_gfxTextObjectCombo);
    m_gfxTextObjectCombo->clear();

    if (m_clip && m_clip->clipType() == ClipType::Graphic) {
        auto* gc = static_cast<GraphicClip*>(m_clip);
        // Show topmost first, matching the Graphics Editor layer list.
        for (size_t i = gc->layerCount(); i > 0; --i) {
            auto* layer = gc->layer(i - 1);
            if (!layer || layer->layerType() != GraphicLayerType::Text)
                continue;
            auto* tl = static_cast<TextLayer*>(layer);
            QString preview = QString::fromStdString(tl->text())
                                  .simplified();
            if (preview.isEmpty()) preview = tr("(empty text)");
            if (preview.size() > 36) preview = preview.left(33) + QStringLiteral("...");
            m_gfxTextObjectCombo->addItem(
                preview, QVariant::fromValue<qulonglong>(tl->layerId()));
        }
    }

    int selectedIndex = -1;
    for (int i = 0; i < m_gfxTextObjectCombo->count(); ++i) {
        if (m_gfxTextObjectCombo->itemData(i).toULongLong()
            == m_gfxSelectedTextLayerId) {
            selectedIndex = i;
            break;
        }
    }
    if (selectedIndex < 0 && m_gfxTextObjectCombo->count() > 0)
        selectedIndex = 0;
    m_gfxTextObjectCombo->setCurrentIndex(selectedIndex);
    m_gfxSelectedTextLayerId = selectedIndex >= 0
        ? m_gfxTextObjectCombo->itemData(selectedIndex).toULongLong() : 0;
    m_gfxTextObjectCombo->setEnabled(selectedIndex >= 0
                                     && !m_monitorTextEditing);
    if (m_gfxDuplicateTextObjectBtn)
        m_gfxDuplicateTextObjectBtn->setEnabled(selectedIndex >= 0
                                                && !m_monitorTextEditing);
}

void PropertiesPanel::selectGraphicTextObject(uint64_t layerId)
{
    if (!m_clip || m_clip->clipType() != ClipType::Graphic) return;
    auto* layer = static_cast<GraphicClip*>(m_clip)->findLayerById(layerId);
    if (!layer || layer->layerType() != GraphicLayerType::Text) return;
    m_gfxSelectedTextLayerId = layerId;
    const bool wasUpdating = m_updating;
    m_updating = true;
    populateFromGraphic();
    m_updating = wasUpdating;
}

void PropertiesPanel::setMonitorTextEditing(bool active) noexcept
{
    m_monitorTextEditing = active;
    if (m_gfxTextEdit) m_gfxTextEdit->setEnabled(!active);
    if (m_gfxTextObjectCombo) m_gfxTextObjectCombo->setEnabled(!active);
    if (m_gfxAddTextObjectBtn) m_gfxAddTextObjectBtn->setEnabled(!active);
    if (m_gfxDuplicateTextObjectBtn)
        m_gfxDuplicateTextObjectBtn->setEnabled(!active
                                                && selectedGraphicTextLayer());
}

void PropertiesPanel::setInlineTextSelectionFormat(
    const QString& family, float pointSize, int weight, bool italic,
    bool allCaps, bool smallCaps, float, float, float leading,
    uint32_t mixedFlags)
{
    if (!m_monitorTextEditing) return;
    const bool oldUpdating = m_updating;
    m_updating = true;
    if (!(mixedFlags & (1u << 0)) && m_gfxFontFamilyCombo)
        m_gfxFontFamilyCombo->setCurrentText(family);
    if (!(mixedFlags & (1u << 1)) && m_gfxFontSizeSpin)
        m_gfxFontSizeSpin->setValue(pointSize);
    if (!(mixedFlags & (1u << 2)) && m_gfxFontWeightSpin)
        m_gfxFontWeightSpin->setValue(weight);
    if (!(mixedFlags & (1u << 7)) && m_gfxLeadingSpin)
        m_gfxLeadingSpin->setValue(leading);
    auto setCheck = [](QCheckBox* check, bool value, bool mixed) {
        if (!check) return;
        QSignalBlocker blocker(check);
        check->setTristate(mixed);
        check->setCheckState(mixed ? Qt::PartiallyChecked
                                   : (value ? Qt::Checked : Qt::Unchecked));
    };
    setCheck(m_gfxItalicCheck, italic, mixedFlags & (1u << 3));
    setCheck(m_gfxAllCapsCheck, allCaps, mixedFlags & (1u << 4));
    setCheck(m_gfxSmallCapsCheck, smallCaps, mixedFlags & (1u << 4));
    m_updating = oldUpdating;
}

void PropertiesPanel::setInlineTextAdvancedFormat(
    const QString&, float kerning, float, float, bool, bool, bool, bool,
    bool, uint32_t mixedFlags)
{
    if (!m_monitorTextEditing) return;
    const bool oldUpdating = m_updating;
    m_updating = true;
    if (!(mixedFlags & (1u << 9)) && m_gfxKerningSpin)
        m_gfxKerningSpin->setValue(kerning);
    m_updating = oldUpdating;
}

void PropertiesPanel::setInlineTextSelectionAppearance(
    bool, uint32_t fillColor, bool strokeEnabled, uint32_t strokeColor,
    float strokeWidth, int, bool shadowEnabled, uint32_t,
    float, float, float, float, bool, uint32_t, float, uint32_t)
{
    if (!m_monitorTextEditing) return;
    const bool oldUpdating = m_updating;
    m_updating = true;
    const QColor fill = QColor::fromRgba(fillColor);
    m_gfxFillColorBtn->setStyleSheet(QStringLiteral(
        "QPushButton { background: %1; border: 1px solid #555; min-width: 40px; min-height: 18px; }")
        .arg(fill.name()));
    m_gfxStrokeCheck->setChecked(strokeEnabled);
    m_gfxStrokeWidthSpin->setValue(strokeWidth);
    const QColor stroke = QColor::fromRgba(strokeColor);
    m_gfxStrokeColorBtn->setStyleSheet(QStringLiteral(
        "QPushButton { background: %1; border: 1px solid #555; }")
        .arg(stroke.name()));
    m_gfxShadowCheck->setChecked(shadowEnabled);
    m_updating = oldUpdating;
}

void PropertiesPanel::setInlineParagraphFormat(int alignment, bool,
                                               uint32_t mixedFlags)
{
    if (!m_monitorTextEditing || (mixedFlags & (1u << 19))) return;
    QSignalBlocker blocker(m_gfxAlignCombo);
    m_gfxAlignCombo->setCurrentIndex(alignment);
}

void PropertiesPanel::addGfxTextObject()
{
    if (m_updating || !canMutateBoundClip()
        || m_clip->clipType() != ClipType::Graphic) return;
    auto* gc = static_cast<GraphicClip*>(m_clip);
    auto layer = std::make_unique<TextLayer>();
    layer->setText("Title");
    layer->setName("Text");
    auto templateLayer = std::shared_ptr<GraphicLayer>(layer->clone().release());
    const size_t index = gc->layerCount();
    auto* added = gc->addLayer(std::move(layer));
    if (!added) return;
    m_gfxSelectedTextLayerId = added->layerId();

    if (m_commandStack) {
        m_commandStack->pushWithoutExecute(std::make_unique<LambdaCommand>(
            "Add Graphic Text Object",
            [this, gc, index, templateLayer]() {
                auto* restored = gc->insertLayer(index, templateLayer->clone());
                m_gfxSelectedTextLayerId = restored ? restored->layerId() : 0;
                populateFromClip();
                emit graphicLayerStackChanged(m_gfxSelectedTextLayerId);
                emit propertyChanged();
            },
            [this, gc, index]() {
                gc->removeLayer(index);
                m_gfxSelectedTextLayerId = 0;
                populateFromClip();
                emit graphicLayerStackChanged(m_gfxSelectedTextLayerId);
                emit propertyChanged();
            }));
    }
    populateFromClip();
    emit graphicLayerStackChanged(m_gfxSelectedTextLayerId);
    emit propertyChanged();
}

void PropertiesPanel::duplicateGfxTextObject()
{
    if (m_updating || !canMutateBoundClip()
        || m_clip->clipType() != ClipType::Graphic) return;
    auto* source = selectedGraphicTextLayer();
    if (!source) return;
    auto* gc = static_cast<GraphicClip*>(m_clip);
    auto duplicate = source->clone();
    const int64_t tick = clipRelativeTick();
    auto& xf = duplicate->transform();
    xf.posX.writeValue(tick, xf.posX.evaluate(tick) + 20.0f);
    xf.posY.writeValue(tick, xf.posY.evaluate(tick) + 20.0f);
    duplicate->setName(source->name() + " Copy");
    auto templateLayer = std::shared_ptr<GraphicLayer>(duplicate->clone().release());
    const size_t index = gc->layerCount();
    auto* added = gc->addLayer(std::move(duplicate));
    if (!added) return;
    m_gfxSelectedTextLayerId = added->layerId();

    if (m_commandStack) {
        m_commandStack->pushWithoutExecute(std::make_unique<LambdaCommand>(
            "Duplicate Graphic Text Object",
            [this, gc, index, templateLayer]() {
                auto* restored = gc->insertLayer(index, templateLayer->clone());
                m_gfxSelectedTextLayerId = restored ? restored->layerId() : 0;
                populateFromClip();
                emit graphicLayerStackChanged(m_gfxSelectedTextLayerId);
                emit propertyChanged();
            },
            [this, gc, index]() {
                gc->removeLayer(index);
                m_gfxSelectedTextLayerId = 0;
                populateFromClip();
                emit graphicLayerStackChanged(m_gfxSelectedTextLayerId);
                emit propertyChanged();
            }));
    }
    populateFromClip();
    emit graphicLayerStackChanged(m_gfxSelectedTextLayerId);
    emit propertyChanged();
}

void PropertiesPanel::focusGraphicTextField()
{
    if (!m_gfxTextEdit) return;
    if (!m_clip || m_clip->clipType() != ClipType::Graphic) return;
    // Defer to the next event-loop tick: the caller typically just made
    // the Properties dock visible, and setFocus() is ignored on a widget
    // that is not yet shown. By then the dock is realized.
    QPointer<QLineEdit> edit(m_gfxTextEdit);
    QTimer::singleShot(0, edit, [edit]() {
        if (!edit) return;
        edit->setFocus(Qt::OtherFocusReason);
        edit->selectAll();
    });
}

void PropertiesPanel::applyGfxText()
{
    if (m_updating || !canMutateBoundClip() || m_clip->clipType() != ClipType::Graphic) return;
    if (m_monitorTextEditing) return;
    auto* tl = selectedGraphicTextLayer();
    if (!tl) return;
    std::string val = m_gfxTextEdit->text().toStdString();
    if (val == tl->text()) return;
    tl->replaceTextPreservingStyles(val);
    rebuildGfxTextObjectCombo();
    emit propertyChanged();
}

void PropertiesPanel::applyGfxFontFamily()
{
    if (m_updating || !canMutateBoundClip() || m_clip->clipType() != ClipType::Graphic) return;
    auto* tl = selectedGraphicTextLayer();
    if (!tl) return;
    const QString requested = m_gfxFontFamilyCombo->currentText().trimmed();
    const int index = m_gfxFontFamilyCombo->findText(
        requested, Qt::MatchFixedString);
    if (index < 0) return;
    const QString family = m_gfxFontFamilyCombo->itemText(index);
    std::string val = family.toStdString();
    m_gfxFontFamilyCombo->setCurrentIndex(index);
    if (m_monitorTextEditing) {
        emit inlineFontFamilyRequested(family);
        return;
    }
    if (val == tl->fontFamily()) return;
    tl->setFontFamily(val);
    emit propertyChanged();
}

void PropertiesPanel::applyGfxFontSize()
{
    if (m_updating || !canMutateBoundClip() || m_clip->clipType() != ClipType::Graphic) return;
    auto* tl = selectedGraphicTextLayer();
    if (!tl) return;
    float val = static_cast<float>(m_gfxFontSizeSpin->value());
    if (m_monitorTextEditing) {
        emit inlineFontSizeRequested(val);
        return;
    }
    if (std::abs(val - tl->fontSize()) < 0.01f) return;
    tl->setFontSize(val);
    emit propertyChanged();
}

void PropertiesPanel::applyGfxFontWeight()
{
    if (m_updating || !canMutateBoundClip() || m_clip->clipType() != ClipType::Graphic) return;
    auto* tl = selectedGraphicTextLayer();
    if (!tl) return;
    int val = static_cast<int>(m_gfxFontWeightSpin->value());
    if (m_monitorTextEditing) {
        emit inlineFontWeightRequested(val);
        return;
    }
    if (val == tl->fontWeight()) return;
    tl->setFontWeight(val);
    emit propertyChanged();
}

void PropertiesPanel::applyGfxLeading()
{
    if (m_updating || !canMutateBoundClip()
        || m_clip->clipType() != ClipType::Graphic) return;
    auto* tl = selectedGraphicTextLayer();
    if (!tl) return;
    const float value = static_cast<float>(m_gfxLeadingSpin->value());
    if (m_monitorTextEditing) {
        emit inlineLeadingRequested(value);
        return;
    }
    if (std::abs(value - tl->leading().evaluate(0)) < 0.01f) return;
    tl->setLeadingForAll(value);
    emit propertyChanged();
}

void PropertiesPanel::applyGfxKerning()
{
    if (m_updating || !canMutateBoundClip()
        || m_clip->clipType() != ClipType::Graphic) return;
    auto* tl = selectedGraphicTextLayer();
    if (!tl) return;
    const float value = static_cast<float>(m_gfxKerningSpin->value());
    if (m_monitorTextEditing) {
        emit inlineKerningRequested(value);
        return;
    }
    if (std::abs(value - tl->kerning()) < 0.01f) return;
    tl->setKerningForAll(value);
    emit propertyChanged();
}

void PropertiesPanel::applyGfxItalic()
{
    if (m_updating || !canMutateBoundClip() || m_clip->clipType() != ClipType::Graphic) return;
    auto* tl = selectedGraphicTextLayer();
    if (!tl) return;
    if (m_monitorTextEditing) {
        emit inlineItalicRequested(m_gfxItalicCheck->isChecked());
        return;
    }
    tl->setItalic(m_gfxItalicCheck->isChecked());
    emit propertyChanged();
}

void PropertiesPanel::applyGfxAllCaps()
{
    if (m_updating || !canMutateBoundClip() || m_clip->clipType() != ClipType::Graphic) return;
    auto* tl = selectedGraphicTextLayer();
    if (!tl) return;
    if (m_monitorTextEditing) {
        emit inlineCapitalizationRequested(m_gfxAllCapsCheck->isChecked(),
                                           m_gfxSmallCapsCheck->isChecked());
        return;
    }
    tl->setAllCaps(m_gfxAllCapsCheck->isChecked());
    emit propertyChanged();
}

void PropertiesPanel::applyGfxSmallCaps()
{
    if (m_updating || !canMutateBoundClip() || m_clip->clipType() != ClipType::Graphic) return;
    auto* tl = selectedGraphicTextLayer();
    if (!tl) return;
    if (m_monitorTextEditing) {
        emit inlineCapitalizationRequested(m_gfxAllCapsCheck->isChecked(),
                                           m_gfxSmallCapsCheck->isChecked());
        return;
    }
    tl->setSmallCaps(m_gfxSmallCapsCheck->isChecked());
    emit propertyChanged();
}

void PropertiesPanel::applyGfxAlign()
{
    if (m_updating || !canMutateBoundClip() || m_clip->clipType() != ClipType::Graphic) return;
    auto* tl = selectedGraphicTextLayer();
    if (!tl) return;
    auto val = static_cast<GTextAlign>(m_gfxAlignCombo->currentIndex());
    if (m_monitorTextEditing) {
        emit inlineParagraphAlignmentRequested(static_cast<int>(val));
        return;
    }
    if (val == tl->alignment()) return;
    tl->setAlignment(val);
    emit propertyChanged();
}

void PropertiesPanel::applyGfxFillColor()
{
    if (m_updating || !canMutateBoundClip() || m_clip->clipType() != ClipType::Graphic) return;
    auto* tl = selectedGraphicTextLayer();
    if (!tl) return;
    uint32_t current = 0xFFFFFFFF;
    if (!tl->appearance().fills.empty())
        current = tl->appearance().fills[0].color;
    QColor curCol(static_cast<int>((current>>16)&0xFF),
                  static_cast<int>((current>>8)&0xFF),
                  static_cast<int>(current&0xFF),
                  static_cast<int>((current>>24)&0xFF));
    QColor chosen = QColorDialog::getColor(curCol, this, "Fill Color",
        QColorDialog::ShowAlphaChannel);
    if (!chosen.isValid()) return;
    uint32_t packed = (static_cast<uint32_t>(chosen.alpha()) << 24)
                    | (static_cast<uint32_t>(chosen.red())   << 16)
                    | (static_cast<uint32_t>(chosen.green()) << 8)
                    |  static_cast<uint32_t>(chosen.blue());
    if (m_monitorTextEditing) {
        emit inlineFillRequested(true, packed);
    } else if (tl->appearance().fills.empty())
        tl->appearance().fills.push_back({packed, true});
    else {
        tl->appearance().fills[0].color = packed;
        tl->appearance().fills[0].enabled = true;
    }
    m_gfxFillColorBtn->setStyleSheet(
        QStringLiteral("QPushButton { background: %1; border: 1px solid #555; min-width: 40px; min-height: 18px; }")
        .arg(chosen.name()));
    if (!m_monitorTextEditing) emit propertyChanged();
}

void PropertiesPanel::applyGfxStrokeEnabled()
{
    if (m_updating || !canMutateBoundClip() || m_clip->clipType() != ClipType::Graphic) return;
    auto* tl = selectedGraphicTextLayer();
    if (!tl) return;
    bool enabled = m_gfxStrokeCheck->isChecked();
    const auto stroke = tl->appearance().strokes.empty()
        ? StrokeEntry{0xFF000000, 2.0f, StrokePosition::Outer, 1.0f, enabled}
        : tl->appearance().strokes[0];
    if (m_monitorTextEditing) {
        emit inlineStrokeRequested(enabled, stroke.color, stroke.width,
                                   static_cast<int>(stroke.position));
        return;
    }
    if (tl->appearance().strokes.empty()) {
        if (enabled)
            tl->appearance().strokes.push_back(
                {0xFF000000, 2.0f, StrokePosition::Outer, 1.0f, true});
    } else {
        tl->appearance().strokes[0].enabled = enabled;
    }
    emit propertyChanged();
}

void PropertiesPanel::applyGfxStrokeWidth()
{
    if (m_updating || !canMutateBoundClip() || m_clip->clipType() != ClipType::Graphic) return;
    auto* tl = selectedGraphicTextLayer();
    if (!tl) return;
    const float width = static_cast<float>(m_gfxStrokeWidthSpin->value());
    if (m_monitorTextEditing) {
        const auto stroke = tl->appearance().strokes.empty()
            ? StrokeEntry{0xFF000000, width, StrokePosition::Outer, 1.0f, true}
            : tl->appearance().strokes[0];
        emit inlineStrokeRequested(stroke.enabled, stroke.color, width,
                                   static_cast<int>(stroke.position));
        return;
    }
    if (tl->appearance().strokes.empty()) return;
    tl->appearance().strokes[0].width = width;
    emit propertyChanged();
}

void PropertiesPanel::applyGfxStrokeColor()
{
    if (m_updating || !canMutateBoundClip() || m_clip->clipType() != ClipType::Graphic) return;
    auto* tl = selectedGraphicTextLayer();
    if (!tl) return;
    uint32_t current = 0xFF000000;
    if (!tl->appearance().strokes.empty())
        current = tl->appearance().strokes[0].color;
    QColor curCol(static_cast<int>((current>>16)&0xFF),
                  static_cast<int>((current>>8)&0xFF),
                  static_cast<int>(current&0xFF));
    QColor chosen = QColorDialog::getColor(curCol, this, "Stroke Color");
    if (!chosen.isValid()) return;
    uint32_t packed = 0xFF000000
                    | (static_cast<uint32_t>(chosen.red()) << 16)
                    | (static_cast<uint32_t>(chosen.green()) << 8)
                    |  static_cast<uint32_t>(chosen.blue());
    if (m_monitorTextEditing) {
        const auto stroke = tl->appearance().strokes.empty()
            ? StrokeEntry{packed, 2.0f, StrokePosition::Outer, 1.0f, true}
            : tl->appearance().strokes[0];
        emit inlineStrokeRequested(stroke.enabled, packed, stroke.width,
                                   static_cast<int>(stroke.position));
    } else if (tl->appearance().strokes.empty())
        tl->appearance().strokes.push_back(
            {packed, 2.0f, StrokePosition::Outer, 1.0f, true});
    else
        tl->appearance().strokes[0].color = packed;
    m_gfxStrokeColorBtn->setStyleSheet(
        QStringLiteral("QPushButton { background: %1; border: 1px solid #555; }")
        .arg(chosen.name()));
    if (!m_monitorTextEditing) emit propertyChanged();
}

void PropertiesPanel::applyGfxShadowEnabled()
{
    if (m_updating || !canMutateBoundClip() || m_clip->clipType() != ClipType::Graphic) return;
    auto* tl = selectedGraphicTextLayer();
    if (!tl) return;
    bool enabled = m_gfxShadowCheck->isChecked();
    const auto shadow = tl->appearance().shadows.empty()
        ? ShadowEntry{0x80000000, 4.0f, 135.0f, 4.0f, 0.6f, enabled}
        : tl->appearance().shadows[0];
    if (m_monitorTextEditing) {
        emit inlineShadowRequested(enabled, shadow.color, shadow.distance,
                                   shadow.angle, shadow.softness,
                                   shadow.opacity);
        return;
    }
    if (tl->appearance().shadows.empty()) {
        if (enabled)
            tl->appearance().shadows.push_back({0x80000000, 4.0f, 135.0f, 4.0f, 0.6f, true});
    } else {
        tl->appearance().shadows[0].enabled = enabled;
    }
    emit propertyChanged();
}

void PropertiesPanel::setupGraphicSection(QWidget* container)
{
    const auto& m = Theme::metrics();
    m_graphicSection = new QGroupBox("Graphic", container);
    auto* form = new QFormLayout(m_graphicSection);
    form->setContentsMargins(m.spacingXs, 18, m.spacingXs, m.spacingXs);
    form->setSpacing(m.spacingSm);

    m_gfxTextObjectCombo = new QComboBox(m_graphicSection);
    m_gfxTextObjectCombo->setObjectName(
        QStringLiteral("propertiesGraphicTextObjectCombo"));
    m_gfxTextObjectCombo->setToolTip(tr(
        "Choose the text object to edit inside this graphic clip"));
    connect(m_gfxTextObjectCombo, &QComboBox::currentIndexChanged,
            this, [this](int index) {
        if (m_updating || index < 0) return;
        const uint64_t id = m_gfxTextObjectCombo->itemData(index).toULongLong();
        if (!id) return;
        m_gfxSelectedTextLayerId = id;
        const bool oldUpdating = m_updating;
        m_updating = true;
        populateFromGraphic();
        m_updating = oldUpdating;
        emit graphicTextObjectSelected(id);
    });
    form->addRow(tr("Text object:"), m_gfxTextObjectCombo);

    auto* objectButtons = new QHBoxLayout;
    m_gfxAddTextObjectBtn = new QPushButton(tr("+ Text"), m_graphicSection);
    m_gfxAddTextObjectBtn->setObjectName(
        QStringLiteral("propertiesGraphicAddTextButton"));
    m_gfxAddTextObjectBtn->setToolTip(tr(
        "Add another independent text object to this graphic clip"));
    connect(m_gfxAddTextObjectBtn, &QPushButton::clicked,
            this, &PropertiesPanel::addGfxTextObject);
    objectButtons->addWidget(m_gfxAddTextObjectBtn);
    m_gfxDuplicateTextObjectBtn = new QPushButton(
        tr("Duplicate"), m_graphicSection);
    m_gfxDuplicateTextObjectBtn->setObjectName(
        QStringLiteral("propertiesGraphicDuplicateTextButton"));
    m_gfxDuplicateTextObjectBtn->setToolTip(tr(
        "Duplicate the active text object inside this graphic clip"));
    connect(m_gfxDuplicateTextObjectBtn, &QPushButton::clicked,
            this, &PropertiesPanel::duplicateGfxTextObject);
    objectButtons->addWidget(m_gfxDuplicateTextObjectBtn);
    objectButtons->addStretch();
    form->addRow(QString(), objectButtons);

    m_gfxTextEdit = new QLineEdit(m_graphicSection);
    m_gfxTextEdit->setObjectName(QStringLiteral("propertiesGraphicTextEdit"));
    m_gfxTextEdit->setToolTip(tr("Graphic overlay text"));
    m_gfxTextEdit->setPlaceholderText("Enter text...");
    connect(m_gfxTextEdit, &QLineEdit::editingFinished, this, &PropertiesPanel::applyGfxText);
    form->addRow("Text:", m_gfxTextEdit);

    m_gfxFontFamilyCombo = new QComboBox(m_graphicSection);
    m_gfxFontFamilyCombo->setObjectName(
        QStringLiteral("propertiesGraphicFontFamilyCombo"));
    m_gfxFontFamilyCombo->setToolTip(tr(
        "Choose an installed font family, or type to search"));
    m_gfxFontFamilyCombo->addItems(QFontDatabase::families());
    m_gfxFontFamilyCombo->setEditable(true);
    m_gfxFontFamilyCombo->setInsertPolicy(QComboBox::NoInsert);
    m_gfxFontFamilyCombo->setMaxVisibleItems(20);
    auto* fontCompleter = new QCompleter(
        m_gfxFontFamilyCombo->model(), m_gfxFontFamilyCombo);
    fontCompleter->setCaseSensitivity(Qt::CaseInsensitive);
    fontCompleter->setFilterMode(Qt::MatchContains);
    fontCompleter->setCompletionMode(QCompleter::PopupCompletion);
    m_gfxFontFamilyCombo->setCompleter(fontCompleter);
    m_gfxFontFamilyCombo->lineEdit()->setClearButtonEnabled(true);
    m_gfxFontFamilyCombo->lineEdit()->setPlaceholderText(
        tr("Type to find a font"));
    connect(m_gfxFontFamilyCombo, &QComboBox::textActivated,
            this, [this](const QString&) { applyGfxFontFamily(); });
    connect(m_gfxFontFamilyCombo->lineEdit(), &QLineEdit::editingFinished,
            this, &PropertiesPanel::applyGfxFontFamily);
    form->addRow("Font:", m_gfxFontFamilyCombo);

    m_gfxFontSizeSpin = createScrubby(1.0, 1000.0, 1.0, 1, " pt");
    m_gfxFontSizeSpin->setObjectName(
        QStringLiteral("propertiesGraphicFontSizeSpin"));
    m_gfxFontSizeSpin->setToolTip(tr("Font size for graphic text"));
    connect(m_gfxFontSizeSpin, &ScrubbySpinBox::valueCommitted, this, [this](double, double) { applyGfxFontSize(); });
    // Live preview during the drag, mirroring transform/effect spinboxes.
    connect(m_gfxFontSizeSpin, &ScrubbySpinBox::valueScrubbed, this, [this](double) { applyGfxFontSize(); });
    connect(m_gfxFontSizeSpin, &QDoubleSpinBox::editingFinished, this, &PropertiesPanel::applyGfxFontSize);
    form->addRow("Size:", m_gfxFontSizeSpin);

    m_gfxFontWeightSpin = createScrubby(100.0, 900.0, 100.0, 0, "");
    m_gfxFontWeightSpin->setObjectName(
        QStringLiteral("propertiesGraphicFontWeightSpin"));
    m_gfxFontWeightSpin->setToolTip(tr("Font weight (100 = thin, 400 = normal, 700 = bold, 900 = heavy)"));
    connect(m_gfxFontWeightSpin, &ScrubbySpinBox::valueCommitted, this, [this](double, double) { applyGfxFontWeight(); });
    connect(m_gfxFontWeightSpin, &ScrubbySpinBox::valueScrubbed, this, [this](double) { applyGfxFontWeight(); });
    connect(m_gfxFontWeightSpin, &QDoubleSpinBox::editingFinished, this, &PropertiesPanel::applyGfxFontWeight);
    form->addRow("Weight:", m_gfxFontWeightSpin);

    m_gfxLeadingSpin = createScrubby(0.0, 500.0, 0.5, 1, " px");
    m_gfxLeadingSpin->setObjectName(
        QStringLiteral("propertiesGraphicLeadingSpin"));
    m_gfxLeadingSpin->setToolTip(tr(
        "Leading: extra vertical space between lines of text"));
    connect(m_gfxLeadingSpin, &ScrubbySpinBox::valueCommitted,
            this, [this](double, double) { applyGfxLeading(); });
    connect(m_gfxLeadingSpin, &ScrubbySpinBox::valueScrubbed,
            this, [this](double) { applyGfxLeading(); });
    connect(m_gfxLeadingSpin, &QDoubleSpinBox::editingFinished,
            this, &PropertiesPanel::applyGfxLeading);
    form->addRow(tr("Line spacing:"), m_gfxLeadingSpin);

    m_gfxKerningSpin = createScrubby(-500.0, 500.0, 1.0, 0, "");
    m_gfxKerningSpin->setObjectName(
        QStringLiteral("propertiesGraphicKerningSpin"));
    m_gfxKerningSpin->setToolTip(tr(
        "Kerning: spacing adjustment between the selected characters"));
    connect(m_gfxKerningSpin, &ScrubbySpinBox::valueCommitted,
            this, [this](double, double) { applyGfxKerning(); });
    connect(m_gfxKerningSpin, &ScrubbySpinBox::valueScrubbed,
            this, [this](double) { applyGfxKerning(); });
    connect(m_gfxKerningSpin, &QDoubleSpinBox::editingFinished,
            this, &PropertiesPanel::applyGfxKerning);
    form->addRow(tr("Kerning:"), m_gfxKerningSpin);

    auto* styleRow = new QHBoxLayout;
    m_gfxItalicCheck = new QCheckBox("Italic", m_graphicSection);
    m_gfxItalicCheck->setToolTip(tr("Toggle italic for graphic text"));
    connect(m_gfxItalicCheck, &QCheckBox::toggled, this, &PropertiesPanel::applyGfxItalic);
    styleRow->addWidget(m_gfxItalicCheck);
    m_gfxAllCapsCheck = new QCheckBox("All Caps", m_graphicSection);
    m_gfxAllCapsCheck->setToolTip(tr("Convert graphic text to uppercase"));
    connect(m_gfxAllCapsCheck, &QCheckBox::toggled, this, &PropertiesPanel::applyGfxAllCaps);
    styleRow->addWidget(m_gfxAllCapsCheck);
    m_gfxSmallCapsCheck = new QCheckBox("Small Caps", m_graphicSection);
    m_gfxSmallCapsCheck->setToolTip(tr("Render lowercase letters as small capitals (overridden by All Caps)"));
    connect(m_gfxSmallCapsCheck, &QCheckBox::toggled, this, &PropertiesPanel::applyGfxSmallCaps);
    styleRow->addWidget(m_gfxSmallCapsCheck);
    styleRow->addStretch();
    form->addRow(styleRow);

    m_gfxAlignCombo = new QComboBox(m_graphicSection);
    m_gfxAlignCombo->setToolTip(tr("Graphic text alignment"));
    m_gfxAlignCombo->addItems({"Left", "Center", "Right", "Justify"});
    connect(m_gfxAlignCombo, &QComboBox::currentIndexChanged, this, [this](int) { applyGfxAlign(); });
    form->addRow("Align:", m_gfxAlignCombo);

    m_gfxFillColorBtn = new QPushButton(m_graphicSection);
    m_gfxFillColorBtn->setToolTip(tr("Pick the fill color for graphic text"));
    m_gfxFillColorBtn->setFixedSize(50, 22);
    m_gfxFillColorBtn->setStyleSheet("QPushButton { background: #ffffff; border: 1px solid #555; min-width: 40px; min-height: 18px; }");
    connect(m_gfxFillColorBtn, &QPushButton::clicked, this, &PropertiesPanel::applyGfxFillColor);
    form->addRow("Fill:", m_gfxFillColorBtn);

    auto* strokeRow = new QHBoxLayout;
    m_gfxStrokeCheck = new QCheckBox("Stroke", m_graphicSection);
    m_gfxStrokeCheck->setToolTip(tr("Enable a stroke outline on graphic text"));
    connect(m_gfxStrokeCheck, &QCheckBox::toggled, this, &PropertiesPanel::applyGfxStrokeEnabled);
    strokeRow->addWidget(m_gfxStrokeCheck);
    m_gfxStrokeWidthSpin = createScrubby(0.0, 50.0, 0.5, 1, " px");
    m_gfxStrokeWidthSpin->setToolTip(tr("Stroke outline width in pixels"));
    m_gfxStrokeWidthSpin->setFixedWidth(70);
    connect(m_gfxStrokeWidthSpin, &ScrubbySpinBox::valueCommitted, this, [this](double, double) { applyGfxStrokeWidth(); });
    connect(m_gfxStrokeWidthSpin, &ScrubbySpinBox::valueScrubbed, this, [this](double) { applyGfxStrokeWidth(); });
    connect(m_gfxStrokeWidthSpin, &QDoubleSpinBox::editingFinished, this, &PropertiesPanel::applyGfxStrokeWidth);
    strokeRow->addWidget(m_gfxStrokeWidthSpin);
    m_gfxStrokeColorBtn = new QPushButton(m_graphicSection);
    m_gfxStrokeColorBtn->setToolTip(tr("Pick the stroke outline color"));
    m_gfxStrokeColorBtn->setFixedSize(30, 22);
    m_gfxStrokeColorBtn->setStyleSheet("QPushButton { background: #000000; border: 1px solid #555; }");
    connect(m_gfxStrokeColorBtn, &QPushButton::clicked, this, &PropertiesPanel::applyGfxStrokeColor);
    strokeRow->addWidget(m_gfxStrokeColorBtn);
    strokeRow->addStretch();
    form->addRow(strokeRow);

    m_gfxShadowCheck = new QCheckBox("Drop Shadow", m_graphicSection);
    m_gfxShadowCheck->setToolTip(tr("Enable a drop shadow on graphic text"));
    connect(m_gfxShadowCheck, &QCheckBox::toggled, this, &PropertiesPanel::applyGfxShadowEnabled);
    form->addRow(m_gfxShadowCheck);

    // Shared text appearance presets.
    m_gfxPresetCombo = new QComboBox(m_graphicSection);
    m_gfxPresetCombo->setToolTip(tr("Apply a saved text appearance preset"));
    connect(m_gfxPresetCombo, &QComboBox::activated,
            this, [this](int idx) { applyTextPreset(idx); });
    form->addRow("Preset:", m_gfxPresetCombo);

    m_gfxSavePresetBtn = new QPushButton(tr("Save Preset\xE2\x80\xA6"), m_graphicSection);
    m_gfxSavePresetBtn->setToolTip(tr("Save the current appearance as a reusable text preset"));
    connect(m_gfxSavePresetBtn, &QPushButton::clicked,
            this, &PropertiesPanel::saveTextPresetAs);
    form->addRow("", m_gfxSavePresetBtn);

    container->layout()->addWidget(m_graphicSection);
}

} // namespace rt
