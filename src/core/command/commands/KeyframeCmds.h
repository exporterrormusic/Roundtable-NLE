/*
 * KeyframeCmds — undo/redo commands for keyframe operations.
 *
 * These work on KeyframeTrack<float> since all animated properties in the
 * NLE use float tracks (opacity, position, scale, rotation, volume, etc.).
 */

#pragma once

#include <cstdint>
#include <string>

#include "command/Command.h"
#include "command/commands/ClipCommands.h"    // CommandTypeId
#include "timeline/Keyframe.h"
#include "timeline/KeyframeTrack.h"

namespace rt {

// ─────────────────────────────────────────────────────────────────────────────
// AddKeyframeCommand — adds or replaces a keyframe on a float track
// ─────────────────────────────────────────────────────────────────────────────
class AddKeyframeCommand : public Command
{
public:
    AddKeyframeCommand(KeyframeTrack<float>* track, int64_t time, float value,
                       InterpMode interp = InterpMode::Linear);
    /// Add or replace a complete keyframe, preserving temporal/spatial
    /// handles. Used by clipboard and other state-restoration paths.
    AddKeyframeCommand(KeyframeTrack<float>* track,
                       const Keyframe<float>& keyframe);

    void execute() override;
    void undo() override;
    [[nodiscard]] std::string description() const override;
    [[nodiscard]] int typeId() const override { return static_cast<int>(CommandTypeId::AddKeyframe); }

private:
    KeyframeTrack<float>* m_track;
    int64_t               m_time;
    float                 m_value;
    InterpMode            m_interp;
    bool                  m_restoreFull{false};
    Keyframe<float>       m_newKeyframe;

    bool                  m_hadExisting{false};
    Keyframe<float>       m_oldKeyframe; // Saved if we replaced an existing one
};

// ─────────────────────────────────────────────────────────────────────────────
// RemoveKeyframeCommand — removes a keyframe at a specific time
// ─────────────────────────────────────────────────────────────────────────────
class RemoveKeyframeCommand : public Command
{
public:
    RemoveKeyframeCommand(KeyframeTrack<float>* track, int64_t time);

    void execute() override;
    void undo() override;
    [[nodiscard]] std::string description() const override;
    [[nodiscard]] int typeId() const override { return static_cast<int>(CommandTypeId::RemoveKeyframe); }

private:
    KeyframeTrack<float>* m_track;
    int64_t               m_time;
    bool                  m_removed{false};
    Keyframe<float>       m_savedKeyframe;
};

// ─────────────────────────────────────────────────────────────────────────────
// SetKeyframeInterpCommand — change a keyframe's interpolation mode
//
// Used by the right-click "Temporal Interpolation" menu. Stores the full old
// keyframe (interp + bezier handles) so undo restores everything, and on
// execute optionally bakes the previously-effective handles into the stored
// bezierIn/bezierOut so a switch from Auto/Continuous → manual Bezier carries
// the visible curve shape forward (Premiere behavior).
// ─────────────────────────────────────────────────────────────────────────────
class SetKeyframeInterpCommand : public Command
{
public:
    SetKeyframeInterpCommand(KeyframeTrack<float>* track, int64_t time, InterpMode newInterp);

    void execute() override;
    void undo() override;
    [[nodiscard]] std::string description() const override;
    [[nodiscard]] int typeId() const override { return static_cast<int>(CommandTypeId::SetKeyframeInterp); }

private:
    KeyframeTrack<float>* m_track;
    int64_t               m_time;
    InterpMode            m_newInterp;
    Keyframe<float>       m_savedKeyframe;
    bool                  m_executed{false};
};

// ─────────────────────────────────────────────────────────────────────────────
// SetKeyframeSpatialInterpCommand — change a Position keyframe's *spatial*
// interpolation (motion-path shape) on both the X and Y tracks atomically.
// Switching from Linear to Bezier seeds handles in chord direction (1/3 of
// each adjacent segment) so a sensible curve is visible immediately.
// ─────────────────────────────────────────────────────────────────────────────
class SetKeyframeSpatialInterpCommand : public Command
{
public:
    SetKeyframeSpatialInterpCommand(KeyframeTrack<float>* trackX,
                                    KeyframeTrack<float>* trackY,
                                    int64_t time, InterpMode newSpatial);

    void execute() override;
    void undo() override;
    [[nodiscard]] std::string description() const override;
    [[nodiscard]] int typeId() const override { return static_cast<int>(CommandTypeId::SetKeyframeInterp); }

private:
    KeyframeTrack<float>* m_trackX;
    KeyframeTrack<float>* m_trackY;
    int64_t               m_time;
    InterpMode            m_newSpatial;
    Keyframe<float>       m_savedX;
    Keyframe<float>       m_savedY;
    bool                  m_executed{false};
};

// ─────────────────────────────────────────────────────────────────────────────
// MoveKeyframeCommand — moves a keyframe to a new time and/or value
// ─────────────────────────────────────────────────────────────────────────────
class MoveKeyframeCommand : public Command
{
public:
    MoveKeyframeCommand(KeyframeTrack<float>* track,
                        int64_t oldTime, int64_t newTime, float newValue);

    void execute() override;
    void undo() override;
    [[nodiscard]] std::string description() const override;
    [[nodiscard]] int typeId() const override { return static_cast<int>(CommandTypeId::MoveKeyframe); }
    bool mergeWith(const Command& next) override;

private:
    KeyframeTrack<float>* m_track;
    int64_t               m_oldTime;
    int64_t               m_newTime;
    float                 m_newValue;
    Keyframe<float>       m_savedKeyframe; // Full keyframe at m_oldTime (saved on first execute)
};

} // namespace rt
