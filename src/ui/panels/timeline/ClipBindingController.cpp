#include "panels/timeline/ClipBindingController.h"

#include "panels/effects/ColorGradingPanel.h"
#include "panels/effects/EffectControlsPanel.h"
#include "panels/effects/EffectsPanel.h"
#include "panels/effects/GraphicsEditorPanel.h"
#include "panels/properties/PropertiesPanel.h"
#include "panels/tierlist/TierListPanel.h"
#include "panels/timeline/SelectionState.h"
#include "timeline/Clip.h"
#include "timeline/Timeline.h"
#include "timeline/Track.h"
#include "timeline/VideoClip.h"

#include <QDockWidget>

#include <unordered_set>

namespace rt {

ClipBindingController::ClipBindingController(Config cfg)
    : m_cfg(std::move(cfg))
{
}

void ClipBindingController::selectClip(Clip* clip, Track* track,
                                       size_t trackIdx, size_t clipIdx, Bind bind)
{
    if (!clip) return;
    SelectionState& selection = *m_cfg.selection;
    // A different clip forgets the selected Graphic layer. The same clip
    // keeps it: setClip returns early, so layerSelected won't set it again.
    if (clip != selection.clip)
        selection.graphicLayerIdx = -1;
    selection.clip = clip;
    selection.trackIdx = trackIdx;
    selection.clipIdx = clipIdx;

    if (bind == Bind::OpenDocks)
        bindOpenDocks(clip, track);
    else
        bindClipPanels(clip, track);
    m_cfg.scheduleOverlayRefresh();
    raiseDockFor(*clip);
}

void ClipBindingController::bindOpenDocks(Clip* clip, Track* track)
{
    const auto open = [this](const char* name) {
        QDockWidget* dock = m_cfg.dockForPanel(QString::fromLatin1(name));
        return !dock || dock->isVisible();
    };
    if (m_panels.effectControls && open("Effect Controls"))
        m_panels.effectControls->setClip(clip, track);
    // A Graphic clip always binds: layerSelected sets the selected layer for
    // the per-layer transform overlay.
    if (m_panels.graphicsEditor
            && (open("Graphics Editor") || clip->clipType() == ClipType::Graphic))
        m_panels.graphicsEditor->setClip(clip, track);
    // Never skipped: the old check looked up a dock named "Color Grading",
    // and the dock is "Color Correction".
    if (m_panels.colorGrading)
        m_panels.colorGrading->setClip(clip, track);
    if (m_panels.properties && open("Properties"))
        m_panels.properties->setClip(clip, track);
    // Cheap; keeps the panel populated for when its dock is opened.
    if (m_panels.tierList)
        m_panels.tierList->setClip(clip, track);
}

void ClipBindingController::selectClips(const std::vector<Clip*>& clips, bool editable)
{
    if (m_panels.properties)
        m_panels.properties->setMultiSelection(clips, editable);
}

void ClipBindingController::clearSelection()
{
    clearClipPanels();
    m_cfg.selection->clip = nullptr;
    m_cfg.selection->graphicLayerIdx = -1;
    m_cfg.clearTransformOverlays();
}

void ClipBindingController::bindClipPanels(Clip* clip, Track* track)
{
    if (m_panels.effectControls) m_panels.effectControls->setClip(clip, track);
    if (m_panels.graphicsEditor) m_panels.graphicsEditor->setClip(clip, track);
    if (m_panels.colorGrading)   m_panels.colorGrading->setClip(clip, track);
    if (m_panels.properties)     m_panels.properties->setClip(clip, track);
}

void ClipBindingController::clearClipPanels()
{
    if (m_panels.effectControls) m_panels.effectControls->clearClip();
    if (m_panels.graphicsEditor) m_panels.graphicsEditor->clearClip();
    if (m_panels.colorGrading)   m_panels.colorGrading->clearClip();
    if (m_panels.properties)     m_panels.properties->clearClip();
}

void ClipBindingController::raiseDockFor(const Clip& clip)
{
    const ClipType type = clip.clipType();
    if (type == ClipType::Graphic)
        raiseDock(QStringLiteral("Graphics Editor"));
    else if (type == ClipType::Spine
             || (type == ClipType::Video
                 && static_cast<const VideoClip&>(clip).isVideoCharacter()))
        raiseDock(QStringLiteral("Properties"));
    else
        raiseDock(QStringLiteral("Effect Controls"));
}

void ClipBindingController::raiseDock(const QString& name)
{
    if (QDockWidget* dock = m_cfg.dockForPanel(name)) {
        dock->setVisible(true);
        dock->raise();
    }
}

void ClipBindingController::releaseClipsNotOnTimeline()
{
    std::unordered_set<const Clip*> live;
    if (const Timeline* timeline = m_cfg.timeline()) {
        for (size_t ti = 0; ti < timeline->trackCount(); ++ti) {
            const Track* track = timeline->track(ti);
            if (!track) continue;
            for (size_t ci = 0; ci < track->clipCount(); ++ci)
                live.insert(track->clip(ci));
        }
    }
    const auto gone = [&live](const Clip* clip) { return clip && !live.count(clip); };

    SelectionState& selection = *m_cfg.selection;
    if (gone(selection.clip)) {
        selection.clip = nullptr;
        selection.graphicLayerIdx = -1;
    }
    if (m_panels.properties && gone(m_panels.properties->clip()))
        m_panels.properties->clearClip();
    if (m_panels.effectControls && gone(m_panels.effectControls->clip()))
        m_panels.effectControls->clearClip();
    if (m_panels.graphicsEditor && gone(m_panels.graphicsEditor->clip()))
        m_panels.graphicsEditor->clearClip();
    if (m_panels.colorGrading && gone(m_panels.colorGrading->clip()))
        m_panels.colorGrading->clearClip();
    if (m_panels.effects && gone(m_panels.effects->clip()))
        m_panels.effects->setClip(nullptr, nullptr);
    if (m_panels.tierList && gone(m_panels.tierList->clip()))
        m_panels.tierList->setClip(nullptr, nullptr);
}

} // namespace rt
