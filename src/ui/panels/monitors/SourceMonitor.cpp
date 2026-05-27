/*
 * SourceMonitor.cpp -- media loading, playback, events, audio.
 *
 * Constructor/setupUI --> SourceMonitorUI.cpp
 */

#include "panels/monitors/SourceMonitor.h"
#include "panels/monitors/WaveformDisplayWidget.h"

#include "Theme.h"
#include "media/FrameCache.h"

#include "viewport/Viewport.h"
#include "widgets/MiniTimeline.h"
#include "widgets/TransportButton.h"
#include "media/PlaybackController.h"
#include "media/MediaPool.h"
#include "media/MediaSourceService.h"
#include "media/AudioFile.h"
#include "media/AudioEngine.h"
#include "media/AudioPlaybackService.h"
#include "media/AVSyncClock.h"
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

// Still-image extensions. FFmpeg's image2 demuxer reports a real but tiny
// "duration" for JPGs (e.g. 0.04s @ 25fps), which would otherwise shrink
// the Source Monitor's standalone duration to a single frame and leave the
// viewport blank.  PNG happens to come back with duration=0, which is why
// it "just works" without this guard.
bool isStillImageSourcePath(const std::filesystem::path& path)
{
    auto ext = path.extension().string();
    for (auto& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return ext == ".png"  || ext == ".jpg"  || ext == ".jpeg" || ext == ".bmp" ||
           ext == ".gif"  || ext == ".tif"  || ext == ".tiff" || ext == ".webp" ||
           ext == ".tga"  || ext == ".dds";
}

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


void SourceMonitor::refreshDragButtons(bool hasVideo, bool hasAudio)
{
    if (m_btnDragVideo) {
        m_btnDragVideo->setEnabled(hasVideo);
        m_btnDragVideo->setCursor(hasVideo ? Qt::OpenHandCursor
                                           : Qt::ArrowCursor);
    }
    if (m_btnDragAudio) {
        m_btnDragAudio->setEnabled(hasAudio);
        m_btnDragAudio->setCursor(hasAudio ? Qt::OpenHandCursor
                                           : Qt::ArrowCursor);
    }
}

void SourceMonitor::loadClip(uint64_t mediaHandle, MediaPool* pool)
{
    clearClip();

    if (!pool || !pool->isValid(mediaHandle))
        return;

    m_pool        = pool;
    m_mediaHandle = mediaHandle;
    m_hasClip     = true;
    m_audioOnly   = false;

    // Get clip info
    const auto* info = pool->getInfo(mediaHandle);
    if (info)
    {
        m_audioOnly = (info->videoStreamIndex < 0);
        m_fps          = info->fps > 0.0 ? info->fps : 24.0;
        m_frameCount   = info->frameCount;
        m_clipDuration = static_cast<int64_t>(info->duration * 48000.0);

        // Treat still images uniformly regardless of how FFmpeg's image2
        // demuxer reports them.  JPG comes back with a tiny duration
        // (~0.04s) that would otherwise make the standalone duration
        // ~1920 ticks and leave the Source Monitor showing nothing useful;
        // PNG happens to report duration=0 already.  Normalise both to a
        // 5-second default and a single canonical frame.
        auto path = pool->getPath(mediaHandle);
        if (isStillImageSourcePath(path)) {
            m_clipDuration = static_cast<int64_t>(48000.0 * 5.0);
            m_frameCount   = 1;
        }

        m_controller->setFrameRate(m_fps);
        m_controller->setStandaloneDuration(m_clipDuration);
        m_miniTimeline->setDuration(m_clipDuration);
        m_miniTimeline->setFrameRate(m_fps);

        // Set clip name from file path
        setClipName(QString::fromStdString(path.filename().string()));
    }

    // Show first frame (skip for audio-only — no video to decode)
    m_controller->seekTo(0);
    if (!m_audioOnly) {
        m_viewStack->setCurrentIndex(0); // video viewport
        updateFrameDisplay();
    } else {
        m_viewStack->setCurrentIndex(1); // waveform display
        m_waveformWidget->clear();
        updateTimecodeDisplay();
        updateWaveformSelection();
    }

    // Eagerly kick off async audio decode for video clips that have an
    // audio stream, so audio is ready when the user presses play. The
    // previous fully-lazy approach meant the FIRST play silently exited
    // (audio not loaded yet) and the user had to press play TWICE to hear
    // anything — and on the first press startSourceAudio bailed without
    // actually playing.
    m_audioSamples.reset();
    if (m_audioEngine && !m_audioOnly && info && info->audioStreamIndex >= 0)
        requestSourceAudioLoadAsync();
    m_audioChannels = 0;
    m_audioLoadFailed = false;
    m_audioLoadInFlight = false;
    ++m_audioLoadGeneration;
    m_scrubAudioSamples.reset();
    m_scrubAudioChannels = 0;
    m_scrubAudioStartFrame = 0;
    ++m_waveformLoadGeneration;
    if (m_audioOnly) {
        loadWaveformAsync();
    }

    // A still image / single-frame media has a video stream but no audio;
    // a .wav has audio but no video. Enable only the applicable buttons.
    const bool clipHasAudio = (info && info->audioStreamIndex >= 0);
    refreshDragButtons(/*hasVideo=*/!m_audioOnly, /*hasAudio=*/clipHasAudio);

    // The first updateFrameDisplay above returns no frame because getFrame()
    // for !scrubMode schedules an async prefetch and returns null. Without a
    // retry the viewport stays blank until the user does something that
    // triggers another paint — for stills that meant dropping a second time.
    // Run the poll timer briefly so subsequent ticks pick up the cached
    // frame the prefetch worker delivers. (Mirrors loadSequence's settle.)
    if (!m_audioOnly) {
        m_scrubSettleCounter = 100;  // ~1.6s of retries at 16ms
        if (!m_pollTimer->isActive())
            m_pollTimer->start();
    }
}

void SourceMonitor::loadSequence(size_t sequenceIndex, const QString& name,
                                 int64_t durationTicks, double fps,
                                 SequenceFrameProvider frameProvider,
                                 SequenceTimelineGetter timelineGetter)
{
    clearClip();

    m_isSequence      = true;
    m_sequenceIndex   = sequenceIndex;
    m_seqFrameProvider = std::move(frameProvider);
    m_seqTimelineGetter = std::move(timelineGetter);
    m_hasClip         = true;
    m_audioOnly       = false;
    m_fps             = fps > 0.0 ? fps : 24.0;
    m_clipDuration    = durationTicks;
    m_frameCount      = static_cast<int64_t>(
        (static_cast<double>(durationTicks) / 48000.0) * m_fps);

    m_controller->setFrameRate(m_fps);
    m_controller->setStandaloneDuration(m_clipDuration);
    m_miniTimeline->setDuration(m_clipDuration);
    m_miniTimeline->setFrameRate(m_fps);
    setClipName(name);

    // Reset the sequence audio service so the first play rebuilds sources
    // from the (now-current) inner timeline rather than reusing stale state
    // from a previously-previewed sequence.
    if (m_seqAudioPlayback)
        m_seqAudioPlayback->reset();

    m_controller->seekTo(0);
    m_viewStack->setCurrentIndex(0); // video viewport
    updateFrameDisplay();

    // A sequence always has a (composited) video stream. Audio is only
    // available if the inner timeline actually has audio content — mirror
    // the nesting drop's innerHasAudio test.
    bool seqHasAudio = false;
    if (Timeline* inner = resolveSequenceTimeline()) {
        for (size_t ti = 0; ti < inner->trackCount() && !seqHasAudio; ++ti) {
            auto* trk = inner->track(ti);
            if (!trk || trk->type() != TrackType::Audio) continue;
            for (size_t ci = 0; ci < trk->clipCount(); ++ci) {
                auto* c = trk->clip(ci);
                if (c && c->isEnabled() &&
                    (dynamic_cast<AudioClip*>(c) ||
                     dynamic_cast<SequenceClip*>(c))) {
                    seqHasAudio = true; break;
                }
            }
        }
    }
    m_seqHasAudio = seqHasAudio;
    refreshDragButtons(/*hasVideo=*/true, /*hasAudio=*/seqHasAudio);

    // The first compositeFrame for the inner sequence usually returns an
    // empty frame because no clips are decoded yet. Kick the poll timer
    // with a long settle so prefetch can land and a real frame arrives
    // without the user having to scrub first. (Same mechanism the scrub
    // path uses; longer counter here = ~1.6s of retries at 16ms.)
    m_scrubSettleCounter = 100;
    if (!m_pollTimer->isActive())
        m_pollTimer->start();
}

#ifdef ROUNDTABLE_HAS_SPINE
void SourceMonitor::loadSpineClip(SpineClip* spineClip, CompositeService* compositeService)
{
    if (!spineClip || !compositeService) return;
    
    // Clear any previous clip
    clearClip();
    
    // Render first frame of the animation at preview size
    auto frame = compositeService->renderSpineClip(spineClip, 0, 960, 540);
    if (!frame || !frame->ensurePixels()) return;
    
    m_isSequence = true;
    m_hasClip = true;
    m_audioOnly = false;
    m_fps = 30.0;
    m_frameCount = 150;
    m_clipDuration = secondsToTicks(5.0);
    
    m_controller->setFrameRate(m_fps);
    m_controller->setStandaloneDuration(m_clipDuration);
    m_miniTimeline->setDuration(m_clipDuration);
    m_miniTimeline->setFrameRate(m_fps);
    setClipName(QString::fromStdString(spineClip->label()));
    
    m_controller->seekTo(0);
    m_viewStack->setCurrentIndex(0);
    
    // Display the rendered frame directly
    if (m_viewport) {
        m_viewport->displayFrame(*frame);
        updateTimecodeDisplay();
    }

    // Spine characters are video-only.
    refreshDragButtons(/*hasVideo=*/true, /*hasAudio=*/false);
}
#endif // ROUNDTABLE_HAS_SPINE

void SourceMonitor::clearClip()
{
    m_pollTimer->stop();

    // Stop audio before stopping the controller (stopSourceAudio
    // restores the timeline's sync clock ownership of the AudioEngine).
    stopSourceAudio();
    m_audioSamples.reset();
    m_audioChannels = 0;
    m_audioLoadFailed = false;
    m_audioLoadInFlight = false;
    ++m_audioLoadGeneration;
    m_scrubAudioSamples.reset();
    m_scrubAudioChannels = 0;
    m_scrubAudioStartFrame = 0;
    ++m_waveformLoadGeneration;

    if (m_hasClip && m_controller)
        m_controller->stop();

    // NOTE: do NOT null m_pool here. It is a long-lived, non-owning pointer
    // to the app-wide MediaPool injected via setMediaPool() at startup (the
    // same lifetime model as ProgramMonitor, which never clears it). Nulling
    // it on clearClip() silently broke path-based drops: dragging an asset
    // from the Library (items carry handle 0 → loadFromPath()) did nothing,
    // while bin drags survived only via the dropReceived() handle fallback.
    m_mediaHandle  = 0;
    m_hasClip      = false;
    m_audioOnly    = false;
    m_isSequence   = false;
    m_sequenceIndex = 0;
    m_seqHasAudio  = false;
    m_seqFrameProvider = nullptr;
    m_seqTimelineGetter = nullptr;
    if (m_seqAudioPlayback) {
        m_seqAudioPlayback->setTimeline(nullptr);
        m_seqAudioPlayback->reset();
    }
    m_frameCount   = 0;
    m_clipDuration = 0;
    m_fps          = 24.0;

    m_viewport->clearFrame();
    m_waveformWidget->clear();
    m_viewStack->setCurrentIndex(0);
    m_miniTimeline->setDuration(0);
    m_miniTimeline->clearInOutPoints();
    m_clipLabel->setText(tr("No clip loaded"));
    m_timecodeLabel->setText(QStringLiteral("00:00:00:00"));

    // Nothing loaded — neither stream is draggable.
    refreshDragButtons(/*hasVideo=*/false, /*hasAudio=*/false);
}

// ═════════════════════════════════════════════════════════════════════════════
//  In / Out points
// ═════════════════════════════════════════════════════════════════════════════

void SourceMonitor::markIn()
{
    if (!m_hasClip) return;
    m_miniTimeline->setInPoint(currentTick());
    updateTimecodeDisplay();
    updateWaveformSelection();
    emit inOutChanged();
}

void SourceMonitor::markOut()
{
    if (!m_hasClip) return;
    m_miniTimeline->setOutPoint(currentTick());
    updateTimecodeDisplay();
    updateWaveformSelection();
    emit inOutChanged();
}

void SourceMonitor::clearInOut()
{
    m_miniTimeline->clearInOutPoints();
    updateTimecodeDisplay();
    updateWaveformSelection();
    emit inOutChanged();
}

int64_t SourceMonitor::inPoint() const noexcept
{
    return m_miniTimeline ? m_miniTimeline->inPoint() : -1;
}

int64_t SourceMonitor::outPoint() const noexcept
{
    return m_miniTimeline ? m_miniTimeline->outPoint() : -1;
}

bool SourceMonitor::hasInPoint() const noexcept
{
    return m_miniTimeline && m_miniTimeline->hasInPoint();
}

bool SourceMonitor::hasOutPoint() const noexcept
{
    return m_miniTimeline && m_miniTimeline->hasOutPoint();
}

int64_t SourceMonitor::selectedDuration() const noexcept
{
    return m_miniTimeline ? m_miniTimeline->selectedDuration() : 0;
}

// ═════════════════════════════════════════════════════════════════════════════
//  Transport
// ═════════════════════════════════════════════════════════════════════════════

int64_t SourceMonitor::currentTick() const noexcept
{
    return m_controller ? m_controller->currentTick() : 0;
}

void SourceMonitor::scrubTo(int64_t tick)
{
    if (!m_hasClip || !m_controller) return;
    m_controller->seekTo(tick);
    updateFrameDisplay();
}

void SourceMonitor::scrubToFrame(int64_t frameNumber)
{
    if (!m_hasClip || m_fps <= 0.0) return;
    int64_t tick = static_cast<int64_t>((static_cast<double>(frameNumber) / m_fps) * 48000.0);
    scrubTo(tick);
}

// ═════════════════════════════════════════════════════════════════════════════
//  Source region
// ═════════════════════════════════════════════════════════════════════════════

SourceMonitor::SourceRegion SourceMonitor::sourceRegion() const
{
    SourceRegion region;
    region.mediaHandle = m_mediaHandle;
    region.fps         = m_fps;

    if (hasInPoint())
        region.sourceIn = inPoint();
    else
        region.sourceIn = 0;

    if (hasOutPoint())
        region.sourceOut = outPoint();
    else
        region.sourceOut = m_clipDuration;

    region.duration = region.sourceOut - region.sourceIn;
    if (region.duration < 0) region.duration = 0;

    return region;
}

void SourceMonitor::updateWaveformSelection()
{
    if (!m_waveformWidget) return;
    if (!m_audioOnly || m_clipDuration <= 0 || (!hasInPoint() && !hasOutPoint())) {
        m_waveformWidget->clearSelection();
        return;
    }

    int64_t inTick = hasInPoint() ? inPoint() : 0;
    int64_t outTick = hasOutPoint() ? outPoint() : m_clipDuration;
    if (outTick < inTick) std::swap(inTick, outTick);

    double duration = static_cast<double>(m_clipDuration);
    double startRatio = std::clamp(static_cast<double>(inTick) / duration, 0.0, 1.0);
    double endRatio = std::clamp(static_cast<double>(outTick) / duration, 0.0, 1.0);
    m_waveformWidget->setSelectionRange(startRatio, endRatio);
}

// ═════════════════════════════════════════════════════════════════════════════
//  Display
// ═════════════════════════════════════════════════════════════════════════════

void SourceMonitor::setClipName(const QString& name)
{
    m_clipLabel->setText(name);
}

QSize SourceMonitor::sizeHint() const
{
    return QSize(480, 400);
}

// ═════════════════════════════════════════════════════════════════════════════
//  Slots
// ═════════════════════════════════════════════════════════════════════════════

void SourceMonitor::onScrub(int64_t tick)
{
    if (!m_hasClip) return;

    // Emit on the first scrub event of a sequence so the timeline can
    // halt its own playback for mutual exclusion.
    if (!m_scrubPending)
        emit scrubbed();

    // Coalesce: just record the latest scrub target.  The poll timer
    // (16 ms) will pick it up and do a single seek+decode for the most
    // recent position, instead of one full-resolution precise-seek per
    // mouse-move event.  On long-GOP H.264 sources each precise seek is
    // 100-300 ms; without coalescing, dragging the playhead generates a
    // huge backlog and feels frozen.
    m_pendingScrubTick = tick;
    m_scrubPending     = true;

    // Start settle polling so late prefetch deliveries update the display
    // after the user stops scrubbing (poll timer is normally off when paused).
    m_scrubSettleCounter = 10; // ~160ms at 16ms interval
    if (!m_pollTimer->isActive())
        m_pollTimer->start();
}

void SourceMonitor::onPollTimer()
{
    if (!m_hasClip || !m_controller) return;

    // Drain any pending scrub: a single seek+decode for the latest
    // mouse-move target, regardless of how many scrub events arrived
    // since the last poll.
    if (m_scrubPending) {
        m_scrubPending = false;
        m_controller->seekTo(m_pendingScrubTick);
        scrubAudioAt(m_pendingScrubTick);
        emit playheadChanged(m_pendingScrubTick);
    }

    (void)m_controller->pollPosition();
    updateFrameDisplay();

    // For sequence playback, periodically refresh the audio decode window
    // so audio keeps playing past the initial 16-second look-ahead — same
    // mechanism the main timeline uses via scheduleAudioPlaybackWindowRefresh.
    if (m_isSequence && m_sourceAudioActive && m_seqAudioPlayback &&
        m_seqAudioPlayback->needsPlaybackWindowRefresh() &&
        !m_seqAudioWindowRefreshScheduled) {
        m_seqAudioWindowRefreshScheduled = true;
        m_seqAudioPlayback->warmCacheAsync();
        QTimer::singleShot(0, this, [this]() {
            m_seqAudioWindowRefreshScheduled = false;
            if (m_destroying.load(std::memory_order_acquire)) return;
            if (m_seqAudioPlayback && m_seqAudioPlayback->needsPlaybackWindowRefresh())
                m_seqAudioPlayback->loadSources(/*allowBlockingMisses=*/false);
        });
    }

    // Stop polling if playback ended (but honour scrub settle window)
    if (!m_controller->isPlaying()) {
        if (m_scrubSettleCounter > 0) {
            --m_scrubSettleCounter;
        } else {
            m_pollTimer->stop();
        }
    }
}

// ═════════════════════════════════════════════════════════════════════════════
//  Internal helpers
// ═════════════════════════════════════════════════════════════════════════════

void SourceMonitor::updateFrameDisplay()
{
    if (!m_hasClip || !m_controller) return;

    int64_t tick = m_controller->currentTick();
    m_miniTimeline->setPlayhead(tick);

    if (m_audioOnly) {
        // Update waveform playhead position
        if (m_clipDuration > 0)
            m_waveformWidget->setPlayheadRatio(
                static_cast<double>(tick) / static_cast<double>(m_clipDuration));
        updateWaveformSelection();
    }
    else if (m_isSequence && m_seqFrameProvider)
    {
        auto frame = m_seqFrameProvider(
            tick,
            static_cast<uint32_t>(m_viewport->width()),
            static_cast<uint32_t>(m_viewport->height()),
            false);
        if (frame)
            m_viewport->displayFrame(frame);
    }
    else if (m_pool && m_pool->isValid(m_mediaHandle))
    {
        // Convert tick → frame number
        int64_t frameNum = 0;
        if (m_fps > 0.0)
            frameNum = static_cast<int64_t>(
                (static_cast<double>(tick) / 48000.0) * m_fps);
        frameNum = std::clamp(frameNum, int64_t(0), std::max(int64_t(0), m_frameCount - 1));

        const bool playing = m_controller->isPlaying();
        // Honor the user's playback-resolution setting (the dropdown in the
        // program monitor, persisted in QSettings) instead of always
        // forcing Half tier. Without this, character/source preview was
        // stuck at Half even when the user picked Full, which made
        // characters look blurry in the source monitor.
        ResolutionTier tier = ResolutionTier::Half;
        {
            int idx = QSettings().value(
                QStringLiteral("playback/resolutionIndex"), 1).toInt();
            switch (idx) {
                case 0: tier = ResolutionTier::Full;    break;
                case 1: tier = ResolutionTier::Half;    break;
                case 2: tier = ResolutionTier::Quarter; break;
                case 3: tier = ResolutionTier::Quarter; break;  // 1/8 falls back to 1/4 (no Eighth tier)
                default: tier = ResolutionTier::Half;   break;
            }
        }
        std::shared_ptr<CachedFrame> frame;
        if (m_mediaSources) {
            auto result = m_mediaSources->requestFrame({
                m_mediaHandle, frameNum,
                RenderRequestType::SourceMonitor,
                RenderQuality::Auto,
                playing ? RenderExactness::BestEffortAllowed : RenderExactness::ExactRequired,
                tier
            });
            frame = result.frame;
        } else if (m_pool) {
            frame = playing
                ? m_pool->tryGetFrame(m_mediaHandle, frameNum, tier)
                : m_pool->getFrame(m_mediaHandle, frameNum, tier, /*scrubMode=*/true);
        }
        if (frame) {
            frame->ensurePixels();
            m_viewport->displayFrame(*frame);
        }
    }

    updateTimecodeDisplay();

    // Update shuttle speed display
    if (m_shuttleSpeedLabel && m_controller) {
        double speed = m_controller->shuttleSpeed();
        if (m_controller->isPlaying() && speed != 0.0 && speed != 1.0) {
            QString speedText;
            if (speed == static_cast<int>(speed))
                speedText = QStringLiteral("%1x").arg(static_cast<int>(speed));
            else
                speedText = QStringLiteral("%1x").arg(speed, 0, 'g', 2);
            m_shuttleSpeedLabel->setText(speedText);
            m_shuttleSpeedLabel->show();
        } else {
            m_shuttleSpeedLabel->hide();
        }
    }

    emit playheadChanged(tick);
}

void SourceMonitor::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    // Mirror of ProgramMonitor::resizeEvent — see that file for the full
    // explanation of why repaint() + updateFrameDisplay() + a deferred
    // update() are all needed (Windows modal WM_SIZE loop suppresses
    // queued paint events while the dock still receives stale WM_PAINTs).
    auto forceRepaint = [this]() {
        repaint();
        if (auto* lay = layout()) {
            for (int i = 0; i < lay->count(); ++i) {
                auto* item = lay->itemAt(i);
                auto* w = item ? item->widget() : nullptr;
                if (!w || !w->isVisible()) continue;
                if (m_viewport && w == m_viewport->parentWidget()) continue;
                w->repaint();
            }
        }
    };
    forceRepaint();

    // Produce a fresh decoded frame at the new widget size so the viewport
    // content is correct during/after resize even when paused.  Without
    // this, the poll timer stops (when not playing) and the old stretched
    // frame remains — the "echo" effect.
    updateFrameDisplay();
    // Keep the poll timer alive briefly after resize to settle, so any
    // late decodes or paint corrections take effect.
    if (!m_controller->isPlaying()) {
        m_scrubSettleCounter = 10;
        if (!m_pollTimer->isActive())
            m_pollTimer->start();
    }

    QTimer::singleShot(0, this, [this]() {
        if (m_destroying.load(std::memory_order_acquire)) return;
        update();
        if (auto* lay = layout()) {
            for (int i = 0; i < lay->count(); ++i) {
                auto* item = lay->itemAt(i);
                auto* w = item ? item->widget() : nullptr;
                if (!w || !w->isVisible()) continue;
                if (m_viewport && w == m_viewport->parentWidget()) continue;
                w->update();
            }
        }
    });
}

void SourceMonitor::updateTimecodeDisplay()
{
    if (!m_controller) return;

    // Left timecode: current playhead position (updates every scrub /
    // play tick). Without this the label was stuck at 00:00:00:00 the
    // entire session.
    if (m_timecodeLabel && !m_timecodeEdit->isVisible()) {
        m_timecodeLabel->setText(QString::fromStdString(
            m_controller->currentTimecodeString()));
    }

    // Right timecode: if the user marked in/out, show the selected
    // range duration; otherwise show the full clip duration (Premiere
    // parity).
    if (m_durationLabel) {
        const int64_t sel = m_miniTimeline ? m_miniTimeline->selectedDuration() : 0;
        const int64_t shown = (sel > 0) ? sel : m_controller->durationTicks();
        auto dur = tickToTimecode(shown, m_controller->frameRate());
        m_durationLabel->setText(QString::fromStdString(dur.toString()));
    }
}

bool SourceMonitor::eventFilter(QObject* watched, QEvent* event)
{
    // Grab keyboard focus when user clicks anywhere in the Source Monitor
    // (viewport, mini-timeline, waveform) so JKL/Space route here.
    if (event->type() == QEvent::MouseButtonPress ||
        event->type() == QEvent::MouseButtonDblClick) {
        // Timecode label click → show editable timecode entry
        if (watched == m_timecodeLabel && event->type() == QEvent::MouseButtonPress) {
            m_timecodeLabel->hide();
            m_timecodeEdit->setText(m_timecodeLabel->text());
            m_timecodeEdit->show();
            m_timecodeEdit->setFocus();
            m_timecodeEdit->selectAll();
            return true;
        }
        setFocus();
    }

    // Forward drag events from the viewport child to our own handlers
    if (watched == m_viewport) {
        if (event->type() == QEvent::DragEnter) {
            dragEnterEvent(static_cast<QDragEnterEvent*>(event));
            return event->isAccepted();
        }
        if (event->type() == QEvent::DragMove) {
            dragMoveEvent(static_cast<QDragMoveEvent*>(event));
            return event->isAccepted();
        }
        if (event->type() == QEvent::Drop) {
            dropEvent(static_cast<QDropEvent*>(event));
            return event->isAccepted();
        }

        // Drag-out: click and drag from the viewport to the timeline
        if (event->type() == QEvent::MouseButtonPress) {
            auto* me = static_cast<QMouseEvent*>(event);
            if (me->button() == Qt::LeftButton && m_hasClip)
                m_dragStartPos = me->pos();
        }
        if (event->type() == QEvent::MouseMove) {
            auto* me = static_cast<QMouseEvent*>(event);
            if ((me->buttons() & Qt::LeftButton) && m_hasClip
                && !m_dragStartPos.isNull()
                && (me->pos() - m_dragStartPos).manhattanLength()
                       >= QApplication::startDragDistance()) {
                m_dragStartPos = QPoint();
                startSourceDrag(SourceDragMode::Both);
                return true;
            }
        }
    }

    // Drag-out from the dedicated video / audio drag buttons. A disabled
    // button (source lacks that stream) must not start a drag.
    if ((watched == m_btnDragVideo || watched == m_btnDragAudio) && m_hasClip
        && static_cast<QWidget*>(watched)->isEnabled()) {
        if (event->type() == QEvent::MouseButtonPress) {
            auto* me = static_cast<QMouseEvent*>(event);
            if (me->button() == Qt::LeftButton)
                m_dragBtnStartPos = me->pos();
        } else if (event->type() == QEvent::MouseMove) {
            auto* me = static_cast<QMouseEvent*>(event);
            if ((me->buttons() & Qt::LeftButton)
                && !m_dragBtnStartPos.isNull()
                && (me->pos() - m_dragBtnStartPos).manhattanLength()
                       >= QApplication::startDragDistance()) {
                m_dragBtnStartPos = QPoint();
                startSourceDrag(watched == m_btnDragVideo
                                    ? SourceDragMode::VideoOnly
                                    : SourceDragMode::AudioOnly);
                return true;
            }
        }
    }
    return QWidget::eventFilter(watched, event);
}

void SourceMonitor::startSourceDrag(SourceDragMode mode)
{
    if (!m_hasClip) return;

    auto region = sourceRegion();
    auto* mimeData = new QMimeData;

    if (m_isSequence) {
        // Sequence drag-out: encode sequence index + in/out
        mimeData->setData("application/x-roundtable-sequence",
                          QByteArray::number(qulonglong(m_sequenceIndex)));
        mimeData->setData("application/x-roundtable-sequence-duration",
                          QByteArray::number(qlonglong(m_clipDuration)));
        mimeData->setData("application/x-roundtable-source-in",
                          QByteArray::number(qlonglong(region.sourceIn)));
        mimeData->setData("application/x-roundtable-source-out",
                          QByteArray::number(qlonglong(region.sourceOut)));
        mimeData->setData("application/x-roundtable-sequence-has-audio",
                          QByteArray(m_seqHasAudio ? "1" : "0"));
    } else {
        auto filePath = m_mediaSources
            ? m_mediaSources->sourceInfo(m_mediaHandle).value().path
            : (m_pool ? m_pool->getPath(m_mediaHandle) : std::filesystem::path());
        if (filePath.empty()) { delete mimeData; return; }

        mimeData->setData("application/x-roundtable-media",
                          QByteArray::number(qulonglong(m_mediaHandle)));
        mimeData->setUrls({QUrl::fromLocalFile(
            QString::fromStdString(filePath.string()))});

        // Attach source in/out so the timeline can trim the clip
        mimeData->setData("application/x-roundtable-source-in",
                          QByteArray::number(qlonglong(region.sourceIn)));
        mimeData->setData("application/x-roundtable-source-out",
                          QByteArray::number(qlonglong(region.sourceOut)));
    }

    // Premiere-style video-only / audio-only drag flag. Absent = both.
    const char* modeStr = (mode == SourceDragMode::VideoOnly) ? "video"
                        : (mode == SourceDragMode::AudioOnly) ? "audio"
                        : nullptr;
    if (modeStr)
        mimeData->setData("application/x-roundtable-drag-mode",
                          QByteArray(modeStr));

    // Create a clean drag pixmap (Premiere-style pill)
    QString label = m_clipLabel ? m_clipLabel->text()
                                : QStringLiteral("Source");
    if (mode == SourceDragMode::VideoOnly)      label += QStringLiteral(" (V)");
    else if (mode == SourceDragMode::AudioOnly) label += QStringLiteral(" (A)");
    QFontMetrics fm(font());
    int textW = fm.horizontalAdvance(label);
    int pillW = qBound(120, textW + 24, 300);
    int pillH = 28;
    QPixmap pix(pillW, pillH);
    pix.fill(Qt::transparent);
    {
        const auto& tc = Theme::colors();
        QPainter p(&pix);
        p.setRenderHint(QPainter::Antialiasing);
        p.setBrush(QColor(tc.surface2));
        p.setPen(Qt::NoPen);
        p.drawRoundedRect(0, 0, pillW, pillH, 4, 4);
        // accent stripe – purple for sequences, blue for media
        p.setBrush(m_isSequence ? QColor("#7B68EE")
                                : QColor(tc.accent));
        p.drawRoundedRect(0, 0, 4, pillH, 2, 2);
        // text
        p.setPen(QColor(tc.text));
        p.drawText(QRect(10, 0, pillW - 14, pillH),
                   Qt::AlignVCenter | Qt::AlignLeft,
                   fm.elidedText(label, Qt::ElideRight, pillW - 18));
    }

    auto* drag = new QDrag(this);
    drag->setMimeData(mimeData);
    drag->setPixmap(pix);
    drag->setHotSpot(QPoint(pillW / 2, pillH / 2));
    drag->exec(Qt::CopyAction);
}

// Accept any drag that carries either our custom MIME, a tree-widget
// source, or a file URL. The URL clause is what makes NikkeBKG (whose
// BackgroundGridWidget is a QListWidget and only sets URLs +
// application/x-roundtable-asset) actually deliver a drop event; before
// this, dragEnterEvent silently rejected it and the user saw nothing.
// Also covers spine character drags (spine:URI), which the dropEvent
// dispatches to the spine-load path below.
static bool sourceMonitorAcceptsDrag(QDropEvent* e)
{
    const auto* md = e->mimeData();
    return md->hasFormat("application/x-roundtable-media") ||
           md->hasFormat("application/x-roundtable-sequence") ||
           md->hasFormat("application/x-roundtable-asset")   ||
           md->hasUrls()                                      ||
           qobject_cast<QTreeWidget*>(e->source());
}

void SourceMonitor::dragEnterEvent(QDragEnterEvent* event)
{
    if (sourceMonitorAcceptsDrag(event))
        event->acceptProposedAction();
}

void SourceMonitor::dragMoveEvent(QDragMoveEvent* event)
{
    if (sourceMonitorAcceptsDrag(event))
        event->acceptProposedAction();
}

void SourceMonitor::dropEvent(QDropEvent* event)
{
    // Opens a file path in the pool and loads it (Color Mattes are
    // generated PNGs that may not yet have a pool handle, so a drag only
    // carries their file URL). Returns true on success.
    auto loadFromPath = [this](const QString& path) -> bool {
        if (path.isEmpty() || !m_pool) return false;
        // Drop sources sometimes carry a relative path (NikkeBKG stored
        // "assets/NikkeBKG/foo.png"). Both MediaSourceService and
        // MediaPool canonicalise inputs and silently fail when the
        // resolved file doesn't exist relative to the process CWD. Make
        // the path absolute up-front so the drop works regardless of
        // where the app was launched from.
        std::filesystem::path p(path.toStdWString());
        if (p.is_relative()) {
            std::error_code ec;
            auto abs = std::filesystem::absolute(p, ec);
            if (!ec) p = std::move(abs);
        }
        // Prefer MediaSourceService — its handle is what updateFrameDisplay's
        // primary requestFrame() path keys off. A handle opened only via the
        // raw MediaPool isn't recognised by m_mediaSources, so the first
        // frame request after drop comes back empty and the viewport stays
        // blank until the user presses play (the bug the user described
        // for Characters / NikkeBKG / "sometimes Backgrounds").
        uint64_t h = 0;
        if (m_mediaSources) {
            auto res = m_mediaSources->openSource({p, RenderRequestType::Still, false});
            h = res.handle;
        }
        if (h == 0)
            h = m_pool->open(p);
        if (h == 0) return false;
        loadClip(h, m_pool);
        // Stills can miss the cache on the very first synchronous request.
        // A deferred second paint guarantees the frame lands once decoded.
        QTimer::singleShot(0, this, [this]() {
            if (!m_destroying.load(std::memory_order_acquire))
                updateFrameDisplay();
        });
        return true;
    };
    auto firstLocalFile = [](const QDropEvent* e) -> QString {
        for (const QUrl& u : e->mimeData()->urls())
            if (u.isLocalFile()) return u.toLocalFile();
        return {};
    };

    // Try custom mime type first (from ThumbnailGrid / BinTreeWidget)
    if (event->mimeData()->hasFormat("application/x-roundtable-media")) {
        bool ok = false;
        uint64_t handle = event->mimeData()->data("application/x-roundtable-media")
                              .toULongLong(&ok);

        if (ok && handle != 0) {
            if (m_pool) {
                loadClip(handle, m_pool);
                // Stills can miss the cache on the very first synchronous
                // request — same reason loadFromPath schedules a deferred
                // repaint below. Without this, dropping an image from the
                // bin shows a blank viewport until the user drops again.
                QTimer::singleShot(0, this, [this]() {
                    if (!m_destroying.load(std::memory_order_acquire))
                        updateFrameDisplay();
                });
            }
            else
                emit dropReceived(handle);
            event->acceptProposedAction();
            return;
        }

        // No valid pool handle (Library item / Color Matte). Try the file
        // URL the drag carries. If that doesn't load (e.g. the multi-select
        // drag packed a comma handle list and no usable URL, or a
        // Videos/Audio item whose URL round-trip differs), DON'T dead-end —
        // fall through to the QTreeWidget-source and bare-URL fallbacks
        // below, which read the dragged item's path directly.
        if (loadFromPath(firstLocalFile(event))) {
            event->acceptProposedAction();
            return;
        }
        // fall through (no return)
    }

    // Sequence via custom MIME (drag from another source)
    if (event->mimeData()->hasFormat("application/x-roundtable-sequence")) {
        bool ok = false;
        size_t seqIdx = event->mimeData()->data("application/x-roundtable-sequence")
                            .toULongLong(&ok);
        if (ok) {
            emit sequenceDropReceived(seqIdx);
            event->acceptProposedAction();
        }
        return;
    }

    // Fallback: QTreeWidget default drag provides model data but no custom mime.
    // The source QTreeWidget item has media handle in UserRole+1.
    if (auto* tree = qobject_cast<QTreeWidget*>(event->source())) {
        auto* item = tree->currentItem();
        if (!item) return;
        // Skip bins
        if (item->data(0, Qt::UserRole + 2).toBool()) return;

        // Handle sequences from the tree
        if (item->data(0, Qt::UserRole + 3).toBool()) {
            size_t seqIdx = item->data(0, Qt::UserRole + 4).toULongLong();
            emit sequenceDropReceived(seqIdx);
            event->acceptProposedAction();
            return;
        }

        uint64_t handle = item->data(0, Qt::UserRole + 1).toULongLong();
        if (handle == 0) {
            const QString role = item->data(0, Qt::UserRole).toString();
            // Spine character animation drag (CharactersPanel). The payload
            // isn't a real file path — route to TimelineWorkspace which
            // owns the CompositeService needed for live spine rendering.
            if (role.startsWith(QStringLiteral("spine:"))) {
                emit spineDropReceived(role);
                event->acceptProposedAction();
                return;
            }
            // Color Matte / generated asset with no pool handle yet —
            // open it from its stored path.
            if (loadFromPath(role))
                event->acceptProposedAction();
            return;
        }

        if (m_pool) {
            loadClip(handle, m_pool);
            // See note above in the custom-mime branch — stills miss the
            // first synchronous frame request; a deferred repaint guarantees
            // they appear on the initial drop.
            QTimer::singleShot(0, this, [this]() {
                if (!m_destroying.load(std::memory_order_acquire))
                    updateFrameDisplay();
            });
        }
        else
            emit dropReceived(handle);

        event->acceptProposedAction();
        return;
    }

    // Last resort: a plain file-URL drop (Color Matte PNG, external file).
    if (event->mimeData()->hasUrls() && loadFromPath(firstLocalFile(event)))
        event->acceptProposedAction();
}

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
