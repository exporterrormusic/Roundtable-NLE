/*
 * ProjectSerializer.cpp — Binary serialization for .rtp files.
 * Step 5: Project Serialization
 *
 * Uses a simple streaming binary format with section tags.
 * All values little-endian (native on x86/x64).
 */

#include "project/ProjectSerializer.h"
#include "project/ClipSerialization.h"
#include "project/Project.h"
#include "project/Settings.h"
#include "project/AssetDatabase.h"
#include "timeline/Timeline.h"
#include "timeline/Track.h"
#include "timeline/Clip.h"
#include "timeline/Marker.h"
#include "timeline/Transition.h"

#include <fstream>
#include <cstring>
#include <filesystem>
#include <spdlog/spdlog.h>

namespace rt {

namespace {

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

} // namespace


// ═══════════════════════════════════════════════════════════════════════════
// Serialize
// ═══════════════════════════════════════════════════════════════════════════

std::vector<uint8_t> ProjectSerializer::serialize(const Project& project) const
{
    BinaryWriter out;

    // ── Header ──────────────────────────────────────────────────────────
    out.writeBytes(MAGIC, 8);
    out.writeU32(FORMAT_VERSION);
    // Section count placeholder — we'll count as we go
    size_t sectionCountPos = out.size();
    out.writeU32(0);
    // Reserved 16 bytes
    for (int i = 0; i < 16; ++i) out.writeU8(0);

    uint32_t sectionCount = 0;

    // ── Section: Settings ───────────────────────────────────────────────
    {
        BinaryWriter sec;
        const auto& s = project.settings();
        sec.writeU32(s.resolution().width);
        sec.writeU32(s.resolution().height);
        sec.writeF64(s.frameRate());
        sec.writeU8(static_cast<uint8_t>(s.colorSpace()));
        sec.writeU32(s.sampleRate());
        sec.writeU32(s.audioBitDepth());
        sec.writeU32(s.audioChannels());
        sec.writeString(s.exportSettings().codec);
        sec.writeU32(s.exportSettings().quality);
        sec.writeU32(s.exportSettings().audioBitrate);
        sec.writeString(s.exportSettings().outputPath);
        out.beginSection(Section_Settings, sec.data());
        ++sectionCount;
    }

    // ── Section: Timeline metadata (backward compat — active sequence) ──
    {
        BinaryWriter sec;
        const Timeline* tl = project.timeline();
        sec.writeString(tl->name());
        sec.writeString(project.name());
        sec.writeI64(tl->playheadPosition());
        sec.writeI64(tl->inPoint());
        sec.writeI64(tl->outPoint());
        out.beginSection(Section_Timeline, sec.data());
        ++sectionCount;
    }

    // ── Section: Tracks + Clips (backward compat — active sequence) ─────
    {
        BinaryWriter sec;
        const Timeline* tl = project.timeline();
        sec.writeU32(static_cast<uint32_t>(tl->trackCount()));

        for (size_t ti = 0; ti < tl->trackCount(); ++ti)
        {
            const Track* track = tl->track(ti);
            sec.writeU8(static_cast<uint8_t>(track->type()));
            sec.writeString(track->name());
            sec.writeU8(track->isLocked() ? 1 : 0);
            sec.writeU8(track->isMuted() ? 1 : 0);
            sec.writeU8(track->isSoloed() ? 1 : 0);
            sec.writeF32(track->height());
            sec.writeU8(track->isSyncLocked() ? 1 : 0); // v5+
            sec.writeU8(track->isDivider() ? 1 : 0);    // v18+
            sec.writeU8(track->isPermanentDivider() ? 1 : 0); // v20+
            sec.writeU8(track->isCaptionTrack() ? 1 : 0); // v21+

            // Clips for this track
            sec.writeU32(static_cast<uint32_t>(track->clipCount()));
            for (size_t ci = 0; ci < track->clipCount(); ++ci)
            {
                writeClip(sec, *track->clip(ci));
            }

            // Transitions for this track
            sec.writeU32(static_cast<uint32_t>(track->transitionCount()));
            for (size_t xi = 0; xi < track->transitionCount(); ++xi)
            {
                const Transition* t = track->transition(xi);
                sec.writeU8(static_cast<uint8_t>(t->type));
                sec.writeI64(t->duration);
                sec.writeI64(t->offset);
                sec.writeF32(t->param1);
                sec.writeF32(t->param2);
                // v16+: clip linkage + edit-point position. Without these
                // the transition collapses to tick 0 on reload and is
                // effectively lost.
                sec.writeU64(t->leftClipId);
                sec.writeU64(t->rightClipId);
                sec.writeI64(t->editPointTick);
            }
        }

        out.beginSection(Section_Tracks, sec.data());
        ++sectionCount;
    }

    // ── Section: Markers (backward compat — active sequence) ────────────
    {
        BinaryWriter sec;
        const auto& markers = project.timeline()->markers();
        sec.writeU32(static_cast<uint32_t>(markers.size()));
        for (const auto& m : markers)
        {
            sec.writeI64(m.time);
            sec.writeString(m.label);
            sec.writeU32(m.color);
        }
        out.beginSection(Section_Markers, sec.data());
        ++sectionCount;
    }

    // ── Section: Sequences (all sequences including non-active) ─────────
    {
        BinaryWriter sec;
        sec.writeU32(static_cast<uint32_t>(project.sequenceCount()));
        sec.writeU32(static_cast<uint32_t>(project.activeSequenceIndex()));

        for (size_t si = 0; si < project.sequenceCount(); ++si)
        {
            const Timeline* tl = project.sequence(si);

            // Sequence metadata
            sec.writeString(tl->name());
            sec.writeI64(tl->playheadPosition());
            sec.writeI64(tl->inPoint());
            sec.writeI64(tl->outPoint());

            // Tracks + clips
            sec.writeU32(static_cast<uint32_t>(tl->trackCount()));
            for (size_t ti = 0; ti < tl->trackCount(); ++ti)
            {
                const Track* track = tl->track(ti);
                sec.writeU8(static_cast<uint8_t>(track->type()));
                sec.writeString(track->name());
                sec.writeU8(track->isLocked() ? 1 : 0);
                sec.writeU8(track->isMuted() ? 1 : 0);
                sec.writeU8(track->isSoloed() ? 1 : 0);
                sec.writeF32(track->height());
                sec.writeU8(track->isSyncLocked() ? 1 : 0); // v5+
                sec.writeU8(track->isDivider() ? 1 : 0);    // v18+
                sec.writeU8(track->isPermanentDivider() ? 1 : 0); // v20+
                sec.writeU8(track->isCaptionTrack() ? 1 : 0); // v21+

                sec.writeU32(static_cast<uint32_t>(track->clipCount()));
                for (size_t ci = 0; ci < track->clipCount(); ++ci)
                    writeClip(sec, *track->clip(ci));

                sec.writeU32(static_cast<uint32_t>(track->transitionCount()));
                for (size_t xi = 0; xi < track->transitionCount(); ++xi)
                {
                    const Transition* t = track->transition(xi);
                    sec.writeU8(static_cast<uint8_t>(t->type));
                    sec.writeI64(t->duration);
                    sec.writeI64(t->offset);
                    sec.writeF32(t->param1);
                    sec.writeF32(t->param2);
                    // v16+: clip linkage + edit-point position
                    sec.writeU64(t->leftClipId);
                    sec.writeU64(t->rightClipId);
                    sec.writeI64(t->editPointTick);
                }
            }

            // Markers for this sequence
            const auto& markers = tl->markers();
            sec.writeU32(static_cast<uint32_t>(markers.size()));
            for (const auto& m : markers)
            {
                sec.writeI64(m.time);
                sec.writeString(m.label);
                sec.writeU32(m.color);
            }
        }

        out.beginSection(Section_Sequences, sec.data());
        ++sectionCount;
    }

    // ── Section: Asset entries ──────────────────────────────────────────
    {
        BinaryWriter sec;
        const AssetDatabase* db = project.assets();
        sec.writeU32(static_cast<uint32_t>(db->assetCount()));

        // Serialize all assets by type
        for (auto t : {AssetType::Character, AssetType::Background, AssetType::Audio,
                       AssetType::Video, AssetType::Image, AssetType::Font})
        {
            auto assets = db->findByType(t);
            for (const AssetEntry* a : assets)
            {
                sec.writeU64(a->id);
                sec.writeU8(static_cast<uint8_t>(a->type));
                sec.writeString(a->name);
                sec.writePath(a->path);
                sec.writePath(a->absolutePath);
                sec.writeU64(a->fileSize);
                sec.writeString(a->hash);
            }
        }

        out.beginSection(Section_Assets, sec.data());
        ++sectionCount;
    }

    // ── Section: Character assets ───────────────────────────────────────
    {
        BinaryWriter sec;
        const auto& chars = project.assets()->characters();
        sec.writeU32(static_cast<uint32_t>(chars.size()));
        for (const auto& c : chars)
        {
            sec.writeString(c.name);
            sec.writePath(c.basePath);
            sec.writeU32(static_cast<uint32_t>(c.outfits.size()));
            for (const auto& o : c.outfits) sec.writeString(o);
            sec.writeU32(static_cast<uint32_t>(c.stances.size()));
            for (const auto& s : c.stances) sec.writeString(s);
        }
        out.beginSection(Section_Characters, sec.data());
        ++sectionCount;
    }

    // ── Section: Bin state (v14+: rich per-instance items + folders) ────
    {
        BinaryWriter sec;

        // Rich bin items: each is an independent bin entry (footage can
        // appear multiple times as separate "master clips"). Falls back
        // to deriving from binFiles() for projects that never populated
        // the rich model.
        const auto& binItems = project.binItems();
        if (!binItems.empty()) {
            sec.writeU32(static_cast<uint32_t>(binItems.size()));
            for (const auto& it : binItems) {
                sec.writeU64(it.id);
                sec.writePath(it.path);
                sec.writeString(it.displayName);
                sec.writeU32(it.labelColor);
            }
        } else {
            const auto& binFiles = project.binFiles();
            sec.writeU32(static_cast<uint32_t>(binFiles.size()));
            uint64_t synthId = 1;
            for (const auto& f : binFiles) {
                sec.writeU64(synthId++);
                sec.writePath(f);
                sec.writeString(std::string{});      // no custom name
                sec.writeU32(0xFF888888u);           // default label
            }
        }

        const auto& binFolders = project.binFolders();
        sec.writeU32(static_cast<uint32_t>(binFolders.size()));
        for (const auto& bf : binFolders)
        {
            sec.writeString(bf.name);
            sec.writeU8(bf.expanded ? 1 : 0);
            sec.writeU32(static_cast<uint32_t>(bf.childKeys.size()));
            for (const auto& k : bf.childKeys)
                sec.writeString(k);
        }

        out.beginSection(Section_BinState, sec.data());
        ++sectionCount;
    }

    // ── Section: AudioSync state (opaque blob from AudioSync panel) ─────
    {
        const auto& blob = project.audioSyncBlob();
        if (!blob.empty()) {
            out.beginSection(Section_AudioSync, blob);
            ++sectionCount;
        }
    }

    // ── Patch section count ─────────────────────────────────────────────
    auto& d = const_cast<std::vector<uint8_t>&>(out.data());
    d[sectionCountPos]     = static_cast<uint8_t>(sectionCount);
    d[sectionCountPos + 1] = static_cast<uint8_t>(sectionCount >> 8);
    d[sectionCountPos + 2] = static_cast<uint8_t>(sectionCount >> 16);
    d[sectionCountPos + 3] = static_cast<uint8_t>(sectionCount >> 24);

    return out.data();
}

// ═══════════════════════════════════════════════════════════════════════════
// Deserialize
// ═══════════════════════════════════════════════════════════════════════════

std::unique_ptr<Project> ProjectSerializer::deserialize(const std::vector<uint8_t>& data) const
{
    if (data.size() < 32) // Header is 32 bytes minimum
    {
        spdlog::error("ProjectSerializer: file too small ({} bytes)", data.size());
        return nullptr;
    }

    BinaryReader r(data.data(), data.size());

    // ── Verify magic ────────────────────────────────────────────────────
    for (int i = 0; i < 8; ++i)
    {
        if (r.readU8() != MAGIC[i])
        {
            spdlog::error("ProjectSerializer: invalid magic header");
            return nullptr;
        }
    }

    uint32_t version = r.readU32();
    if (version > FORMAT_VERSION)
    {
        spdlog::error("ProjectSerializer: format version {} > supported {}", version, FORMAT_VERSION);
        return nullptr;
    }

    uint32_t sectionCount = r.readU32();
    r.skip(16); // Reserved

    auto project = std::make_unique<Project>();
    bool hasSequencesSection = false;  // Track whether v4 multi-sequence section exists

    // ── Read sections ───────────────────────────────────────────────────
    for (uint32_t s = 0; s < sectionCount && r.remaining() >= 8; ++s)
    {
        uint32_t tag  = r.readU32();
        uint32_t size = r.readU32();

        if (!r.hasRemaining(size))
        {
            spdlog::warn("ProjectSerializer: section {} truncated", tag);
            break;
        }

        // Create a sub-reader for this section
        BinaryReader sr(data.data() + r.position(), size);
        r.skip(size); // Advance past the section

        switch (tag)
        {
        case Section_Settings: {
            auto& settings = project->settings();
            uint32_t w = sr.readU32();
            uint32_t h = sr.readU32();
            settings.setResolution(w, h);
            settings.setFrameRate(sr.readF64());
            settings.setColorSpace(static_cast<ColorSpace>(sr.readU8()));
            AudioFormat af;
            af.sampleRate = sr.readU32();
            af.bitDepth   = sr.readU32();
            af.channels   = sr.readU32();
            settings.setAudioFormat(af);
            ExportSettings es;
            es.codec        = sr.readString();
            es.quality      = sr.readU32();
            es.audioBitrate = sr.readU32();
            es.outputPath   = sr.readString();
            settings.setExportSettings(es);
            break;
        }

        case Section_Timeline: {
            // Legacy single-sequence metadata (used only if no Section_Sequences)
            if (!hasSequencesSection) {
                Timeline* tl = project->timeline();
                tl->setName(sr.readString());
                project->setName(sr.readString());
                tl->setPlayheadPosition(sr.readI64());
                tl->setInPoint(sr.readI64());
                tl->setOutPoint(sr.readI64());
            } else {
                // Just read the project name from the legacy section
                sr.readString(); // timeline name (already loaded from sequences)
                project->setName(sr.readString());
            }
            break;
        }

        case Section_Tracks: {
            if (!hasSequencesSection) {
                Timeline* tl = project->timeline();
                // Remove default tracks (constructor adds empty timeline)
                while (tl->trackCount() > 0)
                    tl->removeTrack(0);

                uint32_t trackCount = sr.readU32();
                for (uint32_t ti = 0; ti < trackCount; ++ti)
                {
                    auto type = static_cast<TrackType>(sr.readU8());
                    std::string name = sr.readString();
                    bool locked = sr.readU8() != 0;
                    bool muted  = sr.readU8() != 0;
                    bool soloed = sr.readU8() != 0;
                    float height = sr.readF32();

                    // Build the track directly and APPEND in saved order.
                    // Going through addVideoTrack/addAudioTrack used to
                    // re-sort by type+divider, which dragged any user-added
                    // divider sitting above the video stack down into the
                    // V/A boundary slot on load.
                    auto trackPtr = std::make_unique<Track>(type, name);
                    Track* track = trackPtr.get();
                    tl->insertTrack(tl->trackCount(), std::move(trackPtr));

                    track->setLocked(locked);
                    track->setMuted(muted);
                    track->setSoloed(soloed);
                    track->setHeight(height);
                    if (version >= 5)
                        track->setSyncLocked(sr.readU8() != 0);
                    if (version >= 18) {
                        track->setDivider(sr.readU8() != 0);
                    } else if (type == TrackType::Video && height < 15.0f) {
                        // v17 and older never persisted isDivider; the V/A
                        // separator was saved as a regular Video track. Auto-
                        // promote tracks matching the divider signature (Video
                        // type + short height — real video tracks default to
                        // 80px). The name check was dropped after we hit cases
                        // where an older build's auto-rename loop gave the
                        // unflagged divider a "V<N>" label before saving.
                        track->setDivider(true);
                    }
                    if (version >= 20) {
                        track->setPermanentDivider(sr.readU8() != 0);
                    }
                    // For v19 and older, leave isPermanentDivider=false —
                    // ensureSectionDivider will promote whichever divider sits
                    // at the V/A boundary on first rebuild (the legacy
                    // "greedy" path, used only for one-time migration).
                    if (version >= 21) {
                        track->setCaptionTrack(sr.readU8() != 0);
                    }

                    // Clips
                    uint32_t clipCount = sr.readU32();
                    for (uint32_t ci = 0; ci < clipCount; ++ci)
                    {
                        auto clip = readClip(sr, version);
                        if (clip)
                            track->addClip(std::move(clip));
                    }

                    // Transitions
                    uint32_t transCount = sr.readU32();
                    for (uint32_t xi = 0; xi < transCount; ++xi)
                    {
                        Transition t;
                        t.type     = static_cast<TransitionType>(sr.readU8());
                        t.duration = sr.readI64();
                        t.offset   = sr.readI64();
                        t.param1   = sr.readF32();
                        t.param2   = sr.readF32();
                        if (version >= 16)
                        {
                            t.leftClipId    = sr.readU64();
                            t.rightClipId   = sr.readU64();
                            t.editPointTick = sr.readI64();
                        }
                        track->addTransition(t);
                    }

                    relinkTransitionsByGeometry(*track);
                }

                // Ensure video tracks are always above audio tracks,
                // even if an old project file saved them in wrong order.
                tl->sortTracksByType();
            }
            // If hasSequencesSection, skip — data already loaded from sequences
            break;
        }

        case Section_Markers: {
            if (!hasSequencesSection) {
                Timeline* tl = project->timeline();
                uint32_t count = sr.readU32();
                for (uint32_t i = 0; i < count; ++i)
                {
                    int64_t time     = sr.readI64();
                    std::string label = sr.readString();
                    uint32_t color   = sr.readU32();
                    tl->addMarker(time, label, color);
                }
            }
            // If hasSequencesSection, skip — markers already loaded per-sequence
            break;
        }

        case Section_Sequences: {
            hasSequencesSection = true;

            // Clear the default sequence created by constructor
            while (project->sequenceCount() > 1)
                project->removeSequence(project->sequenceCount() - 1);

            uint32_t seqCount    = sr.readU32();
            uint32_t activeIndex = sr.readU32();

            for (uint32_t si = 0; si < seqCount; ++si)
            {
                Timeline* tl = nullptr;
                if (si == 0) {
                    // Reuse the default sequence from the constructor
                    tl = project->sequence(0);
                    // Remove its default tracks
                    while (tl->trackCount() > 0)
                        tl->removeTrack(0);
                } else {
                    tl = project->addSequence("");
                    // Remove the default V1+A1 tracks added by addSequence
                    while (tl->trackCount() > 0)
                        tl->removeTrack(0);
                }

                // Read sequence metadata
                tl->setName(sr.readString());
                tl->setPlayheadPosition(sr.readI64());
                tl->setInPoint(sr.readI64());
                tl->setOutPoint(sr.readI64());

                // Read tracks + clips
                uint32_t trackCount = sr.readU32();
                for (uint32_t ti = 0; ti < trackCount; ++ti)
                {
                    auto type = static_cast<TrackType>(sr.readU8());
                    std::string name = sr.readString();
                    bool locked = sr.readU8() != 0;
                    bool muted  = sr.readU8() != 0;
                    bool soloed = sr.readU8() != 0;
                    float height = sr.readF32();

                    // Append in saved order; addVideoTrack/addAudioTrack would
                    // re-sort and shuffle user-added dividers — see comment in
                    // the Section_Tracks reader above.
                    auto trackPtr = std::make_unique<Track>(type, name);
                    Track* track = trackPtr.get();
                    tl->insertTrack(tl->trackCount(), std::move(trackPtr));

                    track->setLocked(locked);
                    track->setMuted(muted);
                    track->setSoloed(soloed);
                    track->setHeight(height);
                    if (version >= 5)
                        track->setSyncLocked(sr.readU8() != 0);
                    if (version >= 18) {
                        track->setDivider(sr.readU8() != 0);
                    } else if (type == TrackType::Video &&
                               name.empty() &&
                               height < 15.0f) {
                        // v17 and older: auto-promote tracks matching the
                        // divider signature (see Section_Tracks reader above).
                        track->setDivider(true);
                    }
                    if (version >= 20) {
                        track->setPermanentDivider(sr.readU8() != 0);
                    }
                    if (version >= 21) {
                        track->setCaptionTrack(sr.readU8() != 0);
                    }

                    uint32_t clipCount = sr.readU32();
                    for (uint32_t ci = 0; ci < clipCount; ++ci)
                    {
                        auto clip = readClip(sr, version);
                        if (clip)
                            track->addClip(std::move(clip));
                    }

                    uint32_t transCount = sr.readU32();
                    for (uint32_t xi = 0; xi < transCount; ++xi)
                    {
                        Transition t;
                        t.type     = static_cast<TransitionType>(sr.readU8());
                        t.duration = sr.readI64();
                        t.offset   = sr.readI64();
                        t.param1   = sr.readF32();
                        t.param2   = sr.readF32();
                        if (version >= 17)  // v17+: Sequences section correctly writes clip-link fields
                        {
                            t.leftClipId    = sr.readU64();
                            t.rightClipId   = sr.readU64();
                            t.editPointTick = sr.readI64();
                        }
                        track->addTransition(t);
                    }

                    relinkTransitionsByGeometry(*track);
                }

                tl->sortTracksByType();

                // Clear any markers that were loaded from the legacy
                // Section_Markers (which runs before Section_Sequences).
                while (!tl->markers().empty())
                    tl->removeMarker(0);

                // Read markers for this sequence
                uint32_t markerCount = sr.readU32();
                for (uint32_t mi = 0; mi < markerCount; ++mi)
                {
                    int64_t time     = sr.readI64();
                    std::string label = sr.readString();
                    uint32_t color   = sr.readU32();
                    tl->addMarker(time, label, color);
                }
            }

            // Set active sequence
            if (activeIndex < project->sequenceCount())
                project->setActiveSequence(activeIndex);

            spdlog::info("ProjectSerializer: loaded {} sequences (active={})",
                         seqCount, activeIndex);
            break;
        }

        case Section_Assets: {
            uint32_t count = sr.readU32();
            for (uint32_t i = 0; i < count; ++i)
            {
                AssetEntry a;
                a.id           = sr.readU64();
                a.type         = static_cast<AssetType>(sr.readU8());
                a.name         = sr.readString();
                a.path         = sr.readPath();
                a.absolutePath = sr.readPath();
                a.fileSize     = sr.readU64();
                a.hash         = sr.readString();

                // PORTABILITY: Re-resolve absolutePath from relativePath
                // when the saved absolutePath no longer exists (folder moved
                // to a different machine/drive). The relative path is always
                // relative to assets/, so resolve from CWD/assets/.
                if (!a.absolutePath.empty() && !std::filesystem::exists(a.absolutePath)) {
                    auto resolved = std::filesystem::current_path() / "assets" / a.path;
                    if (std::filesystem::exists(resolved)) {
                        a.absolutePath = std::filesystem::absolute(resolved);
                        spdlog::info("AssetDatabase: re-resolved {} → {}",
                                     a.path.string(), a.absolutePath.string());
                    }
                }

                project->assets()->addAsset(std::move(a));
            }
            break;
        }

        case Section_Characters: {
            uint32_t count = sr.readU32();
            for (uint32_t i = 0; i < count; ++i)
            {
                // Read and discard for backward compatibility.
                // Characters are restored via AssetDatabase::scanCharacters().
                sr.readString();  // name
                sr.readPath();     // base path
                uint32_t outfitCount = sr.readU32();
                for (uint32_t o = 0; o < outfitCount; ++o) sr.readString();
                uint32_t stanceCount = sr.readU32();
                for (uint32_t st = 0; st < stanceCount; ++st) sr.readString();
            }
            break;
        }

        case Section_BinState: {
            uint32_t fileCount = sr.readU32();
            std::vector<std::filesystem::path> binFiles;
            binFiles.reserve(fileCount);
            if (version >= 14) {
                // Rich per-instance bin items.
                std::vector<Project::BinItem> items;
                items.reserve(fileCount);
                for (uint32_t i = 0; i < fileCount; ++i) {
                    Project::BinItem bi;
                    bi.id          = sr.readU64();
                    bi.path        = sr.readPath();
                    bi.displayName = sr.readString();
                    bi.labelColor  = sr.readU32();
                    binFiles.push_back(bi.path);
                    items.push_back(std::move(bi));
                }
                project->setBinItems(std::move(items));
                project->setBinFiles(std::move(binFiles));
            } else {
                // Legacy: flat path list (no per-item identity).
                for (uint32_t i = 0; i < fileCount; ++i)
                    binFiles.push_back(sr.readPath());
                project->setBinFiles(std::move(binFiles));
            }

            // Bin folder structure
            uint32_t folderCount = sr.readU32();
            std::vector<Project::BinFolder> binFolders;
            binFolders.reserve(folderCount);
            for (uint32_t i = 0; i < folderCount; ++i)
            {
                Project::BinFolder bf;
                bf.name = sr.readString();
                if (version >= 6)
                    bf.expanded = (sr.readU8() != 0);
                uint32_t keyCount = sr.readU32();
                bf.childKeys.reserve(keyCount);
                for (uint32_t k = 0; k < keyCount; ++k)
                    bf.childKeys.push_back(sr.readString());
                binFolders.push_back(std::move(bf));
            }
            project->setBinFolders(std::move(binFolders));

            spdlog::info("ProjectSerializer: loaded bin state ({} files, {} folders)",
                         fileCount, folderCount);
            break;
        }

        case Section_AudioSync: {
            // Store the raw blob — AudioSync panel will deserialize it
            const uint8_t* blobStart = data.data() + r.position() - size;
            std::vector<uint8_t> blob(blobStart, blobStart + size);
            project->setAudioSyncBlob(std::move(blob));
            spdlog::info("ProjectSerializer: loaded AudioSync blob ({} bytes)", size);
            break;
        }

        default:
            spdlog::warn("ProjectSerializer: unknown section tag 0x{:02X}, skipping", tag);
            break;
        }
    }

    // ── Frame-align migration ───────────────────────────────────────────
    // Clips placed before drag/export frame-snapping can have boundaries on
    // fractional-frame ticks (timeline ticks run at 48000/sec; a frame is
    // 48000/fps ticks). Playback samples on exact frame boundaries, so a
    // clip whose end overhangs a boundary by a sub-frame renders one extra
    // frame — the 1-frame "ghost" of a character that should have left at a
    // shot cut. Snap every clip's in/out to the nearest whole frame so the
    // boundaries the user aligned by eye are exact. Idempotent (re-running
    // on already-aligned clips is a no-op).
    if (Timeline* tl = project->timeline()) {
        const int64_t tpf = project->settings().ticksPerFrame();
        if (tpf > 0) {
            auto q = [tpf](int64_t t) { return ((t + tpf / 2) / tpf) * tpf; };
            int adjusted = 0;
            for (size_t ti = 0; ti < tl->trackCount(); ++ti) {
                Track* trk = tl->track(ti);
                if (!trk) continue;
                for (size_t ci = 0; ci < trk->clipCount(); ++ci) {
                    Clip* c = trk->clip(ci);
                    if (!c) continue;
                    const int64_t inQ  = q(c->timelineIn());
                    int64_t       outQ = q(c->timelineOut());
                    if (outQ <= inQ) outQ = inQ + tpf;   // keep at least 1 frame
                    if (inQ != c->timelineIn() || outQ != c->timelineOut()) {
                        c->setTimelineIn(inQ);
                        c->setDuration(outQ - inQ);
                        ++adjusted;
                    }
                }
            }
            if (adjusted > 0)
                spdlog::info("ProjectSerializer: frame-aligned {} clip boundaries "
                             "(tpf={})", adjusted, tpf);
        }
    }

    project->setModified(false);
    return project;
}

// ═════════════════════════════════════════════════════════════════════════════
// save() and load() are in ProjectSerializerIO.cpp
// ═════════════════════════════════════════════════════════════════════════════
// Lightweight metadata read (no full deserialization)
// ═════════════════════════════════════════════════════════════════════════════

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

    [[maybe_unused]] uint32_t version = r.readU32();
    uint32_t sectionCount = r.readU32();
    r.skip(16); // reserved

    bool gotSettings = false, gotName = false;

    for (uint32_t si = 0; si < sectionCount && r.remaining() >= 8; ++si) {
        uint32_t tag  = r.readU32();
        uint32_t size = r.readU32();

        if (!r.hasRemaining(size)) break;

        BinaryReader sr(buf.data() + r.position(), size);
        r.skip(size);

        if (tag == Section_Settings) {
            out.resW = sr.readU32();
            out.resH = sr.readU32();
            out.fps  = sr.readF64();
            gotSettings = true;
        } else if (tag == Section_Timeline) {
            sr.readString(); // timeline name (skip)
            out.name = sr.readString(); // project name
            gotName = true;
        }

        if (gotSettings && gotName) break; // early exit
    }

    return gotSettings;
}

} // namespace rt
