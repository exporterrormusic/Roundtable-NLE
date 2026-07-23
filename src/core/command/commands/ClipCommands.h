/*
 * ClipCommands — concrete undo/redo commands for clip operations.
 *
 * Each command stores only the delta needed for undo, not full state copies.
 * Commands reference clips by ID and tracks by pointer for stability.
 */

#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "command/Command.h"
#include "timeline/Track.h"
#include "timeline/Clip.h"

namespace rt {

class Timeline;

// ── Command type IDs for merge support ──────────────────────────────────────
enum class CommandTypeId : int
{
    AddClip           = 100,
    RemoveClip        = 101,
    MoveClip          = 102,
    TrimClipLeft      = 103,
    TrimClipRight     = 104,
    SetClipSpeed      = 105,
    SetClipEnabled    = 106,
    SetClipLabel      = 107,
    SetClipColor      = 108,
    AddTrack          = 200,
    RemoveTrack       = 201,
    MoveTrack         = 202,
    SetTrackName      = 203,
    SetTrackLocked    = 204,
    SetTrackMuted     = 205,
    SetTrackSoloed    = 206,
    SetTrackHeight    = 207,
    AddKeyframe       = 300,
    RemoveKeyframe    = 301,
    MoveKeyframe      = 302,
    SetKeyframeInterp = 303,
    AddTransition     = 400,
    RemoveTransition  = 401,
};

// ─────────────────────────────────────────────────────────────────────────────
// AddClipCommand — adds a clip to a track
// ─────────────────────────────────────────────────────────────────────────────
class AddClipCommand : public Command
{
public:
    /// Takes ownership of the clip. The clip is moved into the track on execute.
    AddClipCommand(Track* track, std::unique_ptr<Clip> clip);

    void execute() override;
    void undo() override;
    [[nodiscard]] std::string description() const override;
    [[nodiscard]] int typeId() const override { return static_cast<int>(CommandTypeId::AddClip); }

private:
    Track*                 m_track;
    std::unique_ptr<Clip>  m_clip;     // Held when not on the track (before execute / after undo)
    uint64_t               m_clipId;
};

// ─────────────────────────────────────────────────────────────────────────────
// RemoveClipCommand — removes a clip from a track
// ─────────────────────────────────────────────────────────────────────────────
class RemoveClipCommand : public Command
{
public:
    RemoveClipCommand(Track* track, uint64_t clipId);

    void execute() override;
    void undo() override;
    [[nodiscard]] std::string description() const override;
    [[nodiscard]] int typeId() const override { return static_cast<int>(CommandTypeId::RemoveClip); }

private:
    Track*                 m_track;
    uint64_t               m_clipId;
    std::unique_ptr<Clip>  m_clip;     // Held when removed (after execute / before undo)
    // Transitions that referenced the removed clip. Track::removeClip() drops
    // them (so no dissolve renders through empty space after a cut); we capture
    // them here so undo can put them back — otherwise a cross-dissolve/fade
    // silently vanished when a move/delete was undone.
    std::vector<Transition> m_savedTransitions;
};

// ─────────────────────────────────────────────────────────────────────────────
// MoveClipCommand — moves a clip to a new timeline position
// ─────────────────────────────────────────────────────────────────────────────
class MoveClipCommand : public Command
{
public:
    MoveClipCommand(Track* track, uint64_t clipId, int64_t newPosition);

    void execute() override;
    void undo() override;
    [[nodiscard]] std::string description() const override;
    [[nodiscard]] int typeId() const override { return static_cast<int>(CommandTypeId::MoveClip); }
    bool mergeWith(const Command& next) override;

private:
    Track*   m_track;
    uint64_t m_clipId;
    int64_t  m_oldPosition;
    int64_t  m_newPosition;
};

// ─────────────────────────────────────────────────────────────────────────────
// TrimClipCommand — changes clip duration and optionally sourceIn
// ─────────────────────────────────────────────────────────────────────────────
class TrimClipCommand : public Command
{
public:
    /// trimLeft: adjusts timelineIn + sourceIn. trimRight: adjusts duration only.
    TrimClipCommand(Track* track, uint64_t clipId,
                    int64_t newTimelineIn, int64_t newDuration, int64_t newSourceIn,
                    bool preserveKeyframeTimes = true);

    void execute() override;
    void undo() override;
    [[nodiscard]] std::string description() const override;
    [[nodiscard]] int typeId() const override { return static_cast<int>(CommandTypeId::TrimClipLeft); }
    bool mergeWith(const Command& next) override;

private:
    Track*   m_track;
    uint64_t m_clipId;
    int64_t  m_oldTimelineIn, m_newTimelineIn;
    int64_t  m_oldDuration,   m_newDuration;
    int64_t  m_oldSourceIn,   m_newSourceIn;
    bool     m_preserveKeyframeTimes;
};

// ─────────────────────────────────────────────────────────────────────────────
// SetClipPropertyCommand<T> — generic property setter
// ─────────────────────────────────────────────────────────────────────────────
template <typename T>
class SetClipPropertyCommand : public Command
{
public:
    using Getter = T (*)(const Clip&);
    using Setter = void (*)(Clip&, T);

    SetClipPropertyCommand(Track* track, uint64_t clipId,
                           T newValue, Getter getter, Setter setter,
                           std::string desc, int typeIdVal = -1)
        : m_track(track)
        , m_clipId(clipId)
        , m_newValue(std::move(newValue))
        , m_getter(getter)
        , m_setter(setter)
        , m_desc(std::move(desc))
        , m_typeIdVal(typeIdVal)
    {
    }

    void execute() override
    {
        size_t idx = m_track->findClipIndexById(m_clipId);
        if (idx == m_track->clipCount()) return;
        Clip* c = m_track->clip(idx);
        m_oldValue = m_getter(*c);
        m_setter(*c, m_newValue);
    }

    void undo() override
    {
        size_t idx = m_track->findClipIndexById(m_clipId);
        if (idx == m_track->clipCount()) return;
        Clip* c = m_track->clip(idx);
        m_setter(*c, m_oldValue);
    }

    [[nodiscard]] std::string description() const override { return m_desc; }
    [[nodiscard]] int typeId() const override { return m_typeIdVal; }

private:
    Track*   m_track;
    uint64_t m_clipId;
    T        m_oldValue{};
    T        m_newValue;
    Getter   m_getter;
    Setter   m_setter;
    std::string m_desc;
    int         m_typeIdVal;
};

} // namespace rt
