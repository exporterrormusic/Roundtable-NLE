/*
 * CompositeServiceLayerBuild.cpp - Layer collection / building for compositeFrame().
 * Extracted from CompositeServiceFrame.cpp (Step P1.3 of modularization plan).
 */

#include "CompositeService.h"
#include "CompositeServiceLayerBuild.h"
#include "ClipRenderers.h"
#include "CompositeServiceBlend.h"

#include "media/FrameCache.h"
#include "media/MediaPool.h"
#include "Constants.h"
#include "timeline/AdjustmentClip.h"
#include "timeline/AudioClip.h"
#include "timeline/ImageClip.h"
#include "timeline/SequenceClip.h"
#include "timeline/SpineClip.h"
#include "timeline/TitleClip.h"
#include "timeline/GraphicClip.h"
#include "timeline/GraphicLayer.h"
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
#include <unordered_set>

namespace rt {

// ---- buildLayersForFrame ----
std::vector<LayerInfo> CompositeService::buildLayersForFrame(
    int64_t tick, uint32_t outW, uint32_t outH,
    bool scrubMode, bool playbackNonBlocking,
    int& clipsAtTick, bool perfLog,
    std::unique_lock<std::recursive_mutex>& lock,
    bool& gpuSpineUsedThisFrame)
{
    // fetchMediaFrame lambda replaced by CompositeService::resolveMediaFrame()
    // (extracted to CompositeServiceLayerBuildVideo.cpp for modularization).

    std::vector<LayerInfo> layers;

    clipsAtTick = 0;
    m_gpuSpineCount = 0;
    m_gpuSpineInsertedLayer = -1;
    m_gpuSpinePrevLayer = -1;
    m_gpuSpineJustRendered = false;

    // ── Adjustment-layer pre-pass ────────────────────────────────────────
    // Track convention in this app: lower index = HIGHER in the visual stack
    // (see TimelinePanelTracks: "Video: topmost (lowest index) = highest
    // number"). An adjustment on track T must therefore affect clips on
    // tracks with index > T (visually below it), matching Premiere/AE.
    //
    // adjustmentEffectsForTrack[T] = effects from every adjustment clip
    // active at this tick on any track with index < T (above T in the
    // visual stack), in top-down order so they post-process correctly.
    const size_t trackCnt = m_timeline->trackCount();
    std::vector<std::vector<EffectStack::EffectSnapshot>>
        adjustmentEffectsForTrack(trackCnt);
    {
        std::vector<EffectStack::EffectSnapshot> running;
        for (size_t ti = 0; ti < trackCnt; ++ti) {
            adjustmentEffectsForTrack[ti] = running;  // applies BEFORE this track contributes
            auto* tr = m_timeline->track(ti);
            if (!tr || tr->type() != TrackType::Video || tr->isMuted())
                continue;
            for (auto* clip : tr->clipsAtTime(tick)) {
                if (!clip || !clip->isEnabled()) continue;
                auto* adj = dynamic_cast<AdjustmentClip*>(clip);
                if (!adj || !adj->effects().hasActiveEffects()) continue;
                auto eval = adj->effects().evaluate(tick - adj->timelineIn());
                for (auto& e : eval)
                    running.push_back(std::move(e));
            }
        }
    }

    for (size_t ti_rev = trackCnt; ti_rev > 0; --ti_rev) {
        const size_t currentTrackIdx = ti_rev - 1;
        const auto& pendingAdjustmentEffects =
            adjustmentEffectsForTrack[currentTrackIdx];
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
            // Adjustment layers don't render a layer of their own — they
            // contribute their effect stack to lower tracks (harvested after
            // this inner loop). Skip BEFORE ++clipsAtTick so the settle
            // window's "layers.size() < clipsAtTick" check doesn't fire and
            // return an empty sentinel (black flash) every time the cache
            // is invalidated by an effect add/move.
            if (dynamic_cast<AdjustmentClip*>(clip))
                continue;
            ++clipsAtTick;
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
                // Keyframe vector corruption — use defaults and continue.
                // The preroll deferral (isBackgroundWarmupActive gate) should
                // prevent this path from being hit, but this catch-all ensures
                // we never crash the compositor on corrupted keyframe state.
                spdlog::warn("compositeFrame: keyframe evaluation failed for clip {} — using defaults",
                             clipId);
            }

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
                if (prog < 0.0f) continue; // tick outside transition range

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
            bool gpuSpineZeroCopy = false;
            bool isPreRenderedSpine = false;  // set when using cached spine video
            bool cpuSpineRendered  = false;   // set when CPU Spine fallback succeeds
            VkDescriptorImageInfo gpuSpineDescriptor{};
            uint32_t gpuSpineW{0}, gpuSpineH{0};

            // Program monitor video tier follows the playback-resolution
            // dropdown (Full / 1/2 / 1/4 / 1/8).  Used by the SpineClip
            // video-fallback path below; the main VideoClip path computes
            // its own charVideoTier with the same intent + a scrub-time
            // Half override.  forceFullResolution (ExportPanel) wins.
            const auto videoTier = m_forceFullResolution.load()
                ? ResolutionTier::Full
                : playbackTier();

            // ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ VideoClip ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬
            if (auto* videoClip = dynamic_cast<VideoClip*>(clip)) {
                if (!m_mediaPool) continue;  // VideoClip needs MediaPool
                const auto& mediaPath = videoClip->mediaPath();
                if (mediaPath.empty()) continue;

                // Lazy-open media handle (cached by path)
                uint64_t handle = 0;
                auto it = m_openMediaHandles.find(mediaPath);
                if (it != m_openMediaHandles.end() && it->second != 0) {
                    handle = it->second;
                } else if (playbackNonBlocking && !m_forceFullResolution.load()) {
                    // Check if MediaPool finished opening this path in the background.
                    if (m_mediaPool->isPathOpen(mediaPath)) {
                        handle = m_mediaPool->open(mediaPath);
                        m_openMediaHandles[mediaPath] = handle;
                    } else {
                        // Not open yet — start or continue async open.
                        m_mediaPool->openAsync(mediaPath);
                        handle = 0;
                    }
                } else {
                    // Prefer .mp4 packed-alpha (NVDEC hw decode) over .mov/.webm
                    // (software decode).  Try .mp4 FIRST before the original path
                    // so we never open a slow ProRes when a fast HEVC exists.
                    namespace fs = std::filesystem;
                    fs::path p(mediaPath);
                    if (p.extension() == ".mov" || p.extension() == ".webm") {
                        fs::path mp4Path = fs::path(mediaPath).replace_extension(".mp4");
                        if (fs::exists(mp4Path))
                            handle = m_mediaPool->open(mp4Path);
                        if (handle == 0) {
                            fs::path mp4InVideos = fs::path("assets") / "videos" / mp4Path.filename();
                            if (fs::exists(mp4InVideos))
                                handle = m_mediaPool->open(mp4InVideos);
                        }
                    }

                    // Fall back to original path
                    if (handle == 0)
                        handle = m_mediaPool->open(mediaPath);

                    // If that fails, try resolving common search paths
                    if (handle == 0) {
                        // Try alternate video extensions Ã¢â‚¬â€ prefer .mp4 packed-alpha
                        // (NVDEC hardware decode) over .webm (software VP9).
                        std::vector<fs::path> altExts;
                        if (p.extension() == ".webm") {
                            altExts.push_back(fs::path(mediaPath).replace_extension(".mp4"));
                            altExts.push_back(fs::path(mediaPath).replace_extension(".mov"));
                        } else if (p.extension() == ".mov") {
                            altExts.push_back(fs::path(mediaPath).replace_extension(".mp4"));
                            altExts.push_back(fs::path(mediaPath).replace_extension(".webm"));
                        } else if (p.extension() == ".mp4") {
                            altExts.push_back(fs::path(mediaPath).replace_extension(".webm"));
                            altExts.push_back(fs::path(mediaPath).replace_extension(".mov"));
                        }

                        // Common image extensions — for background images referenced
                        // as bare filenames without extension (e.g. "TABLE_LARGE_FINAL")
                        const fs::path imgExts[] = {".png", ".jpg", ".jpeg"};
                        for (const auto& imgExt : imgExts) {
                            altExts.push_back(fs::path(mediaPath).replace_extension(imgExt));
                        }

                        // Search paths for unresolved media (bare filenames or
                        // relative paths)
                        std::vector<fs::path> searchPaths = {
                            fs::path("assets") / "backgrounds" / p.filename(),
                            fs::path("assets") / "videos" / p.filename(),
                            fs::path("assets") / p,
                            p.filename()
                        };

                        // Also try alternate extensions in each search dir
                        for (const auto& altExt : altExts) {
                            if (altExt.empty()) continue;
                            fs::path altName = altExt.filename();
                            searchPaths.push_back(altExt);
                            searchPaths.push_back(fs::path("assets") / "videos" / altName);
                            searchPaths.push_back(fs::path("assets") / altExt);
                            searchPaths.push_back(altName);
                        }

                        for (const auto& candidate : searchPaths) {
                            if (fs::exists(candidate)) {
                                handle = m_mediaPool->open(candidate);
                                if (handle != 0) {
                                    break;
                                }
                            }
                        }
                    }
                    if (handle == 0) {
                        spdlog::warn("compositeFrame: could not resolve media '{}'", mediaPath);
                        continue;
                    }
                    m_openMediaHandles[mediaPath] = handle;
                }

                // Calculate source frame number — scale localTick by clip
                // speed so 2x clips advance source 2x as fast (matches
                // TimelineSnapshotBuilder/FrameRenderer/EditOperationsTrim).
                int64_t srcTick = clip->sourceIn() +
                    static_cast<int64_t>(localTick * clip->effectiveSpeed(localTick));
                // Clamp to 0 instead of skipping: during transition overlap
                // the incoming clip has negative localTick (tick < timelineIn).
                // Using frame 0 gives the correct visual for the transition.
                if (srcTick < 0) srcTick = 0;

                double fps = videoClip->sourceFps();
                if (fps <= 0.0) fps = 24.0;

                int64_t frameNum = static_cast<int64_t>(
                    ticksToSeconds(srcTick) * fps);

                auto* mediaInfo = m_mediaPool->getInfo(handle);

                // If clip had no stored fps, use the authoritative MediaPool fps
                if (videoClip->sourceFps() <= 0.0 && mediaInfo && mediaInfo->fps > 0.0) {
                    fps = mediaInfo->fps;
                    frameNum = static_cast<int64_t>(ticksToSeconds(srcTick) * fps);
                }

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
                    : playbackTier();
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

                // Clamp frame number to valid range.
                // For video characters, wrap with modulo so the animation
                // loops continuously when the clip is stretched beyond the
                // source video's duration.
                if (mediaInfo) {
                    if (mediaInfo->frameCount <= 1) {
                        frameNum = 0;
                    } else if (isVideoChar && mediaInfo->frameCount > 1) {
                        // Looping character animation: modulo wrap
                        frameNum = ((frameNum % mediaInfo->frameCount) + mediaInfo->frameCount)
                                   % mediaInfo->frameCount;
                    } else if (mediaInfo->frameCount > 0) {
                        frameNum = std::clamp(frameNum, int64_t(0),
                                              mediaInfo->frameCount - 1);
                    }
                }

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
                    if (gpuHit.found) {
                        LayerInfo layer;
                        layer.gpuTextureReady = true;
                        layer.gpuDescriptor   = gpuHit.descriptor;
                        layer.frameWidth      = gpuHit.width;
                        layer.frameHeight     = gpuHit.height;
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
                        // Portrait character frames need contain-fit.
                        // Packed-alpha: use the flag stored in the GPU cache
                        // entry at upload time â€” avoids fragile height heuristics.
                        layer.isPacked = gpuHit.isPacked;
                        layer.isPMA    = gpuHit.isPMA;
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
                        // Append adjustment-layer effects from tracks above.
                        if (!pendingAdjustmentEffects.empty()) {
                            layer.effects.insert(layer.effects.end(),
                                                 pendingAdjustmentEffects.begin(),
                                                 pendingAdjustmentEffects.end());
                        }
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
                    frame = resolveMediaFrame(handle, frameNum, charVideoTier, scrubMode);
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

                // Packed-alpha unpack is now handled entirely by:
                //   - GPU path: compositor shader isPacked UV split
                //   - CPU path: MediaPool::unpackPackedAlphaInPlace()
                // No manual unpack needed here.
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
            // â”€â”€ SequenceClip (nested sequence) â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
            else if (auto* seqClip = dynamic_cast<SequenceClip*>(clip)) {
                if (m_project && seqClip->sequenceIndex() < m_project->sequenceCount()) {
                    auto* innerTimeline = m_project->sequence(seqClip->sequenceIndex());
                    if (innerTimeline && innerTimeline != m_timeline) {
                        // Map the local tick into the inner timeline, honoring
                        // the clip's sourceIn so a trimmed nested sequence
                        // (in/out set in the source monitor) shows the right
                        // inner content instead of always starting at 0.
                        int64_t innerTick = localTick + clip->sourceIn();
                        if (innerTick < 0) innerTick = 0;

                        // Force CPU display mode for the recursive composite
                        // so it does the GPU→CPU readback INLINE while we
                        // still hold the composite mutex. In GPU display mode
                        // the inner composite returns a GPU-resident frame
                        // backed by the SHARED composite output image; the
                        // outer composite then immediately reuses that image,
                        // racing the inner's deferred readback.
                        const bool wasGpuMode = m_gpuDisplayMode;
                        m_gpuDisplayMode = false;

                        // Temporarily swap to the inner timeline and release
                        // the lock so the recursive compositeFrame can acquire it.
                        Timeline* outerTimeline = m_timeline;
                        m_timeline = innerTimeline;
                        lock.unlock();

                        // Render the inner at the project's master
                        // resolution rather than the (possibly scrubbed)
                        // outer outW/outH.  Inner-layer decodes still
                        // honour playbackTier internally — this only sets
                        // the inner's output canvas size, keeping the
                        // SequenceClip snapshot's dimensions consistent
                        // between Full playback and scrubbed ticks so the
                        // SequenceClip's transform produces stable geometry
                        // every frame instead of subtly jittering as the
                        // canvas shrinks/grows.  Falls back to outW/outH
                        // when m_project is somehow null.
                        uint32_t innerW = outW;
                        uint32_t innerH = outH;
                        if (m_project) {
                            const auto& res = m_project->settings().resolution();
                            if (res.width > 0 && res.height > 0) {
                                innerW = res.width;
                                innerH = res.height;
                            }
                        }

                        // isNestedRecursion=true so the inner composite
                        // doesn't touch the outer's m_lastGoodComposite /
                        // LRU / invalidate flag.  Without this, the inner
                        // overwrites the cached frame the presenter reads,
                        // producing the "nested sequence glitches to its
                        // own first frame every other display tick" bug.
                        auto innerFrame = compositeFrame(innerTick, innerW, innerH, scrubMode,
                                                          /*isNestedRecursion=*/true);

                        // Snapshot into a clean CPU-only BGRA frame. The
                        // inner composite returns its shared m_lastGoodComposite,
                        // which:
                        //   • still has gpuReady/gpuImageView set (CPU display
                        //     mode only changes presentation, tryCompositeOnGpu
                        //     still tags the frame GPU-resident), AND
                        //   • alternates between the GPU-composited frame and
                        //     the single-layer fast path that returns the raw
                        //     decoded frame.
                        // Those two cases differ in channel order, so the old
                        // unconditional needsSwapRB swapped only every other
                        // frame → "every other frame inverted". A decoded /
                        // read-back frame is ALWAYS straight BGRA (the project
                        // convention), so copying pixels into a plain CPU
                        // frame and treating it exactly like a normal video
                        // layer (needsSwapRB=false) is correct for BOTH inner
                        // paths and removes the GPU-aliasing race entirely.
                        if (innerFrame && innerFrame->ensurePixels() &&
                            !innerFrame->pixels.empty()) {
                            auto cpu = std::make_shared<CachedFrame>();
                            cpu->width              = innerFrame->width;
                            cpu->height             = innerFrame->height;
                            cpu->stride             = innerFrame->stride
                                ? innerFrame->stride : innerFrame->width * 4;
                            cpu->pixels             = innerFrame->pixels;
                            cpu->unpackedAlpha      = true;
                            cpu->premultipliedAlpha = innerFrame->premultipliedAlpha;
                            frame = std::move(cpu);
                        } else {
                            frame = nullptr;
                        }
                        // NOTE: deliberately NOT setting fromNestedSequence —
                        // the snapshot is plain BGRA, identical to any other
                        // CPU video layer, so no R/B swap is needed.

                        // Restore outer timeline and reacquire the lock
                        lock.lock();
                        m_timeline = outerTimeline;
                        m_gpuDisplayMode = wasGpuMode;
                    }
                }
            }
#ifdef ROUNDTABLE_HAS_SPINE
            // ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ SpineClip ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬
            else if (auto* spineClip = dynamic_cast<SpineClip*>(clip)) {
              try {

                // Ã¢â€â‚¬Ã¢â€â‚¬ Pre-rendered animation video cache (fast path) Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬
                // If this character/outfit/animation has been pre-rendered
                // to a VP9+alpha WebM, decode a frame from it instead of
                // evaluating Spine live.  This is ~10x faster.
                bool usedPreRendered = false;
                if (false /* SpineClips use GPU live rendering */ && m_animVideoCache) {
                    const std::string& charName = spineClip->characterName();
                    const std::string& outfit   = spineClip->outfit();
                    const std::string& animName = spineClip->animationName();

                    // Pick the talk variant when talking is enabled
                    const std::string cacheAnimName = spineClip->isTalking()
                        ? (animName + "_talk") : animName;

                    MediaHandle animHandle = m_animVideoCache->getMediaHandle(
                        charName, outfit, cacheAnimName);
                    if (animHandle != InvalidMedia && m_mediaPool) {
                        const auto* animInfo = m_mediaPool->getInfo(animHandle);
                        if (animInfo && animInfo->frameCount > 0 && animInfo->fps > 0.0) {
                            // Calculate loop frame index
                            const int64_t animTick = spineClip->useGlobalTime()
                                                         ? tick : localTick;
                            const float animTime = static_cast<float>(
                                ticksToSeconds(animTick)) * spineClip->animationSpeed();
                            const int64_t totalFrames = animInfo->frameCount;
                            int64_t animFrame = static_cast<int64_t>(
                                animTime * animInfo->fps);
                            // Wrap for looping
                            if (spineClip->isLooping() && totalFrames > 0) {
                                animFrame = ((animFrame % totalFrames) + totalFrames) % totalFrames;
                            } else {
                                animFrame = std::clamp(animFrame, int64_t(0), totalFrames - 1);
                            }

                            // Ã¢â€â‚¬Ã¢â€‚¬ DIRTY TRACKING: Skip decode for pre-rendered spine Ã¢â€â‚¬Ã¢â€â‚¬
                            // Pre-rendered spine tier: matches VideoClip logic
                            // (drop Full->Half during scrub since composite
                            // output is already halved by ProgramMonitor).
                            auto baseSpineTier = m_forceFullResolution.load()
                                ? ResolutionTier::Full
                                : playbackTier();
                            ResolutionTier spineVideoTier;
                            if (scrubMode && !m_forceFullResolution.load()) {
                                if (baseSpineTier == ResolutionTier::Full)
                                    spineVideoTier = ResolutionTier::Half;
                                else if (baseSpineTier == ResolutionTier::Half)
                                    spineVideoTier = ResolutionTier::Quarter;
                                else
                                    spineVideoTier = ResolutionTier::Quarter;
                            } else {
                                spineVideoTier = baseSpineTier;
                            }

                            auto* texCache2 = m_engine ? m_engine->textureCache() : nullptr;
                            if (texCache2 && m_engine->isGpuCompositeEnabled()) {
                                auto gpuHit = texCache2->get(animHandle, animFrame,
                                    static_cast<uint8_t>(spineVideoTier));
                                if (gpuHit.found) {
                                    LayerInfo layer;
                                    layer.gpuTextureReady = true;
                                    layer.gpuDescriptor   = gpuHit.descriptor;
                                    layer.frameWidth      = gpuHit.width;
                                    layer.frameHeight     = gpuHit.height;
                                    layer.opacity  = opac;
                                    layer.posX     = px;
                                    layer.posY     = py;
                                    layer.scX      = sx;
                                    layer.scY      = sy;
                                    layer.rot      = rot;
                                    layer.anchorX  = ancX;
                                    layer.anchorY  = ancY;
                                    layer.clipId   = clipId;
                                    layer.blendMode = clip->blendMode();
                                    // Pre-rendered Spine cache video: composite
                                    // under CONTAIN-fit + 0.85× user scale to
                                    // match COMPOSE (see notes in the regular
                                    // video-character path below).
                                    {
                                        constexpr float kCF = 0.85f;
                                        layer.scX *= kCF;
                                        layer.scY *= kCF;
                                        layer.containFit = true;
                                    }
                                    // Packed-alpha: use the flag stored in the GPU cache
                                    // entry at upload time.
                                    layer.isPacked = gpuHit.isPacked;
                                    layer.isPMA    = gpuHit.isPMA;
                                    if (activeWipeProgress >= 0.0f) {
                                        layer.wipeType        = activeWipeType;
                                        layer.wipeProgress    = activeWipeProgress;
                                        layer.wipeSoftness    = activeWipeSoftness;
                                        layer.wipePeerClipId  = activeWipePeer;
                                        layer.isWipeOutgoing  = activeWipeOutgoing;
                                    }
                                    layer.cropL = spineClip->cropLeft();
                                    layer.cropR = spineClip->cropRight();
                                    layer.cropT = spineClip->cropTop();
                                    layer.cropB = spineClip->cropBottom();
                                    if (clip->effects().hasActiveEffects())
                                        layer.effects = clip->effects().evaluate(localTick);
                                    if (!pendingAdjustmentEffects.empty()) {
                                        layer.effects.insert(layer.effects.end(),
                                                             pendingAdjustmentEffects.begin(),
                                                             pendingAdjustmentEffects.end());
                                    }
                                    if (perfLog) {
                                        auto perfClipT1 = std::chrono::high_resolution_clock::now();
                                        double clipMs = std::chrono::duration<double, std::milli>(
                                            perfClipT1 - perfClipT0).count();
                                        spdlog::info("  [PERF] clip '{}' type=Spine (GPU CACHE HIT) -> {:.1f}ms ({}x{})",
                                                     clip->label(), clipMs, gpuHit.width, gpuHit.height);
                                    }
                                    layer.clipPtr = clip;
                                    layers.push_back(std::move(layer));
                                    continue;
                                }
                            }

                            // Spine character overlays follow the playback-
                            // resolution dropdown (same as other clips). The
                            // old pinned-Half meant "Full" never raised the
                            // character resolution in the source-monitor
                            // sequence preview.
                            frame = resolveMediaFrame(animHandle, animFrame,
                                                    spineVideoTier, scrubMode);
                            if (frame) {
                                // Packed-alpha unpack is now handled by:
                                //   - GPU path: compositor shader isPacked UV split
                                //   - CPU path: MediaPool::unpackPackedAlphaInPlace()
                                usedPreRendered = true;
                                isPreRenderedSpine = true;
                                m_lastPreRenderedSpineFrame[clipId] = frame;
                            } else {
                                // Keep source stable: if pre-rendered media exists but
                                // this frame missed (non-blocking tryGetFrame), reuse the
                                // last pre-rendered frame for this clip instead of falling
                                // back to live Spine.  This prevents visible oscillation
                                // between 816x1920 packed video and 960x540 live Spine.
                                auto itLast = m_lastPreRenderedSpineFrame.find(clipId);
                                if (itLast != m_lastPreRenderedSpineFrame.end() && itLast->second) {
                                    frame = itLast->second;
                                    usedPreRendered = true;
                                    isPreRenderedSpine = true;
                                } else {
                                    // No pre-rendered frame has ever landed for this clip
                                    // yet.  Fall back to live Spine so the character stays
                                    // visible during warm-up.
                                    usedPreRendered = false;
                                    isPreRenderedSpine = false;
                                }
                            }
                        } else {
                            // Bad/corrupt cache file (0 frames) Ã¢â‚¬â€ remove entry
                            // and queue re-render so we don't retry every frame
                            m_animVideoCache->removeEntry(charName, outfit, cacheAnimName);
                            m_animVideoCache->queueRender(charName, outfit, animName,
                                                           spineClip->isTalking());
                        }
                    } else if (animHandle == InvalidMedia) {
                        // Not cached yet Ã¢â‚¬â€ queue background pre-render
                        m_animVideoCache->queueRender(charName, outfit, animName,
                                                       spineClip->isTalking());
                    }
                }

                // Ã¢â€â‚¬Ã¢â€â‚¬ Live Spine evaluation (fallback when no pre-render) Ã¢â€â‚¬Ã¢â€â‚¬
                if (!usedPreRendered) {
                // Try GPU Spine rendering first (faster + better quality)
                bool gpuSpineDone = false;
                auto& gpuCtx = GpuContext::get();
                if (gpuCtx.isInitialized()) {
                    auto* sr = gpuCtx.spineRenderer(outW, outH);
                    if (sr && sr->isInitialized()) {
                        // Multi-character GPU Spine: each character renders on
                        // GPU.  The first character stays as GPU zero-copy (fast,
                        // no readback).  When a SECOND character arrives, we
                        // readback the FBO (which holds the FIRST character's
                        // pixels) before the next beginFrame() clears it, and
                        // assign that readback to the first character's layer.
                        // Subsequent characters follow the same pattern.
                        // Non-blocking: get cached state or schedule background load
                        const uint64_t cid = spineClip->id();
                        auto sit = m_spineCache.find(cid);
                        if (sit == m_spineCache.end()) {
                            tryGetSpineState(spineClip);
                            sit = m_spineCache.find(cid);
                        }

                        if (sit != m_spineCache.end() && sit->second
                            && sit->second->engine.isLoaded()
                            && sit->second->shared
                            && sit->second->shared->boundsCached) {
                            auto& state = *sit->second;
                            auto& shared = *state.shared;

                            // Upload atlas textures to GPU only when the
                            // active character set changes.  Consecutive
                            // clips of the same character skip the upload.
                            // MUST release old textures first: different
                            // characters have different atlas page counts.
                            // Without a full release, extra pages from the
                            // previous character leak and the wrong texture
                            // gets sampled for pages beyond the current
                            // character's atlas size.
                            const std::string charKey = spineCharKey(*spineClip);
                            if (m_gpuSpineActiveCharKey != charKey) {
                                sr->releaseAllTextures();
                                m_gpuSpineActiveCharKey.clear();
                                int loaded = 0;
                                if (!shared.pagePixels.empty()) {
                                    for (size_t pi = 0; pi < shared.pagePixels.size(); ++pi) {
                                        if (shared.pagePixels[pi].empty()) continue;
                                        if (sr->uploadAtlasTexture(
                                                static_cast<int>(pi),
                                                shared.pagePixels[pi].data(),
                                                static_cast<uint32_t>(shared.pageWidths[pi]),
                                                static_cast<uint32_t>(shared.pageHeights[pi]),
                                                "shared")) {
                                            ++loaded;
                                        }
                                    }
                                } else {
                                    loaded = sr->loadAtlasTextures(state.engine.atlas());
                                }
                                if (loaded > 0) {
                                    m_gpuSpineActiveCharKey = charKey;
                                    spdlog::info("[SPINE-GPU] uploaded atlas for '{}': {} pages (was '{}')",
                                                  spineClip->characterName(), loaded,
                                                  m_gpuSpineActiveCharKey);
                                } else {
                                    spdlog::warn("[SPINE-GPU] atlas upload FAILED for '{}' ({} pages)",
                                                  spineClip->characterName(), shared.pagePixels.size());
                                }
                            }

                            if (m_gpuSpineActiveCharKey == charKey) {
                                // Evaluate animation
                                const int64_t animTick = spineClip->useGlobalTime()
                                                             ? tick : localTick;
                                const float timeSeconds = static_cast<float>(ticksToSeconds(animTick));
                                state.engine.evaluateAtTime(
                                    timeSeconds * spineClip->animationSpeed(), timeSeconds);

                                SpineRenderData renderData = state.engine.extractMeshes();
                                if (!renderData.batches.empty()) {
                                    // Use STABLE (setup-pose) bounds for scale so the character
                                    // doesn't appear to zoom in/out when live bounds fluctuate
                                    // per frame (e.g. Crown whose idle animation bbox oscillates).
                                    // Fall back to live bounds when stable bounds are degenerate.
                                    float liveBx{0}, liveBy{0}, liveBw{0}, liveBh{0};
                                    state.engine.getBounds(liveBx, liveBy, liveBw, liveBh);
                                    const float bw = (shared.stableBoundsW > 1.0f) ? shared.stableBoundsW
                                                    : ((liveBw > 1.0f) ? liveBw : shared.stableBoundsW);
                                    const float bh = (shared.stableBoundsH > 1.0f) ? shared.stableBoundsH
                                                    : ((liveBh > 1.0f) ? liveBh : shared.stableBoundsH);
                                    const float fW = static_cast<float>(outW);
                                    const float fH = static_cast<float>(outH);
                                    float fitZoom = 1.0f;
                                    float cx = shared.stableBoundsX + shared.stableBoundsW * 0.5f;
                                    float cy = shared.stableBoundsY + shared.stableBoundsH * 0.5f;
                                    if (bw > 1.0f && bh > 1.0f) {
                                        fitZoom = (fH / bh) * 0.85f;
                                    }
                                    // One-time diagnostic: compare stableBounds vs liveBounds
                                    {
                                        static bool s_spineGpuBoundsLogged = false;
                                        if (!s_spineGpuBoundsLogged) {
                                            s_spineGpuBoundsLogged = true;
                                            spdlog::info("=== SPINE SIZING DIAGNOSTIC (GPU) ===");
                                            spdlog::info("  clip='{}' char='{}'",
                                                         spineClip->label(), spineClip->characterName());
                                            spdlog::info("  stableBounds: x={:.1f} y={:.1f} w={:.1f} h={:.1f}",
                                                         shared.stableBoundsX, shared.stableBoundsY,
                                                         shared.stableBoundsW, shared.stableBoundsH);
                                            spdlog::info("  liveBounds:   x={:.1f} y={:.1f} w={:.1f} h={:.1f}",
                                                         liveBx, liveBy, liveBw, liveBh);
                                            spdlog::info("  usedBounds:   w={:.1f} h={:.1f}", bw, bh);
                                            spdlog::info("  FBO={}x{} fitZoom={:.4f} cx={:.1f} cy={:.1f}",
                                                         outW, outH, fitZoom, cx, cy);
                                            spdlog::info("  sx={:.4f} finalSx={:.4f}",
                                                         sx, sx);
                                            spdlog::info("=====================================");
                                        }
                                    }

                                    glm::mat4 proj = SpineRenderer::orthoProjection(
                                        fW, fH, cx, cy, fitZoom);
                                    glm::mat4 model = SpineRenderer::modelMatrix(0.0f, 0.0f);
                                    glm::mat4 mvp = proj * model;

                                    // Readback the PREVIOUS character's FBO content
                                    // BEFORE rendering the next one.  The FBO still
                                    // holds the previous character's pixels (from
                                    // its waitForFrame), so read them out now before
                                    // beginFrame() clears the FBO.
                                    if (m_gpuSpineCount > 0) {
                                        sr->waitForFrame();
                                        auto readback = sr->readbackPixels();
                                        if (readback && m_gpuSpineInsertedLayer >= 0 &&
                                            static_cast<size_t>(m_gpuSpineInsertedLayer) < layers.size()) {
                                            auto& prevLayer = layers[m_gpuSpineInsertedLayer];
                                            prevLayer.frame = std::move(readback);
                                            prevLayer.gpuTextureReady = false;
                                        }
                                    }

                                    sr->beginFrame();
                                    sr->renderSkeleton(renderData, mvp, 1.0f);
                                    sr->endFrame();

                                    sr->waitForFrame();

                                    gpuSpineDone = true;
                                    gpuSpineZeroCopy = true;
                                    gpuSpineUsedThisFrame = true;
                                    ++m_gpuSpineCount;
                                    m_gpuSpineJustRendered = true;
                                    gpuSpineDescriptor = sr->outputDescriptorInfo();
                                    gpuSpineW = outW;
                                    gpuSpineH = outH;
                                }
                            }
                        }
                    }
                }

                // CPU fallback if GPU Spine didn't produce a frame
                // WARNING: CPU Spine rendering is very slow. This is a TEMPORARY
                // LAST RESORT only Ã¢â‚¬â€ fix the GPU Spine path instead.
                if (!gpuSpineDone) {
                    frame = renderSpineClip(spineClip, tick, outW, outH);
                    if (frame) {
                        cpuSpineRendered = true;
                        spdlog::warn("[SPINE-RENDER] '{}' GPU unavailable Ã¢â‚¬â€ SLOW CPU raster: {}x{}",
                                     spineClip->characterName(),
                                     frame->width, frame->height);
                    } else {
                        spdlog::error("[SPINE-RENDER] '{}' FAILED both GPU and CPU paths",
                                      spineClip->characterName());
                    }
                } else if (gpuSpineZeroCopy) {
                    spdlog::info("[SPINE-RENDER] '{}' GPU zero-copy: {}x{}",
                                 spineClip->characterName(), gpuSpineW, gpuSpineH);
                }

                // Fallback: if Spine rendering failed (no skeleton), check
                // whether this character is actually a video character and
                // render via MediaPool instead.  Cache the result so we
                // don't re-lookup the preset on every frame.
                if (false /* no video fallback for SpineClips */) {
                    const uint64_t cid = spineClip->id();
                    auto& fb = m_videoFallbackCache[cid];

                    if (!fb.looked_up) {
                        fb.looked_up = true;
                        if (m_shotPresetManager) {
                            const std::string charName = spineClip->characterName();
                            std::string presetName = charName + " (Default)";
                            auto preset = m_shotPresetManager->load(presetName);
                            if (preset) {
                                for (int ci2 = 0; ci2 < preset->characterCount(); ++ci2) {
                                    auto* ch = preset->character(ci2);
                                    if (!ch || ch->characterName != charName || !ch->isVideoCharacter())
                                        continue;
                                    fb.videoPath = ch->activeVideoPath();
                                    break;
                                }
                            }
                            if (!fb.videoPath.empty()) {
                                // Prefer .mp4 packed-alpha (NVDEC hw decode)
                                // over .mov/.webm (software decode).
                                {
                                    namespace fs = std::filesystem;
                                    fs::path vp(fb.videoPath);
                                    if (vp.extension() == ".mov" || vp.extension() == ".webm") {
                                        fs::path mp4 = fs::path(fb.videoPath).replace_extension(".mp4");
                                        if (fs::exists(mp4))
                                            fb.handle = m_mediaPool->open(mp4);
                                        if (fb.handle == 0) {
                                            fs::path mp4v = fs::path("assets") / "videos" / mp4.filename();
                                            if (fs::exists(mp4v))
                                                fb.handle = m_mediaPool->open(mp4v);
                                        }
                                    }
                                }
                                if (fb.handle == 0)
                                    fb.handle = m_mediaPool->open(fb.videoPath);
                                if (fb.handle == 0) {
                                    namespace fs = std::filesystem;
                                    fs::path vp(fb.videoPath);
                                    // Try filename in assets/videos
                                    fs::path candidate = fs::path("assets") / "videos" / vp.filename();
                                    if (fs::exists(candidate))
                                        fb.handle = m_mediaPool->open(candidate);
                                    // Try alternate extensions Ã¢â‚¬â€ prefer .mp4 packed-alpha
                                    if (fb.handle == 0) {
                                        std::vector<std::string> tryExts;
                                        if (vp.extension() == ".webm") {
                                            tryExts = {".mp4", ".mov"};
                                        } else if (vp.extension() == ".mov") {
                                            tryExts = {".mp4", ".webm"};
                                        } else if (vp.extension() == ".mp4") {
                                            tryExts = {".webm", ".mov"};
                                        }
                                        for (const auto& ext : tryExts) {
                                            if (fb.handle != 0) break;
                                            fs::path altPath = vp;
                                            altPath.replace_extension(ext);
                                            if (altPath != vp) {
                                                if (fs::exists(altPath))
                                                    fb.handle = m_mediaPool->open(altPath);
                                                if (fb.handle == 0) {
                                                    candidate = fs::path("assets") / "videos" / altPath.filename();
                                                    if (fs::exists(candidate))
                                                        fb.handle = m_mediaPool->open(candidate);
                                                }
                                            }
                                        }
                                    }
                                }
                                if (fb.handle != 0) {
                                }
                            }
                        }
                    }

                    if (fb.handle != 0) {
                        int64_t srcTick = clip->sourceIn() +
                            static_cast<int64_t>(localTick * clip->effectiveSpeed(localTick));
                        if (srcTick >= 0) {
                            double fps = 24.0;
                            auto* mInfo = m_mediaPool->getInfo(fb.handle);
                            if (mInfo && mInfo->fps > 0.0) fps = mInfo->fps;
                            int64_t frameNum = static_cast<int64_t>(ticksToSeconds(srcTick) * fps);
                            if (mInfo && mInfo->frameCount <= 1) frameNum = 0;
                            frame = resolveMediaFrame(fb.handle, frameNum,
                                                    videoTier, scrubMode);
                        }
                    } else if (playbackNonBlocking && !m_forceFullResolution.load() && !fb.videoPath.empty()) {
                        // Spine video fallback async open
                        if (m_mediaPool->isPathOpen(fb.videoPath)) {
                            fb.handle = m_mediaPool->open(fb.videoPath);
                        } else {
                            m_mediaPool->openAsync(fb.videoPath);
                        }
                    }
                }
                } // end if (!usedPreRendered)

              } // try
              catch (const std::exception& ex) {
                spdlog::error("compositeFrame: SpineClip '{}' exception: {}",
                              spineClip->characterName(), ex.what());
              }
              catch (...) {
                spdlog::error("compositeFrame: SpineClip '{}' unknown exception",
                              spineClip->characterName());
              }
            }
#endif
            else {
                continue;
            }

            // GPU zero-copy spine layers bypass the CachedFrame requirement.
            // GPU-resident decoded frames (gpuReady) may have empty pixels.
            if (!gpuSpineZeroCopy && (!frame || (frame->pixels.empty() && !frame->gpuReady))) {
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
                    frame = stickyFrame;
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

            LayerInfo layer;
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
                        frame->isLoopFrame);
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
            float finalSx = sx, finalSy = sy;
            if (isVideoCharClip || isPreRenderedSpine) {
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
                    auto* mi = m_mediaPool ? m_mediaPool->getInfo(
                        m_openMediaHandles.count(vc->mediaPath())
                            ? m_openMediaHandles[vc->mediaPath()] : 0)
                        : nullptr;
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
                layer.cropL = vc->cropLeft();
                layer.cropR = vc->cropRight();
                layer.cropT = vc->cropTop();
                layer.cropB = vc->cropBottom();
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
                        float bw = shared.stableBoundsW;
                        float bh = shared.stableBoundsH;
                        if (bw > 1.0f && bh > 1.0f) {
                            float fW = static_cast<float>(outW);
                            float fH = static_cast<float>(outH);
                            // Height-based fit matching COMPOSE (0.85×)
                            float fitZoom = (fH / bh) * 0.85f;
                            float charFracW = std::min(1.0f, fitZoom * bw / fW);
                            float charFracH = std::min(1.0f, fitZoom * bh / fH);
                            float marginH = (1.0f - charFracW) * 0.5f;
                            float marginV = (1.0f - charFracH) * 0.5f;
                            // Include transparent margin + proportional character crop.
                            // Values stay in 0-100 range (divided by 100 later for GPU,
                            // used directly for CPU blitLayerWithTransform).
                            layer.cropL = (marginH + (layer.cropL / 100.0f) * charFracW) * 100.0f;
                            layer.cropR = (marginH + (layer.cropR / 100.0f) * charFracW) * 100.0f;
                            layer.cropT = (marginV + (layer.cropT / 100.0f) * charFracH) * 100.0f;
                            layer.cropB = (marginV + (layer.cropB / 100.0f) * charFracH) * 100.0f;
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
            // Append adjustment-layer effects from tracks above this one
            // so they post-process this layer (Premiere/AE behaviour).
            if (!pendingAdjustmentEffects.empty()) {
                layer.effects.insert(layer.effects.end(),
                                     pendingAdjustmentEffects.begin(),
                                     pendingAdjustmentEffects.end());
            }

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
                             "(frame {}x{} gpuTex={} tier={})",
                             clip->label(), clipType, tick, clipMs,
                             layer.frameWidth, layer.frameHeight,
                             layer.gpuTextureReady,
                             playbackNonBlocking ? "Half" : "Full");
            }
            if (perfLog) {
                spdlog::info("  [PERF] clip '{}' type={} -> {:.1f}ms (frame {}x{}, gpuTex={} tier={})",
                             clip->label(), clipType, clipMs,
                             layer.frameWidth, layer.frameHeight,
                             layer.gpuTextureReady,
                             playbackNonBlocking ? "Half" : "Full");
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
        }
    }
    return layers;
}

} // namespace rt
