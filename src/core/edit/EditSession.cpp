/*
 * EditSession.cpp — see EditSession.h.
 */

#include "edit/EditSession.h"

#include "command/Command.h"
#include "command/CommandStack.h"
#include "edit/ModelDelta.h"
#include "timeline/Timeline.h"

#include <spdlog/spdlog.h>

namespace rt {

namespace {

/// One recorded edit: applies its delta's after side (redo) or before side
/// (undo) to whichever live sequence carries the delta's sequence id.
class DeltaCommand final : public Command
{
public:
    DeltaCommand(EditSession& session, std::string description, ModelDelta delta,
                 std::vector<std::unique_ptr<Command>> absorbed)
        : m_session(session)
        , m_description(std::move(description))
        , m_delta(std::move(delta))
        , m_absorbed(std::move(absorbed))
    {}

    void execute() override { apply(DeltaSide::After); }
    void undo() override { apply(DeltaSide::Before); }
    [[nodiscard]] std::string description() const override { return m_description; }

private:
    void apply(DeltaSide side)
    {
        Timeline* live = m_session.resolve(m_delta.sequenceId);
        if (!live) {
            spdlog::warn("[EDIT] '{}': its sequence no longer exists; nothing to {}",
                         m_description, side == DeltaSide::Before ? "undo" : "redo");
            return;
        }
        applyDelta(*live, m_delta, side);
        m_session.notifyChanged(m_delta.sequenceId);
    }

    EditSession&  m_session;
    std::string   m_description;
    ModelDelta    m_delta;
    // Legacy commands this step recorded. Never undone (the delta covers
    // them) but kept, like the stack used to keep them, so whatever they
    // own or point at lives exactly as long as this step is in history.
    std::vector<std::unique_ptr<Command>> m_absorbed;
};

} // namespace

struct EditSession::Open
{
    std::string               description;
    Timeline*                 sequence{nullptr};
    std::unique_ptr<Timeline> baseline;   // exact copy at begin
    int                       depth{1};
    uint64_t                  generation{0};
    // Legacy commands run inside this gesture, and the clips/tracks they
    // held off the timeline (e.g. a RemoveClipCommand's removed clip).
    std::vector<std::unique_ptr<Command>> absorbed;
    HeldModelObjects                      held;
};

namespace {

// The objects legacy commands handed over become the delta's parked objects,
// so undo/cancel brings back these exact objects rather than copies.
void parkHeldObjects(ModelDelta& delta, HeldModelObjects& held)
{
    for (auto& clip : held.clips) delta.parkClip(std::move(clip));
    for (auto& track : held.tracks) delta.parkTrack(std::move(track));
    held.clips.clear();
    held.tracks.clear();
}

} // namespace

uint64_t EditSession::openGeneration() const noexcept
{
    return m_open ? m_open->generation : 0;
}

EditSession::EditSession(CommandStack& stack)
    : m_stack(stack)
{
    m_stack.setEditSession(this);
}

EditSession::~EditSession()
{
    if (m_stack.editSession() == this)
        m_stack.setEditSession(nullptr);
}

const std::string& EditSession::openDescription() const noexcept
{
    static const std::string kNone;
    return m_open ? m_open->description : kNone;
}

void EditSession::renameOpen(std::string description)
{
    if (m_open) m_open->description = std::move(description);
}

bool EditSession::beginGesture(std::string description, Timeline& sequence)
{
    if (m_open) {
        if (m_open->sequence != &sequence)
            spdlog::warn("[EDIT] '{}' began inside '{}' on a different sequence; "
                         "its changes are not recorded", description, m_open->description);
        ++m_open->depth;
        return false;
    }
    m_open = std::make_unique<Open>();
    m_open->generation = m_nextGeneration++;
    m_open->description = std::move(description);
    m_open->sequence = &sequence;
    m_open->baseline = sequence.cloneForSnapshot();
    return true;
}

bool EditSession::commit()
{
    if (!m_open) return false;
    if (--m_open->depth > 0) return false;

    // Close before pushing so the stack doesn't treat our own step as a
    // legacy command arriving mid-gesture.
    std::unique_ptr<Open> open = std::move(m_open);
    ModelDelta delta = diffSequence(*open->baseline, *open->sequence);
    if (delta.empty()) return false;
    parkHeldObjects(delta, open->held);

    const uint64_t sequenceId = delta.sequenceId;
    spdlog::debug("[EDIT] '{}' recorded ({} clip(s) changed)",
                  open->description, delta.changedClipCount());
    m_stack.pushWithoutExecute(std::make_unique<DeltaCommand>(
        *this, std::move(open->description), std::move(delta),
        std::move(open->absorbed)));
    notifyChanged(sequenceId);
    return true;
}

void EditSession::cancel()
{
    if (!m_open) return;
    std::unique_ptr<Open> open = std::move(m_open);
    ModelDelta delta = diffSequence(*open->baseline, *open->sequence);
    if (delta.empty()) return;
    parkHeldObjects(delta, open->held);
    applyDelta(*open->sequence, delta, DeltaSide::Before);
    notifyChanged(delta.sequenceId);
}

void EditSession::absorb(std::unique_ptr<Command> cmd)
{
    if (!cmd) return;
    if (!m_open) return;   // nothing to record into; the caller's own path
    cmd->releaseHeldObjects(m_open->held);
    m_open->absorbed.push_back(std::move(cmd));
}

void EditSession::abandon() noexcept
{
    m_open.reset();
}

bool EditSession::execute(Timeline& sequence, std::unique_ptr<Command> cmd)
{
    if (!cmd) return false;
    const bool outer = beginGesture(cmd->description(), sequence);
    try {
        cmd->execute();
    } catch (...) {
        cancel();
        throw;
    }
    absorb(std::move(cmd));   // keeps what it owns alive in the recorded step
    const bool recorded = commit();
    return outer && recorded;
}

void executeTimelineEdit(CommandStack* stack, Timeline* timeline,
                         std::unique_ptr<Command> cmd)
{
    if (!cmd || !stack) return;
    if (EditSession* edits = stack->editSession(); edits && timeline)
        edits->execute(*timeline, std::move(cmd));
    else
        stack->execute(std::move(cmd));
}

Timeline* EditSession::resolve(uint64_t sequenceId) const
{
    return m_resolver ? m_resolver(sequenceId) : nullptr;
}

void EditSession::notifyChanged(uint64_t sequenceId) const
{
    if (m_listener) m_listener(sequenceId);
}

} // namespace rt
