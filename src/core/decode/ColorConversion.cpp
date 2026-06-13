/*
 * ColorConversion.cpp — Phase 4.1 per-source colour decision.
 *
 * Turns a source's tagged colour metadata (VideoStreamInfo::colorMatrix /
 * colorRange / colorTransfer, populated in VideoDecoderInit) into the single
 * conversion decision shared by:
 *   - the CPU sws_scale path (ConvertDecodedFrame.cpp), which maps `matrix`
 *     to an SWS_CS_* coefficients table and `fullRange` to the swscale src
 *     range, and
 *   - the GPU convert gate (MediaPoolPrefetchConvertGpu.cpp), which only runs
 *     the BT.709-limited-SDR compute shaders when `isGpuShaderDefault` holds
 *     and otherwise defers to the CPU path.
 *
 * Both producers asking the SAME function the SAME question is what keeps a
 * clip's frames from mixing colourspaces across the GPU/CPU cache boundary.
 *
 * Deliberately FFmpeg-free: the AVColorSpace/AVColorRange/AVColorTransfer
 * enums are translated to rt:: enums at read time in VideoDecoderInit, so
 * this stays a pure, unit-testable function (see tests/core/test_color_conversion.cpp).
 */

#include "decode/VideoDecoder.h"

namespace rt {

ColorConversion resolveColorConversion(const VideoStreamInfo& info) noexcept
{
    ColorConversion c;

    // ── Matrix ──────────────────────────────────────────────────────────
    switch (info.colorMatrix) {
        case ColorMatrix::BT601:  c.matrix = ColorMatrix::BT601;  break;
        case ColorMatrix::BT709:  c.matrix = ColorMatrix::BT709;  break;
        case ColorMatrix::BT2020: c.matrix = ColorMatrix::BT2020; break;
        case ColorMatrix::Other:
        case ColorMatrix::Unspecified:
        default: {
            // Untagged source: the long-standing broadcast heuristic — SD
            // content (≤ 576 visible lines) is BT.601, HD and up is BT.709.
            // contentHeight() unpacks the nominal picture height for
            // packed-alpha sources (e.g. Wells' stacked HEVC) so a 1080×3776
            // packed clip is treated as its 1888-line picture, not 3776.
            const int h = info.contentHeight(static_cast<int>(info.height));
            c.matrix = (h > 0 && h <= 576) ? ColorMatrix::BT601 : ColorMatrix::BT709;
            break;
        }
    }

    // ── Range ───────────────────────────────────────────────────────────
    // Only an explicit `Full` tag (JPEG/MJPEG/yuvj*) selects full swing;
    // untagged YUV video is studio swing, which is the safe default.
    c.fullRange = (info.colorRange == ColorRange::Full);

    // ── Transfer (HDR detection) ────────────────────────────────────────
    c.isHdr = (info.colorTransfer == ColorTransfer::PQ ||
               info.colorTransfer == ColorTransfer::HLG);

    // ── GPU-shader default gate ─────────────────────────────────────────
    // The NV12 / YUV420P / P010 / YUVA444P12 compute shaders hardcode the
    // BT.709 matrix, studio-swing (limited→full) range expansion, and an SDR
    // transfer.  The GPU convert path may only run when the source matches
    // all three; anything else takes the CPU sws_scale path, which honours
    // `matrix`/`fullRange` (and, later, will tone-map `isHdr` — 4.1c).
    c.isGpuShaderDefault =
        (c.matrix == ColorMatrix::BT709 && !c.fullRange && !c.isHdr);

    return c;
}

} // namespace rt
