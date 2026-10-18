/*
 * ProjectSerializer.cpp — saving and loading .rtp project files.
 *
 * Saving always writes the newest format: field-tagged records (v48+, see
 * ProjectSerializer.h and schema/Schema.h). Loading dispatches on the
 * file's version: records here, positional files (v1–47) through the frozen
 * ProjectSerializerLegacy.cpp. The load repairs both share live here too.
 */

#include "project/ProjectSerializer.h"
#include "project/ProjectSerializerInternal.h"
#include "project/BinaryIO.h"
#include "project/Project.h"
#include "project/Settings.h"
#include "project/AssetDatabase.h"
#include "project/schema/ClipSchemas.h"
#include "project/schema/LeafSchemas.h"
#include "project/schema/Schema.h"
#include "timeline/Timeline.h"
#include "timeline/Track.h"
#include "timeline/Clip.h"
#include "timeline/ImageClip.h"
#include "timeline/Marker.h"
#include "timeline/Transition.h"
#include "timeline/VideoClip.h"
#include "AppPaths.h"
#include "FrameTime.h"
#include "PathUtils.h"

#include <fstream>
#include <cstring>
#include <filesystem>
#include <algorithm>
#include <stdexcept>
#include <unordered_map>
#include <spdlog/spdlog.h>

namespace rt {

// ═══════════════════════════════════════════════════════════════════════════
// Load repairs (both formats)
// ═══════════════════════════════════════════════════════════════════════════

namespace project_detail {

// Re-link a track's transitions to its clips by timeline geometry.
//
// readClip() regenerates clip IDs on load (the global ID counter is process-
// wide and never reset), so the leftClipId/rightClipId stored in each
// transition no longer point at the reloaded clips. The render path looks
// up the two source clips by those IDs to blend them, so a stale ID makes
// the transition silently disappear. This also corrupts on resave (the new
// clip IDs get written while the transition still references the old ones),
// which is why transitions vanished after switching projects but came back
// on a fresh launch (where the counter happened to realign).
//
// The transition's editPointTick is an absolute timeline position that is
// always saved faithfully, so re-derive the linkage from it: the left clip
// is the one ending exactly at the edit point, the right clip the one
// starting exactly at it. Single-sided fades (leftClipId==0 / rightClipId==0)
// keep their zero side untouched. Only an exact, unambiguous boundary match
// relinks a side — if nothing lines up (e.g. an oddly re-anchored edit
// point) the saved ID is left as-is so we never mislink.
void relinkTransitionsByGeometry(Track& track)
{
    for (Transition& t : track.transitions()) {
        const bool wantLeft  = (t.leftClipId  != 0);
        const bool wantRight = (t.rightClipId != 0);
        if (!wantLeft && !wantRight)
            continue;

        uint64_t leftId = 0;   int leftHits  = 0;
        uint64_t rightId = 0;  int rightHits = 0;
        for (size_t i = 0; i < track.clipCount(); ++i) {
            const Clip* c = track.clip(i);
            if (!c) continue;
            if (wantLeft && c->timelineOut() == t.editPointTick) {
                leftId = c->id(); ++leftHits;
            }
            if (wantRight && c->timelineIn() == t.editPointTick) {
                rightId = c->id(); ++rightHits;
            }
        }
        if (wantLeft  && leftHits  == 1) t.leftClipId  = leftId;
        if (wantRight && rightHits == 1) t.rightClipId = rightId;
    }
}

// v32 and older saved every mask in normalized sequence-frame coordinates;
// v33 can explicitly carry the same legacy enum. Upgrade only when the source
// geometry is authoritative. In particular, pre-v34 VideoClip records have no
// display rotation, so guessing 0 here would permanently corrupt masks from
// portrait phone footage. Those video masks remain legacy (and therefore keep
// their saved appearance) until MediaPool supplies dimensions + rotation.
int migrateLegacyMasks(Project& project)
{
    int migrated = 0;
    for (size_t si = 0; si < project.sequenceCount(); ++si) {
        Timeline* timeline = project.sequence(si);
        if (!timeline) continue;
        const auto resolution = timeline->settings().resolution();
        if (resolution.width == 0 || resolution.height == 0) continue;

        for (size_t ti = 0; ti < timeline->trackCount(); ++ti) {
            Track* track = timeline->track(ti);
            if (!track) continue;
            for (size_t ci = 0; ci < track->clipCount(); ++ci) {
                Clip* clip = track->clip(ci);
                if (!clip) continue;

                uint32_t sourceWidth = 0;
                uint32_t sourceHeight = 0;
                int sourceRotation = 0;
                if (const auto* video = dynamic_cast<const VideoClip*>(clip)) {
                    if (!video->sourceMetadataAuthoritative() ||
                        video->sourceWidth() == 0 || video->sourceHeight() == 0)
                        continue;
                    sourceWidth = video->sourceWidth();
                    sourceHeight = video->sourceHeight();
                    sourceRotation = video->sourceRotation();
                } else if (const auto* image = dynamic_cast<const ImageClip*>(clip)) {
                    if (image->sourceWidth() == 0 || image->sourceHeight() == 0)
                        continue;
                    sourceWidth = image->sourceWidth();
                    sourceHeight = image->sourceHeight();
                } else if (clip->clipType() == ClipType::Graphic) {
                    // GraphicClip renders into the sequence-sized canvas.
                    sourceWidth = resolution.width;
                    sourceHeight = resolution.height;
                } else {
                    // Other procedural clip types do not yet expose a stable
                    // native-source contract. Preserve their legacy masks.
                    continue;
                }
                migrated += clip->migrateLegacyMasksToSourceLocal(
                    resolution.width, resolution.height,
                    sourceWidth, sourceHeight, sourceRotation);
            }
        }
    }
    return migrated;
}

// Pre-v46 files: clips placed before drag/export frame-snapping can sit on
// fractional-frame ticks, and a visual clip whose end overhangs a frame
// boundary renders one extra frame (the "ghost" of a character that should
// have left at a shot cut). Older builds also re-snapped every load onto a
// whole-tick 48000/fps grid (1601 at 29.97) that drifts off the canonical
// one by a frame every ~100 s. Snap visual clips once onto each sequence's
// canonical grid (FrameTime.h). Audio never needed frame alignment, so it
// only follows its linked video by the same shift to keep A/V sync; free
// audio keeps its exact position and length.
int alignLegacyClipsToFrameGrid(Project& project)
{
    int adjusted = 0;
    for (size_t si = 0; si < project.sequenceCount(); ++si) {
        Timeline* timeline = project.sequence(si);
        if (!timeline) continue;
        const RationalFrameRate grid = timeline->settings().frameGrid();
        std::unordered_map<uint64_t, int64_t> linkShift;

        for (size_t ti = 0; ti < timeline->trackCount(); ++ti) {
            Track* track = timeline->track(ti);
            if (!track) continue;
            for (size_t ci = 0; ci < track->clipCount(); ++ci) {
                Clip* clip = track->clip(ci);
                if (!clip || clip->isAudio()) continue;
                const int64_t inQ = snapTickToNearestFrame(clip->timelineIn(), grid);
                int64_t outQ = snapTickToNearestFrame(clip->timelineOut(), grid);
                if (outQ <= inQ)   // keep at least 1 frame
                    outQ = frameIndexToTick(tickToFrameIndex(inQ, grid) + 1, grid);
                if (clip->linkId() != 0)
                    linkShift.try_emplace(clip->linkId(), inQ - clip->timelineIn());
                if (inQ != clip->timelineIn() || outQ != clip->timelineOut()) {
                    clip->setTimelineIn(inQ);
                    clip->setDuration(outQ - inQ);
                    ++adjusted;
                }
            }
        }

        if (linkShift.empty()) continue;
        for (size_t ti = 0; ti < timeline->trackCount(); ++ti) {
            Track* track = timeline->track(ti);
            if (!track) continue;
            for (size_t ci = 0; ci < track->clipCount(); ++ci) {
                Clip* clip = track->clip(ci);
                if (!clip || !clip->isAudio() || clip->linkId() == 0) continue;
                const auto it = linkShift.find(clip->linkId());
                if (it == linkShift.end() || it->second == 0) continue;
                clip->setTimelineIn(std::max<int64_t>(0, clip->timelineIn() + it->second));
                ++adjusted;
            }
        }
    }
    return adjusted;
}

// Remove duplicate transitions left on a track.  Track::addTransition does a
// blind push_back with no dedup, so a double-add (or a project re-saved after
// one) leaves two+ identical transitions on the same edit point.  The
// compositor then applies the same fade twice — opacity becomes prog² and the
// fade reads as too dark / eased instead of linear.  An edit point is uniquely
// identified by its (leftClipId, rightClipId) endpoints, so any later
// transition sharing both endpoints with an earlier one is a duplicate.  Runs
// AFTER relinkTransitionsByGeometry so endpoints are final.  Fully-unlinked
// (0,0) entries are never merged — relink may simply have failed to resolve
// them and they could be genuinely distinct.
void dedupeTransitions(Track& track)
{
    auto& trans = track.transitions();
    std::vector<Transition> unique;
    unique.reserve(trans.size());
    for (const auto& t : trans) {
        if (t.leftClipId == 0 && t.rightClipId == 0) {
            unique.push_back(t);
            continue;
        }
        const bool dup = std::any_of(unique.begin(), unique.end(),
            [&](const Transition& u) {
                return u.leftClipId == t.leftClipId &&
                       u.rightClipId == t.rightClipId;
            });
        if (!dup)
            unique.push_back(t);
    }
    if (unique.size() != trans.size()) {
        spdlog::warn("ProjectSerializer: removed {} duplicate transition(s) on track '{}'",
                     trans.size() - unique.size(), track.name());
        trans = std::move(unique);
    }
}

} // namespace project_detail

using namespace project_detail;

// ═══════════════════════════════════════════════════════════════════════════
// v48 records: what each section holds. Field ids are append-only (see
// schema/Schema.h): add new fields at the end with the next id.
// ═══════════════════════════════════════════════════════════════════════════

/// The bin as one record (items + folders).
struct BinStateRecord
{
    std::vector<Project::BinItem>   items;
    std::vector<Project::BinFolder> folders;
};

} // namespace rt

namespace rt::record {

template <> struct SchemaOf<Project::BinItem> { static const Schema<Project::BinItem>& get(); };
template <> struct SchemaOf<Project::BinFolder> { static const Schema<Project::BinFolder>& get(); };
template <> struct SchemaOf<AssetEntry> { static const Schema<AssetEntry>& get(); };

#define RT_MEMBER(Type, id, name) \
    member<Type>(id, #name, [](Type& o) -> auto& { return o.name; })

const Schema<Project::BinItem>& SchemaOf<Project::BinItem>::get()
{
    using B = Project::BinItem;
    static const Schema<B> s("BinItem", {
        RT_MEMBER(B, 1, id),
        RT_MEMBER(B, 2, path),
        RT_MEMBER(B, 3, displayName),
        RT_MEMBER(B, 4, labelColor),
    });
    return s;
}

const Schema<Project::BinFolder>& SchemaOf<Project::BinFolder>::get()
{
    using F = Project::BinFolder;
    static const Schema<F> s("BinFolder", {
        RT_MEMBER(F, 1, name),
        RT_MEMBER(F, 2, expanded),
        RT_MEMBER(F, 3, childKeys),
    });
    return s;
}

const Schema<AssetEntry>& SchemaOf<AssetEntry>::get()
{
    static const Schema<AssetEntry> s("AssetEntry", {
        RT_MEMBER(AssetEntry, 1, id),
        RT_MEMBER(AssetEntry, 2, type),
        RT_MEMBER(AssetEntry, 3, name),
        RT_MEMBER(AssetEntry, 4, path),
        RT_MEMBER(AssetEntry, 5, absolutePath),
        RT_MEMBER(AssetEntry, 6, fileSize),
        RT_MEMBER(AssetEntry, 7, hash),
    });
    return s;
}

} // namespace rt::record

namespace rt {

namespace {

using record::RecordReader;
using record::RecordWriter;
using record::Schema;
using record::SchemaOf;
using record::custom;
using record::field;
using record::member;
using record::detail::kListElement;

/// Each element of the list record in the current field of `r`, as a
/// nested record. False when the field isn't a list; a malformed list
/// fails `r`.
template <class Fn>
bool forEachListRecord(RecordReader& r, Fn&& fn)
{
    RecordReader list;
    if (r.kind() != record::Kind::Bytes) return false;
    if (!r.readRecord(list)) { r.fail(); return true; }
    while (list.next()) {
        RecordReader item;
        if (list.id() != kListElement || !list.readRecord(item)) continue;
        if (!fn(item)) { list.fail(); break; }
    }
    if (!list.ok()) r.fail();
    return true;
}

/// The first field `id` of a record, read as V (the record is not consumed).
template <class V>
bool peekField(const RecordReader& r, uint32_t id, V& out)
{
    RecordReader scan = r;
    while (scan.next())
        if (scan.id() == id) return record::readValue(scan, out);
    return false;
}

const Schema<Track>& trackSchema()
{
    using T = Track;
    static const Schema<T> s("Track", {
        custom<T>(1, "type",
            [](RecordWriter& w, uint32_t id, const T& t) { record::writeValue(w, id, t.type()); },
            // Read first, by the sequence, to create the track.
            [](RecordReader&, T&) { return true; }),
        field<T>(2, "name", [](const T& t) { return t.name(); },
            [](T& t, std::string v) { t.setName(v); }),
        field<T>(3, "locked", [](const T& t) { return t.isLocked(); },
            [](T& t, bool v) { t.setLocked(v); }),
        field<T>(4, "muted", [](const T& t) { return t.isMuted(); },
            [](T& t, bool v) { t.setMuted(v); }),
        field<T>(5, "soloed", [](const T& t) { return t.isSoloed(); },
            [](T& t, bool v) { t.setSoloed(v); }),
        field<T>(6, "height", [](const T& t) { return t.height(); },
            [](T& t, float v) { t.setHeight(v); }),
        field<T>(7, "syncLocked", [](const T& t) { return t.isSyncLocked(); },
            [](T& t, bool v) { t.setSyncLocked(v); }),
        field<T>(8, "divider", [](const T& t) { return t.isDivider(); },
            [](T& t, bool v) { t.setDivider(v); }),
        field<T>(9, "permanentDivider", [](const T& t) { return t.isPermanentDivider(); },
            [](T& t, bool v) { t.setPermanentDivider(v); }),
        field<T>(10, "captionTrack", [](const T& t) { return t.isCaptionTrack(); },
            [](T& t, bool v) { t.setCaptionTrack(v); }),
        custom<T>(11, "clips",
            [](RecordWriter& w, uint32_t id, const T& t) {
                RecordWriter list = w.nested();
                for (size_t i = 0; i < t.clipCount(); ++i) {
                    RecordWriter n = list.nested();
                    record::writeClip(n, *t.clip(i));
                    list.writeRecord(kListElement, n);
                }
                w.writeRecord(id, list);
            },
            [](RecordReader& r, T& t) -> bool {
                return forEachListRecord(r, [&t](RecordReader& item) {
                    if (auto clip = record::readClip(item))
                        t.addClip(std::move(clip), TrackMutationPolicy::BypassLock);
                    return item.ok();
                });
            }),
        custom<T>(12, "transitions",
            [](RecordWriter& w, uint32_t id, const T& t) {
                record::writeValue(w, id, t.transitions());
            },
            [](RecordReader& r, T& t) -> bool {
                std::vector<Transition> transitions;
                if (!record::readValue(r, transitions)) return false;
                for (const Transition& tr : transitions)
                    t.addTransition(tr, TrackMutationPolicy::BypassLock);
                return true;
            }),
    });
    return s;
}

const Schema<Timeline>& sequenceSchema()
{
    using S = Timeline;
    static const Schema<S> s("Sequence", {
        field<S>(1, "name", [](const S& t) { return t.name(); },
            [](S& t, std::string v) { t.setName(v); }),
        field<S>(2, "playhead", [](const S& t) { return t.playheadPosition(); },
            [](S& t, int64_t v) { t.setPlayheadPosition(v); }),
        field<S>(3, "inPoint", [](const S& t) { return t.inPoint(); },
            [](S& t, int64_t v) { t.setInPoint(v); }),
        field<S>(4, "outPoint", [](const S& t) { return t.outPoint(); },
            [](S& t, int64_t v) { t.setOutPoint(v); }),
        field<S>(5, "settings", [](const S& t) -> const Settings& { return t.settings(); },
            [](S& t, Settings v) { t.setSettings(v); }),
        custom<S>(6, "tracks",
            [](RecordWriter& w, uint32_t id, const S& t) {
                RecordWriter list = w.nested();
                for (size_t i = 0; i < t.trackCount(); ++i) {
                    RecordWriter n = list.nested();
                    trackSchema().write(n, *t.track(i));
                    list.writeRecord(kListElement, n);
                }
                w.writeRecord(id, list);
            },
            [](RecordReader& r, S& t) -> bool {
                const bool ok = forEachListRecord(r, [&t](RecordReader& item) {
                    TrackType type = TrackType::Video;
                    if (!peekField(item, 1, type)) return item.ok();   // no type: left out
                    // Appended in saved order (addVideoTrack/addAudioTrack
                    // would re-sort and move user dividers).
                    Track* track = t.insertTrack(t.trackCount(), std::make_unique<Track>(type));
                    if (!trackSchema().read(item, *track)) return false;
                    relinkTransitionsByGeometry(*track);
                    dedupeTransitions(*track);
                    return true;
                });
                t.sortTracksByType();
                return ok;
            }),
        field<S>(7, "markers",
            [](const S& t) -> const std::vector<Marker>& { return t.markers(); },
            [](S& t, std::vector<Marker> markers) {
                while (!t.markers().empty()) t.removeMarker(0);
                for (const Marker& m : markers) t.addMarker(m.time, m.label, m.color);
            }),
    });
    return s;
}

const Schema<Project>& projectMetaSchema()
{
    using P = Project;
    static const Schema<P> s("ProjectMeta", {
        field<P>(1, "name", [](const P& p) { return p.name(); },
            [](P& p, std::string v) { p.setName(v); }),
        field<P>(2, "show", [](const P& p) { return p.show(); },
            [](P& p, std::string v) { p.setShow(v); }),
    });
    return s;
}

const Schema<BinStateRecord>& binStateSchema()
{
    static const Schema<BinStateRecord> s("BinState", {
        member<BinStateRecord>(1, "items", [](BinStateRecord& b) -> auto& { return b.items; }),
        member<BinStateRecord>(2, "folders", [](BinStateRecord& b) -> auto& { return b.folders; }),
    });
    return s;
}

// Fields of the Sequences, Assets and WorkspaceState records.
constexpr uint32_t kSequencesActive = 1;
constexpr uint32_t kSequencesList = 2;
constexpr uint32_t kAssetsList = 1;
constexpr uint32_t kWorkspaceOpenSequences = 1;

} // namespace

// ═══════════════════════════════════════════════════════════════════════════
// Save (always the newest format)
// ═══════════════════════════════════════════════════════════════════════════

std::vector<uint8_t> ProjectSerializer::serialize(const Project& project) const
{
    return serialize(project, nullptr);
}

std::vector<uint8_t> ProjectSerializer::serialize(const Project& project,
                                                  record::FieldLog* log) const
{
    BinaryWriter out;
    out.writeBytes(MAGIC, 8);
    out.writeU32(FORMAT_VERSION);
    const size_t sectionCountPos = out.size();
    out.writeU32(0);
    for (int i = 0; i < 16; ++i) out.writeU8(0);   // reserved

    uint32_t sectionCount = 0;
    auto section = [&](uint32_t tag, const std::vector<uint8_t>& data) {
        out.beginSection(tag, data);
        ++sectionCount;
    };

    // Settings and ProjectMeta first: readMetadata reads only the start.
    {
        RecordWriter w(log);
        SchemaOf<Settings>::get().write(w, project.defaultSettings());
        section(Section_Settings, w.data());
    }
    {
        RecordWriter w(log);
        projectMetaSchema().write(w, project);
        section(Section_ProjectMeta, w.data());
    }
    {
        RecordWriter w(log);
        w.writeVarint(kSequencesActive, project.activeSequenceIndex());
        RecordWriter list = w.nested();
        for (size_t i = 0; i < project.sequenceCount(); ++i) {
            RecordWriter n = list.nested();
            sequenceSchema().write(n, *project.sequence(i));
            list.writeRecord(kListElement, n);
        }
        w.writeRecord(kSequencesList, list);
        section(Section_Sequences, w.data());
    }
    {
        // By type, in the same order the positional writer used.
        std::vector<AssetEntry> assets;
        for (auto t : {AssetType::Character, AssetType::Background, AssetType::Audio,
                       AssetType::Video, AssetType::Image, AssetType::Font})
            for (const AssetEntry* a : project.assets()->findByType(t))
                assets.push_back(*a);
        RecordWriter w(log);
        record::writeValue(w, kAssetsList, assets);
        section(Section_Assets, w.data());
    }
    {
        // Rich bin items; a project that never filled them derives them from
        // its bin files, as the positional writer did.
        BinStateRecord bin;
        bin.items = project.binItems();
        if (bin.items.empty()) {
            uint64_t synthId = 1;
            for (const auto& f : project.binFiles())
                bin.items.push_back({synthId++, f, std::string{}, 0xFF888888u});
        }
        bin.folders = project.binFolders();
        RecordWriter w(log);
        binStateSchema().write(w, bin);
        section(Section_BinState, w.data());
    }
    if (!project.audioSyncBlob().empty())
        section(Section_AudioSync, project.audioSyncBlob());
    {
        std::vector<uint64_t> open(project.openSequenceIndices().begin(),
                                   project.openSequenceIndices().end());
        RecordWriter w(log);
        record::writeValue(w, kWorkspaceOpenSequences, open);
        section(Section_WorkspaceState, w.data());
    }

    auto& d = const_cast<std::vector<uint8_t>&>(out.data());
    for (int i = 0; i < 4; ++i)
        d[sectionCountPos + i] = static_cast<uint8_t>(sectionCount >> (8 * i));
    return out.data();
}

// ═══════════════════════════════════════════════════════════════════════════
// Load
// ═══════════════════════════════════════════════════════════════════════════

std::unique_ptr<Project> ProjectSerializer::deserialize(const std::vector<uint8_t>& data) const
{
    if (data.size() < 32) {
        spdlog::error("ProjectSerializer: file too small ({} bytes)", data.size());
        return nullptr;
    }
    if (!std::equal(MAGIC, MAGIC + 8, data.begin())) {
        spdlog::error("ProjectSerializer: invalid magic header");
        return nullptr;
    }
    const uint32_t version = static_cast<uint32_t>(data[8])
        | (static_cast<uint32_t>(data[9]) << 8)
        | (static_cast<uint32_t>(data[10]) << 16)
        | (static_cast<uint32_t>(data[11]) << 24);
    if (version > FORMAT_VERSION) {
        spdlog::error("ProjectSerializer: format version {} > supported {}", version,
                      FORMAT_VERSION);
        return nullptr;
    }
    // A damaged file must fail to load, not throw: corrupt strings can make
    // path conversion or filesystem calls throw deep inside the readers.
    try {
        return version <= LAST_POSITIONAL_VERSION ? deserializeLegacy(data)
                                                  : deserializeRecords(data);
    } catch (const std::exception& e) {
        spdlog::error("ProjectSerializer: corrupt project (v{}): {}", version, e.what());
        return nullptr;
    }
}

std::unique_ptr<Project> ProjectSerializer::deserializeRecords(const std::vector<uint8_t>& data)
{
    BinaryReader r(data.data(), data.size());
    r.skip(12);   // magic + version (checked by deserialize)
    const uint32_t sectionCount = r.readCount(kMaxProjectSections, 8);
    r.skip(16);   // reserved
    if (!r.ok()) {
        spdlog::error("ProjectSerializer: corrupt project header");
        return nullptr;
    }

    auto project = std::make_unique<Project>();
    bool hasWorkspaceState = false;
    std::vector<size_t> openSequences;

    for (uint32_t s = 0; s < sectionCount; ++s) {
        if (!r.hasRemaining(8)) {
            spdlog::error("ProjectSerializer: missing section header {} of {}", s + 1,
                          sectionCount);
            return nullptr;
        }
        const uint32_t tag = r.readU32();
        const uint32_t size = r.readU32();
        if (size > kMaxSectionBytes || !r.hasRemaining(size)) {
            spdlog::error("ProjectSerializer: section {} has invalid size {}", tag, size);
            return nullptr;
        }
        const uint8_t* payload = data.data() + r.position();
        r.skip(size);
        RecordReader rec(payload, size);
        bool ok = true;

        switch (tag) {
        case Section_Settings:
            ok = SchemaOf<Settings>::get().read(rec, project->defaultSettings());
            break;

        case Section_ProjectMeta:
            ok = projectMetaSchema().read(rec, *project);
            break;

        case Section_Sequences: {
            uint64_t active = 0;
            while (rec.next()) {
                if (rec.id() == kSequencesActive) {
                    (void)rec.readVarint(active);
                } else if (rec.id() == kSequencesList) {
                    while (project->sequenceCount() > 1)
                        project->removeSequence(project->sequenceCount() - 1);
                    size_t index = 0;
                    forEachListRecord(rec, [&](RecordReader& item) {
                        // The first sequence reuses the constructor's;
                        // either way start without default tracks.
                        Timeline* tl = index++ == 0 ? project->sequence(0)
                                                    : project->addSequence("");
                        while (tl->trackCount() > 0) tl->removeTrack(0);
                        return sequenceSchema().read(item, *tl);
                    });
                }
            }
            ok = rec.ok();
            if (active < project->sequenceCount())
                project->setActiveSequence(static_cast<size_t>(active));
            break;
        }

        case Section_Assets:
            while (rec.next()) {
                if (rec.id() != kAssetsList) continue;
                std::vector<AssetEntry> assets;
                if (!record::readValue(rec, assets)) continue;
                for (AssetEntry& a : assets) {
                    // PORTABILITY: re-resolve a moved install's absolute path
                    // from the assets/-relative one.
                    if (!a.absolutePath.empty() && !std::filesystem::exists(a.absolutePath)) {
                        auto resolved = AppPaths::assets() / a.path;
                        if (std::filesystem::exists(resolved)) {
                            a.absolutePath = std::filesystem::absolute(resolved);
                            spdlog::info("AssetDatabase: re-resolved {} → {}",
                                         pathToUtf8(a.path), pathToUtf8(a.absolutePath));
                        }
                    }
                    project->assets()->addAsset(std::move(a));
                }
            }
            ok = rec.ok();
            break;

        case Section_BinState: {
            BinStateRecord bin;
            ok = binStateSchema().read(rec, bin);
            std::vector<std::filesystem::path> files;
            files.reserve(bin.items.size());
            for (const auto& item : bin.items) files.push_back(item.path);
            project->setBinItems(std::move(bin.items));
            project->setBinFiles(std::move(files));
            project->setBinFolders(std::move(bin.folders));
            break;
        }

        case Section_AudioSync:
            project->setAudioSyncBlob(std::vector<uint8_t>(payload, payload + size));
            break;

        case Section_WorkspaceState:
            while (rec.next()) {
                std::vector<uint64_t> open;
                if (rec.id() == kWorkspaceOpenSequences && record::readValue(rec, open)) {
                    openSequences.assign(open.begin(), open.end());
                    hasWorkspaceState = true;
                }
            }
            ok = rec.ok();
            break;

        default:
            spdlog::warn("ProjectSerializer: unknown section tag 0x{:02X}, skipping", tag);
            break;
        }

        if (!ok) {
            spdlog::error("ProjectSerializer: corrupt section {}", tag);
            return nullptr;
        }
    }

    // A project saved without tab state opens every sequence.
    if (!hasWorkspaceState) {
        openSequences.clear();
        for (size_t i = 0; i < project->sequenceCount(); ++i) openSequences.push_back(i);
    }
    project->setOpenSequenceIndices(std::move(openSequences));

    // Masks still in legacy coordinates move once their clip's source size
    // is known (it may have become known since the last save).
    if (const int migrated = migrateLegacyMasks(*project); migrated > 0)
        spdlog::info("ProjectSerializer: migrated {} legacy mask(s) to clip-local coordinates",
                     migrated);

    project->setModified(false);
    return project;
}

// ═══════════════════════════════════════════════════════════════════════════
// Lightweight metadata reads (no full load)
// ═══════════════════════════════════════════════════════════════════════════

bool ProjectSerializer::readMetadata(const std::filesystem::path& path, Metadata& out)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;

    // Read the whole file into memory — these are small project files,
    // reading is still fast.  The win is *not* constructing full Timeline/
    // Track/Clip objects.
    file.seekg(0, std::ios::end);
    size_t fileSize = static_cast<size_t>(file.tellg());
    if (fileSize < 32) return false; // minimum header size
    file.seekg(0);

    // Only read the first 4 KB — that's enough for header + Settings + Timeline sections.
    constexpr size_t kMaxRead = 4096;
    size_t readSize = std::min(fileSize, kMaxRead);
    std::vector<uint8_t> buf(readSize);
    file.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(readSize));
    file.close();

    BinaryReader r(buf.data(), buf.size());

    // Validate magic
    for (int i = 0; i < 8; ++i) {
        if (r.readU8() != MAGIC[i]) return false;
    }

    const uint32_t version = r.readU32();
    if (version > FORMAT_VERSION) return false;
    const uint32_t sectionCount = r.readCount(kMaxProjectSections, 8);
    r.skip(16); // reserved
    if (!r.ok()) return false;

    bool gotSettings = false, gotName = false;

    for (uint32_t si = 0; si < sectionCount && r.remaining() >= 8; ++si) {
        uint32_t tag  = r.readU32();
        uint32_t size = r.readU32();

        if (!r.hasRemaining(size)) break;

        BinaryReader sr(buf.data() + r.position(), size);
        r.skip(size);

        const bool records = version > LAST_POSITIONAL_VERSION;
        if (records && tag == Section_Settings) {
            RecordReader rec(buf.data() + r.position() - size, size);
            Settings settings;
            if (!SchemaOf<Settings>::get().read(rec, settings)) return false;
            out.resW = settings.resolution().width;
            out.resH = settings.resolution().height;
            out.fps  = settings.frameRate();
            gotSettings = true;
        } else if (records && tag == Section_ProjectMeta) {
            RecordReader rec(buf.data() + r.position() - size, size);
            gotName = peekField(rec, 1, out.name);
        } else if (!records && tag == Section_Settings) {
            out.resW = sr.readU32();
            out.resH = sr.readU32();
            out.fps  = sr.readF64();
            gotSettings = sr.ok();
        } else if (!records && tag == Section_Timeline) {
            sr.readString(); // timeline name (skip)
            out.name = sr.readString(); // project name
            gotName = sr.ok();
        }

        if (!sr.ok()) return false;

        if (gotSettings && gotName) break; // early exit
    }

    return gotSettings;
}

std::string ProjectSerializer::readProjectShow(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) return {};

    const std::streamoff fileSize = file.tellg();
    if (fileSize < 32) return {};
    file.seekg(0, std::ios::beg);

    auto readU32 = [&file]() -> uint32_t {
        uint8_t b[4];
        file.read(reinterpret_cast<char*>(b), 4);
        if (!file) return 0;
        return static_cast<uint32_t>(b[0]) | (static_cast<uint32_t>(b[1]) << 8)
             | (static_cast<uint32_t>(b[2]) << 16) | (static_cast<uint32_t>(b[3]) << 24);
    };

    // Header: magic(8) + version(u32) + sectionCount(u32) + reserved(16).
    uint8_t magic[8];
    file.read(reinterpret_cast<char*>(magic), 8);
    if (!file) return {};
    for (int i = 0; i < 8; ++i)
        if (magic[i] != MAGIC[i]) return {};

    const uint32_t version = readU32();
    if (!file || version > FORMAT_VERSION) return {};
    const uint32_t sectionCount = readU32();
    if (!file || sectionCount > kMaxProjectSections) return {};
    file.seekg(16, std::ios::cur);         // reserved
    if (!file) return {};

    // Scan section headers; read only the ProjectMeta section's data.
    for (uint32_t si = 0; si < sectionCount && file; ++si) {
        uint32_t tag  = readU32();
        uint32_t size = readU32();
        if (!file || size > kMaxSectionBytes) return {};

        const std::streamoff payloadStart = file.tellg();
        if (payloadStart < 0 ||
            static_cast<uint64_t>(payloadStart) + size >
                static_cast<uint64_t>(fileSize))
            return {};

        if (tag == Section_ProjectMeta) {
            // v48+: a record of name and show; before: just the show string.
            const bool records = version > LAST_POSITIONAL_VERSION;
            if (size > 2 * BinaryReader::kMaxStringBytes + 64) return {};
            std::vector<uint8_t> data(size);
            file.read(reinterpret_cast<char*>(data.data()),
                      static_cast<std::streamsize>(size));
            if (!file) return {};
            if (records) {
                RecordReader rec(data.data(), data.size());
                std::string show;
                return peekField(rec, 2, show) ? show : std::string{};
            }
            BinaryReader sr(data.data(), data.size());
            std::string show = sr.readString(); // first field of ProjectMeta = show
            return sr.ok() ? show : std::string{};
        }
        file.seekg(size, std::ios::cur);   // skip this section's data
        if (!file) return {};
    }
    return {};
}

#undef RT_MEMBER

} // namespace rt
