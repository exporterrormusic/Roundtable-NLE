/*
 * AudioPlaybackServiceCache.cpp — Decode cache and background prefetch for AudioPlaybackService.
 * Extracted from AudioPlaybackService.cpp for maintainability.
 */

#include "audio/AudioPlaybackService.h"

#include "audio/AudioFile.h"
#include "playback/PlaybackController.h"

#include "timeline/AudioClip.h"
#include "timeline/Timeline.h"
#include "timeline/Track.h"

#include <algorithm>
#include <future>
#include <spdlog/spdlog.h>

namespace rt {

// ─── Anonymous helpers ──────────────────────────────────────────────────────

namespace {

constexpr int64_t kTimelineAudioWindowBehindFrames       = 4 * 48000;
constexpr int64_t kTimelineAudioWindowAheadFrames        = 16 * 48000;
constexpr int64_t kAudioDecodePageFrames                 = 4 * 48000;

struct TimelineAudioWindow {
    int64_t startFrame{0};
    int64_t endFrame{0};
};

struct TimelineAudioRegion {
    int64_t timelineStartFrame{0};
    int64_t sourceStartFrame{0};
    int64_t sourceFrameCount{0};
    int64_t clipSourceOffsetFrames{0};
    int64_t fullClipSourceFrames{0};
};

struct CachedAudioPageRequest {
    std::string path;
    std::string cacheKey;
    int64_t startFrame{0};
    int64_t frameCount{0};
    int      audioStreamOrdinal{-1};
};

// MUST stay byte-identical to the copy in AudioPlaybackServiceLoad.cpp: this
// background thread populates m_decodeCache, loadSources reads it by this key.
std::string makeAudioPageCacheKey(const std::string& path, int audioStreamOrdinal,
                                  int64_t startFrame)
{
    return path + "|" + std::to_string(audioStreamOrdinal) + "|" + std::to_string(startFrame);
}

std::vector<CachedAudioPageRequest> makeAudioPageRequests(const std::string& path,
                                                          int audioStreamOrdinal,
                                                          int64_t startFrame,
                                                          int64_t frameCount)
{
    std::vector<CachedAudioPageRequest> requests;
    if (path.empty() || frameCount <= 0) return requests;

    const int64_t regionStartFrame = std::max<int64_t>(0, startFrame);
    const int64_t regionEndFrame   = regionStartFrame + frameCount;
    const int64_t firstPageStart   = (regionStartFrame / kAudioDecodePageFrames) * kAudioDecodePageFrames;

    for (int64_t pageStart = firstPageStart; pageStart < regionEndFrame; pageStart += kAudioDecodePageFrames) {
        requests.push_back(CachedAudioPageRequest{
            path,
            makeAudioPageCacheKey(path, audioStreamOrdinal, pageStart),
            pageStart,
            kAudioDecodePageFrames,
            audioStreamOrdinal
        });
    }
    return requests;
}

TimelineAudioWindow timelineAudioWindowForTick(int64_t tick)
{
    TimelineAudioWindow window;
    window.startFrame = std::max<int64_t>(0, tick - kTimelineAudioWindowBehindFrames);
    window.endFrame   = std::max(window.startFrame + 1, tick + kTimelineAudioWindowAheadFrames);
    return window;
}

std::vector<TimelineAudioWindow> prefetchTimelineAudioWindows(const PlaybackController* controller)
{
    const int64_t tick = controller ? std::max<int64_t>(0, controller->currentTick()) : 0;
    std::vector<TimelineAudioWindow> windows;
    windows.push_back(timelineAudioWindowForTick(tick));
    windows.push_back(timelineAudioWindowForTick(tick + (kTimelineAudioWindowAheadFrames / 2)));
    windows.push_back(timelineAudioWindowForTick(tick + kTimelineAudioWindowAheadFrames));
    return windows;
}

std::optional<TimelineAudioRegion> computeTimelineAudioRegion(const AudioClip& clip,
                                                              const TimelineAudioWindow* window)
{
    const int64_t clipTimelineStart = clip.timelineIn();
    const int64_t clipTimelineEnd   = clipTimelineStart + clip.duration();
    int64_t regionTimelineStart = clipTimelineStart;
    int64_t regionTimelineEnd   = clipTimelineEnd;

    if (window) {
        regionTimelineStart = std::max(regionTimelineStart, window->startFrame);
        regionTimelineEnd   = std::min(regionTimelineEnd, window->endFrame);
        if (regionTimelineEnd <= regionTimelineStart) return std::nullopt;
    }

    const double absSpeed = std::max(0.000001, std::abs(clip.speed()));
    const int64_t clipTimelineOffset  = std::max<int64_t>(0, regionTimelineStart - clipTimelineStart);
    const int64_t regionTimelineFrames = std::max<int64_t>(1, regionTimelineEnd - regionTimelineStart);
    const int64_t sourceStartFrame = clip.sourceIn() + static_cast<int64_t>(std::floor(
        static_cast<double>(clipTimelineOffset) * absSpeed));
    const int64_t sourceFrameCount = std::max<int64_t>(1, static_cast<int64_t>(std::ceil(
        static_cast<double>(regionTimelineFrames) * absSpeed)));

    TimelineAudioRegion region;
    region.timelineStartFrame    = regionTimelineStart;
    region.sourceStartFrame      = sourceStartFrame;
    region.sourceFrameCount      = sourceFrameCount;
    region.clipSourceOffsetFrames = std::max<int64_t>(0, sourceStartFrame - clip.sourceIn());
    region.fullClipSourceFrames  = std::max<int64_t>(1, static_cast<int64_t>(std::ceil(
        static_cast<double>(clip.duration()) * std::abs(clip.speed()))));
    return region;
}

} // anonymous namespace

// ─── Perf logging ───────────────────────────────────────────────────────────

void AudioPlaybackService::logPerfSnapshot(const char* reason)
{
    size_t residentBytes = 0;
    size_t entryCount    = 0;
    {
        std::lock_guard<std::mutex> lock(m_decodeMutex);
        entryCount = m_decodeCache.size();
        for (const auto& [key, entry] : m_decodeCache) {
            (void)key;
            residentBytes += entry.bytes;
        }
    }

    const uint64_t requests    = m_cacheRequests.load();
    const uint64_t hits        = m_cacheHits.load();
    const uint64_t blkMisses   = m_blockingMisses.load();
    const uint64_t defMisses   = m_deferredMisses.load();
    const uint64_t pfReqs      = m_prefetchRequests.load();
    const uint64_t pfBusy      = m_prefetchBusySkips.load();
    const uint64_t pfDone      = m_prefetchCompletions.load();
    const uint64_t pfInserts   = m_prefetchInsertions.load();
    const uint64_t misses      = blkMisses + defMisses;
    const double hitRate = requests > 0
        ? (100.0 * static_cast<double>(hits) / static_cast<double>(requests))
        : 0.0;

    spdlog::info("[PERF] Timeline audio cache [{}]: req={} hit={} miss={} blockingMiss={} deferredMiss={} "
                 "hitRate={:.1f}% entries={} residentMiB={:.1f} prefetchReq={} prefetchBusy={} "
                 "prefetchDone={} prefetchInsert={} window=[{}..{})",
                 reason, requests, hits, misses, blkMisses, defMisses, hitRate,
                 entryCount, static_cast<double>(residentBytes) / (1024.0 * 1024.0),
                 pfReqs, pfBusy, pfDone, pfInserts,
                 m_loadedWindowStartFrame, m_loadedWindowEndFrame);
}

// ─── Cache pruning ──────────────────────────────────────────────────────────

void AudioPlaybackService::pruneDecodeCacheLocked()
{
    size_t totalBytes = 0;
    struct EvictionCandidate {
        std::string key;
        uint64_t lastUseSerial{0};
        size_t bytes{0};
    };
    std::vector<EvictionCandidate> candidates;

    for (const auto& [key, entry] : m_decodeCache) {
        totalBytes += entry.bytes;
        if (entry.samples && entry.samples.use_count() == 1)
            candidates.push_back(EvictionCandidate{key, entry.lastUseSerial, entry.bytes});
    }

    if (totalBytes <= kDecodeCacheBudgetBytes) return;

    std::sort(candidates.begin(), candidates.end(),
              [](const EvictionCandidate& a, const EvictionCandidate& b) {
                  return a.lastUseSerial < b.lastUseSerial;
              });

    size_t reclaimedBytes = 0;
    size_t evictedCount   = 0;
    for (const auto& candidate : candidates) {
        if (totalBytes <= kDecodeCacheBudgetBytes) break;
        auto it = m_decodeCache.find(candidate.key);
        if (it == m_decodeCache.end() || !it->second.samples || it->second.samples.use_count() != 1) continue;
        totalBytes -= it->second.bytes;
        reclaimedBytes += it->second.bytes;
        ++evictedCount;
        m_decodeCache.erase(it);
    }

    if (reclaimedBytes > 0) {
        spdlog::info("pruneAudioDecodeCache: evicted {} page(s), freed {:.1f} MiB, remaining {:.1f} MiB",
                     evictedCount,
                     static_cast<double>(reclaimedBytes) / (1024.0 * 1024.0),
                     static_cast<double>(totalBytes) / (1024.0 * 1024.0));
    }
}

// ─── warmCacheAsync ─────────────────────────────────────────────────────────

void AudioPlaybackService::warmCacheAsync()
{
    if (!m_timeline) return;

    if (m_warmFuture.valid()) {
        if (m_warmFuture.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
            m_prefetchBusySkips.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        m_warmFuture.wait();
    }

    m_warmCancel.store(false);

    struct PendingPage {
        std::string path;
        std::string cacheKey;
        int64_t startFrame{0};
        int64_t frameCount{0};
        int      audioStreamOrdinal{-1};
    };
    std::vector<PendingPage> uncachedPages;
    const auto audioWindows = prefetchTimelineAudioWindows(m_playbackController);
    {
        std::lock_guard<std::mutex> lock(m_decodeMutex);
        for (const auto& audioWindow : audioWindows) {
            for (size_t ti = 0; ti < m_timeline->trackCount(); ++ti) {
                auto* track = m_timeline->track(ti);
                if (!track || track->type() != TrackType::Audio) continue;
                for (size_t ci = 0; ci < track->clipCount(); ++ci) {
                    auto* clip = track->clip(ci);
                    if (!clip) continue;
                    auto* audioClip = dynamic_cast<AudioClip*>(clip);
                    if (!audioClip) continue;
                    const auto& path = audioClip->mediaPath();
                    if (path.empty()) continue;
                    const auto region = computeTimelineAudioRegion(*audioClip, &audioWindow);
                    if (!region) continue;
                    const int audioOrdinal = audioClip->audioStreamIndex();
                    const auto pageRequests = makeAudioPageRequests(
                        path, audioOrdinal, region->sourceStartFrame, region->sourceFrameCount);
                    for (const auto& request : pageRequests) {
                        if (m_decodeCache.find(request.cacheKey) == m_decodeCache.end()) {
                            uncachedPages.push_back(PendingPage{
                                request.path, request.cacheKey,
                                request.startFrame, request.frameCount,
                                request.audioStreamOrdinal
                            });
                        }
                    }
                }
            }
        }
    }

    if (uncachedPages.empty()) return;

    // Deduplicate
    std::sort(uncachedPages.begin(), uncachedPages.end(),
              [](const PendingPage& a, const PendingPage& b) {
                  return a.path != b.path ? a.path < b.path : a.startFrame < b.startFrame;
              });
    uncachedPages.erase(
        std::unique(uncachedPages.begin(), uncachedPages.end(),
                    [](const PendingPage& a, const PendingPage& b) { return a.cacheKey == b.cacheKey; }),
        uncachedPages.end());

    const auto primaryWindow = audioWindows.front();
    m_prefetchRequests.fetch_add(static_cast<uint64_t>(uncachedPages.size()), std::memory_order_relaxed);
    spdlog::info("warmAudioCacheAsync: pre-decoding {} audio page(s) in background for windows [{}..{}) + lookahead",
                 uncachedPages.size(), primaryWindow.startFrame, primaryWindow.endFrame);

    m_warmFuture = std::async(std::launch::async,
        [this, pages = std::move(uncachedPages)]() {
            if (m_destroying.load(std::memory_order_acquire)) return;

            for (const auto& page : pages) {
                if (m_warmCancel.load()) break;

                // Unified per-path AudioFile cache — shared with the
                // synchronous loadSources(true) blocking-miss path
                // (UPGRADE_PLAN item 4) so a single source file is
                // probed once per project session, regardless of how
                // many code paths touch its audio.
                AudioFile* file = getOrOpenCachedAudioFile(page.path, page.audioStreamOrdinal);
                if (!file) continue;

                auto buffer = std::make_shared<std::vector<float>>();
                const int64_t framesRead = file->readRegionResampled(
                    page.startFrame, page.frameCount, 48000, *buffer);
                if (framesRead <= 0 || buffer->empty()) continue;

                CachedDecode entry;
                entry.startFrame  = page.startFrame;
                entry.channels    = static_cast<uint16_t>(file->info().channels);
                entry.samples     = buffer;
                entry.totalFrames = framesRead;
                entry.bytes       = buffer->size() * sizeof(float);

                {
                    std::lock_guard<std::mutex> lock(m_decodeMutex);
                    auto [it, inserted] = m_decodeCache.emplace(page.cacheKey, std::move(entry));
                    if (inserted) {
                        it->second.lastUseSerial = ++m_decodeUseSerial;
                        m_prefetchInsertions.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            }

            m_prefetchCompletions.fetch_add(1, std::memory_order_relaxed);
            spdlog::info("warmAudioCacheAsync: completed background pre-decode of {} page(s)",
                         pages.size());
        });
}

} // namespace rt
