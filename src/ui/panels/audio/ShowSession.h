/*
 * ShowSession — what the COMFYUI page needs from the open shot-table script:
 * its lines and picked takes, the per-show pipeline settings, and take
 * playback / trimming.
 *
 * The AUDIO page (AudioSync) implements it. The COMFYUI page depends only on
 * this interface, not on the AUDIO page's widget, so it can be driven by a
 * stand-in in tests and the session can later move out of the widget without
 * touching the COMFYUI page.
 */

#pragma once

#include "ai/ShotPipeline.h"
#include "audiosync/SyncClip.h"

#include <QString>

#include <string>
#include <vector>

namespace rt {

/// One script line of a shot-table script as the COMFYUI page sees it.
struct ShowShot
{
    int         lineNumber{-1};
    std::string dialogue;
    std::string action;
    std::string segment;
    std::string shot;        ///< effectiveShot(): doc value, in-app pick, or empty
    std::string key;         ///< pipeline key: ScriptMatcher::normalize(dialogue), + " #n" for a repeated line
    bool        hasTake{false};
    SyncClip    take;        ///< the CONFIRMED take (valid when hasTake)
};

class ShowSession
{
public:
    virtual ~ShowSession() = default;

    [[nodiscard]] virtual bool isShotScript() const = 0;
    [[nodiscard]] virtual QString activeScriptName() const = 0;
    /// Every line of the active shot-table script in script order, with its
    /// picked take (if any).
    [[nodiscard]] virtual std::vector<ShowShot> showShots() const = 0;
    /// Framing / per-shot settings of the active script. Saved with the
    /// project; edits need no extra notification.
    [[nodiscard]] virtual ShowPipelineState& pipeline() noexcept = 0;
    [[nodiscard]] virtual const ShowPipelineState& pipeline() const noexcept = 0;

    /// Play a take (by SyncClip::id) in the take player and adjust its in/out
    /// points (one undo step).  Returns true if the in/out changed.
    virtual bool openTakePlayerForClipId(int clipId) = 0;
    /// Set a take's in/out (seconds in its source) as one undo step.
    virtual bool setTakeInOut(int clipId, double start, double end) = 0;

    // ── Preview audio (SHOTS transport) ─────────────────────────────────
    // Load a source range once, then play it at any speed (reverse and slow
    // included — the engine time-stretches) or scrub it.
    /// Load [rangeStart, rangeEnd] of a take's source audio. False if none.
    virtual bool loadPreviewAudioForTake(int clipId, double rangeStart, double rangeEnd) = 0;
    /// Load a media file's own audio (e.g. a ComfyUI render), decoded once.
    virtual bool loadPreviewAudioForMedia(const std::string& path) = 0;
    /// Play the loaded range from `fromSec` at `speed` (1 = normal, <0 = reverse).
    virtual bool playPreviewAudio(double fromSec, double speed) = 0;
    /// A short burst at `atSec` (scrub / frame step).
    virtual void scrubPreviewAudio(double atSec) = 0;
    /// Pause without unloading (the next play / scrub is instant).
    virtual void pausePreviewAudio() = 0;
    virtual void stopTakeAudio() = 0;
    /// Source time being heard while playing, or -1 when stopped.
    [[nodiscard]] virtual double takeAudioTime() const = 0;
};

} // namespace rt
