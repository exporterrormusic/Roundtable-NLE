/*
 * RenderQueue.cpp — Background render job queue.
 */

#include "RenderQueue.h"
#include "AudioMixdown.h"
#include "Muxer.h"
#include "Encoder.h"
#include "SmartRenderAnalyzer.h"
#include "PacketDemuxer.h"

#include "cache/FrameCache.h"
#include "timeline/Timeline.h"
#include "PathUtils.h"

#include <spdlog/spdlog.h>

#include <chrono>
#include <exception>
#include <stdexcept>
#include <unordered_map>

#ifdef _MSC_VER
#include <excpt.h>  // GetExceptionCode, EXCEPTION_EXECUTE_HANDLER
#endif

namespace rt {

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
    if (m_worker.joinable()) m_worker.join();
}

uint32_t RenderQueue::addJob(const ExportJobConfig& config)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    ExportJob job;
    job.id     = m_nextJobId++;
    job.config = config;
    job.status = JobStatus::Queued;
    m_jobs.push_back(std::move(job));
    spdlog::info("RenderQueue: Added job {} → {}", job.id, pathToUtf8(config.outputPath));
    return m_jobs.back().id;
}

bool RenderQueue::removeJob(uint32_t jobId)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto it = m_jobs.begin(); it != m_jobs.end(); ++it) {
        if (it->id == jobId) {
            if (it->status == JobStatus::Running) return false;
            m_jobs.erase(it);
            return true;
        }
    }
    return false;
}

void RenderQueue::start(Timeline* timeline, Compositor* compositor)
{
    if (m_running.load()) return;

    // Join previous worker thread if it finished but wasn't joined yet.
    // Without this, assigning to a joinable std::thread calls std::terminate().
    if (m_worker.joinable()) m_worker.join();

    m_timeline   = timeline;
    m_compositor = compositor;
    m_running    = true;
    m_cancelAll  = false;

    m_worker = std::thread(&RenderQueue::workerThread, this);
}

void RenderQueue::cancelJob(uint32_t jobId)
{
    auto it = m_cancelFlags.find(jobId);
    if (it != m_cancelFlags.end()) {
        it->second.store(true);
    }
}

void RenderQueue::cancelAll()
{
    m_cancelAll.store(true);
    for (auto& [id, flag] : m_cancelFlags) {
        flag.store(true);
    }
}

void RenderQueue::waitForAll()
{
    if (m_worker.joinable()) m_worker.join();
}

std::vector<ExportJob> RenderQueue::jobs() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_jobs;
}

const ExportJob* RenderQueue::job(uint32_t jobId) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    for (const auto& j : m_jobs) {
        if (j.id == jobId) return &j;
    }
    return nullptr;
}

size_t RenderQueue::pendingCount() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    size_t count = 0;
    for (const auto& j : m_jobs) {
        if (j.status == JobStatus::Queued || j.status == JobStatus::Running)
            ++count;
    }
    return count;
}

namespace {

/// Safe wrapper around processJob that catches both C++ and SEH exceptions.
/// SEH (ACCESS_VIOLATION) is NOT a C++ exception — it requires __try/__except
/// on MSVC, which cannot coexist with C++ destructors in the same function.
/// Hence this is a standalone free function with NO C++ object destructors.
void safeProcessJob(rt::RenderQueue* queue, rt::ExportJob& job,
                    rt::Timeline* timeline, rt::Compositor* compositor)
{
#if defined(_MSC_VER)
    __try {
        queue->processJob(job, timeline, compositor);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        unsigned int code = GetExceptionCode();
        spdlog::error("RenderQueue: SEH exception 0x{:08X} in job {}", code, job.id);
        job.status = rt::JobStatus::Failed;
        job.error  = "Fatal: Hardware exception (ACCESS_VIOLATION)";
        // Cannot call m_completeCb here — it's a member of RenderQueue.
        // Caller handles notification after safeProcessJob returns.
    }
#else
    try {
        queue->processJob(job, timeline, compositor);
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
        ExportJob* nextJob = nullptr;

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            for (auto& j : m_jobs) {
                if (j.status == JobStatus::Queued) {
                    nextJob = &j;
                    nextJob->status = JobStatus::Running;
                    m_cancelFlags[j.id].store(false);
                    break;
                }
            }
        }

        if (!nextJob) break; // No more queued jobs

        // Run job through the safe wrapper that catches ALL exceptions.
        safeProcessJob(this, *nextJob, m_timeline, m_compositor);

        // If safeProcessJob caught a SEH exception or C++ exception,
        // the job was marked Failed above.  Notify the completion callback.
        if (nextJob->status == JobStatus::Failed) {
            if (m_completeCb)
                m_completeCb(nextJob->id, false, nextJob->error);
        }
    }

    m_running.store(false);
    spdlog::info("RenderQueue: Worker finished");
}

void RenderQueue::processJob(ExportJob& job, Timeline* timeline, Compositor* compositor)
{
    spdlog::info("RenderQueue: Processing job {} → {}", job.id, pathToUtf8(job.config.outputPath));

    auto startTime = std::chrono::steady_clock::now();

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

    if (!m_frameRenderCb) {
        job.status = JobStatus::Failed;
        job.error = "RenderQueue: no frame-render callback set";
        if (m_completeCb) m_completeCb(job.id, false, job.error);
        return;
    }

    // ── Step 2: Encoder creation ────────────────────────────────────
    spdlog::info("RndQ[{}]: Step 2 — create encoder (codec={}, hw={})",
                 job.id, static_cast<int>(job.config.encoderConfig.codec),
                 static_cast<int>(job.config.encoderConfig.hwAccel));
    auto encoder = Encoder::create(job.config.encoderConfig.codec,
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
        // only Ã¢â‚¬â€ fix the HW encoder path instead.
        spdlog::error("RndQ[{}]: HW encoder failed Ã¢â‚¬â€ falling back to SLOW CPU encoding", job.id);
        job.config.encoderConfig.hwAccel = HardwareAccel::None;
        encoder = Encoder::create(job.config.encoderConfig.codec, HardwareAccel::None);
        if (!encoder || !encoder->init(job.config.encoderConfig)) {
            job.status = JobStatus::Failed;
            job.error = "Failed to initialize encoder";
            if (m_completeCb) m_completeCb(job.id, false, job.error);
            return;
        }
    }
    spdlog::info("RndQ[{}]: Step 2c — encoder ready", job.id);

    // ── Step 3: Frame range ─────────────────────────────────────────
    spdlog::info("RndQ[{}]: Step 3 — frame range", job.id);
    int64_t startFrame = job.config.startFrame;
    int64_t endFrame   = job.config.endFrame;
    if (endFrame <= startFrame && timeline) {
        double duration = ticksToSeconds(timeline->duration());
        endFrame = static_cast<int64_t>(duration * fpsNum / fpsDen);
    }
    int64_t totalFrames = endFrame - startFrame;
    if (totalFrames <= 0) totalFrames = 1;
    spdlog::info("RndQ[{}]: frames [{}, {}) total={}", job.id, startFrame, endFrame, totalFrames);

    job.progress.totalFrames = totalFrames;

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
    constexpr bool kEnableSmartRenderPassthrough = false;
    SmartRenderPlan smartPlan;
    if constexpr (kEnableSmartRenderPassthrough) {
        if (timeline) {
            smartPlan = analyzeSmartRender(*timeline, job.config.encoderConfig,
                                           job.config.outputWidth, job.config.outputHeight,
                                           startFrame, endFrame);
        }
    }

    // Open PacketDemuxers for source files used in passthrough.
    // Key = media path, value = shared demuxer instance.
    std::unordered_map<std::string, std::unique_ptr<PacketDemuxer>> demuxers;
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
    }

    // Render frame by frame
    // OwnedPacket stores a deep copy of encoded data so packet pointers
    // remain valid until muxing completes (encoder reuses its internal
    // AVPacket buffer on each encode call).
    struct OwnedPacket {
        std::vector<uint8_t> storage;
        EncodedPacket        pkt;
    };

    // Helper: create an OwnedPacket from an EncodedPacket by deep-copying storage
    auto makeOwnedPacket = [](const EncodedPacket& pkt) -> OwnedPacket {
        OwnedPacket op;
        op.storage.assign(pkt.data, pkt.data + pkt.size);
        op.pkt = pkt;
        op.pkt.data = op.storage.data();
        op.pkt.ownsData = false;
        return op;
    };

    // Helper: update render progress
    auto updateProgress = [&](int64_t currentFrame) {
        auto now = std::chrono::steady_clock::now();
        double elapsed = std::chrono::duration<double>(now - startTime).count();
        int64_t framesCompleted = currentFrame - startFrame + 1;

        job.progress.currentFrame = framesCompleted;
        job.progress.percent = 100.0f * static_cast<float>(framesCompleted) / totalFrames;
        job.progress.elapsedSeconds = elapsed;
        job.progress.fps = elapsed > 0 ? framesCompleted / elapsed : 0;
        job.progress.estimatedRemaining = job.progress.fps > 0
            ? (totalFrames - framesCompleted) / job.progress.fps : 0;
        job.progress.statusText = "Rendering frame " + std::to_string(framesCompleted) +
                                  "/" + std::to_string(totalFrames);

        if (m_progressCb) m_progressCb(job.id, job.progress);
    };

    // ── Step 4: Frame loop ──────────────────────────────────────────
    spdlog::info("RndQ[{}]: Step 4 — starting frame loop ({} frames)", job.id, totalFrames);
    std::vector<OwnedPacket> allPackets;
    bool cancelled = false;

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

    for (int64_t f = startFrame; f < endFrame; ++f) {
        // Check cancellation
        auto it = m_cancelFlags.find(job.id);
        if (m_cancelAll.load() || (it != m_cancelFlags.end() && it->second.load())) {
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

                        allPackets.push_back(makeOwnedPacket(rawPkt));
                        ptRunMedia    = pf.mediaPath;
                        ptExpectedSrc = pf.sourceFrame + 1;
                        updateProgress(f);
                        continue;
                    }
                    // Non-keyframe run start (clip in-point not GOP-aligned):
                    // fall through to re-encode so the leading frames survive.
                }
                // Fall through to normal render if read failed
            }
        }
        // Reaching here means this frame is composited + re-encoded, which
        // breaks any in-progress passthrough run.  The next passthrough frame
        // must re-validate from a keyframe before copying resumes.
        ptRunMedia.clear();
        ptExpectedSrc = -1;

        {
        // Render frame. Composited frames are BGRA; the encoder's swscale
        // converts BGRA→YUV420P directly (SIMD), so we pass the buffer
        // straight through — no per-frame CPU channel-swap + 4K alloc.
        // The holders below MUST outlive the encodeFrame() call.
        std::shared_ptr<CachedFrame> cframe;
        const uint8_t* encodePixels = nullptr;

        // Composite this frame via the main-thread callback (preview compositor;
        // produces a BGRA CachedFrame). Async pre-submit of the next frame's
        // tick lets the main thread composite f+1 while the worker encodes f.
        int64_t tick = static_cast<int64_t>(
            static_cast<double>(f) * 48000.0 * fpsDen / fpsNum);
        int64_t nextTick = -1;
        if (f + 1 < endFrame) {
            nextTick = static_cast<int64_t>(
                static_cast<double>(f + 1) * 48000.0 * fpsDen / fpsNum);
        }
        cframe = m_frameRenderCb(tick, nextTick, outW, outH, true);
        // The callback MUST have populated CPU pixels on the main thread before
        // returning (it does ensurePixels inside the BlockingQueuedConnection
        // dispatch). We do NOT call ensurePixels() here — it may trigger a GPU
        // readback (lazyReadback) which is NOT thread-safe.
        if (!cframe || cframe->pixels.empty()) {
            spdlog::warn("RndQ[{}]: null or empty frame at f={}, skipping", job.id, f);
            updateProgress(f);
            continue;
        }
        encodePixels = cframe->pixels.data();

        // Encode frame
        if (encoder->encodeFrame(encodePixels, f - startFrame)) {
            const auto& lp = encoder->lastPacket();
            allPackets.push_back(makeOwnedPacket(lp));
            // Collect any extra packets the encoder produced (B-frame drain)
            for (const auto& ep : encoder->pendingPackets())
                allPackets.push_back(makeOwnedPacket(ep));
        } else {
            spdlog::warn("RndQ[{}]: encodeFrame returned false at f={}", job.id, f);
            // Even when encodeFrame returns false, there may be pending
            // packets (encoder buffered frames but hadn't output them yet).
            for (const auto& ep : encoder->pendingPackets())
                allPackets.push_back(makeOwnedPacket(ep));
        }
        } // end normal render block

        updateProgress(f);
    }

    if (cancelled) {
        job.status = JobStatus::Cancelled;
        encoder->shutdown();
        if (m_completeCb) m_completeCb(job.id, false, "Cancelled");
        return;
    }

    // Flush encoder
    encoder->flush();
    for (const auto& ep : encoder->flushedPackets()) {
        allPackets.push_back(makeOwnedPacket(ep));
    }

    // Audio mixdown
    MixdownResult audioResult;
    if (job.config.includeAudio && timeline) {
        AudioMixdown mixdown;
        audioResult = mixdown.mix(*timeline, job.config.audioConfig);
    }

    // Mux video+audio into container file.
    // NOTE: the encoder is intentionally kept alive (NOT shut down) until
    // after muxing so the muxer can copy its extradata (SPS/PPS, hvcC,
    // AV1 seq header) into the container. Shutting it down here would free
    // the AVCodecContext and produce a file that only plays in VLC.
    MuxerConfig mCfg;
    mCfg.outputPath     = job.config.outputPath;
    mCfg.format         = static_cast<ContainerFormat>(job.config.containerFormat);
    mCfg.videoWidth     = job.config.outputWidth;
    mCfg.videoHeight    = job.config.outputHeight;
    mCfg.videoFpsNum    = job.config.encoderConfig.fpsNum;
    mCfg.videoFpsDen    = job.config.encoderConfig.fpsDen;
    mCfg.hasAudio       = audioResult.isValid();
    mCfg.audioSampleRate = job.config.audioConfig.sampleRate;
    mCfg.audioChannels   = static_cast<uint16_t>(job.config.audioConfig.channels);
    // Map encoder codec to AVCodecID for the container header.
    // Without this the muxer falls back to H264, which is wrong for H265/AV1.
    mCfg.videoCodecId = encoder ? encoder->avCodecId() : 0;
    // Hand the opened codec context to the muxer so it can copy
    // extradata + full codec params into the container's stream.
    mCfg.videoCodecContext = encoder ? encoder->avCodecContext() : nullptr;
    spdlog::info("RndQ[{}]: mux config fps={}/{} codecId={}", job.id,
                 mCfg.videoFpsNum, mCfg.videoFpsDen, mCfg.videoCodecId);

    job.progress.statusText = "Muxing...";
    if (m_progressCb) m_progressCb(job.id, job.progress);

    const MixdownResult* audioPtr = audioResult.isValid() ? &audioResult : nullptr;
    // Log packet diagnostics
    if (!allPackets.empty()) {
        spdlog::info("RndQ[{}]: total packets={} first_pts={} last_pts={} fps={}/{}",
                     job.id, allPackets.size(),
                     allPackets.front().pkt.pts, allPackets.back().pkt.pts,
                     mCfg.videoFpsNum, mCfg.videoFpsDen);
    }

    // Build a plain EncodedPacket vector for the muxer (data pointers are
    // valid because OwnedPacket storage stays alive until end of scope).
    // Packets are in encoder DTS order thanks to the drain loop in sendFrame.
    std::vector<EncodedPacket> muxPackets;
    muxPackets.reserve(allPackets.size());
    for (const auto& op : allPackets)
        muxPackets.push_back(op.pkt);
    bool muxOk = Muxer::muxFile(job.config.outputPath, muxPackets, audioPtr, mCfg);

    // Safe to release the encoder now that the muxer has read its params.
    encoder->shutdown();

    if (!muxOk) {
        spdlog::error("RenderQueue: Muxing failed for job {}", job.id);
        job.status = JobStatus::Failed;
        job.error  = "Muxing failed — output file not written";
        if (m_completeCb) m_completeCb(job.id, false, job.error);
        return;
    }

    job.status = JobStatus::Completed;
    job.progress.percent = 100.0f;
    job.progress.statusText = "Complete";

    auto endTime = std::chrono::steady_clock::now();
    double totalElapsed = std::chrono::duration<double>(endTime - startTime).count();
    spdlog::info("RenderQueue: Job {} complete ({:.1f}s, {:.0f} fps)",
                 job.id, totalElapsed, totalFrames / totalElapsed);

    if (m_completeCb) m_completeCb(job.id, true, "");
}

} // namespace rt
