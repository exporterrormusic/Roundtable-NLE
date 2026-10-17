/*
 * CompoundCommand — groups multiple commands into a single undo step.
 *
 * Used for operations that touch multiple items simultaneously
 * (e.g. "delete selected" when multiple clips are selected).
 */

#pragma once

#include <memory>
#include <string>
#include <vector>

#include "Command.h"

namespace rt {

class CompoundCommand : public Command
{
public:
    explicit CompoundCommand(std::string description);
    ~CompoundCommand() override;

    /// Add a sub-command. Must be called before execute().
    void addCommand(std::unique_ptr<Command> cmd);

    /// Add a sub-command that has already been executed.
    /// The compound's execute() will skip it (already applied),
    /// but undo() will still undo it in reverse order.
    void addExecuted(std::unique_ptr<Command> cmd);

    void execute() override;
    void undo() override;
    void releaseHeldObjects(HeldModelObjects& out) override
    {
        for (auto& cmd : m_commands)
            if (cmd) cmd->releaseHeldObjects(out);
    }
    [[nodiscard]] std::string description() const override;

    /// Number of sub-commands
    [[nodiscard]] size_t size() const noexcept { return m_commands.size(); }

private:
    std::string                          m_description;
    std::vector<std::unique_ptr<Command>> m_commands;
    size_t                               m_executedCount{0}; ///< Commands already executed via addExecuted
};

} // namespace rt
