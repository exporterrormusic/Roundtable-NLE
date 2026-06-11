/*
 * AudioSyncPlayback.cpp - Audio sample loading and playback for AudioSync.
 * Split from AudioSync.cpp for maintainability.
 */

#include "panels/audio/AudioSync.h"

#include "audio/AudioEngine.h"
#include "audio/AudioFile.h"
#include "widgets/MiniWaveformWidget.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTimer>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <vector>
#include <cmath>

namespace rt {

// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
//  Audio sample loading
// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•

// â”€â”€â”€ Waveform sample cache (binary) â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
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

static bool readAudioCache(const std::string& sourcePath,
                           std::vector<float>& outSamples,
                           uint32_t& outSampleRate)
{
    QFileInfo srcInfo(QString::fromStdString(sourcePath));
    if (!srcInfo.exists()) return false;

    QString cachePath = audioCacheDir() + "/" + audioCacheKey(sourcePath) + ".pcm";
    QFile cacheFile(cachePath);
    if (!cacheFile.open(QIODevice::ReadOnly))
        return false;

    AudioCacheHeader hdr{};
    if (cacheFile.read(reinterpret_cast<char*>(&hdr), sizeof(hdr)) != sizeof(hdr))
        return false;
    if (hdr.magic != 0x52544157 || hdr.version != 1)
        return false;

    // Validate source hasn't changed
    if (hdr.sourceSize != srcInfo.size())
        return false;
    if (hdr.sourceModTime != srcInfo.lastModified().toMSecsSinceEpoch())
        return false;

    if (hdr.numSamples == 0) return false;

    outSamples.resize(static_cast<size_t>(hdr.numSamples));
    qint64 dataBytes = static_cast<qint64>(hdr.numSamples * sizeof(float));
    if (cacheFile.read(reinterpret_cast<char*>(outSamples.data()), dataBytes) != dataBytes)
        return false;

    outSampleRate = hdr.sampleRate;
    return true;
}

static void writeAudioCache(const std::string& sourcePath,
                             const std::vector<float>& samples,
                             uint32_t sampleRate)
{
    QFileInfo srcInfo(QString::fromStdString(sourcePath));
    if (!srcInfo.exists() || samples.empty()) return;

    QString cachePath = audioCacheDir() + "/" + audioCacheKey(sourcePath) + ".pcm";
    QFile cacheFile(cachePath);
    if (!cacheFile.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return;

    AudioCacheHeader hdr{};
    hdr.sampleRate    = sampleRate;
    hdr.numSamples    = samples.size();
    hdr.sourceSize    = srcInfo.size();
    hdr.sourceModTime = srcInfo.lastModified().toMSecsSinceEpoch();

    cacheFile.write(reinterpret_cast<const char*>(&hdr), sizeof(hdr));
    cacheFile.write(reinterpret_cast<const char*>(samples.data()),
                    static_cast<qint64>(samples.size() * sizeof(float)));
}

void AudioSync::loadAudioSamples()
{
    for (const auto& path : m_audioPaths) {
        if (m_audioSamples.count(path))
            continue; // already loaded

        // Try cached mono samples first (skips expensive audio decoding)
        {
            std::vector<float> cached;
            uint32_t cachedRate = 0;
            if (readAudioCache(path, cached, cachedRate)) {
                AudioSampleData asd;
                auto sampleCount = cached.size();
                asd.sampleRate = cachedRate;
                asd.samples    = std::move(cached);
                m_audioSamples[path] = std::move(asd);
                spdlog::info("AudioSync: Loaded {} samples ({}Hz) from cache for {}",
                             sampleCount, static_cast<int>(cachedRate), path);
                continue;
            }
        }

        AudioFile af;
        if (!af.open(path)) {
            spdlog::warn("AudioSync: Failed to open audio file for waveform: {}", path);
            continue;
        }

        auto info = af.info();
        auto allSamples = af.readAll();
        if (allSamples.empty()) {
            spdlog::warn("AudioSync: Empty audio data from: {}", path);
            continue;
        }

        // Mix to mono for waveform display
        std::vector<float> mono;
        if (info.channels == 1) {
            mono = std::move(allSamples);
        } else {
            mono.resize(allSamples.size() / info.channels);
            for (size_t i = 0; i < mono.size(); ++i) {
                float sum = 0.0f;
                for (uint32_t ch = 0; ch < info.channels; ++ch)
                    sum += allSamples[i * info.channels + ch];
                mono[i] = sum / static_cast<float>(info.channels);
            }
        }

        // Cache the decoded mono samples for fast reload
        writeAudioCache(path, mono, info.sampleRate);

        AudioSampleData asd;
        auto sampleCount = mono.size();
        auto sr = info.sampleRate;
        asd.samples    = std::move(mono);
        asd.sampleRate = sr;
        m_audioSamples[path] = std::move(asd);

        spdlog::info("AudioSync: Loaded {} samples ({}Hz) from {}",
                     sampleCount, static_cast<int>(sr), path);
    }
}

// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
//  Playback
// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•

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
    m_playbackTimeMap.clear();  // maps buffer frame offset â†’ original time
    double sr = audioData.sampleRate;

    double cursor = clip.start;
    for (const auto& [delStart, delEnd] : sortedDeleted) {
        if (delStart >= clip.end) break;
        double regionStart = std::max(delStart, clip.start);
        double regionEnd   = std::min(delEnd, clip.end);
        if (regionStart <= cursor && regionEnd > cursor) {
            // Cursor is inside this deleted region â€” skip ahead
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

    // â”€â”€ Resample buffer to engine output rate if rates differ â”€â”€â”€â”€â”€â”€â”€â”€â”€
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

            // Convert buffer frame â†’ original source time using the time map
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

            // Update transport bar display
            updateTransportBar();
        });
    }
    m_playheadTimer->start();
}

void AudioSync::stopPlayback()
{
    spdlog::info("AudioSync::stopPlayback: playingClip={}", m_playingClipIdx);

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
}

// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
//  Scrub audio at a position (for non-playing clips)
// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•

void AudioSync::scrubClipAt(size_t clipIdx, double timeSec)
{
    if (!m_audioEngine || clipIdx >= m_clips.size())
        return;

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

// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
//  Auto-trim silence
// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•

void AudioSync::autoTrimClip(size_t clipIdx)
{
    if (clipIdx >= m_clips.size()) return;

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

    // Scan forward â€” require 2 consecutive windows above threshold
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

    // Scan backward â€” full windows only (no partial window RMS)
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