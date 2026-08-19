/*
 * MediaPoolPrefetchConvertGpu.cpp — GPU-resident prefetch conversion.
 *
 * Implements MediaPool::convertDecodedToCacheGpu: the GPU-resident
 * sibling of convertDecodedToCache.  Routes NV12 / YUV420P → BGRA
 * through Nv12Converter into a per-frame pooled VkImage so the
 * compositor can sample without a CPU bounce.
 *
 * Lives in its own TU so the rest of core/media stays Vulkan-free.
 * Also defines WorkerGpuState's destructor (the only spot that needs
 * vkDestroySemaphore).
 *
 * Gated by CompositeService::gpuResidentDecodeEnabled() at the call
 * site (MediaPoolPrefetchDecode.cpp); this file does not check the
 * flag itself — callers that reach in are committing to the GPU path.
 */

#include "MediaPool.h"
#include "PathUtils.h"
#include "MediaPoolPrefetchGpu.h"
#include "cache/PrefetchTexturePool.h"
#include "WorkerBreadcrumb.h"

#include "CompositeService.h"     // feature flag (gpuResidentDecodeEnabled)
#include "GpuContext.h"
#include "GpuScheduler.h"
#include "Nv12Converter.h"
#include "cuda/CudaVulkanInterop.h"
#include "vulkan/Texture.h"
#include "vulkan/Device.h"
#include "vulkan/Allocator.h"

#include <spdlog/spdlog.h>

#include <volk.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <set>
#include <unordered_set>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

#ifdef ROUNDTABLE_HAS_FFMPEG
extern "C" {
#include <libavutil/pixfmt.h>
#include <libavutil/frame.h>       // AVFrame (CUDA hwframe data ptrs)
#include <libavutil/hwcontext.h>   // AVHWFramesContext::sw_format
}
#endif

namespace rt {

// ─────────────────────────────────────────────────────────────────────────
// WorkerGpuState constructor — defined out-of-line so the complete type
// of Nv12Converter is in scope when the implicit unique_ptr member
// destructor it triggers (during stack unwinding on a hypothetical
// constructor exception) is instantiated.
// ─────────────────────────────────────────────────────────────────────────
WorkerGpuState::WorkerGpuState() = default;

// ─────────────────────────────────────────────────────────────────────────
// WorkerGpuState destructor — needs vkDestroySemaphore, hence Vulkan TU.
// ─────────────────────────────────────────────────────────────────────────
WorkerGpuState::~WorkerGpuState()
{
    // Drain any in-flight submissions. The worker thread has exited by
    // the time this runs (MediaPool::stopPrefetchThread joins workers
    // before MediaPool dtor runs to completion). Normal shutdown waits for
    // each pending fence before freeing submission resources. A lost device
    // cannot resume work and may never signal those fences, so fatal shutdown
    // skips only the waits and continues handle cleanup.
    const bool deviceLost = GpuContext::get().gpuState() != GpuState::Healthy;
    if (device != VK_NULL_HANDLE) {
        for (auto& p : pending) {
            if (p.fence != VK_NULL_HANDLE) {
                if (!deviceLost)
                    vkWaitForFences(device, 1, &p.fence, VK_TRUE, UINT64_MAX);
                vkDestroyFence(device, p.fence, nullptr);
            }
            if (p.cmdBuf != VK_NULL_HANDLE) {
                cmdPool.freeBuffer(p.cmdBuf);
            }
            for (auto& s : p.staging) s.destroy();
            // Return the shared CUDA buffer to its interop pool, if
            // this submission used the zero-copy path.
            if (p.sharedAlloc && p.interop) {
                p.interop->free(std::move(p.sharedAlloc));
            }
        }
    }
    pending.clear();

    // Per-worker Nv12Converter — destroyed before signalSem / cmdPool so
    // its own internal Vulkan objects release while the device is still
    // alive.  Must happen before the unique_ptr would otherwise unwind
    // (declaration order would still put it last, but explicit reset
    // documents intent and orders cleanup deterministically).
    if (nv12Converter) {
        nv12Converter->shutdown();
        nv12Converter.reset();
    }

    if (signalSem != VK_NULL_HANDLE && device != VK_NULL_HANDLE) {
        vkDestroySemaphore(device, signalSem, nullptr);
    }

    // Chroma-key pass resources
    const VkDevice d = ckDevice ? ckDevice : device;
    if (chromaKeyPipeline != VK_NULL_HANDLE)
        vkDestroyPipeline(d, chromaKeyPipeline, nullptr);
    if (chromaKeyLayout != VK_NULL_HANDLE)
        vkDestroyPipelineLayout(d, chromaKeyLayout, nullptr);
    if (chromaKeyShader != VK_NULL_HANDLE)
        vkDestroyShaderModule(d, chromaKeyShader, nullptr);
    if (chromaKeyDescPool != VK_NULL_HANDLE)
        vkDestroyDescriptorPool(d, chromaKeyDescPool, nullptr);
    if (chromaKeyDescLayout != VK_NULL_HANDLE)
        vkDestroyDescriptorSetLayout(d, chromaKeyDescLayout, nullptr);

    // cmdPool: RAII via rt::CommandPool destructor.
}

// ─────────────────────────────────────────────────────────────────────────
// WorkerGpuState::pollAndCleanup — non-blocking sweep of completed work.
// ─────────────────────────────────────────────────────────────────────────
void WorkerGpuState::pollAndCleanup()
{
    if (device == VK_NULL_HANDLE) return;
    while (!pending.empty()) {
        auto& front = pending.front();
        if (front.fence == VK_NULL_HANDLE) {
            // Defensive: malformed entry, drop it.
            pending.pop_front();
            continue;
        }
        const VkResult r = vkGetFenceStatus(device, front.fence);
        if (r != VK_SUCCESS) {
            // VK_NOT_READY: fence still pending — stop here.  Later
            // entries were submitted after this one and on the same
            // queue, so their fences cannot have signalled yet.
            // Any other code (e.g. VK_ERROR_DEVICE_LOST): leave the
            // entry for the destructor to drain; this avoids freeing
            // a cmd buffer the driver thinks is still in use.
            break;
        }
        vkDestroyFence(device, front.fence, nullptr);
        if (front.cmdBuf != VK_NULL_HANDLE) {
            cmdPool.freeBuffer(front.cmdBuf);
        }
        for (auto& s : front.staging) s.destroy();
        // Return the shared CUDA buffer to its interop pool now that
        // the GPU is done reading from it.
        if (front.sharedAlloc && front.interop) {
            front.interop->free(std::move(front.sharedAlloc));
        }
        pending.pop_front();
    }
}

// ─────────────────────────────────────────────────────────────────────────
// WorkerGpuState::ensureNv12Converter — lazy-create the per-worker
// converter on first GPU-eligible frame.  The converter owns its own
// input/output textures and descriptor sets, so multiple workers can
// pipeline submissions on the compute queue without contending on a
// shared apiMutex.
// ─────────────────────────────────────────────────────────────────────────
Nv12Converter* WorkerGpuState::ensureNv12Converter(uint32_t w, uint32_t h)
{
    if (nv12Converter && nv12Converter->isInitialized()) {
        // Already constructed.  Internal ensureOutputSize() inside
        // recordConvertScaled / recordConvertFromBufferScaled will
        // resize the output texture if (w, h) changed.
        nv12ConverterW = w;
        nv12ConverterH = h;
        return nv12Converter.get();
    }

    auto& ctx = GpuContext::get();
    if (!ctx.isInitialized()) return nullptr;
    if (cmdPool.handle() == VK_NULL_HANDLE) return nullptr;

    nv12Converter = std::make_unique<Nv12Converter>();
    Nv12ConverterConfig cfg;
    cfg.width  = w;
    cfg.height = h;
    if (!nv12Converter->init(ctx.device(), ctx.allocator(),
                              cmdPool, ctx.computeQueue(), cfg)) {
        spdlog::warn("WorkerGpuState::ensureNv12Converter: init failed for "
                     "{}x{} — falling back to CPU path", w, h);
        nv12Converter.reset();
        return nullptr;
    }
    nv12ConverterW = w;
    nv12ConverterH = h;
    spdlog::debug("WorkerGpuState: per-worker Nv12Converter created {}x{}",
                 w, h);
    return nv12Converter.get();
}

// ─────────────────────────────────────────────────────────────────────────
// Phase-boundary breadcrumb used by the crash handler.  Stored per-thread
// (rt::setLastWorkerStep — see WorkerBreadcrumb.h) so the SEH handler,
// which runs on the faulting thread, reads THIS thread's last step —
// not whatever a different thread happened to write most recently.
// Anchors the otherwise frame-pointer-only crash stacks observed at
// roundtable.exe+0x3DE56B / +0x3DD70B / +0x3DC3FB to a human-readable
// step name without needing PDB symbols.  The macro narrows the
// call-site boilerplate.
// ─────────────────────────────────────────────────────────────────────────
#define gWorkerStep ::rt::setLastWorkerStep

// ── Chroma-key helpers (file-local, called from convertDecodedToCacheGpu)
namespace {

fs::path findShaderFile(const char* name)
{
    fs::path candidates[] = {
        fs::path(__FILE__).parent_path().parent_path().parent_path() / "build" / "shaders" / name,
        fs::current_path() / "shaders" / name,
        fs::current_path().parent_path() / "shaders" / name,
        fs::current_path().parent_path() / "build" / "shaders" / name,
    };
    for (auto& p : candidates)
        if (fs::exists(p)) return p;
    return {};
}

void updateChromaKeyDescriptors(
    VkDevice dev, VkDescriptorSet ds,
    const Texture& srcTex, Texture& dstTex);

void recordChromaKeyPass(
    VkCommandBuffer cmd, WorkerGpuState& wgs,
    const Texture& srcTex, Texture& dstTex,
    uint32_t width, uint32_t height);

} // namespace

// ─────────────────────────────────────────────────────────────────────────
// MediaPool::convertDecodedToCacheGpu
// ─────────────────────────────────────────────────────────────────────────
std::shared_ptr<CachedFrame> MediaPool::convertDecodedToCacheGpu(
    PrefetchDecoderState& state,
    const PrefetchTask&   task,
    DecodedFrame&         decoded,
    int64_t               frameNumber,
    WorkerGpuState&       wgs)
{
    gWorkerStep("convertDecodedToCacheGpu/entry");

    // ── Eligibility (UPGRADE_PLAN H.2, plus L.1 device-lost check) ──────
    auto& ctx = GpuContext::get();
    if (!ctx.isInitialized() || !ctx.isOperational()) return nullptr;
    if (!wgs.ready())                                  return nullptr;
    if (!m_prefetchTexPool)                            return nullptr;
    if (prefetchTimelineSem() == 0)                    return nullptr;

    // ── Colourspace gate (Phase 4.1) ────────────────────────────────────
    // The convert shaders (nv12/yuv420p/p010/yuva444p12) hardcode the BT.709
    // matrix + studio-swing range + SDR transfer.  Sources tagged otherwise —
    // BT.601 SD, full-range JPEG, BT.2020/HDR — must take the CPU sws_scale
    // path, which honours the source's actual matrix/range (see
    // ConvertDecodedFrame.cpp).  Bailing here also keeps a clip's frames from
    // mixing a GPU-709 frame with a CPU-601 frame in the same cache, which
    // would flicker.  resolveColorConversion is the SAME decision the CPU
    // path makes, so the GPU/CPU split is clean per-clip.
    if (!resolveColorConversion(task.info).isGpuShaderDefault)
        return nullptr;   // → caller falls back to convertDecodedToCache (CPU)

    // Packed-alpha sources (Wells and other video characters) used to bail
    // here, which forced them onto the CPU bounce path (transferHardwareFrame
    // + sws_scale + per-frame PCIe re-upload).  That's the dominant cost on
    // packed-alpha character clips.  The GPU path produces the same BGRA
    // layout sws_scale would have produced — full 2× height, top RGB,
    // bottom alpha-as-greyscale — and the compositor's existing UV-split
    // shader is keyed on the cudaPacked flag the GpuTexCache stores at
    // putShared time, so the downstream behaviour is identical to the
    // CPU path (see CompositeServiceLayerBuild.cpp around line 1306).
    // Keeping the unpackedAlpha=false default on the new CachedFrame so
    // the GpuTexCache lookup correctly reports isPacked=true to the
    // compositor.

    // Non-blocking sweep of completed prior submissions.  Frees their
    // fence + staging + cmd buffer so we don't accumulate them.  Each
    // entry walks at most once across two prefetch calls.
    wgs.pollAndCleanup();

    // ── Backpressure: cap in-flight submissions per worker ─────────────
    //
    // Without this, the deferred-cleanup design lets the worker submit
    // as fast as it can record (~150 fps of NVDEC + convert+copy work),
    // which saturates the SHARED compute queue and starves the
    // compositor's own submit — causing the compositor's frame
    // callback to take 100ms per frame instead of 33ms, dropping the
    // FrameClock thread from 30fps to ~10fps.  The user sees this as
    // sustained video stutter that appears at the 30-60s mark, right
    // when cache pressure ramps prefetch throughput up to its max.
    //
    // Cap = 1 (2026-05-22 tighten): one frame in flight per worker.
    //
    // Originally 3 to maximize prefetch throughput.  Reduced to 1 after
    // observing 200-285 ms compositor `submit=` stalls in
    // perf_log.txt at 2026-05-22 12:38:58+: even though `ZC=100%` and
    // gpuDisplay=true, every compositor submit queued behind 6 in-flight
    // prefetch convert+copy submissions (2 NVDEC workers × cap 3).
    // Some devices expose distinct logical graphics/compute queues backed by
    // the same execution hardware, so deep prefetch bursts can still delay
    // interactive work. Cap=1 is the measured conservative limit: at most one
    // submission per worker owns staging/CUDA resources while preserving more
    // than enough aggregate cache-fill throughput for playback.
    gWorkerStep("convertDecodedToCacheGpu/backpressure-wait");
    constexpr size_t kMaxPendingPerWorker = 1;
    while (wgs.pending.size() >= kMaxPendingPerWorker) {
        auto& oldest = wgs.pending.front();
        if (oldest.fence == VK_NULL_HANDLE) {
            wgs.pending.pop_front();
            continue;
        }
        constexpr uint64_t kBackpressureWaitNs = 1'500'000'000ull;
        const VkResult waitResult = vkWaitForFences(
            ctx.vkDevice(), 1, &oldest.fence, VK_TRUE, kBackpressureWaitNs);
        if (waitResult != VK_SUCCESS) {
            spdlog::warn("GPU prefetch backpressure wait failed/timed out (vk={}); "
                         "using CPU conversion for this frame",
                         static_cast<int>(waitResult));
            if (waitResult == VK_ERROR_DEVICE_LOST)
                ctx.signalDeviceLost();
            return nullptr;
        }
        vkDestroyFence(ctx.vkDevice(), oldest.fence, nullptr);
        if (oldest.cmdBuf != VK_NULL_HANDLE) {
            wgs.cmdPool.freeBuffer(oldest.cmdBuf);
        }
        for (auto& s : oldest.staging) s.destroy();
        if (oldest.sharedAlloc && oldest.interop) {
            oldest.interop->free(std::move(oldest.sharedAlloc));
        }
        wgs.pending.pop_front();
    }

    // ── UPGRADE_PLAN A: zero-copy preflight ─────────────────────────────
    // If we have an NVDEC hardware frame (data lives in CUDA-owned GPU
    // memory) AND the CUDA↔Vulkan interop is up, GPU-copy the NV12
    // planes into a shared Vulkan buffer instead of CPU-bouncing them
    // via transferHardwareFrame.  copyNv12FromCuda is synchronous on
    // the CUDA side and signals an external timeline semaphore that the
    // compute submit waits on (pNext = VkTimelineSemaphoreSubmitInfo).
    //
    // On any failure the path falls through to the legacy
    // transferHardwareFrame + recordConvertScaled flow without
    // affecting the original logic — including the case where CUDA
    // isn't compiled in, where cudaVulkanInterop() returns nullptr.
    std::unique_ptr<SharedAllocation> zeroCopyAlloc;
    CudaVulkanInterop* interop = nullptr;
    const uint32_t hwW = decoded.width;
    const uint32_t hwH = decoded.height;

    // Classify the hardware frame so the zero-copy preflight
    // picks the right copy routine.  NVDEC HEVC 10-bit / AV1 10-bit
    // outputs sw_format=P010LE; HEVC 12-bit can output P016LE.  Both
    // share the NV12 plane layout (Y then interleaved UV at 4:2:0) but
    // every sample is 2 bytes, so the wrong path produces garbage.
    bool hwIsNv12 = decoded.isHardware;
    bool hwIsP010 = false;
#ifdef ROUNDTABLE_HAS_FFMPEG
    if (decoded.isHardware && decoded.avFrame && decoded.avFrame->hw_frames_ctx) {
        auto* hwfc = reinterpret_cast<AVHWFramesContext*>(
            decoded.avFrame->hw_frames_ctx->data);
        if (hwfc) {
            hwIsNv12 = (hwfc->sw_format == AV_PIX_FMT_NV12);
            hwIsP010 = (hwfc->sw_format == AV_PIX_FMT_P010LE)
                    || (hwfc->sw_format == AV_PIX_FMT_P016LE);
        }
    }
#endif
    // Tracks which buffer layout zeroCopyAlloc holds, when ZC succeeds.
    // Needed below so the dispatch picks recordConvertFromBufferScaled vs
    // recordConvertP010FromBufferScaled.
    bool zcIsP010 = false;

    // One-shot per-(handle, reason) FAILURE diagnostic.  Logs at warn
    // level so it survives the warn+ filter.  Tracks failures only (not
    // successes), so a handle that initially succeeds at ZC and later
    // starts failing will still log the first failure — fixing the
    // "first-success suppresses subsequent failure logging" bug that
    // made the 2026-05-22 13:28 log silent about ZC dropping from
    // 100% to 0%.
    //
    // Key failures on (handle, reason). The previous
    // handle-only key meant the FIRST failure mode for a handle silenced
    // every other reason on the same handle — so a clip whose NVDEC went
    // soft (isHardware=false) early would never log a later
    // copyNv12FromCuda failure on a different handle reusing the same
    // ID.  Distinct reasons now log once each.
    enum class ZcFailReason : uint8_t {
        NotHardware,
        NullAvFrame,
        ZeroDims,
        OddDims,
        NoInterop,
        InteropUnavailable,
        NullPlanes,
        AllocateFailed,
        CopyFailed,
    };
    auto shouldLogZcFail = [task](ZcFailReason reason) -> bool {
        static thread_local std::set<std::pair<MediaHandle, uint8_t>> s_failed;
        return s_failed.insert({task.handle, static_cast<uint8_t>(reason)}).second;
    };

    if (!decoded.isHardware) {
        if (shouldLogZcFail(ZcFailReason::NotHardware))
            spdlog::debug("[ZC-DIAG] handle={} skipped: decoded.isHardware=false "
                         "(software decode, NVDEC not engaged for this file)",
                         task.handle);
    } else if (decoded.avFrame == nullptr) {
        if (shouldLogZcFail(ZcFailReason::NullAvFrame))
            spdlog::debug("[ZC-DIAG] handle={} skipped: decoded.avFrame=null",
                         task.handle);
    } else if (hwW == 0 || hwH == 0) {
        if (shouldLogZcFail(ZcFailReason::ZeroDims))
            spdlog::debug("[ZC-DIAG] handle={} skipped: zero dimensions",
                         task.handle);
    } else if ((hwW & 1) != 0 || (hwH & 1) != 0) {
        if (shouldLogZcFail(ZcFailReason::OddDims))
            spdlog::debug("[ZC-DIAG] handle={} skipped: odd dimensions W={} H={}",
                         task.handle, hwW, hwH);
    }

    gWorkerStep("convertDecodedToCacheGpu/zc-preflight");
    if (decoded.isHardware
        && (hwIsNv12 || hwIsP010)
        && decoded.avFrame != nullptr
        && hwW > 0 && hwH > 0
        && (hwW & 1) == 0 && (hwH & 1) == 0)   // 4:2:0 needs even dims
    {
        interop = ctx.cudaVulkanInterop();
        if (!interop) {
            if (shouldLogZcFail(ZcFailReason::NoInterop))
                spdlog::debug("[ZC-DIAG] handle={} skipped: ctx.cudaVulkanInterop=null",
                             task.handle);
        } else if (!interop->isAvailable()) {
            if (shouldLogZcFail(ZcFailReason::InteropUnavailable))
                spdlog::debug("[ZC-DIAG] handle={} skipped: interop->isAvailable=false",
                             task.handle);
        }
        if (interop && interop->isAvailable()) {
#ifdef ROUNDTABLE_HAS_FFMPEG
            // CUDA hwframes from NVDEC store Y at data[0], UV at data[1]
            // as CUdeviceptr values (raw cast to uint8_t* by ffmpeg).
            const void* yPtr  = static_cast<const void*>(decoded.avFrame->data[0]);
            const void* uvPtr = static_cast<const void*>(decoded.avFrame->data[1]);
            const int yPitch  = decoded.avFrame->linesize[0];
            const int uvPitch = decoded.avFrame->linesize[1];
            if (!yPtr || !uvPtr || yPitch <= 0 || uvPitch <= 0) {
                if (shouldLogZcFail(ZcFailReason::NullPlanes))
                    spdlog::debug("[ZC-DIAG] handle={} skipped: null plane ptrs/pitch "
                                 "(yPtr={} uvPtr={} yPitch={} uvPitch={})",
                                 task.handle, yPtr, uvPtr, yPitch, uvPitch);
            } else {
                auto alloc = interop->allocate(hwW, hwH, /*tenBit=*/hwIsP010);
                if (!alloc) {
                    if (shouldLogZcFail(ZcFailReason::AllocateFailed))
                        spdlog::debug("[ZC-DIAG] handle={} skipped: interop->allocate "
                                     "returned null (vkAllocateMemory exhaustion?)",
                                     task.handle);
                } else {
                    // Pick the matching copy routine based on hwframe bit depth.
                    const bool copyOk = hwIsP010
                        ? interop->copyP010FromCuda(*alloc,
                              yPtr, yPitch, uvPtr, uvPitch, hwW, hwH)
                        : interop->copyNv12FromCuda(*alloc,
                              yPtr, yPitch, uvPtr, uvPitch, hwW, hwH);
                    if (!copyOk) {
                        if (shouldLogZcFail(ZcFailReason::CopyFailed))
                            spdlog::debug("[ZC-DIAG] handle={} skipped: "
                                         "{} copy failed",
                                         task.handle,
                                         hwIsP010 ? "P010" : "NV12");
                        interop->free(std::move(alloc));
                    } else {
                        zeroCopyAlloc = std::move(alloc);
                        zcIsP010 = hwIsP010;
                    }
                }
            }
#endif
        }
    }

    // Hardware → CPU transfer (fallback when zero-copy did not fire).
    // Same step the CPU path takes (NVDEC surfaces aren't directly
    // accessible to Nv12Converter without the interop bridge above).
    gWorkerStep("convertDecodedToCacheGpu/hw-transfer");
    if (!zeroCopyAlloc && decoded.isHardware) {
        DecodedFrame cpuFrame;
        if (!state.decoder->transferHardwareFrame(decoded, cpuFrame))
            return nullptr;
        decoded = std::move(cpuFrame);
    }
    if (!zeroCopyAlloc &&
        (!decoded.data[0] || decoded.width == 0 || decoded.height == 0))
        return nullptr;

#ifdef ROUNDTABLE_HAS_FFMPEG
    // Zero-copy always produces NV12 in the shared buffer (per
    // copyNv12FromCuda's contract).  The legacy fallback paths read
    // decoded.rawFormat / decoded.format as before.
    AVPixelFormat srcFmt = AV_PIX_FMT_NV12;
    if (!zeroCopyAlloc) {
        srcFmt = AV_PIX_FMT_NONE;
        if (decoded.rawFormat >= 0) {
            srcFmt = static_cast<AVPixelFormat>(decoded.rawFormat);
        } else {
            switch (decoded.format) {
                case PixelFormat::YUV420P: srcFmt = AV_PIX_FMT_YUV420P; break;
                case PixelFormat::NV12:    srcFmt = AV_PIX_FMT_NV12;    break;
                default:                   return nullptr;
            }
        }
        // Accept P010 / P016 (10/16-bit NV12) on top
        // of the original NV12 + YUV420P GPU paths.  Bail out for anything
        // else and let the CPU sws_scale path handle it.
        const bool acceptedFmt =
            (srcFmt == AV_PIX_FMT_NV12)    ||
            (srcFmt == AV_PIX_FMT_YUV420P) ||
            (srcFmt == AV_PIX_FMT_P010LE)  ||
            (srcFmt == AV_PIX_FMT_P016LE)  ||
            (srcFmt == AV_PIX_FMT_YUVA444P12LE);  // ProRes 4444 (4:4:4 + alpha)
        if (!acceptedFmt)
            return nullptr;
        // ProRes 4444 needs all four planes (Y, U, V, A).
        if (srcFmt == AV_PIX_FMT_YUVA444P12LE &&
            (!decoded.data[0] || !decoded.data[1] ||
             !decoded.data[2] || !decoded.data[3]))
            return nullptr;
    }
#else
    return nullptr;
#endif

    // For zero-copy, decoded is still the (hardware) original frame and
    // its width/height come from NVDEC.  For the CPU path, decoded has
    // been transferred and width/height likewise reflect the source.
    const int srcW = static_cast<int>(zeroCopyAlloc ? hwW : decoded.width);
    const int srcH = static_cast<int>(zeroCopyAlloc ? hwH : decoded.height);

    // Match the CPU conversion path so cached CPU/GPU frames never alternate
    // dimensions. Full is native; reduced tiers divide both source axes.
    const ResolutionTier decodeTier = task.exportFullRes
        ? ResolutionTier::Full : task.tier;
    const auto tierSize = resolutionTierDimensions(srcW, srcH, decodeTier);
    const int dstW = tierSize.width;
    const int dstH = tierSize.height;
    if (dstW > 16384 || dstH > 16384) return nullptr;

    // ── Acquire this worker's OWN Nv12Converter sized for dst.
    //    Per-worker instance — no shared apiMutex,
    //    no inline wait, no cross-worker serialisation.  Multiple
    //    workers pipeline freely on the compute queue. ──────────────────
    gWorkerStep("convertDecodedToCacheGpu/ensure-converter");
    Nv12Converter* conv = wgs.ensureNv12Converter(
        static_cast<uint32_t>(dstW), static_cast<uint32_t>(dstH));
    if (!conv || !conv->isInitialized()) return nullptr;

    // ── Acquire a pooled destination texture (BGRA, concurrent sharing
    //    so compute writes + graphics reads need no queue-ownership
    //    transfer barrier).  See PrefetchTexturePool.h. ─────────────────
    const auto& qf = ctx.device().queueFamilies();
    const uint32_t computeFamily  = qf.compute.value_or(qf.graphics.value_or(0));
    const uint32_t graphicsFamily = qf.graphics.value_or(0);
    std::vector<uint32_t> concurrent;
    if (computeFamily != graphicsFamily) {
        concurrent.push_back(computeFamily);
        concurrent.push_back(graphicsFamily);
    }
    constexpr VkImageUsageFlags kDstUsage =
        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT
      | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;  // TRANSFER_SRC for lazy readback
    gWorkerStep("convertDecodedToCacheGpu/pooled-dst-texture");
    auto dstTex = makePooledTexture(*m_prefetchTexPool, ctx,
                                    static_cast<uint32_t>(dstW),
                                    static_cast<uint32_t>(dstH),
                                    VK_FORMAT_B8G8R8A8_UNORM,
                                    kDstUsage,
                                    std::move(concurrent));
    if (!dstTex) return nullptr;

    // ── Per-worker converter means no shared mutable state across
    //    workers — no apiMutex lock needed, no inline fence wait
    //    needed.  Each worker's converter is touched only by its own
    //    thread, so descriptor updates + texture uploads can pipeline
    //    freely on the compute queue.
    //
    // ── Open per-worker command buffer.  beginSingleTime allocates + ────
    //    begins with ONE_TIME_SUBMIT flag set.  ─────────────────────────
    VkCommandBuffer cmd = wgs.cmdPool.beginSingleTime();
    if (cmd == VK_NULL_HANDLE) return nullptr;

    std::vector<Texture::StagingCleanup> stagingOut;

    // ── Record convert into cmd.  Five paths:                       ──
    //    - zero-copy NV12: read 8-bit NV12 from the shared VkBuffer the
    //                  interop populated (no CPU staging, no per-plane upload).
    //    - zero-copy P010: read 16-bit P010 from the shared VkBuffer
    //                  (HEVC 10-bit / AV1 10-bit zero-copy).
    //                  UV plane lives at offset W*H*2 (twice NV12's offset
    //                  because each sample is 2 bytes).
    //    - NV12 CPU:  upload 8-bit Y/UV planes from CPU, run NV12 shader.
    //    - P010 CPU:  upload 16-bit Y/UV planes from CPU, run P010 shader.
    //    - YUV420P:   upload Y/U/V planes from CPU, run YUV420P shader.
    bool recordOk = false;
    if (zeroCopyAlloc) {
        if (zcIsP010) {
            gWorkerStep("convertDecodedToCacheGpu/record-zc-p010");
            const VkDeviceSize uvOffset = static_cast<VkDeviceSize>(srcW) * srcH * 2;
            recordOk = conv->recordConvertP010FromBufferScaled(
                cmd,
                reinterpret_cast<VkBuffer>(zeroCopyAlloc->vulkanBuffer),
                static_cast<uint32_t>(srcW), static_cast<uint32_t>(srcH),
                static_cast<uint32_t>(dstW), static_cast<uint32_t>(dstH),
                /*yOffset=*/  0,
                /*uvOffset=*/ uvOffset);
        } else {
            gWorkerStep("convertDecodedToCacheGpu/record-zc-nv12");
            const VkDeviceSize uvOffset = static_cast<VkDeviceSize>(srcW) * srcH;
            recordOk = conv->recordConvertFromBufferScaled(
                cmd,
                reinterpret_cast<VkBuffer>(zeroCopyAlloc->vulkanBuffer),
                static_cast<uint32_t>(srcW), static_cast<uint32_t>(srcH),
                static_cast<uint32_t>(dstW), static_cast<uint32_t>(dstH),
                /*yOffset=*/  0,
                /*uvOffset=*/ uvOffset);
        }
    } else if (srcFmt == AV_PIX_FMT_NV12) {
        gWorkerStep("convertDecodedToCacheGpu/record-cpu-nv12");
        recordOk = conv->recordConvertScaled(
            cmd,
            decoded.data[0], decoded.linesize[0],
            decoded.data[1], decoded.linesize[1],
            static_cast<uint32_t>(srcW), static_cast<uint32_t>(srcH),
            static_cast<uint32_t>(dstW), static_cast<uint32_t>(dstH),
            stagingOut);
    } else if (srcFmt == AV_PIX_FMT_P010LE || srcFmt == AV_PIX_FMT_P016LE) {
        gWorkerStep("convertDecodedToCacheGpu/record-cpu-p010");
        recordOk = conv->recordConvertP010Scaled(
            cmd,
            decoded.data[0], decoded.linesize[0],
            decoded.data[1], decoded.linesize[1],
            static_cast<uint32_t>(srcW), static_cast<uint32_t>(srcH),
            static_cast<uint32_t>(dstW), static_cast<uint32_t>(dstH),
            stagingOut);
    } else if (srcFmt == AV_PIX_FMT_YUVA444P12LE) {
        // ProRes 4444 — per-worker converter + per-worker cmd buffer, so the
        // 4-plane upload + convert is thread-safe (unlike the shared-pool path
        // that crashed).  Real alpha, full 4:4:4 chroma, GPU-resident output.
        gWorkerStep("convertDecodedToCacheGpu/record-cpu-yuva444p12");
        recordOk = conv->recordConvertYuva444p12Scaled(
            cmd,
            decoded.data[0], decoded.linesize[0],
            decoded.data[1], decoded.linesize[1],
            decoded.data[2], decoded.linesize[2],
            decoded.data[3], decoded.linesize[3],
            static_cast<uint32_t>(srcW), static_cast<uint32_t>(srcH),
            static_cast<uint32_t>(dstW), static_cast<uint32_t>(dstH),
            stagingOut);
    } else {
        gWorkerStep("convertDecodedToCacheGpu/record-cpu-yuv420p");
        recordOk = conv->recordConvertYuv420pScaled(
            cmd,
            decoded.data[0], decoded.linesize[0],
            decoded.data[1], decoded.linesize[1],
            decoded.data[2], decoded.linesize[2],
            static_cast<uint32_t>(srcW), static_cast<uint32_t>(srcH),
            static_cast<uint32_t>(dstW), static_cast<uint32_t>(dstH),
            stagingOut);
    }
    if (!recordOk) {
        vkEndCommandBuffer(cmd);
        wgs.cmdPool.freeBuffer(cmd);
        for (auto& s : stagingOut) s.destroy();
        if (zeroCopyAlloc && interop)
            interop->free(std::move(zeroCopyAlloc));
        return nullptr;
    }

    // ── Transition dstTex UNDEFINED → TRANSFER_DST_OPTIMAL.
    //    oldLayout=UNDEFINED discards the previous contents — correct
    //    for both freshly created textures and recycled ones (we're
    //    about to overwrite the entire image via vkCmdCopyImage). ───────
    gWorkerStep("convertDecodedToCacheGpu/image-copy");
    {
        VkImageMemoryBarrier b{};
        b.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.srcAccessMask       = 0;
        b.dstAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image               = dstTex->image();
        b.subresourceRange    = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &b);
    }

    // ── vkCmdCopyImage: Nv12Converter output (GENERAL) → dstTex
    //    (TRANSFER_DST_OPTIMAL).  Sizes match because Nv12Converter was
    //    sized for dstW×dstH and the converter renders at that size. ────
    {
        VkImageCopy region{};
        region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.extent         = {static_cast<uint32_t>(dstW),
                                 static_cast<uint32_t>(dstH), 1};
        vkCmdCopyImage(cmd,
            conv->outputTexture().image(), VK_IMAGE_LAYOUT_GENERAL,
            dstTex->image(),                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            1, &region);
    }

    // ── Transition dstTex TRANSFER_DST_OPTIMAL → SHADER_READ_ONLY_OPTIMAL.
    //    dstStageMask must be valid on the COMPUTE queue family this cmd
    //    buffer belongs to — FRAGMENT_SHADER_BIT is graphics-only and
    //    fails VUID-vkCmdPipelineBarrier-dstStageMask-06462 on dedicated
    //    compute queues.  BOTTOM_OF_PIPE_BIT + dstAccessMask=0 says
    //    "complete the layout transition by the end of this submission;
    //    later queues handle their own memory visibility on first use."
    //    The sampling queue (compositor) issues its own implicit
    //    visibility when binding the descriptor.
    {
        VkImageMemoryBarrier b{};
        b.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.srcAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.dstAccessMask       = 0;
        b.oldLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.newLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image               = dstTex->image();
        b.subresourceRange    = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
            0, 0, nullptr, 0, nullptr, 1, &b);
    }
    // Note: Texture::m_currentLayout is now out-of-sync with reality
    // (UNDEFINED instead of SHADER_READ_ONLY_OPTIMAL).  Acceptable
    // because the only consumers — compositor sampling via the saved
    // gpuImageView and GpuContext::readbackTexture (which hardcodes
    // SHADER_READ_ONLY_OPTIMAL as oldLayout) — do not use the tracker.

    gWorkerStep("convertDecodedToCacheGpu/end-cmd-buffer");
    if (vkEndCommandBuffer(cmd) != VK_SUCCESS) {
        wgs.cmdPool.freeBuffer(cmd);
        for (auto& s : stagingOut) s.destroy();
        if (zeroCopyAlloc && interop)
            interop->free(std::move(zeroCopyAlloc));
        return nullptr;
    }

    gWorkerStep("convertDecodedToCacheGpu/submit");

    // ── Per-call fence for deferred CPU-side cleanup of staging
    //    buffers + cmd buffer. NOT used for compositor ordering: the
    //    compositor submits on the graphics queue and waits on the producer
    //    timeline semaphore instead. See WorkerGpuState::pending in
    //    MediaPoolPrefetchGpu.h.
    gWorkerStep("convertDecodedToCacheGpu/submit/create-fence");
    VkFenceCreateInfo fci{};
    fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    VkFence fence = VK_NULL_HANDLE;
    if (vkCreateFence(ctx.vkDevice(), &fci, nullptr, &fence) != VK_SUCCESS) {
        wgs.cmdPool.freeBuffer(cmd);
        for (auto& s : stagingOut) s.destroy();
        if (zeroCopyAlloc && interop)
            interop->free(std::move(zeroCopyAlloc));
        return nullptr;
    }

    // ── Build VkTimelineSemaphoreSubmitInfo for both wait (ZC) and signal
    //    (cross-queue producer→compositor sync).  One struct carries
    //    both arrays; we always signal the prefetch timeline sem at the
    //    next monotonic value, and ALSO wait on the interop's timeline
    //    semaphore when this submission consumed CUDA writes (zero-copy
    //    path).  Both signal and wait counts must match the matching
    //    arrays in VkSubmitInfo, hence the parallel sub.signalSemaphores
    //    and sub.waitSemaphores arrays below.
    VkTimelineSemaphoreSubmitInfo tlInfo{};
    tlInfo.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;

    gWorkerStep("convertDecodedToCacheGpu/submit/wait-semaphore");
    VkSemaphore           waitSem   = VK_NULL_HANDLE;
    VkPipelineStageFlags  waitStage = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    uint64_t              waitValue = 0;
    if (zeroCopyAlloc && interop) {
        waitSem   = reinterpret_cast<VkSemaphore>(interop->vkSemaphore());
        waitValue = interop->lastSignalValue();
        tlInfo.waitSemaphoreValueCount = 1;
        tlInfo.pWaitSemaphoreValues    = &waitValue;
    }

    // Signal the shared prefetch timeline semaphore at a monotonically
    // increasing value.
    // The compositor's submit on the graphics queue will wait on this
    // (sem, value) pair before sampling textures whose CachedFrame
    // carries this value — making the cross-queue memory-visibility
    // wait GPU-side, off the prefetch worker thread, replacing the
    // previous synchronous vkWaitForFences that cost ~5 ms / frame.
    gWorkerStep("convertDecodedToCacheGpu/submit/signal-semaphore");
    VkSemaphore  prefetchSignalSem   = VK_NULL_HANDLE;
    uint64_t     prefetchSignalValue = 0;
    {
        const uint64_t semHandle = prefetchTimelineSem();
        if (semHandle != 0) {
            prefetchSignalSem   = reinterpret_cast<VkSemaphore>(semHandle);
            prefetchSignalValue = nextPrefetchTimelineValue();
            tlInfo.signalSemaphoreValueCount = 1;
            tlInfo.pSignalSemaphoreValues    = &prefetchSignalValue;
        }
    }

    GpuSubmission sub{};
    sub.cmd             = cmd;
    sub.queue           = GpuQueueKind::Compute;
    sub.completionFence = fence;
    sub.tag             = zeroCopyAlloc ? "prefetch-zerocopy"
                                        : "prefetch-decode-convert";

    // Attach pNext only when at least one timeline operation is present.
    // VkTimelineSemaphoreSubmitInfo with both counts at zero would still
    // be valid but it's cleaner to skip the chain in the trivial case.
    const bool needTimelineInfo =
        (zeroCopyAlloc && waitSem != VK_NULL_HANDLE) ||
        (prefetchSignalSem != VK_NULL_HANDLE);
    if (needTimelineInfo) {
        sub.pNext = &tlInfo;
    }
    if (zeroCopyAlloc && waitSem != VK_NULL_HANDLE) {
        sub.waitSemaphores     = &waitSem;
        sub.waitStages         = &waitStage;
        sub.waitSemaphoreCount = 1;
    }
    if (prefetchSignalSem != VK_NULL_HANDLE) {
        sub.signalSemaphores     = &prefetchSignalSem;
        sub.signalSemaphoreCount = 1;
    }

    gWorkerStep("convertDecodedToCacheGpu/submit/queue-submit");
    const VkResult sr = ctx.scheduler().submit(sub);
    if (sr != VK_SUCCESS) {
        spdlog::warn("convertDecodedToCacheGpu: submit failed vk={} handle={} frame={}",
                     static_cast<int>(sr), task.handle, frameNumber);
        vkDestroyFence(ctx.vkDevice(), fence, nullptr);
        wgs.cmdPool.freeBuffer(cmd);
        for (auto& s : stagingOut) s.destroy();
        if (zeroCopyAlloc && interop)
            interop->free(std::move(zeroCopyAlloc));
        return nullptr;
    }

    // ── Cross-queue visibility (UPGRADE_PLAN Path C optimisation) ───────
    //
    // No inline vkWaitForFences here.  The compositor's submit waits on
    // the prefetch timeline semaphore GPU-side (see CompositeEngine's
    // collectProducerWaits + slot.submit with timeline wait), so the
    // worker can immediately return the CachedFrame; its texture will
    // only be sampled after the GPU semaphore wait is satisfied.
    //
    // The entry gate disables this GPU path if the shared timeline semaphore
    // could not be created, so every returned GPU-resident frame has a valid
    // producer signal for the graphics-queue wait.

    // Deferred cleanup: with the fence already signalled above, the
    // first pollAndCleanup pass on the next worker iteration will
    // reclaim these resources immediately — no GPU work outstanding.
    // We still push to wgs.pending rather than freeing inline so the
    // per-worker resource recycling stays in one place.
    gWorkerStep("convertDecodedToCacheGpu/submit/push-pending");
    WorkerGpuState::PendingSubmit p;
    p.fence       = fence;
    p.cmdBuf      = cmd;
    p.staging     = std::move(stagingOut);
    p.sharedAlloc = std::move(zeroCopyAlloc);
    p.interop     = interop;
    p.dstHold     = dstTex;  // keep the dst texture alive until fence signals
    wgs.pending.push_back(std::move(p));

    // Increment telemetry based on which path won.  zeroCopyAlloc has
    // been moved into the pending entry, so check the path indirectly
    // via whether interop was set up for this call.
    const bool wasZeroCopy = (wgs.pending.back().sharedAlloc != nullptr);
    if (wasZeroCopy) {
        m_perf.zeroCopyDecoded.fetch_add(1, std::memory_order_relaxed);
    } else {
        m_perf.gpuResidentDecoded.fetch_add(1, std::memory_order_relaxed);
    }

    // ── Build the GPU-resident CachedFrame.  ────────────────────────────
    // Same deleter pattern as convertDecodedToCache: recycle the empty
    // (or filled-by-lazy-readback) pixel buffer into the pixel pool.
    auto pool = m_pixelPool;
    auto cached = std::shared_ptr<CachedFrame>(
        new CachedFrame, [pool](CachedFrame* f) {
            pool->recycle(std::move(f->pixels));
            delete f;
        });
    cached->mediaId         = task.handle;
    cached->frameNumber     = frameNumber;
    cached->width           = static_cast<uint32_t>(dstW);
    cached->height          = static_cast<uint32_t>(dstH);
    cached->tier            = task.tier;
    cached->stride          = static_cast<uint32_t>(dstW) * 4;
    cached->isKeyframe      = decoded.isKeyframe;
    cached->timestamp       = decoded.timestamp;
    cached->pinned          = (task.info.frameCount <= 1);
    cached->isLoopFrame     = task.isLoop;
    cached->gpuReady        = true;
    cached->origin          = wasZeroCopy ? ConverterOrigin::GpuZeroCopy
                                          : ConverterOrigin::GpuShader;
    cached->gpuImageView    = reinterpret_cast<uint64_t>(dstTex->imageView());
    cached->gpuSampler      = reinterpret_cast<uint64_t>(dstTex->sampler());
    cached->gpuTextureOwner = dstTex;   // shared_ptr — co-owned with the cache
    // cached->pixels stays empty; lazyReadback materialises on demand.

    // Producer (sem, value) for the cross-queue visibility wait
    // performed by the compositor's submit.  Both fields are 0 when
    // the timeline semaphore could not be created (degraded mode).
    cached->producerTimelineSem   = reinterpret_cast<uint64_t>(prefetchSignalSem);
    cached->producerTimelineValue = prefetchSignalValue;

    // Lazy CPU readback for disk cache + export.  Captures the texture
    // shared_ptr by value so the readback can outlive the original
    // gpuTextureOwner if needed.  GpuContext::readbackTexture is
    // thread-safe.
    cached->lazyReadback = [texOwner = dstTex](std::vector<uint8_t>& outPixels) {
        if (!texOwner) return false;
        const uint32_t w = texOwner->width();
        const uint32_t h = texOwner->height();
        return GpuContext::get().readbackTexture(
            const_cast<Texture*>(texOwner.get()), w, h, outPixels);
    };

    return cached;
}

// ─────────────────────────────────────────────────────────────────────────
// WorkerGpuState::ensureChromaKeyPass
// ─────────────────────────────────────────────────────────────────────────
bool WorkerGpuState::ensureChromaKeyPass()
{
    if (chromaKeyReady) return true;

    auto& ctx = GpuContext::get();
    if (!ctx.isInitialized()) return false;
    const VkDevice dev = ctx.vkDevice();
    ckDevice = dev;

    // ── Descriptor set layout: binding 0 = storage image (output),
    //    binding 1 = combined image sampler (Nv12Converter output) ──────
    VkDescriptorSetLayoutBinding bindings[2]{};
    bindings[0].binding         = 0;
    bindings[0].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;

    bindings[1].binding         = 1;
    bindings[1].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutCreateInfo dslCI{};
    dslCI.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dslCI.bindingCount = 2;
    dslCI.pBindings    = bindings;
    if (vkCreateDescriptorSetLayout(dev, &dslCI, nullptr, &chromaKeyDescLayout) != VK_SUCCESS) {
        spdlog::warn("ChromaKeyPass: failed to create descriptor set layout");
        return false;
    }

    // ── Descriptor pool ────────────────────────────────────────────────
    VkDescriptorPoolSize poolSizes[2]{};
    poolSizes[0].type            = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    poolSizes[0].descriptorCount = 1;
    poolSizes[1].type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSizes[1].descriptorCount = 1;

    VkDescriptorPoolCreateInfo poolCI{};
    poolCI.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolCI.flags         = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    poolCI.maxSets       = 1;
    poolCI.poolSizeCount = 2;
    poolCI.pPoolSizes    = poolSizes;
    if (vkCreateDescriptorPool(dev, &poolCI, nullptr, &chromaKeyDescPool) != VK_SUCCESS) {
        spdlog::warn("ChromaKeyPass: failed to create descriptor pool");
        return false;
    }

    // ── Allocate descriptor set ────────────────────────────────────────
    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool     = chromaKeyDescPool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts        = &chromaKeyDescLayout;
    if (vkAllocateDescriptorSets(dev, &allocInfo, &chromaKeyDescSet) != VK_SUCCESS) {
        spdlog::warn("ChromaKeyPass: failed to allocate descriptor set");
        return false;
    }

    // ── Pipeline layout + push constants (matches chroma_key.comp) ─────
    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pushRange.offset     = 0;
    // width(int), height(int), paramCount(int), _pad(int), params[16](float)
    pushRange.size       = sizeof(int32_t) * 4 + sizeof(float) * 16;

    VkPipelineLayoutCreateInfo plCI{};
    plCI.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plCI.setLayoutCount         = 1;
    plCI.pSetLayouts            = &chromaKeyDescLayout;
    plCI.pushConstantRangeCount = 1;
    plCI.pPushConstantRanges    = &pushRange;
    if (vkCreatePipelineLayout(dev, &plCI, nullptr, &chromaKeyLayout) != VK_SUCCESS) {
        spdlog::warn("ChromaKeyPass: failed to create pipeline layout");
        return false;
    }

    // ── Load SPIR-V shader ─────────────────────────────────────────────
    fs::path spvPath = findShaderFile("chroma_key.comp.spv");
    if (spvPath.empty()) {
        spdlog::warn("ChromaKeyPass: chroma_key.comp.spv not found");
        return false;
    }
    std::ifstream file(spvPath, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        spdlog::warn("ChromaKeyPass: failed to open {}", pathToUtf8(spvPath));
        return false;
    }
    const size_t fileSize = static_cast<size_t>(file.tellg());
    std::vector<char> spirv(fileSize);
    file.seekg(0);
    file.read(spirv.data(), static_cast<std::streamsize>(fileSize));

    VkShaderModuleCreateInfo smCI{};
    smCI.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smCI.codeSize = spirv.size();
    smCI.pCode    = reinterpret_cast<const uint32_t*>(spirv.data());
    if (vkCreateShaderModule(dev, &smCI, nullptr, &chromaKeyShader) != VK_SUCCESS) {
        spdlog::warn("ChromaKeyPass: failed to create shader module");
        return false;
    }

    // ── Compute pipeline ───────────────────────────────────────────────
    VkComputePipelineCreateInfo pci{};
    pci.sType  = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pci.stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pci.stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
    pci.stage.module = chromaKeyShader;
    pci.stage.pName  = "main";
    pci.layout       = chromaKeyLayout;
    if (vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &pci, nullptr,
                                  &chromaKeyPipeline) != VK_SUCCESS) {
        spdlog::warn("ChromaKeyPass: failed to create compute pipeline");
        return false;
    }

    chromaKeyReady = true;
    spdlog::warn("ChromaKeyPass: pipeline ready (per-worker)");
    return true;
}

// ─────────────────────────────────────────────────────────────────────────
// Helpers for convertDecodedToCacheGpu's chroma-key branch
// ─────────────────────────────────────────────────────────────────────────
namespace {

/// Update the chroma-key descriptor set to point at the Nv12Converter
/// output (sampled) and the pooled destination texture (storage write).
void updateChromaKeyDescriptors(
    VkDevice dev,
    VkDescriptorSet ds,
    const Texture& srcTex,   // Nv12Converter output (BGRA, VK_IMAGE_LAYOUT_GENERAL)
    Texture&       dstTex)   // pooled dst (will be written as storage)
{
    // src: sampler2D — read converter output
    VkDescriptorImageInfo srcInfo{};
    srcInfo.sampler     = srcTex.sampler();
    srcInfo.imageView   = srcTex.imageView();
    srcInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    // dst: storage image — write keyed result
    VkDescriptorImageInfo dstInfo{};
    dstInfo.sampler     = VK_NULL_HANDLE;
    dstInfo.imageView   = dstTex.imageView();
    dstInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    VkWriteDescriptorSet writes[2]{};
    writes[0].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet          = ds;
    writes[0].dstBinding      = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[0].pImageInfo      = &dstInfo;

    writes[1].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet          = ds;
    writes[1].dstBinding      = 1;
    writes[1].descriptorCount = 1;
    writes[1].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[1].pImageInfo      = &srcInfo;

    vkUpdateDescriptorSets(dev, 2, writes, 0, nullptr);
}

/// Record the chroma-key compute dispatch into cmd.
/// On entry: srcTex (converter output) is in VK_IMAGE_LAYOUT_GENERAL.
///           dstTex must be in VK_IMAGE_LAYOUT_UNDEFINED (fresh from pool).
/// On exit:  dstTex is in VK_IMAGE_LAYOUT_GENERAL (storage-written).
///           Caller must transition dstTex to SHADER_READ_ONLY_OPTIMAL after.
void recordChromaKeyPass(
    VkCommandBuffer         cmd,
    WorkerGpuState&         wgs,
    const Texture&          srcTex,    // Nv12Converter output
    Texture&                dstTex,    // pooled destination
    uint32_t                width,
    uint32_t                height)
{
    const VkDevice dev = wgs.ckDevice;

    // ── Transition srcTex GENERAL → SHADER_READ_ONLY_OPTIMAL ──────────
    {
        VkImageMemoryBarrier b{};
        b.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.srcAccessMask       = VK_ACCESS_SHADER_WRITE_BIT;
        b.dstAccessMask       = VK_ACCESS_SHADER_READ_BIT;
        b.oldLayout           = VK_IMAGE_LAYOUT_GENERAL;
        b.newLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image               = const_cast<Texture&>(srcTex).image();  // read-only op
        b.subresourceRange    = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &b);
    }

    // ── Transition dstTex UNDEFINED → GENERAL (storage write) ──────────
    {
        VkImageMemoryBarrier b{};
        b.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.srcAccessMask       = 0;
        b.dstAccessMask       = VK_ACCESS_SHADER_WRITE_BIT;
        b.oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout           = VK_IMAGE_LAYOUT_GENERAL;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image               = dstTex.image();
        b.subresourceRange    = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &b);
    }

    // ── Update descriptor set ──────────────────────────────────────────
    updateChromaKeyDescriptors(dev, wgs.chromaKeyDescSet, srcTex, dstTex);

    // ── Bind + push + dispatch ─────────────────────────────────────────
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, wgs.chromaKeyPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, wgs.chromaKeyLayout,
                            0, 1, &wgs.chromaKeyDescSet, 0, nullptr);

    // Push constants: width, height, paramCount(6), _pad(0), params[6]
    struct CKPush {
        int32_t width;
        int32_t height;
        int32_t paramCount;
        int32_t _pad;
        float   params[16];
    } ck{};
    ck.width      = static_cast<int32_t>(width);
    ck.height     = static_cast<int32_t>(height);
    ck.paramCount = 6;
    ck._pad       = 0;
    // keyHue ~114° for #18FF00 green in HSL space
    ck.params[0] = 114.0f;   // hue (degrees)
    ck.params[1] = 0.85f;    // saturation
    ck.params[2] = 0.52f;    // lightness
    ck.params[3] = 0.38f;    // tolerance (generous for H.264)
    ck.params[4] = 0.12f;    // edge softness
    ck.params[5] = 1.0f;     // spill suppression (max)

    vkCmdPushConstants(cmd, wgs.chromaKeyLayout, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(ck), &ck);

    uint32_t gx = (width  + 15) / 16;
    uint32_t gy = (height + 15) / 16;
    vkCmdDispatch(cmd, gx, gy, 1);

    // ── Memory barrier: ensure storage writes are visible to next pass ─
    {
        VkMemoryBarrier mb{};
        mb.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0, 1, &mb, 0, nullptr, 0, nullptr);
    }
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────
// tryConvertDecodedToCacheGpu — see header.
// ─────────────────────────────────────────────────────────────────────────
std::shared_ptr<CachedFrame> tryConvertDecodedToCacheGpu(
    MediaPool&            pool,
    PrefetchDecoderState& state,
    const PrefetchTask&   task,
    DecodedFrame&         decoded,
    int64_t               frameNumber,
    WorkerGpuState*       wgs)
{
    if (!wgs || !wgs->ready())                          return nullptr;
    if (!CompositeService::gpuResidentDecodeEnabled())  return nullptr;
    return pool.convertDecodedToCacheGpu(state, task, decoded, frameNumber, *wgs);
}

} // namespace rt
