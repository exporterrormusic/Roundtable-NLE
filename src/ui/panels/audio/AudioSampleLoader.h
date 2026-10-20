/*
 * AudioSampleLoader.h — decodes AudioSync's audio files to whole-file mono
 * samples (waveforms, auto-sync, trims) in the background, one file at a
 * time (the peak memory of the old synchronous loop), and hands each result
 * to its owner on the UI thread.
 *
 *   request(paths)   queue every file not loaded, failed or queued yet
 *   whenReady(...)   run an action once nothing is decoding (a repeat of
 *                    the same waiting action is dropped)
 *   cancel()         drop the queue and the waiting actions; a decode
 *                    already running may still deliver its samples
 *
 * A file that fails to decode is not retried until forgetFailure() (the
 * owner calls it when the file is imported again). Decoded samples are also
 * written to a sample cache (assets/cache/audio_samples) that is memory-
 * mapped on the next load. Progress shows as a NotificationCenter task.
 *
 * The owner says which files still matter (`wanted`: imported in the open
 * script) and which are loaded; a result for a file no longer wanted is
 * dropped.
 */

#pragma once

#include "audiosync/SyncClip.h"   // AudioSampleData

#include <QObject>
#include <QString>

#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <stop_token>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace rt {

class MediaTaskQueue;

class AudioSampleLoader : public QObject
{
    Q_OBJECT

public:
    using DecodeFn = std::function<std::optional<AudioSampleData>(
        const std::string& path, const std::stop_token& stop)>;

    struct Hooks
    {
        /// The file still belongs to the open script.
        std::function<bool(const std::string& path)> wanted;
        /// Its samples are installed already.
        std::function<bool(const std::string& path)> loaded;
        /// Take decoded samples of a wanted file.
        std::function<void(const std::string& path, AudioSampleData samples)> install;
    };

    /// `decode` defaults to the real decoder (with the sample cache); tests
    /// pass a fake. `queue` defaults to MediaTaskQueue::instance().
    explicit AudioSampleLoader(Hooks hooks, DecodeFn decode = {},
                               MediaTaskQueue* queue = nullptr, QObject* parent = nullptr);
    ~AudioSampleLoader() override;

    /// Queue a background decode for each of `paths` not loaded, failed or
    /// queued yet, and start the next one.
    void request(const std::vector<std::string>& paths);
    /// Decode the files of `paths` still missing on this thread, now.
    void loadNow(const std::vector<std::string>& paths);
    /// Run `fn` now when nothing is decoding (after request(paths)), else
    /// once the decodes finish. A waiting action of the same name is kept
    /// and this one dropped (double click). Posts "starts when the audio
    /// finishes loading" when it has to wait.
    void whenReady(const std::vector<std::string>& paths, const QString& action,
                   std::function<void()> fn);
    /// Drop the actions waiting for the decodes (the open script changed).
    void dropWaitingActions() { m_waiting.clear(); }
    /// Stop the queued/running decodes and drop the waiting actions.
    void cancel();
    /// Allow a failed file to be decoded again.
    void forgetFailure(const std::string& path) { m_failed.erase(path); }

    /// A decode is queued or running.
    [[nodiscard]] bool pending() const noexcept
    {
        return !m_inFlight.empty() || !m_queue.empty();
    }
    /// `path` is queued or decoding.
    [[nodiscard]] bool pending(const std::string& path) const;
    [[nodiscard]] bool failed(const std::string& path) const { return m_failed.count(path) > 0; }

private:
    void pump();
    void finishIfIdle();
    void onDecoded(const std::string& path, uint64_t generation,
                   std::optional<AudioSampleData> samples);

    Hooks m_hooks;
    DecodeFn m_decode;
    MediaTaskQueue* m_tasks;
    uint64_t m_owner{0};
    std::deque<std::string> m_queue;
    std::string m_inFlight;
    std::vector<std::pair<QString, std::function<void()>>> m_waiting;
    std::unordered_set<std::string> m_failed;   ///< not retried until forgetFailure()
    uint64_t m_notice{0};                       ///< NotificationCenter task
    int m_total{0};
    int m_done{0};
    int m_failedCount{0};
    uint64_t m_generation{0};                   ///< bumped by cancel()
};

} // namespace rt
