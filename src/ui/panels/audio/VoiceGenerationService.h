#pragma once

#include <QObject>
#include <QList>
#include <QString>

class QProcess;

namespace rt {

class Project;

struct VoiceReferenceSegment
{
    QString audioFile;
    QString transcript;
    double start{0.0};
    double end{0.0};
};

struct VoiceGenerationRequest
{
    QString requestId;         // Identifies the panel that owns the audition draft.
    QString provider;          // "omnivoice" or "fish-s2"
    QString text;
    QString character;
    QString referenceAudio;
    QString referenceText;
    double referenceStart{0.0};
    double referenceEnd{0.0};
    QList<VoiceReferenceSegment> references;
    double speed{1.0};
    double targetDuration{0.0};
    int seed{42};
    int scriptLineNumber{-1};
    QString scriptSegment;
};

/// Owns the persistent local TTS worker.  At most one provider process is
/// alive, which prevents Fish and OmniVoice from occupying VRAM together.
class VoiceGenerationService final : public QObject
{
    Q_OBJECT

public:
    explicit VoiceGenerationService(QObject* parent = nullptr);
    ~VoiceGenerationService() override;

    void setProject(Project* project) noexcept { m_project = project; }
    [[nodiscard]] bool isBusy() const noexcept;
    [[nodiscard]] bool isModelResident() const noexcept { return m_process != nullptr; }
    [[nodiscard]] QString activeProvider() const { return m_activeProvider; }
    [[nodiscard]] static bool providerBuilt(const QString& provider);
    [[nodiscard]] static bool providerInstalled(const QString& provider);
    [[nodiscard]] static QString providerInstallHint(const QString& provider);

    void enqueue(const VoiceGenerationRequest& request);
    void cancel();
    /// Stop the local worker process and release its GPU model allocation.
    /// Safe to call while idle, loading, or generating.
    void unloadModel();

    /// Move/copy an audition draft beside the source reference audio using a
    /// unique CHARACTER-yyyyMMdd-HHmmss-zzz.wav filename. Falls back to the
    /// project Generated Audio folder when no reference source is available.
    [[nodiscard]] QString approveDraft(const VoiceGenerationRequest& request,
                                       const QString& draftPath,
                                       QString* error = nullptr) const;

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
    void stopWorker();
    void sendCurrentRequest();
    void handleStdout();
    void handleWorkerExit(int exitCode);
    void failCurrent(const QString& error);
    [[nodiscard]] QString buildOutputPath(const VoiceGenerationRequest& request) const;
    [[nodiscard]] QString buildApprovedOutputPath(
        const VoiceGenerationRequest& request, const QString& suffix) const;

    Project* m_project{nullptr};
    QProcess* m_process{nullptr};
    QByteArray m_stdoutBuffer;
    QList<VoiceGenerationRequest> m_queue;
    VoiceGenerationRequest m_current;
    bool m_hasCurrent{false};
    bool m_workerReady{false};
    bool m_cancelled{false};
    QString m_activeProvider;
    QString m_currentOutput;
};

} // namespace rt
