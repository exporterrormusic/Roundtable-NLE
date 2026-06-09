/*
 * PropertiesPanelPuppet.cpp — PNG puppet property section for PropertiesPanel.
 *
 * Builds the "PNG Puppet" group (Outfit + Action dropdowns, Talking toggle,
 * and the blink/talk/breathe timing sliders) and applies edits via the
 * command stack, mirroring the Spine section's Character → Outfit → Action
 * pattern.
 */

#include "panels/properties/PropertiesPanel.h"
#include "panels/characters/PuppetLibrary.h"
#include "widgets/ScrubbySpinBox.h"
#include "Theme.h"

#include "timeline/Clip.h"
#include "timeline/PngPuppetClip.h"
#include "command/CommandStack.h"
#include "command/LambdaCommand.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>

#include <array>

namespace rt {

namespace {

// Resolve the on-disk puppet folder for a clip from its display name.  Tries
// the sanitized-name shortcut first, then scans all puppets for a matching
// display name.  Returns empty if none found.
QString folderForPuppet(const PngPuppetClip* pc)
{
    if (!pc) return {};
    const QString display = QString::fromStdString(pc->characterName());

    const QString guess = puppetlib::sanitizeFolderName(display);
    PuppetManifest probe;
    if (puppetlib::load(guess, probe))
        return guess;

    for (const QString& folder : puppetlib::listPuppetFolders()) {
        PuppetManifest m;
        if (puppetlib::load(folder, m) && m.displayName == display)
            return folder;
    }
    return {};
}

} // namespace

void PropertiesPanel::setupPuppetSection(QWidget* container)
{
    const auto& m = Theme::metrics();
    m_puppetSection = new QGroupBox(tr("PNG Puppet"), container);
    auto* form = new QFormLayout(m_puppetSection);
    form->setContentsMargins(m.spacingMd, 24, m.spacingMd, m.spacingMd);
    form->setSpacing(m.spacingMd);

    // Outfit (costume) dropdown
    m_puppetOutfitCombo = new QComboBox(m_puppetSection);
    m_puppetOutfitCombo->setToolTip(tr("Costume / outfit"));
    m_puppetOutfitCombo->installEventFilter(this);
    connect(m_puppetOutfitCombo, &QComboBox::currentTextChanged,
            this, [this](const QString&) { if (!m_updating) applyPuppetOutfit(); });
    form->addRow(tr("Outfit:"), m_puppetOutfitCombo);

    // Action (expression / pose) dropdown
    m_puppetActionCombo = new QComboBox(m_puppetSection);
    m_puppetActionCombo->setToolTip(tr("Expression / action"));
    m_puppetActionCombo->installEventFilter(this);
    connect(m_puppetActionCombo, &QComboBox::currentTextChanged,
            this, [this](const QString&) { if (!m_updating) applyPuppetAction(); });
    form->addRow(tr("Action:"), m_puppetActionCombo);

    // Talking toggle
    m_puppetTalkingCheck = new QCheckBox(tr("Talking"), m_puppetSection);
    m_puppetTalkingCheck->setToolTip(tr("Animate the mouth as if speaking"));
    connect(m_puppetTalkingCheck, &QCheckBox::toggled,
            this, [this](bool) { if (!m_updating) applyPuppetTalking(); });
    form->addRow(m_puppetTalkingCheck);

    // Motion timing sliders.  Each commits one undoable change via the generic
    // applyPuppetFloat helper, reading the previous value off the clip.
    // Helper that wires a spin box to applyPuppetFloat with the right setter.
    auto wireFloat = [this, form](const QString& label, ScrubbySpinBox*& spin,
                                  double mn, double mx, double step, int dec,
                                  const QString& suffix, const char* cmdName,
                                  std::function<void(PngPuppetClip*, float)> setter,
                                  std::function<float(PngPuppetClip*)> getter) {
        spin = createScrubby(mn, mx, step, dec, suffix);
        auto commit = [this, spin, cmdName, setter, getter]() {
            if (m_updating || !m_puppetClip) return;
            const float oldVal = getter(m_puppetClip);
            const float newVal = static_cast<float>(spin->value());
            if (newVal == oldVal) return;
            applyPuppetFloat(cmdName, setter, newVal, oldVal);
        };
        connect(spin, &ScrubbySpinBox::valueCommitted, this,
                [commit](double, double) { commit(); });
        connect(spin, &QDoubleSpinBox::editingFinished, this, commit);
        form->addRow(label, spin);
    };

    wireFloat(tr("Talk speed:"), m_puppetTalkSwapSpin, 0.02, 0.6, 0.01, 2, tr(" s"),
              "Change talk speed",
              [](PngPuppetClip* p, float v) { p->setTalkSwapSeconds(v); },
              [](PngPuppetClip* p) { return p->talkSwapSeconds(); });

    wireFloat(tr("Blink every:"), m_puppetBlinkIntervalSpin, 0.5, 15.0, 0.1, 1, tr(" s"),
              "Change blink interval",
              [](PngPuppetClip* p, float v) { p->setBlinkIntervalSeconds(v); },
              [](PngPuppetClip* p) { return p->blinkIntervalSeconds(); });

    wireFloat(tr("Blink length:"), m_puppetBlinkDurSpin, 0.02, 0.6, 0.01, 2, tr(" s"),
              "Change blink length",
              [](PngPuppetClip* p, float v) { p->setBlinkDurationSeconds(v); },
              [](PngPuppetClip* p) { return p->blinkDurationSeconds(); });

    wireFloat(tr("Breathe amount:"), m_puppetBreathAmpSpin, 0.0, 60.0, 0.5, 1, tr(" px"),
              "Change breathe amount",
              [](PngPuppetClip* p, float v) { p->setBreathAmplitude(v); },
              [](PngPuppetClip* p) { return p->breathAmplitude(); });

    wireFloat(tr("Breathe speed:"), m_puppetBreathSpeedSpin, 0.0, 2.0, 0.01, 2, tr("/s"),
              "Change breathe speed",
              [](PngPuppetClip* p, float v) { p->setBreathSpeed(v); },
              [](PngPuppetClip* p) { return p->breathSpeed(); });

    wireFloat(tr("Sway amount:"), m_puppetSwaySpin, 0.0, 40.0, 0.5, 1, tr(" px"),
              "Change sway amount",
              [](PngPuppetClip* p, float v) { p->setSwayAmplitude(v); },
              [](PngPuppetClip* p) { return p->swayAmplitude(); });

    m_puppetSection->setVisible(false);
    container->layout()->addWidget(m_puppetSection);
}

void PropertiesPanel::populateFromPuppet()
{
    if (!m_puppetClip) return;

    // Load the puppet manifest.
    const QString folder = folderForPuppet(m_puppetClip);
    PuppetManifest manifest;
    const bool haveManifest = !folder.isEmpty() && puppetlib::load(folder, manifest);

    // Split the clip's current variant key into outfit + action.
    QString curOutfit, curAction;
    puppetlib::splitVariantKey(QString::fromStdString(m_puppetClip->variant()),
                               curOutfit, curAction);

    // ── Outfit dropdown ──────────────────────────────────────────────────
    m_puppetOutfitCombo->blockSignals(true);
    m_puppetOutfitCombo->clear();
    if (haveManifest) {
        const QStringList outfits = puppetlib::listOutfits(manifest);
        for (const QString& o : outfits)
            m_puppetOutfitCombo->addItem(o);
    }
    if (m_puppetOutfitCombo->findText(curOutfit) < 0)
        m_puppetOutfitCombo->addItem(curOutfit);
    m_puppetOutfitCombo->setCurrentText(curOutfit);
    m_puppetOutfitCombo->blockSignals(false);

    // ── Action dropdown (scoped to current outfit) ───────────────────────
    m_puppetActionCombo->blockSignals(true);
    m_puppetActionCombo->clear();
    if (haveManifest) {
        const QStringList actions = puppetlib::listActions(manifest, curOutfit);
        for (const QString& a : actions)
            m_puppetActionCombo->addItem(a);
    }
    if (m_puppetActionCombo->findText(curAction) < 0)
        m_puppetActionCombo->addItem(curAction);
    m_puppetActionCombo->setCurrentText(curAction);
    m_puppetActionCombo->blockSignals(false);

    m_puppetTalkingCheck->setChecked(m_puppetClip->isTalking());
    m_puppetTalkSwapSpin->setValue(m_puppetClip->talkSwapSeconds());
    m_puppetBlinkIntervalSpin->setValue(m_puppetClip->blinkIntervalSeconds());
    m_puppetBlinkDurSpin->setValue(m_puppetClip->blinkDurationSeconds());
    m_puppetBreathAmpSpin->setValue(m_puppetClip->breathAmplitude());
    m_puppetBreathSpeedSpin->setValue(m_puppetClip->breathSpeed());
    m_puppetSwaySpin->setValue(m_puppetClip->swayAmplitude());
}

void PropertiesPanel::applyPuppetTalking()
{
    if (m_updating || !m_puppetClip) return;
    auto* pc = m_puppetClip;
    const bool newVal = m_puppetTalkingCheck->isChecked();
    if (newVal == pc->isTalking()) return;
    const bool oldVal = pc->isTalking();
    if (m_commandStack) {
        m_commandStack->execute(std::make_unique<LambdaCommand>(
            "Toggle talking",
            [pc, newVal, this]() {
                pc->setTalking(newVal);
                m_updating = true; m_puppetTalkingCheck->setChecked(newVal); m_updating = false;
                emit propertyChanged();
            },
            [pc, oldVal, this]() {
                pc->setTalking(oldVal);
                m_updating = true; m_puppetTalkingCheck->setChecked(oldVal); m_updating = false;
                emit propertyChanged();
            }));
    } else {
        pc->setTalking(newVal);
        emit propertyChanged();
    }
}

void PropertiesPanel::applyPuppetOutfit()
{
    if (m_updating || !m_puppetClip) return;
    auto* pc = m_puppetClip;
    const QString newOutfit = m_puppetOutfitCombo->currentText();

    QString curOutfit, curAction;
    puppetlib::splitVariantKey(QString::fromStdString(pc->variant()),
                               curOutfit, curAction);
    if (newOutfit == curOutfit) return;

    // Resolve the new outfit → pick the same action if it exists, else first.
    const QString folder = folderForPuppet(pc);
    PuppetManifest manifest;
    if (folder.isEmpty() || !puppetlib::load(folder, manifest)) return;

    const QStringList actions = puppetlib::listActions(manifest, newOutfit);
    if (actions.isEmpty()) return;
    const QString newAction = actions.contains(curAction) ? curAction : actions.first();
    const QString newVar = puppetlib::resolveVariantKey(manifest, newOutfit, newAction);
    if (newVar.isEmpty() || !manifest.variants.contains(newVar)) return;

    // Fetch the new face paths.
    const std::array<QString, puppetlib::kFaceCount> newFaces =
        manifest.variants.value(newVar).faces;

    const std::string oldVar = pc->variant();
    std::array<std::string, puppetlib::kFaceCount> oldFaces;
    std::array<std::string, puppetlib::kFaceCount> newFacesStd;
    for (int i = 0; i < puppetlib::kFaceCount; ++i) {
        oldFaces[static_cast<size_t>(i)] = pc->facePath(i);
        newFacesStd[static_cast<size_t>(i)] = newFaces[static_cast<size_t>(i)].toStdString();
    }
    const std::string newVarStd = newVar.toStdString();

    auto applyState = [this, pc](const std::string& var,
                                 const std::array<std::string, puppetlib::kFaceCount>& faces) {
        pc->setVariant(var);
        for (int i = 0; i < puppetlib::kFaceCount; ++i)
            pc->setFacePath(i, faces[static_cast<size_t>(i)]);
        populateFromClip();
        emit propertyChanged();
    };

    if (m_commandStack) {
        m_commandStack->execute(std::make_unique<LambdaCommand>(
            "Change puppet outfit",
            [applyState, newVarStd, newFacesStd]() { applyState(newVarStd, newFacesStd); },
            [applyState, oldVar, oldFaces]()       { applyState(oldVar, oldFaces); }));
    } else {
        applyState(newVarStd, newFacesStd);
    }
}

void PropertiesPanel::applyPuppetAction()
{
    if (m_updating || !m_puppetClip) return;
    auto* pc = m_puppetClip;
    const QString newAction = m_puppetActionCombo->currentText();

    QString curOutfit, curAction;
    puppetlib::splitVariantKey(QString::fromStdString(pc->variant()),
                               curOutfit, curAction);
    if (newAction == curAction) return;

    // Compose the new variant key (same outfit, new action).
    const QString folder = folderForPuppet(pc);
    PuppetManifest manifest;
    if (folder.isEmpty() || !puppetlib::load(folder, manifest)) return;

    const QString newVar = puppetlib::resolveVariantKey(manifest, curOutfit, newAction);
    if (newVar.isEmpty() || !manifest.variants.contains(newVar)) return;

    // Fetch the new face paths.
    const std::array<QString, puppetlib::kFaceCount> newFaces =
        manifest.variants.value(newVar).faces;

    const std::string oldVar = pc->variant();
    std::array<std::string, puppetlib::kFaceCount> oldFaces;
    std::array<std::string, puppetlib::kFaceCount> newFacesStd;
    for (int i = 0; i < puppetlib::kFaceCount; ++i) {
        oldFaces[static_cast<size_t>(i)] = pc->facePath(i);
        newFacesStd[static_cast<size_t>(i)] = newFaces[static_cast<size_t>(i)].toStdString();
    }
    const std::string newVarStd = newVar.toStdString();

    auto applyState = [this, pc](const std::string& var,
                                 const std::array<std::string, puppetlib::kFaceCount>& faces) {
        pc->setVariant(var);
        for (int i = 0; i < puppetlib::kFaceCount; ++i)
            pc->setFacePath(i, faces[static_cast<size_t>(i)]);
        populateFromClip();
        emit propertyChanged();
    };

    if (m_commandStack) {
        m_commandStack->execute(std::make_unique<LambdaCommand>(
            "Change puppet action",
            [applyState, newVarStd, newFacesStd]() { applyState(newVarStd, newFacesStd); },
            [applyState, oldVar, oldFaces]()       { applyState(oldVar, oldFaces); }));
    } else {
        applyState(newVarStd, newFacesStd);
    }
}

void PropertiesPanel::applyPuppetFloat(
    const char* cmdName,
    const std::function<void(PngPuppetClip*, float)>& setter,
    float newVal, float oldVal)
{
    if (!m_puppetClip) return;
    auto* pc = m_puppetClip;
    if (m_commandStack) {
        m_commandStack->execute(std::make_unique<LambdaCommand>(
            cmdName,
            [pc, setter, newVal, this]() {
                setter(pc, newVal);
                m_updating = true; populateFromClip(); m_updating = false;
                emit propertyChanged();
            },
            [pc, setter, oldVal, this]() {
                setter(pc, oldVal);
                m_updating = true; populateFromClip(); m_updating = false;
                emit propertyChanged();
            }));
    } else {
        setter(pc, newVal);
        emit propertyChanged();
    }
}

} // namespace rt
