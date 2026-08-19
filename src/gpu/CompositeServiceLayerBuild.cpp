/*
 * CompositeServiceLayerBuild.cpp - Layer collection / building for compositeFrame().
 * Extracted from CompositeServiceFrame.cpp (Step P1.3 of modularization plan).
 */

#include "CompositeService.h"
#include "CompositeServiceLayerBuild.h"
#include "ClipRenderers.h"
#include "CompositeServiceBlend.h"

#include "cache/FrameCache.h"
#include "playback/MediaPool.h"
#include "playback/FrameFallbackPolicy.h"
#include "decode/VideoFrameMapping.h"
#include "Constants.h"
#include "timeline/AdjustmentClip.h"
#include "timeline/AudioClip.h"
#include "timeline/ImageClip.h"
#include "timeline/PngPuppetClip.h"
#include "timeline/SequenceClip.h"
#include "timeline/SpineClip.h"
#include "timeline/TitleClip.h"
#include "timeline/GraphicClip.h"
#include "timeline/GraphicLayer.h"
#include "timeline/CaptionClip.h"
#include "timeline/TierListClip.h"
#include "timeline/Timeline.h"
#include "timeline/Track.h"
#include "timeline/Transition.h"
#include "timeline/VideoClip.h"
#include "timeline/OpacityMask.h"
#include "timeline/Position2D.h"

#include "project/Project.h"

#ifdef ROUNDTABLE_HAS_SPINE
#include "spine/AnimationVideoCache.h"
#include "spine/ModelManager.h"
#include "spine/ShotPreset.h"
#include "stb_image.h"
#endif

#include "CompositeEngine.h"
#include "GpuContext.h"
#include "SpineRenderer.h"

#include <thread>
#include <algorithm>
#include <unordered_set>
#include <unordered_map>
#include <mutex>
#include <cmath>
#include <cstdlib>

namespace rt {

// ---- buildLayersForFrame ----
std::vector<LayerInfo> CompositeService::buildLayersForFrame(
    int64_t tick, uint32_t outW, uint32_t outH,
    bool scrubMode, bool playbackNonBlocking,
    ResolutionTier requestTier, bool stillMode,
    int& clipsAtTick, int& resolvedClipsAtTick, bool perfLog,
    std::unique_lock<std::recursive_mutex>& lock,
    bool& gpuSpineUsedThisFrame)
{
    // Track reentrancy so the sticky-cache prune at the end only fires for
    // the top-level frame (nested SequenceClips re-enter via compositeFrame).
    struct DepthGuard {
        int& d;
        explicit DepthGuard(int& x) : d(x) { ++d; }
        ~DepthGuard() { --d; }
    } buildDepthGuard(m_buildLayersDepth);

    // fetchMediaFrame lambda replaced by CompositeService::resolveMediaFrame()
    // (extracted to CompositeServiceLayerBuildVideo.cpp for modularization).

    std::vector<LayerInfo> layers;

    // At the top level, reset the per-frame active sets. They accumulate
    // across the whole (possibly nested) build and drive the end-of-frame
    // sticky-cache prune, so a clip/character that has left the active set
    // (e.g. a shot cut) cannot linger and its held (possibly GPU-resident)
    // CachedFrame is released promptly.
    if (m_buildLayersDepth == 1) {
        m_frameActiveClipIds.clear();
        m_frameActiveCharKeys.clear();
    }

    // Static-recomposite detection (top level only): when this build repeats
    // the previous build's tick, the playhead is parked and we're refreshing a
    // paused frame (e.g. a transform-handle drag fires a burst of same-tick
    // composites). On these frames the GPU Spine path reads its FBO back into a
    // stable frame so a later same-tick composite that briefly misses the live
    // render still shows the character via the sticky cache. During playback
    // the tick advances each frame, so this stays false and zero-copy is kept.
    const bool staticRecomposite =
        (m_buildLayersDepth == 1) && (tick == m_lastBuiltTick);

    clipsAtTick = 0;
    resolvedClipsAtTick = 0;
    m_gpuSpineCount = 0;
    m_gpuSpineInsertedLayer = -1;
    m_gpuSpinePrevLayer = -1;
    m_gpuSpineJustRendered = false;

    const size_t trackCnt = m_timeline->trackCount();

    for (size_t ti_rev = trackCnt; ti_rev > 0; --ti_rev) {
        auto* track = m_timeline->track(ti_rev - 1);
        if (!track || track->type() != TrackType::Video || track->isMuted())
            continue;

        auto active = track->clipsAtTime(tick);

        // ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ Cross-dissolve fix: include clips outside their normal range
        // that participate in an active cross-dissolve transition at this
        // tick.  Without this, only ONE clip renders and we get a dip-to-
        // black instead of a true simultaneous dissolve.
        for (size_t trI2 = 0; trI2 < track->transitionCount(); ++trI2) {
            const Transition* trans = track->transition(trI2);
            if (!trans) continue;
            float prog = trans->progress(tick);
            if (prog < 0.0f) continue; // tick outside transition

            // Add leftClip if not already active (past its timelineOut)
            if (trans->leftClipId != 0) {
                bool found = false;
                for (auto* a : active) if (a->id() == trans->leftClipId) { found = true; break; }
                if (!found) {
                    size_t li = track->findClipIndexById(trans->leftClipId);
                    if (li < track->clipCount())
                        active.push_back(track->clip(li));
                }
            }
            // Add rightClip if not already active (before its timelineIn)
            if (trans->rightClipId != 0) {
                bool found = false;
                for (auto* a : active) if (a->id() == trans->rightClipId) { found = true; break; }
                if (!found) {
                    size_t rri = track->findClipIndexById(trans->rightClipId);
                    if (rri < track->clipCount())
                        active.push_back(track->clip(rri));
                }
            }
        }

        for (auto* clip : active) {
            auto perfClipT0 = std::chrono::high_resolution_clock::now();
            if (!clip->isEnabled()) continue;
            // Adjustment layers are explicit render-stream boundaries.  The
            // GPU compositor flattens everything below this marker and runs
            // the stack once over the full frame.  Do not count the marker as
            // a decoded clip: it intentionally has no source frame.
            if (auto* adjustment = dynamic_cast<AdjustmentClip*>(clip)) {
                appendAdjustmentLayerBoundary(
                    layers, *adjustment, tick - adjustment->timelineIn());
                continue;
            }
            ++clipsAtTick;

            // Record this clip/character as active this frame so its sticky
            // last-frame entry survives the end-of-frame prune (keys must
            // match the format used when recording sticky frames below).
            m_frameActiveClipIds.insert(clip->id());
#ifdef ROUNDTABLE_HAS_SPINE
            if (auto* scKey = dynamic_cast<SpineClip*>(clip))
                m_frameActiveCharKeys.insert(scKey->characterName() + "|" + scKey->outfit() + "|" +
                                             scKey->animationName() + (scKey->isTalking() ? "|t" : "|m"));
            else
#endif
            if (auto* vcKey = dynamic_cast<VideoClip*>(clip))
                m_frameActiveCharKeys.insert(vcKey->mediaPath());

            bool fromNestedSequence = false;

            // Evaluate common transform properties.
            // REFERENCE resolution (1920ÃƒÆ’Ã†â€™ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â1080).
            // Scale them proportionally to the actual output resolution so
            // compositing looks correct at any viewport size.  (Effect
            // Controls converts the stored value to/from sequence pixels
            // for display only — see EffectControlsPanelTree.)
            constexpr float REF_W = 1920.0f;
            constexpr float REF_H = 1080.0f;
            const float scaleToOutX = static_cast<float>(outW) / REF_W;
            const float scaleToOutY = static_cast<float>(outH) / REF_H;
            const int64_t localTick = tick - clip->timelineIn();

            // Guard: if the clip's internal state is invalid (e.g. timeline
            // population is still in progress from a background thread),
            // skip this clip rather than crashing on Keyframe<float> iteration.
            // Each evaluate() call does a binary search on std::vector<Keyframe>;
            // if the vector was moved/corrupted concurrently, we'd ACCESS_VIOLATION.
            const uint64_t clipId = clip->id();
            float opac = 1.0f;
            float px = 0.0f, py = 0.0f;
            float sx = 1.0f, sy = 1.0f, rot = 0.0f;
            float ancX = 0.0f, ancY = 0.0f;
            try {
                opac = clip->opacity().evaluate(localTick);
                {
                    auto p2 = evaluatePosition2D(clip->positionX(), clip->positionY(), localTick);
                    px = p2.first  * scaleToOutX;
                    py = p2.second * scaleToOutY;
                }
                sx   = clip->scaleX().evaluate(localTick);
                sy   = clip->scaleY().evaluate(localTick);
                rot  = clip->rotation().evaluate(localTick);
                // Anchor stored as REF-1920 px (same convention as position);
                // convert to output px so the GPU transform builder treats it
                // in the same space as posX/posY.
                ancX = clip->anchorX().evaluate(localTick) * scaleToOutX;
                ancY = clip->anchorY().evaluate(localTick) * scaleToOutY;
            } catch (...) {
                // Catches a C++ exception thrown from evaluate() (e.g. a
                // std::bad_alloc / std::length_error if a keyframe vector's
                // size/capacity is garbage) — use defaults and continue.
                // NOTE: this TU compiles with /EHsc (see src/gpu/CMakeLists.txt),
                // so this does NOT catch a hardware ACCESS_VIOLATION from
                // dereferencing a freed/corrupted vector. The real protection
                // against concurrent timeline mutation is the preroll deferral
                // (isBackgroundWarmupActive gate) plus the fact that
                // buildLayersForFrame runs under m_compositeMutex; this catch is
                // only a soft backstop for thrown exceptions, not a memory-safety
                // guarantee.
                spdlog::warn("compositeFrame: keyframe evaluation failed for clip {} — using defaults",
                             clipId);
            }

            // PNG puppet "breathing": add a gentle, deterministic drift (sway +
            // rise/fall + tiny scale/rotation) on top of the clip's own
            // transform so the character feels alive instead of static.  Pure
            // function of GLOBAL timeline time so the motion is continuous
            // across cuts between same-character clips (no jump when switching
            // expressions); identical in preview and export.
            if (auto* puppetClip = dynamic_cast<PngPuppetClip*>(clip)) {
                const auto bo = puppetClip->breathing(ticksToSeconds(tick));
                px  += bo.dx * scaleToOutX;
                py  += bo.dy * scaleToOutY;
                sx  *= bo.scale;
                sy  *= bo.scale;
                rot += bo.rot;
            }

            // Evaluate the transform exposure endpoints for shutter-angle
            // motion blur. The source frame stays at the current time; only
            // Position, Scale, Rotation, and Anchor are sampled temporally.
            LayerTransformSample motionStart{px, py, sx, sy, rot, ancX, ancY};
            LayerTransformSample motionEnd = motionStart;
            int32_t motionSampleCount = 1;
            float shutterAngle = 0.0f;
            try {
                shutterAngle = std::clamp(
                    clip->shutterAngle().evaluate(localTick), 0.0f, 720.0f);
            } catch (...) {
                shutterAngle = 0.0f;
            }
            const double sequenceFps = m_timeline->settings().frameRate();
            if (shutterAngle > 0.01f && sequenceFps > 0.0 && clip->duration() > 1) {
                const double frameTicks =
                    static_cast<double>(kTicksPerSecond) / sequenceFps;
                const double exposureTicks = frameTicks *
                    (static_cast<double>(shutterAngle) / 360.0);
                const int64_t maxLocalTick =
                    std::max<int64_t>(0, clip->duration() - 1);
                const int64_t startTick = std::clamp<int64_t>(
                    static_cast<int64_t>(std::llround(
                        static_cast<double>(localTick) - exposureTicks * 0.5)),
                    0, maxLocalTick);
                const int64_t endTick = std::clamp<int64_t>(
                    static_cast<int64_t>(std::llround(
                        static_cast<double>(localTick) + exposureTicks * 0.5)),
                    0, maxLocalTick);

                auto evaluateMotionAt = [&](int64_t sampleTick) {
                    LayerTransformSample sample;
                    const auto p2 = evaluatePosition2D(
                        clip->positionX(), clip->positionY(), sampleTick);
                    sample.posX = p2.first * scaleToOutX;
                    sample.posY = p2.second * scaleToOutY;
                    sample.scX = clip->scaleX().evaluate(sampleTick);
                    sample.scY = clip->scaleY().evaluate(sampleTick);
                    sample.rot = clip->rotation().evaluate(sampleTick);
                    sample.anchorX = clip->anchorX().evaluate(sampleTick) * scaleToOutX;
                    sample.anchorY = clip->anchorY().evaluate(sampleTick) * scaleToOutY;
                    if (auto* puppetClip = dynamic_cast<PngPuppetClip*>(clip)) {
                        const auto bo = puppetClip->breathing(ticksToSeconds(
                            clip->timelineIn() + sampleTick));
                        sample.posX += bo.dx * scaleToOutX;
                        sample.posY += bo.dy * scaleToOutY;
                        sample.scX *= bo.scale;
                        sample.scY *= bo.scale;
                        sample.rot += bo.rot;
                    }
                    return sample;
                };

                try {
                    motionStart = evaluateMotionAt(startTick);
                    motionEnd = evaluateMotionAt(endTick);
                    const bool moved =
                        std::abs(motionStart.posX - motionEnd.posX) > 0.001f ||
                        std::abs(motionStart.posY - motionEnd.posY) > 0.001f ||
                        std::abs(motionStart.scX - motionEnd.scX) > 0.00001f ||
                        std::abs(motionStart.scY - motionEnd.scY) > 0.00001f ||
                        std::abs(motionStart.rot - motionEnd.rot) > 0.0001f ||
                        std::abs(motionStart.anchorX - motionEnd.anchorX) > 0.001f ||
                        std::abs(motionStart.anchorY - motionEnd.anchorY) > 0.001f;
                    if (moved)
                        motionSampleCount = 8;
                } catch (...) {
                    motionStart = {px, py, sx, sy, rot, ancX, ancY};
                    motionEnd = motionStart;
                    motionSampleCount = 1;
                }
            }

            auto assignMotionBlur = [&](LayerInfo& layer, float composeFit) {
                layer.motionStart = motionStart;
                layer.motionEnd = motionEnd;
                layer.motionStart.scX *= composeFit;
                layer.motionStart.scY *= composeFit;
                layer.motionEnd.scX *= composeFit;
                layer.motionEnd.scY *= composeFit;
                layer.motionSampleCount = motionSampleCount;
            };

            // Apply transition opacity modulation (for fades & dissolves).
            // Wipe transitions store metadata for GPU spatial blending.
            TransitionType activeWipeType = TransitionType::CrossDissolve;
            float activeWipeProgress = -1.0f;
            float activeWipeSoftness = -1.0f;
            uint64_t activeWipePeer = 0;
            bool activeWipeOutgoing = false;

            for (size_t trI = 0; trI < track->transitionCount(); ++trI) {
                const Transition* trans = track->transition(trI);
                if (!trans) continue;
                float prog = trans->progress(tick);
                if (prog < 0.0f) {
                    // Sub-frame misalignment guard.  A single-sided fade's
                    // editPointTick may not sit exactly on the clip's
                    // frame-aligned head/tail (rounding when the clip was
                    // trimmed/moved/created), so at a frame-boundary tick the
                    // playhead can land just OUTSIDE the transition range while
                    // still inside the clip.  progress() then returns -1 and the
                    // fade is skipped, leaving the clip at FULL opacity for that
                    // one frame — the "color matte flashes bright on its first
                    // frame during playback/export" bug (stepping happened to
                    // miss the exact tick).  Clamp instead of skipping: an
                    // incoming single-sided fade (right==clip, no left peer) is
                    // invisible before its range; an outgoing one (left==clip,
                    // no right peer) is fully gone after its range.  Two-clip
                    // transitions stay strictly range-gated (genuine continue).
                    int64_t rs = 0, re = 0;
                    trans->getRange(rs, re);
                    const bool singleIncoming =
                        (trans->rightClipId == clipId && trans->leftClipId == 0);
                    const bool singleOutgoing =
                        (trans->leftClipId == clipId && trans->rightClipId == 0);
                    if (singleIncoming && tick < rs)       prog = 0.0f;
                    else if (singleOutgoing && tick >= re) prog = 1.0f;
                    else continue; // tick genuinely outside transition range
                }

                if (trans->type == TransitionType::FadeFromBlack) {
                    // Single-clip GPU fade: clip fades in from black.
                    if (trans->rightClipId == clipId) {
                        activeWipeType = TransitionType::FadeFromBlack;
                        activeWipeProgress = prog;
                        activeWipeSoftness = trans->param1;
                        activeWipePeer = 0;
                        activeWipeOutgoing = true;
                    }
                } else if (trans->type == TransitionType::FadeToBlack) {
                    // Single-clip GPU fade: clip fades out to black.
                    if (trans->leftClipId == clipId) {
                        activeWipeType = TransitionType::FadeToBlack;
                        activeWipeProgress = prog;
                        activeWipeSoftness = trans->param1;
                        activeWipePeer = 0;
                        activeWipeOutgoing = true;
                    }
                } else if (trans->type == TransitionType::FadeFromWhite) {
                    // Single-clip GPU fade: clip fades in from white.
                    if (trans->rightClipId == clipId) {
                        activeWipeType = TransitionType::FadeFromWhite;
                        activeWipeProgress = prog;
                        activeWipeSoftness = trans->param1;
                        activeWipePeer = 0;
                        activeWipeOutgoing = true;
                    }
                } else if (trans->type == TransitionType::FadeToWhite) {
                    // Single-clip GPU fade: clip fades out to white.
                    if (trans->leftClipId == clipId) {
                        activeWipeType = TransitionType::FadeToWhite;
                        activeWipeProgress = prog;
                        activeWipeSoftness = trans->param1;
                        activeWipePeer = 0;
                        activeWipeOutgoing = true;
                    }
                } else if (trans->type == TransitionType::CrossDissolve) {
                    // Route CrossDissolve through the GPU transition path
                    // (TransitionRenderer mixes A and B in a single pass).
                    //
                    // Pure-opacity modulation is WRONG when a lower video
                    // track is present: at progress=0.5 each clip renders
                    // at 50% alpha, so the combined front coverage is only
                    // p*R + (1-p)^2*L = 0.75, and 0.25 of the lower track
                    // leaks through â€” visually "darkening" the dissolve.
                    // The GPU Dissolve shader does mix(A, B, p) directly,
                    // producing full coverage.
                    //
                    // For single-clip dissolves (peer == 0) the GPU mix
                    // path is WRONG: mix(transparentBlack, clip, p) yields a
                    // PREMULTIPLIED result (p*rgb, p*a) that the compositor
                    // reads as straight alpha, so the clip emerges "from
                    // black". Premiere's single-clip Cross Dissolve is just
                    // a straight-alpha opacity fade — so modulate opacity
                    // directly and skip the transition pass entirely.
                    if (trans->leftClipId == clipId && trans->rightClipId != 0) {
                        // Two-clip: this is the outgoing side (drives merge).
                        activeWipeType = TransitionType::CrossDissolve;
                        activeWipeProgress = prog;
                        activeWipeSoftness = trans->param1;
                        activeWipePeer = trans->rightClipId;
                        activeWipeOutgoing = true;
                    } else if (trans->rightClipId == clipId && trans->leftClipId != 0) {
                        // Two-clip: this is the incoming side (passive).
                        activeWipeType = TransitionType::CrossDissolve;
                        activeWipeProgress = prog;
                        activeWipeSoftness = trans->param1;
                        activeWipePeer = trans->leftClipId;
                        activeWipeOutgoing = false;
                    } else if (trans->leftClipId == clipId) {
                        // Single-clip outgoing dissolve (no right peer):
                        // straight-alpha opacity fade-out. RGB stays intact
                        // so it reveals lower tracks (or black if none)
                        // cleanly, never fading "through black".
                        opac *= (1.0f - prog);
                    } else if (trans->rightClipId == clipId) {
                        // Single-clip incoming dissolve (no left peer):
                        // straight-alpha opacity fade-in (transparent →
                        // opaque), exactly like Premiere's Cross Dissolve
                        // at a clip head.
                        opac *= prog;
                    }
                } else {
                    // GPU-spatial transition: don't modulate opacity ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÂ¢Ã¢â€šÂ¬Ã‚Â store info for GPU path.
                    // Both clips render at full opacity; spatial blending is
                    // done later by TransitionRenderer.
                    if (trans->leftClipId == clipId) {
                        activeWipeType = trans->type;
                        activeWipeProgress = prog;
                        activeWipeSoftness = trans->param1;
                        activeWipePeer = trans->rightClipId;
                        activeWipeOutgoing = true;
                    } else if (trans->rightClipId == clipId) {
                        activeWipeType = trans->type;
                        activeWipeProgress = prog;
                        activeWipeSoftness = trans->param1;
                        activeWipePeer = trans->leftClipId;
                        activeWipeOutgoing = false;
                    }
                }
            }

            std::shared_ptr<CachedFrame> frame;
            std::shared_ptr<CachedFrame> temporalFrame;
            float temporalPhase = 0.0f;
            int32_t temporalMode = 0;
            bool sourceFallbackPending = false;
            bool contentStableForStateCache = false;
            int64_t temporalPrimaryFrameNum = -1;
            int64_t temporalSecondFrameNum = -1;
            bool gpuSpineZeroCopy = false;
            bool isPreRenderedSpine = false;  // set when using cached spine video
            bool cpuSpineRendered  = false;   // set when CPU Spine fallback succeeds
            VkDescriptorImageInfo gpuSpineDescriptor{};
            uint32_t gpuSpineW{0}, gpuSpineH{0};

            // (Each clip-type branch below computes its own decode tier —
            // the VideoClip path's baseVideoTier/charVideoTier follow the
            // playback-resolution dropdown with forceFullResolution and
            // scrub overrides.)

            // ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ VideoClip ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬
            if (auto* videoClip = dynamic_cast<VideoClip*>(clip)) {
                // Resolve (lazily open / search-fallback) the media handle.
                // skipClip = no MediaPool / empty path / unresolved media.
                // A 0 handle WITHOUT skipClip means the async open is still
                // pending - proceed so the sticky-frame fallback can run.
                bool skipClip = false;
                uint64_t handle =
                    resolveVideoClipHandle(videoClip, playbackNonBlocking, skipClip);
                if (skipClip) continue;

                // Tick → source-frame mapping: ONE shared authority
                // (speed scaling, negative clamp, llround, fps priority,
                // still/character wrap policy) — see VideoFrameMapping.h.
                // This is the RENDER side; the pending-frame collector and
                // the lookahead prewarm call the same function, so what
                // gets prefetched is exactly what gets rendered.
                auto* mediaInfo = m_mediaPool->getInfo(handle);
                contentStableForStateCache = mediaInfo &&
                    (mediaInfo->duration <= 0.0 || mediaInfo->frameCount <= 1);
                const auto mapped =
                    mapTickToSourceFrame(*videoClip, tick, mediaInfo);
                const TimeInterpolation interpolation =
                    videoClip->timeInterpolation();
                const bool wantsTemporal =
                    interpolation != TimeInterpolation::FrameSampling &&
                    mapped.blendPhase > 0.000001 &&
                    mapped.lowerFrame != mapped.upperFrame;
                // Sampling uses Premiere's nearest-frame behavior.  Both
                // temporal modes synthesize between floor/ceil endpoints.
                const int64_t frameNum = wantsTemporal
                    ? mapped.lowerFrame : mapped.frame;
                temporalPrimaryFrameNum = frameNum;
                temporalSecondFrameNum = wantsTemporal
                    ? mapped.upperFrame : -1;
                temporalPhase = wantsTemporal
                    ? static_cast<float>(mapped.blendPhase) : 0.0f;
                temporalMode = wantsTemporal
                    ? static_cast<int32_t>(interpolation) : 0;

                const bool isVideoChar = videoClip->isVideoCharacter();

                // Character overlays always Half (they composite tiny),
                // UNLESS forceFullResolution is set (ExportPanel preview/export).
                // Both characters and other video follow the playback-
                // resolution dropdown so Full really means Full, etc.
                // (Previously characters were pinned to Half regardless of
                // the dropdown setting — the user picked Full and the
                // character preview stayed blurry. forceFullResolution
                // from ExportPanel still wins for the export/preview path.)
                //
                // Scrub override: during timeline scrub at Full tier, decode
                // at Half. The Program Monitor already renders scrub composites
                // at output/2 (ProgramMonitor::onPollTimer "resDivisor * 2"),
                // so a Full-tier media decode is 4× the cost for pixels that
                // get downscaled away. Premiere does the same — scrubs use a
                // lower-quality preview. forceFullResolution (export) still
                // gets Full. Half/Quarter playback already scrubs at the same
                // tier and would only over-reduce if we generalized further.
                // Scrub / fast-playback: drop one tier for smooth decode.
                // At Full→Half, decode cost is 4× lower; at Half→Quarter,
                // 2× lower.  When the user stops scrubbing, the next frame
                // request at the normal tier restores full quality.
                // Premiere Pro does the same — scrubs are lower quality.
                auto baseVideoTier = m_forceFullResolution.load()
                    ? ResolutionTier::Full
                    : requestTier;
                ResolutionTier charVideoTier;
                if (scrubMode && !m_forceFullResolution.load()) {
                    // Always drop at least one tier during scrub/4x playback
                    if (baseVideoTier == ResolutionTier::Full)
                        charVideoTier = ResolutionTier::Half;
                    else if (baseVideoTier == ResolutionTier::Half)
                        charVideoTier = ResolutionTier::Quarter;
                    else
                        charVideoTier = ResolutionTier::Quarter; // floor
                } else {
                    charVideoTier = baseVideoTier;
                }

                // frameNum is already wrapped/clamped by mapTickToSourceFrame
                // (still → 0, video character → modulo loop, else clamp).

                // Ã¢â€â‚¬Ã¢â€â‚¬ DIRTY TRACKING: Skip decode when GPU already has this frame Ã¢â€â‚¬Ã¢â€â‚¬
                // If the GPU texture cache already has this (mediaId, frameNumber),
                // skip the entire decode + CPU packed-alpha unpack path.  This is
                // the single biggest performance win: backgrounds and unchanged
                // animation loops skip ALL CPU work (~60% of frames at 60fps when
                // source is 24fps).
                auto* texCache = m_engine ? m_engine->textureCache() : nullptr;
                if (texCache && m_engine->isGpuCompositeEnabled()) {
                    auto gpuHit = texCache->get(handle, frameNum,
                        static_cast<uint8_t>(charVideoTier));
                    auto temporalGpuHit = wantsTemporal
                        ? texCache->get(handle, temporalSecondFrameNum,
                            static_cast<uint8_t>(charVideoTier))
                        : GpuTextureCache::LookupResult{};
                    const bool temporalGpuCompatible =
                        !wantsTemporal ||
                        (temporalGpuHit.found &&
                         temporalGpuHit.width == gpuHit.width &&
                         temporalGpuHit.height == gpuHit.height &&
                         temporalGpuHit.isPacked == gpuHit.isPacked);
                    // Never let a partial temporal cache hit escape as a raw
                    // descriptor. Only the all-endpoints-ready branch below
                    // copies descriptors into LayerInfo, records both cache
                    // identities, and lets RenderGraph pin both until fence
                    // completion. A primary-only hit falls through to owned
                    // CachedFrame endpoint resolution.
                    if (gpuHit.found && temporalGpuCompatible) {
                        LayerInfo layer;
                        layer.contentStableForStateCache = contentStableForStateCache;
                        layer.gpuTextureReady = true;
                        layer.gpuDescriptor   = gpuHit.descriptor;
                        layer.gpuCacheBacked = true;
                        layer.gpuCacheMediaId = handle;
                        layer.gpuCacheFrameNumber = frameNum;
                        layer.gpuCacheTier = static_cast<uint8_t>(charVideoTier);
                        layer.frameWidth      = gpuHit.width;
                        layer.frameHeight     = gpuHit.height;
                        if (wantsTemporal) {
                            layer.temporalMode = temporalMode;
                            layer.temporalPhase = temporalPhase;
                            layer.temporalGpuTextureReady = true;
                            layer.temporalGpuDescriptor = temporalGpuHit.descriptor;
                            layer.temporalGpuCacheBacked = true;
                            layer.temporalGpuCacheMediaId = handle;
                            layer.temporalGpuCacheFrameNumber = temporalSecondFrameNum;
                            layer.temporalGpuCacheTier =
                                static_cast<uint8_t>(charVideoTier);
                        }
                        layer.srcRotation = mediaInfo
                            ? mediaInfo->rotation : videoClip->sourceRotation();
                        layer.opacity  = opac;
                        layer.posX     = px;
                        layer.posY     = py;
                        layer.rot      = rot;
                        layer.anchorX  = ancX;
                        layer.anchorY  = ancY;
                        layer.clipId   = clipId;
                        layer.blendMode = clip->blendMode();
                        // Video characters: composite under CONTAIN-fit + 0.85×
                        // user scale, matching COMPOSE's
                        //   displayH = canvasH * 0.85 * userScale
                        //   (preserves source aspect; portrait sources fit
                        //    canvas height, not canvas width).
                        // The transform overlay (TimelineWorkspaceOverlay.cpp)
                        // already documents this contract.  Don't normalize by
                        // srcH here — Compositor::buildViewportTransform's
                        // contain-fit (min of outH/srcH, outW/srcW) collapses
                        // a portrait source's fittedH to outH, so a constant
                        // 0.85 × userScale gives the correct final height
                        // regardless of frame-decode state (no flicker on
                        // scrub / seek / prewarm misses).
                        layer.scX = sx;
                        layer.scY = sy;
                        if (isVideoChar) {
                            constexpr float kComposeFit = 0.85f;
                            layer.scX *= kComposeFit;
                            layer.scY *= kComposeFit;
                            layer.containFit = true;
                        }
                        assignMotionBlur(layer, isVideoChar ? 0.85f : 1.0f);
                        // Portrait character frames need contain-fit.
                        // Packed-alpha: use the flag stored in the GPU cache
                        // entry at upload time â€” avoids fragile height heuristics.
                        layer.isPacked = gpuHit.isPacked;
                        layer.isPMA    = gpuHit.isPMA;
                        layer.contentBoundsValid = gpuHit.contentBoundsValid;
                        layer.contentLeft = gpuHit.contentLeft;
                        layer.contentTop = gpuHit.contentTop;
                        layer.contentRight = gpuHit.contentRight;
                        layer.contentBottom = gpuHit.contentBottom;
                        // Wipe transition metadata
                        if (activeWipeProgress >= 0.0f) {
                            layer.wipeType        = activeWipeType;
                            layer.wipeProgress    = activeWipeProgress;
                            layer.wipeSoftness    = activeWipeSoftness;
                            layer.wipePeerClipId  = activeWipePeer;
                            layer.isWipeOutgoing  = activeWipeOutgoing;
                        }
                        // Crop
                        layer.cropL = videoClip->cropLeft();
                        layer.cropR = videoClip->cropRight();
                        layer.cropT = videoClip->cropTop();
                        layer.cropB = videoClip->cropBottom();
                        // Effects
                        if (clip->effects().hasActiveEffects()) {
                            layer.effects = clip->effects().evaluate(localTick);
                        }
                        // Opacity masks — snapshot at the same local time
                        if (clip->maskCount() > 0)
                            layer.masks = evaluateMaskStates(clip->masks(), localTick);
                        if (perfLog) {
                            auto perfClipT1 = std::chrono::high_resolution_clock::now();
                            double clipMs = std::chrono::duration<double, std::milli>(
                                perfClipT1 - perfClipT0).count();
                            spdlog::info("  [PERF] clip '{}' type=Video (GPU CACHE HIT) -> {:.1f}ms ({}x{}) frame={}",
                                         clip->label(), clipMs, gpuHit.width, gpuHit.height, frameNum);
                        }
                        layer.clipPtr = clip;
                        layer.isLoopContent = isVideoChar;
                        layers.push_back(std::move(layer));
                        ++resolvedClipsAtTick;
                        continue;
                    }
                }

                {
                    // Time resolveMediaFrame (cache lookup + scheduling
                    // + potential blocking decode).  Slow values here
                    // mean MediaPool's m_mutex is contended (most
                    // likely by openWorker doing a synchronous decoder
                    // open) — exactly the pattern suspected of being
                    // behind the 6 s LAYER-SLOW events.
                    auto rmfT0 = std::chrono::high_resolution_clock::now();
                    frame = resolveMediaFrame(handle, frameNum, charVideoTier,
                                              scrubMode,
                                              /*exactCacheOnly=*/wantsTemporal,
                                              stillMode);
                    // MediaPool may return a nearby/last-good frame to keep
                    // sequential playback smooth.  It is provisional input,
                    // not the render for this timeline tick.  In particular,
                    // adjustment effects must not turn it into a persistent
                    // composite-cache entry under the requested tick.
                    sourceFallbackPending =
                        frame && frame->frameNumber != frameNum;
                    auto rmfT1 = std::chrono::high_resolution_clock::now();
                    const double rmfMs = std::chrono::duration<double, std::milli>(
                        rmfT1 - rmfT0).count();
                    if (rmfMs > 30.0) {
                        spdlog::warn("[RESOLVE-SLOW] handle={} frame={} tier={} "
                                     "scrub={} -> {:.1f}ms",
                                     handle, frameNum, static_cast<int>(charVideoTier),
                                     scrubMode, rmfMs);
                    }
                }

                // Resolve the later endpoint independently.  During normal
                // playback this remains non-blocking and schedules the cache
                // miss; export's force-exact path resolves it synchronously.
                // Never blend against resolveMediaFrame's stale fallback.
                if (wantsTemporal) {
                    temporalFrame = resolveMediaFrame(
                        handle, temporalSecondFrameNum, charVideoTier, scrubMode,
                        /*exactCacheOnly=*/true, stillMode);
                    const bool exactEndpointsReady =
                        frame && frame->frameNumber == temporalPrimaryFrameNum &&
                        temporalFrame &&
                        temporalFrame->frameNumber == temporalSecondFrameNum &&
                        frame->width == temporalFrame->width &&
                        frame->height == temporalFrame->height;
                    if (!exactEndpointsReady) {
                        // While either endpoint warms, show Premiere-style
                        // nearest sampling instead of always holding the lower
                        // frame. Reuse an already-held exact endpoint when it
                        // is the mapped nearest; otherwise use the ordinary
                        // best-effort playback resolver for that one frame.
                        std::shared_ptr<CachedFrame> nearest;
                        if (mapped.frame == temporalPrimaryFrameNum && frame &&
                            frame->frameNumber == mapped.frame) {
                            nearest = frame;
                        } else if (mapped.frame == temporalSecondFrameNum &&
                                   temporalFrame &&
                                   temporalFrame->frameNumber == mapped.frame) {
                            nearest = temporalFrame;
                        }
                        if (!nearest) {
                            nearest = resolveMediaFrame(handle, mapped.frame,
                                                        charVideoTier, scrubMode,
                                                        /*exactCacheOnly=*/false,
                                                        stillMode);
                        }
                        frame = std::move(nearest);
                        temporalFrame.reset();
                        temporalMode = 0;
                        temporalPhase = 0.0f;
                        sourceFallbackPending = true;
                    }
                }

                // Packed-alpha unpack is now handled entirely by:
                //   - GPU path: compositor shader isPacked UV split
                //   - CPU path: MediaPool::unpackPackedAlphaInPlace()
                // No manual unpack needed here.
            }
            // Still ImageClip: resolve frame zero once through MediaPool.  The
            // decoded frame is pinned, and the final-composite state cache can
            // then reuse an unchanged stack of images across timeline ticks.
            else if (auto* imageClip = dynamic_cast<ImageClip*>(clip)) {
                if (!m_mediaPool || imageClip->mediaPath().empty()) continue;
                const std::string& mediaPath = imageClip->mediaPath();
                uint64_t handle = findMediaHandle(mediaPath);
                if (handle == 0) {
                    if (playbackNonBlocking && !m_forceFullResolution.load()) {
                        if (m_mediaPool->isPathOpen(mediaPath)) {
                            handle = m_mediaPool->open(mediaPath);
                            if (handle != 0) registerMediaHandle(mediaPath, handle);
                        } else {
                            m_mediaPool->openAsync(mediaPath);
                            continue;
                        }
                    } else {
                        handle = m_mediaPool->open(mediaPath);
                        if (handle != 0) registerMediaHandle(mediaPath, handle);
                    }
                }
                if (handle == 0) continue;
                contentStableForStateCache = true;
                frame = resolveMediaFrame(handle, 0, requestTier, scrubMode,
                                          /*exactCacheOnly=*/false, stillMode);
            }
            // ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ TitleClip ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬
            else if (auto* titleClip = dynamic_cast<TitleClip*>(clip)) {
                frame = renderTitleClip(titleClip, tick, outW, outH);
            }
            // Ã¢â€â‚¬Ã¢â€â‚¬ GraphicClip (multi-layer) Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬
            else if (auto* graphicClip = dynamic_cast<GraphicClip*>(clip)) {
                // Pass project output resolution as reference so text scales
                // proportionally at reduced render resolutions (scrub).
                uint32_t refW = 0, refH = 0;
                if (m_project) {
                    refW = m_project->settings().resolution().width;
                    refH = m_project->settings().resolution().height;
                }
                frame = rt::renderGraphicClip(graphicClip, tick, outW, outH, refW, refH);
            }
            // ── CaptionClip (burned-in subtitle overlay) ────────────────────
            else if (auto* captionClip = dynamic_cast<CaptionClip*>(clip)) {
                // Pass project resolution as reference so caption font metrics
                // stay proportional at reduced render resolutions (scrub).
                uint32_t refW = 0, refH = 0;
                if (m_project) {
                    refW = m_project->settings().resolution().width;
                    refH = m_project->settings().resolution().height;
                }
                frame = rt::renderCaptionClip(captionClip, tick, outW, outH, refW, refH);
            }
            // â”€â”€ SequenceClip (nested sequence) â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
            else if (auto* seqClip = dynamic_cast<SequenceClip*>(clip)) {
                // Recursive composite of the nested sequence into a clean CPU
                // BGRA frame (see CompositeServiceLayerBuildNested.cpp).
                frame = buildSequenceClipFrame(seqClip, localTick, outW, outH,
                                               scrubMode, requestTier, stillMode,
                                               lock);
            }
            // ── PngPuppetClip (Veadotube-style 4-image character) ───────────
            // Use GLOBAL tick so talk/blink phase carries across cuts between
            // same-character clips (kept in sync via a character-derived seed).
            else if (auto* puppetClip = dynamic_cast<PngPuppetClip*>(clip)) {
                frame = renderPngPuppetClip(puppetClip, tick, outW, outH);
            }
            // ── TierListClip (ranking board) ────────────────────────────────
            else if (auto* tierClip = dynamic_cast<TierListClip*>(clip)) {
                contentStableForStateCache = true;
                uint32_t refW = 0, refH = 0;
                if (m_project) {
                    refW = m_project->settings().resolution().width;
                    refH = m_project->settings().resolution().height;
                }
                frame = rt::renderTierListClip(tierClip, tick, outW, outH, refW, refH);
            }
#ifdef ROUNDTABLE_HAS_SPINE
            // ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ SpineClip ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬
            else if (auto* spineClip = dynamic_cast<SpineClip*>(clip)) {
                auto spineRes = buildSpineClipLayer(
                    spineClip, tick, localTick, outW, outH, scrubMode,
                    playbackNonBlocking, staticRecomposite,
                    gpuSpineUsedThisFrame, layers, sx);
                frame              = spineRes.frame;
                gpuSpineZeroCopy   = spineRes.gpuSpineZeroCopy;
                gpuSpineDescriptor = spineRes.gpuSpineDescriptor;
                gpuSpineW          = spineRes.gpuSpineW;
                gpuSpineH          = spineRes.gpuSpineH;
                cpuSpineRendered   = spineRes.cpuSpineRendered;
            }
#endif
            else {
                continue;
            }

            // GPU zero-copy spine layers bypass the CachedFrame requirement.
            // GPU-resident decoded frames (gpuReady) may have empty pixels.
            if (!gpuSpineZeroCopy && (!frame || (frame->pixels.empty() && !frame->gpuReady))) {
                // Exact still/export requests must never borrow a sticky
                // last-good source picture. Leave this clip unresolved so
                // the caller retries this tick or fails the export cleanly.
                if (stillMode)
                    continue;
                // Build a character/media-level sticky key — same string
                // across all clip IDs that reference the same video file.
                // This is what lets a BRAND NEW shot of Modernia/Chime
                // reuse the previous shot's last frame while her loop
                // cache warms up.
                std::string charKey;
#ifdef ROUNDTABLE_HAS_SPINE
                if (auto* sc = dynamic_cast<SpineClip*>(clip)) {
                    charKey = sc->characterName() + "|" + sc->outfit() + "|" +
                              sc->animationName() +
                              (sc->isTalking() ? "|t" : "|m");
                } else
#endif
                if (auto* vc = dynamic_cast<VideoClip*>(clip)) {
                    charKey = vc->mediaPath();
                }

                // Sticky last-good-frame fallback: if we've previously
                // rendered this clip successfully, reuse that frame
                // instead of dropping the layer.  Eliminates the
                // "character vanishes for a few frames" pop-out when
                // loop pre-decode hasn't caught up, a Spine skeleton
                // is still loading, or a scrub outpaces prefetch.
                std::shared_ptr<CachedFrame> stickyFrame;
                auto stickyIt = m_stickyLastClipFrame.find(clipId);
                if (stickyIt != m_stickyLastClipFrame.end() && stickyIt->second &&
                    (!stickyIt->second->pixels.empty() || stickyIt->second->gpuReady)) {
                    stickyFrame = stickyIt->second;
                } else if (!charKey.empty()) {
                    auto charIt = m_stickyLastCharFrame.find(charKey);
                    if (charIt != m_stickyLastCharFrame.end() && charIt->second &&
                        (!charIt->second->pixels.empty() || charIt->second->gpuReady)) {
                        stickyFrame = charIt->second;
                    }
                }
                if (stickyFrame) {
                    // A regular footage clip must not borrow a sticky frame
                    // from a distant source position after a seek. Character
                    // loops intentionally wrap and may use their sticky image
                    // briefly while the loop decoder catches up.
                    auto* videoClip = dynamic_cast<VideoClip*>(clip);
                    if (videoClip && !videoClip->isVideoCharacter() &&
                        temporalPrimaryFrameNum >= 0 &&
                        !isNearbyPlaybackFrame(temporalPrimaryFrameNum,
                                               stickyFrame->frameNumber)) {
                        stickyFrame.reset();
                    }
                }
                if (stickyFrame) {
                    frame = stickyFrame;
                    if (auto* videoClip = dynamic_cast<VideoClip*>(clip)) {
                        if (temporalPrimaryFrameNum >= 0 &&
                            frame->frameNumber != temporalPrimaryFrameNum) {
                            sourceFallbackPending = true;
                        }
                    }
                } else {
                    static int s_skipLog = 0;
                    if (++s_skipLog <= 30 || s_skipLog % 60 == 0) {
                        spdlog::info("[FLICKER-DIAG] compositeFrame tick={}: SKIPPING clip '{}' "
                                     "(frame={} pixels={} gpuReady={})",
                                     tick, clip->label(),
                                     frame ? "non-null" : "NULL",
                                     frame ? frame->pixels.size() : 0,
                                     frame ? frame->gpuReady : false);
                    }
                    continue;
                }
            } else if (!gpuSpineZeroCopy && frame &&
                       (!frame->pixels.empty() || frame->gpuReady)) {
                // Record this frame as the sticky fallback for future ticks.
                m_stickyLastClipFrame[clipId] = frame;
                // Also record under a character/media key so future shots
                // of the same character can use this as a starting point.
                std::string charKey;
#ifdef ROUNDTABLE_HAS_SPINE
                if (auto* sc = dynamic_cast<SpineClip*>(clip)) {
                    charKey = sc->characterName() + "|" + sc->outfit() + "|" +
                              sc->animationName() +
                              (sc->isTalking() ? "|t" : "|m");
                } else
#endif
                if (auto* vc = dynamic_cast<VideoClip*>(clip)) {
                    charKey = vc->mediaPath();
                }
                if (!charKey.empty())
                    m_stickyLastCharFrame[charKey] = frame;
            }

            // Temporal interpolation may use nearest sampling while its second
            // endpoint warms during interactive playback. That is not a
            // complete export frame: exact requests wait for both endpoints.
            if (stillMode && sourceFallbackPending)
                continue;

            LayerInfo layer;
            layer.sourceFallbackPending = sourceFallbackPending;
            layer.contentStableForStateCache = contentStableForStateCache;
            if (gpuSpineZeroCopy) {
                // No CachedFrame needed ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â the spine FBO is already on the GPU
                layer.gpuTextureReady = true;
                layer.gpuDescriptor   = gpuSpineDescriptor;
                layer.frameWidth      = gpuSpineW;
                layer.frameHeight     = gpuSpineH;
                layer.isPMA           = true;  // Spine FBO uses PMA blending
            } else if (frame->gpuReady && frame->gpuImageView && frame->gpuSampler) {
                // GPU-resident decoded frame Ã¢â‚¬â€ use GPU texture directly,
                // bypassing CPUÃ¢â€ â€™GPU upload in the compositor.
                layer.frame           = frame;
                layer.gpuTextureReady = true;
                VkDescriptorImageInfo gpuInfo{};
                gpuInfo.imageView   = reinterpret_cast<VkImageView>(frame->gpuImageView);
                gpuInfo.sampler     = reinterpret_cast<VkSampler>(frame->gpuSampler);
                gpuInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                layer.gpuDescriptor = gpuInfo;
                layer.frameWidth    = frame->width;
                layer.frameHeight   = frame->height;

                // Ã¢â€â‚¬Ã¢â€â‚¬ Bridge CUDA frame into GpuTexCache for dirty-tracking Ã¢â€â‚¬Ã¢â€â‚¬
                // CUDA zero-copy frames bypass the cache-owned upload path,
                // so the GpuTexCache never sees them.  Register the frame's
                // GPU texture here (shared ownership with FrameCache) so
                // future composites can hit the dirty-tracking early-out
                // and skip the entire decode + FrameCache lookup.
                auto* texCache3 = m_engine ? m_engine->textureCache() : nullptr;
                if (texCache3 && m_engine->isGpuCompositeEnabled() &&
                    frame->mediaId != 0 && frame->gpuTextureOwner) {
                    // Sub-timing to pin down which step inside the
                    // GPU-resident layer-build is slow (the 2026-05-22
                    // 13:28:56 [LAYER-SLOW] tick=912000 -> 6372 ms case
                    // had gpuTex=true, so the freeze is in this branch
                    // — but `getInfo`, `putShared`, and the shared_ptr
                    // reset are all candidates and we can't tell which
                    // from LAYER-SLOW alone).
                    auto subT0 = std::chrono::high_resolution_clock::now();
                    auto subMs = [](auto a, auto b) {
                        return std::chrono::duration<double, std::milli>(b - a).count();
                    };

                    // Determine if CUDA frame is still packed-alpha
                    bool cudaPacked = false;
                    if (!frame->unpackedAlpha && m_mediaPool) {
                        auto* fInfo = m_mediaPool->getInfo(frame->mediaId);
                        cudaPacked = (fInfo && fInfo->packedAlpha);
                    }
                    auto subT1 = std::chrono::high_resolution_clock::now();

                    texCache3->putShared(
                        frame->mediaId, frame->frameNumber,
                        static_cast<uint8_t>(frame->tier),
                        frame->gpuTextureOwner,
                        gpuInfo,
                        frame->width, frame->height,
                        static_cast<size_t>(frame->width) * frame->height * 4,
                        cudaPacked, frame->premultipliedAlpha,
                        frame->isLoopFrame,
                        frame->contentBoundsValid,
                        frame->contentLeft, frame->contentTop,
                        frame->contentRight, frame->contentBottom);
                    layer.gpuCacheBacked = true;
                    layer.gpuCacheMediaId = frame->mediaId;
                    layer.gpuCacheFrameNumber = frame->frameNumber;
                    layer.gpuCacheTier = static_cast<uint8_t>(frame->tier);
                    auto subT2 = std::chrono::high_resolution_clock::now();

                    // Transfer sole ownership to GpuTextureCache
                    // (2026-05-22 architectural fix).  putShared took
                    // its own shared_ptr; resetting ours ensures
                    // FrameCache LRU eviction no longer pulls the
                    // texture out from under in-flight compositor work.
                    // From this point GpuTextureCache's pin/unpin +
                    // VRAM budget are the sole controllers of this
                    // texture's lifetime.
                    frame->gpuTextureOwner.reset();
                    auto subT3 = std::chrono::high_resolution_clock::now();

                    const double getInfoMs = subMs(subT0, subT1);
                    const double putSharedMs = subMs(subT1, subT2);
                    const double resetMs   = subMs(subT2, subT3);
                    if (getInfoMs > 10.0 || putSharedMs > 10.0 || resetMs > 10.0) {
                        spdlog::warn("[GPU-LAYER-SUB] clip mediaId={} frame={} "
                                     "getInfo={:.1f}ms putShared={:.1f}ms "
                                     "ownerReset={:.1f}ms",
                                     frame->mediaId, frame->frameNumber,
                                     getInfoMs, putSharedMs, resetMs);
                    }
                }
            } else {
                layer.frame       = frame;
                layer.frameWidth  = frame->width;
                layer.frameHeight = frame->height;
            }

            if (frame && frame->contentBoundsValid) {
                layer.contentBoundsValid = true;
                layer.contentLeft = frame->contentLeft;
                layer.contentTop = frame->contentTop;
                layer.contentRight = frame->contentRight;
                layer.contentBottom = frame->contentBottom;
            }

            if (temporalMode != 0 && temporalFrame && frame &&
                temporalFrame->width == frame->width &&
                temporalFrame->height == frame->height) {
                layer.temporalMode = temporalMode;
                layer.temporalPhase = temporalPhase;
                layer.temporalFrame = temporalFrame;
                // Resolve the second endpoint through uploadLayer(), even when
                // it is already GPU resident.  That path promotes the shared
                // texture into GpuTextureCache and pins it to the active
                // submission slot; handing the raw descriptor straight to the
                // graph here could let its CachedFrame owner disappear before
                // the GPU fence signals.
            }

            // ── [FLICKER-DIAG] (Phase 0 of pipeline upgrade) ─────────────
            // Detect when the SAME media's displayed frame changes converter
            // origin, scale, or tier between consecutive FORWARD frames during
            // steady (non-scrub) playback.  Such a change is exactly what reads
            // on screen as "brightness/position flicker every ~0.5s": the GPU
            // and CPU convert paths are not byte-identical (brightness/edge),
            // and a tier/scale change shifts sub-pixel sampling position.
            // Gated to df∈[1,4] so pauses (df=0) and seeks/scrubs (df<0 or
            // large jumps) don't trigger false positives; scrubMode excluded.
            if (!scrubMode && frame && frame->mediaId != 0) {
                struct FlickerState {
                    ConverterOrigin origin{ConverterOrigin::Unknown};
                    uint32_t w{0}, h{0};
                    ResolutionTier tier{ResolutionTier::Full};
                    int64_t frameNo{-1};
                };
                static std::mutex s_flickerMtx;
                static std::unordered_map<uint64_t, FlickerState> s_flicker;
                std::lock_guard<std::mutex> lk(s_flickerMtx);
                auto& st = s_flicker[frame->mediaId];
                const int64_t df = frame->frameNumber - st.frameNo;
                if (st.frameNo >= 0 && df >= 1 && df <= 4) {
                    const bool originChanged =
                        (st.origin != ConverterOrigin::Unknown &&
                         st.origin != frame->origin);
                    const bool scaleChanged = (st.w != frame->width ||
                                               st.h != frame->height);
                    const bool tierChanged = (st.tier != frame->tier);
                    if (originChanged || scaleChanged || tierChanged) {
                        spdlog::warn("[FLICKER-DIAG] mediaId={} frame {}->{} "
                                     "origin {}->{} dims {}x{}->{}x{} tier {}->{}",
                                     frame->mediaId, st.frameNo, frame->frameNumber,
                                     converterOriginName(st.origin),
                                     converterOriginName(frame->origin),
                                     st.w, st.h, frame->width, frame->height,
                                     static_cast<int>(st.tier),
                                     static_cast<int>(frame->tier));
                    }
                }
                st.origin  = frame->origin;
                st.w       = frame->width;
                st.h       = frame->height;
                st.tier    = frame->tier;
                st.frameNo = frame->frameNumber;
            }

            bool isSpineRendered = false;
#ifdef ROUNDTABLE_HAS_SPINE
            if (dynamic_cast<SpineClip*>(clip)) {
                isSpineRendered = isPreRenderedSpine
                               || gpuSpineZeroCopy
                               || cpuSpineRendered;
            }
#endif
            layer.opacity = opac;
            layer.posX    = px;
            layer.posY    = py;
            // Detect video-character clips so we can apply the 0.85× fit.
            bool isVideoCharClip = false;
            if (auto* vc = dynamic_cast<VideoClip*>(clip)) {
                if (vc->isVideoCharacter())
                    isVideoCharClip = true;
            }
            // Video characters and pre-rendered Spine cache videos composite
            // under CONTAIN-fit + 0.85× user scale, matching COMPOSE's:
            //   displayH = canvasH * 0.85 * userScale
            // The compositor's contain-fit (min of outW/srcW, outH/srcH)
            // collapses a portrait source's fittedH to outH, so a constant
            // 0.85 × userScale gives the correct final height regardless of
            // the current frame's decode state.  This removes the size
            // flicker that previously appeared during scrub / seek / prewarm
            // misses (when frame was momentarily null and the old
            // outH/srcH-based hFit was skipped entirely).
            //
            // The transform overlay (TimelineWorkspaceOverlay.cpp:965-984)
            // already keys off this contract — it switches to contain-fit for
            // SpineClips and video characters, expecting the compositor to
            // do the same.
            // PNG puppets composite like characters: contain-fit the native
            // PNG to canvas height + the shared 0.85× compose-fit.
            const bool isPuppetClip = (dynamic_cast<PngPuppetClip*>(clip) != nullptr);
            float finalSx = sx, finalSy = sy;
            const bool useComposeFit =
                isVideoCharClip || isPreRenderedSpine || gpuSpineZeroCopy ||
                cpuSpineRendered || isPuppetClip;
            if (useComposeFit) {
                constexpr float kComposeFit = 0.85f;
                finalSx *= kComposeFit;
                finalSy *= kComposeFit;
                layer.containFit = true;
            }
            layer.scX     = finalSx;
            layer.scY     = finalSy;
            layer.rot     = rot;
            layer.anchorX = ancX;
            layer.anchorY = ancY;
            assignMotionBlur(layer, useComposeFit ? 0.85f : 1.0f);
            layer.clipId  = clipId;
            layer.blendMode = clip->blendMode();
            layer.needsSwapRB = fromNestedSequence;

            // Packed-alpha: if the frame is still packed (GPU-resident or
            // not yet unpacked by MediaPool), tell the compositor shader
            // to split UV (top half = RGB, bottom half = alpha).
            if (frame && !frame->unpackedAlpha) {
                // Check if source media is packed-alpha
                bool srcPacked = false;
                if (auto* vc = dynamic_cast<VideoClip*>(clip)) {
                    const uint64_t sourceHandle = findMediaHandle(vc->mediaPath());
                    auto* mi = m_mediaPool
                        ? m_mediaPool->getInfo(sourceHandle) : nullptr;
                    srcPacked = (mi && mi->packedAlpha);
                }
                if (isPreRenderedSpine && m_mediaPool) {
                    auto* mi = m_mediaPool->getInfo(frame->mediaId);
                    if (mi && mi->packedAlpha)
                        srcPacked = true;
                }
                if (srcPacked)
                    layer.isPacked = true;
            }

            // Native-alpha video frames are pre-multiplied during decode
            // to avoid white-fringe from linear texture filtering.
            if (frame && frame->premultipliedAlpha)
                layer.isPMA = true;

            // Store wipe transition metadata for GPU spatial blending
            if (activeWipeProgress >= 0.0f) {
                layer.wipeType        = activeWipeType;
                layer.wipeProgress    = activeWipeProgress;
                layer.wipeSoftness    = activeWipeSoftness;
                layer.wipePeerClipId  = activeWipePeer;
                layer.isWipeOutgoing  = activeWipeOutgoing;
            }

            // Read crop from clip if available
            if (auto* vc = dynamic_cast<VideoClip*>(clip)) {
                layer.srcRotation = vc->sourceRotation();
                layer.cropL = vc->cropLeft();
                layer.cropR = vc->cropRight();
                layer.cropT = vc->cropTop();
                layer.cropB = vc->cropBottom();
                // Source display rotation (portrait phone footage etc.).  Same
                // handle lookup the packed-alpha check above uses.
                if (m_mediaPool) {
                    const uint64_t sourceHandle = findMediaHandle(vc->mediaPath());
                    auto* mi = m_mediaPool->getInfo(sourceHandle);
                    if (mi) layer.srcRotation = mi->rotation;
                }
            }
            else if (auto* image = dynamic_cast<ImageClip*>(clip)) {
                layer.cropL = image->cropLeft();
                layer.cropR = image->cropRight();
                layer.cropT = image->cropTop();
                layer.cropB = image->cropBottom();
            }
#ifdef ROUNDTABLE_HAS_SPINE
            else if (auto* sc = dynamic_cast<SpineClip*>(clip)) {
                layer.cropL = sc->cropLeft();
                layer.cropR = sc->cropRight();
                layer.cropT = sc->cropTop();
                layer.cropB = sc->cropBottom();

                // Remap spine crop coordinates: the SHOTS preview crops
                // relative to the character's visible layer rect, but the
                // timeline compositor crops relative to the full FBO/texture
                // UV space which includes transparent margins from the
                // fitZoom=0.9 centering.  Convert character-relative crop
                // to FBO-relative crop so visuals match the SHOTS preview.
                //
                // Skip for pre-rendered cache frames Ã¢â‚¬â€ the character fills
                // the texture with minimal padding, so crops apply directly.
                if (!isPreRenderedSpine &&
                    (layer.cropL > 0.01f || layer.cropR > 0.01f ||
                    layer.cropT > 0.01f || layer.cropB > 0.01f)) {
                    auto cit = m_spineCache.find(sc->id());
                    if (cit != m_spineCache.end() && cit->second->engine.isLoaded()
                        && cit->second->shared) {
                        auto& shared = *cit->second->shared;
                        // Use the SAME bounds the renderer framed the character
                        // with (this animation's box) so the crop margins line
                        // up with the actual character extent in the FBO.
                        float bw = shared.stableBoundsW;
                        float bh = shared.stableBoundsH;
                        {
                            auto abIt = shared.animBounds.find(sc->animationName());
                            if (abIt != shared.animBounds.end()) {
                                bw = abIt->second.w;
                                bh = abIt->second.h;
                            }
                        }
                        if (bw > 1.0f && bh > 1.0f) {
                            float fW = static_cast<float>(outW);
                            float fH = static_cast<float>(outH);
                            // The live Spine texture renders the character at
                            // the FULL texture height (spineScale = outH/bh in
                            // CompositeServiceSpine), centered. So vertically
                            // the character fills the texture (crop already
                            // matches COMPOSE's character-relative crop and
                            // passes through), and horizontally it occupies
                            // only charFracW, centered with marginH each side.
                            // The 0.85x compose-fit scales character AND margins
                            // together, so it cancels in these fractions.
                            // charFracW is aspect-based => tier-independent.
                            float charFracW = std::min(1.0f, (bw / bh) * (fH / fW));
                            float marginH = (1.0f - charFracW) * 0.5f;
                            // Horizontal: convert character-% -> texture-%.
                            layer.cropL = (marginH + (layer.cropL / 100.0f) * charFracW) * 100.0f;
                            layer.cropR = (marginH + (layer.cropR / 100.0f) * charFracW) * 100.0f;
                            // Vertical crop is already character-relative.
                        }
                    }
                }
            }
#endif

            // Evaluate clip effects for GPU processing
            if (clip->effects().hasActiveEffects()) {
                const int64_t effectLocalTick = tick - clip->timelineIn();
                layer.effects = clip->effects().evaluate(effectLocalTick);
            }
            // Opacity masks — snapshot at the same local time
            if (clip->maskCount() > 0)
                layer.masks = evaluateMaskStates(clip->masks(),
                                                 tick - clip->timelineIn());
            // PERF: per-clip timing
            auto perfClipT1 = std::chrono::high_resolution_clock::now();
            double clipMs = std::chrono::duration<double, std::milli>(perfClipT1 - perfClipT0).count();
            const char* clipType = "Unknown";
            if (dynamic_cast<VideoClip*>(clip)) clipType = "Video";
            else if (dynamic_cast<TitleClip*>(clip)) clipType = "Title";
            else if (dynamic_cast<GraphicClip*>(clip)) clipType = "Graphic";
#ifdef ROUNDTABLE_HAS_SPINE
            else if (dynamic_cast<SpineClip*>(clip)) clipType = "Spine";
#endif
            // Warn-level for slow clips so it survives the warn+ logger
            // filter — this is the diagnostic that pins down the 6.4 s
            // layer-build stall observed at 12:16:40 (tick=1811200) in
            // the 2026-05-22 scrub session.  > 50 ms for a single clip
            // is well outside steady-state expectations (a typical
            // 1080p video clip's layer-build is sub-millisecond per the
            // existing info-level timing).
            if (clipMs > 50.0) {
                spdlog::warn("[LAYER-SLOW] clip '{}' type={} tick={} -> {:.1f}ms "
                             "(layer {}x{} backing {}x{} gpuTex={} requestedTier={})",
                             clip->label(), clipType, tick, clipMs,
                             layer.frameWidth, layer.frameHeight,
                             frame ? frame->width : 0,
                             frame ? frame->height : 0,
                             layer.gpuTextureReady,
                             static_cast<int>(requestTier));
            }
            if (perfLog) {
                spdlog::info("  [PERF] clip '{}' type={} -> {:.1f}ms "
                             "(frame {}x{}, gpuTex={} requestedTier={})",
                             clip->label(), clipType, clipMs,
                             layer.frameWidth, layer.frameHeight,
                             layer.gpuTextureReady,
                             static_cast<int>(requestTier));
            }

            layer.clipPtr = clip;
            // Mark loop content so GPU tex cache path is used for character anims
            if (auto* vc = dynamic_cast<VideoClip*>(clip))
                layer.isLoopContent = vc->isVideoCharacter();

            // Track layer index of GPU Spine renders for multi-char readback
            if (m_gpuSpineJustRendered) {
                m_gpuSpinePrevLayer = m_gpuSpineInsertedLayer;
                m_gpuSpineInsertedLayer = static_cast<int>(layers.size());
                m_gpuSpineJustRendered = false;
            }

            layers.push_back(std::move(layer));
            ++resolvedClipsAtTick;
        }
    }

    // ── Prune sticky last-frame caches ──────────────────────────────────
    // Drop entries for clips/characters that are no longer in the active
    // set (e.g. after a shot cut). This honours the documented contract
    // ("pruned when clip IDs go out of the active set"), stops a character
    // from lingering past a cut, and — crucially — releases the held
    // CachedFrame (often GPU-resident, owning a pooled texture) instead of
    // retaining it across shots. Safe here: this map is only touched by
    // buildLayersForFrame (under m_compositeMutex) and reset() (after the
    // prewarm thread is joined). Only at the top level — a nested
    // SequenceClip build must not evict the outer shot's entries.
    if (m_buildLayersDepth == 1) {
        for (auto it = m_stickyLastClipFrame.begin(); it != m_stickyLastClipFrame.end(); ) {
            if (m_frameActiveClipIds.find(it->first) == m_frameActiveClipIds.end())
                it = m_stickyLastClipFrame.erase(it);
            else
                ++it;
        }
        for (auto it = m_stickyLastCharFrame.begin(); it != m_stickyLastCharFrame.end(); ) {
            if (m_frameActiveCharKeys.find(it->first) == m_frameActiveCharKeys.end())
                it = m_stickyLastCharFrame.erase(it);
            else
                ++it;
        }
        // Record the tick so the NEXT top-level build can detect a parked
        // playhead (same tick = static recomposite). See staticRecomposite.
        m_lastBuiltTick = tick;
    }

    return layers;
}

} // namespace rt
