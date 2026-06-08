/*
 * TrackCommands — concrete undo/redo commands for track operations.
 *
 * Handles adding, removing, moving, and modifying tracks on the timeline.
 */

#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "command/Command.h"
#include "command/commands/ClipCommands.h"   // CommandTypeId
#include "timeline/Track.h"                  // TrackType

namespace rt {

class Timeline;

// ─────────────────────────────────────────────────────────────────────────────
// AddTrackCommand — adds a new track to the timeline
// ─────────────────────────────────────────────────────────────────────────────
class AddTrackCommand : public Command
{
public:
    AddTrackCommand(Timeline* timeline, TrackType type, const std::string& name = "");

    /// Insert a PRE-BUILT track at an explicit index (e.g. paste creating
    /// overflow video tracks above the existing stack). Unlike the type/name
    /// ctor — which delegates to addVideoTrack()/addAudioTrack() and lands at
    /// the section default — this controls the exact insert position.
    AddTrackCommand(Timeline* timeline, std::unique_ptr<Track> prebuilt, size_t index);

    void execute() override;
    void undo() override;
    [[nodiscard]] std::string description() const override;
    [[nodiscard]] int typeId() const override { return static_cast<int>(CommandTypeId::AddTrack); }

    /// Get the track pointer after execute (valid until undo)
    [[nodiscard]] Track* track() const noexcept { return m_trackPtr; }

private:
    Timeline*              m_timeline;
    TrackType              m_type;
    std::string            m_name;
    size_t                 m_index{0};
    std::unique_ptr<Track> m_track;    // Held before execute / after undo
    Track*                 m_trackPtr{nullptr};
};

// ─────────────────────────────────────────────────────────────────────────────
// RemoveTrackCommand — removes a track from the timeline
// ─────────────────────────────────────────────────────────────────────────────
class RemoveTrackCommand : public Command
{
public:
    RemoveTrackCommand(Timeline* timeline, size_t index);

    void execute() override;
    void undo() override;
    [[nodiscard]] std::string description() const override;
    [[nodiscard]] int typeId() const override { return static_cast<int>(CommandTypeId::RemoveTrack); }

private:
    Timeline*              m_timeline;
    size_t                 m_index;
    std::unique_ptr<Track> m_track;    // Held after execute / before undo
};

// ─────────────────────────────────────────────────────────────────────────────
// MoveTrackCommand — reorders a track in the timeline
// ─────────────────────────────────────────────────────────────────────────────
class MoveTrackCommand : public Command
{
public:
    MoveTrackCommand(Timeline* timeline, size_t from, size_t to);

    void execute() override;
    void undo() override;
    [[nodiscard]] std::string description() const override;
    [[nodiscard]] int typeId() const override { return static_cast<int>(CommandTypeId::MoveTrack); }

private:
    Timeline* m_timeline;
    size_t    m_from;
    size_t    m_to;
};

// ─────────────────────────────────────────────────────────────────────────────
// SetTrackPropertyCommand<T> — generic track property setter
// ─────────────────────────────────────────────────────────────────────────────
template <typename T>
class SetTrackPropertyCommand : public Command
{
public:
    using Getter = T (*)(const Track&);
    using Setter = void (*)(Track&, T);

    SetTrackPropertyCommand(Track* track, T newValue,
                            Getter getter, Setter setter,
                            std::string desc, int typeIdVal = -1)
        : m_track(track)
        , m_newValue(std::move(newValue))
        , m_getter(getter)
        , m_setter(setter)
        , m_desc(std::move(desc))
        , m_typeIdVal(typeIdVal)
    {
    }

    void execute() override
    {
        m_oldValue = m_getter(*m_track);
        m_setter(*m_track, m_newValue);
    }

    void undo() override
    {
        m_setter(*m_track, m_oldValue);
    }

    [[nodiscard]] std::string description() const override { return m_desc; }
    [[nodiscard]] int typeId() const override { return m_typeIdVal; }

    bool mergeWith(const Command& next) override
    {
        if (m_typeIdVal < 0) return false;
        auto* other = dynamic_cast<const SetTrackPropertyCommand<T>*>(&next);
        if (!other || other->m_track != m_track) return false;
        m_newValue = other->m_newValue;
        return true;
    }

private:
    Track*      m_track;
    T           m_oldValue{};
    T           m_newValue;
    Getter      m_getter;
    Setter      m_setter;
    std::string m_desc;
    int         m_typeIdVal;
};

} // namespace rt
