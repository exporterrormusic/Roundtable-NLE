/*
 * CompositeServiceLayerBuildSpine.cpp - live SpineClip layer builder.
 * Extracted verbatim from buildLayersForFrame() (modularization). The two
 * disabled if(false) blocks (pre-rendered video cache + video fallback) were
 * removed first, leaving only the live GPU-first / CPU-fallback Spine path.
 * Outputs are returned via a result struct using reference aliases.
 * [SPINE-SLOW] sub-timings were added 2026-09-26.
 */

#include "CompositeService.h"
#include "CompositeServiceLayerBuild.h"

#include "cache/FrameCache.h"
#include "playback/MediaPool.h"
#include "Constants.h"
#include "timeline/SpineClip.h"
#include "timeline/Track.h"

#include "CompositeEngine.h"
#include "GpuContext.h"
#include "SpineRenderer.h"

#ifdef ROUNDTABLE_HAS_SPINE
#include "spine/ModelManager.h"
#endif

#include <spdlog/spdlog.h>
#include <algorithm>
#include <chrono>
#include <cmath>

#ifdef ROUNDTABLE_HAS_SPINE
namespace rt {

CompositeService::SpineLayerResult CompositeService::buildSpineClipLayer(
    SpineClip* spineClip, int64_t tick, int64_t localTick,
    uint32_t outW, uint32_t outH, uint32_t simultaneousAssetInstance,
    bool& gpuSpineUsedThisFrame, float sx,
    RenderExecutionOutcome* outcome)
{
    SpineLayerResult R;
    auto&     frame              = R.frame;
    bool&     gpuSpineZeroCopy   = R.gpuSpineZeroCopy;
    auto&     gpuSpineDescriptor = R.gpuSpineDescriptor;
    auto&     gpuSpineOwner      = R.gpuSpineOwner;
    uint32_t& gpuSpineW          = R.gpuSpineW;
    uint32_t& gpuSpineH          = R.gpuSpineH;
    bool&     cpuSpineRendered   = R.cpuSpineRendered;

    // [SPINE-SLOW] sub-timings: separates CPU work (state/atlas/evaluate/
    // extract/record) from the host fence wait inside beginFrame() and the
    // submit in endFrame(), which [LAYER-SLOW] lumps into one number.
    using SpineClock = std::chrono::steady_clock;
    const auto spineT0 = SpineClock::now();
    auto spineLap = [](SpineClock::time_point& t) {
        const auto now = SpineClock::now();
        const double lapMs =
            std::chrono::duration<double, std::milli>(now - t).count();
        t = now;
        return lapMs;
    };
    double rendererMs = 0, stateMs = 0, atlasMs = 0, evalMs = 0, extractMs = 0;
    double beginMs = 0, recordMs = 0, endMs = 0;

    try {
                // The UI may have reconciled a newer edit before this queued
                // snapshot acquired the render lock. Evaluate this snapshot's
                // character settings, never whichever edit ran most recently.
                resyncSpineClip(spineClip);
                // Try GPU Spine rendering first (faster + better quality)
                bool gpuSpineDone = false;
                if (GpuContext::get().isInitialized()) {
                    const std::string charKey = spineCharKey(*spineClip);
                    // The FBO grows taller than the output when this
                    // animation reaches above or below the setup pose; the
                    // layer build scales it back so the character keeps her
                    // setup-pose size (spineRenderHeight).
                    uint32_t renderH = outH;
                    {
                        auto pre = m_spineCache.find(spineClip->id());
                        if (pre == m_spineCache.end()) {
                            tryGetSpineState(spineClip);
                            pre = m_spineCache.find(spineClip->id());
                        }
                        if (pre != m_spineCache.end() && pre->second &&
                            pre->second->shared && pre->second->shared->boundsCached) {
                            const auto& sd = *pre->second->shared;
                            renderH = spineRenderHeight(
                                outH, sd.framingBox(""),
                                sd.framingBox(spineClip->animationName()));
                        }
                    }
                    // Reuse one persistent renderer across edits of the same
                    // immutable character asset. Only simultaneous duplicate
                    // instances require separate mutable framebuffers.
                    const std::string rendererContentKey =
                        spineRendererContentKey(charKey,
                                                simultaneousAssetInstance);
                    const SpineRendererCacheKey rendererKey{
                        outW, renderH, rendererContentKey};
                    const auto rendererT0 = SpineClock::now();
                    auto sr = renderSpineRenderer(
                        outW, renderH, rendererContentKey);
                    rendererMs = std::chrono::duration<double, std::milli>(
                        SpineClock::now() - rendererT0).count();
                    if (sr && sr->isInitialized()) {
                        auto spineLapT = SpineClock::now();
                        // Each active character clip owns a persistent renderer,
                        // FBO, and atlas set. Multiple on-screen characters (or
                        // duplicate instances of one character) can therefore
                        // remain GPU-resident together instead of repeatedly
                        // reading one shared FBO back to the CPU.
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
                            stateMs = spineLap(spineLapT);

                            // Upload once per exact renderer generation. reset()
                            // clears the weak preparation map, forcing a refresh
                            // after asset invalidation without sacrificing normal
                            // frame-to-frame atlas residency.
                            auto preparedIt = m_gpuSpinePreparedRenderers.find(
                                rendererKey);
                            const bool atlasPrepared =
                                preparedIt != m_gpuSpinePreparedRenderers.end() &&
                                preparedIt->second.lock() == sr;
                            if (!atlasPrepared) {
                                sr->releaseAllTextures();
                                int loaded = 0;
                                const size_t atlasPageCount =
                                    state.engine.atlas().pages().size();
                                const size_t expectedPages = atlasPageCount > 0
                                    ? atlasPageCount : shared.pagePixels.size();
                                // Shared GPU pages (pre-uploaded on the spine
                                // loader thread, or by another renderer size)
                                // need only a descriptor set here.
                                const auto gpuPages = spineGpuPages(charKey);
                                const bool gpuPagesComplete =
                                    expectedPages > 0 &&
                                    gpuPages.size() == expectedPages &&
                                    std::all_of(gpuPages.begin(), gpuPages.end(),
                                                [](const auto& p) { return p != nullptr; });
                                if (gpuPagesComplete) {
                                    for (size_t pi = 0; pi < expectedPages; ++pi) {
                                        if (sr->adoptAtlasTexture(
                                                static_cast<int>(pi), gpuPages[pi],
                                                "shared"))
                                            ++loaded;
                                    }
                                } else if (!shared.pagePixels.empty()) {
                                    for (size_t pi = 0; pi < expectedPages; ++pi) {
                                        if (pi >= shared.pagePixels.size() ||
                                            pi >= shared.pageWidths.size() ||
                                            pi >= shared.pageHeights.size() ||
                                            shared.pagePixels[pi].empty() ||
                                            shared.pageWidths[pi] <= 0 ||
                                            shared.pageHeights[pi] <= 0) {
                                            continue;
                                        }
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
                                if (expectedPages > 0 &&
                                    loaded == static_cast<int>(expectedPages)) {
                                    m_gpuSpinePreparedRenderers[rendererKey] = sr;
                                    if (!gpuPagesComplete) {
                                        // First upload of this character:
                                        // share it with other renderer sizes.
                                        std::vector<std::shared_ptr<Texture>> pages;
                                        pages.reserve(expectedPages);
                                        for (size_t pi = 0; pi < expectedPages; ++pi)
                                            pages.push_back(sr->atlasTexture(static_cast<int>(pi)));
                                        storeSpineGpuPages(charKey, std::move(pages));
                                    }
                                    spdlog::info("[SPINE-GPU] {} complete atlas for '{}': {} pages",
                                                 gpuPagesComplete ? "bound shared" : "uploaded",
                                                 spineClip->characterName(), loaded);
                                } else {
                                    // Never render a partly uploaded character. A
                                    // missing page would otherwise sample a descriptor
                                    // left over from the preceding shot.
                                    sr->releaseAllTextures();
                                    m_gpuSpinePreparedRenderers.erase(rendererKey);
                                    spdlog::warn("[SPINE-GPU] atlas upload incomplete for '{}': {}/{} pages",
                                                 spineClip->characterName(), loaded,
                                                 expectedPages);
                                }
                            }
                            atlasMs = spineLap(spineLapT);

                            if (auto readyIt =
                                    m_gpuSpinePreparedRenderers.find(rendererKey);
                                readyIt != m_gpuSpinePreparedRenderers.end() &&
                                readyIt->second.lock() == sr) {
                                // Evaluate animation
                                const int64_t animTick = spineClip->useGlobalTime()
                                                             ? tick : localTick;
                                const float timeSeconds = static_cast<float>(ticksToSeconds(animTick));
                                state.engine.evaluateAtTime(
                                    timeSeconds * spineClip->animationSpeed(), timeSeconds);
                                evalMs = spineLap(spineLapT);

                                SpineRenderData renderData = state.engine.extractMeshes();
                                extractMs = spineLap(spineLapT);

                                // ── SPINE-BLEND-DIAG (opt-in) ──────────────
                                // Diagnostic for one-frame Spine flicker: logs a
                                // per-clip batch signature (blend letters +
                                // non-Normal slot names) plus the min vertex
                                // brightness/alpha and the slot they land in,
                                // edge-triggered so it only fires when something
                                // changes between frames.  Pinpoints the loop
                                // time + slot behind a flicker.  Off unless the
                                // ROUNDTABLE_SPINE_DIAG env var is set, since
                                // it's a one-off investigation tool.
                                static const bool s_spineDiagEnabled =
                                    std::getenv("ROUNDTABLE_SPINE_DIAG") != nullptr;
                                if (s_spineDiagEnabled) {
                                    auto blendLetter = [](SpineBlendMode m) -> char {
                                        switch (m) {
                                        case SpineBlendMode::Additive: return 'A';
                                        case SpineBlendMode::Multiply: return 'M';
                                        case SpineBlendMode::Screen:   return 'S';
                                        default:                       return 'N';
                                        }
                                    };
                                    std::string sig;
                                    sig.reserve(renderData.batches.size());
                                    std::string nonNormal;
                                    int nonNormalCount = 0;
                                    // Per-frame vertex color/alpha range.  A one-frame
                                    // darkening on a static (single-batch, Normal) skeleton
                                    // is the signature of a slot color/alpha (tint) key:
                                    // track the darkest tint and the slot batch it lands in.
                                    float minBright = 2.0f;   // min over verts of (r+g+b)/3
                                    float minA      = 2.0f;   // min vertex alpha
                                    std::string darkSlot;     // batch whose vert hit minBright
                                    for (size_t bi = 0; bi < renderData.batches.size(); ++bi) {
                                        const auto& b = renderData.batches[bi];
                                        sig.push_back(blendLetter(b.blendMode));
                                        if (b.blendMode != SpineBlendMode::Normal) {
                                            ++nonNormalCount;
                                            if (!nonNormal.empty()) nonNormal += ", ";
                                            nonNormal += "#" + std::to_string(bi) + ":'"
                                                       + b.debugFirstSlot + "'="
                                                       + blendLetter(b.blendMode);
                                        }
                                        for (const auto& v : b.vertices) {
                                            const float bright = (v.r + v.g + v.b) * (1.0f / 3.0f);
                                            if (bright < minBright) { minBright = bright; darkSlot = b.debugFirstSlot; }
                                            if (v.a < minA) minA = v.a;
                                        }
                                    }
                                    // Edge-trigger on a coarse (sig + brightness/alpha bucket)
                                    // key so a one-frame tint dip prints exactly twice per loop
                                    // (enter + exit) with its animTime, instead of every frame.
                                    const int brightBucket = static_cast<int>(minBright * 20.0f);
                                    const int alphaBucket  = static_cast<int>(minA * 20.0f);
                                    const std::string key = sig + "|" + std::to_string(brightBucket)
                                                          + "|" + std::to_string(alphaBucket);
                                    static std::unordered_map<uint64_t, std::string> s_spineBlendSig;
                                    auto sigIt = s_spineBlendSig.find(spineClip->id());
                                    if (sigIt == s_spineBlendSig.end() || sigIt->second != key) {
                                        spdlog::warn(
                                            "[SPINE-BLEND-DIAG] char='{}' outfit='{}' anim='{}' "
                                            "animTime={:.3f}s tick={} batches={} nonNormal={} [{}] "
                                            "minBright={:.3f} minA={:.3f} darkSlot='{}' sig={} (prev={})",
                                            spineClip->characterName(), spineClip->outfit(),
                                            spineClip->animationName(),
                                            timeSeconds * spineClip->animationSpeed(), tick,
                                            renderData.batches.size(), nonNormalCount,
                                            nonNormal.empty() ? "-" : nonNormal,
                                            minBright, minA, darkSlot, sig,
                                            sigIt == s_spineBlendSig.end() ? "<new>" : sigIt->second);
                                        s_spineBlendSig[spineClip->id()] = key;
                                    }
                                }

                                if (!renderData.batches.empty()) {
                                    // Size and centre by the setup pose, as COMPOSE
                                    // does, whatever the animation: switching idle to
                                    // action used to shrink the character to fit the
                                    // action's reach.  That reach is drawn into the
                                    // FBO's extra height instead (renderH), and nothing
                                    // zooms or sways the way it would with live
                                    // per-frame bounds.  Live bounds are only a
                                    // fallback for a degenerate box.
                                    float liveBx{0}, liveBy{0}, liveBw{0}, liveBh{0};
                                    state.engine.getBounds(liveBx, liveBy, liveBw, liveBh);
                                    const SpineBox box = shared.framingBox("");
                                    const float boundsW = box.w;
                                    const float boundsH = box.h;
                                    const float boundsX = box.x;
                                    const float boundsY = box.y;
                                    const float bw = (boundsW > 1.0f) ? boundsW
                                                    : ((liveBw > 1.0f) ? liveBw : boundsW);
                                    const float bh = (boundsH > 1.0f) ? boundsH
                                                    : ((liveBh > 1.0f) ? liveBh : boundsH);
                                    const float fW = static_cast<float>(outW);
                                    const float fH = static_cast<float>(outH);
                                    float fitZoom = 1.0f;
                                    float cx = boundsX + boundsW * 0.5f;
                                    float cy = boundsY + boundsH * 0.5f;
                                    if (bw > 1.0f && bh > 1.0f) {
                                        // Fill the FBO as much as possible for maximum
                                        // native resolution on close-up shots.  The 0.85×
                                        // compose-fit is applied in the compositor
                                        // transform (scX/scY), not baked into the render.
                                        fitZoom = std::min(fW / bw, fH / bh);
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
                                                         outW, renderH, fitZoom, cx, cy);
                                            spdlog::info("  sx={:.4f} finalSx={:.4f}",
                                                         sx, sx);
                                            spdlog::info("=====================================");
                                        }
                                    }

                                    // Same zoom and centre over the taller FBO:
                                    // the extra height is reach above and below.
                                    glm::mat4 proj = SpineRenderer::orthoProjection(
                                        fW, static_cast<float>(renderH), cx, cy, fitZoom);
                                    glm::mat4 model = SpineRenderer::modelMatrix(0.0f, 0.0f);
                                    glm::mat4 mvp = proj * model;

                                    spineLap(spineLapT);  // exclude bounds/diag
                                    const bool began = sr->beginFrame();
                                    beginMs = spineLap(spineLapT);
                                    const bool drew = began &&
                                        sr->renderSkeleton(renderData, mvp, 1.0f);
                                    recordMs = spineLap(spineLapT);
                                    // Finish a begun command buffer even if atlas
                                    // validation rejected its draw commands.
                                    const bool submitted = began && sr->endFrame();
                                    endMs = spineLap(spineLapT);
                                    // Spine production and compositor sampling
                                    // use the same scheduled graphics queue.
                                    // Submission order is the dependency; a
                                    // host fence wait here only drains the GPU
                                    // between every character. Export's final
                                    // composite readback remains synchronized.
                                    const bool renderComplete = drew && submitted;

                                    if (renderComplete) {
                                        gpuSpineDone = true;
                                        gpuSpineUsedThisFrame = true;
                                        gpuSpineZeroCopy = true;
                                        gpuSpineDescriptor = sr->outputDescriptorInfo();
                                        gpuSpineOwner = sr;
                                        gpuSpineW = outW;
                                        gpuSpineH = renderH;
                                    }
                                }
                            }
                        }
                    }
                }

                // CPU fallback if GPU Spine didn't produce a frame
                // WARNING: CPU Spine rendering is very slow. This is a TEMPORARY
                // LAST RESORT only — fix the GPU Spine path instead.
                if (!gpuSpineDone) {
                    frame = renderSpineClip(spineClip, tick, outW, outH);
                    if (frame) {
                        cpuSpineRendered = true;
                        spdlog::warn("[SPINE-RENDER] '{}' GPU unavailable — SLOW CPU raster: {}x{}",
                                     spineClip->characterName(),
                                     frame->width, frame->height);
                    } else {
                        // Distinguish the expected async-load window (the Spine
                        // skeleton hasn't finished loading yet — normal for the
                        // first composites after a project opens; the character
                        // appears as soon as its asset is ready) from a genuine
                        // render failure.  Only the latter is error-worthy; the
                        // former was spamming [error] at startup.
                        auto sit2 = m_spineCache.find(spineClip->id());
                        const bool stillLoading =
                            (sit2 == m_spineCache.end() || !sit2->second ||
                             !sit2->second->engine.isLoaded());
                        if (stillLoading) {
                            spdlog::debug("[SPINE-RENDER] '{}' skeleton still "
                                          "loading — layer deferred this frame",
                                          spineClip->characterName());
                        } else {
                            spdlog::error("[SPINE-RENDER] '{}' FAILED both GPU and "
                                          "CPU paths (skeleton loaded but render "
                                          "produced no frame)",
                                          spineClip->characterName());
                        }
                    }
                } else if (gpuSpineZeroCopy) {
                    spdlog::info("[SPINE-RENDER] '{}' GPU zero-copy: {}x{}",
                                 spineClip->characterName(), gpuSpineW, gpuSpineH);
                }

    }
    catch (const std::exception& ex) {
        spdlog::error("compositeFrame: SpineClip '{}' exception: {}",
                      spineClip->characterName(), ex.what());
    }
    catch (...) {
        spdlog::error("compositeFrame: SpineClip '{}' unknown exception",
                      spineClip->characterName());
    }
    if (const double totalMs = std::chrono::duration<double, std::milli>(
            SpineClock::now() - spineT0).count();
        totalMs > 50.0) {
        spdlog::warn("[SPINE-SLOW] '{}' total={:.1f}ms renderer={:.1f} state={:.1f} "
                     "atlas={:.1f} eval={:.1f} extract={:.1f} fenceWait={:.1f} "
                     "record={:.1f} submit={:.1f} gpu={} cpuFallback={}",
                     spineClip->characterName(), totalMs, rendererMs, stateMs, atlasMs,
                     evalMs, extractMs, beginMs, recordMs, endMs,
                     gpuSpineZeroCopy, cpuSpineRendered);
    }
    if (!gpuSpineZeroCopy && !frame) {
        const auto state = spineResourceState(*spineClip);
        std::string warning = spineResourceWarning(*spineClip);
        if (warning.empty()) {
            warning = "Spine resource unavailable: " +
                      spineClip->characterName() + " / " + spineClip->outfit();
        }
        if (outcome && state == ResourceLoadState::Missing) {
            outcome->report(RenderResultStatus::MissingMedia, std::move(warning));
        } else if (outcome && state == ResourceLoadState::Failed) {
            outcome->report(RenderResultStatus::Failed, std::move(warning));
        } else if (outcome) {
            outcome->report(RenderResultStatus::Pending, std::move(warning));
        }
    }
    return R;
}

} // namespace rt
#endif // ROUNDTABLE_HAS_SPINE
