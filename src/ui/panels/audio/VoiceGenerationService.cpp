#include "panels/audio/VoiceGenerationService.h"

#include "PathUtils.h"
#include "project/Project.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTimer>

#include <algorithm>

namespace rt {

namespace {

QString cleanPathPart(QString value)
{
    value = value.trimmed();
    if (value.isEmpty()) value = QStringLiteral("Unassigned");
    value.replace(QRegularExpression(QStringLiteral(R"([<>:"/\\|?*\x00-\x1f])")),
                  QStringLiteral("_"));
    while (value.endsWith('.') || value.endsWith(' ')) value.chop(1);
    return value.left(80);
}

QString pythonPathFor(const QString& provider)
{
    if (provider == QStringLiteral("fish-s2"))
        return QString::fromUtf8(ROUNDTABLE_FISH_S2_PYTHON_PATH);
    return QString::fromUtf8(ROUNDTABLE_OMNIVOICE_PYTHON_PATH);
}

QString rootPathFor(const QString& provider)
{
    if (provider == QStringLiteral("fish-s2"))
        return QString::fromUtf8(ROUNDTABLE_FISH_S2_ROOT);
    return QString::fromUtf8(ROUNDTABLE_OMNIVOICE_ROOT);
}

QString modelPathFor(const QString& provider)
{
    if (provider == QStringLiteral("fish-s2"))
        return QString::fromUtf8(ROUNDTABLE_FISH_S2_MODEL_PATH);
    return QString::fromUtf8(ROUNDTABLE_OMNIVOICE_MODEL_PATH);
}

} // namespace

VoiceGenerationService::VoiceGenerationService(QObject* parent)
    : QObject(parent)
{
    if (auto* app = QCoreApplication::instance()) {
        connect(app, &QCoreApplication::aboutToQuit,
                this, &VoiceGenerationService::unloadModel,
                Qt::DirectConnection);
    }
}

VoiceGenerationService::~VoiceGenerationService()
{
    unloadModel();
}

bool VoiceGenerationService::isBusy() const noexcept
{
    return m_hasCurrent || !m_queue.isEmpty();
}

bool VoiceGenerationService::providerBuilt(const QString& provider)
{
    if (provider == QStringLiteral("fish-s2")) {
#ifdef ROUNDTABLE_HAS_FISH_S2
        return true;
#else
        return false;
#endif
    }
    if (provider == QStringLiteral("omnivoice")) {
#ifdef ROUNDTABLE_HAS_OMNIVOICE
        return true;
#else
        return false;
#endif
    }
    return false;
}

bool VoiceGenerationService::providerInstalled(const QString& provider)
{
    if (!providerBuilt(provider)) return false;
    const QString python = pythonPathFor(provider);
    const QString model = modelPathFor(provider);
    if (!QFileInfo::exists(python)) return false;
    const QDir modelDir(model);
    if (provider == QStringLiteral("fish-s2")) {
        return QFileInfo::exists(modelDir.filePath(QStringLiteral("codec.pth")))
            && QFileInfo::exists(modelDir.filePath(QStringLiteral("config.json")))
            && QFileInfo::exists(modelDir.filePath(
                QStringLiteral("model.safetensors.index.json")));
    }
    return QFileInfo::exists(modelDir.filePath(QStringLiteral("model.safetensors")))
        && QFileInfo::exists(modelDir.filePath(QStringLiteral("config.json")))
        && QDir(modelDir.filePath(QStringLiteral("audio_tokenizer"))).exists();
}

QString VoiceGenerationService::providerInstallHint(const QString& provider)
{
    if (!providerBuilt(provider))
        return QStringLiteral("This engine is disabled in this build.");
    if (providerInstalled(provider)) return {};
    return QStringLiteral("Run tools\\install_voice_models.ps1 from the project root.");
}

void VoiceGenerationService::enqueue(const VoiceGenerationRequest& request)
{
    if (request.text.trimmed().isEmpty()) {
        emit generationFailed(request, QStringLiteral("Enter text to generate."));
        return;
    }
    if (!providerInstalled(request.provider)) {
        emit generationFailed(request, providerInstallHint(request.provider));
        return;
    }

    const bool wasBusy = isBusy();
    m_queue.push_back(request);
    if (!wasBusy) emit busyChanged(true);
    processNext();
}

void VoiceGenerationService::cancel()
{
    m_cancelled = true;
    m_queue.clear();
    if (m_hasCurrent) {
        const auto cancelled = m_current;
        stopWorker();
        m_hasCurrent = false;
        emit generationFailed(cancelled, QStringLiteral("Generation cancelled."));
    }
    emit statusChanged(QStringLiteral("Cancelled"));
    emit busyChanged(false);
}

void VoiceGenerationService::unloadModel()
{
    m_cancelled = true;
    m_queue.clear();
    if (m_hasCurrent) {
        const auto cancelled = m_current;
        m_hasCurrent = false;
        m_currentOutput.clear();
        emit generationFailed(cancelled, QStringLiteral("Generation cancelled: model unloaded."));
    }
    stopWorker();
    emit busyChanged(false);
    emit statusChanged(QStringLiteral("Voice model unloaded. GPU memory released."));
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

    // QFile::rename cannot move between volumes (the draft is normally in the
    // system temp directory while imported media may be on another drive).
    // Copy first, then remove the draft only after the approved file is safe.
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

    m_cancelled = false;
    m_current = m_queue.takeFirst();
    m_hasCurrent = true;
    m_currentOutput = buildOutputPath(m_current);
    if (m_currentOutput.isEmpty()) {
        failCurrent(QStringLiteral("Could not create the voice audition draft folder."));
        return;
    }

    if (!m_process || m_activeProvider != m_current.provider) {
        stopWorker();
        startWorker(m_current.provider);
    } else if (m_workerReady) {
        sendCurrentRequest();
    }
}

void VoiceGenerationService::startWorker(const QString& provider)
{
    m_process = new QProcess(this);
    m_activeProvider = provider;
    m_workerReady = false;
    m_stdoutBuffer.clear();

    connect(m_process, &QProcess::readyReadStandardOutput,
            this, &VoiceGenerationService::handleStdout);
    connect(m_process, &QProcess::readyReadStandardError, this, [this]() {
        if (!m_process) return;
        const QString detail = QString::fromUtf8(m_process->readAllStandardError()).trimmed();
        if (!detail.isEmpty()) emit statusChanged(detail.section('\n', -1).trimmed());
    });
    connect(m_process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
            this, [this](int code, QProcess::ExitStatus) { handleWorkerExit(code); });
    connect(m_process, &QProcess::errorOccurred, this,
            [this](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart || !m_hasCurrent) return;
        const QString detail = m_process ? m_process->errorString()
                                         : QStringLiteral("unknown process error");
        stopWorker();
        failCurrent(QStringLiteral("Could not start the voice worker: %1").arg(detail));
    });

    QStringList args{
        QString::fromUtf8(ROUNDTABLE_VOICE_WORKER_PATH),
        QStringLiteral("--provider"), provider,
        QStringLiteral("--runtime-root"), rootPathFor(provider),
        QStringLiteral("--model"), modelPathFor(provider)
    };
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("HF_HOME"),
               QDir(rootPathFor(provider)).absoluteFilePath(QStringLiteral("../huggingface")));
    env.insert(QStringLiteral("PYTHONUTF8"), QStringLiteral("1"));
    m_process->setProcessEnvironment(env);
    m_process->setWorkingDirectory(rootPathFor(provider));
    emit statusChanged(provider == QStringLiteral("fish-s2")
        ? QStringLiteral("Loading Fish S2 Pro (this can take a few minutes)...")
        : QStringLiteral("Loading OmniVoice..."));
    m_process->start(pythonPathFor(provider), args, QIODevice::ReadWrite);
    emit modelResidentChanged(true);

    QTimer::singleShot(10 * 60 * 1000, this, [this, provider]() {
        if (m_hasCurrent && m_activeProvider == provider && !m_workerReady) {
            stopWorker();
            failCurrent(QStringLiteral("The voice model did not finish loading within 10 minutes."));
        }
    });
}

void VoiceGenerationService::stopWorker()
{
    if (!m_process) return;
    disconnect(m_process, nullptr, this, nullptr);
    if (m_process->state() != QProcess::NotRunning) {
        m_process->write("{\"op\":\"shutdown\"}\n");
        m_process->waitForBytesWritten(500);
        m_process->terminate();
        if (!m_process->waitForFinished(2500)) {
            m_process->kill();
            m_process->waitForFinished(1000);
        }
    }
    m_process->deleteLater();
    m_process = nullptr;
    m_workerReady = false;
    m_activeProvider.clear();
    emit modelResidentChanged(false);
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

    QJsonObject obj{
        {QStringLiteral("op"), QStringLiteral("generate")},
        {QStringLiteral("text"), m_current.text},
        {QStringLiteral("reference_audio"), m_current.referenceAudio},
        {QStringLiteral("reference_text"), m_current.referenceText},
        {QStringLiteral("reference_start"), m_current.referenceStart},
        {QStringLiteral("reference_end"), m_current.referenceEnd},
        {QStringLiteral("reference_segments"), references},
        {QStringLiteral("speed"), m_current.speed},
        {QStringLiteral("duration"), m_current.targetDuration},
        {QStringLiteral("seed"), m_current.seed},
        {QStringLiteral("output"), m_currentOutput}
    };
    m_process->write(QJsonDocument(obj).toJson(QJsonDocument::Compact) + '\n');
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
            emit statusChanged(QStringLiteral("%1 ready").arg(
                m_activeProvider == QStringLiteral("fish-s2")
                    ? QStringLiteral("Fish S2 Pro") : QStringLiteral("OmniVoice")));
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

void VoiceGenerationService::handleWorkerExit(int exitCode)
{
    if (m_process) m_process->deleteLater();
    m_process = nullptr;
    m_workerReady = false;
    m_activeProvider.clear();
    emit modelResidentChanged(false);
    if (m_hasCurrent && !m_cancelled)
        failCurrent(QStringLiteral("Voice worker exited unexpectedly (code %1).").arg(exitCode));
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
    QDir dir(QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation))
                 .filePath(QStringLiteral("Roundtable Voice Drafts")));
    if (!dir.mkpath(QStringLiteral("."))) return {};

    QString words = request.text.simplified().left(42);
    words.replace(QRegularExpression(QStringLiteral(R"([^\p{L}\p{N}]+)")), QStringLiteral("_"));
    words = words.trimmed();
    if (words.isEmpty()) words = QStringLiteral("line");
    const QString stamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss_zzz"));
    return dir.filePath(QStringLiteral("DRAFT_%1_%2_%3.wav")
        .arg(cleanPathPart(request.character), stamp, words));
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
    if (destinationDirectory.isEmpty() && !request.referenceAudio.isEmpty()) {
        const QFileInfo source(request.referenceAudio);
        if (source.exists() && source.isFile()) destinationDirectory = source.absolutePath();
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

    const QString character = cleanPathPart(request.character).toUpper();
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
