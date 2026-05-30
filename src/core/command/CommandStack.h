/*
 * CommandStack — manages the undo/redo history.
 *
 * Owns a stack of executed commands. Supports:
 * - execute(cmd) — push and execute a command
 * - undo() / redo()
 * - merge continuous operations (e.g. dragging)
 * - configurable max depth
 *
 * ════════════════════════════════════════════════════════════════════════════
 *  EDIT DISCIPLINE  (read before adding a new undoable edit — #21)
 * ════════════════════════════════════════════════════════════════════════════
 *
 *  The #1 source of undo bugs here is choosing the wrong push. The rule:
 *
 *    • Has the model ALREADY been mutated when you record the command?
 *        - NO  (the command should perform the change)  -> execute(cmd)
 *               or, in a TimelinePanel, executeCommand(cmd) which also
 *               refreshes track widgets + emits contentChanged().
 *        - YES (a live drag/spinbox already applied it)  -> pushWithoutExecute(cmd)
 *
 *    Using execute() on an already-applied change DOUBLE-applies it.
 *    Using pushWithoutExecute() on a not-yet-applied change makes the action
 *    do nothing until the user redoes it. Both are classic regressions.
 *
 *    • The command's execute()/undo() must be SYMMETRIC and capture their own
 *      before/after state at construction — never read "current" state at
 *      undo() time (it may have moved on).
 *
 *    • Group N edits into ONE undo step with beginMacro()/endMacro()
 *      (e.g. a multi-clip move, a paste, an effect-with-keyframes add).
 *
 *  EDIT INVARIANTS (fold these into any new edit of the same kind):
 *
 *    • Keyframe time is CLIP-RELATIVE, not absolute timeline tick. Use the
 *      clip-relative tick (see EffectControlsPanel::clipRelativeTick()) when
 *      reading/writing a clip's KeyframeTrack, or keyframes land at the wrong
 *      time after the clip moves.
 *
 *    • Multi-clip move clamps the GROUP delta, not each clip. Compute one
 *      delta for the whole selection and clamp THAT so the leftmost clip
 *      stops at tick 0 — never clamp clips individually (they'd pile up /
 *      overlap at 0). See TimelinePanelMouseDragMove.cpp "Group-floor clamp".
 * ════════════════════════════════════════════════════════════════════════════
 */

#pragma once

#include <deque>
#include <memory>
#include <functional>
#include <string>
#include <vector>

#include "Command.h"

namespace rt {

class CommandStack
{
public:
    explicit CommandStack(size_t maxDepth = 200);
    ~CommandStack();

    /// Execute a command and push it onto the undo stack.
    /// Clears the redo stack.
    void execute(std::unique_ptr<Command> cmd);

    /// Push a command onto the undo stack WITHOUT calling execute().
    /// Use when the action has already been applied (e.g. live drag) and
    /// you only need the undo record.  Clears the redo stack.
    void pushWithoutExecute(std::unique_ptr<Command> cmd);

    /// Undo the most recent command.
    /// Returns true if there was something to undo.
    bool undo();

    /// Redo the most recently undone command.
    /// Returns true if there was something to redo.
    bool redo();

    /// Can we currently undo/redo?
    [[nodiscard]] bool canUndo() const noexcept;
    [[nodiscard]] bool canRedo() const noexcept;

    /// Description of the next undo/redo action
    [[nodiscard]] std::string undoDescription() const;
    [[nodiscard]] std::string redoDescription() const;

    /// Number of commands in the undo stack
    [[nodiscard]] size_t undoCount() const noexcept;
    [[nodiscard]] size_t redoCount() const noexcept;

    /// Get all command descriptions in the history.
    /// Returns undo descriptions (oldest-to-newest) + redo descriptions (newest-to-oldest).
    /// The currentIndex indicates the boundary: items [0..currentIndex) are undo,
    /// items [currentIndex..size) are redo.
    struct HistorySnapshot
    {
        std::vector<std::string> descriptions;
        size_t currentIndex{0};  ///< Points past the last executed command
    };
    [[nodiscard]] HistorySnapshot historySnapshot() const;

    /// Jump to a specific position in the history.
    /// index < currentIndex => undo (currentIndex - index) times.
    /// index > currentIndex => redo (index - currentIndex) times.
    /// Returns true if the position was reached.
    bool jumpToIndex(size_t index);

    /// Clear all history
    void clear();

    /// Callback for UI updates (called after execute/undo/redo)
    using ChangeCallback = std::function<void()>;
    void setChangeCallback(ChangeCallback cb) { m_callback = std::move(cb); }

    /// Maximum undo depth
    [[nodiscard]] size_t maxDepth() const noexcept { return m_maxDepth; }
    void setMaxDepth(size_t depth) noexcept { m_maxDepth = depth; }

    /// Begin a macro — subsequent execute() / pushWithoutExecute() calls are
    /// buffered into a single undo group until endMacro() is called.
    /// Nested beginMacro() calls are ignored (only outermost matters).
    void beginMacro(std::string description);

    /// End the current macro and push the grouped command onto the stack.
    /// If no macro is active, this is a no-op.
    void endMacro();

    /// Returns true if a macro is currently being recorded.
    [[nodiscard]] bool isMacroActive() const noexcept { return m_macroDepth > 0; }

private:
    std::deque<std::unique_ptr<Command>> m_undoStack;
    std::deque<std::unique_ptr<Command>> m_redoStack;
    size_t                               m_maxDepth;
    ChangeCallback                       m_callback;

    // Macro recording state
    int                                   m_macroDepth{0};
    std::unique_ptr<class MacroCommand>   m_activeMacro;

    void trimToMaxDepth();
    void notifyChange();
};

} // namespace rt
