/*
 * RenderQueue — Background render job queue for export.
 *
 * Manages a queue of export jobs that run in background threads.
 * Each job: frame-render callback (preview compositor) → Encoder → Muxer.
 * Supports multiple concurrent jobs, progress tracking, cancellation.
 */

#pragma once

#include "Encoder.h"
#include "AudioMixdown.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace rt {

// Forward declarations
class Timeline;
class Project;
class Compositor;
struct CachedFrame;

/// Callback that composites a single frame at a given tick.
/// Returns a CachedFrame (BGRA) or nullptr on failure.
/// nextTick is the tick of the NEXT frame (for async pre-submit pipeline),
/// or -1 if this is the last frame.
using FrameRenderFn = std::function<std::shared_ptr<CachedFrame>(
    int64_t tick, int64_t nextTick,
    uint32_t width, uint32_t height, bool scrubMode)>;

/// Optional: receives each finished export frame (full-res, CPU pixels ready)
/// right before it is encoded, so it can be written into the segment render
/// cache (§4.6 export write-through).  Called on the export worker thread.
using FrameStoreFn = std::function<void(int64_t tick,
                                        const std::shared_ptr<CachedFrame>&)>;

// ── Export preset ────────────────────────────────────────────────────────────

/// Pre-configured export settings
enum class ExportPreset : uint8_t
{
    YouTube1080p30,
    YouTube1080p60,
    YouTube4K30,
    YouTube4K60,
    Broadcast1080i,
    ArchiveProRes,
    ArchiveLossless,
    WebOptimized,
    Custom,
    Count
};

[[nodiscard]] const char* exportPresetName(ExportPreset preset) noexcept;

// ── Export job configuration ────────────────────────────────────────────────

struct ExportJobConfig
{
    // Output
    std::filesystem::path outputPath;
    ExportPreset          preset{ExportPreset::Custom};

    // Video
    EncoderConfig         encoderConfig;
    uint32_t              outputWidth{1920};
    uint32_t              outputHeight{1080};

    // Container
    uint8_t               containerFormat{0}; // ContainerFormat enum

    // Audio
    AudioMixdownConfig    audioConfig;
    bool                  includeAudio{true};

    // Audio-only export: skip all video work and write a standalone audio file
    // (WAV / MP3 / AAC / FLAC) using audioConfig.codec.  outputPath carries the
    // matching extension.  See RenderQueue::processAudioOnlyJob.
    bool                  audioOnly{false};

    // Range (frames). 0,0 = full timeline.
    int64_t               startFrame{0};
    int64_t               endFrame{0};

    /// Apply an export preset's default settings.
    void applyPreset(ExportPreset preset);
};

// ── Job status ──────────────────────────────────────────────────────────────

enum class JobStatus : uint8_t
{
    Queued,
    Running,
    Completed,
    Failed,
    Cancelled
};

/// Progress information for a running job.
/// Numeric fields are atomics: the worker thread writes them per frame while
/// the UI thread polls them through RenderQueue::job().  statusText is guarded
/// by RenderQueue's mutex — cross-thread readers must use
/// RenderQueue::jobStatusText() instead of touching it directly.
struct JobProgress
{
    std::atomic<int64_t> currentFrame{0};
    std::atomic<int64_t> totalFrames{0};
    std::atomic<float>   percent{0.0f};       // 0-100
    std::atomic<double>  elapsedSeconds{0.0};
    std::atomic<double>  estimatedRemaining{0.0};
    std::atomic<double>  fps{0.0};            // Render speed (frames/sec)
    std::string statusText;                   // guarded by RenderQueue::m_mutex
};

/// A single export job.
struct ExportJob
{
    uint32_t        id{0};
    ExportJobConfig config;
    std::atomic<JobStatus> status{JobStatus::Queued};
    JobProgress     progress;
    std::string     error;   // written by the worker; snapshot is handed to JobCompleteFn

    /// Per-job cancellation flag.  Set (under RenderQueue's mutex lookup) by
    /// cancelJob/cancelAll, polled lock-free by the worker's frame loop.
    std::atomic<bool> cancelRequested{false};

    /// Deep clone of the timeline taken on the MAIN thread when the job is
    /// enqueued (addJob), so the worker's AudioMixdown never reads the live
    /// timeline while the user edits it.  Owned by the job — it outlives
    /// processJob, so an SEH unwind there cannot leak it.  Null = fall back
    /// to the live timeline (tests / callers that pass no timeline).
    std::shared_ptr<const Timeline> timelineSnapshot;

    /// Set when the encoder requested for `config.encoderConfig.hwAccel`
    /// failed to initialise and processJob silently fell back to CPU
    /// encoding (see RenderQueue.cpp).  Used by ExportPanel to surface
    /// the fallback to the user (a Pascal GPU with Discord/OBS running
    /// will trip the 2-session NVENC cap and end up here).
    bool            fellBackToCpuEncoder{false};
    std::string     fellBackReason;
};

// ── Callbacks ───────────────────────────────────────────────────────────────

using JobProgressFn  = std::function<void(uint32_t jobId, const JobProgress& progress)>;
using JobCompleteFn  = std::function<void(uint32_t jobId, bool success, const std::string& error)>;

/// Per-job run resources (encoder, demuxers) owned OUTSIDE processJob's
/// __try frame — see RenderQueue.cpp (SEH unwinding skips local destructors
/// because this TU is not compiled /EHa).
struct JobRunContext;

// ═════════════════════════════════════════════════════════════════════════════

class RenderQueue
{
public:
    RenderQueue();
    ~RenderQueue();

    RenderQueue(const RenderQueue&) = delete;
    RenderQueue& operator=(const RenderQueue&) = delete;

    // ── Job management ──────────────────────────────────────────────────

    /// Add a new export job. Returns job ID.
    /// `timelineForAudio` (optional) is deep-cloned HERE — call addJob on the
    /// main thread — and stored on the job as the snapshot the audio mixdown
    /// reads, so live edits during the export can't race it.
    uint32_t addJob(const ExportJobConfig& config,
                    const Timeline* timelineForAudio = nullptr);

    /// Remove a job (must be Queued or Completed/Failed/Cancelled).
    bool removeJob(uint32_t jobId);

    /// Set Project pointer for resolving nested SequenceClips.
    void setProject(const Project* proj) noexcept { m_project = proj; }

    /// Start processing the queue (launches background threads).
    void start(Timeline* timeline, Compositor* compositor = nullptr);

    /// Cancel a specific job.
    void cancelJob(uint32_t jobId);

    /// Cancel all jobs and stop the queue.
    void cancelAll();

    /// Wait for all jobs to complete.
    void waitForAll();

    // ── Queries ─────────────────────────────────────────────────────────

    /// Get all jobs (shared ownership — safe to hold across removeJob).
    [[nodiscard]] std::vector<std::shared_ptr<const ExportJob>> jobs() const;

    /// Get a specific job.  Returns shared ownership so the pointer stays
    /// valid even if the job is removed from the queue afterwards.
    [[nodiscard]] std::shared_ptr<const ExportJob> job(uint32_t jobId) const;

    /// Locked read of a job's progress.statusText (worker writes it under the
    /// same mutex).  Returns "" if the job doesn't exist.
    [[nodiscard]] std::string jobStatusText(uint32_t jobId) const;

    /// Number of queued/running jobs.
    [[nodiscard]] size_t pendingCount() const;

    /// Is the queue currently processing?
    [[nodiscard]] bool isRunning() const noexcept { return m_running.load(); }

    // ── Callbacks ───────────────────────────────────────────────────────

    void setProgressCallback(const JobProgressFn& cb) { m_progressCb = cb; }
    void setCompleteCallback(const JobCompleteFn& cb) { m_completeCb = cb; }

    /// Set a callback that produces composited frames for export. REQUIRED:
    /// processJob composites every frame through this (the preview compositor).
    /// The old internal FrameRenderer fallback was removed (#18).
    void setFrameRenderCallback(FrameRenderFn fn) { m_frameRenderCb = std::move(fn); }

    /// Optional segment-cache write-through (§4.6): called with each finished
    /// full-res frame just before encoding (pixels guaranteed present).
    void setFrameStoreCallback(FrameStoreFn fn) { m_frameStoreCb = std::move(fn); }

    // processJob is public so the SEH-safe wrapper (safeProcessJob in
    // RenderQueue.cpp) can call it.  It is NOT part of the public API.
    // `ctx` owns the encoder/demuxers and lives in workerThread's frame so
    // an SEH unwind (which skips processJob's local destructors — this TU
    // is not /EHa) still releases the NVENC session.
    void processJob(ExportJob& job, JobRunContext& ctx,
                    Timeline* timeline, Compositor* compositor);

private:
    void workerThread();

    /// Audio-only job: mix the timeline's audio and write a standalone audio
    /// file (no video encode/composite).  Called from processJob when
    /// job.config.audioOnly is set.
    void processAudioOnlyJob(ExportJob& job, Timeline* timeline);

    // Jobs are held by shared_ptr: the worker keeps its own reference to the
    // job it is processing, so removeJob erasing a vector element can never
    // dangle the worker's pointer (the old std::vector<ExportJob> storage
    // shifted elements on erase while the worker held a raw pointer).
    mutable std::mutex          m_mutex;
    std::vector<std::shared_ptr<ExportJob>> m_jobs;
    uint32_t                    m_nextJobId{1};
    std::atomic<bool>           m_running{false};
    std::atomic<bool>           m_cancelAll{false};
    std::thread                 m_worker;
    const Project*              m_project{nullptr};
    Timeline*                   m_timeline{nullptr};
    Compositor*                 m_compositor{nullptr};

    JobProgressFn               m_progressCb;
    JobCompleteFn               m_completeCb;
    FrameRenderFn               m_frameRenderCb;
    FrameStoreFn                m_frameStoreCb;
};

} // namespace rt
