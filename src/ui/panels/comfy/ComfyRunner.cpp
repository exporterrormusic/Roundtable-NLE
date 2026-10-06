/*
 * ComfyRunner.cpp — see header.
 */
#include "panels/comfy/ComfyRunner.h"
#include "panels/comfy/ComfyEventStream.h"

#include "ai/ShotPipeline.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHttpMultiPart>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QProcess>
#include <QRegularExpression>
#include <QSet>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>
#include <QUuid>

#include <spdlog/spdlog.h>

#include <algorithm>

namespace rt {

namespace {

constexpr int kMaxPollFailures = 6;          // ~12 s of no answer → server is gone
constexpr int kMaxAttempts = 3;              // as-is, restart, restart @12 fps

QString fmtDuration(qint64 ms)
{
    const qint64 s = ms / 1000;
    return QStringLiteral("%1:%2").arg(s / 60).arg(s % 60, 2, 10, QLatin1Char('0'));
}

/// Human summary of a /prompt 400 body ({"error": {...}, "node_errors": {...}}).
QString describeValidationError(const QJsonObject& body)
{
    QStringList parts;
    const QJsonObject err = body.value(QLatin1String("error")).toObject();
    if (!err.isEmpty())
        parts << err.value(QLatin1String("message")).toString() + QStringLiteral(" ") +
                     err.value(QLatin1String("details")).toString();
    // ComfyUI names the node by its TITLE; the type is what's missing.
    if (err.value(QLatin1String("type")).toString() == QLatin1String("missing_node_type")) {
        const QString type = err.value(QLatin1String("extra_info")).toObject()
                                 .value(QLatin1String("class_type")).toString();
        parts << QStringLiteral("Node type %1 is not loaded in this ComfyUI: its custom node pack failed to "
                                "import (see the ComfyUI log on QUEUE) or isn't installed. Restart ComfyUI.")
                     .arg(type.isEmpty() ? QStringLiteral("(unknown)") : QStringLiteral("'%1'").arg(type));
    }
    const QJsonObject nodeErrors = body.value(QLatin1String("node_errors")).toObject();
    for (auto it = nodeErrors.begin(); it != nodeErrors.end(); ++it) {
        const QJsonObject ne = it.value().toObject();
        for (const auto& e : ne.value(QLatin1String("errors")).toArray()) {
            const QJsonObject eo = e.toObject();
            parts << QStringLiteral("node %1 (%2): %3 %4")
                         .arg(it.key(), ne.value(QLatin1String("class_type")).toString(),
                              eo.value(QLatin1String("message")).toString(),
                              eo.value(QLatin1String("details")).toString());
        }
    }
    return parts.join(QStringLiteral("; ")).trimmed();
}

} // namespace

QString ComfyRunner::serverCommandFromLauncher(const QString& launcherPath)
{
    QFile f(launcherPath);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return {};
    for (QString line : QString::fromLocal8Bit(f.readAll()).split(QLatin1Char('\n'))) {
        line = line.trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1String("REM"), Qt::CaseInsensitive) ||
            line.startsWith(QLatin1String("::")) || line.startsWith(QLatin1String("echo"), Qt::CaseInsensitive))
            continue;
        if (!line.contains(QLatin1String("main.py"), Qt::CaseInsensitive)) continue;
        line.remove(QRegularExpression(QStringLiteral("\\s--auto-launch\\b")));
        if (!line.contains(QLatin1String("--disable-auto-launch")))
            line += QStringLiteral(" --disable-auto-launch");
        return line;
    }
    return {};
}

QString ComfyRunner::serverWorkingDirFromLauncher(const QString& launcherPath)
{
    QString dir = QFileInfo(launcherPath).absolutePath();
    QFile f(launcherPath);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return dir;
    static const QRegularExpression cdLine(QStringLiteral("^cd\\s+(?:/d\\s+)?\"?([^\"]+?)\"?\\s*$"),
                                           QRegularExpression::CaseInsensitiveOption);
    for (QString line : QString::fromLocal8Bit(f.readAll()).split(QLatin1Char('\n'))) {
        line = line.trimmed();
        if (line.contains(QLatin1String("main.py"), Qt::CaseInsensitive)) break;
        const auto m = cdLine.match(line);
        if (!m.hasMatch()) continue;
        const QString target = m.captured(1).trimmed();
        const QString abs = QDir::isAbsolutePath(target) ? target : QDir(dir).absoluteFilePath(target);
        if (QFileInfo(abs).isDir()) dir = QDir::cleanPath(abs);
    }
    return dir;
}

ComfyRunner::ComfyRunner(QObject* parent)
    : QObject(parent)
    , m_net(new QNetworkAccessManager(this))
    , m_pollTimer(new QTimer(this))
    , m_clientId(QUuid::createUuid().toString(QUuid::WithoutBraces))
{
    connect(m_pollTimer, &QTimer::timeout, this, &ComfyRunner::pollCurrent);

    m_events = new ComfyEventStream(this);
    connect(m_events, &ComfyEventStream::progress, this,
            [this](const QString& promptId, const QString& node, int value, int max) {
        if (!m_running || m_index < 0 || promptId.isEmpty() || promptId != m_promptId) return;
        RenderJob& job = current();
        if (max != job.steps) m_consoleDrivesStep = false;   // a different bar/node
        if (!m_consoleDrivesStep) job.step = value;
        job.steps = max;
        const QJsonObject apiNode = m_config.prompt.value(node).toObject();
        const QString title = apiNode.value(QLatin1String("_meta")).toObject().value(QLatin1String("title")).toString();
        job.stepNode = title.isEmpty() ? apiNode.value(QLatin1String("class_type")).toString() : title;
        job.status = renderingStatus(job);
        emit jobChanged(m_index);
    });
    connect(m_events, &ComfyEventStream::previewImage, this, [this](const QImage& image) {
        if (m_running && !m_promptId.isEmpty()) emit preview(image);
    });
    connect(m_events, &ComfyEventStream::previewClip, this,
            [this](const QByteArray& data, const QString& mime, double fps) {
        if (m_running && !m_promptId.isEmpty()) emit previewClip(data, mime, fps);
    });
}

QString ComfyRunner::renderingStatus(const RenderJob& job) const
{
    QString status = QStringLiteral("Rendering");
    if (job.steps > 0)
        status += QStringLiteral(" · %1%2/%3")
                      .arg(job.stepNode.isEmpty() ? QString() : job.stepNode + QLatin1Char(' '))
                      .arg(job.step).arg(job.steps);
    status += QStringLiteral(" · %1").arg(fmtDuration(job.elapsedMs));
    if (const double spf = secondsPerFrame(); spf > 0.0 && job.frames > 0) {
        const qint64 etaMs = static_cast<qint64>(spf * job.frames * job.fpsUsed / std::max(1, job.fps) * 1000.0);
        status += QStringLiteral(" / ~%1").arg(fmtDuration(etaMs));
    }
    return status;
}

ComfyRunner::~ComfyRunner()
{
    ++m_generation;
    // A ComfyUI we launched holds ~20 GB of VRAM; don't leave it behind.
    if (m_server && m_server->state() != QProcess::NotRunning) {
        QProcess::execute(QStringLiteral("taskkill"),
                          {QStringLiteral("/PID"), QString::number(m_server->processId()),
                           QStringLiteral("/T"), QStringLiteral("/F")});
        m_server->waitForFinished(5000);
    }
}

double ComfyRunner::secondsPerFrame() const noexcept
{
    return m_renderFrames > 0 ? m_renderSeconds / static_cast<double>(m_renderFrames) : 0.0;
}

// ═════════════════════════════════════════════════════════════════════════
//  HTTP helpers
// ═════════════════════════════════════════════════════════════════════════

QNetworkReply* ComfyRunner::get(const QString& path)
{
    QNetworkRequest req{QUrl(m_config.url + path)};
    req.setTransferTimeout(30000);
    return m_net->get(req);
}

QNetworkReply* ComfyRunner::postJson(const QString& path, const QJsonObject& body)
{
    QNetworkRequest req{QUrl(m_config.url + path)};
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    req.setTransferTimeout(60000);
    return m_net->post(req, QJsonDocument(body).toJson(QJsonDocument::Compact));
}

void ComfyRunner::fireAndForget(const QString& path, const QJsonObject& body)
{
    QNetworkReply* reply = postJson(path, body);
    connect(reply, &QNetworkReply::finished, reply, &QObject::deleteLater);
}

void ComfyRunner::upload(const QString& localPath, const QString& remoteName, Done then)
{
    auto* file = new QFile(localPath);
    if (!file->open(QIODevice::ReadOnly)) {
        delete file;
        then(false, QStringLiteral("cannot read %1").arg(localPath));
        return;
    }
    auto* multi = new QHttpMultiPart(QHttpMultiPart::FormDataType);
    QHttpPart filePart;
    filePart.setHeader(QNetworkRequest::ContentDispositionHeader,
                       QStringLiteral("form-data; name=\"image\"; filename=\"%1\"").arg(remoteName));
    filePart.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/octet-stream"));
    filePart.setBodyDevice(file);
    file->setParent(multi);
    multi->append(filePart);
    for (const auto& [name, value] : {std::pair{"type", "input"}, std::pair{"overwrite", "true"}}) {
        QHttpPart part;
        part.setHeader(QNetworkRequest::ContentDispositionHeader,
                       QStringLiteral("form-data; name=\"%1\"").arg(QLatin1String(name)));
        part.setBody(QByteArray(value));
        multi->append(part);
    }
    QNetworkRequest req{QUrl(m_config.url + QStringLiteral("/upload/image"))};
    req.setTransferTimeout(10 * 60 * 1000);
    QNetworkReply* reply = m_net->post(req, multi);
    multi->setParent(reply);
    const qint64 gen = m_generation;
    connect(reply, &QNetworkReply::finished, this, [this, reply, then, gen]() {
        reply->deleteLater();
        if (gen != m_generation) return;
        if (reply->error() != QNetworkReply::NoError) {
            then(false, reply->errorString());
            return;
        }
        const QJsonObject o = QJsonDocument::fromJson(reply->readAll()).object();
        const QString sub = o.value(QLatin1String("subfolder")).toString();
        const QString name = o.value(QLatin1String("name")).toString();
        then(!name.isEmpty(), sub.isEmpty() ? name : sub + QLatin1Char('/') + name);
    });
}

// ═════════════════════════════════════════════════════════════════════════
//  Server lifecycle
// ═════════════════════════════════════════════════════════════════════════

void ComfyRunner::checkAlive(std::function<void(bool)> then, int timeoutMs)
{
    QNetworkRequest req{QUrl(m_config.url + QStringLiteral("/system_stats"))};
    req.setTransferTimeout(timeoutMs);
    QNetworkReply* reply = m_net->get(req);
    connect(reply, &QNetworkReply::finished, this, [reply, then]() {
        reply->deleteLater();
        then(reply->error() == QNetworkReply::NoError);
    });
}

void ComfyRunner::ensureServer(Done then)
{
    checkAlive([this, then](bool alive) {
        if (alive) { then(true, {}); return; }
        // A ComfyUI we launched is still loading (e.g. "Start ComfyUI now"
        // then RENDER): wait for it.  Launching a second one used to discard
        // the first QProcess, which kills only cmd.exe — the orphaned python
        // then crashes on its next print (broken stdout pipe, Errno 22) and
        // custom node packs fail to import ("Node '…' not found").
        if (m_server && m_server->state() != QProcess::NotRunning) {
            emit log(QStringLiteral("ComfyUI is still starting…"));
            waitForServer(true, m_config.serverStartTimeoutSec, then);
            return;
        }
        startServer(then);
    });
}

void ComfyRunner::launchServer(const QString& url, const QString& launcher)
{
    if (m_running) return;
    m_config.url = url;
    m_config.launcher = launcher;
    ensureServer([this](bool ok, const QString& err) {
        emit log(ok ? QStringLiteral("ComfyUI is running at %1").arg(m_config.url)
                    : QStringLiteral("Could not start ComfyUI: %1").arg(err));
    });
}

void ComfyRunner::startServer(Done then)
{
    if (m_config.launcher.isEmpty() || !QFileInfo::exists(m_config.launcher)) {
        then(false, QStringLiteral("ComfyUI is not running and no launcher (.bat) is set"));
        return;
    }
    if (m_server) {
        // Never just delete a running QProcess: that ends cmd.exe but orphans
        // the python server with a dead stdout. End the whole tree.
        if (m_server->state() != QProcess::NotRunning) {
            QProcess::execute(QStringLiteral("taskkill"),
                              {QStringLiteral("/PID"), QString::number(m_server->processId()),
                               QStringLiteral("/T"), QStringLiteral("/F")});
            m_server->waitForFinished(5000);
        }
        m_server->disconnect(this);
        m_server->deleteLater();
    }
    emit log(QStringLiteral("Starting ComfyUI: %1").arg(QDir::toNativeSeparators(m_config.launcher)));
    m_server = new QProcess(this);
    m_server->setProgram(QStringLiteral("cmd.exe"));
    // Run the launcher's own "python ... main.py ..." line with the browser
    // auto-open switched off (the portable build's --windows-standalone-build
    // opens a tab otherwise). The .bat itself is left untouched.
    const QString command = serverCommandFromLauncher(m_config.launcher);
    if (!command.isEmpty())
        m_server->setNativeArguments(QStringLiteral("/c ") + command);
    else
        m_server->setArguments({QStringLiteral("/c"), QDir::toNativeSeparators(m_config.launcher)});
    m_server->setWorkingDirectory(serverWorkingDirFromLauncher(m_config.launcher));
    m_server->setProcessChannelMode(QProcess::MergedChannels);
    m_serverLogPath = m_config.rendersDir.isEmpty() ? QString()
                    : m_config.rendersDir + QStringLiteral("/comfyui.log");
    m_serverDecoder.resetState();
    m_serverLineBuf.clear();
    connect(m_server, &QProcess::readyRead, this, &ComfyRunner::onServerOutput);
    m_server->start();
    if (!m_server->waitForStarted(10000)) {
        then(false, QStringLiteral("could not run %1").arg(m_config.launcher));
        return;
    }
    waitForServer(true, m_config.serverStartTimeoutSec, then);
}

void ComfyRunner::onServerOutput()
{
    if (!m_server) return;
    const QByteArray data = m_server->readAll();
    if (!m_serverLogPath.isEmpty()) {
        QFile f(m_serverLogPath);
        if (f.open(QIODevice::Append)) f.write(data);
    }
    // Stateful decode: a UTF-8 character may straddle two reads.
    m_serverLineBuf += m_serverDecoder.decode(data);
    // Lines end at \n, or at a lone \r (tqdm redrawing its bar in place).
    qsizetype start = 0;
    for (qsizetype i = 0; i < m_serverLineBuf.size(); ++i) {
        const QChar c = m_serverLineBuf.at(i);
        if (c != QLatin1Char('\n') && c != QLatin1Char('\r')) continue;
        const QString line = m_serverLineBuf.mid(start, i - start).trimmed();
        start = i + 1;
        if (line.isEmpty()) continue;
        const bool progressBar = line.contains(QLatin1String("%|"));
        if (progressBar && m_running && m_index >= 0 && !m_promptId.isEmpty()) {
            // tqdm: " 50%|█████     | 4/8 [01:23<01:23, 20.9s/it]"
            static const QRegularExpression bar(QStringLiteral("\\|\\s*(\\d+)/(\\d+)\\s*\\["));
            if (const auto m = bar.match(line); m.hasMatch()) {
                RenderJob& job = current();
                if (job.steps > 0 && m.captured(2).toInt() == job.steps) {
                    m_consoleDrivesStep = true;
                    job.step = m.captured(1).toInt();
                    job.status = renderingStatus(job);
                    emit jobChanged(m_index);
                }
            }
        }
        emit serverOutput(line.left(2000), progressBar);
    }
    m_serverLineBuf.remove(0, start);
}

void ComfyRunner::waitForServer(bool wantAlive, int timeoutSec, Done then)
{
    auto clock = std::make_shared<QElapsedTimer>();
    clock->start();
    auto tick = std::make_shared<std::function<void()>>();
    const qint64 gen = m_generation;
    *tick = [this, wantAlive, timeoutSec, then, clock, tick, gen]() {
        if (gen != m_generation) return;
        checkAlive([this, wantAlive, timeoutSec, then, clock, tick, gen](bool alive) {
            if (gen != m_generation) return;
            if (alive == wantAlive) { then(true, {}); return; }
            if (wantAlive && m_server && m_server->state() == QProcess::NotRunning) {
                then(false, QStringLiteral("ComfyUI exited while starting (see comfyui.log)"));
                return;
            }
            if (clock->elapsed() > timeoutSec * 1000LL) {
                then(false, wantAlive ? QStringLiteral("ComfyUI did not come up in time")
                                      : QStringLiteral("ComfyUI did not shut down"));
                return;
            }
            QTimer::singleShot(2000, this, [tick]() { (*tick)(); });
        }, 2000);
    };
    (*tick)();
}

void ComfyRunner::killServer(Done then)
{
    // Every process listening on the port, plus the tree we launched.
    // Asynchronous: PowerShell takes seconds to start, and this used to wait
    // for it (up to 20 s) and then for each taskkill on the UI thread.
    const int port = QUrl(m_config.url).port(8188);
    auto* ps = new QProcess(this);
    auto finished = std::make_shared<bool>(false);
    auto proceed = [this, ps, then, finished]() {
        if (*finished) return;
        *finished = true;
        QSet<QString> pids;
        for (const QString& tok : QString::fromLocal8Bit(ps->readAllStandardOutput())
                                      .split(QRegularExpression(QStringLiteral("\\s+")),
                                             Qt::SkipEmptyParts))
            if (tok.toLongLong() > 0) pids.insert(tok);
        ps->deleteLater();
        if (m_server && m_server->state() != QProcess::NotRunning)
            pids.insert(QString::number(m_server->processId()));
        for (const QString& pid : pids) {
            emit log(QStringLiteral("Stopping ComfyUI (PID %1)").arg(pid));
            QProcess::startDetached(QStringLiteral("taskkill"),
                                    {QStringLiteral("/PID"), pid, QStringLiteral("/T"),
                                     QStringLiteral("/F")});
        }
        if (pids.isEmpty()) {
            then(true, {});
            return;
        }
        waitForServer(false, 60, then);   // polls until the port is free
    };
    connect(ps, &QProcess::finished, this, [proceed](int, QProcess::ExitStatus) { proceed(); });
    connect(ps, &QProcess::errorOccurred, this, [proceed](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) proceed();   // still end our own tree
    });
    QTimer::singleShot(20000, ps, [ps]() { ps->kill(); });   // -> finished -> proceed
    ps->start(QStringLiteral("powershell"),
              {QStringLiteral("-NoProfile"), QStringLiteral("-Command"),
               QStringLiteral("(Get-NetTCPConnection -LocalPort %1 -State Listen -ErrorAction SilentlyContinue).OwningProcess")
                   .arg(port)});
}

void ComfyRunner::restartServer(Done then)
{
    if (m_config.launcher.isEmpty() || !QFileInfo::exists(m_config.launcher)) {
        emit log(QStringLiteral("Cannot restart ComfyUI: no launcher (.bat) set — retrying on the running server"));
        then(true, {});
        return;
    }
    const qint64 gen = m_generation;
    killServer([this, then, gen](bool, const QString&) {
        if (gen != m_generation) return;
        // Let the driver actually release the VRAM before relaunching.
        QTimer::singleShot(3000, this, [this, then, gen]() {
            if (gen != m_generation) return;
            startServer(then);
        });
    });
}

// ═════════════════════════════════════════════════════════════════════════
//  Batch
// ═════════════════════════════════════════════════════════════════════════

void ComfyRunner::start(const Config& config, std::vector<RenderJob> jobs)
{
    if (m_running) return;
    m_config = config;
    m_jobs = std::move(jobs);
    m_index = -1;
    m_running = true;
    m_stopRequested = false;
    m_done = m_failed = 0;
    m_uploadedWide.clear();
    m_uploadedCloseUp.clear();
    ++m_generation;
    m_pollTimer->setInterval(std::max(20, m_config.pollIntervalMs));
    QDir().mkpath(m_config.rendersDir);
    emit log(QStringLiteral("Batch started: %1 shot(s)").arg(m_jobs.size()));
    m_events->open(QUrl(m_config.url), m_clientId);   // step progress + live preview
    nextJob();
}

void ComfyRunner::stop()
{
    if (!m_running) return;
    m_stopRequested = true;
    m_pollTimer->stop();
    fireAndForget(QStringLiteral("/interrupt"), {});
    ++m_generation;
    if (m_index >= 0 && m_index < static_cast<int>(m_jobs.size()) &&
        current().state == RenderJob::State::Running) {
        current().state = RenderJob::State::Pending;
        current().status = QStringLiteral("Stopped");
        emit jobChanged(m_index);
    }
    emit log(QStringLiteral("Batch stopped"));
    m_events->close();
    m_running = false;
    emit finished(m_done, m_failed, true);
}

void ComfyRunner::nextJob()
{
    m_pollTimer->stop();
    while (++m_index < static_cast<int>(m_jobs.size())) {
        if (current().state == RenderJob::State::Pending) {
            runCurrentJob();
            return;
        }
    }
    finishBatch();
}

void ComfyRunner::setStatus(const QString& status)
{
    current().status = status;
    emit jobChanged(m_index);
}

void ComfyRunner::runCurrentJob()
{
    RenderJob& job = current();
    job.state = RenderJob::State::Running;
    job.fpsUsed = job.attempt >= 2 ? kScailReducedFps : job.fps;
    setStatus(QStringLiteral("Connecting to ComfyUI…"));
    const qint64 gen = m_generation;

    ensureServer([this, gen](bool ok, const QString& err) {
        if (gen != m_generation) return;
        if (!ok) {
            // Nothing to restart with → every shot would fail the same way.
            if (m_config.launcher.isEmpty()) stopBatch(err);
            else failCurrent(err);
            return;
        }

        // Reference images once per batch (they stay in ComfyUI's input folder).
        auto uploadRefs = std::make_shared<std::function<void()>>();
        *uploadRefs = [this, gen, uploadRefs]() {
            if (gen != m_generation) return;
            const bool closeUp = current().closeUp;
            QString& uploaded = closeUp ? m_uploadedCloseUp : m_uploadedWide;
            const QString& local = closeUp ? m_config.closeUpReference : m_config.wideReference;
            if (!uploaded.isEmpty() || local.isEmpty()) {
                // Driving clip next.
                setStatus(QStringLiteral("Uploading driving clip…"));
                const QString remote = QFileInfo(current().drivingVideo).fileName();
                upload(current().drivingVideo, remote, [this, gen](bool ok, const QString& nameOrErr) {
                    if (gen != m_generation) return;
                    if (!ok) { failCurrent(QStringLiteral("upload failed: ") + nameOrErr); return; }
                    current().remoteVideo = nameOrErr;
                    submitCurrent();
                });
                return;
            }
            setStatus(QStringLiteral("Uploading reference image…"));
            const QString remote = (closeUp ? QStringLiteral("rt_ref_closeup_") : QStringLiteral("rt_ref_wide_"))
                                 + QFileInfo(local).fileName();
            upload(local, remote, [this, gen, uploadRefs, closeUp](bool ok, const QString& nameOrErr) {
                if (gen != m_generation) return;
                if (!ok) { stopBatch(QStringLiteral("Reference image upload failed: ") + nameOrErr); return; }
                (closeUp ? m_uploadedCloseUp : m_uploadedWide) = nameOrErr;
                (*uploadRefs)();
            });
        };
        (*uploadRefs)();
    });
}

void ComfyRunner::submitCurrent()
{
    RenderJob& job = current();
    comfy::ShotParams params;
    params.videoName = job.remoteVideo;
    params.fps = job.fpsUsed;
    params.referenceImageName = job.closeUp ? m_uploadedCloseUp : m_uploadedWide;
    params.expression = job.expression;
    params.seed = job.seed;
    params.filenamePrefix = m_config.filenamePrefix + QLatin1Char('/') + QFileInfo(job.drivingVideo).completeBaseName();
    QStringList missing;
    const QJsonObject prompt = comfy::applyShotParams(m_config.prompt, m_config.targets, params, &missing);
    if (!missing.isEmpty()) {
        stopBatch(QStringLiteral("The workflow has no node for: %1").arg(missing.join(QStringLiteral(", "))));
        return;
    }

    setStatus(QStringLiteral("Queueing…"));
    job.step = job.steps = 0;
    job.stepNode.clear();
    m_consoleDrivesStep = false;
    QJsonObject body{{QStringLiteral("prompt"), prompt}, {QStringLiteral("client_id"), m_clientId}};
    if (!m_config.uiWorkflow.isEmpty())
        body.insert(QStringLiteral("extra_data"),
                    QJsonObject{{QStringLiteral("extra_pnginfo"),
                                 QJsonObject{{QStringLiteral("workflow"), m_config.uiWorkflow}}}});
    QNetworkReply* reply = postJson(QStringLiteral("/prompt"), body);
    const qint64 gen = m_generation;
    connect(reply, &QNetworkReply::finished, this, [this, reply, gen]() {
        reply->deleteLater();
        if (gen != m_generation) return;
        const int http = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QJsonObject body = QJsonDocument::fromJson(reply->readAll()).object();
        if (http == 400) {
            stopBatch(QStringLiteral("ComfyUI rejected the workflow: ") + describeValidationError(body));
            return;
        }
        if (reply->error() != QNetworkReply::NoError) {
            failCurrent(QStringLiteral("could not queue: ") + reply->errorString());
            return;
        }
        m_promptId = body.value(QLatin1String("prompt_id")).toString();
        if (m_promptId.isEmpty()) { failCurrent(QStringLiteral("ComfyUI returned no prompt id")); return; }
        m_pollFailures = 0;
        m_jobClock.start();
        const RenderJob& j = current();
        emit log(QStringLiteral("%1: rendering (%2 fps, seed %3%4)")
                     .arg(j.label).arg(j.fpsUsed)
                     .arg(j.seed >= 0 ? QString::number(j.seed) : QStringLiteral("workflow"))
                     .arg(j.attempt ? QStringLiteral(", attempt %1").arg(j.attempt + 1) : QString()));
        setStatus(QStringLiteral("Rendering…"));
        m_pollTimer->start();
    });
}

void ComfyRunner::pollCurrent()
{
    if (!m_running || m_index < 0 || m_promptId.isEmpty()) return;
    RenderJob& job = current();
    job.elapsedMs = m_jobClock.elapsed();
    if (job.elapsedMs > m_config.jobTimeoutSec * 1000LL) {
        m_pollTimer->stop();
        fireAndForget(QStringLiteral("/interrupt"), {});
        failCurrent(QStringLiteral("timed out after %1").arg(fmtDuration(job.elapsedMs)));
        return;
    }
    job.status = renderingStatus(job);
    emit jobChanged(m_index);

    QNetworkReply* reply = get(QStringLiteral("/history/") + m_promptId);
    const qint64 gen = m_generation;
    connect(reply, &QNetworkReply::finished, this, [this, reply, gen]() {
        reply->deleteLater();
        if (gen != m_generation || !m_pollTimer->isActive()) return;
        if (reply->error() != QNetworkReply::NoError) {
            if (++m_pollFailures >= kMaxPollFailures) {
                m_pollTimer->stop();
                failCurrent(QStringLiteral("ComfyUI stopped responding (crashed?)"));
            }
            return;
        }
        m_pollFailures = 0;
        const QJsonObject entry = QJsonDocument::fromJson(reply->readAll()).object().value(m_promptId).toObject();
        if (entry.isEmpty()) return;   // still queued / running
        const QJsonObject status = entry.value(QLatin1String("status")).toObject();
        const QString state = status.value(QLatin1String("status_str")).toString();
        if (state == QLatin1String("error")) {
            m_pollTimer->stop();
            QString why = QStringLiteral("render failed");
            for (const auto& m : status.value(QLatin1String("messages")).toArray()) {
                const QJsonArray pair = m.toArray();
                if (pair.size() == 2 && pair.at(0).toString() == QLatin1String("execution_error")) {
                    const QJsonObject d = pair.at(1).toObject();
                    why = QStringLiteral("%1: %2").arg(d.value(QLatin1String("node_type")).toString(),
                                                      d.value(QLatin1String("exception_message")).toString().trimmed());
                }
            }
            failCurrent(why.left(400));
            return;
        }
        if (status.value(QLatin1String("completed")).toBool() || state == QLatin1String("success")) {
            m_pollTimer->stop();
            downloadOutput(entry.value(QLatin1String("outputs")).toObject());
        }
    });
}

void ComfyRunner::downloadOutput(const QJsonObject& outputs)
{
    // The save node's file, else the first video file any node produced.
    QJsonObject found;
    auto scan = [&found](const QJsonObject& nodeOut) {
        for (const char* key : {"images", "videos", "gifs"}) {
            for (const auto& v : nodeOut.value(QLatin1String(key)).toArray()) {
                const QJsonObject f = v.toObject();
                const QString name = f.value(QLatin1String("filename")).toString().toLower();
                if (name.endsWith(QLatin1String(".mp4")) || name.endsWith(QLatin1String(".mov")) ||
                    name.endsWith(QLatin1String(".webm")) || name.endsWith(QLatin1String(".mkv"))) {
                    found = f;
                    return true;
                }
            }
        }
        return false;
    };
    if (!scan(outputs.value(m_config.targets.save).toObject())) {
        for (auto it = outputs.begin(); it != outputs.end() && found.isEmpty(); ++it)
            scan(it.value().toObject());
    }
    if (found.isEmpty()) {
        failCurrent(QStringLiteral("the render finished but produced no video"));
        return;
    }

    RenderJob& job = current();
    QString base = QStringLiteral("%1/%2_s%3_%4fps")
        .arg(m_config.rendersDir, QFileInfo(job.drivingVideo).completeBaseName(),
             job.seed >= 0 ? QString::number(job.seed) : QStringLiteral("wf"))
        .arg(job.fpsUsed);
    QString dest = base + QStringLiteral(".mp4");
    for (int n = 2; QFileInfo::exists(dest); ++n) dest = base + QStringLiteral("_%1.mp4").arg(n);

    QUrlQuery q;
    q.addQueryItem(QStringLiteral("filename"), found.value(QLatin1String("filename")).toString());
    q.addQueryItem(QStringLiteral("subfolder"), found.value(QLatin1String("subfolder")).toString());
    q.addQueryItem(QStringLiteral("type"), found.value(QLatin1String("type")).toString(QStringLiteral("output")));
    QNetworkReply* reply = get(QStringLiteral("/view?") + q.toString(QUrl::FullyEncoded));
    setStatus(QStringLiteral("Downloading…"));
    const qint64 gen = m_generation;
    connect(reply, &QNetworkReply::finished, this, [this, reply, gen, dest]() {
        reply->deleteLater();
        if (gen != m_generation) return;
        if (reply->error() != QNetworkReply::NoError) {
            failCurrent(QStringLiteral("download failed: ") + reply->errorString());
            return;
        }
        QFile f(dest);
        if (!f.open(QIODevice::WriteOnly) || f.write(reply->readAll()) <= 0) {
            failCurrent(QStringLiteral("cannot write %1").arg(dest));
            return;
        }
        f.close();
        RenderJob& job = current();
        job.state = RenderJob::State::Done;
        job.outputFile = dest;
        job.elapsedMs = m_jobClock.elapsed();
        job.status = QStringLiteral("Done in %1").arg(fmtDuration(job.elapsedMs));
        if (job.frames > 0) {
            m_renderSeconds += job.elapsedMs / 1000.0;
            m_renderFrames += static_cast<long long>(job.frames) * job.fpsUsed / std::max(1, job.fps);
        }
        ++m_done;
        emit log(QStringLiteral("%1: done → %2").arg(job.label, QFileInfo(dest).fileName()));
        emit jobChanged(m_index);
        emit jobRendered(m_index, dest);
        nextJob();
    });
}

void ComfyRunner::failCurrent(const QString& why)
{
    if (!m_running || m_stopRequested) return;
    m_pollTimer->stop();
    RenderJob& job = current();
    emit log(QStringLiteral("%1: FAILED (attempt %2/%3) — %4").arg(job.label).arg(job.attempt + 1)
                 .arg(kMaxAttempts).arg(why));
    const qint64 gen = m_generation;
    if (job.attempt + 1 < kMaxAttempts) {
        ++job.attempt;
        setStatus(job.attempt >= 2 ? QStringLiteral("Restarting ComfyUI, then retrying at 12 fps…")
                                   : QStringLiteral("Restarting ComfyUI, then retrying…"));
        restartServer([this, gen](bool ok, const QString& err) {
            if (gen != m_generation) return;
            if (!ok) emit log(QStringLiteral("Restart failed: ") + err);
            runCurrentJob();
        });
        return;
    }
    job.state = RenderJob::State::Failed;
    job.status = QStringLiteral("Failed: ") + why.left(160);
    ++m_failed;
    emit jobChanged(m_index);
    // Fresh server for the next shot.
    restartServer([this, gen](bool ok, const QString& err) {
        if (gen != m_generation) return;
        if (!ok) emit log(QStringLiteral("Restart failed: ") + err);
        nextJob();
    });
}

void ComfyRunner::stopBatch(const QString& why)
{
    emit log(QStringLiteral("STOPPED: ") + why);
    if (m_index >= 0 && m_index < static_cast<int>(m_jobs.size())) {
        current().state = RenderJob::State::Failed;
        current().status = why.left(200);
        ++m_failed;
        emit jobChanged(m_index);
    }
    m_pollTimer->stop();
    ++m_generation;
    m_events->close();
    m_running = false;
    emit finished(m_done, m_failed, true);
}

void ComfyRunner::finishBatch()
{
    m_events->close();
    m_running = false;
    emit log(QStringLiteral("Batch finished: %1 rendered, %2 failed").arg(m_done).arg(m_failed));
    // Give the VRAM back to editing while nothing renders.
    fireAndForget(QStringLiteral("/free"), QJsonObject{{QStringLiteral("unload_models"), true},
                                                       {QStringLiteral("free_memory"), true}});
    emit finished(m_done, m_failed, false);
}

} // namespace rt
