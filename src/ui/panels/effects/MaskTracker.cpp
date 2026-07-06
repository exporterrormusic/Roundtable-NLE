/*
 * MaskTracker.cpp — translation mask tracking via NCC template matching.
 */

#include "panels/effects/MaskTracker.h"

#include "decode/VideoDecoder.h"
#include "Compositor.h"     // src/gpu — buildViewportTransform
#include "Constants.h"

#include <QApplication>
#include <QProgressDialog>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <vector>

#include <spdlog/spdlog.h>

namespace rt {
namespace {

/// Luma value at (x, y) of a decoded frame (0 when out of bounds).
inline float lumaAt(const DecodedFrame& f, int x, int y, int contentH)
{
    if (x < 0 || y < 0 || x >= static_cast<int>(f.width) || y >= contentH)
        return 0.0f;
    switch (f.format) {
    case PixelFormat::NV12:
    case PixelFormat::YUV420P:
        return static_cast<float>(f.data[0][y * f.linesize[0] + x]);
    case PixelFormat::BGRA:
    case PixelFormat::RGBA: {
        const uint8_t* p = f.data[0] + y * f.linesize[0] + x * 4;
        // Order-agnostic approximation: G dominates either way.
        return 0.25f * p[0] + 0.5f * p[1] + 0.25f * p[2];
    }
    default:
        return 0.0f;
    }
}

struct Patch
{
    int halfW{0}, halfH{0};
    int stride{1};                 ///< sample step (bounds cost on big masks)
    std::vector<float> samples;    ///< luma samples, row-major over the grid
    float mean{0.0f};

    void extract(const DecodedFrame& f, int cx, int cy, int contentH)
    {
        samples.clear();
        double sum = 0.0;
        for (int dy = -halfH; dy <= halfH; dy += stride)
            for (int dx = -halfW; dx <= halfW; dx += stride) {
                float v = lumaAt(f, cx + dx, cy + dy, contentH);
                samples.push_back(v);
                sum += v;
            }
        mean = samples.empty() ? 0.0f
                               : static_cast<float>(sum / samples.size());
    }
};

/// Zero-mean NCC of the template against the frame at candidate center.
float nccScore(const Patch& tpl, const DecodedFrame& f, int cx, int cy,
               int contentH)
{
    double sum = 0.0;
    size_t i = 0;
    std::vector<float> cand(tpl.samples.size());
    for (int dy = -tpl.halfH; dy <= tpl.halfH; dy += tpl.stride)
        for (int dx = -tpl.halfW; dx <= tpl.halfW; dx += tpl.stride) {
            float v = lumaAt(f, cx + dx, cy + dy, contentH);
            cand[i++] = v;
            sum += v;
        }
    const float candMean = cand.empty() ? 0.0f
        : static_cast<float>(sum / cand.size());

    double num = 0.0, dT = 0.0, dC = 0.0;
    for (size_t k = 0; k < cand.size(); ++k) {
        const float a = tpl.samples[k] - tpl.mean;
        const float b = cand[k] - candMean;
        num += a * b;
        dT  += a * a;
        dC  += b * b;
    }
    const double den = std::sqrt(dT * dC);
    return den > 1e-6 ? static_cast<float>(num / den) : -1.0f;
}

/// Normalized bounding box (center + half extents) of a mask geometry.
void geometryBounds(const OpacityMask& mask, const MaskGeometry& g,
                    float& cx, float& cy, float& hw, float& hh)
{
    if (mask.shape == MaskShape::FreeDrawBezier && !g.vertices.empty()) {
        float minX = 1e9f, minY = 1e9f, maxX = -1e9f, maxY = -1e9f;
        for (const auto& v : g.vertices) {
            minX = std::min(minX, v.x); maxX = std::max(maxX, v.x);
            minY = std::min(minY, v.y); maxY = std::max(maxY, v.y);
        }
        cx = (minX + maxX) * 0.5f; cy = (minY + maxY) * 0.5f;
        hw = (maxX - minX) * 0.5f; hh = (maxY - minY) * 0.5f;
    } else {
        cx = g.centerX; cy = g.centerY;
        hw = g.width * 0.5f; hh = g.height * 0.5f;
    }
}

/// Translate a geometry by a normalized delta.
void translateGeometry(MaskGeometry& g, float dx, float dy)
{
    g.centerX += dx;
    g.centerY += dy;
    for (auto& v : g.vertices) { v.x += dx; v.y += dy; }
}

} // namespace

namespace MaskTracker {

int track(const MaskTrackParams& p, OpacityMask& mask, QWidget* parent)
{
    VideoDecoder dec;
    if (!dec.open(p.mediaPath, /*forceSoftware=*/true, /*maxThreads=*/0,
                  /*sliceOnlyThreading=*/true))
    {
        spdlog::warn("[MASK-TRACK] cannot open '{}': {}",
                     p.mediaPath.string(), dec.lastError());
        return 0;
    }
    const auto& info = dec.info();
    if (info.width == 0 || info.height == 0 || info.fps <= 0.0) return 0;

    const int contentH = info.contentHeight(static_cast<int>(info.height));
    const uint32_t srcW = info.width;
    const uint32_t srcH = static_cast<uint32_t>(contentH);

    // frame-norm → source-px affine, identical to the composite mapping
    // (output UV → layer UV, scaled to source pixels).
    const glm::mat4 t = Compositor::buildViewportTransform(
        srcW, srcH, p.outW, p.outH,
        p.posX, p.posY, p.scaleX, p.scaleY, p.rotation,
        /*containFit=*/false, p.anchorX, p.anchorY, info.rotation);
    const float m0 = t[0][0] * srcW, m1 = t[1][0] * srcW, m2 = t[3][0] * srcW;
    const float m3 = t[0][1] * srcH, m4 = t[1][1] * srcH, m5 = t[3][1] * srcH;
    const float det = m0 * m4 - m1 * m3;
    if (std::abs(det) < 1e-9f) return 0;

    auto toSrc = [&](float u, float v, float& sx, float& sy) {
        sx = m0 * u + m1 * v + m2;
        sy = m3 * u + m4 * v + m5;
    };
    auto deltaToNorm = [&](float dsx, float dsy, float& du, float& dv) {
        du = ( m4 * dsx - m1 * dsy) / det;
        dv = (-m3 * dsx + m0 * dsy) / det;
    };

    // Local tick ↔ source frame mapping.
    const double speed = (p.speed > 1e-6) ? p.speed : 1.0;
    auto frameForTick = [&](int64_t localTick) -> int64_t {
        const double srcSeconds = ticksToSeconds(
            p.sourceInTicks + static_cast<int64_t>(localTick * speed));
        return dec.secondsToFrame(srcSeconds);
    };
    auto tickForFrame = [&](int64_t frame) -> int64_t {
        const double srcSeconds = dec.frameToSeconds(frame);
        return static_cast<int64_t>(
            (secondsToTicks(srcSeconds) - p.sourceInTicks) / speed);
    };

    const int64_t startFrame = frameForTick(p.startLocalTick);
    const int64_t endFrame = p.forward
        ? frameForTick(p.clipDurationTicks)      // clip out point
        : frameForTick(0);                       // clip in point
    const int64_t totalSteps =
        std::max<int64_t>(0, p.forward ? endFrame - startFrame
                                       : startFrame - endFrame);
    if (totalSteps <= 0) return 0;

    // Seed the template from the start frame.
    DecodedFrame frame{};
    if (!dec.seekToFrame(startFrame, SeekMode::Precise) ||
        !dec.decodeNext(frame))
    {
        spdlog::warn("[MASK-TRACK] cannot decode start frame {}", startFrame);
        return 0;
    }

    MaskGeometry geo = mask.geometryAt(p.startLocalTick);
    float ncx, ncy, nhw, nhh;
    geometryBounds(mask, geo, ncx, ncy, nhw, nhh);

    float scx, scy;
    toSrc(ncx, ncy, scx, scy);
    // Half extents in source px (linear part magnitudes per axis)
    const float axisU = std::hypot(m0, m3);
    const float axisV = std::hypot(m1, m4);
    Patch tpl;
    tpl.halfW = std::clamp(static_cast<int>(nhw * axisU), 8, 48);
    tpl.halfH = std::clamp(static_cast<int>(nhh * axisV), 8, 48);
    tpl.stride = (tpl.halfW * tpl.halfH > 1600) ? 2 : 1;
    int curX = static_cast<int>(std::lround(scx));
    int curY = static_cast<int>(std::lround(scy));
    tpl.extract(frame, curX, curY, contentH);

    constexpr int kSearchRadius = 32;

    QProgressDialog progress(
        p.forward ? QObject::tr("Tracking mask forward…")
                  : QObject::tr("Tracking mask backward…"),
        QObject::tr("Cancel"), 0, static_cast<int>(totalSteps), parent);
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(300);

    // The Mask Path stopwatch turns on; seed a key at the start position.
    mask.pathAnimated = true;
    mask.addPathKey(p.startLocalTick, geo);

    int keysWritten = 1;
    const int64_t step = p.forward ? 1 : -1;
    for (int64_t f = startFrame + step;
         p.forward ? (f <= endFrame) : (f >= endFrame); f += step)
    {
        if (progress.wasCanceled()) break;

        if (p.forward) {
            if (!dec.decodeNext(frame)) break;
            // Sequential decode can lag the target on VFR content — skip
            // ahead until we reach the requested frame.
            while (frame.frameIndex < f && dec.decodeNext(frame)) {}
        } else {
            if (!dec.seekToFrame(f, SeekMode::Precise) ||
                !dec.decodeNext(frame))
                break;
        }

        // Search the neighborhood for the best template match.
        float bestScore = -2.0f;
        int bestDx = 0, bestDy = 0;
        for (int dy = -kSearchRadius; dy <= kSearchRadius; dy += 2) {
            for (int dx = -kSearchRadius; dx <= kSearchRadius; dx += 2) {
                float s = nccScore(tpl, frame, curX + dx, curY + dy, contentH);
                if (s > bestScore) { bestScore = s; bestDx = dx; bestDy = dy; }
            }
        }
        // Refine ±1 around the coarse best (search stepped by 2).
        for (int dy = bestDy - 1; dy <= bestDy + 1; ++dy) {
            for (int dx = bestDx - 1; dx <= bestDx + 1; ++dx) {
                float s = nccScore(tpl, frame, curX + dx, curY + dy, contentH);
                if (s > bestScore) { bestScore = s; bestDx = dx; bestDy = dy; }
            }
        }

        if (bestScore < 0.2f) {
            // Lost the target (occlusion / cut) — stop like Premiere does.
            spdlog::info("[MASK-TRACK] target lost at frame {} (score {:.2f})",
                         f, bestScore);
            break;
        }

        curX += bestDx;
        curY += bestDy;

        float du, dv;
        deltaToNorm(static_cast<float>(bestDx), static_cast<float>(bestDy),
                    du, dv);
        translateGeometry(geo, du, dv);
        mask.addPathKey(tickForFrame(f), geo);
        ++keysWritten;

        // Adaptive template: re-sample at the new position so gradual
        // appearance changes don't break the match.
        tpl.extract(frame, curX, curY, contentH);

        progress.setValue(static_cast<int>(std::llabs(f - startFrame)));
        QApplication::processEvents();
    }
    progress.setValue(static_cast<int>(totalSteps));

    return keysWritten;
}

} // namespace MaskTracker
} // namespace rt
