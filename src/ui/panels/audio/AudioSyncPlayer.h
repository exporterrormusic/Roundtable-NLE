/*
 * AudioSyncPlayer.h — AudioSync's turns on the app's shared AudioEngine.
 *
 * Whatever AudioSync plays goes through here: a sync clip (trimmed, deleted
 * regions cut out — audiosync::ClipPlayback), a take or rendered-media
 * preview (COMFYUI page), an imported file (IMPORT / TRANSCRIBE lists), a
 * TTS draft, or a scrub burst. While AudioSync plays, the timeline's sync
 * clock is detached so the timeline playhead never moves; stopping gives it
 * back. The buffers the engine reads live here until their sources are
 * cleared.
 *
 * Only the engine side: AudioSync keeps what is playing (clip / file index),
 * the playhead timers and every widget update.
 */

#pragma once

#include "audiosync/ClipPlayback.h"
#include "audiosync/SyncClip.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace rt {

class AudioEngine;
class AVSyncClock;

class AudioSyncPlayer
{
public:
    void setEngine(AudioEngine* engine) noexcept { m_engine = engine; }
    [[nodiscard]] AudioEngine* engine() const noexcept { return m_engine; }
    /// The engine's transport is playing / paused.
    [[nodiscard]] bool isPlaying() const;
    [[nodiscard]] bool isPaused() const;

    /// Stop whatever plays (clip, preview, draft) and give the engine back.
    void stop();
    void pause();
    void resume();

    // ── Sync clips ──────────────────────────────────────────────────────
    /// Play `clip` from source time `fromTime`. False when it has nothing
    /// to play (no engine, or no samples in its range). The caller stops
    /// other playback first.
    bool playClip(const AudioSampleData& audio, const SyncClip& clip, double fromTime);
    /// Source time of the engine position in the playing clip; `fallback`
    /// past its end.
    [[nodiscard]] double clipTime(double fallback) const;
    /// Seek the playing clip. A time inside a deleted region moves to the
    /// region's end; returns the time sought.
    double seekClip(const SyncClip& clip, double time);
    /// A short burst of `audio` at `time` (a clip that is not playing).
    void scrub(const AudioSampleData& audio, double time);

    // ── Take / media preview ────────────────────────────────────────────
    /// A preview of `key` is loaded and active.
    [[nodiscard]] bool hasPreview(const std::string& key) const;
    /// Load [from, to) of `data` (resampled to the engine rate) as the
    /// preview `key`. The caller stops other playback first.
    bool loadPreview(const std::string& key, const AudioSampleData& data, double from, double to);
    /// A rendered media file's mono samples (decoded once; a few are kept).
    [[nodiscard]] const AudioSampleData* mediaAudio(const std::string& path);
    bool playPreview(double fromSec, double speed);
    void scrubPreview(double atSec);
    void pausePreview();
    void stopPreview();
    /// Source time of a playing preview, -1 when none plays.
    [[nodiscard]] double previewTime() const;
    [[nodiscard]] bool previewActive() const noexcept { return m_previewActive; }

    // ── Imported files ──────────────────────────────────────────────────
    /// Play a whole imported file from `fromFrame` (its own rate). The
    /// caller stops other playback first.
    void playFile(const AudioSampleData& audio, int64_t fromFrame);
    /// Stop a file preview and give the engine back.
    void stopFile();

    // ── TTS drafts ──────────────────────────────────────────────────────
    /// Play interleaved samples at `sampleRate`. The caller stops other
    /// playback first.
    bool playDraft(std::vector<float> samples, uint16_t channels, uint32_t sampleRate);
    [[nodiscard]] bool draftActive() const noexcept { return m_draftActive; }

private:
    /// Detach the timeline's clock (once) so play/seek never move it.
    void detachSyncClock();
    void restoreSyncClock();
    void installPreview();

    AudioEngine* m_engine{nullptr};
    AVSyncClock* m_savedSyncClock{nullptr};

    audiosync::ClipPlayback m_clip;

    // Take preview: its own buffer, full take audio from `m_previewFrom`.
    std::vector<float> m_previewBuffer;
    double m_previewFrom{0.0};
    double m_previewEnd{0.0};
    double m_previewRate{48000.0};
    std::string m_previewKey;
    bool m_previewActive{false};
    std::unordered_map<std::string, AudioSampleData> m_mediaAudioCache;

    std::vector<float> m_draftSamples;
    bool m_draftActive{false};
};

} // namespace rt
