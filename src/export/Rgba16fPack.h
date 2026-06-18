/*
 * Rgba16fPack — pure RGBA16F → 10-bit planar YUV packing (Phase 4.2).
 *
 * The inverse of the GPU decode shaders (shaders/p010_to_bgra.comp,
 * yuva444p12_to_bgra.comp): those expand a 10/12-bit YUV source to non-linear
 * BT.709 R'G'B' in an RGBA16F texture; this packs that same R'G'B' back into
 * the encoder's native 10-bit planar YUV WITHOUT going through the lossy 8-bit
 * BGRA stage.  Used only by the targeted export passthrough (a single
 * full-frame opaque >8-bit clip exported to ProRes / DNxHR), so it lives in the
 * export library next to the encoders.
 *
 * Deliberately FFmpeg-FREE and Vulkan-FREE so it is a pure, unit-testable
 * function (see tests/export/test_rgba16f_pack.cpp), in the same spirit as
 * resolveColorConversion / ConvertDecodedFrame.  The encoder glue
 * (FfmpegEncoderBase::encodeFrame16f) maps the target enum to the AVFrame's
 * AV_PIX_FMT_* and supplies the destination plane pointers; this function never
 * sees a libav type.
 *
 * Colour model: the input is NON-LINEAR BT.709 R'G'B' in [0,1] (the same
 * gamma-encoded primaries the 8-bit compositor and viewport use — NOT
 * scene-linear light), so NO opto-electronic transfer is applied here; the pack
 * is the direct algebraic inverse of the decode shaders.  Output is
 * limited-range ("studio swing") BT.709 10-bit YCbCr.
 */

#pragma once

#include <cstdint>

namespace rt {

/// Encoder target pixel layout.  Mirrors the AV_PIX_FMT_* the ProRes / DNxHR
/// encoders open with, but expressed FFmpeg-free.  P010LE is included for
/// completeness / reuse (it documents the high-bits-vs-low-bits asymmetry);
/// the v1 encoder glue wires only the three planar P10LE targets.
enum class PackTarget : uint8_t {
    P010LE,        ///< 2 planes: Y (W×H) + interleaved CbCr (W/2×H/2). 4:2:0. Value in HIGH 10 bits.
    YUV422P10LE,   ///< 3 planes: Y (W×H) + Cb,Cr each (W/2)×H. 4:2:2. Value in LOW 10 bits.
    YUV444P10LE,   ///< 3 planes: Y,Cb,Cr all W×H. 4:4:4. Value in LOW 10 bits.
    YUVA444P10LE,  ///< 4 planes: Y,Cb,Cr,A all W×H. 4:4:4 + full-range alpha. Value in LOW 10 bits.
};

/// Destination plane pointers + row strides (BYTES — taken straight from
/// AVFrame::data[i] / AVFrame::linesize[i], which may exceed width*2 due to
/// FFmpeg alignment, so ALWAYS index via the stride, never width*2).
struct PackPlanes {
    uint16_t* y{nullptr}; int yStride{0};
    uint16_t* u{nullptr}; int uStride{0};   ///< P010: interleaved CbCr lives here. Planar: Cb.
    uint16_t* v{nullptr}; int vStride{0};   ///< Planar Cr. Unused for P010LE.
    uint16_t* a{nullptr}; int aStride{0};   ///< YUVA444P10LE only; nullptr otherwise.
};

/// Pack a width×height RGBA16F frame (4 IEEE-754 binary16 halfs per pixel,
/// interleaved R,G,B,A) into `dst` for the given `target`.
///   @param srcRgba16f   pointer to the top-left RGBA16F pixel.
///   @param srcStrideBytes row pitch of the source in BYTES (lets the caller
///                         pass a GPU-readback buffer with its own pitch).
/// Chroma for 4:2:0 / 4:2:2 is box-averaged (co-sited top-left), edge-clamped
/// for odd dimensions.  No dither — round-to-nearest only (a 10/12-bit source
/// quantised to 10-bit loses ≤2 bits, below the SDR perceptual threshold, and
/// dither would only add noise the encoder must then spend bitrate on).
void packRgba16fToYuv(const uint16_t* srcRgba16f, int srcStrideBytes,
                      int width, int height,
                      PackTarget target, const PackPlanes& dst) noexcept;

// ── Scalar pieces (exposed for unit tests) ──────────────────────────────────

/// IEEE-754 binary16 (little-endian uint16) → float32.  Subnormals flush to
/// zero (≈0 for [0,1] colour); Inf/NaN clamp to 1.0 so they can never reach the
/// integer quantisers as Inf/NaN (which would be undefined behaviour).
float halfToFloat(uint16_t h) noexcept;

/// BT.709 limited-range 10-bit quantisers.  Y': round(876·Y'+64) → [64,940];
/// chroma: round(896·C+512) → [64,960] (C∈[-0.5,0.5], centre 512); alpha is
/// FULL range: round(1023·a) → [0,1023].  All clamp their input first.
uint16_t quantizeY10(float yLuma)  noexcept;
uint16_t quantizeC10(float chroma) noexcept;
uint16_t quantizeA10(float alpha)  noexcept;

} // namespace rt
