/*
 * EditOperations.cpp -- move, resolve, delete, clipboard, navigation,
 * lift/extract, and close-gaps.
 *
 * SelectionSet/SnapEngine --> EditOperationsSelection.cpp
 * Split/Trim/Slip/Slide   --> EditOperationsTrim.cpp
 */

#include "timeline/EditOperations.h"

#include "timeline/Timeline.h"
#include "timeline/Track.h"
#include "timeline/Clip.h"
#include "timeline/VideoClip.h"
#include "timeline/AudioClip.h"
#include "timeline/GraphicClip.h"
#include "timeline/TitleClip.h"
#include "timeline/Marker.h"
#include "command/Command.h"
#include "command/CompoundCommand.h"
#include "command/LambdaCommand.h"
#include "command/commands/ClipCommands.h"
#include "command/commands/TrackCommands.h"
#include "command/commands/TransitionCmds.h"

#include <spdlog/spdlog.h>
#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_map>
#include <unordered_set>

namespace rt {

namespace {

// Overwrite remnants are independent clips. Since keyframe times are local to
// a clip, a verbatim clone duplicates the animation and a head trim moves it.
// Keep only keys in the surviving interval and rebase them to its local zero.
KeyframeTrack<float> sliceKeyframeTrack(const KeyframeTrack<float>& source,
                                        int64_t localIn, int64_t localOut)
{
    KeyframeTrack<float> result(source.evaluate(localIn));
    for (const auto& key : source.keyframes()) {
        if (key.time < localIn || key.time >= localOut)
            continue;
        auto rebased = key;
        rebased.time -= localIn;
        result.restoreKeyframe(rebased);
    }
    return result;
}

OpacityMask sliceOpacityMask(const OpacityMask& source,
                             int64_t localIn, int64_t localOut)
{
    OpacityMask result = source;
    const MaskGeometry boundaryGeometry = source.geometryAt(localIn);

    result.feather = sliceKeyframeTrack(source.feather, localIn, localOut);
    result.maskOpacity = sliceKeyframeTrack(source.maskOpacity, localIn, localOut);
    result.expansion = sliceKeyframeTrack(source.expansion, localIn, localOut);

    result.pathKeys.clear();
    for (const auto& key : source.pathKeys) {
        if (key.time < localIn || key.time >= localOut)
            continue;
        auto rebased = key;
        rebased.time -= localIn;
        result.pathKeys.push_back(std::move(rebased));
    }
    if (result.pathKeys.empty())
        result.base = boundaryGeometry;

    return result;
}

struct ClipKeyframeTargets
{
    std::vector<KeyframeTrack<float>*> tracks;
    std::vector<OpacityMask*> masks;
};

ClipKeyframeTargets collectClipKeyframeTargets(Clip& clip)
{
    ClipKeyframeTargets result;
    auto addTrack = [&result](KeyframeTrack<float>& track) {
        result.tracks.push_back(&track);
    };
    auto addMask = [&result](OpacityMask& mask) {
        result.masks.push_back(&mask);
    };

    addTrack(clip.speedRamp());
    addTrack(clip.opacity());
    addTrack(clip.positionX());
    addTrack(clip.positionY());
    addTrack(clip.scaleX());
    addTrack(clip.scaleY());
    addTrack(clip.rotation());
    addTrack(clip.shutterAngle());
    addTrack(clip.anchorX());
    addTrack(clip.anchorY());

    if (auto* audio = dynamic_cast<AudioClip*>(&clip)) {
        addTrack(audio->volume());
        addTrack(audio->pan());
    }
    if (auto* title = dynamic_cast<TitleClip*>(&clip)) {
        addTrack(title->tracking());
        addTrack(title->lineHeight());
    }
    if (auto* graphic = dynamic_cast<GraphicClip*>(&clip)) {
        for (size_t i = 0; i < graphic->layerCount(); ++i) {
            GraphicLayer* layer = graphic->layer(i);
            if (!layer) continue;
            auto& transform = layer->transform();
            addTrack(transform.posX);
            addTrack(transform.posY);
            addTrack(transform.scaleX);
            addTrack(transform.scaleY);
            addTrack(transform.rotation);
            addTrack(transform.anchorX);
            addTrack(transform.anchorY);
            addTrack(transform.opacity);
            if (auto* text = dynamic_cast<TextLayer*>(layer)) {
                addTrack(text->tracking());
                addTrack(text->leading());
                addTrack(text->baselineShift());
            }
        }
    }

    for (auto& mask : clip.masks())
        addMask(mask);

    auto& effects = clip.effects();
    for (size_t i = 0; i < effects.effectCount(); ++i) {
        Effect& effect = effects.effect(i);
        for (size_t p = 0; p < effect.paramCount(); ++p)
            addTrack(effect.param(p).track);
        for (auto& mask : effect.masks())
            addMask(mask);
    }

    return result;
}

void sliceClipKeyframes(Clip& clip, int64_t localIn, int64_t localOut)
{
    auto targets = collectClipKeyframeTargets(clip);
    for (auto* track : targets.tracks)
        *track = sliceKeyframeTrack(*track, localIn, localOut);
    for (auto* mask : targets.masks)
        *mask = sliceOpacityMask(*mask, localIn, localOut);
}

struct ClipKeyframeSliceState
{
    ClipKeyframeTargets targets;
    std::vector<KeyframeTrack<float>> beforeTracks;
    std::vector<KeyframeTrack<float>> afterTracks;
    std::vector<OpacityMask> beforeMasks;
    std::vector<OpacityMask> afterMasks;
};

std::unique_ptr<Command> sliceClipKeyframesCommand(Clip& clip,
                                                   int64_t localIn,
                                                   int64_t localOut)
{
    auto state = std::make_shared<ClipKeyframeSliceState>();
    state->targets = collectClipKeyframeTargets(clip);

    for (auto* track : state->targets.tracks) {
        state->beforeTracks.push_back(*track);
        state->afterTracks.push_back(sliceKeyframeTrack(*track, localIn, localOut));
    }
    for (auto* mask : state->targets.masks) {
        state->beforeMasks.push_back(*mask);
        state->afterMasks.push_back(sliceOpacityMask(*mask, localIn, localOut));
    }

    auto apply = [state](bool after) {
        const auto& tracks = after ? state->afterTracks : state->beforeTracks;
        const auto& masks = after ? state->afterMasks : state->beforeMasks;
        for (size_t i = 0; i < state->targets.tracks.size(); ++i)
            *state->targets.tracks[i] = tracks[i];
        for (size_t i = 0; i < state->targets.masks.size(); ++i)
            *state->targets.masks[i] = masks[i];
    };

    return std::make_unique<LambdaCommand>(
        "Preserve overwrite keyframes",
        [apply]() { apply(true); },
        [apply]() { apply(false); });
}

} // namespace

// ─── Move ───────────────────────────────────────────────────────────────────

std::unique_ptr<Command> EditOperations::moveClip(
    Timeline& timeline, size_t trackIndex, uint64_t clipId,
    int64_t newTimelineIn)
{
    if (trackIndex >= timeline.trackCount())
        return nullptr;

    Track* track = timeline.track(trackIndex);
    if (!track || track->isLocked())
        return nullptr;
    size_t idx = track->findClipIndexById(clipId);
    if (idx == track->clipCount())
        return nullptr;

    newTimelineIn = std::max<int64_t>(0, newTimelineIn);

    return std::make_unique<MoveClipCommand>(track, clipId, newTimelineIn);
}

std::unique_ptr<Command> EditOperations::moveClipToTrack(
    Timeline& timeline, size_t fromTrack, size_t toTrack,
    uint64_t clipId, int64_t newTimelineIn)
{
    if (fromTrack >= timeline.trackCount() || toTrack >= timeline.trackCount())
        return nullptr;
    if (fromTrack == toTrack)
        return moveClip(timeline, fromTrack, clipId, newTimelineIn);

    Track* srcTrack = timeline.track(fromTrack);
    Track* dstTrack = timeline.track(toTrack);

    // Dividers cannot hold clips (Track::addClip returns nullptr). Without
    // this guard the AddClipCommand would silently no-op while the paired
    // RemoveClipCommand still pulls the clip off srcTrack — the clip ends
    // up destroyed instead of moved.
    if (!srcTrack || !dstTrack || srcTrack->isLocked() ||
        dstTrack->isLocked() || dstTrack->isDivider())
        return nullptr;

    size_t idx = srcTrack->findClipIndexById(clipId);
    if (idx == srcTrack->clipCount())
        return nullptr;

    newTimelineIn = std::max<int64_t>(0, newTimelineIn);

    // ── Save single-sided transitions before removal ────────────────────
    // Single-sided fades referencing the moved clip follow it to the
    // destination track.  Two-sided dissolves where the OTHER side stays
    // on srcTrack are intentionally dropped (matches Premiere — moving
    // one half of a dissolve removes it).  Joint moves where BOTH halves
    // travel together are handled by the caller before reaching here:
    // it strips the shared transition out of srcTrack and re-adds it on
    // dstTrack after the moves complete.
    const Clip* clip = srcTrack->clip(idx);
    std::vector<Transition> savedTransitions;
    for (const auto& t : srcTrack->transitions()) {
        bool isSingleSided = (t.leftClipId == 0) || (t.rightClipId == 0);
        bool referencesClip = (t.leftClipId == clipId) || (t.rightClipId == clipId);
        if (isSingleSided && referencesClip) {
            savedTransitions.push_back(t);
        }
    }

    // Clone the clip for the destination track, preserving its ID so any
    // transitions that reference this clipId (saved single-sided fades,
    // or joint-move shared transitions added by the caller) stay valid.
    // The original is removed in the same compound, so there is no ID
    // collision at any point.
    auto cloned = clip->clone();
    cloned->setId(clipId);
    cloned->setTimelineIn(newTimelineIn);
    const int64_t newOut = cloned->timelineOut();

    auto compound = std::make_unique<CompoundCommand>("Move clip to track");

    // Remove from source track (this drops transitions referencing clipId
    // on srcTrack, which is fine — single-sided ones were saved above and
    // any shared joint-move transitions were already extracted by caller).
    compound->addCommand(std::make_unique<RemoveClipCommand>(srcTrack, clipId));

    // Add to destination track (with the original clipId preserved).
    compound->addCommand(std::make_unique<AddClipCommand>(dstTrack, std::move(cloned)));

    // Restore the single-sided transitions on the destination track,
    // re-anchoring editPointTick to the clip's new position.  Clip ID is
    // unchanged so leftClipId / rightClipId still resolve correctly.
    for (auto& t : savedTransitions) {
        if (t.leftClipId == clipId) {
            t.editPointTick = newOut;
        }
        if (t.rightClipId == clipId) {
            t.editPointTick = newTimelineIn;
        }
        // clipIndexA / clipIndexB are unused by AddTransitionCommand (the
        // Transition struct already carries the clip IDs).
        compound->addCommand(std::make_unique<AddTransitionCommand>(
            dstTrack, 0, 0, t));
    }

    return compound;
}

// ─── Overwrite / Overlap Resolution ──────────────────────────────────────────

int64_t EditOperations::nonOverlappingInsertDuration(
    const Track& track, int64_t timelineIn, int64_t requestedDuration)
{
    if (requestedDuration <= 0) return 0;

    for (size_t i = 0; i < track.clipCount(); ++i) {
        const Clip* clip = track.clip(i);
        if (!clip) continue;

        if (timelineIn >= clip->timelineIn() && timelineIn < clip->timelineOut())
            return 0;

        if (clip->timelineIn() > timelineIn)
            return std::min(requestedDuration, clip->timelineIn() - timelineIn);
    }

    return requestedDuration;
}

std::unique_ptr<Command> EditOperations::resolveOverlaps(
    Timeline& timeline, size_t trackIndex, uint64_t movedClipId)
{
    return resolveOverlaps(timeline, trackIndex, movedClipId, {});
}

std::unique_ptr<Command> EditOperations::resolveOverlaps(
    Timeline& timeline, size_t trackIndex, uint64_t movedClipId,
    const std::unordered_set<uint64_t>& excludeClipIds)
{
    if (trackIndex >= timeline.trackCount())
        return nullptr;

    Track* track = timeline.track(trackIndex);
    if (!track || track->isLocked())
        return nullptr;
    size_t movedIdx = track->findClipIndexById(movedClipId);
    if (movedIdx == track->clipCount()) {
        spdlog::warn("[OVERLAP-DIAG] resolveOverlaps: clip id={} NOT FOUND on track {}",
                     movedClipId, trackIndex);
        return nullptr;
    }

    const Clip* movedClip = track->clip(movedIdx);
    const int64_t movedIn  = movedClip->timelineIn();
    const int64_t movedOut = movedClip->timelineOut();

    spdlog::info("[OVERLAP-DIAG] resolveOverlaps: track={} movedClip id={} range=[{}, {}) "
                 "clipCount={} excludeCount={}",
                 trackIndex, movedClipId, movedIn, movedOut, track->clipCount(),
                 excludeClipIds.size());

    auto compound = std::make_unique<CompoundCommand>("Resolve overlaps");

    // Collect IDs of clips that overlap the moved clip
    for (size_t i = 0; i < track->clipCount(); ++i) {
        Clip* other = track->clip(i);
        if (other->id() == movedClipId) continue;  // skip the moved clip itself

        int64_t otherIn  = other->timelineIn();
        int64_t otherOut = other->timelineOut();

        // No overlap?
        if (otherOut <= movedIn || otherIn >= movedOut) continue;

        // `excludeClipIds` used to bypass resolution here for fellow clips in
        // a multi-selection. That made it possible for graphics from separate
        // tracks to collapse onto one destination track and remain stacked in
        // the same lane. Keep the parameter for compatibility, but never let
        // it override the track's non-overlap invariant.
        if (excludeClipIds.count(other->id())) {
            spdlog::debug("resolveOverlaps: resolving excluded moving clip id={} because ranges overlap",
                          other->id());
        }

        spdlog::info("[OVERLAP-DIAG]   OVERLAP FOUND: other clip id={} range=[{}, {}) vs moved=[{}, {})",
                     other->id(), otherIn, otherOut, movedIn, movedOut);

        // Fully covered by the moved clip → remove entirely
        if (otherIn >= movedIn && otherOut <= movedOut) {
            compound->addCommand(
                std::make_unique<RemoveClipCommand>(track, other->id()));
            continue;
        }

        // Partial overlap — trim the other clip
        if (otherIn < movedIn && otherOut > movedOut) {
            // The moved clip lands in the MIDDLE of the other clip.
            // Create a right remnant first (clone before trimming the original).
            int64_t rightIn       = movedOut;
            int64_t rightDuration = otherOut - movedOut;
            // Convert timeline-tick delta to source ticks via speed
            double otherSpd = std::max(other->speed(), 0.01);
            int64_t rightSourceIn = other->sourceIn() + static_cast<int64_t>(std::llround((movedOut - otherIn) * otherSpd));
            auto rightClip = other->clone();
            rightClip->setTimelineIn(rightIn);
            rightClip->setDuration(rightDuration);
            rightClip->setSourceIn(rightSourceIn);
            const int64_t rightLocalIn = movedOut - otherIn;
            sliceClipKeyframes(*rightClip, rightLocalIn, other->duration());
            compound->addCommand(std::make_unique<AddClipCommand>(
                track, std::move(rightClip)));

            // Trim the original clip's tail to the moved clip's in-point (left remnant).
            int64_t newDuration = movedIn - otherIn;
            compound->addCommand(std::make_unique<TrimClipCommand>(
                track, other->id(),
                otherIn,           // timelineIn stays
                newDuration,       // trimmed duration
                other->sourceIn() // sourceIn stays
            ));
            compound->addCommand(sliceClipKeyframesCommand(
                *other, 0, newDuration));
        }
        else if (otherIn < movedIn) {
            // Other clip's tail extends into the moved clip → trim tail
            int64_t newDuration = movedIn - otherIn;
            compound->addCommand(std::make_unique<TrimClipCommand>(
                track, other->id(),
                otherIn,             // timelineIn stays
                newDuration,         // new shorter duration
                other->sourceIn()    // sourceIn stays
            ));
            compound->addCommand(sliceClipKeyframesCommand(
                *other, 0, newDuration));
        }
        else {
            // Other clip's head is inside the moved clip → trim head
            int64_t trimAmount = movedOut - otherIn;
            int64_t newIn = movedOut;
            int64_t newDuration = other->duration() - trimAmount;
            int64_t newSourceIn = other->sourceIn() + trimAmount;
            if (newDuration <= 0) {
                compound->addCommand(
                    std::make_unique<RemoveClipCommand>(track, other->id()));
            } else {
                compound->addCommand(std::make_unique<TrimClipCommand>(
                    track, other->id(),
                    newIn,          // move timelineIn forward
                    newDuration,    // shorter duration
                    newSourceIn,    // advance sourceIn
                    false           // slice command below owns keyframe rebasing
                ));
                compound->addCommand(sliceClipKeyframesCommand(
                    *other, trimAmount, other->duration()));
            }
        }
    }

    if (compound->size() == 0) {
        spdlog::info("[OVERLAP-DIAG] resolveOverlaps: NO overlaps found (compound empty)");
        return nullptr;  // No overlaps found
    }

    spdlog::info("[OVERLAP-DIAG] resolveOverlaps: {} overlap commands generated",
                 compound->size());
    return compound;
}

// ─── Delete ──────────────────────────────────────────────────────────────────

std::unique_ptr<Command> EditOperations::deleteSelection(
    Timeline& timeline, const SelectionSet& selection)
{
    if (selection.empty()) return nullptr;

    auto compound = std::make_unique<CompoundCommand>("Delete clips");

    for (const auto& ref : selection.clips())
    {
        if (ref.trackIndex >= timeline.trackCount()) continue;
        Track* track = timeline.track(ref.trackIndex);
        if (!track || track->isLocked()) continue;
        if (track->findClipIndexById(ref.clipId) == track->clipCount()) continue;

        compound->addCommand(std::make_unique<RemoveClipCommand>(
            track, ref.clipId));
    }

    return compound->size() > 0 ? std::move(compound) : nullptr;
}

// ─── Clipboard ───────────────────────────────────────────────────────────────

// Ascending-index list of REAL video tracks (excludes the V/A divider AND
// the pinned caption/subtitle track, neither of which is a valid paste
// target). front() = topmost video, back() = V1 (bottom, adjacent to the
// divider).
static std::vector<size_t> realVideoTracks(const Timeline& tl)
{
    std::vector<size_t> v;
    for (size_t i = 0; i < tl.trackCount(); ++i) {
        const Track* t = tl.track(i);
        if (t && t->type() == TrackType::Video && !t->isDivider() && !t->isCaptionTrack())
            v.push_back(i);
    }
    return v;
}

// Ascending-index list of audio tracks. front() = A1 (top, adjacent to the
// divider), back() = bottom-most audio.
static std::vector<size_t> realAudioTracks(const Timeline& tl)
{
    std::vector<size_t> v;
    for (size_t i = 0; i < tl.trackCount(); ++i) {
        const Track* t = tl.track(i);
        if (t && t->type() == TrackType::Audio && !t->isDivider())
            v.push_back(i);
    }
    return v;
}

// Offset of source track `ti` from its type's "track 1": video counts up
// from V1 (bottom), audio counts down from A1 (top). Returns 0 if the track
// can't be classified (safe fallback to V1/A1).
static int trackOffsetFromBase(const Timeline& tl, size_t ti)
{
    const Track* t = tl.track(ti);
    if (!t) return 0;
    if (t->type() == TrackType::Audio) {
        const auto a = realAudioTracks(tl);
        for (size_t p = 0; p < a.size(); ++p)
            if (a[p] == ti) return static_cast<int>(p);              // A1 = front
        return 0;
    }
    const auto v = realVideoTracks(tl);
    for (size_t p = 0; p < v.size(); ++p)
        if (v[p] == ti) return static_cast<int>(v.size() - 1 - p);  // V1 = back
    return 0;
}

void EditOperations::copySelection(const Timeline& timeline,
                                    const SelectionSet& selection,
                                    ClipboardContents& clipboard)
{
    clipboard.clear();

    if (selection.empty()) return;

    // Find the earliest timelineIn among selected clips
    int64_t earliest = std::numeric_limits<int64_t>::max();
    for (const auto& ref : selection.clips())
    {
        if (ref.trackIndex >= timeline.trackCount()) continue;
        const Track* track = timeline.track(ref.trackIndex);
        size_t idx = track->findClipIndexById(ref.clipId);
        if (idx == track->clipCount()) continue;
        earliest = std::min(earliest, track->clip(idx)->timelineIn());
    }

    // Clone each selected clip
    std::unordered_set<uint64_t> selectedClipIds;
    std::unordered_set<size_t>   selectedTrackIndices;
    for (const auto& ref : selection.clips())
    {
        if (ref.trackIndex >= timeline.trackCount()) continue;
        const Track* track = timeline.track(ref.trackIndex);
        size_t idx = track->findClipIndexById(ref.clipId);
        if (idx == track->clipCount()) continue;

        const Clip* clip = track->clip(idx);
        ClipboardContents::Entry entry;
        entry.trackIndex     = ref.trackIndex;
        entry.relativeTime   = clip->timelineIn() - earliest;
        entry.originalClipId = clip->id();
        entry.trackOffset    = trackOffsetFromBase(timeline, ref.trackIndex);
        entry.clip           = clip->clone();
        clipboard.entries.push_back(std::move(entry));

        selectedClipIds.insert(clip->id());
        selectedTrackIndices.insert(ref.trackIndex);
    }

    // Capture transitions whose clips are entirely within the selection so a
    // copied fade-in/fade-out/cross-dissolve survives paste.  A side with
    // clipId 0 is "to/from nothing" and needs no source clip; any non-zero
    // side must be among the selected clips, otherwise the transition would
    // dangle (its partner clip isn't being pasted) and is skipped.
    for (size_t ti : selectedTrackIndices)
    {
        const Track* track = timeline.track(ti);
        if (!track) continue;
        for (size_t tr = 0; tr < track->transitionCount(); ++tr)
        {
            const Transition* trans = track->transition(tr);
            if (!trans) continue;
            const bool leftOk  = trans->leftClipId  == 0
                              || selectedClipIds.count(trans->leftClipId);
            const bool rightOk = trans->rightClipId == 0
                              || selectedClipIds.count(trans->rightClipId);
            const bool touchesSelection =
                (trans->leftClipId  != 0 && selectedClipIds.count(trans->leftClipId)) ||
                (trans->rightClipId != 0 && selectedClipIds.count(trans->rightClipId));
            if (!leftOk || !rightOk || !touchesSelection) continue;

            ClipboardContents::TransitionEntry te;
            te.transition   = *trans;                       // original clip ids preserved
            te.relEditPoint = trans->editPointTick - earliest;
            clipboard.transitions.push_back(te);
        }
    }
}

void EditOperations::copyTransition(const Timeline& timeline,
                                    size_t trackIndex, size_t transitionIndex,
                                    ClipboardContents& clipboard)
{
    clipboard.clear();
    if (trackIndex >= timeline.trackCount()) return;

    const Track* track = timeline.track(trackIndex);
    if (!track) return;
    const Transition* transition = track->transition(transitionIndex);
    if (!transition) return;

    clipboard.standaloneTransition = *transition;
}

std::unique_ptr<Command> EditOperations::cutSelection(
    Timeline& timeline, const SelectionSet& selection,
    ClipboardContents& clipboard)
{
    copySelection(timeline, selection, clipboard);
    return deleteSelection(timeline, selection);
}

// Fresh starting groupId for a paste/duplicate batch. Returns a value
// strictly greater than any existing groupId OR clip id on the
// timeline so the new id is guaranteed not to collide with anything
// applyShotSwitch might scan by.
static uint64_t freshGroupIdBase(const Timeline& timeline)
{
    uint64_t base = 1;
    for (size_t ti = 0; ti < timeline.trackCount(); ++ti) {
        const Track* trk = timeline.track(ti);
        if (!trk) continue;
        for (size_t ci = 0; ci < trk->clipCount(); ++ci) {
            const Clip* c = trk->clip(ci);
            if (!c) continue;
            if (c->groupId() >= base) base = c->groupId() + 1;
            if (c->id()      >= base) base = c->id() + 1;
        }
    }
    return base;
}

// Re-stamp groupId on a freshly cloned paste/duplicate clip so it
// doesn't share identity with the source. Pasted clips that shared a
// source groupId all map to the same new id (preserves multi-layer
// shot grouping within the paste batch); independent source groups
// each get their own fresh id. Without this, applyShotSwitch sweeps
// the original (still-at-source) clip into the new clip's shot swap,
// inflating groupStart/groupEnd across the gap and stretching the new
// shot's layers from the paste position all the way back to the
// original.
static void remapClonedGroupId(
    Clip& cloned,
    std::unordered_map<uint64_t, uint64_t>& gidRemap,
    uint64_t& nextGid)
{
    const uint64_t oldGid = cloned.groupId();
    if (oldGid == 0) return; // Ungrouped — nothing to remap
    auto it = gidRemap.find(oldGid);
    if (it == gidRemap.end()) {
        cloned.setGroupId(nextGid);
        gidRemap.emplace(oldGid, nextGid);
        ++nextGid;
    } else {
        cloned.setGroupId(it->second);
    }
}

// Re-stamp linkId on a freshly cloned paste/duplicate clip so a linked
// A/V pair stays linked WITHIN the paste batch without aliasing an
// unrelated clip in the destination sequence. linkId values live in the
// clip-id space (a clip's linkId equals its partner's id), so a clone
// that kept its source linkId would collide with whatever destination
// clip happens to own that id — selecting/dragging the pasted clip would
// then sweep that stranger into the move (setLinkPartnersSelected scans
// every track by linkId) and the group-floor clamp would pin the group
// in place, making the pasted clip appear "stuck". Map each distinct
// source linkId to the fresh id of the first clone that carried it, so
// partners share a brand-new, collision-free linkId. clone() must have
// already assigned the new m_id before this runs.
static void remapClonedLinkId(
    Clip& cloned,
    std::unordered_map<uint64_t, uint64_t>& linkRemap)
{
    const uint64_t oldLink = cloned.linkId();
    if (oldLink == 0) return; // Unlinked — nothing to do
    auto it = linkRemap.find(oldLink);
    if (it == linkRemap.end()) {
        // First clone of this link-group: adopt its own fresh id as the
        // shared linkId (guaranteed unique, never collides).
        cloned.setLinkId(cloned.id());
        linkRemap.emplace(oldLink, cloned.id());
    } else {
        cloned.setLinkId(it->second);
    }
}

// Index of the pinned caption/subtitle track, or SIZE_MAX if none.
static size_t captionTrackIndex(const Timeline& tl)
{
    for (size_t i = 0; i < tl.trackCount(); ++i) {
        const Track* t = tl.track(i);
        if (t && t->isCaptionTrack()) return i;
    }
    return SIZE_MAX;
}

// One compatibility-preserving destination resolver for clipboard edits.
// Placement remains V1/A1-relative exactly as before; callers choose whether
// Premiere-style track targeting is required. Validation is batch-atomic.
struct ClipboardDestinationPlan {
    std::vector<size_t> destinations;
    bool valid{true};
};

static ClipboardDestinationPlan planClipboardDestinations(
    const Timeline& timeline, const ClipboardContents& clipboard,
    bool requireTargeted)
{
    ClipboardDestinationPlan plan;
    const auto videos = realVideoTracks(timeline);
    const auto audios = realAudioTracks(timeline);
    const size_t captions = captionTrackIndex(timeline);
    plan.destinations.reserve(clipboard.entries.size());
    for (const auto& entry : clipboard.entries) {
        size_t destination = SIZE_MAX;
        if (!entry.clip) {
            plan.destinations.push_back(destination);
            continue;
        }
        if (entry.clip->isCaption()) {
            destination = captions;
        } else if (entry.clip->isAudio() && !audios.empty()) {
            const int offset = std::clamp(entry.trackOffset, 0,
                                          static_cast<int>(audios.size()) - 1);
            destination = audios[static_cast<size_t>(offset)];
        } else if (!entry.clip->isAudio() && !videos.empty()) {
            const int offset = std::clamp(entry.trackOffset, 0,
                                          static_cast<int>(videos.size()) - 1);
            destination = videos[videos.size() - 1 - static_cast<size_t>(offset)];
        }
        plan.destinations.push_back(destination);
        if (destination >= timeline.trackCount()) {
            plan.valid = false;
            continue;
        }
        const Track* track = timeline.track(destination);
        if (!track || track->isLocked() || track->isDivider()
            || (requireTargeted && !track->isTargeted()))
            plan.valid = false;
    }
    return plan;
}

std::unique_ptr<Command> EditOperations::paste(
    Timeline& timeline, const ClipboardContents& clipboard,
    int64_t playhead)
{
    if (!clipboard.hasClips()) return nullptr;

    auto compound = std::make_unique<CompoundCommand>("Paste");

    // ── Destination tracks: V1/A1-anchored, create overflow tracks ──────
    // Clips are placed relative to V1 (bottom video) / A1 (top audio) — the
    // tracks adjacent to the V/A divider — using each entry's captured
    // trackOffset, NOT its absolute source index. So a cross-sequence paste
    // lands on the same V1..Vn / A1..An the user copied from (filling up from
    // V1), and a within-sequence paste is an identity map. If the source used
    // more video/audio tracks than the destination has, new tracks are
    // created — video ABOVE the existing stack, audio BELOW it — so no clip
    // is silently dropped.
    {
        int maxVideoOff = -1, maxAudioOff = -1;
        for (const auto& e : clipboard.entries) {
            if (!e.clip) continue;
            if (e.clip->isAudio())
                maxAudioOff = std::max(maxAudioOff, e.trackOffset);
            else if (!e.clip->isCaption())
                maxVideoOff = std::max(maxVideoOff, e.trackOffset);
        }
        // Overflow VIDEO tracks at the TOP (existing V1..Vn keep their slots;
        // the new ones become V(n+1).. above them). Inserting repeatedly at
        // the same index stacks the new tracks on top.
        {
            const auto vids = realVideoTracks(timeline);
            int need = (maxVideoOff + 1) - static_cast<int>(vids.size());
            const bool capTop = !vids.empty() ? false
                : (timeline.trackCount() > 0 && timeline.track(0)
                   && timeline.track(0)->isCaptionTrack());
            size_t topInsert = vids.empty() ? (capTop ? 1u : 0u) : vids.front();
            for (int k = 0; k < need; ++k) {
                auto cmd = std::make_unique<AddTrackCommand>(
                    &timeline, std::make_unique<Track>(TrackType::Video, ""), topInsert);
                cmd->execute();
                compound->addExecuted(std::move(cmd));
            }
        }
        // Overflow AUDIO tracks at the BOTTOM (higher A-numbers go downward).
        {
            const auto auds = realAudioTracks(timeline);
            int need = (maxAudioOff + 1) - static_cast<int>(auds.size());
            for (int k = 0; k < need; ++k) {
                auto cmd = std::make_unique<AddTrackCommand>(
                    &timeline, std::make_unique<Track>(TrackType::Audio, ""),
                    timeline.trackCount());
                cmd->execute();
                compound->addExecuted(std::move(cmd));
            }
        }
    }
    const auto destinationPlan = planClipboardDestinations(timeline, clipboard, false);
    if (!destinationPlan.valid) {
        compound->undo();
        return nullptr;
    }

    uint64_t nextGid = freshGroupIdBase(timeline);
    std::unordered_map<uint64_t, uint64_t> gidRemap;
    std::unordered_map<uint64_t, uint64_t> linkRemap;

    // Track each pasted clip so we can resolve overlaps after all are placed.
    struct PasteTarget { size_t trackIdx; uint64_t clipId; };
    std::vector<PasteTarget> targets;

    // Map original source-clip ids → freshly pasted ids (and their dest track)
    // so copied transitions can be re-anchored to the pasted clips.
    std::unordered_map<uint64_t, uint64_t> clipIdRemap;
    std::unordered_map<uint64_t, size_t>   newClipTrack;

    for (size_t entryIndex = 0; entryIndex < clipboard.entries.size(); ++entryIndex)
    {
        const auto& entry = clipboard.entries[entryIndex];
        if (!entry.clip) continue;
        // Resolve the destination track from the captured V1/A1 offset.
        // Caption cues stay on the pinned caption track; video counts up from
        // V1 (back of the list), audio counts down from A1 (front). The
        // overflow tracks created above guarantee the offset is in range, but
        // clamp defensively so a stray offset never indexes out of bounds.
        const size_t destIdx = destinationPlan.destinations[entryIndex];

        auto cloned = entry.clip->clone();
        cloned->setTimelineIn(playhead + entry.relativeTime);
        remapClonedGroupId(*cloned, gidRemap, nextGid);
        remapClonedLinkId(*cloned, linkRemap);
        const uint64_t newClipId = cloned->id();

        Track* track = timeline.track(destIdx);
        if (!track || track->isLocked()) continue;

        // Execute the add immediately so the clip is resident on the
        // track when we call resolveOverlaps below.  Use addExecuted so
        // the compound re-executes everything in order on redo.
        auto addCmd = std::make_unique<AddClipCommand>(track, std::move(cloned));
        addCmd->execute();
        compound->addExecuted(std::move(addCmd));

        if (entry.originalClipId != 0) {
            clipIdRemap[entry.originalClipId] = newClipId;
            newClipTrack[newClipId] = destIdx;
        }
        targets.push_back({destIdx, newClipId});
    }

    // Resolve overlaps for each pasted clip — the paste operation
    // overwrites any clip that overlaps the pasted range (Premiere-style
    // overwrite paste).
    for (const auto& tgt : targets) {
        Track* track = timeline.track(tgt.trackIdx);
        if (!track) continue;

        // Use the clone's known ID. Looking it up by timeline position can
        // select the pre-existing clip when both begin at the paste point,
        // causing the new paste to be deleted instead of overwriting it.
        const uint64_t pastedId = tgt.clipId;
        if (track->findClipIndexById(pastedId) == track->clipCount()) continue;

        auto overlapCmd = resolveOverlaps(timeline, tgt.trackIdx, pastedId);
        if (overlapCmd) {
            overlapCmd->execute();
            compound->addExecuted(std::move(overlapCmd));
        }
    }

    // Recreate copied transitions on the pasted clips: remap clip ids and the
    // edit point to the paste location.  A transition is skipped if any of its
    // (non-zero) source clips wasn't pasted.
    for (const auto& te : clipboard.transitions) {
        uint64_t newLeft = 0, newRight = 0;
        if (te.transition.leftClipId != 0) {
            auto it = clipIdRemap.find(te.transition.leftClipId);
            if (it == clipIdRemap.end()) continue;
            newLeft = it->second;
        }
        if (te.transition.rightClipId != 0) {
            auto it = clipIdRemap.find(te.transition.rightClipId);
            if (it == clipIdRemap.end()) continue;
            newRight = it->second;
        }
        const uint64_t anchorId = newLeft ? newLeft : newRight;
        if (anchorId == 0) continue;
        auto trkIt = newClipTrack.find(anchorId);
        if (trkIt == newClipTrack.end()) continue;
        Track* track = timeline.track(trkIt->second);
        if (!track) continue;

        Transition t   = te.transition;
        t.leftClipId   = newLeft;
        t.rightClipId  = newRight;
        t.editPointTick = playhead + te.relEditPoint;

        auto addTrans = std::make_unique<AddTransitionCommand>(track, 0, 0, t);
        addTrans->execute();
        compound->addExecuted(std::move(addTrans));
    }

    // Leave the playhead where it was — at the paste point — rather than
    // jumping it to the end of the pasted content. The paste commands above
    // don't touch the playhead, so the model already holds the paste-point
    // tick; the UI callers sync from it after executing. (Deliberate: do NOT
    // re-add a "move playhead to end" LambdaCommand here.)

    return compound->size() > 0 ? std::move(compound) : nullptr;
}

bool EditOperations::fitTransitionToAvailableDuration(
    const Track& track, Transition& transition)
{
    if (transition.duration <= 0) return false;

    const bool centered = transition.leftClipId != 0 &&
                          transition.rightClipId != 0;
    int64_t maxDuration = transition.duration;

    auto clipById = [&](uint64_t id) -> const Clip* {
        if (id == 0) return nullptr;
        const size_t index = track.findClipIndexById(id);
        return index < track.clipCount() ? track.clip(index) : nullptr;
    };

    // First fit the transition inside the clip content available around its
    // edit point. A two-sided transition consumes half its duration from each
    // clip; a one-sided fade consumes its full duration from its one clip.
    if (transition.leftClipId == 0) {
        const Clip* right = clipById(transition.rightClipId);
        if (!right) return false;
        maxDuration = std::min(
            maxDuration, right->timelineOut() - transition.editPointTick);
    } else if (transition.rightClipId == 0) {
        const Clip* left = clipById(transition.leftClipId);
        if (!left) return false;
        maxDuration = std::min(
            maxDuration, transition.editPointTick - left->timelineIn());
    } else {
        const Clip* left = clipById(transition.leftClipId);
        const Clip* right = clipById(transition.rightClipId);
        if (!left || !right) return false;
        const int64_t halfRoom = std::min(
            transition.editPointTick - left->timelineIn(),
            right->timelineOut() - transition.editPointTick);
        if (halfRoom <= 0) return false;
        const int64_t doubledRoom = halfRoom > INT64_MAX / 2
            ? INT64_MAX : halfRoom * 2;
        maxDuration = std::min(maxDuration, doubledRoom);
    }

    const int64_t minimumDuration = centered ? 2 : 1;
    if (maxDuration < minimumDuration) return false;

    // Range expansion is monotonic with duration, so find the longest value
    // at or below the request that does not collide with another edit point.
    // A transition at the same endpoint pair is ignored because adding it
    // replaces that transition rather than stacking a duplicate.
    auto fitsWithoutOverlap = [&](int64_t duration) {
        Transition candidate = transition;
        candidate.duration = duration;
        int64_t candidateStart = 0;
        int64_t candidateEnd = 0;
        candidate.getRange(candidateStart, candidateEnd);
        if (candidateEnd <= candidateStart) return false;

        for (size_t i = 0; i < track.transitionCount(); ++i) {
            const Transition* existing = track.transition(i);
            if (!existing) continue;
            if (existing->leftClipId == candidate.leftClipId &&
                existing->rightClipId == candidate.rightClipId)
                continue;

            int64_t existingStart = 0;
            int64_t existingEnd = 0;
            existing->getRange(existingStart, existingEnd);
            if (candidateStart < existingEnd && existingStart < candidateEnd)
                return false;
        }
        return true;
    };

    int64_t low = minimumDuration;
    int64_t high = maxDuration;
    int64_t best = 0;
    while (low <= high) {
        const int64_t mid = low + (high - low) / 2;
        if (fitsWithoutOverlap(mid)) {
            best = mid;
            low = mid + 1;
        } else {
            high = mid - 1;
        }
    }

    // Centered ranges use duration/2 on each side; keep the stored duration
    // even so it exactly matches the visible range instead of losing one tick.
    if (centered && (best & 1)) --best;
    if (best < minimumDuration) return false;

    transition.duration = best;
    return true;
}

std::unique_ptr<Command> EditOperations::pasteTransitionAtEdge(
    Timeline& timeline, const ClipboardContents& clipboard,
    size_t trackIndex, uint64_t clipId, ClipEdge edge)
{
    if (!clipboard.standaloneTransition || trackIndex >= timeline.trackCount())
        return nullptr;

    Track* track = timeline.track(trackIndex);
    if (!track || track->isLocked()) return nullptr;

    const size_t clipIndex = track->findClipIndexById(clipId);
    if (clipIndex >= track->clipCount()) return nullptr;
    const Clip* targetClip = track->clip(clipIndex);
    if (!targetClip) return nullptr;

    Transition pasted = *clipboard.standaloneTransition;
    pasted.leftClipId = 0;
    pasted.rightClipId = 0;

    size_t leftIndex = clipIndex;
    size_t rightIndex = clipIndex;
    if (edge == ClipEdge::Head) {
        pasted.rightClipId = targetClip->id();
        pasted.editPointTick = targetClip->timelineIn();
        for (size_t i = 0; i < track->clipCount(); ++i) {
            const Clip* candidate = track->clip(i);
            if (candidate && candidate->id() != targetClip->id()
                && candidate->timelineOut() == pasted.editPointTick) {
                pasted.leftClipId = candidate->id();
                leftIndex = i;
                break;
            }
        }
    } else {
        pasted.leftClipId = targetClip->id();
        pasted.editPointTick = targetClip->timelineOut();
        for (size_t i = 0; i < track->clipCount(); ++i) {
            const Clip* candidate = track->clip(i);
            if (candidate && candidate->id() != targetClip->id()
                && candidate->timelineIn() == pasted.editPointTick) {
                pasted.rightClipId = candidate->id();
                rightIndex = i;
                break;
            }
        }
    }

    if (!fitTransitionToAvailableDuration(*track, pasted)) return nullptr;

    return std::make_unique<AddTransitionCommand>(
        track, leftIndex, rightIndex, pasted);
}

std::unique_ptr<Command> EditOperations::pasteInsert(
    Timeline& timeline, const ClipboardContents& clipboard,
    int64_t playhead)
{
    if (!clipboard.hasClips()) return nullptr;

    auto compound = std::make_unique<CompoundCommand>("Insert paste");

    // Premiere-style Insert opens a SINGLE uniform gap across the whole
    // stack so all content stays in sync — not just the tracks that
    // receive pasted clips. The gap width is the full span of the pasted
    // content (earliest clip start = relativeTime 0 … latest clip edge).
    int64_t insertSpan = 0;
    for (const auto& entry : clipboard.entries) {
        if (!entry.clip) continue;
        insertSpan = std::max(insertSpan,
                              entry.relativeTime + entry.clip->duration());
    }
    if (insertSpan <= 0) return nullptr;

    // ── Destination tracks: V1/A1-anchored, create overflow tracks ──────
    // Same mapping as paste(): clips land relative to V1 (bottom video) /
    // A1 (top audio) via each entry's captured trackOffset, NOT its
    // absolute source index — so a cross-sequence insert-paste fills from
    // V1 instead of landing on (or dropping off) mismatched indices.
    // Overflow tracks are created up-front and EXECUTED immediately;
    // CompoundCommand requires all addExecuted entries to precede the
    // deferred addCommand ones, which holds because nothing below has
    // been added yet.
    {
        int maxVideoOff = -1, maxAudioOff = -1;
        for (const auto& e : clipboard.entries) {
            if (!e.clip) continue;
            if (e.clip->isAudio())
                maxAudioOff = std::max(maxAudioOff, e.trackOffset);
            else if (!e.clip->isCaption())
                maxVideoOff = std::max(maxVideoOff, e.trackOffset);
        }
        {
            const auto vids = realVideoTracks(timeline);
            int need = (maxVideoOff + 1) - static_cast<int>(vids.size());
            const bool capTop = !vids.empty() ? false
                : (timeline.trackCount() > 0 && timeline.track(0)
                   && timeline.track(0)->isCaptionTrack());
            size_t topInsert = vids.empty() ? (capTop ? 1u : 0u) : vids.front();
            for (int k = 0; k < need; ++k) {
                auto cmd = std::make_unique<AddTrackCommand>(
                    &timeline, std::make_unique<Track>(TrackType::Video, ""), topInsert);
                cmd->execute();
                compound->addExecuted(std::move(cmd));
            }
        }
        {
            const auto auds = realAudioTracks(timeline);
            int need = (maxAudioOff + 1) - static_cast<int>(auds.size());
            for (int k = 0; k < need; ++k) {
                auto cmd = std::make_unique<AddTrackCommand>(
                    &timeline, std::make_unique<Track>(TrackType::Audio, ""),
                    timeline.trackCount());
                cmd->execute();
                compound->addExecuted(std::move(cmd));
            }
        }
    }
    const auto destinationPlan = planClipboardDestinations(timeline, clipboard, true);
    if (!destinationPlan.valid) {
        compound->undo();
        return nullptr;
    }
    const auto& destFor = destinationPlan.destinations;

    // Tracks that will actually receive a pasted clip (valid destination
    // AND targeted — same gate as the add loop below). They must ripple to
    // make room regardless of their sync-lock state.
    std::unordered_set<size_t> contentTracks;
    for (size_t i = 0; i < clipboard.entries.size(); ++i) {
        const size_t destIdx = destFor[i];
        if (destIdx >= timeline.trackCount()) continue;
        const Track* t = timeline.track(destIdx);
        if (t && !t->isLocked() && t->isTargeted())
            contentTracks.insert(destIdx);
    }

    // First: shift existing clips at/after the playhead to the right.
    // Every track ripples as long as it is sync-locked (or is receiving
    // content); locked tracks never move. Mirrors openGap()/closeGap().
    for (size_t ti = 0; ti < timeline.trackCount(); ++ti)
    {
        Track* track = timeline.track(ti);
        if (!track || track->isLocked()) continue;
        const bool receivesContent = contentTracks.count(ti) != 0;
        if (!receivesContent && !track->isSyncLocked()) continue;

        // Collect clips to shift (iterate in reverse timeline order to be safe)
        struct ShiftInfo { uint64_t id; int64_t oldIn; };
        std::vector<ShiftInfo> toShift;
        for (size_t ci = 0; ci < track->clipCount(); ++ci) {
            const Clip* c = track->clip(ci);
            if (c->timelineIn() >= playhead) {
                toShift.push_back({c->id(), c->timelineIn()});
            }
        }
        // Sort by position descending so moves don't collide
        std::sort(toShift.begin(), toShift.end(),
                  [](const ShiftInfo& a, const ShiftInfo& b) {
                      return a.oldIn > b.oldIn;
                  });
        for (const auto& si : toShift) {
            compound->addCommand(std::make_unique<MoveClipCommand>(
                track, si.id, si.oldIn + insertSpan));
        }
    }

    // Second: add the pasted clips at playhead on targeted tracks
    uint64_t nextGid = freshGroupIdBase(timeline);
    std::unordered_map<uint64_t, uint64_t> gidRemap;
    std::unordered_map<uint64_t, uint64_t> linkRemap;
    // Map original source-clip ids → freshly cloned ids (and dest track) so
    // copied transitions can be re-anchored to the inserted clips.  Clone
    // ids are assigned at clone() time, so they're valid before execute().
    std::unordered_map<uint64_t, uint64_t> clipIdRemap;
    std::unordered_map<uint64_t, size_t>   newClipTrack;
    for (size_t i = 0; i < clipboard.entries.size(); ++i)
    {
        const auto& entry = clipboard.entries[i];
        const size_t destIdx = destFor[i];
        if (destIdx >= timeline.trackCount()) continue;
        Track* track = timeline.track(destIdx);
        if (!track || track->isLocked() || !track->isTargeted()) continue;

        auto cloned = entry.clip->clone();
        cloned->setTimelineIn(playhead + entry.relativeTime);
        remapClonedGroupId(*cloned, gidRemap, nextGid);
        remapClonedLinkId(*cloned, linkRemap);
        const uint64_t newClipId = cloned->id();

        compound->addCommand(std::make_unique<AddClipCommand>(
            track, std::move(cloned)));

        if (entry.originalClipId != 0) {
            clipIdRemap[entry.originalClipId] = newClipId;
            newClipTrack[newClipId] = destIdx;
        }
    }

    // Recreate copied transitions on the inserted clips (parity with
    // paste()): remap clip ids and the edit point to the paste location.
    // Skipped if any (non-zero) source clip wasn't inserted.  Deferred via
    // addCommand so they execute after the clip adds above.
    for (const auto& te : clipboard.transitions) {
        uint64_t newLeft = 0, newRight = 0;
        if (te.transition.leftClipId != 0) {
            auto it = clipIdRemap.find(te.transition.leftClipId);
            if (it == clipIdRemap.end()) continue;
            newLeft = it->second;
        }
        if (te.transition.rightClipId != 0) {
            auto it = clipIdRemap.find(te.transition.rightClipId);
            if (it == clipIdRemap.end()) continue;
            newRight = it->second;
        }
        const uint64_t anchorId = newLeft ? newLeft : newRight;
        if (anchorId == 0) continue;
        auto trkIt = newClipTrack.find(anchorId);
        if (trkIt == newClipTrack.end()) continue;
        Track* track = timeline.track(trkIt->second);
        if (!track) continue;

        Transition t    = te.transition;
        t.leftClipId    = newLeft;
        t.rightClipId   = newRight;
        t.editPointTick = playhead + te.relEditPoint;

        compound->addCommand(
            std::make_unique<AddTransitionCommand>(track, 0, 0, t));
    }

    // Leave the playhead at the paste point rather than jumping it to the end
    // of the inserted content (matches paste() above). The insert commands
    // don't move the playhead, so the model already holds the paste-point tick
    // and the UI callers sync from it. (Deliberate: do NOT re-add a "move
    // playhead to end" LambdaCommand here.)

    return compound->size() > 0 ? std::move(compound) : nullptr;
}

std::unique_ptr<Command> EditOperations::duplicateSelection(
    Timeline& timeline, const SelectionSet& selection)
{
    if (selection.empty()) return nullptr;

    auto compound = std::make_unique<CompoundCommand>("Duplicate");

    uint64_t nextGid = freshGroupIdBase(timeline);
    std::unordered_map<uint64_t, uint64_t> gidRemap;
    std::unordered_map<uint64_t, uint64_t> linkRemap;

    // Map original source-clip ids → duplicated ids (and dest track) so
    // attached transitions can be re-anchored to the duplicates.
    std::unordered_map<uint64_t, uint64_t> clipIdRemap;
    std::unordered_map<uint64_t, size_t>   newClipTrack;
    std::unordered_set<uint64_t>           selectedClipIds;
    std::unordered_set<size_t>             selectedTrackIndices;

    for (const auto& ref : selection.clips())
    {
        if (ref.trackIndex >= timeline.trackCount()) continue;
        Track* track = timeline.track(ref.trackIndex);
        if (!track || track->isLocked()) continue;
        size_t idx = track->findClipIndexById(ref.clipId);
        if (idx == track->clipCount()) continue;

        const Clip* clip = track->clip(idx);
        const uint64_t origId = clip->id();
        auto cloned = clip->clone();
        cloned->setTimelineIn(clip->timelineIn() + kDuplicateOffset);
        remapClonedGroupId(*cloned, gidRemap, nextGid);
        remapClonedLinkId(*cloned, linkRemap);
        const uint64_t newId = cloned->id();

        compound->addCommand(std::make_unique<AddClipCommand>(
            track, std::move(cloned)));

        clipIdRemap[origId] = newId;
        newClipTrack[newId] = ref.trackIndex;
        selectedClipIds.insert(origId);
        selectedTrackIndices.insert(ref.trackIndex);
    }

    // Duplicate transitions whose clips are entirely within the selection,
    // re-anchored to the duplicated clips (same logic as copy/paste).
    for (size_t ti : selectedTrackIndices)
    {
        Track* track = timeline.track(ti);
        if (!track) continue;
        for (size_t tr = 0; tr < track->transitionCount(); ++tr)
        {
            const Transition* trans = track->transition(tr);
            if (!trans) continue;
            const bool leftOk  = trans->leftClipId  == 0
                              || selectedClipIds.count(trans->leftClipId);
            const bool rightOk = trans->rightClipId == 0
                              || selectedClipIds.count(trans->rightClipId);
            const bool touchesSelection =
                (trans->leftClipId  != 0 && selectedClipIds.count(trans->leftClipId)) ||
                (trans->rightClipId != 0 && selectedClipIds.count(trans->rightClipId));
            if (!leftOk || !rightOk || !touchesSelection) continue;

            uint64_t newLeft  = trans->leftClipId  ? clipIdRemap[trans->leftClipId]  : 0;
            uint64_t newRight = trans->rightClipId ? clipIdRemap[trans->rightClipId] : 0;
            const uint64_t anchorId = newLeft ? newLeft : newRight;
            if (anchorId == 0) continue;
            Track* destTrack = timeline.track(newClipTrack[anchorId]);
            if (!destTrack) continue;

            Transition t    = *trans;
            t.leftClipId    = newLeft;
            t.rightClipId   = newRight;
            t.editPointTick = trans->editPointTick + kDuplicateOffset;

            compound->addCommand(std::make_unique<AddTransitionCommand>(
                destTrack, 0, 0, t));
        }
    }

    // Compute the end tick of the duplicated content for playhead movement.
    int64_t maxEnd = timeline.playheadPosition();
    for (const auto& ref : selection.clips()) {
        if (ref.trackIndex >= timeline.trackCount()) continue;
        const Track* track = timeline.track(ref.trackIndex);
        size_t idx = track->findClipIndexById(ref.clipId);
        if (idx >= track->clipCount()) continue;
        const Clip* clip = track->clip(idx);
        int64_t end = clip->timelineIn() + kDuplicateOffset + clip->duration();
        if (end > maxEnd) maxEnd = end;
    }
    const int64_t playheadBefore = timeline.playheadPosition();

    // Move playhead to end of duplicated content.  Wrap in a LambdaCommand
    // so Ctrl+Z also restores the playhead to where it was before duplicate.
    compound->addCommand(std::make_unique<LambdaCommand>(
        "Playhead",
        [&timeline, maxEnd]() { timeline.setPlayheadPosition(maxEnd); },
        [&timeline, playheadBefore]() { timeline.setPlayheadPosition(playheadBefore); }
    ));

    return compound->size() > 0 ? std::move(compound) : nullptr;
}

// ─── In/Out Points ───────────────────────────────────────────────────────────

void EditOperations::setInPoint(Timeline& timeline, int64_t playhead)
{
    timeline.setInPoint(playhead);
    // If out-point is before in-point, clear it
    if (timeline.outPoint() >= 0 && timeline.outPoint() <= playhead)
        timeline.setOutPoint(-1);
}

void EditOperations::setOutPoint(Timeline& timeline, int64_t playhead)
{
    timeline.setOutPoint(playhead);
    // If in-point is after out-point, clear it
    if (timeline.inPoint() >= 0 && timeline.inPoint() >= playhead)
        timeline.setInPoint(-1);
}

void EditOperations::clearInOutPoints(Timeline& timeline)
{
    timeline.clearInOutPoints();
}

// ─── Navigation ──────────────────────────────────────────────────────────────

int64_t EditOperations::nextEditPoint(const Timeline& timeline, int64_t fromTime)
{
    int64_t nearest = std::numeric_limits<int64_t>::max();

    for (size_t ti = 0; ti < timeline.trackCount(); ++ti)
    {
        const Track* track = timeline.track(ti);
        for (size_t ci = 0; ci < track->clipCount(); ++ci)
        {
            const Clip* clip = track->clip(ci);
            // Check in-point
            if (clip->timelineIn() > fromTime)
                nearest = std::min(nearest, clip->timelineIn());
            // Check out-point
            if (clip->timelineOut() > fromTime)
                nearest = std::min(nearest, clip->timelineOut());
        }
    }

    return nearest == std::numeric_limits<int64_t>::max() ? fromTime : nearest;
}

int64_t EditOperations::prevEditPoint(const Timeline& timeline, int64_t fromTime)
{
    int64_t nearest = -1;

    for (size_t ti = 0; ti < timeline.trackCount(); ++ti)
    {
        const Track* track = timeline.track(ti);
        for (size_t ci = 0; ci < track->clipCount(); ++ci)
        {
            const Clip* clip = track->clip(ci);
            // Check in-point
            if (clip->timelineIn() < fromTime)
                nearest = std::max(nearest, clip->timelineIn());
            // Check out-point
            if (clip->timelineOut() < fromTime)
                nearest = std::max(nearest, clip->timelineOut());
        }
    }

    return nearest >= 0 ? nearest : fromTime;
}

// ─── Helpers ─────────────────────────────────────────────────────────────────

const Clip* EditOperations::clipAtTime(const Track& track, int64_t tick)
{
    for (size_t ci = 0; ci < track.clipCount(); ++ci)
    {
        const Clip* clip = track.clip(ci);
        if (tick >= clip->timelineIn() && tick < clip->timelineOut())
            return clip;
    }
    return nullptr;
}

EditOperations::EditPoint EditOperations::findEditPoint(const Track& track, int64_t nearTime)
{
    EditPoint result;
    int64_t bestDist = std::numeric_limits<int64_t>::max();

    for (size_t ci = 0; ci + 1 < track.clipCount(); ++ci)
    {
        const Clip* left  = track.clip(ci);
        const Clip* right = track.clip(ci + 1);

        int64_t editTime = left->timelineOut();
        int64_t dist = std::abs(editTime - nearTime);

        if (dist < bestDist)
        {
            bestDist = dist;
            result.leftClip  = left;
            result.rightClip = right;
            result.editTime  = editTime;
        }
    }

    return result;
}

EditOperations::MatchFrameResult EditOperations::matchFrame(
    const Timeline& timeline, int64_t playhead)
{
    MatchFrameResult result;

    // Search topmost video track first, then audio
    for (size_t ti = 0; ti < timeline.trackCount(); ++ti)
    {
        const Track* track = timeline.track(ti);
        if (track->type() != TrackType::Video) continue;
        if (track->isMuted()) continue;

        for (size_t ci = 0; ci < track->clipCount(); ++ci)
        {
            const Clip* clip = track->clip(ci);
            if (playhead >= clip->timelineIn() && playhead < clip->timelineOut())
            {
                int64_t offset = playhead - clip->timelineIn();
                result.trackIndex = ti;
                result.clipId = clip->id();
                result.sourceTime = clip->sourceIn() + static_cast<int64_t>(offset * clip->effectiveSpeed(offset));
                result.valid = true;
                return result;
            }
        }
    }

    // Fallback: check audio tracks
    for (size_t ti = 0; ti < timeline.trackCount(); ++ti)
    {
        const Track* track = timeline.track(ti);
        if (track->type() != TrackType::Audio) continue;
        if (track->isMuted()) continue;

        for (size_t ci = 0; ci < track->clipCount(); ++ci)
        {
            const Clip* clip = track->clip(ci);
            if (playhead >= clip->timelineIn() && playhead < clip->timelineOut())
            {
                int64_t offset = playhead - clip->timelineIn();
                result.trackIndex = ti;
                result.clipId = clip->id();
                result.sourceTime = clip->sourceIn() + static_cast<int64_t>(offset * clip->effectiveSpeed(offset));
                result.valid = true;
                return result;
            }
        }
    }

    return result;
}

// ─── Match Frame (extended — includes media path) ────────────────────────────

EditOperations::MatchFrameResultEx EditOperations::matchFrameEx(
    const Timeline& timeline, int64_t playhead)
{
    MatchFrameResultEx result;

    // Search topmost video track first, then audio
    for (int pass = 0; pass < 2; ++pass)
    {
        TrackType targetType = (pass == 0) ? TrackType::Video : TrackType::Audio;
        for (size_t ti = 0; ti < timeline.trackCount(); ++ti)
        {
            const Track* track = timeline.track(ti);
            if (track->type() != targetType) continue;
            if (track->isMuted()) continue;

            for (size_t ci = 0; ci < track->clipCount(); ++ci)
            {
                const Clip* clip = track->clip(ci);
                if (playhead >= clip->timelineIn() && playhead < clip->timelineOut())
                {
                    int64_t offset = playhead - clip->timelineIn();
                    result.trackIndex = ti;
                    result.clipId = clip->id();
                    result.sourceTime = clip->sourceIn() + static_cast<int64_t>(offset * clip->effectiveSpeed(offset));
                    result.valid = true;

                    // Extract media path from derived clip type
                    if (auto* vc = dynamic_cast<const VideoClip*>(clip))
                        result.mediaPath = vc->mediaPath();
                    else if (auto* ac = dynamic_cast<const AudioClip*>(clip))
                        result.mediaPath = ac->mediaPath();

                    return result;
                }
            }
        }
    }

    return result;
}

// ─── Lift / Extract (I/O range) ──────────────────────────────────────────────

std::unique_ptr<Command> EditOperations::liftInOut(
    Timeline& timeline, int64_t inPoint, int64_t outPoint)
{
    if (inPoint < 0 || outPoint < 0 || inPoint >= outPoint)
        return nullptr;

    auto compound = std::make_unique<CompoundCommand>("Lift (I/O)");

    for (size_t ti = 0; ti < timeline.trackCount(); ++ti)
    {
        Track* track = timeline.track(ti);
        if (track->isLocked()) continue;

        // Collect clips that overlap the I/O range
        std::vector<const Clip*> overlapping;
        for (size_t ci = 0; ci < track->clipCount(); ++ci)
        {
            const Clip* clip = track->clip(ci);
            if (clip->timelineOut() <= inPoint || clip->timelineIn() >= outPoint) continue;
            overlapping.push_back(clip);
        }

        for (const Clip* clip : overlapping)
        {
            if (clip->timelineIn() >= inPoint && clip->timelineOut() <= outPoint)
            {
                // Fully inside — remove entirely
                compound->addCommand(std::make_unique<RemoveClipCommand>(track, clip->id()));
            }
            else if (clip->timelineIn() < inPoint && clip->timelineOut() > outPoint)
            {
                // Clip spans the entire I/O range — split at both boundaries.
                // Trim original to end at inPoint, create right portion from outPoint.
                int64_t origIn = clip->timelineIn();
                int64_t origSourceIn = clip->sourceIn();
                int64_t origDuration = clip->duration();

                // Trim the original clip's tail to the in-point
                int64_t leftDur = inPoint - origIn;
                compound->addCommand(std::make_unique<TrimClipCommand>(
                    track, clip->id(), origIn, leftDur, origSourceIn));

                // Create right-half clone starting at outPoint
                int64_t rightOffset = outPoint - origIn;
                int64_t rightDuration = origDuration - rightOffset;
                double clipSpd = std::max(clip->speed(), 0.01);
                auto rightClip = clip->clone();
                rightClip->setTimelineIn(outPoint);
                rightClip->setDuration(rightDuration);
                rightClip->setSourceIn(origSourceIn + static_cast<int64_t>(std::llround(rightOffset * clipSpd)));
                compound->addCommand(std::make_unique<AddClipCommand>(track, std::move(rightClip)));
            }
            else if (clip->timelineIn() < inPoint)
            {
                // Clip starts before and extends into I/O range — trim tail
                int64_t newDuration = inPoint - clip->timelineIn();
                compound->addCommand(std::make_unique<TrimClipCommand>(
                    track, clip->id(), clip->timelineIn(), newDuration, clip->sourceIn()));
            }
            else
            {
                // Clip starts inside I/O range and extends beyond — trim head
                int64_t trimAmount = outPoint - clip->timelineIn();
                int64_t newIn = outPoint;
                int64_t newDuration = clip->duration() - trimAmount;
                int64_t newSourceIn = clip->sourceIn() + trimAmount;
                compound->addCommand(std::make_unique<TrimClipCommand>(
                    track, clip->id(), newIn, newDuration, newSourceIn));
            }
        }
    }

    return compound->size() > 0 ? std::move(compound) : nullptr;
}

std::unique_ptr<Command> EditOperations::extractInOut(
    Timeline& timeline, int64_t inPoint, int64_t outPoint)
{
    if (inPoint < 0 || outPoint < 0 || inPoint >= outPoint)
        return nullptr;

    auto compound = std::make_unique<CompoundCommand>("Extract (I/O)");

    int64_t gapDuration = outPoint - inPoint;

    for (size_t ti = 0; ti < timeline.trackCount(); ++ti)
    {
        Track* track = timeline.track(ti);
        if (track->isLocked()) continue;

        // Collect clips that overlap the I/O range
        std::vector<const Clip*> overlapping;
        for (size_t ci = 0; ci < track->clipCount(); ++ci)
        {
            const Clip* clip = track->clip(ci);
            if (clip->timelineOut() <= inPoint || clip->timelineIn() >= outPoint) continue;
            overlapping.push_back(clip);
        }

        for (const Clip* clip : overlapping)
        {
            if (clip->timelineIn() >= inPoint && clip->timelineOut() <= outPoint)
            {
                // Fully inside — remove entirely
                compound->addCommand(std::make_unique<RemoveClipCommand>(track, clip->id()));
            }
            else if (clip->timelineIn() < inPoint && clip->timelineOut() > outPoint)
            {
                // Clip spans entire range — trim original to inPoint, create right part
                int64_t origIn = clip->timelineIn();
                int64_t origSourceIn = clip->sourceIn();
                int64_t origDuration = clip->duration();

                int64_t leftDur = inPoint - origIn;
                compound->addCommand(std::make_unique<TrimClipCommand>(
                    track, clip->id(), origIn, leftDur, origSourceIn));

                // Right portion starts at inPoint (gap is closed) instead of outPoint
                int64_t rightOffset = outPoint - origIn;
                int64_t rightDuration = origDuration - rightOffset;
                double clipSpd = std::max(clip->speed(), 0.01);
                auto rightClip = clip->clone();
                rightClip->setTimelineIn(inPoint); // Rippled: placed right after left
                rightClip->setDuration(rightDuration);
                rightClip->setSourceIn(origSourceIn + static_cast<int64_t>(std::llround(rightOffset * clipSpd)));
                compound->addCommand(std::make_unique<AddClipCommand>(track, std::move(rightClip)));
            }
            else if (clip->timelineIn() < inPoint)
            {
                // Clip starts before I/O range — trim tail
                int64_t newDuration = inPoint - clip->timelineIn();
                compound->addCommand(std::make_unique<TrimClipCommand>(
                    track, clip->id(), clip->timelineIn(), newDuration, clip->sourceIn()));
            }
            else
            {
                // Clip starts inside I/O range and extends beyond — trim head and shift left
                int64_t trimAmount = outPoint - clip->timelineIn();
                // Keep the head trim separate from the ripple move: the trim
                // preserves surviving keyframes, then MoveClipCommand moves
                // the shortened clip and its animation left with the extract.
                int64_t newDuration = clip->duration() - trimAmount;
                int64_t newSourceIn = clip->sourceIn() + trimAmount;
                compound->addCommand(std::make_unique<TrimClipCommand>(
                    track, clip->id(), outPoint, newDuration, newSourceIn));
                compound->addCommand(std::make_unique<MoveClipCommand>(
                    track, clip->id(), inPoint));
            }
        }

        // Shift all clips after the outPoint to the left to close the gap
        for (size_t ci = 0; ci < track->clipCount(); ++ci)
        {
            const Clip* clip = track->clip(ci);
            if (clip->timelineIn() >= outPoint)
            {
                // Check this clip isn't one we already handled above
                bool alreadyHandled = false;
                for (const Clip* ov : overlapping) {
                    if (ov->id() == clip->id()) { alreadyHandled = true; break; }
                }
                if (!alreadyHandled) {
                    compound->addCommand(std::make_unique<MoveClipCommand>(
                        track, clip->id(), clip->timelineIn() - gapDuration));
                }
            }
        }
    }

    return compound->size() > 0 ? std::move(compound) : nullptr;
}

// ─── Auto Gap Close ──────────────────────────────────────────────────────────

std::unique_ptr<Command> EditOperations::closeAllGaps(Timeline& timeline)
{
    auto compound = std::make_unique<CompoundCommand>("Close all gaps");
    bool anyMoved = false;

    for (size_t ti = 0; ti < timeline.trackCount(); ++ti)
    {
        Track* track = timeline.track(ti);
        if (track->isLocked()) continue;
        if (track->clipCount() == 0) continue;

        // Collect and sort clips by timeline position
        struct ClipInfo { uint64_t id; int64_t timelineIn; int64_t timelineOut; };
        std::vector<ClipInfo> clips;
        clips.reserve(track->clipCount());
        for (size_t ci = 0; ci < track->clipCount(); ++ci)
        {
            const Clip* c = track->clip(ci);
            clips.push_back({c->id(), c->timelineIn(), c->timelineOut()});
        }
        std::sort(clips.begin(), clips.end(),
                  [](const ClipInfo& a, const ClipInfo& b) { return a.timelineIn < b.timelineIn; });

        // First clip slides to time 0
        int64_t nextFree = 0;
        for (const auto& ci : clips)
        {
            if (ci.timelineIn > nextFree)
            {
                compound->addCommand(std::make_unique<MoveClipCommand>(
                    track, ci.id, nextFree));
                anyMoved = true;
                nextFree += (ci.timelineOut - ci.timelineIn); // duration
            }
            else
            {
                nextFree = ci.timelineOut;
            }
        }
    }

    return anyMoved ? std::move(compound) : nullptr;
}


} // namespace rt
