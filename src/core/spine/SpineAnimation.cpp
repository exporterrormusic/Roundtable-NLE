/*
 * SpineAnimation.cpp — Multi-track animation state management.
 */

#ifdef ROUNDTABLE_HAS_SPINE

#include "spine/SpineAnimation.h"

#include <spine/AnimationState.h>
#include <spine/AnimationStateData.h>
#include <spine/Animation.h>
#include <spine/Skeleton.h>
#include <spine/SkeletonData.h>
#include <spine/MixBlend.h>

#include <spdlog/spdlog.h>

#include <algorithm>

namespace rt {

// ─── Deleters ───────────────────────────────────────────────────────────────
void SpineAnimation::AnimStateDeleter::operator()(spine::AnimationState* p) const { delete p; }
void SpineAnimation::AnimStateDataDeleter::operator()(spine::AnimationStateData* p) const { delete p; }

// ─── Construction ───────────────────────────────────────────────────────────
SpineAnimation::SpineAnimation() = default;
SpineAnimation::~SpineAnimation() = default;
SpineAnimation::SpineAnimation(SpineAnimation&&) noexcept = default;

SpineAnimation& SpineAnimation::operator=(SpineAnimation&& other) noexcept
{
    if (this != &other) {
        // AnimationState references AnimationStateData internally, so
        // m_animState MUST be destroyed before m_animStateData.
        m_animState.reset();
        m_animStateData.reset();

        m_skeleton        = other.m_skeleton;
        m_skelData        = other.m_skelData;
        m_animStateData   = std::move(other.m_animStateData);
        m_animState       = std::move(other.m_animState);
        m_talking         = other.m_talking;
        m_speed           = other.m_speed;
        m_talkMixDuration = other.m_talkMixDuration;

        other.m_skeleton = nullptr;
        other.m_skelData = nullptr;
        other.m_talking  = false;
    }
    return *this;
}

// ─── Init ───────────────────────────────────────────────────────────────────
void SpineAnimation::init(spine::Skeleton* skeleton, spine::SkeletonData* skelData, float defaultMix)
{
    // Destroy old state in correct order (AnimationState before AnimationStateData)
    m_animState.reset();
    m_animStateData.reset();

    m_skeleton = skeleton;
    m_skelData = skelData;

    auto* stateData = new spine::AnimationStateData(skelData);
    stateData->setDefaultMix(defaultMix);
    m_animStateData.reset(stateData);

    m_animState.reset(new spine::AnimationState(stateData));

    spdlog::debug("SpineAnimation: initialized with {} animations",
                  skelData->getAnimations().size());
}

// ─── Animation queries ──────────────────────────────────────────────────────
std::vector<AnimationInfo> SpineAnimation::listAnimations() const
{
    std::vector<AnimationInfo> result;
    if (!m_skelData) return result;

    auto& anims = m_skelData->getAnimations();
    result.reserve(anims.size());
    for (size_t i = 0; i < anims.size(); ++i) {
        AnimationInfo info;
        info.name     = anims[i]->getName().buffer();
        info.duration = anims[i]->getDuration();
        result.push_back(std::move(info));
    }
    return result;
}

spine::Animation* SpineAnimation::findAnimation(const std::string& name) const
{
    if (!m_skelData) return nullptr;
    return m_skelData->findAnimation(spine::String(name.c_str()));
}

bool SpineAnimation::hasAnimation(const std::string& name) const
{
    return findAnimation(name) != nullptr;
}

// ─── Body animation (Track 0) ───────────────────────────────────────────────
void SpineAnimation::setBodyAnimation(const std::string& name, bool loop)
{
    if (!m_animState) return;

    auto* anim = findAnimation(name);
    if (!anim) {
        spdlog::warn("SpineAnimation: animation '{}' not found", name);
        return;
    }

    m_animState->setAnimation(
        static_cast<size_t>(AnimTrack::Body),
        anim,
        loop
    );

    spdlog::debug("SpineAnimation: body → '{}' (loop={})", name, loop);
}

void SpineAnimation::replacePlaybackState(const std::string& bodyAnimation,
                                          bool loop, bool talking)
{
    if (!m_animState || !m_skeleton) return;

    // setAnimation() normally links the outgoing TrackEntry through
    // mixingFrom. That is useful for a continuously-updated preview, but the
    // timeline seeks with evaluateAtTime(): trackTime is assigned directly
    // while mixTime is never advanced. The outgoing pose can therefore remain
    // partially applied after an undo/state restore (most visibly on small
    // face attachments such as Kilo's eyes). Remove the entire graph first.
    m_animState->clearTracks();
    m_talking = false;
    m_skeleton->setToSetupPose();

    if (!bodyAnimation.empty()) {
        if (auto* body = findAnimation(bodyAnimation)) {
            auto* entry = m_animState->setAnimation(
                static_cast<size_t>(AnimTrack::Body), body, loop);
            if (entry) entry->setMixDuration(0.0f);
        } else {
            spdlog::warn("SpineAnimation: animation '{}' not found",
                         bodyAnimation);
        }
    }

    if (talking) startTalking();
    m_skeleton->updateWorldTransform();

    spdlog::debug("SpineAnimation: playback state replaced body='{}' "
                  "loop={} talking={}", bodyAnimation, loop, talking);
}

std::string SpineAnimation::currentBodyAnimation() const
{
    if (!m_animState) return {};

    auto* entry = m_animState->getCurrent(static_cast<size_t>(AnimTrack::Body));
    if (!entry) return {};

    auto* anim = entry->getAnimation();
    return anim ? std::string(anim->getName().buffer()) : std::string{};
}

// ─── Talk animation (Track 1) ───────────────────────────────────────────────

std::string SpineAnimation::detectTalkAnimation() const
{
    if (!m_skelData) return {};

    auto& anims = m_skelData->getAnimations();

    // Helper: convert spine::String to lowercase std::string
    auto toLower = [](const spine::String& s) -> std::string {
        std::string str(s.buffer());
        std::transform(str.begin(), str.end(), str.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return str;
    };

    // Priority 1: "talk_start"
    for (size_t i = 0; i < anims.size(); ++i) {
        auto lower = toLower(anims[i]->getName());
        if (lower == "talk_start") return std::string(anims[i]->getName().buffer());
    }

    // Priority 2: contains "mouth"
    for (size_t i = 0; i < anims.size(); ++i) {
        auto lower = toLower(anims[i]->getName());
        if (lower.find("mouth") != std::string::npos)
            return std::string(anims[i]->getName().buffer());
    }

    // Priority 3: talk_loop, speak_loop, idle_talk
    for (size_t i = 0; i < anims.size(); ++i) {
        auto lower = toLower(anims[i]->getName());
        if (lower == "talk_loop" || lower == "speak_loop" || lower == "idle_talk")
            return std::string(anims[i]->getName().buffer());
    }

    // Priority 4: any animation with "talk" or "speak" in name
    for (size_t i = 0; i < anims.size(); ++i) {
        auto lower = toLower(anims[i]->getName());
        if (lower.find("talk") != std::string::npos ||
            lower.find("speak") != std::string::npos)
            return std::string(anims[i]->getName().buffer());
    }

    return {};
}

void SpineAnimation::startTalking()
{
    if (!m_animState || m_talking) return;

    auto talkAnim = detectTalkAnimation();
    if (talkAnim.empty()) {
        spdlog::debug("SpineAnimation: no talk animation found");
        return;
    }

    auto* anim = findAnimation(talkAnim);
    if (!anim) return;

    auto* entry = m_animState->setAnimation(
        static_cast<size_t>(AnimTrack::Talk),
        anim,
        true  // talk loops
    );

    if (entry) {
        // Use default mix — NOT MixBlend_Add (additive causes chin/jaw
        // bones to stretch infinitely because translations accumulate).
        entry->setMixDuration(m_talkMixDuration);
        entry->setAlpha(1.0f);
    }

    m_talking = true;

    spdlog::debug("SpineAnimation: talk → '{}'", talkAnim);
}

void SpineAnimation::stopTalking()
{
    if (!m_animState || !m_talking) return;

    m_animState->setEmptyAnimation(
        static_cast<size_t>(AnimTrack::Talk),
        m_talkMixDuration
    );

    m_talking = false;

    spdlog::debug("SpineAnimation: talk stopped");
}

// ─── Time control ───────────────────────────────────────────────────────────
void SpineAnimation::update(float dt)
{
    if (!m_animState || !m_skeleton) return;
    m_animState->update(dt * m_speed);
}

// Absolute-time seeking (evaluateAtTime) can land a looping track exactly on
// its loop boundary, where animationTime = trackTime mod duration ≈ 0.  At that
// instant the animation samples its t=0 keyframe values, which for assets whose
// loops are not perfectly seamless (e.g. a slot opacity that is 0 just before
// the loop end but keyed differently at t=0) produces a one-frame "pop" of that
// slot — visible only on the timeline, since the COMPOSE/LIBRARY previews drive
// the engine with continuous update(dt) and never sample exactly at the seam.
// Nudge the track time a fraction of a frame past the boundary so the seam
// frame matches its neighbours, mirroring continuous-playback behaviour.
void SpineAnimation::nudgeOffLoopSeam(spine::TrackEntry* entry)
{
    if (!entry || !entry->getLoop()) return;
    // ~one 60 fps frame: small enough to be imperceptible, large enough to clear
    // a single-frame boundary keyframe and the slot-cull alpha threshold.
    constexpr float kSeamEps = 1.0f / 60.0f;
    const float rel = entry->getAnimationTime() - entry->getAnimationStart();
    if (rel >= 0.0f && rel < kSeamEps)
        entry->setTrackTime(entry->getTrackTime() + (kSeamEps - rel));
}

void SpineAnimation::evaluateAtTime(float bodyTime, float talkTime)
{
    if (!m_animState || !m_skeleton) return;

    // Reset skeleton to setup pose
    m_skeleton->setToSetupPose();

    // Set body track time directly
    auto* bodyEntry = m_animState->getCurrent(static_cast<size_t>(AnimTrack::Body));
    if (bodyEntry) {
        bodyEntry->setTrackTime(bodyTime);
        nudgeOffLoopSeam(bodyEntry);
    }

    // Set talk track time directly
    if (m_talking) {
        auto* talkEntry = m_animState->getCurrent(static_cast<size_t>(AnimTrack::Talk));
        if (talkEntry) {
            talkEntry->setTrackTime(talkTime);
            nudgeOffLoopSeam(talkEntry);
        }
    }

    // Apply and update world transforms
    m_animState->apply(*m_skeleton);
    m_skeleton->updateWorldTransform();
}

void SpineAnimation::apply()
{
    if (!m_animState || !m_skeleton) return;

    // Reset to setup pose every frame so that animations with root-bone
    // translations (walk cycles, etc.) don't accumulate and cause the
    // character to "wander" across the screen.
    m_skeleton->setToSetupPose();

    m_animState->apply(*m_skeleton);
    m_skeleton->updateWorldTransform();
}

} // namespace rt

#endif // ROUNDTABLE_HAS_SPINE

