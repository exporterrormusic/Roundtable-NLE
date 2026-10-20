/*
 * AudioSync — audio sync workflow panel.
 *
 * Workflow: Load Script → Import Audio → Transcribe → Auto-Sync → Refine
 *
 * Layout (v2):
 *   ┌─────────────────── Smart Bar ───────────────────────┐ 36 px
 *   │ ▸ Setup │ status │ actions                          │
 *   ├──────────── (disclosure) Setup Panel ───────────────┤ collapsible
 *   │ Script │ Audio │ Transcribe                         │
 *   ├──────────┬──────────────────────────────────────────┤
 *   │ Char tabs│  Continuous-scroll cards per script line  │
 *   │ Script   │  ┌─────────────────────────────────┐     │
 *   │ list     │  │ #1 WELLS: "Hello everyone..."   │     │
 *   │          │  │ ▓▓▓▒▒▓▓▓▓▒▒▓▓▓▓ [trimming]     │     │
 *   │ ────────│  │ ▶ ⏹ [ ] ✂ ✓ ✕                   │     │
 *   │ Orphans  │  └─────────────────────────────────┘     │
 *   │ Progress │                                          │
 *   ├──────────┴──────────────────────────────────────────┤
 *   │ Transport │ Export →                                │
 *   └────────────────────────────────────────────────────┘
 *
 * Runs transcription on a background thread — never blocks UI.
 */

#pragma once

#include <QWidget>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QProgressBar>
#include <QProgressDialog>
#include <QListWidget>
#include <QScrollArea>
#include <QStackedWidget>
#include <QSplitter>
#include <QGroupBox>
#include <QTabBar>
#include <QCheckBox>
#include <QThread>
#include <QTimer>
#include <QSettings>
#include <QVBoxLayout>
#include <QPointer>
#include <QElapsedTimer>

#include "audiosync/SyncClip.h"
#include "ai/Transcriber.h"    // For TranscriptionResult
#include "ai/ShotPipeline.h"   // ShowPipelineState (per-session)
#include "panels/audio/AudioSampleLoader.h"
#include "panels/audio/AudioSyncPlayer.h"
#include "panels/audio/TranscriptionRunner.h"

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace rt {

class AudioEngine;
class AVSyncClock;
class CommandStack;
class MiniWaveformWidget;
struct Script;
class ShotPresetManager;
class Timeline;
class VideoFrameGrabber;
class VideoFrameView;
struct ScriptLine;
struct MatchResult;
namespace audiosync { struct SyncSource; }

/// A previously imported/matched line that can serve as cloning reference.
struct VoiceReferenceCandidate {
    int clipId{-1};
    int lineNumber{-1};
    QString character;
    QString transcript;
    QString sourceFile;
    QString segment;
    double start{0.0};
    double end{0.0};
    float confidence{0.0f};
};

/// An audio file already present on Audio Sync's IMPORT page.
struct VoiceImportedAudioTrack {
    QString sourceFile;
    QString displayName;
    double duration{0.0};
    int approvedClipCount{0};
};

// ─── Script session data ─────────────────────────────────────────────────────

/// A stored session corresponds to one loaded script and all data attached to it.
struct ScriptSession
{
    std::unique_ptr<Script>                 script;
    std::vector<SyncClip>                   clips;
    std::string                             audioPath;
    std::vector<std::string>                audioPaths;
    std::unordered_map<std::string, AudioSampleData> audioSamples;
    bool                                    scriptLoaded{false};
    bool                                    audioImported{false};
    bool                                    transcriptionDone{false};
    bool                                    syncDone{false};
    std::string                             displayName;
    std::string                             sourceUrl;
    std::string                             rawContent;    ///< Raw script text for offline restore
    std::unordered_map<int, std::string>    lineAudioFile;
    mutable std::unordered_map<std::string, QColor> characterColors;
    std::vector<TranscriptionResult>        allTranscriptionResults;
    std::unordered_map<std::string, std::string> shotOverrides; ///< see AudioSync::session().shotOverrides
    ShowPipelineState                       pipeline;      ///< see AudioSync::session().pipeline
};

/// One script line of a shot-table script as the COMFYUI page sees it.
struct ShowShot
{
    int         lineNumber{-1};
    std::string dialogue;
    std::string action;
    std::string segment;
    std::string shot;        ///< effectiveShot(): doc value, in-app pick, or empty
    std::string key;         ///< ScriptMatcher::normalize(dialogue) — pipeline key
    bool        hasTake{false};
    SyncClip    take;        ///< the CONFIRMED take (valid when hasTake)
};

// ─── AudioSync panel ────────────────────────────────────────────────────────

class AudioSync : public QWidget
{
    Q_OBJECT

public:
    explicit AudioSync(QWidget* parent = nullptr);
    ~AudioSync() override;

    /// Set the command stack for undo/redo integration.
    void setCommandStack(CommandStack* stack);
    void setAudioEngine(AudioEngine* engine);
    void setShotPresetManager(ShotPresetManager* mgr) noexcept { m_shotPresetManager = mgr; }
    [[nodiscard]] ShotPresetManager* shotPresetManager() const noexcept { return m_shotPresetManager; }

    /// Set the show of the currently-open project. Default-shot resolution
    /// uses this so a character can have a different default shot per show.
    void setCurrentShow(const std::string& show) { m_currentShow = show; }
    [[nodiscard]] const std::string& currentShow() const noexcept { return m_currentShow; }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void showEvent(QShowEvent* event) override;

public:
    // ── Workflow steps ──────────────────────────────────────────────────

    /// Load a script from text, file path, or URL.
    /// If sourceIdentifier is provided, use it as the session key instead of pathOrContent.
    bool loadScript(const std::string& pathOrContent,
                    const std::string& sourceIdentifier = {});

    /// Import an audio file for transcription.
    bool importAudio(const std::string& audioPath);

    /// Import multiple audio files at once.
    void importAudioFiles(const QStringList& paths);

    /// Start transcription (async — updates UI via signals).
    void startTranscription();

    /// Run auto-sync (match transcribed segments to script lines).
    void runAutoSync();

    /// Word-level forced alignment of transcript words to the script (see
    /// AudioSyncAlign.cpp).  Rebuilds session().clips so each clip spans exactly one
    /// script line's words.  Returns true when it handled the segmentation
    /// (word timestamps present + at least one file aligned), false to fall
    /// back to the heuristic merge/resegment/match path in runAutoSync().
    bool alignClipsToScript();

    /// Shot-table scripts: one tentative clip per TAKE of each line (a line
    /// read several times yields several candidate clips for the user to pick
    /// from). Confirmed clips are kept. Called by alignClipsToScript().
    bool alignShotTakes();

    /// Save/restore state for project persistence.
    void saveProjectState(const QString& projectName);
    void restoreProjectState(const QString& projectName);

    /// Restore audio paths from QSettings (supplemental).
    void restoreAudioPaths(const QString& projectName);

    /// Serialize AudioSync state to a binary blob for .rtp file storage.
    [[nodiscard]] std::vector<uint8_t> serializeToBlob() const;

    /// Restore AudioSync state from a binary blob loaded from .rtp file.
    void deserializeFromBlob(const std::vector<uint8_t>& blob);

    // ── Accessors (for testing) ─────────────────────────────────────────

    [[nodiscard]] int clipCount() const { return static_cast<int>(session().clips.size()); }
    [[nodiscard]] const SyncClip& clip(int index) const { return session().clips.at(static_cast<size_t>(index)); }
    [[nodiscard]] const std::vector<std::string>& audioPaths() const { return session().audioPaths; }
    [[nodiscard]] int scriptLineCount() const;
    [[nodiscard]] bool isScriptLoaded() const noexcept { return session().scriptLoaded; }
    [[nodiscard]] bool isAudioImported() const noexcept { return session().audioImported; }
    [[nodiscard]] bool isTranscriptionComplete() const noexcept { return session().transcriptionDone; }
    [[nodiscard]] bool isSyncComplete() const noexcept { return session().syncDone; }

    /// Waveform samples decode in the background (MediaTaskQueue). True while
    /// any imported file is still queued or decoding.
    [[nodiscard]] bool audioSamplesPending() const noexcept;
    [[nodiscard]] bool hasAudioSamples(const std::string& path) const
    {
        return session().audioSamples.count(path) != 0;
    }

    /// MATCH cards build a viewport's worth at once, then the rest in short
    /// slices. Rows = cards in the current view; built = cards created so far.
    [[nodiscard]] int cardRowCount() const noexcept { return static_cast<int>(m_cardWidgets.size()); }
    [[nodiscard]] int builtCardCount() const noexcept;
    [[nodiscard]] bool cardBuildPending() const noexcept
    {
        return m_cardBuild.active || m_leftBuild.active;
    }
    /// Build every remaining card now.
    void finishCardBuild();
    /// Test seam: replace the active session's clips and rebuild the cards.
    void setClipsForTesting(std::vector<SyncClip> clips);
    /// Test seam: install per-file transcription results (index-aligned with
    /// audioPaths()) and build clips exactly as a finished run does.
    void setTranscriptionsForTesting(std::vector<TranscriptionResult> results);
    /// Test seam: transcribe files with `transcribe` instead of whisper.
    void setTranscribeFunctionForTesting(TranscriptionRunner::TranscribeFn transcribe);

    /// Get character names from the loaded script.
    [[nodiscard]] QStringList scriptCharacters() const;

    /// Host the Audio page's compact TTS controls in the same left workflow
    /// rail as SCRIPT / IMPORT / TRANSCRIBE / MATCH / SETTINGS.
    void setVoiceGenerationPanel(QWidget* panel);
    void showVoiceGenerationPanel();
    /// Add "Generate voice for this line" to a script dialogue label's
    /// context menu. Triggering it opens the TTS rail and emits
    /// voiceLineRequested so the panel links the draft to that exact line.
    void addGenerateVoiceAction(QLabel* label, int lineNumber, const QString& character,
                                const QString& dialogue, const QString& segment);

    /// Usable imported line references for TTS.
    [[nodiscard]] QVector<VoiceImportedAudioTrack> voiceImportedAudioTracks() const;
    [[nodiscard]] QVector<VoiceReferenceCandidate> voiceReferenceCandidates() const;
    [[nodiscard]] const AudioSampleData* voiceAudioSamples(const QString& path) const;
    /// Return the existing automatic transcript that overlaps a manually
    /// selected source range. This avoids asking for text the ASR already made.
    [[nodiscard]] QString voiceTranscriptForRange(const QString& path,
                                                  double start,
                                                  double end) const;

    /// Pure projections used by the panel and by headless validation without
    /// constructing the full Audio Sync workspace.
    [[nodiscard]] static QVector<VoiceReferenceCandidate>
        approvedVoiceReferenceCandidates(const std::vector<SyncClip>& clips);
    static bool saveApprovedVoiceReferenceClips(const std::vector<SyncClip>& clips,
                                                const QString& character,
                                                QString* savedPath,
                                                QString* error);

    /// Consolidate all confirmed clips for one character into the application
    /// reference library as lossless FLAC plus a transcript metadata sidecar.
    bool saveApprovedVoiceReference(const QString& character,
                                    QString* savedPath,
                                    QString* error) const;

    /// Register generated audio in AudioSync. A non-negative line number marks
    /// it confirmed; -1 leaves it unmatched for later manual/automatic work.
    void attachGeneratedAudio(const QString& path, const QString& character,
                              const QString& dialogue, int scriptLineNumber,
                              const QString& segment, double durationSeconds);

    /// Match approved generated dialogue against script lines belonging to the
    /// selected character, preferring the explicitly selected script line.
    /// The clip is still imported as unmatched when no suitable line exists.
    [[nodiscard]] bool syncGeneratedAudio(const QString& path,
                                          const QString& character,
                                          const QString& dialogue,
                                          int preferredScriptLine,
                                          const QString& preferredSegment,
                                          double durationSeconds);

    /// In-app audition for an unapproved generated draft. This never imports or
    /// mutates the project/AudioSync model.
    [[nodiscard]] bool auditionVoiceDraft(const QString& path);
    void stopVoiceDraftAudition();

    /// Shot for a line of a shot-table script: the doc's SHOT cell, else the
    /// shot the user picked in-app for that blank cell, else empty (missing).
    [[nodiscard]] std::string effectiveShot(const ScriptLine& line) const;

    /// Pick the shot for a line whose SHOT cell is blank in the doc. Undoable.
    /// Keyed by the line's text so the pick survives doc edits / re-imports;
    /// a shot later typed into the doc takes precedence.
    void setShotOverride(const ScriptLine& line, const std::string& shot);

    // ── COMFYUI page (shot-table scripts) ───────────────────────────────
    [[nodiscard]] bool isShotScript() const;
    [[nodiscard]] QString activeScriptName() const;
    /// Every line of the active shot-table script in script order, with its
    /// picked take (if any).
    [[nodiscard]] std::vector<ShowShot> showShots() const;
    /// Play a take (by SyncClip::id) in the take player and adjust its in/out
    /// points (one undo step).  Returns true if the in/out changed.
    bool openTakePlayerForClipId(int clipId);

    // ── Preview audio for other pages (COMFYUI SHOTS transport) ────────
    // Load a source range once, then play it at any speed (reverse and slow
    // included — the engine time-stretches) or scrub it.
    /// Load [rangeStart, rangeEnd] of a take's source audio. False if none.
    bool loadPreviewAudioForTake(int clipId, double rangeStart, double rangeEnd);
    /// Load a media file's own audio (e.g. a ComfyUI render), decoded once.
    bool loadPreviewAudioForMedia(const std::string& path);
    /// Play the loaded range from `fromSec` at `speed` (1 = normal, <0 = reverse).
    bool playPreviewAudio(double fromSec, double speed);
    /// A short burst at `atSec` (scrub / frame step).
    void scrubPreviewAudio(double atSec);
    /// Pause without unloading (the next play / scrub is instant).
    void pausePreviewAudio();
    void stopTakeAudio();
    /// Source time being heard while playing, or -1 when stopped.
    [[nodiscard]] double takeAudioTime() const;
    /// Set a take's in/out (seconds in its source) as one undo step.
    bool setTakeInOut(int clipId, double start, double end);
    /// Framing / per-shot settings of the active script session. Saved with
    /// the project (AudioSync blob v8); edits need no extra notification.
    [[nodiscard]] ShowPipelineState& pipeline() noexcept { return session().pipeline; }
    [[nodiscard]] const ShowPipelineState& pipeline() const noexcept { return session().pipeline; }

    /// Return the subset of script characters that have no default shot configured.
    /// Always empty for shot-table scripts (their announcer is video, not a sprite).
    /// Characters are checked via ShotPresetManager::resolveDefaultShot.
    [[nodiscard]] QStringList missingDefaultShots() const;

    /// Export confirmed clips to a Timeline as AudioClips (back-to-back).
    /// Ported from Python _export_timeline(). Returns number of clips exported.
    static constexpr int kExportBlockedByLockedTrack = -1;
    int exportToTimeline(Timeline* timeline);
    [[nodiscard]] const QString& lastExportError() const noexcept { return m_lastExportError; }

    /// Derive a friendly display name from a URL / file path.
    static QString displayNameForScriptUrl(const QString& url);

    /// Extract the <title> from an HTML string (Google Docs export).
    static std::string extractHtmlTitle(const std::string& html);

    /// Reset all state for a new/opened project.
    void resetForNewProject();

    [[nodiscard]] QListWidget* clipListWidget() const { return m_clipList; }
    [[nodiscard]] QListWidget* scriptListWidget() const { return m_leftScriptList; }
    [[nodiscard]] QPushButton* transcribeButton() const { return m_transcribeBtn; }
    [[nodiscard]] QComboBox* transcriptionModelCombo() const { return m_modelCombo; }
    [[nodiscard]] QPushButton* autoSyncButton() const { return m_autoSyncBtn; }
    [[nodiscard]] QPushButton* exportButton() const { return m_exportBtn; }
    [[nodiscard]] QPushButton* voiceGenerationButton() const { return m_voiceRailBtn; }
    [[nodiscard]] int audioSidePanelMode() const noexcept { return m_audioSidePanelMode; }
    [[nodiscard]] QProgressBar* progressBar() const { return m_progressBar; }

signals:
    void scriptLoaded(int lineCount);
    void voiceContextChanged();
    void voiceLineRequested(int lineNumber, const QString& character,
                            const QString& dialogue, const QString& segment);
    void audioImported(const QString& path);
    void transcriptionStarted();
    void transcriptionProgress(float percent, const QString& status);
    void transcriptionFinished(int segmentCount);
    void transcriptionFailed(const QString& error);
    void syncCompleted(int matchCount, int totalCount);
    void exportRequested();
    /// One-click re-export: repeat the last export (same show, same
    /// sequence) with no prompts; MainWindow owns the settings lookup.
    void reExportRequested();

private slots:
    void onLoadScriptClicked();
    void onImportAudioClicked();
    void onTranscribeClicked();
    void onCancelTranscriptionClicked();
    void onAutoSyncClicked();
    void onExportClicked();
    void onReExportClicked();
    void onTranscriptionProgress(float percent, const QString& status);
    void onClipSelectionChanged();
    void onScriptFilterChanged(int index);

private:
    void setupUi();
    void updateWorkflowState();
    void populateLeftList();
    void populateCards();
    void populateScriptList();   // delegates to populateLeftList + populateCards
    void populateClipList();     // delegates to populateCards (compat)
    void runAutoSyncImpl();      // algorithm body; public runAutoSync adds undo history
    void createClipsFromTranscription();
    void createClipsFromAllTranscriptions();
    void resegmentByScript();          // script-guided re-segmentation (#2)
    void startSingleFileTranscription(size_t index);
    void beginTranscriptionRun(std::vector<size_t> indices);
    /// Whisper / CrisperWhisper with the model picked in the model combo.
    [[nodiscard]] TranscriptionRunner::TranscribeFn whisperTranscribeFunction() const;
    void onTranscriptionFileStarted(const std::string& path, size_t number, size_t total);
    void onTranscriptionFileFinished(const std::string& path, const TranscriptionResult& result);
    void onTranscriptionFileFailed(const QString& error);
    void finishTranscriptionRun(bool cancelled);
    /// The session the current run writes to: the one it started in, wherever
    /// that is now. Null when that script was closed or replaced.
    [[nodiscard]] ScriptSession* transcriptionSession();
    /// Cancel the run and drop its results (the sessions are being replaced).
    void detachTranscriptionRun();
    /// Clips from newly transcribed files, merged to the script; what a run
    /// that transcribed at least one file ends with.
    void applyFinishedTranscriptions(ScriptSession& target);
    void updateTranscriptionControls();
    [[nodiscard]] bool ensureCrisperWhisperConsent();
    [[nodiscard]] bool isCrisperWhisperSelected() const;
    void selectDefaultWhisperModel();
    void fetchScriptFromUrl(const QString& url);
    void loadScriptHistory();
    void addToScriptHistory(const QString& url);
    void deleteScriptHistoryEntry(int index);
    void renameScriptHistoryEntry(int index);
    void populateScriptFilter();       // updates character tabs
    void populateCharacterTabs();      // rebuild tabs from script
    void clearTranscriptionForFile(size_t fileIndex);
    void clearAllTranscriptions();
    void setupScriptPage();
    void saveCurrentSession();
    void restoreSession(const std::string& sessionKey);
    void switchToScript(const std::string& sessionKey);
    void clearCurrentSession();
    void populateScriptSessionList();
    /// Update an existing session's script with new content while preserving
    /// existing clips/matches by remapping scriptLineNumber via dialogue similarity.
    void updateExistingSessionScript(const std::string& sessionKey,
                                     std::unique_ptr<Script> newScript,
                                     const std::string& rawContent);
    void closeInterClipGaps();
    void downloadWhisperModel(const std::string& modelName, std::function<void(bool)> onComplete);
    void toggleSetupPanel();           // disclosure expand/collapse (legacy)
    void updateSmartBar();             // refresh smart bar label/actions
    void showAudioSidePanel(int mode);  // 0=Script, 1=Import, 2=Transcribe, 3=Match, 4=Settings, 5=TTS
    void hideAudioSidePanel();
    void toggleAudioSidePanel(int mode);
    void scrollToCard(int scriptLineNumber);  // sync left→right
    void updateCardMatchStyle(size_t clipIdx); // lightweight restyle after matchState change
    static void styleConfirmButton(QPushButton* button, int matchState);
    void syncLeftListFromScroll();     // sync right→left

    /// Snapshot-then-mutate helper for bulk operations.  Captures session().clips
    /// before running `mutate`, runs it, then pushes a LambdaCommand whose
    /// undo restores the snapshot and whose redo replays the new state.
    /// `rebuild` is called immediately after the mutation and again on each
    /// undo/redo to refresh the UI.  Skips the command push when the
    /// mutation produced no observable change.
    void runClipsMutationWithUndo(const std::string& description,
                                  std::function<void()> mutate,
                                  std::function<void()> rebuild = nullptr);

    // Audio playback / waveform helpers
    /// Queue a background decode for every imported file not loaded yet.
    void loadAudioSamples();
    /// Decode any still-missing samples on this thread (programmatic callers
    /// that need complete audio right now; the UI waits instead).
    void loadAudioSamplesNow();
    /// Run `fn` now when every imported file's samples are loaded, else once
    /// the background decodes finish (a repeat of the same waiting action is
    /// dropped). For session-wide actions that analyse the audio.
    void whenAudioSamplesReady(const QString& action, std::function<void()> fn);
    /// False, with a notice, while `path`'s samples (empty path: any file's)
    /// are still decoding. Per-card actions use this instead of waiting.
    bool audioReadyOrNotify(const std::string& path, const QString& action);
    void installAudioSamples(const std::string& path, AudioSampleData decoded);
    /// Stop queued/running decodes and drop actions waiting on them.
    void cancelAudioSampleDecodes();
    void refreshAudioFileListItem(const std::string& path);
    void playClip(size_t clipIdx);
    void togglePlayClip(size_t clipIdx);
    void pausePlayback();
    void stopPlayback();
    void seekPlayingClip(double timeSec);
    void scrubClipAt(size_t clipIdx, double timeSec);
    void autoTrimClip(size_t clipIdx);
    void openManualMatch(int lineNumber);

    // Card building: populateCards() builds every card; rebuildCardsForLines()
    // rebuilds only the named lines' cards in place (take swap, confirm,
    // reject, reassign...), which is what keeps those clicks fast.
    struct CardBuildContext;
    [[nodiscard]] CardBuildContext makeCardBuildContext() const;
    void fillCard(QFrame* card, const ScriptLine& line, int cardIdx,
                  const CardBuildContext& ctx,
                  MiniWaveformWidget*& waveformOut, int& clipIdxOut);
    void rebuildCardsForLines(const std::vector<int>& lineNumbers);
    /// Create row `row` of the current card build at its layout position.
    void buildCardRow(size_t row, const CardBuildContext& ctx);
    /// Build the next pending rows: for about `budgetMs` (at least one row),
    /// or all of them when `budgetMs` < 0.
    void buildPendingCardRows(int budgetMs);
    /// One time-boxed slice of the background card build (timer).
    void continueCardBuild();
    /// Keep the page at least as tall as the projected finished build.
    void reserveCardPageHeight();
    void buildLeftRow(size_t row, const std::unordered_map<int, int>& lineMatchState);
    void buildPendingLeftRows(int budgetMs);
    void setTrailingCardStretch(int stretch);
    void cancelCardBuild();
    /// Cheap left-list update after match-state changes only (same rows):
    /// restyles changed rows; falls back to populateLeftList() otherwise.
    void refreshLeftListStates();
    /// Deferred rebuildCardsForLines(): safe from inside a card's own signal.
    void scheduleCardRebuild(std::vector<int> lineNumbers);
    /// Accent outline on the selected card.  A lightweight overlay child, NOT
    /// a QGraphicsDropShadowEffect: an effect forces the whole card (waveform,
    /// video, buttons) through an offscreen blur on every repaint, which made
    /// every click and every playback tick slow.
    void setHighlightedCard(QWidget* card);
    void splitClip(size_t clipIdx);
    void mergeClipWithNext(size_t clipIdx);
    void updateSelectedClipHighlight();
    void updateTransportBar();
    QColor characterColor(const std::string& character) const;

    /// Walk up from a child widget to find which clip-list row it belongs to.
    /// Returns -1 if the widget is not inside any clip card.
    [[nodiscard]] int clipRowForWidget(QWidget* widget) const;

    /// Add a rich item to the audio file list (filename + details).
    void addAudioFileListItem(const QString& fullPath, int row = -1);

    /// Re-link an audio file to a new path, preserving all sync data.
    void relinkAudioFile(int fileIdx);

    /// Remove an audio file and all associated clips / transcriptions.
    void removeFileFromSync(int fileIdx);

    /// Remove several audio files as one operation, then rebuild dependent UI once.
    void removeFilesFromSync(std::vector<int> fileIndices);

    /// Sort the audio file list by the given criterion.
    void sortAudioFileList(int criterion);  // 0=name, 1=date, 2=size

    /// Rebuild the transcribe-page file list with per-file status indicators.
    void refreshTranscribeFileList();

    /// Play/stop a raw audio file by index into session().audioPaths.
    void playAudioFile(size_t fileIndex);
    void stopFilePlayback();

    /// Find the MiniWaveformWidget for a given clip index via the card mapping.
    [[nodiscard]] MiniWaveformWidget* waveformForClip(int clipIdx) const;

    /// Get effective time ranges for a SyncClip, excluding deleted regions.
    /// Ported from Python _get_effective_ranges().
    static std::vector<std::pair<double,double>> getEffectiveRanges(const SyncClip& clip);

    /// Workflow tests reach private steps through this (tests/ui).
    friend struct AudioSyncTestAccess;

    // ── State ───────────────────────────────────────────────────────────
    CommandStack*     m_commandStack{nullptr};
    bool              m_exportTransactionActive{false};
    QString           m_lastExportError;
    AudioEngine*      m_audioEngine{nullptr};
    ShotPresetManager* m_shotPresetManager{nullptr};
    std::string        m_currentShow;  ///< Show of the open project (per-show defaults)

    std::unique_ptr<Transcriber> m_transcriber;



    // Background sample decode of the open script's files, installed with
    // installAudioSamples().
    std::unique_ptr<AudioSampleLoader> m_sampleLoader;

    // The engine side of everything AudioSync plays (clips, previews, files,
    // TTS drafts): buffers, sync-clock detach. What plays and the widgets
    // that follow it stay here.
    AudioSyncPlayer m_player;
    bool loadPreviewAudio(const std::string& key, const AudioSampleData& data, double rangeStart, double rangeEnd);

    // Playback state
    int     m_playingClipIdx{-1};
    int     m_selectedClipIdx{-1};  // Currently highlighted clip row
    QTimer* m_playheadTimer{nullptr};

    // File preview playback (Import / Transcribe lists)
    int     m_previewFileIdx{-1};   // index into session().audioPaths, -1 = none
    QTimer* m_previewTimer{nullptr};
    std::unordered_map<size_t, MiniWaveformWidget*> m_fileWaveforms;       // file index -> waveform (IMPORT list)
    std::unordered_map<size_t, MiniWaveformWidget*> m_transcribeWaveforms; // file index -> waveform (TRANSCRIBE list)
    std::unordered_map<size_t, QPushButton*>        m_filePlayBtns;        // play/pause buttons (IMPORT)
    std::unordered_map<size_t, QPushButton*>        m_transcribePlayBtns;  // play/pause buttons (TRANSCRIBE)
    std::unordered_map<size_t, QLabel*>             m_fileTimeLabels;      // time labels (IMPORT)
    std::unordered_map<size_t, QLabel*>             m_transcribeTimeLabels;// time labels (TRANSCRIBE)
    void updateFilePlayButton(size_t fileIndex, bool playing);
    void updateFileTimeLabel(size_t fileIndex, double timeSec);

    // Character color cache (assigned deterministically)

    bool m_restoring{false};        // true during restoreProjectState to skip redundant rebuilds
    bool m_cardsDirty{false};       // populateScriptList/populateClipList deferred until widget is shown
    bool m_manualMatchOpen{false};  // suppress event filter while ManualMatchDialog is open
    bool m_forwardingKey{false};      // re-entrancy guard for JKL sendEvent
    QProgressDialog* m_syncProgress{nullptr}; // shown during auto-sync
    QString m_lastScriptSource;   // For project persistence
    QString m_lastImportDir;      // Remembers last file dialog directory

    // Per-line audio file assignment: lineNumber → audio file path.
    // Set by the user via the audio file combo on unmatched cards.
    // Used as the pre-selected file when MANUAL match is opened.

    // Shot picked in-app for shot-table lines whose SHOT cell is blank:
    // ScriptMatcher::normalize(dialogue) → "WIDE" / "CLOSE-UP".

    // ── Video takes (video sources only; audio cards are unchanged) ─────
    std::unique_ptr<VideoFrameGrabber> m_frameGrabber;   ///< lazily created
    [[nodiscard]] VideoFrameGrabber* frameGrabber();
    /// lineNumber → SyncClip::id of the take the user clicked to view.
    /// Transient view state (not saved); ignored if the id left that line.
    std::unordered_map<int, int> m_displayedTakeId;
    /// clip index → its card's video preview (rebuilt with the cards).
    std::unordered_map<size_t, QPointer<VideoFrameView>> m_takePreviewLabels;
    /// Show the source frame at timeSec in the clip's card preview, if any.
    void requestTakePreviewFrame(size_t clipIdx, double timeSec);
    /// Big take player: video + full-file waveform, drag/I/O to adjust the
    /// take's in/out points (applied with undo on confirm).
    void openTakePlayer(size_t clipIdx);
    /// "Take 1 · 0:42" chips for every candidate take of a line.
    [[nodiscard]] QWidget* createTakeStrip(int lineNumber, const std::vector<size_t>& clipIndices,
                                           size_t shownClipIdx);
    static void styleTakeChip(QPushButton* chip, bool chosen, bool shown);

    /// Build the WIDE / CLOSE-UP tag for a shot-table line. Missing shots
    /// render as a red "SET SHOT" button; picked ones stay clickable.
    [[nodiscard]] QWidget* createShotTag(const ScriptLine& line, int width);

    // Transcription runs: the runner is created after m_transcriber and
    // destroyed first (~AudioSync resets it explicitly).
    std::unique_ptr<TranscriptionRunner> m_transcriptionRunner;
    /// Key of the session the run started in (empty: the scratch session);
    /// nullopt once the run is detached.
    std::optional<std::string> m_transcriptionSessionKey;
    TranscriptionRunner::TranscribeFn m_transcribeForTesting;

    // ── UI widgets ──────────────────────────────────────────────────────

    // Icon Rail (left sidebar, like ProjectPanel)
    QWidget*      m_audioIconRail{nullptr};
    QPushButton*  m_scriptRailBtn{nullptr};
    QPushButton*  m_importRailBtn{nullptr};
    QPushButton*  m_transcribeRailBtn{nullptr};
    QPushButton*  m_matchRailBtn{nullptr};
    QPushButton*  m_voiceRailBtn{nullptr};
    QPushButton*  m_audioSettingsRailBtn{nullptr};
    int           m_audioSidePanelMode{-1};  // -1=None, 0=Script, 1=Import, 2=Transcribe, 3=Match, 4=Settings, 5=TTS

    // Side Panel (inline expanding column)
    QWidget*        m_audioSidePanel{nullptr};
    QStackedWidget* m_audioSidePanelStack{nullptr};
    QWidget*        m_scriptPage{nullptr};
    QWidget*        m_importPage{nullptr};
    QWidget*        m_transcribePage{nullptr};
    QWidget*        m_matchPage{nullptr};
    QListWidget*    m_charFilterList{nullptr};
    QWidget*        m_audioSettingsPage{nullptr};
    QWidget*        m_voiceGenerationPage{nullptr};

    // Content area (match workspace + action bar)
    QWidget*      m_audioContentArea{nullptr};
    QWidget*      m_audioActionBar{nullptr};
    QPushButton*  m_syncActionBtn{nullptr};
    QDoubleSpinBox* m_syncFrontPaddingSpin{nullptr};
    QDoubleSpinBox* m_syncEndPaddingSpin{nullptr};
    QPushButton*  m_confirmAllActionBtn{nullptr};
    QPushButton*  m_unconfirmAllActionBtn{nullptr};
    QPushButton*  m_clearActionBtn{nullptr};
    QPushButton*  m_exportActionBtn{nullptr};
    QPushButton*  m_reExportActionBtn{nullptr};
    QLabel*       m_statusLabel{nullptr};

    // Smart Bar (top) — kept for updateSmartBar compat
    QWidget*      m_smartBar{nullptr};
    QLabel*       m_smartBarIcon{nullptr};
    QLabel*       m_smartBarLabel{nullptr};
    QPushButton*  m_disclosureBtn{nullptr};
    QPushButton*  m_smartAutoSyncBtn{nullptr};
    QPushButton*  m_smartImportBtn{nullptr};
    QPushButton*  m_smartConfirmAllBtn{nullptr};
    bool          m_setupExpanded{false};

    // Setup Panel (collapsible, below smart bar)
    QWidget*      m_setupPanel{nullptr};

    // Script session management. Every loaded script's data lives in its
    // session; the open one is m_scriptSessions[m_activeScriptKey].
    std::unordered_map<std::string, ScriptSession> m_scriptSessions;
    std::string m_activeScriptKey;   // key into m_scriptSessions, empty = none
    /// Holds what is entered while no session exists for the open key
    /// (audio imported before any script); moves into the first session.
    ScriptSession m_scratchSession;
    /// The open script's data: its session, else the scratch session.
    [[nodiscard]] ScriptSession& session() noexcept;
    [[nodiscard]] const ScriptSession& session() const noexcept;
    /// The open script's data as the core sync algorithms take it.
    [[nodiscard]] audiosync::SyncSource syncSource() const;
    [[nodiscard]] static audiosync::SyncSource syncSourceOf(const ScriptSession& data);
    std::string m_pendingSessionName; // name to apply on next interactive load, empty = auto-generate

    // Setup Panel — Script column
    QListWidget*  m_scriptSessionList{nullptr};
    QPushButton*  m_loadScriptBtn{nullptr};
    QLabel*       m_scriptStatus{nullptr};
    QPushButton*  m_scriptFormatToggle{nullptr};
    QFrame*       m_scriptFormatBody{nullptr};

    // Setup Panel — Audio column
    QPushButton*  m_importAudioBtn{nullptr};
    QPushButton*  m_removeAudioBtn{nullptr};
    QListWidget*  m_audioFileList{nullptr};
    QComboBox*    m_audioSortCombo{nullptr};
    QComboBox*    m_modelCombo{nullptr};
    QLabel*       m_audioStatus{nullptr};

    // Setup Panel — Transcribe column
    QPushButton*  m_transcribeBtn{nullptr};
    QPushButton*  m_cancelTranscriptionBtn{nullptr};
    QPushButton*  m_clearSelectedTranscriptionBtn{nullptr};
    QPushButton*  m_clearAllTranscriptionsBtn{nullptr};
    QProgressBar* m_progressBar{nullptr};
    QLabel*       m_transcribeStatus{nullptr};
    QListWidget*  m_transcribeFileList{nullptr};

    // Split pane
    QSplitter*    m_splitter{nullptr};

    // Left pane (character tabs + script lines + orphans)
    QTabBar*      m_charTabBar{nullptr};  // hidden legacy, kept for compat
    QListWidget*  m_leftScriptList{nullptr};
    QLabel*       m_leftOrphanLabel{nullptr};
    QListWidget*  m_leftOrphanList{nullptr};

    // Right pane (continuous scroll of cards)
    QScrollArea*  m_rightScrollArea{nullptr};
    QWidget*      m_rightScrollContent{nullptr};
    QVBoxLayout*  m_rightLayout{nullptr};

    // ── Card widget pool (Phase 5.A: reuse instead of destroy+recreate) ─
    struct CardWidgetPool {
        std::vector<QFrame*> pool;  // pre-allocated, recycled
        size_t activeCount = 0;

        QFrame* acquire() {
            if (activeCount < pool.size())
                return pool[activeCount++];
            return nullptr;  // caller must allocate, then adopt()
        }

        /// Add a card the caller just allocated, counted as IN USE.  (Pushing
        /// it without counting it let the next acquire() hand the same card
        /// to the next script line, dropping the earlier line's card.)
        void adopt(QFrame* frame) {
            pool.push_back(frame);
            activeCount = pool.size();
        }

        void releaseAll() {
            for (auto* frame : pool)
                frame->setVisible(false);
            activeCount = 0;
        }

        void shrink(size_t targetSize) {
            while (pool.size() > targetSize) {
                delete pool.back();
                pool.pop_back();
            }
        }
    };
    CardWidgetPool m_cardPool;

    // Card → waveform mapping (indexed by script line index in current view)
    std::vector<QPointer<MiniWaveformWidget>> m_cardWaveforms;
    std::vector<QPointer<QWidget>>            m_cardWidgets;      // card frames in right pane
    std::vector<int>                 m_cardScriptLineNums; // script line number per card
    std::vector<int>                 m_cardClipIndices;    // clip index per card (-1 if unmatched)
    // Rows above are sized to the whole view up front (index-aligned with
    // the left list); a row's widget/waveform stays null until it is built.
    struct PendingCardBuild {
        bool active{false};
        std::vector<size_t> lineIndices;  ///< session().script->lines index per row
        size_t next{0};                   ///< next row to build
        const Script* script{nullptr};    ///< script the indices refer to
        int pendingScrollLine{-1};        ///< scrollToCard() target not built yet
        int width{0};                     ///< card width the heights below assume
        int builtHeight{0};               ///< built cards' heights + spacing
        int reservedHeight{0};            ///< page minimum height while building
        QElapsedTimer sinceSlice;         ///< end of the last slice (Qt's pass since)
    };
    PendingCardBuild m_cardBuild;
    /// Left script list: every row's item exists; widgets stream in.
    struct PendingLeftBuild {
        bool active{false};
        std::vector<size_t> lineIndices;  ///< session().script->lines index per row
        size_t next{0};                   ///< next row whose widget to build
        const Script* script{nullptr};
    };
    PendingLeftBuild m_leftBuild;
    QTimer*          m_cardBuildTimer{nullptr};
    QPointer<QWidget>                m_highlightedCard;          // currently highlighted card (outline overlay)
    QPointer<QFrame>                 m_cardHighlight;            // the outline overlay (child of m_highlightedCard)
    QPointer<QFrame>                 m_selectedLeftCard;         // currently selected left card (glow effect)

    // Trim drag undo debouncing
    QTimer*  m_trimDebounceTimer{nullptr};
    double   m_preTrimStart{0}, m_preTrimEnd{0};
    int      m_preTrimMatchState{0};
    size_t   m_trimDebounceClipIdx{0};

    // Hidden clip list (kept for compat with clipRowForWidget)
    QListWidget*  m_clipList{nullptr};

    // Workflow control (in smart bar / setup)
    QPushButton*  m_autoSyncBtn{nullptr};
    QCheckBox*    m_retakesCheck{nullptr};
    QLabel*       m_syncStatus{nullptr};

    // Bottom bar
    QPushButton*  m_exportBtn{nullptr};
    QPushButton*  m_confirmBtn{nullptr};      // Clear Matches
    QPushButton*  m_confirmAllBtn{nullptr};    // Batch Confirm

    // Transport bar
    QPushButton*  m_transportPlayBtn{nullptr};
    QPushButton*  m_transportStopBtn{nullptr};
    QLabel*       m_transportTimeLabel{nullptr};
    QLabel*       m_transportClipLabel{nullptr};

    // Legacy — kept for updateWorkflowState compat
    QLineEdit*    m_audioPathEdit{nullptr};
    QGroupBox*    m_scriptGroup{nullptr};
    QGroupBox*    m_audioGroup{nullptr};
    QGroupBox*    m_transcribeGroup{nullptr};
    QGroupBox*    m_syncGroup{nullptr};
    QGroupBox*    m_reviewGroup{nullptr};
    QLabel*       m_clipCountLabel{nullptr};
    QListWidget*  m_scriptList{nullptr};
    QComboBox*    m_scriptFilterCombo{nullptr};

    // Snapshotted when Auto-Sync is clicked; confirmed clip bounds are never
    // rebuilt, so changing these only affects the next unconfirmed sync pass.
    double        m_syncFrontPaddingSec{0.0};
    double        m_syncEndPaddingSec{0.2};

    // Use-after-free guard
    std::atomic<bool> m_destroying{false};
};

} // namespace rt
