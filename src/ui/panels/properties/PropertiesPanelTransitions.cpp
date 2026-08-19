/*
 * PropertiesPanelTransitions.cpp - Transition property methods extracted from PropertiesPanelApply.cpp.
 */

#include "panels/properties/PropertiesPanel.h"
#include "widgets/ScrubbySpinBox.h"
#include "timeline/Clip.h"
#include "timeline/Track.h"
#include "timeline/Transition.h"
#include "command/CommandStack.h"
#include "command/LambdaCommand.h"

#include <QComboBox>
#include <QLabel>

#include <cmath>

namespace rt {
// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•

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

void PropertiesPanel::populateFromTransition()
{
    if (!m_track || m_transitionIndex >= m_track->transitionCount()) return;
    const auto* tr = m_track->transition(m_transitionIndex);
    if (!tr) return;

    m_updating = true;

    // Header
    m_headerLabel->setText(transitionTypeName(tr->type));
    m_typeLabel->setText("Transition");

    // Type
    m_transTypeCombo->setCurrentIndex(static_cast<int>(tr->type));

    // Duration in frames (ticks / ticksPerFrame)
    // Default assumes 30fps -> 1600 ticks/frame
    constexpr double kTicksPerFrame = 1600.0;
    double frames = static_cast<double>(tr->duration) / kTicksPerFrame;
    m_transDurationSpin->setValue(frames);

    // Softness (param1: 0.0 â€“ 1.0 â†’ display as 0 â€“ 100%)
    m_transSoftnessSpin->setValue(static_cast<double>(tr->param1) * 100.0);

    // Alignment: determine from offset relative to duration
    // offset == 0                   â†’ Center on Cut (default)
    // offset == -duration/2         â†’ Start at Cut (transition starts at edit point)
    // offset == +duration/2         â†’ End at Cut (transition ends at edit point)
    if (tr->leftClipId == 0 || tr->rightClipId == 0) {
        m_transAlignCombo->setCurrentIndex(1); // fades are always edge-aligned
    } else {
        m_transAlignCombo->setCurrentIndex(1); // center on cut (default for now)
    }

    // Clip labels
    auto findClipLabel = [this](uint64_t clipId) -> QString {
        if (clipId == 0) return "(none)";
        for (size_t i = 0; i < m_track->clipCount(); ++i) {
            if (m_track->clip(i)->id() == clipId)
                return QString::fromStdString(m_track->clip(i)->label());
        }
        return QStringLiteral("Clip %1").arg(clipId);
    };
    m_transClipALabel->setText(findClipLabel(tr->leftClipId));
    m_transClipBLabel->setText(findClipLabel(tr->rightClipId));

    m_updating = false;
}

// â”€â”€ Transition apply methods â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

void PropertiesPanel::applyTransitionType()
{
    if (m_updating || !m_track || m_track->isLocked()
        || m_transitionIndex >= m_track->transitionCount()) return;
    const auto* tr = m_track->transition(m_transitionIndex);
    if (!tr) return;
    auto newType = static_cast<TransitionType>(m_transTypeCombo->currentIndex());
    if (newType == tr->type) return;

    auto oldTr = *tr;
    auto newTr = oldTr;
    newTr.type = newType;

    auto* track = m_track;
    size_t idx = m_transitionIndex;
    if (m_commandStack) {
        m_commandStack->execute(std::make_unique<LambdaCommand>(
            "Change transition type",
            [track, idx, newTr, this]() { track->setTransition(idx, newTr, TrackMutationPolicy::BypassLock); populateFromTransition(); emit propertyChanged(); },
            [track, idx, oldTr, this]() { track->setTransition(idx, oldTr, TrackMutationPolicy::BypassLock); populateFromTransition(); emit propertyChanged(); }));
    } else {
        track->setTransition(idx, newTr);
        populateFromTransition();
        emit propertyChanged();
    }
}

void PropertiesPanel::applyTransitionDuration()
{
    if (m_updating || !m_track || m_track->isLocked()
        || m_transitionIndex >= m_track->transitionCount()) return;
    const auto* tr = m_track->transition(m_transitionIndex);
    if (!tr) return;

    constexpr double kTicksPerFrame = 1600.0;
    int64_t newDuration = static_cast<int64_t>(m_transDurationSpin->value() * kTicksPerFrame);
    if (newDuration == tr->duration) return;

    auto oldTr = *tr;
    auto newTr = oldTr;
    newTr.duration = newDuration;

    auto* track = m_track;
    size_t idx = m_transitionIndex;
    if (m_commandStack) {
        m_commandStack->execute(std::make_unique<LambdaCommand>(
            "Change transition duration",
            [track, idx, newTr, this]() { track->setTransition(idx, newTr, TrackMutationPolicy::BypassLock); populateFromTransition(); emit propertyChanged(); },
            [track, idx, oldTr, this]() { track->setTransition(idx, oldTr, TrackMutationPolicy::BypassLock); populateFromTransition(); emit propertyChanged(); }));
    } else {
        track->setTransition(idx, newTr);
        populateFromTransition();
        emit propertyChanged();
    }
}

void PropertiesPanel::applyTransitionSoftness()
{
    if (m_updating || !m_track || m_track->isLocked()
        || m_transitionIndex >= m_track->transitionCount()) return;
    const auto* tr = m_track->transition(m_transitionIndex);
    if (!tr) return;

    float newSoftness = static_cast<float>(m_transSoftnessSpin->value() / 100.0);
    if (std::abs(newSoftness - tr->param1) < 0.0001f) return;

    auto oldTr = *tr;
    auto newTr = oldTr;
    newTr.param1 = newSoftness;

    auto* track = m_track;
    size_t idx = m_transitionIndex;
    if (m_commandStack) {
        m_commandStack->execute(std::make_unique<LambdaCommand>(
            "Change transition softness",
            [track, idx, newTr, this]() { track->setTransition(idx, newTr, TrackMutationPolicy::BypassLock); populateFromTransition(); emit propertyChanged(); },
            [track, idx, oldTr, this]() { track->setTransition(idx, oldTr, TrackMutationPolicy::BypassLock); populateFromTransition(); emit propertyChanged(); }));
    } else {
        track->setTransition(idx, newTr);
        populateFromTransition();
        emit propertyChanged();
    }
}

void PropertiesPanel::applyTransitionAlignment()
{
    if (m_updating || !m_track || m_track->isLocked()
        || m_transitionIndex >= m_track->transitionCount()) return;
    const auto* tr = m_track->transition(m_transitionIndex);
    if (!tr) return;

    // Alignment modifies the offset relative to the edit point.
    // 0 = Start at Cut: offset = 0 (transition starts at edit point)
    // 1 = Center on Cut: offset = -duration/2
    // 2 = End at Cut: offset = -duration
    int alignIdx = m_transAlignCombo->currentIndex();
    int64_t newOffset;
    switch (alignIdx) {
    case 0:  newOffset = 0; break;
    case 2:  newOffset = -tr->duration; break;
    default: newOffset = -tr->duration / 2; break;
    }
    if (newOffset == tr->offset) return;

    auto oldTr = *tr;
    auto newTr = oldTr;
    newTr.offset = newOffset;

    auto* track = m_track;
    size_t idx = m_transitionIndex;
    if (m_commandStack) {
        m_commandStack->execute(std::make_unique<LambdaCommand>(
            "Change transition alignment",
            [track, idx, newTr, this]() { track->setTransition(idx, newTr, TrackMutationPolicy::BypassLock); populateFromTransition(); emit propertyChanged(); },
            [track, idx, oldTr, this]() { track->setTransition(idx, oldTr, TrackMutationPolicy::BypassLock); populateFromTransition(); emit propertyChanged(); }));
    } else {
        track->setTransition(idx, newTr);
        populateFromTransition();
        emit propertyChanged();
    }
}
} // namespace rt

