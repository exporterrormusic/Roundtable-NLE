/*
 * EditSession — the one gateway for undoable timeline edits.
 *
 * Instead of every edit writing its own inverse, an edit is bracketed:
 *
 *     edits.beginGesture("Move Clip", *timeline);   // exact copy taken
 *     ...mutate the live timeline freely (drag moves, commands, ...)
 *     edits.commit();      // diff -> ONE undo step holding only what changed
 *     // or edits.cancel(); // put everything back (Esc mid-drag)
 *
 * or in one call: edits.edit("Delete", *timeline, [&]{ ... });
 *               edits.execute(*timeline, std::move(legacyCommand));
 *
 * Undo/redo of the recorded step apply the delta in place (ModelDelta.h), so
 * clip/track/keyframe pointers held by panels survive. Steps find their
 * sequence by Timeline::id() through the resolver, never a stored pointer.
 *
 * Gestures nest: a begin while one is open joins it, and only the outermost
 * commit records. While a gesture is open, legacy commands sent to the
 * CommandStack still run but their own undo is dropped (the delta already
 * covers them) and an "[EDIT] ... absorbed" warning names the site.
 *
 * Scope: one sequence's content. Sequence add/remove, the bin, and other
 * non-timeline state keep their own commands.
 */

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace rt {

class Command;
class CommandStack;
class Timeline;

class EditSession
{
public:
    using SequenceResolver = std::function<Timeline*(uint64_t sequenceId)>;
    using CommitListener   = std::function<void(uint64_t sequenceId)>;

    explicit EditSession(CommandStack& stack);
    ~EditSession();

    EditSession(const EditSession&) = delete;
    EditSession& operator=(const EditSession&) = delete;

    /// How recorded steps find their live sequence at undo/redo time.
    void setSequenceResolver(SequenceResolver resolver) { m_resolver = std::move(resolver); }
    /// Told after each recorded step and each undo/redo of one.
    void setCommitListener(CommitListener listener) { m_listener = std::move(listener); }

    // ── Gestures ────────────────────────────────────────────────────────
    /// Start recording edits to `sequence`. Returns false when this joined an
    /// already-open gesture (only the outer one records).
    bool beginGesture(std::string description, Timeline& sequence);
    /// Close the gesture. The outermost commit records one undo step when the
    /// sequence changed. Returns true when a step was recorded.
    bool commit();
    /// Close the gesture and restore the sequence to how it was at begin.
    void cancel();
    /// Drop an open gesture without touching the model (history cleared).
    void abandon() noexcept;

    [[nodiscard]] bool isOpen() const noexcept { return m_open != nullptr; }
    [[nodiscard]] const std::string& openDescription() const noexcept;
    /// Rename the open gesture's undo step (e.g. a move that became a copy).
    void renameOpen(std::string description);
    /// Number of the open (outermost) gesture, 0 when none. EditGesture uses
    /// it so a stale handle never closes someone else's gesture.
    [[nodiscard]] uint64_t openGeneration() const noexcept;

    // ── One-shot edits ──────────────────────────────────────────────────
    template <class Fn>
    bool edit(std::string description, Timeline& sequence, Fn&& fn)
    {
        const bool outer = beginGesture(std::move(description), sequence);
        try {
            fn();
        } catch (...) {
            cancel();
            throw;
        }
        const bool recorded = commit();
        return outer && recorded;
    }

    /// Run a legacy command's execute() as one recorded edit (its own undo is
    /// not used). Description comes from the command.
    bool execute(Timeline& sequence, std::unique_ptr<Command> cmd);

    // ── Used by DeltaCommand ────────────────────────────────────────────
    [[nodiscard]] Timeline* resolve(uint64_t sequenceId) const;
    void notifyChanged(uint64_t sequenceId) const;

private:
    struct Open;
    CommandStack&         m_stack;
    std::unique_ptr<Open> m_open;
    uint64_t              m_nextGeneration{1};
    SequenceResolver      m_resolver;
    CommitListener        m_listener;
};

/// RAII gesture: commits on commit(), cancels on cancel(), and cancels when
/// destroyed while still open (an exception or early return mid-edit).
class EditGesture
{
public:
    EditGesture() = default;
    EditGesture(EditSession& session, std::string description, Timeline& sequence)
        : m_session(&session)
    {
        session.beginGesture(std::move(description), sequence);
        m_generation = session.openGeneration();
    }
    ~EditGesture() { cancel(); }

    EditGesture(EditGesture&& o) noexcept
        : m_session(o.m_session), m_generation(o.m_generation)
    {
        o.m_session = nullptr;
    }
    EditGesture& operator=(EditGesture&& o) noexcept
    {
        if (this != &o) {
            cancel();
            m_session = o.m_session;
            m_generation = o.m_generation;
            o.m_session = nullptr;
        }
        return *this;
    }
    EditGesture(const EditGesture&) = delete;
    EditGesture& operator=(const EditGesture&) = delete;

    /// True while this handle's gesture is still the open one.
    [[nodiscard]] bool active() const noexcept
    {
        return m_session && m_session->openGeneration() == m_generation;
    }
    bool commit()
    {
        const bool mine = active();
        auto* s = m_session;
        m_session = nullptr;
        return mine ? s->commit() : false;
    }
    void cancel()
    {
        const bool mine = active();
        auto* s = m_session;
        m_session = nullptr;
        if (mine) s->cancel();
    }

private:
    EditSession* m_session{nullptr};
    uint64_t     m_generation{0};
};

} // namespace rt
