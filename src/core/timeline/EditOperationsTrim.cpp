/*
 * EditOperationsTrim.cpp -- split, trim, rolling, ripple, slip, slide.
 *
 * Split from EditOperations.cpp for maintainability.
 */

#include "timeline/EditOperations.h"

#include "timeline/Timeline.h"
#include "timeline/Track.h"
#include "timeline/Clip.h"
#include "timeline/VideoClip.h"
#include "timeline/AudioClip.h"
#include "timeline/ImageClip.h"
#include "timeline/SpineClip.h"
#include "timeline/GraphicClip.h"
#include "timeline/TitleClip.h"
#include "command/Command.h"
#include "command/CompoundCommand.h"
#include "command/LambdaCommand.h"
#include "command/commands/ClipCommands.h"
#include "timeline/Transition.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <spdlog/spdlog.h>

namespace rt {

// Returns true if the media path has a still-image extension. Such "videos"
// are really single frames and should be trim-able to any duration (Premiere-style).
static bool isStillImageMediaPath(const std::string& path)
{
    auto pos = path.find_last_of('.');
    if (pos == std::string::npos) return false;
    std::string ext = path.substr(pos + 1);
    for (auto& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return ext == "png"  || ext == "jpg"  || ext == "jpeg" || ext == "bmp" ||
           ext == "gif"  || ext == "tif"  || ext == "tiff" || ext == "webp" ||
           ext == "tga"  || ext == "dds";
}

// True for a path carrying a known VIDEO extension. Used to keep a (rare)
// video background from being treated as an infinite still.
static bool isVideoMediaPath(const std::string& path)
{
    auto pos = path.find_last_of('.');
    if (pos == std::string::npos) return false;
    std::string ext = path.substr(pos + 1);
    for (auto& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return ext == "mp4"  || ext == "mov"  || ext == "webm" || ext == "mkv" ||
           ext == "avi"  || ext == "m4v"  || ext == "wmv"  || ext == "flv" ||
           ext == "mpg"  || ext == "mpeg" || ext == "ts"   || ext == "m2ts";
}

// A shot-preset BACKGROUND layer (export-to-timeline tags it
// layerId="background_<N>") resolves to a still image in assets/backgrounds —
// the renderer only tries .png/.jpg/.jpeg for these. Its STORED path is often
// a bare name with no extension (e.g. "INTERVIEW"), which isStillImageMediaPath
// can't classify, leaving the head edge un-extendable. Treat such a background
// as a still unless its path explicitly names a video file.
static bool isBackgroundStillClip(const Clip* c)
{
    if (!c) return false;
    auto* vc = dynamic_cast<const VideoClip*>(c);
    if (!vc) return false;
    return c->layerId().rfind("background_", 0) == 0
           && !isVideoMediaPath(vc->mediaPath());
}

// ─── Split ───────────────────────────────────────────────────────────────────

/// Thread-safe unique group-ID generator (shared across all split operations).
static uint64_t nextSplitGroupId()
{
    static std::atomic<uint64_t> s_counter{0x8000'0000'0000'0000ULL}; // high bit avoids AudioSync range
    return s_counter.fetch_add(1, std::memory_order_relaxed);
}

std::unique_ptr<Command> EditOperations::splitClip(
    Timeline& timeline, size_t trackIndex, uint64_t clipId, int64_t splitTime)
{
    return splitClipInternal(timeline, trackIndex, clipId, splitTime, nullptr);
}

std::unique_ptr<Command> EditOperations::splitClipInternal(
    Timeline& timeline, size_t trackIndex, uint64_t clipId, int64_t splitTime,
    std::unordered_map<uint64_t, uint64_t>* groupRemap)
{
    if (trackIndex >= timeline.trackCount())
        return nullptr;

    Track* track = timeline.track(trackIndex);
    size_t idx = track->findClipIndexById(clipId);
    if (idx == track->clipCount())
        return nullptr;

    const Clip* clip = track->clip(idx);

    // Split must be within the clip
    if (splitTime <= clip->timelineIn() || splitTime >= clip->timelineOut())
        return nullptr;

    // Duration must be > 0 on both sides. kMinClipDuration (1 frame @ 24fps =
    // 2000 ticks) is too coarse for split: at 60fps a single frame is 800
    // ticks, so requiring kMinClipDuration on each side blocked the user
    // from pressing F to slice a single frame off a clip. Splits are
    // frame-snapped by the caller (playhead is on a frame boundary), so
    // accepting any positive duration here is safe.
    int64_t leftDuration  = splitTime - clip->timelineIn();
    int64_t rightDuration = clip->timelineOut() - splitTime;
    if (leftDuration <= 0 || rightDuration <= 0)
        return nullptr;

    // ── Capture tail transitions BEFORE the trim mangles them ───────────
    // A transition anchored to this clip's END (a cross-dissolve into the
    // next clip, or a fade-out) conceptually belongs to whatever clip
    // occupies that tail. After the split that's the NEW right half — not
    // the left half. TrimClipCommand's retargetTransitionsForClip() would
    // otherwise drag the dissolve back to the cut point (the left half's
    // new end), which is exactly the "cross fade jumps to the left clip"
    // bug. Snapshot the tail transitions' peers here so we can re-anchor
    // them to the right half once it exists.
    const int64_t originalOut = clip->timelineOut();
    std::vector<uint64_t> tailPeers;  // rightClipId of each tail transition
    for (size_t i = 0; i < track->transitionCount(); ++i) {
        const Transition* tp = track->transition(i);
        if (!tp) continue;
        if (tp->leftClipId == clipId && tp->editPointTick == originalOut)
            tailPeers.push_back(tp->rightClipId);
    }

    auto compound = std::make_unique<CompoundCommand>("Split clip");

    // 1. Trim the original clip to end at splitTime
    compound->addCommand(std::make_unique<TrimClipCommand>(
        track, clipId,
        clip->timelineIn(), leftDuration, clip->sourceIn()));

    // 2. Clone the clip for the right half
    auto rightClip = clip->clone();
    rightClip->setTimelineIn(splitTime);
    rightClip->setDuration(rightDuration);
    // leftDuration is in timeline ticks; convert to source ticks for sourceIn.
    // Use truncation (static_cast) to match the compositor's sourceTick
    // calculation (CompositeServiceLayerBuild / FrameRenderer), so the right
    // half's first frame matches the left half's last.
    const double clipSpd = std::max(clip->speed(), 0.01);
    int64_t rightSrcIn = clip->sourceIn() + static_cast<int64_t>(static_cast<double>(leftDuration) * clipSpd);
    rightClip->setSourceIn(rightSrcIn);
    rightClip->setLabel(clip->label());

    // Assign a new unique group ID so the right half is independently
    // switchable.  When splitting multiple clips from the same group
    // (splitAllAtPlayhead), the groupRemap map ensures all right halves
    // sharing an old group get the SAME new group ID.
    if (rightClip->groupId() != 0) {
        uint64_t oldGid = rightClip->groupId();
        uint64_t newGid = 0;
        if (groupRemap) {
            auto it = groupRemap->find(oldGid);
            if (it != groupRemap->end()) {
                newGid = it->second;
            } else {
                newGid = nextSplitGroupId();
                (*groupRemap)[oldGid] = newGid;
            }
        } else {
            newGid = nextSplitGroupId();
        }
        rightClip->setGroupId(newGid);
    }

    const uint64_t rightId = rightClip->id();
    compound->addCommand(std::make_unique<AddClipCommand>(track, std::move(rightClip)));

    // 3. Re-anchor tail transitions to the new right half. Runs AFTER the
    //    trim+add so the right clip exists. editPointTick is restored to the
    //    original clip end (== rightClip->timelineOut()); the trim's
    //    retarget had shoved it to splitTime.
    if (!tailPeers.empty()) {
        auto doFix = [track, clipId, rightId, originalOut, tailPeers]() {
            for (size_t i = 0; i < track->transitionCount(); ++i) {
                const Transition* tp = track->transition(i);
                if (!tp || tp->leftClipId != clipId) continue;
                if (std::find(tailPeers.begin(), tailPeers.end(),
                              tp->rightClipId) == tailPeers.end()) continue;
                Transition t = *tp;
                t.leftClipId   = rightId;
                t.editPointTick = originalOut;
                track->setTransition(i, t);
            }
        };
        auto undoFix = [track, clipId, rightId, originalOut, tailPeers]() {
            for (size_t i = 0; i < track->transitionCount(); ++i) {
                const Transition* tp = track->transition(i);
                if (!tp || tp->leftClipId != rightId) continue;
                if (std::find(tailPeers.begin(), tailPeers.end(),
                              tp->rightClipId) == tailPeers.end()) continue;
                Transition t = *tp;
                t.leftClipId   = clipId;
                t.editPointTick = originalOut;
                track->setTransition(i, t);
            }
        };
        compound->addCommand(std::make_unique<LambdaCommand>(
            "Re-anchor split transition", std::move(doFix), std::move(undoFix)));
    }

    return compound;
}

std::unique_ptr<Command> EditOperations::splitAllAtPlayhead(
    Timeline& timeline, int64_t playhead)
{
    auto compound = std::make_unique<CompoundCommand>("Split all at playhead");
    bool anyAdded = false;

    // Share a remap table so clips from the same group get the same new group ID
    std::unordered_map<uint64_t, uint64_t> groupRemap;

    for (size_t ti = 0; ti < timeline.trackCount(); ++ti)
    {
        Track* track = timeline.track(ti);
        if (track->isLocked()) continue;

        for (size_t ci = 0; ci < track->clipCount(); ++ci)
        {
            const Clip* clip = track->clip(ci);
            if (playhead > clip->timelineIn() && playhead < clip->timelineOut())
            {
                auto splitCmd = splitClipInternal(timeline, ti, clip->id(), playhead, &groupRemap);
                if (splitCmd)
                {
                    compound->addCommand(std::move(splitCmd));
                    anyAdded = true;
                }
            }
        }
    }

    return anyAdded ? std::move(compound) : nullptr;
}

// ─── Trim ────────────────────────────────────────────────────────────────────

std::unique_ptr<Command> EditOperations::trimClip(
    Timeline& timeline, size_t trackIndex, uint64_t clipId,
    ClipEdge edge, int64_t newEdgeTime)
{
    if (trackIndex >= timeline.trackCount())
        return nullptr;

    Track* track = timeline.track(trackIndex);
    size_t idx = track->findClipIndexById(clipId);
    if (idx == track->clipCount())
        return nullptr;

    const Clip* clip = track->clip(idx);

    // Looping/unbounded clips (Spine character animations, video characters,
    // graphic-layer/still-image "videos", image clips, GraphicClips, and
    // TitleClips) can be extended in either direction because they loop or
    // hold a single frame / are procedurally generated. For these we skip
    // sourceIn>=0 / source-extent clamps so the user can drag the head
    // edge backwards past tick 0 of the source.
    auto isLoopingClip = [](const Clip* c) -> bool {
        if (auto* vc = dynamic_cast<const VideoClip*>(c))
            return vc->isVideoCharacter() || isStillImageMediaPath(vc->mediaPath())
                   || isBackgroundStillClip(c);
        if (dynamic_cast<const ImageClip*>(c))   return true;
        if (dynamic_cast<const SpineClip*>(c))   return true;
        if (dynamic_cast<const GraphicClip*>(c)) return true;
        if (dynamic_cast<const TitleClip*>(c))   return true;
        return false;
    };
    const bool looping = isLoopingClip(clip);
    const double speed = std::max(clip->speed(), 0.01);

    int64_t newIn = clip->timelineIn();
    int64_t newDuration = clip->duration();
    int64_t newSourceIn = clip->sourceIn();

    if (edge == ClipEdge::Head)
    {
        // Moving the in-point: adjust timelineIn, sourceIn, and duration.
        // delta is in timeline ticks; sourceIn is in source ticks, so
        // scale by speed (e.g. speed 0.5 → 1000 timeline ticks = 500 source ticks).
        int64_t delta = newEdgeTime - clip->timelineIn();
        newIn       = newEdgeTime;
        newSourceIn = clip->sourceIn() + static_cast<int64_t>(std::llround(delta * speed));
        newDuration = clip->duration() - delta;

        // Clamp minimum duration
        if (newDuration < kMinClipDuration)
        {
            newDuration = kMinClipDuration;
            newIn       = clip->timelineOut() - kMinClipDuration;
            newSourceIn = clip->sourceIn() + static_cast<int64_t>(std::llround((clip->duration() - kMinClipDuration) * speed));
        }
        // Don't allow timeline to go negative.
        if (newIn < 0)
        {
            int64_t correction = -newIn;
            newIn       = 0;
            newSourceIn += static_cast<int64_t>(std::llround(correction * speed));
            newDuration -= correction;
            if (newDuration < kMinClipDuration)
                newDuration = kMinClipDuration;
        }
        // For non-looping media, also clamp sourceIn so we don't read before
        // the start of the source. Looping clips intentionally allow negative
        // sourceIn (the playback layer wraps modulo source duration).
        if (!looping && newSourceIn < 0)
        {
            // Convert source tick deficit back to timeline ticks
            int64_t correction = static_cast<int64_t>(std::llround(-newSourceIn / speed));
            newSourceIn = 0;
            newIn       += correction;
            newDuration -= correction;
            if (newDuration < kMinClipDuration)
                newDuration = kMinClipDuration;
        }

        // DIAG-TRIM-HEAD: surface why a leftward head-extend may be refusing
        // to move. Logs the still-image/looping classification and the
        // clamps that fired, so an "infinite" PNG that won't extend can be
        // traced to either source-clamp (looping=false) or the tick-0 wall.
        // Only when NOT detected as looping AND the user is dragging the head
        // LEFT (extend) — that's the case the source clamp can block, so the
        // log stays quiet once a clip is correctly classified.
        if (!looping && newEdgeTime < clip->timelineIn()) {
            std::string mp;
            if (auto* vc = dynamic_cast<const VideoClip*>(clip)) mp = vc->mediaPath();
            spdlog::warn("DIAG-TRIM-HEAD clipId={} type={} looping=false layerId='{}' "
                         "still='{}' reqEdge={} origIn={} -> newIn={} (delta={})",
                         clipId, static_cast<int>(clip->clipType()), clip->layerId(),
                         mp.empty() ? "<n/a>" : mp,
                         newEdgeTime, clip->timelineIn(), newIn,
                         newEdgeTime - clip->timelineIn());
        }
    }
    else // Tail
    {
        newDuration = newEdgeTime - clip->timelineIn();
        if (newDuration < kMinClipDuration)
            newDuration = kMinClipDuration;

        // Clamp to source media extent for non-looping clips.
        // sourceDuration == 0 means unknown/unlimited (e.g. SpineClip loops).
        // Still-image "videos" (.png/.jpg/...) are also unbounded.
        int64_t srcDur = 0;
        if (auto* vc = dynamic_cast<const VideoClip*>(clip)) {
            if (!vc->isVideoCharacter() && !isStillImageMediaPath(vc->mediaPath())
                && !isBackgroundStillClip(clip))
                srcDur = vc->sourceDuration();
        } else if (auto* ac = dynamic_cast<const AudioClip*>(clip)) {
            srcDur = ac->sourceDuration();
        }
        if (srcDur > 0) {
            // Duration is in timeline ticks, source range is in source ticks.
            // Convert: maxTimelineDur = sourceRange / speed
            int64_t maxDur = static_cast<int64_t>(std::llround((srcDur - newSourceIn) / speed));
            if (maxDur < kMinClipDuration) maxDur = kMinClipDuration;
            if (newDuration > maxDur) {
                spdlog::info("DIAG-TRIM tail clamp: clipId={} srcDur={} ({:.3f}s) sourceIn={} "
                             "speed={:.2f} requested={} ({:.3f}s) clamped={} ({:.3f}s)",
                             clipId, srcDur, srcDur/48000.0, newSourceIn,
                             speed, newDuration, newDuration/48000.0, maxDur, maxDur/48000.0);
                newDuration = maxDur;
            }
        }
    }

    return std::make_unique<TrimClipCommand>(
        track, clipId, newIn, newDuration, newSourceIn);
}

// ─── Rolling Edit ────────────────────────────────────────────────────────────

std::unique_ptr<Command> EditOperations::rollingEdit(
    Timeline& timeline, size_t trackIndex,
    uint64_t leftClipId, uint64_t rightClipId, int64_t newEditPoint)
{
    if (trackIndex >= timeline.trackCount())
        return nullptr;

    Track* track = timeline.track(trackIndex);

    size_t li = track->findClipIndexById(leftClipId);
    size_t ri = track->findClipIndexById(rightClipId);
    if (li == track->clipCount() || ri == track->clipCount())
        return nullptr;

    const Clip* leftClip  = track->clip(li);
    const Clip* rightClip = track->clip(ri);

    const double leftSpd  = std::max(leftClip->speed(), 0.01);
    const double rightSpd = std::max(rightClip->speed(), 0.01);

    int64_t leftNewDuration = newEditPoint - leftClip->timelineIn();
    int64_t rightEnd = rightClip->timelineOut();

    // Looping/unbounded clips (Spine character animations, video characters,
    // still-image "videos", image clips, GraphicClips, and TitleClips) have
    // no real source extent — they loop or hold a single frame / are
    // procedurally generated, so rolling should let the edit point travel
    // freely past sourceIn=0 / sourceDuration.  Mirrors the same exemption
    // in trimClip above.
    auto isLoopingClip = [](const Clip* c) -> bool {
        if (auto* vc = dynamic_cast<const VideoClip*>(c))
            return vc->isVideoCharacter() || isStillImageMediaPath(vc->mediaPath())
                   || isBackgroundStillClip(c);
        if (dynamic_cast<const ImageClip*>(c))   return true;
        if (dynamic_cast<const SpineClip*>(c))   return true;
        if (dynamic_cast<const GraphicClip*>(c)) return true;
        if (dynamic_cast<const TitleClip*>(c))   return true;
        return false;
    };
    const bool leftLooping  = isLoopingClip(leftClip);
    const bool rightLooping = isLoopingClip(rightClip);

    // Clamp left clip tail to source media extent (non-looping clips)
    int64_t leftMaxDur = INT64_MAX;
    if (!leftLooping) {
        int64_t srcDur = 0;
        if (auto* vc = dynamic_cast<const VideoClip*>(leftClip)) {
            srcDur = vc->sourceDuration();
        } else if (auto* ac = dynamic_cast<const AudioClip*>(leftClip)) {
            srcDur = ac->sourceDuration();
        }
        if (srcDur > 0) {
            leftMaxDur = static_cast<int64_t>(std::llround((srcDur - leftClip->sourceIn()) / leftSpd));
            if (leftMaxDur < kMinClipDuration) leftMaxDur = kMinClipDuration;
        }
    }

    // Clamp source limit for right clip (non-looping).
    // In a rolling edit the right clip's tail is fixed; only its head
    // moves, so the source extent [sourceIn, sourceIn+duration] is
    // constant.  The only source-side constraint is sourceIn >= 0
    // (can't roll the right-clip head left past the start of its media).
    bool  rightHasSourceLimit = false;
    if (!rightLooping) {
        if (auto* vc = dynamic_cast<const VideoClip*>(rightClip)) {
            (void)vc;
            rightHasSourceLimit = true;
        } else if (dynamic_cast<const AudioClip*>(rightClip)) {
            rightHasSourceLimit = true;
        }
    }

    int64_t rightNewDuration = rightEnd - newEditPoint;

    // ── Check if right clip is fully consumed ───────────────────────────
    if (rightNewDuration < kMinClipDuration && leftNewDuration <= rightEnd - leftClip->timelineIn())
    {
        // Clamp left extension to source limit
        int64_t leftExtendDur = rightEnd - leftClip->timelineIn();
        if (leftExtendDur > leftMaxDur)
            leftExtendDur = leftMaxDur;
        if (leftExtendDur < kMinClipDuration)
            leftExtendDur = kMinClipDuration;

        auto compound = std::make_unique<CompoundCommand>("Rolling edit");
        compound->addCommand(std::make_unique<TrimClipCommand>(
            track, leftClipId,
            leftClip->timelineIn(), leftExtendDur, leftClip->sourceIn()));
        compound->addCommand(std::make_unique<RemoveClipCommand>(track, rightClipId));
        return compound;
    }

    // ── Check if left clip is fully consumed ────────────────────────────
    if (leftNewDuration < kMinClipDuration)
    {
        // Right clip extends backwards: new timelineIn = leftClip->timelineIn()
        int64_t rightExtendDur = rightEnd - leftClip->timelineIn();
        int64_t rightSrcDelta = leftClip->timelineIn() - rightClip->timelineIn(); // negative, timeline ticks
        int64_t rightNewSourceIn = rightClip->sourceIn() + static_cast<int64_t>(std::llround(rightSrcDelta * rightSpd));

        // Clamp right source to source limit
        if (rightHasSourceLimit) {
            // The right clip's source can't go negative.
            // Convert sourceIn ticks to timeline ticks for comparison.
            int64_t maxBackShift = static_cast<int64_t>(std::llround(rightClip->sourceIn() / rightSpd));
            if (rightSrcDelta < -maxBackShift) {
                // Can't shift source that far back; clamp
                rightNewSourceIn = 0;
                int64_t actualBackShift = -maxBackShift;
                int64_t actualLeftIn = rightClip->timelineIn() + actualBackShift;
                rightExtendDur = rightEnd - actualLeftIn;
            }
        }
        if (rightExtendDur < kMinClipDuration)
            rightExtendDur = kMinClipDuration;

        auto compound = std::make_unique<CompoundCommand>("Rolling edit");
        compound->addCommand(std::make_unique<RemoveClipCommand>(track, leftClipId));
        compound->addCommand(std::make_unique<TrimClipCommand>(
            track, rightClipId,
            leftClip->timelineIn(), rightExtendDur, rightNewSourceIn));
        return compound;
    }

    // ── Clamp to minimum durations ──────────────────────────────────────
    if (leftNewDuration < kMinClipDuration)
    {
        newEditPoint = leftClip->timelineIn() + kMinClipDuration;
        leftNewDuration = kMinClipDuration;
        rightNewDuration = rightEnd - newEditPoint;
    }
    if (leftNewDuration > leftMaxDur)
    {
        leftNewDuration = leftMaxDur;
        newEditPoint = leftClip->timelineIn() + leftNewDuration;
        rightNewDuration = rightEnd - newEditPoint;
    }
    // Right clip head can't roll left past source start (sourceIn >= 0).
    // Its tail is fixed, so the duration check used for regular trims
    // doesn't apply — rolling only shifts the head.
    if (rightHasSourceLimit) {
        int64_t rightSrcDelta = newEditPoint - rightClip->timelineIn(); // timeline ticks
        int64_t rightNewSourceIn = rightClip->sourceIn() + static_cast<int64_t>(std::llround(rightSrcDelta * rightSpd));
        if (rightNewSourceIn < 0) {
            // Clamp: move the edit point right until sourceIn reaches 0.
            // maxLeftShift is how far the head can move left before sourceIn hits 0.
            int64_t maxLeftShift = static_cast<int64_t>(std::llround(rightClip->sourceIn() / rightSpd));
            newEditPoint = rightClip->timelineIn() - maxLeftShift;
            leftNewDuration = newEditPoint - leftClip->timelineIn();
            rightNewDuration = rightEnd - newEditPoint;
        }
    }
    if (rightNewDuration < kMinClipDuration)
    {
        newEditPoint = rightEnd - kMinClipDuration;
        leftNewDuration = newEditPoint - leftClip->timelineIn();
        rightNewDuration = kMinClipDuration;
    }

    int64_t rightSourceDelta = newEditPoint - rightClip->timelineIn(); // timeline ticks

    auto compound = std::make_unique<CompoundCommand>("Rolling edit");

    // Trim left clip tail
    compound->addCommand(std::make_unique<TrimClipCommand>(
        track, leftClipId,
        leftClip->timelineIn(), leftNewDuration, leftClip->sourceIn()));

    // Trim right clip head — convert timeline delta to source ticks
    compound->addCommand(std::make_unique<TrimClipCommand>(
        track, rightClipId,
        newEditPoint, rightNewDuration,
        rightClip->sourceIn() + static_cast<int64_t>(std::llround(rightSourceDelta * rightSpd))));

    return compound;
}

// ─── Ripple Edit ─────────────────────────────────────────────────────────────

std::unique_ptr<Command> EditOperations::rippleTrim(
    Timeline& timeline, size_t trackIndex, uint64_t clipId,
    ClipEdge edge, int64_t newEdgeTime)
{
    if (trackIndex >= timeline.trackCount())
        return nullptr;

    Track* track = timeline.track(trackIndex);
    size_t idx = track->findClipIndexById(clipId);
    if (idx == track->clipCount())
        return nullptr;

    const Clip* clip = track->clip(idx);
    int64_t oldOut = clip->timelineOut();
    int64_t oldIn  = clip->timelineIn();

    auto compound = std::make_unique<CompoundCommand>("Ripple trim");

    // First, do the normal trim
    auto trimCmd = trimClip(timeline, trackIndex, clipId, edge, newEdgeTime);
    if (!trimCmd) return nullptr;
    compound->addCommand(std::move(trimCmd));

    // Calculate the ripple delta
    int64_t rippleDelta = 0;
    if (edge == ClipEdge::Tail)
    {
        int64_t newOut = newEdgeTime;
        if (newOut - oldIn < kMinClipDuration)
            newOut = oldIn + kMinClipDuration;
        rippleDelta = newOut - oldOut;
    }
    else
    {
        int64_t newIn = newEdgeTime;
        if (oldOut - newIn < kMinClipDuration)
            newIn = oldOut - kMinClipDuration;
        rippleDelta = newIn - oldIn;
    }

    // Shift all subsequent clips
    if (rippleDelta != 0)
    {
        for (size_t ci = 0; ci < track->clipCount(); ++ci)
        {
            const Clip* subsequent = track->clip(ci);
            if (subsequent->id() == clipId) continue;

            bool isAfter = (edge == ClipEdge::Tail)
                ? subsequent->timelineIn() >= oldOut
                : subsequent->timelineIn() > oldIn;

            if (isAfter)
            {
                compound->addCommand(std::make_unique<MoveClipCommand>(
                    track, subsequent->id(),
                    subsequent->timelineIn() + rippleDelta));
            }
        }
    }

    return compound;
}

std::unique_ptr<Command> EditOperations::rippleDelete(
    Timeline& timeline, const SelectionSet& selection)
{
    if (selection.empty()) return nullptr;

    auto compound = std::make_unique<CompoundCommand>("Ripple delete");

    // Group by track and sort by position for gap calculation
    struct ClipInfo {
        size_t trackIndex;
        uint64_t clipId;
        int64_t timelineIn;
        int64_t duration;
    };

    std::vector<ClipInfo> toRemove;
    for (const auto& ref : selection.clips())
    {
        if (ref.trackIndex >= timeline.trackCount()) continue;
        const Track* track = timeline.track(ref.trackIndex);
        size_t idx = track->findClipIndexById(ref.clipId);
        if (idx == track->clipCount()) continue;
        const Clip* clip = track->clip(idx);
        toRemove.push_back({ref.trackIndex, ref.clipId,
                           clip->timelineIn(), clip->duration()});
    }

    // Sort by track, then by timeline position (descending for safe removal)
    std::sort(toRemove.begin(), toRemove.end(),
              [](const ClipInfo& a, const ClipInfo& b)
              {
                  if (a.trackIndex != b.trackIndex) return a.trackIndex < b.trackIndex;
                  return a.timelineIn > b.timelineIn;
              });

    // Process each track
    size_t currentTrack = SIZE_MAX;
    int64_t totalGap = 0;

    for (const auto& info : toRemove)
    {
        if (info.trackIndex != currentTrack)
        {
            currentTrack = info.trackIndex;
            totalGap = 0;
        }

        Track* track = timeline.track(info.trackIndex);

        // Remove the clip
        compound->addCommand(std::make_unique<RemoveClipCommand>(
            track, info.clipId));

        totalGap += info.duration;
    }

    // Shift subsequent clips to close gaps (per track)
    // Re-process from left to right
    std::sort(toRemove.begin(), toRemove.end(),
              [](const ClipInfo& a, const ClipInfo& b)
              {
                  if (a.trackIndex != b.trackIndex) return a.trackIndex < b.trackIndex;
                  return a.timelineIn < b.timelineIn;
              });

    // Collect per-track ripple info: earliest edit point and total gap
    struct TrackRipple {
        int64_t earliestEdit{INT64_MAX};
        int64_t totalGap{0};
    };
    std::unordered_map<size_t, TrackRipple> trackRipples;

    currentTrack = SIZE_MAX;
    int64_t accumulatedGap = 0;

    // Build O(1) lookup for removed clip IDs
    std::unordered_set<uint64_t> removedIds;
    removedIds.reserve(toRemove.size());
    for (const auto& info : toRemove)
        removedIds.insert(info.clipId);

    for (const auto& info : toRemove)
    {
        if (info.trackIndex != currentTrack)
        {
            currentTrack = info.trackIndex;
            accumulatedGap = 0;
        }

        accumulatedGap += info.duration;
        Track* track = timeline.track(info.trackIndex);

        auto& rip = trackRipples[info.trackIndex];
        rip.earliestEdit = std::min(rip.earliestEdit, info.timelineIn);
        rip.totalGap = accumulatedGap;

        // Shift all clips after this one by the accumulated gap
        for (size_t ci = 0; ci < track->clipCount(); ++ci)
        {
            const Clip* clip = track->clip(ci);
            if (removedIds.find(clip->id()) == removedIds.end() &&
                clip->timelineIn() > info.timelineIn)
            {
                compound->addCommand(std::make_unique<MoveClipCommand>(
                    track, clip->id(),
                    clip->timelineIn() - accumulatedGap));
            }
        }
    }

    // Sync Lock: shift clips on other sync-locked tracks by the same gap
    for (const auto& [editedTrackIdx, rip] : trackRipples)
    {
        for (size_t ti = 0; ti < timeline.trackCount(); ++ti)
        {
            if (ti == editedTrackIdx) continue;
            Track* otherTrack = timeline.track(ti);
            if (!otherTrack->isSyncLocked()) continue;

            for (size_t ci = 0; ci < otherTrack->clipCount(); ++ci)
            {
                const Clip* clip = otherTrack->clip(ci);
                if (clip->timelineIn() >= rip.earliestEdit)
                {
                    int64_t newPos = clip->timelineIn() - rip.totalGap;
                    if (newPos < 0) newPos = 0;
                    compound->addCommand(std::make_unique<MoveClipCommand>(
                        otherTrack, clip->id(), newPos));
                }
            }
        }
    }

    return compound;
}

// ─── Close Gap ───────────────────────────────────────────────────────────────

std::unique_ptr<Command> EditOperations::closeGap(
    Timeline& timeline, size_t trackIndex,
    int64_t gapStart, int64_t gapEnd)
{
    if (trackIndex >= timeline.trackCount() || gapEnd <= gapStart)
        return nullptr;

    int64_t gapDuration = gapEnd - gapStart;
    auto compound = std::make_unique<CompoundCommand>("Close gap");
    bool anyMoved = false;

    Track* track = timeline.track(trackIndex);
    for (size_t ci = 0; ci < track->clipCount(); ++ci) {
        const Clip* clip = track->clip(ci);
        if (clip->timelineIn() >= gapEnd) {
            compound->addCommand(std::make_unique<MoveClipCommand>(
                track, clip->id(), clip->timelineIn() - gapDuration));
            anyMoved = true;
        }
    }

    // Also shift clips on sync-locked tracks
    for (size_t ti = 0; ti < timeline.trackCount(); ++ti) {
        if (ti == trackIndex) continue;
        Track* otherTrack = timeline.track(ti);
        if (!otherTrack->isSyncLocked()) continue;

        for (size_t ci = 0; ci < otherTrack->clipCount(); ++ci) {
            const Clip* clip = otherTrack->clip(ci);
            if (clip->timelineIn() >= gapEnd) {
                int64_t newPos = clip->timelineIn() - gapDuration;
                if (newPos < 0) newPos = 0;
                compound->addCommand(std::make_unique<MoveClipCommand>(
                    otherTrack, clip->id(), newPos));
                anyMoved = true;
            }
        }
    }

    return anyMoved ? std::move(compound) : nullptr;
}

std::unique_ptr<Command> EditOperations::openGap(
    Timeline& timeline, size_t trackIndex,
    int64_t insertTick, int64_t insertDuration)
{
    if (trackIndex >= timeline.trackCount() || insertDuration <= 0)
        return nullptr;

    auto compound = std::make_unique<CompoundCommand>("Insert (open gap)");
    bool anyMoved = false;

    Track* track = timeline.track(trackIndex);
    for (size_t ci = 0; ci < track->clipCount(); ++ci) {
        const Clip* clip = track->clip(ci);
        // Only shift clips whose START is at or after the insert point.
        // Clips that straddle the insert tick are left alone; the drop's
        // existing overwrite-resolve pass trims them as before.
        if (clip->timelineIn() >= insertTick) {
            compound->addCommand(std::make_unique<MoveClipCommand>(
                track, clip->id(), clip->timelineIn() + insertDuration));
            anyMoved = true;
        }
    }

    // Mirror closeGap(): sync-locked tracks ripple along with the edit.
    for (size_t ti = 0; ti < timeline.trackCount(); ++ti) {
        if (ti == trackIndex) continue;
        Track* otherTrack = timeline.track(ti);
        if (!otherTrack->isSyncLocked()) continue;

        for (size_t ci = 0; ci < otherTrack->clipCount(); ++ci) {
            const Clip* clip = otherTrack->clip(ci);
            if (clip->timelineIn() >= insertTick) {
                compound->addCommand(std::make_unique<MoveClipCommand>(
                    otherTrack, clip->id(), clip->timelineIn() + insertDuration));
                anyMoved = true;
            }
        }
    }

    return anyMoved ? std::move(compound) : nullptr;
}

// ─── Slip ────────────────────────────────────────────────────────────────────

std::unique_ptr<Command> EditOperations::slipClip(
    Timeline& timeline, size_t trackIndex, uint64_t clipId,
    int64_t sourceInDelta)
{
    if (trackIndex >= timeline.trackCount())
        return nullptr;

    Track* track = timeline.track(trackIndex);
    size_t idx = track->findClipIndexById(clipId);
    if (idx == track->clipCount())
        return nullptr;

    const Clip* clip = track->clip(idx);
    int64_t newSourceIn = clip->sourceIn() + sourceInDelta;

    // Clamp sourceIn to non-negative
    if (newSourceIn < 0) newSourceIn = 0;

    // Clamp sourceIn so sourceOut doesn't exceed source media extent
    int64_t srcDur = 0;
    if (auto* vc = dynamic_cast<const VideoClip*>(clip)) {
        if (!vc->isVideoCharacter())
            srcDur = vc->sourceDuration();
    } else if (auto* ac = dynamic_cast<const AudioClip*>(clip)) {
        srcDur = ac->sourceDuration();
    }
    if (srcDur > 0) {
        // duration is in timeline ticks; convert to source ticks via speed
        double spd = std::max(clip->speed(), 0.01);
        int64_t srcConsumed = static_cast<int64_t>(std::llround(clip->duration() * spd));
        int64_t maxSourceIn = srcDur - srcConsumed;
        if (maxSourceIn < 0) maxSourceIn = 0;
        if (newSourceIn > maxSourceIn)
            newSourceIn = maxSourceIn;
    }

    // Only change the sourceIn, not the timeline position or duration
    return std::make_unique<TrimClipCommand>(
        track, clipId,
        clip->timelineIn(), clip->duration(), newSourceIn);
}

// ─── Slide ───────────────────────────────────────────────────────────────────

std::unique_ptr<Command> EditOperations::slideClip(
    Timeline& timeline, size_t trackIndex, uint64_t clipId,
    int64_t slideDelta)
{
    if (trackIndex >= timeline.trackCount() || slideDelta == 0)
        return nullptr;

    Track* track = timeline.track(trackIndex);
    size_t idx = track->findClipIndexById(clipId);
    if (idx == track->clipCount())
        return nullptr;

    const Clip* clip = track->clip(idx);
    auto compound = std::make_unique<CompoundCommand>("Slide clip");

    // Move the clip
    int64_t newPos = std::max<int64_t>(0, clip->timelineIn() + slideDelta);
    compound->addCommand(std::make_unique<MoveClipCommand>(
        track, clipId, newPos));

    // Find neighbor clips and adjust their durations
    // Left neighbor: clip whose out-point touches our in-point
    // Right neighbor: clip whose in-point touches our out-point
    for (size_t ci = 0; ci < track->clipCount(); ++ci)
    {
        const Clip* neighbor = track->clip(ci);
        if (neighbor->id() == clipId) continue;

        // Left neighbor — adjust its tail
        if (neighbor->timelineOut() == clip->timelineIn())
        {
            int64_t newDuration = neighbor->duration() + slideDelta;
            if (newDuration >= kMinClipDuration)
            {
                compound->addCommand(std::make_unique<TrimClipCommand>(
                    track, neighbor->id(),
                    neighbor->timelineIn(), newDuration, neighbor->sourceIn()));
            }
        }

        // Right neighbor — adjust its head
        if (neighbor->timelineIn() == clip->timelineOut())
        {
            int64_t newIn = neighbor->timelineIn() + slideDelta;
            int64_t newDuration = neighbor->duration() - slideDelta;
            int64_t newSourceIn = neighbor->sourceIn() + slideDelta;
            if (newDuration >= kMinClipDuration && newSourceIn >= 0)
            {
                compound->addCommand(std::make_unique<TrimClipCommand>(
                    track, neighbor->id(),
                    newIn, newDuration, newSourceIn));
            }
        }
    }

    return compound;
}

} // namespace rt
