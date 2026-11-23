/*
 * ComfyRunner.cpp — see header.
 */
#include "panels/comfy/ComfyRunner.h"
#include "panels/comfy/ComfyEventStream.h"

#include "ai/ShotPipeline.h"

#include <QBuffer>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHttpMultiPart>
#include <QHostAddress>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QProcess>
#include <QRegularExpression>
#include <QSet>
#include <QSaveFile>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>
#include <QUuid>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <utility>

namespace rt {

namespace {

constexpr int kMaxPollFailures = 6;          // ~12 s of no answer → server is gone
constexpr int kMaxAttempts = 3;              // retries always preserve quality

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
    QString status = m_stage == 1 ? QStringLiteral("Preparing") : m_stage == 3 ? QStringLiteral("Cleaning background") : QStringLiteral("Rendering");
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

void ComfyRunner::upload(const QString& localPath, const QString& remoteName, Done then,
                         bool referenceImage)
{
    QIODevice* data = nullptr;
    if (referenceImage) {
        // Bound reference segmentation as well as encoding: the workflow may
        // not resize until after SAM. Keep framing/alpha; never upscale.
        QImageReader reader(localPath);
        reader.setAutoTransform(true);
        QImage image = reader.read();
        if (image.isNull()) {
            then(false, QStringLiteral("cannot read reference %1: %2")
                            .arg(localPath, reader.errorString()));
            return;
        }
        const QSize original = image.size();
        if (image.width() > 1920 || image.height() > 1080)
            image = image.scaled(1920, 1080, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        auto* buffer = new QBuffer;
        buffer->open(QIODevice::ReadWrite);
        if (!image.save(buffer, "PNG")) {
            delete buffer;
            then(false, QStringLiteral("cannot encode reference %1").arg(localPath));
            return;
        }
        buffer->seek(0);
        data = buffer;
        emit log(QStringLiteral("Reference %1: %2×%3 → %4×%5")
                     .arg(QFileInfo(localPath).fileName())
                     .arg(original.width()).arg(original.height())
                     .arg(image.width()).arg(image.height()));
    } else {
        auto* file = new QFile(localPath);
        if (!file->open(QIODevice::ReadOnly)) {
            delete file;
            then(false, QStringLiteral("cannot read %1").arg(localPath));
            return;
        }
        data = file;
    }
    auto* multi = new QHttpMultiPart(QHttpMultiPart::FormDataType);
    QHttpPart filePart;
    filePart.setHeader(QNetworkRequest::ContentDispositionHeader,
                       QStringLiteral("form-data; name=\"image\"; filename=\"%1\"").arg(remoteName));
    filePart.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/octet-stream"));
    filePart.setBodyDevice(data);
    data->setParent(multi);
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

static QString launcherEntryPoint(const QString& launcher)
{
    if (launcher.isEmpty()) return {};
    const auto args = QProcess::splitCommand(ComfyRunner::serverCommandFromLauncher(launcher));
    for (const QString& arg : args) {
        if (QFileInfo(arg).fileName().compare("main.py", Qt::CaseInsensitive) == 0)
            return QDir(ComfyRunner::serverWorkingDirFromLauncher(launcher)).absoluteFilePath(arg);
    }
    return {};
}

void ComfyRunner::checkAlive(std::function<void(bool, QString)> then, int timeoutMs)
{
    QNetworkRequest req{QUrl(m_config.url + QStringLiteral("/system_stats"))};
    req.setTransferTimeout(timeoutMs);
    QNetworkReply* reply = m_net->get(req);
    connect(reply, &QNetworkReply::finished, this, [reply, then]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) { then(false, {}); return; }
        const auto stats = QJsonDocument::fromJson(reply->readAll()).object().value("system").toObject();
        then(true, stats.value("argv").toArray().first().toString());
    });
}

void ComfyRunner::ensureServer(Done then, bool onlyIfRunning)
{
    const qint64 gen = m_generation;
    checkAlive([this, then, gen, onlyIfRunning](bool alive, const QString& entry) {
        if (gen != m_generation) return;
        const bool owned = m_server && m_server->state() != QProcess::NotRunning;
        const QString host = QUrl(m_config.url).host();
        const bool local = QHostAddress(host).isLoopback() || host.compare("localhost", Qt::CaseInsensitive) == 0;
        const bool knownEntry = alive && local && QDir::isAbsolutePath(entry);
        const QString wanted = launcherEntryPoint(m_config.launcher);
        const bool wrongEntry = knownEntry && !wanted.isEmpty() && QFileInfo(entry) != QFileInfo(wanted);
        const bool wrongOwner = owned && !m_config.launcher.isEmpty() && QFileInfo(m_serverLauncher) != QFileInfo(m_config.launcher);
        if (local && (wrongEntry || wrongOwner)) {
            // A generic retry restarts the listener. A profile conflict or
            // someone else's queued work must never go down that path.
            const auto reject = [this, then](const QString& error) {
                if (m_running) stopBatch(error); else then(false, error);
            };
            if (!QFileInfo(m_config.launcher).isFile()) {
                reject(QStringLiteral("Set an existing launcher for the selected engine in SETTINGS."));
                return;
            }
            if (wrongEntry && QFileInfo(entry) != QFileInfo(launcherEntryPoint(m_config.otherLauncher))
                && (!owned || QFileInfo(entry) != QFileInfo(launcherEntryPoint(m_serverLauncher)))) {
                reject(QStringLiteral("Another ComfyUI installation is listening at %1 (%2). Check the launcher or URL.")
                     .arg(m_config.url, entry));
                return;
            }
            auto replace = [this, then, gen]() {
                emit log(QStringLiteral("Switching ComfyUI installations to %1").arg(m_config.launcher));
                m_verifiedNodes.clear();
                killServer([this, then, gen](bool ok, const QString& error) {
                    if (gen != m_generation) return;
                    if (ok) startServer(then); else then(false, error);
                });
            };
            if (!alive) { replace(); return; }
            auto* queue = get(QStringLiteral("/queue"));
            connect(queue, &QNetworkReply::finished, this, [this, queue, gen, replace, reject]() {
                queue->deleteLater();
                if (gen != m_generation) return;
                const auto jobs = QJsonDocument::fromJson(queue->readAll()).object();
                for (const auto name : {"queue_running", "queue_pending"}) {
                    if (!jobs.value(name).isArray() || !jobs.value(name).toArray().isEmpty()) {
                        reject(QStringLiteral("The previous ComfyUI queue must be idle to switch installations."));
                        return;
                    }
                }
                replace();
            });
            return;
        }
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
        if (onlyIfRunning) then(true, {}); else startServer(then);
    });
}

void ComfyRunner::launchServer(const QString& url, const QString& launcher, const QString& otherLauncher, bool onlyIfRunning)
{
    if (m_running) return;
    if (m_warming) { emit log(QStringLiteral("ComfyUI is still starting…")); return; }
    m_warming = true;
    emit serverStartupChanged();
    m_config.url = url;
    m_config.launcher = launcher;
    m_config.otherLauncher = otherLauncher;
    ensureServer([this, onlyIfRunning](bool ok, const QString& err) {
        m_warming = false;
        if (!ok || !onlyIfRunning)
            emit log(ok ? QStringLiteral("ComfyUI is running at %1").arg(m_config.url)
                        : QStringLiteral("Could not start ComfyUI: %1").arg(err));
        emit serverStartupChanged();
    }, onlyIfRunning);
}

void ComfyRunner::startServer(Done then)
{
    if (m_config.launcher.isEmpty() || !QFileInfo::exists(m_config.launcher)) {
        then(false, QStringLiteral("ComfyUI is not running and no launcher (.bat) is set"));
        return;
    }
    if (m_server && m_server->state() != QProcess::NotRunning) {
        // Never just delete a running QProcess: that ends cmd.exe but orphans
        // the python server with a dead stdout. End the whole tree, without
        // blocking the UI thread: continue once it has exited, or after 5 s.
        QProcess::startDetached(QStringLiteral("taskkill"),
                                {QStringLiteral("/PID"), QString::number(m_server->processId()),
                                 QStringLiteral("/T"), QStringLiteral("/F")});
        QProcess* old = m_server;
        const qint64 gen = m_generation;
        auto resumed = std::make_shared<bool>(false);
        auto resume = [this, old, then, gen, resumed]() {
            if (*resumed) return;
            *resumed = true;
            if (gen != m_generation) return;
            if (m_server == old) {   // drop it so the retry does not kill again
                m_server->disconnect(this);
                m_server->deleteLater();
                m_server = nullptr;
            }
            startServer(then);
        };
        connect(old, &QProcess::finished, this, [resume](int, QProcess::ExitStatus) { resume(); });
        QTimer::singleShot(5000, this, resume);
        return;
    }
    if (m_server) {
        m_server->disconnect(this);
        m_server->deleteLater();
    }
    emit log(QStringLiteral("Starting ComfyUI: %1").arg(QDir::toNativeSeparators(m_config.launcher)));
    m_server = new QProcess(this);
    m_serverLauncher = m_config.launcher;
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
    // One file kept open for the server's lifetime; an old log over 20 MB
    // becomes comfyui.log.old so it cannot grow forever.
    m_serverLog.close();
    if (!m_serverLogPath.isEmpty()) {
        if (QFileInfo(m_serverLogPath).size() > 20LL * 1024 * 1024) {
            QFile::remove(m_serverLogPath + QStringLiteral(".old"));
            QFile::rename(m_serverLogPath, m_serverLogPath + QStringLiteral(".old"));
        }
        QDir().mkpath(QFileInfo(m_serverLogPath).absolutePath());
        m_serverLog.setFileName(m_serverLogPath);
        if (!m_serverLog.open(QIODevice::Append))
            spdlog::warn("ComfyRunner: cannot write {}", m_serverLogPath.toStdString());
    }
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
    if (m_serverLog.isOpen()) {
        m_serverLog.write(data);
        m_serverLog.flush();   // readable in an editor while ComfyUI runs
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
        checkAlive([this, wantAlive, timeoutSec, then, clock, tick, gen](bool alive, const QString&) {
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
            checkAlive([then](bool alive, const QString&) {
                then(!alive, alive ? QStringLiteral("Could not identify the process listening on the ComfyUI port.") : QString());
            });
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
    m_verifiedNodes.clear();   // a restarted server may load different node packs
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
    if (m_warming) {
        connect(this, &ComfyRunner::serverStartupChanged, this, [this, config, jobs = std::move(jobs)]() mutable {
            start(config, std::move(jobs));
        }, Qt::SingleShotConnection);
        return;
    }
    m_config = config;
    m_jobs = std::move(jobs);
    m_index = -1;
    m_orderPosition = -1;
    m_executionOrder.clear();
    const QString batchId = m_batchId = QUuid::createUuid().toString(QUuid::Id128);
    for (int i = 0; i < static_cast<int>(m_jobs.size()); ++i) {
        m_executionOrder.push_back(i);
        m_jobs[i].artifactKey = batchId + QStringLiteral("_%1").arg(i);
    }
    const bool h3 = config.targets.engine == comfy::Engine::H3;
    m_stageCount = h3 ? comfy::kH3StageCount : 3;
    // H3 passes need the whole shot in one clip; longer shots render whole
    // after the passes.
    auto stageable = [h3](const RenderJob& job) {
        return !h3 || (job.frames > 0 && job.frames <= comfy::kH3MaxSingleClipFrames);
    };
    const bool anyStageable = std::any_of(m_jobs.begin(), m_jobs.end(), stageable);
    const QString probeKey = batchId + QStringLiteral("_0");
    const bool supported = h3
        ? !comfy::h3StagePrompt(comfy::h3ShotPrompt(config.prompt, config.targets, true), config.targets, 1, probeKey).isEmpty()
        : !comfy::batchStagePrompt(config.prompt, config.targets, comfy::BatchStage::Prepare, probeKey).isEmpty();
    m_stage = config.stagedBatch && anyStageable && supported ? 1 : 0;
    if (m_stage) {
        for (auto& job : m_jobs) job.staged = stageable(job);
        std::stable_sort(m_executionOrder.begin(), m_executionOrder.end(), [this](int a, int b) {
            return m_jobs[a].closeUp < m_jobs[b].closeUp;
        });
    }
    m_running = true;
    m_stopRequested = false;
    m_done = m_failed = 0;
    m_uploadedWide.clear();
    m_verifiedNodes.clear();
    m_sweptLeftovers = false;
    m_uploadedCloseUp.clear();
    ++m_generation;
    m_pollTimer->setInterval(std::max(20, m_config.pollIntervalMs));
    QDir().mkpath(m_config.rendersDir);
    emit log(QStringLiteral("Batch started: %1 shot(s)").arg(m_jobs.size()));
    if (m_stage && h3) {
        emit log(QStringLiteral("Five passes, each model loaded once: describe (Qwen3-VL), track (SAM 3.1), "
                                "encode, generate (H3), then cut out with alpha. Timeline order is preserved."));
        const auto whole = std::count_if(m_jobs.begin(), m_jobs.end(), [](const RenderJob& j) { return !j.staged; });
        if (whole > 0)
            emit log(QStringLiteral("%1 shot(s) longer than one H3 clip (%2 s) render whole after the passes.")
                         .arg(whole).arg(comfy::kH3MaxSingleClipFrames / double(comfy::kH3Fps), 0, 'f', 1));
    } else if (m_stage) {
        emit log(QStringLiteral("Three passes: prepare references/audio, generate shots, then clean backgrounds. Wide shots first; timeline order is preserved."));
    }
    m_events->open(QUrl(m_config.url), m_clientId, h3);   // step progress + live preview
    nextJob();
}

bool ComfyRunner::append(std::vector<RenderJob> jobs)
{
    if (!m_running || m_stopRequested || jobs.empty()) return false;
    const bool h3 = m_config.targets.engine == comfy::Engine::H3;
    for (auto& job : jobs) {
        const int index = static_cast<int>(m_jobs.size());
        job.artifactKey = m_batchId + QStringLiteral("_%1").arg(index);
        job.staged = m_stage == 1 && (!h3 || (job.frames > 0 && job.frames <= comfy::kH3MaxSingleClipFrames));
        m_jobs.push_back(std::move(job));
        m_executionOrder.push_back(index);
        emit jobChanged(index);
    }
    emit log(QStringLiteral("Added %1 shot(s) to the running batch").arg(jobs.size()));
    return true;
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
    releaseAfterEarlyEnd();
    m_events->close();
    m_running = false;
    emit finished(m_done, m_failed, true);
}

int ComfyRunner::remove(const std::vector<int>& indices)
{
    int removed = 0;
    for (int i : indices) {
        if (i < 0 || i >= static_cast<int>(m_jobs.size())) continue;
        RenderJob& job = m_jobs[static_cast<size_t>(i)];
        if (job.state == RenderJob::State::Skipped) continue;
        if (job.state == RenderJob::State::Running || (m_running && i == m_index)) continue;
        job.state = RenderJob::State::Skipped;
        job.status = QStringLiteral("Removed");
        releaseArtifacts(job);
        ++removed;
        emit jobChanged(i);
    }
    if (removed > 0 && m_running)
        emit log(QStringLiteral("Removed %1 shot(s) from the render queue").arg(removed));
    return removed;
}

int ComfyRunner::runningIndex() const noexcept
{
    if (!m_running || m_stopRequested || m_index < 0 || m_index >= static_cast<int>(m_jobs.size()))
        return -1;
    return m_jobs[static_cast<size_t>(m_index)].state == RenderJob::State::Running ? m_index : -1;
}

bool ComfyRunner::cancelCurrent()
{
    const int index = runningIndex();
    if (index < 0) return false;
    m_pollTimer->stop();
    ++m_generation;   // drop the shot's pending callbacks (upload, queue, poll, download)
    const qint64 gen = m_generation;
    const QString promptId = m_promptId;
    m_promptId.clear();

    RenderJob& job = current();
    job.state = RenderJob::State::Skipped;
    job.status = QStringLiteral("Stopped");
    job.step = job.steps = 0;
    releaseArtifacts(job);
    emit jobChanged(index);
    emit log(QStringLiteral("%1: render stopped; going on with the queue").arg(job.label));

    if (promptId.isEmpty()) {   // not on the server yet
        nextJob();
        return true;
    }
    // Interrupt only this prompt (an older server ignores the id and
    // interrupts whatever runs, which is this prompt) and drop it in case it
    // is still waiting in ComfyUI's queue.
    fireAndForget(QStringLiteral("/interrupt"), QJsonObject{{QStringLiteral("prompt_id"), promptId}});
    fireAndForget(QStringLiteral("/queue"), QJsonObject{{QStringLiteral("delete"), QJsonArray{promptId}}});
    waitForPromptGone(promptId, 60, [this, gen]() {
        if (gen == m_generation) nextJob();
    });
    return true;
}

void ComfyRunner::waitForPromptGone(const QString& promptId, int triesLeft, std::function<void()> then)
{
    QNetworkReply* reply = get(QStringLiteral("/queue"));
    connect(reply, &QNetworkReply::finished, this, [this, reply, promptId, triesLeft, then]() {
        reply->deleteLater();
        bool present = false;
        if (reply->error() == QNetworkReply::NoError) {
            const QJsonObject queue = QJsonDocument::fromJson(reply->readAll()).object();
            for (const char* list : {"queue_running", "queue_pending"})
                for (const auto& item : queue.value(QLatin1String(list)).toArray())
                    if (item.toArray().at(1).toString() == promptId) present = true;
        }
        if (!present || triesLeft <= 1) { then(); return; }
        QTimer::singleShot(500, this, [this, promptId, triesLeft, then]() {
            waitForPromptGone(promptId, triesLeft - 1, then);
        });
    });
}

void ComfyRunner::releaseArtifacts(RenderJob& job)
{
    if (!job.hasArtifacts) return;
    job.hasArtifacts = false;
    fireAndForget(QStringLiteral("/roundtable_batch/release"),
                  QJsonObject{{QStringLiteral("artifact_key"), job.artifactKey}});
}

void ComfyRunner::releaseAfterEarlyEnd()
{
    for (auto& job : m_jobs)
        releaseArtifacts(job);
    fireAndForget(QStringLiteral("/free"), QJsonObject{{QStringLiteral("unload_models"), true},
                                                       {QStringLiteral("free_memory"), false}});
}

void ComfyRunner::nextJob()
{
    m_pollTimer->stop();
    m_promptId.clear();
    while (++m_orderPosition < static_cast<int>(m_executionOrder.size())) {
        m_index = m_executionOrder[m_orderPosition];
        // A pass takes its staged shots; the whole-shot pass takes the rest.
        if (current().state == RenderJob::State::Pending && current().staged == (m_stage > 0)) {
            runCurrentJob();
            return;
        }
    }
    if (m_stage > 0 && m_stage < m_stageCount) {
        ++m_stage;
        m_orderPosition = -1;
        for (auto& job : m_jobs) {
            if (job.state != RenderJob::State::WaitingForNextPass) continue;
            job.state = RenderJob::State::Pending;
            job.attempt = 0;
        }
        if (m_config.targets.engine == comfy::Engine::H3) {
            static const char* next[] = {"", "", "Descriptions done. Tracking people (SAM 3.1).",
                                         "Tracking done. Encoding prompts and references.",
                                         "Encoding done. Generating all shots (H3).",
                                         "Generation complete. Cutting out each shot with alpha."};
            emit log(QString::fromLatin1(next[m_stage]));
        } else {
            emit log(m_stage == 2 ? QStringLiteral("Preparation complete. Generating all shots.")
                                  : QStringLiteral("Generation complete. Cleaning backgrounds and saving each shot with its audio."));
        }
        nextJob();
        return;
    }
    if (m_stage == m_stageCount && std::any_of(m_jobs.begin(), m_jobs.end(), [](const RenderJob& j) {
            return !j.staged && j.state == RenderJob::State::Pending; })) {
        m_stage = 0;
        m_orderPosition = -1;
        emit log(QStringLiteral("Passes complete. Rendering the long shots whole."));
        nextJob();
        return;
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
    job.fpsUsed = job.fps;
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

        if (!m_sweptLeftovers) {
            // Files a crashed or killed batch left on the server are never
            // reused (every batch has its own id): clear them before this one
            // adds its own. Shots marked "keep" stay.
            m_sweptLeftovers = true;
            fireAndForget(QStringLiteral("/roundtable_batch/sweep"),
                          QJsonObject{{QStringLiteral("batch_id"), m_batchId}});
        }

        if (m_stage >= 2) {
            submitCurrent();
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
                if (!QFileInfo(current().drivingVideo).isReadable()) {
                    failCurrent(QStringLiteral("cannot read the shot's driving clip %1").arg(current().drivingVideo),
                                /*retry=*/false);
                    return;
                }
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
                                 + QFileInfo(local).completeBaseName() + QStringLiteral(".png");
            upload(local, remote, [this, gen, uploadRefs, closeUp](bool ok, const QString& nameOrErr) {
                if (gen != m_generation) return;
                if (!ok) { stopBatch(QStringLiteral("Reference image upload failed: ") + nameOrErr); return; }
                (closeUp ? m_uploadedCloseUp : m_uploadedWide) = nameOrErr;
                (*uploadRefs)();
            }, true);
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
    // H3 follows the actor's face and has no expression node, so a shot's expression must not be applied.
    if (m_config.targets.engine != comfy::Engine::H3)
        params.expression = job.expression;
    params.seed = job.seed;
    params.filenamePrefix = m_config.filenamePrefix + QLatin1Char('/') + QFileInfo(job.drivingVideo).completeBaseName();
    QStringList missing;
    QJsonObject prompt = comfy::applyShotParams(m_config.prompt, m_config.targets, params, &missing);
    const QJsonObject sourcePrompt = prompt;
    m_sourcePrompt = sourcePrompt;
    if (!missing.isEmpty()) {
        stopBatch(QStringLiteral("The workflow has no node for: %1").arg(missing.join(QStringLiteral(", "))));
        return;
    }

    if (m_config.targets.engine == comfy::Engine::H3) {
        prompt = comfy::h3ShotPrompt(prompt, m_config.targets, job.staged);
        m_sourcePrompt = prompt;
        if (m_stage) {
            prompt = comfy::h3StagePrompt(prompt, m_config.targets, m_stage, job.artifactKey);
            if (prompt.isEmpty()) { stopBatch(QStringLiteral("The H3 workflow no longer supports staged rendering.")); return; }
        }
    } else if (m_stage) {
        prompt = comfy::batchStagePrompt(prompt, m_config.targets,
            m_stage == 1 ? comfy::BatchStage::Prepare : m_stage == 2 ? comfy::BatchStage::Generate : comfy::BatchStage::Finish,
            job.artifactKey);
        if (prompt.isEmpty()) { stopBatch(QStringLiteral("The workflow no longer supports staged rendering.")); return; }
    } else {
        prompt = comfy::alphaOutputPrompt(prompt, m_config.targets);
    }

    QStringList ours;
    for (const auto& node : std::as_const(prompt)) {
        const QString type = node.toObject().value(QLatin1String("class_type")).toString();
        if (type.startsWith(QLatin1String("Roundtable")) && !m_verifiedNodes.contains(type) && !ours.contains(type))
            ours << type;
    }
    const qint64 checkGen = m_generation;
    verifyH3Cutout([this, ours, prompt, sourcePrompt, checkGen]() {
        verifyBatchNodes(ours, [this, prompt, sourcePrompt, checkGen]() {
            if (checkGen == m_generation) queuePrompt(prompt, sourcePrompt);
        });
    });
}

void ComfyRunner::verifyH3Cutout(std::function<void()> then)
{
    // In a staged batch ComfyUI cannot validate the final cutout when it
    // accepts pass one. Check the actual finishing graph before generation.
    const QString type = QStringLiteral("ToonOut Selected BG JNK");
    if (m_config.targets.engine != comfy::Engine::H3 || m_stage != 1 || m_verifiedNodes.contains(type)) {
        then();
        return;
    }
    const auto finish = comfy::h3StagePrompt(m_sourcePrompt, m_config.targets, comfy::kH3StageCount, current().artifactKey);
    QStringList models;
    for (const auto& value : finish) {
        const auto node = value.toObject();
        const auto model = node.value("inputs").toObject().value("model_name");
        if (node.value("class_type") == type && model.isString()) models << model.toString();
    }
    if (models.isEmpty()) { then(); return; }
    setStatus(QStringLiteral("Checking H3 cutout model…"));
    auto* reply = get(QStringLiteral("/object_info/") + type);
    const qint64 gen = m_generation;
    connect(reply, &QNetworkReply::finished, this, [this, reply, gen, type, models, then]() {
        reply->deleteLater();
        if (gen != m_generation) return;
        if (reply->error() != QNetworkReply::NoError) {
            emit log(QStringLiteral("Could not check the H3 cutout model: %1").arg(reply->errorString()));
            then();   // Let ComfyUI validate the prompt if metadata is unavailable.
            return;
        }
        const auto schema = QJsonDocument::fromJson(reply->readAll());
        if (!schema.isObject()) { then(); return; }
        if (!schema.object().contains(type)) {
            stopBatch(QStringLiteral("H3 cutout requires the ComfyUI node '%1'. Check the JNK Tiny Nodes installation.").arg(type));
            return;
        }
        const auto info = schema.object().value(type).toObject();
        const auto spec = info.value("input").toObject().value("required").toObject().value("model_name").toArray();
        // Legacy combo widgets embed their choices directly. Newer schemas
        // expose them in the options object after the COMBO type.
        const auto choices = spec.first().isArray() ? spec.first()
            : spec.last().toObject().value("options");
        if (choices.isArray()) {
            for (const auto& model : models) {
                if (!choices.toArray().contains(model)) {
                    stopBatch(QStringLiteral("H3 cutout model '%1' is unavailable. Check models/RMBG on the H3 server, "
                                             "including its folder link, or select an installed model in the workflow.").arg(model));
                    return;
                }
            }
        }
        m_verifiedNodes.insert(type);
        then();
    });
}

void ComfyRunner::verifyBatchNodes(QStringList nodeTypes, std::function<void()> then)
{
    if (nodeTypes.isEmpty()) { then(); return; }
    const QString type = nodeTypes.takeFirst();
    setStatus(QStringLiteral("Checking ComfyUI…"));
    QNetworkReply* reply = get(QStringLiteral("/object_info/") + type);
    const qint64 gen = m_generation;
    connect(reply, &QNetworkReply::finished, this, [this, reply, gen, type, nodeTypes, then]() {
        reply->deleteLater();
        if (gen != m_generation) return;
        // Only a clear "no such node" answer stops the batch; if the question
        // itself fails, queue as before and let ComfyUI answer.
        if (reply->error() == QNetworkReply::NoError &&
            !QJsonDocument::fromJson(reply->readAll()).object().contains(type)) {
            stopBatch(QStringLiteral(
                "ComfyUI is missing the Roundtable node \"%1\". Install: "
                "python tools\\comfy\\install_batch.py \"<ComfyUI folder>\" --install, then restart ComfyUI. "
                "If it is installed, it is out of date or failed to load (see the ComfyUI log on QUEUE).")
                          .arg(type));
            return;
        }
        m_verifiedNodes.insert(type);
        verifyBatchNodes(nodeTypes, then);
    });
}

void ComfyRunner::queuePrompt(const QJsonObject& prompt, const QJsonObject& sourcePrompt)
{
    RenderJob& job = current();
    if (m_stage) job.hasArtifacts = true;
    setStatus(QStringLiteral("Queueing…"));
    job.step = job.steps = 0;
    job.stepNode.clear();
    m_consoleDrivesStep = false;
    QJsonObject body{{QStringLiteral("prompt"), prompt}, {QStringLiteral("client_id"), m_clientId}};
    m_events->resetLatentPreview();
    QJsonObject workflow = m_config.uiWorkflow;
    if (m_config.targets.engine == comfy::Engine::H3) {
        // VideoHelperSuite reads its animation switch here even for API-only workflows.
        QJsonObject extra = workflow.value("extra").toObject();
        extra.insert("VHS_latentpreview", true);
        workflow.insert("extra", extra);
    }
    if (!workflow.isEmpty()) {
        QJsonObject metadata{{QStringLiteral("workflow"), workflow}};
        if (m_stage) metadata.insert(QStringLiteral("roundtable_source_prompt"), sourcePrompt);
        body.insert(QStringLiteral("extra_data"),
                    QJsonObject{{QStringLiteral("extra_pnginfo"),
                                 metadata}});
    }
    QNetworkReply* reply = postJson(QStringLiteral("/prompt"), body);
    const qint64 gen = m_generation;
    connect(reply, &QNetworkReply::finished, this, [this, reply, gen]() {
        reply->deleteLater();
        const int http = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QJsonObject body = QJsonDocument::fromJson(reply->readAll()).object();
        if (gen != m_generation) {
            // Stopped while the prompt was being sent: take it back off the server.
            const QString stale = body.value(QLatin1String("prompt_id")).toString();
            if (!stale.isEmpty()) {
                fireAndForget(QStringLiteral("/queue"), QJsonObject{{QStringLiteral("delete"), QJsonArray{stale}}});
                fireAndForget(QStringLiteral("/interrupt"), QJsonObject{{QStringLiteral("prompt_id"), stale}});
            }
            return;
        }
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
        if (m_config.targets.engine == comfy::Engine::H3 && m_stage) {
            static const char* doing[] = {"", "Describing…", "Tracking…", "Encoding…", "Rendering…", "Cutting out…"};
            setStatus(QString::fromUtf8(doing[m_stage]));
        } else {
            setStatus(m_stage == 1 ? QStringLiteral("Preparing…") : m_stage == 3 ? QStringLiteral("Cleaning background…") : QStringLiteral("Rendering…"));
        }
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
            if (m_stage > 0 && m_stage < m_stageCount) {
                const bool h3 = m_config.targets.engine == comfy::Engine::H3;
                const QString done = h3 ? comfy::h3StageName(m_stage)
                                        : m_stage == 1 ? QStringLiteral("prepared") : QStringLiteral("generated");
                const QJsonObject outputs = entry.value(QLatin1String("outputs")).toObject();
                bool verified = false;
                for (const auto& value : outputs) {
                    const auto out = value.toObject();
                    const QJsonArray keys = out.value("artifact_key").toArray();
                    const QJsonArray stages = out.value("roundtable_stage").toArray();
                    if (keys.contains(current().artifactKey) && stages.contains(done)) verified = true;
                }
                if (!verified) { failCurrent(QStringLiteral("Batch phase completed without its shot artifact.")); return; }
                RenderJob& j = current();
                j.completedPhaseMs += m_jobClock.elapsed();
                emit log(QStringLiteral("%1: %2 in %3").arg(j.label, done, fmtDuration(m_jobClock.elapsed())));
                if (m_stage == 1) j.prepared = true;
                if (m_stage == m_stageCount - 1) j.generated = true;
                j.status = h3 ? done.left(1).toUpper() + done.mid(1) + QStringLiteral("; waiting for the next pass")
                         : m_stage == 1 ? QStringLiteral("Prepared; waiting to generate") : QStringLiteral("Generated; waiting for cleanup");
                j.state = RenderJob::State::WaitingForNextPass;
                emit jobChanged(m_index);
                nextJob();
                return;
            }
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
        failCurrent(QStringLiteral("the render finished but produced no video"), /*retry=*/false);
        return;
    }

    RenderJob& job = current();
    QString base = QStringLiteral("%1/%2_s%3_%4fps")
        .arg(m_config.rendersDir, QFileInfo(job.drivingVideo).completeBaseName(),
             job.seed >= 0 ? QString::number(job.seed) : QStringLiteral("wf"))
        .arg(job.fpsUsed);
    const QString extension = QLatin1Char('.') + QFileInfo(found.value("filename").toString()).suffix().toLower();
    QString dest = base + extension;
    for (int n = 2; QFileInfo::exists(dest); ++n) dest = base + QStringLiteral("_%1").arg(n) + extension;

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
        const QByteArray video = reply->readAll();
        QSaveFile file(dest);
        if (video.isEmpty() || !file.open(QIODevice::WriteOnly) ||
            file.write(video) != video.size() || !file.commit()) {
            failCurrent(QStringLiteral("cannot save %1").arg(dest), /*retry=*/false);
            return;
        }
        RenderJob& job = current();
        job.state = RenderJob::State::Done;
        job.outputFile = dest;
        job.elapsedMs = job.completedPhaseMs + m_jobClock.elapsed();
        job.status = QStringLiteral("Done in %1").arg(fmtDuration(job.elapsedMs));
        if (job.frames > 0) {
            m_renderSeconds += job.elapsedMs / 1000.0;
            m_renderFrames += static_cast<long long>(job.frames) * job.fpsUsed / std::max(1, job.fps);
        }
        ++m_done;
        emit log(QStringLiteral("%1: done → %2").arg(job.label, QFileInfo(dest).fileName()));
        emit jobChanged(m_index);
        emit jobRendered(m_index, dest);
        if (m_stage) {
            auto* cleanup = postJson(QStringLiteral("/roundtable_batch/release"), QJsonObject{{"artifact_key", job.artifactKey}});
            connect(cleanup, &QNetworkReply::finished, this, [this, cleanup, gen]() {
                cleanup->deleteLater();
                if (gen != m_generation) return;
                if (cleanup->error() == QNetworkReply::NoError)
                    current().hasArtifacts = false;
                else
                    emit log(QStringLiteral("Video saved; temporary batch files could not be released: ") + cleanup->errorString());
                nextJob();
            });
            return;
        }
        nextJob();
    });
}

void ComfyRunner::failCurrent(const QString& why, bool retry)
{
    if (!m_running || m_stopRequested) return;
    m_pollTimer->stop();
    RenderJob& job = current();
    if (!retry) {
        // Nothing a fresh server would change: skip the shot, keep the models loaded.
        emit log(QStringLiteral("%1: FAILED — %2").arg(job.label, why));
        job.state = RenderJob::State::Failed;
        job.status = QStringLiteral("Failed: ") + why.left(160);
        ++m_failed;
        releaseArtifacts(job);
        emit jobChanged(m_index);
        nextJob();
        return;
    }
    emit log(QStringLiteral("%1: FAILED (attempt %2/%3) — %4").arg(job.label).arg(job.attempt + 1)
                 .arg(kMaxAttempts).arg(why));
    const qint64 gen = m_generation;
    if (job.attempt + 1 < kMaxAttempts) {
        ++job.attempt;
        setStatus(QStringLiteral("Restarting ComfyUI, then retrying with unchanged settings…"));
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
    releaseArtifacts(job);
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
    releaseAfterEarlyEnd();
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
                                                       {QStringLiteral("free_memory"), false}});
    emit finished(m_done, m_failed, false);
}

} // namespace rt
