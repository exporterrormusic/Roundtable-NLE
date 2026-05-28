/*
 * FrameRenderer.cpp — Timeline evaluation and GPU compositing for export.
 */

#include "FrameRenderer.h"

#include <spdlog/spdlog.h>

#include <chrono>
#include <functional>

// Core types
#include "timeline/Timeline.h"
#include "timeline/Track.h"
#include "timeline/Clip.h"
#include "timeline/Position2D.h"
#include "timeline/VideoClip.h"
#include "timeline/ImageClip.h"
#include "timeline/SequenceClip.h"
#include "timeline/TitleClip.h"
#include "timeline/GraphicClip.h"
#include "timeline/AdjustmentClip.h"
#include "media/MediaPool.h"
#include "media/FrameCache.h"
#include "ClipRenderers.h"
#include "project/Project.h"

// GPU
#include "Compositor.h"
#include "VideoUploader.h"
#include "EffectProcessor.h"
#include "GpuContext.h"
#include "vulkan/Texture.h"

// Effects
#include "effects/LUT.h"

namespace rt {

FrameRenderer::FrameRenderer() = default;
FrameRenderer::~FrameRenderer() { shutdown(); }

bool FrameRenderer::init(const FrameRendererConfig& config, Compositor* compositor)
{
    m_config     = config;
    m_compositor = compositor;
    m_stats      = {};

    // Compositor is optional — without it we produce blank frames
    // (useful for testing the pipeline without GPU)
    if (compositor) {
        spdlog::info("FrameRenderer: Initialized with GPU compositor {}x{}",
                     config.outputWidth, config.outputHeight);
    } else {
        spdlog::info("FrameRenderer: Initialized without GPU (stub mode) {}x{}",
                     config.outputWidth, config.outputHeight);
    }

    m_initialized = true;
    return true;
}

void FrameRenderer::shutdown()
{
    m_compositor  = nullptr;
    m_initialized = false;
}

RenderedFrame FrameRenderer::renderFrame(const Timeline& timeline, int64_t frameIndex)
{
    RenderedFrame result;
    if (!m_initialized) {
        m_lastError = "FrameRenderer: Not initialized";
        return result;
    }

    auto t0 = std::chrono::high_resolution_clock::now();

    int64_t tick = frameToTick(frameIndex);

    result.frameIndex = frameIndex;
    result.timestamp  = static_cast<double>(frameIndex) * m_config.fpsDen / m_config.fpsNum;
    result.width      = m_config.outputWidth;
    result.height     = m_config.outputHeight;

    // Evaluate active clips and build compositor layers
    int layerCount = evaluateLayers(timeline, tick);

    if (m_compositor) {
        // Dispatch GPU composite
        if (!m_compositor->compositeSync()) {
            m_lastError = "FrameRenderer: Composite failed at frame " + std::to_string(frameIndex);
            spdlog::error("{}", m_lastError);
            return result;
        }

        // Read back pixels unless GPU-only mode
        if (!m_config.gpuOnly) {
            if (!m_compositor->readbackOutput(result.pixels)) {
                m_lastError = "FrameRenderer: Readback failed at frame " + std::to_string(frameIndex);
                spdlog::error("{}", m_lastError);
                return result;
            }
        }
    } else {
        // Stub mode: produce a solid-color frame (dark gray)
        if (!m_config.gpuOnly) {
            size_t pixelCount = static_cast<size_t>(result.width) * result.height * 4;
            result.pixels.resize(pixelCount);
            for (size_t i = 0; i < pixelCount; i += 4) {
                result.pixels[i]   = 30;   // R
                result.pixels[i+1] = 30;   // G
                result.pixels[i+2] = 30;   // B
                result.pixels[i+3] = 255;  // A
            }
        }
    }

    auto t1 = std::chrono::high_resolution_clock::now();
    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    ++m_stats.framesRendered;
    m_stats.totalRenderTimeMs += ms;
    m_stats.lastFrameTimeMs = ms;
    m_stats.avgFrameTimeMs = m_stats.totalRenderTimeMs / m_stats.framesRendered;
    m_stats.activeLayers = layerCount;

    return result;
}

RenderedFrame FrameRenderer::renderAtTime(const Timeline& timeline, double timeSeconds)
{
    int64_t frameIndex = static_cast<int64_t>(timeSeconds * m_config.fpsNum / m_config.fpsDen);
    return renderFrame(timeline, frameIndex);
}

int64_t FrameRenderer::renderRange(const Timeline& timeline,
                                    int64_t startFrame, int64_t endFrame,
                                    const FrameCallback& callback)
{
    if (!m_initialized || !callback) return 0;

    int64_t rendered = 0;
    for (int64_t f = startFrame; f <= endFrame; ++f) {
        auto frame = renderFrame(timeline, f);
        if (!frame.isValid()) continue;

        if (!callback(frame)) {
            spdlog::info("FrameRenderer: Render cancelled at frame {}", f);
            break;
        }
        ++rendered;
    }
    return rendered;
}

bool FrameRenderer::setResolution(uint32_t width, uint32_t height)
{
    if (width == 0 || height == 0) return false;
    m_config.outputWidth  = width;
    m_config.outputHeight = height;

    if (m_compositor) {
        return m_compositor->resize(width, height);
    }
    return true;
}

void FrameRenderer::resetStats()
{
    m_stats = {};
}

int64_t FrameRenderer::frameToTick(int64_t frameIndex) const noexcept
{
    // Convert frame index to TimeTick (48000 ticks/second)
    // tick = frameIndex * TICKS_PER_SECOND * fpsDen / fpsNum
    return static_cast<int64_t>(
        static_cast<double>(frameIndex) * 48000.0 * m_config.fpsDen / m_config.fpsNum);
}

int FrameRenderer::evaluateLayers(const Timeline& timeline, int64_t tick, int depth, bool setOnCompositor)
{
    // Walk all video tracks (bottom-to-top), find active clips at this time.
    // For each active clip, create a compositor layer with GPU texture.

    std::vector<CompositorLayer> gpuLayers;

    // ── Adjustment-layer pre-pass ────────────────────────────────────────
    // Track convention: lower index = HIGHER in the visual stack.
    // An adjustment on track T applies its effects to clips on tracks with
    // index > T (visually below), matching Premiere/AE adjustment layers.
    //
    // adjustmentEffectsForTrack[T] = effects from every adjustment clip
    // active at this tick on any track with index < T (above T).
    const size_t trackCnt = timeline.trackCount();
    std::vector<std::vector<EffectStack::EffectSnapshot>>
        adjustmentEffectsForTrack(trackCnt);
    {
        std::vector<EffectStack::EffectSnapshot> running;
        for (size_t ti = 0; ti < trackCnt; ++ti) {
            adjustmentEffectsForTrack[ti] = running;
            auto* tr = const_cast<Track*>(timeline.track(ti));
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

    for (size_t ri = timeline.trackCount(); ri > 0; --ri) {
        size_t ti = ri - 1;
        const auto& pendingAdjustmentEffects =
            adjustmentEffectsForTrack[ti];
        // const_cast is safe: we only call read-only accessors (clipsAtTime,
        // opacity().evaluate, etc.) which are logically const but lack const
        // overloads on KeyframeTrack.
        auto* track = const_cast<Track*>(timeline.track(ti));
        if (!track || track->type() != TrackType::Video) continue;
        if (track->isMuted()) continue;

        auto clips = track->clipsAtTime(tick);
        for (auto* clip : clips) {
            if (!clip || !clip->isEnabled()) continue;

            // Adjustment layers don't render a layer of their own —
            // their effects are harvested in the pre-pass above.
            if (dynamic_cast<AdjustmentClip*>(clip))
                continue;

            // ── Decode video frame via MediaPool ────────────────────────
            auto* videoClip = dynamic_cast<VideoClip*>(clip);
            auto* imageClip = dynamic_cast<ImageClip*>(clip);
            auto* seqClip   = dynamic_cast<SequenceClip*>(clip);

            if (seqClip) {
                // Nested sequence clip — recursively composite the inner
                // timeline to a single BGRA frame, then treat it exactly
                // like a video layer so the outer clip's effects (blur,
                // opacity, transform, etc.) are applied correctly.
                if (!m_project || depth >= kMaxNestDepth) {
                    gpuLayers.emplace_back();
                    gpuLayers.back().enabled = false;
                    continue;
                }
                size_t seqIdx = seqClip->sequenceIndex();
                const Timeline* nested = m_project->sequence(seqIdx);
                if (!nested) {
                    gpuLayers.emplace_back();
                    gpuLayers.back().enabled = false;
                    continue;
                }
                // Compute local tick within the nested timeline
                int64_t localTick = tick - clip->timelineIn();
                int64_t innerTick = clip->sourceIn()
                    + static_cast<int64_t>(localTick * clip->effectiveSpeed(localTick));
                if (innerTick < 0) innerTick = 0;

                // Render inner timeline to a CPU BGRA frame
                auto innerFrame = renderNestedFrame(*nested, innerTick, depth + 1);
                if (!innerFrame || !innerFrame->ensurePixels()) {
                    gpuLayers.emplace_back();
                    gpuLayers.back().enabled = false;
                    continue;
                }

                // ── Upload inner composite to GPU ──────────────────────
                CompositorLayer cl;
                cl.enabled = true;

                VkDescriptorImageInfo layerTexInfo{};
                bool hasTexture = false;

                if (m_videoUploader && m_compositor) {
                    auto gpuFrame = m_videoUploader->upload(*innerFrame);
                    if (gpuFrame && gpuFrame->valid && gpuFrame->vkImageView) {
                        layerTexInfo.sampler     = static_cast<VkSampler>(gpuFrame->vkSampler);
                        layerTexInfo.imageView   = static_cast<VkImageView>(gpuFrame->vkImageView);
                        layerTexInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                        hasTexture = true;
                    } else {
                        cl.enabled = false;
                    }
                } else {
                    cl.enabled = false;
                }

                // ── Apply outer SequenceClip effects ────────────────────
                auto snapshots = clip->effects().evaluate(localTick);

                // Append adjustment-layer effects from tracks above
                if (!pendingAdjustmentEffects.empty()) {
                    snapshots.insert(snapshots.end(),
                                     pendingAdjustmentEffects.begin(),
                                     pendingAdjustmentEffects.end());
                }

                if (hasTexture && m_effectProcessor && m_effectProcessor->isInitialized()
                    && !snapshots.empty())
                {
                    // Check for LUT effect
                    for (const auto& snap : snapshots) {
                        if (snap.type == EffectType::LUT) {
                            for (size_t ei = 0; ei < clip->effects().effectCount(); ++ei) {
                                auto& fx = clip->effects().effect(ei);
                                if (fx.effectType() == EffectType::LUT && fx.isEnabled()) {
                                    auto* lutFx = static_cast<LUT*>(&fx);
                                    if (lutFx->hasLUT())
                                        m_effectProcessor->uploadLUT3D(lutFx->lutData(), lutFx->lutSize());
                                    break;
                                }
                            }
                            break;
                        }
                    }

                    if (m_effectProcessor->processSync(layerTexInfo, snapshots))
                        layerTexInfo = snapshottedEffectOutput(
                            gpuLayers.size(), layerTexInfo);
                }

                cl.textureInfo = layerTexInfo;

                // ── Build transform from outer SequenceClip ─────────────
                float opac = clip->opacity().evaluate(localTick);
                auto p2  = evaluatePosition2D(clip->positionX(), clip->positionY(), localTick);
                float px = p2.first;
                float py = p2.second;
                float sx = clip->scaleX().evaluate(localTick);
                float sy = clip->scaleY().evaluate(localTick);
                float rot = clip->rotation().evaluate(localTick);
                float ancX = clip->anchorX().evaluate(localTick);
                float ancY = clip->anchorY().evaluate(localTick);

                cl.transform = Compositor::buildViewportTransform(
                    innerFrame->width, innerFrame->height,
                    m_config.outputWidth, m_config.outputHeight,
                    px, py, sx, sy, rot,
                    /*containFit=*/false, ancX, ancY);

                cl.opacity   = opac;
                cl.blendMode = static_cast<BlendMode>(clip->blendMode());
                cl.cropLeft = cl.cropRight = cl.cropTop = cl.cropBottom = 0.0f;

                gpuLayers.push_back(cl);
                continue;
            }

            // ── TitleClip / GraphicClip ── (CPU-render text/shape/.png to BGRA, then upload to GPU)
            // These clip types have no MediaPool source — they are rendered on-the-fly
            // via ClipRenderers, same as the program monitor preview path.
            auto* titleClip   = dynamic_cast<TitleClip*>(clip);
            auto* graphicClip = dynamic_cast<GraphicClip*>(clip);

            if (titleClip || graphicClip) {
                int64_t localTick = tick - clip->timelineIn();
                std::shared_ptr<CachedFrame> renderedFrame;

                if (titleClip) {
                    renderedFrame = renderTitleClip(titleClip, tick,
                        m_config.outputWidth, m_config.outputHeight);
                } else {
                    uint32_t refW = 0, refH = 0;
                    if (m_project) {
                        refW = m_project->settings().resolution().width;
                        refH = m_project->settings().resolution().height;
                    }
                    renderedFrame = renderGraphicClip(graphicClip, tick,
                        m_config.outputWidth, m_config.outputHeight, refW, refH);
                }

                if (!renderedFrame || !renderedFrame->ensurePixels()) {
                    gpuLayers.emplace_back();
                    gpuLayers.back().enabled = false;
                    continue;
                }

                // ── Upload to GPU via VideoUploader ─────────────────────
                CompositorLayer cl;
                cl.enabled = true;

                VkDescriptorImageInfo layerTexInfo{};
                bool hasTexture = false;

                if (m_videoUploader && m_compositor) {
                    auto gpuFrame = m_videoUploader->upload(*renderedFrame);
                    if (gpuFrame && gpuFrame->valid && gpuFrame->vkImageView) {
                        layerTexInfo.sampler     = static_cast<VkSampler>(gpuFrame->vkSampler);
                        layerTexInfo.imageView   = static_cast<VkImageView>(gpuFrame->vkImageView);
                        layerTexInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                        hasTexture = true;
                    } else {
                        cl.enabled = false;
                    }
                } else {
                    cl.enabled = false;
                }

                // ── Apply per-clip effects via EffectProcessor ──────────
                // (blur, color correction, etc. on text/shape layers)
                auto snapshots = clip->effects().evaluate(localTick);

                // Append adjustment-layer effects from tracks above
                if (!pendingAdjustmentEffects.empty()) {
                    snapshots.insert(snapshots.end(),
                                     pendingAdjustmentEffects.begin(),
                                     pendingAdjustmentEffects.end());
                }

                if (hasTexture && m_effectProcessor && m_effectProcessor->isInitialized()
                    && !snapshots.empty())
                {
                    // Check for LUT effect
                    for (const auto& snap : snapshots) {
                        if (snap.type == EffectType::LUT) {
                            for (size_t ei = 0; ei < clip->effects().effectCount(); ++ei) {
                                auto& fx = clip->effects().effect(ei);
                                if (fx.effectType() == EffectType::LUT && fx.isEnabled()) {
                                    auto* lutFx = static_cast<LUT*>(&fx);
                                    if (lutFx->hasLUT())
                                        m_effectProcessor->uploadLUT3D(lutFx->lutData(), lutFx->lutSize());
                                    break;
                                }
                            }
                            break;
                        }
                    }

                    if (m_effectProcessor->processSync(layerTexInfo, snapshots))
                        layerTexInfo = snapshottedEffectOutput(
                            gpuLayers.size(), layerTexInfo);
                }

                cl.textureInfo = layerTexInfo;

                // ── Build transform ─────────────────────────────────────
                float opac = clip->opacity().evaluate(localTick);
                auto p2  = evaluatePosition2D(clip->positionX(), clip->positionY(), localTick);
                float px = p2.first;
                float py = p2.second;
                float sx = clip->scaleX().evaluate(localTick);
                float sy = clip->scaleY().evaluate(localTick);
                float rot = clip->rotation().evaluate(localTick);
                float ancX = clip->anchorX().evaluate(localTick);
                float ancY = clip->anchorY().evaluate(localTick);

                cl.transform = Compositor::buildViewportTransform(
                    renderedFrame->width, renderedFrame->height,
                    m_config.outputWidth, m_config.outputHeight,
                    px, py, sx, sy, rot,
                    /*containFit=*/false, ancX, ancY);

                cl.opacity   = opac;
                cl.blendMode = static_cast<BlendMode>(clip->blendMode());
                cl.cropLeft = cl.cropRight = cl.cropTop = cl.cropBottom = 0.0f;

                gpuLayers.push_back(cl);
                continue;
            }

            if ((!videoClip && !imageClip) || !m_mediaPool) {
                // Non-video/image clips or no media pool — skip
                if (!videoClip && !imageClip) {
                    gpuLayers.emplace_back();
                    gpuLayers.back().enabled = false;
                }
                continue;
            }

            const std::string& mediaPath = videoClip
                ? videoClip->mediaPath() : imageClip->mediaPath();
            if (mediaPath.empty()) continue;

            uint64_t handle = m_mediaPool->open(mediaPath);
            if (handle == 0) continue;

            int64_t localTick = tick - clip->timelineIn();
            int64_t srcTick   = clip->sourceIn() + static_cast<int64_t>(localTick * clip->effectiveSpeed(localTick));
            if (srcTick < 0) continue;

            int64_t frameNum = 0;
            if (videoClip) {
                double fps = videoClip->sourceFps();
                if (fps <= 0.0) fps = 24.0;
                frameNum = static_cast<int64_t>(ticksToSeconds(srcTick) * fps);

                auto* mediaInfo = m_mediaPool->getInfo(handle);
                if (mediaInfo && mediaInfo->frameCount <= 1) frameNum = 0;
            }
            // ImageClip always uses frame 0

            auto frame = m_mediaPool->getFrame(handle, frameNum, ResolutionTier::Full, false);
            if (!frame || !frame->ensurePixels()) continue;

            // Packed-alpha detection (matches VideoDecoderInit heuristic)
            bool isPacked = false;
            uint32_t nominalW = frame->width;
            uint32_t nominalH = frame->height;
            if (auto* info = m_mediaPool->getInfo(handle)) {
                isPacked = info->packedAlpha;
            }
            if (!isPacked && nominalH > 0 && nominalW > 0 &&
                nominalH >= nominalW * 2) {
                isPacked = true;
            }
            if (isPacked) nominalH /= 2;

            // ── Upload to GPU via VideoUploader ─────────────────────────
            CompositorLayer cl;
            cl.enabled = true;
            cl.isPacked = isPacked;

            VkDescriptorImageInfo layerTexInfo{};
            bool hasTexture = false;

            if (m_videoUploader && m_compositor) {
                auto gpuFrame = m_videoUploader->upload(*frame);
                if (gpuFrame && gpuFrame->valid && gpuFrame->vkImageView) {
                    layerTexInfo.sampler     = static_cast<VkSampler>(gpuFrame->vkSampler);
                    layerTexInfo.imageView   = static_cast<VkImageView>(gpuFrame->vkImageView);
                    layerTexInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                    hasTexture = true;
                } else {
                    cl.enabled = false;
                }
            } else {
                cl.enabled = false;
            }

            // ── Apply per-clip effects via EffectProcessor ──────────────
            int64_t localTick2 = tick - clip->timelineIn();
            auto snapshots = clip->effects().evaluate(localTick2);

            // Append adjustment-layer effects from tracks above
            if (!pendingAdjustmentEffects.empty()) {
                snapshots.insert(snapshots.end(),
                                 pendingAdjustmentEffects.begin(),
                                 pendingAdjustmentEffects.end());
            }

            if (hasTexture && m_effectProcessor && m_effectProcessor->isInitialized()
                && !snapshots.empty())
            {
                // Check for LUT effect and upload its 3D texture if needed
                for (const auto& snap : snapshots) {
                    if (snap.type == EffectType::LUT) {
                        // Find the LUT effect in the clip's effect stack
                        for (size_t ei = 0; ei < clip->effects().effectCount(); ++ei) {
                            auto& fx = clip->effects().effect(ei);
                            if (fx.effectType() == EffectType::LUT && fx.isEnabled()) {
                                auto* lutFx = static_cast<LUT*>(&fx);
                                if (lutFx->hasLUT())
                                    m_effectProcessor->uploadLUT3D(lutFx->lutData(), lutFx->lutSize());
                                break;
                            }
                        }
                        break;
                    }
                }

                if (m_effectProcessor->processSync(layerTexInfo, snapshots))
                    layerTexInfo = snapshottedEffectOutput(
                        gpuLayers.size(), layerTexInfo);
            }

            cl.textureInfo = layerTexInfo;

            // ── Build transform ─────────────────────────────────────────
            float opac = clip->opacity().evaluate(localTick);
            auto p2  = evaluatePosition2D(clip->positionX(), clip->positionY(), localTick);
            float px = p2.first;
            float py = p2.second;
            float sx = clip->scaleX().evaluate(localTick);
            float sy = clip->scaleY().evaluate(localTick);
            float rot = clip->rotation().evaluate(localTick);
            // Clip-level anchor is stored REF-1920 px; FrameRenderer's
            // px/py are already in REF-1920 (no scale to output here, the
            // shader handles the output mapping), so pass anchor in the
            // same space.
            float ancX = clip->anchorX().evaluate(localTick);
            float ancY = clip->anchorY().evaluate(localTick);

            cl.transform = Compositor::buildViewportTransform(
                nominalW, nominalH,
                m_config.outputWidth, m_config.outputHeight,
                px, py, sx, sy, rot,
                /*containFit=*/false, ancX, ancY);

            cl.opacity   = opac;
            cl.blendMode = static_cast<BlendMode>(clip->blendMode());

            // Crop (VideoClip stores as 0–100 percentages, GPU uses 0–1)
            cl.cropLeft   = videoClip->cropLeft()   / 100.0f;
            cl.cropRight  = videoClip->cropRight()  / 100.0f;
            cl.cropTop    = videoClip->cropTop()    / 100.0f;
            cl.cropBottom = videoClip->cropBottom() / 100.0f;

            gpuLayers.push_back(cl);
        }
    }

    // Set layers on compositor (skip when recursively building layers for
    // a nested sequence — the caller will composite them manually).
    if (setOnCompositor) {
        if (m_compositor && !gpuLayers.empty()) {
            // Filter out disabled placeholder layers
            std::vector<CompositorLayer> enabled;
            for (auto& l : gpuLayers)
                if (l.enabled) enabled.push_back(l);

            if (!enabled.empty())
                m_compositor->setLayers(enabled);
            else
                m_compositor->clearLayers();
        } else if (m_compositor) {
            m_compositor->clearLayers();
        }
    }

    return static_cast<int>(gpuLayers.size());
}

std::shared_ptr<CachedFrame> FrameRenderer::renderNestedFrame(
    const Timeline& nested, int64_t tick, int depth)
{
    if (!m_compositor) return nullptr;

    // Clear any stale compositor state from previous frames, then
    // evaluate the inner timeline fully (build layers + set on compositor).
    m_compositor->clearLayers();
    int innerLayerCount = evaluateLayers(nested, tick, depth, /*setOnCompositor=*/true);

    if (innerLayerCount == 0) {
        m_compositor->clearLayers();
        return nullptr;
    }

    if (!m_compositor->compositeSync()) {
        spdlog::error("FrameRenderer: Nested sequence composite failed at tick {}", tick);
        m_compositor->clearLayers();
        return nullptr;
    }

    std::vector<uint8_t> pixels;
    if (!m_compositor->readbackOutput(pixels)) {
        spdlog::error("FrameRenderer: Nested sequence readback failed at tick {}", tick);
        m_compositor->clearLayers();
        return nullptr;
    }

    // Leave compositor clean for the outer caller.
    m_compositor->clearLayers();

    if (pixels.empty()) return nullptr;

    auto frame = std::make_shared<CachedFrame>();
    frame->width  = m_config.outputWidth;
    frame->height = m_config.outputHeight;
    frame->stride = m_config.outputWidth * 4;
    frame->pixels = std::move(pixels);
    frame->unpackedAlpha = true;
    // Nested composite output is BGRA (compositor writes BGRA for Qt readback).
    // The preview path treats this as straight BGRA (needsSwapRB=false), matching here.

    return frame;
}

VkDescriptorImageInfo FrameRenderer::snapshottedEffectOutput(
    size_t layerIndex,
    const VkDescriptorImageInfo& fallback)
{
    if (!m_effectProcessor)
        return fallback;

    // Ensure per-layer snapshot texture pool is large enough
    if (layerIndex >= m_effectSnapshots.size())
        m_effectSnapshots.resize(layerIndex + 1);

    auto& snapTex = m_effectSnapshots[layerIndex];

    // Lazy-allocate or re-allocate when output resolution changes
    if (!snapTex ||
        snapTex->width()  != m_config.outputWidth ||
        snapTex->height() != m_config.outputHeight)
    {
        if (snapTex)
            snapTex->destroy();
        else
            snapTex = std::make_unique<Texture>();

        TextureConfig cfg;
        cfg.width  = m_config.outputWidth;
        cfg.height = m_config.outputHeight;
        cfg.format = VK_FORMAT_R8G8B8A8_UNORM;
        cfg.usage  = VK_IMAGE_USAGE_SAMPLED_BIT
                   | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

        auto& gpuCtx = GpuContext::get();
        if (!snapTex->create(gpuCtx.allocator().handle(),
                             gpuCtx.vkDevice(), cfg))
        {
            spdlog::warn("[FRAMERENDERER] Failed to create effect snapshot "
                         "texture {}x{} for layer {} — using shared output",
                         m_config.outputWidth, m_config.outputHeight,
                         layerIndex);
            return fallback;
        }
    }

    if (m_effectProcessor->snapshotOutputSync(*snapTex))
        return snapTex->descriptorInfo();

    // Fallback: use the shared EffectProcessor output directly
    return fallback;
}

} // namespace rt
