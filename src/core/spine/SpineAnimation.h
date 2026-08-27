/*
 * SpineAnimation — manages animation state for a Spine skeleton.
 *
 * Wraps spine-cpp's AnimationState with multi-track support:
 *   Track 0 — body animation  (idle, aim, cover, action, special)
 *   Track 1 — talk overlay    (mixed additively on top of body)
 *
 * Provides:
 *   - Animation listing from skeleton data
 *   - Talk-animation auto-detection (talk_start > mouth > talk_loop > speak > "talk")
 *   - Frame-accurate pose evaluation (video/export mode)
 *   - Real-time update (preview mode)
 */

#pragma once

#ifdef ROUNDTABLE_HAS_SPINE

#include <memory>
#include <string>
#include <vector>
#include <cstdint>

namespace spine {
    class Skeleton;
    class SkeletonData;
    class AnimationState;
    class AnimationStateData;
    class Animation;
    class TrackEntry;
}

namespace rt {

/// Describes a single animation within a skeleton
struct AnimationInfo
{
    std::string name;
    float       duration = 0.0f;  ///< seconds
};

/// Track indices for multi-track animation
enum class AnimTrack : size_t
{
    Body = 0,  ///< Main body animation (idle, aim, cover, etc.)
    Talk = 1   ///< Talk overlay (additive blend)
};

class SpineAnimation
{
public:
    SpineAnimation();
    ~SpineAnimation();

    SpineAnimation(const SpineAnimation&) = delete;
    SpineAnimation& operator=(const SpineAnimation&) = delete;
    SpineAnimation(SpineAnimation&&) noexcept;
    SpineAnimation& operator=(SpineAnimation&&) noexcept;

    /// Initialize with a skeleton instance and its data.
    /// @param skeleton   Owned externally — must outlive this object.
    /// @param skelData   The skeleton data (for AnimationStateData).
    /// @param defaultMix Default crossfade duration in seconds.
    void init(spine::Skeleton* skeleton, spine::SkeletonData* skelData, float defaultMix = 0.2f);

    /// @return true if animation state is initialized
    [[nodiscard]] bool isInitialized() const noexcept { return m_animState != nullptr; }

    // ── Animation queries ───────────────────────────────────────────────

    /// List all animations in the skeleton data
    [[nodiscard]] std::vector<AnimationInfo> listAnimations() const;

    /// Find an animation by name (returns nullptr if not found)
    [[nodiscard]] spine::Animation* findAnimation(const std::string& name) const;

    /// Check if an animation exists
    [[nodiscard]] bool hasAnimation(const std::string& name) const;

    // ── Body animation (Track 0) ────────────────────────────────────────

    /// Set the body animation (track 0)
    /// @param name  Animation name (e.g., "idle", "aim", "cover_idle")
    /// @param loop  Whether the animation loops
    void setBodyAnimation(const std::string& name, bool loop = true);

    /// Atomically replace the timeline-driven animation state. Unlike
    /// setBodyAnimation()/stopTalking(), this removes every previous track
    /// entry and mixing chain before installing the requested body/talk
    /// tracks. Absolute-time timeline evaluation does not advance Spine's
    /// crossfade clock, so undo/redo and other state restoration must use
    /// this path to avoid stale pose fragments surviving in the new state.
    void replacePlaybackState(const std::string& bodyAnimation,
                              bool loop, bool talking);

    /// Get the current body animation name
    [[nodiscard]] std::string currentBodyAnimation() const;

    // ── Talk animation (Track 1) ─────────────────────────────────────────

    /// Start talking — auto-selects the best talk animation
    void startTalking();

    /// Stop talking — clears track 1 with mix-out
    void stopTalking();

    /// @return true if talk overlay is active
    [[nodiscard]] bool isTalking() const noexcept { return m_talking; }

    /// Auto-detect the best talk animation from the skeleton data.
    /// Priority: talk_start > contains "mouth" > talk_loop/speak_loop/idle_talk
    ///           > any with "talk"/"speak" in name
    [[nodiscard]] std::string detectTalkAnimation() const;

    // ── Time control ────────────────────────────────────────────────────

    /// Update animation by delta time (real-time preview mode)
    /// @param dt  Delta time in seconds
    void update(float dt);

    /// Evaluate pose at an exact time (video/export mode).
    /// Resets to setup pose, then applies animation at the given time.
    /// @param bodyTime  Time in seconds for body animation
    /// @param talkTime  Time in seconds for talk animation (if talking)
    void evaluateAtTime(float bodyTime, float talkTime = 0.0f);

    /// Apply the current animation state to the skeleton
    void apply();

    // ── Configuration ───────────────────────────────────────────────────

    /// Set animation speed multiplier
    void setSpeed(float speed) noexcept { m_speed = speed; }
    [[nodiscard]] float speed() const noexcept { return m_speed; }

    /// Set the mix duration for talk overlay fade in/out
    void setTalkMixDuration(float seconds) noexcept { m_talkMixDuration = seconds; }

    /// Get the underlying spine AnimationState (for advanced usage)
    [[nodiscard]] spine::AnimationState* getAnimationState() const noexcept { return m_animState.get(); }

private:
    struct AnimStateDeleter { void operator()(spine::AnimationState* p) const; };
    struct AnimStateDataDeleter { void operator()(spine::AnimationStateData* p) const; };

    /// If a looping track entry has landed within a fraction of a frame of its
    /// loop boundary (animationTime ≈ start), nudge it past the seam so
    /// absolute-time seeking doesn't sample the t=0 keyframe and pop a slot for
    /// one frame.  No-op for non-looping entries.  See evaluateAtTime().
    static void nudgeOffLoopSeam(spine::TrackEntry* entry);

    spine::Skeleton*                                          m_skeleton = nullptr;
    spine::SkeletonData*                                      m_skelData = nullptr;
    std::unique_ptr<spine::AnimationStateData, AnimStateDataDeleter> m_animStateData;
    std::unique_ptr<spine::AnimationState, AnimStateDeleter>         m_animState;

    bool  m_talking         = false;
    float m_speed           = 1.0f;
    float m_talkMixDuration = 0.15f;
};

} // namespace rt

#endif // ROUNDTABLE_HAS_SPINE
