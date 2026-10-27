/*
 * CorridorKeySetup — installs CorridorKey's runtime into
 * <app root>/runtimes/corridorkey (layout: ai/CorridorKey.h).
 *
 * Steps, each skipped when already done, so a cancelled or failed setup
 * resumes where it stopped:
 *   1. GPU + disk check (nvidia-smi; RTX 30-series+, driver 570+)
 *   2. uv (pinned release, SHA-256 checked, unpacked with Windows' tar)
 *   3. a private Python 3.12 (uv, not registered, nothing on PATH)
 *   4. a venv + PyTorch 2.9.1 for CUDA 12.8
 *   5. the pinned libraries (tools/corridorkey/requirements.txt)
 *   6. the models (CorridorKey's own download script: CorridorKey from
 *      Hugging Face, C-RADIO, MoGe, RVM from their publishers)
 *   7. a CUDA + import check, then the download caches are deleted
 *
 * Everything it writes stays inside the runtime folder (uv cache, Hugging
 * Face home), nothing goes to the user profile.
 */

#pragma once

#include "ai/CorridorKey.h"

#include <QElapsedTimer>
#include <QObject>
#include <QPointer>
#include <QProcess>
#include <QString>
#include <QStringList>

class QFile;
class QNetworkAccessManager;
class QNetworkReply;
class QTimer;

namespace rt {

class CorridorKeySetup final : public QObject
{
    Q_OBJECT

public:
    explicit CorridorKeySetup(corridorkey::Layout layout, QObject* parent = nullptr);
    ~CorridorKeySetup() override;

    void start();
    void cancel();
    [[nodiscard]] bool isRunning() const noexcept { return m_running; }

    /// Checks the GPU synchronously (nvidia-smi, a second or so). Empty
    /// problem = fine; `gpu` gets the card's name when one was found.
    static QString checkGpu(QString* gpuName = nullptr, double* memoryGB = nullptr);

signals:
    /// progress 0..1 over the whole setup; stage = what is happening now.
    void progressChanged(double progress, const QString& stage, const QString& detail);
    void finished(bool ok, const QString& message);

private:
    enum class Step { Check, Uv, Python, Venv, Torch, Libraries, Models, Verify, Done };

    void runStep();
    void next();
    void fail(const QString& message);
    void report(const QString& detail = {});
    void startProcess(const QString& program, const QStringList& args, const QString& stage);
    void onProcessFinished(int exitCode, QProcess::ExitStatus status);
    void downloadUv();
    void onUvDownloaded();
    void pollModels();
    void killProcess();
    [[nodiscard]] QProcessEnvironment environment() const;
    [[nodiscard]] double stepStart(Step step) const;
    [[nodiscard]] double stepWeight(Step step) const;

    corridorkey::Layout     m_layout;
    Step                    m_step{Step::Check};
    bool                    m_running{false};
    bool                    m_cancelled{false};
    QString                 m_stage;
    QPointer<QProcess>      m_process;
    QStringList             m_processTail;   ///< last output lines, for errors
    QNetworkAccessManager*  m_network{nullptr};
    QPointer<QNetworkReply> m_reply;
    QFile*                  m_download{nullptr};
    QTimer*                 m_poll{nullptr};
    double                  m_stepProgress{0.0};
    QElapsedTimer           m_stepClock;
};

} // namespace rt
