/*
 * PropertiesPanelCaption.cpp — Caption clip styling in the Properties panel.
 *
 * Lets the user restyle captions just like normal text (font, size, position,
 * colours) from the Properties panel. Style changes apply to ALL currently
 * selected caption clips at once (multi-select), each as one undoable command.
 * Text / speaker edits apply to the representative (focused) caption.
 */

#include "panels/properties/PropertiesPanel.h"
#include "widgets/ScrubbySpinBox.h"
#include "Theme.h"

#include "timeline/Clip.h"
#include "timeline/CaptionClip.h"
#include "timeline/TitleClip.h"
#include "timeline/GraphicClip.h"
#include "timeline/GraphicLayer.h"
#include "command/CommandStack.h"
#include "command/LambdaCommand.h"
#include "QtHelpers.h"

#include <QFormLayout>
#include <QGroupBox>
#include <QCheckBox>
#include <QComboBox>
#include <QFontComboBox>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTextOption>
#include <QColorDialog>
#include <QInputDialog>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QFile>

#include <algorithm>
#include <utility>
#include <vector>

namespace rt {

namespace {
QColor argbToQColor(uint32_t c) {
    return QColor(static_cast<int>((c >> 16) & 0xFF),
                  static_cast<int>((c >> 8)  & 0xFF),
                  static_cast<int>( c        & 0xFF),
                  static_cast<int>((c >> 24) & 0xFF));
}
uint32_t qcolorToArgb(const QColor& c) {
    return (static_cast<uint32_t>(c.alpha()) << 24)
         | (static_cast<uint32_t>(c.red())   << 16)
         | (static_cast<uint32_t>(c.green()) << 8)
         |  static_cast<uint32_t>(c.blue());
}
void setSwatch(QPushButton* b, uint32_t argb) {
    if (!b) return;
    QColor c = argbToQColor(argb);
    b->setStyleSheet(QStringLiteral(
        "QPushButton { background: %1; border: 1px solid #555; "
        "min-width: 40px; min-height: 18px; }").arg(c.name()));
}
} // namespace

// ── Target collection ────────────────────────────────────────────────────────

std::vector<CaptionClip*> PropertiesPanel::captionTargets() const
{
    std::vector<CaptionClip*> out;
    if (!m_multiSelection.empty()) {
        for (auto* c : m_multiSelection)
            if (c && c->isCaption())
                out.push_back(static_cast<CaptionClip*>(c));
    }
    if (out.empty() && m_clip && m_clip->isCaption())
        out.push_back(static_cast<CaptionClip*>(m_clip));
    return out;
}

// ── Apply (text / speaker edit the representative caption) ───────────────────

void PropertiesPanel::applyCaptionText()
{
    if (m_updating || !canMutateBoundClip() || !m_clip->isCaption()) return;
    auto* cc = static_cast<CaptionClip*>(m_clip);
    std::string newVal = m_capTextEdit->toPlainText().toStdString();
    if (newVal == cc->text()) return;
    std::string oldVal = cc->text();
    const auto oldStyles = cc->styleRuns();
    const auto newStyles = remapTextStyleRuns(oldVal, newVal, oldStyles);
    auto* self = this;
    if (m_commandStack) {
        m_commandStack->execute(std::make_unique<LambdaCommand>(
            "Edit caption text",
            [cc, newVal, newStyles, self]() { cc->setText(newVal); cc->setStyleRuns(newStyles); self->populateFromCaption(); emit self->propertyChanged(); },
            [cc, oldVal, oldStyles, self]() { cc->setText(oldVal); cc->setStyleRuns(oldStyles); self->populateFromCaption(); emit self->propertyChanged(); }));
    } else { cc->setText(newVal); cc->setStyleRuns(newStyles); emit propertyChanged(); }
}

void PropertiesPanel::applyCaptionSpeaker()
{
    if (m_updating || !canMutateBoundClip() || !m_clip->isCaption()) return;
    auto* cc = static_cast<CaptionClip*>(m_clip);
    std::string newVal = m_capSpeakerEdit->text().toStdString();
    if (newVal == cc->speaker()) return;
    std::string oldVal = cc->speaker();
    auto* self = this;
    if (m_commandStack) {
        m_commandStack->execute(std::make_unique<LambdaCommand>(
            "Edit caption speaker",
            [cc, newVal, self]() { cc->setSpeaker(newVal); self->populateFromCaption(); emit self->propertyChanged(); },
            [cc, oldVal, self]() { cc->setSpeaker(oldVal); self->populateFromCaption(); emit self->propertyChanged(); }));
    } else { cc->setSpeaker(newVal); emit propertyChanged(); }
}

// ── Apply (style applies to ALL selected captions) ───────────────────────────

void PropertiesPanel::applyCaptionFontFamily()
{
    if (m_updating || !canMutateBoundClip()) return;
    auto targets = captionTargets();
    if (targets.empty()) return;
    std::string newVal = m_capFontCombo->currentFont().family().toStdString();
    std::vector<std::pair<CaptionClip*, std::string>> olds;
    bool changed = false;
    for (auto* cc : targets) { olds.emplace_back(cc, cc->fontFamily()); if (cc->fontFamily() != newVal) changed = true; }
    if (!changed) return;
    auto* self = this;
    if (m_commandStack) {
        m_commandStack->execute(std::make_unique<LambdaCommand>(
            "Change caption font",
            [olds, newVal, self]() { for (auto& p : olds) p.first->setFontFamily(newVal); self->populateFromCaption(); emit self->propertyChanged(); },
            [olds, self]()         { for (auto& p : olds) p.first->setFontFamily(p.second); self->populateFromCaption(); emit self->propertyChanged(); }));
    } else { for (auto& p : olds) p.first->setFontFamily(newVal); emit propertyChanged(); }
}

void PropertiesPanel::applyCaptionFontSize()
{
    if (m_updating || !canMutateBoundClip()) return;
    auto targets = captionTargets();
    if (targets.empty()) return;
    float newVal = static_cast<float>(m_capFontSizeSpin->value());
    std::vector<std::pair<CaptionClip*, float>> olds;
    bool changed = false;
    for (auto* cc : targets) { olds.emplace_back(cc, cc->fontSize()); if (cc->fontSize() != newVal) changed = true; }
    if (!changed) return;
    auto* self = this;
    if (m_commandStack) {
        m_commandStack->execute(std::make_unique<LambdaCommand>(
            "Change caption font size",
            [olds, newVal, self]() { for (auto& p : olds) p.first->setFontSize(newVal); self->populateFromCaption(); emit self->propertyChanged(); },
            [olds, self]()         { for (auto& p : olds) p.first->setFontSize(p.second); self->populateFromCaption(); emit self->propertyChanged(); }));
    } else { for (auto& p : olds) p.first->setFontSize(newVal); emit propertyChanged(); }
}

void PropertiesPanel::applyCaptionPosition()
{
    if (m_updating || !canMutateBoundClip()) return;
    auto targets = captionTargets();
    if (targets.empty()) return;
    auto newVal = static_cast<CaptionPosition>(m_capPositionCombo->currentIndex());
    std::vector<std::pair<CaptionClip*, CaptionPosition>> olds;
    bool changed = false;
    for (auto* cc : targets) { olds.emplace_back(cc, cc->position()); if (cc->position() != newVal) changed = true; }
    if (!changed) return;
    auto* self = this;
    if (m_commandStack) {
        m_commandStack->execute(std::make_unique<LambdaCommand>(
            "Change caption position",
            [olds, newVal, self]() { for (auto& p : olds) p.first->setPosition(newVal); self->populateFromCaption(); emit self->propertyChanged(); },
            [olds, self]()         { for (auto& p : olds) p.first->setPosition(p.second); self->populateFromCaption(); emit self->propertyChanged(); }));
    } else { for (auto& p : olds) p.first->setPosition(newVal); emit propertyChanged(); }
}

void PropertiesPanel::applyCaptionTextColor()
{
    auto targets = captionTargets();
    if (targets.empty()) return;
    QColor cur = argbToQColor(targets.front()->textColor());
    QColor chosen = QColorDialog::getColor(cur, this, "Caption Text Color",
                                           QColorDialog::ShowAlphaChannel);
    if (!chosen.isValid()) return;
    uint32_t packed = qcolorToArgb(chosen);
    std::vector<std::pair<CaptionClip*, uint32_t>> olds;
    for (auto* cc : targets) olds.emplace_back(cc, cc->textColor());
    auto* self = this;
    if (m_commandStack) {
        m_commandStack->execute(std::make_unique<LambdaCommand>(
            "Change caption text color",
            [olds, packed, self]() { for (auto& p : olds) p.first->setTextColor(packed); self->populateFromCaption(); emit self->propertyChanged(); },
            [olds, self]()         { for (auto& p : olds) p.first->setTextColor(p.second); self->populateFromCaption(); emit self->propertyChanged(); }));
    } else { for (auto& p : olds) p.first->setTextColor(packed); emit propertyChanged(); }
    setSwatch(m_capTextColorBtn, packed);
}

void PropertiesPanel::applyCaptionBgColor()
{
    auto targets = captionTargets();
    if (targets.empty()) return;
    QColor cur = argbToQColor(targets.front()->bgColor());
    QColor chosen = QColorDialog::getColor(cur, this, "Caption Background Color",
                                           QColorDialog::ShowAlphaChannel);
    if (!chosen.isValid()) return;
    uint32_t packed = qcolorToArgb(chosen);
    std::vector<std::pair<CaptionClip*, uint32_t>> olds;
    for (auto* cc : targets) olds.emplace_back(cc, cc->bgColor());
    auto* self = this;
    if (m_commandStack) {
        m_commandStack->execute(std::make_unique<LambdaCommand>(
            "Change caption background color",
            [olds, packed, self]() { for (auto& p : olds) p.first->setBgColor(packed); self->populateFromCaption(); emit self->propertyChanged(); },
            [olds, self]()         { for (auto& p : olds) p.first->setBgColor(p.second); self->populateFromCaption(); emit self->propertyChanged(); }));
    } else { for (auto& p : olds) p.first->setBgColor(packed); emit propertyChanged(); }
    setSwatch(m_capBgColorBtn, packed);
}

void PropertiesPanel::applyCaptionBold()
{
    if (m_updating || !canMutateBoundClip()) return;
    auto targets = captionTargets();
    if (targets.empty()) return;
    bool newVal = m_capBoldCheck && m_capBoldCheck->isChecked();
    std::vector<std::pair<CaptionClip*, bool>> olds;
    bool changed = false;
    for (auto* cc : targets) { olds.emplace_back(cc, cc->isBold()); if (cc->isBold() != newVal) changed = true; }
    if (!changed) return;
    auto* self = this;
    if (m_commandStack) {
        m_commandStack->execute(std::make_unique<LambdaCommand>(
            "Change caption bold",
            [olds, newVal, self]() { for (auto& p : olds) p.first->setBold(newVal); self->populateFromCaption(); emit self->propertyChanged(); },
            [olds, self]()         { for (auto& p : olds) p.first->setBold(p.second); self->populateFromCaption(); emit self->propertyChanged(); }));
    } else { for (auto& p : olds) p.first->setBold(newVal); emit propertyChanged(); }
}

void PropertiesPanel::applyCaptionOutlineWidth()
{
    if (m_updating || !canMutateBoundClip()) return;
    auto targets = captionTargets();
    if (targets.empty()) return;
    float newVal = m_capOutlineWidthSpin
                 ? static_cast<float>(m_capOutlineWidthSpin->value()) : 0.0f;
    std::vector<std::pair<CaptionClip*, float>> olds;
    bool changed = false;
    for (auto* cc : targets) { olds.emplace_back(cc, cc->outlineWidth()); if (cc->outlineWidth() != newVal) changed = true; }
    if (!changed) return;
    auto* self = this;
    if (m_commandStack) {
        m_commandStack->execute(std::make_unique<LambdaCommand>(
            "Change caption outline",
            [olds, newVal, self]() { for (auto& p : olds) p.first->setOutlineWidth(newVal); self->populateFromCaption(); emit self->propertyChanged(); },
            [olds, self]()         { for (auto& p : olds) p.first->setOutlineWidth(p.second); self->populateFromCaption(); emit self->propertyChanged(); }));
    } else { for (auto& p : olds) p.first->setOutlineWidth(newVal); emit propertyChanged(); }
}

void PropertiesPanel::applyCaptionOutlineColor()
{
    auto targets = captionTargets();
    if (targets.empty()) return;
    QColor cur = argbToQColor(targets.front()->outlineColor());
    QColor chosen = QColorDialog::getColor(cur, this, "Caption Outline Color");
    if (!chosen.isValid()) return;
    uint32_t packed = qcolorToArgb(chosen);
    std::vector<std::pair<CaptionClip*, uint32_t>> olds;
    for (auto* cc : targets) olds.emplace_back(cc, cc->outlineColor());
    auto* self = this;
    if (m_commandStack) {
        m_commandStack->execute(std::make_unique<LambdaCommand>(
            "Change caption outline color",
            [olds, packed, self]() { for (auto& p : olds) p.first->setOutlineColor(packed); self->populateFromCaption(); emit self->propertyChanged(); },
            [olds, self]()         { for (auto& p : olds) p.first->setOutlineColor(p.second); self->populateFromCaption(); emit self->propertyChanged(); }));
    } else { for (auto& p : olds) p.first->setOutlineColor(packed); emit propertyChanged(); }
    setSwatch(m_capOutlineColorBtn, packed);
}

void PropertiesPanel::applyCaptionShowSpeaker()
{
    if (m_updating || !canMutateBoundClip()) return;
    auto targets = captionTargets();
    if (targets.empty()) return;
    bool newVal = m_capShowSpeakerCheck && m_capShowSpeakerCheck->isChecked();
    std::vector<std::pair<CaptionClip*, bool>> olds;
    bool changed = false;
    for (auto* cc : targets) { olds.emplace_back(cc, cc->showSpeaker()); if (cc->showSpeaker() != newVal) changed = true; }
    if (!changed) return;
    auto* self = this;
    if (m_commandStack) {
        m_commandStack->execute(std::make_unique<LambdaCommand>(
            "Change caption speaker burn-in",
            [olds, newVal, self]() { for (auto& p : olds) p.first->setShowSpeaker(newVal); self->populateFromCaption(); emit self->propertyChanged(); },
            [olds, self]()         { for (auto& p : olds) p.first->setShowSpeaker(p.second); self->populateFromCaption(); emit self->propertyChanged(); }));
    } else { for (auto& p : olds) p.first->setShowSpeaker(newVal); emit propertyChanged(); }
}

// ── Populate fields from the representative caption ──────────────────────────

void PropertiesPanel::populateFromCaption()
{
    auto targets = captionTargets();
    if (targets.empty()) return;
    CaptionClip* cc = targets.front();
    const bool prevUpdating = m_updating;  // restore (caller may already be populating)
    m_updating = true;
    if (m_capTextEdit)    m_capTextEdit->setPlainText(QString::fromStdString(cc->text()));
    if (m_capSpeakerEdit) m_capSpeakerEdit->setText(QString::fromStdString(cc->speaker()));
    if (m_capFontCombo)   m_capFontCombo->setCurrentFont(QFont(QString::fromStdString(cc->fontFamily())));
    if (m_capFontSizeSpin) m_capFontSizeSpin->setValue(cc->fontSize());
    if (m_capPositionCombo) m_capPositionCombo->setCurrentIndex(static_cast<int>(cc->position()));
    setSwatch(m_capTextColorBtn, cc->textColor());
    setSwatch(m_capBgColorBtn,   cc->bgColor());
    if (m_capBoldCheck)        m_capBoldCheck->setChecked(cc->isBold());
    if (m_capOutlineWidthSpin) m_capOutlineWidthSpin->setValue(cc->outlineWidth());
    setSwatch(m_capOutlineColorBtn, cc->outlineColor());
    if (m_capShowSpeakerCheck) m_capShowSpeakerCheck->setChecked(cc->showSpeaker());
    // Text/speaker only make sense for a single caption — disable when many.
    const bool single = (targets.size() == 1);
    if (m_capTextEdit)    m_capTextEdit->setEnabled(single);
    if (m_capSpeakerEdit) m_capSpeakerEdit->setEnabled(single);
    m_updating = prevUpdating;
}

// ── Section UI ───────────────────────────────────────────────────────────────

void PropertiesPanel::setupCaptionSection(QWidget* container)
{
    const auto& m = Theme::metrics();
    m_captionSection = new QGroupBox("Caption", container);
    auto* form = new QFormLayout(m_captionSection);
    form->setContentsMargins(m.spacingXs, 18, m.spacingXs, m.spacingXs);
    form->setSpacing(m.spacingSm);

    // Multi-line, word-wrapping caption text so long lines wrap instead of
    // spilling off the side of the panel. Committed on focus-out (eventFilter).
    m_capTextEdit = new QPlainTextEdit(m_captionSection);
    m_capTextEdit->setToolTip(tr("Caption text"));
    m_capTextEdit->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    m_capTextEdit->setWordWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    m_capTextEdit->setMaximumHeight(72);
    // Ignore the wide sizeHint so the field follows the panel width (wrapping
    // to it) and never pushes the Properties panel past the window edge.
    m_capTextEdit->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_capTextEdit->installEventFilter(this);
    form->addRow("Text:", m_capTextEdit);

    m_capSpeakerEdit = new QLineEdit(m_captionSection);
    m_capSpeakerEdit->setToolTip(tr("Speaker / character label"));
    connect(m_capSpeakerEdit, &QLineEdit::editingFinished,
            this, &PropertiesPanel::applyCaptionSpeaker);
    form->addRow("Speaker:", m_capSpeakerEdit);

    m_capFontCombo = new QFontComboBox(m_captionSection);
    m_capFontCombo->setToolTip(tr("Font family (applies to all selected captions)"));
    connect(m_capFontCombo, &QFontComboBox::currentFontChanged,
            this, [this](const QFont&) { applyCaptionFontFamily(); });
    form->addRow("Font:", m_capFontCombo);

    m_capFontSizeSpin = createScrubby(1.0, 500.0, 1.0, 0, " px");
    m_capFontSizeSpin->setToolTip(tr("Font size (applies to all selected captions)"));
    connect(m_capFontSizeSpin, &ScrubbySpinBox::valueCommitted,
            this, [this](double, double) { applyCaptionFontSize(); });
    connect(m_capFontSizeSpin, &QDoubleSpinBox::editingFinished,
            this, &PropertiesPanel::applyCaptionFontSize);
    form->addRow("Size:", m_capFontSizeSpin);

    m_capPositionCombo = new QComboBox(m_captionSection);
    m_capPositionCombo->addItems({"Bottom", "Top", "Middle"});
    m_capPositionCombo->setToolTip(tr("On-screen position (applies to all selected captions)"));
    connect(m_capPositionCombo, &QComboBox::currentIndexChanged,
            this, [this](int) { applyCaptionPosition(); });
    form->addRow("Position:", m_capPositionCombo);

    m_capTextColorBtn = new QPushButton(m_captionSection);
    m_capTextColorBtn->setFixedSize(50, 22);
    m_capTextColorBtn->setToolTip(tr("Text color (applies to all selected captions)"));
    connect(m_capTextColorBtn, &QPushButton::clicked,
            this, &PropertiesPanel::applyCaptionTextColor);
    form->addRow("Text Color:", m_capTextColorBtn);

    m_capBgColorBtn = new QPushButton(m_captionSection);
    m_capBgColorBtn->setFixedSize(50, 22);
    m_capBgColorBtn->setToolTip(tr("Background box color (applies to all selected captions)"));
    connect(m_capBgColorBtn, &QPushButton::clicked,
            this, &PropertiesPanel::applyCaptionBgColor);
    form->addRow("Background:", m_capBgColorBtn);

    m_capBoldCheck = new QCheckBox(m_captionSection);
    m_capBoldCheck->setToolTip(tr("Bold text (applies to all selected captions)"));
    connect(m_capBoldCheck, &QCheckBox::toggled,
            this, [this](bool) { applyCaptionBold(); });
    form->addRow("Bold:", m_capBoldCheck);

    // Outline: width 0 disables; color picks the outline color.
    auto* outlineRow = new QHBoxLayout();
    m_capOutlineWidthSpin = createScrubby(0.0, 20.0, 0.5, 1, " px");
    m_capOutlineWidthSpin->setToolTip(tr("Outline width — 0 = no outline (applies to all selected captions)"));
    connect(m_capOutlineWidthSpin, &ScrubbySpinBox::valueCommitted,
            this, [this](double, double) { applyCaptionOutlineWidth(); });
    connect(m_capOutlineWidthSpin, &QDoubleSpinBox::editingFinished,
            this, &PropertiesPanel::applyCaptionOutlineWidth);
    outlineRow->addWidget(m_capOutlineWidthSpin);
    m_capOutlineColorBtn = new QPushButton(m_captionSection);
    m_capOutlineColorBtn->setFixedSize(50, 22);
    m_capOutlineColorBtn->setToolTip(tr("Outline color (applies to all selected captions)"));
    connect(m_capOutlineColorBtn, &QPushButton::clicked,
            this, &PropertiesPanel::applyCaptionOutlineColor);
    outlineRow->addWidget(m_capOutlineColorBtn);
    outlineRow->addStretch();
    form->addRow("Outline:", outlineRow);

    m_capShowSpeakerCheck = new QCheckBox(m_captionSection);
    m_capShowSpeakerCheck->setToolTip(tr("Burn the speaker label into the caption (\"SPEAKER: text\")"));
    connect(m_capShowSpeakerCheck, &QCheckBox::toggled,
            this, [this](bool) { applyCaptionShowSpeaker(); });
    form->addRow("Burn Speaker:", m_capShowSpeakerCheck);

    // ── Appearance presets (Premiere-style, shared with title/graphic) ────
    m_capPresetCombo = new QComboBox(m_captionSection);
    m_capPresetCombo->setToolTip(tr("Apply a saved text appearance preset to all selected captions"));
    connect(m_capPresetCombo, &QComboBox::activated,
            this, [this](int idx) { applyTextPreset(idx); });
    form->addRow("Preset:", m_capPresetCombo);

    m_capSavePresetBtn = new QPushButton(tr("Save Preset\xE2\x80\xA6"), m_captionSection);
    m_capSavePresetBtn->setToolTip(tr("Save the current appearance as a reusable text preset"));
    connect(m_capSavePresetBtn, &QPushButton::clicked,
            this, &PropertiesPanel::saveTextPresetAs);
    form->addRow("", m_capSavePresetBtn);

    container->layout()->addWidget(m_captionSection);
}

// ═════════════════════════════════════════════════════════════════════════════
//  Universal text appearance presets (Caption + Title + Graphic text)
// ═════════════════════════════════════════════════════════════════════════════

void PropertiesPanel::loadTextPresets()
{
    m_textPresets.clear();
    const QString path = rt::userDataDir() + QStringLiteral("/text_presets.json");
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return;
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
    f.close();
    if (!doc.isArray()) return;
    for (const QJsonValue& v : doc.array()) {
        const QJsonObject o = v.toObject();
        TextStylePreset p;
        p.name       = o.value("name").toString().toStdString();
        p.fontFamily = o.value("font").toString("Arial").toStdString();
        p.fontStyle  = o.value("fontStyle").toString().toStdString();
        p.fontSize   = static_cast<float>(o.value("size").toDouble(32.0));
        p.fontWeight = o.value("fontWeight").toInt(
            o.value("bold").toBool(false) ? 700 : 400);
        p.textColor  = static_cast<uint32_t>(o.value("textColor").toVariant().toULongLong());
        p.bgColor    = static_cast<uint32_t>(o.value("bgColor").toVariant().toULongLong());
        p.position   = o.value("position").toInt(0);
        p.bold       = o.value("bold").toBool(false);
        p.italic     = o.value("italic").toBool(false);
        p.alignment  = o.value("align").toInt(1);
        p.verticalAlignment = o.value("verticalAlign").toInt(1);
        p.allCaps = o.value("allCaps").toBool(false);
        p.smallCaps = o.value("smallCaps").toBool(false);
        p.underline = o.value("underline").toBool(false);
        p.superscript = o.value("superscript").toBool(false);
        p.subscript = o.value("subscript").toBool(false);
        p.fauxBold = o.value("fauxBold").toBool(false);
        p.fauxItalic = o.value("fauxItalic").toBool(false);
        p.rightToLeft = o.value("rightToLeft").toBool(false);
        p.tracking = static_cast<float>(o.value("tracking").toDouble(0.0));
        p.leading = static_cast<float>(o.value("leading").toDouble(0.0));
        p.baselineShift = static_cast<float>(
            o.value("baselineShift").toDouble(0.0));
        p.kerning = static_cast<float>(o.value("kerning").toDouble(0.0));
        p.tabWidth = static_cast<float>(o.value("tabWidth").toDouble(48.0));
        p.tsume = static_cast<float>(o.value("tsume").toDouble(0.0));
        p.backgroundEnabled = o.value("backgroundEnabled").toBool(false);
        p.backgroundPadding = static_cast<float>(
            o.value("backgroundPadding").toDouble(4.0));
        p.maskWithText = o.value("maskWithText").toBool(false);
        p.showSpeaker = o.value("showSpeaker").toBool(false);
        p.strokeEnabled = o.value("strokeEnabled").toBool(false);
        if (o.contains("strokeColor"))
            p.strokeColor = static_cast<uint32_t>(o.value("strokeColor").toVariant().toULongLong());
        p.strokeWidth   = static_cast<float>(o.value("strokeWidth").toDouble(2.0));
        p.strokePosition = o.value("strokePos").toInt(2);
        p.shadowEnabled = o.value("shadowEnabled").toBool(false);
        if (o.contains("shadowColor"))
            p.shadowColor = static_cast<uint32_t>(o.value("shadowColor").toVariant().toULongLong());
        p.shadowDistance = static_cast<float>(o.value("shadowDistance").toDouble(4.0));
        p.shadowAngle    = static_cast<float>(o.value("shadowAngle").toDouble(135.0));
        p.shadowSoftness = static_cast<float>(o.value("shadowSoftness").toDouble(4.0));
        p.shadowOpacity  = static_cast<float>(o.value("shadowOpacity").toDouble(0.6));
        if (!p.name.empty()) m_textPresets.push_back(std::move(p));
    }
}

void PropertiesPanel::saveTextPresetsToDisk() const
{
    QJsonArray arr;
    for (const auto& p : m_textPresets) {
        QJsonObject o;
        o["name"]      = QString::fromStdString(p.name);
        o["font"]      = QString::fromStdString(p.fontFamily);
        o["fontStyle"] = QString::fromStdString(p.fontStyle);
        o["size"]      = static_cast<double>(p.fontSize);
        o["fontWeight"] = p.fontWeight;
        o["textColor"] = static_cast<qint64>(p.textColor);
        o["bgColor"]   = static_cast<qint64>(p.bgColor);
        o["position"]  = p.position;
        o["bold"]      = p.bold;
        o["italic"]    = p.italic;
        o["align"]     = p.alignment;
        o["verticalAlign"] = p.verticalAlignment;
        o["allCaps"] = p.allCaps;
        o["smallCaps"] = p.smallCaps;
        o["underline"] = p.underline;
        o["superscript"] = p.superscript;
        o["subscript"] = p.subscript;
        o["fauxBold"] = p.fauxBold;
        o["fauxItalic"] = p.fauxItalic;
        o["rightToLeft"] = p.rightToLeft;
        o["tracking"] = static_cast<double>(p.tracking);
        o["leading"] = static_cast<double>(p.leading);
        o["baselineShift"] = static_cast<double>(p.baselineShift);
        o["kerning"] = static_cast<double>(p.kerning);
        o["tabWidth"] = static_cast<double>(p.tabWidth);
        o["tsume"] = static_cast<double>(p.tsume);
        o["backgroundEnabled"] = p.backgroundEnabled;
        o["backgroundPadding"] = static_cast<double>(p.backgroundPadding);
        o["maskWithText"] = p.maskWithText;
        o["showSpeaker"] = p.showSpeaker;
        o["strokeEnabled"]  = p.strokeEnabled;
        o["strokeColor"]    = static_cast<qint64>(p.strokeColor);
        o["strokeWidth"]    = static_cast<double>(p.strokeWidth);
        o["strokePos"]      = p.strokePosition;
        o["shadowEnabled"]  = p.shadowEnabled;
        o["shadowColor"]    = static_cast<qint64>(p.shadowColor);
        o["shadowDistance"] = static_cast<double>(p.shadowDistance);
        o["shadowAngle"]    = static_cast<double>(p.shadowAngle);
        o["shadowSoftness"] = static_cast<double>(p.shadowSoftness);
        o["shadowOpacity"]  = static_cast<double>(p.shadowOpacity);
        arr.append(o);
    }
    const QString path = rt::userDataDir() + QStringLiteral("/text_presets.json");
    QFile f(path);
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        f.write(QJsonDocument(arr).toJson(QJsonDocument::Indented));
        f.close();
    }
}

void PropertiesPanel::rebuildPresetCombos()
{
    const bool prev = m_updating;
    m_updating = true;
    QComboBox* combos[] = { m_capPresetCombo, m_titlePresetCombo, m_gfxPresetCombo };
    for (QComboBox* c : combos) {
        if (!c) continue;
        c->clear();
        c->addItem(tr("Choose preset\xE2\x80\xA6"));  // index 0 = no-op
        for (const auto& p : m_textPresets)
            c->addItem(QString::fromStdString(p.name));
        c->setCurrentIndex(0);
    }
    m_updating = prev;
}

void PropertiesPanel::applyTextPreset(int index)
{
    if (m_updating || !canMutateBoundClip()) return;
    if (index <= 0 || index > static_cast<int>(m_textPresets.size())) return;
    const TextStylePreset preset = m_textPresets[static_cast<size_t>(index - 1)];
    auto* self = this;

    if (m_clip->isCaption()) {
        auto targets = captionTargets();
        if (targets.empty()) return;
        CaptionStyle style;
        style.fontFamily = preset.fontFamily;
        style.fontStyle = preset.fontStyle;
        style.fontSize = preset.fontSize;
        style.textColor = preset.textColor;
        style.bgColor = preset.bgColor;
        style.position = static_cast<CaptionPosition>(preset.position);
        style.bold = preset.fontWeight >= 700 || preset.bold;
        style.outlineColor = preset.strokeColor;
        style.outlineWidth = preset.strokeEnabled ? preset.strokeWidth : 0.0f;
        style.showSpeaker = preset.showSpeaker;
        style.italic = preset.italic;
        style.allCaps = preset.allCaps;
        style.smallCaps = preset.smallCaps;
        style.underline = preset.underline;
        style.superscript = preset.superscript;
        style.subscript = preset.subscript;
        style.fauxBold = preset.fauxBold;
        style.fauxItalic = preset.fauxItalic;
        style.tracking = preset.tracking;
        style.leading = preset.leading;
        style.alignment = static_cast<GTextAlign>(preset.alignment);
        style.name = preset.name;
        struct Old { CaptionClip* cc; CaptionStyle style; };
        auto olds = std::make_shared<std::vector<Old>>();
        for (auto* cc : targets)
            olds->push_back({cc, cc->captionStyle()});
        auto doIt = [olds, style, self]() {
            for (auto& o : *olds) o.cc->setCaptionStyle(style);
            self->populateFromCaption(); emit self->propertyChanged();
        };
        auto undoIt = [olds, self]() {
            for (auto& o : *olds) o.cc->setCaptionStyle(o.style);
            self->populateFromCaption(); emit self->propertyChanged();
        };
        if (m_commandStack) m_commandStack->execute(std::make_unique<LambdaCommand>("Apply text preset", doIt, undoIt));
        else doIt();
    }
    else if (m_clip->clipType() == ClipType::Title) {
        auto* tc = static_cast<TitleClip*>(m_clip);
        struct Old { std::string font; float size; uint32_t tcol; uint32_t bcol; bool bold; bool ital; TextAlign align; };
        auto old = std::make_shared<Old>(Old{tc->fontFamily(), tc->fontSize(), tc->textColor(),
                                             tc->bgColor(), tc->isBold(), tc->isItalic(), tc->alignment()});
        auto doIt = [tc, preset, self]() {
            tc->setFontFamily(preset.fontFamily); tc->setFontSize(preset.fontSize);
            tc->setTextColor(preset.textColor);   tc->setBgColor(preset.bgColor);
            tc->setBold(preset.bold); tc->setItalic(preset.italic);
            tc->setAlignment(static_cast<TextAlign>(preset.alignment));
            self->populateFromClip(); emit self->propertyChanged();
        };
        auto undoIt = [tc, old, self]() {
            tc->setFontFamily(old->font); tc->setFontSize(old->size);
            tc->setTextColor(old->tcol);  tc->setBgColor(old->bcol);
            tc->setBold(old->bold); tc->setItalic(old->ital); tc->setAlignment(old->align);
            self->populateFromClip(); emit self->propertyChanged();
        };
        if (m_commandStack) m_commandStack->execute(std::make_unique<LambdaCommand>("Apply text preset", doIt, undoIt));
        else doIt();
    }
    else if (m_clip->clipType() == ClipType::Graphic) {
        auto* tl = selectedGraphicTextLayer();
        if (!tl) return;
        auto old = std::shared_ptr<GraphicLayer>(tl->clone().release());
        auto doIt = [tl, preset, self]() {
            tl->setFontFamily(preset.fontFamily);
            tl->setFontStyleForAll(preset.fontStyle);
            tl->setFontSize(preset.fontSize);
            tl->setFontWeight(preset.fontWeight);
            tl->setItalic(preset.italic);
            tl->setAllCaps(preset.allCaps);
            tl->setSmallCaps(preset.smallCaps);
            tl->setAlignment(static_cast<GTextAlign>(preset.alignment));
            tl->setVAlignment(static_cast<GTextVAlign>(
                preset.verticalAlignment));
            tl->setTrackingForAll(preset.tracking);
            tl->setLeadingForAll(preset.leading);
            tl->setBaselineShiftForAll(preset.baselineShift);
            tl->setKerningForAll(preset.kerning);
            tl->setTabWidthForAll(preset.tabWidth);
            tl->setTsumeForAll(preset.tsume);
            tl->setFauxStylesForAll(preset.fauxBold, preset.fauxItalic);
            tl->setUnderlineForAll(preset.underline);
            tl->setScriptForAll(preset.superscript, preset.subscript);
            tl->setRightToLeft(preset.rightToLeft);
            tl->setFillForAll(true, preset.textColor);
            tl->setStrokeForAll(preset.strokeEnabled, preset.strokeColor,
                preset.strokeWidth, static_cast<StrokePosition>(
                    std::clamp(preset.strokePosition, 0, 2)));
            tl->setShadowForAll(preset.shadowEnabled, preset.shadowColor,
                preset.shadowDistance, preset.shadowAngle,
                preset.shadowSoftness, preset.shadowOpacity);
            tl->setBackgroundForAll(preset.backgroundEnabled, preset.bgColor,
                                    preset.backgroundPadding);
            tl->setMaskWithText(preset.maskWithText);
            tl->setLinkedStyleName(preset.name);
            self->populateFromClip(); emit self->propertyChanged();
        };
        auto undoIt = [tl, old, self]() {
            tl->assignStateFrom(*old);
            self->populateFromClip(); emit self->propertyChanged();
        };
        if (m_commandStack) m_commandStack->execute(std::make_unique<LambdaCommand>("Apply text preset", doIt, undoIt));
        else doIt();
    }

    rebuildPresetCombos(); // reset all combos to the placeholder
}

void PropertiesPanel::saveTextPresetAs()
{
    if (!m_clip) return;
    TextStylePreset p;

    if (m_clip->isCaption()) {
        auto targets = captionTargets();
        if (targets.empty()) return;
        CaptionClip* cc = targets.front();
        const CaptionStyle style = cc->captionStyle();
        p.fontFamily = style.fontFamily; p.fontStyle = style.fontStyle;
        p.fontSize = style.fontSize; p.fontWeight = style.bold ? 700 : 400;
        p.textColor = style.textColor; p.bgColor = style.bgColor;
        p.position = static_cast<int>(style.position);
        p.bold = style.bold; p.italic = style.italic;
        p.allCaps = style.allCaps; p.smallCaps = style.smallCaps;
        p.underline = style.underline;
        p.superscript = style.superscript; p.subscript = style.subscript;
        p.fauxBold = style.fauxBold; p.fauxItalic = style.fauxItalic;
        p.tracking = style.tracking; p.leading = style.leading;
        p.alignment = static_cast<int>(style.alignment);
        p.strokeEnabled = style.outlineWidth > 0.0f;
        p.strokeColor = style.outlineColor;
        p.strokeWidth = style.outlineWidth;
        p.showSpeaker = style.showSpeaker;
    } else if (m_clip->clipType() == ClipType::Title) {
        auto* tc = static_cast<TitleClip*>(m_clip);
        p.fontFamily = tc->fontFamily(); p.fontSize = tc->fontSize();
        p.textColor = tc->textColor();   p.bgColor = tc->bgColor();
        p.bold = tc->isBold(); p.italic = tc->isItalic();
        p.fontWeight = p.bold ? 700 : 400;
        p.alignment = static_cast<int>(tc->alignment());
    } else if (m_clip->clipType() == ClipType::Graphic) {
        auto* tl = selectedGraphicTextLayer();
        if (!tl) return;
        p.fontFamily = tl->fontFamily(); p.fontStyle = tl->fontStyle();
        p.fontSize = tl->fontSize(); p.fontWeight = tl->fontWeight();
        p.textColor = tl->appearance().fills.empty() ? 0xFFFFFFFFu
                                                     : tl->appearance().fills[0].color;
        p.bold = tl->fontWeight() >= 600; p.italic = tl->isItalic();
        p.alignment = static_cast<int>(tl->alignment());
        p.verticalAlignment = static_cast<int>(tl->vAlignment());
        p.allCaps = tl->allCaps(); p.smallCaps = tl->smallCaps();
        p.underline = tl->underline();
        p.superscript = tl->superscript(); p.subscript = tl->subscript();
        p.fauxBold = tl->fauxBold(); p.fauxItalic = tl->fauxItalic();
        p.rightToLeft = tl->rightToLeft();
        p.tracking = tl->tracking().evaluate(0);
        p.leading = tl->leading().evaluate(0);
        p.baselineShift = tl->baselineShift().evaluate(0);
        p.kerning = tl->kerning(); p.tabWidth = tl->tabWidth();
        p.tsume = tl->tsume();
        p.backgroundEnabled = tl->backgroundEnabled();
        p.bgColor = tl->backgroundColor();
        p.backgroundPadding = tl->backgroundPadding();
        p.maskWithText = tl->maskWithText();
        if (!tl->appearance().strokes.empty()) {
            const StrokeEntry& s = tl->appearance().strokes[0];
            p.strokeEnabled  = s.enabled;
            p.strokeColor    = s.color;
            p.strokeWidth    = s.width;
            p.strokePosition = static_cast<int>(s.position);
        }
        if (!tl->appearance().shadows.empty()) {
            const ShadowEntry& sh = tl->appearance().shadows[0];
            p.shadowEnabled  = sh.enabled;
            p.shadowColor    = sh.color;
            p.shadowDistance = sh.distance;
            p.shadowAngle    = sh.angle;
            p.shadowSoftness = sh.softness;
            p.shadowOpacity  = sh.opacity;
        }
    } else {
        return; // not a text clip
    }

    bool ok = false;
    const QString name = QInputDialog::getText(
        this, tr("Save Text Preset"), tr("Preset name:"),
        QLineEdit::Normal, QString(), &ok);
    if (!ok || name.trimmed().isEmpty()) return;
    p.name = name.trimmed().toStdString();

    bool replaced = false;
    for (auto& existing : m_textPresets)
        if (existing.name == p.name) { existing = p; replaced = true; break; }
    if (!replaced) m_textPresets.push_back(p);

    saveTextPresetsToDisk();
    rebuildPresetCombos();
}

} // namespace rt
