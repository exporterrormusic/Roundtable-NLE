/*
 * PropertiesPanelTitle.cpp — Title clip property application and UI setup.
 * Split from PropertiesPanelApply.cpp + PropertiesPanelUI.cpp.
 *
 * Contains: applyTitleText(), applyTitleFontFamily(), applyTitleFontSize(),
 *           applyTitleBold(), applyTitleItalic(), applyTitleAlign(),
 *           setupTitleSection().
 */

#include "panels/properties/PropertiesPanel.h"
#include "widgets/ScrubbySpinBox.h"
#include "Theme.h"

#include "timeline/Clip.h"
#include "timeline/TitleClip.h"
#include "command/CommandStack.h"
#include "command/LambdaCommand.h"

#include <QFormLayout>
#include <QGroupBox>
#include <QComboBox>
#include <QCheckBox>
#include <QLineEdit>
#include <QPushButton>

namespace rt {

// ═════════════════════════════════════════════════════════════════════════════
//  Title property apply
// ═════════════════════════════════════════════════════════════════════════════

void PropertiesPanel::applyTitleText()
{
    if (m_updating || !m_clip || m_clip->clipType() != ClipType::Title) return;
    auto* tc = static_cast<TitleClip*>(m_clip);
    auto newVal = m_textEdit->text().toStdString();
    if (newVal == tc->text()) return;
    auto oldVal = tc->text();
    if (m_commandStack) {
        m_commandStack->execute(std::make_unique<LambdaCommand>(
            "Change title text",
            [tc, newVal, this]() { tc->setText(newVal); populateFromClip(); emit propertyChanged(); },
            [tc, oldVal, this]() { tc->setText(oldVal); populateFromClip(); emit propertyChanged(); }));
    } else {
        tc->setText(newVal);
        emit propertyChanged();
    }
}

void PropertiesPanel::applyTitleFontFamily()
{
    if (m_updating || !m_clip || m_clip->clipType() != ClipType::Title) return;
    auto* tc = static_cast<TitleClip*>(m_clip);
    auto newVal = m_fontFamilyEdit->text().toStdString();
    if (newVal == tc->fontFamily()) return;
    auto oldVal = tc->fontFamily();
    if (m_commandStack) {
        m_commandStack->execute(std::make_unique<LambdaCommand>(
            "Change font family",
            [tc, newVal, this]() { tc->setFontFamily(newVal); populateFromClip(); emit propertyChanged(); },
            [tc, oldVal, this]() { tc->setFontFamily(oldVal); populateFromClip(); emit propertyChanged(); }));
    } else {
        tc->setFontFamily(newVal);
        emit propertyChanged();
    }
}

void PropertiesPanel::applyTitleFontSize()
{
    if (m_updating || !m_clip || m_clip->clipType() != ClipType::Title) return;
    auto* tc = static_cast<TitleClip*>(m_clip);
    float newVal = static_cast<float>(m_fontSizeSpin->value());
    if (newVal == tc->fontSize()) return;
    float oldVal = tc->fontSize();
    if (m_commandStack) {
        m_commandStack->execute(std::make_unique<LambdaCommand>(
            "Change font size",
            [tc, newVal, this]() { tc->setFontSize(newVal); populateFromClip(); emit propertyChanged(); },
            [tc, oldVal, this]() { tc->setFontSize(oldVal); populateFromClip(); emit propertyChanged(); }));
    } else {
        tc->setFontSize(newVal);
        emit propertyChanged();
    }
}

void PropertiesPanel::applyTitleBold()
{
    if (m_updating || !m_clip || m_clip->clipType() != ClipType::Title) return;
    auto* tc = static_cast<TitleClip*>(m_clip);
    bool newVal = m_boldCheck->isChecked();
    if (newVal == tc->isBold()) return;
    bool oldVal = tc->isBold();
    if (m_commandStack) {
        m_commandStack->execute(std::make_unique<LambdaCommand>(
            "Toggle bold",
            [tc, newVal, this]() { tc->setBold(newVal); populateFromClip(); emit propertyChanged(); },
            [tc, oldVal, this]() { tc->setBold(oldVal); populateFromClip(); emit propertyChanged(); }));
    } else {
        tc->setBold(newVal);
        emit propertyChanged();
    }
}

void PropertiesPanel::applyTitleItalic()
{
    if (m_updating || !m_clip || m_clip->clipType() != ClipType::Title) return;
    auto* tc = static_cast<TitleClip*>(m_clip);
    bool newVal = m_italicCheck->isChecked();
    if (newVal == tc->isItalic()) return;
    bool oldVal = tc->isItalic();
    if (m_commandStack) {
        m_commandStack->execute(std::make_unique<LambdaCommand>(
            "Toggle italic",
            [tc, newVal, this]() { tc->setItalic(newVal); populateFromClip(); emit propertyChanged(); },
            [tc, oldVal, this]() { tc->setItalic(oldVal); populateFromClip(); emit propertyChanged(); }));
    } else {
        tc->setItalic(newVal);
        emit propertyChanged();
    }
}

void PropertiesPanel::applyTitleAlign()
{
    if (m_updating || !m_clip || m_clip->clipType() != ClipType::Title) return;
    auto* tc = static_cast<TitleClip*>(m_clip);
    auto newVal = static_cast<TextAlign>(m_alignCombo->currentIndex());
    if (newVal == tc->alignment()) return;
    auto oldVal = tc->alignment();
    if (m_commandStack) {
        m_commandStack->execute(std::make_unique<LambdaCommand>(
            "Change text alignment",
            [tc, newVal, this]() { tc->setAlignment(newVal); populateFromClip(); emit propertyChanged(); },
            [tc, oldVal, this]() { tc->setAlignment(oldVal); populateFromClip(); emit propertyChanged(); }));
    } else {
        tc->setAlignment(newVal);
        emit propertyChanged();
    }
}

// ═════════════════════════════════════════════════════════════════════════════
//  Title section UI
// ═════════════════════════════════════════════════════════════════════════════

void PropertiesPanel::setupTitleSection(QWidget* container)
{
    const auto& m = Theme::metrics();
    m_titleSection = new QGroupBox("Title", container);
    auto* form = new QFormLayout(m_titleSection);
    form->setContentsMargins(m.spacingXs, 18, m.spacingXs, m.spacingXs);
    form->setSpacing(m.spacingSm);

    m_textEdit = new QLineEdit(m_titleSection);
    m_textEdit->setToolTip(tr("Title text content"));
    connect(m_textEdit, &QLineEdit::editingFinished,
            this, &PropertiesPanel::applyTitleText);
    form->addRow("Text:", m_textEdit);

    m_fontFamilyEdit = new QLineEdit(m_titleSection);
    m_fontFamilyEdit->setToolTip(tr("Font family name (e.g. Arial, Times New Roman)"));
    connect(m_fontFamilyEdit, &QLineEdit::editingFinished,
            this, &PropertiesPanel::applyTitleFontFamily);
    form->addRow("Font:", m_fontFamilyEdit);

    m_fontSizeSpin = createScrubby(1.0, 500.0, 1.0, 1, " pt");
    m_fontSizeSpin->setToolTip(tr("Font size in points"));
    // Live preview while scrubbing: write the model + repaint on every tick,
    // but DON'T push an undo command per tick (that flooded the stack). A
    // single command is recorded on release (valueCommitted) using the signal's
    // pre-drag baseline. Typed entry (editingFinished) still uses the full
    // applyTitleFontSize() path (its old value is the as-yet-unmodified model).
    connect(m_fontSizeSpin, &ScrubbySpinBox::valueScrubbed, this, [this](double v) {
        if (m_updating || !m_clip || m_clip->clipType() != ClipType::Title) return;
        static_cast<TitleClip*>(m_clip)->setFontSize(static_cast<float>(v));
        emit propertyChanged();
    });
    connect(m_fontSizeSpin, &ScrubbySpinBox::valueCommitted, this,
            [this](double oldV, double newV) {
        if (m_updating || !m_clip || m_clip->clipType() != ClipType::Title) return;
        if (static_cast<float>(oldV) == static_cast<float>(newV)) return;
        auto* tc = static_cast<TitleClip*>(m_clip);
        tc->setFontSize(static_cast<float>(newV));      // ensure applied
        if (m_commandStack) {
            const float o = static_cast<float>(oldV), n = static_cast<float>(newV);
            m_commandStack->pushWithoutExecute(std::make_unique<LambdaCommand>(
                "Change font size",
                [tc, n, this]() { tc->setFontSize(n); populateFromClip(); emit propertyChanged(); },
                [tc, o, this]() { tc->setFontSize(o); populateFromClip(); emit propertyChanged(); }));
        }
        emit propertyChanged();
    });
    connect(m_fontSizeSpin, &QDoubleSpinBox::editingFinished,
            this, &PropertiesPanel::applyTitleFontSize);
    form->addRow("Size:", m_fontSizeSpin);

    m_boldCheck = new QCheckBox("Bold", m_titleSection);
    m_boldCheck->setToolTip(tr("Toggle bold text"));
    connect(m_boldCheck, &QCheckBox::toggled,
            this, &PropertiesPanel::applyTitleBold);
    form->addRow(m_boldCheck);

    m_italicCheck = new QCheckBox("Italic", m_titleSection);
    m_italicCheck->setToolTip(tr("Toggle italic text"));
    connect(m_italicCheck, &QCheckBox::toggled,
            this, &PropertiesPanel::applyTitleItalic);
    form->addRow(m_italicCheck);

    m_alignCombo = new QComboBox(m_titleSection);
    m_alignCombo->setToolTip(tr("Text horizontal alignment"));
    m_alignCombo->addItems({"Left", "Center", "Right"});
    connect(m_alignCombo, &QComboBox::currentIndexChanged,
            this, [this](int) { applyTitleAlign(); });
    form->addRow("Align:", m_alignCombo);

    // Shared text appearance presets.
    m_titlePresetCombo = new QComboBox(m_titleSection);
    m_titlePresetCombo->setToolTip(tr("Apply a saved text appearance preset"));
    connect(m_titlePresetCombo, &QComboBox::activated,
            this, [this](int idx) { applyTextPreset(idx); });
    form->addRow("Preset:", m_titlePresetCombo);

    m_titleSavePresetBtn = new QPushButton(tr("Save Preset\xE2\x80\xA6"), m_titleSection);
    m_titleSavePresetBtn->setToolTip(tr("Save the current appearance as a reusable text preset"));
    connect(m_titleSavePresetBtn, &QPushButton::clicked,
            this, &PropertiesPanel::saveTextPresetAs);
    form->addRow("", m_titleSavePresetBtn);

    container->layout()->addWidget(m_titleSection);
}

} // namespace rt
