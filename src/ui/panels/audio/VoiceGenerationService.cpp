#include "panels/audio/VoiceGenerationService.h"

#include "panels/audio/VoiceProviders.h"

#include "PathUtils.h"
#include "Settings.h"
#include "project/Project.h"

#include <spdlog/spdlog.h>

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QPointer>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QStorageInfo>
#include <QTimer>

namespace rt {

namespace {

constexpr auto kBreezeRootSetting = "voice/breezeInstallationRoot";
constexpr qint64 kDraftMaxAgeSeconds = 24 * 60 * 60;

QString voiceSafePathPart(QString value)
{
    value = value.trimmed();
    if (value.isEmpty()) value = QStringLiteral("Unassigned");
    value.replace(QRegularExpression(QStringLiteral(R"([<>:"/\\|?*\x00-\x1f])")),
                  QStringLiteral("_"));
    while (value.endsWith('.') || value.endsWith(' ')) value.chop(1);
    return value.left(80);
}

QString voiceDraftDirectory()
{
    return QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation))
        .filePath(QStringLiteral("Roundtable Voice Drafts"));
}

QString voiceReferenceCacheDirectory()
{
    return QDir(QStandardPaths::writableLocation(QStandardPaths::CacheLocation))
        .filePath(QStringLiteral("voice-reference-cache"));
}

QString ffmpegExecutable()
{
    return QString::fromUtf8(ROUNDTABLE_FFMPEG_EXE_PATH);
}

QString describeDuration(int milliseconds)
{
    return milliseconds >= 60000
        ? QStringLiteral("%1 minutes").arg(milliseconds / 60000)
        : QStringLiteral("%1 seconds").arg(milliseconds / 1000);
}

// ── Breeze installation discovery ───────────────────────────────────────
// Breeze may live in an existing SPEECH-TEXT-SPEECH install on any drive.
// Probing drives is slow (and can stall on disconnected network drives), so
// the result is computed once and reused until the user picks a new folder.

struct BreezeInstallation
{
    QString root;
    QString python;
    QString model;
    QString server;

    [[nodiscard]] bool complete() const
    {
        return QFileInfo::exists(python)
            && QFileInfo::exists(model)
            && QFileInfo::exists(server);
    }
};

QString firstExistingVoiceFile(const QStringList& candidates)
{
    for (const auto& candidate : candidates) {
        if (QFileInfo::exists(candidate)) return QDir::cleanPath(candidate);
    }
    return {};
}

BreezeInstallation breezeInstallationAt(const QString& selectedRoot)
{
    const QString root = QDir::cleanPath(selectedRoot.trimmed());
    if (root.isEmpty()) return {};
    const QDir directory(root);
    return {
        root,
        firstExistingVoiceFile({
            directory.filePath(QStringLiteral("runtime/python/python.exe")),
            directory.filePath(QStringLiteral(".venv/Scripts/python.exe"))
        }),
        firstExistingVoiceFile({
            directory.filePath(QStringLiteral("models/tts/breeze-tts-2-q8_0.gguf")),
            directory.filePath(QStringLiteral("models/breeze-tts-2-q8_0.gguf"))
        }),
        firstExistingVoiceFile({
            directory.filePath(QStringLiteral("runtime/audio-cpp/audiocpp_server.exe")),
            directory.filePath(QStringLiteral("audio-cpp/audiocpp_server.exe"))
        })
    };
}

BreezeInstallation scanForBreezeInstallation()
{
    const BreezeInstallation packaged{
        QString::fromUtf8(ROUNDTABLE_BREEZE_ROOT),
        QString::fromUtf8(ROUNDTABLE_BREEZE_PYTHON_PATH),
        QString::fromUtf8(ROUNDTABLE_BREEZE_MODEL_PATH),
        QString::fromUtf8(ROUNDTABLE_BREEZE_SERVER_PATH)
    };
    if (packaged.complete()) return packaged;

    QStringList roots;
    roots << appSettings().value(QLatin1String(kBreezeRootSetting)).toString();
    roots << qEnvironmentVariable("ROUNDTABLE_BREEZE_ROOT");
    for (const auto& volume : QStorageInfo::mountedVolumes()) {
        if (!volume.isValid() || !volume.isReady()) continue;
        roots << QDir(volume.rootPath()).filePath(
            QStringLiteral("1_PROGRAMS/AUDIO/SPEECH-TEXT-SPEECH"));
    }
    roots.removeAll(QString());
    roots.removeDuplicates();
    for (const auto& root : roots) {
        auto installation = breezeInstallationAt(root);
        if (installation.complete()) return installation;
    }
    return packaged;
}

struct BreezeInstallationCache
{
    bool valid{false};
    BreezeInstallation installation;
};

BreezeInstallationCache& breezeInstallationCache()
{
    static BreezeInstallationCache cache;
    return cache;
}

const BreezeInstallation& breezeInstallation()
{
    auto& cache = breezeInstallationCache();
    if (!cache.valid) {
        cache.installation = scanForBreezeInstallation();
        cache.valid = true;
    }
    return cache.installation;
}

// ── Per-provider runtime locations ──────────────────────────────────────

struct ProviderRuntime
{
    QString python;
    QString root;     // worker scratch/working directory
    QString model;
    QString server;   // Breeze only
};

ProviderRuntime providerRuntime(const QString& provider)
{
    if (provider == QStringLiteral("breeze")) {
        const auto& breeze = breezeInstallation();
        return {breeze.python, QString::fromUtf8(ROUNDTABLE_BREEZE_ROOT),
                breeze.model, breeze.server};
    }
    if (provider == QStringLiteral("fish-s2")) {
        return {QString::fromUtf8(ROUNDTABLE_FISH_S2_PYTHON_PATH),
                QString::fromUtf8(ROUNDTABLE_FISH_S2_ROOT),
                QString::fromUtf8(ROUNDTABLE_FISH_S2_MODEL_PATH), {}};
    }
    return {QString::fromUtf8(ROUNDTABLE_OMNIVOICE_PYTHON_PATH),
            QString::fromUtf8(ROUNDTABLE_OMNIVOICE_ROOT),
            QString::fromUtf8(ROUNDTABLE_OMNIVOICE_MODEL_PATH), {}};
}

void removeStaleDrafts()
{
    const QDir drafts(voiceDraftDirectory());
    if (!drafts.exists()) return;
    const QDateTime cutoff = QDateTime::currentDateTime().addSecs(-kDraftMaxAgeSeconds);
    for (const auto& draft : drafts.entryInfoList(
             {QStringLiteral("DRAFT_*.wav")}, QDir::Files)) {
        if (draft.lastModified() < cutoff) QFile::remove(draft.absoluteFilePath());
    }
}

} // namespace

VoiceGenerationService::VoiceGenerationService(QObject* parent)
    : QObject(parent)
{
    if (auto* app = QCoreApplication::instance()) {
        connect(app, &QCoreApplication::aboutToQuit,
                this, &VoiceGenerationService::shutdown,
                Qt::DirectConnection);
    }
    // Drafts that were never approved or discarded (e.g. the app closed with
    // one pending) would otherwise accumulate in the temp folder.
    removeStaleDrafts();
}

VoiceGenerationService::~VoiceGenerationService()
{
    shutdown();
}

bool VoiceGenerationService::isBusy() const noexcept
{
    return m_hasCurrent || !m_queue.isEmpty();
}

bool VoiceGenerationService::isModelResident() const noexcept
{
    return m_process != nullptr || m_retiring != nullptr;
}

bool VoiceGenerationService::providerInstalled(const QString& provider)
{
    const auto* info = findVoiceProvider(provider);
    if (!info) return false;
    if (!QFileInfo::exists(ffmpegExecutable())) return false;
    const auto runtime = providerRuntime(provider);
    if (!QFileInfo::exists(runtime.python)) return false;
    if (info->modelIsFile) {
        if (!QFileInfo(runtime.model).isFile()) return false;
    } else {
        const QDir modelDir(runtime.model);
        for (const auto& required : info->requiredModelFiles) {
            if (!QFileInfo::exists(modelDir.filePath(required))) return false;
        }
    }
    if (provider == QStringLiteral("breeze") && !QFileInfo::exists(runtime.server))
        return false;
    return true;
}

QString VoiceGenerationService::providerInstallHint(const QString& provider)
{
    if (!findVoiceProvider(provider))
        return QStringLiteral("This engine is disabled in this build.");
    if (providerInstalled(provider)) return {};
    if (!QFileInfo::exists(ffmpegExecutable())) {
        return QStringLiteral("FFmpeg was not found at %1. Voice references are "
                              "prepared with ffmpeg.exe.")
            .arg(QDir::toNativeSeparators(ffmpegExecutable()));
    }
    if (provider == QStringLiteral("breeze")) {
        return QStringLiteral(
            "Breeze-TTS-2 is not connected. Choose Locate Existing Breeze, or run "
            "tools\\install_voice_models.ps1 from the project root.");
    }
    return QStringLiteral("Run tools\\install_voice_models.ps1 from the project root.");
}

QString VoiceGenerationService::breezeInstallationRoot()
{
    const auto& installation = breezeInstallation();
    return installation.complete() ? installation.root : QString{};
}

bool VoiceGenerationService::configureBreezeInstallation(
    const QString& root, QString* error)
{
    const auto installation = breezeInstallationAt(root);
    if (!installation.complete()) {
        if (error) {
            *error = QStringLiteral(
                "That folder does not contain the Breeze Python runtime, Q8 model, and "
                "audio.cpp server. Select the SPEECH-TEXT-SPEECH root folder.");
        }
        return false;
    }
    auto settings = appSettings();
    settings.setValue(QLatin1String(kBreezeRootSetting), installation.root);
    settings.sync();
    auto& cache = breezeInstallationCache();
    cache.installation = installation;
    cache.valid = true;
    return true;
}

void VoiceGenerationService::enqueue(const VoiceGenerationRequest& request)
{
    if (request.text.trimmed().isEmpty()) {
        emit generationFailed(request, QStringLiteral("Enter text to generate."));
        return;
    }
    if (!m_launchOverride && !providerInstalled(request.provider)) {
        emit generationFailed(request, providerInstallHint(request.provider));
        return;
    }

    const bool wasBusy = isBusy();
    m_queue.push_back(request);
    if (!wasBusy) emit busyChanged(true);
    processNext();
}

void VoiceGenerationService::abortRequests(const QString& reason)
{
    const bool wasBusy = isBusy();
    m_queue.clear();
    if (m_hasCurrent) {
        const auto aborted = m_current;
        m_hasCurrent = false;
        m_currentOutput.clear();
        emit generationFailed(aborted, reason);
    }
    if (wasBusy) emit busyChanged(false);
}

void VoiceGenerationService::cancel()
{
    const bool inFlight = m_hasCurrent;
    abortRequests(QStringLiteral("Generation cancelled."));
    // The worker only reads stdin between requests, so an in-flight
    // generation (or a model still loading for it) can only be stopped by
    // ending the process.
    if (inFlight) retireWorker(true);
    emit statusChanged(QStringLiteral("Cancelled"));
}

void VoiceGenerationService::unloadModel()
{
    const bool inFlight = m_hasCurrent;
    abortRequests(QStringLiteral("Generation cancelled: model unloaded."));
    if (!isModelResident()) {
        emit statusChanged(QStringLiteral("No voice model is loaded."));
        return;
    }
    m_announceUnload = true;
    retireWorker(inFlight);
    if (isModelResident()) {
        emit statusChanged(QStringLiteral("Unloading voice model..."));
    } else {
        m_announceUnload = false;
        emit statusChanged(QStringLiteral("Voice model unloaded. GPU memory released."));
    }
}

void VoiceGenerationService::shutdown()
{
    m_queue.clear();
    m_hasCurrent = false;
    m_currentOutput.clear();
    for (QProcess* process : {m_process, m_retiring}) {
        if (!process) continue;
        disconnect(process, nullptr, this, nullptr);
        if (process->state() != QProcess::NotRunning) {
            process->write("{\"op\":\"shutdown\"}\n");
            process->closeWriteChannel();
            if (!process->waitForFinished(3000)) {
                process->kill();
                process->waitForFinished(2000);
            }
        }
        delete process;
    }
    m_process = nullptr;
    m_retiring = nullptr;
    m_workerReady = false;
    m_activeProvider.clear();
    m_residentReported = false;
}

QString VoiceGenerationService::approveDraft(const VoiceGenerationRequest& request,
                                              const QString& draftPath,
                                              QString* error) const
{
    if (draftPath.isEmpty() || !QFileInfo::exists(draftPath)) {
        if (error) *error = QStringLiteral("The generated audition draft no longer exists.");
        return {};
    }

    const QString suffix = QFileInfo(draftPath).suffix().isEmpty()
        ? QStringLiteral("wav") : QFileInfo(draftPath).suffix().toLower();
    const QString approvedPath = buildApprovedOutputPath(request, suffix);
    if (approvedPath.isEmpty()) {
        if (error) *error = QStringLiteral(
            "Could not create the approved voice clip beside its source audio.");
        return {};
    }
    if (QFileInfo(draftPath).absoluteFilePath() == QFileInfo(approvedPath).absoluteFilePath())
        return approvedPath;

    // A rename is instant on the same volume. It fails across volumes (the
    // draft normally sits in the system temp folder while imported media may
    // be on another drive), so fall back to copy, then remove the draft only
    // after the approved file is safe.
    if (QFile::rename(draftPath, approvedPath)) return approvedPath;
    if (!QFile::copy(draftPath, approvedPath)) {
        if (error) *error = QStringLiteral("Could not save the approved voice clip to %1")
                                .arg(approvedPath);
        return {};
    }
    QFile::remove(draftPath);
    return approvedPath;
}

void VoiceGenerationService::processNext()
{
    if (m_hasCurrent || m_queue.isEmpty()) return;

    m_current = m_queue.takeFirst();
    m_hasCurrent = true;
    m_announceUnload = false;
    m_currentOutput = buildOutputPath(m_current);
    if (m_currentOutput.isEmpty()) {
        failCurrent(QStringLiteral("Could not create the voice audition draft folder."));
        return;
    }

    if (m_process && m_activeProvider == m_current.provider) {
        if (m_workerReady) sendCurrentRequest();
        return;   // still loading: "ready" sends the request
    }
    // Different engine: the old model must leave VRAM before the new one
    // loads. The retired worker's exit starts the new one.
    if (m_process) retireWorker(false);
    if (m_retiring) {
        emit statusChanged(QStringLiteral("Releasing the previous voice model..."));
        return;
    }
    startWorker(m_current.provider);
}

VoiceGenerationService::WorkerLaunch VoiceGenerationService::launchFor(
    const QString& provider) const
{
    if (m_launchOverride) return m_launchOverride(provider);

    const auto runtime = providerRuntime(provider);
    QStringList arguments{
        QString::fromUtf8(ROUNDTABLE_VOICE_WORKER_PATH),
        QStringLiteral("--provider"), provider,
        QStringLiteral("--runtime-root"), runtime.root,
        QStringLiteral("--model"), runtime.model,
        QStringLiteral("--ffmpeg"), ffmpegExecutable(),
        QStringLiteral("--reference-cache"), voiceReferenceCacheDirectory()
    };
    if (!runtime.server.isEmpty())
        arguments << QStringLiteral("--server") << runtime.server;
    return {runtime.python, arguments, runtime.root};
}

void VoiceGenerationService::startWorker(const QString& provider)
{
    const WorkerLaunch launch = launchFor(provider);
    if (!launch.workingDirectory.isEmpty() && !QDir().mkpath(launch.workingDirectory)) {
        failCurrent(QStringLiteral("Could not create the voice runtime folder."));
        return;
    }
    m_process = new QProcess(this);
    m_activeProvider = provider;
    m_workerReady = false;
    m_stdoutBuffer.clear();
    m_stderrBuffer.clear();
    m_lastWorkerError.clear();

    connect(m_process, &QProcess::readyReadStandardOutput,
            this, &VoiceGenerationService::handleStdout);
    connect(m_process, &QProcess::readyReadStandardError,
            this, &VoiceGenerationService::handleStderr);
    connect(m_process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
            this, [this](int code, QProcess::ExitStatus) { handleWorkerExit(code); });
    connect(m_process, &QProcess::errorOccurred, this,
            [this](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart || !m_process) return;
        const QString detail = m_process->errorString();
        m_process->deleteLater();
        m_process = nullptr;
        m_activeProvider.clear();
        ++m_loadGeneration;
        updateResidency();
        failCurrent(QStringLiteral("Could not start the voice worker: %1").arg(detail));
    });

    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    if (!launch.workingDirectory.isEmpty()) {
        env.insert(QStringLiteral("HF_HOME"), QDir(launch.workingDirectory)
                       .absoluteFilePath(QStringLiteral("../huggingface")));
        m_process->setWorkingDirectory(launch.workingDirectory);
    }
    env.insert(QStringLiteral("PYTHONUTF8"), QStringLiteral("1"));
    env.insert(QStringLiteral("PYTHONUNBUFFERED"), QStringLiteral("1"));
    m_process->setProcessEnvironment(env);

    const auto* info = findVoiceProvider(provider);
    emit statusChanged(info ? info->loadingStatus
                            : QStringLiteral("Loading voice model..."));
    spdlog::info("[voice] starting {} worker", provider.toStdString());
    m_process->start(launch.program, launch.arguments, QIODevice::ReadWrite);
    updateResidency();

    // The generation id makes a timer from an earlier (unloaded) worker
    // harmless to a newer worker of the same engine that is still loading.
    const quint64 generation = ++m_loadGeneration;
    const int timeout = m_loadTimeoutMs;
    QTimer::singleShot(timeout, this, [this, generation, timeout]() {
        if (generation != m_loadGeneration || !m_process || m_workerReady) return;
        retireWorker(true);
        failCurrent(QStringLiteral("The voice model did not finish loading within %1.")
                        .arg(describeDuration(timeout)));
    });
}

void VoiceGenerationService::retireWorker(bool immediate)
{
    if (!m_process) return;
    QProcess* process = m_process;
    const bool graceful = !immediate && m_workerReady;
    m_process = nullptr;
    m_workerReady = false;
    m_activeProvider.clear();
    ++m_loadGeneration;
    disconnect(process, nullptr, this, nullptr);

    if (m_retiring) {
        // Only one worker may be exiting at a time; end the older one now.
        disconnect(m_retiring, nullptr, this, nullptr);
        m_retiring->kill();
        m_retiring->deleteLater();
        m_retiring = nullptr;
    }
    if (process->state() == QProcess::NotRunning) {
        process->deleteLater();
        updateResidency();
        return;
    }

    m_retiring = process;
    connect(process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
            this, &VoiceGenerationService::onRetiredWorkerFinished);
    if (graceful) {
        // An idle worker reads this immediately and closes its model/server.
        process->write("{\"op\":\"shutdown\"}\n");
        process->closeWriteChannel();
        QPointer<QProcess> guard(process);
        QTimer::singleShot(m_retireTimeoutMs, this, [guard]() {
            if (guard && guard->state() != QProcess::NotRunning) guard->kill();
        });
    } else {
        // Loading or mid-generation: the worker is not reading stdin.
        process->kill();
    }
    updateResidency();
}

void VoiceGenerationService::onRetiredWorkerFinished()
{
    if (m_retiring) {
        m_retiring->deleteLater();
        m_retiring = nullptr;
    }
    updateResidency();
    if (m_hasCurrent && !m_process) {
        startWorker(m_current.provider);
    } else if (m_announceUnload && !isModelResident()) {
        m_announceUnload = false;
        emit statusChanged(QStringLiteral("Voice model unloaded. GPU memory released."));
    }
}

void VoiceGenerationService::updateResidency()
{
    const bool resident = isModelResident();
    if (resident == m_residentReported) return;
    m_residentReported = resident;
    emit modelResidentChanged(resident);
}

void VoiceGenerationService::sendCurrentRequest()
{
    if (!m_process || !m_workerReady || !m_hasCurrent) return;

    QJsonArray references;
    for (const auto& reference : m_current.references) {
        references.push_back(QJsonObject{
            {QStringLiteral("audio"), reference.audioFile},
            {QStringLiteral("text"), reference.transcript},
            {QStringLiteral("start"), reference.start},
            {QStringLiteral("end"), reference.end}
        });
    }

    const QJsonObject request{
        {QStringLiteral("op"), QStringLiteral("generate")},
        {QStringLiteral("text"), m_current.text},
        {QStringLiteral("reference_segments"), references},
        {QStringLiteral("speed"), m_current.speed},
        {QStringLiteral("duration"), m_current.targetDuration},
        {QStringLiteral("seed"), m_current.seed},
        {QStringLiteral("output"), m_currentOutput}
    };
    m_process->write(QJsonDocument(request).toJson(QJsonDocument::Compact) + '\n');
    emit statusChanged(QStringLiteral("Generating %1...").arg(m_current.character));
}

void VoiceGenerationService::handleStdout()
{
    if (!m_process) return;
    m_stdoutBuffer += m_process->readAllStandardOutput();
    while (true) {
        const auto newline = m_stdoutBuffer.indexOf('\n');
        if (newline < 0) break;
        const QByteArray line = m_stdoutBuffer.left(newline).trimmed();
        m_stdoutBuffer.remove(0, newline + 1);
        if (line.isEmpty()) continue;

        QJsonParseError parseError;
        const auto doc = QJsonDocument::fromJson(line, &parseError);
        if (parseError.error != QJsonParseError::NoError || !doc.isObject()) continue;
        const auto obj = doc.object();
        const QString event = obj.value(QStringLiteral("event")).toString();
        if (event == QStringLiteral("ready")) {
            m_workerReady = true;
            const auto* info = findVoiceProvider(m_activeProvider);
            emit statusChanged(QStringLiteral("%1 ready").arg(
                info ? info->displayName : m_activeProvider));
            sendCurrentRequest();
        } else if (event == QStringLiteral("status")) {
            emit statusChanged(obj.value(QStringLiteral("message")).toString());
        } else if (event == QStringLiteral("done")) {
            if (!m_hasCurrent) continue;
            const auto completed = m_current;
            const QString output = obj.value(QStringLiteral("output")).toString(m_currentOutput);
            const double duration = obj.value(QStringLiteral("duration")).toDouble();
            m_hasCurrent = false;
            m_currentOutput.clear();
            emit statusChanged(QStringLiteral("Draft ready for audition"));
            emit generationFinished(completed, output, duration);
            if (m_queue.isEmpty()) emit busyChanged(false);
            processNext();
        } else if (event == QStringLiteral("error")) {
            failCurrent(obj.value(QStringLiteral("message")).toString(
                QStringLiteral("Voice generation failed.")));
        }
    }
}

void VoiceGenerationService::handleStderr()
{
    // Worker diagnostics (model loading, audio.cpp logs, tracebacks) go to the
    // application log rather than the status line, where they used to flicker
    // past unreadably and were then lost.
    if (!m_process) return;
    m_stderrBuffer += m_process->readAllStandardError();
    while (true) {
        const auto newline = m_stderrBuffer.indexOf('\n');
        if (newline < 0) break;
        const QString line = QString::fromUtf8(m_stderrBuffer.left(newline)).trimmed();
        m_stderrBuffer.remove(0, newline + 1);
        if (line.isEmpty()) continue;
        spdlog::info("[voice] {}", line.toStdString());
        m_lastWorkerError = line;
    }
}

void VoiceGenerationService::handleWorkerExit(int exitCode)
{
    if (m_process) {
        handleStderr();
        m_process->deleteLater();
    }
    m_process = nullptr;
    m_workerReady = false;
    m_activeProvider.clear();
    ++m_loadGeneration;
    updateResidency();
    spdlog::warn("[voice] worker exited with code {}", exitCode);
    if (m_hasCurrent) {
        QString error = QStringLiteral("Voice worker exited unexpectedly (code %1).")
                            .arg(exitCode);
        if (!m_lastWorkerError.isEmpty())
            error += QStringLiteral(" Last message: %1").arg(m_lastWorkerError);
        failCurrent(error);
    }
}

void VoiceGenerationService::failCurrent(const QString& error)
{
    if (!m_hasCurrent) return;
    const auto failed = m_current;
    m_hasCurrent = false;
    m_currentOutput.clear();
    emit generationFailed(failed, error);
    emit statusChanged(error);
    if (m_queue.isEmpty()) emit busyChanged(false);
    processNext();
}

QString VoiceGenerationService::buildOutputPath(const VoiceGenerationRequest& request) const
{
    QDir dir(voiceDraftDirectory());
    if (!dir.mkpath(QStringLiteral("."))) return {};

    QString words = request.text.simplified().left(42);
    words.replace(QRegularExpression(QStringLiteral(R"([^\p{L}\p{N}]+)")), QStringLiteral("_"));
    words = words.trimmed();
    if (words.isEmpty()) words = QStringLiteral("line");
    const QString stamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss_zzz"));
    return dir.filePath(QStringLiteral("DRAFT_%1_%2_%3.wav")
        .arg(voiceSafePathPart(request.character), stamp, words));
}

QString VoiceGenerationService::buildApprovedOutputPath(
    const VoiceGenerationRequest& request, const QString& suffix) const
{
    QString destinationDirectory;
    // The first automatic reference is the best approved source clip after
    // confidence/duration sorting. Manual reference mode also supplies its
    // selected imported track here.
    for (const auto& reference : request.references) {
        const QFileInfo source(reference.audioFile);
        if (source.exists() && source.isFile()) {
            destinationDirectory = source.absolutePath();
            break;
        }
    }
    if (destinationDirectory.isEmpty() && m_project && !m_project->filePath().empty()) {
        const QString projectFile = QString::fromStdString(pathToUtf8(m_project->filePath()));
        destinationDirectory = QDir(QFileInfo(projectFile).absolutePath())
                                   .filePath(QStringLiteral("Generated Audio"));
    }
    if (destinationDirectory.isEmpty()) {
        destinationDirectory = QDir(QStandardPaths::writableLocation(
            QStandardPaths::AppLocalDataLocation)).filePath(QStringLiteral("Generated Audio"));
    }
    QDir dir(destinationDirectory);
    if (!dir.mkpath(QStringLiteral("."))) return {};

    const QString character = voiceSafePathPart(request.character).toUpper();
    const QString stamp = QDateTime::currentDateTime().toString(
        QStringLiteral("yyyyMMdd-HHmmss-zzz"));
    QString candidate = dir.filePath(QStringLiteral("%1-%2.%3")
        .arg(character, stamp, suffix));
    for (int duplicate = 2; QFileInfo::exists(candidate); ++duplicate) {
        candidate = dir.filePath(QStringLiteral("%1-%2-%3.%4")
            .arg(character, stamp).arg(duplicate).arg(suffix));
    }
    return candidate;
}

} // namespace rt
