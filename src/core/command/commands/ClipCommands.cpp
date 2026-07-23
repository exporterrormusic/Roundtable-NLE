/*
 * ClipCommands.cpp — Clip command implementations.
 * Step 4: Command System
 */

#include "command/commands/ClipCommands.h"
#include "timeline/AudioClip.h"
#include "timeline/Track.h"
#include "timeline/Clip.h"
#include "timeline/GraphicClip.h"
#include "timeline/TitleClip.h"

namespace rt {

// ── AddClipCommand ──────────────────────────────────────────────────────────

AddClipCommand::AddClipCommand(Track* track, std::unique_ptr<Clip> clip)
    : m_track(track)
    , m_clip(std::move(clip))
    , m_clipId(m_clip ? m_clip->id() : 0)
{
}

void AddClipCommand::execute()
{
    if (m_clip)
    {
        m_track->addClip(std::move(m_clip));
        m_clip = nullptr; // Track now owns it
    }
}

void AddClipCommand::undo()
{
    m_clip = m_track->removeClipById(m_clipId);
}

std::string AddClipCommand::description() const
{
    return "Add Clip";
}

// ── RemoveClipCommand ───────────────────────────────────────────────────────

RemoveClipCommand::RemoveClipCommand(Track* track, uint64_t clipId)
    : m_track(track)
    , m_clipId(clipId)
{
}

void RemoveClipCommand::execute()
{
    // Capture transitions that reference this clip BEFORE removeClipById drops
    // them, so undo can restore the dissolve/fade. (Re-captured on every redo,
    // keyed by clip id, so this stays correct across undo/redo cycles.)
    m_savedTransitions.clear();
    if (m_track) {
        for (const auto& t : m_track->transitions()) {
            if (t.leftClipId == m_clipId || t.rightClipId == m_clipId)
                m_savedTransitions.push_back(t);
        }
    }
    m_clip = m_track->removeClipById(m_clipId);
}

void RemoveClipCommand::undo()
{
    if (m_clip)
    {
        m_track->addClip(std::move(m_clip));
        m_clip = nullptr;
        // Re-attach the transitions dropped on execute. addTransition() is
        // add-or-replace keyed on the clip-id edit point, so this won't stack
        // duplicates if one already exists.
        for (const auto& t : m_savedTransitions)
            m_track->addTransition(t);
        m_savedTransitions.clear();
    }
}

std::string RemoveClipCommand::description() const
{
    return "Remove Clip";
}

// ── MoveClipCommand ─────────────────────────────────────────────────────────

MoveClipCommand::MoveClipCommand(Track* track, uint64_t clipId, int64_t newPosition)
    : m_track(track)
    , m_clipId(clipId)
    , m_oldPosition(0)
    , m_newPosition(newPosition)
{
}

void MoveClipCommand::execute()
{
    size_t idx = m_track->findClipIndexById(m_clipId);
    if (idx == m_track->clipCount()) return;

    m_oldPosition = m_track->clip(idx)->timelineIn();
    m_track->moveClip(idx, m_newPosition);
}

void MoveClipCommand::undo()
{
    size_t idx = m_track->findClipIndexById(m_clipId);
    if (idx == m_track->clipCount()) return;

    m_track->moveClip(idx, m_oldPosition);
}

std::string MoveClipCommand::description() const
{
    return "Move Clip";
}

bool MoveClipCommand::mergeWith(const Command& next)
{
    auto* moveCmd = dynamic_cast<const MoveClipCommand*>(&next);
    if (!moveCmd) return false;
    if (moveCmd->m_clipId != m_clipId || moveCmd->m_track != m_track) return false;

    // Absorb the new destination, keep our original old position
    m_newPosition = moveCmd->m_newPosition;
    return true;
}

// ── TrimClipCommand ─────────────────────────────────────────────────────────

TrimClipCommand::TrimClipCommand(Track* track, uint64_t clipId,
                                 int64_t newTimelineIn, int64_t newDuration, int64_t newSourceIn,
                                 bool preserveKeyframeTimes)
    : m_track(track)
    , m_clipId(clipId)
    , m_oldTimelineIn(0), m_newTimelineIn(newTimelineIn)
    , m_oldDuration(0),   m_newDuration(newDuration)
    , m_oldSourceIn(0),   m_newSourceIn(newSourceIn)
    , m_preserveKeyframeTimes(preserveKeyframeTimes)
{
}

namespace {
void shiftTrackKeyframes(KeyframeTrack<float>& track, int64_t delta)
{
    for (size_t i = 0; i < track.keyframeCount(); ++i)
        track.keyframe(i).time += delta;
}

void shiftMaskKeyframes(OpacityMask& mask, int64_t delta)
{
    shiftTrackKeyframes(mask.feather, delta);
    shiftTrackKeyframes(mask.maskOpacity, delta);
    shiftTrackKeyframes(mask.expansion, delta);
    for (auto& key : mask.pathKeys)
        key.time += delta;
}

// Keyframe times are clip-relative. When a head trim changes timelineIn, move
// every local key by the opposite amount so timelineIn + key.time remains
// constant. Keys outside the newly visible clip range are intentionally kept:
// extending the edge again must reveal the original animation (Premiere-style).
void preserveKeyframeTimelineTimes(Clip& clip, int64_t newTimelineIn)
{
    const int64_t delta = clip.timelineIn() - newTimelineIn;
    if (delta == 0) return;

    shiftTrackKeyframes(clip.speedRamp(), delta);
    shiftTrackKeyframes(clip.opacity(), delta);
    shiftTrackKeyframes(clip.positionX(), delta);
    shiftTrackKeyframes(clip.positionY(), delta);
    shiftTrackKeyframes(clip.scaleX(), delta);
    shiftTrackKeyframes(clip.scaleY(), delta);
    shiftTrackKeyframes(clip.rotation(), delta);
    shiftTrackKeyframes(clip.shutterAngle(), delta);
    shiftTrackKeyframes(clip.anchorX(), delta);
    shiftTrackKeyframes(clip.anchorY(), delta);

    if (auto* audio = dynamic_cast<AudioClip*>(&clip)) {
        shiftTrackKeyframes(audio->volume(), delta);
        shiftTrackKeyframes(audio->pan(), delta);
    }
    if (auto* title = dynamic_cast<TitleClip*>(&clip)) {
        shiftTrackKeyframes(title->tracking(), delta);
        shiftTrackKeyframes(title->lineHeight(), delta);
    }
    if (auto* graphic = dynamic_cast<GraphicClip*>(&clip)) {
        for (size_t i = 0; i < graphic->layerCount(); ++i) {
            GraphicLayer* layer = graphic->layer(i);
            if (!layer) continue;
            auto& transform = layer->transform();
            shiftTrackKeyframes(transform.posX, delta);
            shiftTrackKeyframes(transform.posY, delta);
            shiftTrackKeyframes(transform.scaleX, delta);
            shiftTrackKeyframes(transform.scaleY, delta);
            shiftTrackKeyframes(transform.rotation, delta);
            shiftTrackKeyframes(transform.anchorX, delta);
            shiftTrackKeyframes(transform.anchorY, delta);
            shiftTrackKeyframes(transform.opacity, delta);
            if (auto* text = dynamic_cast<TextLayer*>(layer)) {
                shiftTrackKeyframes(text->tracking(), delta);
                shiftTrackKeyframes(text->leading(), delta);
                shiftTrackKeyframes(text->baselineShift(), delta);
            }
        }
    }

    for (auto& mask : clip.masks())
        shiftMaskKeyframes(mask, delta);

    auto& effects = clip.effects();
    for (size_t i = 0; i < effects.effectCount(); ++i) {
        Effect& effect = effects.effect(i);
        for (size_t p = 0; p < effect.paramCount(); ++p)
            shiftTrackKeyframes(effect.param(p).track, delta);
        for (auto& mask : effect.masks())
            shiftMaskKeyframes(mask, delta);
    }
}

// Keep transitions anchored to the clip edge they belong to. A transition
// stores an absolute editPointTick; when the clip is trimmed/split the
// edge moves, so without this the dissolve stays at the old tick (renders
// in empty space, or "cut off" instead of moving with the clip).
void retargetTransitionsForClip(Track* track, uint64_t clipId, const Clip* c)
{
    if (!track || !c) return;
    const int64_t inTick  = c->timelineIn();
    const int64_t outTick = c->timelineOut();
    for (size_t i = 0; i < track->transitionCount(); ++i) {
        const Transition* tp = track->transition(i);
        if (!tp) continue;
        Transition t = *tp;
        bool changed = false;
        // Clip on the LEFT side of the edit → edit point is its tail.
        if (t.leftClipId == clipId) {
            if (t.editPointTick != outTick) { t.editPointTick = outTick; changed = true; }
        }
        // Clip on the RIGHT side of the edit → edit point is its head.
        else if (t.rightClipId == clipId) {
            if (t.editPointTick != inTick) { t.editPointTick = inTick; changed = true; }
        }
        if (changed) track->setTransition(i, t);
    }
}
} // namespace

void TrimClipCommand::execute()
{
    size_t idx = m_track->findClipIndexById(m_clipId);
    if (idx == m_track->clipCount()) return;

    Clip* c = m_track->clip(idx);
    m_oldTimelineIn = c->timelineIn();
    m_oldDuration   = c->duration();
    m_oldSourceIn   = c->sourceIn();

    if (m_preserveKeyframeTimes)
        preserveKeyframeTimelineTimes(*c, m_newTimelineIn);
    c->setTimelineIn(m_newTimelineIn);
    c->setDuration(m_newDuration);
    c->setSourceIn(m_newSourceIn);

    retargetTransitionsForClip(m_track, m_clipId, c);
}

void TrimClipCommand::undo()
{
    size_t idx = m_track->findClipIndexById(m_clipId);
    if (idx == m_track->clipCount()) return;

    Clip* c = m_track->clip(idx);
    if (m_preserveKeyframeTimes)
        preserveKeyframeTimelineTimes(*c, m_oldTimelineIn);
    c->setTimelineIn(m_oldTimelineIn);
    c->setDuration(m_oldDuration);
    c->setSourceIn(m_oldSourceIn);

    retargetTransitionsForClip(m_track, m_clipId, c);
}

std::string TrimClipCommand::description() const
{
    return "Trim Clip";
}

bool TrimClipCommand::mergeWith(const Command& next)
{
    auto* trimCmd = dynamic_cast<const TrimClipCommand*>(&next);
    if (!trimCmd) return false;
    if (trimCmd->m_clipId != m_clipId || trimCmd->m_track != m_track) return false;
    if (trimCmd->m_preserveKeyframeTimes != m_preserveKeyframeTimes) return false;

    // Absorb new values, keep our original old values
    m_newTimelineIn = trimCmd->m_newTimelineIn;
    m_newDuration   = trimCmd->m_newDuration;
    m_newSourceIn   = trimCmd->m_newSourceIn;
    return true;
}

} // namespace rt

