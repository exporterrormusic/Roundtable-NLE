/*
 * PropertiesPanelGraphic.cpp — Graphic clip property application and UI setup.
 * Split from PropertiesPanelApply.cpp + PropertiesPanelUI.cpp.
 *
 * Contains: applyGfxText(), applyGfxFontFamily(), applyGfxFontSize(),
 *           applyGfxFontWeight(), applyGfxItalic(), applyGfxAllCaps(),
 *           applyGfxAlign(), applyGfxFillColor(), applyGfxStrokeEnabled(),
 *           applyGfxStrokeWidth(), applyGfxStrokeColor(),
 *           applyGfxShadowEnabled(), firstTextLayer(), setupGraphicSection().
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
#include <QCheckBox>
#include <QLineEdit>
#include <QPushButton>
#include <QColorDialog>
#include <QPointer>
#include <QTimer>

#include <cmath>

namespace rt {

static TextLayer* firstTextLayer(GraphicClip* gc)
{
    for (size_t i = 0; i < gc->layerCount(); ++i)
        if (gc->layer(i)->layerType() == GraphicLayerType::Text)
            return static_cast<TextLayer*>(gc->layer(i));
    return nullptr;
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
    if (m_updating || !m_clip || m_clip->clipType() != ClipType::Graphic) return;
    auto* tl = firstTextLayer(static_cast<GraphicClip*>(m_clip));
    if (!tl) return;
    std::string val = m_gfxTextEdit->text().toStdString();
    if (val == tl->text()) return;
    tl->setText(val);
    emit propertyChanged();
}

void PropertiesPanel::applyGfxFontFamily()
{
    if (m_updating || !m_clip || m_clip->clipType() != ClipType::Graphic) return;
    auto* tl = firstTextLayer(static_cast<GraphicClip*>(m_clip));
    if (!tl) return;
    std::string val = m_gfxFontFamilyEdit->text().toStdString();
    if (val == tl->fontFamily()) return;
    tl->setFontFamily(val);
    emit propertyChanged();
}

void PropertiesPanel::applyGfxFontSize()
{
    if (m_updating || !m_clip || m_clip->clipType() != ClipType::Graphic) return;
    auto* tl = firstTextLayer(static_cast<GraphicClip*>(m_clip));
    if (!tl) return;
    float val = static_cast<float>(m_gfxFontSizeSpin->value());
    if (std::abs(val - tl->fontSize()) < 0.01f) return;
    tl->setFontSize(val);
    emit propertyChanged();
}

void PropertiesPanel::applyGfxFontWeight()
{
    if (m_updating || !m_clip || m_clip->clipType() != ClipType::Graphic) return;
    auto* tl = firstTextLayer(static_cast<GraphicClip*>(m_clip));
    if (!tl) return;
    int val = static_cast<int>(m_gfxFontWeightSpin->value());
    if (val == tl->fontWeight()) return;
    tl->setFontWeight(val);
    emit propertyChanged();
}

void PropertiesPanel::applyGfxItalic()
{
    if (m_updating || !m_clip || m_clip->clipType() != ClipType::Graphic) return;
    auto* tl = firstTextLayer(static_cast<GraphicClip*>(m_clip));
    if (!tl) return;
    tl->setItalic(m_gfxItalicCheck->isChecked());
    emit propertyChanged();
}

void PropertiesPanel::applyGfxAllCaps()
{
    if (m_updating || !m_clip || m_clip->clipType() != ClipType::Graphic) return;
    auto* tl = firstTextLayer(static_cast<GraphicClip*>(m_clip));
    if (!tl) return;
    tl->setAllCaps(m_gfxAllCapsCheck->isChecked());
    emit propertyChanged();
}

void PropertiesPanel::applyGfxSmallCaps()
{
    if (m_updating || !m_clip || m_clip->clipType() != ClipType::Graphic) return;
    auto* tl = firstTextLayer(static_cast<GraphicClip*>(m_clip));
    if (!tl) return;
    tl->setSmallCaps(m_gfxSmallCapsCheck->isChecked());
    emit propertyChanged();
}

void PropertiesPanel::applyGfxAlign()
{
    if (m_updating || !m_clip || m_clip->clipType() != ClipType::Graphic) return;
    auto* tl = firstTextLayer(static_cast<GraphicClip*>(m_clip));
    if (!tl) return;
    auto val = static_cast<GTextAlign>(m_gfxAlignCombo->currentIndex());
    if (val == tl->alignment()) return;
    tl->setAlignment(val);
    emit propertyChanged();
}

void PropertiesPanel::applyGfxFillColor()
{
    if (m_updating || !m_clip || m_clip->clipType() != ClipType::Graphic) return;
    auto* tl = firstTextLayer(static_cast<GraphicClip*>(m_clip));
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
    if (tl->appearance().fills.empty())
        tl->appearance().fills.push_back({packed, true});
    else {
        tl->appearance().fills[0].color = packed;
        tl->appearance().fills[0].enabled = true;
    }
    m_gfxFillColorBtn->setStyleSheet(
        QStringLiteral("QPushButton { background: %1; border: 1px solid #555; min-width: 40px; min-height: 18px; }")
        .arg(chosen.name()));
    emit propertyChanged();
}

void PropertiesPanel::applyGfxStrokeEnabled()
{
    if (m_updating || !m_clip || m_clip->clipType() != ClipType::Graphic) return;
    auto* tl = firstTextLayer(static_cast<GraphicClip*>(m_clip));
    if (!tl) return;
    bool enabled = m_gfxStrokeCheck->isChecked();
    if (tl->appearance().strokes.empty()) {
        if (enabled)
            tl->appearance().strokes.push_back({0xFF000000, 2.0f, StrokePosition::Outer, true});
    } else {
        tl->appearance().strokes[0].enabled = enabled;
    }
    emit propertyChanged();
}

void PropertiesPanel::applyGfxStrokeWidth()
{
    if (m_updating || !m_clip || m_clip->clipType() != ClipType::Graphic) return;
    auto* tl = firstTextLayer(static_cast<GraphicClip*>(m_clip));
    if (!tl || tl->appearance().strokes.empty()) return;
    tl->appearance().strokes[0].width = static_cast<float>(m_gfxStrokeWidthSpin->value());
    emit propertyChanged();
}

void PropertiesPanel::applyGfxStrokeColor()
{
    if (m_updating || !m_clip || m_clip->clipType() != ClipType::Graphic) return;
    auto* tl = firstTextLayer(static_cast<GraphicClip*>(m_clip));
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
    if (tl->appearance().strokes.empty())
        tl->appearance().strokes.push_back({packed, 2.0f, StrokePosition::Outer, true});
    else
        tl->appearance().strokes[0].color = packed;
    m_gfxStrokeColorBtn->setStyleSheet(
        QStringLiteral("QPushButton { background: %1; border: 1px solid #555; }")
        .arg(chosen.name()));
    emit propertyChanged();
}

void PropertiesPanel::applyGfxShadowEnabled()
{
    if (m_updating || !m_clip || m_clip->clipType() != ClipType::Graphic) return;
    auto* tl = firstTextLayer(static_cast<GraphicClip*>(m_clip));
    if (!tl) return;
    bool enabled = m_gfxShadowCheck->isChecked();
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

    m_gfxTextEdit = new QLineEdit(m_graphicSection);
    m_gfxTextEdit->setToolTip(tr("Graphic overlay text"));
    m_gfxTextEdit->setPlaceholderText("Enter text...");
    connect(m_gfxTextEdit, &QLineEdit::editingFinished, this, &PropertiesPanel::applyGfxText);
    form->addRow("Text:", m_gfxTextEdit);

    m_gfxFontFamilyEdit = new QLineEdit(m_graphicSection);
    m_gfxFontFamilyEdit->setToolTip(tr("Font family for graphic text"));
    connect(m_gfxFontFamilyEdit, &QLineEdit::editingFinished, this, &PropertiesPanel::applyGfxFontFamily);
    form->addRow("Font:", m_gfxFontFamilyEdit);

    m_gfxFontSizeSpin = createScrubby(1.0, 1000.0, 1.0, 1, " pt");
    m_gfxFontSizeSpin->setToolTip(tr("Font size for graphic text"));
    connect(m_gfxFontSizeSpin, &ScrubbySpinBox::valueCommitted, this, [this](double, double) { applyGfxFontSize(); });
    // Live preview during the drag, mirroring transform/effect spinboxes.
    connect(m_gfxFontSizeSpin, &ScrubbySpinBox::valueScrubbed, this, [this](double) { applyGfxFontSize(); });
    connect(m_gfxFontSizeSpin, &QDoubleSpinBox::editingFinished, this, &PropertiesPanel::applyGfxFontSize);
    form->addRow("Size:", m_gfxFontSizeSpin);

    m_gfxFontWeightSpin = createScrubby(100.0, 900.0, 100.0, 0, "");
    m_gfxFontWeightSpin->setToolTip(tr("Font weight (100 = thin, 400 = normal, 700 = bold, 900 = heavy)"));
    connect(m_gfxFontWeightSpin, &ScrubbySpinBox::valueCommitted, this, [this](double, double) { applyGfxFontWeight(); });
    connect(m_gfxFontWeightSpin, &ScrubbySpinBox::valueScrubbed, this, [this](double) { applyGfxFontWeight(); });
    connect(m_gfxFontWeightSpin, &QDoubleSpinBox::editingFinished, this, &PropertiesPanel::applyGfxFontWeight);
    form->addRow("Weight:", m_gfxFontWeightSpin);

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
