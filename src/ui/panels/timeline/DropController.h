/*
 * DropController.h — drag-and-drop binder for the timeline workspace.
 *
 * Owns the signal wiring for everything dropped onto the timeline /
 * monitors:
 *   - media files from the Project Bin / Explorer / Source Monitor
 *     (wireMediaDropSignals — incl. the no-project bootstrap path that
 *     emits TimelineWorkspace::requestNewProjectForMedia)
 *   - effects / transitions from the Effects panel (wireEffectDropSignals)
 *   - nested-sequence drops + nest/un-nest plumbing (wireNestSignals)
 *
 * Extracted from TimelineWorkspace (god-class decomposition,
 * cleanup audit §3.1).  TRANSITIONAL DESIGN like OverlayController:
 * friend + back-pointer (m_X → m_ws->m_X) so the moved code stays
 * byte-comparable; narrowing to injected collaborators is follow-up.
 *
 * Definitions: DropControllerMediaDrop.cpp, DropControllerEffectDrop.cpp,
 * DropControllerNest.cpp (one per former TimelineWorkspaceWiring* TU).
 */
#pragma once

#include <QObject>

namespace rt {

class TimelineWorkspace;

class DropController : public QObject {
public:
    explicit DropController(TimelineWorkspace* ws) : m_ws(ws) {}

    void wireMediaDropSignals();   // DropControllerMediaDrop.cpp
    void wireEffectDropSignals();  // DropControllerEffectDrop.cpp
    void wireNestSignals();        // DropControllerNest.cpp

private:
    TimelineWorkspace* m_ws{nullptr};
};

} // namespace rt
