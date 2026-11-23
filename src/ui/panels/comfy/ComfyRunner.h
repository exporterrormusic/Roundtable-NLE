/*
 * ComfyRunner — unattended ComfyUI batch rendering for the COMFYUI page.
 *
 * For each shot: make sure the server is up (launching the owner's .bat if
 * needed), upload the exported driving clip, fill the workflow's numbered
 * nodes (video + fps, reference image, expression, seed, save prefix), queue
 * it, poll until done and download the result.
 *
 * Failure ladder (owner's rule — failures are nearly always ComfyUI itself):
 *   1st failure → restart ComfyUI, retry as-is
 *   2nd failure → restart ComfyUI, retry with unchanged quality settings
 *   3rd failure → restart ComfyUI (fresh for the next shot), skip this shot
 * A workflow VALIDATION error (HTTP 400) would fail every shot the same way,
 * so it stops the batch instead.
 *
 * Restart = kill whatever process tree listens on the server's port (plus
 * the one this runner launched) and relaunch the .bat.
 */

#pragma once

#include "panels/comfy/ComfyWorkflow.h"

#include <QElapsedTimer>
#include <QFile>
#include <QImage>
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QStringDecoder>

#include <functional>
#include <string>
#include <vector>

class QNetworkAccessManager;
class QNetworkReply;
class QProcess;
class QTimer;

namespace rt {

class ComfyEventStream;

struct RenderJob
{
    enum class State { Pending, Running, Done, Failed, Skipped, WaitingForNextPass };

    std::string key;            ///< pipeline shot key
    int         order{0};       ///< 1-based position in the script
    QString     label;          ///< "007 · STORY · CU"
    QString     drivingVideo;   ///< exported 1080p clip
    bool        closeUp{false};
    QString     expression;
    qint64      seed{-1};
    int         fps{16};
    double      tailPadSec{0.0};
    int         frames{0};      ///< SCAIL frames at `fps` (for ETA)

    State       state{State::Pending};
    int         attempt{0};     ///< attempt within the current phase
    int         fpsUsed{0};
    QString     status;
    QString     outputFile;
    QString     remoteVideo;    ///< name of the uploaded clip in ComfyUI's input folder
    qint64      elapsedMs{0};
    qint64      completedPhaseMs{0};
    bool        prepared{false};
    bool        generated{false};
    bool        staged{false};
    bool        hasArtifacts{false}; ///< a pass left files for this shot on the server
    QString     artifactKey;
    // Live progress from ComfyUI's event feed (0 = none yet).
    int         step{0};
    int         steps{0};
    QString     stepNode;       ///< title of the node reporting the steps (e.g. the sampler)
};

class ComfyRunner : public QObject
{
    Q_OBJECT
public:
    struct Config
    {
        QString                   url{QStringLiteral("http://127.0.0.1:8188")};
        QString                   launcher;           ///< run_nvidia_gpu.bat (empty → never launch)
        QString                   otherLauncher;      ///< other engine's configured local installation
        QJsonObject               prompt;             ///< API prompt (from convertUiWorkflow)
        /// The editor-format workflow, sent as extra_pnginfo like the ComfyUI
        /// editor does: VideoHelperSuite reads its latent-preview switch from
        /// it, and outputs get the workflow embedded.
        QJsonObject               uiWorkflow;
        comfy::WorkflowSlots      targets;
        QString                   wideReference;
        QString                   closeUpReference;
        QString                   rendersDir;
        QString                   filenamePrefix{QStringLiteral("roundtable")};
        int                       jobTimeoutSec{2 * 60 * 60};
        int                       serverStartTimeoutSec{5 * 60};
        int                       pollIntervalMs{2000};
        bool                      stagedBatch{true};
    };

    explicit ComfyRunner(QObject* parent = nullptr);
    ~ComfyRunner() override;

    void start(const Config& config, std::vector<RenderJob> jobs);
    /// Add shots to the batch that is running (false if none is). They use
    /// the running batch's workflow and references. Added during the first
    /// pass they join the passes; later they render whole after the passes.
    bool append(std::vector<RenderJob> jobs);
    /// Interrupts the running render and abandons the rest of the batch.
    void stop();
    /// Takes these jobs out of the queue (State::Skipped, "Removed") and frees
    /// their pass files on the server. The shot rendering right now is left
    /// alone (Stop ends it). Returns how many were removed.
    int remove(const std::vector<int>& indices);
    /// Interrupts only the shot rendering right now (State::Skipped,
    /// "Stopped") and goes on with the rest of the queue. False if no shot
    /// is rendering.
    bool cancelCurrent();
    /// Index of the shot rendering right now, or -1.
    [[nodiscard]] int runningIndex() const noexcept;
    [[nodiscard]] bool isRunning() const noexcept { return m_running; }
    [[nodiscard]] const std::vector<RenderJob>& jobs() const noexcept { return m_jobs; }
    /// Seconds per rendered frame measured so far (0 = unknown).
    [[nodiscard]] double secondsPerFrame() const noexcept;

    /// Launch the server now (without a batch). Result via log().
    void launchServer(const QString& url, const QString& launcher, const QString& otherLauncher = {},
                      bool onlyIfRunning = false);
    bool isStartingServer() const { return m_warming; }

    /// The launcher's "python ... main.py ..." line with the browser
    /// auto-open disabled, or empty if the .bat has no such line (then the
    /// .bat is run as-is).
    [[nodiscard]] static QString serverCommandFromLauncher(const QString& launcherPath);
    /// Folder the server runs in: the launcher's last "cd /d <dir>" before its
    /// main.py line, else the launcher's own folder.
    [[nodiscard]] static QString serverWorkingDirFromLauncher(const QString& launcherPath);

signals:
    void serverStartupChanged();
    void jobChanged(int index);
    void jobRendered(int index, const QString& file);
    void log(const QString& line);
    /// One line of the ComfyUI server's own console (only for a server we
    /// launched).  `progress` = a tqdm bar redrawn in place (CR), which the
    /// view should overwrite rather than append.
    void serverOutput(const QString& line, bool progress);
    /// Latest preview frame of the shot being rendered.
    void preview(const QImage& image);
    /// Latest animated preview (MP4 / WebP bytes) of the shot being rendered.
    void previewClip(const QByteArray& data, const QString& mime, double fps);
    void finished(int done, int failed, bool stopped);

private:
    using Done = std::function<void(bool ok, const QString& error)>;

    // Server
    void checkAlive(std::function<void(bool, QString)> then, int timeoutMs = 3000);
    void ensureServer(Done then, bool onlyIfRunning = false);
    void startServer(Done then);
    void onServerOutput();
    /// "Rendering · WanVideo Sampler 3/8 · 1:23 / ~4:10"
    [[nodiscard]] QString renderingStatus(const RenderJob& job) const;
    ComfyEventStream* m_events{nullptr};
    QStringDecoder m_serverDecoder{QStringDecoder::Utf8};
    QString        m_serverLineBuf;      ///< partial line awaiting CR / LF
    QString        m_serverLogPath;
    QFile          m_serverLog;          ///< open while a server we launched runs
    /// The console's own bar ("4/8 [") drives job.step for this prompt: the
    /// event feed reports a sampler step as soon as its model pass ends
    /// (update_absolute(step + 1)), one ahead of the console.
    bool           m_consoleDrivesStep{false};
    void waitForServer(bool wantAlive, int timeoutSec, Done then);
    void killServer(Done then);
    void restartServer(Done then);

    // HTTP
    QNetworkReply* get(const QString& path);
    QNetworkReply* postJson(const QString& path, const QJsonObject& body);
    void fireAndForget(const QString& path, const QJsonObject& body);
    void upload(const QString& localPath, const QString& remoteName, Done then,
                bool referenceImage = false);

    // Batch
    void nextJob();
    void runCurrentJob();
    void submitCurrent();
    void verifyH3Cutout(std::function<void()> then);
    /// Before a prompt that uses Roundtable's own nodes is queued: ask the
    /// server whether it has each one (once per server start). A missing node
    /// stops the batch with install instructions instead of ComfyUI's raw
    /// rejection.
    void verifyBatchNodes(QStringList nodeTypes, std::function<void()> then);
    void queuePrompt(const QJsonObject& prompt, const QJsonObject& sourcePrompt);
    void pollCurrent();
    void downloadOutput(const QJsonObject& outputs);
    /// `retry` false: a restart cannot fix it (a local file problem), so the
    /// shot fails at once and ComfyUI keeps running for the next one.
    void failCurrent(const QString& why, bool retry = true);
    void stopBatch(const QString& why);
    /// Ask the server to delete one shot's pass files.
    void releaseArtifacts(RenderJob& job);
    /// Release remaining pass files and unload the models after an early stop.
    void releaseAfterEarlyEnd();
    /// Polls /queue until `promptId` is no longer running or pending (or
    /// `triesLeft` runs out), then calls `then`. Keeps an interrupt from
    /// landing on the next shot's prompt.
    void waitForPromptGone(const QString& promptId, int triesLeft, std::function<void()> then);
    void finishBatch();
    void setStatus(const QString& status);
    [[nodiscard]] RenderJob& current() { return m_jobs[static_cast<size_t>(m_index)]; }

    QNetworkAccessManager* m_net{nullptr};
    QPointer<QProcess>     m_server;          ///< ComfyUI launched by us
    QString                m_serverLauncher;
    bool                   m_warming{false};
    QTimer*                m_pollTimer{nullptr};
    QElapsedTimer          m_jobClock;

    Config                 m_config;
    QJsonObject            m_sourcePrompt;
    std::vector<RenderJob> m_jobs;
    int                    m_index{-1};
    std::vector<int>        m_executionOrder;
    int                    m_orderPosition{-1};
    int                    m_stage{0}; // 0 = whole shot; SCAIL 1 = prepare, 2 = generate, 3 = finish; H3 see kH3StageCount
    int                    m_stageCount{3};
    bool                   m_running{false};
    bool                   m_stopRequested{false};
    QString                m_promptId;
    bool                   m_sweptLeftovers{false}; ///< this batch asked the server to clear old batches' files
    QSet<QString>          m_verifiedNodes;   ///< Roundtable node types this server is known to have
    QString                m_batchId;         ///< prefix of every artifact key in this batch
    QString                m_clientId;
    int                    m_pollFailures{0};
    QString                m_uploadedWide;
    QString                m_uploadedCloseUp;
    double                 m_renderSeconds{0.0};
    long long              m_renderFrames{0};
    int                    m_done{0};
    int                    m_failed{0};
    qint64                 m_generation{0};   ///< invalidates callbacks after stop()
};

} // namespace rt
