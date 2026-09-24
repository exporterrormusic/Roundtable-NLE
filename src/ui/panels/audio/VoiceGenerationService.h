#pragma once

#include <QObject>
#include <QList>
#include <QString>
#include <QStringList>

#include <functional>

class QProcess;

namespace rt {

class Project;

struct VoiceReferenceSegment
{
    QString audioFile;
    QString transcript;
    double start{0.0};
    double end{0.0};      // 0 = to the end of the file
};

struct VoiceGenerationRequest
{
    QString requestId;         // Identifies the panel that owns the audition draft.
    QString provider;          // VoiceProvider::key
    QString text;
    QString character;
    QList<VoiceReferenceSegment> references;
    double speed{1.0};
    double targetDuration{0.0};
    int seed{42};
    int scriptLineNumber{-1};
    QString scriptSegment;
};

/// Owns the persistent local TTS worker.  At most one provider process is
/// alive, which prevents multiple large voice models from occupying VRAM
/// together: switching engines waits for the old worker to exit before the
/// new one starts, without blocking the UI thread.
class VoiceGenerationService final : public QObject
{
    Q_OBJECT

public:
    /// Program + arguments used to start a worker.  Tests replace it with a
    /// fake worker that speaks the same JSON-lines protocol.
    struct WorkerLaunch
    {
        QString program;
        QStringList arguments;
        QString workingDirectory;
    };
    using LaunchOverride = std::function<WorkerLaunch(const QString& provider)>;

    explicit VoiceGenerationService(QObject* parent = nullptr);
    ~VoiceGenerationService() override;

    void setProject(Project* project) noexcept { m_project = project; }
    [[nodiscard]] bool isBusy() const noexcept;
    [[nodiscard]] bool isModelResident() const noexcept;
    [[nodiscard]] QString activeProvider() const { return m_activeProvider; }
    [[nodiscard]] static bool providerInstalled(const QString& provider);
    [[nodiscard]] static QString providerInstallHint(const QString& provider);
    [[nodiscard]] static QString breezeInstallationRoot();
    [[nodiscard]] static bool configureBreezeInstallation(
        const QString& root, QString* error = nullptr);

    void enqueue(const VoiceGenerationRequest& request);
    void cancel();
    /// Stop the local worker and release its GPU model allocation.  Returns
    /// immediately; modelResidentChanged(false) fires once the process exits.
    void unloadModel();
    /// Blocking stop used at application exit so no model stays resident.
    void shutdown();

    /// Move/copy an audition draft beside the source reference audio using a
    /// unique CHARACTER-yyyyMMdd-HHmmss-zzz.wav filename. Falls back to the
    /// project Generated Audio folder when no reference source is available.
    [[nodiscard]] QString approveDraft(const VoiceGenerationRequest& request,
                                       const QString& draftPath,
                                       QString* error = nullptr) const;

    // Test seams.
    void setLaunchOverrideForTesting(LaunchOverride launch) { m_launchOverride = std::move(launch); }
    void setLoadTimeoutForTesting(int milliseconds) { m_loadTimeoutMs = milliseconds; }
    void setRetireTimeoutForTesting(int milliseconds) { m_retireTimeoutMs = milliseconds; }

signals:
    void statusChanged(const QString& status);
    void busyChanged(bool busy);
    void modelResidentChanged(bool resident);
    void generationFinished(const rt::VoiceGenerationRequest& request,
                            const QString& outputPath, double durationSeconds);
    void generationFailed(const rt::VoiceGenerationRequest& request,
                          const QString& error);

private:
    void processNext();
    void startWorker(const QString& provider);
    /// Hand the active worker off to exit in the background.  `immediate`
    /// kills it (it is loading or mid-generation and not reading stdin);
    /// otherwise an idle worker is asked to shut down cleanly.
    void retireWorker(bool immediate);
    void onRetiredWorkerFinished();
    void updateResidency();
    void abortRequests(const QString& reason);
    void sendCurrentRequest();
    void handleStdout();
    void handleStderr();
    void handleWorkerExit(int exitCode);
    void failCurrent(const QString& error);
    [[nodiscard]] WorkerLaunch launchFor(const QString& provider) const;
    [[nodiscard]] QString buildOutputPath(const VoiceGenerationRequest& request) const;
    [[nodiscard]] QString buildApprovedOutputPath(
        const VoiceGenerationRequest& request, const QString& suffix) const;

    Project* m_project{nullptr};
    QProcess* m_process{nullptr};   // active worker
    QProcess* m_retiring{nullptr};  // previous worker, exiting
    QByteArray m_stdoutBuffer;
    QByteArray m_stderrBuffer;
    QString m_lastWorkerError;
    QList<VoiceGenerationRequest> m_queue;
    VoiceGenerationRequest m_current;
    bool m_hasCurrent{false};
    bool m_workerReady{false};
    bool m_announceUnload{false};
    bool m_residentReported{false};
    quint64 m_loadGeneration{0};
    QString m_activeProvider;
    QString m_currentOutput;
    LaunchOverride m_launchOverride;
    int m_loadTimeoutMs{10 * 60 * 1000};
    int m_retireTimeoutMs{8000};
};

} // namespace rt
