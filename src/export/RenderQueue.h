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
struct JobProgress
{
    int64_t    currentFrame{0};
    int64_t    totalFrames{0};
    float      percent{0.0f};       // 0-100
    double     elapsedSeconds{0.0};
    double     estimatedRemaining{0.0};
    double     fps{0.0};            // Render speed (frames/sec)
    std::string statusText;
};

/// A single export job.
struct ExportJob
{
    uint32_t        id{0};
    ExportJobConfig config;
    JobStatus       status{JobStatus::Queued};
    JobProgress     progress;
    std::string     error;

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
    uint32_t addJob(const ExportJobConfig& config);

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

    /// Get all jobs.
    [[nodiscard]] std::vector<ExportJob> jobs() const;

    /// Get a specific job.
    [[nodiscard]] const ExportJob* job(uint32_t jobId) const;

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

    // processJob is public so the SEH-safe wrapper (safeProcessJob in
    // RenderQueue.cpp) can call it.  It is NOT part of the public API.
    void processJob(ExportJob& job, Timeline* timeline, Compositor* compositor);

private:
    void workerThread();

    mutable std::mutex          m_mutex;
    std::vector<ExportJob>      m_jobs;
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

    // Per-job cancellation
    std::unordered_map<uint32_t, std::atomic<bool>> m_cancelFlags;
};

} // namespace rt
