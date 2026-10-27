/*
 * CommandStack.cpp — Undo/redo history implementation.
 * Step 4: Command System
 */

#include "command/CommandStack.h"
#include "command/Command.h"  // for MacroCommand
#include "edit/EditSession.h"

#include <spdlog/spdlog.h>

#include <algorithm>

namespace rt {

CommandStack::CommandStack(size_t maxDepth)
    : m_maxDepth(maxDepth)
{
}

CommandStack::~CommandStack() = default;

bool CommandStack::absorbedByOpenGesture(std::unique_ptr<Command>& cmd, bool alreadyApplied)
{
    if (!m_editSession || !m_editSession->isOpen()) return false;
    // The gesture's before/after diff records whatever this command changed,
    // so keeping its own undo too would revert the change twice. The step
    // takes the command's (more precise) name.
    if (alreadyApplied) {
        // A finished edit reporting what it already applied — the normal
        // pattern of widgets inside a bracketed gesture.
        spdlog::debug("[EDIT] '{}' recorded by gesture '{}'", cmd->description(),
                      m_editSession->openDescription());
    } else {
        // Ran while a gesture was open: correct for timeline data, but any
        // non-timeline state it changed is no longer undoable. Worth a look.
        spdlog::warn("[EDIT] legacy command '{}' executed inside '{}' — absorbed "
                     "into that undo step", cmd->description(),
                     m_editSession->openDescription());
    }
    m_editSession->renameOpen(cmd->description());
    // The step keeps the command (and the clips/tracks it holds) alive.
    m_editSession->absorb(std::move(cmd));
    return true;
}

void CommandStack::execute(std::unique_ptr<Command> cmd)
{
    if (!cmd) return;

    // Always execute first (apply the change)
    cmd->execute();
    if (absorbedByOpenGesture(cmd, /*alreadyApplied=*/false)) return;

    // During a macro, buffer the command instead of pushing directly
    if (m_macroDepth > 0 && m_activeMacro) {
        m_activeMacro->addCommand(std::move(cmd));
        return;
    }

    // A new edit abandons the redo branch even when it merges into the most
    // recent undo command. Clear it before mergeWith() can return early.
    m_redoStack.clear();

    // Try to merge with the most recent command (absorb into existing),
    // but only inside the current gesture — see sealMerge().
    const bool canMerge = !m_mergeSealed;
    m_mergeSealed = false;
    if (canMerge && !m_undoStack.empty() && m_undoStack.back()->typeId() >= 0
        && m_undoStack.back()->typeId() == cmd->typeId()
        && m_undoStack.back()->mergeWith(*cmd))
    {
        spdlog::trace("Merged command: {}", m_undoStack.back()->description());
        notifyChange();
        notifyHistory(HistoryEvent::Recorded);
        return;
    }

    m_undoStack.push_back(std::move(cmd));

    trimToMaxDepth();
    notifyChange();
    notifyHistory(HistoryEvent::Recorded);

    spdlog::trace("Executed command: {} (undo depth: {})",
                  m_undoStack.back()->description(), m_undoStack.size());
}

void CommandStack::pushWithoutExecute(std::unique_ptr<Command> cmd)
{
    if (!cmd) return;
    if (absorbedByOpenGesture(cmd, /*alreadyApplied=*/true)) return;

    // During a macro, buffer the command instead of pushing directly
    if (m_macroDepth > 0 && m_activeMacro) {
        m_activeMacro->addCommand(std::move(cmd));
        return;
    }

    // Push onto undo stack without calling cmd->execute().
    // The caller is responsible for having already applied the change.
    m_mergeSealed = true;
    m_undoStack.push_back(std::move(cmd));
    m_redoStack.clear();
    trimToMaxDepth();
    notifyChange();
    notifyHistory(HistoryEvent::Recorded);

    spdlog::trace("Pushed (no-exec) command: {} (undo depth: {})",
                  m_undoStack.back()->description(), m_undoStack.size());
}

bool CommandStack::undo()
{
    if (m_undoStack.empty()) return false;
    // Undo mid-drag would rewind the model under the open gesture's copy.
    if (m_editSession && m_editSession->isOpen()) {
        spdlog::debug("[EDIT] undo ignored while '{}' is in progress",
                      m_editSession->openDescription());
        return false;
    }

    auto cmd = std::move(m_undoStack.back());
    m_undoStack.pop_back();
    // The new top is an OLDER command; a fresh edit must not merge into it.
    m_mergeSealed = true;

    cmd->undo();
    spdlog::trace("Undid command: {}", cmd->description());

    m_redoStack.push_back(std::move(cmd));
    notifyChange();
    notifyHistory(HistoryEvent::Undone);
    return true;
}

bool CommandStack::redo()
{
    if (m_redoStack.empty()) return false;
    if (m_editSession && m_editSession->isOpen()) {
        spdlog::debug("[EDIT] redo ignored while '{}' is in progress",
                      m_editSession->openDescription());
        return false;
    }

    auto cmd = std::move(m_redoStack.back());
    m_redoStack.pop_back();
    m_mergeSealed = true;

    cmd->execute();
    spdlog::trace("Redid command: {}", cmd->description());

    m_undoStack.push_back(std::move(cmd));
    notifyChange();
    notifyHistory(HistoryEvent::Redone);
    return true;
}

bool CommandStack::canUndo() const noexcept
{
    return !m_undoStack.empty();
}

bool CommandStack::canRedo() const noexcept
{
    return !m_redoStack.empty();
}

std::string CommandStack::undoDescription() const
{
    return m_undoStack.empty() ? "" : m_undoStack.back()->description();
}

std::string CommandStack::redoDescription() const
{
    return m_redoStack.empty() ? "" : m_redoStack.back()->description();
}

size_t CommandStack::undoCount() const noexcept
{
    return m_undoStack.size();
}

bool CommandStack::undoAffectsTimeline() const
{
    return !m_undoStack.empty() && m_undoStack.back()->affectsTimeline();
}

bool CommandStack::redoAffectsTimeline() const
{
    return !m_redoStack.empty() && m_redoStack.back()->affectsTimeline();
}

size_t CommandStack::redoCount() const noexcept
{
    return m_redoStack.size();
}

void CommandStack::clear()
{
    // History is going away (project switch/close): an open gesture's copy
    // may belong to a sequence that is about to be destroyed.
    if (m_editSession) m_editSession->abandon();
    m_undoStack.clear();
    m_redoStack.clear();
    m_mergeSealed = true;
    notifyChange();
    notifyHistory(HistoryEvent::Cleared);
}

CommandStack::HistorySnapshot CommandStack::historySnapshot() const
{
    HistorySnapshot snap;
    snap.descriptions.reserve(m_undoStack.size() + m_redoStack.size());

    // Undo stack: oldest first
    for (auto& cmd : m_undoStack)
        snap.descriptions.push_back(cmd->description());

    snap.currentIndex = m_undoStack.size();

    // Redo stack: newest first (redo stack is LIFO, so reverse order)
    for (auto it = m_redoStack.rbegin(); it != m_redoStack.rend(); ++it)
        snap.descriptions.push_back((*it)->description());

    return snap;
}

bool CommandStack::jumpToIndex(size_t index)
{
    size_t current = m_undoStack.size();
    size_t total = m_undoStack.size() + m_redoStack.size();

    if (index > total) return false;

    if (index < current) {
        // Undo (current - index) times
        size_t count = current - index;
        for (size_t i = 0; i < count; ++i) {
            if (!undo()) return false;
        }
    } else if (index > current) {
        // Redo (index - current) times
        size_t count = index - current;
        for (size_t i = 0; i < count; ++i) {
            if (!redo()) return false;
        }
    }

    return true;
}

void CommandStack::trimToMaxDepth()
{
    while (m_undoStack.size() > m_maxDepth)
    {
        m_undoStack.pop_front(); // Drop oldest commands
    }
}

void CommandStack::notifyChange()
{
    if (m_callback) m_callback();
}

size_t CommandStack::addHistoryListener(HistoryListener listener)
{
    const size_t id = m_nextHistoryListenerId++;
    m_historyListeners.emplace_back(id, std::move(listener));
    return id;
}

void CommandStack::removeHistoryListener(size_t id)
{
    m_historyListeners.erase(
        std::remove_if(m_historyListeners.begin(), m_historyListeners.end(),
                       [id](const auto& entry) { return entry.first == id; }),
        m_historyListeners.end());
}

void CommandStack::notifyHistory(HistoryEvent event)
{
    // Copy: a listener may remove itself (or another) while being told.
    const auto listeners = m_historyListeners;
    for (const auto& [id, listener] : listeners)
        if (listener) listener(event);
}

void CommandStack::beginMacro(std::string description)
{
    if (m_macroDepth == 0) {
        m_activeMacro = std::make_unique<MacroCommand>(std::move(description));
    }
    ++m_macroDepth;
    spdlog::trace("CommandStack::beginMacro depth={}", m_macroDepth);
}

void CommandStack::endMacro()
{
    if (m_macroDepth <= 0) {
        spdlog::warn("CommandStack::endMacro called without matching beginMacro");
        return;
    }
    --m_macroDepth;
    spdlog::trace("CommandStack::endMacro depth={}", m_macroDepth);

    if (m_macroDepth == 0 && m_activeMacro && !m_activeMacro->empty()) {
        std::string desc = m_activeMacro->description();
        size_t count = m_activeMacro->size();

        m_undoStack.push_back(std::move(m_activeMacro));
        m_redoStack.clear();
        m_mergeSealed = true;
        trimToMaxDepth();
        notifyChange();
        notifyHistory(HistoryEvent::Recorded);

        spdlog::trace("Pushed macro command: {} ({} children, undo depth: {})",
                      desc, count, m_undoStack.size());
    }
    // Clean up if empty or after pushing
    m_activeMacro.reset();
}

} // namespace rt

