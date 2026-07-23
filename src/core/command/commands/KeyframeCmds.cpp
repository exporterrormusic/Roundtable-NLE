/*
 * KeyframeCmds.cpp — Keyframe command implementations.
 * Step 4: Command System
 */

#include "command/commands/KeyframeCmds.h"

#include <algorithm>

namespace rt {

// ── AddKeyframeCommand ──────────────────────────────────────────────────────

AddKeyframeCommand::AddKeyframeCommand(KeyframeTrack<float>* track, int64_t time,
                                       float value, InterpMode interp)
    : m_track(track)
    , m_time(time)
    , m_value(value)
    , m_interp(interp)
{
}

AddKeyframeCommand::AddKeyframeCommand(
    KeyframeTrack<float>* track, const Keyframe<float>& keyframe)
    : m_track(track)
    , m_time(keyframe.time)
    , m_value(keyframe.value)
    , m_interp(keyframe.interp)
    , m_restoreFull(true)
    , m_newKeyframe(keyframe)
{
}

void AddKeyframeCommand::execute()
{
    // Check if there's an existing keyframe at this time (for undo)
    const auto& kfs = m_track->keyframes();
    auto it = std::find_if(kfs.begin(), kfs.end(),
        [this](const Keyframe<float>& kf) { return kf.time == m_time; });

    if (it != kfs.end())
    {
        m_hadExisting = true;
        m_oldKeyframe = *it;
    }
    else
    {
        m_hadExisting = false;
    }

    if (m_restoreFull)
        m_track->restoreKeyframe(m_newKeyframe);
    else
        m_track->addKeyframe(m_time, m_value, m_interp);
}

void AddKeyframeCommand::undo()
{
    if (m_hadExisting)
    {
        // Restore the full old keyframe (including bezier handles)
        m_track->restoreKeyframe(m_oldKeyframe);
    }
    else
    {
        // Remove the keyframe we added
        m_track->removeKeyframeAtTime(m_time);
    }
}

std::string AddKeyframeCommand::description() const
{
    return "Add Keyframe";
}

// ── RemoveKeyframeCommand ───────────────────────────────────────────────────

RemoveKeyframeCommand::RemoveKeyframeCommand(KeyframeTrack<float>* track, int64_t time)
    : m_track(track)
    , m_time(time)
{
}

void RemoveKeyframeCommand::execute()
{
    // Save the keyframe before removing
    const auto& kfs = m_track->keyframes();
    auto it = std::find_if(kfs.begin(), kfs.end(),
        [this](const Keyframe<float>& kf) { return kf.time == m_time; });

    if (it != kfs.end())
    {
        m_savedKeyframe = *it;
        m_track->removeKeyframeAtTime(m_time);
        m_removed = true;
    }
}

void RemoveKeyframeCommand::undo()
{
    if (m_removed)
    {
        // Restore full keyframe including bezier handles
        m_track->restoreKeyframe(m_savedKeyframe);
    }
}

std::string RemoveKeyframeCommand::description() const
{
    return "Remove Keyframe";
}

// ── SetKeyframeInterpCommand ────────────────────────────────────────────────

SetKeyframeInterpCommand::SetKeyframeInterpCommand(KeyframeTrack<float>* track,
                                                   int64_t time, InterpMode newInterp)
    : m_track(track)
    , m_time(time)
    , m_newInterp(newInterp)
{
}

void SetKeyframeInterpCommand::execute()
{
    if (!m_track) return;
    const auto& kfs = m_track->keyframes();
    auto it = std::find_if(kfs.begin(), kfs.end(),
        [this](const Keyframe<float>& kf) { return kf.time == m_time; });
    if (it == kfs.end()) return;

    m_savedKeyframe = *it;
    m_executed = true;

    // Find this keyframe's mutable index for handle baking + write-back.
    size_t idx = static_cast<size_t>(std::distance(kfs.begin(), it));

    // Bake currently-effective handles when leaving Auto/Continuous for manual
    // Bezier so the visible curve shape is preserved (Premiere behavior).
    const bool wasComputed = (m_savedKeyframe.interp == InterpMode::AutoBezier
                              || m_savedKeyframe.interp == InterpMode::ContinuousBezier
                              || m_savedKeyframe.interp == InterpMode::EaseIn
                              || m_savedKeyframe.interp == InterpMode::EaseOut);
    if (m_newInterp == InterpMode::Bezier && wasComputed)
    {
        // Bake out-side handle from the segment going forward.
        if (idx + 1 < kfs.size()) {
            const auto& kfNext = kfs[idx + 1];
            double dt = static_cast<double>(kfNext.time - it->time);
            double dv = static_cast<double>(kfNext.value - it->value);
            auto h = m_track->effectiveOutHandle(idx, idx + 1, dt, dv);
            m_track->keyframe(idx).bezierOutX = h.x;
            m_track->keyframe(idx).bezierOutY = h.y;
        }
        // Bake in-side handle from the segment coming back.
        if (idx > 0) {
            const auto& kfPrev = kfs[idx - 1];
            double dt = static_cast<double>(it->time - kfPrev.time);
            double dv = static_cast<double>(it->value - kfPrev.value);
            auto h = m_track->effectiveInHandle(idx - 1, idx, dt, dv);
            m_track->keyframe(idx).bezierInX = h.x;
            m_track->keyframe(idx).bezierInY = h.y;
        }
    }

    m_track->keyframe(idx).interp = m_newInterp;
}

void SetKeyframeInterpCommand::undo()
{
    if (!m_track || !m_executed) return;
    m_track->restoreKeyframe(m_savedKeyframe);
}

std::string SetKeyframeInterpCommand::description() const
{
    return "Set Keyframe Interpolation";
}

// ── SetKeyframeSpatialInterpCommand ─────────────────────────────────────────

SetKeyframeSpatialInterpCommand::SetKeyframeSpatialInterpCommand(
    KeyframeTrack<float>* trackX, KeyframeTrack<float>* trackY,
    int64_t time, InterpMode newSpatial)
    : m_trackX(trackX)
    , m_trackY(trackY)
    , m_time(time)
    , m_newSpatial(newSpatial)
{
}

namespace {
// Seed spatial handles in chord-direction at ~1/3 of each adjacent segment.
// This makes a Linear→Bezier flip produce an immediately-visible curve rather
// than the collapsed degenerate case where both handles are at the origin.
inline void seedSpatialHandles(KeyframeTrack<float>& tx, KeyframeTrack<float>& ty, int64_t time)
{
    size_t idx = 0;
    bool found = false;
    for (size_t i = 0; i < tx.keyframeCount(); ++i)
        if (tx.keyframe(i).time == time) { idx = i; found = true; break; }
    if (!found) return;
    // X and Y tracks may not be perfectly aligned (defensive — never index
    // ty out of range).
    if (idx >= ty.keyframeCount()) return;
    if (tx.keyframe(idx).time != ty.keyframe(idx).time) return;
    auto& kx = tx.keyframe(idx);
    auto& ky = ty.keyframe(idx);

    if (idx + 1 < tx.keyframeCount() && idx + 1 < ty.keyframeCount())
    {
        const float dx = tx.keyframe(idx + 1).value - kx.value;
        const float dy = ty.keyframe(idx + 1).value - ky.value;
        kx.spatialOutX = dx / 3.0f;
        ky.spatialOutY = dy / 3.0f;
    }
    if (idx > 0)
    {
        const float dx = kx.value - tx.keyframe(idx - 1).value;
        const float dy = ky.value - ty.keyframe(idx - 1).value;
        kx.spatialInX = -dx / 3.0f;
        ky.spatialInY = -dy / 3.0f;
    }
}
} // namespace

void SetKeyframeSpatialInterpCommand::execute()
{
    if (!m_trackX || !m_trackY) return;

    auto findIdx = [this](KeyframeTrack<float>* t) -> int {
        for (size_t i = 0; i < t->keyframeCount(); ++i)
            if (t->keyframe(i).time == m_time) return static_cast<int>(i);
        return -1;
    };
    const int ix = findIdx(m_trackX);
    const int iy = findIdx(m_trackY);
    if (ix < 0 || iy < 0) return;

    m_savedX = m_trackX->keyframe(static_cast<size_t>(ix));
    m_savedY = m_trackY->keyframe(static_cast<size_t>(iy));
    m_executed = true;

    m_trackX->keyframe(static_cast<size_t>(ix)).spatialInterp = m_newSpatial;
    m_trackY->keyframe(static_cast<size_t>(iy)).spatialInterp = m_newSpatial;

    // Seed handles when promoting from a non-bezier mode to manual Bezier so
    // the path immediately bends (otherwise both handles sit at the keyframe
    // and the curve collapses to the chord). Auto Bezier computes its own
    // handles dynamically; Continuous Bezier inherits the seeded handles too.
    const bool wasLinear   = m_savedX.spatialInterp == InterpMode::Linear;
    const bool wantsBezier = (m_newSpatial == InterpMode::Bezier
                              || m_newSpatial == InterpMode::ContinuousBezier);
    if (wasLinear && wantsBezier)
        seedSpatialHandles(*m_trackX, *m_trackY, m_time);
}

void SetKeyframeSpatialInterpCommand::undo()
{
    if (!m_executed || !m_trackX || !m_trackY) return;
    m_trackX->restoreKeyframe(m_savedX);
    m_trackY->restoreKeyframe(m_savedY);
}

std::string SetKeyframeSpatialInterpCommand::description() const
{
    return "Set Spatial Interpolation";
}

// ── MoveKeyframeCommand ─────────────────────────────────────────────────────

MoveKeyframeCommand::MoveKeyframeCommand(KeyframeTrack<float>* track,
                                         int64_t oldTime, int64_t newTime, float newValue)
    : m_track(track)
    , m_oldTime(oldTime)
    , m_newTime(newTime)
    , m_newValue(newValue)
{
}

void MoveKeyframeCommand::execute()
{
    // Save full old keyframe state (including bezier handles)
    const auto& kfs = m_track->keyframes();
    auto it = std::find_if(kfs.begin(), kfs.end(),
        [this](const Keyframe<float>& kf) { return kf.time == m_oldTime; });

    if (it != kfs.end())
        m_savedKeyframe = *it;

    // Remove at old time, restore the moved full keyframe at the destination.
    // addKeyframe() would recreate only time/value/interp and silently drop
    // temporal and spatial handles on redo.
    Keyframe<float> moved = m_savedKeyframe;
    moved.time = m_newTime;
    moved.value = m_newValue;
    m_track->removeKeyframeAtTime(m_oldTime);
    m_track->restoreKeyframe(moved);
}

void MoveKeyframeCommand::undo()
{
    m_track->removeKeyframeAtTime(m_newTime);
    m_track->restoreKeyframe(m_savedKeyframe);
}

std::string MoveKeyframeCommand::description() const
{
    return "Move Keyframe";
}

bool MoveKeyframeCommand::mergeWith(const Command& next)
{
    auto* moveCmd = dynamic_cast<const MoveKeyframeCommand*>(&next);
    if (!moveCmd || moveCmd->m_track != m_track) return false;

    // Only merge if the next move starts where we ended
    if (moveCmd->m_oldTime != m_newTime) return false;

    m_newTime  = moveCmd->m_newTime;
    m_newValue = moveCmd->m_newValue;
    return true;
}

} // namespace rt
