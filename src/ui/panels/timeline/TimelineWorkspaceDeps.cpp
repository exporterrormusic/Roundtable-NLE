/*
 * TimelineWorkspaceDeps.cpp — Dependency injection setters extracted from
 * TimelineWorkspace.cpp.
 *
 * Contains: setCommandStack, setShortcutManager, setAudioEngine,
 * setPlaybackController, setMediaPool, setMediaSourceService,
 * setModelManager, setShotPresetManager, setProject,
 * animVideoCache, animVideoCacheMutable, cancelPendingDefaultLayoutReset,
 * dockForPanel.
 */

#include "panels/timeline/TimelineWorkspace.h"

#include "panels/timeline/MediaWatchController.h"

#include "CompositeService.h"
#include "spine/AnimationVideoCache.h"

#include "panels/characters/CharactersPanel.h"
#include "panels/monitors/SourceMonitor.h"
#include "panels/properties/PropertiesPanel.h"
#include "panels/timeline/TimelinePanel.h"

#include "command/CommandStack.h"
#include "audio/AudioEngine.h"
#include "audio/AudioPlaybackService.h"
#include "playback/MediaPool.h"
#include "playback/MediaSourceService.h"
#include "playback/PlaybackController.h"
#include "spine/ModelManager.h"
#include "spine/ShotPreset.h"
#include "project/Project.h"
#include "panels/audio/AudioSync.h"
#include "panels/audio/VoiceGenerationPanel.h"
#include "panels/audio/VoiceGenerationService.h"
#include "panels/project/ProjectBin.h"
#include "PathUtils.h"

#include <QDockWidget>

#include <spdlog/spdlog.h>

namespace rt {

void TimelineWorkspace::setCommandStack(CommandStack* stack) { m_commandStack = stack; }
void TimelineWorkspace::setShortcutManager(ShortcutManager* mgr) { m_shortcutManager = mgr; }

void TimelineWorkspace::setAudioEngine(AudioEngine* engine) {
    m_audioEngine = engine;
    if (m_audioPlayback) m_audioPlayback->setAudioEngine(engine);
}

void TimelineWorkspace::setPlaybackController(PlaybackController* controller) {
    m_playbackController = controller;
    if (m_audioPlayback) m_audioPlayback->setPlaybackController(controller);
}

const AnimationVideoCache* TimelineWorkspace::animVideoCache() const noexcept {
#ifdef ROUNDTABLE_HAS_SPINE
    return m_compositeService ? m_compositeService->animVideoCache() : nullptr;
#else
    return nullptr;
#endif
}

AnimationVideoCache* TimelineWorkspace::animVideoCacheMutable() noexcept {
#ifdef ROUNDTABLE_HAS_SPINE
    return m_compositeService ? m_compositeService->animVideoCache() : nullptr;
#else
    return nullptr;
#endif
}

void TimelineWorkspace::setMediaPool(MediaPool* pool) {
    if (m_mediaPool && m_mediaPool != pool) {
        // Stop new callbacks first, then wait for every worker that still owns
        // the old non-owning MediaPool pointer before replacing it.
        m_mediaPool->setOnMediaOpened({});
        cancelAndJoinBackgroundMediaWarmups();
    }
    m_mediaPool = pool;
    if (m_compositeService) m_compositeService->setMediaPool(pool);
    if (m_exportCompositeService) m_exportCompositeService->setMediaPool(pool);
    if (m_sourceMonitor)
        m_sourceMonitor->setMediaPool(pool);
#ifdef ROUNDTABLE_HAS_SPINE
    if (pool && m_compositeService)
        m_compositeService->initAnimVideoCache(pool);
    if (pool && m_exportCompositeService)
        m_exportCompositeService->initAnimVideoCache(pool);
#endif

    // Drive the live file-swap watcher from MediaPool itself: whenever the
    // pool opens ANY file (timeline clip of any subtype, bin/source preview,
    // prewarm/lookahead open) we re-arm the watcher.  This replaces the
    // brittle clip-type enumeration that missed STUCK.png (opened by the
    // shot-boundary prewarm, never via a timeline-edit hook).  The callback
    // fires on arbitrary threads; MediaWatchController coalesces and
    // marshals to the GUI thread.
    if (pool)
        pool->setOnMediaOpened(m_mediaWatch->mediaOpenedCallback());
}

void TimelineWorkspace::setMediaSourceService(MediaSourceService* service) {
    m_mediaSourceService = service;
    if (m_compositeService) m_compositeService->setMediaSourceService(service);
    if (m_exportCompositeService) m_exportCompositeService->setMediaSourceService(service);
}

void TimelineWorkspace::setModelManager(ModelManager* mgr) {
    m_modelManager = mgr;
    if (m_compositeService) m_compositeService->setModelManager(mgr);
    if (m_exportCompositeService) m_exportCompositeService->setModelManager(mgr);
    spdlog::info("TimelineWorkspace::setModelManager — mgr={}, scanned={}, "
                 "charsPanel={}",
                 static_cast<const void*>(mgr),
                 mgr ? mgr->isScanned() : false,
                 static_cast<const void*>(m_charactersPanel));
    if (m_charactersPanel) {
        m_charactersPanel->setModelManager(mgr);
        m_charactersPanel->refresh();
    }
}

void TimelineWorkspace::setShotPresetManager(ShotPresetManager* mgr) {
    m_shotPresetManager = mgr;
    if (m_compositeService) m_compositeService->setShotPresetManager(mgr);
    if (m_exportCompositeService) m_exportCompositeService->setShotPresetManager(mgr);
    if (m_propertiesPanel) m_propertiesPanel->setShotPresetManager(mgr);
}

void TimelineWorkspace::setProject(Project* project)
{
    const bool projectChanged = (m_project != project);
    m_project = project;
    if (projectChanged) {
        m_openSequenceTabs.clear();
        m_tabToSeq.clear();
        if (project) {
            for (size_t index : project->openSequenceIndices())
                m_openSequenceTabs.insert(index);
        }
    }
    if (m_compositeService) m_compositeService->setProject(project);
    if (m_audioPlayback) m_audioPlayback->setProject(project);
    if (m_sourceMonitor) m_sourceMonitor->setSequenceProject(project);
    if (m_voiceGenerationService) m_voiceGenerationService->setProject(project);
    refreshSequenceTabs();
}

void TimelineWorkspace::setVoiceGenerationService(VoiceGenerationService* service) noexcept
{
    m_voiceGenerationService = service;
    if (m_voiceGenerationService) m_voiceGenerationService->setProject(m_project);
}

void TimelineWorkspace::setVoiceScriptSource(AudioSync* audioSync) noexcept
{
    m_voiceScriptSource = audioSync;
    if (m_voiceGenerationPanel) m_voiceGenerationPanel->setAudioSync(audioSync);
}

void TimelineWorkspace::importApprovedVoiceClip(const QString& outputPath)
{
    if (outputPath.isEmpty()) return;
    if (m_projectBin) {
        m_projectBin->addFilesToNamedBin(
            {utf8ToPath(outputPath.toUtf8().toStdString())},
            QStringLiteral("Generated VO"));
    }
    if (m_project) m_project->setModified(true);
}

void TimelineWorkspace::cancelPendingDefaultLayoutReset()
{
    if (m_pendingDefaultLayoutReset) {
        spdlog::info("cancelPendingDefaultLayoutReset: clearing deferred reset");
        m_pendingDefaultLayoutReset = false;
    }
}

QDockWidget* TimelineWorkspace::dockForPanel(const QString& panelName) const
{
    return m_dockWidgets.value(panelName, nullptr);
}

// ═════════════════════════════════════════════════════════════════════════════

} // namespace rt
