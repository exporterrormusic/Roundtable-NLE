/*
 * ShotPanel.cpp — Dedicated dock panel for shot/character properties.
 */

#include "panels/characters/ShotPanel.h"

#include "Theme.h"

#include "timeline/Clip.h"
#include "timeline/SpineClip.h"
#include "spine/ModelManager.h"
#include "spine/ShotPreset.h"
#include "command/CommandStack.h"
#include "command/LambdaCommand.h"
#include "widgets/ScrubbySpinBox.h"

#include <QFormLayout>
#include <QGroupBox>
#include <QScrollArea>
#include <QWheelEvent>

#include <spdlog/spdlog.h>
#include <chrono>

namespace rt {

// ═════════════════════════════════════════════════════════════════════════════
//  Construction
// ═════════════════════════════════════════════════════════════════════════════

ShotPanel::ShotPanel(QWidget* parent)
    : QWidget(parent)
{
    setupUI();
}

ShotPanel::~ShotPanel() = default;

QSize ShotPanel::sizeHint() const { return {260, 400}; }

// ═════════════════════════════════════════════════════════════════════════════
//  UI Setup
// ═════════════════════════════════════════════════════════════════════════════

void ShotPanel::setupUI()
{
    const auto& tc = Theme::colors();
    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(0);
    setStyleSheet(QStringLiteral("background: %1;").arg(Theme::hex(tc.surface1)));

    // ── Header bar (matches Lumetri Color style) ────────────────────────
    {
        auto* headerBar = new QWidget(this);
        headerBar->setFixedHeight(28);
        headerBar->setStyleSheet(QStringLiteral(
            "background: %1; border-bottom: 1px solid %2;")
            .arg(Theme::hex(tc.surface3), Theme::hex(tc.border)));
        auto* hl = new QHBoxLayout(headerBar);
        hl->setContentsMargins(8, 0, 8, 0);
        hl->setSpacing(6);

        m_headerLabel = new QLabel(this);
        m_headerLabel->setStyleSheet(QStringLiteral(
            "color: %1; font-size: 12px; background: transparent;")
            .arg(Theme::hex(tc.textSecondary)));
        hl->addWidget(m_headerLabel);
        hl->addStretch();

        mainLayout->addWidget(headerBar);
    }

    // Scroll area
    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setStyleSheet(QStringLiteral(
        "QScrollArea { background: %1; border: none; }")
        .arg(Theme::hex(tc.surface1)));

    auto* container = new QWidget;
    auto* containerLayout = new QVBoxLayout(container);
    containerLayout->setContentsMargins(12, 8, 12, 8);
    containerLayout->setSpacing(Theme::metrics().spacingMd);

    setupCharacterSection(container);
    setupAnimationSection(container);
    setupShotGroupSection(container);

    // Empty state
    m_emptyLabel = new QLabel("Select a Spine clip to edit shot properties.", container);
    m_emptyLabel->setAlignment(Qt::AlignCenter);
    m_emptyLabel->setWordWrap(true);
    m_emptyLabel->setStyleSheet(QStringLiteral("color: %1; font-size: 14px; padding: 24px;")
        .arg(Theme::hex(Theme::colors().textTertiary)));
    containerLayout->addWidget(m_emptyLabel);

    containerLayout->addStretch();
    scroll->setWidget(container);
    mainLayout->addWidget(scroll, 1);

    showSectionsForType();
}

bool ShotPanel::eventFilter(QObject* obj, QEvent* event)
{
    if (event->type() == QEvent::Wheel) {
        // Suppress wheel events on combo boxes to prevent accidental
        // character/outfit/stance/animation changes when scrolling content.
        if (qobject_cast<QComboBox*>(obj))
            return true;
    }
    return QWidget::eventFilter(obj, event);
}

void ShotPanel::setupCharacterSection(QWidget* container)
{
    const auto& m = Theme::metrics();
    m_characterSection = new QGroupBox("Character", container);
    auto* form = new QFormLayout(m_characterSection);
    form->setContentsMargins(m.spacingMd, 24, m.spacingMd, m.spacingMd);
    form->setSpacing(m.spacingMd);

    // Character dropdown — populated from ModelManager
    m_characterCombo = new QComboBox(m_characterSection);
    m_characterCombo->setEditable(false);
    m_characterCombo->setMinimumWidth(150);
    m_characterCombo->installEventFilter(this);
    connect(m_characterCombo, &QComboBox::currentTextChanged,
            this, [this](const QString&) {
                if (m_updating) return;
                applyCharacter();
                // Refresh outfit & animation dropdowns for the new character
                populateOutfitDropdown();
                populateStanceDropdown();
                populateAnimationDropdown();
            });
    form->addRow("Character:", m_characterCombo);

    // Outfit dropdown — populated from ModelManager metadata
    m_outfitCombo = new QComboBox(m_characterSection);
    m_outfitCombo->setEditable(false);
    m_outfitCombo->installEventFilter(this);
    connect(m_outfitCombo, &QComboBox::currentTextChanged,
            this, [this](const QString&) {
                if (m_updating) return;
                applyOutfit();
                populateStanceDropdown();
                populateAnimationDropdown();
            });
    form->addRow("Outfit:", m_outfitCombo);

    // Stance dropdown — populated dynamically based on character/outfit
    m_stanceCombo = new QComboBox(m_characterSection);
    m_stanceCombo->addItem("Default");  // placeholder; repopulated on clip load
    m_stanceCombo->installEventFilter(this);
    connect(m_stanceCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int) {
                if (m_updating) return;
                applyStance();
                populateAnimationDropdown();
            });
    form->addRow("Stance:", m_stanceCombo);

    container->layout()->addWidget(m_characterSection);
}

void ShotPanel::setupAnimationSection(QWidget* container)
{
    const auto& m = Theme::metrics();
    m_animationSection = new QGroupBox("Animation", container);
    auto* form = new QFormLayout(m_animationSection);
    form->setContentsMargins(m.spacingMd, 24, m.spacingMd, m.spacingMd);
    form->setSpacing(m.spacingMd);

    // Animation dropdown — populated from loaded skeleton's available animations
    m_animationCombo = new QComboBox(m_animationSection);
    m_animationCombo->setEditable(false);
    m_animationCombo->installEventFilter(this);
    connect(m_animationCombo, &QComboBox::currentTextChanged,
            this, [this](const QString&) {
                if (m_updating) return;
                applyAnimation();
            });
    form->addRow("Animation:", m_animationCombo);

    m_loopingCheck = new QCheckBox("Loop", m_animationSection);
    connect(m_loopingCheck, &QCheckBox::toggled,
            this, [this](bool) { if (!m_updating) applyLooping(); });
    form->addRow(m_loopingCheck);

    m_talkingCheck = new QCheckBox("Talking", m_animationSection);
    connect(m_talkingCheck, &QCheckBox::toggled,
            this, [this](bool) { if (!m_updating) applyTalking(); });
    form->addRow(m_talkingCheck);

    m_animSpeedSpin = new ScrubbySpinBox(m_animationSection);
    m_animSpeedSpin->setRange(0.01, 10.0);
    m_animSpeedSpin->setSingleStep(0.01);
    m_animSpeedSpin->setDecimals(3);
    m_animSpeedSpin->setSuffix("x");
    m_animSpeedSpin->setValue(1.0);
    connect(m_animSpeedSpin, &ScrubbySpinBox::valueCommitted,
            this, [this](double, double) { if (!m_updating) applyAnimSpeed(); });
    connect(m_animSpeedSpin, &QDoubleSpinBox::editingFinished,
            this, [this]() { if (!m_updating) applyAnimSpeed(); });
    form->addRow("Speed:", m_animSpeedSpin);

    // Continuity checkbox — when checked, looping animations use global
    // timeline time so the animation loop carries across cuts between
    // same-character clips (ported from Python codebase pattern).
    m_continuityCheck = new QCheckBox("Seamless across cuts", m_animationSection);
    m_continuityCheck->setToolTip(
        "When enabled, looping animations (idle, walk, etc.) use global timeline time\n"
        "so animation position carries smoothly across cuts between same-character clips.\n"
        "Action/special animations always use clip-local time (reset at each clip start).");
    m_continuityCheck->setChecked(true);
    connect(m_continuityCheck, &QCheckBox::toggled,
            this, [this](bool) { if (!m_updating) applyContinuity(); });
    form->addRow(m_continuityCheck);

    container->layout()->addWidget(m_animationSection);
}

void ShotPanel::setupShotGroupSection(QWidget* container)
{
    const auto& m = Theme::metrics();
    m_shotGroupSection = new QGroupBox("Shot Group", container);
    auto* form = new QFormLayout(m_shotGroupSection);
    form->setContentsMargins(m.spacingMd, 24, m.spacingMd, m.spacingMd);
    form->setSpacing(m.spacingMd);

    m_shotInfoLabel = new QLabel(m_shotGroupSection);
    m_shotInfoLabel->setWordWrap(true);
    m_shotInfoLabel->setStyleSheet(QStringLiteral("color: %1;")
        .arg(Theme::hex(Theme::colors().textSecondary)));
    form->addRow("Group:", m_shotInfoLabel);

    m_shotCombo = new QComboBox(m_shotGroupSection);
    m_shotCombo->setEditable(false);
    // Use activated(int) instead of currentTextChanged so selecting the
    // same shot from the dropdown re-applies it (resets the shot).  Qt's
    // currentTextChanged only fires when the text actually changes, so
    // clicking the currently-active entry in the popup was a silent no-op.
    connect(m_shotCombo, QOverload<int>::of(&QComboBox::activated),
            this, [this](int /*index*/) {
                if (m_updating) return;
                if (!m_clip || m_clip->groupId() == 0) return;
                QString text = m_shotCombo->currentText();
                onShotChanged(text.toStdString());
            });
    form->addRow("Preset:", m_shotCombo);

    container->layout()->addWidget(m_shotGroupSection);
}

// ═════════════════════════════════════════════════════════════════════════════
//  Dropdown population
// ═════════════════════════════════════════════════════════════════════════════

void ShotPanel::populateCharacterDropdown()
{
    if (!m_characterCombo) return;

    m_characterCombo->blockSignals(true);
    QString current = m_characterCombo->currentText();
    m_characterCombo->clear();

    if (m_modelManager) {
        auto names = m_modelManager->characterNames();
        for (const auto& name : names) {
            QString dispName = QString::fromStdString(m_modelManager->getDisplayName(name));
            m_characterCombo->addItem(dispName, QString::fromStdString(name));
        }
    }

    // Restore or set current
    if (!current.isEmpty()) {
        // Try matching by folder name in data, then by text
        int idx = m_characterCombo->findData(current);
        if (idx < 0)
            idx = m_characterCombo->findText(current, Qt::MatchFixedString);
        if (idx >= 0) {
            m_characterCombo->setCurrentIndex(idx);
        } else {
            // Character not in the list (maybe typed manually) — add it
            m_characterCombo->addItem(current, current);
            m_characterCombo->setCurrentText(current);
        }
    }
    m_characterCombo->blockSignals(false);
}

void ShotPanel::populateOutfitDropdown()
{
    if (!m_outfitCombo || !m_spineClip) return;

    m_outfitCombo->blockSignals(true);
    QString current = QString::fromStdString(m_spineClip->outfit());
    m_outfitCombo->clear();

    if (m_modelManager) {
        auto outfits = m_modelManager->getMetadataOutfits(m_spineClip->characterName());
        for (const auto& outfit : outfits) {
            m_outfitCombo->addItem(QString::fromStdString(outfit.key));
        }
    }

    // Ensure current outfit is in the list
    if (m_outfitCombo->findText(current) < 0) {
        m_outfitCombo->addItem(current);
    }
    m_outfitCombo->setCurrentText(current);
    m_outfitCombo->blockSignals(false);
}

void ShotPanel::populateStanceDropdown()
{
    if (!m_stanceCombo || !m_spineClip) return;

    m_stanceCombo->blockSignals(true);
    int currentIdx = m_stanceCombo->currentIndex();
    QString currentText = m_stanceCombo->currentText();
    m_stanceCombo->clear();

    // Always add Default (every character has it)
    m_stanceCombo->addItem("Default");

    // Check which additional stances exist on disk via ModelManager
    if (m_modelManager) {
        auto* aim = m_modelManager->findVariant(
            m_spineClip->characterName(), m_spineClip->outfit(),
            CharacterStance::Aim);
        if (aim) m_stanceCombo->addItem("Aim");

        auto* cover = m_modelManager->findVariant(
            m_spineClip->characterName(), m_spineClip->outfit(),
            CharacterStance::Cover);
        if (cover) m_stanceCombo->addItem("Cover");
    } else {
        // No model manager — show all stances as fallback
        m_stanceCombo->addItem("Aim");
        m_stanceCombo->addItem("Cover");
    }

    // Restore previous selection
    int idx = m_stanceCombo->findText(currentText);
    if (idx >= 0) {
        m_stanceCombo->setCurrentIndex(idx);
    } else {
        m_stanceCombo->setCurrentIndex(0); // Default
    }
    m_stanceCombo->blockSignals(false);
}

void ShotPanel::populateAnimationDropdown()
{
    if (!m_animationCombo || !m_spineClip) return;

    m_animationCombo->blockSignals(true);
    QString current = QString::fromStdString(m_spineClip->animationName());
    m_animationCombo->clear();

    // Get animation names from the provider (queries TimelineWorkspace's
    // spine shared cache — skeletons are already in memory from project load).
    bool hasSkeletonAnims = false;

    if (m_animNamesProvider) {
        auto names = m_animNamesProvider(
            m_spineClip->characterName(),
            m_spineClip->outfit(),
            static_cast<int>(m_spineClip->stance()));
        if (!names.empty()) {
            hasSkeletonAnims = true;
            for (const auto& name : names)
                m_animationCombo->addItem(QString::fromStdString(name));
        }
    }

    // Last-resort fallback: common NIKKE animation names.
    // (Only reached if character files are missing from disk.)
    if (!hasSkeletonAnims) {
        QStringList commonAnims = {
            "idle", "action", "angry", "sad", "delight",
            "smile", "shy", "surprise", "special",
            "cry", "pain", "think", "expression_0"};
        for (const auto& anim : commonAnims)
            m_animationCombo->addItem(anim);
    }

    // If current animation isn't in the list, add it
    if (m_animationCombo->findText(current) < 0) {
        m_animationCombo->addItem(current);
    }
    m_animationCombo->setCurrentText(current);
    m_animationCombo->blockSignals(false);
}

// ═════════════════════════════════════════════════════════════════════════════
//  Visibility
// ═════════════════════════════════════════════════════════════════════════════

void ShotPanel::showSectionsForType()
{
    bool isSpine = (m_spineClip != nullptr);

    m_characterSection->setVisible(isSpine);
    m_animationSection->setVisible(isSpine);
    m_emptyLabel->setVisible(!isSpine);

    // Shot group: only visible when clip belongs to a shot group
    bool hasGroup = (m_clip && m_clip->groupId() != 0 && !m_clip->shotName().empty());
    m_shotGroupSection->setVisible(hasGroup);
}

// ═════════════════════════════════════════════════════════════════════════════
//  Clip binding
// ═════════════════════════════════════════════════════════════════════════════

void ShotPanel::setClip(Clip* clip, Track* track)
{
    auto t0 = std::chrono::steady_clock::now();
    m_clip  = clip;
    m_track = track;
    m_spineClip = (clip && clip->clipType() == ClipType::Spine)
                      ? static_cast<SpineClip*>(clip) : nullptr;

    showSectionsForType();

    if (m_spineClip) {
        auto t1 = std::chrono::steady_clock::now();
        populateCharacterDropdown();
        auto t2 = std::chrono::steady_clock::now();
        populateFromClip();
        auto t3 = std::chrono::steady_clock::now();
        spdlog::info("ShotPanel::setClip  charDropdown={:.1f}ms  populateFromClip={:.1f}ms  total={:.1f}ms",
            std::chrono::duration<double, std::milli>(t2 - t1).count(),
            std::chrono::duration<double, std::milli>(t3 - t2).count(),
            std::chrono::duration<double, std::milli>(t3 - t0).count());
    }
}

void ShotPanel::clearClip()
{
    m_clip = nullptr;
    m_spineClip = nullptr;
    m_track = nullptr;
    showSectionsForType();
    m_headerLabel->setText("");
}

void ShotPanel::setMultiSelection(const std::vector<Clip*>& clips)
{
    if (clips.empty()) {
        clearClip();
        return;
    }
    if (clips.size() == 1) {
        setClip(clips.front());
        return;
    }

    // Check if all clips share the same non-zero groupId (i.e. a shot group).
    uint64_t commonGroup = clips.front()->groupId();
    bool allSameGroup = (commonGroup != 0);
    for (size_t i = 1; i < clips.size() && allSameGroup; ++i) {
        if (clips[i]->groupId() != commonGroup)
            allSameGroup = false;
    }

    if (allSameGroup) {
        // Use the first clip as representative.
        m_clip = clips.front();
        m_track = nullptr;
        m_spineClip = (m_clip->clipType() == ClipType::Spine)
                          ? static_cast<SpineClip*>(m_clip) : nullptr;

        m_headerLabel->setText(QString("Group %1 (%2 clips)")
                                   .arg(commonGroup)
                                   .arg(clips.size()));

        // Show shot group section, hide single-clip character/animation
        m_characterSection->setVisible(false);
        m_animationSection->setVisible(false);
        m_emptyLabel->setVisible(false);
        updateShotSection();
    } else {
        // Mixed selection — clear
        clearClip();
    }
}

void ShotPanel::refresh()
{
    if (m_spineClip) {
        populateFromClip();
    }
}

void ShotPanel::populateFromClip()
{
    if (!m_spineClip) return;
    auto pfcStart = std::chrono::steady_clock::now();

    m_updating = true;

    // Header
    m_headerLabel->setText(QString::fromStdString(m_clip->label()));

    // Character
    // Character — find by folder name in item data
    {
        int idx = m_characterCombo->findData(
            QString::fromStdString(m_spineClip->characterName()));
        if (idx >= 0)
            m_characterCombo->setCurrentIndex(idx);
        else
            m_characterCombo->setCurrentText(
                QString::fromStdString(m_spineClip->characterName()));
    }

    // Outfit
    auto tOutfit0 = std::chrono::steady_clock::now();
    populateOutfitDropdown();
    auto tOutfit1 = std::chrono::steady_clock::now();

    // Stance — populate available stances, then set current
    populateStanceDropdown();
    auto tStance1 = std::chrono::steady_clock::now();
    {
        // Map enum to combo text
        int stanceIdx = 0;
        switch (m_spineClip->stance()) {
            case CharacterStance::Default: stanceIdx = m_stanceCombo->findText("Default"); break;
            case CharacterStance::Aim:     stanceIdx = m_stanceCombo->findText("Aim"); break;
            case CharacterStance::Cover:   stanceIdx = m_stanceCombo->findText("Cover"); break;
        }
        m_stanceCombo->setCurrentIndex(stanceIdx >= 0 ? stanceIdx : 0);
    }

    // Animation
    auto tAnim0 = std::chrono::steady_clock::now();
    populateAnimationDropdown();
    auto tAnim1 = std::chrono::steady_clock::now();

    // Looping
    m_loopingCheck->setChecked(m_spineClip->isLooping());

    // Talking
    m_talkingCheck->setChecked(m_spineClip->isTalking());

    // Anim speed
    m_animSpeedSpin->setValue(m_spineClip->animationSpeed());

    // Continuity
    m_continuityCheck->setChecked(m_spineClip->useGlobalTime());

    // Shot group
    updateShotSection();

    m_updating = false;

    auto pfcEnd = std::chrono::steady_clock::now();
    spdlog::info("ShotPanel::populateFromClip  outfit={:.1f}ms  stance={:.1f}ms  anim={:.1f}ms  total={:.1f}ms",
        std::chrono::duration<double, std::milli>(tOutfit1 - tOutfit0).count(),
        std::chrono::duration<double, std::milli>(tStance1 - tOutfit1).count(),
        std::chrono::duration<double, std::milli>(tAnim1 - tAnim0).count(),
        std::chrono::duration<double, std::milli>(pfcEnd - pfcStart).count());
}

void ShotPanel::updateShotSection()
{
    if (!m_clip || m_clip->groupId() == 0 || m_clip->shotName().empty()) {
        m_shotGroupSection->setVisible(false);
        return;
    }

    m_shotGroupSection->setVisible(true);

    m_shotInfoLabel->setText(
        QString("Group %1 — Layer: %2")
            .arg(m_clip->groupId())
            .arg(QString::fromStdString(m_clip->layerId())));

    m_shotCombo->blockSignals(true);
    m_shotCombo->clear();

    if (m_shotManager) {
        auto names = m_shotManager->presetNames();
        for (auto& name : names)
            m_shotCombo->addItem(QString::fromStdString(name));

        int idx = m_shotCombo->findText(QString::fromStdString(m_clip->shotName()));
        if (idx >= 0) m_shotCombo->setCurrentIndex(idx);
    } else {
        m_shotCombo->addItem(QString::fromStdString(m_clip->shotName()));
        m_shotCombo->setCurrentIndex(0);
    }

    m_shotCombo->blockSignals(false);
}

// ═════════════════════════════════════════════════════════════════════════════
//  Apply changes
// ═════════════════════════════════════════════════════════════════════════════

void ShotPanel::applyCharacter()
{
    if (!m_spineClip) return;
    auto newVal = m_characterCombo->currentData().toString().toStdString();
    if (newVal.empty()) newVal = m_characterCombo->currentText().toStdString();
    if (newVal == m_spineClip->characterName()) return;
    auto oldVal = m_spineClip->characterName();
    auto* sc = m_spineClip;
    if (m_commandStack) {
        m_commandStack->execute(std::make_unique<LambdaCommand>(
            "Change character",
            [sc, newVal, this]() { sc->setCharacterName(newVal); populateFromClip(); emit propertyChanged(); },
            [sc, oldVal, this]() { sc->setCharacterName(oldVal); populateFromClip(); emit propertyChanged(); }));
    } else {
        sc->setCharacterName(newVal);
        emit propertyChanged();
    }
}

void ShotPanel::applyOutfit()
{
    if (!m_spineClip) return;
    auto newVal = m_outfitCombo->currentText().toStdString();
    if (newVal == m_spineClip->outfit()) return;
    auto oldVal = m_spineClip->outfit();
    auto* sc = m_spineClip;
    if (m_commandStack) {
        m_commandStack->execute(std::make_unique<LambdaCommand>(
            "Change outfit",
            [sc, newVal, this]() { sc->setOutfit(newVal); populateFromClip(); emit propertyChanged(); },
            [sc, oldVal, this]() { sc->setOutfit(oldVal); populateFromClip(); emit propertyChanged(); }));
    } else {
        sc->setOutfit(newVal);
        emit propertyChanged();
    }
}

void ShotPanel::applyStance()
{
    if (!m_spineClip) return;
    // Map combo text to enum (combo items are dynamic, can't use index)
    CharacterStance newVal = CharacterStance::Default;
    QString text = m_stanceCombo->currentText();
    if (text == "Aim")   newVal = CharacterStance::Aim;
    if (text == "Cover") newVal = CharacterStance::Cover;

    if (newVal == m_spineClip->stance()) return;
    auto oldVal = m_spineClip->stance();
    auto* sc = m_spineClip;
    if (m_commandStack) {
        m_commandStack->execute(std::make_unique<LambdaCommand>(
            "Change stance",
            [sc, newVal, this]() { sc->setStance(newVal); populateFromClip(); emit propertyChanged(); },
            [sc, oldVal, this]() { sc->setStance(oldVal); populateFromClip(); emit propertyChanged(); }));
    } else {
        sc->setStance(newVal);
        emit propertyChanged();
    }
}

void ShotPanel::applyAnimation()
{
    if (!m_spineClip) return;
    auto newVal = m_animationCombo->currentText().toStdString();
    if (newVal == m_spineClip->animationName()) return;
    auto oldVal = m_spineClip->animationName();
    auto* sc = m_spineClip;
    if (m_commandStack) {
        m_commandStack->execute(std::make_unique<LambdaCommand>(
            "Change animation",
            [sc, newVal, this]() { sc->setAnimationName(newVal); populateFromClip(); emit propertyChanged(); },
            [sc, oldVal, this]() { sc->setAnimationName(oldVal); populateFromClip(); emit propertyChanged(); }));
    } else {
        sc->setAnimationName(newVal);
        emit propertyChanged();
    }
}

void ShotPanel::applyLooping()
{
    if (!m_spineClip) return;
    bool newVal = m_loopingCheck->isChecked();
    if (newVal == m_spineClip->isLooping()) return;
    bool oldVal = m_spineClip->isLooping();
    auto* sc = m_spineClip;
    if (m_commandStack) {
        m_commandStack->execute(std::make_unique<LambdaCommand>(
            "Toggle looping",
            [sc, newVal, this]() { sc->setLooping(newVal); populateFromClip(); emit propertyChanged(); },
            [sc, oldVal, this]() { sc->setLooping(oldVal); populateFromClip(); emit propertyChanged(); }));
    } else {
        sc->setLooping(newVal);
        emit propertyChanged();
    }
}

void ShotPanel::applyTalking()
{
    if (!m_spineClip) return;
    bool newVal = m_talkingCheck->isChecked();
    if (newVal == m_spineClip->isTalking()) return;
    bool oldVal = m_spineClip->isTalking();
    auto* sc = m_spineClip;
    if (m_commandStack) {
        m_commandStack->execute(std::make_unique<LambdaCommand>(
            "Toggle talking",
            [sc, newVal, this]() { sc->setTalking(newVal); populateFromClip(); emit propertyChanged(); },
            [sc, oldVal, this]() { sc->setTalking(oldVal); populateFromClip(); emit propertyChanged(); }));
    } else {
        sc->setTalking(newVal);
        emit propertyChanged();
    }
}

void ShotPanel::applyAnimSpeed()
{
    if (!m_spineClip) return;
    float newVal = static_cast<float>(m_animSpeedSpin->value());
    if (newVal == m_spineClip->animationSpeed()) return;
    float oldVal = m_spineClip->animationSpeed();
    auto* sc = m_spineClip;
    if (m_commandStack) {
        m_commandStack->execute(std::make_unique<LambdaCommand>(
            "Change animation speed",
            [sc, newVal, this]() { sc->setAnimationSpeed(newVal); populateFromClip(); emit propertyChanged(); },
            [sc, oldVal, this]() { sc->setAnimationSpeed(oldVal); populateFromClip(); emit propertyChanged(); }));
    } else {
        sc->setAnimationSpeed(newVal);
        emit propertyChanged();
    }
}

void ShotPanel::applyContinuity()
{
    if (!m_spineClip) return;
    bool newVal = m_continuityCheck->isChecked();
    if (newVal == m_spineClip->useGlobalTime()) return;
    bool oldVal = m_spineClip->useGlobalTime();
    auto* sc = m_spineClip;
    if (m_commandStack) {
        m_commandStack->execute(std::make_unique<LambdaCommand>(
            "Toggle continuity",
            [sc, newVal, this]() { sc->setUseGlobalTime(newVal); populateFromClip(); emit propertyChanged(); },
            [sc, oldVal, this]() { sc->setUseGlobalTime(oldVal); populateFromClip(); emit propertyChanged(); }));
    } else {
        sc->setUseGlobalTime(newVal);
        emit propertyChanged();
    }
}

void ShotPanel::onShotChanged(const std::string& newShotName)
{
    if (!m_clip || m_clip->groupId() == 0) return;
    spdlog::info("ShotPanel: shot switch requested for group {} -> '{}'",
                 m_clip->groupId(), newShotName);
    emit shotSwitchRequested(m_clip->groupId(), newShotName);
}

} // namespace rt
