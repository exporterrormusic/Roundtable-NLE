/*
 * ComfyPanel — the COMFYUI page: turns the picked green-screen takes of a
 * shot-table script (AUDIO page) into framed 1080p driving clips for the
 * ComfyUI SCAIL workflow.
 *
 *   SHOTS  — every script line with its shot type, picked take, length and
 *            export status; per-recording WIDE / CLOSE-UP framing with
 *            per-shot overrides; tail padding; batch export via ffmpeg.
 *
 * All state lives in AudioSync's active session (ShowPipelineState), so it
 * follows script switching and is saved with the project.
 */

#pragma once

#include "ai/ShotPipeline.h"
#include "panels/comfy/ComfyWorkflow.h"

#include <QImage>
#include <QJsonObject>
#include <QPointer>
#include <QWidget>

#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class QButtonGroup;
class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QPlainTextEdit;
class QSplitter;
class QStackedWidget;
class QTableWidget;
class QLabel;
class QLineEdit;
class QProcess;
class QProgressBar;
class QPushButton;
class QRadioButton;
class QSpinBox;
class QHBoxLayout;
class QKeyEvent;
class QHeaderView;
class QTimer;
class QTreeWidget;
class QTreeWidgetItem;

namespace rt {

class AudioSync;
class CommandStack;
class ComfyRunner;
class Timeline;
class FramingEditor;
class VideoFrameGrabber;
class VideoFrameView;
class ComfyTrimBar;
struct ShowShot;

class ComfyPanel : public QWidget
{
    Q_OBJECT
public:
    explicit ComfyPanel(AudioSync* audioSync, QWidget* parent = nullptr);
    ~ComfyPanel() override;

    /// Rebuild the shot list from the AUDIO page's current script + takes.
    void refresh();

    /// For tests: the shot list and the RENDER ALL button.
    [[nodiscard]] QTreeWidget* shotTree() const noexcept { return m_shotTree; }
    [[nodiscard]] QPushButton* renderAllButton() const noexcept { return m_renderAllBtn; }

    /// Lay the chosen render of every shot onto `timeline` (after its
    /// existing content) as one undoable step. Returns shots placed; `error`
    /// gets a reason (0 placed) or a note about skipped lines.
    int assembleIntoTimeline(Timeline* timeline, CommandStack* stack, QString* error);

signals:
    /// "ASSEMBLE ON TIMELINE" — MainWindow picks the sequence and calls
    /// assembleIntoTimeline().
    void assembleRequested();

protected:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    struct ExportItem
    {
        std::string   key;
        ShotExportJob job;
        double        tailPadSec{0.0};
        std::string   signature;          ///< shotExportSignature() of `job`
    };

    void setupUi();
    void selectShot(int index);
    void loadFramingForSelection();
    void applyFraming(const ShotFraming& framing, bool commit);
    void updateFramingControls(const ShotFraming& framing);
    void updateOutputPreview();
    void updateShotRow(int index);
    void updateExportSummary();
    [[nodiscard]] QSize sourceSize(const std::string& path);
    [[nodiscard]] QString defaultOutputDir() const;
    [[nodiscard]] static QString findFfmpeg();
    [[nodiscard]] bool isExportable(const ShowShot& shot) const;

    /// Export (frame + trim + tail pad → 1920×1080 driving video) the given
    /// shots, then run `then`.  Always re-exports, so a render can never use
    /// a stale export after framing / take / pad edits.  Part of RENDER —
    /// there is no separate export step.
    void prepareShots(const std::vector<int>& indices, std::function<void()> then);
    void runNextExport();
    void finishExport(const QString& message);

    // Pages, in workflow order.
    enum PageId : int { kPageShots = 0, kPageQueue = 1, kPageSettings = 2 };
    void showPage(int id);
    /// Selected shot rows (indices into m_shots), in list order.
    [[nodiscard]] std::vector<int> selectedShotIndices() const;
    void showShotContextMenu(const QPoint& pos);
    /// Columns can be dragged wider/narrower and hidden from the header's
    /// right-click menu; the layout is remembered under `settingsKey`.
    static void makeColumnsAdjustable(QHeaderView* header, const QString& settingsKey);
    /// Live preview: a still frame, or an animated clip (KJNodes MP4 / WebP)
    /// decoded off-thread and looped.
    void showLivePreviewFrame(const QImage& image);
    void showLivePreviewClip(const QByteArray& clip, const QString& mime, double fps);
    /// Play the shot's take and adjust its in/out (AUDIO page's take player).
    void playAndTrimTake(int shotIndex);
    /// "PREPARING" / "QUEUED" / "RENDERING 3/8" while this shot is in the
    /// current prepare/render batch, else empty.
    [[nodiscard]] QString batchStatusFor(const std::string& key) const;
    [[nodiscard]] static int existingRenderCount(const ShotSettings& settings);
public:
    /// The render the timeline should use: the chosen one if its file still
    /// exists, else the newest one still on disk, else nullptr.
    [[nodiscard]] static const RenderVersion* usableRender(const ShotSettings& settings);
    [[nodiscard]] static bool needsRender(const ShotSettings& settings);
private:

    // RENDERS strip (SHOTS pane): every render on disk; click = use + view
    // big, right-click = use / open / show / delete.
    void rebuildRenderStrip();
    void updateRenderStripMarks();
    void useRender(int renderIndex, bool showBig);
    void showRenderMenu(int renderIndex, const QPoint& globalPos);
    void deleteRender(int renderIndex);
    void showRenderPreview();

    // Big view + transport (SHOTS pane).  Mode 0 = framing (the take:
    // plays with audio, in/out handles), 1 = the chosen render.
    void setBigView(int mode);
    void updateTransport();
    void togglePlayback();
    /// speed: 1 = normal (with audio), ±2/±4 shuttle, ±0.25 slow (silent).
    void startPlayback(double speed);
    void stopShotPlayback();
    void stepFrames(int frames);
    /// Load the shown take's (view range) or render's audio for preview.
    bool ensurePreviewAudio();
    /// A short burst of sound at t (scrub / step / handle drag).
    void scrubAudio(double t);
    [[nodiscard]] double frameStep();
    /// Premiere-style transport keys on the SHOTS page. True = handled.
    bool handleTransportKey(QKeyEvent* event, bool press);
    /// In / out on the render shown big (manual tail trim).
    void commitRenderTrim(double in, double out);
    void seekTo(double t);
    void showFrameAt(double t);
    void commitTrim(double in, double out);
    [[nodiscard]] bool framingMode() const noexcept { return m_bigMode == 0; }
    [[nodiscard]] const RenderVersion* shownRender() const;
    [[nodiscard]] double mediaDuration(const std::string& path);

    // ── Render / settings (ComfyPanelRender.cpp) ───────────────────────
    QWidget* setupSettingsPage();
    QWidget* setupQueuePage();
    QWidget* setupActionBar();
    void setupRunner();
    void refreshRenderPage();
    /// A ComfyUI console line in the log; a progress bar overwrites the
    /// previous progress line instead of stacking (like a terminal).
    void appendServerLine(const QString& line, bool progress);
    /// Fill EMPTY render settings (workflow, refs, default expression,
    /// launcher) from a local dev_defaults/comfy/comfy_defaults.json found
    /// above the exe.  Only for roundtable.exe (not tests); the file is
    /// gitignored and never shipped, so the distributed app starts blank.
    void applyDevDefaults();
    /// Loads + converts the workflow file; false (with a message) on error.
    bool loadWorkflow(QString* error);
    /// Render the given shots (indices into m_shots). `includeRendered`
    /// re-renders shots that already have a render.
    void startRender(const std::vector<int>& shotIndices, bool includeRendered, bool freshSeeds = false);
    /// After prepareShots(): queue the shots that exported OK in ComfyUI.
    void launchRender(const std::vector<int>& shotIndices, bool freshSeeds);
    [[nodiscard]] QString expressionFor(const ShowShot& shot) const;
    [[nodiscard]] QString rendersDir() const;
    void appendLog(const QString& line);
    void updateQueueRow(int jobIndex);
    void updateRenderButtons();

    AudioSync*                         m_audioSync{nullptr};
    std::unique_ptr<VideoFrameGrabber> m_grabber;
    std::vector<ShowShot>              m_shots;
    int                                m_selected{-1};
    std::unordered_map<std::string, QSize> m_sourceSizes;
    QImage                             m_sourceFrame;
    std::string                        m_sourcePath;   ///< recording m_sourceFrame came from
    bool                               m_updatingControls{false};

    // Header
    QLabel*        m_titleLabel{nullptr};
    QLabel*        m_hintLabel{nullptr};

    // Shot list
    QTreeWidget*   m_shotTree{nullptr};

    // Framing
    FramingEditor* m_framingEditor{nullptr};
    QLabel*        m_outputPreview{nullptr};
    QWidget*       m_renderStrip{nullptr};       ///< thumbnails of this shot's renders
    QHBoxLayout*   m_renderStripLayout{nullptr};
    QLabel*        m_renderStripEmpty{nullptr};
    struct RenderThumb { QPointer<VideoFrameView> view; QPointer<QLabel> caption; int index{-1}; };
    std::vector<RenderThumb> m_renderThumbs;
    int            m_bigRenderIndex{-1};          ///< render shown big (index into ShotSettings::renders)
    uint64_t       m_renderPreviewGeneration{0};  ///< reject frames from a previously selected render
    QStackedWidget* m_bigView{nullptr};          ///< FramingEditor | big render view
    VideoFrameView* m_bigRender{nullptr};
    QPushButton*   m_playBtn{nullptr};
    QLabel*        m_timeLabel{nullptr};
    ComfyTrimBar*  m_trimBar{nullptr};
    QTimer*        m_playTimer{nullptr};
    int            m_bigMode{0};
    bool           m_playing{false};
    bool           m_playWithAudio{false};
    double         m_playhead{0.0};
    double         m_speed{1.0};
    bool           m_kHeld{false};
    int            m_slowDir{0};
    QTimer*        m_slowHoldTimer{nullptr};
    bool           m_keyFilterInstalled{false};
    std::unordered_map<std::string, double> m_mediaFps;
    double         m_playStartHead{0.0};
    qint64         m_playStartMs{0};
    std::unordered_map<std::string, double> m_durations;   ///< media path → seconds
    std::unordered_map<std::string, std::pair<qint64, qint64>> m_mediaRevisions; ///< size + modification time
    QLabel*        m_framingTitle{nullptr};
    QRadioButton*  m_recordingModeBtn{nullptr};
    QRadioButton*  m_shotModeBtn{nullptr};
    QDoubleSpinBox* m_scaleSpin{nullptr};
    QDoubleSpinBox* m_xSpin{nullptr};
    QDoubleSpinBox* m_ySpin{nullptr};
    QSpinBox*      m_shotPadSpin{nullptr};

    // Shot files (SETTINGS page) + prepare progress (action bar)
    QLineEdit*     m_outputDirEdit{nullptr};
    QSpinBox*      m_defaultPadSpin{nullptr};
    QProgressBar*  m_progress{nullptr};
    std::function<void()>           m_afterExport;     ///< continuation of prepareShots()
    bool                            m_exportCancelled{false};
    std::unordered_set<std::string> m_preparedKeys;    ///< shots exported OK by the last prepare

    QPointer<QProcess>      m_ffmpeg;
    std::deque<ExportItem>  m_exportQueue;
    ExportItem              m_currentExport;
    int                     m_exportTotal{0};
    int                     m_exportDone{0};
    int                     m_exportFailed{0};
    std::string             m_encoder{"h264_nvenc"};
    QString                 m_lastError;

    QStackedWidget*  m_pages{nullptr};
    QButtonGroup*    m_pageButtons{nullptr};

    // Per-shot render controls (SHOTS page)
    QLineEdit*       m_expressionEdit{nullptr};
    QComboBox*       m_fpsCombo{nullptr};
    QLabel*          m_seedLabel{nullptr};

    // Assembly (SETTINGS page + action bar)
    QLineEdit*       m_backgroundEdit{nullptr};
    QLineEdit*       m_timelineTemplateEdit{nullptr};
    QDoubleSpinBox*  m_cuZoomSpin{nullptr};
    QDoubleSpinBox*  m_cuBackgroundXSpin{nullptr};
    QDoubleSpinBox*  m_cuBackgroundYSpin{nullptr};
    QDoubleSpinBox*  m_gapSpin{nullptr};
    QPushButton*     m_assembleBtn{nullptr};

    // SETTINGS / QUEUE pages + action bar
    QLineEdit*       m_urlEdit{nullptr};
    QLineEdit*       m_launcherEdit{nullptr};
    QLineEdit*       m_workflowEdit{nullptr};
    QLabel*          m_workflowStatus{nullptr};
    QLineEdit*       m_wideRefEdit{nullptr};
    QLineEdit*       m_closeRefEdit{nullptr};
    QLineEdit*       m_seedEdit{nullptr};
    QLineEdit*       m_defaultExprEdit{nullptr};
    QSpinBox*        m_frameLimitSpin{nullptr};
    QPushButton*     m_renderAllBtn{nullptr};
    QPushButton*     m_stopRenderBtn{nullptr};
    QLabel*          m_renderStatus{nullptr};
    QTableWidget*    m_queueTable{nullptr};
    QPlainTextEdit*  m_logView{nullptr};
    VideoFrameView*  m_livePreview{nullptr};    ///< ComfyUI's preview of the shot rendering now
    QLabel*          m_livePreviewLabel{nullptr};
    QSplitter*       m_consoleSplitter{nullptr}; ///< live preview over log
    QTimer*          m_livePreviewTimer{nullptr}; ///< steps m_livePreviewFrames
    std::vector<QImage> m_livePreviewFrames;
    size_t           m_livePreviewIndex{0};
    int              m_livePreviewGen{0};        ///< drops stale off-thread decodes
    QSplitter*       m_renderSplitter{nullptr};   ///< queue | log (sizes remembered)
    bool             m_lastLogWasProgress{false};

    ComfyRunner*     m_runner{nullptr};
    QJsonObject      m_workflowPrompt;
    QJsonObject      m_workflowUi;       ///< editor JSON (sent as extra_pnginfo)
    comfy::WorkflowSlots m_workflowTargets;
    QString          m_workflowLoadedFrom;
    std::vector<std::string> m_jobShotKeys;   ///< runner job index → shot key
};

} // namespace rt
