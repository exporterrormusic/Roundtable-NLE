/*
 * EffectCommands.cpp — undo/redo command implementations for effects.
 * Step 22: Effects System
 */

#include "command/commands/EffectCommands.h"
#include "command/CompoundCommand.h"

#include <string>

namespace rt {

// ── Glitch preset → compound add command ────────────────────────────────────

std::unique_ptr<Command>
makeAddGlitchPresetCommand(EffectStack* stack, GlitchPreset preset)
{
    auto effects = buildGlitchPreset(preset);
    if (!stack || effects.empty()) return nullptr;

    auto compound = std::make_unique<CompoundCommand>(
        std::string("Apply ") + glitchPresetName(preset) + " glitch");
    // Append in order; each new effect goes at the current end of the stack.
    size_t index = stack->effectCount();
    for (auto& fx : effects)
        compound->addCommand(
            std::make_unique<AddEffectCommand>(stack, std::move(fx), index++));
    return compound;
}

// ═══════════════════════════════════════════════════════════════════════════
//  AddEffectCommand
// ═══════════════════════════════════════════════════════════════════════════

AddEffectCommand::AddEffectCommand(EffectStack* stack, EffectType type)
    : m_stack(stack)
    , m_type(type)
    , m_index(0)
    , m_insertAtIndex(false)
{
}

AddEffectCommand::AddEffectCommand(EffectStack* stack,
                                   std::unique_ptr<Effect> effect,
                                   size_t index)
    : m_stack(stack)
    , m_type(effect ? effect->effectType() : EffectType::ColorCorrect)
    , m_effect(std::move(effect))
    , m_index(index)
    , m_insertAtIndex(true)
{
    if (m_effect) m_effectId = m_effect->id();
}

void AddEffectCommand::execute()
{
    if (m_insertAtIndex && m_effect) {
        m_effectId = m_effect->id();
        m_stack->insertEffect(m_index, std::move(m_effect));
    } else if (!m_insertAtIndex) {
        auto fx = createEffect(m_type);
        m_effectId = fx->id();
        m_index = m_stack->effectCount();
        m_stack->addEffect(std::move(fx));
    }
}

void AddEffectCommand::undo()
{
    auto idx = m_stack->indexOf(m_effectId);
    if (idx < m_stack->effectCount()) {
        m_effect = m_stack->removeEffect(idx);
        m_index = idx;
        m_insertAtIndex = true;
    }
}

std::string AddEffectCommand::description() const
{
    return "Add " + std::string(effectTypeName(m_type));
}

// ═══════════════════════════════════════════════════════════════════════════
//  RemoveEffectCommand
// ═══════════════════════════════════════════════════════════════════════════

RemoveEffectCommand::RemoveEffectCommand(EffectStack* stack, size_t index)
    : m_stack(stack)
    , m_index(index)
{
    if (index < stack->effectCount())
        m_effectName = stack->effect(index).name();
}

void RemoveEffectCommand::execute()
{
    m_removed = m_stack->removeEffect(m_index);
}

void RemoveEffectCommand::undo()
{
    if (m_removed)
        m_stack->insertEffect(m_index, std::move(m_removed));
}

std::string RemoveEffectCommand::description() const
{
    return "Remove " + m_effectName;
}

// ═══════════════════════════════════════════════════════════════════════════
//  MoveEffectCommand
// ═══════════════════════════════════════════════════════════════════════════

MoveEffectCommand::MoveEffectCommand(EffectStack* stack,
                                     size_t fromIndex, size_t toIndex)
    : m_stack(stack), m_from(fromIndex), m_to(toIndex)
{
}

void MoveEffectCommand::execute()
{
    m_stack->moveEffect(m_from, m_to);
}

void MoveEffectCommand::undo()
{
    m_stack->moveEffect(m_to, m_from);
}

std::string MoveEffectCommand::description() const
{
    return "Move Effect";
}

// ═══════════════════════════════════════════════════════════════════════════
//  SetEffectParamCommand
// ═══════════════════════════════════════════════════════════════════════════

SetEffectParamCommand::SetEffectParamCommand(EffectStack* stack,
                                             uint64_t effectId,
                                             size_t paramIndex,
                                             float newValue)
    : m_stack(stack)
    , m_effectId(effectId)
    , m_paramIndex(paramIndex)
    , m_oldValue(0.0f)
    , m_newValue(newValue)
{
    auto* fx = m_stack->effectById(m_effectId);
    if (fx && m_paramIndex < fx->paramCount())
        m_oldValue = fx->param(m_paramIndex).track.evaluate(0);
}

void SetEffectParamCommand::execute()
{
    auto* fx = m_stack->effectById(m_effectId);
    if (fx && m_paramIndex < fx->paramCount())
        fx->param(m_paramIndex).track.addKeyframe(0, m_newValue);
}

void SetEffectParamCommand::undo()
{
    auto* fx = m_stack->effectById(m_effectId);
    if (fx && m_paramIndex < fx->paramCount())
        fx->param(m_paramIndex).track.addKeyframe(0, m_oldValue);
}

std::string SetEffectParamCommand::description() const
{
    return "Set Effect Parameter";
}

bool SetEffectParamCommand::mergeWith(const Command& next)
{
    if (next.typeId() != EffectCmdId::SetEffectParam) return false;
    auto& other = static_cast<const SetEffectParamCommand&>(next);
    if (other.m_effectId != m_effectId || other.m_paramIndex != m_paramIndex)
        return false;
    m_newValue = other.m_newValue;
    return true;
}

// ═══════════════════════════════════════════════════════════════════════════
//  SetEffectEnabledCommand
// ═══════════════════════════════════════════════════════════════════════════

SetEffectEnabledCommand::SetEffectEnabledCommand(EffectStack* stack,
                                                 uint64_t effectId,
                                                 bool enabled)
    : m_stack(stack)
    , m_effectId(effectId)
    , m_oldEnabled(true)
    , m_newEnabled(enabled)
{
    auto* fx = m_stack->effectById(m_effectId);
    if (fx) m_oldEnabled = fx->isEnabled();
}

void SetEffectEnabledCommand::execute()
{
    auto* fx = m_stack->effectById(m_effectId);
    if (fx) fx->setEnabled(m_newEnabled);
}

void SetEffectEnabledCommand::undo()
{
    auto* fx = m_stack->effectById(m_effectId);
    if (fx) fx->setEnabled(m_oldEnabled);
}

std::string SetEffectEnabledCommand::description() const
{
    return m_newEnabled ? "Enable Effect" : "Disable Effect";
}

} // namespace rt
