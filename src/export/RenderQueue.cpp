/*
 * RenderQueue.cpp — Background render job queue.
 */

#include "RenderQueue.h"
#include "AudioMixdown.h"
#include "Muxer.h"
#include "Encoder.h"
#include "ExportIntegrity.h"
#include "SmartRenderAnalyzer.h"
#include "PacketDemuxer.h"

#include "cache/FrameCache.h"
#include "project/Project.h"
#include "project/ProjectSerializer.h"
#include "timeline/Timeline.h"
#include "PathUtils.h"
#include "FrameTime.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <exception>
#include <stdexcept>
#include <unordered_map>

#ifdef _MSC_VER
#include <excpt.h>  // GetExceptionCode, EXCEPTION_EXECUTE_HANDLER
#endif

namespace rt {

namespace {

/// Share of a video job's progress bar for rendering frames; the strict
/// validation decode of the finished file fills the rest. Validation runs at
/// roughly a quarter of the render's time (it decodes every frame back), so
/// a thin share left the bar sitting near the end for most of a minute.
constexpr float kRenderProgressShare = 80.0f;

/// A small BGRA picture of an NV12 export frame for the export panel's
/// preview: every `step`-th pixel, BT.709 limited range back to RGB.
std::shared_ptr<CachedFrame> nv12PreviewFrame(const CachedFrame& src, uint32_t step)
{
    auto out = std::make_shared<CachedFrame>();
    out->width  = std::max(1u, src.width / step);
    out->height = std::max(1u, src.height / step);
    out->stride = out->width * 4;
    out->pixels.resize(static_cast<size_t>(out->stride) * out->height);
    const size_t stride = src.nv12Stride;
    const uint8_t* yPlane = src.nv12.get();
    const uint8_t* uvPlane = yPlane + stride * src.height;
    auto toByte = [](float v) {
        return static_cast<uint8_t>(std::clamp(v + 0.5f, 0.0f, 255.0f));
    };
    for (uint32_t y = 0; y < out->height; ++y) {
        const size_t sy = static_cast<size_t>(y) * step;
        uint8_t* row = out->pixels.data() + static_cast<size_t>(y) * out->stride;
        for (uint32_t x = 0; x < out->width; ++x) {
            const size_t sx = static_cast<size_t>(x) * step;
            const float luma = 1.16438f * (yPlane[sy * stride + sx] - 16.0f);
            const uint8_t* c = uvPlane + (sy / 2) * stride + (sx / 2) * 2;
            const float cb = c[0] - 128.0f;
            const float cr = c[1] - 128.0f;
            row[x * 4 + 0] = toByte(luma + 2.11240f * cb);
            row[x * 4 + 1] = toByte(luma - 0.21325f * cb - 0.53291f * cr);
            row[x * 4 + 2] = toByte(luma + 1.79274f * cr);
            row[x * 4 + 3] = 255;
        }
    }
    return out;
}

// Every immutable capture receives a non-zero generation. The composite
// cache additionally hashes pixel-affecting timeline state, but this value
// keeps capture provenance explicit and prevents two independently captured
// graphs from masquerading as the same revision if an address is ever reused.
std::atomic<uint64_t> g_nextRenderSnapshotVersion{1};

[[nodiscard]] uint64_t nextRenderSnapshotVersion() noexcept
{
    uint64_t version = g_nextRenderSnapshotVersion.fetch_add(
        1, std::memory_order_relaxed);
    // Wraparound is practically unreachable, but zero is reserved for live
    // graphs with no explicit immutable-capture generation.
    if (version == 0) {
        version = g_nextRenderSnapshotVersion.fetch_add(
            1, std::memory_order_relaxed);
    }
    return version;
}

void setSnapshotExportRange(ExportRenderSnapshot& snapshot,
                            const ExportJobConfig& config,
                            const Timeline& timeline)
{
    const uint32_t fpsNum = config.encoderConfig.fpsNum > 0
        ? static_cast<uint32_t>(config.encoderConfig.fpsNum) : 1u;
    const uint32_t fpsDen = config.encoderConfig.fpsDen > 0
        ? static_cast<uint32_t>(config.encoderConfig.fpsDen) : 1u;
    int64_t startFrame = config.startFrame;
    int64_t endFrame = config.endFrame;
    if (endFrame <= startFrame)
        endFrame = tickToEndFrameIndex(timeline.duration(), fpsNum, fpsDen);
    if (endFrame <= startFrame)
        endFrame = startFrame + 1;
    snapshot.rangeStartTick = frameIndexToTick(
        startFrame, fpsNum, fpsDen);
    snapshot.rangeEndTick = frameIndexToTick(
        endFrame, fpsNum, fpsDen);
}

} // namespace

const char* exportPresetName(ExportPreset preset) noexcept
{
    switch (preset) {
        case ExportPreset::YouTube1080p30:   return "YouTube 1080p 30fps";
        case ExportPreset::YouTube1080p60:   return "YouTube 1080p 60fps";
        case ExportPreset::YouTube4K30:      return "YouTube 4K 30fps";
        case ExportPreset::YouTube4K60:      return "YouTube 4K 60fps";
        case ExportPreset::Broadcast1080i:   return "Broadcast 1080i";
        case ExportPreset::ArchiveProRes:    return "Archive ProRes HQ";
        case ExportPreset::ArchiveLossless:  return "Archive Lossless";
        case ExportPreset::WebOptimized:     return "Web Optimized";
        case ExportPreset::Custom:           return "Custom";
        default:                             return "Unknown";
    }
}

void ExportJobConfig::applyPreset(ExportPreset p)
{
    preset = p;
    switch (p) {
        case ExportPreset::YouTube1080p30:
            outputWidth = 1920; outputHeight = 1080;
            encoderConfig.width = 1920; encoderConfig.height = 1080;
            encoderConfig.codec = EncoderCodec::H264;
            encoderConfig.fpsNum = 30; encoderConfig.fpsDen = 1;
            encoderConfig.crf = 18;
            encoderConfig.preset = EncoderPreset::Slow;
            encoderConfig.hwAccel = HardwareAccel::NVENC;
            containerFormat = static_cast<uint8_t>(ContainerFormat::MP4);
            break;

        case ExportPreset::YouTube1080p60:
            outputWidth = 1920; outputHeight = 1080;
            encoderConfig.width = 1920; encoderConfig.height = 1080;
            encoderConfig.codec = EncoderCodec::H264;
            encoderConfig.fpsNum = 60; encoderConfig.fpsDen = 1;
            encoderConfig.crf = 18;
            encoderConfig.hwAccel = HardwareAccel::NVENC;
            containerFormat = static_cast<uint8_t>(ContainerFormat::MP4);
            break;

        case ExportPreset::YouTube4K30:
            outputWidth = 3840; outputHeight = 2160;
            encoderConfig.width = 3840; encoderConfig.height = 2160;
            encoderConfig.codec = EncoderCodec::AV1;
            encoderConfig.fpsNum = 30; encoderConfig.fpsDen = 1;
            encoderConfig.crf = 23;
            encoderConfig.hwAccel = HardwareAccel::NVENC;
            containerFormat = static_cast<uint8_t>(ContainerFormat::MP4);
            break;

        case ExportPreset::YouTube4K60:
            outputWidth = 3840; outputHeight = 2160;
            encoderConfig.width = 3840; encoderConfig.height = 2160;
            encoderConfig.codec = EncoderCodec::AV1;
            encoderConfig.fpsNum = 60; encoderConfig.fpsDen = 1;
            encoderConfig.crf = 23;
            encoderConfig.hwAccel = HardwareAccel::NVENC;
            containerFormat = static_cast<uint8_t>(ContainerFormat::MP4);
            break;

        case ExportPreset::ArchiveProRes:
            outputWidth = 1920; outputHeight = 1080;
            encoderConfig.width = 1920; encoderConfig.height = 1080;
            encoderConfig.codec = EncoderCodec::ProRes;
            encoderConfig.fpsNum = 30; encoderConfig.fpsDen = 1;
            encoderConfig.proresProfile = ProResProfile::HQ;
            containerFormat = static_cast<uint8_t>(ContainerFormat::MOV);
            break;

        case ExportPreset::WebOptimized:
            outputWidth = 1280; outputHeight = 720;
            encoderConfig.width = 1280; encoderConfig.height = 720;
            encoderConfig.codec = EncoderCodec::H264;
            encoderConfig.fpsNum = 30; encoderConfig.fpsDen = 1;
            encoderConfig.crf = 23;
            encoderConfig.preset = EncoderPreset::Medium;
            containerFormat = static_cast<uint8_t>(ContainerFormat::MP4);
            break;

        default:
            break;
    }
}

RenderQueue::RenderQueue() = default;

RenderQueue::~RenderQueue()
{
    cancelAll();
    m_worker.stop();
}

uint32_t RenderQueue::addJob(const ExportJobConfig& config,
                             const Timeline* timeline)
{
    const size_t cleaned = cleanupAbandonedStagedExports(
        config.outputPath.parent_path(), std::chrono::hours(24 * 7));
    if (cleaned > 0)
        spdlog::info("RenderQueue: removed {} abandoned staged export(s)", cleaned);
    // Legacy callers may not own a Project.  Still snapshot the timeline for
    // BOTH video-derived calculations and audio (not just when audio is on),
    // so no worker-side duration/smart-render decision observes later edits.
    std::shared_ptr<const ExportRenderSnapshot> snapshot;
    std::string captureError;
    if (timeline) {
        try {
            auto captured = std::make_shared<ExportRenderSnapshot>();
            captured->editVersion = nextRenderSnapshotVersion();
            captured->timeline = timeline->clone();
            if (!captured->timeline)
                captureError = "Failed to clone export timeline";
            else {
                setSnapshotExportRange(*captured, config, *captured->timeline);
                snapshot = std::move(captured);
            }
        } catch (const std::exception& e) {
            captureError = std::string("Failed to clone export timeline: ") + e.what();
        } catch (...) {
            captureError = "Failed to clone export timeline";
        }
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    auto job = std::make_shared<ExportJob>();
    job->id     = m_nextJobId++;
    job->config = config;
    job->renderSnapshot = std::move(snapshot);
    job->snapshotCaptureError = std::move(captureError);
    m_jobs.push_back(job);
    spdlog::info("RenderQueue: Added job {} → {}", job->id, pathToUtf8(config.outputPath));
    return job->id;
}

uint32_t RenderQueue::addJob(const ExportJobConfig& config,
                             const Project* project,
                             const Timeline* timeline,
                             std::shared_ptr<const RenderSnapshot> published)
{
    const size_t cleaned = cleanupAbandonedStagedExports(
        config.outputPath.parent_path(), std::chrono::hours(24 * 7));
    if (cleaned > 0)
        spdlog::info("RenderQueue: removed {} abandoned staged export(s)", cleaned);

    // Preferred: share the UI's published render snapshot. Its sequences are
    // already immutable copies, so nothing is copied here.
    if (published && timeline) {
        size_t index = published->sequences.size();
        for (size_t i = 0; i < published->sequences.size(); ++i) {
            const auto& seq = published->sequences[i];
            if (seq && seq->id() == timeline->id()) { index = i; break; }
        }
        if (index < published->sequences.size()) {
            auto shared = std::make_shared<ExportRenderSnapshot>();
            static_cast<RenderSnapshot&>(*shared) = *published;
            shared->project.reset();
            shared->timeline = published->sequences[index];
            shared->sequenceIndex = index;
            shared->editVersion = nextRenderSnapshotVersion();
            setSnapshotExportRange(*shared, config, *shared->timeline);

            std::lock_guard<std::mutex> lock(m_mutex);
            auto job = std::make_shared<ExportJob>();
            job->id     = m_nextJobId++;
            job->config = config;
            job->renderSnapshot = std::move(shared);
            m_jobs.push_back(job);
            spdlog::info("RenderQueue: Added job {} from the published render "
                         "snapshot → {}", job->id, pathToUtf8(config.outputPath));
            return job->id;
        }
        spdlog::warn("RenderQueue: published snapshot does not contain the "
                     "export sequence; capturing the project instead");
    }
    // Capture before taking the queue mutex: serialization is non-trivial and
    // addJob is a main-thread operation, where the live project is stable.
    // The serializer is deliberately used instead of independently cloning a
    // Timeline: nested SequenceClips resolve through Project, so only one full
    // graph can guarantee video and audio see the same queue-time state.
    std::shared_ptr<const ExportRenderSnapshot> snapshot;
    std::string captureError;

    if (!project) {
        captureError = "Cannot capture export: no project is loaded";
    } else if (!timeline) {
        captureError = "Cannot capture export: no sequence is selected";
    } else {
        size_t selectedIndex = project->sequenceCount();
        for (size_t i = 0; i < project->sequenceCount(); ++i) {
            if (project->sequence(i) == timeline) {
                selectedIndex = i;
                break;
            }
        }

        if (selectedIndex == project->sequenceCount()) {
            captureError = "Cannot capture export: selected sequence is not part of the project";
        } else {
            try {
                ProjectSerializer serializer;
                const auto bytes = serializer.serialize(*project);
                if (bytes.empty()) {
                    captureError = "Cannot capture export: project serialization returned no data";
                } else {
                    auto capturedProject = serializer.deserialize(bytes);
                    if (!capturedProject ||
                        selectedIndex >= capturedProject->sequenceCount() ||
                        !capturedProject->sequence(selectedIndex)) {
                        captureError = "Cannot capture export: project snapshot validation failed";
                    } else {
                        // Make Project::settings()/timeline() agree with the
                        // explicitly exported sequence inside the snapshot.
                        // filePath is intentionally file-I/O metadata rather
                        // than part of the .rtp payload, so carry it across the
                        // in-memory round-trip explicitly as well.
                        capturedProject->setFilePath(project->filePath());
                        capturedProject->setModified(project->isModified());
                        capturedProject->setActiveSequence(selectedIndex);

                        auto renderSnapshot = std::make_shared<ExportRenderSnapshot>();
                        renderSnapshot->editVersion = nextRenderSnapshotVersion();
                        renderSnapshot->sequenceIndex = selectedIndex;
                        renderSnapshot->project =
                            std::shared_ptr<const Project>(std::move(capturedProject));
                        renderSnapshot->timeline = std::shared_ptr<const Timeline>(
                            renderSnapshot->project,
                            renderSnapshot->project->sequence(selectedIndex));
                        setSnapshotExportRange(
                            *renderSnapshot, config, *renderSnapshot->timeline);
                        snapshot = std::move(renderSnapshot);
                    }
                }
            } catch (const std::exception& e) {
                captureError = std::string("Cannot capture export project: ") + e.what();
            } catch (...) {
                captureError = "Cannot capture export project";
            }
        }
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    auto job = std::make_shared<ExportJob>();
    job->id     = m_nextJobId++;
    job->config = config;
    job->renderSnapshot = std::move(snapshot);
    job->snapshotCaptureError = std::move(captureError);
    m_jobs.push_back(job);

    if (job->snapshotCaptureError.empty()) {
        spdlog::info("RenderQueue: Added immutable project snapshot job {} → {}",
                     job->id, pathToUtf8(config.outputPath));
    } else {
        spdlog::error("RenderQueue: Job {} snapshot capture failed: {}",
                      job->id, job->snapshotCaptureError);
    }
    return job->id;
}

bool RenderQueue::removeJob(uint32_t jobId)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto it = m_jobs.begin(); it != m_jobs.end(); ++it) {
        if ((*it)->id == jobId) {
            if ((*it)->status.load() == JobStatus::Running) return false;
            // Erasing is safe even if the worker still holds this job's
            // shared_ptr (it keeps its own reference).
            m_jobs.erase(it);
            return true;
        }
    }
    return false;
}

void RenderQueue::start(Timeline* timeline, Compositor* compositor)
{
    if (m_running.load()) return;

    // Join the previous worker if it finished but wasn't joined yet.
    m_worker.join();

    m_timeline   = timeline;
    m_compositor = compositor;
    m_running    = true;
    m_cancelAll  = false;

    m_worker.start("Export render queue", [this](std::stop_token) { workerThread(); });
}

void RenderQueue::cancelJob(uint32_t jobId)
{
    // Find the job under the mutex; the flag itself is atomic so the worker
    // polls it lock-free.  (The old m_cancelFlags map was read without the
    // lock while the worker inserted into it — a crash-class race.)
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto& j : m_jobs) {
        if (j->id == jobId) {
            j->cancelRequested.store(true);
            return;
        }
    }
}

void RenderQueue::cancelAll()
{
    m_cancelAll.store(true);
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto& j : m_jobs)
        j->cancelRequested.store(true);
}

void RenderQueue::waitForAll()
{
    m_worker.join();
}

std::vector<std::shared_ptr<const ExportJob>> RenderQueue::jobs() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return {m_jobs.begin(), m_jobs.end()};
}

std::shared_ptr<const ExportJob> RenderQueue::job(uint32_t jobId) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    for (const auto& j : m_jobs) {
        if (j->id == jobId) return j;
    }
    return nullptr;
}

std::string RenderQueue::jobStatusText(uint32_t jobId) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    for (const auto& j : m_jobs) {
        if (j->id == jobId) return j->progress.statusText;
    }
    return {};
}

std::shared_ptr<const CachedFrame> RenderQueue::latestFrame() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_latestFrame;
}

size_t RenderQueue::pendingCount() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    size_t count = 0;
    for (const auto& j : m_jobs) {
        const JobStatus st = j->status.load();
        if (st == JobStatus::Queued || st == JobStatus::Running)
            ++count;
    }
    return count;
}

/// Per-job resources that must survive an SEH unwind.  RenderQueue.cpp is
/// NOT compiled with /EHa (see src/export/CMakeLists.txt — only
/// core/FrameProducer.cpp and ui/ExportPanel.cpp are), so when
/// safeProcessJob's __except catches an access violation, the destructors of
/// processJob's LOCALS are skipped.  Anything owning hardware / OS resources
/// (the NVENC encoder session, demuxer file handles) therefore lives here,
/// in workerThread's frame, which unwinds normally after __except returns.
struct JobRunContext
{
    std::unique_ptr<Encoder> encoder;
    std::unordered_map<std::string, std::unique_ptr<PacketDemuxer>> demuxers;
    std::filesystem::path stagedOutputPath;
    // The audio the muxer pulls while it writes the video (declared before
    // the muxer so it outlives it).
    std::unique_ptr<AudioMixStream> audioStream;
    MuxAudioSource audioSource;
    std::unique_ptr<Muxer> muxer;   ///< writes stagedOutputPath as packets arrive

    ~JobRunContext()
    {
        // Close the container first: an open file cannot be removed.
        muxer.reset();

        // A staged container is cleared only after atomic publication.  Any
        // ordinary failure, cancellation, C++ exception, or caught SEH leaves
        // it here for guaranteed cleanup while preserving the prior target.
        if (!stagedOutputPath.empty()) {
            std::error_code ec;
            std::filesystem::remove(stagedOutputPath, ec);
            if (ec) {
                spdlog::warn("RenderQueue: could not remove incomplete staged "
                             "export '{}': {}",
                             pathToUtf8(stagedOutputPath), ec.message());
            }
        }
    }
};

namespace {

/// Safe wrapper around processJob that catches both C++ and SEH exceptions.
/// SEH (ACCESS_VIOLATION) is NOT a C++ exception — it requires __try/__except
/// on MSVC, which cannot coexist with C++ destructors in the same function.
/// Hence this is a standalone free function with NO C++ object destructors
/// (references only — ctx is owned by the caller).
void safeProcessJob(rt::RenderQueue* queue, rt::ExportJob& job,
                    rt::JobRunContext& ctx,
                    rt::Timeline* timeline, rt::Compositor* compositor)
{
#if defined(_MSC_VER)
    __try {
        queue->processJob(job, ctx, timeline, compositor);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        unsigned int code = GetExceptionCode();
        spdlog::error("RenderQueue: SEH exception 0x{:08X} in job {}", code, job.id);
        job.status = rt::JobStatus::Failed;
        job.error  = "Fatal: Hardware exception (ACCESS_VIOLATION)";
        // Cannot call m_completeCb here — it's a member of RenderQueue.
        // Caller handles notification after safeProcessJob returns.
        // ctx (encoder/demuxers) is released by the caller's normal unwind.
    }
#else
    try {
        queue->processJob(job, ctx, timeline, compositor);
    } catch (const std::exception& e) {
        spdlog::error("RenderQueue: Unhandled exception in job {}: {}", job.id, e.what());
        job.status = rt::JobStatus::Failed;
        job.error  = std::string("Fatal: ") + e.what();
    } catch (...) {
        spdlog::error("RenderQueue: Unknown exception in job {}", job.id);
        job.status = rt::JobStatus::Failed;
        job.error  = "Fatal: Unknown exception";
    }
#endif
}

} // anonymous namespace

void RenderQueue::workerThread()
{
    spdlog::info("RenderQueue: Worker started");

    while (!m_cancelAll.load()) {
        // Own reference: keeps the job alive even if removeJob erases it
        // from m_jobs while we're processing (fix for the dangling raw
        // pointer into the vector).
        std::shared_ptr<ExportJob> nextJob;

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            for (auto& j : m_jobs) {
                if (j->status.load() == JobStatus::Queued) {
                    nextJob = j;
                    j->status.store(JobStatus::Running);
                    break;
                }
            }
        }

        if (!nextJob) break; // No more queued jobs
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_latestFrame.reset();
        }

        // ctx owns the encoder + demuxers OUTSIDE the __try frame so an SEH
        // unwind (which skips processJob's local destructors) still releases
        // the NVENC session when ctx is destroyed at the end of this iteration.
        JobRunContext ctx;

        // Run job through the safe wrapper that catches ALL exceptions.
        safeProcessJob(this, *nextJob, ctx, m_timeline, m_compositor);

        // SINGLE completion-callback call site: processJob (and the SEH
        // handler) only set status/error — nothing else calls m_completeCb,
        // so a job can never notify twice.
        const JobStatus st = nextJob->status.load();
        if (m_completeCb) {
            if (st == JobStatus::Completed)
                m_completeCb(nextJob->id, true, "");
            else if (st == JobStatus::Cancelled)
                m_completeCb(nextJob->id, false, "Cancelled");
            else
                m_completeCb(nextJob->id, false, nextJob->error);
        }
    }

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_latestFrame.reset();
    }
    m_running.store(false);
    spdlog::info("RenderQueue: Worker finished");
}

void RenderQueue::processJob(ExportJob& job, JobRunContext& ctx,
                             Timeline* timeline, Compositor* compositor)
{
    spdlog::info("RenderQueue: Processing job {} → {}", job.id, pathToUtf8(job.config.outputPath));

    // A production capture failure is terminal.  Falling back to `timeline`
    // here would silently render whichever project happens to be open when the
    // worker starts, defeating queue-time immutability.
    if (!job.snapshotCaptureError.empty()) {
        job.status = JobStatus::Failed;
        job.error = job.snapshotCaptureError;
        return;
    }

    const Timeline* renderTimeline =
        (job.renderSnapshot && job.renderSnapshot->timeline)
            ? job.renderSnapshot->timeline.get()
            : timeline; // legacy no-source addJob/start callers only

    // Audio-only export: no compositor / encoder / frame loop — just mix the
    // timeline audio and write a standalone WAV/MP3/AAC/FLAC file.
    if (job.config.audioOnly) {
        processAudioOnlyJob(job, ctx, renderTimeline);
        return;
    }

    // ── Step 1: Frame renderer setup ────────────────────────────────
    // The live export composites each frame through m_frameRenderCb
    // (ExportPanel -> the preview compositor with forceFullResolution). The
    // old internal FrameRenderer was an unreachable fallback and was removed
    // (#18: the export already shared the preview compositing path), so a
    // frame-render callback is now REQUIRED.
    const uint32_t outW   = job.config.outputWidth;
    const uint32_t outH   = job.config.outputHeight;
    const int      fpsNum = job.config.encoderConfig.fpsNum;
    const int      fpsDen = job.config.encoderConfig.fpsDen;

    if (job.config.outputPath.empty() || outW == 0 || outH == 0 ||
        fpsNum <= 0 || fpsDen <= 0) {
        job.status = JobStatus::Failed;
        job.error = "RenderQueue: invalid output path, dimensions, or frame rate";
        return;
    }
    if (!m_frameRenderCb) {
        job.status = JobStatus::Failed;
        job.error = "RenderQueue: no frame-render callback set";
        return;
    }

    // Resolve every typed visual dependency before creating the encoder.
    // MediaPool and Spine loading remain asynchronous: the callback schedules
    // work on their owning UI-side services and this worker polls until the
    // snapshot is Ready, terminal, canceled, or the bounded wait expires.
    if (m_resourcePreflightCb && job.renderSnapshot) {
        constexpr auto kPreflightTimeout = std::chrono::minutes(2);
        const auto deadline = std::chrono::steady_clock::now() + kPreflightTimeout;
        for (;;) {
            if (m_cancelAll.load() || job.cancelRequested.load()) {
                job.status = JobStatus::Cancelled;
                return;
            }

            const RenderPreflightResult preflight =
                m_resourcePreflightCb(job.renderSnapshot);
            if (preflight.isReady())
                break;
            if (preflight.status == RenderResultStatus::Canceled) {
                job.status = JobStatus::Cancelled;
                return;
            }
            if (!preflight.isRetryable()) {
                job.status = JobStatus::Failed;
                job.error = preflight.warning.empty()
                    ? "Export resource preflight failed"
                    : preflight.warning;
                return;
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                job.status = JobStatus::Failed;
                job.error = "Timed out preparing export resources (" +
                    std::to_string(preflight.readyResources) + "/" +
                    std::to_string(preflight.totalResources) + " ready)";
                if (!preflight.warning.empty())
                    job.error += ": " + preflight.warning;
                return;
            }

            {
                std::lock_guard lock(m_mutex);
                job.progress.statusText = "Preparing media " +
                    std::to_string(preflight.readyResources) + "/" +
                    std::to_string(preflight.totalResources);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        std::lock_guard lock(m_mutex);
        job.progress.statusText = "Starting encoder...";
    }

    auto startTime = std::chrono::steady_clock::now();

    // ── Step 2: Encoder creation ────────────────────────────────────
    // The encoder is owned by ctx (workerThread's frame), NOT a local:
    // this TU is not /EHa, so an SEH unwind out of this function skips
    // local destructors and would leak the NVENC session.
    spdlog::info("RndQ[{}]: Step 2 — create encoder (codec={}, hw={})",
                 job.id, static_cast<int>(job.config.encoderConfig.codec),
                 static_cast<int>(job.config.encoderConfig.hwAccel));
    // The requested output canvas is authoritative.  Keeping the encoder's
    // input dimensions in lock-step prevents it from reading a different row
    // width/height than the validated compositor payload.
    job.config.encoderConfig.width = outW;
    job.config.encoderConfig.height = outH;

    std::unique_ptr<Encoder>& encoder = ctx.encoder;
    encoder = Encoder::create(job.config.encoderConfig.codec,
                              job.config.encoderConfig.hwAccel);
    if (!encoder) {
        spdlog::error("RndQ[{}]: Encoder::create returned null", job.id);
    }
    spdlog::info("RndQ[{}]: Step 2b — encoder init", job.id);
    const HardwareAccel kRequestedAccel = job.config.encoderConfig.hwAccel;
    if (!encoder || !encoder->init(job.config.encoderConfig)) {
        // Fallback: try CPU encoding
        // Record the fact so ExportPanel can surface a friendly toast.
        // On NVIDIA Pascal consumer GPUs the most common cause is that
        // another app (Discord / OBS / a previous export still draining)
        // is holding the second NVENC session — Pascal hardware caps
        // consumer encoders at 2 concurrent sessions and the NVENC API
        // doesn't surface a distinct "session limit" error code, just
        // a generic init failure.
        if (kRequestedAccel != HardwareAccel::None) {
            job.fellBackToCpuEncoder = true;
            job.fellBackReason = "Hardware encoder unavailable. If another app is "
                                 "using the GPU encoder (Discord / OBS / screen "
                                 "recorders) close it and retry for HW encoding. "
                                 "Continuing this export with CPU encoding (slower).";
        }
        // WARNING: CPU encoding is very slow. This is a TEMPORARY LAST RESORT
        // only — fix the HW encoder path instead.
        spdlog::error("RndQ[{}]: HW encoder failed — falling back to SLOW CPU encoding", job.id);
        job.config.encoderConfig.hwAccel = HardwareAccel::None;
        encoder = Encoder::create(job.config.encoderConfig.codec, HardwareAccel::None);
        if (!encoder || !encoder->init(job.config.encoderConfig)) {
            job.status = JobStatus::Failed;
            job.error = "Failed to initialize encoder";
            return;
        }
    }
    spdlog::info("RndQ[{}]: Step 2c — encoder ready", job.id);

    // An 8-bit 4:2:0 encoder takes the composite as NV12, converted on the
    // GPU: 1.5 bytes/pixel read back instead of 4, and no CPU colour
    // conversion. The video snapshot carries the format to the compositor;
    // a frame that still comes back BGRA (a cached, blank or 16F frame)
    // takes the BGRA path.
    // A hardware encoder leaves the CPU free, so decoded source frames are
    // converted on the GPU too (RenderSnapshot::gpuSourceConvert).
    const bool nv12Output = encoder->acceptsNv12() && !job.config.preserveAlpha;
    const bool gpuSourceConvert = encoder->isHardwareAccelerated();
    std::shared_ptr<const ExportRenderSnapshot> videoSnapshot = job.renderSnapshot;
    if ((nv12Output || gpuSourceConvert) && videoSnapshot) {
        auto withFormat = std::make_shared<ExportRenderSnapshot>(*videoSnapshot);
        if (nv12Output) withFormat->outputFormat = RenderOutputFormat::Nv12;
        withFormat->gpuSourceConvert = gpuSourceConvert;
        videoSnapshot = std::move(withFormat);
    }

    // ── Step 3: Frame range ─────────────────────────────────────────
    spdlog::info("RndQ[{}]: Step 3 — frame range", job.id);
    int64_t startFrame = job.config.startFrame;
    int64_t endFrame   = job.config.endFrame;
    if (endFrame <= startFrame && renderTimeline) {
        // Exact inverse of frameIndexToTick (truncating seconds*fps could
        // drop the last frame at NTSC rates).
        endFrame = tickToEndFrameIndex(
            renderTimeline->duration(),
            static_cast<uint32_t>(fpsNum), static_cast<uint32_t>(fpsDen));
    }
    int64_t totalFrames = endFrame - startFrame;
    if (totalFrames <= 0) {
        endFrame = startFrame + 1;
        totalFrames = 1;
    }
    spdlog::info("RndQ[{}]: frames [{}, {}) total={}", job.id, startFrame, endFrame, totalFrames);

    job.progress.totalFrames = totalFrames;

    // Estimate same-volume staging space before expensive rendering. This is
    // advisory because CRF output size is content-dependent; publication
    // remains fail-closed even when the estimate is low.
    {
        const double seconds = static_cast<double>(totalFrames) * fpsDen / fpsNum;
        double estimatedMbps = job.config.encoderConfig.bitrateMbps > 0
            ? static_cast<double>(job.config.encoderConfig.bitrateMbps)
            : std::max(8.0, static_cast<double>(outW) * outH * fpsNum / fpsDen
                              / (1920.0 * 1080.0 * 30.0) * 16.0);
        const uintmax_t estimatedBytes = static_cast<uintmax_t>(
            seconds * estimatedMbps * 1000000.0 / 8.0 * 1.25);
        std::error_code spaceError;
        const auto space = std::filesystem::space(job.config.outputPath.parent_path(), spaceError);
        if (!spaceError) {
            spdlog::info("RndQ[{}]: estimated staging space {:.1f} MiB; available {:.1f} MiB",
                         job.id, estimatedBytes / 1048576.0, space.available / 1048576.0);
            if (space.available < estimatedBytes)
                spdlog::warn("RndQ[{}]: available disk space is below the estimated staging requirement",
                             job.id);
        }
    }

    // ── Smart Render Analysis ───────────────────────────────────────────
    // Determine which frames can be passed through (raw packet copy from
    // source) and which must be composited + re-encoded.
    // Smart Render passthrough is DISABLED.  It copied raw source packets into
    // an output stream whose H.264 parameter sets (SPS/PPS) come from the
    // re-encoder and are written as the mp4 avcC global header (Muxer.cpp).
    // There is no bitstream filter or per-source extradata reconciliation, so
    // any passthrough packet whose source SPS/PPS differ from the encoder's
    // (essentially always — and always when two different source files share
    // one track) decodes against the wrong parameters: the player shows
    // unrelated content until the next re-encoded IDR resyncs → the "flickers
    // to a completely different video" bug.  The analyzer was also blind to
    // transitions, passing through transition-affected frames at full opacity
    // (the color-matte "bright first frame" on export).  Re-encoding every
    // frame is guaranteed correct.  To safely re-enable, this needs: an
    // h264_mp4toannexb/av_bsf bitstream path, source-extradata handling, a
    // refusal to mix incompatible/multiple sources, and transition-awareness
    // in analyzeSmartRender.
    SmartRenderPlan smartPlan;
    const bool intraCodec = job.config.encoderConfig.codec == EncoderCodec::ProRes ||
                            job.config.encoderConfig.codec == EncoderCodec::DNxHR;
    if (intraCodec && renderTimeline) {
        smartPlan = analyzeSmartRender(*renderTimeline, job.config.encoderConfig,
                                       job.config.outputWidth, job.config.outputHeight,
                                       startFrame, endFrame);
        // Never mix copied and encoded packets. A partial plan can cross codec
        // headers or edit/GOP boundaries, so it always falls back in full.
        if (smartPlan.passthroughCount != totalFrames)
            smartPlan = {};
    }

    // Open PacketDemuxers for source files used in passthrough.
    // Key = media path, value = shared demuxer instance.
    // Owned by ctx for the same SEH-unwind reason as the encoder.
    auto& demuxers = ctx.demuxers;
    if (smartPlan.passthroughCount > 0) {
        for (const auto& [frameIdx, pf] : smartPlan.passthroughFrames) {
            if (demuxers.count(pf.mediaPath) == 0) {
                auto dmx = std::make_unique<PacketDemuxer>();
                if (dmx->open(pf.mediaPath)) {
                    demuxers[pf.mediaPath] = std::move(dmx);
                } else {
                    spdlog::warn("SmartRender: Could not open '{}' — those frames will re-encode",
                                 pf.mediaPath);
                }
            }
        }
        if (demuxers.size() != 1 ||
            !demuxers.begin()->second->strictlyMatches(
                encoder->avCodecContext(), outW, outH, fpsNum, fpsDen)) {
            spdlog::info("SmartRender: source parameters are not an exact match; full render");
            smartPlan = {};
            demuxers.clear();
        }
    }

    // ── Output container, written as the packets are produced ───────
    // Packets used to be deep-copied and held until the last frame (~4.5 GB
    // per hour at 10 Mbps). The muxer now writes each one as the encoder
    // emits it, pulling the audio mix up to that point, into a staged file
    // that is published only after validation.
    ctx.stagedOutputPath = makeStagedExportPath(job.config.outputPath, job.id);
    if (ctx.stagedOutputPath.empty()) {
        job.status = JobStatus::Failed;
        job.error = "Could not create a staged export path";
        encoder->shutdown();
        return;
    }

    // Audio consumes the SAME renderTimeline used for duration/video dispatch,
    // so edits cannot produce queue-time audio under live/current video (or the
    // reverse). It is mixed block by block while the muxer writes the video.
    if (job.config.includeAudio && renderTimeline) {
        ctx.audioStream = std::make_unique<AudioMixStream>(*renderTimeline, job.config.audioConfig);
        if (!ctx.audioStream->isValid()) ctx.audioStream.reset();
    }
    if (ctx.audioStream) {
        AudioMixStream* stream = ctx.audioStream.get();
        ctx.audioSource.sampleRate = stream->sampleRate();
        ctx.audioSource.channels   = stream->channels();
        ctx.audioSource.pull = [stream](float* out, int64_t maxFrames) {
            return stream->read(out, maxFrames);
        };
    }

    MuxerConfig mCfg;
    mCfg.outputPath     = ctx.stagedOutputPath;
    mCfg.format         = static_cast<ContainerFormat>(job.config.containerFormat);
    mCfg.videoWidth     = job.config.outputWidth;
    mCfg.videoHeight    = job.config.outputHeight;
    mCfg.videoFpsNum    = job.config.encoderConfig.fpsNum;
    mCfg.videoFpsDen    = job.config.encoderConfig.fpsDen;
    mCfg.hasAudio       = ctx.audioStream != nullptr;
    mCfg.audioSampleRate = job.config.audioConfig.sampleRate;
    mCfg.audioChannels   = static_cast<uint16_t>(job.config.audioConfig.channels);
    // Map encoder codec to AVCodecID for the container header.
    // Without this the muxer falls back to H264, which is wrong for H265/AV1.
    mCfg.videoCodecId = encoder ? encoder->avCodecId() : 0;
    // The opened codec context, so the muxer copies extradata (SPS/PPS, hvcC,
    // AV1 seq header) + full codec params into the container's stream. Every
    // encoder opens with AV_CODEC_FLAG_GLOBAL_HEADER, so the extradata exists
    // before the first frame; the encoder stays alive until the file is closed.
    mCfg.videoCodecContext = smartPlan.passthroughCount == totalFrames && demuxers.size() == 1
        ? demuxers.begin()->second->codecContext()
        : (encoder ? encoder->avCodecContext() : nullptr);
    spdlog::info("RndQ[{}]: mux config fps={}/{} codecId={}", job.id,
                 mCfg.videoFpsNum, mCfg.videoFpsDen, mCfg.videoCodecId);

    ctx.muxer = std::make_unique<Muxer>();
    if (!ctx.muxer->beginStream(mCfg, ctx.audioStream ? &ctx.audioSource : nullptr)) {
        spdlog::error("RenderQueue: could not open the output for job {}", job.id);
        job.status = JobStatus::Failed;
        job.error = "Muxing failed — output file not written";
        encoder->shutdown();
        return;
    }

    // Render frame by frame. Packets go to the muxer in the order the encoder
    // emits them (decode order). An encoder packet with no payload cannot
    // contribute a decodable frame and must never reach the muxer.
    // Returns "" when written, else the job error.
    int64_t packetsWritten = 0;
    int64_t firstPacketPts = 0;
    int64_t lastPacketPts  = 0;
    auto writePacket = [&](const EncodedPacket& pkt,
                           const char* emptyPacketError) -> std::string {
        if (!pkt.data || pkt.size <= 0)
            return emptyPacketError;
        if (!ctx.muxer->writeStreamVideo(pkt))
            return "Muxing failed at video packet " + std::to_string(pkt.pts) +
                   (ctx.muxer->lastError().empty() ? "" : ": " + ctx.muxer->lastError());
        if (packetsWritten++ == 0) firstPacketPts = pkt.pts;
        lastPacketPts = pkt.pts;
        return {};
    };

    // Helper: update render progress.  Numeric fields are atomics (polled by
    // the UI thread); statusText is written under the mutex (readers use
    // RenderQueue::jobStatusText).
    auto updateProgress = [&](int64_t currentFrame) {
        auto now = std::chrono::steady_clock::now();
        double elapsed = std::chrono::duration<double>(now - startTime).count();
        int64_t framesCompleted = currentFrame - startFrame + 1;
        const double fps = elapsed > 0 ? framesCompleted / elapsed : 0.0;

        job.progress.currentFrame = framesCompleted;
        job.progress.percent = kRenderProgressShare * static_cast<float>(framesCompleted) / totalFrames;
        job.progress.elapsedSeconds = elapsed;
        job.progress.fps = fps;
        job.progress.estimatedRemaining = fps > 0
            ? (totalFrames - framesCompleted) / fps : 0.0;
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            job.progress.statusText = "Rendering frame " + std::to_string(framesCompleted) +
                                      "/" + std::to_string(totalFrames);
        }

        if (m_progressCb) m_progressCb(job.id, job.progress);
    };

    // ── Step 4: Frame loop ──────────────────────────────────────────
    spdlog::info("RndQ[{}]: Step 4 — starting frame loop ({} frames)", job.id, totalFrames);
    bool cancelled = false;
    uint64_t segmentCacheHits = 0;
    uint64_t segmentCacheMisses = 0;
    int64_t acceptedVideoFrames = 0;
    int64_t encodedVideoFrames = 0;

    // ── Smart Render passthrough run state ──────────────────────────────
    // A copied-packet run is only a decodable output GOP if it BEGINS on a
    // source keyframe and the copied frames are contiguous in source order.
    // Starting a run on a P/B frame — which happens whenever a clip's
    // in-point is not GOP-aligned (the common case for long-GOP H.264) —
    // emits packets that reference frames the output stream never contains,
    // so the player drops them until the next keyframe.  That is exactly the
    // "exported clip skips its first frames / jumps into the middle" bug.
    // We therefore only pass a packet through when it is a keyframe (valid
    // run start) or it directly continues the current run; otherwise the
    // frame falls through to composite + re-encode.  This only ever shifts
    // work from passthrough to re-encode, so it cannot make output worse
    // than a full re-encode.
    std::string ptRunMedia;
    int64_t     ptExpectedSrc = -1;
    const bool passthroughRequired = smartPlan.passthroughCount == totalFrames;

    // After an encode call: fail the job if the encoder rejected frame f,
    // else write whatever packets it produced (none while it fills its
    // look-ahead, several when B-frames drain). False when the job failed.
    auto collectEncoded = [&](bool gotPacket, int64_t submissionsBefore,
                              int64_t f, const char* rejected) -> bool {
        if (encoder->hasFatalError() ||
            encoder->framesSubmitted() != submissionsBefore + 1) {
            job.status = JobStatus::Failed;
            job.error = encoder->lastError().empty()
                ? rejected + std::to_string(f)
                : encoder->lastError();
            encoder->shutdown();
            return false;
        }
        ++acceptedVideoFrames;
        ++encodedVideoFrames;
        std::string err;
        if (gotPacket)
            err = writePacket(encoder->lastPacket(), "Encoder emitted an empty packet");
        else
            spdlog::debug("RndQ[{}]: encoder buffering at f={} (no packet yet)",
                          job.id, f);
        for (const auto& ep : encoder->pendingPackets()) {
            if (!err.empty()) break;
            err = writePacket(ep, "Encoder emitted an empty pending packet");
        }
        if (!err.empty()) {
            job.status = JobStatus::Failed;
            job.error = err;
            encoder->shutdown();
            return false;
        }
        return true;
    };
    int64_t lastNv12PreviewFrame = -1;

    for (int64_t f = startFrame; f < endFrame; ++f) {
        // Check cancellation (per-job atomic on the job itself — no map race)
        if (m_cancelAll.load() || job.cancelRequested.load()) {
            cancelled = true;
            break;
        }

        // ── Smart Render Passthrough ────────────────────────────────────
        // If this frame is passthrough-eligible, copy the raw packet from
        // source instead of compositing + re-encoding.
        int64_t frameIdx = f - startFrame;
        auto ptIt = smartPlan.passthroughFrames.find(frameIdx);
        if (ptIt != smartPlan.passthroughFrames.end()) {
            const auto& pf = ptIt->second;
            auto dmxIt = demuxers.find(pf.mediaPath);
            if (dmxIt != demuxers.end() && dmxIt->second) {
                EncodedPacket rawPkt{};
                if (dmxIt->second->readFrame(pf.sourceFrame, rawPkt)) {
                    // A passthrough packet may only be emitted if it keeps the
                    // output GOP decodable: it is a source keyframe (a valid
                    // run start) OR it contiguously continues the current run
                    // from the same source file.  Otherwise the emitted stream
                    // would begin mid-GOP and the player would drop the clip's
                    // leading frames until the next keyframe.
                    const bool validStart = rawPkt.isKeyframe;
                    const bool validContinue =
                        (pf.mediaPath == ptRunMedia &&
                         pf.sourceFrame == ptExpectedSrc);
                    if (validStart || validContinue) {
                        // Restamp PTS/DTS to output timeline
                        rawPkt.pts = frameIdx;
                        rawPkt.dts = frameIdx;
                        rawPkt.duration = 1;

                        if (!rawPkt.data || rawPkt.size <= 0) {
                            spdlog::warn("RndQ[{}]: passthrough source returned an "
                                         "empty packet for frame {}; re-encoding",
                                         job.id, f);
                        } else if (const std::string err = writePacket(rawPkt, "");
                                   !err.empty()) {
                            job.status = JobStatus::Failed;
                            job.error = err;
                            encoder->shutdown();
                            return;
                        } else {
                            ++acceptedVideoFrames;
                            ptRunMedia    = pf.mediaPath;
                            ptExpectedSrc = pf.sourceFrame + 1;
                            updateProgress(f);
                            continue;
                        }
                    }
                    // Non-keyframe run start (clip in-point not GOP-aligned):
                    // fall through to re-encode so the leading frames survive.
                }
                // Fall through to normal render if read failed
            }
        }
        // A proven plan is all-or-nothing. Never silently create a stream that
        // mixes source codec headers with newly encoded packets if a source
        // packet fails the final runtime checks.
        if (passthroughRequired) {
            job.status = JobStatus::Failed;
            job.error = "Smart rendering source packet validation failed at frame " +
                        std::to_string(f) + "; no output was published";
            encoder->shutdown();
            return;
        }
        // Reaching here means this frame is composited + re-encoded, which
        // breaks any in-progress passthrough run.  The next passthrough frame
        // must re-validate from a keyframe before copying resumes.
        ptRunMedia.clear();
        ptExpectedSrc = -1;

        // Render frame. Composited frames are BGRA; the encoder's swscale
        // converts BGRA→YUV420P directly (SIMD), so we pass the buffer
        // straight through — no per-frame CPU channel-swap + 4K alloc.
        // The holders below MUST outlive the encodeFrame() call.
        std::shared_ptr<CachedFrame> cframe;
        const uint8_t* encodePixels = nullptr;
        ExportFrameValidation frameValidation;

        // Composite this frame via the main-thread callback (preview compositor;
        // produces a BGRA CachedFrame). Async pre-submit of the next frame's
        // tick lets the main thread composite f+1 while the worker encodes f.
        const int64_t tick = frameIndexToTick(
            f, static_cast<uint32_t>(fpsNum), static_cast<uint32_t>(fpsDen));
        int64_t nextTick = -1;
        if (f + 1 < endFrame) {
            nextTick = frameIndexToTick(
                f + 1, static_cast<uint32_t>(fpsNum), static_cast<uint32_t>(fpsDen));
        }
        // Transient decoder/GPU misses are recoverable, but only by retrying
        // the SAME timeline tick.  Never advance progress or touch the encoder
        // until a complete frame payload has been returned.
        constexpr int kMaxCompositeAttempts = 3;
        RenderResult renderResult;
        int compositeAttempts = 0;
        for (int attempt = 1; attempt <= kMaxCompositeAttempts; ++attempt) {
            compositeAttempts = attempt;
            if (m_cancelAll.load() || job.cancelRequested.load()) {
                cancelled = true;
                break;
            }
            renderResult = m_frameRenderCb(videoSnapshot,
                                           tick, nextTick, outW, outH, true,
                                           job.config.preserveAlpha);
            cframe = renderResult.frame;

            if (renderResult.status == RenderResultStatus::Canceled) {
                cancelled = true;
                break;
            }
            if (renderResult.status == RenderResultStatus::Failed ||
                renderResult.status == RenderResultStatus::MissingMedia) {
                frameValidation.valid = false;
                frameValidation.error = std::string(toString(renderResult.status));
                if (!renderResult.diagnostics.warning.empty())
                    frameValidation.error += ": " + renderResult.diagnostics.warning;
                break;
            }

            if (renderResult.status == RenderResultStatus::Pending ||
                renderResult.status == RenderResultStatus::HeldPrevious) {
                frameValidation.valid = false;
                frameValidation.error = renderResult.status == RenderResultStatus::Pending
                    ? "render is not complete yet"
                    : "renderer returned a held preview frame";
            } else {
                frameValidation = validateExportFrame(
                    cframe.get(), outW, outH, encoder->is10BitTarget(), nv12Output);
            }
            if (frameValidation.valid)
                break;

            spdlog::warn("RndQ[{}]: {} composite for frame {} "
                         "(attempt {}/{}): {}",
                         job.id, toString(renderResult.status), f,
                         attempt, kMaxCompositeAttempts,
                         frameValidation.error);
            if (attempt < kMaxCompositeAttempts)
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        if (cancelled)
            break;
        if (!frameValidation.valid) {
            job.status = JobStatus::Failed;
            job.error = "Rendering failed at frame " + std::to_string(f) +
                        " after " + std::to_string(compositeAttempts) +
                        " attempts: " + frameValidation.error;
            encoder->shutdown();
            return;
        }

        if (cframe && cframe->segmentCacheHit)
            ++segmentCacheHits;
        else
            ++segmentCacheMisses;

        if (cframe && !cframe->pixels.empty()) {
            std::lock_guard<std::mutex> lk(m_mutex);
            m_latestFrame = cframe;
        }

        // §4.2 export 16F passthrough: a single full-frame opaque >8-bit clip
        // arrives as a dual-payload frame (RGBA16F + an 8-bit BGRA copy).  For
        // a 10-bit encoder, feed the RGBA16F directly — real 10-bit, no lossy
        // BGRA round-trip — and SKIP the 8-bit segment write-through (the cache
        // is BGRA-only).  8-bit encoders / non-passthrough frames fall through
        // to the normal path below.
        if (frameValidation.useNv12) {
            // The export panel's preview needs BGRA; a small one every few
            // frames is plenty (it polls ten times a second).
            constexpr int64_t kNv12PreviewEvery = 8;
            if (lastNv12PreviewFrame < 0 || f - lastNv12PreviewFrame >= kNv12PreviewEvery) {
                auto preview = nv12PreviewFrame(*cframe, /*step=*/4);
                std::lock_guard<std::mutex> lk(m_mutex);
                m_latestFrame = std::move(preview);
                lastNv12PreviewFrame = f;
            }
            const uint32_t stride = cframe->nv12Stride;
            const uint8_t* y = cframe->nv12.get();
            const int64_t submissionsBefore = encoder->framesSubmitted();
            const bool got = encoder->encodeFrameNv12(
                y, static_cast<int>(stride),
                y + static_cast<size_t>(stride) * outH, static_cast<int>(stride),
                f - startFrame);
            if (!collectEncoded(got, submissionsBefore, f, "Encoder rejected NV12 frame "))
                return;
            updateProgress(f);
            continue;
        }

        if (frameValidation.useRgba16f) {
            const int64_t submissionsBefore = encoder->framesSubmitted();
            const bool got = encoder->encodeFrame16f(
                reinterpret_cast<const uint16_t*>(cframe->rgba16f.data()),
                static_cast<int>(cframe->rgba16fStride), f - startFrame);
            if (!collectEncoded(got, submissionsBefore, f, "Encoder rejected RGBA16F frame "))
                return;
            updateProgress(f);
            continue;
        }

        // The callback MUST have populated CPU pixels on the main thread before
        // returning (it does ensurePixels inside the BlockingQueuedConnection
        // dispatch). We do NOT call ensurePixels() here — it may trigger a GPU
        // readback (lazyReadback) which is NOT thread-safe.
        std::vector<uint8_t> tightBgra;
        if (frameValidation.needsBgraRepack) {
            const size_t rowBytes = static_cast<size_t>(outW) * 4u;
            tightBgra.resize(rowBytes * static_cast<size_t>(outH));
            for (uint32_t row = 0; row < outH; ++row) {
                std::memcpy(tightBgra.data() + static_cast<size_t>(row) * rowBytes,
                            cframe->pixels.data() +
                                static_cast<size_t>(row) * cframe->stride,
                            rowBytes);
            }
            encodePixels = tightBgra.data();
        } else {
            encodePixels = cframe->pixels.data();
        }

        // §4.6 export write-through: this frame is full-res with CPU pixels
        // ready — store it in the segment cache so a re-export reuses it.
        if (m_frameStoreCb)
            m_frameStoreCb(videoSnapshot, tick, cframe);

        // Encode frame
        const int64_t submissionsBefore = encoder->framesSubmitted();
        const bool gotPacket = encoder->encodeFrame(encodePixels, f - startFrame);
        if (!collectEncoded(gotPacket, submissionsBefore, f, "Encoder rejected frame "))
            return;
        updateProgress(f);
    }

    if (cancelled) {
        job.status = JobStatus::Cancelled;
        encoder->shutdown();
        return;
    }

    if (acceptedVideoFrames != totalFrames ||
        encoder->framesSubmitted() != encodedVideoFrames) {
        job.status = JobStatus::Failed;
        job.error = "Export frame accounting mismatch; refusing incomplete output";
        encoder->shutdown();
        return;
    }

    // Flush encoder
    encoder->flush();
    if (encoder->hasFatalError()) {
        job.status = JobStatus::Failed;
        job.error = encoder->lastError().empty()
            ? "Encoder failed while flushing"
            : encoder->lastError();
        encoder->shutdown();
        return;
    }
    for (const auto& ep : encoder->flushedPackets()) {
        if (const std::string err = writePacket(ep, "Encoder emitted an empty flushed packet");
            !err.empty()) {
            job.status = JobStatus::Failed;
            job.error = err;
            encoder->shutdown();
            return;
        }
    }
    if (packetsWritten == 0) {
        job.status = JobStatus::Failed;
        job.error = "Encoder produced no video packets";
        encoder->shutdown();
        return;
    }

    {
        std::lock_guard<std::mutex> lk(m_mutex);
        job.progress.statusText = "Muxing...";
    }
    if (m_progressCb) m_progressCb(job.id, job.progress);

    spdlog::info("RndQ[{}]: total packets={} first_pts={} last_pts={} fps={}/{}",
                 job.id, packetsWritten, firstPacketPts, lastPacketPts,
                 mCfg.videoFpsNum, mCfg.videoFpsDen);

    // The rest of the audio, then the trailer.
    const bool muxOk = ctx.muxer->finishStream();
    ctx.muxer.reset();

    // Safe to release the encoder now that the file is closed.
    encoder->shutdown();

    if (!muxOk) {
        spdlog::error("RenderQueue: Muxing failed for job {}", job.id);
        job.status = JobStatus::Failed;
        job.error  = "Muxing failed — output file not written";
        return;
    }

    {
        std::lock_guard<std::mutex> lk(m_mutex);
        job.progress.statusText = "Validating export...";
    }
    if (m_progressCb) m_progressCb(job.id, job.progress);

    const auto validationStart = std::chrono::steady_clock::now();
    const ExportFileValidation validation = validateExportFile(
        ctx.stagedOutputPath, totalFrames, outW, outH, fpsNum, fpsDen,
        [this, &job](int64_t decoded, int64_t expected) {
            job.progress.percent = kRenderProgressShare
                + (99.9f - kRenderProgressShare) * static_cast<float>(decoded) / expected;
            {
                std::lock_guard<std::mutex> lk(m_mutex);
                job.progress.statusText = "Validating frame " + std::to_string(decoded)
                    + "/" + std::to_string(expected);
            }
            if (m_progressCb) m_progressCb(job.id, job.progress);
        });
    const double validationSeconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - validationStart).count();
    spdlog::info("RndQ[{}]: strict validation decoded {} frames in {:.2f}s ({:.1f} fps)",
                 job.id, validation.decodedFrames, validationSeconds,
                 validationSeconds > 0.0 ? validation.decodedFrames / validationSeconds : 0.0);
    if (!validation.ok) {
        spdlog::error("RenderQueue: validation failed for job {}: {}",
                      job.id, validation.error);
        job.status = JobStatus::Failed;
        job.error = "Export validation failed: " + validation.error;
        return;
    }

    std::string publishError;
    if (!publishStagedExport(ctx.stagedOutputPath, job.config.outputPath,
                             publishError)) {
        spdlog::error("RenderQueue: publication failed for job {}: {}",
                      job.id, publishError);
        job.status = JobStatus::Failed;
        job.error = publishError;
        return;
    }
    // Ownership moved atomically to outputPath; prevent context cleanup from
    // attempting to remove the now-nonexistent staged name.
    ctx.stagedOutputPath.clear();

    job.progress.percent = 100.0f;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        job.progress.statusText = "Complete";
    }
    job.status = JobStatus::Completed;

    auto endTime = std::chrono::steady_clock::now();
    double totalElapsed = std::chrono::duration<double>(endTime - startTime).count();
    const uint64_t cacheLookups = segmentCacheHits + segmentCacheMisses;
    const double cacheHitPercent = cacheLookups > 0
        ? 100.0 * static_cast<double>(segmentCacheHits) / cacheLookups : 0.0;
    spdlog::info("RndQ[{}]: composite segment cache: {} hit(s), {} miss(es) "
                 "({:.1f}% reused); cached frames were still encoded",
                 job.id, segmentCacheHits, segmentCacheMisses, cacheHitPercent);
    spdlog::info("RenderQueue: Job {} complete ({:.1f}s, {:.0f} fps)",
                 job.id, totalElapsed, totalFrames / totalElapsed);
    // Completion callback fires ONCE, in workerThread, after this returns.
}

void RenderQueue::processAudioOnlyJob(ExportJob& job, JobRunContext& ctx,
                                      const Timeline* timeline)
{
    spdlog::info("RndQ[{}]: audio-only export → {}", job.id, pathToUtf8(job.config.outputPath));

    if (!timeline) {
        job.status = JobStatus::Failed;
        job.error  = "No timeline loaded — nothing to export";
        return;
    }

    auto startTime = std::chrono::steady_clock::now();

    auto isCancelled = [&]() {
        return m_cancelAll.load() || job.cancelRequested.load();
    };

    auto setStatusText = [&](const std::string& s) {
        std::lock_guard<std::mutex> lk(m_mutex);
        job.progress.statusText = s;
    };

    job.progress.totalFrames = 1;
    setStatusText("Mixing audio...");
    if (m_progressCb) m_progressCb(job.id, job.progress);

    if (isCancelled()) {
        job.status = JobStatus::Cancelled;
        return;
    }

    // Mix every audio track over the configured range and write it as it is
    // mixed, a block at a time, so memory does not grow with the duration.
    AudioMixStream stream(*timeline, job.config.audioConfig);
    if (!stream.isValid()) {
        job.status = JobStatus::Failed;
        job.error  = stream.lastError().empty()
            ? "No audio to export (the timeline has no audio in this range)"
            : stream.lastError();
        return;
    }

    // Written beside the destination and published only once complete, so a
    // cancel or failure never leaves a truncated file at the output path
    // (JobRunContext removes the staged file).
    ctx.stagedOutputPath = makeStagedExportPath(job.config.outputPath, job.id);
    if (ctx.stagedOutputPath.empty()) {
        job.status = JobStatus::Failed;
        job.error  = "Could not create a staged export path";
        return;
    }

    const auto writeFailed = [&](const std::string& detail) {
        spdlog::error("RndQ[{}]: audio-only export failed: {}", job.id, detail);
        job.status = JobStatus::Failed;
        job.error  = "Failed to write the audio file (the encoder or output path "
                     "may be unavailable — see the log for details)";
    };

    AudioFileWriter writer;
    if (!writer.open(ctx.stagedOutputPath, job.config.audioConfig.codec,
                     stream.sampleRate(), stream.channels(),
                     job.config.audioConfig.bitrate)) {
        writeFailed(writer.lastError());
        return;
    }

    constexpr int64_t kBlockFrames = 1 << 16;   // ~1.4 s at 48 kHz
    std::vector<float> block(static_cast<size_t>(kBlockFrames) * stream.channels());
    const int64_t totalFrames = stream.totalFrames();
    while (stream.position() < totalFrames) {
        if (isCancelled()) {
            job.status = JobStatus::Cancelled;
            return;
        }
        const int64_t framesMixed = stream.read(block.data(), kBlockFrames);
        if (framesMixed <= 0) break;
        if (!writer.write(block.data(), framesMixed)) {
            writeFailed(writer.lastError());
            return;
        }
        job.progress.percent = 99.0f * static_cast<float>(stream.position()) /
                               static_cast<float>(totalFrames);
        if (m_progressCb) m_progressCb(job.id, job.progress);
    }

    setStatusText("Writing audio file...");
    if (m_progressCb) m_progressCb(job.id, job.progress);
    if (!writer.finish()) {
        writeFailed(writer.lastError());
        return;
    }

    std::string publishError;
    if (!publishStagedExport(ctx.stagedOutputPath, job.config.outputPath, publishError)) {
        spdlog::error("RenderQueue: publication failed for job {}: {}", job.id, publishError);
        job.status = JobStatus::Failed;
        job.error  = publishError;
        return;
    }
    ctx.stagedOutputPath.clear();

    job.progress.currentFrame = 1;
    job.progress.percent    = 100.0f;
    setStatusText("Complete");
    job.status = JobStatus::Completed;

    double elapsed = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - startTime).count();
    spdlog::info("RndQ[{}]: audio-only export complete ({:.1f}s, {:.1f}s of audio)",
                 job.id, elapsed, stream.duration());
    // Completion callback fires ONCE, in workerThread, after this returns.
}

} // namespace rt
