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

#include <cmath>
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

/// YUV→RGB matrix family (selects the Kr/Kb coefficients).  Mapped from the
/// source's tagged AVColorSpace at open; `Unspecified` falls back to the
/// SD(≤576 lines)→BT.601 / HD→BT.709 height heuristic at convert time.
enum class ColorMatrix : uint8_t { Unspecified, BT601, BT709, BT2020, Other };

/// Luma/chroma sample range.  `Full` = JPEG/PC swing (0–255), `Limited` =
/// studio swing (16–235).  Mapped from AVColorRange (+ yuvj* pixel formats).
enum class ColorRange : uint8_t { Unspecified, Limited, Full };

/// Transfer characteristic (EOTF).  Only the HDR curves are distinguished;
/// every SDR curve (BT.709 / sRGB / gamma) collapses to `Sdr`.
enum class ColorTransfer : uint8_t { Unspecified, Sdr, PQ, HLG };

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
    int         packedTiles{0};         ///< Packed tile count: 0=not packed, 2=legacy RGB+alpha, 4=luma-packed R/G/B/A
    bool        isVFR{false};           ///< True if variable frame rate detected
    int         audioStreamIndex{-1};
    int         videoStreamIndex{-1};

    // ── Display rotation ────────────────────────────────────────────────
    // Clockwise rotation (degrees) the source's container display-matrix
    // asks for at presentation time — 0 / 90 / 180 / 270.  Portrait phone
    // footage is the common case: stored as landscape pixels + a 90°/270°
    // rotate flag.  Read at open from the stream's DISPLAYMATRIX side data
    // (see VideoDecoderInit).  Default 0 = untagged / no rotation, so a file
    // with no rotation tag behaves exactly as before.
    // Consumed by (a) the compositor — Compositor::buildViewportTransform
    // swaps the fit dims and re-orients UVs for 90/180/270 so rotated footage
    // displays upright in preview AND normal export — and (b) the 16F export
    // passthrough, which rejects rotated sources (it packs the raw frame and
    // would ship it sideways; falls back to the compositor path instead).
    int           rotation{0};

    // ── Colour management (Phase 4.1) ───────────────────────────────────
    // Read from the source's codec/container tags at open.  Drive the
    // per-source YUV→RGB conversion (see resolveColorConversion); untagged
    // sources fall back to the SD/HD height heuristic.  Default values mean
    // "untagged" so a file with no colour metadata behaves exactly as it did
    // before this feature (heuristic 709/601, limited range).
    ColorMatrix   colorMatrix{ColorMatrix::Unspecified};
    ColorRange    colorRange{ColorRange::Unspecified};
    ColorTransfer colorTransfer{ColorTransfer::Unspecified};
    int           colorPrimaries{2};    ///< Raw AVColorPrimaries (2 = unspecified); informational / future gamut work.

    // ── Source bit depth (Phase 4.2) ────────────────────────────────────
    // Bits per luma component of the SOURCE pixel format (8 for 8-bit
    // 4:2:0 / NV12, 10 for P010 / HEVC Main10, 12 for ProRes 4444).  Read
    // at open from av_pix_fmt_desc_get(...)->comp[0].depth; defaults to 8 so
    // an untagged / 8-bit source behaves exactly as before.  Drives the 16F
    // GPU convert routing (a source with bitDepth > 8 may produce RGBA16F
    // cache frames instead of the 8-bit BGRA default — see CachedFrame::depth).
    int           bitDepth{8};

    // Timebase for PTS conversion
    int         timebaseNum{1};
    int         timebaseDen{1};

    /// Nominal (visible) content height of a decoded frame.  Packed-alpha
    /// sources stack tiles vertically (2 = RGB+alpha, 4 = luma-packed
    /// R/G/B/A), so the content is decodedH / tiles.  ALL resolution-tier
    /// downscale clamps must use this — the urgent-decode, prefetch and
    /// loop-pre-decode paths previously each had their own copy of the
    /// formula and two of them hardcoded "/2", which would produce
    /// different frame dimensions for the same (handle, frame, tier) the
    /// day a 4-tile source ships.
    /// packedAlpha with an unset tile count is treated as the legacy
    /// 2-tile layout (packedAlpha is defined as "2× height").
    [[nodiscard]] int contentHeight(int decodedH) const noexcept
    {
        if (!packedAlpha || decodedH <= 1) return decodedH;
        return decodedH / (packedTiles < 2 ? 2 : packedTiles);
    }
};

/// The conversion decision derived from a VideoStreamInfo's colour tags
/// (falling back to the SD/HD height heuristic when untagged).  Computed
/// once and shared by the CPU sws_scale path (ConvertDecodedFrame) and the
/// GPU convert gate so a clip's frames never mix colourspaces across the
/// GPU/CPU cache boundary — a mismatch shows as brightness/saturation
/// flicker as the cache churns between the two producers.
struct ColorConversion
{
    ColorMatrix matrix{ColorMatrix::BT709};
    bool        fullRange{false};
    bool        isHdr{false};               ///< PQ/HLG — needs tone-map (4.1c, deferred).
    bool        isGpuShaderDefault{false};  ///< BT.709 + limited + SDR: exactly what the convert shaders hardcode.
};

/// Resolve the effective YUV→RGB conversion for `info`.  Pure; FFmpeg-free,
/// so it is unit-testable and callable from both the core CPU convert path
/// and the GPU gate.  Untagged sources resolve via the broadcast height
/// heuristic (≤576 visible lines → BT.601, otherwise BT.709) at limited
/// range.  See VideoDecoderInit.cpp for where the tags are read.
[[nodiscard]] ColorConversion resolveColorConversion(const VideoStreamInfo& info) noexcept;

/// Snap a display-matrix rotation angle to the nearest quarter turn, expressed
/// as a CLOCKWISE display rotation in {0, 90, 180, 270}.
///
/// FFmpeg's av_display_rotation_get() returns the COUNTER-clockwise angle that
/// would restore the frame to its un-rotated state; the caller negates it to
/// get the clockwise rotation to apply for correct display, then passes it
/// here.  Cameras only ever tag exact quarter turns, so we round to the
/// nearest 90° and fold into [0, 360).  Pure + header-inline so it is
/// unit-testable without FFmpeg (see tests/core/test_color_conversion.cpp).
[[nodiscard]] inline int snapDisplayRotation(double clockwiseDeg) noexcept
{
    if (!std::isfinite(clockwiseDeg)) return 0;
    const long quarters = std::lround(clockwiseDeg / 90.0);
    return static_cast<int>((((quarters % 4) + 4) % 4) * 90);
}

/// Seek mode
enum class SeekMode : uint8_t
{
    Keyframe,   // Seek to nearest keyframe (fast, imprecise)
    Precise,    // Seek to keyframe then decode forward to exact frame (slow, precise)
};

/// High-bit-depth planar layout of a decoded frame (Phase 4.2 export
/// passthrough).  Maps DecodedFrame::rawFormat (an AVPixelFormat) to the two
/// >8-bit layouts the 16F GPU converter understands; everything else (8-bit,
/// or an unsupported HBD format) is None.  Kept here (FFmpeg-free at the call
/// site) so the GPU layer can branch on it without pulling in libav.
enum class HbdPlaneFormat : uint8_t { None, P010, Yuva444p12 };

/// Classify a decoded frame's high-bit-depth planar layout.  P010/P016 →
/// P010 (Y + interleaved UV, 10/16-bit); yuva444p12le → Yuva444p12 (4 planar
/// 12-bit planes Y/U/V/A).  CPU-decoded frames only (rawFormat reflects the
/// software pixel format).
[[nodiscard]] HbdPlaneFormat hbdPlaneFormat(const DecodedFrame& f) noexcept;

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
    void*                m_avioMemory{nullptr}; // MemoryAvioSource for stills; no live file handle
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
