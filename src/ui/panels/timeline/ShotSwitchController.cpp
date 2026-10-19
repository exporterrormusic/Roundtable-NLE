/*
 * ShotSwitchController.cpp - undo-aware shot-preset switching
 * (see ShotSwitchController.h).
 *
 * apply() runs in phases, each a helper below:
 *   1. measureGroup     — the group's time span and the video tracks it uses
 *   2. snapshotGroup    — clones of the old visual clips (ids kept) for undo
 *   3. planTracks       — which track each new layer lands on, and how many
 *                         tracks must be inserted/appended (pure: no edits)
 *   4. buildNewClips    — one clip per visible preset layer
 *   5. redo/undo        — swap the clips and apply/revert the track plan
 */
#include "panels/timeline/ShotSwitchController.h"
#include "panels/characters/PuppetLibrary.h"
#include "command/CommandStack.h"
#include "command/LambdaCommand.h"
#include "timeline/Timeline.h"
#include "timeline/Track.h"
#include "timeline/VideoClip.h"
#include "timeline/SpineClip.h"
#include "timeline/PngPuppetClip.h"
#include <QFileInfo>
#include <QString>
#include <algorithm>
#include <functional>
#include <memory>
#include <vector>

#ifdef ROUNDTABLE_HAS_SPINE
#include "spine/ShotPreset.h"
#endif

#include "effects/Blur.h"

#include <spdlog/spdlog.h>

namespace rt {

namespace shot_switch_detail {

struct ClipSnapshot {
    size_t trackIndex;
    std::unique_ptr<Clip> clip;
};
using Snapshots = std::vector<ClipSnapshot>;

/// A shot switch replaces every visual member and updates metadata on the
/// rest of the group. Refuse the whole operation if any member is on a
/// locked track; partially switching a group would leave a broken shot.
bool groupHasLockedMember(const Timeline& timeline, uint64_t groupId)
{
    for (size_t ti = 0; ti < timeline.trackCount(); ++ti) {
        const Track* track = timeline.track(ti);
        if (!track || !track->isLocked()) continue;
        for (size_t ci = 0; ci < track->clipCount(); ++ci) {
            const Clip* clip = track->clip(ci);
            if (clip && clip->groupId() == groupId) {
                spdlog::info(
                    "TimelineWorkspace: shot switch refused because track '{}' is locked",
                    track->name());
                return true;
            }
        }
    }
    return false;
}

struct GroupSpan {
    int64_t start{0};
    int64_t end{48000};                // 1s fallback (used only if no group clips found)
    std::vector<size_t> videoTracks;   // ascending; front (top) -> back (bottom)
    [[nodiscard]] int64_t duration() const { return std::max<int64_t>(1, end - start); }
};

/// The group's time position/duration AND the video tracks it currently
/// occupies (so the new preset's layers land on those same tracks instead
/// of being re-routed elsewhere).
///
/// The span is [min(timelineIn), max(timelineOut)] over every visual clip
/// in this one shot instance. For a normal shot group all members are
/// aligned, so this matches the first clip's range. PropertiesPanel
/// partitions discontinuous multi-selections before they arrive here,
/// preventing the span from bridging unselected clips.
GroupSpan measureGroup(const Timeline& timeline, uint64_t groupId)
{
    GroupSpan span;
    bool foundGroupTime = false;
    for (size_t ti = 0; ti < timeline.trackCount(); ++ti) {
        const Track* trk = timeline.track(ti);
        if (!trk) continue;
        bool trackHasGroupVisual = false;
        for (size_t ci = 0; ci < trk->clipCount(); ++ci) {
            const Clip* c = trk->clip(ci);
            if (c && c->groupId() == groupId && c->isVisual()) {
                const int64_t cIn  = c->timelineIn();
                const int64_t cOut = c->timelineOut();
                if (!foundGroupTime) {
                    span.start = cIn;
                    span.end   = cOut;
                    foundGroupTime = true;
                } else {
                    if (cIn  < span.start) span.start = cIn;
                    if (cOut > span.end)   span.end   = cOut;
                }
                trackHasGroupVisual = true;
            }
        }
        if (trackHasGroupVisual && trk->type() == TrackType::Video)
            span.videoTracks.push_back(ti);
    }
    return span;
}

struct GroupSnapshot {
    Snapshots clips;
    std::string shotName;
    /// AudioSync export back-link of the group being replaced — the new
    /// clips must inherit it, or the incremental re-export would no longer
    /// recognise the (shot-swapped) group and would rebuild it from the
    /// default shot, losing the user's swap.
    int32_t syncLine{-1};
};

/// Clone the old visual clips in this group (for undo).
/// IMPORTANT: clone() assigns a fresh global ID, so the original id is
/// copied onto each snapshot. insertSnapshots() then keeps that id stable
/// across every undo/redo cycle. Without this, undoing a shot switch
/// resurrects the old clips with brand-new ids and any earlier undo command
/// on the stack that referenced them by id (a prior MoveClipCommand, trim,
/// etc.) silently fails to apply.
GroupSnapshot snapshotGroup(const Timeline& timeline, uint64_t groupId)
{
    GroupSnapshot snap;
    for (size_t ti = 0; ti < timeline.trackCount(); ++ti) {
        const Track* trk = timeline.track(ti);
        for (size_t ci = 0; ci < trk->clipCount(); ++ci) {
            const Clip* c = trk->clip(ci);
            if (c && c->groupId() == groupId && c->isVisual()) {
                if (snap.shotName.empty() && !c->shotName().empty())
                    snap.shotName = c->shotName();
                if (snap.syncLine < 0) snap.syncLine = c->syncLine();
                auto snapClone = c->clone();
                snapClone->setId(c->id()); // preserve original id for undo correctness
                snap.clips.push_back({ti, std::move(snapClone)});
            }
        }
    }
    return snap;
}

size_t visibleLayerCount(const ShotPreset& preset)
{
    size_t count = 0;
    for (const auto& lr : preset.layerOrder()) {
        bool vis = false;
        if (lr.type == LayerType::Background) {
            const auto* bg = preset.background(lr.index);
            vis = (bg && bg->visible);
        } else {
            const auto* ch = preset.character(lr.index);
            vis = (ch && ch->visible);
        }
        if (vis) ++count;
    }
    return count;
}

/// Where each new layer goes. layerIdx counts from 0 = back (BG) upward to
/// N-1 = front (top char).
struct TrackPlan {
    std::vector<size_t> layerTracks;
    /// Index at which to insert new tracks (all stacked at the same
    /// position; smaller index = top of stack).
    size_t insertAt{SIZE_MAX};
    /// How many new video tracks to insert there.
    size_t inserts{0};
    /// Height for each inserted track (inherits from the track currently at
    /// insertAt so the user's customised heights aren't visually disrupted).
    float insertHeight{80.0f};
    /// Extra tracks appended to the bottom of the video stack (used when
    /// the group has no visual clips on the timeline).
    size_t appends{0};
};

/// Pick the video tracks each new layer should occupy.
///
/// Constraint: replace the OLD shot's clips on the SAME video tracks the
/// group is currently using. The back/BG layer keeps its bottom track,
/// front layers keep theirs. Only when the new preset has MORE layers than
/// the old shot are tracks added — empty tracks near the group first, then
/// new ones ABOVE the top-most group track so the BG track index doesn't
/// shift.
///
/// Track-index convention: smaller index = HIGHER in the video stack
/// (renders on top). So span.videoTracks.back() = bottom of group = BG
/// track, span.videoTracks.front() = top of group = front character.
///
/// IMPORTANT: this planning is PURE — it does NOT modify the timeline. Any
/// track inserts/adds are deferred to redo so undo can reverse them.
TrackPlan planTracks(const Timeline& timeline, const GroupSpan& span, size_t neededTracks)
{
    TrackPlan plan;
    plan.layerTracks.assign(neededTracks, SIZE_MAX);
    const auto& groupVideoTracks = span.videoTracks;

    if (!groupVideoTracks.empty() && neededTracks > 0) {
        const size_t reusableN = std::min(neededTracks, groupVideoTracks.size());

        // ── Reuse empty tracks before creating new ones ──────────────
        // A track is "empty" if it has no clips that overlap the group's
        // time span [start, end]. Tracks with clips elsewhere on the
        // timeline are still usable when they're clear at this spot.
        auto trackEmptyInRange = [&timeline](size_t ti, int64_t start, int64_t end) -> bool {
            const Track* t = timeline.track(ti);
            if (!t || t->isLocked()) return false;
            for (size_t ci = 0; ci < t->clipCount(); ++ci) {
                const Clip* c = t->clip(ci);
                if (!c) continue;
                // Overlap check: clip [cIn, cOut) vs range [start, end)
                if (c->timelineIn() < end && c->timelineOut() > start)
                    return false; // overlaps — not empty here
            }
            return true; // clear in this time range
        };

        size_t shortage = (neededTracks > groupVideoTracks.size())
            ? neededTracks - groupVideoTracks.size() : 0;
        std::vector<size_t> emptyCandidates;
        if (shortage > 0) {
            // Scan every video track, checking for clips that overlap
            // the group's time span (not the entire track lifetime).
            for (size_t ti = 0; ti < timeline.trackCount(); ++ti) {
                const Track* t = timeline.track(ti);
                if (!t || t->isDivider() || t->type() != TrackType::Video)
                    continue;
                if (!trackEmptyInRange(ti, span.start, span.end)) continue;
                bool inGroup = false;
                for (size_t g : groupVideoTracks)
                    if (g == ti) { inGroup = true; break; }
                if (inGroup) continue;
                emptyCandidates.push_back(ti);
            }
            // Sort by distance to group for compactness
            const size_t gf = groupVideoTracks.front();
            const size_t gr = groupVideoTracks.back();
            std::sort(emptyCandidates.begin(), emptyCandidates.end(),
                [gf, gr](size_t a, size_t b) {
                    auto d = [gf, gr](size_t x) {
                        if (x < gf) return gf - x;
                        if (x > gr) return x - gr;
                        return size_t{0};
                    };
                    size_t da = d(a), db = d(b);
                    if (da != db) return da < db;
                    return a < b;
                });
        }

        // Consume empty candidates in proximity order (closest first for
        // compact placement). Only create new tracks when there are
        // genuinely no tracks clear in this time range anywhere.
        size_t shift = 0;
        std::vector<size_t> extraTracks;
        if (shortage > 0) {
            size_t usedEmpty = 0;
            for (size_t ei = 0; ei < emptyCandidates.size() && usedEmpty < shortage; ++ei) {
                extraTracks.push_back(emptyCandidates[ei]);
                ++usedEmpty;
            }
            shortage -= usedEmpty;

            // Only insert new tracks as a last resort
            if (shortage > 0) {
                plan.inserts  = shortage;
                plan.insertAt = groupVideoTracks.front();
                if (plan.insertAt < timeline.trackCount()) {
                    const Track* refTrack = timeline.track(plan.insertAt);
                    if (refTrack && refTrack->height() >= 1.0f)
                        plan.insertHeight = refTrack->height();
                }
                shift = plan.inserts;
            }

            // Map the extra (reused empty) track indices into layerTracks.
            // If new tracks are being inserted at insertAt, any reused
            // track at-or-after insertAt shifts down by the insert count.
            for (size_t e = 0; e < extraTracks.size(); ++e) {
                size_t idx = extraTracks[e];
                if (idx >= plan.insertAt && plan.insertAt != SIZE_MAX)
                    idx += shift;
                plan.layerTracks[reusableN + e] = idx;
            }
        }

        // Reused-group layer indices, accounting for the upcoming shift:
        // existing group tracks at-or-after insertAt move down by `shift`.
        for (size_t k = 0; k < reusableN; ++k) {
            size_t orig = groupVideoTracks[groupVideoTracks.size() - 1 - k];
            plan.layerTracks[k] = (orig >= plan.insertAt && plan.insertAt != SIZE_MAX)
                ? orig + shift : orig;
        }

        // New tracks all land at insertAt (each insert shifts the previous
        // one to insertAt+1, etc.). Map the topmost (front) layer to the
        // topmost (smallest index) new track.
        for (size_t e = 0; e < plan.inserts; ++e) {
            plan.layerTracks[reusableN + extraTracks.size() + e] =
                plan.insertAt + (plan.inserts - 1 - e);
        }

        spdlog::warn("applyShotSwitch: layerTracks=[{}] reusableN={} extraTracks={} "
                     "numInserts={} insertPlanAt={} shift={}",
            [&]{
                std::string s;
                for (size_t i = 0; i < plan.layerTracks.size(); ++i) {
                    if (i) s += ",";
                    s += std::to_string(plan.layerTracks[i]);
                }
                return s;
            }(),
            reusableN, extraTracks.size(), plan.inserts,
            plan.insertAt == SIZE_MAX ? std::string{"NONE"} : std::to_string(plan.insertAt),
            shift);
    } else if (neededTracks > 0) {
        // No existing group on timeline -- place layers at the bottom of
        // the video stack, appending tracks (via addVideoTrack) as needed.
        std::vector<size_t> videoIndices;
        for (size_t ti = 0; ti < timeline.trackCount(); ++ti)
            if (timeline.track(ti)->type() == TrackType::Video)
                videoIndices.push_back(ti);
        if (videoIndices.size() < neededTracks)
            plan.appends = neededTracks - videoIndices.size();
        // After the appends, video indices run from
        // [old front .. old back, new1, new2, ...] -- where new tracks
        // sit just after the existing video tracks. Their indices come
        // immediately after videoIndices.back() (or 0 if no video yet).
        std::vector<size_t> finalVideo = videoIndices;
        size_t nextIdx = videoIndices.empty() ? 0 : videoIndices.back() + 1;
        for (size_t e = 0; e < plan.appends; ++e)
            finalVideo.push_back(nextIdx + e);
        for (size_t k = 0; k < neededTracks; ++k)
            plan.layerTracks[k] = finalVideo[finalVideo.size() - 1 - k];
    }
    return plan;
}

/// Shared placement of every new layer clip.
struct LayerPlacement {
    int64_t start{0};
    int64_t duration{0};
    uint64_t groupId{0};
    const std::string* shotName{nullptr};
};

constexpr float kLayoutW = 1920.0f, kLayoutH = 1080.0f;

void place(Clip& clip, const LayerPlacement& at, const std::string& label,
           const std::string& layerId)
{
    clip.setTimelineIn(at.start);
    clip.setDuration(at.duration);
    clip.setLabel(label);
    clip.setShotName(*at.shotName);
    clip.setGroupId(at.groupId);
    clip.setLayerId(layerId);
}

template<typename ClipT>
void addBlur(ClipT& clip, float radius)
{
    if (radius <= 0.0f) return;
    auto fx = std::make_unique<Blur>();
    fx->param(Blur::Radius).track.setDefaultValue(radius);
    clip.effects().addEffect(std::move(fx));
}

/// "CHARACTER - ANIMATION"
std::string characterLabel(const CharacterState& ch)
{
    std::string label = ch.characterName;
    if (!ch.animation.empty())
        label += " - " + ch.animation;
    return label;
}

std::unique_ptr<Clip> makeBackgroundClip(const BackgroundState& bg, int index,
                                         const LayerPlacement& at)
{
    auto vc = std::make_unique<VideoClip>();
    vc->setMediaPath(bg.path);
    // Use original filename (no extension) as label
    const QString bgLabel = QFileInfo(QString::fromStdString(bg.path)).baseName();
    place(*vc, at, bgLabel.toStdString(), "background_" + std::to_string(index));
    vc->positionX().setDefaultValue((bg.posX - 0.5f) * kLayoutW);
    vc->positionY().setDefaultValue((bg.posY - 0.5f) * kLayoutH);
    vc->scaleX().setDefaultValue(bg.scale);
    vc->scaleY().setDefaultValue(bg.scale);
    vc->opacity().setDefaultValue(bg.opacity);
    if (bg.cropLeft > 0 || bg.cropRight > 0 || bg.cropTop > 0 || bg.cropBottom > 0)
        vc->setCrop(bg.cropLeft, bg.cropRight, bg.cropTop, bg.cropBottom);
    addBlur(*vc, bg.blur);
    return vc;
}

/// Position, flip and opacity shared by every character clip kind. Flip is
/// encoded as the sign of the scale so the PropertiesPanel flip checkboxes
/// round-trip cleanly. The compositor applies the 0.85× COMPOSE
/// character-fit dynamically (srcH→outH normalization / the GPU spine
/// FBO's native fit), so the preset scale is stored as-is.
void placeCharacter(Clip& clip, const CharacterState& ch)
{
    clip.positionX().setDefaultValue((ch.posX - 0.5f) * kLayoutW);
    clip.positionY().setDefaultValue((ch.posY - 0.5f) * kLayoutH);
    clip.scaleX().setDefaultValue(ch.flipX ? -ch.scale : ch.scale);
    clip.scaleY().setDefaultValue(ch.flipY ? -ch.scale : ch.scale);
    clip.opacity().setDefaultValue(ch.opacity);
}

/// Null when the character has no video for its current state.
std::unique_ptr<Clip> makeVideoCharacterClip(const CharacterState& ch, int index,
                                             const LayerPlacement& at)
{
    const std::string& videoPath = ch.activeVideoPath();
    if (videoPath.empty()) return nullptr;
    auto vc = std::make_unique<VideoClip>();
    vc->setMediaPath(videoPath);
    place(*vc, at, characterLabel(ch), "char_" + std::to_string(index));
    // Character metadata for Properties panel controls
    vc->setCharacterName(ch.characterName);
    vc->setTalking(ch.isTalking);
    vc->setVideoMutePath(ch.videoMutePath);
    vc->setVideoTalkPath(ch.videoTalkPath);
    placeCharacter(*vc, ch);
    if (ch.cropLeft > 0 || ch.cropRight > 0 || ch.cropTop > 0 || ch.cropBottom > 0)
        vc->setCrop(ch.cropLeft, ch.cropRight, ch.cropTop, ch.cropBottom);
    addBlur(*vc, ch.blur);
    return vc;
}

/// Custom (PNG-puppet) character → PngPuppetClip. Mirrors the puppet branch
/// in AudioSyncExport / the timeline drop handler. Without it, custom
/// characters like Wells fell through to the Spine branch and rendered
/// nothing (there is no Spine model behind them). Null when the puppet
/// can't be loaded.
std::unique_ptr<Clip> makePuppetClip(const CharacterState& ch, int index,
                                     const LayerPlacement& at)
{
    PuppetManifest man;
    if (!puppetlib::load(QString::fromStdString(ch.puppetFolder), man)) {
        spdlog::warn("applyShotSwitch: cannot load puppet manifest '{}'",
                     ch.puppetFolder);
        return nullptr;
    }
    std::string variant = ch.puppetVariant.empty() ? std::string("default")
                                                   : ch.puppetVariant;
    auto vit = man.variants.find(QString::fromStdString(variant));
    if (vit == man.variants.end() && !man.variantOrder.isEmpty()) {
        variant = man.variantOrder.first().toStdString();
        vit = man.variants.find(man.variantOrder.first());
    }
    if (vit == man.variants.end()) return nullptr;

    auto pc = std::make_unique<PngPuppetClip>(ch.characterName, variant);
    for (int f = 0; f < puppetlib::kFaceCount; ++f)
        pc->setFacePath(f, vit->faces[static_cast<size_t>(f)].toStdString());
    place(*pc, at, ch.characterName, "char_" + std::to_string(index));
    pc->setTalking(ch.isTalking);
    // Seed from the puppet folder (not the clip id) so repeated clips of
    // the same character stay in motion phase across cuts.
    pc->setSeed(static_cast<uint32_t>(
        std::hash<std::string>{}(ch.puppetFolder) & 0xFFFFFFFFu));
    placeCharacter(*pc, ch);
    // (PngPuppetClip has no crop; crop is a video/spine-only feature.)
    return pc;
}

std::unique_ptr<Clip> makeSpineClip(const CharacterState& ch, int index,
                                    const LayerPlacement& at)
{
    auto sc = std::make_unique<SpineClip>();
    sc->setCharacterName(ch.characterName);
    sc->setOutfit(ch.outfit);
    sc->setStance(ch.stance);
    sc->setAnimationName(ch.animation);
    sc->setTalking(ch.isTalking);
    place(*sc, at, characterLabel(ch), "char_" + std::to_string(index));
    placeCharacter(*sc, ch);
    if (ch.cropLeft > 0 || ch.cropRight > 0 || ch.cropTop > 0 || ch.cropBottom > 0)
        sc->setCrop(ch.cropLeft, ch.cropRight, ch.cropTop, ch.cropBottom);
    addBlur(*sc, ch.blur);
    return sc;
}

/// One clip per visible layer, mapped to tracks by position:
///   layerOrder[0]    = FRONT (top of UI → lowest index → VN, top)
///   layerOrder[last] = BACK  (bottom of UI → highest index → V1, bottom)
/// Layers are walked back-to-front so layerIdx matches the plan's order.
Snapshots buildNewClips(const ShotPreset& preset, const TrackPlan& plan,
                        const LayerPlacement& at)
{
    Snapshots newClips;
    const auto& order = preset.layerOrder();
    int layerIdx = 0;
    for (int oi = static_cast<int>(order.size()) - 1; oi >= 0; --oi) {
        const auto& ref = order[static_cast<size_t>(oi)];

        // Target track is whatever was assigned for this layerIdx.
        size_t targetTrack = (static_cast<size_t>(layerIdx) < plan.layerTracks.size())
            ? plan.layerTracks[static_cast<size_t>(layerIdx)]
            : 0;

        if (ref.type == LayerType::Background) {
            const auto* bg = preset.background(ref.index);
            if (!bg || !bg->visible) continue;   // not counted in the plan
            newClips.push_back({targetTrack, makeBackgroundClip(*bg, ref.index, at)});
        } else { // Character
            const auto* ch = preset.character(ref.index);
            if (!ch || !ch->visible) continue;   // not counted in the plan
            std::unique_ptr<Clip> clip;
            if (ch->isVideoCharacter())
                clip = makeVideoCharacterClip(*ch, ref.index, at);
            else if (ch->isPuppet())
                clip = makePuppetClip(*ch, ref.index, at);
            else
                clip = makeSpineClip(*ch, ref.index, at);
            // A layer that yields no clip still holds its planned track.
            if (clip) newClips.push_back({targetTrack, std::move(clip)});
        }
        ++layerIdx;
    }
    return newClips;
}

void removeGroupVisualClips(Timeline& timeline, uint64_t groupId)
{
    for (size_t ti = 0; ti < timeline.trackCount(); ++ti) {
        Track* trk = timeline.track(ti);
        if (!trk) continue;
        for (int ci = static_cast<int>(trk->clipCount()) - 1; ci >= 0; --ci) {
            Clip* c = trk->clip(static_cast<size_t>(ci));
            if (c && c->groupId() == groupId && c->isVisual())
                trk->removeClip(static_cast<size_t>(ci),
                                TrackMutationPolicy::BypassLock);
        }
    }
}

void insertSnapshots(Timeline& timeline, uint64_t groupId, const Snapshots& source,
                     const std::string& shotNameToPropagate)
{
    for (const auto& snap : source) {
        if (!snap.clip) continue;
        Track* trk = nullptr;
        size_t idx = snap.trackIndex;
        if (idx < timeline.trackCount()) {
            trk = timeline.track(idx);
            if (trk && (trk->type() != TrackType::Video || trk->isLocked()))
                trk = nullptr;
        }
        if (!trk) {
            // Fallback: nearest video track upward, then downward.
            for (size_t si = idx; si < timeline.trackCount(); ++si) {
                Track* t = timeline.track(si);
                if (t && t->type() == TrackType::Video && !t->isLocked()) {
                    trk = t;
                    break;
                }
            }
            if (!trk) {
                for (size_t si = idx; si > 0; --si) {
                    Track* t = timeline.track(si - 1);
                    if (t && t->type() == TrackType::Video && !t->isLocked()) {
                        trk = t;
                        break;
                    }
                }
            }
            if (!trk) trk = timeline.addVideoTrack("");
        }
        if (trk) {
            // Preserve the snapshot's clip id on the live re-inserted
            // clone. snap.clip already holds the canonical id (set at
            // capture time for old clips, the original creation id for
            // new shot-layer clips) — without restoring it here, every
            // redo/undo would mint a new global id and stale undo
            // commands referencing the prior id would silently no-op.
            auto reClone = snap.clip->clone();
            reClone->setId(snap.clip->id());
            trk->addClip(std::move(reClone),
                         TrackMutationPolicy::BypassLock);
        }
    }
    // Propagate the shot name to remaining group members (audio).
    if (!shotNameToPropagate.empty()) {
        for (size_t ti = 0; ti < timeline.trackCount(); ++ti) {
            Track* trk = timeline.track(ti);
            if (!trk) continue;
            for (size_t ci = 0; ci < trk->clipCount(); ++ci) {
                Clip* c = trk->clip(ci);
                if (c && c->groupId() == groupId)
                    c->setShotName(shotNameToPropagate);
            }
        }
    }
}

/// Insert the planned tracks ABOVE the existing top-of-group, then append
/// the planned tracks at the bottom of the video stack.
void applyTrackPlan(Timeline& timeline, const TrackPlan& plan)
{
    for (size_t e = 0; e < plan.inserts; ++e) {
        auto t = std::make_unique<Track>(TrackType::Video, std::string{});
        t->setHeight(plan.insertHeight);
        timeline.insertTrack(plan.insertAt, std::move(t));
    }
    for (size_t e = 0; e < plan.appends; ++e)
        (void)timeline.addVideoTrack("");
}

void revertTrackPlan(Timeline& timeline, const TrackPlan& plan)
{
    // The inserted tracks occupy [insertAt .. insertAt+inserts-1]; remove
    // from the highest index down so each takeTrack uses an index that's
    // still valid.
    for (size_t e = 0; e < plan.inserts; ++e) {
        size_t removeAt = plan.insertAt + (plan.inserts - 1 - e);
        if (removeAt < timeline.trackCount())
            (void)timeline.takeTrack(removeAt);
    }
    // The appended tracks should be the last `appends` (now empty) video
    // tracks. Walk from the end.
    for (size_t e = 0; e < plan.appends; ++e) {
        for (size_t i = timeline.trackCount(); i-- > 0; ) {
            Track* t = timeline.track(i);
            if (t && t->type() == TrackType::Video && t->clipCount() == 0) {
                (void)timeline.takeTrack(i);
                break;
            }
        }
    }
}

Clip* firstVisualMember(Timeline& timeline, uint64_t groupId, Track*& track)
{
    for (size_t ti = 0; ti < timeline.trackCount(); ++ti) {
        Track* trk = timeline.track(ti);
        if (!trk) continue;
        for (size_t ci = 0; ci < trk->clipCount(); ++ci) {
            Clip* c = trk->clip(ci);
            if (c && c->groupId() == groupId && c->isVisual()) {
                track = trk;
                return c;
            }
        }
    }
    track = nullptr;
    return nullptr;
}

} // namespace shot_switch_detail

void ShotSwitchController::apply(uint64_t groupId, const std::string& newShotName)
{
    using namespace shot_switch_detail;

    if (!timeline() || !m_cfg.presets() || groupId == 0) {
        m_cfg.refreshViews();
        return;
    }
    if (groupHasLockedMember(*timeline(), groupId)) return;

    auto presetOpt = m_cfg.presets()->load(newShotName);
    if (!presetOpt) {
        spdlog::warn("TimelineWorkspace: shot preset '{}' not found", newShotName);
        m_cfg.refreshViews();
        return;
    }
    const auto& preset = *presetOpt;
    spdlog::info("TimelineWorkspace: applying shot '{}' to group {}", newShotName, groupId);

    const GroupSpan span = measureGroup(*timeline(), groupId);
    auto old = std::make_shared<GroupSnapshot>(snapshotGroup(*timeline(), groupId));
    const auto plan = std::make_shared<const TrackPlan>(
        planTracks(*timeline(), span, visibleLayerCount(preset)));
    auto newClips = std::make_shared<Snapshots>(buildNewClips(
        preset, *plan, LayerPlacement{span.start, span.duration(), groupId, &newShotName}));
    // Propagate the AudioSync back-link onto every replacement clip.
    for (auto& snap : *newClips)
        if (snap.clip) snap.clip->setSyncLine(old->syncLine);

    // Panels must let go of clips before they are freed; afterwards the
    // workspace refreshes and reselects the group's first visual clip.
    // Both act on the timeline current at execute time.
    auto finish = [this, groupId]() {
        Track* pickedTrack = nullptr;
        Clip* picked = firstVisualMember(*timeline(), groupId, pickedTrack);
        m_cfg.shotSwitched(picked, pickedTrack);
    };
    auto redo = [this, groupId, newClips, newShotName, plan, finish]() {
        m_cfg.releaseClipPanels();
        removeGroupVisualClips(*timeline(), groupId);
        applyTrackPlan(*timeline(), *plan);
        insertSnapshots(*timeline(), groupId, *newClips, newShotName);
        finish();
    };
    auto undo = [this, groupId, old, plan, finish]() {
        m_cfg.releaseClipPanels();
        removeGroupVisualClips(*timeline(), groupId);
        revertTrackPlan(*timeline(), *plan);
        insertSnapshots(*timeline(), groupId, old->clips, old->shotName);
        finish();
    };

    if (CommandStack* stack = m_cfg.commandStack()) {
        auto cmd = std::make_unique<LambdaCommand>(
            "Switch Shot to " + newShotName, redo, undo);
        cmd->execute();
        stack->pushWithoutExecute(std::move(cmd));
    } else {
        // No command stack: run the redo body inline.
        redo();
    }

    spdlog::info("TimelineWorkspace: shot switch complete");
}

} // namespace rt
