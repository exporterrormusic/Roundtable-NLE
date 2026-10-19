/*
 * TimelineWorkspacePanelsMedia.cpp — the media docks: Project Bin, Voice
 * Generator and Library (Characters). Part of createPanelWidgets().
 */

#include "panels/timeline/TimelineWorkspace.h"

#include "CompositeService.h"
#include "panels/audio/VoiceGenerationPanel.h"
#include "panels/characters/CharactersPanel.h"
#include "panels/library/LibraryPanel.h"
#include "panels/monitors/SourceMonitor.h"
#include "panels/project/ProjectBin.h"
#include "playback/MediaPool.h"
#include "spine/AnimationVideoCache.h"

#include <QDockWidget>
#include <QMetaObject>

namespace rt {

void TimelineWorkspace::createBinDocks()
{
    // -- Project Bin ------------------------------------------------------
    m_projectBin = new ProjectBin(this);
    m_projectBin->setMinimumWidth(180);
    if (m_mediaPool)
        m_projectBin->setMediaPool(m_mediaPool);
    if (m_mediaSourceService)
        m_projectBin->setMediaSourceService(m_mediaSourceService);
    connect(m_projectBin, &ProjectBin::projectCreated,
            this, [this](Project* project) {
        if (m_destroying.load(std::memory_order_acquire)) return;
        m_projectBin->setProject(project);
        setProject(project);
        emit autoProjectCreated(project);
    });
    connect(m_projectBin, &ProjectBin::mediaRelinkRequested,
            this, &TimelineWorkspace::relinkMedia);
    makeDock("Project Bin", m_projectBin);

    // -- Voice Generator -------------------------------------------------
    // Shares one persistent service with the Audio page. Only user-approved
    // drafts are imported into Project Bin and become draggable from the list.
    if (m_voiceGenerationService) {
        m_voiceGenerationPanel = new VoiceGenerationPanel(
            m_voiceGenerationService, this);
        m_voiceGenerationPanel->setAudioSync(m_voiceScriptSource);
        m_voiceGenerationPanel->setMinimumWidth(240);
        makeDock("Voice Generator", m_voiceGenerationPanel);

        connect(m_voiceGenerationPanel,
                &VoiceGenerationPanel::approvedForProject,
                this, [this](const QString& outputPath) {
            if (m_destroying.load(std::memory_order_acquire)) return;
            importApprovedVoiceClip(outputPath);
        });
    }
}

void TimelineWorkspace::createLibraryDock()
{
    m_libraryPanel = new LibraryPanel(this);
    m_charactersPanel = m_libraryPanel->charactersPanel();
    m_charactersPanel->setModelManager(m_modelManager);
#ifdef ROUNDTABLE_HAS_SPINE
    if (m_compositeService)
        m_charactersPanel->setAnimVideoCache(m_compositeService->animVideoCache());
    if (m_compositeService && m_compositeService->animVideoCache()) {
        m_compositeService->animVideoCache()->setCompletionCallback(
            [panel = m_charactersPanel](const std::string&, const std::string&,
                                         const std::string&, bool) {
                QMetaObject::invokeMethod(panel, &CharactersPanel::refresh,
                                          Qt::QueuedConnection);
            });
    }
#endif
    m_charactersPanel->setMediaPool(m_mediaPool);
    m_libraryPanel->refreshCurrentTab();
    connect(m_libraryPanel, &LibraryPanel::mediaRelinkRequested,
            this, &TimelineWorkspace::relinkMedia);
    connect(m_libraryPanel, &LibraryPanel::loadInSourceMonitor,
            this, [this](const QString& filePath) {
        if (m_destroying.load(std::memory_order_acquire)) return;
        if (!m_sourceMonitor || !m_mediaPool || filePath.isEmpty()) return;
        const auto handle = m_mediaPool->open(filePath.toStdString());
        if (handle == 0) return;
        m_sourceMonitor->loadClip(handle, m_mediaPool);
        // Make the Source Monitor the transport target (next Space plays it).
        setSourceTransportActive(true);
    });
    makeDock("Library", m_libraryPanel);
}

} // namespace rt
