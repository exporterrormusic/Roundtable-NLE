/*
 * AudioSyncPlayback.cpp - Audio samples and playback for AudioSync (the
 * background decode itself is AudioSampleLoader).
 * Split from AudioSync.cpp for maintainability.
 */

#include "panels/audio/AudioSync.h"
#include "panels/audio/VideoFrameGrabber.h"

#include "audio/AudioEngine.h"
#include "audio/AudioFile.h"
#include "widgets/MiniWaveformWidget.h"
#include "NotificationCenter.h"

#include <QTimer>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>
#include <vector>

namespace rt {

// ═══════════════════════════════════════════════════════════════════════════
//  Audio samples (decoded by AudioSampleLoader)
// ═══════════════════════════════════════════════════════════════════════════

void AudioSync::loadAudioSamples()
{
    m_sampleLoader->request(session().audioPaths);
}

bool AudioSync::audioSamplesPending() const noexcept
{
    return m_sampleLoader->pending();
}

void AudioSync::cancelAudioSampleDecodes()
{
    m_sampleLoader->cancel();
}

void AudioSync::installAudioSamples(const std::string& path, AudioSampleData decoded)
{
    if (decoded.samples.empty() || session().audioSamples.count(path)) return;
    const AudioSampleData& stored = session().audioSamples[path] = std::move(decoded);

    // Cards built before the samples arrived show an empty waveform.
    for (size_t i = 0; i < m_cardWaveforms.size() && i < m_cardClipIndices.size(); ++i) {
        const int ci = m_cardClipIndices[i];
        MiniWaveformWidget* wf = m_cardWaveforms[i];
        if (!wf || ci < 0 || static_cast<size_t>(ci) >= session().clips.size()) continue;
        const SyncClip& clip = session().clips[static_cast<size_t>(ci)];
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
    m_sampleLoader->loadNow(session().audioPaths);
}

void AudioSync::whenAudioSamplesReady(const QString& action, std::function<void()> fn)
{
    m_sampleLoader->whenReady(session().audioPaths, action, std::move(fn));
}

bool AudioSync::audioReadyOrNotify(const std::string& path, const QString& action)
{
    const bool pending = path.empty() ? m_sampleLoader->pending() : m_sampleLoader->pending(path);
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
    if (!m_audioEngine || clipIdx >= session().clips.size())
        return;

    spdlog::info("AudioSync::playClip: clip={} clips.size={}", clipIdx, session().clips.size());

    stopPlayback();

    const auto& clip = session().clips[clipIdx];
    auto it = session().audioSamples.find(clip.sourceFile);
    if (it == session().audioSamples.end()) {
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
            if (m_playingClipIdx < 0 || static_cast<size_t>(m_playingClipIdx) >= session().clips.size()) {
                spdlog::info("AudioSync timer: invalid playingClipIdx={} clips.size={}",
                             m_playingClipIdx, session().clips.size());
                stopPlayback();
                return;
            }
            if (!m_audioEngine) {
                spdlog::warn("AudioSync timer: engine null");
                stopPlayback();
                return;
            }
            auto frame = m_audioEngine->currentFrame();
            const auto& pClip = session().clips[static_cast<size_t>(m_playingClipIdx)];

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
    for (const auto& c : session().clips) if (c.id == clipId) { clip = &c; break; }
    if (!clip) return false;
    auto it = session().audioSamples.find(clip->sourceFile);
    if (it == session().audioSamples.end() || it->second.samples.empty()) return false;
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
    if (m_playingClipIdx < 0 || static_cast<size_t>(m_playingClipIdx) >= session().clips.size())
        return;

    const auto& clip = session().clips[static_cast<size_t>(m_playingClipIdx)];

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
    if (!m_audioEngine || clipIdx >= session().clips.size())
        return;

    requestTakePreviewFrame(clipIdx, timeSec);

    const auto& clip = session().clips[clipIdx];
    auto it = session().audioSamples.find(clip.sourceFile);
    if (it == session().audioSamples.end())
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
    if (clipIdx >= session().clips.size()) return;
    if (!audioReadyOrNotify(session().clips[clipIdx].sourceFile, QStringLiteral("Auto-trim"))) return;

    auto& clip = session().clips[clipIdx];
    auto it = session().audioSamples.find(clip.sourceFile);
    if (it == session().audioSamples.end()) return;

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
                if (clipIdx >= session().clips.size()) return;
                session().clips[clipIdx].start = newStart;
                session().clips[clipIdx].end   = newEnd;
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
    if (!m_audioEngine || fileIndex >= session().audioPaths.size()) return;

    // If already playing this file, toggle off
    if (m_previewFileIdx == static_cast<int>(fileIndex)) {
        stopFilePlayback();
        return;
    }

    // Stop any clip playback first
    stopPlayback();
    stopFilePlayback();

    const auto& path = session().audioPaths[fileIndex];
    auto it = session().audioSamples.find(path);
    if (it == session().audioSamples.end()) return;

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
            if (fidx >= session().audioPaths.size()) { stopFilePlayback(); return; }

            auto sampIt = session().audioSamples.find(session().audioPaths[fidx]);
            if (sampIt == session().audioSamples.end()) { stopFilePlayback(); return; }

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
