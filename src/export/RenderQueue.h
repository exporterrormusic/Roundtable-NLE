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
#include <cstddef>
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

/// Immutable project graph captured when an export job is enqueued.
///
/// `timeline` aliases the selected sequence inside `project` (and therefore
/// shares its lifetime/control block) for full production snapshots.  The
/// project may be null only for the legacy timeline-only addJob overload used
/// by low-level tests/callers.  Video and audio must consume this same object;
/// no queued job is allowed to re-snapshot later.
struct ExportRenderSnapshot
{
    std::shared_ptr<const Project>  project;
    std::shared_ptr<const Timeline> timeline;
    size_t                          sequenceIndex{0};

    [[nodiscard]] bool hasTimeline() const noexcept { return timeline != nullptr; }
    [[nodiscard]] bool isFullProject() const noexcept {
        return project != nullptr && timeline != nullptr;
    }
};

/// Callback that composites a single frame at a given tick.
/// Returns a CachedFrame (BGRA) or nullptr on failure.
/// nextTick is the tick of the NEXT frame (for async pre-submit pipeline),
/// or -1 if this is the last frame.
using FrameRenderFn = std::function<std::shared_ptr<CachedFrame>(
    int64_t tick, int64_t nextTick,
    uint32_t width, uint32_t height, bool scrubMode)>;

/// Snapshot-aware production compositor callback.  preserveAlpha is also
/// job-local, so changing Export-panel controls cannot affect a running job.
using SnapshotFrameRenderFn = std::function<std::shared_ptr<CachedFrame>(
    const std::shared_ptr<const ExportRenderSnapshot>& snapshot,
    int64_t tick, int64_t nextTick,
    uint32_t width, uint32_t height, bool scrubMode, bool preserveAlpha)>;

/// Optional: receives each finished export frame (full-res, CPU pixels ready)
/// right before it is encoded, so it can be written into the segment render
/// cache (§4.6 export write-through).  Called on the export worker thread.
using FrameStoreFn = std::function<void(int64_t tick,
                                        const std::shared_ptr<CachedFrame>&)>;

using SnapshotFrameStoreFn = std::function<void(
    const std::shared_ptr<const ExportRenderSnapshot>& snapshot,
    int64_t tick, const std::shared_ptr<CachedFrame>&)>;

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
    bool                  preserveAlpha{false};

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

    /// One deep project/timeline snapshot captured on the MAIN thread at
    /// enqueue time.  Video, audio, duration analysis, nested-sequence
    /// resolution, alpha mode and cache writes all receive this exact object.
    /// Null is retained only for legacy callers that enqueue no source state.
    std::shared_ptr<const ExportRenderSnapshot> renderSnapshot;

    /// Non-empty when a requested production snapshot could not be captured.
    /// The worker fails before opening an encoder/output and never substitutes
    /// live mutable state after a capture failure.
    std::string snapshotCaptureError;

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

    /// Legacy path.  A supplied timeline is deep-cloned here and used for all
    /// timeline-derived worker work.  Production UI code should use the full
    /// Project overload so nested sequences and project render state are also
    /// captured.
    uint32_t addJob(const ExportJobConfig& config,
                    const Timeline* timeline = nullptr);

    /// Production enqueue path.  Captures the complete project graph on the
    /// caller/main thread and aliases `timeline` into that captured graph.
    uint32_t addJob(const ExportJobConfig& config,
                    const Project* project,
                    const Timeline* timeline);

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
    void setFrameRenderCallback(FrameRenderFn fn) {
        m_frameRenderCb = [fn = std::move(fn)](
            const std::shared_ptr<const ExportRenderSnapshot>&,
            int64_t tick, int64_t nextTick, uint32_t width, uint32_t height,
            bool scrubMode, bool) -> std::shared_ptr<CachedFrame> {
                if (!fn) return nullptr;
                return fn(tick, nextTick, width, height, scrubMode);
            };
    }
    void setSnapshotFrameRenderCallback(SnapshotFrameRenderFn fn) {
        m_frameRenderCb = std::move(fn);
    }

    /// Optional segment-cache write-through (§4.6): called with each finished
    /// full-res frame just before encoding (pixels guaranteed present).
    void setFrameStoreCallback(FrameStoreFn fn) {
        m_frameStoreCb = [fn = std::move(fn)](
            const std::shared_ptr<const ExportRenderSnapshot>&,
            int64_t tick, const std::shared_ptr<CachedFrame>& frame) {
                if (fn) fn(tick, frame);
            };
    }
    void setSnapshotFrameStoreCallback(SnapshotFrameStoreFn fn) {
        m_frameStoreCb = std::move(fn);
    }

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
    void processAudioOnlyJob(ExportJob& job, const Timeline* timeline);

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
    SnapshotFrameRenderFn       m_frameRenderCb;
    SnapshotFrameStoreFn        m_frameStoreCb;
};

} // namespace rt
