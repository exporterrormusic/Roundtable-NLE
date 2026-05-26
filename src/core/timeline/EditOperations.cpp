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
#include "timeline/Marker.h"
#include "command/Command.h"
#include "command/CompoundCommand.h"
#include "command/commands/ClipCommands.h"
#include "command/commands/TransitionCmds.h"

#include <spdlog/spdlog.h>
#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_map>
#include <unordered_set>

namespace rt {

// ─── Move ───────────────────────────────────────────────────────────────────

std::unique_ptr<Command> EditOperations::moveClip(
    Timeline& timeline, size_t trackIndex, uint64_t clipId,
    int64_t newTimelineIn)
{
    if (trackIndex >= timeline.trackCount())
        return nullptr;

    Track* track = timeline.track(trackIndex);
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
    if (!srcTrack || !dstTrack || dstTrack->isDivider())
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

std::unique_ptr<Command> EditOperations::resolveOverlaps(
    Timeline& timeline, size_t trackIndex, uint64_t movedClipId)
{
    if (trackIndex >= timeline.trackCount())
        return nullptr;

    Track* track = timeline.track(trackIndex);
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
                 "clipCount={}",
                 trackIndex, movedClipId, movedIn, movedOut, track->clipCount());

    auto compound = std::make_unique<CompoundCommand>("Resolve overlaps");

    // Collect IDs of clips that overlap the moved clip
    for (size_t i = 0; i < track->clipCount(); ++i) {
        const Clip* other = track->clip(i);
        if (other->id() == movedClipId) continue;  // skip the moved clip itself

        int64_t otherIn  = other->timelineIn();
        int64_t otherOut = other->timelineOut();

        // No overlap?
        if (otherOut <= movedIn || otherIn >= movedOut) continue;

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
            int64_t rightSourceIn = other->sourceIn() + (movedOut - otherIn);
            auto rightClip = other->clone();
            rightClip->setTimelineIn(rightIn);
            rightClip->setDuration(rightDuration);
            rightClip->setSourceIn(rightSourceIn);
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
                    newSourceIn     // advance sourceIn
                ));
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
        if (track->findClipIndexById(ref.clipId) == track->clipCount()) continue;

        compound->addCommand(std::make_unique<RemoveClipCommand>(
            track, ref.clipId));
    }

    return compound->size() > 0 ? std::move(compound) : nullptr;
}

// ─── Clipboard ───────────────────────────────────────────────────────────────

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
    for (const auto& ref : selection.clips())
    {
        if (ref.trackIndex >= timeline.trackCount()) continue;
        const Track* track = timeline.track(ref.trackIndex);
        size_t idx = track->findClipIndexById(ref.clipId);
        if (idx == track->clipCount()) continue;

        const Clip* clip = track->clip(idx);
        ClipboardContents::Entry entry;
        entry.trackIndex   = ref.trackIndex;
        entry.relativeTime = clip->timelineIn() - earliest;
        entry.clip         = clip->clone();
        clipboard.entries.push_back(std::move(entry));
    }
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

// Remap a source track index to a destination track index by matching
// TrackType, preserving the relative ordering of source tracks within
// that type. Critical when pasting between sequences: the clipboard's
// stored trackIndex is from the SOURCE timeline, so applying it
// blindly puts audio clips on whatever track sits at that index in
// the destination — often a video track when the two sequences have
// different layouts.
static std::unordered_map<size_t, size_t> buildTrackRemap(
    const Timeline& timeline, const ClipboardContents& clipboard)
{
    // Per-type sorted unique source track indices (taken from clipboard
    // entries, classified by the clip's type).
    std::unordered_map<TrackType, std::vector<size_t>> srcByType;
    for (const auto& entry : clipboard.entries) {
        if (!entry.clip) continue;
        TrackType t = (entry.clip->clipType() == ClipType::Audio)
                      ? TrackType::Audio : TrackType::Video;
        auto& vec = srcByType[t];
        if (std::find(vec.begin(), vec.end(), entry.trackIndex) == vec.end())
            vec.push_back(entry.trackIndex);
    }
    for (auto& [t, vec] : srcByType) std::sort(vec.begin(), vec.end());

    // Per-type destination track indices in their natural order.
    // Dividers are skipped — they aren't valid paste targets.
    std::unordered_map<TrackType, std::vector<size_t>> dstByType;
    for (size_t i = 0; i < timeline.trackCount(); ++i) {
        const Track* t = timeline.track(i);
        if (t && !t->isDivider())
            dstByType[t->type()].push_back(i);
    }

    std::unordered_map<size_t, size_t> remap;
    for (const auto& [type, srcVec] : srcByType) {
        const auto dstIt = dstByType.find(type);
        if (dstIt == dstByType.end() || dstIt->second.empty()) continue;
        const auto& dstVec = dstIt->second;
        for (size_t i = 0; i < srcVec.size(); ++i) {
            size_t dstPos = std::min(i, dstVec.size() - 1);
            remap[srcVec[i]] = dstVec[dstPos];
        }
    }
    return remap;
}

std::unique_ptr<Command> EditOperations::paste(
    Timeline& timeline, const ClipboardContents& clipboard,
    int64_t playhead)
{
    if (clipboard.empty()) return nullptr;

    auto compound = std::make_unique<CompoundCommand>("Paste");
    const auto remap = buildTrackRemap(timeline, clipboard);

    uint64_t nextGid = freshGroupIdBase(timeline);
    std::unordered_map<uint64_t, uint64_t> gidRemap;

    // Track each pasted clip so we can resolve overlaps after all are placed.
    struct PasteTarget { size_t trackIdx; uint64_t clipId; int64_t timelineIn; };
    std::vector<PasteTarget> targets;

    for (const auto& entry : clipboard.entries)
    {
        if (!entry.clip) continue;
        // Resolve the destination track: prefer the original index when it
        // exists AND matches the clip's type, otherwise fall back to the
        // type-aware remap. This keeps within-sequence paste exactly where
        // it was and only kicks in when crossing sequences with different
        // track layouts.
        size_t destIdx = entry.trackIndex;
        TrackType wantType = (entry.clip->clipType() == ClipType::Audio)
                             ? TrackType::Audio : TrackType::Video;
        bool needRemap = (destIdx >= timeline.trackCount());
        if (!needRemap) {
            const Track* t = timeline.track(destIdx);
            if (!t || t->isDivider() || t->type() != wantType) needRemap = true;
        }
        if (needRemap) {
            auto it = remap.find(entry.trackIndex);
            if (it == remap.end()) continue;
            destIdx = it->second;
        }
        if (destIdx >= timeline.trackCount()) continue;

        auto cloned = entry.clip->clone();
        cloned->setTimelineIn(playhead + entry.relativeTime);
        const int64_t pasteIn = cloned->timelineIn();
        remapClonedGroupId(*cloned, gidRemap, nextGid);

        Track* track = timeline.track(destIdx);

        // Execute the add immediately so the clip is resident on the
        // track when we call resolveOverlaps below.  Use addExecuted so
        // the compound re-executes everything in order on redo.
        auto addCmd = std::make_unique<AddClipCommand>(track, std::move(cloned));
        addCmd->execute();
        compound->addExecuted(std::move(addCmd));

        targets.push_back({destIdx, 0, pasteIn});
    }

    // Resolve overlaps for each pasted clip — the paste operation
    // overwrites any clip that overlaps the pasted range (Premiere-style
    // overwrite paste).
    for (const auto& tgt : targets) {
        Track* track = timeline.track(tgt.trackIdx);
        if (!track) continue;

        // Find the clip we just pasted by matching timelineIn (the ID
        // changed after clone, so we locate by position).
        uint64_t pastedId = 0;
        for (size_t ci = 0; ci < track->clipCount(); ++ci) {
            if (track->clip(ci)->timelineIn() == tgt.timelineIn) {
                pastedId = track->clip(ci)->id();
                break;
            }
        }
        if (pastedId == 0) continue;

        auto overlapCmd = resolveOverlaps(timeline, tgt.trackIdx, pastedId);
        if (overlapCmd) {
            overlapCmd->execute();
            compound->addExecuted(std::move(overlapCmd));
        }
    }

    return compound->size() > 0 ? std::move(compound) : nullptr;
}

std::unique_ptr<Command> EditOperations::pasteInsert(
    Timeline& timeline, const ClipboardContents& clipboard,
    int64_t playhead)
{
    if (clipboard.empty()) return nullptr;

    auto compound = std::make_unique<CompoundCommand>("Insert paste");

    // Determine the total duration of the pasted content per track
    // (from earliest to latest pasted clip edge)
    std::unordered_map<size_t, int64_t> trackInsertDuration;
    for (const auto& entry : clipboard.entries)
    {
        if (entry.trackIndex >= timeline.trackCount()) continue;
        int64_t end = entry.relativeTime + entry.clip->duration();
        auto it = trackInsertDuration.find(entry.trackIndex);
        if (it == trackInsertDuration.end())
            trackInsertDuration[entry.trackIndex] = end;
        else
            it->second = std::max(it->second, end);
    }

    // First: shift existing clips at/after playhead to the right on targeted tracks
    for (auto& [trackIdx, insertDur] : trackInsertDuration)
    {
        Track* track = timeline.track(trackIdx);
        if (!track->isTargeted()) continue;

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
                track, si.id, si.oldIn + insertDur));
        }
    }

    // Second: add the pasted clips at playhead on targeted tracks
    uint64_t nextGid = freshGroupIdBase(timeline);
    std::unordered_map<uint64_t, uint64_t> gidRemap;
    for (const auto& entry : clipboard.entries)
    {
        if (entry.trackIndex >= timeline.trackCount()) continue;
        Track* track = timeline.track(entry.trackIndex);
        if (!track->isTargeted()) continue;

        auto cloned = entry.clip->clone();
        cloned->setTimelineIn(playhead + entry.relativeTime);
        remapClonedGroupId(*cloned, gidRemap, nextGid);

        compound->addCommand(std::make_unique<AddClipCommand>(
            track, std::move(cloned)));
    }

    return compound->size() > 0 ? std::move(compound) : nullptr;
}

std::unique_ptr<Command> EditOperations::duplicateSelection(
    Timeline& timeline, const SelectionSet& selection)
{
    if (selection.empty()) return nullptr;

    auto compound = std::make_unique<CompoundCommand>("Duplicate");

    uint64_t nextGid = freshGroupIdBase(timeline);
    std::unordered_map<uint64_t, uint64_t> gidRemap;

    for (const auto& ref : selection.clips())
    {
        if (ref.trackIndex >= timeline.trackCount()) continue;
        Track* track = timeline.track(ref.trackIndex);
        size_t idx = track->findClipIndexById(ref.clipId);
        if (idx == track->clipCount()) continue;

        const Clip* clip = track->clip(idx);
        auto cloned = clip->clone();
        cloned->setTimelineIn(clip->timelineIn() + kDuplicateOffset);
        remapClonedGroupId(*cloned, gidRemap, nextGid);

        compound->addCommand(std::make_unique<AddClipCommand>(
            track, std::move(cloned)));
    }

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
                auto rightClip = clip->clone();
                rightClip->setTimelineIn(outPoint);
                rightClip->setDuration(rightDuration);
                rightClip->setSourceIn(origSourceIn + rightOffset);
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
                auto rightClip = clip->clone();
                rightClip->setTimelineIn(inPoint); // Rippled: placed right after left
                rightClip->setDuration(rightDuration);
                rightClip->setSourceIn(origSourceIn + rightOffset);
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
                int64_t newIn = inPoint; // Ripple left to close gap
                int64_t newDuration = clip->duration() - trimAmount;
                int64_t newSourceIn = clip->sourceIn() + trimAmount;
                compound->addCommand(std::make_unique<TrimClipCommand>(
                    track, clip->id(), newIn, newDuration, newSourceIn));
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
