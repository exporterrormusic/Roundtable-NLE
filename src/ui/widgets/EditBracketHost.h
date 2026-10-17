/*
 * EditBracketHost — lets a reusable input widget (ScrubbySpinBox, the Effect
 * Controls keyframe lane) mark where one user edit begins and ends, so the
 * panel hosting it can record that edit as a single undo step through the
 * EditSession.
 *
 * Widgets walk their parent chain for the nearest host and call
 * beginWidgetEdit() before the edit's first change and endWidgetEdit() after
 * its commit. Panels opt in by implementing this; panels whose widgets also
 * edit non-timeline state (export settings, dialogs) simply don't, and keep
 * recording through their own commands.
 */

#pragma once

#include <QWidget>

namespace rt {

class EditBracketHost
{
public:
    virtual ~EditBracketHost() = default;
    virtual void beginWidgetEdit() = 0;
    virtual void endWidgetEdit() = 0;

    /// Nearest host above `w`, or nullptr.
    [[nodiscard]] static EditBracketHost* findFor(const QWidget* w)
    {
        for (QWidget* p = w ? w->parentWidget() : nullptr; p; p = p->parentWidget())
            if (auto* host = dynamic_cast<EditBracketHost*>(p))
                return host;
        return nullptr;
    }
};

} // namespace rt
