/*
 * VideoDecoder — FFmpeg-based video decoder with NVDEC hardware acceleration.
 *
 * Wraps FFmpeg's demuxer + decoder pipeline. When an NVIDIA GPU is present,
 * uses CUDA hardware acceleration (AV_HWDEVICE_TYPE_CUDA) for zero-CPU decode.
 * Falls back to software decode (libavcodec) transparently.
 *
 * Thread-safe: all public methods are guarded by a mutex.
 */

#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

struct AVFormatContext;
struct AVCodecContext;
struct AVIOContext;
struct AVFrame;
struct AVPacket;
struct AVBufferRef;

namespace rt {

/// Pixel format of decoded frames
enum class PixelFormat : uint8_t
{
    Unknown,
    NV12,       // NVDEC native output (Y + interleaved UV)
    YUV420P,    // Software decode default
    BGRA,       // Converted for display
    RGBA,       // Converted for display
};

/// Information about a decoded video frame
struct DecodedFrame
{
    uint8_t*    data[4]{};      // Plane pointers (Y, U, V, or packed)
    int         linesize[4]{};  // Bytes per row per plane
    uint32_t    width{0};
    uint32_t    height{0};
    PixelFormat format{PixelFormat::Unknown};
    int64_t     pts{0};         // Presentation timestamp (in stream timebase)
    double      timestamp{0.0}; // Presentation timestamp in seconds
    int64_t     frameIndex{0};  // Sequential frame number
    bool        isKeyframe{false};
    bool        isHardware{false}; // True if decoded on GPU (NVDEC)
    int         rawFormat{-1};     // Raw AVPixelFormat value for sws_scale

    // If hardware frame, this points to the device frame (GPU memory)
    AVFrame*    avFrame{nullptr}; // Caller must NOT free — owned by decoder
};

/// Stream information extracted from container
struct VideoStreamInfo
{
    uint32_t    width{0};
    uint32_t    height{0};
    double      fps{0.0};
    double      duration{0.0};      // Seconds
    int64_t     frameCount{0};
    int64_t     bitrate{0};
    std::string codecName;
    std::string containerFormat;
    bool        hasAudio{false};
    bool        hasAlpha{false};        ///< True if video has native alpha (VP9/ProRes)
    bool        packedAlpha{false};     ///< True if packed-alpha layout (top=RGB, bot=alpha, 2× height)
    bool        isVFR{false};           ///< True if variable frame rate detected
    int         audioStreamIndex{-1};
    int         videoStreamIndex{-1};

    // Timebase for PTS conversion
    int         timebaseNum{1};
    int         timebaseDen{1};
};

/// Seek mode
enum class SeekMode : uint8_t
{
    Keyframe,   // Seek to nearest keyframe (fast, imprecise)
    Precise,    // Seek to keyframe then decode forward to exact frame (slow, precise)
};

class VideoDecoder
{
public:
    VideoDecoder();
    ~VideoDecoder();

    // Non-copyable
    VideoDecoder(const VideoDecoder&) = delete;
    VideoDecoder& operator=(const VideoDecoder&) = delete;

    /// Open a video file. Returns true on success.
    /// Automatically tries NVDEC hardware acceleration, falls back to software.
    /// Set forceSoftware=true to skip hardware acceleration entirely (e.g.
    /// for prefetch workers that shouldn't consume NVDEC sessions).
    /// maxThreads limits FFmpeg thread_count (0 = auto/all cores).
    /// sliceOnlyThreading disables FF_THREAD_FRAME (required for
    /// seek-heavy decoders to avoid H.264 reference frame corruption).
    bool open(const std::filesystem::path& path, bool forceSoftware = false,
              int maxThreads = 0, bool sliceOnlyThreading = false);

    /// Close the current file and release all resources.
    void close();

    /// Is a file currently open?
    [[nodiscard]] bool isOpen() const noexcept;

    /// Get stream information.
    [[nodiscard]] const VideoStreamInfo& info() const noexcept;

    /// Seek to a specific time in seconds.
    bool seek(double timeSeconds, SeekMode mode = SeekMode::Precise);

    /// Seek to a specific frame number.
    bool seekToFrame(int64_t frameNumber, SeekMode mode = SeekMode::Precise);

    /// Decode the next frame. Returns true if a frame was decoded.
    /// The DecodedFrame is valid until the next call to decodeNext() or seek().
    bool decodeNext(DecodedFrame& outFrame);

    /// Decode and return a frame at a specific time. Convenience method.
    bool decodeAt(double timeSeconds, DecodedFrame& outFrame);

    /// Is hardware (NVDEC) acceleration active?
    [[nodiscard]] bool isHardwareAccelerated() const noexcept;

    /// Convert a hardware frame to a CPU-accessible frame (NV12 or YUV420P).
    /// Only needed if isHardware is true and you want CPU access.
    bool transferHardwareFrame(const DecodedFrame& hwFrame, DecodedFrame& cpuFrame);

    /// Get the last error message.
    [[nodiscard]] const std::string& lastError() const noexcept;

    /// Convert seconds → frame number using stream FPS.
    [[nodiscard]] int64_t secondsToFrame(double seconds) const noexcept;

    /// Convert frame number → seconds.
    [[nodiscard]] double frameToSeconds(int64_t frame) const noexcept;

    /// Convert PTS → seconds using stream timebase.
    [[nodiscard]] double ptsToSeconds(int64_t pts) const noexcept;

private:
    bool initHardwareDecoder(int deviceType);  // AVHWDeviceType
    bool initSoftwareDecoder(const char* preferredDecoderName = nullptr);
    int m_maxThreads{0};  // 0 = auto
    bool m_sliceOnlyThreading{false};
    bool decodePacket(AVPacket* packet, DecodedFrame& outFrame);
    void fillFrameInfo(DecodedFrame& outFrame, AVFrame* frame);

    AVFormatContext*     m_fmtCtx{nullptr};
    AVCodecContext*      m_codecCtx{nullptr};

    // Custom AVIO so the source file is opened with full share access
    // (FILE_SHARE_READ | _WRITE | _DELETE on Windows via _SH_DENYNO).
    // Without this FFmpeg's default file: protocol denies Explorer's
    // overwrite/delete while the decoder is cached. Premiere-style
    // hot-swap requires the lock be cooperative, not exclusive.
    AVIOContext*         m_avioCtx{nullptr};
    void*                m_avioBuf{nullptr};   // av_malloc'd, owned by m_avioCtx after init
    void*                m_avioFile{nullptr};  // FILE* opened in shared mode
    AVFrame*             m_frame{nullptr};
    AVFrame*             m_hwFrame{nullptr};     // For hw→sw transfer
    AVPacket*            m_packet{nullptr};
    AVBufferRef*         m_hwDeviceCtx{nullptr};
    int                  m_hwPixFmt{-1};         // AV_PIX_FMT_CUDA or AV_PIX_FMT_D3D11

    VideoStreamInfo      m_info;
    bool                 m_isOpen{false};
    bool                 m_hwAccel{false};
    bool                 m_audioOnly{false};
    bool                 m_draining{false};    // True after EOF while flushing B-frames
    int64_t              m_currentFrame{0};
    std::string          m_lastError;
    // Source path remembered for the close+reopen still-image seek path.
    // FFmpeg's image2 demuxer cannot rewind a single-image file: seek then
    // decodeNext returns EOF because the only packet was already consumed
    // by the preceding read. Reopening from scratch gives decodeNext a
    // fresh demuxer that emits the lone packet again.
    std::filesystem::path m_path;
    bool                 m_forceSoftware{false};
    mutable std::mutex   m_mutex;
};

// ── Global user preference for decode mode ───────────────────────────────
// Set from AppPreferencesDialog via VideoDecoder::setForceSoftwareDecode().
// When true, VideoDecoder::open() skips all hardware decoder attempts.
void setForceSoftwareDecode(bool force);
bool forceSoftwareDecode();

// ── Hardware-decoder pre-warm pool (UPGRADE_PLAN item 3) ────────────────
//
// The first NVDEC/CUDA decoder ever opened in a process pays a 100-200 ms
// "cold init" cost — av_hwdevice_ctx_create() loads nvcuda.dll / nvcuvid.dll,
// creates a CUcontext, and probes the NVDEC engine.  Premiere amortises
// this by holding the GPU decoder session alive across opens.
//
// prewarmHardwareDecoders() does the same: at app startup it creates one
// shared AVBufferRef* per device type and caches it.  Subsequent
// VideoDecoder::initHardwareDecoder() calls take an av_buffer_ref of the
// cached handle instead of paying the create cost again.  All decoders
// share the same CUDA context — which is what FFmpeg expects anyway, and
// what makes cross-decoder zero-copy work correctly.
//
// Safe to call multiple times — only the first call does real work.  No-op
// if FFmpeg or CUDA are unavailable.  Call after GpuContext::init() so
// the CUDA runtime is already loaded.
void prewarmHardwareDecoders();

/// Release cached hw device contexts.  Call before FFmpeg / CUDA teardown.
void shutdownHardwareDecoders() noexcept;

} // namespace rt
