/*
 * PropertiesPanelUI.cpp - UI construction extracted from PropertiesPanel.cpp.
 *
 * Contains: constructor, destructor, createScrubby, all setup*Section methods,
 * transitionTypeName helper, and setupTransitionSection.
 */


#include "panels/properties/PropertiesPanel.h"
#include "widgets/ScrubbySpinBox.h"
#include "Theme.h"

#include "timeline/Clip.h"
#include "timeline/SpineClip.h"
#include "timeline/VideoClip.h"
#include "timeline/AudioClip.h"
#include "timeline/TitleClip.h"
#include "timeline/GraphicClip.h"
#include "timeline/Track.h"
#include "timeline/Timeline.h"
#include "timeline/Transition.h"
#include "spine/ShotPreset.h"
#include "command/CommandStack.h"
#include "command/LambdaCommand.h"
#include "command/commands/ClipCommands.h"
#include "effects/Effect.h"
#include "effects/EffectStack.h"
#include "command/commands/EffectCommands.h"

#include <QFormLayout>
#include <QFrame>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QCheckBox>
#include <QListWidget>
#include <QPushButton>

#include <cmath>
#include <QScrollArea>
#include <QVBoxLayout>
#include <QPlainTextEdit>
#include <QComboBox>
#include <QColorDialog>
#include <QTimer>

#include <spdlog/spdlog.h>
#include <chrono>

namespace rt {

// ═════════════════════════════════════════════════════════════════════════════
//  Construction
// ═════════════════════════════════════════════════════════════════════════════

PropertiesPanel::PropertiesPanel(QWidget* parent)
    : QWidget(parent)
{
    setupUI();
}

PropertiesPanel::~PropertiesPanel()
{
    m_destroying.store(true, std::memory_order_release);

    // Stop the search debounce timer — prevents it firing during destruction
    if (m_searchDebounce) {
        m_searchDebounce->stop();
    }
}

// ═════════════════════════════════════════════════════════════════════════════
//  UI Setup
// ═════════════════════════════════════════════════════════════════════════════


ScrubbySpinBox* PropertiesPanel::createScrubby(double min, double max,
                                                double step, int decimals,
                                                const QString& suffix)
{
    auto* spin = new ScrubbySpinBox(this);
    spin->setRange(min, max);
    spin->setScrubStep(step);
    spin->setSingleStep(step);
    spin->setDecimals(decimals);
    if (!suffix.isEmpty())
        spin->setSuffix(suffix);
    return spin;
}

void PropertiesPanel::makeCollapsible(QGroupBox* box)
{
    if (!box) return;
    const auto& tc = Theme::colors();
    QString title = box->title();
    box->setTitle(QStringLiteral("\u25BC ") + title); // ▼ prefix
    box->setCheckable(true);
    box->setChecked(true);

    box->setStyleSheet(box->styleSheet() +
        QStringLiteral(
            "QGroupBox::indicator { width: 0px; height: 0px; }"
            "QGroupBox { margin-top: 0px; margin-bottom: 0px; padding-top: 20px; "
            "border: none; border-bottom: 1px solid %1; }"
            "QGroupBox::title { subcontrol-origin: margin; padding: 4px 6px; "
            "background: %2; color: %3; font-size: 12px; font-weight: bold; }")
            .arg(Theme::hex(tc.panelBorder),
                 Theme::hex(tc.surface2),
                 Theme::hex(tc.text)));

    connect(box, &QGroupBox::toggled, box, [box, title](bool expanded) {
        box->setTitle((expanded ? QStringLiteral("\u25BC ") : QStringLiteral("\u25B6 ")) + title);
        if (auto* lay = box->layout())
            lay->setEnabled(expanded);
        const auto children = box->findChildren<QWidget*>(QString(), Qt::FindDirectChildrenOnly);
        for (auto* child : children)
            child->setVisible(expanded);
        if (!expanded)
            box->setMaximumHeight(20);
        else
            box->setMaximumHeight(16777215); // QWIDGETSIZE_MAX
    });
}

void PropertiesPanel::setupUI()
{
    const auto& tc = Theme::colors();
    const auto& m  = Theme::metrics();
    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(0);

    // ── Header toolbar (28px) ───────────────────────────────────────────
    auto* toolbar = new QWidget(this);
    toolbar->setFixedHeight(28);
    toolbar->setStyleSheet(QStringLiteral(
        "QWidget { background: %1; border-bottom: 1px solid %2; }")
        .arg(Theme::hex(tc.surface2), Theme::hex(tc.panelBorder)));
    auto* toolbarLayout = new QHBoxLayout(toolbar);
    toolbarLayout->setContentsMargins(m.spacingXs, m.spacingXxs, m.spacingXs, m.spacingXxs);
    toolbarLayout->setSpacing(m.spacingXs);

    m_headerLabel = new QLabel(this);
    m_headerLabel->setStyleSheet(
        QStringLiteral("QLabel { color: %1; font-weight: bold; font-size: 12px; "
                       "background: transparent; border: none; }")
            .arg(Theme::hex(tc.text)));
    // Don't let a long clip name (e.g. a full caption line) dictate the panel
    // width — ignore the label's wide sizeHint so it stays inside the dock
    // (and the rest of the panel keeps its right padding instead of spilling).
    m_headerLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_headerLabel->setMinimumWidth(0);
    toolbarLayout->addWidget(m_headerLabel, 1);

    m_typeLabel = new QLabel(this);
    m_typeLabel->setFixedHeight(18);
    m_typeLabel->setStyleSheet(
        QStringLiteral("QLabel { color: %1; font-size: 11px; padding: 1px 6px; "
                       "border-radius: %2px; background: transparent; border: none; }")
            .arg(Theme::hex(tc.textSecondary), QString::number(m.radiusSm)));
    toolbarLayout->addWidget(m_typeLabel);
    toolbarLayout->addStretch();
    mainLayout->addWidget(toolbar);

    // ── Search / filter bar (26px) ──────────────────────────────────────
    auto* filterBar = new QWidget(this);
    filterBar->setFixedHeight(26);
    filterBar->setStyleSheet(QStringLiteral(
        "QWidget { background: %1; border-bottom: 1px solid %2; }")
        .arg(Theme::hex(tc.surface1), Theme::hex(tc.panelBorder)));
    auto* filterLayout = new QHBoxLayout(filterBar);
    filterLayout->setContentsMargins(m.spacingXs, m.spacingXxs, m.spacingXs, m.spacingXxs);
    filterLayout->setSpacing(m.spacingXs);

    m_searchField = new QLineEdit(filterBar);
    m_searchField->setToolTip(tr("Filter properties by name"));
    m_searchField->setPlaceholderText(QStringLiteral("\U0001F50D Filter properties\u2026"));
    m_searchField->setClearButtonEnabled(true);
    m_searchField->setFixedHeight(20);
    m_searchField->setStyleSheet(
        QString("QLineEdit { background: %1; color: %2; border: 1px solid %3; "
                "border-radius: %4px; padding: 1px 4px; font-size: 12px; }"
                "QLineEdit:focus { border: 1px solid %5; }")
            .arg(Theme::hex(tc.inputBg), Theme::hex(tc.text),
                 Theme::hex(tc.controlBorder),
                 QString::number(m.radiusSm),
                 Theme::hex(tc.accent)));
    filterLayout->addWidget(m_searchField, 1);

    m_searchDebounce = new QTimer(this);
    m_searchDebounce->setSingleShot(true);
    m_searchDebounce->setInterval(200);
    connect(m_searchField, &QLineEdit::textChanged,
            this, [this]() { m_searchDebounce->start(); });
    connect(m_searchDebounce, &QTimer::timeout, this, [this]() {
        QString term = m_searchField->text().trimmed().toLower();
        if (term.isEmpty()) {
            showSectionsForType(); // restore proper clip-type visibility
            return;
        }
        // Hide sections whose title doesn't match the search term
        for (auto* box : {m_characterSection, m_animationSection}) {
            if (box && box->isVisible() && !box->title().toLower().contains(term))
                box->setVisible(false);
        }
        for (auto* w : {m_transformSection, m_videoSection,
                         m_audioSection, m_titleSection, m_graphicSection,
                         m_shotSection, m_effectsSection, m_transitionSection}) {
            if (!w || !w->isVisible()) continue;
            if (auto* box = qobject_cast<QGroupBox*>(w)) {
                if (!box->title().toLower().contains(term))
                    box->setVisible(false);
            }
        }
    });
    mainLayout->addWidget(filterBar);

    // ── Scroll area for property sections ───────────────────────────────
    m_scrollArea = new QScrollArea(this);
    m_scrollArea->setWidgetResizable(true);
    m_scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_scrollArea->setStyleSheet(
        QStringLiteral("QScrollArea { background: %1; border: none; }")
            .arg(Theme::hex(tc.surface0)));

    m_scrollContainer = new QWidget;
    m_scrollContainer->setStyleSheet(
        QStringLiteral("QDoubleSpinBox:hover, QComboBox:hover, QCheckBox:hover { background: %1; }")
            .arg(Theme::hex(tc.controlBgHover)));
    auto* containerLayout = new QVBoxLayout(m_scrollContainer);
    containerLayout->setContentsMargins(0, 0, 0, 0);
    containerLayout->setSpacing(0);

    setupIdentitySection(m_scrollContainer);
    m_identitySection->setVisible(false); // Identity kept for data binding but hidden
    setupShotSection(m_scrollContainer);
    setupCharacterSection(m_scrollContainer);
    setupAnimationSection(m_scrollContainer);
    setupSpineSection(m_scrollContainer);   // legacy hidden placeholder
    setupPuppetSection(m_scrollContainer);
    setupVideoSection(m_scrollContainer);
    setupAudioSection(m_scrollContainer);
    setupTitleSection(m_scrollContainer);
    setupGraphicSection(m_scrollContainer);
    setupCaptionSection(m_scrollContainer);
    setupTransformSection(m_scrollContainer);
    setupEffectsSection(m_scrollContainer);
    setupTransitionSection(m_scrollContainer);

    // Load shared text-appearance presets once all preset combos exist
    // (caption/title/graphic sections each created their combo above).
    loadTextPresets();
    rebuildPresetCombos();

    // Make visible QGroupBox sections collapsible (click title to expand/collapse)
    for (auto* w : {m_transformSection, m_videoSection,
                     m_audioSection, m_titleSection, m_graphicSection,
                     m_shotSection, m_effectsSection, m_transitionSection}) {
        if (auto* box = qobject_cast<QGroupBox*>(w))
            makeCollapsible(box);
    }
    makeCollapsible(m_characterSection);
    makeCollapsible(m_animationSection);

    containerLayout->addStretch();
    m_scrollArea->setWidget(m_scrollContainer);
    mainLayout->addWidget(m_scrollArea, 1);

    // ── Empty state label (shown when no clip selected) ─────────────────
    m_emptyLabel = new QLabel(tr("Select a clip to view properties"), this);
    m_emptyLabel->setAlignment(Qt::AlignCenter);
    m_emptyLabel->setStyleSheet(
        QString("QLabel { color: %1; font-size: 13px; background: %2; border: none; }")
            .arg(Theme::hex(tc.textDisabled), Theme::hex(tc.surface0)));
    mainLayout->addWidget(m_emptyLabel, 1);

    // ── Status bar (22px) ───────────────────────────────────────────────
    auto* statusBar = new QWidget(this);
    statusBar->setFixedHeight(22);
    statusBar->setStyleSheet(QStringLiteral(
        "QWidget { background: %1; border-top: 1px solid %2; }")
        .arg(Theme::hex(tc.surface2), Theme::hex(tc.panelBorder)));
    auto* statusLayout = new QHBoxLayout(statusBar);
    statusLayout->setContentsMargins(m.spacingMd, 0, m.spacingMd, 0);
    statusLayout->setSpacing(0);

    m_statusLabel = new QLabel(QStringLiteral("No clip"), statusBar);
    m_statusLabel->setStyleSheet(
        QStringLiteral("QLabel { color: %1; font-size: 12px; background: transparent; border: none; }")
            .arg(Theme::hex(tc.textSecondary)));
    statusLayout->addWidget(m_statusLabel);
    statusLayout->addStretch();
    mainLayout->addWidget(statusBar);

    // Start with empty state visible
    m_scrollArea->setVisible(false);
    m_emptyLabel->setVisible(true);
    showSectionsForType();
}

void PropertiesPanel::setupIdentitySection(QWidget* container)
{
    const auto& m = Theme::metrics();
    m_identitySection = new QGroupBox("Identity", container);
    auto* form = new QFormLayout(m_identitySection);
    form->setContentsMargins(m.spacingXs, 18, m.spacingXs, m.spacingXs);
    form->setSpacing(m.spacingSm);

    m_labelEdit = new QLineEdit(m_identitySection);
    m_labelEdit->setToolTip(tr("Clip display name"));
    m_labelEdit->setPlaceholderText("Clip name...");
    connect(m_labelEdit, &QLineEdit::editingFinished,
            this, &PropertiesPanel::applyLabel);
    form->addRow("Label:", m_labelEdit);

    m_enabledCheck = new QCheckBox("Enabled", m_identitySection);
    m_enabledCheck->setToolTip(tr("Toggle clip visibility on the timeline"));
    connect(m_enabledCheck, &QCheckBox::toggled,
            this, &PropertiesPanel::applyEnabled);
    form->addRow(m_enabledCheck);

    m_speedSpin = createScrubby(1.0, 10000.0, 1.0, 0, " %");
    m_speedSpin->setToolTip(tr("Playback speed percentage (100% = normal). Duration adjusts automatically."));
    connect(m_speedSpin, &ScrubbySpinBox::valueCommitted,
            this, [this](double, double) { applySpeed(); });
    connect(m_speedSpin, &QDoubleSpinBox::editingFinished,
            this, &PropertiesPanel::applySpeed);
    form->addRow("Speed:", m_speedSpin);

    container->layout()->addWidget(m_identitySection);
}

void PropertiesPanel::setupTransformSection(QWidget* container)
{
    const auto& m = Theme::metrics();
    m_transformSection = new QGroupBox("Transform", container);

    auto makeSeparator = [this]() {
        auto* line = new QFrame(m_transformSection);
        line->setFrameShape(QFrame::HLine);
        line->setFrameShadow(QFrame::Plain);
        line->setStyleSheet(QStringLiteral("QFrame { color: %1; max-height: 1px; }")
            .arg(Theme::hex(Theme::colors().border)));
        return line;
    };

    auto makeAxisLabel = [this](const QString& text) {
        auto* lbl = new QLabel(text, m_transformSection);
        lbl->setFixedWidth(14);
        lbl->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        lbl->setStyleSheet(QStringLiteral("QLabel { color: %1; font-size: 11px; font-weight: bold; }")
            .arg(Theme::hex(Theme::colors().textTertiary)));
        return lbl;
    };

    auto* grid = new QGridLayout(m_transformSection);
    grid->setContentsMargins(m.spacingXs, 18, m.spacingXs, m.spacingXs);
    grid->setHorizontalSpacing(m.spacingXs);
    grid->setVerticalSpacing(m.spacingXs);

    int row = 0;

    // ── Position ────────────────────────────────────────────────────
    auto* posLabel = new QLabel("Position", m_transformSection);
    posLabel->setStyleSheet(QStringLiteral("QLabel { color: %1; font-weight: bold; }")
        .arg(Theme::hex(Theme::colors().textPrimary)));
    m_posXSpin = createScrubby(-1000000.0, 1000000.0, 1.0, 1);
    m_posXSpin->setToolTip(tr("Horizontal position in pixels"));
    m_posYSpin = createScrubby(-1000000.0, 1000000.0, 1.0, 1);
    m_posYSpin->setToolTip(tr("Vertical position in pixels"));
    grid->addWidget(posLabel,              row, 0);
    grid->addWidget(makeAxisLabel("X"),    row, 1);
    grid->addWidget(m_posXSpin,            row, 2);
    grid->addWidget(makeAxisLabel("Y"),    row, 3);
    grid->addWidget(m_posYSpin,            row, 4);
    ++row;
    grid->addWidget(makeSeparator(), row, 0, 1, 5);
    ++row;

    // ── Scale ───────────────────────────────────────────────────────
    auto* scaleLabel = new QLabel("Scale", m_transformSection);
    scaleLabel->setStyleSheet(QStringLiteral("QLabel { color: %1; font-weight: bold; }")
        .arg(Theme::hex(Theme::colors().textPrimary)));
    m_scaleXSpin = createScrubby(-1000.0, 1000.0, 0.1, 1);
    m_scaleXSpin->setToolTip(tr("Horizontal scale percentage (negative = flip horizontally)"));
    m_scaleYSpin = createScrubby(-1000.0, 1000.0, 0.1, 1);
    m_scaleYSpin->setToolTip(tr("Vertical scale percentage (negative = flip vertically)"));
    grid->addWidget(scaleLabel,            row, 0);
    grid->addWidget(makeAxisLabel("W"),    row, 1);
    grid->addWidget(m_scaleXSpin,          row, 2);
    grid->addWidget(makeAxisLabel("H"),    row, 3);
    grid->addWidget(m_scaleYSpin,          row, 4);
    ++row;

    // ── Flip (negates the corresponding scale axis) ─────────────────
    m_flipHCheck = new QCheckBox(tr("Flip Horizontal"), m_transformSection);
    m_flipHCheck->setToolTip(tr("Mirror the clip left/right (negative horizontal scale)"));
    m_flipVCheck = new QCheckBox(tr("Flip Vertical"), m_transformSection);
    m_flipVCheck->setToolTip(tr("Mirror the clip top/bottom (negative vertical scale)"));
    grid->addWidget(m_flipHCheck, row, 0, 1, 2);
    grid->addWidget(m_flipVCheck, row, 2, 1, 3);
    ++row;
    grid->addWidget(makeSeparator(), row, 0, 1, 5);
    ++row;

    // ── Rotation ────────────────────────────────────────────────────
    auto* rotLabel = new QLabel("Rotation", m_transformSection);
    rotLabel->setStyleSheet(QStringLiteral("QLabel { color: %1; font-weight: bold; }")
        .arg(Theme::hex(Theme::colors().textPrimary)));
    m_rotationSpin = createScrubby(-3600.0, 3600.0, 0.1, 1, QStringLiteral("\u00B0"));
    m_rotationSpin->setToolTip(tr("Rotation in degrees"));
    grid->addWidget(rotLabel,        row, 0);
    grid->addWidget(m_rotationSpin,  row, 1, 1, 4);
    ++row;
    grid->addWidget(makeSeparator(), row, 0, 1, 5);
    ++row;

    // ── Opacity ─────────────────────────────────────────────────────
    auto* opLabel = new QLabel("Opacity", m_transformSection);
    opLabel->setStyleSheet(QStringLiteral("QLabel { color: %1; font-weight: bold; }")
        .arg(Theme::hex(Theme::colors().textPrimary)));
    m_opacitySpin = createScrubby(0.0, 100.0, 0.1, 1, "%");
    m_opacitySpin->setToolTip(tr("Clip opacity percentage"));
    grid->addWidget(opLabel,        row, 0);
    grid->addWidget(m_opacitySpin,  row, 1, 1, 4);
    ++row;
    grid->addWidget(makeSeparator(), row, 0, 1, 5);
    ++row;

    // ── Crop ────────────────────────────────────────────────────────
    auto* cropLabel = new QLabel("Crop", m_transformSection);
    cropLabel->setStyleSheet(QStringLiteral("QLabel { color: %1; font-weight: bold; }")
        .arg(Theme::hex(Theme::colors().textPrimary)));
    grid->addWidget(cropLabel, row, 0);
    ++row;

    // Left / Right
    auto* cropLLabel = new QLabel("L", m_transformSection);
    cropLLabel->setFixedWidth(14);
    cropLLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    cropLLabel->setStyleSheet(QStringLiteral("QLabel { color: %1; font-size: 11px; font-weight: bold; }")
        .arg(Theme::hex(Theme::colors().textTertiary)));
    m_tfCropLeftSpin = createScrubby(0.0, 100.0, 0.5, 1, "%");
    m_tfCropLeftSpin->setToolTip(tr("Crop from the left edge (%)"));
    auto* cropRLabel = new QLabel("R", m_transformSection);
    cropRLabel->setFixedWidth(14);
    cropRLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    cropRLabel->setStyleSheet(QStringLiteral("QLabel { color: %1; font-size: 11px; font-weight: bold; }")
        .arg(Theme::hex(Theme::colors().textTertiary)));
    m_tfCropRightSpin = createScrubby(0.0, 100.0, 0.5, 1, "%");
    m_tfCropRightSpin->setToolTip(tr("Crop from the right edge (%)"));
    grid->addWidget(cropLLabel,         row, 1);
    grid->addWidget(m_tfCropLeftSpin,   row, 2);
    grid->addWidget(cropRLabel,         row, 3);
    grid->addWidget(m_tfCropRightSpin,  row, 4);
    ++row;

    // Top / Bottom
    auto* cropTLabel = new QLabel("T", m_transformSection);
    cropTLabel->setFixedWidth(14);
    cropTLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    cropTLabel->setStyleSheet(QStringLiteral("QLabel { color: %1; font-size: 11px; font-weight: bold; }")
        .arg(Theme::hex(Theme::colors().textTertiary)));
    m_tfCropTopSpin = createScrubby(0.0, 100.0, 0.5, 1, "%");
    m_tfCropTopSpin->setToolTip(tr("Crop from the top edge (%)"));
    auto* cropBLabel = new QLabel("B", m_transformSection);
    cropBLabel->setFixedWidth(14);
    cropBLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    cropBLabel->setStyleSheet(QStringLiteral("QLabel { color: %1; font-size: 11px; font-weight: bold; }")
        .arg(Theme::hex(Theme::colors().textTertiary)));
    m_tfCropBottomSpin = createScrubby(0.0, 100.0, 0.5, 1, "%");
    m_tfCropBottomSpin->setToolTip(tr("Crop from the bottom edge (%)"));
    grid->addWidget(cropTLabel,          row, 1);
    grid->addWidget(m_tfCropTopSpin,     row, 2);
    grid->addWidget(cropBLabel,          row, 3);
    grid->addWidget(m_tfCropBottomSpin,  row, 4);

    // Column stretch: label fixed, axis-labels fixed, values expand equally
    grid->setColumnStretch(0, 0);
    grid->setColumnStretch(1, 0);
    grid->setColumnStretch(2, 1);
    grid->setColumnStretch(3, 0);
    grid->setColumnStretch(4, 1);

    // Connect live scrub + commit signals
    auto connectTransform = [this](ScrubbySpinBox* spin) {
        // Live preview during scrub — update clip properties immediately
        // so the program monitor recomposites on the next poll cycle.
        connect(spin, &ScrubbySpinBox::valueScrubbed,
                this, [this](double) { applyTransformLive(); });
        // Commit with undo support on drag release or Enter.
        // ScrubbySpinBox::onEditingFinished already emits valueCommitted for
        // keyboard entry, so we do NOT connect editingFinished here — doing
        // so would create duplicate undo commands (double-undo bug).
        connect(spin, &ScrubbySpinBox::valueCommitted,
                this, [this, spin](double oldV, double newV) {
                    applyTransform(spin, oldV, newV);
                });
    };
    connectTransform(m_posXSpin);
    connectTransform(m_posYSpin);
    connectTransform(m_scaleXSpin);
    connectTransform(m_scaleYSpin);
    connectTransform(m_rotationSpin);
    connectTransform(m_opacitySpin);
    connectTransform(m_tfCropLeftSpin);
    connectTransform(m_tfCropRightSpin);
    connectTransform(m_tfCropTopSpin);
    connectTransform(m_tfCropBottomSpin);

    // Flip checkboxes drive the sign of the corresponding scale track.
    // A flip = negative scale on that axis (the compositor already treats
    // negative scaleX/Y as a mirror).  NOTE: do NOT route this through
    // applyTransform() — that path uses pushWithoutExecute() and reads the
    // OLD value from each spinbox's scrubStartValue(), which is only set
    // during a scrub-drag.  A checkbox toggle never scrubs, so the new
    // value would never be applied and an undo would restore a stale
    // scrubStartValue() of 0 → scale 0 → the clip vanishes.  Instead we
    // snapshot the whole scale KeyframeTrack and run a real execute-style
    // undoable command (same pattern as applySpeed()).
    auto applyFlip = [this](bool vertical, bool on) {
        if (!m_clip) return;
        auto* clip = m_clip;
        KeyframeTrack<float>& track = vertical ? clip->scaleY()
                                               : clip->scaleX();
        KeyframeTrack<float> before = track;
        KeyframeTrack<float> after  = track;
        // Force the target sign on the default value + every keyframe so a
        // keyframed scale still flips uniformly across the whole clip.
        auto withSign = [on](float v) {
            float m = std::abs(v);
            return on ? -m : m;
        };
        after.setDefaultValue(withSign(after.defaultValue()));
        for (size_t i = 0; i < after.keyframeCount(); ++i)
            after.keyframe(i).value = withSign(after.keyframe(i).value);

        if (before.defaultValue() == after.defaultValue()
            && before.keyframeCount() == 0)
            return; // no-op (already in the requested state)

        if (!m_commandStack) {
            track = after;
            populateFromClip();
            emit propertyChanged();
            return;
        }
        m_commandStack->execute(std::make_unique<LambdaCommand>(
            vertical ? "Flip Vertical" : "Flip Horizontal",
            [clip, vertical, after, this]() {
                (vertical ? clip->scaleY() : clip->scaleX()) = after;
                populateFromClip();
                emit propertyChanged();
            },
            [clip, vertical, before, this]() {
                (vertical ? clip->scaleY() : clip->scaleX()) = before;
                populateFromClip();
                emit propertyChanged();
            }));
    };
    connect(m_flipHCheck, &QCheckBox::toggled, this,
            [applyFlip](bool on) { applyFlip(false, on); });
    connect(m_flipVCheck, &QCheckBox::toggled, this,
            [applyFlip](bool on) { applyFlip(true, on); });

    container->layout()->addWidget(m_transformSection);
}

bool PropertiesPanel::eventFilter(QObject* obj, QEvent* event)
{
    if (event->type() == QEvent::Wheel) {
        // Suppress wheel events on combo boxes to prevent accidental
        // character/outfit/stance/animation changes when scrolling content.
        if (qobject_cast<QComboBox*>(obj))
            return true;
    }
    // The caption Text field is a multi-line wrapping editor (no
    // editingFinished signal), so commit its edit when it loses focus.
    if (event->type() == QEvent::FocusOut && obj == m_capTextEdit)
        applyCaptionText();
    return QWidget::eventFilter(obj, event);
}

void PropertiesPanel::setupCharacterSection(QWidget* container)
{
    const auto& m = Theme::metrics();
    m_characterSection = new QGroupBox("Character", container);
    auto* form = new QFormLayout(m_characterSection);
    form->setContentsMargins(m.spacingMd, 24, m.spacingMd, m.spacingMd);
    form->setSpacing(m.spacingMd);

    // Character dropdown — populated from ModelManager
    m_characterCombo = new QComboBox(m_characterSection);
    m_characterCombo->setToolTip(tr("Select the character for this clip"));
    m_characterCombo->setEditable(false);
    m_characterCombo->setMinimumWidth(150);
    m_characterCombo->installEventFilter(this);
    connect(m_characterCombo, &QComboBox::currentTextChanged,
            this, [this](const QString&) {
                if (m_updating) return;
                applySpineCharacter();
                populateOutfitDropdown();
                populateStanceDropdown();
                populateAnimationDropdown();
            });
    form->addRow("Character:", m_characterCombo);

    // Outfit dropdown — populated from ModelManager metadata
    m_outfitCombo = new QComboBox(m_characterSection);
    m_outfitCombo->setToolTip(tr("Select the character outfit"));
    m_outfitCombo->setEditable(false);
    m_outfitCombo->installEventFilter(this);
    connect(m_outfitCombo, &QComboBox::currentTextChanged,
            this, [this](const QString&) {
                if (m_updating) return;
                applySpineOutfit();
                populateStanceDropdown();
                populateAnimationDropdown();
            });
    form->addRow("Outfit:", m_outfitCombo);

    // Stance dropdown — populated dynamically based on character/outfit
    m_stanceCombo = new QComboBox(m_characterSection);
    m_stanceCombo->setToolTip(tr("Select the character stance / pose"));
    m_stanceCombo->addItem("Default");
    m_stanceCombo->installEventFilter(this);
    connect(m_stanceCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int) {
                if (m_updating) return;
                applySpineStance();
                populateAnimationDropdown();
            });
    form->addRow("Stance:", m_stanceCombo);

    container->layout()->addWidget(m_characterSection);
}

void PropertiesPanel::setupAnimationSection(QWidget* container)
{
    const auto& m = Theme::metrics();
    m_animationSection = new QGroupBox("Animation", container);
    auto* form = new QFormLayout(m_animationSection);
    form->setContentsMargins(m.spacingMd, 24, m.spacingMd, m.spacingMd);
    form->setSpacing(m.spacingMd);

    // Animation dropdown — populated from loaded skeleton's available animations
    m_animationCombo = new QComboBox(m_animationSection);
    m_animationCombo->setToolTip(tr("Select a Spine animation to play"));
    m_animationCombo->setEditable(false);
    m_animationCombo->installEventFilter(this);
    connect(m_animationCombo, &QComboBox::currentTextChanged,
            this, [this](const QString&) {
                if (m_updating) return;
                applySpineAnimation();
            });
    form->addRow("Animation:", m_animationCombo);

    m_loopingCheck = new QCheckBox("Loop", m_animationSection);
    m_loopingCheck->setToolTip(tr("Loop the animation continuously"));
    connect(m_loopingCheck, &QCheckBox::toggled,
            this, [this](bool) { if (!m_updating) applySpineLooping(); });
    form->addRow(m_loopingCheck);

    m_talkingCheck = new QCheckBox("Talking", m_animationSection);
    m_talkingCheck->setToolTip(tr("Enable talking mouth animation"));
    connect(m_talkingCheck, &QCheckBox::toggled,
            this, [this](bool) { if (!m_updating) applySpineTalking(); });
    form->addRow(m_talkingCheck);

    m_animSpeedSpin = new ScrubbySpinBox(m_animationSection);
    m_animSpeedSpin->setToolTip(tr("Animation playback speed multiplier"));
    m_animSpeedSpin->setRange(0.01, 10.0);
    m_animSpeedSpin->setSingleStep(0.01);
    m_animSpeedSpin->setDecimals(3);
    m_animSpeedSpin->setSuffix("x");
    m_animSpeedSpin->setValue(1.0);
    connect(m_animSpeedSpin, &ScrubbySpinBox::valueCommitted,
            this, [this](double, double) { if (!m_updating) applySpineAnimSpeed(); });
    connect(m_animSpeedSpin, &QDoubleSpinBox::editingFinished,
            this, [this]() { if (!m_updating) applySpineAnimSpeed(); });
    form->addRow("Speed:", m_animSpeedSpin);

    m_continuityCheck = new QCheckBox("Seamless across cuts", m_animationSection);
    m_continuityCheck->setToolTip(
        "When enabled, looping animations (idle, walk, etc.) use global timeline time\n"
        "so animation position carries smoothly across cuts between same-character clips.\n"
        "Action/special animations always use clip-local time (reset at each clip start).");
    m_continuityCheck->setChecked(true);
    connect(m_continuityCheck, &QCheckBox::toggled,
            this, [this](bool) { if (!m_updating) applySpineContinuity(); });
    form->addRow(m_continuityCheck);

    container->layout()->addWidget(m_animationSection);
}

// Legacy stub — m_spineSection kept as hidden placeholder for backward compat
void PropertiesPanel::setupSpineSection(QWidget* container)
{
    m_spineSection = new QWidget(container);
    m_spineSection->setVisible(false);
    container->layout()->addWidget(m_spineSection);
}









void PropertiesPanel::setupShotSection(QWidget* container)
{
    const auto& m = Theme::metrics();
    m_shotSection = new QGroupBox("Shot", container);
    auto* form = new QFormLayout(m_shotSection);
    form->setContentsMargins(m.spacingXs, 18, m.spacingXs, m.spacingXs);
    form->setSpacing(m.spacingSm);

    m_shotInfoLabel = new QLabel(m_shotSection);
    m_shotInfoLabel->setWordWrap(true);
    m_shotInfoLabel->setStyleSheet(QStringLiteral("color: %1;")
        .arg(Theme::hex(Theme::colors().textSecondary)));
    form->addRow("Group:", m_shotInfoLabel);

    // Navigation filters — narrow the Shot dropdown by show, then character.
    // These do NOT modify the clip; they only filter which presets are listed.
    m_shotShowCombo = new QComboBox(m_shotSection);
    m_shotShowCombo->setToolTip(tr("Filter shots by show"));
    m_shotShowCombo->setEditable(false);
    connect(m_shotShowCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int) {
                if (m_updating) return;
                populateShotCombo(QString());
            });
    form->addRow("Show:", m_shotShowCombo);

    m_shotCharCombo = new QComboBox(m_shotSection);
    m_shotCharCombo->setToolTip(tr("Filter shots by character"));
    m_shotCharCombo->setEditable(false);
    connect(m_shotCharCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int) {
                if (m_updating) return;
                populateShotCombo(QString());
            });
    form->addRow("Character:", m_shotCharCombo);

    m_shotCombo = new QComboBox(m_shotSection);
    m_shotCombo->setToolTip(tr("Select a camera shot preset"));
    m_shotCombo->setEditable(false);
    // Use activated(int) so selecting the same shot re-applies it.
    connect(m_shotCombo, QOverload<int>::of(&QComboBox::activated),
            this, [this](int /*index*/) {
                if (m_updating) return;
                if (!m_clip) return;
                // Item data carries the full "show/name" key; fall back to the
                // display text for the "-- Choose a shot --" placeholder.
                QString key = m_shotCombo->currentData().toString();
                if (key.isEmpty()) key = m_shotCombo->currentText();
                onShotChanged(key.toStdString());
            });
    form->addRow("Shot:", m_shotCombo);

    // Add to container layout and start hidden — becomes visible when
    // a clip with groupId != 0 is selected (see showSectionsForType()).
    m_shotSection->setVisible(false);
    container->layout()->addWidget(m_shotSection);
}

void PropertiesPanel::setupEffectsSection(QWidget* container)
{
    m_effectsSection = new QGroupBox("Effect Controls", container);

    const auto& m = Theme::metrics();
    auto* fxLayout = new QVBoxLayout(m_effectsSection);
    fxLayout->setContentsMargins(m.spacingXs, 18, m.spacingXs, m.spacingXs);
    fxLayout->setSpacing(0);

    // Hidden list used only for programmatic row tracking (remove operations).
    m_fxList = new QListWidget(m_effectsSection);
    m_fxList->setVisible(false);
    m_fxList->setMaximumHeight(0);

    // Dynamic area — per-effect collapsible groups are built in refreshEffects()
    m_fxParamsLayout = new QVBoxLayout;
    m_fxParamsLayout->setContentsMargins(0, 0, 0, 0);
    m_fxParamsLayout->setSpacing(2);
    fxLayout->addLayout(m_fxParamsLayout);

    fxLayout->addStretch();

    container->layout()->addWidget(m_effectsSection);
}


//  Transition section
// ═════════════════════════════════════════════════════════════════════════════

static const char* transitionTypeName(TransitionType t)
{
    switch (t) {
    case TransitionType::CrossDissolve:     return "Cross Dissolve";
    case TransitionType::FadeToBlack:       return "Fade To Black";
    case TransitionType::FadeFromBlack:     return "Fade From Black";
    case TransitionType::FadeToWhite:       return "Fade To White";
    case TransitionType::FadeFromWhite:     return "Fade From White";
    case TransitionType::WipeLeft:          return "Wipe Left";
    case TransitionType::WipeRight:         return "Wipe Right";
    case TransitionType::WipeUp:            return "Wipe Up";
    case TransitionType::WipeDown:          return "Wipe Down";
    case TransitionType::PushLeft:          return "Push Left";
    case TransitionType::PushRight:         return "Push Right";
    case TransitionType::PushUp:            return "Push Up";
    case TransitionType::PushDown:          return "Push Down";
    case TransitionType::DipToBlack:        return "Dip To Black";
    case TransitionType::DipToWhite:        return "Dip To White";
    case TransitionType::FilmDissolve:      return "Film Dissolve";
    case TransitionType::AdditiveDissolve:  return "Additive Dissolve";
    case TransitionType::BarnDoor:          return "Barn Door";
    case TransitionType::ClockWipe:         return "Clock Wipe";
    case TransitionType::RadialWipe:        return "Radial Wipe";
    case TransitionType::IrisRound:         return "Iris Round";
    case TransitionType::IrisDiamond:       return "Iris Diamond";
    case TransitionType::IrisCross:         return "Iris Cross";
    case TransitionType::DiagonalWipe:      return "Diagonal Wipe";
    case TransitionType::CheckerWipe:       return "Checker Wipe";
    case TransitionType::VenetianBlinds:    return "Venetian Blinds";
    case TransitionType::Inset:             return "Inset";
    case TransitionType::SlideLeft:         return "Slide Left";
    case TransitionType::SlideRight:        return "Slide Right";
    case TransitionType::SlideUp:           return "Slide Up";
    case TransitionType::SlideDown:         return "Slide Down";
    case TransitionType::Split:             return "Split";
    case TransitionType::CenterSplit:       return "Center Split";
    case TransitionType::Swap:              return "Swap";
    case TransitionType::Zoom:              return "Zoom";
    case TransitionType::CrossZoom:         return "Cross Zoom";
    case TransitionType::WhipPan:           return "Whip Pan";
    case TransitionType::RandomBlocks:      return "Random Blocks";
    case TransitionType::MorphCut:          return "Morph Cut";
    case TransitionType::GradientWipe:      return "Gradient Wipe";
    }
    return "Unknown";
}

void PropertiesPanel::setupTransitionSection(QWidget* container)
{
    const auto& m = Theme::metrics();
    m_transitionSection = new QGroupBox("Transition", container);
    auto* form = new QFormLayout(m_transitionSection);
    form->setContentsMargins(m.spacingXs, 18, m.spacingXs, m.spacingXs);
    form->setSpacing(m.spacingSm);

    // Type combo
    m_transTypeCombo = new QComboBox(m_transitionSection);
    m_transTypeCombo->setToolTip(tr("Transition effect type"));
    for (int i = 0; i <= static_cast<int>(TransitionType::GradientWipe); ++i)
        m_transTypeCombo->addItem(transitionTypeName(static_cast<TransitionType>(i)));
    connect(m_transTypeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int) { applyTransitionType(); });
    form->addRow("Type:", m_transTypeCombo);

    // Duration (in frames)
    m_transDurationSpin = createScrubby(1.0, 300.0, 1.0, 0, " f");
    m_transDurationSpin->setToolTip(tr("Transition duration in frames"));
    connect(m_transDurationSpin, &ScrubbySpinBox::valueCommitted,
            this, [this](double, double) { applyTransitionDuration(); });
    connect(m_transDurationSpin, &QDoubleSpinBox::editingFinished,
            this, &PropertiesPanel::applyTransitionDuration);
    form->addRow("Duration:", m_transDurationSpin);

    // Edge softness (0 – 100 %)
    m_transSoftnessSpin = createScrubby(0.0, 100.0, 0.5, 1, "%");
    m_transSoftnessSpin->setToolTip(tr("Edge softness percentage"));
    connect(m_transSoftnessSpin, &ScrubbySpinBox::valueCommitted,
            this, [this](double, double) { applyTransitionSoftness(); });
    connect(m_transSoftnessSpin, &QDoubleSpinBox::editingFinished,
            this, &PropertiesPanel::applyTransitionSoftness);
    form->addRow("Edge Softness:", m_transSoftnessSpin);

    // Alignment
    m_transAlignCombo = new QComboBox(m_transitionSection);
    m_transAlignCombo->setToolTip(tr("Alignment relative to the cut point"));
    m_transAlignCombo->addItem("Start at Cut");
    m_transAlignCombo->addItem("Center on Cut");
    m_transAlignCombo->addItem("End at Cut");
    connect(m_transAlignCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int) { applyTransitionAlignment(); });
    form->addRow("Alignment:", m_transAlignCombo);

    // Clip info labels (read-only)
    m_transClipALabel = new QLabel(m_transitionSection);
    m_transClipALabel->setStyleSheet(QStringLiteral("QLabel { color: %1; font-size: 11px; }")
        .arg(Theme::hex(Theme::colors().textSecondary)));
    form->addRow("Clip A:", m_transClipALabel);

    m_transClipBLabel = new QLabel(m_transitionSection);
    m_transClipBLabel->setStyleSheet(QStringLiteral("QLabel { color: %1; font-size: 11px; }")
        .arg(Theme::hex(Theme::colors().textSecondary)));
    form->addRow("Clip B:", m_transClipBLabel);

    m_transitionSection->setVisible(false);
    container->layout()->addWidget(m_transitionSection);
}


} // namespace rt
