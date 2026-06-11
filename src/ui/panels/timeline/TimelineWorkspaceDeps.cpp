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

#include <QMetaObject>
#include <QTimer>

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
    m_mediaPool = pool;
    if (m_compositeService) m_compositeService->setMediaPool(pool);
    if (m_sourceMonitor)
        m_sourceMonitor->setMediaPool(pool);
#ifdef ROUNDTABLE_HAS_SPINE
    if (pool && m_compositeService)
        m_compositeService->initAnimVideoCache(pool);
#endif

    // Drive the live file-swap watcher from MediaPool itself: whenever the
    // pool opens ANY file (timeline clip of any subtype, bin/source preview,
    // prewarm/lookahead open) we re-arm the watcher.  This replaces the
    // brittle clip-type enumeration that missed STUCK.png (opened by the
    // shot-boundary prewarm, never via a timeline-edit hook).  The callback
    // fires on arbitrary threads, so coalesce and marshal to the GUI thread.
    if (pool) {
        pool->setOnMediaOpened([this](std::filesystem::path) {
            if (m_destroying.load(std::memory_order_acquire)) return;
            bool expected = false;
            if (!m_mediaWatchRescanQueued.compare_exchange_strong(
                    expected, true))
                return;  // a marshal is already queued — coalesce
            // Callback fires on arbitrary decode/prewarm threads.  Marshal
            // to the GUI thread and (re)start a single-shot debounce timer
            // instead of rescanning immediately: MediaPool opens files
            // constantly during playback, and an unthrottled rescan per
            // open is a GUI-thread storm.  A real timeline edit still
            // rescans synchronously via the edit hooks; a swap of an
            // already-watched file fires fileChanged without a rescan, so
            // a ~1.5 s coalescing delay here is harmless.
            QMetaObject::invokeMethod(this, [this]() {
                m_mediaWatchRescanQueued.store(false,
                                               std::memory_order_release);
                if (m_destroying.load(std::memory_order_acquire)) return;
                if (!m_mediaWatchRescanTimer) {
                    m_mediaWatchRescanTimer = new QTimer(this);
                    m_mediaWatchRescanTimer->setSingleShot(true);
                    m_mediaWatchRescanTimer->setInterval(1500);
                    connect(m_mediaWatchRescanTimer, &QTimer::timeout, this,
                            [this]() {
                        if (m_destroying.load(std::memory_order_acquire))
                            return;
                        rescanMediaWatch();
                    });
                }
                if (!m_mediaWatchRescanTimer->isActive())
                    m_mediaWatchRescanTimer->start();
            }, Qt::QueuedConnection);
        });
    }
}

void TimelineWorkspace::setMediaSourceService(MediaSourceService* service) {
    m_mediaSourceService = service;
    if (m_compositeService) m_compositeService->setMediaSourceService(service);
}

void TimelineWorkspace::setModelManager(ModelManager* mgr) {
    m_modelManager = mgr;
    if (m_compositeService) m_compositeService->setModelManager(mgr);
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
    if (m_propertiesPanel) m_propertiesPanel->setShotPresetManager(mgr);
}

void TimelineWorkspace::setProject(Project* project)
{
    m_project = project;
    if (m_compositeService) m_compositeService->setProject(project);
    if (m_audioPlayback) m_audioPlayback->setProject(project);
    if (m_sourceMonitor) m_sourceMonitor->setSequenceProject(project);
    refreshSequenceTabs();
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
