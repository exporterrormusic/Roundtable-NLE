/*
 * ProjectSerializerLegacy.cpp — loading positional project files (format
 * v1–47). FROZEN: v48+ files are records (ProjectSerializer.cpp); nothing
 * new is ever added here. Old files keep loading through exactly this code,
 * including its version-keyed migrations.
 */

#include "project/ProjectSerializer.h"
#include "project/ClipSerializationLegacy.h"
#include "project/ProjectSerializerInternal.h"
#include "project/Project.h"
#include "project/Settings.h"
#include "project/AssetDatabase.h"
#include "timeline/Timeline.h"
#include "timeline/Track.h"
#include "timeline/Clip.h"
#include "timeline/ImageClip.h"
#include "timeline/Marker.h"
#include "timeline/Transition.h"
#include "timeline/VideoClip.h"
#include "AppPaths.h"
#include "PathUtils.h"

#include <fstream>
#include <cstring>
#include <filesystem>
#include <algorithm>
#include <unordered_map>
#include <spdlog/spdlog.h>

namespace rt {

using namespace project_detail;

std::unique_ptr<Project> ProjectSerializer::deserializeLegacy(const std::vector<uint8_t>& data)
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
    if (version > LAST_POSITIONAL_VERSION)
    {
        spdlog::error("ProjectSerializer: positional format version {} > {}", version,
                      LAST_POSITIONAL_VERSION);
        return nullptr;
    }

    const uint32_t sectionCount = r.readCount(kMaxProjectSections, 8);
    r.skip(16); // Reserved

    if (!r.ok()) {
        spdlog::error("ProjectSerializer: corrupt project header at byte {}",
                      r.errorPosition());
        return nullptr;
    }

    auto project = std::make_unique<Project>();
    bool hasSequencesSection = false;  // Track whether v4 multi-sequence section exists
    bool hasWorkspaceStateSection = false;
    std::vector<size_t> savedOpenSequenceIndices;

    // ── Read sections ───────────────────────────────────────────────────
    for (uint32_t s = 0; s < sectionCount; ++s)
    {
        if (!r.hasRemaining(8)) {
            spdlog::error("ProjectSerializer: missing section header {} of {}",
                          s + 1, sectionCount);
            return nullptr;
        }
        uint32_t tag  = r.readU32();
        uint32_t size = r.readU32();

        if (size > kMaxSectionBytes || !r.hasRemaining(size))
        {
            spdlog::error("ProjectSerializer: section {} has invalid size {} ({} bytes remain)",
                          tag, size, r.remaining());
            return nullptr;
        }

        // Create a sub-reader for this section
        const size_t sectionStart = r.position();
        BinaryReader sr = r.readSubReader(size);

        switch (tag)
        {
        case Section_Settings: {
            auto& settings = project->defaultSettings();
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

                const uint32_t trackCount = sr.readCount(kMaxTracksPerSequence, 20);
                for (uint32_t ti = 0; ti < trackCount && sr.ok(); ++ti)
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
                    const uint32_t clipCount = sr.readCount(kMaxClipsPerTrack, 50);
                    for (uint32_t ci = 0; ci < clipCount && sr.ok(); ++ci)
                    {
                        auto clip = readClip(sr, version);
                        if (clip)
                            track->addClip(
                                std::move(clip), TrackMutationPolicy::BypassLock);
                    }

                    // Transitions
                    const uint32_t transCount = sr.readCount(
                        kMaxTransitionsPerTrack, version >= 16 ? 49 : 25);
                    for (uint32_t xi = 0; xi < transCount && sr.ok(); ++xi)
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
                        track->addTransition(
                            t, TrackMutationPolicy::BypassLock);
                    }

                    relinkTransitionsByGeometry(*track);
                    dedupeTransitions(*track);
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
                const uint32_t count = sr.readCount(kMaxMarkersPerSequence, 16);
                for (uint32_t i = 0; i < count && sr.ok(); ++i)
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

            const uint32_t seqCount = sr.readCount(kMaxSequences, 36);
            uint32_t activeIndex = sr.readU32();

            for (uint32_t si = 0; si < seqCount && sr.ok(); ++si)
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
                const uint32_t trackCount = sr.readCount(kMaxTracksPerSequence, 20);
                for (uint32_t ti = 0; ti < trackCount && sr.ok(); ++ti)
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

                    const uint32_t clipCount = sr.readCount(kMaxClipsPerTrack, 50);
                    for (uint32_t ci = 0; ci < clipCount && sr.ok(); ++ci)
                    {
                        auto clip = readClip(sr, version);
                        if (clip)
                            track->addClip(
                                std::move(clip), TrackMutationPolicy::BypassLock);
                    }

                    const uint32_t transCount = sr.readCount(
                        kMaxTransitionsPerTrack, version >= 17 ? 49 : 25);
                    for (uint32_t xi = 0; xi < transCount && sr.ok(); ++xi)
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
                        track->addTransition(
                            t, TrackMutationPolicy::BypassLock);
                    }

                    relinkTransitionsByGeometry(*track);
                    dedupeTransitions(*track);
                }

                tl->sortTracksByType();

                // Clear any markers that were loaded from the legacy
                // Section_Markers (which runs before Section_Sequences).
                while (!tl->markers().empty())
                    tl->removeMarker(0);

                // Read markers for this sequence
                const uint32_t markerCount = sr.readCount(kMaxMarkersPerSequence, 16);
                for (uint32_t mi = 0; mi < markerCount && sr.ok(); ++mi)
                {
                    int64_t time     = sr.readI64();
                    std::string label = sr.readString();
                    uint32_t color   = sr.readU32();
                    tl->addMarker(time, label, color);
                }

                // Per-sequence settings (v25+). Pre-v25 files have none — the
                // post-load fallback below copies the project default into
                // every sequence (matches the old global-settings behaviour).
                if (version >= 25) {
                    Settings ss;
                    uint32_t sw = sr.readU32();
                    uint32_t sh = sr.readU32();
                    ss.setResolution(sw, sh);
                    ss.setFrameRate(sr.readF64());
                    ss.setColorSpace(static_cast<ColorSpace>(sr.readU8()));
                    AudioFormat af;
                    af.sampleRate = sr.readU32();
                    af.bitDepth   = sr.readU32();
                    af.channels   = sr.readU32();
                    ss.setAudioFormat(af);
                    ExportSettings es;
                    es.codec        = sr.readString();
                    es.quality      = sr.readU32();
                    es.audioBitrate = sr.readU32();
                    es.outputPath   = sr.readString();
                    ss.setExportSettings(es);
                    tl->setSettings(ss);
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
            const uint32_t count = sr.readCount(kMaxAssets, 33);
            for (uint32_t i = 0; i < count && sr.ok(); ++i)
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
                // relative to assets/, so resolve it against this install.
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
            break;
        }

        case Section_Characters: {
            const uint32_t count = sr.readCount(kMaxCharacters, 16);
            for (uint32_t i = 0; i < count && sr.ok(); ++i)
            {
                // Read and discard for backward compatibility.
                // Characters are restored via AssetDatabase::scanCharacters().
                sr.readString();  // name
                sr.readPath();     // base path
                const uint32_t outfitCount = sr.readCount(100000, 4);
                for (uint32_t o = 0; o < outfitCount && sr.ok(); ++o) sr.readString();
                const uint32_t stanceCount = sr.readCount(100000, 4);
                for (uint32_t st = 0; st < stanceCount && sr.ok(); ++st) sr.readString();
            }
            break;
        }

        case Section_BinState: {
            const uint32_t fileCount = sr.readCount(
                kMaxBinItems, version >= 14 ? 20 : 4);
            std::vector<std::filesystem::path> binFiles;
            binFiles.reserve(fileCount);
            if (version >= 14) {
                // Rich per-instance bin items.
                std::vector<Project::BinItem> items;
                items.reserve(fileCount);
                for (uint32_t i = 0; i < fileCount && sr.ok(); ++i) {
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
                for (uint32_t i = 0; i < fileCount && sr.ok(); ++i)
                    binFiles.push_back(sr.readPath());
                project->setBinFiles(std::move(binFiles));
            }

            // Bin folder structure
            const uint32_t folderCount = sr.readCount(
                kMaxBinFolders, version >= 6 ? 9 : 8);
            std::vector<Project::BinFolder> binFolders;
            binFolders.reserve(folderCount);
            for (uint32_t i = 0; i < folderCount && sr.ok(); ++i)
            {
                Project::BinFolder bf;
                bf.name = sr.readString();
                if (version >= 6)
                    bf.expanded = (sr.readU8() != 0);
                const uint32_t keyCount = sr.readCount(kMaxFolderChildren, 4);
                bf.childKeys.reserve(keyCount);
                for (uint32_t k = 0; k < keyCount && sr.ok(); ++k)
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
            const uint8_t* blobStart = data.data() + sectionStart;
            std::vector<uint8_t> blob(blobStart, blobStart + size);
            project->setAudioSyncBlob(std::move(blob));
            spdlog::info("ProjectSerializer: loaded AudioSync blob ({} bytes)", size);
            break;
        }

        case Section_ProjectMeta: {
            project->setShow(sr.readString());
            spdlog::info("ProjectSerializer: loaded project show '{}'", project->show());
            break;
        }

        case Section_WorkspaceState: {
            const uint32_t openCount = sr.readCount(kMaxSequences, 4);
            savedOpenSequenceIndices.clear();
            savedOpenSequenceIndices.reserve(openCount);
            for (uint32_t i = 0; i < openCount && sr.ok(); ++i)
                savedOpenSequenceIndices.push_back(sr.readU32());
            hasWorkspaceStateSection = true;
            break;
        }

        default:
            spdlog::warn("ProjectSerializer: unknown section tag 0x{:02X}, skipping", tag);
            break;
        }

        if (!sr.ok()) {
            spdlog::error(
                "ProjectSerializer: corrupt section {} at section byte {}",
                tag, sr.errorPosition());
            return nullptr;
        }
    }

    if (!r.ok()) {
        spdlog::error("ProjectSerializer: corrupt section table at byte {}",
                      r.errorPosition());
        return nullptr;
    }

    // ── Pre-v25 per-sequence settings migration ─────────────────────────
    // Before v25 there was a single global Settings shared by all sequences.
    // Copy the loaded project default into every sequence so old projects keep
    // their resolution/fps; from here each sequence's settings are independent.
    if (version < 25) {
        for (size_t si = 0; si < project->sequenceCount(); ++si)
            if (Timeline* tl = project->sequence(si))
                tl->setSettings(project->defaultSettings());
    }

    // Projects written before v43 had no persisted tab state and historically
    // opened every sequence. Preserve that behavior for old files; new files
    // restore the exact saved subset. The setter also filters stale indices
    // and guarantees at least the active sequence remains open.
    if (hasWorkspaceStateSection) {
        project->setOpenSequenceIndices(std::move(savedOpenSequenceIndices));
    } else {
        std::vector<size_t> allSequenceIndices;
        allSequenceIndices.reserve(project->sequenceCount());
        for (size_t i = 0; i < project->sequenceCount(); ++i)
            allSequenceIndices.push_back(i);
        project->setOpenSequenceIndices(std::move(allSequenceIndices));
    }

    const int migratedMaskCount = migrateLegacyMasks(*project);
    if (migratedMaskCount > 0) {
        spdlog::info(
            "ProjectSerializer: migrated {} legacy mask(s) to clip-local coordinates",
            migratedMaskCount);
    }

    // One-time frame-align migration (see alignLegacyClipsToFrameGrid).
    // v46+ files were edited on the canonical grid; re-snapping them on
    // every load would keep moving deliberate sub-frame audio placements.
    if (version < 46) {
        const int aligned = alignLegacyClipsToFrameGrid(*project);
        if (aligned > 0)
            spdlog::info("ProjectSerializer: frame-aligned {} clip(s) from a v{} file",
                         aligned, version);
    }

    project->setModified(false);
    return project;
}

} // namespace rt
