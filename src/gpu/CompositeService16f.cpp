/*
 * CompositeService16f.cpp — Phase 4.2 export 16-bit-float passthrough.
 *
 * tryBuild16fPassthrough: when the composite at a tick is a single full-frame
 * opaque >8-bit BT.709-limited-SDR video clip rendered 1:1, decode that one
 * source frame, GPU-convert it to RGBA16F, and hand back a CachedFrame carrying
 * the RGBA16F (for the 10-bit encoder) plus a dithered 8-bit BGRA copy (for the
 * export-preview display).  Any miss returns nullptr → the caller falls back to
 * the normal 8-bit compositeFrame, so a bug here only ever degrades export to
 * 8-bit; it cannot corrupt output.  See the header for the full contract.
 *
 * Runs on the compositor (main) thread alongside compositeFrame — it uses the
 * shared GpuContext Nv12Converter (internally locked) and the GpuContext
 * command pool, which are single-threaded on this thread.
 */

#include "CompositeService.h"

#include "GpuContext.h"
#include "Nv12Converter.h"

#include "PathUtils.h"
#include "cache/FrameCache.h"
#include "decode/VideoDecoder.h"
#include "decode/VideoFrameMapping.h"
#include "playback/MediaPool.h"
#include "timeline/PassthroughEligibility.h"
#include "timeline/Timeline.h"
#include "timeline/VideoClip.h"

#include <spdlog/spdlog.h>
#include <cstring>

namespace rt {

namespace {

// IEEE-754 binary16 (little-endian uint16) → float32.  Local copy so the GPU
// layer stays free of the export-side Rgba16fPack; only used for the 8-bit
// preview down-copy below, where exact rounding is immaterial.
inline float halfToF(uint16_t h) noexcept
{
    const uint32_t sign = static_cast<uint32_t>(h & 0x8000u) << 16;
    const uint32_t exp  = (h >> 10) & 0x1Fu;
    const uint32_t mant = h & 0x3FFu;
    if (exp == 0x1Fu) return 1.0f;                 // Inf/NaN → clamp
    if (exp == 0)     return sign ? -0.0f : 0.0f;  // flush subnormal
    const uint32_t bits = sign | ((exp + 112u) << 23) | (mant << 13);
    float f;
    std::memcpy(&f, &bits, sizeof(f));
    return f;
}

inline uint8_t to8(float v) noexcept
{
    const float c = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
    return static_cast<uint8_t>(c * 255.0f + 0.5f);
}

// Down-copy straight-RGBA16F → 8-bit BGRA (the compositor's native layout) so
// the export-preview display can show the passthrough frame.  Plain round —
// this copy is never encoded (the encoder takes the 16F directly).
void rgba16fToBgra8(const std::vector<uint8_t>& rgba16f, uint32_t srcStride,
                    uint32_t w, uint32_t h, std::vector<uint8_t>& outBgra)
{
    outBgra.resize(static_cast<size_t>(w) * h * 4);
    for (uint32_t y = 0; y < h; ++y) {
        const auto* row = reinterpret_cast<const uint16_t*>(
            rgba16f.data() + static_cast<size_t>(y) * srcStride);
        uint8_t* dst = outBgra.data() + static_cast<size_t>(y) * w * 4;
        for (uint32_t x = 0; x < w; ++x) {
            const uint16_t* px = row + static_cast<size_t>(x) * 4;
            const float r = halfToF(px[0]);
            const float g = halfToF(px[1]);
            const float b = halfToF(px[2]);
            const float a = halfToF(px[3]);
            dst[x * 4 + 0] = to8(b);   // BGRA
            dst[x * 4 + 1] = to8(g);
            dst[x * 4 + 2] = to8(r);
            dst[x * 4 + 3] = to8(a);
        }
    }
}

} // namespace

std::shared_ptr<CachedFrame> CompositeService::tryBuild16fPassthrough(
    int64_t tick, uint32_t outW, uint32_t outH)
{
    if (!m_timeline || !m_mediaPool) return nullptr;

    // ── 1. Timeline-level eligibility (single opaque 1:1 video clip) ─────────
    PassthroughTarget pt = evaluatePassthroughAt(*m_timeline, tick);
    if (!pt.eligible || !pt.clip) return nullptr;
    VideoClip* clip = pt.clip;

    // ── 2. Resolve source handle + stream info ──────────────────────────────
    bool skip = false;
    const uint64_t handle = resolveVideoClipHandle(clip, /*playbackNonBlocking=*/false, skip);
    if (skip || handle == 0) return nullptr;
    const VideoStreamInfo* info = m_mediaPool->getInfo(handle);
    if (!info) return nullptr;

    // ── 3. Source-level gates ───────────────────────────────────────────────
    if (info->bitDepth <= 8) return nullptr;                       // only >8-bit benefits
    if (info->hasAlpha && info->packedAlpha) return nullptr;       // packed-alpha needs the unpack path
    if (info->packedTiles > 0) return nullptr;                     // stacked-tile proxy unsupported here
    if (info->isVFR) return nullptr;                               // frame mapping assumes CFR
    if (static_cast<uint32_t>(info->width) != outW ||
        static_cast<uint32_t>(info->contentHeight(static_cast<int>(info->height))) != outH)
        return nullptr;                                            // 1:1 only (no scale)
    if (!resolveColorConversion(*info).isGpuShaderDefault)
        return nullptr;                                            // 16F shaders hardcode BT.709-limited-SDR

    // ── 4. Source frame number (the ONE shared mapping authority) ───────────
    const int64_t frameNum = mapTickToSourceFrame(*clip, tick, info).frame;
    if (frameNum < 0) return nullptr;

    // ── 5. Decode that source frame to native CPU planes ────────────────────
    // Force software decode so data[]/linesize[] are CPU-side P010 / yuva444p12
    // planes (NVDEC would hand back a GPU frame).  The decoder is reused across
    // frames of the same source.
    const std::string& path = clip->mediaPath();
    if (path.empty()) return nullptr;
    if (!m_passthroughDecoder || m_passthroughDecoderPath != path) {
        m_passthroughDecoder = std::make_unique<VideoDecoder>();
        if (!m_passthroughDecoder->open(utf8ToPath(path), /*forceSoftware=*/true)) {
            m_passthroughDecoder.reset();
            m_passthroughDecoderPath.clear();
            m_passthroughLastFrame = -1;
            return nullptr;
        }
        m_passthroughDecoderPath = path;
        m_passthroughLastFrame = -1;
    }
    // Sequential export is the common case: when the requested frame is the one
    // right after the last, just decode forward — a Precise SEEK per frame would
    // re-decode from the GOP keyframe each time (O(N²) on long-GOP HEVC/AV1).
    DecodedFrame decoded;
    if (frameNum == m_passthroughLastFrame + 1) {
        if (!m_passthroughDecoder->decodeNext(decoded)) { m_passthroughLastFrame = -1; return nullptr; }
    } else {
        if (!m_passthroughDecoder->seekToFrame(frameNum, SeekMode::Precise)) { m_passthroughLastFrame = -1; return nullptr; }
        if (!m_passthroughDecoder->decodeNext(decoded)) { m_passthroughLastFrame = -1; return nullptr; }
    }
    m_passthroughLastFrame = frameNum;
    if (decoded.width != outW || decoded.height != outH) return nullptr;  // unexpected; bail safely

    // ── 6. GPU convert native planes → RGBA16F, read back to CPU ────────────
    Nv12Converter* conv = GpuContext::get().nv12Converter(outW, outH);
    if (!conv) return nullptr;

    std::vector<uint8_t> rgba16f;
    bool ok = false;
    switch (hbdPlaneFormat(decoded)) {
        case HbdPlaneFormat::P010:
            ok = conv->convertAndReadbackP010Scaled16F(
                decoded.data[0], decoded.linesize[0],
                decoded.data[1], decoded.linesize[1],
                decoded.width, decoded.height, outW, outH, rgba16f);
            break;
        case HbdPlaneFormat::Yuva444p12:
            ok = conv->convertAndReadbackYuva444p12Scaled16F(
                decoded.data[0], decoded.linesize[0],
                decoded.data[1], decoded.linesize[1],
                decoded.data[2], decoded.linesize[2],
                decoded.data[3], decoded.linesize[3],
                decoded.width, decoded.height, outW, outH, rgba16f);
            break;
        case HbdPlaneFormat::None:
        default:
            return nullptr;   // a >8-bit format the 16F converter doesn't handle
    }
    if (!ok || rgba16f.empty()) return nullptr;

    // ── 7. Build the dual-payload CachedFrame ───────────────────────────────
    auto frame = std::make_shared<CachedFrame>();
    frame->mediaId       = handle;
    frame->frameNumber   = frameNum;
    frame->width         = outW;
    frame->height        = outH;
    frame->stride        = outW * 4;             // 8-bit BGRA copy
    frame->timestamp     = decoded.timestamp;
    frame->depth         = 16;
    frame->rgba16fStride = outW * 8;             // RGBA16F: 8 bytes/px, tightly packed
    frame->rgba16f       = std::move(rgba16f);
    // 8-bit BGRA copy so display / export-preview consumers work unchanged; the
    // encoder ignores this and packs the rgba16f directly.
    rgba16fToBgra8(frame->rgba16f, frame->rgba16fStride, outW, outH, frame->pixels);

    static uint64_t s_count = 0;
    if ((++s_count % 120) == 1)
        spdlog::info("[16F-PASS] tick={} src='{}' frame={} {}x{} bitDepth={} → RGBA16F (#{} so far)",
                     tick, pathToUtf8(utf8ToPath(path).filename()), frameNum,
                     outW, outH, info->bitDepth, s_count);
    return frame;
}

} // namespace rt
