# ─── UI Library (Qt 6) ────────────────────────────────────────────────────────

set(CMAKE_AUTOMOC ON)

add_library(roundtable_ui STATIC
    App.cpp
    MainWindow.cpp
    MainWindowUI.cpp
    MainWindowUIBuild.cpp
    MainWindowUINav.cpp
    MainWindowUIWorkspace.cpp
    MainWindowMenus.cpp
    ProjectController.cpp
    ProjectControllerSet.cpp
    ProjectControllerHandlers.cpp
    ProjectControllerMisc.cpp
    ProjectControllerRecovery.cpp
    ShortcutManager.cpp
    SplashScreen.cpp
    Theme.cpp
    ThemeStylesheet.cpp
    ThemeStylesheetControls.cpp
    ThemeStylesheetViews.cpp
    ThemePresets.cpp
    UiScale.cpp
    UpdateChecker.cpp
    QtHelpers.cpp

    # Panels
    panels/timeline/TrackHeader.cpp
    panels/timeline/DockBehavior.cpp
    panels/timeline/DockTabBarWatcher.cpp
    panels/timeline/DockTabDragFilter.cpp
    panels/timeline/DockEdgeOverlay.cpp
    panels/timeline/EdgeColumnGuard.cpp
    panels/timeline/DockEdgeDragWatcher.cpp
    panels/timeline/PlayheadLineWidget.cpp
    panels/timeline/TimelinePanel.cpp
    panels/timeline/TimelinePanelMenus.cpp
    panels/timeline/TimelinePanelMedia.cpp
    panels/timeline/TimelinePanelMouse.cpp
    panels/timeline/TimelinePanelMouseDrag.cpp
    panels/timeline/TimelinePanelMouseDragMove.cpp
    panels/timeline/TimelinePanelMouseDragRelease.cpp
    panels/timeline/TimelinePanelDragDrop.cpp
    panels/timeline/TimelinePanelDrop.cpp
    panels/timeline/TimelinePanelLayout.cpp
    panels/timeline/TimelinePanelTracks.cpp
    panels/timeline/TimelinePanelZoomScroll.cpp
    panels/timeline/TimelinePanelPaintEvents.cpp
    panels/timeline/TimelinePanelHitTest.cpp
    panels/timeline/TimelinePanelCommands.cpp
    panels/monitors/SourceMonitor.cpp
    panels/monitors/SourceMonitorDrag.cpp
    panels/monitors/SourceMonitorAudio.cpp
    panels/monitors/SourceMonitorUI.cpp
    panels/monitors/ProgramMonitor.cpp
    panels/monitors/ProgramMonitorUI.cpp
    panels/monitors/MonitorUiKit.cpp
    panels/project/ProjectBin.cpp
    panels/project/ProjectBinConfig.cpp
    panels/project/ProjectBinUI.cpp
    panels/project/ProjectBinSync.cpp
    panels/project/ProjectBinDragDrop.cpp
    panels/project/ProjectBinImport.cpp
    panels/project/ProjectBinQueries.cpp
    panels/project/ProjectBinEvents.cpp
    panels/project/ProjectBinSlots.cpp
    panels/project/ProjectBinSequence.cpp
    panels/properties/PropertiesPanel.cpp
    panels/properties/PropertiesPanelUI.cpp
    panels/properties/PropertiesPanelApply.cpp
    panels/properties/PropertiesPanelSpine.cpp
    panels/properties/PropertiesPanelPuppet.cpp
    panels/properties/PropertiesPanelTransitions.cpp
    panels/properties/PropertiesPanelVideo.cpp
    panels/properties/PropertiesPanelAudio.cpp
    panels/properties/PropertiesPanelTitle.cpp
    panels/properties/PropertiesPanelGraphic.cpp
    panels/properties/PropertiesPanelCaption.cpp
    panels/effects/EffectControlsPanel.cpp
    panels/effects/EffectControlsPanelUI.cpp
    panels/effects/EffectControlsPanelPropertyRow.cpp
    panels/effects/EffectControlsPanelKeyframeTimeline.cpp
    panels/effects/EffectControlsPanelTree.cpp
    panels/effects/EffectControlsPanelEffects.cpp
    panels/effects/MaskTracker.cpp
    panels/effects/GraphicsEditorPanel.cpp
    panels/effects/GraphicsEditorControls.cpp
    panels/effects/GraphicsEditorProperties.cpp
    panels/effects/ColorGradingPanel.cpp
    panels/effects/EffectsPanel.cpp
    panels/captions/CaptionsPanel.cpp
    panels/tierlist/TierListPanel.cpp
    panels/characters/ShotComposer.cpp
    panels/characters/ShotComposerVideo.cpp
    panels/characters/ShotComposerUI.cpp
    panels/characters/ShotComposerUIShotsColumn.cpp
    panels/characters/ShotComposerUILeftPanel.cpp
    panels/characters/ShotComposerProperties.cpp
    panels/characters/ShotComposerThumbnails.cpp
    panels/characters/ShotComposerThumbnailGen.cpp
    panels/characters/ShotComposerLibrary.cpp
    panels/characters/ShotComposerPreview.cpp
    panels/characters/ShotComposerShots.cpp
    panels/characters/ShotComposerLayers.cpp
    panels/characters/ShotComposerGroups.cpp
    panels/characters/ShotComposerClipboard.cpp
    panels/characters/ShotComposerEvents.cpp
    panels/audio/AudioSync.cpp
    panels/audio/AudioSyncUI.cpp
    panels/audio/AudioSyncAlign.cpp
    panels/audio/AudioSyncCards.cpp
    panels/audio/AudioSyncData.cpp
    panels/audio/AudioSyncExport.cpp
    panels/audio/AudioSyncHandlers.cpp
    panels/audio/AudioSyncPersistence.cpp
    panels/audio/AudioSyncState.cpp
    panels/audio/AudioSyncFileList.cpp
    panels/audio/AudioSyncPlayback.cpp
    panels/audio/AudioSyncPopulateCards.cpp
    panels/audio/AudioSyncSession.cpp
    panels/audio/AudioSyncResegmentAndManual.cpp
    panels/audio/AudioSyncUIScriptPage.cpp
    panels/export/ExportPanel.cpp
    panels/export/ExportPanelPresets.cpp
    panels/export/ExportPanelRun.cpp
    panels/export/ExportPanelTransport.cpp
    panels/export/ExportPanelUI.cpp
    panels/export/ExportMiniTimeline.cpp
    panels/effects/KeyframeEditor.cpp
    panels/effects/KeyframeEditorRender.cpp
    panels/project/HistoryPanel.cpp
    panels/monitors/ScopesPanel.cpp
    panels/characters/CharacterBrowser.cpp
    panels/characters/CharacterBrowserUI.cpp
    panels/characters/CharacterBrowserNetwork.cpp
    panels/characters/CharacterThumbnailCache.cpp
    panels/characters/CharacterShotPanel.cpp
    panels/characters/CharactersPanel.cpp
    panels/characters/PuppetLibrary.cpp
    panels/characters/PuppetLibraryPanel.cpp
    panels/characters/PuppetCreatorDialog.cpp
    panels/library/LibraryPanel.cpp
    panels/backgrounds/BackgroundDownloadPanel.cpp
    panels/backgrounds/BackgroundDownloadUI.cpp
    panels/project/ConversionPanel.cpp
    panels/project/ProjectPanel.cpp
    panels/project/ProjectPanelUI.cpp
    panels/project/ProjectPanelSidePanelBuild.cpp
    panels/project/ProjectPanelHelpers.cpp
    panels/project/ProjectPanelTable.cpp
    panels/project/ProjectPanelNewPanel.cpp
    panels/project/ProjectPanelSidePanel.cpp
    panels/project/ProjectPanelContextMenu.cpp
    panels/project/ProjectPanelEvents.cpp
    panels/project/ProjectPanelThumbnails.cpp
    panels/timeline/TimelineWorkspace.cpp
    panels/timeline/TimelineWorkspaceIntegration.cpp
    panels/timeline/TimelineWorkspaceEditCommands.cpp
    panels/timeline/TimelineWorkspaceDeps.cpp
    panels/timeline/TimelineWorkspaceDock.cpp
    panels/timeline/PanelMaximizeController.cpp
    panels/timeline/TimelineWorkspacePanels.cpp
    panels/timeline/TimelineWorkspacePanelsCreate.cpp
    panels/timeline/TimelineWorkspacePanelsLayout.cpp
    panels/timeline/TimelineWorkspacePanelsPlayback.cpp
    panels/timeline/ShortcutControllerRegister.cpp
    panels/timeline/TimelineWorkspaceWiring.cpp
    panels/timeline/TimelineWorkspaceWiringClipSelection.cpp
    panels/timeline/TimelineWorkspaceWiringViewport.cpp
    panels/timeline/OverlayController.cpp
    panels/timeline/OverlayControllerHandlers.cpp
    panels/timeline/TimelineWorkspaceWiringPanels.cpp
    panels/timeline/DropControllerMediaDrop.cpp
    panels/timeline/DropControllerNest.cpp
    panels/timeline/DropControllerEffectDrop.cpp
    panels/timeline/TimelineWorkspaceWiringTrack.cpp
    panels/timeline/TimelineWorkspaceComposite.cpp
    panels/timeline/TimelineWorkspaceShotSwitch.cpp
    panels/timeline/TimelineWorkspaceSpine.cpp
    panels/timeline/TimelineWorkspaceAudio.cpp
    panels/timeline/ShortcutControllerKeys.cpp
    panels/timeline/MediaWatchController.cpp
    panels/timeline/DockLayoutManager.cpp
    panels/timeline/ClipRenderers.cpp

    # Widgets
    widgets/TimelineTrackWidget.cpp
    widgets/TimelineClipWidget.cpp
    widgets/TimelineRuler.cpp
    widgets/NLEScrollBar.cpp
    widgets/TransportBar.cpp
    widgets/WaveformWidget.cpp
    widgets/MiniWaveformWidget.cpp
    widgets/VUMeter.cpp
    widgets/ColorWheelWidget.cpp
    widgets/CurveEditor.cpp
    widgets/ScrubbySpinBox.cpp
    widgets/AudioFxSection.cpp
    widgets/ThumbnailGrid.cpp
    widgets/MiniTimeline.cpp
    widgets/SpinePreviewWidget.cpp
    widgets/SpinePreviewRendering.cpp
    widgets/SpinePreviewOverlay.cpp
    widgets/FullWaveformWidget.cpp
    widgets/ManualMatchDialog.cpp
    widgets/DockTitleBar.cpp
    widgets/MediaDragTreeWidget.cpp

    # Dialogs
    dialogs/ProjectSettingsDialog.cpp
    dialogs/SequenceDialog.cpp
    dialogs/KeyboardShortcutsDialog.cpp
    dialogs/AppPreferencesDialog.cpp
    dialogs/RelinkMediaDialog.cpp
    dialogs/InterpretFootageDialog.cpp

    # Viewport
    viewport/Viewport.cpp
    viewport/VulkanViewport.cpp
    viewport/VulkanViewportPresent.cpp
    viewport/VulkanViewportSetup.cpp
    viewport/OverlayMath.cpp
    viewport/TransformOverlayWidget.cpp
    viewport/TransformOverlayDraw.cpp
    viewport/TransformOverlayMask.cpp
    viewport/TransformOverlayInput.cpp
)

# Newly-extracted TUs — skip unity batch to avoid MSVC heap exhaustion
set_source_files_properties(
    panels/project/ProjectBinConfig.cpp
    panels/project/ProjectBinImport.cpp
    panels/project/ProjectBinQueries.cpp
    panels/project/ProjectBinEvents.cpp
    panels/project/ProjectBinSlots.cpp
    panels/project/ProjectBinSequence.cpp
    panels/timeline/TimelinePanelLayout.cpp
    panels/timeline/TimelinePanelTracks.cpp
    panels/timeline/TimelinePanelZoomScroll.cpp
    panels/timeline/TimelinePanelPaintEvents.cpp
    panels/timeline/TimelinePanelHitTest.cpp
    panels/timeline/TimelinePanelCommands.cpp
    panels/timeline/DropControllerMediaDrop.cpp
    panels/timeline/DropControllerNest.cpp
    panels/timeline/DropControllerEffectDrop.cpp
    panels/timeline/TimelineWorkspaceWiringTrack.cpp
    panels/timeline/TimelineWorkspaceWiringViewport.cpp
    panels/timeline/OverlayController.cpp
    panels/timeline/OverlayControllerHandlers.cpp
    panels/timeline/TimelineWorkspaceWiringPanels.cpp
    panels/timeline/TimelineWorkspaceIntegration.cpp
    panels/timeline/TimelineWorkspaceEditCommands.cpp
    panels/timeline/TimelineWorkspaceDeps.cpp
    panels/timeline/TimelineWorkspaceDock.cpp
    panels/timeline/MediaWatchController.cpp
    panels/timeline/PanelMaximizeController.cpp
    panels/timeline/TimelineWorkspaceShotSwitch.cpp
    panels/timeline/DockTabBarWatcher.cpp
    panels/timeline/DockTabDragFilter.cpp
    panels/timeline/DockEdgeOverlay.cpp
    panels/timeline/EdgeColumnGuard.cpp
    panels/timeline/DockEdgeDragWatcher.cpp
    panels/properties/PropertiesPanelVideo.cpp
    panels/properties/PropertiesPanelAudio.cpp
    panels/properties/PropertiesPanelTitle.cpp
    panels/properties/PropertiesPanelGraphic.cpp
    panels/properties/PropertiesPanelCaption.cpp
    panels/characters/ShotComposerPreview.cpp
    panels/characters/ShotComposerShots.cpp
    panels/characters/ShotComposerLayers.cpp
    panels/characters/ShotComposerGroups.cpp
    panels/characters/ShotComposerClipboard.cpp
    panels/characters/ShotComposerEvents.cpp
    panels/characters/ShotComposerUI.cpp
    panels/characters/ShotComposerUIShotsColumn.cpp
    panels/characters/ShotComposerUILeftPanel.cpp
    panels/characters/ShotComposerLibrary.cpp
    MainWindowUIBuild.cpp
    MainWindowUINav.cpp
    MainWindowUIWorkspace.cpp
    ProjectControllerSet.cpp
    ProjectControllerHandlers.cpp
    ProjectControllerMisc.cpp
    App.cpp
    PROPERTIES SKIP_UNITY_BUILD_INCLUSION ON
)

# (AudioMixer.cpp no longer needs /EHa: the dangling m_timeline that the
#  catch(...) masked is now prevented at the root via TimelineObserver::
#  onTimelineDestroyed() nulling the pointer before the timeline is freed. #20)

# /EHa for ExportPanel.cpp — INTENTIONALLY KEPT (#20). Unlike AudioMixer's
# (now-removed) lifetime band-aid, this guards an EXTERNAL SEH, not a dangling
# pointer, so there is no lifetime fix to make:
# the main-thread composite lambda in
# pipelineComposite runs under Qt::QueuedConnection during export.  A
# raised SEH there (e.g. 0x000006BA from an injected hook DLL while the
# Vulkan ICD is mid-TDR) would otherwise propagate out of the lambda,
# escape Qt's event-loop dispatcher, and kill the process while the
# worker thread is blocked on the promise.  /EHa lets catch(...) inside
# the lambda swallow the SEH and set the promise to null — the worker
# then skips that one frame instead of deadlocking forever.
set_source_files_properties(panels/export/ExportPanel.cpp PROPERTIES COMPILE_FLAGS "/EHa")

target_include_directories(roundtable_ui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# PCH — enabled May 3, 2026.
target_precompile_headers(roundtable_ui PRIVATE pch_ui.h)

target_link_libraries(roundtable_ui PUBLIC
    roundtable_core
    roundtable_gpu
    roundtable_export
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::Network
    spdlog::spdlog
)
