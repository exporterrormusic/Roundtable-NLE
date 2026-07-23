/*
 * OpacityMask — Premiere Pro–style mask data model.
 *
 * Masks live in two places, exactly like Premiere Pro:
 *   - On the clip (Clip::masks())      → gate the clip's OPACITY at composite.
 *   - On an effect (Effect::masks())   → limit WHERE that effect applies.
 *
 * Each mask is an ellipse, rectangle, or free-draw bezier shape. All of
 * Mask Path (the geometry), Mask Feather, Mask Opacity and Mask Expansion
 * are keyframeable (Premiere model): the scalars are KeyframeTrack<float>,
 * and the geometry is a list of whole-path keyframes interpolated linearly.
 *
 * Threading: the UI mutates OpacityMask on the main thread. The render
 * side never reads OpacityMask directly — the producer evaluates each mask
 * into a MaskRenderState snapshot (evaluateMaskStates) that travels with
 * the layer/effect snapshot to the render thread.
 */

#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "timeline/KeyframeTrack.h"

namespace rt {

inline std::atomic<uint64_t>& opacityMaskIdCounter()
{
    static std::atomic<uint64_t> counter{1};
    return counter;
}

inline uint64_t allocateOpacityMaskId() noexcept
{
    return opacityMaskIdCounter().fetch_add(1, std::memory_order_relaxed);
}

inline void reserveOpacityMaskId(uint64_t id) noexcept
{
    if (id == 0) return;
    auto& counter = opacityMaskIdCounter();
    uint64_t expected = counter.load(std::memory_order_relaxed);
    while (expected <= id && !counter.compare_exchange_weak(
               expected, id + 1, std::memory_order_relaxed)) {}
}

/// Mask shape type
enum class MaskShape : uint8_t
{
    Ellipse   = 0,
    Rectangle = 1,
    FreeDrawBezier = 2
};

/// Geometry coordinate system. Projects written before v33 stored mask
/// geometry in normalized sequence-frame space; new masks are source-local so
/// they naturally inherit the owning clip's transform.
enum class MaskCoordinateSpace : uint8_t
{
    SourceLocal = 0,
    LegacySequenceFrame = 1
};

/// A single bezier control point for free-draw masks.
struct MaskVertex
{
    float x{0.0f};      ///< Position X (normalized 0–1 of the clip source)
    float y{0.0f};      ///< Position Y (normalized 0–1 of the clip source)
    float inTanX{0.0f}; ///< Incoming tangent handle X
    float inTanY{0.0f}; ///< Incoming tangent handle Y
    float outTanX{0.0f};///< Outgoing tangent handle X
    float outTanY{0.0f};///< Outgoing tangent handle Y
};

/// The animatable geometry of a mask — Premiere's "Mask Path" value.
/// For Ellipse/Rectangle the parametric fields are used; for
/// FreeDrawBezier the vertex list is used.
struct MaskGeometry
{
    /// Center position (normalized 0–1 of the clip source). Default = center.
    float centerX{0.5f};
    float centerY{0.5f};

    /// Size (normalized 0–1 of the clip source).
    float width{0.5f};
    float height{0.5f};

    /// Rotation in degrees.
    float rotation{0.0f};

    /// Bezier vertices for FreeDrawBezier shape.
    std::vector<MaskVertex> vertices;
};

/// One "Mask Path" keyframe: full geometry at a time (clip-local ticks).
struct MaskPathKeyframe
{
    int64_t      time{0};
    MaskGeometry geometry;
};

/// Render-ready snapshot of one mask at one point in time. Built on the
/// producer thread by evaluateMaskStates(); consumed by rasterizeMasks()
/// on the render thread. Plain data — safe to copy across threads.
struct MaskRenderState
{
    MaskShape    shape{MaskShape::Ellipse};
    MaskCoordinateSpace coordinateSpace{MaskCoordinateSpace::SourceLocal};
    MaskGeometry geometry;
    float        feather{0.0f};     ///< pixels (output-frame pixels)
    float        expansion{0.0f};   ///< pixels, may be negative
    float        maskOpacity{1.0f}; ///< 0–1
    bool         inverted{false};
};

/// A mask on a clip (opacity mask) or on an effect (effect mask).
struct OpacityMask
{
    uint64_t maskId{allocateOpacityMaskId()};
    MaskShape shape{MaskShape::Ellipse};
    MaskCoordinateSpace coordinateSpace{MaskCoordinateSpace::SourceLocal};

    /// When true, the mask is inverted (transparent inside, opaque outside).
    bool inverted{false};

    /// Display name for the UI (e.g. "Mask 1", "Mask 2").
    std::string name;

    /// Static geometry — used whenever the path is not animated.
    MaskGeometry base;

    /// Mask Path stopwatch state. When true, edits write path keyframes at
    /// the current time instead of updating `base` (Premiere stopwatch model).
    bool pathAnimated{false};

    /// Mask Path keyframes, sorted by time. Only used when pathAnimated.
    std::vector<MaskPathKeyframe> pathKeys;

    /// Keyframeable scalars (Premiere: Mask Feather / Opacity / Expansion).
    KeyframeTrack<float> feather{0.0f};       ///< pixels (blurred edge)
    KeyframeTrack<float> maskOpacity{1.0f};   ///< 0–1
    KeyframeTrack<float> expansion{0.0f};     ///< pixels; + grows, − shrinks

    // ── Mask Path evaluation ────────────────────────────────────────────

    /// Geometry at a clip-local time, linearly interpolating path keyframes.
    /// Vertex lists interpolate pairwise when counts match; otherwise the
    /// earlier keyframe's path holds until the next keyframe.
    [[nodiscard]] MaskGeometry geometryAt(int64_t time) const
    {
        if (!pathAnimated || pathKeys.empty())
            return base;
        if (pathKeys.size() == 1 || time <= pathKeys.front().time)
            return pathKeys.front().geometry;
        if (time >= pathKeys.back().time)
            return pathKeys.back().geometry;

        auto it = std::lower_bound(
            pathKeys.begin(), pathKeys.end(), time,
            [](const MaskPathKeyframe& k, int64_t t) { return k.time < t; });
        const auto& k1 = *it;
        const auto& k0 = *(it - 1);
        if (k1.time == time) return k1.geometry;

        const float t = static_cast<float>(
            static_cast<double>(time - k0.time) /
            static_cast<double>(k1.time - k0.time));
        return lerpGeometry(k0.geometry, k1.geometry, t);
    }

    /// Write geometry with Premiere stopwatch semantics: updates `base` when
    /// the path is static, or writes/updates a keyframe at `time` when the
    /// Mask Path stopwatch is on.
    void writeGeometry(int64_t time, const MaskGeometry& g)
    {
        if (!pathAnimated) {
            base = g;
            return;
        }
        addPathKey(time, g);
    }

    void addPathKey(int64_t time, const MaskGeometry& g)
    {
        auto it = std::lower_bound(
            pathKeys.begin(), pathKeys.end(), time,
            [](const MaskPathKeyframe& k, int64_t t) { return k.time < t; });
        if (it != pathKeys.end() && it->time == time)
            it->geometry = g;
        else
            pathKeys.insert(it, MaskPathKeyframe{time, g});
    }

    void removePathKeyAtTime(int64_t time)
    {
        auto it = std::find_if(pathKeys.begin(), pathKeys.end(),
            [time](const MaskPathKeyframe& k) { return k.time == time; });
        if (it != pathKeys.end())
            pathKeys.erase(it);
    }

    [[nodiscard]] bool hasPathKeyAt(int64_t time) const noexcept
    {
        return std::any_of(pathKeys.begin(), pathKeys.end(),
            [time](const MaskPathKeyframe& k) { return k.time == time; });
    }

    /// Previous/next path-keyframe time relative to `time`, or `time` itself
    /// when none exists in that direction (callers check the return).
    [[nodiscard]] int64_t prevPathKeyTime(int64_t time) const noexcept
    {
        int64_t best = time;
        for (const auto& k : pathKeys)
            if (k.time < time && (best == time || k.time > best)) best = k.time;
        return best;
    }
    [[nodiscard]] int64_t nextPathKeyTime(int64_t time) const noexcept
    {
        int64_t best = time;
        for (const auto& k : pathKeys)
            if (k.time > time && (best == time || k.time < best)) best = k.time;
        return best;
    }

    /// Evaluate everything into a render snapshot at a clip-local time.
    [[nodiscard]] MaskRenderState evalRenderState(int64_t time) const
    {
        MaskRenderState s;
        s.shape       = shape;
        s.coordinateSpace = coordinateSpace;
        s.geometry    = geometryAt(time);
        s.feather     = feather.evaluate(time);
        s.expansion   = expansion.evaluate(time);
        s.maskOpacity = maskOpacity.evaluate(time);
        s.inverted    = inverted;
        return s;
    }

private:
    static MaskGeometry lerpGeometry(const MaskGeometry& a,
                                     const MaskGeometry& b, float t)
    {
        MaskGeometry g;
        g.centerX  = a.centerX  + (b.centerX  - a.centerX)  * t;
        g.centerY  = a.centerY  + (b.centerY  - a.centerY)  * t;
        g.width    = a.width    + (b.width    - a.width)    * t;
        g.height   = a.height   + (b.height   - a.height)   * t;
        g.rotation = a.rotation + (b.rotation - a.rotation) * t;
        if (a.vertices.size() == b.vertices.size()) {
            g.vertices.resize(a.vertices.size());
            for (size_t i = 0; i < a.vertices.size(); ++i) {
                const auto& va = a.vertices[i];
                const auto& vb = b.vertices[i];
                g.vertices[i].x       = va.x       + (vb.x       - va.x)       * t;
                g.vertices[i].y       = va.y       + (vb.y       - va.y)       * t;
                g.vertices[i].inTanX  = va.inTanX  + (vb.inTanX  - va.inTanX)  * t;
                g.vertices[i].inTanY  = va.inTanY  + (vb.inTanY  - va.inTanY)  * t;
                g.vertices[i].outTanX = va.outTanX + (vb.outTanX - va.outTanX) * t;
                g.vertices[i].outTanY = va.outTanY + (vb.outTanY - va.outTanY) * t;
            }
        } else {
            // Vertex-count mismatch — hold the earlier path.
            g.vertices = a.vertices;
        }
        return g;
    }
};

/// Parameters needed to upgrade a sequence-frame mask written by an older
/// project into the owning clip's native source space. Position and anchor use
/// the clip model's reference-1920 units; sequence/source dimensions are the
/// real dimensions used by the compositor.
struct LegacyMaskMigrationTransform
{
    uint32_t sequenceWidth{0};
    uint32_t sequenceHeight{0};
    uint32_t sourceWidth{0};
    uint32_t sourceHeight{0};
    float positionX{0.0f};
    float positionY{0.0f};
    float scaleX{1.0f};
    float scaleY{1.0f};
    float rotation{0.0f};
    float anchorX{0.0f};
    float anchorY{0.0f};
    bool containFit{false};
    int sourceRotation{0};
};

namespace mask_migration_detail {

struct Point
{
    float x{0.0f};
    float y{0.0f};
};

/// Mirrors Compositor::buildViewportTransform without introducing a core ->
/// GPU dependency: sequence UV to the native source UV sampled by the shader.
inline Point sequenceToSource(const LegacyMaskMigrationTransform& t,
                              float u, float v) noexcept
{
    constexpr float kRefW = 1920.0f;
    constexpr float kRefH = 1080.0f;
    constexpr float kPi = 3.14159265358979323846f;

    const float outW = static_cast<float>(t.sequenceWidth);
    const float outH = static_cast<float>(t.sequenceHeight);
    const int rotation = ((t.sourceRotation % 360) + 360) % 360;
    const bool quarterTurn = rotation == 90 || rotation == 270;
    const float displayW = static_cast<float>(
        quarterTurn ? t.sourceHeight : t.sourceWidth);
    const float displayH = static_cast<float>(
        quarterTurn ? t.sourceWidth : t.sourceHeight);
    const float fit = t.containFit
        ? std::min(outW / displayW, outH / displayH)
        : std::max(outW / displayW, outH / displayH);
    const float fittedW = displayW * fit;
    const float fittedH = displayH * fit;
    const float offX = (outW - fittedW) * 0.5f;
    const float offY = (outH - fittedH) * 0.5f;
    const float cx = outW * 0.5f;
    const float cy = outH * 0.5f;
    const float posX = t.positionX * (outW / kRefW);
    const float posY = t.positionY * (outH / kRefH);
    const float anchorX = t.anchorX * (outW / kRefW);
    const float anchorY = t.anchorY * (outH / kRefH);

    const float dx = u * outW - cx - posX - anchorX;
    const float dy = v * outH - cy - posY - anchorY;
    const float radians = t.rotation * kPi / 180.0f;
    const float c = std::cos(radians);
    const float s = std::sin(radians);
    const float unrotatedX = c * dx + s * dy;
    const float unrotatedY = -s * dx + c * dy;
    const float displayU =
        (unrotatedX / t.scaleX + cx + anchorX - offX) / fittedW;
    const float displayV =
        (unrotatedY / t.scaleY + cy + anchorY - offY) / fittedH;

    switch (rotation) {
    case 90:  return {displayV, 1.0f - displayU};
    case 180: return {1.0f - displayU, 1.0f - displayV};
    case 270: return {1.0f - displayV, displayU};
    default:  return {displayU, displayV};
    }
}

inline Point rotatedLegacyPoint(const LegacyMaskMigrationTransform& t,
                                const MaskGeometry& g,
                                float dx, float dy) noexcept
{
    constexpr float kPi = 3.14159265358979323846f;
    const float radians = g.rotation * kPi / 180.0f;
    const float c = std::cos(radians);
    const float s = std::sin(radians);
    const float px = dx * static_cast<float>(t.sequenceWidth);
    const float py = dy * static_cast<float>(t.sequenceHeight);
    return {
        g.centerX + (px * c - py * s) /
            static_cast<float>(t.sequenceWidth),
        g.centerY + (px * s + py * c) /
            static_cast<float>(t.sequenceHeight)
    };
}

inline MaskVertex transformVertex(const LegacyMaskMigrationTransform& t,
                                  const MaskVertex& in) noexcept
{
    const Point anchor = sequenceToSource(t, in.x, in.y);
    const Point inControl = sequenceToSource(
        t, in.x + in.inTanX, in.y + in.inTanY);
    const Point outControl = sequenceToSource(
        t, in.x + in.outTanX, in.y + in.outTanY);
    return {anchor.x, anchor.y,
            inControl.x - anchor.x, inControl.y - anchor.y,
            outControl.x - anchor.x, outControl.y - anchor.y};
}

inline MaskGeometry transformGeometry(const LegacyMaskMigrationTransform& t,
                                      MaskShape shape,
                                      const MaskGeometry& in)
{
    MaskGeometry out = in;
    const Point center = sequenceToSource(t, in.centerX, in.centerY);
    out.centerX = center.x;
    out.centerY = center.y;

    if (shape == MaskShape::FreeDrawBezier) {
        out.vertices.clear();
        out.vertices.reserve(in.vertices.size());
        for (const MaskVertex& vertex : in.vertices)
            out.vertices.push_back(transformVertex(t, vertex));
        return out;
    }

    const float hw = in.width * 0.5f;
    const float hh = in.height * 0.5f;
    out.vertices.clear();
    if (shape == MaskShape::Rectangle) {
        const Point local[4] = {
            rotatedLegacyPoint(t, in, -hw, -hh),
            rotatedLegacyPoint(t, in,  hw, -hh),
            rotatedLegacyPoint(t, in,  hw,  hh),
            rotatedLegacyPoint(t, in, -hw,  hh)
        };
        out.vertices.reserve(4);
        for (const Point& p : local) {
            const Point mapped = sequenceToSource(t, p.x, p.y);
            out.vertices.push_back({mapped.x, mapped.y, 0, 0, 0, 0});
        }
        return out;
    }

    // An affine transform can shear an ellipse, which the parametric
    // center/width/height/rotation representation cannot express. Four cubic
    // segments preserve that affine ellipse (including non-uniform clip scale)
    // and keep every tangent attached to its anchor.
    constexpr float kappa = 0.5522847498307936f;
    const float anchorOffset[4][2] = {
        { hw, 0.0f}, {0.0f, hh}, {-hw, 0.0f}, {0.0f, -hh}
    };
    const float inOffset[4][2] = {
        {0.0f, -kappa * hh}, { kappa * hw, 0.0f},
        {0.0f,  kappa * hh}, {-kappa * hw, 0.0f}
    };
    const float outOffset[4][2] = {
        {0.0f,  kappa * hh}, {-kappa * hw, 0.0f},
        {0.0f, -kappa * hh}, { kappa * hw, 0.0f}
    };
    out.vertices.reserve(4);
    for (int i = 0; i < 4; ++i) {
        const Point a = rotatedLegacyPoint(
            t, in, anchorOffset[i][0], anchorOffset[i][1]);
        const Point ci = rotatedLegacyPoint(
            t, in, anchorOffset[i][0] + inOffset[i][0],
            anchorOffset[i][1] + inOffset[i][1]);
        const Point co = rotatedLegacyPoint(
            t, in, anchorOffset[i][0] + outOffset[i][0],
            anchorOffset[i][1] + outOffset[i][1]);
        const Point ma = sequenceToSource(t, a.x, a.y);
        const Point mi = sequenceToSource(t, ci.x, ci.y);
        const Point mo = sequenceToSource(t, co.x, co.y);
        out.vertices.push_back({ma.x, ma.y,
                                mi.x - ma.x, mi.y - ma.y,
                                mo.x - ma.x, mo.y - ma.y});
    }
    return out;
}

inline bool isUsableTransform(const LegacyMaskMigrationTransform& transform) noexcept
{
    return transform.sequenceWidth != 0 && transform.sequenceHeight != 0 &&
           transform.sourceWidth != 0 && transform.sourceHeight != 0 &&
           std::isfinite(transform.positionX) &&
           std::isfinite(transform.positionY) &&
           std::isfinite(transform.scaleX) &&
           std::isfinite(transform.scaleY) &&
           std::isfinite(transform.rotation) &&
           std::isfinite(transform.anchorX) &&
           std::isfinite(transform.anchorY) &&
           std::abs(transform.scaleX) >= 1.0e-6f &&
           std::abs(transform.scaleY) >= 1.0e-6f;
}

/// Legacy feather/expansion are sequence-pixel distances. Return the average
/// native-source pixel distance represented by one sequence pixel under this
/// inverse clip transform.
inline float distanceScale(const LegacyMaskMigrationTransform& transform) noexcept
{
    if (!isUsableTransform(transform)) return 0.0f;
    const Point origin = sequenceToSource(transform, 0.0f, 0.0f);
    const Point oneX = sequenceToSource(
        transform,
        1.0f / static_cast<float>(transform.sequenceWidth), 0.0f);
    const Point oneY = sequenceToSource(
        transform, 0.0f,
        1.0f / static_cast<float>(transform.sequenceHeight));
    const float sourceW = static_cast<float>(transform.sourceWidth);
    const float sourceH = static_cast<float>(transform.sourceHeight);
    const float pxX = std::hypot((oneX.x - origin.x) * sourceW,
                                 (oneX.y - origin.y) * sourceH);
    const float pxY = std::hypot((oneY.x - origin.x) * sourceW,
                                 (oneY.y - origin.y) * sourceH);
    const float factor = 0.5f * (pxX + pxY);
    return std::isfinite(factor) && factor > 0.0f ? factor : 0.0f;
}

inline void scaleTrack(
    KeyframeTrack<float>& track,
    const LegacyMaskMigrationTransform& baseTransform,
    const std::function<LegacyMaskMigrationTransform(int64_t)>& transformAtTime)
{
    const float baseFactor = distanceScale(baseTransform);
    if (!(baseFactor > 0.0f)) return;
    track.setDefaultValue(track.defaultValue() * baseFactor);
    for (size_t i = 0; i < track.keyframeCount(); ++i) {
        auto& key = track.keyframe(i);
        LegacyMaskMigrationTransform keyTransform = transformAtTime
            ? transformAtTime(key.time) : baseTransform;
        if (!isUsableTransform(keyTransform)) keyTransform = baseTransform;
        const float factor = distanceScale(keyTransform);
        key.value *= factor > 0.0f ? factor : baseFactor;
    }
}

} // namespace mask_migration_detail

/// Upgrade one legacy sequence-frame mask in place. The conversion preserves
/// its appearance at the supplied clip transform, then makes all subsequent
/// clip position/scale/rotation changes carry the mask with the source.
/// Base geometry and every Mask Path keyframe are transformed independently.
[[nodiscard]] inline bool migrateLegacyMaskToSourceLocal(
    OpacityMask& mask, const LegacyMaskMigrationTransform& transform,
    const std::function<LegacyMaskMigrationTransform(int64_t)>&
        transformAtTime = {})
{
    if (mask.coordinateSpace != MaskCoordinateSpace::LegacySequenceFrame)
        return false;
    if (!mask_migration_detail::isUsableTransform(transform))
        return false;

    const MaskShape originalShape = mask.shape;
    mask.base = mask_migration_detail::transformGeometry(
        transform, originalShape, mask.base);
    for (MaskPathKeyframe& key : mask.pathKeys) {
        LegacyMaskMigrationTransform keyTransform = transformAtTime
            ? transformAtTime(key.time) : transform;
        // A scale animation may legitimately pass through zero. There is no
        // finite inverse at that instant, so fall back to the valid base
        // transform instead of writing NaNs into the persisted mask path.
        if (!mask_migration_detail::isUsableTransform(keyTransform))
            keyTransform = transform;
        key.geometry = mask_migration_detail::transformGeometry(
            keyTransform, originalShape, key.geometry);
    }
    if (originalShape != MaskShape::FreeDrawBezier)
        mask.shape = MaskShape::FreeDrawBezier;

    // Scalar keys need the clip transform at THEIR time too. Otherwise a
    // constant 20px legacy feather would visibly grow/shrink when the clip's
    // scale is animated after migration.
    mask_migration_detail::scaleTrack(
        mask.feather, transform, transformAtTime);
    mask_migration_detail::scaleTrack(
        mask.expansion, transform, transformAtTime);
    mask.coordinateSpace = MaskCoordinateSpace::SourceLocal;
    return true;
}

/// Snapshot a whole mask list at a clip-local time (producer thread).
[[nodiscard]] inline std::vector<MaskRenderState>
evaluateMaskStates(const std::vector<OpacityMask>& masks, int64_t time)
{
    std::vector<MaskRenderState> out;
    out.reserve(masks.size());
    for (const auto& m : masks)
        out.push_back(m.evalRenderState(time));
    return out;
}

} // namespace rt
