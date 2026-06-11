/*
 * AudioPlaybackServiceLoad.cpp — Timeline audio source loading for AudioPlaybackService.
 * Extracted from AudioPlaybackService.cpp for maintainability.
 */

#include "media/AudioPlaybackService.h"

#include "media/AudioEngine.h"
#include "media/AudioFile.h"
#include "media/PlaybackController.h"

#include "effects/Effect.h"
#include "effects/EffectStack.h"

#include "timeline/AudioClip.h"
#include "timeline/SequenceClip.h"
#include "timeline/Timeline.h"
#include "timeline/Track.h"
#include "timeline/Transition.h"

#include "project/Project.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <optional>
#include <unordered_set>
#include <vector>
#include <spdlog/spdlog.h>

namespace rt {

// ─── Anonymous helpers (moved verbatim from AudioPlaybackService.cpp) ────────

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

struct TimelineAudioProviderState {
    std::shared_ptr<const std::vector<float>> buffer;
    int64_t totalFrames{0};
    int64_t startFrame{0};
    uint32_t channels{0};
    uint32_t sampleRate{48000};
    int64_t clipSourceOffsetFrames{0};
    int64_t fullClipSourceFrames{0};
};

struct CachedAudioPageRequest {
    std::string path;
    std::string cacheKey;
    int64_t startFrame{0};
    int64_t frameCount{0};
};

struct CachedAudioRegionView {
    std::shared_ptr<std::vector<float>> buffer;
    uint32_t channels{0};
    int64_t totalFrames{0};
};

/// Lock-free windowed sample provider — one per active audio clip.
class TimelineAudioWindowProvider final : public AudioSampleProvider {
public:
    [[nodiscard]] AudioSourceView currentView() const override
    {
        AudioSourceView view;
        auto state = m_state.load(std::memory_order_acquire);
        if (!state || !state->buffer || state->buffer->empty() || state->channels == 0 ||
            state->totalFrames <= 0) {
            return view;
        }
        view.buffer     = state->buffer;
        view.samples    = state->buffer->data();
        view.totalFrames = state->totalFrames;
        view.startFrame = state->startFrame;
        view.channels   = state->channels;
        view.sampleRate = state->sampleRate;
        return view;
    }

    void updateWindow(std::shared_ptr<const std::vector<float>> buffer,
                      int64_t startFrame, int64_t totalFrames,
                      uint32_t channels, uint32_t sampleRate,
                      int64_t clipSourceOffsetFrames, int64_t fullClipSourceFrames)
    {
        auto state = std::make_shared<TimelineAudioProviderState>();
        state->buffer               = std::move(buffer);
        state->totalFrames          = totalFrames;
        state->startFrame           = startFrame;
        state->channels             = channels;
        state->sampleRate           = sampleRate;
        state->clipSourceOffsetFrames = clipSourceOffsetFrames;
        state->fullClipSourceFrames   = fullClipSourceFrames;
        m_state.store(std::move(state), std::memory_order_release);
    }

    void clearWindow()
    {
        m_state.store(std::shared_ptr<const TimelineAudioProviderState>{}, std::memory_order_release);
    }

    [[nodiscard]] float clipFrameForNormalizedPosition(float pos) const
    {
        auto state = m_state.load(std::memory_order_acquire);
        if (!state || state->totalFrames <= 0) return -1.0f;
        return static_cast<float>(state->clipSourceOffsetFrames) +
               std::clamp(pos, 0.0f, 1.0f) * static_cast<float>(state->totalFrames);
    }

private:
    std::atomic<std::shared_ptr<const TimelineAudioProviderState>> m_state;
};

std::string makeAudioPageCacheKey(const std::string& path, int64_t startFrame)
{
    return path + "|" + std::to_string(startFrame);
}

std::vector<CachedAudioPageRequest> makeAudioPageRequests(const std::string& path,
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
            makeAudioPageCacheKey(path, pageStart),
            pageStart,
            kAudioDecodePageFrames
        });
    }
    return requests;
}

bool tryAssembleCachedAudioRegionLocked(auto& cache, uint64_t& useSerial,
                                        const std::vector<CachedAudioPageRequest>& requests,
                                        int64_t regionStartFrame, int64_t regionFrameCount,
                                        CachedAudioRegionView& out)
{
    if (requests.empty() || regionFrameCount <= 0) return false;

    uint32_t channels = 0;
    std::shared_ptr<std::vector<float>> singlePageBuffer;
    int64_t singlePageStartFrame = 0;
    int64_t singlePageTotalFrames = 0;
    for (const auto& request : requests) {
        auto it = cache.find(request.cacheKey);
        if (it == cache.end() || !it->second.samples || it->second.samples->empty() || it->second.totalFrames <= 0)
            return false;
        if (channels == 0)
            channels = it->second.channels;
        else if (channels != it->second.channels)
            return false;
        it->second.lastUseSerial = ++useSerial;
        if (requests.size() == 1) {
            singlePageBuffer     = it->second.samples;
            singlePageStartFrame = it->second.startFrame;
            singlePageTotalFrames = it->second.totalFrames;
        }
    }

    if (requests.size() == 1 &&
        singlePageStartFrame == regionStartFrame &&
        singlePageTotalFrames == regionFrameCount) {
        out.buffer     = std::move(singlePageBuffer);
        out.channels   = channels;
        out.totalFrames = regionFrameCount;
        return true;
    }

    auto stitched = std::make_shared<std::vector<float>>();
    stitched->resize(static_cast<size_t>(regionFrameCount * channels));

    size_t copiedFrames = 0;
    const int64_t regionEndFrame = regionStartFrame + regionFrameCount;
    for (const auto& request : requests) {
        auto it = cache.find(request.cacheKey);
        if (it == cache.end() || !it->second.samples || it->second.totalFrames <= 0) return false;

        const int64_t pageStart = request.startFrame;
        const int64_t pageEnd   = request.startFrame + it->second.totalFrames;
        const int64_t copyStart = std::max<int64_t>(regionStartFrame, pageStart);
        const int64_t copyEnd   = std::min<int64_t>(regionEndFrame, pageEnd);
        if (copyEnd <= copyStart) continue;

        const size_t frameOffset  = static_cast<size_t>(copyStart - pageStart);
        const size_t framesToCopy = static_cast<size_t>(copyEnd - copyStart);
        const size_t srcOffset = frameOffset * channels;
        const size_t dstOffset = copiedFrames * channels;
        std::memcpy(stitched->data() + dstOffset,
                    it->second.samples->data() + srcOffset,
                    framesToCopy * channels * sizeof(float));
        copiedFrames += framesToCopy;
    }

    if (copiedFrames == 0) return false;
    if (copiedFrames != static_cast<size_t>(regionFrameCount))
        stitched->resize(copiedFrames * channels);

    out.buffer     = std::move(stitched);
    out.channels   = channels;
    out.totalFrames = static_cast<int64_t>(copiedFrames);
    return true;
}

int64_t requestedClipSourceFrames(const AudioClip& clip)
{
    return static_cast<int64_t>(std::ceil(
        static_cast<double>(clip.duration()) * std::abs(clip.speed())));
}

TimelineAudioWindow timelineAudioWindowForTick(int64_t tick)
{
    TimelineAudioWindow window;
    window.startFrame = std::max<int64_t>(0, tick - kTimelineAudioWindowBehindFrames);
    window.endFrame   = std::max(window.startFrame + 1, tick + kTimelineAudioWindowAheadFrames);
    return window;
}

TimelineAudioWindow currentTimelineAudioWindow(const PlaybackController* controller)
{
    const int64_t tick = controller ? std::max<int64_t>(0, controller->currentTick()) : 0;
    return timelineAudioWindowForTick(tick);
}

bool shouldLogAudioPerfSnapshot(uint64_t value)
{
    return value > 0 && (value % 16) == 0;
}

// Compute how many ticks BEFORE this clip's natural start and AFTER its
// natural end we should pre-roll source samples so cross-dissolve
// transitions can actually crossfade (instead of fading down to silence
// then back up). Only true cross-dissolves — those with BOTH a left and
// a right clip — request extension; pure fade-in/fade-out transitions
// fit inside the clip's range already.
struct ClipTransitionExtension {
    int64_t beforeTicks{0};
    int64_t afterTicks{0};
};
ClipTransitionExtension computeClipTransitionExtension(const Track* track, uint64_t clipId)
{
    ClipTransitionExtension ext;
    if (!track) return ext;
    for (size_t i = 0; i < track->transitionCount(); ++i) {
        const Transition* t = track->transition(i);
        if (!t) continue;
        // Need both sides for a true crossfade — pure fades don't need handles.
        if (t->leftClipId == 0 || t->rightClipId == 0) continue;
        int64_t tStart, tEnd;
        t->getRange(tStart, tEnd);
        if (t->leftClipId == clipId) {
            const int64_t after = tEnd - t->editPointTick;
            if (after > ext.afterTicks) ext.afterTicks = after;
        }
        if (t->rightClipId == clipId) {
            const int64_t before = t->editPointTick - tStart;
            if (before > ext.beforeTicks) ext.beforeTicks = before;
        }
    }
    return ext;
}

std::optional<TimelineAudioRegion> computeTimelineAudioRegion(const AudioClip& clip,
                                                              const TimelineAudioWindow* window,
                                                              int64_t extendBeforeTicks = 0,
                                                              int64_t extendAfterTicks  = 0)
{
    const int64_t clipTimelineStart = clip.timelineIn();
    const int64_t clipTimelineEnd   = clipTimelineStart + clip.duration();

    // Expand the clip's playable timeline range to cover cross-dissolve
    // handles, then let the window/source-file edges clip back.
    int64_t regionTimelineStart = clipTimelineStart - extendBeforeTicks;
    int64_t regionTimelineEnd   = clipTimelineEnd   + extendAfterTicks;

    if (window) {
        regionTimelineStart = std::max(regionTimelineStart, window->startFrame);
        regionTimelineEnd   = std::min(regionTimelineEnd, window->endFrame);
        if (regionTimelineEnd <= regionTimelineStart) return std::nullopt;
    }

    const double absSpeed = std::max(0.000001, std::abs(clip.speed()));
    // clipTimelineOffset is NEGATIVE when the region pre-rolls before
    // the clip's natural start. The source frame can correspondingly
    // sit before sourceIn — clamp to file start if there's not enough
    // source handle, and shift regionTimelineStart to stay aligned.
    int64_t clipTimelineOffset  = regionTimelineStart - clipTimelineStart;
    int64_t sourceStartFrame = clip.sourceIn() + static_cast<int64_t>(std::floor(
        static_cast<double>(clipTimelineOffset) * absSpeed));
    if (sourceStartFrame < 0) {
        const int64_t deficitSrc = -sourceStartFrame;
        const int64_t shiftTicks = static_cast<int64_t>(std::ceil(
            static_cast<double>(deficitSrc) / absSpeed));
        regionTimelineStart += shiftTicks;
        clipTimelineOffset   = regionTimelineStart - clipTimelineStart;
        sourceStartFrame = clip.sourceIn() + static_cast<int64_t>(std::floor(
            static_cast<double>(clipTimelineOffset) * absSpeed));
        if (sourceStartFrame < 0) sourceStartFrame = 0;
        if (regionTimelineEnd <= regionTimelineStart) return std::nullopt;
    }
    const int64_t regionTimelineFrames = std::max<int64_t>(1, regionTimelineEnd - regionTimelineStart);
    const int64_t sourceFrameCount = std::max<int64_t>(1, static_cast<int64_t>(std::ceil(
        static_cast<double>(regionTimelineFrames) * absSpeed)));

    TimelineAudioRegion region;
    region.timelineStartFrame    = regionTimelineStart;
    region.sourceStartFrame      = sourceStartFrame;
    region.sourceFrameCount      = sourceFrameCount;
    // Can be NEGATIVE when the buffer starts before sourceIn — the
    // fade-envelope math relies on this so a cross-dissolve fade-in
    // ramps up over the pre-roll handle.
    region.clipSourceOffsetFrames = sourceStartFrame - clip.sourceIn();
    region.fullClipSourceFrames  = std::max<int64_t>(1, requestedClipSourceFrames(clip));
    return region;
}

// ── Nested-sequence audio expansion ─────────────────────────────────────────
//
// A SequenceClip on an audio track represents the inner sequence's mixed
// audio. The AudioEngine only understands AudioClips, so we recursively
// expand each SequenceClip into synthetic AudioClips mapped from the inner
// sequence's time space into the host (parent) time space, using the same
// in/out + offset math the video compositor uses for nested video.
//
// Synthetic clips get a deterministic id derived from the outer SequenceClip
// id and the inner clip id so their playback provider stays stable across
// reloads (no provider churn → no audio glitches).

constexpr int kMaxNestDepth = 4;

inline uint64_t mixNestId(uint64_t outer, uint64_t inner)
{
    // 64-bit splitmix-style hash so synthetic ids don't collide with real
    // clip ids or with each other across nesting levels.
    uint64_t x = outer * 0x9E3779B97F4A7C15ull;
    x ^= (inner + 0x165667B19E3779F9ull);
    x ^= x >> 30; x *= 0xBF58476D1CE4E5B9ull;
    x ^= x >> 27; x *= 0x94D049BB133111EBull;
    x ^= x >> 31;
    return x | 0x8000000000000000ull; // set high bit: clearly synthetic
}

void expandSequenceAudio(const SequenceClip& outerSeq,
                         const Timeline* hostTimeline,
                         Project* project,
                         int depth,
                         std::vector<std::unique_ptr<AudioClip>>& out)
{
    if (!project || depth >= kMaxNestDepth) return;
    Timeline* inner = project->sequence(outerSeq.sequenceIndex());
    if (!inner || inner == hostTimeline) return;

    const int64_t T_o = outerSeq.timelineIn();
    const int64_t D_o = outerSeq.duration();
    const int64_t S_o = outerSeq.sourceIn();
    const int64_t innerSrcIn  = S_o;
    const int64_t innerSrcOut = S_o + D_o;

    // Map a clip living at [T_i, T_i+D_i) (inner-sequence time, source S_i)
    // into the outer host time space. Returns false if it falls entirely
    // outside the outer clip's visible window.
    auto mapToOuter = [&](int64_t T_i, int64_t D_i, int64_t S_i,
                          int64_t& outIn, int64_t& outDur, int64_t& outSrcIn) {
        const int64_t visStart = std::max(T_i, innerSrcIn);
        const int64_t visEnd   = std::min(T_i + D_i, innerSrcOut);
        if (visEnd <= visStart) return false;
        outIn    = T_o + (visStart - innerSrcIn);
        outDur   = visEnd - visStart;
        outSrcIn = S_i + (visStart - T_i);
        return true;
    };

    for (size_t ti = 0; ti < inner->trackCount(); ++ti) {
        auto* trk = inner->track(ti);
        if (!trk || trk->type() != TrackType::Audio || trk->isMuted())
            continue;

        for (size_t ci = 0; ci < trk->clipCount(); ++ci) {
            auto* c = trk->clip(ci);
            if (!c || !c->isEnabled()) continue;

            if (auto* ac = dynamic_cast<AudioClip*>(c)) {
                int64_t nIn, nDur, nSrcIn;
                if (!mapToOuter(ac->timelineIn(), ac->duration(),
                                ac->sourceIn(), nIn, nDur, nSrcIn))
                    continue;
                auto cloneBase = ac->clone();
                auto* synth = dynamic_cast<AudioClip*>(cloneBase.get());
                if (!synth) continue;
                synth->setTimelineIn(nIn);
                synth->setDuration(nDur);
                synth->setSourceIn(nSrcIn);
                synth->setId(mixNestId(outerSeq.id(), ac->id()));

                // Bake inner-sequence audio crossfades. The parent track
                // has no transitions for this synthetic clip (its id is
                // synthetic), so the main loop's transition-envelope path
                // can't see the inner crossfade. Decompose each inner
                // transition the standard way: the clip on the left of the
                // transition fades OUT over the overlap, the clip on the
                // right fades IN over it. The main loop then turns these
                // fade durations into a playback gain envelope.
                for (size_t tri = 0; tri < trk->transitionCount(); ++tri) {
                    const Transition* tr = trk->transition(tri);
                    if (!tr) continue;
                    int64_t tS, tE;
                    tr->getRange(tS, tE);
                    const int64_t tDur = tE - tS;
                    if (tDur <= 0) continue;
                    if (tr->leftClipId == ac->id())
                        synth->setFadeOutDuration(
                            std::max<int64_t>(synth->fadeOutDuration(), tDur));
                    if (tr->rightClipId == ac->id())
                        synth->setFadeInDuration(
                            std::max<int64_t>(synth->fadeInDuration(), tDur));
                }

                out.push_back(std::unique_ptr<AudioClip>(
                    static_cast<AudioClip*>(cloneBase.release())));
            }
            else if (auto* sc = dynamic_cast<SequenceClip*>(c)) {
                // Recurse: get the deeper sequence's synthetics in *inner*
                // time, then fold them through this outer clip's window.
                std::vector<std::unique_ptr<AudioClip>> deeper;
                expandSequenceAudio(*sc, inner, project, depth + 1, deeper);
                for (auto& d : deeper) {
                    int64_t nIn, nDur, nSrcIn;
                    if (!mapToOuter(d->timelineIn(), d->duration(),
                                    d->sourceIn(), nIn, nDur, nSrcIn))
                        continue;
                    d->setTimelineIn(nIn);
                    d->setDuration(nDur);
                    d->setSourceIn(nSrcIn);
                    d->setId(mixNestId(outerSeq.id(), d->id()));
                    out.push_back(std::move(d));
                }
            }
        }
    }
}

} // anonymous namespace

// ─── loadSources ────────────────────────────────────────────────────────────

void AudioPlaybackService::loadSources(bool allowBlockingMisses)
{
    if (!m_timeline || !m_audioEngine) {
        spdlog::warn("loadAudioSources: skipped - timeline={} audioEngine={}",
                     (void*)m_timeline, (void*)m_audioEngine);
        return;
    }

    const bool rebuildSourceList = !m_sourcesLoaded || m_topologyDirty;
    std::vector<std::shared_ptr<std::vector<float>>> newBuffers;
    std::vector<AudioTrackSource> sources;
    std::unordered_set<uint64_t> activeClipIds;
    const TimelineAudioWindow audioWindow = currentTimelineAudioWindow(m_playbackController);
    size_t deferredCacheMisses = 0;

    // Keeps synthetic AudioClips (from expanding nested-sequence clips on
    // audio tracks) alive for the duration of this call. The build loop
    // only reads them synchronously into AudioTrackSources, so function
    // scope is sufficient.
    std::vector<std::unique_ptr<AudioClip>> synthStorage;

    spdlog::debug("loadAudioSources: scanning {} tracks in window [{}..{})",
                  m_timeline->trackCount(), audioWindow.startFrame, audioWindow.endFrame);

    for (size_t ti = 0; ti < m_timeline->trackCount(); ++ti) {
        auto* track = m_timeline->track(ti);
        if (!track || track->type() != TrackType::Audio) continue;

        spdlog::debug("loadAudioSources: track {} '{}' has {} clips",
                     ti, track->name(), track->clipCount());

        // Build the effective AudioClip list for this track: real audio
        // clips plus the synthetic clips that any nested SequenceClip on
        // this track expands into.
        std::vector<AudioClip*> effectiveClips;
        for (size_t ci = 0; ci < track->clipCount(); ++ci) {
            auto* tc = track->clip(ci);
            if (!tc || !tc->isEnabled()) continue;
            if (auto* ac = dynamic_cast<AudioClip*>(tc)) {
                effectiveClips.push_back(ac);
            } else if (auto* sc = dynamic_cast<SequenceClip*>(tc)) {
                std::vector<std::unique_ptr<AudioClip>> expanded;
                expandSequenceAudio(*sc, m_timeline, m_project, 0, expanded);
                for (auto& e : expanded) {
                    effectiveClips.push_back(e.get());
                    synthStorage.push_back(std::move(e));
                }
            }
        }

        int audioClipsLoaded = 0;
        for (auto* audioClip : effectiveClips) {
            if (!audioClip) continue;

            const auto& path = audioClip->mediaPath();
            if (path.empty()) continue;

            if (audioClipsLoaded == 0) {
                spdlog::debug("  clip {} mediaPath='{}' in={} dur={}",
                             audioClipsLoaded, path, audioClip->timelineIn(),
                             audioClip->duration());
            }

            const uint64_t clipId = audioClip->id();
            activeClipIds.insert(clipId);
            auto providerIt = m_clipProviders.find(clipId);
            if (providerIt == m_clipProviders.end()) {
                providerIt = m_clipProviders.emplace(
                    clipId, std::make_shared<TimelineAudioWindowProvider>()).first;
            }
            auto provider = std::static_pointer_cast<TimelineAudioWindowProvider>(providerIt->second);

            // Pre-roll source handles on either side so cross-dissolve
            // transitions can actually overlap both clips' audio (without
            // this, Ctrl+T crossfade audibly faded to silence at the cut).
            const auto clipExt = computeClipTransitionExtension(track, clipId);
            const auto region = computeTimelineAudioRegion(
                *audioClip, &audioWindow,
                clipExt.beforeTicks, clipExt.afterTicks);
            std::shared_ptr<std::vector<float>> buffer;
            uint32_t ch = 0;
            int64_t cachedFrames = 0;
            if (region) {
                const uint64_t requestCount = m_cacheRequests.fetch_add(1, std::memory_order_relaxed) + 1;
                const auto pageRequests = makeAudioPageRequests(
                    path, region->sourceStartFrame, region->sourceFrameCount);
                {
                    std::lock_guard<std::mutex> lock(m_decodeMutex);
                    CachedAudioRegionView cachedRegion;
                    if (tryAssembleCachedAudioRegionLocked(
                            m_decodeCache, m_decodeUseSerial,
                            pageRequests, region->sourceStartFrame, region->sourceFrameCount,
                            cachedRegion)) {
                        m_cacheHits.fetch_add(1, std::memory_order_relaxed);
                        buffer       = std::move(cachedRegion.buffer);
                        ch           = cachedRegion.channels;
                        cachedFrames = cachedRegion.totalFrames;
                    }
                }

                if (!buffer) {
                    if (!allowBlockingMisses) {
                        m_deferredMisses.fetch_add(1, std::memory_order_relaxed);
                        ++deferredCacheMisses;
                    } else {
                        m_blockingMisses.fetch_add(1, std::memory_order_relaxed);

                        // UPGRADE_PLAN item 4 (2026-05-22): use the
                        // unified per-path AudioFile cache instead of
                        // constructing a stack AudioFile each call.
                        // Previously this path opened the same media
                        // file repeatedly (4 log lines per open — see
                        // BLUE_1.mp4 sequence at 20:43:46-48 in
                        // logs/perf_log.txt); each open ran sndfile +
                        // FFmpeg probes (~200-500 ms total) while
                        // holding shared state needed by the audio
                        // callback, which produced the audio-thread
                        // 3.4 s stall and the subsequent
                        // COMPOSITE-SLOW cascade.  Cache lookup is
                        // near-zero cost on the steady state.
                        AudioFile* file = getOrOpenCachedAudioFile(path);
                        if (!file) {
                            // getOrOpenCachedAudioFile already logged
                            // the open failure; no need to spam here.
                        } else {
                            std::vector<CachedAudioPageRequest> missingPages;
                            {
                                std::lock_guard<std::mutex> lock(m_decodeMutex);
                                for (const auto& request : pageRequests) {
                                    if (m_decodeCache.find(request.cacheKey) == m_decodeCache.end())
                                        missingPages.push_back(request);
                                }
                            }

                            for (const auto& request : missingPages) {
                                auto pageBuffer = std::make_shared<std::vector<float>>();
                                const int64_t framesRead = file->readRegionResampled(
                                    request.startFrame, request.frameCount, 48000, *pageBuffer);
                                if (framesRead <= 0 || pageBuffer->empty()) continue;

                                CachedDecode entry;
                                entry.samples    = pageBuffer;
                                entry.startFrame = request.startFrame;
                                entry.channels   = static_cast<uint16_t>(file->info().channels);
                                entry.totalFrames = framesRead;
                                entry.bytes      = pageBuffer->size() * sizeof(float);

                                {
                                    std::lock_guard<std::mutex> lock(m_decodeMutex);
                                    auto [it, inserted] = m_decodeCache.emplace(request.cacheKey, std::move(entry));
                                    it->second.lastUseSerial = ++m_decodeUseSerial;
                                    if (inserted) pruneDecodeCacheLocked();
                                }
                            }

                            {
                                std::lock_guard<std::mutex> lock(m_decodeMutex);
                                CachedAudioRegionView cachedRegion;
                                if (tryAssembleCachedAudioRegionLocked(
                                        m_decodeCache, m_decodeUseSerial,
                                        pageRequests, region->sourceStartFrame, region->sourceFrameCount,
                                        cachedRegion)) {
                                    buffer       = std::move(cachedRegion.buffer);
                                    ch           = cachedRegion.channels;
                                    cachedFrames = cachedRegion.totalFrames;
                                }
                            }
                        }
                    }
                }

                if (shouldLogAudioPerfSnapshot(requestCount))
                    logPerfSnapshot(allowBlockingMisses ? "sync-load" : "playback-refresh");

                if (buffer && !buffer->empty()) {
                    newBuffers.push_back(buffer);

                    if (ch == 2) {
                        const auto& fxStack = audioClip->effects();
                        for (size_t ei = 0; ei < fxStack.effectCount(); ++ei) {
                            const auto& fx = fxStack.effect(ei);
                            if (!fx.isEnabled()) continue;
                            if (fx.effectType() == EffectType::FillLeftWithRight ||
                                fx.effectType() == EffectType::FillRightWithLeft) {
                                auto modified = std::make_shared<std::vector<float>>(*buffer);
                                const size_t numSamples = modified->size();
                                float* d = modified->data();
                                if (fx.effectType() == EffectType::FillLeftWithRight) {
                                    for (size_t s = 0; s < numSamples; s += 2) d[s] = d[s + 1];
                                } else {
                                    for (size_t s = 0; s < numSamples; s += 2) d[s + 1] = d[s];
                                }
                                buffer = std::move(modified);
                                newBuffers.push_back(buffer);
                            }
                        }
                    }

                    // ── Per-clip audio FX chain (EQ / dynamics) ─────────
                    // Real-time preview parity with export's AudioMixdown:
                    // clone the clip's chain and bake it into the window
                    // buffer here on the load thread — the audio callback
                    // then mixes pre-processed samples, so the RT path
                    // stays allocation-free and the user hears the same
                    // EQ/dynamics the export renders.  COPY before
                    // processing: `buffer` may alias a shared decode-cache
                    // page (same reason the channel-fill above copies).
                    //
                    // Window-boundary caveat: each window starts with a
                    // cold processor state (filters/envelopes don't carry
                    // across window swaps).  The window includes the
                    // transition pre-roll handles, so state has the head
                    // of the region to converge over; for typical EQ /
                    // dynamics settings this is inaudible.
                    if (ch > 0 && audioClip->audioFx().isActive()) {
                        auto processed =
                            std::make_shared<std::vector<float>>(*buffer);
                        auto chain = audioClip->audioFx().clone();
                        chain.prepare(48000.0, static_cast<int>(ch));
                        chain.process(processed->data(),
                                      static_cast<int>(processed->size() / ch));
                        buffer = std::move(processed);
                        newBuffers.push_back(buffer);
                    }
                }
            }

            const int64_t totalRegionFrames = (buffer && ch > 0)
                ? (cachedFrames > 0 ? cachedFrames : static_cast<int64_t>(buffer->size() / ch))
                : 0;

            if (buffer && ch > 0 && totalRegionFrames > 0 && region) {
                audioClip->setChannels(static_cast<uint16_t>(ch));
                provider->updateWindow(buffer,
                                       region->timelineStartFrame,
                                       totalRegionFrames, ch, 48000,
                                       region->clipSourceOffsetFrames,
                                       region->fullClipSourceFrames);
            } else if (allowBlockingMisses || !m_sourcesLoaded) {
                provider->clearWindow();
            }

            if (rebuildSourceList) {
                AudioTrackSource src;
                src.trackId         = clipId;
                src.sampleProvider  = provider;
                src.startFrame      = audioClip->timelineIn();
                src.channels        = ch > 0 ? ch : 2;
                src.sampleRate      = 48000;
                src.volume          = track->volume() * audioClip->volume().evaluate(0);
                src.pan             = std::clamp(track->pan() + audioClip->pan().evaluate(0), -1.0f, 1.0f);
                src.muted           = track->isMuted();
                src.solo            = track->isSoloed();
                src.maintainPitch   = audioClip->maintainPitch();
                src.clipSpeed       = audioClip->speed();

                // Copy active audio effects for real-time playback
                const auto& fxStack = audioClip->effects();
                for (size_t ei = 0; ei < fxStack.effectCount(); ++ei) {
                    const auto& fx = fxStack.effect(ei);
                    if (fx.isEnabled() && isAudioEffect(fx.effectType()))
                        src.audioEffects.push_back(fx.effectType());
                }

                const int64_t tlIn = audioClip->timelineIn();
                const int64_t fullClipSourceFrames = std::max<int64_t>(1, requestedClipSourceFrames(*audioClip));
                for (size_t trI = 0; trI < track->transitionCount(); ++trI) {
                    const Transition* trans = track->transition(trI);
                    if (!trans) continue;
                    int64_t tStart, tEnd;
                    trans->getRange(tStart, tEnd);
                    const int64_t tDur = tEnd - tStart;
                    if (tDur <= 0) continue;

                    // NOTE: fadeStartFrame / fadeEndFrame are intentionally
                    // NOT clamped to [0, fullClipSourceFrames]. A cross-dissolve
                    // transition straddles the cut, so for the LEFT clip
                    // fadeEnd lies PAST the clip's natural end (we pre-rolled
                    // a source handle for it above), and for the RIGHT clip
                    // fadeStart lies BEFORE its natural start. Clamping those
                    // collapsed the crossfade into a fade-out-to-silence-then-
                    // fade-in, which is what users were hearing.
                    if (trans->leftClipId == clipId || trans->rightClipId == clipId) {
                        // DIAG-XFADE: print the fade range vs the clip's own
                        // length so a "completely mute" cross-dissolved clip
                        // can be traced — if [fadeStart,fadeEnd] spans most of
                        // [0, fullClipSourceFrames], the whole clip is faded.
                        // debug-level: the mute-crossfade investigation is
                        // resolved (see the un-clamped fade NOTE above); this
                        // was dumping every transition on every window rebuild
                        // into the warn-filtered perf log.
                        spdlog::debug("DIAG-XFADE clipId={} side={} editPt={} tRange=[{},{}] "
                                     "tlIn={} fade=[{},{}] clipLen={} ({:.3f}s)",
                                     clipId,
                                     trans->leftClipId == clipId ? "LEFT" : "RIGHT",
                                     trans->editPointTick, tStart, tEnd, tlIn,
                                     tStart - tlIn, tEnd - tlIn,
                                     fullClipSourceFrames, fullClipSourceFrames / 48000.0);
                    }
                    if (trans->leftClipId == clipId) {
                        const float fadeStartFrame = static_cast<float>(tStart - tlIn);
                        const float fadeEndFrame   = static_cast<float>(tEnd   - tlIn);
                        auto prevEnv = src.fadeEnvelope;
                        src.fadeEnvelope = [prevEnv, fadeStartFrame, fadeEndFrame, provider](float pos) {
                            float v = 1.0f;
                            const float clipFrame = provider->clipFrameForNormalizedPosition(pos);
                            if (clipFrame >= fadeStartFrame && fadeEndFrame > fadeStartFrame) {
                                const float t = (clipFrame - fadeStartFrame) / (fadeEndFrame - fadeStartFrame);
                                v = 1.0f - std::clamp(t, 0.0f, 1.0f);
                            }
                            return prevEnv ? prevEnv(pos) * v : v;
                        };
                    }
                    if (trans->rightClipId == clipId) {
                        const float fadeStartFrame = static_cast<float>(tStart - tlIn);
                        const float fadeEndFrame   = static_cast<float>(tEnd   - tlIn);
                        auto prevEnv = src.fadeEnvelope;
                        src.fadeEnvelope = [prevEnv, fadeStartFrame, fadeEndFrame, provider](float pos) {
                            float v = 1.0f;
                            const float clipFrame = provider->clipFrameForNormalizedPosition(pos);
                            if (clipFrame <= fadeEndFrame && fadeEndFrame > fadeStartFrame) {
                                const float t = (clipFrame - fadeStartFrame) / (fadeEndFrame - fadeStartFrame);
                                v = std::clamp(t, 0.0f, 1.0f);
                            }
                            return prevEnv ? prevEnv(pos) * v : v;
                        };
                    }
                }

                // Per-clip fade in/out. Track transitions handle clip-to-
                // clip crossfades; this handles a clip's own fade handles
                // AND the crossfades baked into synthetic nested-sequence
                // clips by expandSequenceAudio() (whose fades can't appear
                // as parent-track transitions). Composed multiplicatively
                // with any transition envelope already set above.
                {
                    const float fadeInF  =
                        static_cast<float>(audioClip->fadeInDuration());
                    const float fadeOutF =
                        static_cast<float>(audioClip->fadeOutDuration());
                    const float clipLenF =
                        static_cast<float>(fullClipSourceFrames);
                    if (fadeInF > 0.0f || fadeOutF > 0.0f) {
                        auto prevEnv = src.fadeEnvelope;
                        src.fadeEnvelope =
                            [prevEnv, fadeInF, fadeOutF, clipLenF, provider](float pos) {
                            float v = 1.0f;
                            const float cf =
                                provider->clipFrameForNormalizedPosition(pos);
                            if (cf >= 0.0f) {
                                if (fadeInF > 0.0f && cf < fadeInF)
                                    v *= std::clamp(cf / fadeInF, 0.0f, 1.0f);
                                if (fadeOutF > 0.0f &&
                                    cf > clipLenF - fadeOutF) {
                                    const float t =
                                        (clipLenF - cf) / fadeOutF;
                                    v *= std::clamp(t, 0.0f, 1.0f);
                                }
                            }
                            return prevEnv ? prevEnv(pos) * v : v;
                        };
                    }
                }

                sources.push_back(std::move(src));
            }

            ++audioClipsLoaded;
        }
    }

    if (rebuildSourceList) {
        for (auto it = m_clipProviders.begin(); it != m_clipProviders.end(); ) {
            if (activeClipIds.find(it->first) == activeClipIds.end())
                it = m_clipProviders.erase(it);
            else
                ++it;
        }
        spdlog::info("loadAudioSources: rebuilt {} audio track source(s)", sources.size());
        m_audioEngine->setTrackSources(std::move(sources));
        m_topologyDirty = false;
    } else {
        spdlog::info("loadAudioSources: refreshed audio window for {} provider(s)", activeClipIds.size());
    }

    if (!allowBlockingMisses && deferredCacheMisses > 0) {
        spdlog::info("loadAudioSources: deferred {} cache miss(es) to background audio prefetch",
                     deferredCacheMisses);
        logPerfSnapshot("deferred-miss");
    }

    m_audioEngine->resetStretchers();
    m_audioBuffers = std::move(newBuffers);
    m_sourcesLoaded          = true;

    // Audio dropout fix (2026-05-21): only advance the loaded-window
    // markers when the load actually populated all clips' buffers. If
    // we ran with allowBlockingMisses=false (the periodic playback
    // refresh path) AND any clip had a cache miss that got deferred to
    // the async warm, the affected providers still hold their OLD
    // window. Recording the NEW audioWindow here would suppress
    // needsPlaybackWindowRefresh until the playhead crossed the new
    // window edge — by which point we'd be many seconds into silent
    // playback. Leave the markers unchanged so the next position
    // change re-triggers the refresh; once warmCacheAsync completes
    // and the cache is populated, that retry succeeds and audio
    // resumes in sync.
    if (allowBlockingMisses || deferredCacheMisses == 0) {
        m_loadedWindowStartFrame = audioWindow.startFrame;
        m_loadedWindowEndFrame   = audioWindow.endFrame;
    }
}

} // namespace rt
