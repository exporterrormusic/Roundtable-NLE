/*
 * PropertiesPanelSpine.cpp — Spine/Character property application for PropertiesPanel.
 * Split from PropertiesPanelApply.cpp for maintainability.
 *
 * Contains: applySpineCharacter, applySpineOutfit, applySpineStance,
 * applySpineAnimation, applySpineLooping, applySpineTalking,
 * applySpineAnimSpeed, applySpineContinuity,
 * populateCharacterDropdown, populateOutfitDropdown,
 * populateStanceDropdown, populateAnimationDropdown.
 */

#include "panels/properties/PropertiesPanel.h"
#include "PathUtils.h"
#include "panels/characters/VideoCharacterPaths.h"
#include "widgets/ScrubbySpinBox.h"

#include "timeline/Clip.h"
#include "timeline/SpineClip.h"
#include "timeline/VideoClip.h"
#include "timeline/ClipMutation.h"
#include "spine/ModelManager.h"
#include "command/CommandStack.h"
#include "command/LambdaCommand.h"

#include <QComboBox>
#include <QCheckBox>
#include <algorithm>
#include <filesystem>

namespace rt {

// ── Spine ───────────────────────────────────────────────────────────────────

void PropertiesPanel::applySpineCharacter()
{
    if (m_updating || !canMutateBoundClip() || m_clip->clipType() != ClipType::Spine) return;
    auto* sc = static_cast<SpineClip*>(m_clip);
    // Use folder name from item data (not display text)
    auto newVal = m_characterCombo->currentData().toString().toStdString();
    if (newVal.empty()) newVal = m_characterCombo->currentText().toStdString();
    if (newVal == sc->characterName()) return;
    auto oldVal = sc->characterName();
    auto oldLabel = sc->label();
    auto newLabel = newVal + " - " + sc->animationName();
    if (m_commandStack) {
        m_commandStack->execute(std::make_unique<LambdaCommand>(
            "Change character",
            [sc, newVal, newLabel, this]() { sc->setCharacterName(newVal); sc->setLabel(newLabel); populateFromClip(); emit propertyChanged(); },
            [sc, oldVal, oldLabel, this]() { sc->setCharacterName(oldVal); sc->setLabel(oldLabel); populateFromClip(); emit propertyChanged(); }));
    } else {
        sc->setCharacterName(newVal);
        sc->setLabel(newLabel);
        emit propertyChanged();
    }
}

void PropertiesPanel::applySpineOutfit()
{
    if (m_updating || !canMutateBoundClip()) return;

    // Handle VideoClip video characters
    if (m_clip->clipType() == ClipType::Video) {
        auto* vc = static_cast<VideoClip*>(m_clip);
        if (!vc->isVideoCharacter()) return;
        auto newOutfit = m_outfitCombo->currentText().toStdString();
        if (newOutfit == vc->outfit()) return;
        auto oldOutfit = vc->outfit();
        auto oldMute = vc->videoMutePath();
        auto oldTalk = vc->videoTalkPath();
        auto oldMedia = vc->mediaPath();
        const std::string animName = vc->animationName().empty() ? "idle" : vc->animationName();
        const auto paths = convertedVideoPaths(
            vc->mediaPath(), vc->characterName(), newOutfit, animName);
        const std::string newMute = paths.mute;
        const std::string newTalk = paths.talk;
        std::string newMedia = vc->isTalking() ? newTalk : newMute;
        if (m_commandStack) {
            m_commandStack->execute(std::make_unique<LambdaCommand>(
                "Change outfit",
                [vc, newOutfit, newMute, newTalk, newMedia, this]() {
                    vc->setOutfit(newOutfit);
                    vc->setVideoMutePath(newMute);
                    vc->setVideoTalkPath(newTalk);
                    vc->setMediaPath(newMedia);
                    populateFromClip();
                    emit propertyChanged();
                },
                [vc, oldOutfit, oldMute, oldTalk, oldMedia, this]() {
                    vc->setOutfit(oldOutfit);
                    vc->setVideoMutePath(oldMute);
                    vc->setVideoTalkPath(oldTalk);
                    vc->setMediaPath(oldMedia);
                    populateFromClip();
                    emit propertyChanged();
                }));
        } else {
            vc->setOutfit(newOutfit);
            vc->setVideoMutePath(newMute);
            vc->setVideoTalkPath(newTalk);
            vc->setMediaPath(newMedia);
            emit propertyChanged();
        }
        return;
    }

    if (m_clip->clipType() != ClipType::Spine) return;
    auto* sc = static_cast<SpineClip*>(m_clip);
    auto newVal = m_outfitCombo->currentText().toStdString();
    if (newVal == sc->outfit()) return;
    auto oldVal = sc->outfit();
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

void PropertiesPanel::applySpineStance()
{
    if (m_updating || !canMutateBoundClip() || m_clip->clipType() != ClipType::Spine) return;
    auto* sc = static_cast<SpineClip*>(m_clip);
    auto newVal = static_cast<CharacterStance>(m_stanceCombo->currentIndex());
    if (newVal == sc->stance()) return;
    auto oldVal = sc->stance();
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

// ── Animation section: applies to every selected clip of the same character ──

namespace {

/// "S:<name>" for a Spine clip, "V:<name>" for a video character, "" otherwise.
std::string characterKey(const Clip* clip)
{
    if (!clip) return {};
    if (clip->clipType() == ClipType::Spine)
        return "S:" + static_cast<const SpineClip*>(clip)->characterName();
    if (clip->clipType() == ClipType::Video) {
        const auto* vc = static_cast<const VideoClip*>(clip);
        if (vc->isVideoCharacter()) return "V:" + vc->characterName();
    }
    return {};
}

} // namespace

std::vector<Clip*> PropertiesPanel::characterTargets()
{
    std::vector<Clip*> out;
    if (!m_clip) return out;
    const std::string key = characterKey(m_clip);
    if (m_multiSelection.size() > 1 && !key.empty()) {
        for (Clip* c : m_multiSelection) {
            if (!c || characterKey(c) != key) continue;
            Track* track = nullptr;
            Clip* live = m_timeline ? rt::resolveClipById(m_timeline, c->id(), &track) : c;
            if (!live) continue;
            if (m_timeline && !rt::canMutateClip(live, track)) continue;   // locked track
            if (std::find(out.begin(), out.end(), live) == out.end())
                out.push_back(live);
        }
    }
    if (out.empty()) out.push_back(m_clip);
    return out;
}

void PropertiesPanel::executeCharacterEdits(const char* name, std::vector<CharacterEdit> edits,
                                            std::function<void(bool)> syncUi)
{
    if (edits.empty()) return;
    auto run = [this, edits, syncUi](bool redo) {
        for (const auto& e : edits) {
            Clip* clip = m_timeline ? rt::resolveClipById(m_timeline, e.id) : e.clip;
            if (!clip) continue;
            (redo ? e.redo : e.undo)(clip);
        }
        m_updating = true;
        if (syncUi) syncUi(redo);
        m_updating = false;
        emit propertyChanged();
    };
    if (m_commandStack) {
        m_commandStack->execute(std::make_unique<LambdaCommand>(
            name, [run]() { run(true); }, [run]() { run(false); }));
    } else {
        run(true);
    }
}

void PropertiesPanel::applySpineAnimation()
{
    if (m_updating || !canMutateBoundClip()) return;
    if (characterKey(m_clip).empty()) return;
    const std::string newAnim = m_animationCombo->currentText().toStdString();
    const std::string repOld = m_clip->clipType() == ClipType::Spine
        ? static_cast<SpineClip*>(m_clip)->animationName()
        : static_cast<VideoClip*>(m_clip)->animationName();

    std::vector<CharacterEdit> edits;
    for (Clip* c : characterTargets()) {
        if (c->clipType() == ClipType::Video) {
            auto* vc = static_cast<VideoClip*>(c);
            if (!vc->isVideoCharacter() || vc->animationName() == newAnim) continue;
            const std::string outfit = vc->outfit().empty() ? "default" : vc->outfit();
            const auto paths = convertedVideoPaths(
                vc->mediaPath(), vc->characterName(), outfit, newAnim);
            const std::string newMute = paths.mute;
            const std::string newTalk = paths.talk;
            const std::string newMedia = vc->isTalking() ? newTalk : newMute;
            const std::string newLabel = vc->characterName() + " - " + newAnim;
            const std::string oldAnim = vc->animationName(), oldMute = vc->videoMutePath(),
                              oldTalk = vc->videoTalkPath(), oldMedia = vc->mediaPath(),
                              oldLabel = vc->label();
            edits.push_back({c->id(), c,
                [newAnim, newMute, newTalk, newMedia, newLabel](Clip* x) {
                    auto* v = static_cast<VideoClip*>(x);
                    v->setAnimationName(newAnim);
                    v->setVideoMutePath(newMute);
                    v->setVideoTalkPath(newTalk);
                    v->setMediaPath(newMedia);
                    v->setLabel(newLabel);
                },
                [oldAnim, oldMute, oldTalk, oldMedia, oldLabel](Clip* x) {
                    auto* v = static_cast<VideoClip*>(x);
                    v->setAnimationName(oldAnim);
                    v->setVideoMutePath(oldMute);
                    v->setVideoTalkPath(oldTalk);
                    v->setMediaPath(oldMedia);
                    v->setLabel(oldLabel);
                }});
        } else if (c->clipType() == ClipType::Spine) {
            auto* sc = static_cast<SpineClip*>(c);
            if (sc->animationName() == newAnim) continue;
            const std::string newLabel = sc->characterName() + " - " + newAnim;
            const std::string oldAnim = sc->animationName(), oldLabel = sc->label();
            edits.push_back({c->id(), c,
                [newAnim, newLabel](Clip* x) {
                    auto* s = static_cast<SpineClip*>(x);
                    s->setAnimationName(newAnim);
                    s->setLabel(newLabel);
                },
                [oldAnim, oldLabel](Clip* x) {
                    auto* s = static_cast<SpineClip*>(x);
                    s->setAnimationName(oldAnim);
                    s->setLabel(oldLabel);
                }});
        }
    }
    executeCharacterEdits("Change animation", std::move(edits),
        [this, newAnim, repOld](bool redo) {
            m_animationCombo->setCurrentText(QString::fromStdString(redo ? newAnim : repOld));
        });
}

void PropertiesPanel::applySpineLooping()
{
    if (m_updating || !canMutateBoundClip() || m_clip->clipType() != ClipType::Spine) return;
    const bool newVal = m_loopingCheck->isChecked();
    const bool repOld = static_cast<SpineClip*>(m_clip)->isLooping();
    std::vector<CharacterEdit> edits;
    for (Clip* c : characterTargets()) {
        if (c->clipType() != ClipType::Spine) continue;
        const bool oldVal = static_cast<SpineClip*>(c)->isLooping();
        if (oldVal == newVal) continue;
        edits.push_back({c->id(), c,
            [newVal](Clip* x) { static_cast<SpineClip*>(x)->setLooping(newVal); },
            [oldVal](Clip* x) { static_cast<SpineClip*>(x)->setLooping(oldVal); }});
    }
    executeCharacterEdits("Toggle looping", std::move(edits),
        [this, newVal, repOld](bool redo) { m_loopingCheck->setChecked(redo ? newVal : repOld); });
}

void PropertiesPanel::applySpineTalking()
{
    if (m_updating || !canMutateBoundClip()) return;
    if (characterKey(m_clip).empty()) return;
    const bool newVal = m_talkingCheck->isChecked();
    auto talkingOf = [](const Clip* c) {
        return c->clipType() == ClipType::Spine
            ? static_cast<const SpineClip*>(c)->isTalking()
            : static_cast<const VideoClip*>(c)->isTalking();
    };
    auto setTalking = [](Clip* c, bool v) {
        if (c->clipType() == ClipType::Spine) static_cast<SpineClip*>(c)->setTalking(v);
        else static_cast<VideoClip*>(c)->setTalking(v);
    };
    const bool repOld = talkingOf(m_clip);
    std::vector<CharacterEdit> edits;
    for (Clip* c : characterTargets()) {
        const bool oldVal = talkingOf(c);
        if (oldVal == newVal) continue;
        edits.push_back({c->id(), c,
            [setTalking, newVal](Clip* x) { setTalking(x, newVal); },
            [setTalking, oldVal](Clip* x) { setTalking(x, oldVal); }});
    }
    executeCharacterEdits("Toggle talking", std::move(edits),
        [this, newVal, repOld](bool redo) { m_talkingCheck->setChecked(redo ? newVal : repOld); });
}

void PropertiesPanel::applySpineAnimSpeed()
{
    if (m_updating || !canMutateBoundClip() || m_clip->clipType() != ClipType::Spine) return;
    const float newVal = static_cast<float>(m_animSpeedSpin->value());
    const float repOld = static_cast<SpineClip*>(m_clip)->animationSpeed();
    std::vector<CharacterEdit> edits;
    for (Clip* c : characterTargets()) {
        if (c->clipType() != ClipType::Spine) continue;
        const float oldVal = static_cast<SpineClip*>(c)->animationSpeed();
        if (oldVal == newVal) continue;
        edits.push_back({c->id(), c,
            [newVal](Clip* x) { static_cast<SpineClip*>(x)->setAnimationSpeed(newVal); },
            [oldVal](Clip* x) { static_cast<SpineClip*>(x)->setAnimationSpeed(oldVal); }});
    }
    executeCharacterEdits("Change animation speed", std::move(edits),
        [this, newVal, repOld](bool redo) { m_animSpeedSpin->setValue(redo ? newVal : repOld); });
}

void PropertiesPanel::applySpineContinuity()
{
    if (m_updating || !canMutateBoundClip() || m_clip->clipType() != ClipType::Spine) return;
    const bool newVal = m_continuityCheck->isChecked();
    const bool repOld = static_cast<SpineClip*>(m_clip)->useGlobalTime();
    std::vector<CharacterEdit> edits;
    for (Clip* c : characterTargets()) {
        if (c->clipType() != ClipType::Spine) continue;
        const bool oldVal = static_cast<SpineClip*>(c)->useGlobalTime();
        if (oldVal == newVal) continue;
        edits.push_back({c->id(), c,
            [newVal](Clip* x) { static_cast<SpineClip*>(x)->setUseGlobalTime(newVal); },
            [oldVal](Clip* x) { static_cast<SpineClip*>(x)->setUseGlobalTime(oldVal); }});
    }
    executeCharacterEdits("Toggle continuity", std::move(edits),
        [this, newVal, repOld](bool redo) { m_continuityCheck->setChecked(redo ? newVal : repOld); });
}

// ── Spine dropdown population ───────────────────────────────────────────────

void PropertiesPanel::populateCharacterDropdown()
{
    if (!m_characterCombo) return;

    m_characterCombo->blockSignals(true);
    // The clip is authoritative. On first selection the combo is empty, and
    // after switching clips its text may still belong to the previous clip.
    // Preserve an unlisted/offline character instead of showing a blank row.
    QString current = m_spineClip
        ? QString::fromStdString(m_spineClip->characterName())
        : m_characterCombo->currentText();
    m_characterCombo->clear();

    if (m_modelManager) {
        auto names = m_modelManager->characterNames();
        for (const auto& name : names) {
            QString dispName = QString::fromStdString(m_modelManager->getDisplayName(name));
            m_characterCombo->addItem(dispName, QString::fromStdString(name));
        }
    }

    if (!current.isEmpty()) {
        // Find by folder name stored in item data
        int idx = m_characterCombo->findData(current);
        if (idx < 0)
            idx = m_characterCombo->findText(current, Qt::MatchFixedString);
        if (idx >= 0) {
            m_characterCombo->setCurrentIndex(idx);
        } else {
            m_characterCombo->addItem(current, current);
            m_characterCombo->setCurrentText(current);
        }
    }
    m_characterCombo->blockSignals(false);
}

void PropertiesPanel::populateOutfitDropdown()
{
    if (!m_outfitCombo || !m_spineClip) return;

    m_outfitCombo->blockSignals(true);
    QString current = QString::fromStdString(m_spineClip->outfit());
    m_outfitCombo->clear();

    if (m_modelManager) {
        auto outfits = m_modelManager->getMetadataOutfits(m_spineClip->characterName());
        for (const auto& outfit : outfits)
            m_outfitCombo->addItem(QString::fromStdString(outfit.key));
    }

    if (m_outfitCombo->findText(current) < 0)
        m_outfitCombo->addItem(current);
    m_outfitCombo->setCurrentText(current);
    m_outfitCombo->blockSignals(false);
}

void PropertiesPanel::populateStanceDropdown()
{
    if (!m_stanceCombo || !m_spineClip) return;

    m_stanceCombo->blockSignals(true);
    QString currentText = m_stanceCombo->currentText();
    m_stanceCombo->clear();

    m_stanceCombo->addItem("Default");

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
        m_stanceCombo->addItem("Aim");
        m_stanceCombo->addItem("Cover");
    }

    int idx = m_stanceCombo->findText(currentText);
    if (idx >= 0) m_stanceCombo->setCurrentIndex(idx);
    else m_stanceCombo->setCurrentIndex(0);
    m_stanceCombo->blockSignals(false);
}

void PropertiesPanel::populateAnimationDropdown()
{
    if (!m_animationCombo || !m_spineClip) return;

    m_animationCombo->blockSignals(true);
    QString current = QString::fromStdString(m_spineClip->animationName());
    m_animationCombo->clear();

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

    if (!hasSkeletonAnims) {
        QStringList commonAnims = {
            "idle", "action", "angry", "sad", "delight",
            "smile", "shy", "surprise", "special",
            "cry", "pain", "think", "expression_0"};
        for (const auto& anim : commonAnims)
            m_animationCombo->addItem(anim);
    }

    if (m_animationCombo->findText(current) < 0)
        m_animationCombo->addItem(current);
    m_animationCombo->setCurrentText(current);
    m_animationCombo->blockSignals(false);
}

} // namespace rt
