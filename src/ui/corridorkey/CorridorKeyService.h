/*
 * CorridorKeyService — runs CorridorKey for Ultra Key's "Render with
 * CorridorKey" button: the one-time setup (CorridorKeySetup + its dialog) and
 * a background queue of renders, one at a time.
 *
 * A render: plan (ai/CorridorKey.h) → ffmpeg extracts the clip's frames as
 * the app sees them → the worker keys them on the GPU under a hard memory
 * cap at below-normal priority → the clip is pointed at the result as one
 * undo step ("Use CorridorKey Render"). Renders live in "<project folder>/
 * CorridorKey Renders" and are reused when nothing changed. Waits while a
 * ComfyUI batch holds the GPU. Progress shows in Ultra Key's row and in the
 * activity center.
 *
 * Owned by MainWindow; current() lets Effect Controls reach it.
 */

#pragma once

#include "ai/CorridorKey.h"
#include "NotificationCenter.h"

#include <QElapsedTimer>
#include <QObject>
#include <QPointer>
#include <QProcess>
#include <QString>
#include <QStringList>

#include <functional>
#include <map>
#include <optional>
#include <vector>

class QTimer;
class QWidget;

namespace rt {

class CommandStack;
class CorridorKeySetup;
class CorridorKeySetupDialog;
class Project;
class Timeline;
class VideoClip;

class CorridorKeyService final : public QObject
{
    Q_OBJECT

public:
    struct EditContext
    {
        Project*      project{nullptr};
        CommandStack* stack{nullptr};
    };

    enum class JobState { Queued, Waiting, Extracting, Keying };

    struct JobStatus
    {
        JobState state{JobState::Queued};
        double   progress{0.0};      ///< 0..1
        QString  text;
        double   secondsLeft{-1.0};  ///< < 0 = unknown
    };

    explicit CorridorKeyService(QWidget* dialogParent);
    ~CorridorKeyService() override;

    [[nodiscard]] static CorridorKeyService* current() noexcept;

    void setEditContextProvider(std::function<EditContext()> provider) { m_context = std::move(provider); }
    /// True while something else (a ComfyUI batch) is using the GPU.
    void setGpuBusyProbe(std::function<bool()> probe) { m_gpuBusy = std::move(probe); }

    [[nodiscard]] const corridorkey::Layout& layout() const noexcept { return m_layout; }

    // ── Setup ───────────────────────────────────────────────────────────────
    [[nodiscard]] bool isInstalled() const;
    [[nodiscard]] bool isSetupRunning() const;
    [[nodiscard]] double setupProgress() const noexcept { return m_setupProgress; }
    [[nodiscard]] QString setupStage() const { return m_setupStage; }
    /// Open the setup dialog; with `startNow` the download starts without
    /// asking again (the installer already asked).
    void showSetup(bool startNow = false);
    void startSetup();
    void cancelSetup();

    // ── Renders ─────────────────────────────────────────────────────────────
    /// Queue a render of `clip` (keyed by Ultra Key `effectId`). Not set up
    /// yet → opens the setup and renders when it finishes. Returns false with
    /// `problem` when this clip cannot be rendered.
    bool renderClip(Timeline* sequence, VideoClip* clip, uint64_t effectId, QString* problem);
    void cancel(uint64_t clipId);
    [[nodiscard]] std::optional<JobStatus> jobStatus(uint64_t clipId) const;
    /// The last render of this clip failed: why (empty = it did not).
    [[nodiscard]] QString lastFailure(uint64_t clipId) const;

    /// Point a clip that uses a CorridorKey render back at its source and
    /// turn its Ultra Key back on (one undo step).
    bool backToUltraKey(Timeline* sequence, VideoClip* clip, uint64_t effectId, QString* problem);
    /// Back to the source, then render again (the render no longer covers
    /// what the clip shows — see renderIsStale).
    bool renderAgain(Timeline* sequence, VideoClip* clip, uint64_t effectId, QString* problem);

    [[nodiscard]] static bool usesRender(const VideoClip& clip);
    /// The clip plays a render that misses frames or area it now shows: a
    /// transition was added or lengthened, it was trimmed longer, or its crop
    /// opened up.
    [[nodiscard]] static bool renderIsStale(Timeline* sequence, const VideoClip& clip);

    /// "Steadier": renders may use about twice the GPU memory so CorridorKey
    /// keys more frames at once. Remembered for this computer.
    [[nodiscard]] bool steadier() const;
    void setSteadier(bool on);
    /// What Steadier costs on this GPU, for its tooltip.
    [[nodiscard]] QString steadierDescription() const;

signals:
    void setupChanged();
    void jobChanged(quint64 clipId);
    /// The clip now shows a render, or its source again.
    void clipSwapped(quint64 clipId);

private:
    struct Job
    {
        uint64_t                  id{0};
        uint64_t                  clipId{0};
        uint64_t                  effectId{0};
        uint64_t                  sequenceId{0};
        Project*                  project{nullptr};
        QString                   label;
        corridorkey::RenderPlan   plan;
        corridorkey::RenderRecord record;
        std::filesystem::path     output;
        std::filesystem::path     dir;
        JobState                  state{JobState::Queued};
        double                    progress{0.0};
        QString                   text;
        double                    estimate{0.0};
        double                    vramGB{8.0};
        QElapsedTimer             clock;
        bool                      sawDone{false};
        QString                   error;
        QStringList               tail;
        NotificationCenter::Id    task{0};
    };

    struct PendingRender
    {
        uint64_t sequenceId{0};
        uint64_t clipId{0};
        uint64_t effectId{0};
        Project* project{nullptr};
    };

    void startRequestedSetup();
    void startNext();
    void startExtract(Job& job);
    void startWorker(Job& job);
    void finishJob(uint64_t jobId, bool ok, const QString& message);
    void onWorkerLine(Job& job, const QString& line);
    void updateJob(Job& job);
    void killProcess();
    void pollFrames();
    [[nodiscard]] Job* findJob(uint64_t jobId);
    [[nodiscard]] Job* activeJob();
    [[nodiscard]] bool swapIn(uint64_t sequenceId, uint64_t clipId, uint64_t effectId, Project* project,
                              const std::filesystem::path& render, const corridorkey::RenderRecord& record,
                              QString* problem);
    [[nodiscard]] std::filesystem::path renderFolder(Project* project) const;
    [[nodiscard]] double vramCap(corridorkey::Steadiness steadiness);
    [[nodiscard]] QProcessEnvironment workerEnvironment() const;

    corridorkey::Layout             m_layout;
    QPointer<QWidget>               m_dialogParent;
    std::function<EditContext()>    m_context;
    std::function<bool()>           m_gpuBusy;

    QPointer<CorridorKeySetup>      m_setup;
    QPointer<CorridorKeySetupDialog> m_dialog;
    double                          m_setupProgress{0.0};
    QString                         m_setupStage;
    NotificationCenter::Id          m_setupTask{0};
    std::optional<PendingRender>    m_pending;

    std::vector<Job>                m_jobs;          ///< front = running when m_process is set
    std::map<uint64_t, QString>     m_failures;      ///< clipId → why the last render failed
    uint64_t                        m_nextJobId{1};
    QPointer<QProcess>              m_process;
    QTimer*                         m_retry{nullptr};
    QTimer*                         m_framePoll{nullptr};
    double                          m_gpuMemoryGB{0.0};   ///< 0 = not asked nvidia-smi yet
};

} // namespace rt
