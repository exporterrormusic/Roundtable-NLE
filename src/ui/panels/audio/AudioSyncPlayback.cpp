/*
 * AudioSyncPlayback.cpp - Audio sample loading and playback for AudioSync.
 * Split from AudioSync.cpp for maintainability.
 */

#include "panels/audio/AudioSync.h"
#include "panels/audio/VideoFrameGrabber.h"

#include "audio/AudioEngine.h"
#include "audio/AudioFile.h"
#include "PathUtils.h"
#include "widgets/MiniWaveformWidget.h"
#include "MediaTaskQueue.h"
#include "NotificationCenter.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QPointer>
#include <QSaveFile>
#include <QTimer>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
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

void AudioSync::loadAudioSamples()
{
    for (const auto& path : m_audioPaths) {
        if (m_audioSamples.count(path) || m_audioDecodeFailedPaths.count(path) ||
            path == m_audioDecodeInFlight ||
            std::find(m_audioDecodeQueue.begin(), m_audioDecodeQueue.end(), path)
                != m_audioDecodeQueue.end())
            continue;
        m_audioDecodeQueue.push_back(path);
        ++m_audioDecodeTotal;
    }
    pumpAudioSampleDecode();
}

bool AudioSync::audioSamplesPending() const noexcept
{
    return !m_audioDecodeInFlight.empty() || !m_audioDecodeQueue.empty();
}

void AudioSync::pumpAudioSampleDecode()
{
    if (m_destroying.load(std::memory_order_acquire) || !m_audioDecodeInFlight.empty())
        return;

    while (!m_audioDecodeQueue.empty()) {
        std::string path = std::move(m_audioDecodeQueue.front());
        m_audioDecodeQueue.pop_front();
        // Loaded meanwhile (loadAudioSamplesNow), or no longer imported
        // (removed, or another script session is active now).
        if (m_audioSamples.count(path) ||
            std::find(m_audioPaths.begin(), m_audioPaths.end(), path) == m_audioPaths.end()) {
            ++m_audioDecodeDone;
            continue;
        }

        if (auto* notices = NotificationCenter::current()) {
            const QString fileName = QFileInfo(QString::fromStdString(path)).fileName();
            const int progress = m_audioDecodeTotal > 0
                ? m_audioDecodeDone * 100 / m_audioDecodeTotal : -1;
            if (m_audioDecodeNotice == 0) {
                QPointer<AudioSync> guard(this);
                m_audioDecodeNotice = notices->beginTask(
                    QStringLiteral("Loading audio waveforms"), fileName, progress,
                    [guard]() { if (guard) guard->cancelAudioSampleDecodes(); });
            }
            else
                notices->updateTask(m_audioDecodeNotice, progress, fileName);
        }

        // Keyed per owner: the one subscriber moves the samples out, so no
        // other panel may share the task.
        m_audioDecodeInFlight = path;
        QPointer<AudioSync> self(this);
        const uint64_t generation = m_audioDecodeGeneration;
        const bool accepted = MediaTaskQueue::instance().submit<DecodedAudioSamples>(
            m_audioTaskOwner,
            "audiosync-mono:" + std::to_string(m_audioTaskOwner) + ":" + path,
            MediaTaskQueue::Priority::Visible,
            [path](std::stop_token stop) {
                auto decoded = decodeMonoSamples(path, stop);
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
                    if (!self || self->m_destroying.load(std::memory_order_acquire)) return;
                    // Landed after a cancel: the samples are still good, but
                    // the queue state now belongs to newer requests.
                    const bool current = self->m_audioDecodeGeneration == generation;
                    if (current) {
                        if (self->m_audioDecodeInFlight == path)
                            self->m_audioDecodeInFlight.clear();
                        ++self->m_audioDecodeDone;
                        if (!result.succeeded()) {
                            ++self->m_audioDecodeFailed;
                            self->m_audioDecodeFailedPaths.insert(path);
                        }
                    }
                    if (result.succeeded() &&
                        std::find(self->m_audioPaths.begin(), self->m_audioPaths.end(), path)
                            != self->m_audioPaths.end())
                        self->installAudioSamples(path, std::move(result.value->data));
                    if (current) self->pumpAudioSampleDecode();
                }, Qt::QueuedConnection);
            });
        if (accepted) return;
        m_audioDecodeInFlight.clear();
        ++m_audioDecodeDone;
        ++m_audioDecodeFailed;
    }
    finishAudioSampleDecodes();
}

void AudioSync::finishAudioSampleDecodes()
{
    if (audioSamplesPending()) return;
    if (m_audioDecodeNotice != 0) {
        if (auto* notices = NotificationCenter::current()) {
            // Only a failure is worth a notification; success just clears.
            const int failed = m_audioDecodeFailed;
            notices->finishTask(m_audioDecodeNotice, failed == 0,
                                failed == 0 ? QString()
                                    : QStringLiteral("%1 audio file(s) could not be read "
                                                     "(see the log).").arg(failed),
                                /*addNotification=*/failed > 0);
        }
        m_audioDecodeNotice = 0;
    }
    m_audioDecodeTotal = 0;
    m_audioDecodeDone = 0;
    m_audioDecodeFailed = 0;

    // Actions that waited for complete audio (Auto-Sync, auto-trim...).
    auto ready = std::move(m_afterAudioSamplesReady);
    m_afterAudioSamplesReady.clear();
    for (auto& [action, fn] : ready)
        if (fn && !m_destroying.load(std::memory_order_acquire)) fn();
}

void AudioSync::cancelAudioSampleDecodes()
{
    ++m_audioDecodeGeneration;
    MediaTaskQueue::instance().cancelOwner(m_audioTaskOwner);
    m_audioDecodeQueue.clear();
    m_audioDecodeInFlight.clear();
    m_afterAudioSamplesReady.clear();
    if (m_audioDecodeNotice != 0) {
        if (auto* notices = NotificationCenter::current())
            notices->finishTask(m_audioDecodeNotice, false,
                                QStringLiteral("Cancelled"), /*addNotification=*/false);
        m_audioDecodeNotice = 0;
    }
    m_audioDecodeTotal = 0;
    m_audioDecodeDone = 0;
    m_audioDecodeFailed = 0;
}

void AudioSync::installAudioSamples(const std::string& path, AudioSampleData decoded)
{
    if (decoded.samples.empty() || m_audioSamples.count(path)) return;
    const AudioSampleData& stored = m_audioSamples[path] = std::move(decoded);

    // Cards built before the samples arrived show an empty waveform.
    for (size_t i = 0; i < m_cardWaveforms.size() && i < m_cardClipIndices.size(); ++i) {
        const int ci = m_cardClipIndices[i];
        MiniWaveformWidget* wf = m_cardWaveforms[i];
        if (!wf || ci < 0 || static_cast<size_t>(ci) >= m_clips.size()) continue;
        const SyncClip& clip = m_clips[static_cast<size_t>(ci)];
        if (clip.sourceFile != path) continue;
        wf->setAudioShared(stored.samples, stored.sampleRate, clip.start, clip.end);
        if (!clip.deletedRegions.empty())
            wf->setDeletedRegions(clip.deletedRegions);
    }
    // File lists show duration + a waveform only once samples exist.
    refreshAudioFileListItem(path);
    refreshTranscribeFileList();
}

void AudioSync::loadAudioSamplesNow()
{
    for (const auto& path : m_audioPaths) {
        if (m_audioSamples.count(path) || m_audioDecodeFailedPaths.count(path)) continue;
        // A background decode of the same file may still finish; its result
        // is dropped because the entry exists by then.
        if (auto decoded = decodeMonoSamples(path, std::stop_token{}))
            installAudioSamples(path, std::move(*decoded));
        else
            m_audioDecodeFailedPaths.insert(path);
    }
}

void AudioSync::whenAudioSamplesReady(const QString& action, std::function<void()> fn)
{
    if (!fn) return;
    loadAudioSamples();   // re-queue anything missing, e.g. a cancelled load
    if (!audioSamplesPending()) {
        fn();
        return;
    }
    for (const auto& waiting : m_afterAudioSamplesReady)
        if (waiting.first == action) return;   // already waiting (double click)
    m_afterAudioSamplesReady.emplace_back(action, std::move(fn));
    if (auto* notices = NotificationCenter::current())
        notices->postInfo(action, QStringLiteral("Starts when the audio finishes loading."));
}

bool AudioSync::audioReadyOrNotify(const std::string& path, const QString& action)
{
    const bool pending = path.empty()
        ? audioSamplesPending()
        : (path == m_audioDecodeInFlight ||
           std::find(m_audioDecodeQueue.begin(), m_audioDecodeQueue.end(), path)
               != m_audioDecodeQueue.end());
    if (!pending) return true;
    if (auto* notices = NotificationCenter::current())
        notices->postInfo(action,
                          QStringLiteral("The audio is still loading. Try again in a moment."));
    return false;
}

// ═══════════════════════════════════════════════════════════════════════════
//  Playback
// ═══════════════════════════════════════════════════════════════════════════

void AudioSync::playClip(size_t clipIdx)
{
    if (!m_audioEngine || clipIdx >= m_clips.size())
        return;

    spdlog::info("AudioSync::playClip: clip={} clips.size={}", clipIdx, m_clips.size());

    stopPlayback();

    const auto& clip = m_clips[clipIdx];
    auto it = m_audioSamples.find(clip.sourceFile);
    if (it == m_audioSamples.end()) {
        spdlog::warn("AudioSync: No audio samples for clip {}", clipIdx);
        return;
    }

    const auto& audioData = it->second;

    // Determine playback start: use waveform playhead if available, else trimIn
    double playFromTime = clip.start;
    MiniWaveformWidget* wf = waveformForClip(static_cast<int>(clipIdx));
    if (wf && wf->isPlayheadVisible()) {
        double ph = wf->playhead();
        if (ph >= clip.start && ph < clip.end)
            playFromTime = ph;
    }

    // Build buffer from trimIn..trimOut, excluding deletedRegions
    auto startSample = static_cast<size_t>(clip.start * audioData.sampleRate);
    auto endSample   = static_cast<size_t>(clip.end * audioData.sampleRate);
    if (startSample >= audioData.samples.size()) return;
    if (endSample > audioData.samples.size()) endSample = audioData.samples.size();

    // Sort deleted regions by start time for sequential processing
    auto sortedDeleted = clip.deletedRegions;
    std::sort(sortedDeleted.begin(), sortedDeleted.end());

    // Build buffer skipping deleted regions, and build a time-mapping
    m_playbackBuffer.clear();
    m_playbackTimeMap.clear();  // maps buffer frame offset → original time
    double sr = audioData.sampleRate;

    double cursor = clip.start;
    for (const auto& [delStart, delEnd] : sortedDeleted) {
        if (delStart >= clip.end) break;
        double regionStart = std::max(delStart, clip.start);
        double regionEnd   = std::min(delEnd, clip.end);
        if (regionStart <= cursor && regionEnd > cursor) {
            // Cursor is inside this deleted region — skip ahead
            cursor = regionEnd;
            continue;
        }
        if (regionStart > cursor) {
            // Copy samples from cursor to regionStart
            auto from = static_cast<size_t>(cursor * sr);
            auto to   = static_cast<size_t>(regionStart * sr);
            from = std::min(from, audioData.samples.size());
            to   = std::min(to, audioData.samples.size());
            if (to > from) {
                m_playbackTimeMap.push_back({static_cast<int64_t>(m_playbackBuffer.size()), cursor});
                m_playbackBuffer.insert(m_playbackBuffer.end(),
                    audioData.samples.begin() + static_cast<ptrdiff_t>(from),
                    audioData.samples.begin() + static_cast<ptrdiff_t>(to));
            }
        }
        cursor = regionEnd;
    }
    // Copy remaining samples from cursor to clip.end
    if (cursor < clip.end) {
        auto from = static_cast<size_t>(cursor * sr);
        auto to   = std::min(endSample, audioData.samples.size());
        from = std::min(from, audioData.samples.size());
        if (to > from) {
            m_playbackTimeMap.push_back({static_cast<int64_t>(m_playbackBuffer.size()), cursor});
            m_playbackBuffer.insert(m_playbackBuffer.end(),
                audioData.samples.begin() + static_cast<ptrdiff_t>(from),
                audioData.samples.begin() + static_cast<ptrdiff_t>(to));
        }
    }

    // ── Resample buffer to engine output rate if rates differ ─────────
    // The AudioEngine callback advances m_playPosition at the engine
    // sample rate and reads source samples by index. If the buffer is at
    // a different rate, playback speed and duration will be wrong.
    double engineRate = static_cast<double>(m_audioEngine->config().sampleRate);
    if (std::abs(sr - engineRate) > 1.0 && !m_playbackBuffer.empty()) {
        double ratio = engineRate / sr;
        auto newSize = static_cast<size_t>(
            static_cast<double>(m_playbackBuffer.size()) * ratio) + 1;
        std::vector<float> resampled(newSize);
        for (size_t j = 0; j < newSize; ++j) {
            double srcPos = static_cast<double>(j) / ratio;
            auto idx = static_cast<size_t>(srcPos);
            float frac = static_cast<float>(srcPos - static_cast<double>(idx));
            if (idx + 1 < m_playbackBuffer.size()) {
                resampled[j] = m_playbackBuffer[idx] * (1.0f - frac)
                             + m_playbackBuffer[idx + 1] * frac;
            } else if (idx < m_playbackBuffer.size()) {
                resampled[j] = m_playbackBuffer[idx];
            }
        }
        // Scale time-map offsets from source-rate frames to engine-rate frames
        for (auto& [offset, time] : m_playbackTimeMap) {
            offset = static_cast<int64_t>(static_cast<double>(offset) * ratio);
        }
        m_playbackBuffer = std::move(resampled);
        sr = engineRate;
    }
    m_playbackSourceRate = sr;  // store effective rate for timer / seek

    if (m_playbackBuffer.empty()) {
        spdlog::warn("AudioSync::playClip: empty playback buffer for clip {}", clipIdx);
        return;
    }

    AudioTrackSource src;
    src.trackId     = 9999;
    src.samples     = m_playbackBuffer.data();
    src.totalFrames = static_cast<int64_t>(m_playbackBuffer.size());
    src.startFrame  = 0;
    src.channels    = 1;
    src.sampleRate  = static_cast<uint32_t>(sr);
    src.volume      = 1.0f;
    src.pan         = 0.0f;
    src.muted       = false;
    src.solo        = false;

    // Detach the timeline's sync clock so play()/stop() won't
    // corrupt timeline position (same pattern as SourceMonitor).
    if (!m_savedSyncClock) {
        m_savedSyncClock = m_audioEngine->syncClock();
        m_audioEngine->setSyncClock(nullptr);
    }

    spdlog::info("AudioSync::playClip: setting sources, buffer.size={} totalFrames={}",
                 m_playbackBuffer.size(), src.totalFrames);
    m_audioEngine->setTrackSources({src});

    // Always seek to the correct starting frame before play().
    // pause() does NOT reset m_playPosition, so after stopPlayback()
    // the stale position from a previous clip could be past the end
    // of this buffer — producing silence for short clips.
    int64_t startAtFrame = 0;
    if (playFromTime > clip.start) {
        for (size_t i = 0; i < m_playbackTimeMap.size(); ++i) {
            auto [segStart, segTime] = m_playbackTimeMap[i];
            int64_t segEnd = (i + 1 < m_playbackTimeMap.size())
                ? m_playbackTimeMap[i + 1].first
                : static_cast<int64_t>(m_playbackBuffer.size());
            double segDuration = static_cast<double>(segEnd - segStart) / sr;
            if (playFromTime >= segTime && playFromTime < segTime + segDuration) {
                startAtFrame = segStart + static_cast<int64_t>((playFromTime - segTime) * sr);
                break;
            }
            if (playFromTime < segTime) {
                startAtFrame = segStart;
                break;
            }
            startAtFrame = segEnd;
        }
    }
    m_audioEngine->seekToFrame(startAtFrame);
    m_audioEngine->play();

    m_playingClipIdx = static_cast<int>(clipIdx);
    spdlog::info("AudioSync::playClip: started clip={} engineState={}",
                 clipIdx, static_cast<int>(m_audioEngine->transportState()));

    // Timer to update playhead on waveform widget
    if (!m_playheadTimer) {
        m_playheadTimer = new QTimer(this);
        m_playheadTimer->setInterval(30);
        connect(m_playheadTimer, &QTimer::timeout, this, [this]() {
            if (m_playingClipIdx < 0 || static_cast<size_t>(m_playingClipIdx) >= m_clips.size()) {
                spdlog::info("AudioSync timer: invalid playingClipIdx={} clips.size={}",
                             m_playingClipIdx, m_clips.size());
                stopPlayback();
                return;
            }
            if (!m_audioEngine) {
                spdlog::warn("AudioSync timer: engine null");
                stopPlayback();
                return;
            }
            auto frame = m_audioEngine->currentFrame();
            const auto& pClip = m_clips[static_cast<size_t>(m_playingClipIdx)];

            // Use the effective playback rate (engine rate after resampling)
            double sr = m_playbackSourceRate;

            // Convert buffer frame → original source time using the time map
            double currentTime = pClip.end;  // default: end
            for (size_t i = 0; i < m_playbackTimeMap.size(); ++i) {
                auto [segStart, segTime] = m_playbackTimeMap[i];
                int64_t segEnd = (i + 1 < m_playbackTimeMap.size())
                    ? m_playbackTimeMap[i + 1].first
                    : static_cast<int64_t>(m_playbackBuffer.size());
                if (frame >= segStart && frame < segEnd) {
                    currentTime = segTime + static_cast<double>(frame - segStart) / sr;
                    break;
                }
            }

            if (currentTime >= pClip.end || m_audioEngine->transportState() != TransportState::Playing) {
                // Set playhead to end position before stopping
                if (auto* wfEnd = waveformForClip(m_playingClipIdx)) {
                    wfEnd->setPlayhead(std::min(currentTime, pClip.end));
                    wfEnd->setPlayheadVisible(true);
                }
                stopPlayback();
                return;
            }

            // Find waveform widget via card mapping
            if (auto* wf = waveformForClip(m_playingClipIdx)) {
                wf->setPlayhead(currentTime);
                wf->setPlayheadVisible(true);
            }
            requestTakePreviewFrame(static_cast<size_t>(m_playingClipIdx), currentTime);

            // Update transport bar display
            updateTransportBar();
        });
    }
    m_playheadTimer->start();
}

bool AudioSync::loadPreviewAudioForTake(int clipId, double rangeStart, double rangeEnd)
{
    const SyncClip* clip = nullptr;
    for (const auto& c : m_clips) if (c.id == clipId) { clip = &c; break; }
    if (!clip) return false;
    auto it = m_audioSamples.find(clip->sourceFile);
    if (it == m_audioSamples.end() || it->second.samples.empty()) return false;
    char key[64];
    std::snprintf(key, sizeof(key), "|%.4f|%.4f", rangeStart, rangeEnd);
    return loadPreviewAudio(clip->sourceFile + key, it->second, rangeStart, rangeEnd);
}

bool AudioSync::loadPreviewAudioForMedia(const std::string& path)
{
    auto it = m_mediaAudioCache.find(path);
    if (it == m_mediaAudioCache.end()) {
        if (m_mediaAudioCache.size() >= 6) m_mediaAudioCache.clear();   // renders are short; keep a few
        AudioFile af;
        if (!af.open(path)) return false;
        const auto info = af.info();
        std::vector<float> all = af.readAll();
        if (all.empty() || info.channels == 0) return false;
        std::vector<float> mono(all.size() / info.channels);
        for (size_t i = 0; i < mono.size(); ++i) {
            float sum = 0.0f;
            for (uint32_t ch = 0; ch < info.channels; ++ch) sum += all[i * info.channels + ch];
            mono[i] = sum / static_cast<float>(info.channels);
        }
        AudioSampleData d;
        d.sampleRate = info.sampleRate;
        d.samples = SampleBuffer(std::move(mono));
        it = m_mediaAudioCache.emplace(path, std::move(d)).first;
    }
    const auto& data = it->second;
    return loadPreviewAudio(path + "|media", data, 0.0,
                            static_cast<double>(data.samples.size()) / std::max(1u, data.sampleRate));
}

bool AudioSync::loadPreviewAudio(const std::string& key, const AudioSampleData& data,
                                 double rangeStart, double rangeEnd)
{
    if (!m_audioEngine || data.samples.empty() || data.sampleRate == 0) return false;
    if (m_takeAudioActive && key == m_takeAudioKey && !m_takeAudioBuffer.empty()) return true;   // already loaded
    stopTakeAudio();
    stopPlayback();
    stopFilePlayback();

    const double srcRate = static_cast<double>(data.sampleRate);
    const double engineRate = static_cast<double>(m_audioEngine->config().sampleRate);
    const double from = std::max(0.0, rangeStart);
    const auto s0 = std::min(data.samples.size(), static_cast<size_t>(from * srcRate));
    const auto s1 = std::min(data.samples.size(), static_cast<size_t>(std::max(from, rangeEnd) * srcRate));
    if (s1 <= s0 + 1) return false;

    // Resample the range to the engine rate (linear, like playClip).
    const double ratio = engineRate / srcRate;
    const size_t n = static_cast<size_t>(static_cast<double>(s1 - s0) * ratio);
    m_takeAudioBuffer.assign(n, 0.0f);
    for (size_t j = 0; j < n; ++j) {
        const double pos = static_cast<double>(j) / ratio;
        const size_t i = s0 + static_cast<size_t>(pos);
        const float frac = static_cast<float>(pos - std::floor(pos));
        const float a = data.samples[std::min(i, s1 - 1)];
        const float b = data.samples[std::min(i + 1, s1 - 1)];
        m_takeAudioBuffer[j] = a + (b - a) * frac;
    }
    m_takeAudioKey = key;
    m_takeAudioFrom = static_cast<double>(s0) / srcRate;
    m_takeAudioEnd = static_cast<double>(s1) / srcRate;
    m_takeAudioRate = engineRate;
    // Same isolation as playClip: never move the timeline's clock.
    if (!m_savedSyncClock) {
        m_savedSyncClock = m_audioEngine->syncClock();
        m_audioEngine->setSyncClock(nullptr);
    }
    m_takeAudioActive = true;
    installPreviewAudio();
    return true;
}

void AudioSync::installPreviewAudio()
{
    if (!m_audioEngine || m_takeAudioBuffer.empty()) return;
    AudioTrackSource src;
    src.trackId     = 9999;
    src.samples     = m_takeAudioBuffer.data();
    src.totalFrames = static_cast<int64_t>(m_takeAudioBuffer.size());
    src.startFrame  = 0;
    src.channels    = 1;
    src.sampleRate  = static_cast<uint32_t>(m_takeAudioRate);
    src.volume      = 1.0f;
    // Re-installed on every use: the engine is shared across pages.
    m_audioEngine->setTrackSources({src});
}

bool AudioSync::playPreviewAudio(double fromSec, double speed)
{
    if (!m_takeAudioActive || !m_audioEngine || m_takeAudioBuffer.empty() || speed == 0.0) return false;
    installPreviewAudio();
    const auto last = static_cast<int64_t>(m_takeAudioBuffer.size()) - 1;
    const auto frame = std::clamp(static_cast<int64_t>((fromSec - m_takeAudioFrom) * m_takeAudioRate), int64_t(0), last);
    m_audioEngine->pause();
    m_audioEngine->setPlaybackSpeed(speed);   // time-stretched; negative = reverse
    m_audioEngine->seekToFrame(frame);
    m_audioEngine->play();
    return true;
}

void AudioSync::scrubPreviewAudio(double atSec)
{
    if (!m_takeAudioActive || !m_audioEngine || m_takeAudioBuffer.empty()) return;
    if (atSec < m_takeAudioFrom || atSec >= m_takeAudioEnd) return;
    installPreviewAudio();
    m_audioEngine->setPlaybackSpeed(1.0);
    const auto frame = static_cast<int64_t>((atSec - m_takeAudioFrom) * m_takeAudioRate);
    // ~1 video frame of sound, like Premiere's scrub.
    m_audioEngine->scrub(frame, static_cast<int64_t>(m_takeAudioRate / 24.0));
}

void AudioSync::pausePreviewAudio()
{
    if (!m_takeAudioActive || !m_audioEngine) return;
    m_audioEngine->pause();
    m_audioEngine->setPlaybackSpeed(1.0);
}

void AudioSync::stopTakeAudio()
{
    if (!m_takeAudioActive) return;
    m_takeAudioActive = false;
    m_takeAudioKey.clear();
    if (m_audioEngine) {
        m_audioEngine->pause();
        m_audioEngine->setPlaybackSpeed(1.0);
        m_audioEngine->clearTrackSources();   // before the buffer goes away
        if (m_savedSyncClock) {
            m_audioEngine->setSyncClock(m_savedSyncClock);
            m_savedSyncClock = nullptr;
        }
    }
    m_takeAudioBuffer.clear();
}

double AudioSync::takeAudioTime() const
{
    if (!m_takeAudioActive || !m_audioEngine) return -1.0;
    if (m_audioEngine->transportState() != TransportState::Playing) return -1.0;
    return m_takeAudioFrom + static_cast<double>(m_audioEngine->currentFrame()) / m_takeAudioRate;
}

void AudioSync::stopPlayback()
{
    spdlog::info("AudioSync::stopPlayback: playingClip={}", m_playingClipIdx);
    // Any other playback supersedes a take preview (its sources are cleared below).
    m_takeAudioActive = false;

    if (m_audioEngine) {
        m_audioEngine->pause();
        m_audioEngine->setPlaybackSpeed(1.0);
        m_audioEngine->clearTrackSources();
        m_audioEngine->resetStretchers();

        // Restore the timeline's sync clock
        if (m_savedSyncClock) {
            m_audioEngine->setSyncClock(m_savedSyncClock);
            m_savedSyncClock = nullptr;
        }
    }

    if (m_playheadTimer)
        m_playheadTimer->stop();

    // Keep playhead visible at last position (don't hide it)
    m_playingClipIdx = -1;
    m_voiceDraftAuditionActive = false;
    m_voiceDraftSamples.clear();
    updateTransportBar();
}

void AudioSync::togglePlayClip(size_t clipIdx)
{
    if (m_playingClipIdx == static_cast<int>(clipIdx) &&
        m_audioEngine && m_audioEngine->transportState() == TransportState::Playing) {
        pausePlayback();
    } else if (m_playingClipIdx == static_cast<int>(clipIdx) &&
               m_audioEngine && m_audioEngine->transportState() == TransportState::Paused) {
        // Resume from paused
        m_audioEngine->play();
        if (m_playheadTimer) m_playheadTimer->start();
    } else {
        playClip(clipIdx);
    }
}

void AudioSync::pausePlayback()
{
    if (m_audioEngine)
        m_audioEngine->pause();

    if (m_playheadTimer)
        m_playheadTimer->stop();
}

void AudioSync::seekPlayingClip(double timeSec)
{
    if (m_playingClipIdx < 0 || static_cast<size_t>(m_playingClipIdx) >= m_clips.size())
        return;

    const auto& clip = m_clips[static_cast<size_t>(m_playingClipIdx)];

    // Use the effective playback rate (engine rate after resampling)
    double sr = m_playbackSourceRate;

    // If timeSec falls inside a deleted region, snap to end of that region
    for (const auto& [delStart, delEnd] : clip.deletedRegions) {
        if (timeSec >= delStart && timeSec < delEnd) {
            timeSec = delEnd;
            break;
        }
    }

    // Find the buffer frame that corresponds to timeSec using the time map
    int64_t targetFrame = 0;
    for (size_t i = 0; i < m_playbackTimeMap.size(); ++i) {
        auto [segStart, segTime] = m_playbackTimeMap[i];
        int64_t segEnd = (i + 1 < m_playbackTimeMap.size())
            ? m_playbackTimeMap[i + 1].first
            : static_cast<int64_t>(m_playbackBuffer.size());
        double segDuration = static_cast<double>(segEnd - segStart) / sr;
        if (timeSec >= segTime && timeSec < segTime + segDuration) {
            targetFrame = segStart + static_cast<int64_t>((timeSec - segTime) * sr);
            break;
        }
        if (timeSec < segTime) {
            targetFrame = segStart;
            break;
        }
        targetFrame = segEnd;
    }

    m_audioEngine->seekToFrame(targetFrame);

    // Update waveform playhead
    if (auto* wf = waveformForClip(m_playingClipIdx)) {
        wf->setPlayhead(timeSec);
        wf->setPlayheadVisible(true);
    }
    requestTakePreviewFrame(static_cast<size_t>(m_playingClipIdx), timeSec);
}

// ═══════════════════════════════════════════════════════════════════════════
//  Scrub audio at a position (for non-playing clips)
// ═══════════════════════════════════════════════════════════════════════════

void AudioSync::scrubClipAt(size_t clipIdx, double timeSec)
{
    if (!m_audioEngine || clipIdx >= m_clips.size())
        return;

    requestTakePreviewFrame(clipIdx, timeSec);

    const auto& clip = m_clips[clipIdx];
    auto it = m_audioSamples.find(clip.sourceFile);
    if (it == m_audioSamples.end())
        return;

    const auto& audioData = it->second;
    if (audioData.samples.empty())
        return;

    // Detach sync clock temporarily for scrub
    AVSyncClock* savedClock = m_audioEngine->syncClock();
    m_audioEngine->setSyncClock(nullptr);

    // Set up a temporary source for scrub
    AudioTrackSource src;
    src.trackId     = 9999;
    src.samples     = audioData.samples.data();
    src.totalFrames = static_cast<int64_t>(audioData.samples.size());
    src.startFrame  = 0;
    src.channels    = 1;
    src.sampleRate  = audioData.sampleRate;
    src.volume      = 1.0f;
    src.pan         = 0.0f;
    src.muted       = false;
    src.solo        = false;

    m_audioEngine->setTrackSources({src});

    int64_t frame = static_cast<int64_t>(timeSec * audioData.sampleRate);
    m_audioEngine->scrub(frame, 2048);

    // Restore sync clock
    m_audioEngine->setSyncClock(savedClock);
}

// ═══════════════════════════════════════════════════════════════════════════
//  Auto-trim silence
// ═══════════════════════════════════════════════════════════════════════════

void AudioSync::autoTrimClip(size_t clipIdx)
{
    if (clipIdx >= m_clips.size()) return;
    if (!audioReadyOrNotify(m_clips[clipIdx].sourceFile, QStringLiteral("Auto-trim"))) return;

    auto& clip = m_clips[clipIdx];
    auto it = m_audioSamples.find(clip.sourceFile);
    if (it == m_audioSamples.end()) return;

    const auto& audioData = it->second;
    const int windowSize  = static_cast<int>(0.020 * audioData.sampleRate); // 20ms

    auto startSample = static_cast<int>(clip.start * audioData.sampleRate);
    auto endSample   = static_cast<int>(clip.end * audioData.sampleRate);
    if (startSample >= static_cast<int>(audioData.samples.size())) return;
    if (endSample > static_cast<int>(audioData.samples.size())) endSample = static_cast<int>(audioData.samples.size());
    if (endSample - startSample < windowSize * 3) return;

    // Measure all window RMS values to compute adaptive noise floor
    std::vector<float> rmsValues;
    for (int pos = startSample; pos + windowSize <= endSample; pos += windowSize) {
        float sumSq = 0.0f;
        for (int j = 0; j < windowSize; ++j) {
            float s = audioData.samples[static_cast<size_t>(pos + j)];
            sumSq += s * s;
        }
        rmsValues.push_back(std::sqrt(sumSq / static_cast<float>(windowSize)));
    }
    if (rmsValues.size() < 3) return;

    // Adaptive threshold: sort RMS values, take the 10th percentile as noise floor,
    // then set threshold at 3x that level (min 0.005)
    auto sorted = rmsValues;
    std::sort(sorted.begin(), sorted.end());
    float noiseFloor = sorted[sorted.size() / 10];
    float threshold = std::max(0.005f, noiseFloor * 3.0f);

    // Scan forward — require 2 consecutive windows above threshold
    int speechStart = startSample;
    int aboveCount = 0;
    for (int pos = startSample; pos + windowSize <= endSample; pos += windowSize) {
        float sumSq = 0.0f;
        for (int j = 0; j < windowSize; ++j) {
            float s = audioData.samples[static_cast<size_t>(pos + j)];
            sumSq += s * s;
        }
        float rms = std::sqrt(sumSq / static_cast<float>(windowSize));
        if (rms > threshold) {
            ++aboveCount;
            if (aboveCount >= 2) {
                speechStart = pos - windowSize;
                break;
            }
        } else {
            aboveCount = 0;
        }
    }

    // Scan backward — full windows only (no partial window RMS)
    int speechEnd = endSample;
    aboveCount = 0;
    int backStart = endSample - windowSize;
    backStart = backStart - (backStart % windowSize) + (startSample % windowSize);
    for (int pos = backStart; pos >= startSample; pos -= windowSize) {
        if (pos + windowSize > endSample) continue;
        float sumSq = 0.0f;
        for (int j = 0; j < windowSize; ++j) {
            float s = audioData.samples[static_cast<size_t>(pos + j)];
            sumSq += s * s;
        }
        float rms = std::sqrt(sumSq / static_cast<float>(windowSize));
        if (rms > threshold) {
            ++aboveCount;
            if (aboveCount >= 2) {
                speechEnd = pos + windowSize + windowSize;
                break;
            }
        } else {
            aboveCount = 0;
        }
    }

    // 120ms pre-padding (breath/plosive room), 150ms post-padding
    int prepad  = static_cast<int>(0.120 * audioData.sampleRate);
    int postpad = static_cast<int>(0.150 * audioData.sampleRate);
    speechStart = std::max(startSample, speechStart - prepad);
    speechEnd   = std::min(endSample, speechEnd + postpad);

    double newStart = static_cast<double>(speechStart) / audioData.sampleRate;
    double newEnd   = static_cast<double>(speechEnd) / audioData.sampleRate;

    // Only apply if change is meaningful (>20ms)
    if (std::abs(newStart - clip.start) > 0.02 || std::abs(newEnd - clip.end) > 0.02) {
        runClipsMutationWithUndo(
            "Auto-trim audio clip",
            [this, clipIdx, newStart, newEnd]() {
                if (clipIdx >= m_clips.size()) return;
                m_clips[clipIdx].start = newStart;
                m_clips[clipIdx].end   = newEnd;
            },
            [this]() { populateClipList(); });
        spdlog::info("AudioSync: Auto-trimmed clip {} to {:.3f}-{:.3f}", clipIdx, newStart, newEnd);
    }
}

// ════════════════════════════════════════════════════════════════════════════
//  File preview playback (Import / Transcribe lists)
// ════════════════════════════════════════════════════════════════════════════

void AudioSync::playAudioFile(size_t fileIndex)
{
    if (!m_audioEngine || fileIndex >= m_audioPaths.size()) return;

    // If already playing this file, toggle off
    if (m_previewFileIdx == static_cast<int>(fileIndex)) {
        stopFilePlayback();
        return;
    }

    // Stop any clip playback first
    stopPlayback();
    stopFilePlayback();

    const auto& path = m_audioPaths[fileIndex];
    auto it = m_audioSamples.find(path);
    if (it == m_audioSamples.end()) return;

    const auto& audioData = it->second;
    if (audioData.samples.empty()) return;

    m_previewFileIdx = static_cast<int>(fileIndex);

    // Determine start position from waveform playhead (if user clicked to seek)
    int64_t seekFrame = 0;
    for (auto* map : {&m_fileWaveforms, &m_transcribeWaveforms}) {
        auto wfIt = map->find(fileIndex);
        if (wfIt != map->end() && wfIt->second && wfIt->second->isPlayheadVisible()) {
            double ph = wfIt->second->playhead();
            seekFrame = static_cast<int64_t>(ph * audioData.sampleRate);
            seekFrame = std::clamp(seekFrame, int64_t{0},
                                   static_cast<int64_t>(audioData.samples.size()) - 1);
            break;
        }
    }

    // Build a simple track source — startFrame=0 means file starts at timeline origin
    AudioTrackSource src{};
    src.trackId     = 9998;
    src.samples     = audioData.samples.data();
    src.totalFrames = static_cast<int64_t>(audioData.samples.size());
    src.startFrame  = 0;
    src.channels    = 1;
    src.sampleRate  = audioData.sampleRate;
    src.volume      = 1.0f;

    // Detach timeline sync clock during preview
    m_savedSyncClock = nullptr;
    if (m_audioEngine->syncClock()) {
        m_savedSyncClock = m_audioEngine->syncClock();
        m_audioEngine->setSyncClock(nullptr);
    }

    m_audioEngine->setPlaybackSpeed(1.0);
    m_audioEngine->setTrackSources({src});
    m_audioEngine->seekToFrame(seekFrame);
    m_audioEngine->play();

    // Update play button appearance
    updateFilePlayButton(fileIndex, true);

    // Timer to update waveform playhead and detect end
    if (!m_previewTimer) {
        m_previewTimer = new QTimer(this);
        m_previewTimer->setInterval(30);
        connect(m_previewTimer, &QTimer::timeout, this, [this]() {
            if (!m_audioEngine || m_previewFileIdx < 0) {
                stopFilePlayback();
                return;
            }
            auto fidx = static_cast<size_t>(m_previewFileIdx);
            if (fidx >= m_audioPaths.size()) { stopFilePlayback(); return; }

            auto sampIt = m_audioSamples.find(m_audioPaths[fidx]);
            if (sampIt == m_audioSamples.end()) { stopFilePlayback(); return; }

            double sr = sampIt->second.sampleRate;
            int64_t frame = m_audioEngine->currentFrame();
            double currentTime = static_cast<double>(frame) / sr;
            double duration = static_cast<double>(sampIt->second.samples.size()) / sr;

            if (currentTime >= duration ||
                m_audioEngine->transportState() != TransportState::Playing) {
                // Park playhead at final position
                double t = std::min(currentTime, duration);
                for (auto* map : {&m_fileWaveforms, &m_transcribeWaveforms}) {
                    auto it2 = map->find(fidx);
                    if (it2 != map->end() && it2->second) {
                        it2->second->setPlayhead(t);
                        it2->second->setPlayheadVisible(true);
                    }
                }
                stopFilePlayback();
                return;
            }

            // Update waveform playhead (both IMPORT and TRANSCRIBE lists)
            for (auto* map : {&m_fileWaveforms, &m_transcribeWaveforms}) {
                auto it2 = map->find(fidx);
                if (it2 != map->end() && it2->second) {
                    it2->second->setPlayhead(currentTime);
                    it2->second->setPlayheadVisible(true);
                }
            }

            // Update time label
            updateFileTimeLabel(fidx, currentTime);
        });
    }
    m_previewTimer->start();
}

void AudioSync::stopFilePlayback()
{
    if (m_previewTimer)
        m_previewTimer->stop();

    int prevIdx = m_previewFileIdx;

    if (m_previewFileIdx >= 0 && m_audioEngine) {
        m_audioEngine->pause();
        m_audioEngine->clearTrackSources();
        m_audioEngine->setPlaybackSpeed(1.0);
        if (m_savedSyncClock) {
            m_audioEngine->setSyncClock(m_savedSyncClock);
            m_savedSyncClock = nullptr;
        }
    }

    m_previewFileIdx = -1;

    // Reset play button
    if (prevIdx >= 0)
        updateFilePlayButton(static_cast<size_t>(prevIdx), false);
}

void AudioSync::updateFilePlayButton(size_t fileIndex, bool playing)
{
    for (auto* map : {&m_filePlayBtns, &m_transcribePlayBtns}) {
        auto it = map->find(fileIndex);
        if (it != map->end() && it->second)
            it->second->setText(playing ? QStringLiteral("\u23F8") : QStringLiteral("\u25B6"));
    }
}

void AudioSync::updateFileTimeLabel(size_t fileIndex, double timeSec)
{
    for (auto* map : {&m_fileTimeLabels, &m_transcribeTimeLabels}) {
        auto it = map->find(fileIndex);
        if (it != map->end() && it->second) {
            int mins = static_cast<int>(timeSec) / 60;
            int secs = static_cast<int>(timeSec) % 60;
            int cs   = static_cast<int>(timeSec * 100) % 100;
            it->second->setText(QString("%1:%2.%3")
                .arg(mins).arg(secs, 2, 10, QChar('0')).arg(cs, 2, 10, QChar('0')));
        }
    }
}

} // namespace rt
