/*
 * AudioPlaybackService.cpp - State management for timeline audio playback.
 * Load, cache, and prefetch extracted to AudioPlaybackServiceLoad.cpp
 * and AudioPlaybackServiceCache.cpp.
 */

#include "audio/AudioPlaybackService.h"

#include "audio/AudioEngine.h"
#include "audio/AudioFile.h"
#include "playback/PlaybackController.h"

#include <spdlog/spdlog.h>

namespace rt {

// ---- Anonymous helpers ---------------------------------------------------

namespace {

constexpr int64_t kTimelineAudioWindowBehindFrames       = 4 * 48000;
constexpr int64_t kTimelineAudioWindowAheadFrames        = 16 * 48000;
constexpr int64_t kTimelineAudioWindowRefreshMarginFrames = 1500 * 48;

bool isTimelineAudioWindowValid(int64_t startFrame, int64_t endFrame, int64_t)
{
    return (startFrame >= 0 && endFrame > startFrame);
}

bool shouldRefreshTimelineAudioWindow(int64_t startFrame, int64_t endFrame, int64_t tick)
{
    if (!isTimelineAudioWindowValid(startFrame, endFrame, tick)) return true;
    if (tick < startFrame || tick >= endFrame) return true;

    bool nearStart = (startFrame > 0) &&
                     (tick <= startFrame + kTimelineAudioWindowRefreshMarginFrames);
    bool nearEnd   = tick >= (endFrame - kTimelineAudioWindowRefreshMarginFrames);
    return nearStart || nearEnd;
}

} // anonymous namespace

// ---- Construction / destruction -------------------------------------------

AudioPlaybackService::AudioPlaybackService() = default;

AudioPlaybackService::~AudioPlaybackService()
{
    m_destroying.store(true);
    cancelWarm();
    waitForWarm();
    // Safe to clear now: waitForWarm() guarantees no warm task is in
    // flight, so no thread is dereferencing AudioFile pointers from
    // m_warmFiles.  Order matters — clearing before waitForWarm() would
    // free an AudioFile mid-read.
    {
        std::lock_guard<std::mutex> lock(m_warmFilesMutex);
        m_warmFiles.clear();
    }
}

// ---- State management ------------------------------------------------------

void AudioPlaybackService::invalidateSources()
{
    m_sourcesLoaded          = false;
    m_topologyDirty          = true;
    m_loadedWindowStartFrame = -1;
    m_loadedWindowEndFrame   = -1;

    if (m_audioEngine)
        m_audioEngine->clearTrackSources();
}

void AudioPlaybackService::updateClipLevels(uint64_t clipId, float volume, float pan, bool muted)
{
    if (m_audioEngine)
        m_audioEngine->updateSourceLevels(clipId, volume, pan, muted);
}

void AudioPlaybackService::reset()
{
    cancelWarm();
    waitForWarm();
    m_warmCancel.store(false);

    m_sourcesLoaded  = false;
    m_topologyDirty  = true;
    m_clipProviders.clear();

    {
        std::lock_guard<std::mutex> lock(m_decodeMutex);
        m_decodeCache.clear();
        m_decodeUseSerial = 0;
    }

    // Drop persistent AudioFile handles — typically called on project
    // open / close, where the path set is about to change anyway, so
    // holding stale opens wastes file handles and would mask a file
    // having been moved or replaced on disk.  Safe to clear here for
    // the same reason as the destructor: waitForWarm() above has drained
    // any in-flight warm task that might be reading from these handles.
    {
        std::lock_guard<std::mutex> lock(m_warmFilesMutex);
        m_warmFiles.clear();
    }

    m_loadedWindowStartFrame = -1;
    m_loadedWindowEndFrame   = -1;

    m_cacheRequests.store(0);
    m_cacheHits.store(0);
    m_blockingMisses.store(0);
    m_deferredMisses.store(0);
    m_prefetchRequests.store(0);
    m_prefetchBusySkips.store(0);
    m_prefetchCompletions.store(0);
    m_prefetchInsertions.store(0);
}

void AudioPlaybackService::cancelWarm()
{
    m_warmCancel.store(true);
}

void AudioPlaybackService::waitForWarm()
{
    if (m_warmFuture.valid())
        m_warmFuture.wait();
}

// ---- ensureSourcesLoaded ----------------------------------------------------

void AudioPlaybackService::ensureSourcesLoaded()
{
    const int64_t currentTick = m_playbackController
        ? std::max<int64_t>(0, m_playbackController->currentTick()) : 0;

    if (m_sourcesLoaded &&
        !shouldRefreshTimelineAudioWindow(m_loadedWindowStartFrame,
                                          m_loadedWindowEndFrame,
                                          currentTick)) {
        return;
    }

    if (m_topologyDirty) {
        loadSources(true);
        return;
    }

    loadSources(true);
}

// ---- Persistent AudioFile cache --------------------------------------------

AudioFile* AudioPlaybackService::getOrOpenCachedAudioFile(const std::string& path,
                                                          int audioStreamOrdinal)
{
    // Key by (ordinal, path): distinct streams of the same file must NOT share
    // one AudioFile, or two clips picking different streams would swap PCM.
    // The ordinal prefix makes the key injective regardless of path contents.
    const std::string key = std::to_string(audioStreamOrdinal) + "|" + path;

    std::lock_guard<std::mutex> lock(m_warmFilesMutex);
    auto it = m_warmFiles.find(key);
    if (it != m_warmFiles.end())
        return it->second.get();

    auto fresh = std::make_unique<AudioFile>();
    if (!fresh->open(path, audioStreamOrdinal)) {
        spdlog::warn("AudioPlaybackService: failed to open '{}' (audio stream {})",
                     path, audioStreamOrdinal);
        return nullptr;
    }
    AudioFile* raw = fresh.get();
    m_warmFiles.emplace(key, std::move(fresh));
    return raw;
}

// ---- needsPlaybackWindowRefresh ---------------------------------------------

bool AudioPlaybackService::needsPlaybackWindowRefresh() const
{
    if (!m_playbackController || !m_audioEngine) return false;

    const auto state = m_playbackController->state();
    if (state != PlayState::Playing && state != PlayState::Shuttling) return false;

    if (m_topologyDirty) return true;
    if (!m_sourcesLoaded) return true;

    const int64_t currentTick = std::max<int64_t>(0, m_playbackController->currentTick());
    return shouldRefreshTimelineAudioWindow(m_loadedWindowStartFrame,
                                            m_loadedWindowEndFrame,
                                            currentTick);
}

} // namespace rt