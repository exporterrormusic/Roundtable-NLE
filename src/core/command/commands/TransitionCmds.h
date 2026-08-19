/*
 * TransitionCmds — undo/redo commands for transition operations.
 *
 * Transitions live between adjacent clips on a track. These commands
 * handle adding, removing, and modifying transitions.
 */

#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "command/Command.h"
#include "command/commands/ClipCommands.h"  // CommandTypeId
#include "timeline/Transition.h"

namespace rt {

class Track;

// ─────────────────────────────────────────────────────────────────────────────
// AddTransitionCommand — adds a transition between two clips
// ─────────────────────────────────────────────────────────────────────────────
class AddTransitionCommand : public Command
{
public:
    AddTransitionCommand(Track* track, size_t clipIndexA, size_t clipIndexB,
                         Transition transition);

    void execute() override;
    void undo() override;
    [[nodiscard]] std::string description() const override;
    [[nodiscard]] int typeId() const override { return static_cast<int>(CommandTypeId::AddTransition); }

private:
    Track*     m_track;
    size_t     m_clipIndexA;
    size_t     m_clipIndexB;
    Transition m_transition;
    bool       m_applied{false};
    // Track::addTransition REPLACES an existing transition on the same
    // (leftClipId, rightClipId) edit point instead of stacking a duplicate.
    // Undo must distinguish the two outcomes: restore the prior value
    // (replace) vs remove the appended entry (plain add).
    size_t     m_index{0};
    bool       m_replaced{false};
    Transition m_replacedValue;
    bool       m_authorized{false};
};

// ─────────────────────────────────────────────────────────────────────────────
// RemoveTransitionCommand — removes a transition by index
// ─────────────────────────────────────────────────────────────────────────────
class RemoveTransitionCommand : public Command
{
public:
    RemoveTransitionCommand(Track* track, size_t transitionIndex);

    void execute() override;
    void undo() override;
    [[nodiscard]] std::string description() const override;
    [[nodiscard]] int typeId() const override { return static_cast<int>(CommandTypeId::RemoveTransition); }

private:
    Track*     m_track;
    size_t     m_transitionIndex;
    Transition m_savedTransition;
    bool       m_removed{false};
    bool       m_authorized{false};
};

// ─────────────────────────────────────────────────────────────────────────────
// SetTransitionPropertyCommand — modifies transition properties
// ─────────────────────────────────────────────────────────────────────────────
class SetTransitionPropertyCommand : public Command
{
public:
    SetTransitionPropertyCommand(Track* track, size_t transitionIndex,
                                 Transition newValues);

    void execute() override;
    void undo() override;
    [[nodiscard]] std::string description() const override;

private:
    Track*     m_track;
    size_t     m_transitionIndex;
    Transition m_oldValues;
    Transition m_newValues;
    bool       m_authorized{false};
    bool       m_applied{false};
};

} // namespace rt
