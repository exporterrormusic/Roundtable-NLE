/*
 * TranscriptionRunner.cpp — see TranscriptionRunner.h.
 */

#include "panels/audio/TranscriptionRunner.h"

#include <QMetaObject>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <exception>
#include <utility>

namespace rt {

TranscriptionRunner::TranscriptionRunner(std::function<void()> cancelBackend, QObject* parent)
    : QObject(parent)
    , m_cancelBackend(std::move(cancelBackend))
{
}

TranscriptionRunner::~TranscriptionRunner()
{
    // The thread is joined here, before ~QObject: whatever it posted is
    // still queued for this object and is dropped with it.
    if (m_thread.running()) {
        m_thread.requestStop();
        if (m_state != State::Idle && m_cancelBackend)
            m_cancelBackend();
        m_thread.stop();
    }
}

bool TranscriptionRunner::start(std::vector<std::string> paths, TranscribeFn transcribe)
{
    if (m_state != State::Idle || paths.empty() || !transcribe)
        return false;
    // The previous run's thread has returned (finished() came from its last
    // post); collect it.
    m_thread.join();

    m_total = paths.size();
    m_completed = 0;
    m_hadSuccess = false;
    m_error.clear();
    m_state = State::Running;
    const uint64_t runId = ++m_runId;
    spdlog::info("AudioSync: Starting transcription run {} with {} file(s)", runId, m_total);

    // Posts use the runner as context: the destructor joins the thread before
    // the runner is gone, so `self` outlives every post.
    TranscriptionRunner* self = this;
    m_thread.start("AudioSync transcription",
        [self, runId, paths = std::move(paths), transcribe = std::move(transcribe)](
            std::stop_token stop) {
            for (const auto& path : paths) {
                if (stop.stop_requested()) break;
                QMetaObject::invokeMethod(self, [self, runId, path] {
                    self->onFileStarted(runId, path);
                }, Qt::QueuedConnection);

                const ProgressFn progress = [self, runId, path](float percent,
                                                                const std::string& status) {
                    QMetaObject::invokeMethod(self,
                        [self, runId, path, percent, text = QString::fromStdString(status)] {
                            self->onFileProgress(runId, path, percent, text);
                        }, Qt::QueuedConnection);
                };

                TranscriptionResult result;
                try {
                    result = transcribe(path, stop, progress);
                } catch (const std::exception& error) {
                    result = {};
                    result.status = TranscriptionStatus::Failed;
                    result.error.message = std::string("Transcription exception: ") + error.what();
                } catch (...) {
                    result = {};
                    result.status = TranscriptionStatus::Failed;
                    result.error.message = "Unknown transcription exception";
                }
                QMetaObject::invokeMethod(self,
                    [self, runId, path, result = std::move(result)]() mutable {
                        self->onFileDone(runId, path, std::move(result));
                    }, Qt::QueuedConnection);
            }
            QMetaObject::invokeMethod(self, [self, runId] {
                self->onRunEnd(runId);
            }, Qt::QueuedConnection);
        });
    return true;
}

void TranscriptionRunner::cancel()
{
    if (m_state != State::Running)
        return;
    m_state = State::Cancelling;
    spdlog::info("AudioSync: Cancelling transcription run {}", m_runId);
    m_thread.requestStop();
    if (m_cancelBackend)
        m_cancelBackend();
}

void TranscriptionRunner::onFileStarted(uint64_t runId, const std::string& path)
{
    if (runId != m_runId || m_state != State::Running)
        return;
    emit fileStarted(path, std::min(m_completed + 1, m_total), std::max<size_t>(m_total, 1));
}

void TranscriptionRunner::onFileProgress(uint64_t runId, const std::string& path,
                                         float percent, const QString& status)
{
    if (runId != m_runId || m_state == State::Idle)
        return;
    emit fileProgress(path, percent, status);
}

void TranscriptionRunner::onFileDone(uint64_t runId, const std::string& path,
                                     TranscriptionResult result)
{
    if (runId != m_runId || m_state == State::Idle)
        return;
    // A file stopped by cancel() neither counts nor fails the run.
    if (m_state == State::Cancelling && !result.succeeded())
        return;
    if (m_completed < m_total)
        ++m_completed;

    if (result.succeeded()) {
        m_hadSuccess = true;
        emit fileFinished(path, result);
        return;
    }
    if (result.status == TranscriptionStatus::Cancelled) {
        // Stopped by the backend itself, not by cancel().
        if (m_error.isEmpty())
            m_error = QStringLiteral("Transcription failed");
        return;
    }
    m_error = QString::fromStdString(
        result.error.message.empty() ? std::string("Transcription failed") : result.error.message);
    spdlog::error("AudioSync: Transcription run {} failed: {}", runId, m_error.toStdString());
    emit fileFailed(path, m_error);
}

void TranscriptionRunner::onRunEnd(uint64_t runId)
{
    if (runId != m_runId || m_state == State::Idle)
        return;
    m_thread.join();   // this was its last post; the body is returning
    const bool cancelled = m_state == State::Cancelling;
    m_state = State::Idle;
    if (cancelled)
        spdlog::info("AudioSync: Transcription run {} cancelled after {}/{} file(s)",
                     runId, m_completed, m_total);
    emit finished(cancelled);
}

} // namespace rt
