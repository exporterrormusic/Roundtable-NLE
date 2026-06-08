/*
 * HWAlphaEncoder.cpp — GPU-accelerated H.264 encoder with packed alpha.
 *
 * Uses NVIDIA NVENC (h264_nvenc, YUV 4:2:0, full-range JPEG) with the
 * "packed-alpha" frame layout:
 *   top half  = RGB colour
 *   bottom half = alpha channel as greyscale (R=G=B=alpha)
 *
 * Why H.264 4:2:0 (not HEVC 4:4:4):
 *   • RTX/GeForce NVDEC fast path covers H.264 High @ 4:2:0 universally.
 *     HEVC 4:4:4 (rext) falls off the fast path and triggers a SW fallback
 *     (~60–115ms/frame at 4K), causing playback stalls.
 *   • Alpha precision is preserved: when R=G=B=A, RGB→YUV puts the alpha
 *     value into Y (full-resolution in 4:2:0) with U=V=neutral (128).
 *     Full-range (JPEG) color gives the full 0..255 alpha precision in Y.
 *   • Tiny chroma bleed at the seam between RGB and alpha halves is
 *     invisible because the alpha half's chroma is neutral and only the
 *     luma channel is sampled when reconstructing alpha.
 *
 * The encoder produces an MP4 file at width × (height*2).  On decode the
 * consumer splits the frame to reconstruct RGBA.
 */

#include "HWAlphaEncoder.h"
#include "PathUtils.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstring>
#include <thread>

#ifdef ROUNDTABLE_HAS_FFMPEG
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}
#endif

namespace rt {

#ifdef ROUNDTABLE_HAS_FFMPEG

// ═════════════════════════════════════════════════════════════════════════════
//  Static probe — can we use NVENC?
// ═════════════════════════════════════════════════════════════════════════════

bool HWAlphaEncoder::isNvencAvailable()
{
    // Try to find h264_nvenc encoder (universal NVDEC fast-path codec).
    const AVCodec* codec = avcodec_find_encoder_by_name("h264_nvenc");
    if (!codec) {
        spdlog::debug("HWAlphaEncoder: h264_nvenc codec not found in FFmpeg build");
        return false;
    }

    // Attempt to init a CUDA hw device — this will fail if no NVIDIA GPU
    AVBufferRef* hwCtx = nullptr;
    int ret = av_hwdevice_ctx_create(&hwCtx, AV_HWDEVICE_TYPE_CUDA, nullptr, nullptr, 0);
    if (ret < 0) {
        spdlog::debug("HWAlphaEncoder: CUDA hw device init failed (no NVIDIA GPU?)");
        return false;
    }

    // Try opening a tiny test encoder context to confirm NVENC works
    AVCodecContext* testCtx = avcodec_alloc_context3(codec);
    if (!testCtx) {
        av_buffer_unref(&hwCtx);
        return false;
    }

    testCtx->width = 64;
    testCtx->height = 64;
    testCtx->time_base = {1, 30};
    testCtx->pix_fmt = AV_PIX_FMT_CUDA;
    testCtx->hw_device_ctx = av_buffer_ref(hwCtx);

    // Allocate hw_frames_ctx
    AVBufferRef* hwFramesRef = av_hwframe_ctx_alloc(hwCtx);
    if (hwFramesRef) {
        auto* framesCtx = reinterpret_cast<AVHWFramesContext*>(hwFramesRef->data);
        framesCtx->format    = AV_PIX_FMT_CUDA;
        framesCtx->sw_format = AV_PIX_FMT_YUV420P;
        framesCtx->width     = 64;
        framesCtx->height    = 64;
        framesCtx->initial_pool_size = 4;
        av_hwframe_ctx_init(hwFramesRef);
        testCtx->hw_frames_ctx = av_buffer_ref(hwFramesRef);
        av_buffer_unref(&hwFramesRef);
    }

    ret = avcodec_open2(testCtx, codec, nullptr);
    bool available = (ret >= 0);

    avcodec_free_context(&testCtx);
    av_buffer_unref(&hwCtx);

    if (available)
        spdlog::info("HWAlphaEncoder: NVENC H.264 encoding is AVAILABLE");
    else
        spdlog::info("HWAlphaEncoder: NVENC probe failed — will use CPU fallback");

    return available;
}

// ═════════════════════════════════════════════════════════════════════════════
//  Constructor / Destructor
// ═════════════════════════════════════════════════════════════════════════════

HWAlphaEncoder::HWAlphaEncoder() = default;

HWAlphaEncoder::~HWAlphaEncoder()
{
    if (m_isOpen) finalize();
}

// ═════════════════════════════════════════════════════════════════════════════
//  Open
// ═════════════════════════════════════════════════════════════════════════════

bool HWAlphaEncoder::open(const std::filesystem::path& path,
                           uint32_t width, uint32_t height,
                           int fps, int crf)
{
    if (m_isOpen) finalize();

    m_width  = (width  + 1) & ~1u;
    m_height = (height + 1) & ~1u;
    m_framesWritten = 0;
    m_usingNvenc = false;

    // Packed-alpha doubles the height
    uint32_t packedH = m_height * 2;
    // Ensure packedH is even (it will be since m_height is already even)

    // Allocate intermediate packed RGBA buffer
    m_packedRGBA.resize(static_cast<size_t>(m_width) * packedH * 4);

    // ── Create output format context (MP4) ──────────────────────────────
    int ret = avformat_alloc_output_context2(&m_fmtCtx, nullptr, "mp4",
                                              pathToUtf8(path).c_str());
    if (ret < 0 || !m_fmtCtx) {
        m_lastError = "HWAlpha: Failed to create MP4 output context";
        spdlog::error("{}", m_lastError);
        return false;
    }

    // ── Try NVENC first ─────────────────────────────────────────────────
    const AVCodec* codec = avcodec_find_encoder_by_name("h264_nvenc");
    bool nvencOk = false;

    if (codec) {
        // Create CUDA hw device
        ret = av_hwdevice_ctx_create(&m_hwDeviceCtx, AV_HWDEVICE_TYPE_CUDA,
                                      nullptr, nullptr, 0);
        if (ret >= 0) {
            m_codecCtx = avcodec_alloc_context3(codec);
            if (m_codecCtx) {
                m_codecCtx->width     = static_cast<int>(m_width);
                m_codecCtx->height    = static_cast<int>(packedH);
                m_codecCtx->time_base = {1, fps};
                m_codecCtx->framerate = {fps, 1};
                m_codecCtx->pix_fmt    = AV_PIX_FMT_CUDA;
                // h264_nvenc rejects gop_size=1 with bf=0 ("Gop Length
                // should be greater than number of B frames + 1").  Use
                // gop_size=2 with forced-idr + every-frame-keyframe so
                // every output frame is still an IDR and seeking is O(1).
                m_codecCtx->gop_size   = 2;
                m_codecCtx->max_b_frames = 0;
                // Full-range JPEG color so alpha (encoded as R=G=B=alpha)
                // maps to luma Y across the full 0..255 range, preserving
                // all 256 alpha levels.
                m_codecCtx->color_range     = AVCOL_RANGE_JPEG;
                m_codecCtx->color_primaries = AVCOL_PRI_BT709;
                m_codecCtx->color_trc       = AVCOL_TRC_BT709;
                m_codecCtx->colorspace      = AVCOL_SPC_BT709;

                // NVENC quality settings.  p4 = balanced quality/speed,
                // hq tune = best quality at this preset.  forced-idr
                // converts every keyframe into an IDR so seeking lands
                // on a self-contained frame.
                av_opt_set(m_codecCtx->priv_data, "preset", "p4", 0);
                av_opt_set(m_codecCtx->priv_data, "tune",   "hq", 0);
                av_opt_set(m_codecCtx->priv_data, "rc", "constqp", 0);
                av_opt_set_int(m_codecCtx->priv_data, "qp", crf, 0);
                av_opt_set(m_codecCtx->priv_data, "profile", "high", 0);   // H.264 High @ 4:2:0 (NVDEC fast path)
                av_opt_set_int(m_codecCtx->priv_data, "forced-idr", 1, 0);

                m_codecCtx->hw_device_ctx = av_buffer_ref(m_hwDeviceCtx);

                // Set up hardware frames context
                m_hwFramesCtx = av_hwframe_ctx_alloc(m_hwDeviceCtx);
                if (m_hwFramesCtx) {
                    auto* framesCtx = reinterpret_cast<AVHWFramesContext*>(m_hwFramesCtx->data);
                    framesCtx->format         = AV_PIX_FMT_CUDA;
                    framesCtx->sw_format      = AV_PIX_FMT_YUV420P;
                    framesCtx->width          = static_cast<int>(m_width);
                    framesCtx->height         = static_cast<int>(packedH);
                    framesCtx->initial_pool_size = 8;

                    ret = av_hwframe_ctx_init(m_hwFramesCtx);
                    if (ret >= 0) {
                        m_codecCtx->hw_frames_ctx = av_buffer_ref(m_hwFramesCtx);

                        if (m_fmtCtx->oformat->flags & AVFMT_GLOBALHEADER)
                            m_codecCtx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

                        ret = avcodec_open2(m_codecCtx, codec, nullptr);
                        if (ret >= 0) {
                            nvencOk = true;
                            m_usingNvenc = true;
                            spdlog::info("HWAlpha: NVENC H.264 encoder opened "
                                         "({}x{} packed 4:2:0 full-range, QP={})",
                                         m_width, packedH, crf);
                        } else {
                            char errBuf[256];
                            av_strerror(ret, errBuf, sizeof(errBuf));
                            spdlog::warn("HWAlpha: avcodec_open2 h264_nvenc failed: {}", errBuf);
                        }
                    } else {
                        spdlog::warn("HWAlpha: hw_frames_ctx init failed");
                    }
                }

                if (!nvencOk) {
                    avcodec_free_context(&m_codecCtx);
                    if (m_hwFramesCtx) { av_buffer_unref(&m_hwFramesCtx); m_hwFramesCtx = nullptr; }
                    av_buffer_unref(&m_hwDeviceCtx);
                    m_hwDeviceCtx = nullptr;
                }
            }
        } else {
            spdlog::debug("HWAlpha: CUDA device init failed");
        }
    }

    // ── Fallback: software HEVC (libx265) or H.264 (libx264) ───────────
    if (!nvencOk) {
        // Try libx265 first (HEVC, good compression), then libx264
        codec = avcodec_find_encoder_by_name("libx265");
        if (!codec)
            codec = avcodec_find_encoder_by_name("libx264");
        if (!codec) {
            m_lastError = "HWAlpha: No suitable encoder found (hevc_nvenc/libx265/libx264)";
            spdlog::error("{}", m_lastError);
            avformat_free_context(m_fmtCtx); m_fmtCtx = nullptr;
            return false;
        }

        m_codecCtx = avcodec_alloc_context3(codec);
        m_codecCtx->width     = static_cast<int>(m_width);
        m_codecCtx->height    = static_cast<int>(packedH);
        m_codecCtx->time_base = {1, fps};
        m_codecCtx->framerate = {fps, 1};
        m_codecCtx->pix_fmt   = AV_PIX_FMT_YUV420P;
        m_codecCtx->gop_size  = 1;  // All-intra: every frame is a keyframe (instant seeking)
        m_codecCtx->max_b_frames = 0;

        // CRF mode
        av_opt_set_int(m_codecCtx->priv_data, "crf", crf, 0);

        // Threading
        int hwThreads = static_cast<int>(std::thread::hardware_concurrency());
        m_codecCtx->thread_count = std::max(1, hwThreads / 2);

        // Speed presets
        if (std::string(codec->name) == "libx265") {
            av_opt_set(m_codecCtx->priv_data, "preset", "fast", 0);
        } else {
            av_opt_set(m_codecCtx->priv_data, "preset", "veryfast", 0);
        }

        if (m_fmtCtx->oformat->flags & AVFMT_GLOBALHEADER)
            m_codecCtx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

        ret = avcodec_open2(m_codecCtx, codec, nullptr);
        if (ret < 0) {
            char errBuf[256];
            av_strerror(ret, errBuf, sizeof(errBuf));
            m_lastError = std::string("HWAlpha: Failed to open software encoder: ") + errBuf;
            spdlog::error("{}", m_lastError);
            avcodec_free_context(&m_codecCtx);
            avformat_free_context(m_fmtCtx); m_fmtCtx = nullptr;
            return false;
        }

        spdlog::info("HWAlpha: Using software encoder '{}' ({}x{} packed, CRF={})",
                      codec->name, m_width, packedH, crf);
    }

    // ── Create video stream ─────────────────────────────────────────────
    m_stream = avformat_new_stream(m_fmtCtx, nullptr);
    if (!m_stream) {
        m_lastError = "HWAlpha: Failed to create video stream";
        avcodec_free_context(&m_codecCtx);
        avformat_free_context(m_fmtCtx); m_fmtCtx = nullptr;
        return false;
    }
    m_stream->id = 0;
    m_stream->time_base = {1, fps};
    avcodec_parameters_from_context(m_stream->codecpar, m_codecCtx);

    // ── Open output file ────────────────────────────────────────────────
    if (!(m_fmtCtx->oformat->flags & AVFMT_NOFILE)) {
        ret = avio_open(&m_fmtCtx->pb, pathToUtf8(path).c_str(), AVIO_FLAG_WRITE);
        if (ret < 0) {
            m_lastError = "HWAlpha: Failed to open output file";
            spdlog::error("{}", m_lastError);
            avcodec_free_context(&m_codecCtx);
            avformat_free_context(m_fmtCtx); m_fmtCtx = nullptr;
            return false;
        }
    }

    // Tag the container so consumers auto-detect packed-alpha layout
    av_dict_set(&m_fmtCtx->metadata, "packed_alpha", "1", 0);

    // Write file header – pass movflags to preserve custom metadata in MP4
    AVDictionary* muxOpts = nullptr;
    av_dict_set(&muxOpts, "movflags", "+use_metadata_tags", 0);
    ret = avformat_write_header(m_fmtCtx, &muxOpts);
    av_dict_free(&muxOpts);
    if (ret < 0) {
        m_lastError = "HWAlpha: Failed to write container header";
        spdlog::error("{}", m_lastError);
        avio_closep(&m_fmtCtx->pb);
        avcodec_free_context(&m_codecCtx);
        avformat_free_context(m_fmtCtx); m_fmtCtx = nullptr;
        return false;
    }

    // ── Allocate frame + packet ─────────────────────────────────────────
    // Both NVENC and SW path now use YUV 4:2:0; alpha precision is
    // preserved in the (full-resolution) luma channel.
    AVPixelFormat swFmt = AV_PIX_FMT_YUV420P;

    m_frame = av_frame_alloc();
    m_frame->format = static_cast<int>(swFmt);
    m_frame->width  = static_cast<int>(m_width);
    m_frame->height = static_cast<int>(packedH);
    // Tag the frame with full-range JPEG color so sws_scale uses the
    // full 0..255 mapping and alpha doesn't clip into 16..235.
    m_frame->color_range     = AVCOL_RANGE_JPEG;
    m_frame->color_primaries = AVCOL_PRI_BT709;
    m_frame->color_trc       = AVCOL_TRC_BT709;
    m_frame->colorspace      = AVCOL_SPC_BT709;
    av_frame_get_buffer(m_frame, 0);

    m_packet = av_packet_alloc();

    // ── sws_scale: RGBA → YUV420P (full-range JPEG) ────────────────────
    m_swsCtx = sws_getContext(
        static_cast<int>(m_width), static_cast<int>(packedH), AV_PIX_FMT_RGBA,
        static_cast<int>(m_width), static_cast<int>(packedH), swFmt,
        SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!m_swsCtx) {
        m_lastError = "HWAlpha: Failed to create sws_scale context";
        spdlog::error("{}", m_lastError);
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

// ═════════════════════════════════════════════════════════════════════════════
//  Write Frame
// ═════════════════════════════════════════════════════════════════════════════

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

    // ── Convert packed RGBA → YUV444P/YUV420P ─────────────────────────
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

    // ── If NVENC, upload CPU frame to GPU ────────────────────────────────
    if (m_usingNvenc) {
        AVFrame* hwFrame = av_frame_alloc();
        hwFrame->format = AV_PIX_FMT_CUDA;
        hwFrame->width  = m_frame->width;
        hwFrame->height = m_frame->height;

        int ret = av_hwframe_get_buffer(m_codecCtx->hw_frames_ctx, hwFrame, 0);
        if (ret < 0) {
            m_lastError = "HWAlpha: Failed to get HW frame buffer";
            av_frame_free(&hwFrame);
            return false;
        }

        ret = av_hwframe_transfer_data(hwFrame, m_frame, 0);
        if (ret < 0) {
            m_lastError = "HWAlpha: Failed to upload frame to GPU";
            av_frame_free(&hwFrame);
            return false;
        }

        hwFrame->pts = m_frame->pts;
        hwFrame->pict_type = AV_PICTURE_TYPE_I;
        hwFrame->key_frame = 1;

        ret = avcodec_send_frame(m_codecCtx, hwFrame);
        av_frame_free(&hwFrame);
        if (ret < 0) {
            m_lastError = "HWAlpha: Error sending HW frame to encoder";
            return false;
        }
    } else {
        // Software encode path
        int ret = avcodec_send_frame(m_codecCtx, m_frame);
        if (ret < 0) {
            m_lastError = "HWAlpha: Error sending frame to encoder";
            return false;
        }
    }

    if (!receiveAndWritePackets())
        return false;

    ++m_framesWritten;
    return true;
}

// ═════════════════════════════════════════════════════════════════════════════
//  Receive & Write Packets (shared by writeFrame and flushEncoder)
// ═════════════════════════════════════════════════════════════════════════════

bool HWAlphaEncoder::receiveAndWritePackets()
{
    while (true) {
        int ret = avcodec_receive_packet(m_codecCtx, m_packet);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            break;
        if (ret < 0) {
            m_lastError = "HWAlpha: Error receiving packet from encoder";
            return false;
        }

        av_packet_rescale_ts(m_packet, m_codecCtx->time_base, m_stream->time_base);
        m_packet->stream_index = m_stream->index;
        ret = av_interleaved_write_frame(m_fmtCtx, m_packet);
        if (ret < 0) {
            m_lastError = "HWAlpha: Error writing packet to container";
            return false;
        }
    }
    return true;
}

// ═════════════════════════════════════════════════════════════════════════════
//  Flush / Finalize
// ═════════════════════════════════════════════════════════════════════════════

bool HWAlphaEncoder::flushEncoder()
{
    if (!m_codecCtx) return false;

    avcodec_send_frame(m_codecCtx, nullptr);
    return receiveAndWritePackets();
}

bool HWAlphaEncoder::finalize()
{
    if (!m_isOpen) return false;

    flushEncoder();

    if (m_fmtCtx) {
        av_write_trailer(m_fmtCtx);
    }

    // Cleanup
    if (m_swsCtx) { sws_freeContext(m_swsCtx); m_swsCtx = nullptr; }
    if (m_packet) { av_packet_free(&m_packet); }
    if (m_frame)  { av_frame_free(&m_frame); }
    if (m_codecCtx) { avcodec_free_context(&m_codecCtx); }

    if (m_hwFramesCtx) { av_buffer_unref(&m_hwFramesCtx); m_hwFramesCtx = nullptr; }
    if (m_hwDeviceCtx) { av_buffer_unref(&m_hwDeviceCtx); m_hwDeviceCtx = nullptr; }

    if (m_fmtCtx) {
        if (!(m_fmtCtx->oformat->flags & AVFMT_NOFILE))
            avio_closep(&m_fmtCtx->pb);
        avformat_free_context(m_fmtCtx);
        m_fmtCtx = nullptr;
    }

    m_stream = nullptr;
    m_isOpen = false;
    m_packedRGBA.clear();
    m_packedRGBA.shrink_to_fit();

    spdlog::info("HWAlpha: Finalized — {} frames written [{}]",
                 m_framesWritten, m_usingNvenc ? "NVENC" : "CPU");
    return true;
}

#else // !ROUNDTABLE_HAS_FFMPEG

bool HWAlphaEncoder::isNvencAvailable() { return false; }

HWAlphaEncoder::HWAlphaEncoder() = default;
HWAlphaEncoder::~HWAlphaEncoder() = default;

bool HWAlphaEncoder::open(const std::filesystem::path&,
                           uint32_t, uint32_t, int, int)
{
    m_lastError = "FFmpeg not available";
    return false;
}

bool HWAlphaEncoder::writeFrame(const uint8_t*) { return false; }
bool HWAlphaEncoder::finalize() { return false; }
bool HWAlphaEncoder::flushEncoder() { return false; }

#endif // ROUNDTABLE_HAS_FFMPEG

} // namespace rt
