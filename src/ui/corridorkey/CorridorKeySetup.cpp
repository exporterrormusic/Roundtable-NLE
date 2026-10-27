/*
 * CorridorKeySetup.cpp — see CorridorKeySetup.h.
 */

#include "corridorkey/CorridorKeySetup.h"

#include "PathUtils.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QStorageInfo>
#include <QTimer>
#include <QUrl>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <filesystem>
#include <fstream>

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

QString ckSetupPath(const fs::path& p)
{
    return QString::fromStdString(pathToUtf8(p));
}

uintmax_t ckSetupFolderBytes(const fs::path& dir)
{
    std::error_code error;
    uintmax_t total = 0;
    for (fs::recursive_directory_iterator it(dir, fs::directory_options::skip_permission_denied, error), end;
         !error && it != end; it.increment(error)) {
        std::error_code sizeError;
        if (it->is_regular_file(sizeError)) {
            const auto size = it->file_size(sizeError);
            if (!sizeError) total += size;
        }
    }
    return total;
}

QString ckSetupGB(double bytes)
{
    return QString::number(bytes / 1e9, 'f', 1);
}

void ckSetupLowPriority(QProcess& process)
{
#ifdef _WIN32
    process.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* args) {
        args->flags |= CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS;
    });
#else
    (void)process;
#endif
}

} // namespace

CorridorKeySetup::CorridorKeySetup(ck::Layout layout, QObject* parent)
    : QObject(parent)
    , m_layout(std::move(layout))
{
}

CorridorKeySetup::~CorridorKeySetup()
{
    if (m_reply) m_reply->abort();
    killProcess();
}

QString CorridorKeySetup::checkGpu(QString* gpuName, double* memoryGB)
{
    QProcess smi;
    ckSetupLowPriority(smi);
    smi.start(QStringLiteral("nvidia-smi"),
              {QStringLiteral("--query-gpu=name,compute_cap,driver_version,memory.total"),
               QStringLiteral("--format=csv,noheader,nounits")});
    const QString none = tr("No NVIDIA graphics card was found. CorridorKey needs an NVIDIA "
                            "RTX 30-series or newer graphics card with its driver installed.");
    if (!smi.waitForStarted(5000) || !smi.waitForFinished(15000) || smi.exitCode() != 0)
        return none;
    const QString firstLine = QString::fromUtf8(smi.readAllStandardOutput()).section('\n', 0, 0);
    const auto gpu = ck::parseNvidiaSmiLine(firstLine.toStdString());
    if (!gpu) return none;
    if (gpuName) *gpuName = QString::fromStdString(gpu->name);
    if (memoryGB) *memoryGB = gpu->memoryGB;
    return QString::fromStdString(ck::gpuProblem(*gpu));
}

// ── Progress ─────────────────────────────────────────────────────────────────

double CorridorKeySetup::stepWeight(Step step) const
{
    switch (step) {
    case Step::Check:     return 0.005;
    case Step::Uv:        return 0.005;
    case Step::Python:    return 0.010;
    case Step::Venv:      return 0.005;
    case Step::Torch:     return 0.300;
    case Step::Libraries: return 0.050;
    case Step::Models:    return 0.600;
    case Step::Verify:    return 0.025;
    case Step::Done:      return 0.0;
    }
    return 0.0;
}

double CorridorKeySetup::stepStart(Step step) const
{
    double start = 0.0;
    for (Step s = Step::Check; s != step; s = static_cast<Step>(static_cast<int>(s) + 1))
        start += stepWeight(s);
    return start;
}

void CorridorKeySetup::report(const QString& detail)
{
    const double progress = std::min(1.0, stepStart(m_step) + stepWeight(m_step) * m_stepProgress);
    emit progressChanged(progress, m_stage, detail);
}

// ── Steps ────────────────────────────────────────────────────────────────────

void CorridorKeySetup::start()
{
    if (m_running) return;
    m_running = true;
    m_cancelled = false;
    m_step = Step::Check;
    spdlog::info("CorridorKey setup: starting in {}", pathToUtf8(m_layout.runtimeDir));
    runStep();
}

void CorridorKeySetup::cancel()
{
    if (!m_running) return;
    m_cancelled = true;
    if (m_reply) m_reply->abort();
    killProcess();
    fail(tr("Setup cancelled. Start it again any time; it continues where it stopped."));
}

void CorridorKeySetup::next()
{
    m_step = static_cast<Step>(static_cast<int>(m_step) + 1);
    QTimer::singleShot(0, this, [this]() { if (m_running) runStep(); });
}

void CorridorKeySetup::fail(const QString& message)
{
    if (!m_running) return;
    m_running = false;
    if (m_poll) m_poll->stop();
    spdlog::warn("CorridorKey setup failed at step {}: {}", static_cast<int>(m_step),
                 message.toStdString());
    emit finished(false, message);
}

void CorridorKeySetup::runStep()
{
    m_stepProgress = 0.0;
    m_stepClock.start();
    std::error_code error;
    const QString uv = ckSetupPath(m_layout.uvExe());
    const QString python = ckSetupPath(m_layout.venvPython());

    switch (m_step) {
    case Step::Check: {
        m_stage = tr("Checking this computer");
        report();
        const QString problem = checkGpu();
        if (!problem.isEmpty()) return fail(problem);
        fs::create_directories(m_layout.runtimeDir, error);
        if (error) return fail(tr("Could not create %1.").arg(ckSetupPath(m_layout.runtimeDir)));
        const auto models = ck::modelFiles(m_layout);
        const bool modelsThere = std::all_of(models.begin(), models.end(),
            [](const fs::path& f) { std::error_code e; return fs::exists(f, e); });
        const int64_t needed = modelsThere ? 2'000'000'000 : ck::kSetupDiskBytes;
        const QStorageInfo disk(ckSetupPath(m_layout.runtimeDir));
        if (disk.isValid() && disk.bytesAvailable() < needed)
            return fail(tr("CorridorKey needs about %1 GB of free space on %2 while it installs "
                           "(%3 GB free). Free up some space and try again.")
                            .arg(ckSetupGB(double(needed)), disk.rootPath(),
                                 ckSetupGB(double(disk.bytesAvailable()))));
        return next();
    }
    case Step::Uv:
        m_stage = tr("Downloading the installer tool (uv)");
        if (fs::exists(m_layout.uvExe(), error)) return next();
        return downloadUv();
    case Step::Python:
        m_stage = tr("Installing a private copy of Python 3.12");
        return startProcess(uv, {QStringLiteral("python"), QStringLiteral("install"),
                                 QString::fromLatin1(ck::kPythonVersion.data(), ck::kPythonVersion.size()),
                                 QStringLiteral("--install-dir"), ckSetupPath(m_layout.pythonDir()),
                                 QStringLiteral("--no-bin"), QStringLiteral("--no-registry")},
                            m_stage);
    case Step::Venv:
        m_stage = tr("Creating CorridorKey's Python environment");
        if (fs::exists(m_layout.venvPython(), error)) return next();
        return startProcess(uv, {QStringLiteral("venv"), ckSetupPath(m_layout.venvDir()),
                                 QStringLiteral("--python"),
                                 QString::fromLatin1(ck::kPythonVersion.data(), ck::kPythonVersion.size())},
                            m_stage);
    case Step::Torch:
        m_stage = tr("Downloading PyTorch for NVIDIA GPUs (about 3 GB, the longest step)");
        return startProcess(uv, {QStringLiteral("pip"), QStringLiteral("install"),
                                 QStringLiteral("--python"), python,
                                 QString::fromLatin1(ck::kTorch.data(), ck::kTorch.size()),
                                 QString::fromLatin1(ck::kTorchVision.data(), ck::kTorchVision.size()),
                                 QStringLiteral("--index-url"),
                                 QString::fromLatin1(ck::kTorchIndex.data(), ck::kTorchIndex.size())},
                            m_stage);
    case Step::Libraries:
        m_stage = tr("Installing CorridorKey's libraries");
        return startProcess(uv, {QStringLiteral("pip"), QStringLiteral("install"),
                                 QStringLiteral("--python"), python,
                                 QStringLiteral("-r"), ckSetupPath(m_layout.requirements())},
                            m_stage);
    case Step::Models:
        m_stage = tr("Downloading the CorridorKey models (about 4.9 GB)");
        if (!m_poll) {
            m_poll = new QTimer(this);
            m_poll->setInterval(1000);
            connect(m_poll, &QTimer::timeout, this, &CorridorKeySetup::pollModels);
        }
        m_poll->start();
        return startProcess(python, {QStringLiteral("-I"), QStringLiteral("-u"),
                                     ckSetupPath(m_layout.downloadScript()),
                                     QStringLiteral("--package_root"), ckSetupPath(m_layout.modelsDir())},
                            m_stage);
    case Step::Verify:
        m_stage = tr("Checking that CorridorKey can use the GPU");
        return startProcess(python, {QStringLiteral("-I"), QStringLiteral("-c"),
                                     QStringLiteral(
                                         "import sys, torch\n"
                                         "sys.path.insert(0, sys.argv[1])\n"
                                         "assert torch.cuda.is_available(), 'PyTorch cannot see the NVIDIA GPU'\n"
                                         "import corridorkey.runtime\n"
                                         "print('ok', torch.__version__, torch.version.cuda, "
                                         "torch.cuda.get_device_name(0))\n"),
                                     ckSetupPath(m_layout.codeDir)},
                            m_stage);
    case Step::Done: {
        m_stage = tr("Cleaning up");
        report();
        fs::remove_all(m_layout.cacheDir(), error);
        error.clear();
        fs::remove_all(m_layout.hfHome(), error);
        error.clear();
        fs::remove_all(m_layout.modelsDir() / ".cache", error);
        std::ofstream(m_layout.stampFile(), std::ios::binary | std::ios::trunc) << ck::stampText();
        m_running = false;
        if (!ck::isInstalled(m_layout)) {
            emit finished(false, tr("Setup finished but some files are missing. Try again."));
            return;
        }
        spdlog::info("CorridorKey setup: installed");
        emit progressChanged(1.0, tr("CorridorKey is ready"), {});
        emit finished(true, tr("CorridorKey is ready"));
        return;
    }
    }
}

// ── Processes ────────────────────────────────────────────────────────────────

QProcessEnvironment CorridorKeySetup::environment() const
{
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    for (const char* name : {"PYTHONPATH", "PYTHONHOME", "VIRTUAL_ENV", "CONDA_PREFIX", "UV_INDEX_URL",
                             "PIP_INDEX_URL", "UV_PYTHON"})
        env.remove(QString::fromLatin1(name));
    env.insert(QStringLiteral("UV_CACHE_DIR"), ckSetupPath(m_layout.cacheDir()));
    env.insert(QStringLiteral("UV_PYTHON_INSTALL_DIR"), ckSetupPath(m_layout.pythonDir()));
    env.insert(QStringLiteral("UV_PYTHON_PREFERENCE"), QStringLiteral("only-managed"));
    env.insert(QStringLiteral("UV_NO_CONFIG"), QStringLiteral("1"));
    env.insert(QStringLiteral("UV_NO_PROGRESS"), QStringLiteral("1"));
    env.insert(QStringLiteral("HF_HOME"), ckSetupPath(m_layout.hfHome()));
    env.insert(QStringLiteral("HF_HUB_DISABLE_TELEMETRY"), QStringLiteral("1"));
    env.insert(QStringLiteral("PYTHONUTF8"), QStringLiteral("1"));
    env.insert(QStringLiteral("PYTHONNOUSERSITE"), QStringLiteral("1"));
    return env;
}

void CorridorKeySetup::startProcess(const QString& program, const QStringList& args, const QString& stage)
{
    report();
    m_processTail.clear();
    auto* process = new QProcess(this);
    m_process = process;
    process->setProcessEnvironment(environment());
    process->setProcessChannelMode(QProcess::MergedChannels);
    process->setWorkingDirectory(ckSetupPath(m_layout.runtimeDir));
    ckSetupLowPriority(*process);
    connect(process, &QProcess::readyReadStandardOutput, this, [this, process]() {
        while (process->canReadLine()) {
            const QString line = QString::fromUtf8(process->readLine()).trimmed();
            if (line.isEmpty()) continue;
            spdlog::debug("CorridorKey setup: {}", line.toStdString());
            m_processTail << line;
            if (m_processTail.size() > 12) m_processTail.removeFirst();
        }
        if (m_step == Step::Torch || m_step == Step::Libraries || m_step == Step::Python) {
            const qint64 minutes = m_stepClock.elapsed() / 60000;
            report(minutes > 0 ? tr("%n minute(s) so far", nullptr, int(minutes)) : QString());
        }
    });
    connect(process, &QProcess::finished, this, &CorridorKeySetup::onProcessFinished);
    connect(process, &QProcess::errorOccurred, this, [this, process, stage](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart && m_process == process)
            fail(tr("%1: could not start %2.").arg(stage, process->program()));
    });
    spdlog::info("CorridorKey setup: {} {}", program.toStdString(), args.join(' ').toStdString());
    process->start(program, args);
}

void CorridorKeySetup::onProcessFinished(int exitCode, QProcess::ExitStatus status)
{
    auto* process = qobject_cast<QProcess*>(sender());
    if (!process || process != m_process) return;
    process->deleteLater();
    if (m_poll) m_poll->stop();
    if (!m_running || m_cancelled) return;
    if (status != QProcess::NormalExit || exitCode != 0) {
        QString tail = m_processTail.join('\n');
        if (tail.size() > 1200) tail = tail.right(1200);
        return fail(tr("%1 failed.\n\n%2").arg(m_stage, tail));
    }
    if (m_step == Step::Uv) {
        std::error_code error;
        fs::remove(m_layout.uvDir() / "uv.zip", error);
        if (!fs::exists(m_layout.uvExe(), error)) return fail(tr("The uv download did not contain uv.exe."));
    }
    m_stepProgress = 1.0;
    report();
    next();
}

void CorridorKeySetup::killProcess()
{
    if (!m_process || m_process->state() == QProcess::NotRunning) return;
    // End the whole tree: uv and python start children of their own.
    QProcess::execute(QStringLiteral("taskkill"),
                      {QStringLiteral("/PID"), QString::number(m_process->processId()),
                       QStringLiteral("/T"), QStringLiteral("/F")});
    m_process->waitForFinished(5000);
}

void CorridorKeySetup::pollModels()
{
    if (m_step != Step::Models) return;
    const double bytes = double(ckSetupFolderBytes(m_layout.modelsDir()));
    m_stepProgress = std::min(0.99, bytes / double(ck::kModelsBytes));
    report(tr("%1 of %2 GB").arg(ckSetupGB(bytes), ckSetupGB(double(ck::kModelsBytes))));
}

// ── uv download ──────────────────────────────────────────────────────────────

void CorridorKeySetup::downloadUv()
{
    std::error_code error;
    fs::create_directories(m_layout.uvDir(), error);
    m_download = new QFile(ckSetupPath(m_layout.uvDir() / "uv.zip"), this);
    if (!m_download->open(QIODevice::WriteOnly | QIODevice::Truncate))
        return fail(tr("Could not write %1.").arg(m_download->fileName()));
    if (!m_network) m_network = new QNetworkAccessManager(this);
    QNetworkRequest request(QUrl(QString::fromLatin1(ck::kUvUrl.data(), ck::kUvUrl.size())));
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    report();
    m_reply = m_network->get(request);
    connect(m_reply, &QNetworkReply::readyRead, this, [this]() {
        if (m_reply && m_download) m_download->write(m_reply->readAll());
    });
    connect(m_reply, &QNetworkReply::downloadProgress, this, [this](qint64 got, qint64) {
        m_stepProgress = std::min(0.95, double(got) / double(ck::kUvBytes));
        report();
    });
    connect(m_reply, &QNetworkReply::finished, this, &CorridorKeySetup::onUvDownloaded);
}

void CorridorKeySetup::onUvDownloaded()
{
    QNetworkReply* reply = m_reply;
    if (!reply) return;
    reply->deleteLater();
    if (m_download) m_download->write(reply->readAll());
    const QString zipPath = m_download ? m_download->fileName() : QString();
    if (m_download) {
        m_download->close();
        m_download->deleteLater();
        m_download = nullptr;
    }
    if (!m_running || m_cancelled) return;
    if (reply->error() != QNetworkReply::NoError)
        return fail(tr("Could not download uv: %1\nCheck the internet connection and try again.")
                        .arg(reply->errorString()));

    QFile zip(zipPath);
    QCryptographicHash sha(QCryptographicHash::Sha256);
    if (!zip.open(QIODevice::ReadOnly) || !sha.addData(&zip))
        return fail(tr("Could not read the uv download."));
    zip.close();
    if (sha.result().toHex() != QByteArray(ck::kUvSha256.data(), qsizetype(ck::kUvSha256.size()))) {
        zip.remove();
        return fail(tr("The uv download did not match its published checksum, so it was not used. "
                       "Try again later."));
    }
    const QString tar = QDir(qEnvironmentVariable("SystemRoot", QStringLiteral("C:/Windows")))
                            .filePath(QStringLiteral("System32/tar.exe"));
    startProcess(tar, {QStringLiteral("-xf"), zipPath, QStringLiteral("-C"), ckSetupPath(m_layout.uvDir())},
                 m_stage);
}

} // namespace rt
