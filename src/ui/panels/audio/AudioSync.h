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

#include "ai/Transcriber.h"    // For TranscriptionResult

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace rt {

class AudioEngine;
class AVSyncClock;
class CommandStack;
class MiniWaveformWidget;
struct Script;
class ShotPresetManager;
class Timeline;
struct ScriptLine;
struct MatchResult;

// ─── Transcription worker (runs on QThread) ─────────────────────────────────

class TranscriptionWorker : public QObject
{
    Q_OBJECT
public:
    explicit TranscriptionWorker(Transcriber* transcriber, QObject* parent = nullptr);

    void setAudioPath(const std::string& path) { m_audioPath = path; }
    void setLanguage(const std::string& lang)   { m_language = lang; }
    void setModelSize(WhisperModelSize size)    { m_modelSize = size; }
    void requestCancel() noexcept { m_cancelRequested.store(true, std::memory_order_release); }

    /// Access the result after finished(true).
    [[nodiscard]] const TranscriptionResult& result() const { return m_result; }

public slots:
    void process();

signals:
    void progressChanged(float percent, const QString& status);
    void finished(bool success);
    void errorOccurred(const QString& error);

private:
    Transcriber* m_transcriber;
    std::string  m_audioPath;
    std::string  m_language;
    WhisperModelSize m_modelSize{kDefaultWhisperModel};
    TranscriptionResult m_result;
    std::atomic<bool> m_cancelRequested{false};
};

// ─── Audio clip data model ──────────────────────────────────────────────────

struct SyncClip
{
    int         id{0};
    std::string sourceFile;
    std::string character;
    double      start{0.0};     // seconds
    double      end{0.0};       // seconds
    std::string transcript;
    std::string editedText;
    int         matchState{0};  // 0=unmatched, 1=tentative, 2=confirmed
    float       confidence{0.0f};
    int         scriptLineNumber{-1};
    std::string scriptSegment;
    std::vector<std::pair<double,double>> deletedRegions;

    bool operator==(const SyncClip&) const = default;
};

/// Loaded audio sample data (mono float, keyed by file path)
struct AudioSampleData {
    std::vector<float> samples;  // mono float -1..1
    uint32_t sampleRate{44100};
};

/// Read-only script projection used by the voice-generation panels.
struct VoiceScriptLine {
    int lineNumber{-1};
    QString character;
    QString dialogue;
    QString segment;
};

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
    size_t                                  currentTranscriptionIndex{0};
    size_t                                  transcriptionRunTotal{0};
    size_t                                  transcriptionRunCompleted{0};
    std::vector<size_t>                     pendingTranscriptionIndices;
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
    /// AudioSyncAlign.cpp).  Rebuilds m_clips so each clip spans exactly one
    /// script line's words.  Returns true when it handled the segmentation
    /// (word timestamps present + at least one file aligned), false to fall
    /// back to the heuristic merge/resegment/match path in runAutoSync().
    bool alignClipsToScript();

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

    [[nodiscard]] int clipCount() const { return static_cast<int>(m_clips.size()); }
    [[nodiscard]] const SyncClip& clip(int index) const { return m_clips.at(static_cast<size_t>(index)); }
    [[nodiscard]] const std::vector<std::string>& audioPaths() const { return m_audioPaths; }
    [[nodiscard]] int scriptLineCount() const;
    [[nodiscard]] bool isScriptLoaded() const noexcept { return m_scriptLoaded; }
    [[nodiscard]] bool isAudioImported() const noexcept { return m_audioImported; }
    [[nodiscard]] bool isTranscriptionComplete() const noexcept { return m_transcriptionDone; }
    [[nodiscard]] bool isSyncComplete() const noexcept { return m_syncDone; }

    /// Get character names from the loaded script.
    [[nodiscard]] QStringList scriptCharacters() const;

    /// Host the Audio page's compact TTS controls in the same left workflow
    /// rail as SCRIPT / IMPORT / TRANSCRIBE / MATCH / SETTINGS.
    void setVoiceGenerationPanel(QWidget* panel);
    void showVoiceGenerationPanel();

    /// Snapshot script dialogue and usable imported line references for TTS.
    [[nodiscard]] QVector<VoiceScriptLine> voiceScriptLines() const;
    [[nodiscard]] QVector<VoiceImportedAudioTrack> voiceImportedAudioTracks() const;
    [[nodiscard]] QVector<VoiceReferenceCandidate> voiceReferenceCandidates() const;
    [[nodiscard]] const AudioSampleData* voiceAudioSamples(const QString& path) const;

    /// Pure projections used by the panel and by headless validation without
    /// constructing the full Audio Sync workspace.
    [[nodiscard]] static QVector<VoiceReferenceCandidate>
        approvedVoiceReferenceCandidates(const std::vector<SyncClip>& clips);
    static bool saveApprovedVoiceReferenceClips(const std::vector<SyncClip>& clips,
                                                const QString& character,
                                                QString* savedPath,
                                                QString* error);

    /// Consolidate all confirmed clips for one character into the application
    /// reference library as an MP3 plus transcript metadata sidecar.
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

    /// Return the subset of script characters that have no default shot configured.
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
    [[nodiscard]] QPushButton* autoSyncButton() const { return m_autoSyncBtn; }
    [[nodiscard]] QPushButton* exportButton() const { return m_exportBtn; }
    [[nodiscard]] QPushButton* voiceGenerationButton() const { return m_voiceRailBtn; }
    [[nodiscard]] int audioSidePanelMode() const noexcept { return m_audioSidePanelMode; }
    [[nodiscard]] QProgressBar* progressBar() const { return m_progressBar; }

signals:
    void scriptLoaded(int lineCount);
    void voiceContextChanged();
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
    void createClipsFromTranscription();
    void createClipsFromAllTranscriptions();
    void appendClipsFromNewTranscriptions();
    void mergeSegmentsToMatchScript();
    void resegmentByScript();          // script-guided re-segmentation (#2)
    void startSingleFileTranscription(size_t index);
    void beginTranscriptionRun(std::vector<size_t> indices);
    void startTranscriptionForFile(size_t index, uint64_t runId);
    void handleTranscriptionFinished(uint64_t runId,
                                     const QPointer<TranscriptionWorker>& worker,
                                     bool success);
    void handleTranscriptionError(uint64_t runId, const QString& error);
    void handleTranscriptionThreadFinished(uint64_t runId,
                                           const QPointer<QThread>& thread);
    void finishTranscriptionRun(bool cancelled);
    void updateTranscriptionControls();
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
                                     std::unique_ptr<Script> newScript);
    void closeInterClipGaps();
    void downloadWhisperModel(const std::string& modelName, std::function<void(bool)> onComplete);
    void toggleSetupPanel();           // disclosure expand/collapse (legacy)
    void updateSmartBar();             // refresh smart bar label/actions
    void showAudioSidePanel(int mode);  // 0=Script, 1=Import, 2=Transcribe, 3=Match, 4=Settings, 5=TTS
    void hideAudioSidePanel();
    void toggleAudioSidePanel(int mode);
    void scrollToCard(int scriptLineNumber);  // sync left→right
    void updateCardMatchStyle(size_t clipIdx); // lightweight restyle after matchState change
    void syncLeftListFromScroll();     // sync right→left

    /// Snapshot-then-mutate helper for bulk operations.  Captures m_clips
    /// before running `mutate`, runs it, then pushes a LambdaCommand whose
    /// undo restores the snapshot and whose redo replays the new state.
    /// `rebuild` is called immediately after the mutation and again on each
    /// undo/redo to refresh the UI.  Skips the command push when the
    /// mutation produced no observable change.
    void runClipsMutationWithUndo(const std::string& description,
                                  std::function<void()> mutate,
                                  std::function<void()> rebuild = nullptr);

    // Audio playback / waveform helpers
    void loadAudioSamples();
    void playClip(size_t clipIdx);
    void togglePlayClip(size_t clipIdx);
    void pausePlayback();
    void stopPlayback();
    void seekPlayingClip(double timeSec);
    void scrubClipAt(size_t clipIdx, double timeSec);
    void autoTrimClip(size_t clipIdx);
    void openManualMatch(int lineNumber);
    void splitClip(size_t clipIdx);
    void mergeClipWithNext(size_t clipIdx);
    void updateSelectedClipHighlight();
    void updateTransportBar();
    QColor characterColor(const std::string& character) const;

    /// Walk up from a child widget to find which clip-list row it belongs to.
    /// Returns -1 if the widget is not inside any clip card.
    [[nodiscard]] int clipRowForWidget(QWidget* widget) const;

    /// Add a rich item to the audio file list (filename + details).
    void addAudioFileListItem(const QString& fullPath);

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

    /// Play/stop a raw audio file by index into m_audioPaths.
    void playAudioFile(size_t fileIndex);
    void stopFilePlayback();

    /// Find the MiniWaveformWidget for a given clip index via the card mapping.
    [[nodiscard]] MiniWaveformWidget* waveformForClip(int clipIdx) const;

    /// Get effective time ranges for a SyncClip, excluding deleted regions.
    /// Ported from Python _get_effective_ranges().
    static std::vector<std::pair<double,double>> getEffectiveRanges(const SyncClip& clip);

    // ── State ───────────────────────────────────────────────────────────
    CommandStack*     m_commandStack{nullptr};
    bool              m_exportTransactionActive{false};
    QString           m_lastExportError;
    AudioEngine*      m_audioEngine{nullptr};
    AVSyncClock*      m_savedSyncClock{nullptr};
    ShotPresetManager* m_shotPresetManager{nullptr};
    std::string        m_currentShow;  ///< Show of the open project (per-show defaults)

    std::unique_ptr<Transcriber> m_transcriber;
    std::unique_ptr<Script>      m_script;

    std::vector<SyncClip> m_clips;
    std::string           m_audioPath;       // Currently-transcribing audio path
    std::vector<std::string> m_audioPaths;   // All imported audio paths

    std::unordered_map<std::string, AudioSampleData> m_audioSamples;

    // Playback state
    int     m_playingClipIdx{-1};
    int     m_selectedClipIdx{-1};  // Currently highlighted clip row
    QTimer* m_playheadTimer{nullptr};
    std::vector<float> m_playbackBuffer;  // temp buffer for AudioEngine
    double  m_playbackSourceRate{44100.0}; // sample rate of current playback source

    // Time mapping: buffer frame offset → original source time (for non-destructive editing)
    // Each entry is {bufferFrameOffset, originalTimeSeconds} marking the start of a contiguous segment
    std::vector<std::pair<int64_t, double>> m_playbackTimeMap;

    // Unapproved TTS draft audition (never added to m_audioPaths/m_clips).
    std::vector<float> m_voiceDraftSamples;
    bool m_voiceDraftAuditionActive{false};

    // File preview playback (Import / Transcribe lists)
    int     m_previewFileIdx{-1};   // index into m_audioPaths, -1 = none
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
    mutable std::unordered_map<std::string, QColor> m_characterColors;

    bool m_scriptLoaded{false};
    bool m_audioImported{false};
    bool m_transcriptionDone{false};
    bool m_syncDone{false};
    bool m_restoring{false};        // true during restoreProjectState to skip redundant rebuilds
    bool m_cardsDirty{false};       // populateScriptList/populateClipList deferred until widget is shown
    bool m_manualMatchOpen{false};  // suppress event filter while ManualMatchDialog is open
    bool m_forwardingKey{false};      // re-entrancy guard for JKL sendEvent
    QProgressDialog* m_syncProgress{nullptr}; // shown during auto-sync
    QString m_lastScriptSource;   // For project persistence
    std::string m_scriptRawContent; // Raw text content loaded for this script (for offline restore)
    QString m_lastImportDir;      // Remembers last file dialog directory

    // Per-line audio file assignment: lineNumber → audio file path.
    // Set by the user via the audio file combo on unmatched cards.
    // Used as the pre-selected file when MANUAL match is opened.
    std::unordered_map<int, std::string> m_lineAudioFile;

    // Multi-file transcription state
    size_t m_currentTranscriptionIndex{0};
    size_t m_transcriptionRunTotal{0};
    size_t m_transcriptionRunCompleted{0};
    std::vector<TranscriptionResult> m_allTranscriptionResults;
    std::vector<size_t> m_pendingTranscriptionIndices;  ///< File indices still needing transcription

    // Transcription worker
    enum class TranscriptionState { Idle, Running, Cancelling };
    TranscriptionState m_transcriptionState{TranscriptionState::Idle};
    uint64_t m_transcriptionRunId{0};
    bool m_transcriptionRunHadSuccess{false};
    QString m_transcriptionRunError;
    QPointer<QThread>             m_workerThread;
    QPointer<TranscriptionWorker> m_worker;

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

    // Script session management
    std::unordered_map<std::string, ScriptSession> m_scriptSessions;
    std::string m_activeScriptKey;   // key into m_scriptSessions, empty = none
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
            return nullptr;  // caller must allocate
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
    QPointer<QWidget>                m_highlightedCard;          // currently highlighted card (glow effect)
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
