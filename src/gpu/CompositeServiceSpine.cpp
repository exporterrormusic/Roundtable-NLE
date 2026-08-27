/*
 * CompositeServiceSpine.cpp - Spine character rendering (extracted from TimelineWorkspace).
 * No Qt dependency.
 */

#include "CompositeService.h"
#include "PathUtils.h"
#include "ClipRenderers.h"

#include "cache/FrameCache.h"
#include "timeline/SpineClip.h"
#include "timeline/Timeline.h"
#include "timeline/Track.h"

#ifdef ROUNDTABLE_HAS_SPINE
#include "spine/AnimationVideoCache.h"
#include "spine/ModelManager.h"
#include "spine/ShotPreset.h"
#include "stb_image.h"
#endif


#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>

namespace rt {

#ifdef ROUNDTABLE_HAS_SPINE

// Ã¢â€â‚¬Ã¢â€â‚¬ Shared spine data helpers Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬

std::string CompositeService::spineCharKey(const SpineClip& clip)
{
    // Build a unique key from character identity (name|outfit|stance).
    // All clips with the same key share atlas pixels and skeleton data.
    return clip.characterName() + "|" + clip.outfit() + "|"
           + std::to_string(static_cast<int>(clip.stance()));
}

const CompositeService::SpineSharedData*
CompositeService::getSpineSharedDataForOverlay(
    const std::string& charName, const std::string& outfit, int stance) const
{
    // Build a key matching spineCharKey format
    std::string key = charName + "|" + outfit + "|" + std::to_string(stance);
    auto it = m_spineSharedCache.find(key);
    if (it != m_spineSharedCache.end() && it->second && it->second->boundsCached)
        return it->second.get();
    return nullptr;
}

std::shared_ptr<CompositeService::SpineSharedData>
CompositeService::getOrCreateSharedSpineData(const SpineClip& clip,
                                               const std::string& assetsDir)
{
    const std::string key = spineCharKey(clip);
    auto it = m_spineSharedCache.find(key);
    if (it != m_spineSharedCache.end())
        return it->second;

    // Create new shared data Ã¢â‚¬â€ resolve paths and decode atlas PNGs once.
    auto shared = std::make_shared<SpineSharedData>();

    // Resolve skeleton/atlas file paths
    auto paths = SpineEngine::resolvePaths(
        assetsDir, clip.characterName(), clip.outfit(), clip.stance());
    if (!paths.valid) {
        spdlog::warn("Spine shared: failed to resolve paths for '{}'", clip.characterName());
        m_spineSharedCache.emplace(key, shared); // cache the failure
        return shared;
    }
    shared->skelPath  = paths.skelPath;
    shared->atlasPath = paths.atlasPath;

    // Read skeleton binary + atlas text into memory for fast per-clip
    // SpineEngine creation (avoids re-reading files from disk on every split/clone).
    {
        std::ifstream skelFile(paths.skelPath, std::ios::binary | std::ios::ate);
        if (skelFile.is_open()) {
            auto sz = skelFile.tellg();
            skelFile.seekg(0);
            shared->skelBytes.resize(static_cast<size_t>(sz));
            skelFile.read(reinterpret_cast<char*>(shared->skelBytes.data()), sz);
        }
        std::ifstream atlasFile(paths.atlasPath, std::ios::binary | std::ios::ate);
        if (atlasFile.is_open()) {
            auto sz = atlasFile.tellg();
            atlasFile.seekg(0);
            shared->atlasText.resize(static_cast<size_t>(sz));
            atlasFile.read(shared->atlasText.data(), sz);
            // Store directory for atlas texture path resolution
            shared->atlasDir = pathToUtf8(utf8ToPath(paths.atlasPath).parent_path());
        }
    }

    // Load a temporary engine just to get atlas info + bounds
    SpineEngine tempEngine;
    if (!tempEngine.loadSkeleton(paths.skelPath, paths.atlasPath)) {
        spdlog::warn("Spine shared: failed to load skeleton for '{}'", clip.characterName());
        m_spineSharedCache.emplace(key, shared);
        return shared;
    }

    // Decode atlas page PNGs into CPU memory (the expensive part Ã¢â‚¬â€ done once)
    const auto& pages = tempEngine.atlas().pages();
    const auto& atlasDir = tempEngine.atlas().directory();
    shared->pagePixels.resize(pages.size());
    shared->pageWidths.resize(pages.size(), 0);
    shared->pageHeights.resize(pages.size(), 0);
    shared->pagePMA.resize(pages.size(), false);

    for (size_t pi = 0; pi < pages.size(); ++pi) {
        std::string texPath = atlasDir + "/" + pages[pi].texturePath;
        shared->pagePMA[pi] = pages[pi].pma;
        int w = 0, h = 0, ch = 0;
        uint8_t* pixels = stbi_load(texPath.c_str(), &w, &h, &ch, 4);
        if (pixels) {
            // Premultiply alpha into RGB for any page that isn't already PMA.
            // The GPU shader (spine.frag) assumes PMA input; the CPU rasterizer
            // will un-premultiply later when it needs straight alpha.
            if (!pages[pi].pma) {
                const int total = w * h;
                for (int p = 0; p < total; ++p) {
                    uint8_t a = pixels[p * 4 + 3];
                    if (a < 255) {
                        pixels[p * 4 + 0] = static_cast<uint8_t>((pixels[p * 4 + 0] * a + 127) / 255);
                        pixels[p * 4 + 1] = static_cast<uint8_t>((pixels[p * 4 + 1] * a + 127) / 255);
                        pixels[p * 4 + 2] = static_cast<uint8_t>((pixels[p * 4 + 2] * a + 127) / 255);
                    }
                }
                shared->pagePMA[pi] = true;
            }

            shared->pagePixels[pi].assign(pixels, pixels + w * h * 4);
            shared->pageWidths[pi] = w;
            shared->pageHeights[pi] = h;
            stbi_image_free(pixels);
        }
    }
    // All pages are now PMA; the CPU path will un-premultiply on first use.
    shared->pagePixelsUnpremultiplied = false;

    // Pre-cache bounds from setup pose
    tempEngine.getBounds(
        shared->stableBoundsX, shared->stableBoundsY,
        shared->stableBoundsW, shared->stableBoundsH);
    shared->boundsCached = true;

    // ── Per-animation framing boxes ─────────────────────────────────────
    // Sample each animation across its duration and store, per animation, the
    // union of the setup pose and that animation's own bounding-box envelope.
    // The live renderer frames each clip to its CURRENT animation's box, so a
    // taller "action" pose grows only its own box (framed to fit, never
    // clipped) while normal animations keep their original setup-pose size —
    // exactly one box per animation, not a single shared box that would shrink
    // everything to the largest pose.  Modest sampling (≈15 fps, capped) keeps
    // the one-time load cost bounded; bounding boxes vary smoothly so we don't
    // need frame-accurate sampling to find the extent.
    {
        const float setMinX = shared->stableBoundsX;
        const float setMinY = shared->stableBoundsY;
        const float setMaxX = shared->stableBoundsX + shared->stableBoundsW;
        const float setMaxY = shared->stableBoundsY + shared->stableBoundsH;
        const bool  setValid = shared->stableBoundsW > 1.0f && shared->stableBoundsH > 1.0f;

        const auto animInfos = tempEngine.animation().listAnimations();
        for (const auto& ai : animInfos) {
            if (ai.name.empty()) continue;
            tempEngine.animation().setBodyAnimation(ai.name, false);
            const float dur = ai.duration;
            int samples = (dur > 0.0f)
                ? std::clamp(static_cast<int>(dur * 15.0f), 8, 60)
                : 1;
            // Seed with the setup pose so a normal animation that stays within
            // the setup silhouette keeps the original size.
            float minX = setValid ?  setMinX :  1e9f;
            float minY = setValid ?  setMinY :  1e9f;
            float maxX = setValid ?  setMaxX : -1e9f;
            float maxY = setValid ?  setMaxY : -1e9f;
            for (int s = 0; s <= samples; ++s) {
                const float t = (dur > 0.0f && samples > 0)
                    ? (static_cast<float>(s) / static_cast<float>(samples)) * dur
                    : 0.0f;
                tempEngine.evaluateAtTime(t, 0.0f);
                float bx, by, bw, bh;
                tempEngine.getBounds(bx, by, bw, bh);
                if (bw <= 0.0f || bh <= 0.0f) continue;
                minX = std::min(minX, bx);
                minY = std::min(minY, by);
                maxX = std::max(maxX, bx + bw);
                maxY = std::max(maxY, by + bh);
            }
            if (maxX > minX && maxY > minY) {
                shared->animBounds[ai.name] =
                    SpineSharedData::AnimBounds{minX, minY, maxX - minX, maxY - minY};
            }
        }
    }

    spdlog::info("Spine shared: loaded '{}' ({} atlas pages, setup {:.0f}x{:.0f}, "
                 "{} per-animation boxes)",
                 key, pages.size(), shared->stableBoundsW, shared->stableBoundsH,
                 shared->animBounds.size());

    m_spineSharedCache.emplace(key, shared);
    return shared;
}

CompositeService::SpineCPUState*
CompositeService::getOrCreateSpineState(SpineClip* clip)
{
    if (!clip) return nullptr;

    const uint64_t cid = clip->id();
    auto it = m_spineCache.find(cid);
    if (it != m_spineCache.end())
        return it->second.get();

    std::string assetsDir = "assets";
    if (m_modelManager) assetsDir = m_modelManager->assetsDir();

    auto shared = getOrCreateSharedSpineData(*clip, assetsDir);

    auto state = std::make_unique<SpineCPUState>();
    state->shared = shared;

    // Each clip gets its own SpineEngine for independent animation state.
    // Use in-memory buffers from the shared cache when available to avoid
    // re-reading files from disk (eliminates split/clone freeze).
    if (!shared->skelPath.empty()) {
        if (!shared->skelBytes.empty() && !shared->atlasText.empty()) {
            // Fast path: load from cached buffers (no disk I/O)
            state->engine.loadFromClipBuffered(*clip, shared->skelBytes,
                                                shared->atlasText, shared->atlasDir,
                                                shared->skelPath, shared->atlasPath);
        } else {
            // Fallback: load from disk (first-time or cache miss)
            state->engine.loadFromClip(*clip, assetsDir);
        }
    }

    // Grave's default skeleton contains a multi-part blue eye effect that
    // Compose does not present but the live timeline renderer otherwise
    // exposes as a bright cyan patch. The visible cloud/spark regions are the
    // fx_se_* attachment family; add_eff3/add_eff4 also reuse add_eff1/add_eff2
    // atlas regions. Suppress the complete effect family, not just eff_eye.
    if (clip->characterName() == "Grave the Great (GraGre)" &&
        clip->outfit() == "default") {
        state->engine.setHiddenAttachmentNames(
            {"add_blue1", "add_blue2",
             "add_eff1", "add_eff2", "add_eff3", "add_eff4",
             "eff_eye",
             "fx_se_5", "fx_se_6", "fx_se_7", "fx_se_8", "fx_se_9",
             "fx_se_10", "fx_se_11", "fx_se_12", "fx_se_13", "fx_se_14"});
    }

    // Record the clip settings the engine was just configured with so a
    // later out-of-band clip mutation (undo/redo, shot switch) can be
    // detected and reconciled without a full skeleton reload.
    state->appliedCharKey = spineCharKey(*clip);
    state->appliedAnim    = clip->animationName();
    state->appliedLooping = clip->isLooping();
    state->appliedTalking = clip->isTalking();
    state->appliedSpeed   = clip->animationSpeed();
    state->appliedValid   = true;

    auto* ptr = state.get();
    m_spineCache.emplace(cid, std::move(state));
    return ptr;
}

void CompositeService::resyncSpineClip(SpineClip* clip)
{
    if (!clip) return;
    auto it = m_spineCache.find(clip->id());
    if (it == m_spineCache.end() || !it->second) return;

    auto& st = *it->second;
    if (!st.engine.isLoaded()) return;

    // Character/outfit/stance changed → the loaded skeleton+atlas itself is
    // wrong (not just the animation track). Evict and rebuild the engine
    // from the now-current identity, exactly like an interactive edit.
    if (st.appliedValid && st.appliedCharKey != spineCharKey(*clip)) {
        evictSpineState(clip->id());        // invalidates `it`
        getOrCreateSpineState(clip);        // reloads + records fresh signature
        return;
    }

    // Only touch the engine when a clip-driven setting actually drifted —
    // this is the hot path after every undo/redo.
    const bool needsApply =
        !st.appliedValid ||
        st.appliedAnim    != clip->animationName() ||
        st.appliedLooping != clip->isLooping() ||
        st.appliedTalking != clip->isTalking() ||
        st.appliedSpeed   != clip->animationSpeed();
    if (!needsApply) return;

    // Replace the track graph atomically. A normal Spine setAnimation() keeps
    // the outgoing entry as mixingFrom; absolute-time timeline evaluation
    // does not advance that mix clock, so old bones/attachments can remain
    // partially applied after undo (seen as Kilo's eye layers separating).
    st.engine.animation().replacePlaybackState(
        clip->animationName(), clip->isLooping(), clip->isTalking());
    st.engine.animation().setSpeed(clip->animationSpeed());

    st.appliedAnim    = clip->animationName();
    st.appliedLooping = clip->isLooping();
    st.appliedTalking = clip->isTalking();
    st.appliedSpeed   = clip->animationSpeed();
    st.appliedValid   = true;

    // Drop the per-clip CPU frame cache so the next paint re-rasterizes
    // with the corrected pose (GPU path re-evaluates every composite).
    st.cachedFrame.reset();
    st.cachedTick = -1;
}

// Ã¢â€â‚¬Ã¢â€â‚¬ Non-blocking spine state accessor Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬
// Returns the cached SpineCPUState if available, or nullptr if it needs
// to be loaded. When nullptr is returned, background loading is scheduled
// so the next refresh will find the cache warm.
CompositeService::SpineCPUState*
CompositeService::tryGetSpineState(SpineClip* clip)
{
    if (!clip) return nullptr;

    const uint64_t cid = clip->id();
    auto it = m_spineCache.find(cid);
    if (it != m_spineCache.end())
        return it->second.get();

    // Check if shared data is already available for this character
    const std::string key = spineCharKey(*clip);
    auto sit = m_spineSharedCache.find(key);
    if (sit != m_spineSharedCache.end() && sit->second
        && !sit->second->skelBytes.empty()) {
        // Shared data is cached with valid buffers Ã¢â‚¬â€ create per-clip engine
        // synchronously (fast path: only creates Skeleton + AnimationState
        // from in-memory buffers, ~3-7ms, no disk I/O).
        try {
            return getOrCreateSpineState(clip);
        } catch (const std::exception& ex) {
            spdlog::error("tryGetSpineState: exception creating engine: {}", ex.what());
            return nullptr;
        } catch (...) {
            spdlog::error("tryGetSpineState: unknown exception creating engine");
            return nullptr;
        }
    }

    // Shared data is NOT cached Ã¢â‚¬â€ need heavy loading.
    // Schedule it in the background instead of blocking.
    std::string assetsDir = "assets";
    if (m_modelManager) assetsDir = m_modelManager->assetsDir();
    if (m_spineLoadScheduler)
        m_spineLoadScheduler(clip->characterName(), clip->outfit(),
                              static_cast<int>(clip->stance()), assetsDir);
    return nullptr;
}

// their per-clip SpineEngine using already-cached shared data (fast path).
void CompositeService::warmNewSpineClips()
{
    if (!m_timeline) return;

    std::string assetsDir = "assets";
    if (m_modelManager) assetsDir = m_modelManager->assetsDir();

    for (size_t ti = 0; ti < m_timeline->trackCount(); ++ti) {
        auto* track = m_timeline->track(ti);
        if (!track || track->type() != TrackType::Video) continue;

        for (size_t ci = 0; ci < track->clipCount(); ++ci) {
            auto* spineClip = dynamic_cast<SpineClip*>(track->clip(ci));
            if (!spineClip) continue;

            // Already have a per-clip engine for this clip — nothing to do.
            if (m_spineCache.count(spineClip->id())) continue;

            // Ensure the shared skeleton/atlas data is loaded, but NEVER build
            // the per-clip SpineEngine here.
            //
            // Building it eagerly for every spine clip in the whole timeline is
            // what froze the app for ~6s on the first paste/drag after opening a
            // project (measured via [EDIT-PERF] warmSpine): each clip's
            // Skeleton+AnimationState construction is only ~3-7ms, but the loop
            // pays it for the entire timeline at once, then caches the result in
            // m_spineCache (hence "first time only"). The per-clip engine is
            // instead built lazily by tryGetSpineState() during compositeFrame —
            // only for clips at the current playhead — so the cost is spread out
            // and paid only for characters actually on screen.
            const std::string key = spineCharKey(*spineClip);
            if (!m_spineSharedCache.count(key) && m_spineLoadScheduler)
                m_spineLoadScheduler(spineClip->characterName(), spineClip->outfit(),
                                     static_cast<int>(spineClip->stance()), assetsDir);
        }
    }
}

// Ã¢â€â‚¬Ã¢â€â‚¬ Pre-warm spine cache at project-open time Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬
// Scans all tracks for SpineClips and eagerly loads their skeleton +
// atlas PNG data so the first compositeFrame finds the cache warm and
// doesn't block on disk I/O (~100-200ms per clip eliminated).
//
// OPTIMIZATION: Skip skeleton loading for clips whose animation is
// already pre-rendered to video Ã¢â‚¬â€ the compositor will use the cached
// video and never touch the live Spine engine. This avoids loading
// hundreds of skeletons + atlas PNGs (50+ seconds, ~2 GB RAM) when all
// animations are fully cached.
void CompositeService::preloadSpineAssets()
{
    if (!m_timeline) return;

    std::string assetsDir = "assets";
    if (m_modelManager) assetsDir = m_modelManager->assetsDir();

    int preloaded = 0;
    for (size_t ti = 0; ti < m_timeline->trackCount(); ++ti) {
        Track* track = m_timeline->track(ti);
        if (!track || track->type() != TrackType::Video) continue;

        for (size_t ci = 0; ci < track->clipCount(); ++ci) {
            auto* spineClip = dynamic_cast<SpineClip*>(track->clip(ci));
            if (!spineClip) continue;

            const uint64_t cid = spineClip->id();
            if (m_spineCache.count(cid)) continue;  // Already cached

            // SpineClips always load skeleton + atlas for GPU live rendering.
            // Pre-rendered video existence is irrelevant — the GPU path needs
            // atlas textures regardless.

            // Use shared helpers Ã¢â‚¬â€ atlas data is loaded once per character
            // Schedule the heavy skeleton+atlas decode on a background thread
            // (scheduleSpineSharedLoad) instead of loading it synchronously on
            // the UI thread, which cost ~10s when opening projects with many
            // character clips.  The first composite uses tryGetSpineState(),
            // which paints once the background load integrates the shared data
            // and posts a requestRefresh back to the UI thread.
            if (m_spineLoadScheduler) {
                m_spineLoadScheduler(spineClip->characterName(), spineClip->outfit(),
                                     static_cast<int>(spineClip->stance()), assetsDir);
                ++preloaded;
            }
        }
    }

    if (preloaded > 0)
        spdlog::info("Spine preload: {} clips pre-warmed ({} unique characters)",
                     preloaded, m_spineSharedCache.size());

    // Queue background pre-rendering for all character animations
    // NOTE: Auto-conversion is DISABLED here because NVENC encoder
    // contention with the GPU decode/compositor pipeline caused the
    // UI thread to hang ("Not Responding") when loading large projects.
    // Users should trigger conversions explicitly from the CONVERT tab.
    (void)m_animVideoCache;
}

std::shared_ptr<CachedFrame> CompositeService::renderSpineClip(
    SpineClip* clip, int64_t tick, uint32_t outW, uint32_t outH)
{
    if (!clip) return nullptr;

    // Get or create per-clip state (shares atlas data with other clips
    // of the same character via SpineSharedData).
    // Use tryGetSpineState to avoid blocking on heavy shared data loading.
    auto* statePtr = tryGetSpineState(clip);
    if (!statePtr || !statePtr->engine.isLoaded() || !statePtr->shared) return nullptr;

    auto& state = *statePtr;
    auto& shared = *state.shared;

    // Ensure atlas pixels have been un-premultiplied for CPU rasterizing.
    // The shared cache stores raw PMA pixels (suitable for GPU upload);
    // we un-premultiply in-place on first CPU use.
    if (!shared.pagePixelsUnpremultiplied && !shared.pagePixels.empty()) {
        for (size_t pi = 0; pi < shared.pagePixels.size(); ++pi) {
            if (shared.pagePixels[pi].empty() || !shared.pagePMA[pi]) continue;
            uint8_t* px = shared.pagePixels[pi].data();
            const int total = shared.pageWidths[pi] * shared.pageHeights[pi];
            for (int p = 0; p < total; ++p) {
                uint8_t a = px[p * 4 + 3];
                if (a > 0 && a < 255) {
                    px[p * 4 + 0] = static_cast<uint8_t>(std::min(255, px[p * 4 + 0] * 255 / a));
                    px[p * 4 + 1] = static_cast<uint8_t>(std::min(255, px[p * 4 + 1] * 255 / a));
                    px[p * 4 + 2] = static_cast<uint8_t>(std::min(255, px[p * 4 + 2] * 255 / a));
                } else if (a == 0) {
                    px[p * 4 + 0] = 0; px[p * 4 + 1] = 0; px[p * 4 + 2] = 0;
                }
            }
        }
        shared.pagePixelsUnpremultiplied = true;
    }

    // Return cached frame if tick hasn't changed (scrub-back, paused, etc.)
    if (state.cachedTick == tick && state.cachedFrame)
        return state.cachedFrame;

    // Evaluate spine animation at current time
    const int64_t localTick = tick - clip->timelineIn();
    const int64_t animTick  = clip->useGlobalTime() ? tick : localTick;
    const float timeSeconds = static_cast<float>(ticksToSeconds(animTick));
    state.engine.evaluateAtTime(timeSeconds * clip->animationSpeed(), timeSeconds);

    // Extract meshes
    SpineRenderData renderData = state.engine.extractMeshes();
    if (renderData.batches.empty()) return nullptr;

    // Create output frame
    auto frame = std::make_shared<CachedFrame>();
    frame->width  = outW;
    frame->height = outH;
    frame->stride = outW * 4;
    frame->pixels.resize(static_cast<size_t>(outW) * outH * 4, 0);

    // Use STABLE animation bounds for scale (prevents zooming in/out
    // as the live bounding box changes per frame during animation).
    // Setup-pose stableBounds provide the anchor center to prevent swaying.
    float liveBx{0}, liveBy{0}, liveBw{0}, liveBh{0};
    state.engine.getBounds(liveBx, liveBy, liveBw, liveBh);
    // Frame to THIS animation's box (setup pose ∪ the animation's own
    // envelope) so a taller-than-idle pose isn't clipped, while normal
    // animations keep their original setup-pose size.  Fall back to the
    // setup-pose bounds, then to live bounds, when no per-animation box
    // exists.  Using a constant (per-animation) box also prevents the
    // zoom/sway that live per-frame bounds would cause.
    const SpineSharedData::AnimBounds* ab = nullptr;
    {
        auto abIt = shared.animBounds.find(clip->animationName());
        if (abIt != shared.animBounds.end()) ab = &abIt->second;
    }
    const float boundsW = ab ? ab->w : shared.stableBoundsW;
    const float boundsH = ab ? ab->h : shared.stableBoundsH;
    const float boundsX = ab ? ab->x : shared.stableBoundsX;
    const float boundsY = ab ? ab->y : shared.stableBoundsY;
    const float bw = (boundsW > 1.0f) ? boundsW
                    : ((liveBw > 1.0f) ? liveBw : boundsW);
    const float bh = (boundsH > 1.0f) ? boundsH
                    : ((liveBh > 1.0f) ? liveBh : boundsH);
    // One-time diagnostic
    {
        static bool s_spineCpuBoundsLogged = false;
        if (!s_spineCpuBoundsLogged) {
            s_spineCpuBoundsLogged = true;
            spdlog::info("=== SPINE SIZING DIAGNOSTIC (CPU) ===");
            spdlog::info("  stableBounds: w={:.1f} h={:.1f}",
                         shared.stableBoundsW, shared.stableBoundsH);
            spdlog::info("  liveBounds:   w={:.1f} h={:.1f}",
                         liveBw, liveBh);
            spdlog::info("=====================================");
        }
    }

    if (bw < 1.0f || bh < 1.0f) return frame; // degenerate bounds

    // Height-based fit — fill as much of the output as possible.
    // The 0.85× compose-fit is applied in the compositor transform (scX/scY)
    // so the render has maximum native resolution for close-up shots.
    const float spineScale = (static_cast<float>(outH) / bh);

    const float offsetX = outW * 0.5f;
    const float offsetY = outH * 0.5f;
    // Center on THIS animation's box (constant per animation) — live bounds
    // shift per frame which causes visible swaying during playback, and the
    // setup-pose center would push a taller animation off the top.
    const float spineCX = boundsX + boundsW * 0.5f;
    const float spineCY = boundsY + boundsH * 0.5f;

    auto spineToPixel = [&](float sx, float sy, float& px, float& py) {
        px = (sx - spineCX) * spineScale + offsetX;
        py = -(sy - spineCY) * spineScale + offsetY;
    };

    // Software triangle rasterizer (scanline-based)
    for (const auto& batch : renderData.batches) {
        if (batch.texturePageIndex < 0 ||
            batch.texturePageIndex >= static_cast<int>(shared.pagePixels.size()))
            continue;

        const auto& texPixels = shared.pagePixels[batch.texturePageIndex];
        if (texPixels.empty()) continue;

        const int texW = shared.pageWidths[batch.texturePageIndex];
        const int texH = shared.pageHeights[batch.texturePageIndex];
        if (texW <= 0 || texH <= 0) continue;

        for (size_t ti = 0; ti + 2 < batch.indices.size(); ti += 3) {
            const auto& v0 = batch.vertices[batch.indices[ti]];
            const auto& v1 = batch.vertices[batch.indices[ti + 1]];
            const auto& v2 = batch.vertices[batch.indices[ti + 2]];

            float px0, py0, px1, py1, px2, py2;
            spineToPixel(v0.x, v0.y, px0, py0);
            spineToPixel(v1.x, v1.y, px1, py1);
            spineToPixel(v2.x, v2.y, px2, py2);

            int minX = std::max(0, static_cast<int>(std::floor(std::min({px0, px1, px2}))));
            int maxX = std::min(static_cast<int>(outW) - 1,
                                static_cast<int>(std::ceil(std::max({px0, px1, px2}))));
            int minY = std::max(0, static_cast<int>(std::floor(std::min({py0, py1, py2}))));
            int maxY = std::min(static_cast<int>(outH) - 1,
                                static_cast<int>(std::ceil(std::max({py0, py1, py2}))));

            if (minX > maxX || minY > maxY) continue;

            const float denom = (py1 - py2) * (px0 - px2) + (px2 - px1) * (py0 - py2);
            if (std::abs(denom) < 1e-8f) continue;
            const float invDenom = 1.0f / denom;

            for (int y = minY; y <= maxY; ++y) {
                for (int x = minX; x <= maxX; ++x) {
                    const float fx = static_cast<float>(x) + 0.5f;
                    const float fy = static_cast<float>(y) + 0.5f;

                    const float w0 = ((py1 - py2) * (fx - px2) + (px2 - px1) * (fy - py2)) * invDenom;
                    const float w1 = ((py2 - py0) * (fx - px2) + (px0 - px2) * (fy - py2)) * invDenom;
                    const float w2 = 1.0f - w0 - w1;

                    if (w0 < 0.0f || w1 < 0.0f || w2 < 0.0f) continue;

                    float u = w0 * v0.u + w1 * v1.u + w2 * v2.u;
                    float v = w0 * v0.v + w1 * v1.v + w2 * v2.v;

                    float cr = w0 * v0.r + w1 * v1.r + w2 * v2.r;
                    float cg = w0 * v0.g + w1 * v1.g + w2 * v2.g;
                    float cb = w0 * v0.b + w1 * v1.b + w2 * v2.b;
                    float ca = w0 * v0.a + w1 * v1.a + w2 * v2.a;

                    int tx = std::clamp(static_cast<int>(u * texW), 0, texW - 1);
                    int ty = std::clamp(static_cast<int>(v * texH), 0, texH - 1);
                    const uint8_t* texel = texPixels.data() + (ty * texW + tx) * 4;

                    float tr = texel[0] / 255.0f;
                    float tg = texel[1] / 255.0f;
                    float tb = texel[2] / 255.0f;
                    float ta = texel[3] / 255.0f;

                    float sr = tr * cr;
                    float sg = tg * cg;
                    float sb = tb * cb;
                    float sa = ta * ca;

                    if (sa < 0.001f) continue;

                    uint8_t* dp = frame->pixels.data() + (y * outW + x) * 4;
                    float da = dp[3] / 255.0f;
                    float outA = sa + da * (1.0f - sa);

                    if (outA > 0.001f) {
                        dp[0] = static_cast<uint8_t>(std::clamp((sb * sa + (dp[0] / 255.0f) * da * (1.0f - sa)) / outA * 255.0f, 0.0f, 255.0f));
                        dp[1] = static_cast<uint8_t>(std::clamp((sg * sa + (dp[1] / 255.0f) * da * (1.0f - sa)) / outA * 255.0f, 0.0f, 255.0f));
                        dp[2] = static_cast<uint8_t>(std::clamp((sr * sa + (dp[2] / 255.0f) * da * (1.0f - sa)) / outA * 255.0f, 0.0f, 255.0f));
                        dp[3] = static_cast<uint8_t>(outA * 255.0f);
                    }
                }
            }
        }
    }

    state.cachedFrame = frame;
    state.cachedTick  = tick;

    return frame;
}


#endif // ROUNDTABLE_HAS_SPINE

} // namespace rt
