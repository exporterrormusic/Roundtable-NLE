/*
 * LambdaCommand — lightweight undo/redo command using std::function.
 *
 * Useful for one-off operations where creating a full Command subclass
 * would be overkill (e.g. viewport transform drags).
 */

#pragma once

#include <functional>
#include <string>

#include "Command.h"

namespace rt {

class LambdaCommand : public Command
{
public:
    LambdaCommand(std::string description,
                  std::function<void()> executeFn,
                  std::function<void()> undoFn,
                  bool affectsTimeline = true)
        : m_description(std::move(description))
        , m_executeFn(std::move(executeFn))
        , m_undoFn(std::move(undoFn))
        , m_affectsTimeline(affectsTimeline)
    {
    }

    void execute() override { if (m_executeFn) m_executeFn(); }
    void undo()    override { if (m_undoFn)    m_undoFn();    }

    [[nodiscard]] std::string description() const override { return m_description; }
    [[nodiscard]] bool affectsTimeline() const override { return m_affectsTimeline; }

private:
    std::string            m_description;
    std::function<void()>  m_executeFn;
    std::function<void()>  m_undoFn;
    bool                  m_affectsTimeline;
};

} // namespace rt
