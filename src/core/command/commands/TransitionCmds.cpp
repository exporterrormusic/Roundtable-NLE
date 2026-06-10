/*
 * TransitionCmds.cpp — Transition command implementations.
 * Step 4: Command System
 */

#include "command/commands/TransitionCmds.h"
#include "timeline/Track.h"

namespace rt {

// ── AddTransitionCommand ────────────────────────────────────────────────────

AddTransitionCommand::AddTransitionCommand(Track* track, size_t clipIndexA, size_t clipIndexB,
                                           Transition transition)
    : m_track(track)
    , m_clipIndexA(clipIndexA)
    , m_clipIndexB(clipIndexB)
    , m_transition(transition)
{
}

void AddTransitionCommand::execute()
{
    // addTransition() replaces any existing transition on the same edit
    // point; capture the prior value so undo can restore it.
    const size_t existing = m_track->findTransition(
        m_transition.leftClipId, m_transition.rightClipId);
    if (existing != Track::kNoTransition) {
        m_replaced      = true;
        m_replacedValue = *m_track->transition(existing);
    } else {
        m_replaced = false;
    }
    m_index   = m_track->addTransition(m_transition);
    m_applied = true;
}

void AddTransitionCommand::undo()
{
    if (!m_applied) return;
    if (m_replaced) {
        m_track->setTransition(m_index, m_replacedValue);
    } else if (m_index < m_track->transitionCount()) {
        m_track->removeTransition(m_index);
    }
    m_applied = false;
}

std::string AddTransitionCommand::description() const
{
    return "Add Transition";
}

// ── RemoveTransitionCommand ─────────────────────────────────────────────────

RemoveTransitionCommand::RemoveTransitionCommand(Track* track, size_t transitionIndex)
    : m_track(track)
    , m_transitionIndex(transitionIndex)
{
}

void RemoveTransitionCommand::execute()
{
    m_savedTransition = m_track->removeTransition(m_transitionIndex);
    m_removed = true;
}

void RemoveTransitionCommand::undo()
{
    if (m_removed)
    {
        // Re-insert at the same position. For simplicity, we add back and it goes at the end.
        // A more sophisticated approach would insert at m_transitionIndex.
        m_track->addTransition(m_savedTransition);
        m_removed = false;
    }
}

std::string RemoveTransitionCommand::description() const
{
    return "Remove Transition";
}

// ── SetTransitionPropertyCommand ────────────────────────────────────────────

SetTransitionPropertyCommand::SetTransitionPropertyCommand(
    Track* track, size_t transitionIndex, Transition newValues)
    : m_track(track)
    , m_transitionIndex(transitionIndex)
    , m_newValues(newValues)
{
}

void SetTransitionPropertyCommand::execute()
{
    const Transition* t = m_track->transition(m_transitionIndex);
    if (t)
    {
        m_oldValues = *t;
        m_track->setTransition(m_transitionIndex, m_newValues);
    }
}

void SetTransitionPropertyCommand::undo()
{
    m_track->setTransition(m_transitionIndex, m_oldValues);
}

std::string SetTransitionPropertyCommand::description() const
{
    return "Modify Transition";
}

} // namespace rt
