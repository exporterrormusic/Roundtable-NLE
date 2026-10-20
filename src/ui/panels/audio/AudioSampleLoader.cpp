/*
 * AudioSampleLoader.cpp — see AudioSampleLoader.h. The decoder and its
 * sample cache moved here from AudioSyncPlayback.cpp unchanged.
 */

#include "panels/audio/AudioSampleLoader.h"

#include "audio/AudioFile.h"
#include "PathUtils.h"
#include "MediaTaskQueue.h"
#include "NotificationCenter.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QPointer>
#include <QSaveFile>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <vector>

namespace rt {

// ═══════════════════════════════════════════════════════════════════════════
//  Audio sample loading
// ═══════════════════════════════════════════════════════════════════════════

// ─── Waveform sample cache (binary) ─────────────────────────────────────
// Saves decoded mono samples to a binary file so project re-opens skip
// the expensive audio-decode step.  Cache is invalidated when the source
// file's size or modification time changes.

static QString audioCacheDir()
{
    QString dir = QCoreApplication::applicationDirPath() + "/assets/cache/audio_samples";
    QDir().mkpath(dir);
    return dir;
}

static QString audioCacheKey(const std::string& path)
{
    // Simple hash of the absolute path to create a unique cache filename
    QByteArray pathBytes = QByteArray::fromStdString(path);
    auto hash = qHash(pathBytes);
    return QString::number(hash, 16);
}

struct AudioCacheHeader {
    uint32_t magic     = 0x52544157;  // "RTAW"
    uint32_t version   = 1;
    uint32_t sampleRate = 0;
    uint64_t numSamples = 0;
    int64_t  sourceSize = 0;
    int64_t  sourceModTime = 0;  // msecs since epoch
};

static QString audioCachePath(const std::string& sourcePath)
{
    return audioCacheDir() + "/" + audioCacheKey(sourcePath) + ".pcm";
}

/// The cached mono samples for `sourcePath`, memory-mapped, if the cache is
/// there and still matches the source file's size and modification time.
static std::optional<AudioSampleData> readAudioCache(const std::string& sourcePath)
{
    QFileInfo srcInfo(QString::fromStdString(sourcePath));
    if (!srcInfo.exists()) return std::nullopt;

    const QString cachePath = audioCachePath(sourcePath);
    AudioCacheHeader hdr{};
    {
        QFile cacheFile(cachePath);
        if (!cacheFile.open(QIODevice::ReadOnly))
            return std::nullopt;
        if (cacheFile.read(reinterpret_cast<char*>(&hdr), sizeof(hdr)) != sizeof(hdr))
            return std::nullopt;
        if (hdr.magic != 0x52544157 || hdr.version != 1)
            return std::nullopt;

        // Validate source hasn't changed
        if (hdr.sourceSize != srcInfo.size())
            return std::nullopt;
        if (hdr.sourceModTime != srcInfo.lastModified().toMSecsSinceEpoch())
            return std::nullopt;

        if (hdr.numSamples == 0) return std::nullopt;
        if (cacheFile.size() < static_cast<qint64>(sizeof(hdr) + hdr.numSamples * sizeof(float)))
            return std::nullopt;
    }

    AudioSampleData asd;
    asd.sampleRate = hdr.sampleRate;
    asd.samples = SampleBuffer::mapFile(utf8ToPath(cachePath.toStdString()),
                                        sizeof(hdr), hdr.numSamples);
    if (asd.samples.empty()) {
        // Could not map (address space or handle limits): read it instead.
        QFile cacheFile(cachePath);
        if (!cacheFile.open(QIODevice::ReadOnly) || !cacheFile.seek(sizeof(hdr)))
            return std::nullopt;
        std::vector<float> samples(static_cast<size_t>(hdr.numSamples));
        const qint64 dataBytes = static_cast<qint64>(hdr.numSamples * sizeof(float));
        if (cacheFile.read(reinterpret_cast<char*>(samples.data()), dataBytes) != dataBytes)
            return std::nullopt;
        asd.samples = SampleBuffer(std::move(samples));
    }
    return asd;
}

/// Writes the cache while the file decodes, so the samples never have to be
/// in memory all at once. QSaveFile: a reader (or a crash) never sees a
/// half-written cache.
class AudioCacheWriter
{
public:
    bool begin(const std::string& sourcePath, uint32_t sampleRate)
    {
        QFileInfo srcInfo(QString::fromStdString(sourcePath));
        if (!srcInfo.exists()) return false;
        m_path = audioCachePath(sourcePath);
        m_file = std::make_unique<QSaveFile>(m_path);
        if (!m_file->open(QIODevice::WriteOnly)) {
            m_file.reset();
            return false;
        }
        m_header.sampleRate    = sampleRate;
        m_header.sourceSize    = srcInfo.size();
        m_header.sourceModTime = srcInfo.lastModified().toMSecsSinceEpoch();
        return write(&m_header, sizeof(m_header));   // numSamples patched by finish()
    }

    bool append(const float* samples, size_t count)
    {
        m_header.numSamples += count;
        return write(samples, count * sizeof(float));
    }

    bool finish()
    {
        if (!m_file || m_failed || m_header.numSamples == 0) return false;
        if (!m_file->seek(0) || !write(&m_header, sizeof(m_header)) || !m_file->commit()) {
            spdlog::warn("AudioSync: could not write waveform cache {}", m_path.toStdString());
            return false;
        }
        return true;
    }

private:
    bool write(const void* data, size_t bytes)
    {
        if (!m_file || m_failed) return false;
        if (m_file->write(static_cast<const char*>(data), static_cast<qint64>(bytes)) !=
            static_cast<qint64>(bytes))
            m_failed = true;
        return !m_failed;
    }

    QString                    m_path;
    std::unique_ptr<QSaveFile> m_file;
    AudioCacheHeader           m_header{};
    bool                       m_failed{false};
};

namespace {

/// Whole-file mono samples for waveforms/analysis. Thread-safe (no AudioSync
/// state): runs on a MediaTaskQueue worker. The file decodes straight into
/// the sample cache, which is then memory-mapped; only when the cache cannot
/// be written are the samples kept in memory.
std::optional<AudioSampleData> decodeMonoSamples(const std::string& path,
                                                 const std::stop_token& stop)
{
    // Block peaks let full-file waveforms draw without reading every sample;
    // build them here, off the UI thread.
    auto withPeaks = [](AudioSampleData asd) {
        (void)asd.samples.blockPeaks();
        return asd;
    };

    // Cached mono samples skip the expensive decode.
    if (auto cached = readAudioCache(path)) {
        spdlog::info("AudioSync: Loaded {} samples ({}Hz) from cache for {}",
                     cached->samples.size(), static_cast<int>(cached->sampleRate), path);
        return withPeaks(std::move(*cached));
    }
    if (stop.stop_requested()) return std::nullopt;

    AudioFile af;
    if (!af.open(path)) {
        spdlog::warn("AudioSync: Failed to open audio file for waveform: {}", path);
        return std::nullopt;
    }

    const auto info = af.info();
    if (info.channels == 0) {
        spdlog::warn("AudioSync: Empty audio data from: {}", path);
        return std::nullopt;
    }

    // Decode, mixing to mono for waveform display, into `sink`.
    std::vector<float> mono;
    auto decodeMono = [&](const std::function<bool(const float*, size_t)>& sink) {
        return af.readAllStreaming([&](const float* samples, int64_t frames) {
            if (stop.stop_requested()) return false;
            const float* out = samples;
            if (info.channels != 1) {
                mono.resize(static_cast<size_t>(frames));
                for (size_t i = 0; i < mono.size(); ++i) {
                    float sum = 0.0f;
                    for (uint32_t ch = 0; ch < info.channels; ++ch)
                        sum += samples[i * info.channels + ch];
                    mono[i] = sum / static_cast<float>(info.channels);
                }
                out = mono.data();
            }
            return sink(out, static_cast<size_t>(frames));
        });
    };

    AudioCacheWriter cache;
    if (cache.begin(path, info.sampleRate)) {
        const int64_t frames = decodeMono([&](const float* samples, size_t count) {
            cache.append(samples, count);   // a failure shows in finish()
            return true;
        });
        if (stop.stop_requested()) return std::nullopt;
        if (frames > 0 && cache.finish()) {
            if (auto mapped = readAudioCache(path)) {
                spdlog::info("AudioSync: Loaded {} samples ({}Hz) from {}",
                             mapped->samples.size(), static_cast<int>(info.sampleRate), path);
                return withPeaks(std::move(*mapped));
            }
        }
        if (frames <= 0) {
            spdlog::warn("AudioSync: Empty audio data from: {}", path);
            return std::nullopt;
        }
        // The cache could not be written or read back: decode again into memory.
    }

    std::vector<float> all;
    const int64_t frames = decodeMono([&](const float* samples, size_t count) {
        all.insert(all.end(), samples, samples + count);
        return true;
    });
    if (stop.stop_requested()) return std::nullopt;
    if (frames <= 0 || all.empty()) {
        spdlog::warn("AudioSync: Empty audio data from: {}", path);
        return std::nullopt;
    }

    spdlog::info("AudioSync: Loaded {} samples ({}Hz) from {} (not cached)",
                 all.size(), static_cast<int>(info.sampleRate), path);
    AudioSampleData asd;
    asd.samples    = SampleBuffer(std::move(all));
    asd.sampleRate = info.sampleRate;
    return withPeaks(std::move(asd));
}

/// Task result. `mutable` so the one subscriber can move the samples out
/// of the queue's shared const result instead of copying ~700 MB per hour.
struct DecodedAudioSamples {
    mutable AudioSampleData data;
};

} // namespace

AudioSampleLoader::AudioSampleLoader(Hooks hooks, DecodeFn decode, MediaTaskQueue* queue,
                                     QObject* parent)
    : QObject(parent)
    , m_hooks(std::move(hooks))
    , m_decode(decode ? std::move(decode) : DecodeFn(&decodeMonoSamples))
    , m_tasks(queue ? queue : &MediaTaskQueue::instance())
    , m_owner(m_tasks->createOwner())
{
}

AudioSampleLoader::~AudioSampleLoader()
{
    // A running decode is cancelled; its completion finds this loader gone.
    cancel();
}

void AudioSampleLoader::request(const std::vector<std::string>& paths)
{
    for (const auto& path : paths) {
        if (m_hooks.loaded(path) || m_failed.count(path) || path == m_inFlight ||
            std::find(m_queue.begin(), m_queue.end(), path) != m_queue.end())
            continue;
        m_queue.push_back(path);
        ++m_total;
    }
    pump();
}

bool AudioSampleLoader::pending(const std::string& path) const
{
    return path == m_inFlight || std::find(m_queue.begin(), m_queue.end(), path) != m_queue.end();
}

void AudioSampleLoader::pump()
{
    if (!m_inFlight.empty())
        return;

    while (!m_queue.empty()) {
        std::string path = std::move(m_queue.front());
        m_queue.pop_front();
        // Loaded meanwhile (loadNow), or no longer imported
        // (removed, or another script session is active now).
        if (m_hooks.loaded(path) || !m_hooks.wanted(path)) {
            ++m_done;
            continue;
        }

        if (auto* notices = NotificationCenter::current()) {
            const QString fileName = QFileInfo(QString::fromStdString(path)).fileName();
            const int progress = m_total > 0 ? m_done * 100 / m_total : -1;
            if (m_notice == 0) {
                QPointer<AudioSampleLoader> guard(this);
                m_notice = notices->beginTask(
                    QStringLiteral("Loading audio waveforms"), fileName, progress,
                    [guard]() { if (guard) guard->cancel(); });
            }
            else
                notices->updateTask(m_notice, progress, fileName);
        }

        // Keyed per owner: the one subscriber moves the samples out, so no
        // other panel may share the task.
        m_inFlight = path;
        QPointer<AudioSampleLoader> self(this);
        const uint64_t generation = m_generation;
        const bool accepted = m_tasks->submit<DecodedAudioSamples>(
            m_owner,
            "audiosync-mono:" + std::to_string(m_owner) + ":" + path,
            MediaTaskQueue::Priority::Visible,
            [path, decode = m_decode](std::stop_token stop) {
                auto decoded = decode(path, stop);
                if (!decoded)
                    return MediaTaskQueue::Result<DecodedAudioSamples>::failure(
                        "could not decode audio");
                return MediaTaskQueue::Result<DecodedAudioSamples>::success(
                    DecodedAudioSamples{std::move(*decoded)});
            },
            [self, path, generation](MediaTaskQueue::Result<DecodedAudioSamples> result) {
                auto* app = QCoreApplication::instance();
                if (!app) return;
                QMetaObject::invokeMethod(app, [self, path, generation,
                                                result = std::move(result)]() {
                    if (!self) return;
                    std::optional<AudioSampleData> samples;
                    if (result.succeeded())
                        samples = std::move(result.value->data);
                    self->onDecoded(path, generation, std::move(samples));
                }, Qt::QueuedConnection);
            });
        if (accepted) return;
        m_inFlight.clear();
        ++m_done;
        ++m_failedCount;
    }
    finishIfIdle();
}

void AudioSampleLoader::onDecoded(const std::string& path, uint64_t generation,
                                  std::optional<AudioSampleData> samples)
{
    // Landed after a cancel: the samples are still good, but the queue state
    // now belongs to newer requests.
    const bool current = m_generation == generation;
    if (current) {
        if (m_inFlight == path)
            m_inFlight.clear();
        ++m_done;
        if (!samples) {
            ++m_failedCount;
            m_failed.insert(path);
        }
    }
    if (samples && m_hooks.wanted(path))
        m_hooks.install(path, std::move(*samples));
    if (current) pump();
}

void AudioSampleLoader::finishIfIdle()
{
    if (pending()) return;
    if (m_notice != 0) {
        if (auto* notices = NotificationCenter::current()) {
            // Only a failure is worth a notification; success just clears.
            const int failed = m_failedCount;
            notices->finishTask(m_notice, failed == 0,
                                failed == 0 ? QString()
                                    : QStringLiteral("%1 audio file(s) could not be read "
                                                     "(see the log).").arg(failed),
                                /*addNotification=*/failed > 0);
        }
        m_notice = 0;
    }
    m_total = 0;
    m_done = 0;
    m_failedCount = 0;

    // Actions that waited for complete audio (Auto-Sync, auto-trim...).
    auto ready = std::move(m_waiting);
    m_waiting.clear();
    for (auto& [action, fn] : ready)
        if (fn) fn();
}

void AudioSampleLoader::cancel()
{
    ++m_generation;
    m_tasks->cancelOwner(m_owner);
    m_queue.clear();
    m_inFlight.clear();
    m_waiting.clear();
    if (m_notice != 0) {
        if (auto* notices = NotificationCenter::current())
            notices->finishTask(m_notice, false,
                                QStringLiteral("Cancelled"), /*addNotification=*/false);
        m_notice = 0;
    }
    m_total = 0;
    m_done = 0;
    m_failedCount = 0;
}

void AudioSampleLoader::loadNow(const std::vector<std::string>& paths)
{
    for (const auto& path : paths) {
        if (m_hooks.loaded(path) || m_failed.count(path)) continue;
        // A background decode of the same file may still finish; its result
        // is dropped because the entry exists by then.
        if (auto decoded = m_decode(path, std::stop_token{}))
            m_hooks.install(path, std::move(*decoded));
        else
            m_failed.insert(path);
    }
}

void AudioSampleLoader::whenReady(const std::vector<std::string>& paths, const QString& action,
                                  std::function<void()> fn)
{
    if (!fn) return;
    request(paths);   // re-queue anything missing, e.g. a cancelled load
    if (!pending()) {
        fn();
        return;
    }
    for (const auto& waiting : m_waiting)
        if (waiting.first == action) return;   // already waiting (double click)
    m_waiting.emplace_back(action, std::move(fn));
    if (auto* notices = NotificationCenter::current())
        notices->postInfo(action, QStringLiteral("Starts when the audio finishes loading."));
}

} // namespace rt
