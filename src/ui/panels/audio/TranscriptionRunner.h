/*
 * TranscriptionRunner.h — transcribes a list of audio files one after another
 * on a background thread ("AudioSync transcription") and reports each file to
 * the thread that owns the runner (the UI thread).
 *
 * A run: start(paths, transcribe) → for each file fileStarted →
 * fileProgress* → fileFinished (success) or fileFailed (error) →
 * finished(cancelled).
 * cancel() stops after the current file, which is asked to stop through its
 * stop_token and the backend cancel; files already done keep their results.
 * Destroying the runner cancels and waits for the thread; nothing is
 * reported after that.
 *
 * Files are named by path, never by list position: the owner may reorder
 * its list while a run is going.
 *
 * What transcribes one file is passed to start() (AudioSync passes whisper /
 * CrisperWhisper through Transcriber; tests pass a fake).
 */

#pragma once

#include "ai/Transcriber.h"
#include "threading/WorkerThread.h"

#include <QObject>
#include <QString>

#include <cstdint>
#include <functional>
#include <stop_token>
#include <string>
#include <vector>

namespace rt {

class TranscriptionRunner : public QObject
{
    Q_OBJECT

public:
    using ProgressFn = TranscribeProgressFn;
    /// Transcribes one file on the worker thread. `stop` fires when the run
    /// is cancelled or the runner is destroyed.
    using TranscribeFn = std::function<TranscriptionResult(
        const std::string& path, std::stop_token stop, const ProgressFn& progress)>;

    /// `cancelBackend` interrupts a transcribe call in progress; it is called
    /// on the owning thread when cancelling and when destroyed mid-run.
    explicit TranscriptionRunner(std::function<void()> cancelBackend = {},
                                 QObject* parent = nullptr);
    ~TranscriptionRunner() override;

    TranscriptionRunner(const TranscriptionRunner&) = delete;
    TranscriptionRunner& operator=(const TranscriptionRunner&) = delete;

    /// Transcribe `paths` in order. False when a run is active or there is
    /// nothing to do.
    bool start(std::vector<std::string> paths, TranscribeFn transcribe);
    /// Stop after the current file. No-op unless a run is going.
    void cancel();

    [[nodiscard]] bool isActive() const noexcept { return m_state != State::Idle; }
    [[nodiscard]] bool isCancelling() const noexcept { return m_state == State::Cancelling; }
    /// The last run's figures; they stay readable after finished().
    [[nodiscard]] size_t total() const noexcept { return m_total; }
    /// Files transcribed or failed (a file cut short by cancel() is not).
    [[nodiscard]] size_t completed() const noexcept { return m_completed; }
    [[nodiscard]] bool hadSuccess() const noexcept { return m_hadSuccess; }
    /// The run's last error, empty when none.
    [[nodiscard]] const QString& error() const noexcept { return m_error; }
    [[nodiscard]] uint64_t runId() const noexcept { return m_runId; }

signals:
    /// `number` counts from 1 up to `total`.
    void fileStarted(const std::string& path, size_t number, size_t total);
    void fileProgress(const std::string& path, float percent, const QString& status);
    void fileFinished(const std::string& path, const rt::TranscriptionResult& result);
    void fileFailed(const std::string& path, const QString& error);
    void finished(bool cancelled);

private:
    enum class State { Idle, Running, Cancelling };

    void onFileStarted(uint64_t runId, const std::string& path);
    void onFileProgress(uint64_t runId, const std::string& path, float percent,
                        const QString& status);
    void onFileDone(uint64_t runId, const std::string& path, TranscriptionResult result);
    void onRunEnd(uint64_t runId);

    std::function<void()> m_cancelBackend;
    State m_state{State::Idle};
    uint64_t m_runId{0};
    size_t m_total{0};
    size_t m_completed{0};
    bool m_hadSuccess{false};
    QString m_error;
    WorkerThread m_thread;   // last: stopped before the members above go
};

} // namespace rt
