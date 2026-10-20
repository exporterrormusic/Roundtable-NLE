/*
 * AudioSyncPlayer.cpp — see AudioSyncPlayer.h. The engine calls moved here
 * from AudioSyncPlayback.cpp and AudioSyncVoice.cpp unchanged.
 */

#include "panels/audio/AudioSyncPlayer.h"

#include "audio/AudioEngine.h"
#include "audio/AudioFile.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <utility>

namespace rt {

bool AudioSyncPlayer::isPlaying() const
{
    return m_engine && m_engine->transportState() == TransportState::Playing;
}

bool AudioSyncPlayer::isPaused() const
{
    return m_engine && m_engine->transportState() == TransportState::Paused;
}

void AudioSyncPlayer::detachSyncClock()
{
    if (!m_savedSyncClock) {
        m_savedSyncClock = m_engine->syncClock();
        m_engine->setSyncClock(nullptr);
    }
}

void AudioSyncPlayer::restoreSyncClock()
{
    if (m_savedSyncClock) {
        m_engine->setSyncClock(m_savedSyncClock);
        m_savedSyncClock = nullptr;
    }
}

void AudioSyncPlayer::stop()
{
    // Any other playback supersedes a take preview (its sources are cleared below).
    m_previewActive = false;

    if (m_engine) {
        m_engine->pause();
        m_engine->setPlaybackSpeed(1.0);
        m_engine->clearTrackSources();
        m_engine->resetStretchers();

        // Restore the timeline's sync clock
        restoreSyncClock();
    }
    m_draftActive = false;
    m_draftSamples.clear();
}

void AudioSyncPlayer::pause()
{
    if (m_engine)
        m_engine->pause();
}

void AudioSyncPlayer::resume()
{
    if (m_engine)
        m_engine->play();
}

// ═══════════════════════════════════════════════════════════════════════════
//  Sync clips
// ═══════════════════════════════════════════════════════════════════════════

bool AudioSyncPlayer::playClip(const AudioSampleData& audio, const SyncClip& clip,
                               double playFromTime)
{
    if (!m_engine) return false;
    // The caller stopped the previous playback: the engine no longer reads
    // the buffer replaced here.
    m_clip = audiosync::buildClipPlayback(audio, clip.start, clip.end, clip.deletedRegions,
                                          static_cast<double>(m_engine->config().sampleRate));
    if (m_clip.samples.empty()) {
        spdlog::warn("AudioSync::playClip: empty playback buffer for clip {}", clip.id);
        return false;
    }

    AudioTrackSource src;
    src.trackId     = 9999;
    src.samples     = m_clip.samples.data();
    src.totalFrames = static_cast<int64_t>(m_clip.samples.size());
    src.startFrame  = 0;
    src.channels    = 1;
    src.sampleRate  = static_cast<uint32_t>(m_clip.rate);
    src.volume      = 1.0f;
    src.pan         = 0.0f;
    src.muted       = false;
    src.solo        = false;

    // Detach the timeline's sync clock so play()/stop() won't
    // corrupt timeline position (same pattern as SourceMonitor).
    detachSyncClock();

    spdlog::info("AudioSync::playClip: setting sources, buffer.size={} totalFrames={}",
                 m_clip.samples.size(), src.totalFrames);
    m_engine->setTrackSources({src});

    // Always seek to the correct starting frame before play().
    // pause() does NOT reset the play position, so after a stop
    // the stale position from a previous clip could be past the end
    // of this buffer — producing silence for short clips.
    const int64_t startAtFrame = playFromTime > clip.start ? m_clip.frameAt(playFromTime) : 0;
    m_engine->seekToFrame(startAtFrame);
    m_engine->play();
    return true;
}

double AudioSyncPlayer::clipTime(double fallback) const
{
    if (!m_engine) return fallback;
    return m_clip.sourceTimeAt(m_engine->currentFrame(), fallback);
}

double AudioSyncPlayer::seekClip(const SyncClip& clip, double timeSec)
{
    // If timeSec falls inside a deleted region, snap to end of that region
    for (const auto& [delStart, delEnd] : clip.deletedRegions) {
        if (timeSec >= delStart && timeSec < delEnd) {
            timeSec = delEnd;
            break;
        }
    }
    if (m_engine)
        m_engine->seekToFrame(m_clip.frameAt(timeSec));
    return timeSec;
}

void AudioSyncPlayer::scrub(const AudioSampleData& audioData, double timeSec)
{
    if (!m_engine || audioData.samples.empty())
        return;

    // Detach sync clock temporarily for scrub
    AVSyncClock* savedClock = m_engine->syncClock();
    m_engine->setSyncClock(nullptr);

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

    m_engine->setTrackSources({src});

    int64_t frame = static_cast<int64_t>(timeSec * audioData.sampleRate);
    m_engine->scrub(frame, 2048);

    // Restore sync clock
    m_engine->setSyncClock(savedClock);
}

// ═══════════════════════════════════════════════════════════════════════════
//  Take / media preview
// ═══════════════════════════════════════════════════════════════════════════

bool AudioSyncPlayer::hasPreview(const std::string& key) const
{
    return m_previewActive && key == m_previewKey && !m_previewBuffer.empty();
}

const AudioSampleData* AudioSyncPlayer::mediaAudio(const std::string& path)
{
    auto it = m_mediaAudioCache.find(path);
    if (it == m_mediaAudioCache.end()) {
        if (m_mediaAudioCache.size() >= 6) m_mediaAudioCache.clear();   // renders are short; keep a few
        AudioFile af;
        if (!af.open(path)) return nullptr;
        const auto info = af.info();
        std::vector<float> all = af.readAll();
        if (all.empty() || info.channels == 0) return nullptr;
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
    return &it->second;
}

bool AudioSyncPlayer::loadPreview(const std::string& key, const AudioSampleData& data,
                                  double rangeStart, double rangeEnd)
{
    if (!m_engine || data.samples.empty() || data.sampleRate == 0) return false;

    const double srcRate = static_cast<double>(data.sampleRate);
    const double engineRate = static_cast<double>(m_engine->config().sampleRate);
    const double from = std::max(0.0, rangeStart);
    const auto s0 = std::min(data.samples.size(), static_cast<size_t>(from * srcRate));
    const auto s1 = std::min(data.samples.size(), static_cast<size_t>(std::max(from, rangeEnd) * srcRate));
    if (s1 <= s0 + 1) return false;

    // Resample the range to the engine rate (linear, like playClip).
    const double ratio = engineRate / srcRate;
    const size_t n = static_cast<size_t>(static_cast<double>(s1 - s0) * ratio);
    m_previewBuffer.assign(n, 0.0f);
    for (size_t j = 0; j < n; ++j) {
        const double pos = static_cast<double>(j) / ratio;
        const size_t i = s0 + static_cast<size_t>(pos);
        const float frac = static_cast<float>(pos - std::floor(pos));
        const float a = data.samples[std::min(i, s1 - 1)];
        const float b = data.samples[std::min(i + 1, s1 - 1)];
        m_previewBuffer[j] = a + (b - a) * frac;
    }
    m_previewKey = key;
    m_previewFrom = static_cast<double>(s0) / srcRate;
    m_previewEnd = static_cast<double>(s1) / srcRate;
    m_previewRate = engineRate;
    // Same isolation as playClip: never move the timeline's clock.
    detachSyncClock();
    m_previewActive = true;
    installPreview();
    return true;
}

void AudioSyncPlayer::installPreview()
{
    if (!m_engine || m_previewBuffer.empty()) return;
    AudioTrackSource src;
    src.trackId     = 9999;
    src.samples     = m_previewBuffer.data();
    src.totalFrames = static_cast<int64_t>(m_previewBuffer.size());
    src.startFrame  = 0;
    src.channels    = 1;
    src.sampleRate  = static_cast<uint32_t>(m_previewRate);
    src.volume      = 1.0f;
    // Re-installed on every use: the engine is shared across pages.
    m_engine->setTrackSources({src});
}

bool AudioSyncPlayer::playPreview(double fromSec, double speed)
{
    if (!m_previewActive || !m_engine || m_previewBuffer.empty() || speed == 0.0) return false;
    installPreview();
    const auto last = static_cast<int64_t>(m_previewBuffer.size()) - 1;
    const auto frame = std::clamp(static_cast<int64_t>((fromSec - m_previewFrom) * m_previewRate), int64_t(0), last);
    m_engine->pause();
    m_engine->setPlaybackSpeed(speed);   // time-stretched; negative = reverse
    m_engine->seekToFrame(frame);
    m_engine->play();
    return true;
}

void AudioSyncPlayer::scrubPreview(double atSec)
{
    if (!m_previewActive || !m_engine || m_previewBuffer.empty()) return;
    if (atSec < m_previewFrom || atSec >= m_previewEnd) return;
    installPreview();
    m_engine->setPlaybackSpeed(1.0);
    const auto frame = static_cast<int64_t>((atSec - m_previewFrom) * m_previewRate);
    // ~1 video frame of sound, like Premiere's scrub.
    m_engine->scrub(frame, static_cast<int64_t>(m_previewRate / 24.0));
}

void AudioSyncPlayer::pausePreview()
{
    if (!m_previewActive || !m_engine) return;
    m_engine->pause();
    m_engine->setPlaybackSpeed(1.0);
}

void AudioSyncPlayer::stopPreview()
{
    if (!m_previewActive) return;
    m_previewActive = false;
    m_previewKey.clear();
    if (m_engine) {
        m_engine->pause();
        m_engine->setPlaybackSpeed(1.0);
        m_engine->clearTrackSources();   // before the buffer goes away
        restoreSyncClock();
    }
    m_previewBuffer.clear();
}

double AudioSyncPlayer::previewTime() const
{
    if (!m_previewActive || !m_engine) return -1.0;
    if (m_engine->transportState() != TransportState::Playing) return -1.0;
    return m_previewFrom + static_cast<double>(m_engine->currentFrame()) / m_previewRate;
}

// ═══════════════════════════════════════════════════════════════════════════
//  Imported files
// ═══════════════════════════════════════════════════════════════════════════

void AudioSyncPlayer::playFile(const AudioSampleData& audioData, int64_t seekFrame)
{
    if (!m_engine) return;
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
    detachSyncClock();

    m_engine->setPlaybackSpeed(1.0);
    m_engine->setTrackSources({src});
    m_engine->seekToFrame(seekFrame);
    m_engine->play();
}

void AudioSyncPlayer::stopFile()
{
    if (!m_engine) return;
    m_engine->pause();
    m_engine->clearTrackSources();
    m_engine->setPlaybackSpeed(1.0);
    restoreSyncClock();
}

// ═══════════════════════════════════════════════════════════════════════════
//  TTS drafts
// ═══════════════════════════════════════════════════════════════════════════

bool AudioSyncPlayer::playDraft(std::vector<float> samples, uint16_t channels, uint32_t sampleRate)
{
    if (!m_engine || samples.empty()) return false;
    m_draftSamples = std::move(samples);

    AudioTrackSource source{};
    source.trackId = 9997;
    source.samples = m_draftSamples.data();
    source.totalFrames = static_cast<int64_t>(m_draftSamples.size() / channels);
    source.startFrame = 0;
    source.channels = channels;
    source.sampleRate = sampleRate;
    source.volume = 1.0f;

    detachSyncClock();
    m_engine->setPlaybackSpeed(1.0);
    m_engine->setTrackSources({source});
    m_engine->seekToFrame(0);
    m_engine->play();
    m_draftActive = true;
    return true;
}

} // namespace rt
