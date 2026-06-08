/*
 * EffectCommands — undo/redo commands for effect operations.
 *
 * Commands:
 *  - AddEffectCommand    — add effect to clip's effect stack
 *  - RemoveEffectCommand — remove effect from clip's effect stack
 *  - MoveEffectCommand   — reorder effect within stack
 *  - SetEffectParamCommand — change a parameter value (mergeable for dragging)
 *  - SetEffectEnabledCommand — toggle effect on/off
 */

#pragma once

#include "command/Command.h"
#include "effects/Effect.h"
#include "effects/EffectStack.h"
#include "effects/GlitchEffects.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

// Forward-declare CommandTypeId additions (defined in ClipCommands.h)
// We'll use inline constants here to avoid modifying the existing enum.

namespace rt {

// ── Command type IDs for effects (500+) ─────────────────────────────────────

namespace EffectCmdId {
    inline constexpr int AddEffect         = 500;
    inline constexpr int RemoveEffect      = 501;
    inline constexpr int MoveEffect        = 502;
    inline constexpr int SetEffectParam    = 503;
    inline constexpr int SetEffectEnabled  = 504;
}

/// Build an undoable command that applies a glitch preset (a curated stack of
/// building-block effects) to `stack` in one step. Returns nullptr if the
/// preset is empty. Shared by every entry point (browser, drag-drop, etc.).
[[nodiscard]] std::unique_ptr<Command>
makeAddGlitchPresetCommand(EffectStack* stack, GlitchPreset preset);

// ═══════════════════════════════════════════════════════════════════════════
//  AddEffectCommand
// ═══════════════════════════════════════════════════════════════════════════

class AddEffectCommand : public Command
{
public:
    /// Construct: adds an effect of the given type to the stack.
    AddEffectCommand(EffectStack* stack, EffectType type);

    /// Construct: adds a pre-created effect (for redo of remove).
    AddEffectCommand(EffectStack* stack, std::unique_ptr<Effect> effect, size_t index);

    void execute() override;
    void undo() override;
    [[nodiscard]] std::string description() const override;
    [[nodiscard]] int typeId() const override { return EffectCmdId::AddEffect; }

    /// The effect ID that was added (valid after execute)
    [[nodiscard]] uint64_t addedEffectId() const noexcept { return m_effectId; }

private:
    EffectStack*             m_stack;
    EffectType               m_type;
    std::unique_ptr<Effect>  m_effect;  // held when undone, nullptr when active
    size_t                   m_index;
    uint64_t                 m_effectId{0};
    bool                     m_insertAtIndex{false};
};

// ═══════════════════════════════════════════════════════════════════════════
//  RemoveEffectCommand
// ═══════════════════════════════════════════════════════════════════════════

class RemoveEffectCommand : public Command
{
public:
    RemoveEffectCommand(EffectStack* stack, size_t index);

    void execute() override;
    void undo() override;
    [[nodiscard]] std::string description() const override;
    [[nodiscard]] int typeId() const override { return EffectCmdId::RemoveEffect; }

private:
    EffectStack*             m_stack;
    size_t                   m_index;
    std::unique_ptr<Effect>  m_removed;
    std::string              m_effectName;
};

// ═══════════════════════════════════════════════════════════════════════════
//  MoveEffectCommand
// ═══════════════════════════════════════════════════════════════════════════

class MoveEffectCommand : public Command
{
public:
    MoveEffectCommand(EffectStack* stack, size_t fromIndex, size_t toIndex);

    void execute() override;
    void undo() override;
    [[nodiscard]] std::string description() const override;
    [[nodiscard]] int typeId() const override { return EffectCmdId::MoveEffect; }

private:
    EffectStack* m_stack;
    size_t       m_from;
    size_t       m_to;
};

// ═══════════════════════════════════════════════════════════════════════════
//  SetEffectParamCommand — mergeable for continuous dragging
// ═══════════════════════════════════════════════════════════════════════════

class SetEffectParamCommand : public Command
{
public:
    /// Set a single keyframe at t=0 (static parameter change).
    SetEffectParamCommand(EffectStack* stack, uint64_t effectId,
                          size_t paramIndex, float newValue);

    void execute() override;
    void undo() override;
    [[nodiscard]] std::string description() const override;
    [[nodiscard]] int typeId() const override { return EffectCmdId::SetEffectParam; }
    [[nodiscard]] bool mergeWith(const Command& next) override;

private:
    EffectStack* m_stack;
    uint64_t     m_effectId;
    size_t       m_paramIndex;
    float        m_oldValue;
    float        m_newValue;
};

// ═══════════════════════════════════════════════════════════════════════════
//  SetEffectEnabledCommand
// ═══════════════════════════════════════════════════════════════════════════

class SetEffectEnabledCommand : public Command
{
public:
    SetEffectEnabledCommand(EffectStack* stack, uint64_t effectId, bool enabled);

    void execute() override;
    void undo() override;
    [[nodiscard]] std::string description() const override;
    [[nodiscard]] int typeId() const override { return EffectCmdId::SetEffectEnabled; }

private:
    EffectStack* m_stack;
    uint64_t     m_effectId;
    bool         m_oldEnabled;
    bool         m_newEnabled;
};

} // namespace rt
