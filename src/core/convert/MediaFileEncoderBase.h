/*
 * MediaFileEncoderBase — shared machinery for the character-conversion
 * encoders (ChromaKeyEncoder, HWAlphaEncoder, ProResAlphaEncoder,
 * VP9AlphaEncoder).
 *
 * Unlike the export-side encoders (export/FfmpegEncoderBase, which hand
 * packets to a separate Muxer), these write a complete media FILE
 * themselves: container + single video stream + encode + interleaved
 * write + trailer.  All four previously carried verbatim copies of the
 * container setup, the NVENC H.264 open block, the software fallback,
 * the packet-drain/write loop, flush, finalize, and the NVENC
 * availability probe.  The base owns those once; subclasses keep their
 * open() orchestration (codec choice, pixel format, codec options) and
 * any pre-encode pixel transform (e.g. HWAlpha's packed-alpha stacking).
 *
 * Thread-safe: one instance per thread (same contract as before).
 */

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

// Forward declarations — avoid pulling FFmpeg headers into every TU.
struct AVFormatContext;
struct AVCodecContext;
struct AVFrame;
struct AVPacket;
struct AVStream;
struct SwsContext;
struct AVBufferRef;
struct AVDictionary;

namespace rt {

class MediaFileEncoderBase
{
public:
    MediaFileEncoderBase() = default;
    virtual ~MediaFileEncoderBase();

    MediaFileEncoderBase(const MediaFileEncoderBase&) = delete;
    MediaFileEncoderBase& operator=(const MediaFileEncoderBase&) = delete;

    /// Flush encoder, write trailer, close file.  Virtual so subclasses
    /// can release their own buffers on top (call the base first).
    virtual bool finalize();

    [[nodiscard]] bool isOpen() const noexcept { return m_isOpen; }
    [[nodiscard]] int64_t framesWritten() const noexcept { return m_framesWritten; }
    [[nodiscard]] const std::string& lastError() const noexcept { return m_lastError; }
    /// True when this instance opened NVENC rather than a software encoder.
    [[nodiscard]] bool isHardwareAccelerated() const noexcept { return m_usingNvenc; }

protected:
    /// Short prefix for log/error messages ("ChromaKey", "HWAlpha", ...).
    [[nodiscard]] virtual const char* logName() const noexcept = 0;

#ifdef ROUNDTABLE_HAS_FFMPEG
    /// Probe: open a tiny h264_nvenc CUDA encoder to confirm NVENC works.
    /// `logTag` names the caller in the probe's log lines.
    static bool probeNvencH264(const char* logTag);

    /// avformat_alloc_output_context2 for `formatName` ("mp4"/"mov"/"webm").
    bool createOutputContainer(const std::filesystem::path& path,
                               const char* formatName);

    /// The NVENC H.264 all-intra open block shared by ChromaKey/HWAlpha:
    /// CUDA device + frames ctx, BT.709 tags with the given color range,
    /// gop=2/bf=0 (NVENC rejects gop=1 with bf=0; every frame is still
    /// forced IDR), p4/hq/constqp/qp + profile high + forced-idr.
    /// On success m_codecCtx is open and m_usingNvenc is true; on failure
    /// everything NVENC-related is cleaned up and false is returned (the
    /// caller then tries the software fallback).
    /// `colorRange` is an AVColorRange value (int to keep FFmpeg out of
    /// this header): MPEG for normal video, JPEG for HWAlpha's full-range
    /// alpha-in-luma packing.
    bool tryOpenNvencH264(int encodeW, int encodeH, int fps, int crf,
                          int colorRange);

    /// Software all-intra fallback shared by ChromaKey/HWAlpha: tries
    /// `primaryName` then `secondaryName`, YUV420P, gop=1/bf=0, CRF mode,
    /// half-the-cores threading, preset veryfast (libx264) / fast (libx265).
    bool openSoftwareIntraFallback(const char* primaryName,
                                   const char* secondaryName,
                                   int encodeW, int encodeH, int fps, int crf);

    /// Create the single video stream, copy codec params, open the output
    /// file, and write the container header.  `muxOpts` (optional) is
    /// consumed (freed) like avformat_write_header does.
    bool createStreamOpenFileWriteHeader(const std::filesystem::path& path,
                                         int fps, AVDictionary** muxOpts = nullptr);

    /// Allocate m_frame (pixFmt/encodeW/encodeH) + m_packet.
    /// `pixFmt` is an AVPixelFormat value.
    bool allocFrameAndPacket(int pixFmt, int encodeW, int encodeH);

    /// sws context: RGBA(encodeW×encodeH) → dstPixFmt(encodeW×encodeH).
    bool createSwsFromRgba(int dstPixFmt, int encodeW, int encodeH);

    /// avcodec_send_frame + drain packets into the container.
    bool sendFrameToEncoder(AVFrame* frame);

    /// NVENC path: upload m_frame to a CUDA frame (pict_type I, keyframe)
    /// and send it.  Falls back to the error string on failure.
    bool sendFrameWithCudaUpload();

    /// Drain all pending packets and interleaved-write them to the stream.
    bool receiveAndWritePackets();

    /// Send the EOF frame and drain.
    bool flushEncoder();

    /// Free codec/frame/packet/sws/hw contexts and the format context.
    /// Called by finalize(); also safe mid-open() for error cleanup.
    void releaseAll();
#endif

    AVFormatContext* m_fmtCtx{nullptr};
    AVCodecContext*  m_codecCtx{nullptr};
    AVFrame*         m_frame{nullptr};
    AVPacket*        m_packet{nullptr};
    AVStream*        m_stream{nullptr};
    SwsContext*      m_swsCtx{nullptr};
    AVBufferRef*     m_hwDeviceCtx{nullptr};
    AVBufferRef*     m_hwFramesCtx{nullptr};

    uint32_t    m_width{0};
    uint32_t    m_height{0};
    int64_t     m_framesWritten{0};
    bool        m_isOpen{false};
    bool        m_usingNvenc{false};
    std::string m_lastError;
};

} // namespace rt
