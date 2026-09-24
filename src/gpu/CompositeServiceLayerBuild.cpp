/*
 * CompositeServiceLayerBuild.cpp - Layer collection / building for compositeFrame().
 * Extracted from CompositeServiceFrame.cpp (Step P1.3 of modularization plan).
 *
 * buildLayersForFrame() walks the video tracks bottom-up and turns each
 * active clip into a LayerInfo: evaluate its transform and transition state,
 * resolve its source frame per clip type, fall back to a sticky last-good
 * frame when nothing is ready, then fill in the compositor-facing fields.
 * Each of those steps lives in a helper below.
 */

#include "CompositeService.h"
#include "CompositeServiceLayerBuild.h"
#include "ClipRenderers.h"
#include "effects/CpuComposite.h"
#include "PathUtils.h"

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
#include "GpuTextureCache.h"
#include "SpineRenderer.h"

#include <thread>
#include <algorithm>
#include <unordered_set>
#include <unordered_map>
#include <mutex>
#include <cmath>
#include <cstdlib>
#include <filesystem>

namespace rt {

namespace {

// Video characters, Spine renders and PNG puppets composite under
// CONTAIN-fit + this factor × user scale, matching COMPOSE's
//   displayH = canvasH * 0.85 * userScale
constexpr float kComposeFit = 0.85f;

/// A clip's evaluated transform in output pixels, plus the shutter-angle
/// motion-blur exposure endpoints.
struct ClipTransform
{
    float opac{1.0f};
    float px{0.0f}, py{0.0f};
    float sx{1.0f}, sy{1.0f}, rot{0.0f};
    float ancX{0.0f}, ancY{0.0f};
    LayerTransformSample motionStart{};
    LayerTransformSample motionEnd{};
    int32_t motionSampleCount{1};
};

/// The GPU-blended transition (wipe / fade / two-clip dissolve) a clip takes
/// part in at this tick. progress < 0 means none.
struct ClipTransitionState
{
    TransitionType type{TransitionType::CrossDissolve};
    float progress{-1.0f};
    float softness{-1.0f};
    uint64_t peer{0};
    bool outgoing{false};
};

/// The track's clips at `tick`, plus clips outside their normal range that
/// take part in an active cross-dissolve.  Without the latter only ONE clip
/// renders and we get a dip-to-black instead of a true simultaneous dissolve.
std::vector<Clip*> activeClipsWithTransitionPeers(Track* track, int64_t tick)
{
    auto active = track->clipsAtTime(tick);

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
    return active;
}

/// Evaluate opacity, position, scale, rotation and anchor at `localTick`.
/// Position and anchor are stored in REFERENCE resolution (1920×1080) and
/// are scaled to the actual output resolution here, so compositing looks
/// correct at any viewport size.  (Effect Controls converts the stored value
/// to/from sequence pixels for display only — see EffectControlsPanelTree.)
ClipTransform evaluateClipTransform(Clip* clip, int64_t tick, int64_t localTick,
                                    float scaleToOutX, float scaleToOutY,
                                    double sequenceFps)
{
    ClipTransform t;

    // Guard: if the clip's internal state is invalid (e.g. timeline
    // population is still in progress from a background thread),
    // skip this clip rather than crashing on Keyframe<float> iteration.
    // Each evaluate() call does a binary search on std::vector<Keyframe>;
    // if the vector was moved/corrupted concurrently, we'd ACCESS_VIOLATION.
    try {
        t.opac = clip->opacity().evaluate(localTick);
        {
            auto p2 = evaluatePosition2D(clip->positionX(), clip->positionY(), localTick);
            t.px = p2.first  * scaleToOutX;
            t.py = p2.second * scaleToOutY;
        }
        t.sx   = clip->scaleX().evaluate(localTick);
        t.sy   = clip->scaleY().evaluate(localTick);
        t.rot  = clip->rotation().evaluate(localTick);
        // Anchor stored as REF-1920 px (same convention as position);
        // convert to output px so the GPU transform builder treats it
        // in the same space as posX/posY.
        t.ancX = clip->anchorX().evaluate(localTick) * scaleToOutX;
        t.ancY = clip->anchorY().evaluate(localTick) * scaleToOutY;
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
                     clip->id());
    }

    // PNG puppet "breathing": add a gentle, deterministic drift (sway +
    // rise/fall + tiny scale/rotation) on top of the clip's own
    // transform so the character feels alive instead of static.  Pure
    // function of GLOBAL timeline time so the motion is continuous
    // across cuts between same-character clips (no jump when switching
    // expressions); identical in preview and export.
    if (auto* puppetClip = dynamic_cast<PngPuppetClip*>(clip)) {
        const auto bo = puppetClip->breathing(ticksToSeconds(tick));
        t.px  += bo.dx * scaleToOutX;
        t.py  += bo.dy * scaleToOutY;
        t.sx  *= bo.scale;
        t.sy  *= bo.scale;
        t.rot += bo.rot;
    }

    // Evaluate the transform exposure endpoints for shutter-angle
    // motion blur. The source frame stays at the current time; only
    // Position, Scale, Rotation, and Anchor are sampled temporally.
    t.motionStart = {t.px, t.py, t.sx, t.sy, t.rot, t.ancX, t.ancY};
    t.motionEnd = t.motionStart;
    float shutterAngle = 0.0f;
    try {
        shutterAngle = std::clamp(
            clip->shutterAngle().evaluate(localTick), 0.0f, 720.0f);
    } catch (...) {
        shutterAngle = 0.0f;
    }
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
            t.motionStart = evaluateMotionAt(startTick);
            t.motionEnd = evaluateMotionAt(endTick);
            const bool moved =
                std::abs(t.motionStart.posX - t.motionEnd.posX) > 0.001f ||
                std::abs(t.motionStart.posY - t.motionEnd.posY) > 0.001f ||
                std::abs(t.motionStart.scX - t.motionEnd.scX) > 0.00001f ||
                std::abs(t.motionStart.scY - t.motionEnd.scY) > 0.00001f ||
                std::abs(t.motionStart.rot - t.motionEnd.rot) > 0.0001f ||
                std::abs(t.motionStart.anchorX - t.motionEnd.anchorX) > 0.001f ||
                std::abs(t.motionStart.anchorY - t.motionEnd.anchorY) > 0.001f;
            if (moved)
                t.motionSampleCount = 8;
        } catch (...) {
            t.motionStart = {t.px, t.py, t.sx, t.sy, t.rot, t.ancX, t.ancY};
            t.motionEnd = t.motionStart;
            t.motionSampleCount = 1;
        }
    }
    return t;
}

/// Find the transition this clip takes part in at `tick`.  Fades and
/// dissolves between two clips and spatial wipes are returned for the GPU
/// transition path; single-clip cross-dissolves modulate `opac` directly.
ClipTransitionState resolveClipTransition(Track* track, int64_t tick,
                                          uint64_t clipId, float& opac)
{
    ClipTransitionState st;

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
                st.type = TransitionType::FadeFromBlack;
                st.progress = prog;
                st.softness = trans->param1;
                st.peer = 0;
                st.outgoing = true;
            }
        } else if (trans->type == TransitionType::FadeToBlack) {
            // Single-clip GPU fade: clip fades out to black.
            if (trans->leftClipId == clipId) {
                st.type = TransitionType::FadeToBlack;
                st.progress = prog;
                st.softness = trans->param1;
                st.peer = 0;
                st.outgoing = true;
            }
        } else if (trans->type == TransitionType::FadeFromWhite) {
            // Single-clip GPU fade: clip fades in from white.
            if (trans->rightClipId == clipId) {
                st.type = TransitionType::FadeFromWhite;
                st.progress = prog;
                st.softness = trans->param1;
                st.peer = 0;
                st.outgoing = true;
            }
        } else if (trans->type == TransitionType::FadeToWhite) {
            // Single-clip GPU fade: clip fades out to white.
            if (trans->leftClipId == clipId) {
                st.type = TransitionType::FadeToWhite;
                st.progress = prog;
                st.softness = trans->param1;
                st.peer = 0;
                st.outgoing = true;
            }
        } else if (trans->type == TransitionType::CrossDissolve) {
            // Route CrossDissolve through the GPU transition path
            // (TransitionRenderer mixes A and B in a single pass).
            //
            // Pure-opacity modulation is WRONG when a lower video
            // track is present: at progress=0.5 each clip renders
            // at 50% alpha, so the combined front coverage is only
            // p*R + (1-p)^2*L = 0.75, and 0.25 of the lower track
            // leaks through — visually "darkening" the dissolve.
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
                st.type = TransitionType::CrossDissolve;
                st.progress = prog;
                st.softness = trans->param1;
                st.peer = trans->rightClipId;
                st.outgoing = true;
            } else if (trans->rightClipId == clipId && trans->leftClipId != 0) {
                // Two-clip: this is the incoming side (passive).
                st.type = TransitionType::CrossDissolve;
                st.progress = prog;
                st.softness = trans->param1;
                st.peer = trans->leftClipId;
                st.outgoing = false;
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
            // GPU-spatial transition: don't modulate opacity — store info for GPU path.
            // Both clips render at full opacity; spatial blending is
            // done later by TransitionRenderer.
            if (trans->leftClipId == clipId) {
                st.type = trans->type;
                st.progress = prog;
                st.softness = trans->param1;
                st.peer = trans->rightClipId;
                st.outgoing = true;
            } else if (trans->rightClipId == clipId) {
                st.type = trans->type;
                st.progress = prog;
                st.softness = trans->param1;
                st.peer = trans->leftClipId;
                st.outgoing = false;
            }
        }
    }
    return st;
}

void assignMotionBlur(LayerInfo& layer, const ClipTransform& t, float composeFit)
{
    layer.motionStart = t.motionStart;
    layer.motionEnd = t.motionEnd;
    layer.motionStart.scX *= composeFit;
    layer.motionStart.scY *= composeFit;
    layer.motionEnd.scX *= composeFit;
    layer.motionEnd.scY *= composeFit;
    layer.motionSampleCount = t.motionSampleCount;
}

/// Store wipe transition metadata for GPU spatial blending.
void applyTransitionState(LayerInfo& layer, const ClipTransitionState& st)
{
    if (st.progress >= 0.0f) {
        layer.wipeType        = st.type;
        layer.wipeProgress    = st.progress;
        layer.wipeSoftness    = st.softness;
        layer.wipePeerClipId  = st.peer;
        layer.isWipeOutgoing  = st.outgoing;
    }
}

/// Evaluate clip effects and opacity masks at the same local time.
void applyEffectsAndMasks(LayerInfo& layer, Clip* clip, int64_t localTick)
{
    if (clip->effects().hasActiveEffects())
        layer.effects = clip->effects().evaluate(localTick);
    if (clip->maskCount() > 0)
        layer.masks = evaluateMaskStates(clip->masks(), localTick);
}

/// Character/media-level sticky key — the same string across all clip IDs
/// that reference the same character animation or video file.  This is what
/// lets a BRAND NEW shot of Modernia/Chime reuse the previous shot's last
/// frame while her loop cache warms up.  Empty for other clip types.
std::string stickyCharKey(Clip* clip)
{
#ifdef ROUNDTABLE_HAS_SPINE
    if (auto* sc = dynamic_cast<SpineClip*>(clip)) {
        return sc->characterName() + "|" + sc->outfit() + "|" +
               sc->animationName() + (sc->isTalking() ? "|t" : "|m");
    }
#endif
    if (auto* vc = dynamic_cast<VideoClip*>(clip))
        return vc->mediaPath();
    return {};
}

/// Decode tier for a VideoClip.  Both characters and other video follow the
/// playback-resolution dropdown so Full really means Full, etc.
/// (Previously characters were pinned to Half regardless of the dropdown
/// setting — the user picked Full and the character preview stayed blurry.
/// forceFullResolution from ExportPanel still wins for the export/preview
/// path.)
///
/// Scrub override: during timeline scrub at Full tier, decode at Half. The
/// Program Monitor already renders scrub composites at output/2
/// (ProgramMonitor::onPollTimer "resDivisor * 2"), so a Full-tier media
/// decode is 4× the cost for pixels that get downscaled away. Premiere does
/// the same — scrubs use a lower-quality preview. forceFullResolution
/// (export) still gets Full. At Full→Half, decode cost is 4× lower; at
/// Half→Quarter, 2× lower.  When the user stops scrubbing, the next frame
/// request at the normal tier restores full quality.
ResolutionTier videoDecodeTier(ResolutionTier requestTier,
                               bool forceFullResolution, bool scrubMode)
{
    const auto baseVideoTier = forceFullResolution
        ? ResolutionTier::Full
        : requestTier;
    if (scrubMode && !forceFullResolution) {
        // Always drop at least one tier during scrub/4x playback
        if (baseVideoTier == ResolutionTier::Full)
            return ResolutionTier::Half;
        return ResolutionTier::Quarter; // Half → Quarter; Quarter is the floor
    }
    return baseVideoTier;
}

/// The already-decoded frame(s) a VideoClip needs this tick.
struct VideoFrameRequest
{
    uint64_t handle{0};
    int64_t frameNum{0};
    ResolutionTier tier{ResolutionTier::Full};
    bool wantsTemporal{false};
    int64_t secondFrameNum{-1};
    int32_t temporalMode{0};
    float temporalPhase{0.0f};
};

/// DIRTY TRACKING: when the GPU texture cache already holds every frame the
/// request needs, build the layer straight from the cached descriptors and
/// skip the entire decode + CPU packed-alpha unpack path.  This is the
/// single biggest performance win: backgrounds and unchanged animation loops
/// skip ALL CPU work (~60% of frames at 60fps when source is 24fps).
/// Returns false on a miss (including a partial temporal hit).
bool buildVideoGpuCacheHitLayer(GpuTextureCache& texCache,
                                VideoClip* videoClip,
                                const VideoFrameRequest& req,
                                const VideoStreamInfo* mediaInfo,
                                bool contentStableForStateCache,
                                const ClipTransform& t,
                                const ClipTransitionState& transition,
                                int64_t localTick,
                                LayerInfo& layer,
                                GpuTextureCache::LookupResult& gpuHitOut)
{
    auto gpuHit = texCache.get(req.handle, req.frameNum,
        static_cast<uint8_t>(req.tier));
    auto temporalGpuHit = req.wantsTemporal
        ? texCache.get(req.handle, req.secondFrameNum,
            static_cast<uint8_t>(req.tier))
        : GpuTextureCache::LookupResult{};
    const bool temporalGpuCompatible =
        !req.wantsTemporal ||
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
    if (!gpuHit.found || !temporalGpuCompatible)
        return false;

    const bool isVideoChar = videoClip->isVideoCharacter();
    layer.contentStableForStateCache = contentStableForStateCache;
    layer.gpuTextureReady = true;
    layer.gpuDescriptor   = gpuHit.descriptor;
    layer.gpuCacheBacked = true;
    layer.gpuCacheMediaId = req.handle;
    layer.gpuCacheFrameNumber = req.frameNum;
    layer.gpuCacheTier = static_cast<uint8_t>(req.tier);
    layer.frameWidth      = gpuHit.width;
    layer.frameHeight     = gpuHit.height;
    if (req.wantsTemporal) {
        layer.temporalMode = req.temporalMode;
        layer.temporalPhase = req.temporalPhase;
        layer.temporalGpuTextureReady = true;
        layer.temporalGpuDescriptor = temporalGpuHit.descriptor;
        layer.temporalGpuCacheBacked = true;
        layer.temporalGpuCacheMediaId = req.handle;
        layer.temporalGpuCacheFrameNumber = req.secondFrameNum;
        layer.temporalGpuCacheTier = static_cast<uint8_t>(req.tier);
    }
    layer.srcRotation = mediaInfo
        ? mediaInfo->rotation : videoClip->sourceRotation();
    layer.opacity  = t.opac;
    layer.posX     = t.px;
    layer.posY     = t.py;
    layer.rot      = t.rot;
    layer.anchorX  = t.ancX;
    layer.anchorY  = t.ancY;
    layer.clipId   = videoClip->id();
    layer.blendMode = videoClip->blendMode();
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
    layer.scX = t.sx;
    layer.scY = t.sy;
    if (isVideoChar) {
        layer.scX *= kComposeFit;
        layer.scY *= kComposeFit;
        layer.containFit = true;
    }
    assignMotionBlur(layer, t, isVideoChar ? kComposeFit : 1.0f);
    // Packed-alpha: use the flag stored in the GPU cache
    // entry at upload time — avoids fragile height heuristics.
    layer.isPacked = gpuHit.isPacked;
    layer.isPMA    = gpuHit.isPMA;
    layer.contentBoundsValid = gpuHit.contentBoundsValid;
    layer.contentLeft = gpuHit.contentLeft;
    layer.contentTop = gpuHit.contentTop;
    layer.contentRight = gpuHit.contentRight;
    layer.contentBottom = gpuHit.contentBottom;
    applyTransitionState(layer, transition);
    layer.cropL = videoClip->cropLeft();
    layer.cropR = videoClip->cropRight();
    layer.cropT = videoClip->cropTop();
    layer.cropB = videoClip->cropBottom();
    applyEffectsAndMasks(layer, videoClip, localTick);
    layer.clipPtr = videoClip;
    layer.isLoopContent = isVideoChar;
    gpuHitOut = gpuHit;
    return true;
}

/// [FLICKER-DIAG] (Phase 0 of pipeline upgrade)
/// Detect when the SAME media's displayed frame changes converter
/// origin, scale, or tier between consecutive FORWARD frames during
/// steady (non-scrub) playback.  Such a change is exactly what reads
/// on screen as "brightness/position flicker every ~0.5s": the GPU
/// and CPU convert paths are not byte-identical (brightness/edge),
/// and a tier/scale change shifts sub-pixel sampling position.
/// Gated to df∈[1,4] so pauses (df=0) and seeks/scrubs (df<0 or
/// large jumps) don't trigger false positives.
void logFlickerDiag(const CachedFrame& frame)
{
    struct FlickerState {
        ConverterOrigin origin{ConverterOrigin::Unknown};
        uint32_t w{0}, h{0};
        ResolutionTier tier{ResolutionTier::Full};
        int64_t frameNo{-1};
    };
    static std::mutex s_flickerMtx;
    static std::unordered_map<uint64_t, FlickerState> s_flicker;
    std::lock_guard<std::mutex> lk(s_flickerMtx);
    auto& st = s_flicker[frame.mediaId];
    const int64_t df = frame.frameNumber - st.frameNo;
    if (st.frameNo >= 0 && df >= 1 && df <= 4) {
        const bool originChanged =
            (st.origin != ConverterOrigin::Unknown &&
             st.origin != frame.origin);
        const bool scaleChanged = (st.w != frame.width ||
                                   st.h != frame.height);
        const bool tierChanged = (st.tier != frame.tier);
        if (originChanged || scaleChanged || tierChanged) {
            spdlog::warn("[FLICKER-DIAG] mediaId={} frame {}->{} "
                         "origin {}->{} dims {}x{}->{}x{} tier {}->{}",
                         frame.mediaId, st.frameNo, frame.frameNumber,
                         converterOriginName(st.origin),
                         converterOriginName(frame.origin),
                         st.w, st.h, frame.width, frame.height,
                         static_cast<int>(st.tier),
                         static_cast<int>(frame.tier));
        }
    }
    st.origin  = frame.origin;
    st.w       = frame.width;
    st.h       = frame.height;
    st.tier    = frame.tier;
    st.frameNo = frame.frameNumber;
}

const char* clipTypeName(Clip* clip)
{
    if (dynamic_cast<VideoClip*>(clip)) return "Video";
    if (dynamic_cast<TitleClip*>(clip)) return "Title";
    if (dynamic_cast<GraphicClip*>(clip)) return "Graphic";
#ifdef ROUNDTABLE_HAS_SPINE
    if (dynamic_cast<SpineClip*>(clip)) return "Spine";
#endif
    return "Unknown";
}

double msSince(std::chrono::high_resolution_clock::time_point t0)
{
    return std::chrono::duration<double, std::milli>(
        std::chrono::high_resolution_clock::now() - t0).count();
}

} // namespace

// ---- buildLayersForFrame ----
std::vector<LayerInfo> CompositeService::buildLayersForFrame(
    int64_t tick, uint32_t outW, uint32_t outH,
    bool scrubMode, bool playbackNonBlocking,
    ResolutionTier requestTier, bool stillMode,
    const RenderExecutionContext& context,
    int& clipsAtTick, int& resolvedClipsAtTick, bool perfLog,
    bool& gpuSpineUsedThisFrame)
{
    auto* const renderTimeline = context.timeline;
    const auto& policy = context.policy;
    if (!renderTimeline)
        return {};

    // Track reentrancy so the sticky-cache prune at the end only fires for
    // the top-level frame (nested SequenceClips re-enter via compositeFrame).
    struct DepthGuard {
        int& d;
        explicit DepthGuard(int& x) : d(x) { ++d; }
        ~DepthGuard() { --d; }
    } buildDepthGuard(m_buildLayersDepth);

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

    clipsAtTick = 0;
    resolvedClipsAtTick = 0;
    const size_t trackCnt = renderTimeline->trackCount();
#ifdef ROUNDTABLE_HAS_SPINE
    // Per-asset occurrence numbering is stable because tracks are traversed
    // deterministically. Most shots use instance zero; a duplicate character
    // on another active track gets its own framebuffer for this frame.
    std::unordered_map<std::string, uint32_t> spineAssetOccurrences;
#endif

    for (size_t ti_rev = trackCnt; ti_rev > 0; --ti_rev) {
        auto* track = renderTimeline->track(ti_rev - 1);
        if (!track || track->type() != TrackType::Video || track->isMuted())
            continue;

        for (auto* clip : activeClipsWithTransitionPeers(track, tick)) {
            auto perfClipT0 = std::chrono::high_resolution_clock::now();
            if (!clip->isEnabled()) continue;
            // Adjustment layers are explicit render-stream boundaries.  The
            // GPU compositor flattens everything below this marker and runs
            // the stack once over the full frame.  Do not count the marker as
            // a decoded clip: it intentionally has no source frame.
            if (auto* adjustment = dynamic_cast<AdjustmentClip*>(clip)) {
                const int64_t localTick = tick - adjustment->timelineIn();
                const float effectStrength = adjustmentLayerStrengthAtTick(
                    *adjustment, localTick, tick, track->transitions());
                appendAdjustmentLayerBoundary(
                    layers, *adjustment, localTick, effectStrength);
                continue;
            }
            ++clipsAtTick;

            // Record this clip/character as active this frame so its sticky
            // last-frame entry survives the end-of-frame prune.
            m_frameActiveClipIds.insert(clip->id());
            if (auto charKey = stickyCharKey(clip); !charKey.empty())
                m_frameActiveCharKeys.insert(std::move(charKey));

            const bool fromNestedSequence = false;

            constexpr float REF_W = 1920.0f;
            constexpr float REF_H = 1080.0f;
            const float scaleToOutX = static_cast<float>(outW) / REF_W;
            const float scaleToOutY = static_cast<float>(outH) / REF_H;
            const int64_t localTick = tick - clip->timelineIn();
            auto* graphicClipForTransform = dynamic_cast<GraphicClip*>(clip);
            const bool graphicOuterTransformBaked = graphicClipForTransform
                && rt::graphicClipBakesOuterTransform(graphicClipForTransform,
                                                       localTick);

            const uint64_t clipId = clip->id();
            ClipTransform xf = evaluateClipTransform(
                clip, tick, localTick, scaleToOutX, scaleToOutY,
                renderTimeline->settings().frameRate());
            // Transition opacity modulation (fades & dissolves); wipe
            // transitions return metadata for GPU spatial blending.
            const ClipTransitionState transition =
                resolveClipTransition(track, tick, clipId, xf.opac);

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
            std::shared_ptr<void> gpuSpineOwner;
            uint32_t gpuSpineW{0}, gpuSpineH{0};

            // ── VideoClip ───────────────────────────────────────────────
            if (auto* videoClip = dynamic_cast<VideoClip*>(clip)) {
                // Resolve (lazily open / search-fallback) the media handle.
                // skipClip = no MediaPool / empty path / unresolved media.
                // A 0 handle WITHOUT skipClip means the async open is still
                // pending - proceed so the sticky-frame fallback can run.
                bool skipClip = false;
                uint64_t handle = resolveVideoClipHandle(
                    videoClip, playbackNonBlocking,
                    policy.forceFullResolution, skipClip, context.outcome);
                if (skipClip) continue;

                // Tick → source-frame mapping: ONE shared authority
                // (speed scaling, negative clamp, llround, fps priority,
                // still/character wrap policy) — see VideoFrameMapping.h.
                // This is the RENDER side; the pending-frame collector and
                // the lookahead prewarm call the same function, so what
                // gets prefetched is exactly what gets rendered.
                // frameNum is already wrapped/clamped by mapTickToSourceFrame
                // (still → 0, video character → modulo loop, else clamp).
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

                const ResolutionTier charVideoTier = videoDecodeTier(
                    requestTier, policy.forceFullResolution, scrubMode);

                auto* texCache = m_engine ? m_engine->textureCache() : nullptr;
                if (texCache && m_engine->isGpuCompositeEnabled()) {
                    const VideoFrameRequest req{
                        handle, frameNum, charVideoTier, wantsTemporal,
                        temporalSecondFrameNum, temporalMode, temporalPhase};
                    LayerInfo layer;
                    GpuTextureCache::LookupResult gpuHit;
                    if (buildVideoGpuCacheHitLayer(
                            *texCache, videoClip, req, mediaInfo,
                            contentStableForStateCache, xf, transition,
                            localTick, layer, gpuHit)) {
                        if (perfLog) {
                            spdlog::info("  [PERF] clip '{}' type=Video (GPU CACHE HIT) -> {:.1f}ms ({}x{}) frame={}",
                                         clip->label(), msSince(perfClipT0),
                                         gpuHit.width, gpuHit.height, frameNum);
                        }
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
                                              policy.forceFullResolution,
                                              /*exactCacheOnly=*/wantsTemporal,
                                              stillMode);
                    // MediaPool may return a nearby/last-good frame to keep
                    // sequential playback smooth.  It is provisional input,
                    // not the render for this timeline tick.  In particular,
                    // adjustment effects must not turn it into a persistent
                    // composite-cache entry under the requested tick.
                    sourceFallbackPending =
                        frame && frame->frameNumber != frameNum;
                    const double rmfMs = msSince(rmfT0);
                    if (rmfMs > 30.0) {
                        spdlog::warn("[RESOLVE-SLOW] handle={} frame={} tier={} "
                                     "scrub={} still={} -> {:.1f}ms",
                                     handle, frameNum, static_cast<int>(charVideoTier),
                                     scrubMode, stillMode, rmfMs);
                    }
                }

                // Resolve the later endpoint independently.  During normal
                // playback this remains non-blocking and schedules the cache
                // miss; export's force-exact path resolves it synchronously.
                // Never blend against resolveMediaFrame's stale fallback.
                if (wantsTemporal) {
                    temporalFrame = resolveMediaFrame(
                        handle, temporalSecondFrameNum, charVideoTier, scrubMode,
                        policy.forceFullResolution,
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
                                                        policy.forceFullResolution,
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
                const uint64_t handle = resolveImageClipHandle(
                    imageClip, playbackNonBlocking,
                    policy.forceFullResolution, context.outcome);
                if (handle == 0) continue;
                contentStableForStateCache = true;
                frame = resolveMediaFrame(handle, 0, requestTier, scrubMode,
                                          policy.forceFullResolution,
                                          /*exactCacheOnly=*/false, stillMode);
            }
            // ── TitleClip ───────────────────────────────────────────────
            else if (auto* titleClip = dynamic_cast<TitleClip*>(clip)) {
                frame = renderTitleClip(titleClip, tick, outW, outH);
            }
            // ── GraphicClip (multi-layer) ────────────────────────────────
            else if (auto* graphicClip = dynamic_cast<GraphicClip*>(clip)) {
                // Pass this sequence's output resolution so text scales
                // proportionally at reduced render resolutions (scrub).
                const auto& refResolution = renderTimeline->settings().resolution();
                const uint32_t refW = refResolution.width;
                const uint32_t refH = refResolution.height;
                frame = rt::renderGraphicClip(graphicClip, tick, outW, outH, refW, refH);
            }
            // ── CaptionClip (burned-in subtitle overlay) ────────────────────
            else if (auto* captionClip = dynamic_cast<CaptionClip*>(clip)) {
                // Pass this sequence's resolution so caption font metrics
                // stay proportional at reduced render resolutions (scrub).
                const auto& refResolution = renderTimeline->settings().resolution();
                const uint32_t refW = refResolution.width;
                const uint32_t refH = refResolution.height;
                frame = rt::renderCaptionClip(captionClip, tick, outW, outH, refW, refH);
            }
            // ── SequenceClip (nested sequence) ─────────────────────────────────
            else if (auto* seqClip = dynamic_cast<SequenceClip*>(clip)) {
                // Recursive composite of the nested sequence into a clean CPU
                // BGRA frame (see CompositeServiceLayerBuildNested.cpp).
                frame = buildSequenceClipFrame(seqClip, localTick, outW, outH,
                                               scrubMode, requestTier, stillMode,
                                               context);
            }
            // ── PngPuppetClip (Veadotube-style 4-image character) ───────────
            // Use GLOBAL tick so talk/blink phase carries across cuts between
            // same-character clips (kept in sync via a character-derived seed).
            else if (auto* puppetClip = dynamic_cast<PngPuppetClip*>(clip)) {
                frame = renderPngPuppetClip(puppetClip, tick, outW, outH);
                if (!frame && context.outcome)
                    reportPngPuppetFailure(puppetClip, tick, *context.outcome);
            }
            // ── TierListClip (ranking board) ────────────────────────────────
            else if (auto* tierClip = dynamic_cast<TierListClip*>(clip)) {
                contentStableForStateCache = true;
                const auto& refResolution = renderTimeline->settings().resolution();
                const uint32_t refW = refResolution.width;
                const uint32_t refH = refResolution.height;
                frame = rt::renderTierListClip(tierClip, tick, outW, outH, refW, refH);
            }
#ifdef ROUNDTABLE_HAS_SPINE
            // ── SpineClip ───────────────────────────────────────────────
            else if (auto* spineClip = dynamic_cast<SpineClip*>(clip)) {
                const std::string spineAssetKey = spineCharKey(*spineClip);
                const uint32_t simultaneousAssetInstance =
                    spineAssetOccurrences[spineAssetKey]++;
                auto spineRes = buildSpineClipLayer(
                    spineClip, tick, localTick, outW, outH,
                    simultaneousAssetInstance,
                    gpuSpineUsedThisFrame, xf.sx, context.outcome);
                frame              = spineRes.frame;
                gpuSpineZeroCopy   = spineRes.gpuSpineZeroCopy;
                gpuSpineDescriptor = spineRes.gpuSpineDescriptor;
                gpuSpineOwner      = std::move(spineRes.gpuSpineOwner);
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
                // Export must never borrow a sticky last-good source picture.
                // Interactive still/scrub requests are progressive: showing
                // the available source now keeps seeks and Play responsive,
                // and settle retries replace it when the exact frame arrives.
                if (stillMode && policy.forceFullResolution)
                    continue;
                if (!applyStickyFrameFallback(clip, tick, temporalPrimaryFrameNum,
                                              frame, sourceFallbackPending))
                    continue;
            } else if (!gpuSpineZeroCopy && frame &&
                       (!frame->pixels.empty() || frame->gpuReady)) {
                recordStickyFrame(clip, frame);
            }

            // A provisional source is valid for Program Monitor feedback but
            // never for export. sourceFallbackPending prevents this composite
            // entering the LRU, so a settle retry can replace it with exact
            // media rather than pinning the temporary picture to this tick.
            if (stillMode && policy.forceFullResolution && sourceFallbackPending)
                continue;

            LayerInfo layer;
            layer.sourceFallbackPending = sourceFallbackPending;
            layer.contentStableForStateCache = contentStableForStateCache;
            if (gpuSpineZeroCopy) {
                // No CachedFrame needed — the spine FBO is already on the GPU
                layer.gpuTextureReady = true;
                layer.gpuDescriptor   = gpuSpineDescriptor;
                layer.gpuResourceOwner = std::move(gpuSpineOwner);
                layer.frameWidth      = gpuSpineW;
                layer.frameHeight     = gpuSpineH;
                layer.isPMA           = true;  // Spine FBO uses PMA blending
            } else if (frame->gpuReady && frame->gpuImageView && frame->gpuSampler) {
                bindGpuResidentFrame(layer, frame);
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

            if (!scrubMode && frame && frame->mediaId != 0)
                logFlickerDiag(*frame);

            layer.opacity = xf.opac;
            layer.posX    = graphicOuterTransformBaked ? 0.0f : xf.px;
            layer.posY    = graphicOuterTransformBaked ? 0.0f : xf.py;
            // Video characters, Spine renders (pre-rendered cache, GPU FBO or
            // CPU fallback) and PNG puppets composite under CONTAIN-fit +
            // 0.85× user scale, matching COMPOSE's:
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
            auto* videoClipForFit = dynamic_cast<VideoClip*>(clip);
            const bool isVideoCharClip =
                videoClipForFit && videoClipForFit->isVideoCharacter();
            const bool isPuppetClip = (dynamic_cast<PngPuppetClip*>(clip) != nullptr);
            float finalSx = xf.sx, finalSy = xf.sy;
            const bool useComposeFit =
                isVideoCharClip || isPreRenderedSpine || gpuSpineZeroCopy ||
                cpuSpineRendered || isPuppetClip;
            if (useComposeFit) {
                finalSx *= kComposeFit;
                finalSy *= kComposeFit;
                layer.containFit = true;
            }
            layer.scX     = graphicOuterTransformBaked ? 1.0f : finalSx;
            layer.scY     = graphicOuterTransformBaked ? 1.0f : finalSy;
            layer.rot     = graphicOuterTransformBaked ? 0.0f : xf.rot;
            layer.anchorX = graphicOuterTransformBaked ? 0.0f : xf.ancX;
            layer.anchorY = graphicOuterTransformBaked ? 0.0f : xf.ancY;
            if (graphicOuterTransformBaked) {
                const LayerTransformSample identity{};
                layer.motionStart = identity;
                layer.motionEnd = identity;
                layer.motionStart.scX = layer.motionStart.scY = 1.0f;
                layer.motionEnd.scX = layer.motionEnd.scY = 1.0f;
                layer.motionSampleCount = 1;
            } else {
                assignMotionBlur(layer, xf, useComposeFit ? kComposeFit : 1.0f);
            }
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

            applyTransitionState(layer, transition);
            applyClipCropAndRotation(layer, clip, isPreRenderedSpine, outW, outH);
            applyEffectsAndMasks(layer, clip, localTick);

            // PERF: per-clip timing.  Warn-level for slow clips so it
            // survives the warn+ logger filter — this is the diagnostic that
            // pins down the 6.4 s layer-build stall observed at 12:16:40
            // (tick=1811200) in the 2026-05-22 scrub session.  > 50 ms for a
            // single clip is well outside steady-state expectations (a
            // typical 1080p video clip's layer-build is sub-millisecond per
            // the existing info-level timing).
            const double clipMs = msSince(perfClipT0);
            if (clipMs > 50.0) {
                spdlog::warn("[LAYER-SLOW] clip '{}' type={} tick={} -> {:.1f}ms "
                             "(scrub={} still={} layer {}x{} backing {}x{} "
                             "gpuTex={} requestedTier={})",
                             clip->label(), clipTypeName(clip), tick, clipMs,
                             scrubMode, stillMode,
                             layer.frameWidth, layer.frameHeight,
                             frame ? frame->width : 0,
                             frame ? frame->height : 0,
                             layer.gpuTextureReady,
                             static_cast<int>(requestTier));
            }
            if (perfLog) {
                spdlog::info("  [PERF] clip '{}' type={} -> {:.1f}ms "
                             "(frame {}x{}, gpuTex={} requestedTier={})",
                             clip->label(), clipTypeName(clip), clipMs,
                             layer.frameWidth, layer.frameHeight,
                             layer.gpuTextureReady,
                             static_cast<int>(requestTier));
            }

            layer.clipPtr = clip;
            // Mark loop content so GPU tex cache path is used for character anims
            if (videoClipForFit)
                layer.isLoopContent = videoClipForFit->isVideoCharacter();

            layers.push_back(std::move(layer));
            ++resolvedClipsAtTick;
        }
    }

    // Only at the top level — a nested SequenceClip build must not evict the
    // outer shot's entries.
    if (m_buildLayersDepth == 1)
        pruneStickyFrameCaches();

    return layers;
}

// ---- buildLayersForFrame helpers ----

uint64_t CompositeService::resolveImageClipHandle(
    ImageClip* imageClip, bool playbackNonBlocking, bool forceFullResolution,
    RenderExecutionOutcome* outcome)
{
    if (!m_mediaPool) {
        if (outcome) {
            outcome->report(
                RenderResultStatus::Failed,
                "image source cannot be resolved without a media pool");
        }
        return 0;
    }
    if (imageClip->mediaPath().empty()) {
        if (outcome) {
            outcome->report(
                RenderResultStatus::MissingMedia,
                "image clip has no source path");
        }
        return 0;
    }
    const std::string& mediaPath = imageClip->mediaPath();
    uint64_t handle = findMediaHandle(mediaPath);
    if (handle == 0) {
        if (playbackNonBlocking && !forceFullResolution) {
            if (m_mediaPool->isPathOpen(mediaPath)) {
                handle = m_mediaPool->open(mediaPath);
                if (handle != 0) registerMediaHandle(mediaPath, handle);
            } else {
                if (m_mediaPool->pathState(mediaPath) ==
                    ResourceLoadState::Missing) {
                    if (outcome) {
                        outcome->report(
                            RenderResultStatus::MissingMedia,
                            "image source is offline: " + mediaPath);
                    }
                    return 0;
                }
                m_mediaPool->openAsync(mediaPath);
                return 0;
            }
        } else {
            handle = m_mediaPool->open(mediaPath);
            if (handle != 0) registerMediaHandle(mediaPath, handle);
        }
    }
    if (handle == 0) {
        if (m_mediaPool->pathState(mediaPath) ==
                ResourceLoadState::Missing && outcome) {
            outcome->report(
                RenderResultStatus::MissingMedia,
                "image source is offline: " + mediaPath);
        }
    }
    return handle;
}

void CompositeService::reportPngPuppetFailure(PngPuppetClip* puppetClip,
                                              int64_t tick,
                                              RenderExecutionOutcome& outcome)
{
    const int face = puppetClip->selectFace(ticksToSeconds(tick));
    const std::string selected = puppetClip->facePath(face);
    const std::string idle = puppetClip->facePath(
        PngPuppetClip::MouthClosedEyesOpen);
    const bool hasCandidate = !selected.empty() || !idle.empty();
    const bool candidateExists =
        (!selected.empty() &&
         std::filesystem::exists(utf8ToPath(selected))) ||
        (!idle.empty() &&
         std::filesystem::exists(utf8ToPath(idle)));
    const std::string resource = !selected.empty() ? selected : idle;
    if (!hasCandidate || !candidateExists) {
        outcome.report(
            RenderResultStatus::MissingMedia,
            resource.empty()
                ? "PNG puppet has no configured face image: " +
                      puppetClip->characterName()
                : "PNG puppet image is offline: " + resource);
    } else {
        outcome.report(
            RenderResultStatus::Failed,
            "PNG puppet image could not be decoded: " + resource);
    }
}

// Sticky last-good-frame fallback: if we've previously rendered this clip
// (or another clip of the same character/media) successfully, reuse that
// frame instead of dropping the layer.  Eliminates the "character vanishes
// for a few frames" pop-out when loop pre-decode hasn't caught up, a Spine
// skeleton is still loading, or a scrub outpaces prefetch.
bool CompositeService::applyStickyFrameFallback(
    Clip* clip, int64_t tick, int64_t temporalPrimaryFrameNum,
    std::shared_ptr<CachedFrame>& frame, bool& sourceFallbackPending)
{
    const uint64_t clipId = clip->id();
    const std::string charKey = stickyCharKey(clip);

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
    auto* videoClip = dynamic_cast<VideoClip*>(clip);
    if (stickyFrame) {
        // A regular footage clip must not borrow a sticky frame
        // from a distant source position after a seek. Character
        // loops intentionally wrap and may use their sticky image
        // briefly while the loop decoder catches up.
        if (videoClip && !videoClip->isVideoCharacter() &&
            temporalPrimaryFrameNum >= 0 &&
            !isNearbyPlaybackFrame(temporalPrimaryFrameNum,
                                   stickyFrame->frameNumber)) {
            stickyFrame.reset();
        }
    }
    if (!stickyFrame) {
        static int s_skipLog = 0;
        if (++s_skipLog <= 30 || s_skipLog % 60 == 0) {
            spdlog::info("[FLICKER-DIAG] compositeFrame tick={}: SKIPPING clip '{}' "
                         "(frame={} pixels={} gpuReady={})",
                         tick, clip->label(),
                         frame ? "non-null" : "NULL",
                         frame ? frame->pixels.size() : 0,
                         frame ? frame->gpuReady : false);
        }
        return false;
    }

    frame = stickyFrame;
    if (videoClip && temporalPrimaryFrameNum >= 0 &&
        frame->frameNumber != temporalPrimaryFrameNum) {
        sourceFallbackPending = true;
    }
    return true;
}

void CompositeService::recordStickyFrame(Clip* clip,
                                         const std::shared_ptr<CachedFrame>& frame)
{
    // Record this frame as the sticky fallback for future ticks, and under
    // the character/media key so future shots of the same character can use
    // it as a starting point.
    m_stickyLastClipFrame[clip->id()] = frame;
    const std::string charKey = stickyCharKey(clip);
    if (!charKey.empty())
        m_stickyLastCharFrame[charKey] = frame;
}

// ── Prune sticky last-frame caches ──────────────────────────────────
// Drop entries for clips/characters that are no longer in the active
// set (e.g. after a shot cut). This honours the documented contract
// ("pruned when clip IDs go out of the active set"), stops a character
// from lingering past a cut, and — crucially — releases the held
// CachedFrame (often GPU-resident, owning a pooled texture) instead of
// retaining it across shots. Safe here: this map is only touched by
// buildLayersForFrame (under m_compositeMutex) and reset() (after the
// prewarm thread is joined).
void CompositeService::pruneStickyFrameCaches()
{
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
}

void CompositeService::bindGpuResidentFrame(LayerInfo& layer,
                                            const std::shared_ptr<CachedFrame>& frame)
{
    // GPU-resident decoded frame — use GPU texture directly,
    // bypassing CPU→GPU upload in the compositor.
    layer.frame           = frame;
    layer.gpuTextureReady = true;
    VkDescriptorImageInfo gpuInfo{};
    gpuInfo.imageView   = reinterpret_cast<VkImageView>(frame->gpuImageView);
    gpuInfo.sampler     = reinterpret_cast<VkSampler>(frame->gpuSampler);
    gpuInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    layer.gpuDescriptor = gpuInfo;
    layer.frameWidth    = frame->width;
    layer.frameHeight   = frame->height;

    // ── Bridge CUDA frame into GpuTexCache for dirty-tracking ──
    // CUDA zero-copy frames bypass the cache-owned upload path,
    // so the GpuTexCache never sees them.  Register the frame's
    // GPU texture here (shared ownership with FrameCache) so
    // future composites can hit the dirty-tracking early-out
    // and skip the entire decode + FrameCache lookup.
    auto* texCache3 = m_engine ? m_engine->textureCache() : nullptr;
    if (!texCache3 || !m_engine->isGpuCompositeEnabled() ||
        frame->mediaId == 0 || !frame->gpuTextureOwner)
        return;

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

void CompositeService::applyClipCropAndRotation(LayerInfo& layer, Clip* clip,
                                                bool isPreRenderedSpine,
                                                uint32_t outW, uint32_t outH)
{
    if (auto* vc = dynamic_cast<VideoClip*>(clip)) {
        layer.srcRotation = vc->sourceRotation();
        layer.cropL = vc->cropLeft();
        layer.cropR = vc->cropRight();
        layer.cropT = vc->cropTop();
        layer.cropB = vc->cropBottom();
        // Source display rotation (portrait phone footage etc.).  Same
        // handle lookup the packed-alpha check uses.
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
        // Skip for pre-rendered cache frames — the character fills
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
#else
    (void)isPreRenderedSpine;
    (void)outW;
    (void)outH;
#endif
}

} // namespace rt
