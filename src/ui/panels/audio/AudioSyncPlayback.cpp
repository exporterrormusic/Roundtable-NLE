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

    // Determine playback start: use waveform playhead if available, else trimIn
    double playFromTime = clip.start;
    MiniWaveformWidget* wf = waveformForClip(static_cast<int>(clipIdx));
    if (wf && wf->isPlayheadVisible()) {
        double ph = wf->playhead();
        if (ph >= clip.start && ph < clip.end)
            playFromTime = ph;
    }

    // Trimmed, deleted regions cut out, at the engine rate.
    if (!m_player.playClip(it->second, clip, playFromTime))
        return;

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
            const auto& pClip = session().clips[static_cast<size_t>(m_playingClipIdx)];

            // The engine position as original source time (the clip's end past it)
            double currentTime = m_player.clipTime(pClip.end);

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
    const AudioSampleData* data = m_player.mediaAudio(path);
    if (!data) return false;
    return loadPreviewAudio(path + "|media", *data, 0.0,
                            static_cast<double>(data->samples.size()) / std::max(1u, data->sampleRate));
}

bool AudioSync::loadPreviewAudio(const std::string& key, const AudioSampleData& data,
                                 double rangeStart, double rangeEnd)
{
    if (!m_audioEngine || data.samples.empty() || data.sampleRate == 0) return false;
    if (m_player.hasPreview(key)) return true;   // already loaded
    stopTakeAudio();
    stopPlayback();
    stopFilePlayback();
    return m_player.loadPreview(key, data, rangeStart, rangeEnd);
}

bool AudioSync::playPreviewAudio(double fromSec, double speed)
{
    return m_player.playPreview(fromSec, speed);
}

void AudioSync::scrubPreviewAudio(double atSec)
{
    m_player.scrubPreview(atSec);
}

void AudioSync::pausePreviewAudio()
{
    m_player.pausePreview();
}

void AudioSync::stopTakeAudio()
{
    m_player.stopPreview();
}

double AudioSync::takeAudioTime() const
{
    return m_player.previewTime();
}

void AudioSync::stopPlayback()
{
    spdlog::info("AudioSync::stopPlayback: playingClip={}", m_playingClipIdx);
    // Also ends a take preview or a TTS draft audition.
    m_player.stop();

    if (m_playheadTimer)
        m_playheadTimer->stop();

    // Keep playhead visible at last position (don't hide it)
    m_playingClipIdx = -1;
    updateTransportBar();
}

void AudioSync::togglePlayClip(size_t clipIdx)
{
    if (m_playingClipIdx == static_cast<int>(clipIdx) && m_player.isPlaying()) {
        pausePlayback();
    } else if (m_playingClipIdx == static_cast<int>(clipIdx) && m_player.isPaused()) {
        // Resume from paused
        m_player.resume();
        if (m_playheadTimer) m_playheadTimer->start();
    } else {
        playClip(clipIdx);
    }
}

void AudioSync::pausePlayback()
{
    m_player.pause();

    if (m_playheadTimer)
        m_playheadTimer->stop();
}

void AudioSync::seekPlayingClip(double timeSec)
{
    if (m_playingClipIdx < 0 || static_cast<size_t>(m_playingClipIdx) >= session().clips.size())
        return;

    const auto& clip = session().clips[static_cast<size_t>(m_playingClipIdx)];
    // A time inside a deleted region seeks to the region's end.
    timeSec = m_player.seekClip(clip, timeSec);

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
    m_player.scrub(it->second, timeSec);
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

    m_player.playFile(audioData, seekFrame);

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

    if (m_previewFileIdx >= 0)
        m_player.stopFile();

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
