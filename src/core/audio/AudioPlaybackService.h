/*
 * AudioPlaybackService — Timeline audio source loading, decode caching, and
 * background prefetch.
 *
 * Extracted from TimelineWorkspace to separate audio playback concerns from
 * UI orchestration.  Lives in core/media/ — no Qt dependency.
 */

#pragma once

#include <atomic>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace rt {

class AudioEngine;
class AudioFile;
class AudioSampleProvider;
class PlaybackController;
class Project;
class Timeline;

class AudioPlaybackService {
public:
    AudioPlaybackService();
    ~AudioPlaybackService();

    AudioPlaybackService(const AudioPlaybackService&) = delete;
    AudioPlaybackService& operator=(const AudioPlaybackService&) = delete;

    // ── Dependency injection ────────────────────────────────────────────
    void setTimeline(Timeline* tl)                    { m_timeline = tl; }
    void setAudioEngine(AudioEngine* engine)          { m_audioEngine = engine; }
    void setPlaybackController(PlaybackController* c) { m_playbackController = c; }
    /// Project reference — needed to resolve nested-sequence audio
    /// (SequenceClip on an audio track). May be null (no nesting support).
    void setProject(Project* p)                       { m_project = p; }

    // ── Core operations ─────────────────────────────────────────────────
    /// Scan the timeline for audio clips and feed decoded buffers to the
    /// AudioEngine.  When allowBlockingMisses is false, cache misses are
    /// deferred to the background prefetch thread instead of blocking.
    void loadSources(bool allowBlockingMisses = true);

    /// Load audio sources if not yet loaded, or if the playback window
    /// needs refreshing.  Skips reload during active playback.
    void ensureSourcesLoaded();

    /// Pre-decode audio pages in a background thread for the current
    /// playback window plus a 16-second lookahead.
    void warmCacheAsync();

    /// Cancel any in-flight background decode.
    void cancelWarm();

    /// Block until any in-flight background decode completes.
    void waitForWarm();

    // ── State management ────────────────────────────────────────────────
    /// Invalidate cached audio sources so they are reloaded on next play.
    void invalidateSources();

    /// Live-update a clip's volume/pan/mute without rebuilding the source
    /// list. Used for Effect Controls scrubbing so audio doesn't glitch.
    void updateClipLevels(uint64_t clipId, float volume, float pan, bool muted);

    /// Reset all state (decode cache, providers, stats).  Call on timeline
    /// change (e.g. project open/close).
    void reset();

    /// Whether the playback window needs refreshing at the current
    /// playhead position (for use by UI scheduling code).
    bool needsPlaybackWindowRefresh() const;

    /// Log a performance snapshot of the audio decode cache.
    void logPerfSnapshot(const char* reason);

    [[nodiscard]] bool sourcesLoaded()   const noexcept { return m_sourcesLoaded; }
    [[nodiscard]] bool isTopologyDirty() const noexcept { return m_topologyDirty; }

    std::atomic<bool> m_destroying{false};

private:
    // Dependencies (non-owning)
    Timeline*            m_timeline{nullptr};
    AudioEngine*         m_audioEngine{nullptr};
    PlaybackController*  m_playbackController{nullptr};
    Project*             m_project{nullptr};

    // Provider mapping (clipId → window provider)
    std::vector<std::shared_ptr<std::vector<float>>> m_audioBuffers;
    std::unordered_map<uint64_t, std::shared_ptr<AudioSampleProvider>> m_clipProviders;
    bool    m_sourcesLoaded{false};
    bool    m_topologyDirty{true};
    int64_t m_loadedWindowStartFrame{-1};
    int64_t m_loadedWindowEndFrame{-1};

    // Decoded-audio page cache (path+offset → PCM)
    struct CachedDecode {
        std::shared_ptr<std::vector<float>> samples;
        int64_t  startFrame{0};
        uint16_t channels{0};
        int64_t  totalFrames{0};
        size_t   bytes{0};
        uint64_t lastUseSerial{0};
    };
    static constexpr size_t kDecodeCacheBudgetBytes = 256u * 1024u * 1024u;
    std::mutex m_decodeMutex;
    std::unordered_map<std::string, CachedDecode> m_decodeCache;
    uint64_t m_decodeUseSerial{0};
    void pruneDecodeCacheLocked();

    // Performance statistics
    std::atomic<uint64_t> m_cacheRequests{0};
    std::atomic<uint64_t> m_cacheHits{0};
    std::atomic<uint64_t> m_blockingMisses{0};
    std::atomic<uint64_t> m_deferredMisses{0};
    std::atomic<uint64_t> m_prefetchRequests{0};
    std::atomic<uint64_t> m_prefetchBusySkips{0};
    std::atomic<uint64_t> m_prefetchCompletions{0};
    std::atomic<uint64_t> m_prefetchInsertions{0};

    // Background warm-up
    std::future<void> m_warmFuture;
    std::atomic<bool> m_warmCancel{false};

    // Persistent per-path AudioFile cache used by warmCacheAsync.
    //
    // Without this, every warmCacheAsync invocation constructs a fresh
    // AudioFile and re-runs the ~200-300 ms sndfile + FFmpeg probe
    // sequence (sndfile self-test, sndfile open, FFmpeg self-test,
    // FFmpeg open) — see the four `AudioFile: share-mode self-test`
    // log lines per open in logs/perf_log.txt.  Under the steady-state
    // playback-window slide (and especially during scrub or any clip-
    // boundary playhead-jump cascade), this fires every 300 ms–1 s and
    // saturates disk I/O against the video prefetch workers, which in
    // turn tips the FrameClock into the 10 fps stutter cascade
    // documented in UPGRADE_PLAN section 1.
    //
    // AudioFile is internally mutex-guarded (see AudioFile.h: "Thread-
    // safe: all public methods are guarded by a mutex"), so we can
    // share instances between the warm thread and any incidental
    // caller without an external lock around the AudioFile itself —
    // only the map lookup needs synchronisation.
    //
    // Lifecycle: populated lazily by warmCacheAsync.  Cleared by
    // reset() AFTER waitForWarm() has drained any in-flight task, and
    // by the destructor (same ordering).  Never accessed during the
    // synchronous loadSources(allowBlockingMisses=true) path, which
    // keeps its own short-lived AudioFile — that codepath runs at
    // project-load only, so the open-amortisation payoff is small and
    // mixing it with the warm cache would broaden the synchronisation
    // surface needlessly.
    std::mutex m_warmFilesMutex;
    std::unordered_map<std::string, std::unique_ptr<AudioFile>> m_warmFiles;

    /// Resolve (or open + insert) the persistent AudioFile for `path`.
    /// Returns nullptr on open failure.  Safe to call from any thread;
    /// the map mutex is held only for the lookup / insertion — the
    /// AudioFile's own internal mutex (see AudioFile.h "Thread-safe:
    /// all public methods are guarded by a mutex") guards subsequent
    /// reads, so the returned pointer can be used concurrently by
    /// multiple callers.
    ///
    /// Migrated 2026-05-22 (UPGRADE_PLAN item 4): the cache used to be
    /// scoped to warmCacheAsync only, while the synchronous
    /// loadSources(true) blocking-miss path constructed a fresh
    /// AudioFile each call.  Under seek+play sequences that fired
    /// loadSources(true) twice in 1.8 s for the same media path, this
    /// produced the audio-thread 3.4 s stall + COMPOSITE-SLOW cascade
    /// visible in logs/perf_log.txt at 20:43:46-50.  Both paths now
    /// share this method, so a single sndfile+FFmpeg probe is paid
    /// once per source file per project session.
    AudioFile* getOrOpenCachedAudioFile(const std::string& path);
};

} // namespace rt
