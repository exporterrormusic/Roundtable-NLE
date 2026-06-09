/*
 * PropertiesPanel.cpp - Premiere Pro-style clip properties panel.
 * Step 16 (modularized)
 */

#include "panels/properties/PropertiesPanel.h"
#include "widgets/ScrubbySpinBox.h"
#include "widgets/AudioFxSection.h"
#include "Theme.h"

#include "timeline/Clip.h"
#include "timeline/SpineClip.h"
#include "timeline/PngPuppetClip.h"
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
#include <QListWidget>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>
#include <QComboBox>
#include <QColorDialog>

#include <spdlog/spdlog.h>
#include <chrono>

namespace rt {


void PropertiesPanel::setClip(Clip* clip, Track* track)
{
    auto pp0 = std::chrono::steady_clock::now();
    m_clip  = clip;
    m_multiSelection.clear(); // single-clip path: no multi-selection in flight
    m_track = track;
    m_transitionIndex = SIZE_MAX; // clear transition selection
    m_spineClip = (clip && clip->clipType() == ClipType::Spine)
                      ? static_cast<SpineClip*>(clip)
                      : nullptr;
    m_puppetClip = (clip && clip->clipType() == ClipType::PngPuppet)
                      ? static_cast<PngPuppetClip*>(clip)
                      : nullptr;

    // Toggle empty state vs. scroll area
    bool hasClip = (clip != nullptr);
    m_scrollArea->setVisible(hasClip);
    m_emptyLabel->setVisible(!hasClip);
    if (m_statusLabel)
        m_statusLabel->setText(hasClip ? QStringLiteral("Clip selected") : QStringLiteral("No clip"));

    showSectionsForType();
    auto pp1 = std::chrono::steady_clock::now();
    populateFromClip();
    auto pp2 = std::chrono::steady_clock::now();
    emit clipChanged(clip);
    spdlog::info("PropertiesPanel::setClip  sections={:.1f}ms  populate={:.1f}ms  total={:.1f}ms",
        std::chrono::duration<double, std::milli>(pp1 - pp0).count(),
        std::chrono::duration<double, std::milli>(pp2 - pp1).count(),
        std::chrono::duration<double, std::milli>(pp2 - pp0).count());
}

void PropertiesPanel::clearClip()
{
    m_clip  = nullptr;
    m_multiSelection.clear();
    m_spineClip = nullptr;
    m_puppetClip = nullptr;
    m_track = nullptr;
    m_transitionIndex = SIZE_MAX;
    showSectionsForType();
    if (m_transitionSection) m_transitionSection->setVisible(false);
    m_headerLabel->setText("");
    m_typeLabel->clear();
    m_typeLabel->setStyleSheet(
        QStringLiteral("QLabel { background: transparent; border: none; }"));

    // Show empty state
    m_scrollArea->setVisible(false);
    m_emptyLabel->setVisible(true);
    if (m_statusLabel) m_statusLabel->setText(QStringLiteral("No clip"));

    emit clipChanged(nullptr);
}

void PropertiesPanel::setMultiSelection(const std::vector<Clip*>& clips)
{
    if (clips.empty()) {
        clearClip();
        return;
    }
    if (clips.size() == 1) {
        setClip(clips.front());
        return;
    }

    // Retain the full selection so the Shot dropdown (and any other future
    // multi-clip action) can operate on every selected clip, not just the
    // single representative `m_clip` we pick below for the section bindings.
    m_multiSelection = clips;

    // Check if all clips share the same non-zero groupId
    uint64_t commonGroup = clips.front()->groupId();
    bool allSameGroup = (commonGroup != 0);
    for (size_t i = 1; i < clips.size() && allSameGroup; ++i) {
        if (clips[i]->groupId() != commonGroup)
            allSameGroup = false;
    }

    // Hide all single-clip sections
    m_identitySection->setVisible(false);
    m_transformSection->setVisible(false);
    m_spineSection->setVisible(false);
    m_characterSection->setVisible(false);
    m_animationSection->setVisible(false);
    m_videoSection->setVisible(false);
    m_audioSection->setVisible(false);
    if (m_audioFxSection) m_audioFxSection->setVisible(false);
    m_titleSection->setVisible(false);
    m_graphicSection->setVisible(false);
    if (m_captionSection) m_captionSection->setVisible(false);
    if (m_effectsSection) m_effectsSection->setVisible(false);
    if (m_transitionSection) m_transitionSection->setVisible(false);

    // All-captions selection → show the Caption styling section; edits apply
    // to every selected caption at once.
    bool allCaptions = true;
    for (auto* c : clips)
        if (!c || c->clipType() != ClipType::Caption) { allCaptions = false; break; }
    if (allCaptions) {
        m_clip = clips.front();
        m_track = nullptr;
        m_spineClip = nullptr;
        if (m_captionSection) m_captionSection->setVisible(true);
        m_headerLabel->setText(QString("%1 captions selected").arg(clips.size()));
        m_typeLabel->setText("Captions");
        const auto& tmc = Theme::metrics();
        QColor capBadge(70, 200, 170);
        m_typeLabel->setStyleSheet(
            QStringLiteral("QLabel { color: %1; font-size: 11px; padding: 1px 6px; "
                           "border-radius: %2px; background: %3; border: none; }")
                .arg(Theme::hex(capBadge.lighter(160)),
                     QString::number(tmc.radiusSm),
                     Theme::hex(capBadge.darker(220))));
        m_scrollArea->setVisible(true);
        m_emptyLabel->setVisible(false);
        if (m_statusLabel) m_statusLabel->setText(QStringLiteral("Captions"));
        populateFromCaption();
        emit clipChanged(m_clip);
        return;
    }

    if (allSameGroup) {
        // Use the first clip as representative for shot name / group info
        m_clip = clips.front();
        m_track = nullptr;
        m_spineClip = nullptr;

        m_headerLabel->setText(QString("%1 clips selected")
                                   .arg(clips.size()));
        m_typeLabel->setText("Shot Group");
        // Purple badge for shot groups
        const auto& tm = Theme::metrics();
        QColor badgeColor(160, 100, 220);
        m_typeLabel->setStyleSheet(
            QStringLiteral("QLabel { color: %1; font-size: 11px; padding: 1px 6px; "
                           "border-radius: %2px; background: %3; border: none; }")
                .arg(Theme::hex(badgeColor.lighter(180)),
                     QString::number(tm.radiusSm),
                     Theme::hex(badgeColor.darker(200))));
        m_scrollArea->setVisible(true);
        m_emptyLabel->setVisible(false);
        if (m_statusLabel) m_statusLabel->setText(QStringLiteral("Shot group"));

        updateShotSection();

        // For shot groups, also show character/animation controls for
        // each clip type present (e.g. video character talking toggle).
        // Find a video character or spine clip in the group to populate from.
        for (auto* c : clips) {
            if (c->clipType() == ClipType::Video) {
                auto* vc = static_cast<VideoClip*>(c);
                if (vc->isVideoCharacter()) {
                    m_clip = c;
                    m_characterSection->setVisible(true);
                    m_animationSection->setVisible(true);
                    // Save group header — populateFromClip overwrites it
                    auto savedHeader = m_headerLabel->text();
                    auto savedType   = m_typeLabel->text();
                    populateFromClip();
                    m_headerLabel->setText(savedHeader);
                    m_typeLabel->setText(savedType);
                    break;
                }
            } else if (c->clipType() == ClipType::Spine) {
                m_clip = c;
                m_spineClip = static_cast<SpineClip*>(c);
                m_characterSection->setVisible(true);
                m_animationSection->setVisible(true);
                auto savedHeader = m_headerLabel->text();
                auto savedType   = m_typeLabel->text();
                populateFromClip();
                m_headerLabel->setText(savedHeader);
                m_typeLabel->setText(savedType);
                break;
            }
        }
    } else {
        // Mixed selection (clips don't share a shot group). The Shot dropdown
        // is still useful here: picking a shot replaces the selection's visual
        // clips with the shot's assets. We need a representative `m_clip` so
        // updateShotSection() / onShotChanged() have a Clip* to anchor on —
        // use the first visual clip if there is one, otherwise leave m_clip
        // null and hide the Shot section (audio-only selection has nothing
        // visual to replace).
        Clip* visualRep = nullptr;
        for (auto* c : clips) {
            if (c && c->clipType() != ClipType::Audio) { visualRep = c; break; }
        }
        m_clip = visualRep;
        m_track = nullptr;
        m_spineClip = (visualRep && visualRep->clipType() == ClipType::Spine)
                          ? static_cast<SpineClip*>(visualRep)
                          : nullptr;

        m_headerLabel->setText(QString("%1 clips selected")
                                   .arg(clips.size()));
        m_typeLabel->setText("Mixed");
        m_typeLabel->setStyleSheet(
            QStringLiteral("QLabel { color: %1; font-size: 11px; padding: 1px 6px; "
                           "border-radius: %2px; background: transparent; border: none; }")
                .arg(Theme::hex(Theme::colors().textSecondary),
                     QString::number(Theme::metrics().radiusSm)));
        m_scrollArea->setVisible(true);
        m_emptyLabel->setVisible(false);
        if (m_statusLabel) m_statusLabel->setText(QStringLiteral("Mixed selection"));

        if (visualRep) {
            m_shotSection->setVisible(true);
            updateShotSection();
        } else {
            m_shotSection->setVisible(false);
        }
    }

    emit clipChanged(m_clip);
}

void PropertiesPanel::refresh()
{
    populateFromClip();
}

// ═════════════════════════════════════════════════════════════════════════════
//  Section visibility
// ═════════════════════════════════════════════════════════════════════════════

void PropertiesPanel::showSectionsForType()
{
    bool hasClip = (m_clip != nullptr);
    bool isCaption = (hasClip && m_clip->clipType() == ClipType::Caption);
    // Audio clips have no spatial representation, so the Transform section
    // (position / scale / rotation / anchor) does not apply to them.
    bool isAudio = (hasClip && m_clip->clipType() == ClipType::Audio);
    // Identity section always hidden (data binding kept internally)
    m_identitySection->setVisible(false);
    // Captions are positioned via their own Position control, not the generic
    // transform/shot/effects sections — keep the panel focused.
    m_transformSection->setVisible(hasClip && !isCaption && !isAudio);
    if (m_effectsSection) m_effectsSection->setVisible(hasClip && !isCaption);
    if (m_captionSection) m_captionSection->setVisible(isCaption);
    if (m_transitionSection) m_transitionSection->setVisible(false); // hide when showing clip

    // Legacy spine section stays hidden
    m_spineSection->setVisible(false);

    // Character & Animation sections shown for Spine clips
    // and for video clips that represent video characters.
    bool isSpine = (hasClip && m_clip->clipType() == ClipType::Spine);
    bool isVideoChar = false;
    if (hasClip && m_clip->clipType() == ClipType::Video) {
        auto* vc = static_cast<VideoClip*>(m_clip);
        isVideoChar = vc->isVideoCharacter();
    }
    m_characterSection->setVisible(isSpine || isVideoChar);
    // Show animation section (talking toggle) for spine clips and video characters
    m_animationSection->setVisible(isSpine || isVideoChar);

    spdlog::info("showSectionsForType  hasClip={}  type={}  isSpine={}  isVideoChar={}  charVisible={}",
        hasClip,
        hasClip ? static_cast<int>(m_clip->clipType()) : -1,
        isSpine, isVideoChar,
        m_characterSection->isVisible());

    // Shot section: always show for any clip so the user can apply
    // a shot preset at any time, not just for pre-grouped clips.
    bool inShotGroup = hasClip && !isCaption;
    m_shotSection->setVisible(inShotGroup);
    if (inShotGroup)
        updateShotSection();

    m_videoSection->setVisible(hasClip && m_clip->clipType() == ClipType::Video);
    m_audioSection->setVisible(hasClip && m_clip->clipType() == ClipType::Audio);
    if (m_audioFxSection)
        m_audioFxSection->setVisible(hasClip && m_clip->clipType() == ClipType::Audio);
    m_titleSection->setVisible(hasClip && m_clip->clipType() == ClipType::Title);
    m_graphicSection->setVisible(hasClip && m_clip->clipType() == ClipType::Graphic);
    if (m_puppetSection)
        m_puppetSection->setVisible(hasClip && m_clip->clipType() == ClipType::PngPuppet);
}

// ═════════════════════════════════════════════════════════════════════════════
//  Populate widgets from clip
// ═════════════════════════════════════════════════════════════════════════════

void PropertiesPanel::setTransition(Track* track, size_t transitionIndex)
{
    m_track = track;
    m_transitionIndex = transitionIndex;
    m_clip = nullptr; // clear clip selection
    m_multiSelection.clear();

    // Hide clip sections
    m_identitySection->setVisible(false);
    m_transformSection->setVisible(false);
    if (m_effectsSection) m_effectsSection->setVisible(false);
    m_spineSection->setVisible(false);
    m_characterSection->setVisible(false);
    m_animationSection->setVisible(false);
    m_shotSection->setVisible(false);
    m_videoSection->setVisible(false);
    m_audioSection->setVisible(false);
    if (m_audioFxSection) m_audioFxSection->setVisible(false);
    m_titleSection->setVisible(false);
    m_graphicSection->setVisible(false);
    if (m_captionSection) m_captionSection->setVisible(false);

    m_transitionSection->setVisible(true);
    populateFromTransition();

    // Show scroll area for transition editing
    m_scrollArea->setVisible(true);
    m_emptyLabel->setVisible(false);
    if (m_statusLabel) m_statusLabel->setText(QStringLiteral("Transition"));
}


QSize PropertiesPanel::sizeHint() const
{
    return QSize(280, 500);
}

} // namespace rt

