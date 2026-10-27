/*
 * CorridorKeyService.cpp — see CorridorKeyService.h.
 */

#include "corridorkey/CorridorKeyService.h"

#include "corridorkey/CorridorKeySetup.h"
#include "corridorkey/CorridorKeySetupDialog.h"

#include "AppPaths.h"
#include "Constants.h"
#include "PathUtils.h"
#include "QtHelpers.h"
#include "command/CommandStack.h"
#include "command/LambdaCommand.h"
#include "decode/VideoDecoder.h"
#include "edit/EditSession.h"
#include "project/Project.h"
#include "timeline/Timeline.h"
#include "timeline/Track.h"
#include "timeline/VideoClip.h"

#include <QDir>
#include <QFile>
#include <QSettings>
#include <QTimer>
#include <QWidget>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <filesystem>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace rt {

namespace ck = corridorkey;
namespace fs = std::filesystem;

namespace {

CorridorKeyService* g_corridorKeyService = nullptr;

constexpr const char* kCkCalibrationKey = "CorridorKey/secondsPerMegapixelFrame";

QString ckPath(const fs::path& p)
{
    return QString::fromStdString(pathToUtf8(p));
}

Timeline* ckSequenceById(Project* project, uint64_t id)
{
    if (!project) return nullptr;
    for (size_t i = 0; i < project->sequenceCount(); ++i)
        if (Timeline* seq = project->sequence(i); seq && seq->id() == id) return seq;
    return nullptr;
}

Track* ckTrackOf(Timeline* sequence, uint64_t clipId)
{
    if (!sequence) return nullptr;
    for (size_t t = 0; t < sequence->trackCount(); ++t) {
        Track* track = sequence->track(t);
        if (track && track->findClipIndexById(clipId) < track->clipCount()) return track;
    }
    return nullptr;
}

VideoClip* ckClipById(Timeline* sequence, uint64_t clipId)
{
    Track* track = ckTrackOf(sequence, clipId);
    return track ? dynamic_cast<VideoClip*>(track->clip(track->findClipIndexById(clipId))) : nullptr;
}

/// Frames the clip shows past its cuts (dissolves, wipes, pushes).
ck::Coverage ckCoverage(Timeline* sequence, const Clip& clip)
{
    const Track* track = ckTrackOf(sequence, clip.id());
    return track ? ck::transitionCoverage(*track, clip) : ck::Coverage{};
}

constexpr const char* kCkSteadierKey = "CorridorKey/steadier";

int64_t ckFrameTicks(const Timeline* sequence)
{
    double fps = sequence ? sequence->settings().frameRate() : 30.0;
    if (!(fps > 0.0)) fps = 30.0;
    return std::max<int64_t>(1, static_cast<int64_t>(double(kTicksPerSecond) / fps));
}

bool ckProbe(const std::string& path, VideoStreamInfo& info)
{
    VideoDecoder decoder;
    if (!decoder.open(utf8ToPath(path), /*forceSoftware=*/true)) return false;
    info = decoder.info();
    return true;
}

void ckLowPriority(QProcess& process)
{
#ifdef _WIN32
    process.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* args) {
        args->flags |= CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS;
    });
#else
    (void)process;
#endif
}

void ckRemoveTree(const fs::path& dir)
{
    std::error_code error;
    fs::remove_all(dir, error);
}

QStringList ckArgs(const std::vector<std::string>& args)
{
    QStringList out;
    for (const auto& a : args) out << QString::fromStdString(a);
    return out;
}

} // namespace

CorridorKeyService::CorridorKeyService(QWidget* dialogParent)
    : QObject(dialogParent)
    , m_layout(ck::layoutFor(AppPaths::root()))
    , m_dialogParent(dialogParent)
{
    g_corridorKeyService = this;
    // The installer leaves this file when "Download CorridorKey" was ticked.
    QTimer::singleShot(3000, this, &CorridorKeyService::startRequestedSetup);
    // The GPU's memory size (for the Steadier tooltip), asked in the background.
    QTimer::singleShot(5000, this, [this]() {
        if (m_gpuMemoryGB > 0.0 || !isInstalled()) return;
        auto* smi = new QProcess(this);
        ckLowPriority(*smi);
        connect(smi, &QProcess::finished, this, [this, smi](int code, QProcess::ExitStatus) {
            smi->deleteLater();
            const QString line = QString::fromUtf8(smi->readAllStandardOutput()).section('\n', 0, 0);
            if (code != 0) return;
            if (const auto gpu = ck::parseNvidiaSmiLine(line.toStdString()); gpu && m_gpuMemoryGB <= 0.0)
                m_gpuMemoryGB = gpu->memoryGB;
        });
        connect(smi, &QProcess::errorOccurred, smi, &QObject::deleteLater);
        smi->start(QStringLiteral("nvidia-smi"),
                   {QStringLiteral("--query-gpu=name,compute_cap,driver_version,memory.total"),
                    QStringLiteral("--format=csv,noheader,nounits")});
    });
}

CorridorKeyService::~CorridorKeyService()
{
    if (g_corridorKeyService == this) g_corridorKeyService = nullptr;
    killProcess();
    for (const Job& job : m_jobs) ckRemoveTree(job.dir);
}

CorridorKeyService* CorridorKeyService::current() noexcept
{
    return g_corridorKeyService;
}

bool CorridorKeyService::usesRender(const VideoClip& clip)
{
    return ck::recordForMedia(clip.mediaPath()).has_value();
}

bool CorridorKeyService::renderIsStale(Timeline* sequence, const VideoClip& clip)
{
    const auto record = ck::recordForMedia(clip.mediaPath());
    return record && !ck::renderCovers(clip, *record, ckCoverage(sequence, clip), ckFrameTicks(sequence));
}

bool CorridorKeyService::steadier() const
{
    return QSettings().value(QString::fromLatin1(kCkSteadierKey), false).toBool();
}

void CorridorKeyService::setSteadier(bool on)
{
    QSettings().setValue(QString::fromLatin1(kCkSteadierKey), on);
}

QString CorridorKeyService::steadierDescription() const
{
    if (m_gpuMemoryGB <= 0.0)
        return tr("Keys more frames at once for steadier edges. Renders may use about twice the "
                  "GPU memory, which can slow other GPU work on this computer while they run.");
    const double standard = ck::vramCapGB(m_gpuMemoryGB, ck::Steadiness::Standard);
    const double steadier = ck::vramCapGB(m_gpuMemoryGB, ck::Steadiness::Steadier);
    return tr("Keys more frames at once for steadier edges. Renders may use up to %1 GB of GPU "
              "memory instead of %2 GB, which can slow other GPU work on this computer while they run.")
        .arg(QString::number(steadier, 'f', 0), QString::number(standard, 'f', 0));
}

// ── Setup ────────────────────────────────────────────────────────────────────

bool CorridorKeyService::isInstalled() const
{
    return ck::isInstalled(m_layout);
}

bool CorridorKeyService::isSetupRunning() const
{
    return m_setup && m_setup->isRunning();
}

void CorridorKeyService::startRequestedSetup()
{
    std::error_code error;
    if (!fs::exists(m_layout.setupRequestFile(), error)) return;
    fs::remove(m_layout.setupRequestFile(), error);
    if (isInstalled()) return;
    spdlog::info("CorridorKey: setup requested by the installer");
    showSetup(/*startNow=*/true);
}

void CorridorKeyService::showSetup(bool startNow)
{
    if (!m_dialog) {
        m_dialog = new CorridorKeySetupDialog(this, m_dialogParent);
        m_dialog->setAttribute(Qt::WA_DeleteOnClose);
    }
    m_dialog->show();
    m_dialog->raise();
    m_dialog->activateWindow();
    if (startNow && !isSetupRunning() && !isInstalled()) startSetup();
}

void CorridorKeyService::startSetup()
{
    if (isSetupRunning()) return;
    if (!m_setup) {
        m_setup = new CorridorKeySetup(m_layout, this);
        connect(m_setup, &CorridorKeySetup::progressChanged, this,
                [this](double progress, const QString& stage, const QString& detail) {
            m_setupProgress = progress;
            m_setupStage = detail.isEmpty() ? stage : stage + QStringLiteral(" — ") + detail;
            if (auto* center = NotificationCenter::current(); center && m_setupTask)
                center->updateTask(m_setupTask, int(progress * 100.0), m_setupStage);
            emit setupChanged();
        });
        connect(m_setup, &CorridorKeySetup::finished, this, [this](bool ok, const QString& message) {
            if (auto* center = NotificationCenter::current(); center && m_setupTask)
                center->finishTask(m_setupTask, ok,
                                   ok ? tr("CorridorKey is installed. Use it from Ultra Key's "
                                           "\"Render with CorridorKey\" button.")
                                      : message);
            m_setupTask = 0;
            m_setupStage = ok ? QString() : message;
            emit setupChanged();
            if (ok && m_pending) {
                const PendingRender pending = *m_pending;
                m_pending.reset();
                const EditContext ctx = m_context ? m_context() : EditContext{};
                if (ctx.project == pending.project) {
                    Timeline* sequence = ckSequenceById(ctx.project, pending.sequenceId);
                    QString problem;
                    if (VideoClip* clip = ckClipById(sequence, pending.clipId);
                        clip && !renderClip(sequence, clip, pending.effectId, &problem)
                        && !problem.isEmpty()) {
                        if (auto* center = NotificationCenter::current())
                            center->postWarning(tr("CorridorKey"), problem);
                    }
                }
            }
        });
    }
    m_setupProgress = 0.0;
    m_setupStage = tr("Starting");
    if (auto* center = NotificationCenter::current())
        m_setupTask = center->beginTask(tr("Setting up CorridorKey"), m_setupStage, 0,
                                        [this]() { cancelSetup(); }, [this]() { startSetup(); });
    m_setup->start();
    emit setupChanged();
}

void CorridorKeyService::cancelSetup()
{
    if (m_setup) m_setup->cancel();
}

// ── Renders ──────────────────────────────────────────────────────────────────

fs::path CorridorKeyService::renderFolder(Project* project) const
{
    if (project && !project->filePath().empty())
        return project->filePath().parent_path() / "CorridorKey Renders";
    return utf8ToPath(userDataDir().toUtf8().toStdString()) / "CorridorKey Renders";
}

double CorridorKeyService::vramCap(ck::Steadiness steadiness)
{
    if (m_gpuMemoryGB <= 0.0) {
        double memory = 0.0;
        CorridorKeySetup::checkGpu(nullptr, &memory);
        m_gpuMemoryGB = memory > 0.0 ? memory : 8.0;
    }
    return ck::vramCapGB(m_gpuMemoryGB, steadiness);
}

bool CorridorKeyService::renderClip(Timeline* sequence, VideoClip* clip, uint64_t effectId,
                                    QString* problem)
{
    auto refuse = [&](const QString& why) {
        if (problem) *problem = why;
        return false;
    };
    if (!sequence || !clip) return refuse(tr("Select a video clip first."));
    const EditContext ctx = m_context ? m_context() : EditContext{};
    if (!ctx.project || !ctx.stack) return refuse(tr("Open a project first."));
    if (usesRender(*clip)) return refuse(tr("This clip already uses a CorridorKey render."));
    if (jobStatus(clip->id())) return true;

    if (!isInstalled()) {
        m_pending = PendingRender{sequence->id(), clip->id(), effectId, ctx.project};
        showSetup(false);
        return true;
    }

    VideoStreamInfo info;
    if (!ckProbe(clip->mediaPath(), info))
        return refuse(tr("Could not open this clip's media file."));
    ck::PlanResult planned = ck::planRender(*clip, info, ckFrameTicks(sequence), ckCoverage(sequence, *clip));
    if (!planned.plan) return refuse(QString::fromStdString(planned.problem));
    planned.plan->steadiness = steadier() ? ck::Steadiness::Steadier : ck::Steadiness::Standard;
    const ck::RenderPlan& plan = *planned.plan;

    std::error_code error;
    const fs::path source = utf8ToPath(plan.sourcePath);
    const auto bytes = fs::file_size(source, error);
    const auto modified = fs::last_write_time(source, error).time_since_epoch().count();
    const std::string key = ck::renderKey(plan, error ? 0 : bytes, int64_t(modified));
    const fs::path folder = renderFolder(ctx.project);
    const fs::path output = folder / utf8ToPath(ck::renderFileName(plan, key));

    // Rendered before with the same source, range and crop: use it again.
    if (fs::exists(output, error)) {
        if (auto record = ck::readRecord(ck::recordPathFor(output)); record && record->key == key) {
            QString why;
            if (swapIn(sequence->id(), clip->id(), effectId, ctx.project, output, *record, &why)) return true;
            return refuse(why);
        }
    }

    fs::create_directories(folder, error);
    if (error) return refuse(tr("Could not create %1.").arg(ckPath(folder)));

    Job job;
    job.id = m_nextJobId++;
    job.clipId = clip->id();
    job.effectId = effectId;
    job.sequenceId = sequence->id();
    job.project = ctx.project;
    job.label = QString::fromStdString(clip->label().empty() ? pathToUtf8(utf8ToPath(plan.sourcePath).stem())
                                                             : clip->label());
    job.plan = plan;
    job.record = ck::makeRecord(plan, *clip, key);
    job.output = output;
    job.dir = m_layout.jobsDir() / std::to_string(job.id);
    const double perFrame = QSettings().value(QString::fromLatin1(kCkCalibrationKey), 0.65).toDouble();
    job.estimate = ck::estimateSeconds(plan, std::clamp(perFrame, 0.05, 20.0));
    job.vramGB = vramCap(plan.steadiness);
    job.text = m_process ? tr("Queued") : tr("Starting");
    m_failures.erase(job.clipId);
    spdlog::info("CorridorKey: queued clip {} frames {}-{} rect {}x{} ({}), est {:.0f}s",
                 job.clipId, plan.firstFrame, plan.lastFrame, plan.keyRect.w, plan.keyRect.h,
                 pathToUtf8(output), job.estimate);
    if (auto* center = NotificationCenter::current()) {
        const uint64_t clipId = job.clipId;
        job.task = center->beginTask(tr("CorridorKey: %1").arg(job.label),
                                     tr("Queued (%1)").arg(QString::fromStdString(ck::formatDuration(job.estimate))),
                                     0, [this, clipId]() { cancel(clipId); });
    }
    m_jobs.push_back(std::move(job));
    emit jobChanged(m_jobs.back().clipId);
    startNext();
    return true;
}

bool CorridorKeyService::swapIn(uint64_t sequenceId, uint64_t clipId, uint64_t effectId, Project* project,
                                const fs::path& render, const ck::RenderRecord& record, QString* problem)
{
    auto refuse = [&](const QString& why) {
        if (problem) *problem = why;
        return false;
    };
    const EditContext ctx = m_context ? m_context() : EditContext{};
    if (!ctx.project || ctx.project != project || !ctx.stack)
        return refuse(tr("The project was closed, so the render was kept but not applied. "
                         "Render the clip again to use it."));
    Timeline* sequence = ckSequenceById(ctx.project, sequenceId);
    VideoClip* clip = ckClipById(sequence, clipId);
    if (!clip) return refuse(tr("The clip was removed before CorridorKey finished."));
    if (clip->mediaPath() != record.sourcePath)
        return refuse(tr("The clip's media changed before CorridorKey finished. Render it again."));

    // The clip must still show only frames (and only the area) the render
    // has, its transitions included.
    VideoStreamInfo info;
    if (!ckProbe(clip->mediaPath(), info)) return refuse(tr("Could not open this clip's media file."));
    const auto now = ck::planRender(*clip, info, ckFrameTicks(sequence), ckCoverage(sequence, *clip));
    if (!now.plan) return refuse(QString::fromStdString(now.problem));
    const ck::Rect& need = now.plan->keyRect;
    const ck::Rect& have = record.keyRect;
    if (now.plan->firstFrame < record.firstFrame || now.plan->lastFrame > record.lastFrame
        || need.x < have.x || need.y < have.y || need.x + need.w > have.x + have.w
        || need.y + need.h > have.y + have.h)
        return refuse(tr("The clip was trimmed, given a longer transition, or its crop opened up "
                         "while CorridorKey was rendering. Render it again."));

    auto context = m_context;
    const std::string renderPath = pathToUtf8(render);
    auto apply = [context, sequenceId, clipId, effectId, renderPath, record](bool toRender) {
        const EditContext c = context ? context() : EditContext{};
        if (VideoClip* v = ckClipById(ckSequenceById(c.project, sequenceId), clipId)) {
            if (toRender) ck::useRender(*v, renderPath, record, effectId);
            else ck::useSource(*v, record, effectId);
        }
    };
    executeTimelineEdit(ctx.stack, sequence, std::make_unique<LambdaCommand>(
        "Use CorridorKey Render", [apply]() { apply(true); }, [apply]() { apply(false); }));
    spdlog::info("CorridorKey: clip {} now uses {}", clipId, renderPath);
    emit clipSwapped(clipId);
    return true;
}

bool CorridorKeyService::backToUltraKey(Timeline* sequence, VideoClip* clip, uint64_t effectId,
                                        QString* problem)
{
    const EditContext ctx = m_context ? m_context() : EditContext{};
    if (!sequence || !clip || !ctx.stack) return false;
    const auto record = ck::recordForMedia(clip->mediaPath());
    if (!record) {
        if (problem) *problem = tr("This clip does not use a CorridorKey render.");
        return false;
    }
    std::error_code error;
    if (!fs::exists(utf8ToPath(record->sourcePath), error)) {
        if (problem) *problem = tr("The original footage is missing:\n%1")
                                    .arg(QString::fromStdString(record->sourcePath));
        return false;
    }
    auto context = m_context;
    const uint64_t sequenceId = sequence->id(), clipId = clip->id();
    const std::string renderPath = clip->mediaPath();
    const ck::RenderRecord rec = *record;
    auto apply = [context, sequenceId, clipId, effectId, renderPath, rec](bool toSource) {
        const EditContext c = context ? context() : EditContext{};
        if (VideoClip* v = ckClipById(ckSequenceById(c.project, sequenceId), clipId)) {
            if (toSource) ck::useSource(*v, rec, effectId);
            else ck::useRender(*v, renderPath, rec, effectId);
        }
    };
    executeTimelineEdit(ctx.stack, sequence, std::make_unique<LambdaCommand>(
        "Back to Ultra Key", [apply]() { apply(true); }, [apply]() { apply(false); }));
    emit clipSwapped(clipId);
    return true;
}

bool CorridorKeyService::renderAgain(Timeline* sequence, VideoClip* clip, uint64_t effectId,
                                     QString* problem)
{
    if (!backToUltraKey(sequence, clip, effectId, problem)) return false;
    return renderClip(sequence, clip, effectId, problem);
}

std::optional<CorridorKeyService::JobStatus> CorridorKeyService::jobStatus(uint64_t clipId) const
{
    for (const Job& job : m_jobs) {
        if (job.clipId != clipId) continue;
        JobStatus status{job.state, job.progress, job.text, -1.0};
        if (job.state == JobState::Keying || job.state == JobState::Extracting) {
            const double elapsed = double(job.clock.elapsed()) / 1000.0;
            status.secondsLeft = job.progress > 0.15
                ? elapsed / job.progress * (1.0 - job.progress)
                : std::max(0.0, job.estimate - elapsed);
        } else {
            status.secondsLeft = job.estimate;
        }
        return status;
    }
    return std::nullopt;
}

QString CorridorKeyService::lastFailure(uint64_t clipId) const
{
    const auto it = m_failures.find(clipId);
    return it == m_failures.end() ? QString() : it->second;
}

void CorridorKeyService::cancel(uint64_t clipId)
{
    for (size_t i = 0; i < m_jobs.size(); ++i) {
        if (m_jobs[i].clipId != clipId) continue;
        const bool running = i == 0 && m_process;
        if (running) {
            const uint64_t jobId = m_jobs[i].id;
            killProcess();
            finishJob(jobId, false, tr("Cancelled"));
            return;
        }
        Job job = std::move(m_jobs[i]);
        m_jobs.erase(m_jobs.begin() + static_cast<std::ptrdiff_t>(i));
        if (auto* center = NotificationCenter::current(); center && job.task) center->cancelTask(job.task);
        ckRemoveTree(job.dir);
        emit jobChanged(clipId);
        return;
    }
}

CorridorKeyService::Job* CorridorKeyService::findJob(uint64_t jobId)
{
    for (Job& job : m_jobs)
        if (job.id == jobId) return &job;
    return nullptr;
}

CorridorKeyService::Job* CorridorKeyService::activeJob()
{
    return m_jobs.empty() ? nullptr : &m_jobs.front();
}

void CorridorKeyService::updateJob(Job& job)
{
    if (auto* center = NotificationCenter::current(); center && job.task) {
        QString detail = job.text;
        if (const auto status = jobStatus(job.clipId); status && status->secondsLeft >= 0.0)
            detail += QStringLiteral(" · ") + tr("%1 left").arg(
                QString::fromStdString(ck::formatDuration(status->secondsLeft)));
        center->updateTask(job.task, int(job.progress * 100.0), detail);
    }
    emit jobChanged(job.clipId);
}

void CorridorKeyService::startNext()
{
    if (m_process || m_jobs.empty()) return;
    Job& job = m_jobs.front();
    if (m_gpuBusy && m_gpuBusy()) {
        job.state = JobState::Waiting;
        job.text = tr("Waiting for the ComfyUI render to finish");
        updateJob(job);
        if (!m_retry) {
            m_retry = new QTimer(this);
            m_retry->setSingleShot(true);
            connect(m_retry, &QTimer::timeout, this, &CorridorKeyService::startNext);
        }
        m_retry->start(10000);
        return;
    }
    startExtract(job);
}

QProcessEnvironment CorridorKeyService::workerEnvironment() const
{
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    for (const char* name : {"PYTHONPATH", "PYTHONHOME", "VIRTUAL_ENV", "CONDA_PREFIX"})
        env.remove(QString::fromLatin1(name));
    env.insert(QStringLiteral("PYTHONUTF8"), QStringLiteral("1"));
    env.insert(QStringLiteral("PYTHONNOUSERSITE"), QStringLiteral("1"));
    env.insert(QStringLiteral("HF_HOME"), ckPath(m_layout.hfHome()));
    env.insert(QStringLiteral("HF_HUB_OFFLINE"), QStringLiteral("1"));
    env.insert(QStringLiteral("CUDA_MODULE_LOADING"), QStringLiteral("LAZY"));
    return env;
}

void CorridorKeyService::startExtract(Job& job)
{
    const fs::path ffmpeg = AppPaths::ffmpegExecutable();
    if (ffmpeg.empty()) return finishJob(job.id, false, tr("FFmpeg was not found next to the app."));
    ckRemoveTree(job.dir);
    std::error_code error;
    fs::create_directories(job.dir / "frames", error);
    fs::create_directories(job.dir / "tmp", error);
    if (error) return finishJob(job.id, false, tr("Could not create %1.").arg(ckPath(job.dir)));

    job.state = JobState::Extracting;
    job.text = tr("Preparing frames");
    job.progress = 0.0;
    job.clock.start();
    updateJob(job);

    auto* process = new QProcess(this);
    m_process = process;
    process->setProcessChannelMode(QProcess::SeparateChannels);
    ckLowPriority(*process);
    const uint64_t jobId = job.id;
    connect(process, &QProcess::finished, this, [this, process, jobId](int code, QProcess::ExitStatus status) {
        process->deleteLater();
        if (m_process != process) return;
        m_process = nullptr;
        if (m_framePoll) m_framePoll->stop();
        Job* job = findJob(jobId);
        if (!job) return;
        size_t frames = 0;
        std::error_code e;
        for (fs::directory_iterator it(job->dir / "frames", e), end; !e && it != end; it.increment(e)) ++frames;
        if (status != QProcess::NormalExit || code != 0 || frames == 0) {
            const QString err = QString::fromUtf8(process->readAllStandardError()).trimmed();
            return finishJob(jobId, false, tr("FFmpeg could not read the clip's frames.") +
                                               (err.isEmpty() ? QString() : QStringLiteral("\n") + err.right(600)));
        }
        if (int64_t(frames) != job->plan.frameCount())
            return finishJob(jobId, false, tr("FFmpeg extracted %1 of %2 frames. The source may be "
                                              "damaged or shorter than it says.")
                                               .arg(frames).arg(job->plan.frameCount()));
        startWorker(*job);
    });
    if (!m_framePoll) {
        m_framePoll = new QTimer(this);
        m_framePoll->setInterval(1000);
        connect(m_framePoll, &QTimer::timeout, this, &CorridorKeyService::pollFrames);
    }
    m_framePoll->start();
    const QStringList args = ckArgs(ck::extractArgs(job.plan, job.dir / "frames"));
    spdlog::info("CorridorKey: ffmpeg {}", args.join(' ').toStdString());
    process->start(ckPath(ffmpeg), args);
}

void CorridorKeyService::pollFrames()
{
    Job* job = activeJob();
    if (!job || job->state != JobState::Extracting) return;
    size_t frames = 0;
    std::error_code e;
    for (fs::directory_iterator it(job->dir / "frames", e), end; !e && it != end; it.increment(e)) ++frames;
    job->progress = 0.05 * double(frames) / double(std::max<int64_t>(1, job->plan.frameCount()));
    updateJob(*job);
}

void CorridorKeyService::startWorker(Job& job)
{
    job.state = JobState::Keying;
    job.text = tr("Loading CorridorKey");
    job.progress = 0.05;
    updateJob(job);

    auto* process = new QProcess(this);
    m_process = process;
    process->setProcessChannelMode(QProcess::SeparateChannels);
    process->setProcessEnvironment(workerEnvironment());
    process->setWorkingDirectory(ckPath(job.dir));
    ckLowPriority(*process);
    const uint64_t jobId = job.id;
    auto log = std::make_shared<QFile>(ckPath(job.dir / "worker.log"));
    log->open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text);

    connect(process, &QProcess::readyReadStandardOutput, this, [this, process, jobId]() {
        Job* job = findJob(jobId);
        while (process->canReadLine()) {
            const QString line = QString::fromUtf8(process->readLine());
            if (job) onWorkerLine(*job, line);
        }
    });
    connect(process, &QProcess::readyReadStandardError, this, [this, process, jobId, log]() {
        const QByteArray bytes = process->readAllStandardError();
        log->write(bytes);
        log->flush();
        if (Job* job = findJob(jobId)) {
            for (const QString& line : QString::fromUtf8(bytes).split('\n', Qt::SkipEmptyParts)) {
                job->tail << line.trimmed();
                if (job->tail.size() > 15) job->tail.removeFirst();
            }
        }
    });
    connect(process, &QProcess::finished, this,
            [this, process, jobId, log](int code, QProcess::ExitStatus status) {
        process->deleteLater();
        log->close();
        if (m_process != process) return;
        m_process = nullptr;
        Job* job = findJob(jobId);
        if (!job) return;
        while (process->canReadLine()) onWorkerLine(*job, QString::fromUtf8(process->readLine()));
        std::error_code e;
        if (status == QProcess::NormalExit && code == 0 && job->sawDone && fs::exists(job->output, e))
            return finishJob(jobId, true, {});
        QString why = job->error;
        if (why.isEmpty()) why = job->tail.isEmpty() ? tr("CorridorKey stopped unexpectedly.")
                                                     : job->tail.join('\n').right(800);
        finishJob(jobId, false, why);
    });

    const fs::path ffmpeg = AppPaths::ffmpegExecutable();
    const QStringList args = ckArgs(ck::workerArgs(m_layout, job.plan, job.dir / "frames", job.output,
                                                   job.dir / "tmp", ffmpeg, job.vramGB));
    spdlog::info("CorridorKey: worker {} {}", pathToUtf8(m_layout.venvPython()), args.join(' ').toStdString());
    process->start(ckPath(m_layout.venvPython()), args);
}

void CorridorKeyService::onWorkerLine(Job& job, const QString& line)
{
    const ck::WorkerEvent event = ck::parseWorkerLine(line.toStdString());
    switch (event.kind) {
    case ck::WorkerEvent::Kind::Progress:
        job.progress = 0.05 + 0.95 * event.progress;
        if (!event.text.empty()) job.text = QString::fromStdString(event.text);
        updateJob(job);
        break;
    case ck::WorkerEvent::Kind::Status:
        spdlog::info("CorridorKey: {}", event.text);
        break;
    case ck::WorkerEvent::Kind::Done:
        job.sawDone = true;
        break;
    case ck::WorkerEvent::Kind::Error:
        job.error = QString::fromStdString(event.text);
        break;
    case ck::WorkerEvent::Kind::Log:
        break;
    }
}

void CorridorKeyService::finishJob(uint64_t jobId, bool ok, const QString& message)
{
    Job* found = findJob(jobId);
    if (!found) return;
    Job job = std::move(*found);
    m_jobs.erase(std::find_if(m_jobs.begin(), m_jobs.end(), [jobId](const Job& j) { return j.id == jobId; }));
    if (m_framePoll) m_framePoll->stop();

    QString note = message;
    bool applied = false;
    if (ok) {
        std::string error;
        if (!ck::writeRecord(ck::recordPathFor(job.output), job.record, &error)) {
            ok = false;
            note = QString::fromStdString(error);
        } else {
            // Learn this machine's speed for the next estimate.
            const double seconds = double(job.clock.elapsed()) / 1000.0;
            const double work = double(job.plan.frameCount()) * job.plan.keyedMegapixels();
            if (work > 1.0 && seconds > 30.0) {
                QSettings settings;
                const double old = settings.value(QString::fromLatin1(kCkCalibrationKey), 0.65).toDouble();
                const double measured = std::clamp((seconds - 25.0) / work, 0.05, 20.0);
                settings.setValue(QString::fromLatin1(kCkCalibrationKey), 0.5 * old + 0.5 * measured);
            }
            spdlog::info("CorridorKey: rendered clip {} in {:.0f}s", job.clipId, seconds);
            QString why;
            applied = swapIn(job.sequenceId, job.clipId, job.effectId, job.project, job.output, job.record, &why);
            if (!applied) note = why;
        }
    }

    if (auto* center = NotificationCenter::current(); center && job.task) {
        if (message == tr("Cancelled")) {
            center->finishTask(job.task, false, {}, false);
        } else if (ok && applied) {
            center->finishTask(job.task, true, tr("%1 now uses its CorridorKey render.").arg(job.label));
        } else if (ok) {
            center->finishTask(job.task, true, tr("%1 was rendered. %2").arg(job.label, note));
        } else {
            center->finishTask(job.task, false, tr("CorridorKey could not render %1: %2").arg(job.label, note));
        }
    }
    if (!ok && message != tr("Cancelled")) {
        m_failures[job.clipId] = note;
        spdlog::warn("CorridorKey: clip {} failed: {}", job.clipId, note.toStdString());
    }
    // Keep a failed job's log for diagnosis; frames are large, drop them.
    if (ok || message == tr("Cancelled")) ckRemoveTree(job.dir);
    else ckRemoveTree(job.dir / "frames");
    emit jobChanged(job.clipId);
    startNext();
}

void CorridorKeyService::killProcess()
{
    if (!m_process || m_process->state() == QProcess::NotRunning) return;
    QPointer<QProcess> process = m_process;
    m_process = nullptr;   // its finished() is ignored from here on
    // End the whole tree: the worker runs ffmpeg as a child.
    QProcess::execute(QStringLiteral("taskkill"),
                      {QStringLiteral("/PID"), QString::number(process->processId()),
                       QStringLiteral("/T"), QStringLiteral("/F")});
    if (process) process->waitForFinished(5000);
}

} // namespace rt
