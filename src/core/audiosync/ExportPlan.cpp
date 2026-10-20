/*
 * ExportPlan.cpp -- see ExportPlan.h. Moved from AudioSyncExport.cpp (and
 * AudioSync::getEffectiveRanges) with the panel's state turned into
 * parameters; the logic is unchanged.
 */

#include "audiosync/ExportPlan.h"

#include "Constants.h"
#include "PathUtils.h"
#include "ai/ScriptMatcher.h"
#include "timeline/AudioClip.h"
#include "timeline/Clip.h"
#include "timeline/Timeline.h"
#include "timeline/Track.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <map>
#include <memory>
#include <unordered_map>
#include <unordered_set>

namespace rt::audiosync {

std::string fileNameOf(const std::string& path)
{
    if (path.empty()) return {};
    try {
        return pathToUtf8(utf8ToPath(path).filename());
    } catch (...) {
        auto p = path.find_last_of("/\\");
        return p == std::string::npos ? path : path.substr(p + 1);
    }
}

std::vector<std::pair<double, double>> effectiveRanges(const SyncClip& clip)
{
    if (clip.deletedRegions.empty())
        return {{clip.start, clip.end}};

    // Sort deleted regions by start time
    auto deleted = clip.deletedRegions;
    std::sort(deleted.begin(), deleted.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });

    std::vector<std::pair<double,double>> ranges;
    double currentStart = clip.start;

    for (const auto& [delStart, delEnd] : deleted) {
        double ds = std::max(delStart, clip.start);
        double de = std::min(delEnd, clip.end);

        // Add range before deleted region (at least 50ms)
        if (currentStart < ds - 0.05)
            ranges.emplace_back(currentStart, ds);

        currentStart = de;
    }

    // Final range after all deleted regions
    if (currentStart < clip.end - 0.05)
        ranges.emplace_back(currentStart, clip.end);

    return ranges;
}

std::vector<const SyncClip*> exportedClips(const Script* script, const std::vector<SyncClip>& clips)
{
    // ── EXPORT == MATCH tab, exactly ────────────────────────────────────
    // The MATCH tab shows (and plays) exactly ONE clip per script line: its
    // "primary" — the best CONFIRMED clip on that line. The export mirrors
    // that precisely by walking the script lines IN ORDER and placing each
    // line's primary confirmed clip. This guarantees the timeline matches the
    // MATCH tab in both order AND content — no duplicates, no cross-character
    // strays, no reordering — and needs no re-sync.
    //
    // Primary selection: same rule as populateCards() — highest confidence
    // among confirmed clips (all matchState==2 here, so confidence is the
    // sole differentiator).  Character-name matching is deliberately not
    // used because the MATCH tab doesn't either; manual confirmation
    // already guarantees the right clip is assigned to each line.
    std::vector<const SyncClip*> confirmed;
    if (script) {
        // Confirmed clips per line, in clips order.
        std::unordered_map<int, std::vector<size_t>> confirmedByLine;
        for (size_t i = 0; i < clips.size(); ++i) {
            const auto& c = clips[i];
            if (c.scriptLineNumber >= 0 && c.matchState == 2)
                confirmedByLine[c.scriptLineNumber].push_back(i);
        }
        for (const auto& line : script->lines) {
            auto it = confirmedByLine.find(line.lineNumber);
            if (it == confirmedByLine.end()) continue;
            // Primary: pick the clip with the highest confidence, matching
            // the MATCH tab's populateCards primary-selection rule exactly.
            const SyncClip* best = nullptr;
            float bestConf = -1.0f;
            for (size_t ci : it->second) {
                const auto& c = clips[ci];
                if (!best || c.confidence > bestConf) {
                    best = &c;
                    bestConf = c.confidence;
                }
            }
            if (best) confirmed.push_back(best);   // already in script order
        }
    } else {
        // No script loaded — fall back to every confirmed clip in clips order.
        for (const auto& c : clips)
            if (c.scriptLineNumber >= 0 && c.matchState == 2)
                confirmed.push_back(&c);
    }
    return confirmed;
}

ExportPlan planExport(const Script* script, const std::vector<const SyncClip*>& confirmed)
{
    // ── Build a lookup from lineNumber → ScriptLine character ───────────
    std::unordered_map<int, std::string> lineCharacterMap;
    if (script) {
        for (const auto& line : script->lines)
            lineCharacterMap[line.lineNumber] = line.character;
    }

    // ── Build the plan (pure — no timeline mutation) ────────────────────
    // Audio clips back-to-back from tick 0; consecutive same-character
    // lines form one visual group spanning their combined range.
    ExportPlan plan;
    std::vector<PlannedAudio>& plannedAudio = plan.audio;
    std::vector<PlannedGroup>& groups = plan.groups;
    {
        int64_t currentPos = 0; // ticks
        std::string lastCharacter;
        for (const SyncClip* sc : confirmed) {
            auto ranges = effectiveRanges(*sc);

            // Determine character
            std::string character;
            auto it = lineCharacterMap.find(sc->scriptLineNumber);
            if (it != lineCharacterMap.end() && !it->second.empty())
                character = it->second;
            else
                character = sc->character;

            // Start a new group if character changes or first clip
            if (character != lastCharacter || groups.empty()) {
                PlannedGroup g;
                g.character     = character;
                g.firstLine     = sc->scriptLineNumber;
                g.timelineStart = currentPos;
                groups.push_back(std::move(g));
                lastCharacter = character;
            }

            // Label: use the source audio file's basename so the timeline
            // shows the actual asset name instead of the character/shot.
            std::string label = fileNameOf(sc->sourceFile);
            if (label.empty()) label = character;

            for (const auto& [rangeStart, rangeEnd] : ranges) {
                double rangeDuration = rangeEnd - rangeStart;
                if (rangeDuration <= 0.0) continue;

                PlannedAudio pa;
                pa.line           = sc->scriptLineNumber;
                pa.character      = character;
                pa.sourceFile     = sc->sourceFile;
                pa.label          = label;
                pa.timelineIn     = currentPos;
                pa.duration       = secondsToTicks(rangeDuration);
                pa.sourceIn       = secondsToTicks(rangeStart);
                pa.sourceDuration = secondsToTicks(sc->end);
                plannedAudio.push_back(std::move(pa));

                groups.back().totalDuration += secondsToTicks(rangeDuration);
                currentPos += secondsToTicks(rangeDuration);
            }
        }
    }
    return plan;
}

bool isReexport(const Timeline& timeline)
{
    // Any clip carrying a syncLine back-link means this sequence was
    // exported by a v29+ build before → update it in place instead of
    // wiping.  Legacy timelines (pre-v29 exports) have no back-links and
    // take the full path once; from then on every clip is tagged.
    for (size_t ti = 0; ti < timeline.trackCount(); ++ti) {
        const Track* t = timeline.track(ti);
        if (!t) continue;
        for (size_t ci = 0; ci < t->clipCount(); ++ci) {
            const Clip* c = t->clip(ci);
            if (c && c->syncLine() >= 0) return true;
        }
    }
    return false;
}

std::optional<LockedTrack> lockedTrackInTheWay(const Timeline& timeline, const ExportPlan& plan,
                                               bool incremental)
{
    // Preflight every track the operation owns before making any changes.
    // AudioSync is a compound timeline edit, so partially updating around a
    // locked track would create duplicated dialogue or mismatched shots.
    if (incremental) {
        for (size_t ti = 0; ti < timeline.trackCount(); ++ti) {
            const Track* track = timeline.track(ti);
            if (!track || !track->isLocked()) continue;
            for (size_t ci = 0; ci < track->clipCount(); ++ci) {
                const Clip* clip = track->clip(ci);
                if (clip && clip->syncLine() >= 0)
                    return LockedTrack{track->name(), clip->label(), clip->syncLine()};
            }
        }
    } else {
        std::unordered_set<std::string> speakerNames;
        for (const auto& pa : plan.audio)
            speakerNames.insert(pa.character.empty() ? std::string("VO")
                                                      : pa.character);
        for (size_t ti = 0; ti < timeline.trackCount(); ++ti) {
            const Track* track = timeline.track(ti);
            if (!track || !track->isLocked()) continue;
            bool wouldMutate =
                track->type() == TrackType::Audio
                && !track->isDivider()
                && speakerNames.count(track->name()) != 0;
            if (track->type() == TrackType::Video && !track->isCaptionTrack()) {
                for (size_t ci = 0; ci < track->clipCount() && !wouldMutate; ++ci) {
                    const Clip* clip = track->clip(ci);
                    wouldMutate = clip && (clip->syncLine() >= 0
                        || (clip->groupId() != 0 && !clip->shotName().empty()));
                }
            }
            if (wouldMutate)
                return LockedTrack{track->name(), {}, -1};
        }
    }
    return std::nullopt;
}

namespace {

using TrackForSpeakerFn = std::function<Track*(const std::string&)>;

// Build a fresh AudioClip from a planned entry (tagged with its line).
std::unique_ptr<AudioClip> makeAudioClip(const PlannedAudio& pa)
{
    auto audioClip = std::make_unique<AudioClip>(pa.sourceFile);
    audioClip->setTimelineIn(pa.timelineIn);
    audioClip->setDuration(pa.duration);
    audioClip->setSourceIn(pa.sourceIn);
    audioClip->setSourceDuration(pa.sourceDuration);
    audioClip->setLabel(pa.label);
    audioClip->setSyncLine(pa.line);
    return audioClip;
}

// ── FULL export: clear previous export-owned content, rebuild from plan ────

int applyFull(Timeline* timeline,
              const std::vector<PlannedAudio>& plannedAudio,
              const std::vector<PlannedGroup>& groups,
              bool hasShotMgr,
              const TrackForSpeakerFn& trackForSpeaker,
              const PlaceGroupVisualsFn& placeGroupVisuals)
{
    // Remove old speaker audio tracks from previous exports (clean
    // re-export).  A track is removed when it is empty or when its name
    // matches a speaker in this export (legacy exports only ever created
    // speaker-named tracks) — user-added audio tracks with their own
    // content (music, SFX) survive.
    {
        std::unordered_set<std::string> speakerNames;
        for (const auto& pa : plannedAudio)
            speakerNames.insert(pa.character.empty() ? std::string("VO")
                                                     : pa.character);
        std::vector<size_t> removeIndices;
        for (size_t i = 0; i < timeline->trackCount(); ++i) {
            Track* t = timeline->track(i);
            if (!t || t->type() != TrackType::Audio || t->isDivider()) continue;
            if (t->isLocked()) continue;
            if (t->clipCount() == 0 || speakerNames.count(t->name()))
                removeIndices.push_back(i);
            else
                spdlog::info("AudioSync: keeping non-speaker audio track '{}' "
                             "({} clips)", t->name(), t->clipCount());
        }
        // Remove from end to preserve indices
        for (auto it = removeIndices.rbegin(); it != removeIndices.rend(); ++it)
            timeline->removeTrack(*it);
        spdlog::info("AudioSync: cleared {} old audio tracks", removeIndices.size());
    }

    // Place audio clips
    int exportCount = 0;
    for (const auto& pa : plannedAudio) {
        trackForSpeaker(pa.character)->addClip(makeAudioClip(pa));
        ++exportCount;
    }

    spdlog::info("AudioSync::exportToTimeline placed {} audio clips", exportCount);

    // -- DEFAULT SHOT PLACEMENT -- create SpineClip/VideoClip layers ----
    if (hasShotMgr) {
        // Clear existing export-owned visual clips (clean re-export).
        // Guards: the caption track is NEVER touched (a previous version
        // wiped subtitles here), and standalone user clips (no shot group,
        // no back-link) survive — only shot-group content is rebuilt.
        for (size_t i = 0; i < timeline->trackCount(); ++i) {
            Track* t = timeline->track(i);
            if (!t || t->type() != TrackType::Video) continue;
            if (t->isCaptionTrack() || t->isLocked()) continue;
            for (size_t ci = t->clipCount(); ci > 0; --ci) {
                const Clip* c = t->clip(ci - 1);
                if (!c) continue;
                const bool exportOwned =
                    c->syncLine() >= 0
                    || (c->groupId() != 0 && !c->shotName().empty());
                if (exportOwned)
                    t->removeClip(ci - 1);
            }
        }
        spdlog::info("AudioSync::exportToTimeline cleared old visual clips from video tracks");

        for (const auto& group : groups)
            placeGroupVisuals(group);
    } else {
        spdlog::warn("AudioSync::exportToTimeline: no ShotPresetManager set, "
                     "skipping default shot placement");
    }

    return exportCount;
}

// ── INCREMENTAL export: diff plan vs back-linked clips, update in place ────

int applyIncremental(Timeline* timeline,
                     const std::vector<PlannedAudio>& plannedAudio,
                     const std::vector<PlannedGroup>& groups,
                     const TrackForSpeakerFn& trackForSpeaker,
                     const PlaceGroupVisualsFn& placeGroupVisuals)
{
    struct ExistingRef {
        Track* track;
        Clip*  clip;
    };

    // ── Index existing export-owned clips by back-link ──────────────────
    std::map<int, std::vector<ExistingRef>> exAudio;   // line → audio clips
    std::map<int, std::vector<ExistingRef>> exVisual;  // group firstLine → visuals
    // Export-time speaker of each line = name of the audio track its clip
    // sits on.  Captured BEFORE the audio pass so the visual pass can tell
    // whether a group's character changed (→ rebuild from default shot).
    std::unordered_map<int, std::string> exLineSpeaker;

    for (size_t ti = 0; ti < timeline->trackCount(); ++ti) {
        Track* t = timeline->track(ti);
        if (!t || t->isDivider() || t->isCaptionTrack()) continue;
        for (size_t ci = 0; ci < t->clipCount(); ++ci) {
            Clip* c = t->clip(ci);
            if (!c || c->syncLine() < 0) continue;
            if (t->type() == TrackType::Audio
                && c->clipType() == ClipType::Audio) {
                exAudio[c->syncLine()].push_back({t, c});
                exLineSpeaker.emplace(c->syncLine(), t->name());
            } else if (t->type() == TrackType::Video && c->isVisual()
                       && !c->isCaption()) {
                exVisual[c->syncLine()].push_back({t, c});
            }
        }
    }

    // Audio tracks that lose sync clips may end up empty → removed at the
    // end (user tracks never appear here: their clips carry no back-link).
    std::unordered_set<Track*> touchedAudioTracks;

    auto removeExisting = [&](const ExistingRef& ref) {
        touchedAudioTracks.insert(ref.track);
        ref.track->removeClipById(ref.clip->id());  // frees the clip
    };

    // ── Audio pass ──────────────────────────────────────────────────────
    std::map<int, std::vector<const PlannedAudio*>> planByLine;
    for (const auto& pa : plannedAudio)
        planByLine[pa.line].push_back(&pa);

    int updated = 0, unchanged = 0, created = 0, removed = 0;

    for (const auto& [line, planned] : planByLine) {
        auto exIt = exAudio.find(line);
        std::vector<ExistingRef> existing =
            (exIt != exAudio.end()) ? exIt->second : std::vector<ExistingRef>{};
        std::sort(existing.begin(), existing.end(),
                  [](const ExistingRef& a, const ExistingRef& b) {
                      return a.clip->timelineIn() < b.clip->timelineIn();
                  });

        if (existing.size() == planned.size()) {
            // Update each clip in place — identity (id, fades, audio FX,
            // volume keyframes) survives; AudioSync wins on track, range
            // and position.
            for (size_t k = 0; k < planned.size(); ++k) {
                const PlannedAudio& pa = *planned[k];
                auto* ac = static_cast<AudioClip*>(existing[k].clip);
                Track* cur    = existing[k].track;
                Track* wanted = trackForSpeaker(pa.character);

                const bool fieldsChanged =
                       ac->mediaPath()      != pa.sourceFile
                    || ac->sourceIn()       != pa.sourceIn
                    || ac->duration()       != pa.duration
                    || ac->sourceDuration() != pa.sourceDuration
                    || ac->timelineIn()     != pa.timelineIn
                    || cur != wanted;
                if (!fieldsChanged) { ++unchanged; continue; }

                ac->setMediaPath(pa.sourceFile);
                ac->setLabel(pa.label);
                ac->setSourceIn(pa.sourceIn);
                ac->setSourceDuration(pa.sourceDuration);
                ac->setDuration(pa.duration);
                if (cur != wanted) {
                    touchedAudioTracks.insert(cur);
                    auto owned = cur->removeClipById(ac->id());
                    owned->setTimelineIn(pa.timelineIn);
                    wanted->addClip(std::move(owned));
                } else if (ac->timelineIn() != pa.timelineIn) {
                    cur->moveClip(cur->findClipIndexById(ac->id()),
                                  pa.timelineIn);
                }
                ++updated;
            }
        } else {
            // Range count changed (re-segmented) — recreate this line.
            for (const auto& ref : existing) { removeExisting(ref); ++removed; }
            for (const PlannedAudio* pa : planned) {
                trackForSpeaker(pa->character)->addClip(makeAudioClip(*pa));
                ++created;
            }
        }
    }

    // Lines that vanished from the plan (unconfirmed / removed from script)
    for (const auto& [line, refs] : exAudio) {
        if (planByLine.count(line)) continue;
        for (const auto& ref : refs) { removeExisting(ref); ++removed; }
    }

    // ── Visual pass ─────────────────────────────────────────────────────
    int vReused = 0, vRebuilt = 0, vRemoved = 0;
    std::unordered_set<int> plannedFirstLines;

    for (const auto& group : groups) {
        plannedFirstLines.insert(group.firstLine);

        auto exIt = exVisual.find(group.firstLine);
        std::vector<ExistingRef> existing =
            (exIt != exVisual.end()) ? exIt->second : std::vector<ExistingRef>{};

        // Same group (same first line) AND same speaker as at export time →
        // keep the clips (shot swaps, transforms, effects, transitions all
        // survive) and just re-span them.  Speaker unknown (no audio clip
        // was found for the line) counts as unchanged.
        auto spIt = exLineSpeaker.find(group.firstLine);
        const bool sameCharacter =
            (spIt == exLineSpeaker.end()) || (spIt->second == group.character);

        if (!existing.empty() && sameCharacter) {
            for (const auto& ref : existing) {
                if (ref.clip->duration() != group.totalDuration)
                    ref.clip->setDuration(group.totalDuration);
                if (ref.clip->timelineIn() != group.timelineStart)
                    ref.track->moveClip(
                        ref.track->findClipIndexById(ref.clip->id()),
                        group.timelineStart);
            }
            ++vReused;
        } else {
            for (const auto& ref : existing)
                ref.track->removeClipById(ref.clip->id());
            if (!existing.empty()) ++vRemoved;
            if (placeGroupVisuals(group) > 0) ++vRebuilt;
        }
    }

    // Visual groups that vanished from the plan
    for (const auto& [firstLine, refs] : exVisual) {
        if (plannedFirstLines.count(firstLine)) continue;
        for (const auto& ref : refs)
            ref.track->removeClipById(ref.clip->id());
        ++vRemoved;
    }

    // ── Drop speaker tracks left empty by removals ──────────────────────
    for (size_t i = timeline->trackCount(); i > 0; --i) {
        Track* t = timeline->track(i - 1);
        if (t && t->type() == TrackType::Audio && !t->isDivider()
            && t->clipCount() == 0 && touchedAudioTracks.count(t))
            timeline->removeTrack(i - 1);
    }

    spdlog::info("AudioSync incremental re-export: audio {} updated / {} "
                 "unchanged / {} created / {} removed; visuals {} groups "
                 "kept / {} rebuilt / {} removed",
                 updated, unchanged, created, removed,
                 vReused, vRebuilt, vRemoved);

    return updated + unchanged + created;
}

} // namespace

int applyExport(Timeline& target, const ExportPlan& plan, bool incremental, bool shots,
                const PlaceGroupVisualsFn& placeGroupVisuals)
{
    Timeline* timeline = &target;

    // Find-or-create the per-speaker audio track for a character.  The
    // full path wipes previous speaker tracks first, so "find" only kicks
    // in for the incremental path (and for repeated speakers either way).
    std::unordered_map<std::string, Track*> speakerTracks;
    auto trackForSpeaker = [&](const std::string& character) -> Track* {
        std::string name = character.empty() ? std::string("VO") : character;
        auto it = speakerTracks.find(name);
        if (it != speakerTracks.end()) return it->second;
        for (size_t i = 0; i < timeline->trackCount(); ++i) {
            Track* t = timeline->track(i);
            if (t && t->type() == TrackType::Audio && !t->isDivider()
                && !t->isLocked() && t->name() == name) {
                speakerTracks.emplace(name, t);
                return t;
            }
        }
        Track* t = timeline->addAudioTrack(name);
        // Set new track to minimum height so audio tracks don't "expand"
        // to the default 80px after export — they stay compact.
        t->setHeight(30.0f);
        speakerTracks.emplace(name, t);
        spdlog::info("AudioSync: created audio track '{}'", name);
        return t;
    };

    if (!incremental)
        return applyFull(timeline, plan.audio, plan.groups, shots,
                         trackForSpeaker, placeGroupVisuals);
    return applyIncremental(timeline, plan.audio, plan.groups,
                            trackForSpeaker, placeGroupVisuals);
}

} // namespace rt::audiosync
