/*
 * CompositeServiceLayerBuildSpine.cpp - live SpineClip layer builder.
 * Extracted verbatim from buildLayersForFrame() (modularization). The two
 * disabled if(false) blocks (pre-rendered video cache + video fallback) were
 * removed first, leaving only the live GPU-first / CPU-fallback Spine path.
 * Body is byte-identical to the original; outputs are returned via a result
 * struct using reference aliases so no body line changed.
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
#include <chrono>
#include <cmath>

#ifdef ROUNDTABLE_HAS_SPINE
namespace rt {

CompositeService::SpineLayerResult CompositeService::buildSpineClipLayer(
    SpineClip* spineClip, int64_t tick, int64_t localTick,
    uint32_t outW, uint32_t outH, bool scrubMode, bool playbackNonBlocking,
    bool staticRecomposite, bool& gpuSpineUsedThisFrame,
    std::vector<LayerInfo>& layers, float sx)
{
    SpineLayerResult R;
    auto&     frame              = R.frame;
    bool&     gpuSpineZeroCopy   = R.gpuSpineZeroCopy;
    auto&     gpuSpineDescriptor = R.gpuSpineDescriptor;
    uint32_t& gpuSpineW          = R.gpuSpineW;
    uint32_t& gpuSpineH          = R.gpuSpineH;
    bool&     cpuSpineRendered   = R.cpuSpineRendered;

    try {
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
                                const size_t atlasPageCount =
                                    state.engine.atlas().pages().size();
                                const size_t expectedPages = atlasPageCount > 0
                                    ? atlasPageCount : shared.pagePixels.size();
                                if (!shared.pagePixels.empty()) {
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
                                    m_gpuSpineActiveCharKey = charKey;
                                    spdlog::info("[SPINE-GPU] uploaded complete atlas for '{}': {} pages",
                                                 spineClip->characterName(), loaded);
                                } else {
                                    // Never render a partly uploaded character. A
                                    // missing page would otherwise sample a descriptor
                                    // left over from the preceding shot.
                                    sr->releaseAllTextures();
                                    spdlog::warn("[SPINE-GPU] atlas upload incomplete for '{}': {}/{} pages",
                                                 spineClip->characterName(), loaded,
                                                 expectedPages);
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
                                    // Frame to THIS animation's box (setup pose ∪ the
                                    // animation's own envelope) so a taller-than-idle pose
                                    // (e.g. an "action" animation that raises the head/arms)
                                    // fits inside the FBO instead of being clipped — while
                                    // normal animations keep their original setup-pose size.
                                    // The box is constant per animation, so it still avoids
                                    // the zoom/sway that live per-frame bounds would cause.
                                    // Fall back to setup-pose, then live bounds, when missing.
                                    float liveBx{0}, liveBy{0}, liveBw{0}, liveBh{0};
                                    state.engine.getBounds(liveBx, liveBy, liveBw, liveBh);
                                    const SpineSharedData::AnimBounds* ab = nullptr;
                                    {
                                        auto abIt = shared.animBounds.find(spineClip->animationName());
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
                                    bool previousFramePreserved = true;
                                    if (m_gpuSpineCount > 0) {
                                        const bool priorComplete = sr->waitForFrame();
                                        auto readback = priorComplete
                                            ? sr->readbackPixels() : nullptr;
                                        if (readback && !readback->pixels.empty() &&
                                            m_gpuSpineInsertedLayer >= 0 &&
                                            static_cast<size_t>(m_gpuSpineInsertedLayer) < layers.size()) {
                                            readback->premultipliedAlpha = true;
                                            auto& prevLayer = layers[m_gpuSpineInsertedLayer];
                                            prevLayer.frame = std::move(readback);
                                            prevLayer.gpuTextureReady = false;
                                        } else {
                                            // Keep the preceding character in the FBO
                                            // and render this character on the CPU. If
                                            // we cleared the FBO here, the prior layer's
                                            // zero-copy descriptor would become stale.
                                            previousFramePreserved = false;
                                            spdlog::error("[SPINE-GPU] could not preserve previous character frame; using CPU for '{}'",
                                                          spineClip->characterName());
                                        }
                                    }

                                    bool renderComplete = false;
                                    if (previousFramePreserved) {
                                        const bool began = sr->beginFrame();
                                        const bool drew = began &&
                                            sr->renderSkeleton(renderData, mvp, 1.0f);
                                        // Finish a begun command buffer even if atlas
                                        // validation rejected its draw commands.
                                        const bool submitted = began && sr->endFrame();
                                        const bool completed = submitted && sr->waitForFrame();
                                        renderComplete = drew && submitted && completed;
                                    }

                                    // STATIC frame (parked playhead, e.g. a
                                    // transform-handle drag): read the freshly
                                    // rendered FBO back into a stable CPU frame.
                                    // This (a) stops the layer aliasing the
                                    // shared FBO that the next same-tick
                                    // composite will clear, and (b) populates
                                    // the sticky last-good cache below, so a
                                    // subsequent same-tick composite that
                                    // momentarily fails to re-render the live
                                    // Spine still shows the character instead of
                                    // dropping the layer. Fixes the
                                    // "vanishes when resizing, back on play" bug.
                                    // During playback the tick advances so we
                                    // keep the fast zero-copy path.
                                    bool spineStableReadback = false;
                                    const bool requireStableFrame =
                                        staticRecomposite || m_forceFullResolution.load();
                                    if (renderComplete && requireStableFrame) {
                                        auto stable = sr->readbackPixels();
                                        if (stable && !stable->pixels.empty()) {
                                            stable->premultipliedAlpha = true; // Spine FBO is PMA
                                            frame = std::move(stable);
                                            cpuSpineRendered = true;  // route through contain-fit + 0.85
                                            spineStableReadback = true;
                                        }
                                    }

                                    if (renderComplete &&
                                        (!requireStableFrame || spineStableReadback)) {
                                        gpuSpineDone = true;
                                        gpuSpineUsedThisFrame = true;
                                    }

                                    if (gpuSpineDone && !spineStableReadback) {
                                        gpuSpineZeroCopy = true;
                                        ++m_gpuSpineCount;
                                        m_gpuSpineJustRendered = true;
                                        gpuSpineDescriptor = sr->outputDescriptorInfo();
                                        gpuSpineW = outW;
                                        gpuSpineH = outH;
                                    } else if (renderComplete && requireStableFrame &&
                                               !spineStableReadback) {
                                        spdlog::error("[SPINE-GPU] stable export readback failed for '{}'; using CPU fallback",
                                                      spineClip->characterName());
                                    }
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
    return R;
}

} // namespace rt
#endif // ROUNDTABLE_HAS_SPINE
