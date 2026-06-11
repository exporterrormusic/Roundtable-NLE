/*
 * HWAlphaEncoder.cpp — packed-alpha H.264/HEVC encoder.
 * Shared machinery lives in MediaFileEncoderBase; this file owns the
 * packed-alpha specifics: 2×-height frame, full-range (JPEG) color so
 * alpha-in-luma keeps all 256 levels, the RGB/alpha stacking, and the
 * "packed_alpha" container tag.
 */

#include "HWAlphaEncoder.h"
#include "PathUtils.h"

#include <spdlog/spdlog.h>

#include <cstring>

#ifdef ROUNDTABLE_HAS_FFMPEG
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}
#endif

namespace rt {

HWAlphaEncoder::~HWAlphaEncoder()
{
    if (m_isOpen) finalize();
}

#ifdef ROUNDTABLE_HAS_FFMPEG

bool HWAlphaEncoder::isNvencAvailable()
{
    return probeNvencH264("HWAlphaEncoder");
}

bool HWAlphaEncoder::open(const std::filesystem::path& path,
                          uint32_t width, uint32_t height,
                          int fps, int crf)
{
    if (m_isOpen) finalize();

    m_width  = (width  + 1) & ~1u;
    m_height = (height + 1) & ~1u;
    m_framesWritten = 0;
    m_usingNvenc = false;

    // Packed-alpha doubles the height (even since m_height is even).
    const uint32_t packedH = m_height * 2;
    const int w  = static_cast<int>(m_width);
    const int ph = static_cast<int>(packedH);

    // Allocate intermediate packed RGBA buffer
    m_packedRGBA.resize(static_cast<size_t>(m_width) * packedH * 4);

    if (!createOutputContainer(path, "mp4"))
        return false;

    // NVENC first — FULL-range (JPEG) color so alpha encoded as luma maps
    // across the whole 0..255 range, preserving all 256 alpha levels.
    if (!tryOpenNvencH264(w, ph, fps, crf, AVCOL_RANGE_JPEG)) {
        // Software fallback: libx265 first (better compression), libx264 next.
        if (!openSoftwareIntraFallback("libx265", "libx264", w, ph, fps, crf))
            return false;
    }

    // Tag the container so consumers auto-detect packed-alpha layout;
    // +use_metadata_tags preserves the custom tag in MP4.
    av_dict_set(&m_fmtCtx->metadata, "packed_alpha", "1", 0);
    AVDictionary* muxOpts = nullptr;
    av_dict_set(&muxOpts, "movflags", "+use_metadata_tags", 0);
    const bool headerOk = createStreamOpenFileWriteHeader(path, fps, &muxOpts);
    av_dict_free(&muxOpts);
    if (!headerOk)
        return false;

    if (!allocFrameAndPacket(AV_PIX_FMT_YUV420P, w, ph))
        return false;
    // Tag the frame with full-range JPEG color so sws_scale uses the full
    // 0..255 mapping and alpha doesn't clip into 16..235.
    m_frame->color_range     = AVCOL_RANGE_JPEG;
    m_frame->color_primaries = AVCOL_PRI_BT709;
    m_frame->color_trc       = AVCOL_TRC_BT709;
    m_frame->colorspace      = AVCOL_SPC_BT709;

    if (!createSwsFromRgba(AV_PIX_FMT_YUV420P, w, ph)) {
        finalize();
        return false;
    }

    // Configure sws for full-range (JPEG) BT.709 output.  Without this,
    // alpha values 0..255 get squeezed into Y range 16..235 (~219 levels)
    // and the reconstructed alpha loses ~14% of its precision.
    {
        const int* invTable = sws_getCoefficients(SWS_CS_ITU709);
        const int* table    = sws_getCoefficients(SWS_CS_ITU709);
        sws_setColorspaceDetails(m_swsCtx,
                                 invTable, /*srcRange=*/1,   // RGBA is always full-range
                                 table,    /*dstRange=*/1,   // YUV full-range (JPEG)
                                 /*brightness=*/0, /*contrast=*/1 << 16, /*saturation=*/1 << 16);
    }

    m_isOpen = true;
    spdlog::info("HWAlpha: Opened {}x{} (packed {}x{}) @ {}fps → {} [{}]",
                 m_width, m_height, m_width, packedH, fps, pathToUtf8(path),
                 m_usingNvenc ? "NVENC" : "CPU");
    return true;
}

bool HWAlphaEncoder::writeFrame(const uint8_t* rgbaPixels)
{
    if (!m_isOpen) return false;

    const uint32_t stride = m_width * 4;
    const uint32_t packedH = m_height * 2;

    // ── Build packed-alpha RGBA buffer ───────────────────────────────────
    // Top half: original RGB (keep alpha=255 for opaque encode)
    // Bottom half: alpha channel replicated into R, G, B with A=255
    uint8_t* dst = m_packedRGBA.data();
    const uint8_t* src = rgbaPixels;

    // Top half: copy RGB, force alpha=255 so colour isn't premul-distorted
    for (uint32_t y = 0; y < m_height; ++y) {
        const uint8_t* row = src + y * stride;
        uint8_t* dstRow = dst + y * stride;
        std::memcpy(dstRow, row, stride);
        for (uint32_t x = 0; x < m_width; ++x)
            dstRow[x * 4 + 3] = 255;
    }

    // Bottom half: alpha channel replicated as greyscale (R=G=B=alpha)
    for (uint32_t y = 0; y < m_height; ++y) {
        const uint8_t* row = src + y * stride;
        uint8_t* dstRow = dst + (m_height + y) * stride;
        for (uint32_t x = 0; x < m_width; ++x) {
            uint8_t a = row[x * 4 + 3];
            dstRow[x * 4]     = a;
            dstRow[x * 4 + 1] = a;
            dstRow[x * 4 + 2] = a;
            dstRow[x * 4 + 3] = 255;
        }
    }

    // ── Convert packed RGBA → YUV420P ──────────────────────────────────
    av_frame_make_writable(m_frame);

    const uint8_t* srcSlice[] = { m_packedRGBA.data() };
    int srcStride[] = { static_cast<int>(stride) };

    sws_scale(m_swsCtx, srcSlice, srcStride, 0, static_cast<int>(packedH),
              m_frame->data, m_frame->linesize);

    m_frame->pts = m_framesWritten;
    // Force every frame to be a keyframe.  Combined with NVENC's
    // forced-idr=1, this makes every output frame an IDR so seeking
    // is O(1) regardless of gop_size.
    m_frame->pict_type = AV_PICTURE_TYPE_I;
    m_frame->key_frame = 1;

    const bool ok = m_usingNvenc ? sendFrameWithCudaUpload()
                                 : sendFrameToEncoder(m_frame);
    if (!ok)
        return false;

    ++m_framesWritten;
    return true;
}

bool HWAlphaEncoder::finalize()
{
    if (!MediaFileEncoderBase::finalize())
        return false;

    m_packedRGBA.clear();
    m_packedRGBA.shrink_to_fit();
    return true;
}

#else // !ROUNDTABLE_HAS_FFMPEG

bool HWAlphaEncoder::isNvencAvailable() { return false; }

bool HWAlphaEncoder::open(const std::filesystem::path&,
                          uint32_t, uint32_t, int, int)
{
    m_lastError = "FFmpeg not available";
    return false;
}

bool HWAlphaEncoder::writeFrame(const uint8_t*) { return false; }
bool HWAlphaEncoder::finalize() { return false; }

#endif // ROUNDTABLE_HAS_FFMPEG

} // namespace rt
