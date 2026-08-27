/*
 * MainWindowUIBuild.cpp — buildPanels() extracted from MainWindowUI.cpp.
 *
 * Contains: buildPanels() — creates all 5 top-level pages (PROJECTS,
 * CHARACTERS, AUDIO, TIMELINE, EXPORT), wires all cross-panel signals,
 * registers playback-resolution shortcuts, and kicks off projects-list
 * population.
 */

#include "MainWindow.h"
#include "PathUtils.h"
#include "ProjectController.h"
#include "ShortcutManager.h"

// Composite service (for modal-dialog compositor suppression)
#include "CompositeService.h"

// Pages / panels
#include "panels/audio/AudioSync.h"
#include "panels/audio/VoiceGenerationPanel.h"
#include "panels/audio/VoiceGenerationService.h"
#include "panels/characters/CharacterBrowser.h"
#include "panels/characters/CharacterShotPanel.h"
#include "panels/export/ExportPanel.h"
#include "panels/project/ProjectPanel.h"
#include "panels/characters/ShotComposer.h"
#include "panels/timeline/TimelineWorkspace.h"
#include "panels/library/LibraryPanel.h"
#include "panels/backgrounds/BackgroundDownloadPanel.h"

// Spine cache warming after export
#include "spine/AnimationVideoCache.h"
#include "timeline/Track.h"
#include "timeline/Clip.h"
#include "timeline/SpineClip.h"

// Delegated panel headers (for accessor forwarding)
#include "panels/effects/EffectsPanel.h"
#include "panels/project/HistoryPanel.h"
#include "panels/effects/KeyframeEditor.h"
#include "panels/monitors/ProgramMonitor.h"
#include "playback/PlaybackScheduler.h"
#include "viewport/Viewport.h"
#include "panels/project/ProjectBin.h"
#include "panels/properties/PropertiesPanel.h"
#include "panels/effects/EffectControlsPanel.h"
#include "panels/monitors/SourceMonitor.h"
#include "panels/timeline/TimelinePanel.h"

// Core
#include "command/CommandStack.h"
#include "audio/AudioEngine.h"
#include "playback/PlaybackController.h"
#include "playback/MediaPool.h"
#include "playback/MediaSourceService.h"
#include "spine/ModelManager.h"
#include "timeline/Timeline.h"
#include "timeline/Track.h"
#include "timeline/AudioClip.h"
#include "timeline/Clip.h"
#include "timeline/SpineClip.h"
#include "timeline/VideoClip.h"

#include "project/Project.h"
#include "project/ProjectSerializer.h"
#include "spine/ShotPreset.h"

#include <QAction>
#include <QActionGroup>
#include <QDir>
#include <QDockWidget>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QInputDialog>
#include <QLineEdit>
#include <QMenuBar>
#include <QMessageBox>
#include <QProcess>
#include <QProgressBar>
#include <QTemporaryDir>
#include <QWindow>
#include <QScreen>
#include <QStyle>
#include <QStackedWidget>
#include <QStatusBar>
#include <QButtonGroup>
#include <QPixmap>
#include <QPushButton>
#include <QSettings>
#include <QTimer>

#include "Settings.h"

#include <filesystem>
#include <map>
#include <set>

namespace rt {

// ═════════════════════════════════════════════════════════════════════════════
// Panel / page creation
// ═════════════════════════════════════════════════════════════════════════════

void MainWindow::buildPanels()
{
    if (m_panelsBuilt) return;

    // Suppress paint events during widget construction to prevent
    // paint -> layout -> repaint recursion while geometry is inconsistent.
    setUpdatesEnabled(false);

    spdlog::info("MainWindow::buildPanels() — creating tabbed pages");

    // ── Page 0: PROJECTS ────────────────────────────────────────────────
    m_projectPanel = new ProjectPanel(this);
    m_pageStack->addWidget(m_projectPanel);

    // ── Page 1: CHARACTERS (combined CharacterBrowser + ShotComposer) ────
    m_characterShotPanel = new CharacterShotPanel(this);
    if (m_modelManager) m_characterShotPanel->setModelManager(m_modelManager);
    {
        // Resolve shot presets directory relative to the application folder.
        // In the dev tree this walks up from build/bin/Release/ to the
        // project root; when installed it uses {app}\assets\presets\shots\.
        // The installer creates this with users-modify permissions so users
        // can save their own presets.
        std::filesystem::path presetsDir;
        QString appDir = QCoreApplication::applicationDirPath();
        QDir dir(appDir);
        bool found = false;
        for (int i = 0; i < 5; ++i) {
            QString candidate = dir.absoluteFilePath("assets/presets/shots");
            if (QDir(candidate).exists()) {
                presetsDir = candidate.toStdString();
                found = true;
                break;
            }
            dir.cdUp();
        }
        if (!found)
            presetsDir = (appDir + "/assets/presets/shots").toStdString();
        std::filesystem::create_directories(presetsDir);
        m_characterShotPanel->setPresetsDirectory(presetsDir);
    }
    m_pageStack->addWidget(m_characterShotPanel);

    // ── Page 2: AUDIO ───────────────────────────────────────────────────
    m_voiceGenerationService = new VoiceGenerationService(this);
    m_audioSync = new AudioSync(this);
    if (m_commandStack) m_audioSync->setCommandStack(m_commandStack);
    if (m_audioEngine) m_audioSync->setAudioEngine(m_audioEngine);
    m_voiceGenerationPanel = new VoiceGenerationPanel(
        m_voiceGenerationService, true, this);
    m_voiceGenerationPanel->setAudioSync(m_audioSync);
    m_audioSync->setVoiceGenerationPanel(m_voiceGenerationPanel);
    m_pageStack->addWidget(m_audioSync);

    // Give AudioSync access to the ShotPresetManager for default shot lookup
    m_audioSync->setShotPresetManager(&m_characterShotPanel->shotComposer()->presetManager());

    // When a shot is saved in COMPOSE, refresh the Properties panel's shot
    // dropdown so it immediately reflects the new/updated preset.
    if (auto* tw = m_timelineWorkspace) {
        connect(m_characterShotPanel->shotComposer(), &ShotComposer::shotChanged,
                tw, [tw]() {
            if (auto* pp = tw->propertiesPanel())
                pp->refreshShotDropdown();
        });
    }

    // When a script is loaded, refresh the shot list so any newly-referenced
    // characters appear in the COMPOSE character filter (if they are downloaded
    // or have existing user-created shots).  Do NOT auto-create default shots.
    connect(m_audioSync, &AudioSync::scriptLoaded, this, [this](int /*lineCount*/) {
        if (m_destroying.load(std::memory_order_acquire)) return;
        if (m_characterShotPanel && m_characterShotPanel->shotComposer() && m_audioSync) {
            m_characterShotPanel->shotComposer()->refreshShotList();
        }
    });

    // Stable per-project key for the re-export QSettings entries (path of
    // a saved project; falls back to the project name for unsaved ones).
    auto audioSyncProjectKey = [this]() -> QString {
        if (!m_currentProject) return {};
        const auto& p = m_currentProject->filePath();
        if (!p.empty()) return QString::fromStdString(pathToUtf8(p));
        return QString::fromStdString(m_currentProject->name());
    };

    // Shared post-export steps for both "Export →" and "Re-export":
    // refresh the timeline UI, warm the Spine cache, force audio decode,
    // register the VO bin entries and land on the Timeline page.
    auto finishAudioSyncExport = [this](Timeline* targetTimeline, int count,
                                        bool quiet) {
        // Refresh the timeline UI. Use refreshTrackContents for lightweight
        // update — audio export may have added new tracks, but rebuildTracks
        // resets all track heights which the user has manually adjusted.
        if (m_timelineWorkspace && m_timelineWorkspace->timelinePanel()) {
            auto* panel = m_timelineWorkspace->timelinePanel();
            panel->rebuildTracks();
            panel->notifyZoomChanged();
        }

        // ── Warm the Spine animation cache ─────────────────────────────
        // Proactively queue pre-renders for unique character/outfit pairs
        // on the timeline, so the timeline turns green (cached) immediately
        // instead of staying orange (uncached).  Cap at 10 pairs to avoid
        // flooding the cache worker with hundreds of jobs in large projects.
        static constexpr int kMaxWarmChars = 10;
        if (auto* cache = m_timelineWorkspace
                ? m_timelineWorkspace->animVideoCacheMutable() : nullptr) {
            std::set<std::pair<std::string, std::string>> queuedChars;
            bool capped = false;
            for (size_t ti = 0; ti < targetTimeline->trackCount() && !capped; ++ti) {
                auto* track = targetTimeline->track(ti);
                if (!track || track->type() != TrackType::Video) continue;
                for (size_t ci = 0; ci < track->clipCount() && !capped; ++ci) {
                    auto* clip = track->clip(ci);
                    if (!clip || clip->clipType() != ClipType::Spine) continue;
                    auto* sc = static_cast<SpineClip*>(clip);
                    auto key = std::make_pair(sc->characterName(),
                                               sc->outfit());
                    if (queuedChars.insert(key).second) {
                        if (queuedChars.size() > static_cast<size_t>(kMaxWarmChars)) {
                            spdlog::debug("Spine cache warm: capped at {}, "
                                          "remaining chars warmed on first use",
                                          kMaxWarmChars);
                            capped = true;
                            break;
                        }
                        cache->queueAllAnimations(sc->characterName(),
                                                   sc->outfit());
                    }
                }
            }
        }

        // Force a full blocking audio reload so every clip is decoded
        // and cached before the user presses Play.  Without this, the
        // first playback after export would hit cache misses for most
        // clips and they'd be silent.  Do NOT invalidate first — that
        // clears cached audio sources and triggers recomposite cascades.
        if (m_timelineWorkspace) {
            m_timelineWorkspace->ensureAudioSourcesLoaded();
        }

        // Add synced audio clips to the bin in a single "VO" folder
        if (auto* bin = projectBin()) {
            // Collect all confirmed audio file paths (deduplicated)
            std::set<std::string> seen;
            std::vector<std::filesystem::path> allAudioPaths;
            for (int i = 0; i < m_audioSync->clipCount(); ++i) {
                const auto& sc = m_audioSync->clip(i);
                if (sc.matchState != 2 || sc.sourceFile.empty()) continue;
                std::filesystem::path p(sc.sourceFile);
                if (seen.insert(pathToUtf8(p)).second)
                    allAudioPaths.push_back(p);
            }
            // Drop everything into a single root-level "VO" folder. Don't
            // call ensureDefaultBins() here — the user wants just one
            // folder created on export, not a full Premiere-style scaffold.
            bin->addFilesToNamedBin(allAudioPaths,
                QStringLiteral("VO"), QString());
            bin->refreshSequences();
        }

        // Switch to the Timeline page
        setCurrentPage(Page::Timeline);

        if (quiet) {
            statusBar()->showMessage(
                tr("Re-exported %1 audio clip(s) — timeline updated in place.")
                    .arg(count), 6000);
        } else {
            QMessageBox::information(this, "Export Complete",
                QString("Exported %1 audio clip(s) to the timeline.").arg(count));
        }
    };

    // ── Export to Timeline: wire AudioSync → Timeline population ────────
    connect(m_audioSync, &AudioSync::exportRequested, this,
            [this, audioSyncProjectKey, finishAudioSyncExport]() {
        if (m_destroying.load(std::memory_order_acquire)) return;
        if (!m_audioSync || !m_currentProject) return;

        // Suppress GPU compositing during modal dialogs to prevent NVIDIA
        // driver stack overflow from paint events during QDialog::exec.
        struct ModalGuard {
            ~ModalGuard() { CompositeService::setModalDialogActive(false); }
        };
        CompositeService::setModalDialogActive(true);
        ModalGuard modalGuard;

        // ── Show picker: choose which show's default shots to use ───────
        // Default-shot resolution is per-show, so let the user pick which
        // show's defaults drive this export instead of silently falling back
        // to whichever show happens to resolve first.
        if (auto* mgr = m_audioSync->shotPresetManager()) {
            const auto& shows = mgr->knownShows();
            if (shows.size() > 1) {
                QStringList showItems;
                for (const auto& s : shows)
                    showItems << QString::fromStdString(s);

                // Pre-select the show this project is associated with; fall
                // back to AudioSync's current show if the project has none.
                int curIdx = 0;
                QString preferred = QString::fromStdString(m_currentProject->show());
                if (preferred.isEmpty())
                    preferred = QString::fromStdString(m_audioSync->currentShow());
                int found = showItems.indexOf(preferred);
                if (found >= 0) curIdx = found;

                bool ok = false;
                QString chosen = QInputDialog::getItem(
                    this, tr("Export — Show"),
                    tr("Use default shots from which show?"),
                    showItems, curIdx, /*editable*/ false, &ok);
                if (!ok) return;  // user cancelled the export
                m_audioSync->setCurrentShow(chosen.toStdString());
            } else if (shows.size() == 1) {
                m_audioSync->setCurrentShow(shows.front());
            }
        }

        // ── Warn about missing default shots ───────────────────────────
        {
            QStringList missingDefault = m_audioSync->missingDefaultShots();
            if (!missingDefault.isEmpty()) {
                auto reply = QMessageBox::warning(
                    this, tr("Missing Default Shots"),
                    tr("The following characters have no default shot set:\n\n  %1\n\n"
                       "Visual layers (Spine/background) will be missing for these characters "
                       "in the exported timeline.\n\n"
                       "Open Compose, save a shot, and click \"SET DEFAULT\" to fix.\n\n"
                       "Continue with export anyway?")
                        .arg(missingDefault.join(QStringLiteral("\n  "))),
                    QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
                if (reply != QMessageBox::Yes) return;
            }
        }

        // ── Sequence picker: choose an existing sequence or create a new ──
        // one.  Always shown (even with a single sequence) so the user can
        // route the export wherever they want, or start a fresh sequence.
        Timeline* targetTimeline = nullptr;
        {
            const QString kCreateNew =
                QStringLiteral("+  Create new sequence...");
            QStringList items;
            const int activeIdx =
                static_cast<int>(m_currentProject->activeSequenceIndex());
            for (size_t i = 0; i < m_currentProject->sequenceCount(); ++i) {
                auto* seq = m_currentProject->sequence(i);
                items << (seq ? QString::fromStdString(seq->name())
                              : QStringLiteral("Sequence %1").arg(i + 1));
            }
            const int createRow = items.size();
            items << kCreateNew;

            bool ok = false;
            QString chosen = QInputDialog::getItem(
                this, "Export to Timeline",
                "Choose the sequence to export into:",
                items, activeIdx, /*editable=*/false, &ok);
            if (!ok) return;

            const int chosenRow = items.indexOf(chosen);
            if (chosen == kCreateNew || chosenRow == createRow) {
                // Prompt for a name, then create + switch to a fresh sequence.
                bool nameOk = false;
                const QString defaultName =
                    QString::fromStdString(m_currentProject->nextSequenceName());
                QString name = QInputDialog::getText(
                    this, "New Sequence", "Sequence name:",
                    QLineEdit::Normal, defaultName, &nameOk);
                if (!nameOk) return;
                if (name.trimmed().isEmpty()) name = defaultName;

                targetTimeline =
                    m_currentProject->addSequence(name.toStdString());
                if (!targetTimeline) return;
                // switchSequence() makes it active and refreshes tabs/panels.
                switchSequence(m_currentProject->sequenceCount() - 1);
            } else {
                const size_t idx = static_cast<size_t>(
                    chosenRow < 0 ? activeIdx : chosenRow);
                targetTimeline = m_currentProject->sequence(idx);
                if (idx != m_currentProject->activeSequenceIndex())
                    switchSequence(idx);
            }
        }
        if (!targetTimeline) return;

        int count = m_audioSync->exportToTimeline(targetTimeline);
        if (count == AudioSync::kExportBlockedByLockedTrack) {
            QMessageBox::warning(this, "Locked Tracks",
                m_audioSync->lastExportError().isEmpty()
                    ? tr("Audio Sync cannot update this sequence while an export-owned track is locked.")
                    : m_audioSync->lastExportError());
            return;
        }
        if (count == 0) {
            QMessageBox::warning(this, "No Confirmed Clips",
                "No confirmed clips to export.\n"
                "Use the \xe2\x9c\x93 button to confirm clips before exporting.");
            return;
        }

        // Remember this export so the one-click "Re-export" button can
        // repeat it (same show, same sequence) with zero prompts.
        {
            QString seqName;
            for (size_t i = 0; i < m_currentProject->sequenceCount(); ++i) {
                auto* seq = m_currentProject->sequence(i);
                if (seq == targetTimeline) {
                    seqName = QString::fromStdString(seq->name());
                    break;
                }
            }
            QSettings settings;
            settings.setValue("AudioSyncReExport/project", audioSyncProjectKey());
            settings.setValue("AudioSyncReExport/show",
                              QString::fromStdString(m_audioSync->currentShow()));
            settings.setValue("AudioSyncReExport/sequence", seqName);
        }

        finishAudioSyncExport(targetTimeline, count, /*quiet=*/false);
    });

    // ── Re-export: repeat the last export with zero prompts ─────────────
    // Uses the show + sequence remembered by the last "Export →" for this
    // project.  exportToTimeline() runs incrementally when the sequence
    // already holds back-linked clips, so user timeline edits survive.
    connect(m_audioSync, &AudioSync::reExportRequested, this,
            [this, audioSyncProjectKey, finishAudioSyncExport]() {
        if (m_destroying.load(std::memory_order_acquire)) return;
        if (!m_audioSync || !m_currentProject) return;

        QSettings settings;
        const QString storedProj =
            settings.value("AudioSyncReExport/project").toString();
        const QString storedShow =
            settings.value("AudioSyncReExport/show").toString();
        const QString storedSeq =
            settings.value("AudioSyncReExport/sequence").toString();

        Timeline* targetTimeline = nullptr;
        size_t targetIdx = 0;
        if (!storedSeq.isEmpty() && !storedProj.isEmpty()
            && storedProj == audioSyncProjectKey()) {
            for (size_t i = 0; i < m_currentProject->sequenceCount(); ++i) {
                auto* seq = m_currentProject->sequence(i);
                if (seq && QString::fromStdString(seq->name()) == storedSeq) {
                    targetTimeline = seq;
                    targetIdx = i;
                    break;
                }
            }
        }
        if (!targetTimeline) {
            QMessageBox::information(this, tr("Re-export"),
                tr("No previous export recorded for this project.\n"
                   "Use \"Export →\" once — after that, Re-export repeats "
                   "it into the same sequence with no prompts."));
            return;
        }

        if (!storedShow.isEmpty())
            m_audioSync->setCurrentShow(storedShow.toStdString());

        // No modal gauntlet: missing default shots only log here.
        {
            const QStringList missing = m_audioSync->missingDefaultShots();
            if (!missing.isEmpty())
                spdlog::warn("Re-export: {} character(s) have no default shot "
                             "set: {}", missing.size(),
                             missing.join(QStringLiteral(", ")).toStdString());
        }

        if (targetIdx != m_currentProject->activeSequenceIndex())
            switchSequence(targetIdx);

        const int count = m_audioSync->exportToTimeline(targetTimeline);
        if (count == AudioSync::kExportBlockedByLockedTrack) {
            QMessageBox::warning(this, "Locked Tracks",
                m_audioSync->lastExportError().isEmpty()
                    ? tr("Audio Sync cannot update this sequence while an export-owned track is locked.")
                    : m_audioSync->lastExportError());
            return;
        }
        if (count == 0) {
            QMessageBox::warning(this, "No Confirmed Clips",
                "No confirmed clips to export.\n"
                "Use the \xe2\x9c\x93 button to confirm clips before exporting.");
            return;
        }
        finishAudioSyncExport(targetTimeline, count, /*quiet=*/true);
    });

    // ── Page 3: TIMELINE (splitter-based layout) ────────────────────────
    m_timelineWorkspace = new TimelineWorkspace(this);
    m_timelineWorkspace->setCommandStack(m_commandStack);
    m_timelineWorkspace->setShortcutManager(m_shortcutManager);
    m_timelineWorkspace->setAudioEngine(m_audioEngine);
    m_timelineWorkspace->setPlaybackController(m_playbackController);
    // MediaPool must be set BEFORE timeline so that AnimationVideoCache
    // is available when preloadSpineAssets() runs — otherwise every
    // SpineClip loads its skeleton+atlas even when pre-rendered video
    // exists (wasting 50+ seconds and ~2 GB RAM).
    m_timelineWorkspace->setMediaPool(m_mediaPool);
    m_timelineWorkspace->setMediaSourceService(m_mediaSourceService);
    m_timelineWorkspace->setModelManager(m_modelManager);
    m_timelineWorkspace->setVoiceGenerationService(m_voiceGenerationService);
    m_timelineWorkspace->setVoiceScriptSource(m_audioSync);
    m_timelineWorkspace->setTimeline(m_timeline);
    m_timelineWorkspace->buildPanels();
    connect(m_voiceGenerationPanel,
            &VoiceGenerationPanel::approvedForProject,
            m_timelineWorkspace,
            &TimelineWorkspace::importApprovedVoiceClip);

    // Propagate GPU display mode from ProgramMonitor → TimelineWorkspace
    // so compositeFrame() can skip CPU readback when VulkanViewport is active.
    if (auto* pm = m_timelineWorkspace->programMonitor())
        m_timelineWorkspace->setGpuDisplayMode(pm->isGpuDisplayEnabled());

    // Wire ShotPresetManager to TimelineWorkspace for video character fallback
    if (m_characterShotPanel && m_characterShotPanel->shotComposer())
        m_timelineWorkspace->setShotPresetManager(&m_characterShotPanel->shotComposer()->presetManager());

    // Wire ShotPresetManager to PropertiesPanel for shot switching
    if (m_timelineWorkspace->propertiesPanel() && m_characterShotPanel && m_characterShotPanel->shotComposer()) {
        m_timelineWorkspace->propertiesPanel()->setShotPresetManager(
            &m_characterShotPanel->shotComposer()->presetManager());
    }

    // Wire animation video cache to ShotComposer for character cache indicators
    if (m_characterShotPanel && m_characterShotPanel->shotComposer())
        m_characterShotPanel->shotComposer()->setAnimVideoCache(m_timelineWorkspace->animVideoCache());

    // Wire animation video cache to CharacterBrowser for conversion badges + cache deletion
    if (m_characterShotPanel && m_characterShotPanel->characterBrowser())
        m_characterShotPanel->characterBrowser()->setAnimVideoCache(m_timelineWorkspace->animVideoCacheMutable());

    // Wire animation video cache (non-const) to ConversionPanel for manual conversion
    if (m_characterShotPanel)
        m_characterShotPanel->setAnimVideoCache(m_timelineWorkspace->animVideoCacheMutable());

    // When a character is downloaded, refresh the Library panel's CharactersPanel tree
    if (m_characterShotPanel && m_characterShotPanel->characterBrowser() && m_timelineWorkspace) {
        connect(m_characterShotPanel->characterBrowser(), &CharacterBrowser::downloadRequested,
                this, [this]() {
            if (m_destroying.load(std::memory_order_acquire)) return;
            if (m_timelineWorkspace && m_timelineWorkspace->libraryPanel())
                m_timelineWorkspace->libraryPanel()->refreshCurrentTab();
        });
    }

    // When Nikke backgrounds are downloaded, refresh the COMPOSE background library
    if (m_timelineWorkspace && m_timelineWorkspace->libraryPanel()
        && m_timelineWorkspace->libraryPanel()->nikkeBgsPanel()
        && m_characterShotPanel && m_characterShotPanel->shotComposer()) {
        connect(m_timelineWorkspace->libraryPanel()->nikkeBgsPanel(),
                &BackgroundDownloadPanel::backgroundsDownloaded,
                this, [this]() {
            if (m_destroying.load(std::memory_order_acquire)) return;
            if (m_characterShotPanel && m_characterShotPanel->shotComposer())
                m_characterShotPanel->shotComposer()->refreshBackgroundLibrary();
        });
    }

    m_pageStack->addWidget(m_timelineWorkspace);

    // ── Wire ProjectBin sequence signals ────────────────────────────────
    if (auto* bin = m_timelineWorkspace->projectBin()) {
        connect(bin, &ProjectBin::sequenceOpened, this,
                [this](size_t idx) { switchSequence(idx); });
        connect(bin, &ProjectBin::sequencesChanged, this, [this]() {
            if (m_destroying.load(std::memory_order_acquire)) return;
            if (!m_currentProject) return;
            // After add/delete/duplicate, ensure the active timeline is still valid
            Timeline* active = m_currentProject->sequence(m_currentProject->activeSequenceIndex());
            if (active && active != m_timeline) {
                // Active sequence changed — switch to it
                m_timeline = active;
                if (m_timelineWorkspace) m_timelineWorkspace->setTimeline(active);
                if (m_playbackController) m_playbackController->setTimeline(active);
                if (m_exportPanel) {
                    m_exportPanel->setTimeline(active);
                    m_exportPanel->setProject(m_currentProject.get());
                }
                if (m_timelineWorkspace && m_timelineWorkspace->timelinePanel())
                    m_timelineWorkspace->timelinePanel()->rebuildTracks();
                if (auto* pm = programMonitor()) pm->refresh();
            }
            // Refresh the bin list and sequence tabs
            if (auto* b = projectBin()) b->refreshSequences();
            if (m_timelineWorkspace)
                m_timelineWorkspace->refreshSequenceTabs();
        });
        connect(bin, &ProjectBin::sequenceSettingsChanged, this, [this]() {
            if (m_destroying.load(std::memory_order_acquire)) return;
            if (!m_currentProject) return;
            const auto& s = m_currentProject->settings();
            if (auto* pm = programMonitor()) {
                pm->setOutputResolution(s.resolution().width, s.resolution().height);
                pm->requestRefresh();
            }
            if (auto* ec = effectControlsPanel())
                ec->setSequenceResolution(s.resolution().width, s.resolution().height);
            if (m_playbackController)
                m_playbackController->setFrameRate(s.frameRate());
            if (m_timelineWorkspace && m_timelineWorkspace->timelinePanel())
                m_timelineWorkspace->timelinePanel()->setFrameRate(s.frameRate());
            if (auto* b = projectBin()) b->refreshSequences();
            if (m_timelineWorkspace)
                m_timelineWorkspace->refreshSequenceTabs();
        });
        connect(bin, &ProjectBin::nestSequenceRequested,
                this, [this](size_t seqIdx, const QString& seqName) {
            if (m_destroying.load(std::memory_order_acquire)) return;
            if (!m_currentProject || !m_timeline || !m_timelineWorkspace) return;
            // Prevent circular nesting: don't nest active sequence into itself
            if (seqIdx == m_currentProject->activeSequenceIndex()) return;
            m_timelineWorkspace->nestSequence(seqIdx, seqName);
        });

        // Propagated clip removal from the bin → refresh timeline views and
        // audio/composite caches (keeps playback consistent with the model).
        connect(bin, &ProjectBin::timelineClipsMutated, this, [this]() {
            if (m_destroying.load(std::memory_order_acquire)) return;
            if (m_timelineWorkspace) {
                if (auto* tp = m_timelineWorkspace->timelinePanel())
                    tp->rebuildTracks();
                m_timelineWorkspace->invalidateAudioSources();
                m_timelineWorkspace->refreshAfterUndoRedo();
            }
            if (auto* pm = programMonitor()) pm->refresh();
        });

        // A bin asset's bytes changed on disk (e.g. a recoloured Color
        // Matte) → re-decode it and refresh every timeline instance.
        connect(bin, &ProjectBin::mediaContentChanged, this,
                [this](const std::filesystem::path& path) {
            if (m_destroying.load(std::memory_order_acquire)) return;
            if (m_timelineWorkspace)
                m_timelineWorkspace->refreshChangedMedia(path);
            if (auto* pm = programMonitor()) pm->refresh();
        });

        // Double-click (or drag) media items → load in Source Monitor
        connect(bin, &ProjectBin::loadInSourceMonitor,
                this, [this](const std::filesystem::path& filePath, uint64_t mediaHandle) {
            if (m_destroying.load(std::memory_order_acquire)) return;
            auto* sm = sourceMonitor();
            if (!sm || !m_mediaPool) return;

            // The bin may hand us a stale or 0 handle (e.g. the grid had no
            // current selection).  loadClip() silently no-ops on an invalid
            // handle, which looks like "double-click does nothing", so fall
            // back to (re)opening the file by path.
            uint64_t handle = mediaHandle;
            if ((handle == 0 || !m_mediaPool->isValid(handle)) && !filePath.empty())
                handle = m_mediaPool->open(filePath);
            if (handle == 0 || !m_mediaPool->isValid(handle)) {
                spdlog::warn("loadInSourceMonitor: no valid media handle for '{}'",
                             pathToUtf8(filePath));
                return;
            }

            sm->loadClip(handle, m_mediaPool);

            // Opening a clip here makes the Source Monitor the transport
            // target so the next Space plays it (not the timeline).
            if (m_timelineWorkspace) {
                m_timelineWorkspace->setSourceTransportActive(true);
                // Bring the Source Monitor to the front (it may be tabbed
                // behind the Program Monitor) so the clip is actually visible.
                if (auto* dock = m_timelineWorkspace->dockForPanel(
                        QStringLiteral("Source Monitor"))) {
                    dock->show();
                    dock->raise();
                }
            }
        });
    }

    // ── Drive the loading progress bar + release the input lock on warmup ──
    connect(m_timelineWorkspace, &TimelineWorkspace::backgroundWarmupProgress,
            this, &MainWindow::setLoadingProgress);
    connect(m_timelineWorkspace, &TimelineWorkspace::backgroundWarmupFinished,
            this, &MainWindow::onBackgroundWarmupFinished);

    // ── Wire sequence tab bar to sequence switching ─────────────────────
    connect(m_timelineWorkspace, &TimelineWorkspace::sequenceTabChanged,
            this, [this](size_t idx) { switchSequence(idx); });

    // ── Wire sequence tab settings request ──────────────────────────────
    connect(m_timelineWorkspace, &TimelineWorkspace::sequenceTabSettingsRequested,
            this, &MainWindow::onSequenceSettingsRequested);

    // ── Wire auto-create project on media drop ──────────────────────────
    connect(m_timelineWorkspace, &TimelineWorkspace::requestNewProjectForMedia,
            this, &MainWindow::onNewProjectForMedia);

    // ── Wire auto-created project from sequence creation ────────────────
    connect(m_timelineWorkspace, &TimelineWorkspace::autoProjectCreated,
            this, [this](Project* project) {
        if (m_destroying.load(std::memory_order_acquire)) return;
        // Disconnect old timeline from all consumers BEFORE destroying
        // the old project, otherwise dangling-pointer access will crash.
        if (m_timelineWorkspace)
            m_timelineWorkspace->setTimeline(nullptr);
        if (m_playbackController)
            m_playbackController->setTimeline(nullptr);
        if (m_exportPanel)
            m_exportPanel->setTimeline(nullptr);
        m_timeline = nullptr;

        // Save the auto-created project to disk so it survives swaps and
        // can be reopened from the PROJECTS page later.  Without this, an
        // auto-created project (from Project Bin "New Sequence" with no
        // project open) is only kept in memory — if the user switches to
        // another project, the auto-created one is gone forever.
        {
            QString projName = QString::fromStdString(project->name());
            QString projDir = projectsDirectory();
            QString projectFolder = projDir + "/" + projName;
            QDir().mkpath(projectFolder);
            std::filesystem::path savePath =
                (projectFolder + "/" + projName + ".rtp").toStdWString();
            project->setFilePath(savePath);
            ProjectSerializer serializer;
            if (serializer.save(*project, savePath)) {
                spdlog::info("Auto-created project saved to: {}",
                             pathToUtf8(savePath));
                addToRecentFiles(QString::fromStdString(pathToUtf8(savePath)));
            } else {
                spdlog::warn("Failed to save auto-created project '{}'",
                             projName.toStdString());
            }
        }

        // MainWindow takes ownership of the auto-created project
        m_currentProject.reset(project);
        m_projectController->clearLastSavedAudioSyncBlob();
        if (m_currentProject->timeline()) {
            m_timeline = m_currentProject->timeline();
            if (m_timelineWorkspace)
                m_timelineWorkspace->setTimeline(m_timeline);
            if (m_playbackController)
                m_playbackController->setTimeline(m_timeline);
            if (m_exportPanel)
                m_exportPanel->setTimeline(m_timeline);
        }
        QString name = QString::fromStdString(m_currentProject->name());
        setWindowTitle(QString("ROUNDTABLE NLE %1 \u2014 %2")
                       .arg(ROUNDTABLE_VERSION).arg(name));
        if (auto* bin = projectBin())
            bin->setProjectName(name);
    });

    // ── Page 4: EXPORT ──────────────────────────────────────────────────
    m_exportPanel = new ExportPanel(this);
    if (m_timeline) m_exportPanel->setTimeline(m_timeline);
    if (m_currentProject) m_exportPanel->setProject(m_currentProject.get());
    if (m_playbackController) m_exportPanel->setPlaybackController(m_playbackController);
    if (m_audioEngine) m_exportPanel->setAudioEngine(m_audioEngine);
    if (m_commandStack) m_exportPanel->setCommandStack(m_commandStack);
    m_exportPanel->setPreviewCallback(
        [this](int64_t tick, uint32_t w, uint32_t h, bool scrub)
            -> std::shared_ptr<CachedFrame> {
            if (m_destroying.load(std::memory_order_acquire)) return nullptr;
            if (m_timelineWorkspace) {
                // Phase 4.2 — export 16F passthrough: if this tick is a single
                // full-frame opaque >8-bit clip, return a dual-payload frame
                // (RGBA16F for the 10-bit encoder + dithered 8-bit BGRA for
                // this preview).  RenderQueue routes depth==16 frames to
                // encodeFrame16f; on any miss this returns null and we fall
                // through to the normal 8-bit composite below.
                if (auto pf = m_timelineWorkspace->compositeFrame16f(tick, w, h))
                    return pf;   // single-clip passthrough already preserves source alpha

                // Alpha export (Phase 4.2): straight-alpha (transparent) composite
                // when the user ticked "Export with alpha" on an alpha-capable
                // target (ProRes 4444 / PNG).  Set only around the composite and
                // reset after, so nothing else sees the straight-alpha mode.
                const bool wantAlpha = m_exportPanel && m_exportPanel->exportAlphaRequested();

                // Force Full resolution for export preview (characters too).
                // This is always reset on the next call, and the Program/Source
                // Monitors never go through this callback.  (forceFull also lets
                // compositeFrame's consult REUSE pre-rendered Full-tier segments
                // during export — §4.6 slice 3 read side.)
                m_timelineWorkspace->setForceFullResolution(true);
                m_timelineWorkspace->setExportAlpha(wantAlpha);
                // Export preview/render is an exact-frame request.  Passing
                // stillMode prevents the compositor from satisfying a failed
                // tick with m_lastGoodComposite; RenderQueue can then retry the
                // same tick or fail cleanly instead of encoding a stale frame.
                auto result = m_timelineWorkspace->compositeFrame(
                    tick, w, h, scrub, /*stillMode=*/true);
                if (result) result->preservesAlpha = wantAlpha;
                m_timelineWorkspace->setExportAlpha(false);
                m_timelineWorkspace->setForceFullResolution(false);
                return result;
            }
            return nullptr;
        });
    // Queue/running exports use a separate callback that is permanently bound
    // to the job's immutable project graph. ProjectController may replace the
    // live preview callback on a project switch; it cannot affect this path.
    m_exportPanel->setExportFrameCallback(
        [this](const std::shared_ptr<const ExportRenderSnapshot>& snapshot,
               int64_t tick, uint32_t w, uint32_t h, bool scrub,
               bool preserveAlpha) -> std::shared_ptr<CachedFrame> {
            if (m_destroying.load(std::memory_order_acquire) ||
                !m_timelineWorkspace || !snapshot ||
                !snapshot->project || !snapshot->timeline) {
                return nullptr;
            }
            return m_timelineWorkspace->compositeExportFrame(
                snapshot->project, snapshot->timeline,
                tick, w, h, scrub, preserveAlpha);
        });
    // §4.6 export write-through: RenderQueue hands each finished full-res frame
    // here (worker thread, pixels ready) → store it in the segment cache so a
    // re-export reuses it and the render bar shows it green.
    m_exportPanel->setSnapshotFrameStoreCallback(
        [this](const std::shared_ptr<const ExportRenderSnapshot>& snapshot,
               int64_t tick, const std::shared_ptr<CachedFrame>& frame) {
            if (m_destroying.load(std::memory_order_acquire)) return;
            if (m_timelineWorkspace && snapshot &&
                snapshot->project && snapshot->timeline) {
                m_timelineWorkspace->cacheSnapshotExportFrame(
                    snapshot->project, snapshot->timeline, tick, frame);
            }
        });
    m_pageStack->addWidget(m_exportPanel);

    // Suspend the Program Monitor's playback pipeline while the Export
    // panel is driving its own UI-thread preview render loop.  Without
    // this, the shared PlaybackController makes the main FrameClock
    // keep firing, and the producer thread races the UI thread on
    // CompositeService::m_compositeMutex — the choppy-video /
    // smooth-audio bug on 60 fps ProRes preview playback.
    connect(m_exportPanel, &ExportPanel::previewPlaybackToggled,
            this, [this](bool playing) {
        if (m_destroying.load(std::memory_order_acquire)) return;
        if (auto* pm = programMonitor()) {
            if (auto* pipe = pm->pipeline())
                pipe->setExternallySuspended(playing);
        }
    });

    // ── Wire ProjectPanel signals ────────────────────────────────────
    connect(m_projectPanel, &ProjectPanel::createProject,
            this, &MainWindow::onCreateProjectFromPanel);
    connect(m_projectPanel, &ProjectPanel::openProject,
            this, &MainWindow::onOpenProjectFromPanel);
    connect(m_projectPanel, &ProjectPanel::deleteProject,
            this, &MainWindow::onDeleteProjectFromPanel);
    connect(m_projectPanel, &ProjectPanel::renameProject,
            this, &MainWindow::onRenameProjectFromPanel);
    connect(m_projectPanel, &ProjectPanel::duplicateProject,
            this, &MainWindow::onDuplicateProjectFromPanel);
    connect(m_projectPanel, &ProjectPanel::revealInExplorer,
            this, &MainWindow::onRevealProjectInExplorer);
    connect(m_projectPanel, &ProjectPanel::openRecentProject,
            this, &MainWindow::onOpenRecentProjectFromPanel);
    connect(m_projectPanel, &ProjectPanel::importProject,
            this, &MainWindow::onImportProject);
    connect(m_projectPanel, &ProjectPanel::exportProject,
            this, &MainWindow::onExportProject);
    connect(m_projectPanel, &ProjectPanel::projectsDirChanged,
            this, &MainWindow::onProjectsDirChanged);
    connect(m_projectPanel, &ProjectPanel::assignProjectToShow, this,
            [this](const QString& name, const QString& filePath, const QString& show) {
        const std::string showStd = show.toStdString();
        // If this is the currently-open project, set it in memory (used
        // immediately by AudioSync) and mark modified — it persists on the
        // next save. Otherwise load the file, set the show, and save it back.
        if (m_currentProject &&
            QString::fromStdString(m_currentProject->name()) == name) {
            m_currentProject->setShow(showStd);
            m_currentProject->setModified(true);
            if (m_audioSync) m_audioSync->setCurrentShow(showStd);
        } else {
            QString path = filePath.isEmpty()
                ? m_projectPanel->projectFilePath(name) : filePath;
            if (path.isEmpty()) return;
            std::filesystem::path fp(path.toStdWString());
            ProjectSerializer ser;
            auto proj = ser.load(fp);
            if (!proj) {
                QMessageBox::warning(this, "Assign to Show",
                    "Could not open the project file to assign its show.");
                return;
            }
            proj->setShow(showStd);
            if (!ser.save(*proj, fp)) {
                QMessageBox::warning(this, "Assign to Show",
                    "Failed to save the project's show assignment.");
                return;
            }
        }
        statusBar()->showMessage(show.isEmpty()
            ? QString("Cleared show for '%1'").arg(name)
            : QString("Assigned '%1' to show \"%2\"").arg(name, show), 3000);
        // Refresh so the Assign-to-Show checkmark reflects the new assignment.
        refreshProjectsList();
    });
    connect(m_projectPanel, &ProjectPanel::openFromFile,
            this, &MainWindow::onOpenProject);
    connect(m_projectPanel, &ProjectPanel::openFilePath,
            this, &MainWindow::onOpenRecentProjectFromPanel);
    connect(m_projectPanel, &ProjectPanel::saveRequested,
            this, &MainWindow::onSaveProject);
    connect(m_projectPanel->refreshButton(), &QPushButton::clicked,
            this, &MainWindow::refreshProjectsList);

    // Give the project panel the projects directory for its file browser
    m_projectPanel->setProjectsDirectory(projectsDirectory());

    // Populate the projects list
    refreshProjectsList();

    // Start on the PROJECTS page for first launch
    setCurrentPage(Page::Projects);

    // F12: Remember last project path but don't auto-open it.
    //       The user should explicitly select a project from the Projects page.
    //       We still save/restore the path for "recent projects" convenience.
    {
        // (auto-load removed — start on Projects page every time)
    }

    // ── Playback resolution shortcuts (Alt+1/2/3/4) ────────────────────
    if (m_shortcutManager && m_timelineWorkspace) {
        auto* pm = m_timelineWorkspace->programMonitor();
        if (pm) {
            m_shortcutManager->registerAction(
                "view.res_full", "Playback Resolution: Full",
                QKeySequence(Qt::ALT | Qt::Key_1),
                [pm]() { pm->setPlaybackResolutionIndex(0); }, "View");
            m_shortcutManager->registerAction(
                "view.res_half", "Playback Resolution: 1/2",
                QKeySequence(Qt::ALT | Qt::Key_2),
                [pm]() { pm->setPlaybackResolutionIndex(1); }, "View");
            m_shortcutManager->registerAction(
                "view.res_quarter", "Playback Resolution: 1/4",
                QKeySequence(Qt::ALT | Qt::Key_3),
                [pm]() { pm->setPlaybackResolutionIndex(2); }, "View");
            m_shortcutManager->registerAction(
                "view.res_eighth", "Playback Resolution: 1/8",
                QKeySequence(Qt::ALT | Qt::Key_4),
                [pm]() { pm->setPlaybackResolutionIndex(3); }, "View");
        }
    }

    m_panelsBuilt = true;

    spdlog::info("MainWindow::buildPanels() — 5 pages created "
                 "({} timeline panels)", m_timelineWorkspace->dockCount());

    setUpdatesEnabled(true);
    updateGeometry();
    repaint();
}

// ═════════════════════════════════════════════════════════════════════════════

} // namespace rt
