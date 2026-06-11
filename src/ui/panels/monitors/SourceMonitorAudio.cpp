/*
 * SourceMonitorAudio.cpp -- waveform + source/sequence/scrub audio.
 * Extracted from SourceMonitor.cpp (behavior-preserving).
 */

#include "panels/monitors/SourceMonitor.h"
#include "panels/monitors/WaveformDisplayWidget.h"

#include "Theme.h"
#include "cache/FrameCache.h"

#include "viewport/Viewport.h"
#include "widgets/MiniTimeline.h"
#include "widgets/TransportButton.h"
#include "playback/PlaybackController.h"
#include "playback/MediaPool.h"
#include "playback/MediaSourceService.h"
#include "audio/AudioFile.h"
#include "audio/AudioEngine.h"
#include "audio/AudioPlaybackService.h"
#include "playback/AVSyncClock.h"
#include "timeline/Timeline.h"
#include "timeline/Track.h"
#include "timeline/AudioClip.h"
#include "timeline/SequenceClip.h"

#ifdef ROUNDTABLE_HAS_SPINE
#include "CompositeService.h"
#include "timeline/SpineClip.h"
#endif

#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QStackedLayout>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QDrag>
#include <QMimeData>
#include <QMouseEvent>
#include <QComboBox>
#include <QSettings>
#include <QPainter>
#include <QPointer>
#include <QResizeEvent>
#include <QTimer>
#include <QTreeWidget>
#include <QApplication>

#include <algorithm>
#include <cmath>
#include <thread>

namespace rt {

namespace {
constexpr int64_t kSourceScrubPreRollFrames  = 4096;
constexpr int64_t kSourceScrubPostRollFrames = 8192;
constexpr int64_t kSourceScrubMinFrames      = 4096;

bool loadResampledAudioFile(const std::filesystem::path& filePath,
                            std::vector<float>& samples,
                            uint16_t& channels)
{
    AudioFile file;
    if (!file.open(filePath)) {
        return false;
    }

    samples = file.readAllResampled(48000);
    if (samples.empty()) {
        return false;
    }

    channels = file.info().channels;
    if (channels == 0) {
        channels = 1;
    }
    return true;
}
} // namespace

// ═════════════════════════════════════════════════════════════════════════════
//  Waveform loading
// ═════════════════════════════════════════════════════════════════════════════

void SourceMonitor::loadWaveformAsync()
{
    if (!m_pool || !m_pool->isValid(m_mediaHandle)) return;

    auto filePath = m_mediaSources
        ? m_mediaSources->sourceInfo(m_mediaHandle).value().path
        : m_pool->getPath(m_mediaHandle);
    if (filePath.empty()) return;

    auto pathStr = filePath;
    QPointer<SourceMonitor> self(this);
    QPointer<WaveformDisplayWidget> widget(m_waveformWidget);
    bool audioOnly = m_audioOnly;
    const uint64_t generation = m_waveformLoadGeneration;

    // Load on a background thread and stream a peak envelope so opening a
    // source clip does not require a full decoded playback buffer.
    std::thread([self, widget, pathStr, audioOnly, generation]() {
        AudioFile file;
        if (!file.open(pathStr)) return;

        const auto& info = file.info();
        uint16_t ch = info.channels;
        if (ch == 0) ch = 1;
        constexpr int kPeakWindow = 256;
        std::vector<float> peaks;
        if (file.buildPeakEnvelopeResampled(48000, kPeakWindow, peaks) == 0 ||
            peaks.empty()) {
            return;
        }

        // Deliver to main thread
        if (!self || !widget) {
            return;
        }

        QMetaObject::invokeMethod(self, [self, widget, ch, audioOnly, generation,
                                         peaks = std::move(peaks)]() mutable {
            if (!self || !widget || self->m_waveformLoadGeneration != generation) {
                return;
            }
            if (audioOnly)
                widget->setPeaks(std::move(const_cast<std::vector<float>&>(peaks)), ch);
        }, Qt::QueuedConnection);
    }).detach();
}

bool SourceMonitor::ensureSourceAudioLoaded()
{
    if (m_audioSamples && !m_audioSamples->empty() && m_audioChannels > 0) {
        return true;
    }
    if (m_audioLoadFailed || !m_pool || !m_pool->isValid(m_mediaHandle)) {
        return false;
    }

    requestSourceAudioLoadAsync();
    return false;
}

void SourceMonitor::requestSourceAudioLoadAsync()
{
    if (m_audioLoadInFlight || m_audioLoadFailed || !m_pool || !m_pool->isValid(m_mediaHandle)) {
        return;
    }

    const auto filePath = m_mediaSources
        ? m_mediaSources->sourceInfo(m_mediaHandle).value().path
        : m_pool->getPath(m_mediaHandle);
    if (filePath.empty()) {
        m_audioLoadFailed = true;
        return;
    }

    m_audioLoadInFlight = true;
    const uint64_t generation = m_audioLoadGeneration;
    QPointer<SourceMonitor> self(this);

    std::thread([self, generation, filePath]() {
        std::vector<float> samples;
        uint16_t channels = 0;
        const bool ok = loadResampledAudioFile(filePath, samples, channels);

        if (!self) {
            return;
        }

        QMetaObject::invokeMethod(self, [self, generation, ok, channels,
                                         samples = std::move(samples)]() mutable {
            if (!self || self->m_audioLoadGeneration != generation) {
                return;
            }

            self->m_audioLoadInFlight = false;
            if (!ok || samples.empty() || channels == 0) {
                self->m_audioLoadFailed = true;
                return;
            }

            self->m_audioSamples = std::make_shared<std::vector<float>>(std::move(samples));
            self->m_audioChannels = channels;
            self->m_audioLoadFailed = false;

            if (self->m_controller && self->m_controller->isPlaying() &&
                self->m_audioEngine && !self->m_sourceAudioActive) {
                self->startSourceAudio();
            }
        }, Qt::QueuedConnection);
    }).detach();
}

bool SourceMonitor::ensureScrubAudioLoaded(int64_t frame, int64_t durationFrames)
{
    if (m_audioSamples && !m_audioSamples->empty() && m_audioChannels > 0) {
        return true;
    }

    if (!m_pool || !m_pool->isValid(m_mediaHandle) || m_audioLoadFailed) {
        return false;
    }

    const int64_t neededStart = std::max<int64_t>(0, frame);
    if (m_scrubAudioSamples && !m_scrubAudioSamples->empty() && m_scrubAudioChannels > 0) {
        const int64_t cachedFrames = static_cast<int64_t>(m_scrubAudioSamples->size() / m_scrubAudioChannels);
        const int64_t cachedEnd = m_scrubAudioStartFrame + cachedFrames;
        if (neededStart >= m_scrubAudioStartFrame &&
            (neededStart + durationFrames) <= cachedEnd) {
            return true;
        }
    }

    const auto filePath = m_mediaSources
        ? m_mediaSources->sourceInfo(m_mediaHandle).value().path
        : m_pool->getPath(m_mediaHandle);
    if (filePath.empty()) {
        m_audioLoadFailed = true;
        return false;
    }

    AudioFile file;
    if (!file.open(filePath)) {
        m_audioLoadFailed = true;
        return false;
    }

    uint16_t channels = file.info().channels;
    if (channels == 0) {
        channels = 1;
    }

    const int64_t windowStart = std::max<int64_t>(0, frame - kSourceScrubPreRollFrames);
    const int64_t windowFrames = std::max<int64_t>(
        durationFrames + kSourceScrubPreRollFrames + kSourceScrubPostRollFrames,
        kSourceScrubMinFrames);

    std::vector<float> samples;
    const int64_t framesRead = file.readRegionResampled(windowStart, windowFrames, 48000, samples);
    if (framesRead <= 0 || samples.empty()) {
        return false;
    }

    m_scrubAudioSamples = std::make_shared<std::vector<float>>(std::move(samples));
    m_scrubAudioChannels = channels;
    m_scrubAudioStartFrame = windowStart;
    requestSourceAudioLoadAsync();
    return true;
}

void SourceMonitor::startSourceAudio()
{
    if (!m_audioEngine) return;
    if (m_sourceAudioActive) return;

    if (m_isSequence) {
        startSequenceAudio();
        return;
    }

    if (!ensureSourceAudioLoaded()) return;

    // Detach the timeline's sync clock so audioEngine->play() won't
    // start/stop/reset the timeline position.
    m_savedSyncClock = m_audioEngine->syncClock();
    m_audioEngine->setSyncClock(nullptr);
    m_sourceAudioActive = true;

    emit playbackStarted();

    // Load source clip audio into the engine
    loadSourceAudio();

    // Match audio speed to controller shuttle speed
    double speed = m_controller->shuttleSpeed();
    if (speed == 0.0) speed = 1.0;
    m_audioEngine->setPlaybackSpeed(speed);

    // Seek audio to current playhead
    int64_t tick = m_controller->currentTick();
    int64_t frame = static_cast<int64_t>(
        static_cast<double>(tick) / 48000.0 * m_audioEngine->sampleRate());
    m_audioEngine->seekToFrame(frame);
    m_audioEngine->play();
}

void SourceMonitor::stopSourceAudio()
{
    if (!m_audioEngine || !m_sourceAudioActive) return;

    if (m_isSequence) {
        stopSequenceAudio();
        return;
    }

    m_audioEngine->pause();
    m_audioEngine->setPlaybackSpeed(1.0);  // Reset to normal speed
    m_audioEngine->clearTrackSources();
    m_audioEngine->resetStretchers();

    // Restore the timeline's sync clock
    m_audioEngine->setSyncClock(m_savedSyncClock);
    m_savedSyncClock = nullptr;
    m_sourceAudioActive = false;
}

Timeline* SourceMonitor::resolveSequenceTimeline() const
{
    if (!m_isSequence || !m_seqTimelineGetter) return nullptr;
    return m_seqTimelineGetter();
}

void SourceMonitor::startSequenceAudio()
{
    if (!m_audioEngine || !m_seqAudioPlayback) return;
    if (m_sourceAudioActive) return;

    Timeline* innerTimeline = resolveSequenceTimeline();
    // Mark active and emit playbackStarted even when the inner timeline has
    // no audio clips — playbackStarted is also what halts the main
    // timeline's playback (see TimelineWorkspacePanelsCreate). Without it,
    // dragging a silent sequence into the source monitor would leave the
    // main timeline's audio playing.
    m_savedSyncClock = m_audioEngine->syncClock();
    m_audioEngine->setSyncClock(nullptr);
    m_sourceAudioActive = true;

    emit playbackStarted();

    if (!innerTimeline) {
        // Nothing to play — leave engine paused but in source-monitor mode
        // so stopSequenceAudio() correctly restores the timeline sync clock
        // when the user pauses.
        return;
    }

    // Build track sources from the inner sequence (mirroring how the main
    // timeline drives audio via its own AudioPlaybackService instance).
    m_seqAudioPlayback->setTimeline(innerTimeline);
    m_seqAudioPlayback->setAudioEngine(m_audioEngine);
    m_seqAudioPlayback->setPlaybackController(m_controller.get());
    m_seqAudioPlayback->invalidateSources();
    m_seqAudioPlayback->loadSources(/*allowBlockingMisses=*/true);

    // Match audio speed to controller shuttle speed
    double speed = m_controller->shuttleSpeed();
    if (speed == 0.0) speed = 1.0;
    m_audioEngine->setPlaybackSpeed(speed);

    // Seek audio engine to current playhead
    int64_t tick = m_controller->currentTick();
    int64_t frame = static_cast<int64_t>(
        static_cast<double>(tick) / 48000.0 * m_audioEngine->sampleRate());
    m_audioEngine->seekToFrame(frame);
    m_audioEngine->play();
}

void SourceMonitor::stopSequenceAudio()
{
    if (!m_audioEngine || !m_sourceAudioActive) return;

    m_audioEngine->pause();
    m_audioEngine->setPlaybackSpeed(1.0);
    m_audioEngine->clearTrackSources();
    m_audioEngine->resetStretchers();

    if (m_seqAudioPlayback)
        m_seqAudioPlayback->invalidateSources();

    m_audioEngine->setSyncClock(m_savedSyncClock);
    m_savedSyncClock = nullptr;
    m_sourceAudioActive = false;
}

void SourceMonitor::loadSourceAudio()
{
    if (!m_audioEngine || !ensureSourceAudioLoaded())
        return;

    AudioTrackSource src;
    src.trackId     = 9999;
    src.samples     = m_audioSamples->data();
    src.totalFrames = static_cast<int64_t>(m_audioSamples->size() / m_audioChannels);
    src.startFrame  = 0;
    src.channels    = m_audioChannels;
    src.sampleRate  = 48000;
    src.volume      = 1.0f;
    src.pan         = 0.0f;
    src.muted       = false;
    src.solo        = false;

    m_audioEngine->setTrackSources({src});
}

void SourceMonitor::scrubAudioAt(int64_t tick)
{
    if (!m_audioEngine)
        return;

    const int64_t frame = static_cast<int64_t>(
        static_cast<double>(tick) / 48000.0 * m_audioEngine->sampleRate());
    constexpr int64_t scrubDurationFrames = 2048;

    // Sequence preview: build track sources from the inner timeline (same
    // path startSequenceAudio uses) and let AudioEngine::scrub play a short
    // burst from those mixed sources.
    if (m_isSequence) {
        if (!m_seqAudioPlayback) return;

        Timeline* innerTimeline = resolveSequenceTimeline();
        if (!innerTimeline) return;

        const bool wasActive = m_sourceAudioActive;
        if (!wasActive) {
            m_savedSyncClock = m_audioEngine->syncClock();
            m_audioEngine->setSyncClock(nullptr);
        }

        m_seqAudioPlayback->setTimeline(innerTimeline);
        m_seqAudioPlayback->setAudioEngine(m_audioEngine);
        m_seqAudioPlayback->setPlaybackController(m_controller.get());
        m_seqAudioPlayback->ensureSourcesLoaded();

        m_audioEngine->scrub(frame, scrubDurationFrames);

        if (!wasActive) {
            m_audioEngine->setSyncClock(m_savedSyncClock);
            m_savedSyncClock = nullptr;
        }
        return;
    }

    if (!ensureScrubAudioLoaded(frame, scrubDurationFrames)) {
        return;
    }

    AudioTrackSource src;
    src.trackId = 9999;
    src.startFrame = 0;
    src.sampleRate = 48000;
    src.volume = 1.0f;
    src.pan = 0.0f;
    src.muted = false;
    src.solo = false;

    int64_t localFrame = frame;
    if (m_audioSamples && !m_audioSamples->empty() && m_audioChannels > 0) {
        src.samples = m_audioSamples->data();
        src.totalFrames = static_cast<int64_t>(m_audioSamples->size() / m_audioChannels);
        src.channels = m_audioChannels;
    } else {
        src.samples = m_scrubAudioSamples ? m_scrubAudioSamples->data() : nullptr;
        src.totalFrames = (m_scrubAudioSamples && m_scrubAudioChannels > 0)
            ? static_cast<int64_t>(m_scrubAudioSamples->size() / m_scrubAudioChannels)
            : 0;
        src.channels = m_scrubAudioChannels;
        localFrame = std::max<int64_t>(0, frame - m_scrubAudioStartFrame);
    }

    if (!src.samples || src.totalFrames <= 0 || src.channels == 0) {
        return;
    }

    // Temporarily detach sync clock, load sources, scrub, then restore
    if (!m_sourceAudioActive) {
        m_savedSyncClock = m_audioEngine->syncClock();
        m_audioEngine->setSyncClock(nullptr);
        m_audioEngine->setTrackSources({src});
    }

    m_audioEngine->scrub(localFrame, scrubDurationFrames);

    if (!m_sourceAudioActive) {
        // Restore after scrub burst finishes — scrub is async so we just
        // restore the clock pointer; the callback will stop on its own.
        m_audioEngine->setSyncClock(m_savedSyncClock);
        m_savedSyncClock = nullptr;
    }
}
} // namespace rt
