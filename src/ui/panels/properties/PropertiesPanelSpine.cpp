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
#include "widgets/ScrubbySpinBox.h"

#include "timeline/Clip.h"
#include "timeline/SpineClip.h"
#include "timeline/VideoClip.h"
#include "spine/ModelManager.h"
#include "command/CommandStack.h"
#include "command/LambdaCommand.h"

#include <QComboBox>
#include <QCheckBox>
#include <filesystem>

namespace rt {

// ── Spine ───────────────────────────────────────────────────────────────────

void PropertiesPanel::applySpineCharacter()
{
    if (m_updating || !m_clip || m_clip->clipType() != ClipType::Spine) return;
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
    if (m_updating || !m_clip) return;

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
        // Compute new paths based on the new outfit
        namespace fs = std::filesystem;
        std::string animName = vc->animationName().empty() ? "idle" : vc->animationName();
        std::string ext = pathToUtf8(utf8ToPath(vc->mediaPath()).extension());
        if (ext.empty()) ext = ".mov";
        // Preserve the format subdirectory from the existing media path
        std::string oldPath = vc->mediaPath();
        std::string fmtDir = "H264_Green"; // default fallback
        {
            namespace fs = std::filesystem;
            auto p = fs::path(oldPath);
            // Walk up from the file to find the format directory
            // Path: .../converted/{fmtDir}/{char}/{outfit}/{anim}.ext
            if (p.parent_path().has_parent_path() && p.parent_path().parent_path().has_parent_path()) {
                auto candidate = pathToUtf8(p.parent_path().parent_path().parent_path().filename());
                if (candidate == "H264_Green" || candidate == "H264_Blue" ||
                    candidate == "H264_Custom" || candidate == "ProRes")
                    fmtDir = candidate;
            }
        }
        std::string base = "assets/converted/" + fmtDir + "/"
            + vc->characterName() + "/" + newOutfit + "/";
        std::string newMute = base + animName + ext;
        std::string newTalk = base + animName + "_talk" + ext;
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
    if (m_updating || !m_clip || m_clip->clipType() != ClipType::Spine) return;
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

void PropertiesPanel::applySpineAnimation()
{
    if (m_updating || !m_clip) return;

    // Handle VideoClip video characters
    if (m_clip->clipType() == ClipType::Video) {
        auto* vc = static_cast<VideoClip*>(m_clip);
        if (!vc->isVideoCharacter()) return;
        auto newAnim = m_animationCombo->currentText().toStdString();
        if (newAnim == vc->animationName()) return;
        auto oldAnim = vc->animationName();
        auto oldMute = vc->videoMutePath();
        auto oldTalk = vc->videoTalkPath();
        auto oldMedia = vc->mediaPath();
        auto oldLabel = vc->label();
        auto newLabel = vc->characterName() + " - " + newAnim;
        namespace fs = std::filesystem;
        std::string outfit = vc->outfit().empty() ? "default" : vc->outfit();
        std::string ext = pathToUtf8(utf8ToPath(vc->mediaPath()).extension());
        if (ext.empty()) ext = ".mov";
        // Preserve the format subdirectory from the existing media path
        std::string fmtDir = "H264_Green";
        {
            namespace fs = std::filesystem;
            auto p = fs::path(oldMedia);
            if (p.parent_path().has_parent_path() && p.parent_path().parent_path().has_parent_path()) {
                auto candidate = pathToUtf8(p.parent_path().parent_path().parent_path().filename());
                if (candidate == "H264_Green" || candidate == "H264_Blue" ||
                    candidate == "H264_Custom" || candidate == "ProRes")
                    fmtDir = candidate;
            }
        }
        std::string base = "assets/converted/" + fmtDir + "/"
            + vc->characterName() + "/" + outfit + "/";
        std::string newMute = base + newAnim + ext;
        std::string newTalk = base + newAnim + "_talk" + ext;
        std::string newMedia = vc->isTalking() ? newTalk : newMute;
        if (m_commandStack) {
            m_commandStack->execute(std::make_unique<LambdaCommand>(
                "Change animation",
                [vc, newAnim, newMute, newTalk, newMedia, newLabel, this]() {
                    vc->setAnimationName(newAnim);
                    vc->setVideoMutePath(newMute);
                    vc->setVideoTalkPath(newTalk);
                    vc->setMediaPath(newMedia);
                    vc->setLabel(newLabel);
                    m_updating = true;
                    m_animationCombo->setCurrentText(QString::fromStdString(newAnim));
                    m_updating = false;
                    emit propertyChanged();
                },
                [vc, oldAnim, oldMute, oldTalk, oldMedia, oldLabel, this]() {
                    vc->setAnimationName(oldAnim);
                    vc->setVideoMutePath(oldMute);
                    vc->setVideoTalkPath(oldTalk);
                    vc->setMediaPath(oldMedia);
                    vc->setLabel(oldLabel);
                    m_updating = true;
                    m_animationCombo->setCurrentText(QString::fromStdString(oldAnim));
                    m_updating = false;
                    emit propertyChanged();
                }));
        } else {
            vc->setAnimationName(newAnim);
            vc->setVideoMutePath(newMute);
            vc->setVideoTalkPath(newTalk);
            vc->setMediaPath(newMedia);
            vc->setLabel(newLabel);
            emit propertyChanged();
        }
        return;
    }

    if (m_clip->clipType() != ClipType::Spine) return;
    auto* sc = static_cast<SpineClip*>(m_clip);
    auto newVal = m_animationCombo->currentText().toStdString();
    if (newVal == sc->animationName()) return;
    auto oldVal = sc->animationName();
    auto oldLabel = sc->label();
    auto newLabel = sc->characterName() + " - " + newVal;
    if (m_commandStack) {
        m_commandStack->execute(std::make_unique<LambdaCommand>(
            "Change animation",
            [sc, newVal, newLabel, this]() {
                sc->setAnimationName(newVal);
                sc->setLabel(newLabel);
                m_updating = true;
                m_animationCombo->setCurrentText(QString::fromStdString(newVal));
                m_updating = false;
                emit propertyChanged();
            },
            [sc, oldVal, oldLabel, this]() {
                sc->setAnimationName(oldVal);
                sc->setLabel(oldLabel);
                m_updating = true;
                m_animationCombo->setCurrentText(QString::fromStdString(oldVal));
                m_updating = false;
                emit propertyChanged();
            }));
    } else {
        sc->setAnimationName(newVal);
        sc->setLabel(newLabel);
        emit propertyChanged();
    }
}

void PropertiesPanel::applySpineLooping()
{
    if (m_updating || !m_clip || m_clip->clipType() != ClipType::Spine) return;
    auto* sc = static_cast<SpineClip*>(m_clip);
    bool newVal = m_loopingCheck->isChecked();
    if (newVal == sc->isLooping()) return;
    bool oldVal = sc->isLooping();
    if (m_commandStack) {
        m_commandStack->execute(std::make_unique<LambdaCommand>(
            "Toggle looping",
            [sc, newVal, this]() {
                sc->setLooping(newVal);
                m_updating = true; m_loopingCheck->setChecked(newVal); m_updating = false;
                emit propertyChanged();
            },
            [sc, oldVal, this]() {
                sc->setLooping(oldVal);
                m_updating = true; m_loopingCheck->setChecked(oldVal); m_updating = false;
                emit propertyChanged();
            }));
    } else {
        sc->setLooping(newVal);
        emit propertyChanged();
    }
}

void PropertiesPanel::applySpineTalking()
{
    if (m_updating || !m_clip) return;

    // Handle VideoClip video characters
    if (m_clip->clipType() == ClipType::Video) {
        auto* vc = static_cast<VideoClip*>(m_clip);
        if (!vc->isVideoCharacter()) return;
        bool newVal = m_talkingCheck->isChecked();
        if (newVal == vc->isTalking()) return;
        bool oldVal = vc->isTalking();
        if (m_commandStack) {
            m_commandStack->execute(std::make_unique<LambdaCommand>(
                "Toggle talking",
                [vc, newVal, this]() {
                    vc->setTalking(newVal);
                    m_updating = true; m_talkingCheck->setChecked(newVal); m_updating = false;
                    emit propertyChanged();
                },
                [vc, oldVal, this]() {
                    vc->setTalking(oldVal);
                    m_updating = true; m_talkingCheck->setChecked(oldVal); m_updating = false;
                    emit propertyChanged();
                }));
        } else {
            vc->setTalking(newVal);
            emit propertyChanged();
        }
        return;
    }

    if (m_clip->clipType() != ClipType::Spine) return;
    auto* sc = static_cast<SpineClip*>(m_clip);
    bool newVal = m_talkingCheck->isChecked();
    if (newVal == sc->isTalking()) return;
    bool oldVal = sc->isTalking();
    if (m_commandStack) {
        m_commandStack->execute(std::make_unique<LambdaCommand>(
            "Toggle talking",
            [sc, newVal, this]() {
                sc->setTalking(newVal);
                m_updating = true; m_talkingCheck->setChecked(newVal); m_updating = false;
                emit propertyChanged();
            },
            [sc, oldVal, this]() {
                sc->setTalking(oldVal);
                m_updating = true; m_talkingCheck->setChecked(oldVal); m_updating = false;
                emit propertyChanged();
            }));
    } else {
        sc->setTalking(newVal);
        emit propertyChanged();
    }
}

void PropertiesPanel::applySpineAnimSpeed()
{
    if (m_updating || !m_clip || m_clip->clipType() != ClipType::Spine) return;
    auto* sc = static_cast<SpineClip*>(m_clip);
    float newVal = static_cast<float>(m_animSpeedSpin->value());
    if (newVal == sc->animationSpeed()) return;
    float oldVal = sc->animationSpeed();
    if (m_commandStack) {
        m_commandStack->execute(std::make_unique<LambdaCommand>(
            "Change animation speed",
            [sc, newVal, this]() {
                sc->setAnimationSpeed(newVal);
                m_updating = true; m_animSpeedSpin->setValue(newVal); m_updating = false;
                emit propertyChanged();
            },
            [sc, oldVal, this]() {
                sc->setAnimationSpeed(oldVal);
                m_updating = true; m_animSpeedSpin->setValue(oldVal); m_updating = false;
                emit propertyChanged();
            }));
    } else {
        sc->setAnimationSpeed(newVal);
        emit propertyChanged();
    }
}

void PropertiesPanel::applySpineContinuity()
{
    if (m_updating || !m_clip || m_clip->clipType() != ClipType::Spine) return;
    auto* sc = static_cast<SpineClip*>(m_clip);
    bool newVal = m_continuityCheck->isChecked();
    if (newVal == sc->useGlobalTime()) return;
    bool oldVal = sc->useGlobalTime();
    if (m_commandStack) {
        m_commandStack->execute(std::make_unique<LambdaCommand>(
            "Toggle continuity",
            [sc, newVal, this]() {
                sc->setUseGlobalTime(newVal);
                m_updating = true; m_continuityCheck->setChecked(newVal); m_updating = false;
                emit propertyChanged();
            },
            [sc, oldVal, this]() {
                sc->setUseGlobalTime(oldVal);
                m_updating = true; m_continuityCheck->setChecked(oldVal); m_updating = false;
                emit propertyChanged();
            }));
    } else {
        sc->setUseGlobalTime(newVal);
        emit propertyChanged();
    }
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
