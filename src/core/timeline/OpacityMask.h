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
#include <cstdint>
#include <string>
#include <vector>

#include "timeline/KeyframeTrack.h"

namespace rt {

/// Mask shape type
enum class MaskShape : uint8_t
{
    Ellipse   = 0,
    Rectangle = 1,
    FreeDrawBezier = 2
};

/// A single bezier control point for free-draw masks.
struct MaskVertex
{
    float x{0.0f};      ///< Position X (normalized 0–1 of frame)
    float y{0.0f};      ///< Position Y (normalized 0–1 of frame)
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
    /// Center position (normalized 0–1 of frame). Default = center.
    float centerX{0.5f};
    float centerY{0.5f};

    /// Size (normalized 0–1 of frame).
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
    MaskGeometry geometry;
    float        feather{0.0f};     ///< pixels (output-frame pixels)
    float        expansion{0.0f};   ///< pixels, may be negative
    float        maskOpacity{1.0f}; ///< 0–1
    bool         inverted{false};
};

/// A mask on a clip (opacity mask) or on an effect (effect mask).
struct OpacityMask
{
    MaskShape shape{MaskShape::Ellipse};

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
