/*
 * PlaybackController.cpp — Transport & playback implementation.
 */

#include "playback/PlaybackController.h"

#include "timeline/Timeline.h"
#include "timeline/Clip.h"
#include "timeline/EditOperations.h"
#include "playback/AVSyncClock.h"
#include "audio/AudioEngine.h"
#include "FrameTime.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>

namespace rt {

namespace {

/// Frame grid for a transport rate; unset rates fall back to 24 fps.
RationalFrameRate transportFrameGrid(double fps) noexcept
{
    return canonicalFrameRate(fps > 0.0 ? fps : 24.0);
}

} // namespace

// ═════════════════════════════════════════════════════════════════════════════
//  Timecode helpers
// ═════════════════════════════════════════════════════════════════════════════

std::string Timecode::toString() const
{
    char buf[16]{};
    std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d:%02d",
                  hours, minutes, seconds, frames);
    return buf;
}

Timecode tickToTimecode(int64_t tick, double fps) noexcept
{
    if (fps <= 0.0) fps = 24.0;
    if (tick < 0) tick = 0;

    Timecode tc;
    // The frame on screen at `tick`. Truncating seconds * fps labelled a
    // playhead parked exactly on an NTSC frame start (29.97 frame 2 = tick
    // 3203 = 1.9999 frames) as the frame before it.
    const int64_t totalFrames = tickToFrameIndex(tick, transportFrameGrid(fps));

    // Non-drop-frame labels: count frames at the nominal rate (30 for 29.97).
    int fpsi = static_cast<int>(std::round(fps));
    if (fpsi <= 0) fpsi = 24;

    tc.frames  = static_cast<int>(totalFrames % fpsi);
    int64_t remainingSec = totalFrames / fpsi;
    tc.seconds = static_cast<int>(remainingSec % 60);
    int64_t remainingMin = remainingSec / 60;
    tc.minutes = static_cast<int>(remainingMin % 60);
    tc.hours   = static_cast<int>(remainingMin / 60);

    return tc;
}

int64_t timecodeToTick(const Timecode& tc, double fps) noexcept
{
    if (fps <= 0.0) fps = 24.0;

    // Inverse of tickToTimecode: a non-drop label counts frames at the
    // nominal rate, so 00:00:01:00 at 29.97 is frame 30, not 1.0 s.
    int fpsi = static_cast<int>(std::round(fps));
    if (fpsi <= 0) fpsi = 24;
    const int64_t totalSeconds =
        (static_cast<int64_t>(tc.hours) * 60 + tc.minutes) * 60 + tc.seconds;
    const int64_t frame = totalSeconds * fpsi + tc.frames;
    return frameIndexToTick(std::max<int64_t>(0, frame), transportFrameGrid(fps));
}

// ═════════════════════════════════════════════════════════════════════════════
//  PlaybackController
// ═════════════════════════════════════════════════════════════════════════════

PlaybackController::PlaybackController() = default;
PlaybackController::~PlaybackController()
{
    m_destroying.store(true);
}

// ── Dependencies ────────────────────────────────────────────────────────────

void PlaybackController::setTimeline(Timeline* timeline) noexcept
{
    m_timeline = timeline;
}

void PlaybackController::setAudioEngine(AudioEngine* engine) noexcept
{
    m_audioEngine = engine;
}

void PlaybackController::setSyncClock(AVSyncClock* clock) noexcept
{
    m_syncClock = clock;
}

void PlaybackController::setFrameRate(double fps) noexcept
{
    if (fps > 0.0)
        m_fps = fps;
}

void PlaybackController::setStandaloneDuration(int64_t ticks) noexcept
{
    m_standaloneDuration = ticks;
}

// ── Transport controls ──────────────────────────────────────────────────────

void PlaybackController::play()
{
    // Log and flush BEFORE the play call.  If the app crashes during
    // play(), this message will be the last thing in the log.
    spdlog::info("[PLAY] PlaybackController::play() called (tick={})", currentTick());
    spdlog::default_logger()->flush();

    if (m_destroying.load(std::memory_order_acquire))
        return;
    if (m_state == PlayState::Playing)
        return;

    m_shuttleSpeed = 1.0;
    m_jShuttleLevel = 0;
    m_lShuttleLevel = 0;
    m_lastPollTimeNs = 0;

    int64_t startTick = currentTick();

    // Trigger pre-buffer callback BEFORE setState
    if (onPlayStarting && !m_destroying.load(std::memory_order_acquire))
        onPlayStarting(startTick);

    // Set state — fires onStateChanged → loadAudioSources().
    setState(PlayState::Playing);

    // Sync the audio engine's position with the captured playhead tick
    if (m_audioEngine) {
        // Drain any scrub callback before seeking. Otherwise that callback can
        // publish its old position between seekToFrame() and play().
        m_audioEngine->pause();
        m_audioEngine->setPlaybackSpeed(1.0);
        uint32_t sr = m_audioEngine->sampleRate();
        int64_t frame = static_cast<int64_t>(
            static_cast<double>(startTick) / 48000.0 * sr);
        m_audioEngine->seekToFrame(frame);
    }

    // Reset the clock position
    if (m_syncClock) {
        m_syncClock->reset(startTick);
        m_syncClock->setSpeed(1.0);
    }

    // Start audio engine — this also starts the sync clock running
    if (m_audioEngine)
        m_audioEngine->play();

    // Fallback: if there's no audio engine
    if (!m_audioEngine && m_syncClock)
        m_syncClock->setRunning(true);

    spdlog::info("[PLAY] PlaybackController::play() completed successfully");
    spdlog::default_logger()->flush();
}

void PlaybackController::pause()
{
    if (m_destroying.load(std::memory_order_acquire))
        return;
    if (m_state == PlayState::Paused || m_state == PlayState::Stopped)
        return;

    // Capture the extrapolated position BEFORE stopping the clock.
    // AVSyncClock::currentTick() returns wall-clock-extrapolated ticks
    // while running, but falls back to the raw m_tick (audio-driven)
    // once stopped.  At shuttle speeds >1x the extrapolated position
    // can be several frames ahead of m_tick, so stopping first would
    // cause the playhead to visibly jump backward.
    // Park on the start of the frame on screen: the picture is unchanged,
    // and edits made at the playhead (razor, insert) land on a frame
    // boundary instead of mid-frame.
    const int64_t pauseTick = frameStartAtOrBefore(currentTick(), transportFrameGrid(m_fps));

    m_shuttleSpeed = 0.0;
    m_jShuttleLevel = 0;
    m_lShuttleLevel = 0;

    if (m_syncClock)
        m_syncClock->setRunning(false);

    if (m_audioEngine)
    {
        m_audioEngine->setPlaybackSpeed(1.0);  // Reset to normal speed
        m_audioEngine->pause();
    }

    // Anchor clock + timeline + audio at the tick that was displayed
    // at the moment the user pressed pause.
    seekInternal(pauseTick);

    m_lastPollTimeNs = 0;
    setState(PlayState::Paused);
}

void PlaybackController::togglePlayPause()
{
    if (m_state == PlayState::Playing || m_state == PlayState::Shuttling)
        pause();
    else
        play();
}

void PlaybackController::stop()
{
    if (m_destroying.load(std::memory_order_acquire))
        return;
    m_shuttleSpeed = 0.0;
    m_jShuttleLevel = 0;
    m_lShuttleLevel = 0;

    if (m_syncClock)
    {
        m_syncClock->setRunning(false);
    }

    if (m_audioEngine)
        m_audioEngine->stop();

    m_lastPollTimeNs = 0;

    // Return to in-point if set, otherwise to start
    int64_t returnTo = 0;
    if (m_timeline && m_timeline->inPoint() >= 0)
        returnTo = m_timeline->inPoint();

    seekInternal(returnTo);
    setState(PlayState::Stopped);
}

void PlaybackController::seekTo(int64_t tick)
{
    if (m_destroying.load(std::memory_order_acquire))
        return;
    tick = std::max<int64_t>(0, tick);

    // Reset shuttle speed so next L press starts at 1×
    m_shuttleSpeed = 0.0;
    m_jShuttleLevel = 0;
    m_lShuttleLevel = 0;
    if (m_audioEngine)
        m_audioEngine->setPlaybackSpeed(1.0);
    if (m_state == PlayState::Shuttling)
        setState(PlayState::Paused);

    // NOTE: We do NOT pause during Playing state.  The audio callback's
    // seek-generation guard (m_seekGeneration) prevents the position
    // advance from overwriting the new seek position.  seekInternal()
    // resets the sync clock atomically.  Pausing here would permanently
    // halt playback with no auto-resume mechanism.

    seekInternal(tick);

    if (onPositionChanged)
        onPositionChanged(tick);
}

// ── Frame stepping ──────────────────────────────────────────────────────────

void PlaybackController::stepForward()
{
    // Pause if playing
    if (m_state == PlayState::Playing || m_state == PlayState::Shuttling)
        pause();

    // Next frame START on the canonical grid (NTSC frames alternate
    // 1601/1602 ticks; adding a fixed step drifts off it).
    int64_t newTick = stepFrames(currentTick(), 1, transportFrameGrid(m_fps));

    // Clamp to timeline duration (only if timeline has content)
    if (m_timeline && m_timeline->duration() > 0)
        newTick = std::min(newTick, m_timeline->duration());

    seekInternal(newTick);

    if (onPositionChanged)
        onPositionChanged(newTick);
}

void PlaybackController::stepBackward()
{
    if (m_state == PlayState::Playing || m_state == PlayState::Shuttling)
        pause();

    const int64_t newTick = stepFrames(currentTick(), -1, transportFrameGrid(m_fps));

    seekInternal(newTick);

    if (onPositionChanged)
        onPositionChanged(newTick);
}

// ── JKL Shuttle ─────────────────────────────────────────────────────────────

void PlaybackController::shuttleReverse()
{
    if (m_destroying.load(std::memory_order_acquire))
        return;
    // If currently moving forward, first stop, then go reverse
    if (m_lShuttleLevel > 0)
    {
        m_lShuttleLevel = 0;
        m_jShuttleLevel = 0;
    }

    // Increment reverse shuttle level (capped at 3 → 4x)
    m_jShuttleLevel = std::min(m_jShuttleLevel + 1, 3);

    // Speed: -1, -2, -4
    double speed = -std::pow(2.0, m_jShuttleLevel - 1);
    m_shuttleSpeed = speed;

    // Derive anchor from audio engine's actual position (not wall-clock
    // extrapolated currentTick()) to avoid accumulating drift on each
    // speed change.  currentTick() extrapolates beyond the last audio
    // callback, so using it as anchor shifts both clock and audio forward
    // by up to one buffer (~10-40ms depending on speed).
    int64_t startTick;
    if (isPlaying() && m_audioEngine && m_audioEngine->sampleRate() > 0) {
        uint32_t sr = m_audioEngine->sampleRate();
        int64_t frame = m_audioEngine->currentFrame();
        startTick = static_cast<int64_t>(
            static_cast<double>(frame) / sr * 48000.0);
    } else {
        startTick = currentTick();
    }

    if (m_syncClock)
    {
        m_syncClock->reset(startTick);
        m_syncClock->setSpeed(speed);
        // Don't setRunning until audio starts (same as play()).
    }

    // Set audio engine playback speed for reverse/variable playback.
    // WSOLA time-stretch preserves pitch at any speed.
    if (m_audioEngine)
    {
        m_audioEngine->setPlaybackSpeed(speed);
        if (!isPlaying()) {
            const auto frame = static_cast<int64_t>(
                static_cast<double>(startTick) / 48000.0 * m_audioEngine->sampleRate());
            m_audioEngine->seekToFrame(frame);
        }
        m_audioEngine->play();
    }

    // Fallback: if there's no audio engine, start the clock ourselves.
    if (!m_audioEngine && m_syncClock)
        m_syncClock->setRunning(true);

    setState(PlayState::Shuttling);
    if (onSpeedChanged)
        onSpeedChanged(m_shuttleSpeed);
}

void PlaybackController::shuttlePause()
{
    pause();
}

void PlaybackController::setShuttleSpeed(double speed)
{
    if (m_destroying.load(std::memory_order_acquire))
        return;
    if (speed == 0.0) {
        pause();
        return;
    }

    // Clear ramp counters so the next J/L tap re-enters at level 1.
    m_jShuttleLevel = 0;
    m_lShuttleLevel = 0;
    m_shuttleSpeed  = speed;

    int64_t startTick;
    if (isPlaying() && m_audioEngine && m_audioEngine->sampleRate() > 0) {
        uint32_t sr = m_audioEngine->sampleRate();
        int64_t frame = m_audioEngine->currentFrame();
        startTick = static_cast<int64_t>(
            static_cast<double>(frame) / sr * 48000.0);
    } else {
        startTick = currentTick();
    }

    if (m_syncClock) {
        m_syncClock->reset(startTick);
        m_syncClock->setSpeed(speed);
    }

    if (m_audioEngine) {
        m_audioEngine->setPlaybackSpeed(speed);
        if (!isPlaying()) {
            const auto frame = static_cast<int64_t>(
                static_cast<double>(startTick) / 48000.0 * m_audioEngine->sampleRate());
            m_audioEngine->seekToFrame(frame);
        }
        m_audioEngine->play();
    }

    if (!m_audioEngine && m_syncClock)
        m_syncClock->setRunning(true);

    setState(PlayState::Shuttling);
    if (onSpeedChanged)
        onSpeedChanged(m_shuttleSpeed);
}

void PlaybackController::shuttleForward()
{
    if (m_destroying.load(std::memory_order_acquire))
        return;
    // If currently moving reverse, first stop, then go forward
    if (m_jShuttleLevel > 0)
    {
        m_jShuttleLevel = 0;
        m_lShuttleLevel = 0;
    }

    // Increment forward shuttle level (capped at 3 → 4x)
    m_lShuttleLevel = std::min(m_lShuttleLevel + 1, 3);

    // Speed: 1, 2, 4
    double speed = std::pow(2.0, m_lShuttleLevel - 1);
    m_shuttleSpeed = speed;

    // Derive anchor from audio engine's actual position (not wall-clock
    // extrapolated currentTick()) to avoid accumulating drift on each
    // speed change.  currentTick() extrapolates beyond the last audio
    // callback, so using it as anchor shifts both clock and audio forward
    // by up to one buffer (~10-40ms depending on speed).
    int64_t startTick;
    if (isPlaying() && m_audioEngine && m_audioEngine->sampleRate() > 0) {
        uint32_t sr = m_audioEngine->sampleRate();
        int64_t frame = m_audioEngine->currentFrame();
        startTick = static_cast<int64_t>(
            static_cast<double>(frame) / sr * 48000.0);
    } else {
        startTick = currentTick();
    }

    if (m_syncClock)
    {
        m_syncClock->reset(startTick);
        m_syncClock->setSpeed(speed);
        // Don't setRunning until audio starts (same as play()).
    }

    // Set audio engine playback speed.
    // WSOLA time-stretch preserves pitch at any speed.
    if (m_audioEngine)
    {
        m_audioEngine->setPlaybackSpeed(speed);
        if (!isPlaying()) {
            const auto frame = static_cast<int64_t>(
                static_cast<double>(startTick) / 48000.0 * m_audioEngine->sampleRate());
            m_audioEngine->seekToFrame(frame);
        }
        m_audioEngine->play();
    }

    // Fallback: if there's no audio engine, start the clock ourselves.
    if (!m_audioEngine && m_syncClock)
        m_syncClock->setRunning(true);

    setState(PlayState::Shuttling);
    if (onSpeedChanged)
        onSpeedChanged(m_shuttleSpeed);
}

// ── Navigation ──────────────────────────────────────────────────────────────

// Premiere behaviour: Home/End (and the go-to-start/end buttons) stop
// playback and park the playhead — they never keep playing from the new spot.
void PlaybackController::goToStart()
{
    pause();
    seekTo(0);
}

void PlaybackController::goToEnd()
{
    pause();
    if (m_timeline)
        seekTo(m_timeline->duration());
    else if (m_standaloneDuration > 0)
        seekTo(m_standaloneDuration);
    else
        seekTo(0);
}

void PlaybackController::goToNextEditPoint()
{
    if (!m_timeline) return;

    int64_t next = EditOperations::nextEditPoint(*m_timeline, currentTick());
    seekTo(next);
}

void PlaybackController::goToPrevEditPoint()
{
    if (!m_timeline) return;

    int64_t prev = EditOperations::prevEditPoint(*m_timeline, currentTick());
    seekTo(prev);
}

void PlaybackController::goToNextGap()
{
    if (!m_timeline) return;
    seekTo(EditOperations::nextSequenceGap(*m_timeline, currentTick()));
}

void PlaybackController::goToPrevGap()
{
    if (!m_timeline) return;
    seekTo(EditOperations::prevSequenceGap(*m_timeline, currentTick()));
}

void PlaybackController::goToInPoint()
{
    if (!m_timeline) return;
    if (m_timeline->inPoint() >= 0)
        seekTo(m_timeline->inPoint());
}

void PlaybackController::goToOutPoint()
{
    if (!m_timeline) return;
    if (m_timeline->outPoint() >= 0)
        seekTo(m_timeline->outPoint());
}

// ── Loop ────────────────────────────────────────────────────────────────────

void PlaybackController::setLoopEnabled(bool enabled) noexcept
{
    m_loopEnabled = enabled;
}

// ── State queries ───────────────────────────────────────────────────────────

bool PlaybackController::isPlaying() const noexcept
{
    return m_state == PlayState::Playing || m_state == PlayState::Shuttling;
}

int64_t PlaybackController::currentTick() const noexcept
{
    // A shared audio clock may run for another transport while we are paused.
    // Only follow it while this controller owns active playback.
    if (isPlaying() && m_syncClock && m_syncClock->isRunning())
        return m_syncClock->currentTick();

    // Otherwise use the timeline's stored playhead
    if (m_timeline)
        return m_timeline->playheadPosition();

    // Standalone mode (Source Monitor)
    return m_standaloneTick;
}

Timecode PlaybackController::currentTimecode() const noexcept
{
    return tickToTimecode(currentTick(), m_fps);
}

std::string PlaybackController::currentTimecodeString() const
{
    return currentTimecode().toString();
}

int64_t PlaybackController::durationTicks() const noexcept
{
    return m_timeline ? m_timeline->duration() : 0;
}

// ── Polling ─────────────────────────────────────────────────────────────────

int64_t PlaybackController::pollPosition()
{
    if (m_destroying.load(std::memory_order_acquire))
        return currentTick();
    int64_t tick = currentTick();

    // Standalone time advancement (no sync clock / no timeline)
    if (!m_syncClock && !m_timeline && isPlaying())
    {
        auto now = std::chrono::steady_clock::now().time_since_epoch();
        int64_t nowNs = std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
        if (m_lastPollTimeNs > 0)
        {
            double elapsedSec = static_cast<double>(nowNs - m_lastPollTimeNs) / 1e9;
            int64_t deltaTicks = static_cast<int64_t>(elapsedSec * 48000.0 * m_shuttleSpeed);
            m_standaloneTick += deltaTicks;
            m_standaloneTick = std::max<int64_t>(0, m_standaloneTick);
            tick = m_standaloneTick;
        }
        m_lastPollTimeNs = nowNs;
    }

    // Apply loop boundaries — when the tick wraps, seek the clock and
    // audio engine to the wrapped position so everything stays in sync.
    if (isPlaying() && m_loopEnabled)
    {
        int64_t wrapped = applyLoopBounds(tick);
        if (wrapped != tick)
        {
            seekInternal(wrapped);
            tick = wrapped;
        }
    }

    // Clamp lower bound only — allow playhead past the last clip
    tick = std::max<int64_t>(0, tick);

    // Update timeline playhead
    if (m_timeline && tick != m_timeline->playheadPosition())
    {
        m_timeline->setPlayheadPosition(tick);
    }

    // Notify UI if position changed
    if (tick != m_lastPollTick)
    {
        m_lastPollTick = tick;
        if (onPositionChanged)
            onPositionChanged(tick);
    }

    // Auto-stop at end of timeline (forward)
    if (m_state == PlayState::Playing && !m_loopEnabled)
    {
        int64_t dur = m_timeline ? m_timeline->duration() : m_standaloneDuration;
        if (dur > 0 && tick >= dur)
        {
            pause();
        }
    }

    // Auto-stop at start of timeline (reverse shuttle)
    if (m_state == PlayState::Shuttling && m_shuttleSpeed < 0.0 && !m_loopEnabled)
    {
        if (tick <= 0)
        {
            pause();
        }
    }

    return tick;
}

// ── Private ─────────────────────────────────────────────────────────────────

int64_t PlaybackController::applyLoopBounds(int64_t tick) const
{
    if (!m_timeline) return tick;

    int64_t loopStart = 0;
    int64_t loopEnd = m_timeline->duration();

    // If in/out points are set, loop between them
    if (m_timeline->inPoint() >= 0 && m_timeline->outPoint() > m_timeline->inPoint())
    {
        loopStart = m_timeline->inPoint();
        loopEnd   = m_timeline->outPoint();
    }

    if (loopEnd <= loopStart) return tick;

    if (tick >= loopEnd)
    {
        // Wrap back to start
        int64_t loopLen = loopEnd - loopStart;
        tick = loopStart + ((tick - loopStart) % loopLen);
    }
    else if (tick < loopStart && m_shuttleSpeed < 0.0)
    {
        // Reverse loop: wrap to end
        tick = loopEnd;
    }

    return tick;
}

void PlaybackController::setState(PlayState newState)
{
    if (m_state == newState) return;
    if (m_destroying.load(std::memory_order_acquire)) return;
    m_state = newState;
    if (onStateChanged)
        onStateChanged(newState);
}

void PlaybackController::seekInternal(int64_t tick)
{
    tick = std::max<int64_t>(0, tick);

    // Update clock
    if (m_syncClock)
        m_syncClock->reset(tick);

    // Update audio engine position
    if (m_audioEngine)
    {
        uint32_t sr = m_audioEngine->sampleRate();
        int64_t frame = static_cast<int64_t>(
            static_cast<double>(tick) / 48000.0 * sr);
        m_audioEngine->seekToFrame(frame);
    }

    // Update timeline playhead
    if (m_timeline)
        m_timeline->setPlayheadPosition(tick);

    // Standalone position
    m_standaloneTick = tick;
    m_lastPollTick = tick;
}

} // namespace rt
