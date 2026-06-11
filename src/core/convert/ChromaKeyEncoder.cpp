/*
 * ChromaKeyEncoder.cpp — Standard H.264 encoder for chroma-key output.
 *
 * Encodes opaque RGBA frames as YUV420P H.264 MP4.  The alpha channel
 * is discarded — only RGB colour is encoded.  Uses NVENC for GPU
 * acceleration when available, libx264/libx265 as software fallback.
 * Shared machinery lives in MediaFileEncoderBase.
 */

#include "ChromaKeyEncoder.h"
#include "PathUtils.h"

#include <spdlog/spdlog.h>

#ifdef ROUNDTABLE_HAS_FFMPEG
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}
#endif

namespace rt {

ChromaKeyEncoder::~ChromaKeyEncoder()
{
    if (m_isOpen) finalize();
}

#ifdef ROUNDTABLE_HAS_FFMPEG

bool ChromaKeyEncoder::isNvencAvailable()
{
    return probeNvencH264("ChromaKeyEncoder");
}

bool ChromaKeyEncoder::open(const std::filesystem::path& path,
                            uint32_t width, uint32_t height,
                            int fps, int crf)
{
    if (m_isOpen) finalize();

    m_width  = (width  + 1) & ~1u;
    m_height = (height + 1) & ~1u;
    m_framesWritten = 0;
    m_usingNvenc = false;

    const int w = static_cast<int>(m_width);
    const int h = static_cast<int>(m_height);

    if (!createOutputContainer(path, "mp4"))
        return false;

    // NVENC (limited-range BT.709, normal video) → software fallback.
    if (!tryOpenNvencH264(w, h, fps, crf, AVCOL_RANGE_MPEG)) {
        if (!openSoftwareIntraFallback("libx264", "libx265", w, h, fps, crf))
            return false;
    }

    if (!createStreamOpenFileWriteHeader(path, fps))
        return false;

    if (!allocFrameAndPacket(AV_PIX_FMT_YUV420P, w, h))
        return false;

    if (!createSwsFromRgba(AV_PIX_FMT_YUV420P, w, h)) {
        finalize();
        return false;
    }

    m_isOpen = true;
    spdlog::info("ChromaKey: Opened {}x{} @ {}fps → {} [{}]",
                 m_width, m_height, fps, pathToUtf8(path),
                 m_usingNvenc ? "NVENC" : "CPU");
    return true;
}

bool ChromaKeyEncoder::writeFrame(const uint8_t* rgbaPixels)
{
    if (!m_isOpen) return false;

    // ── Convert RGBA → YUV420P (alpha is simply ignored by sws_scale) ──
    av_frame_make_writable(m_frame);

    const uint8_t* srcSlice[] = { rgbaPixels };
    int srcStride[] = { static_cast<int>(m_width * 4) };

    sws_scale(m_swsCtx, srcSlice, srcStride, 0, static_cast<int>(m_height),
              m_frame->data, m_frame->linesize);

    m_frame->pts = m_framesWritten;
    m_frame->pict_type = AV_PICTURE_TYPE_I;
    m_frame->key_frame = 1;

    const bool ok = m_usingNvenc ? sendFrameWithCudaUpload()
                                 : sendFrameToEncoder(m_frame);
    if (!ok)
        return false;

    ++m_framesWritten;
    return true;
}

#else // !ROUNDTABLE_HAS_FFMPEG

bool ChromaKeyEncoder::isNvencAvailable() { return false; }

bool ChromaKeyEncoder::open(const std::filesystem::path&,
                            uint32_t, uint32_t, int, int)
{
    m_lastError = "ChromaKey: FFmpeg not available in this build";
    return false;
}

bool ChromaKeyEncoder::writeFrame(const uint8_t*)
{
    return false;
}

#endif // ROUNDTABLE_HAS_FFMPEG

} // namespace rt
