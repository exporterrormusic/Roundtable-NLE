/*
 * SpinePrerenderer.cpp — offline Spine → HEVC/ProRes video renderer.
 */

#include <volk.h>
#include <vk_mem_alloc.h>

#include "SpinePrerenderer.h"

#ifdef ROUNDTABLE_HAS_SPINE

#include "SpineEngine.h"
#include "SpineAtlas.h"
#include "SpineAnimation.h"
#include "timeline/SpineClip.h"  // for CharacterStance, resolvePaths

#include "media/ProResEncoder.h"
#include "media/HWAlphaEncoder.h"
#include "media/ChromaKeyEncoder.h"
#include "GpuContext.h"
#include "GpuScheduler.h"
#include "SpineRenderer.h"
#include "vulkan/Buffer.h"

#include "PathUtils.h"
#include <spdlog/spdlog.h>

// stb_image for CPU fallback atlas loading (declaration only — impl is in SpineRenderer.cpp)
#include "stb_image.h"

#include <algorithm>
#include <cmath>
#include <filesystem>

namespace fs = std::filesystem;

namespace rt {

SpinePrerenderer::SpinePrerenderer()  = default;
SpinePrerenderer::~SpinePrerenderer() = default;

// ─── Public API ──────────────────────────────────────────────────────────────

PrerenderResult SpinePrerenderer::render(const PrerenderJob& job,
                                          PrerenderProgressFn progress)
{
    spdlog::info("SpinePrerenderer: rendering '{}' / '{}' / '{}' → {}",
                 job.characterName, job.outfit, job.animationName,
                 pathToUtf8(job.outputPath));

    // Ensure output directory exists
    fs::create_directories(job.outputPath.parent_path());

    // Try GPU first, then CPU fallback
    auto& gpu = GpuContext::get();
    if (!m_forceCPU && gpu.isInitialized()) {
        return renderGPU(job, std::move(progress));
    }
    return renderCPU(job, std::move(progress));
}

// ─── GPU rendering path (SpineRenderer + Vulkan readback) ───────────────────

PrerenderResult SpinePrerenderer::renderGPU(const PrerenderJob& job,
                                             PrerenderProgressFn progress)
{
    PrerenderResult result;
    result.outputPath = job.outputPath;

    // ── 1. Load skeleton ────────────────────────────────────────────────
    SpineEngine engine;
    auto paths = SpineEngine::resolvePaths(m_assetsDir, job.characterName,
                                            job.outfit, CharacterStance::Default);
    if (!paths.valid) {
        result.error = "Failed to resolve skeleton paths for " + job.characterName;
        spdlog::error("SpinePrerenderer: {}", result.error);
        return result;
    }

    if (!engine.loadSkeleton(paths.skelPath, paths.atlasPath)) {
        result.error = "Failed to load skeleton: " + paths.skelPath;
        spdlog::error("SpinePrerenderer: {}", result.error);
        return result;
    }

    // ── 2. Set animation and get duration ───────────────────────────────
    if (!engine.animation().hasAnimation(job.animationName)) {
        result.error = "Animation not found: " + job.animationName;
        spdlog::error("SpinePrerenderer: {}", result.error);
        return result;
    }

    engine.animation().setBodyAnimation(job.animationName, false); // non-looping for render

    // If rendering a talking variant, start the talk track so that
    // evaluateAtTime() applies mouth/jaw bones.
    if (job.isTalking) {
        engine.animation().startTalking();
    }

    auto animInfos = engine.animation().listAnimations();
    float animDuration = 0.0f;
    for (const auto& ai : animInfos) {
        if (ai.name == job.animationName) {
            animDuration = ai.duration;
            break;
        }
    }

    if (animDuration <= 0.0f) {
        // Some animations (talk_end) have 0 duration — skip silently
        result.error = "Animation has zero duration: " + job.animationName;
        spdlog::warn("SpinePrerenderer: {}", result.error);
        return result;
    }

    result.duration = animDuration;
    result.frameCount = static_cast<int64_t>(std::ceil(animDuration * job.fps));
    if (result.frameCount < 1) result.frameCount = 1;

    // ── 3. Determine render dimensions from envelope bounds ─────────────
    // Sample bounds across the ENTIRE animation to find the maximum
    // extent (envelope).  Using a single getBounds() from the setup
    // pose causes the character to appear to "zoom" on loop restart
    // because different frames can have wildly different bounding boxes.
    float envMinX =  1e9f, envMinY =  1e9f;
    float envMaxX = -1e9f, envMaxY = -1e9f;
    {
        const int samples = std::max(30, static_cast<int>(animDuration * 30.0f));
        for (int s = 0; s <= samples; ++s) {
            float t = (animDuration > 0.0f)
                ? (static_cast<float>(s) / static_cast<float>(samples)) * animDuration
                : 0.0f;
            engine.evaluateAtTime(t, job.isTalking ? t : 0.0f);
            float bx, by, bw, bh;
            engine.getBounds(bx, by, bw, bh);
            if (bx < envMinX)       envMinX = bx;
            if (by < envMinY)       envMinY = by;
            if (bx + bw > envMaxX)  envMaxX = bx + bw;
            if (by + bh > envMaxY)  envMaxY = by + bh;
        }
    }
    float bx = envMinX;
    float by = envMinY;
    float bw = envMaxX - envMinX;
    float bh = envMaxY - envMinY;

    if (bw < 1.0f || bh < 1.0f) {
        result.error = "Degenerate skeleton bounds";
        spdlog::error("SpinePrerenderer: {}", result.error);
        return result;
    }

    spdlog::info("SpinePrerenderer: envelope bounds x={:.0f} y={:.0f} w={:.0f} h={:.0f} (sampled)",
                 bx, by, bw, bh);

    // Render at the skeleton's native bounds + padding, rounded to even
    uint32_t renderW = static_cast<uint32_t>(std::ceil(bw * (1.0f / job.paddingFactor)));
    uint32_t renderH = static_cast<uint32_t>(std::ceil(bh * (1.0f / job.paddingFactor)));
    renderW = (renderW + 1) & ~1u; // ensure even
    renderH = (renderH + 1) & ~1u;

    // Cap height first — 1920 is enough for Full HD output.
    renderH = std::min(renderH, 1920u);

    // Compute the required width so the character is NOT clipped when using
    // height-based fitZoom (fH/bh * paddingFactor).  This keeps width in
    // proportion with whatever height cap we applied.
    {
        const float heightFitZoom =
            (static_cast<float>(renderH) / bh) * job.paddingFactor;
        uint32_t requiredW =
            static_cast<uint32_t>(std::ceil(bw * heightFitZoom)) + 4u;
        requiredW = (requiredW + 1) & ~1u; // even
        renderW   = std::max(renderW, requiredW);
    }
    renderW = std::min(renderW, 1920u); // hard ceiling

    result.width = renderW;
    result.height = renderH;

    spdlog::info("SpinePrerenderer: render size {}x{}, {} frames @ {}fps, dur={:.2f}s",
                 renderW, renderH, result.frameCount, job.fps, animDuration);

    // ── 4. Set up dedicated SpineRenderer at the render resolution ──────
    auto& gpu = GpuContext::get();

    // Create a per-thread CommandPool so we don't contend with the
    // main thread's pool (Vulkan pools are NOT thread-safe).
    CommandPool localCmdPool;
    if (!localCmdPool.create(gpu.vkDevice(), gpu.graphicsQueueFamilyIndex())) {
        result.error = "Failed to create thread-local command pool";
        spdlog::error("SpinePrerenderer: {}", result.error);
        return result;
    }
    localCmdPool.setQueueMutex(&gpu.graphicsQueueMutex());

    // Create a dedicated SpineRenderer for this render job to avoid
    // interfering with the live preview renderer.
    SpineRenderer spineRenderer;
    SpineRendererConfig srConfig;
    srConfig.renderWidth  = renderW;
    srConfig.renderHeight = renderH;
    srConfig.framesInFlight = 1; // single-buffered — we wait per frame

    if (!spineRenderer.init(gpu.device(), gpu.allocator(),
                             localCmdPool, gpu.graphicsQueue(),
                             srConfig)) {
        result.error = "Failed to initialize SpineRenderer for prerender";
        spdlog::error("SpinePrerenderer: {}", result.error);
        localCmdPool.destroy();
        return result;
    }

    // Serialise graphics queue submissions with the main thread
    spineRenderer.setQueueMutex(&gpu.graphicsQueueMutex());

    // Upload atlas textures
    int pagesLoaded = spineRenderer.loadAtlasTextures(engine.atlas());
    if (pagesLoaded <= 0) {
        result.error = "Failed to upload atlas textures to GPU";
        spdlog::error("SpinePrerenderer: {}", result.error);
        spineRenderer.shutdown();
        localCmdPool.destroy();
        return result;
    }

    // ── 5. Create staging buffer for GPU→CPU readback ───────────────────
    VkDeviceSize imageSize = static_cast<VkDeviceSize>(renderW) * renderH * 4;
    Buffer readbackStaging;
    if (!readbackStaging.create(gpu.allocator().handle(), imageSize,
                                 BufferUsage::Readback)) {
        result.error = "Failed to create readback staging buffer";
        spdlog::error("SpinePrerenderer: {}", result.error);
        spineRenderer.shutdown();
        localCmdPool.destroy();
        return result;
    }

    // ── 6. Open encoder ────────────────────────────────────────────────
    //
    // Encoder selection:
    //   GreenScreen / BlueScreen / CustomColor → ChromaKeyEncoder (standard H.264 .mp4)
    //   HEVCPackedAlpha (legacy)               → HWAlphaEncoder (packed-alpha .mp4)
    //   ProRes4444                             → ProResAlphaEncoder (native alpha .mov)

    bool useChromaKey = (job.format == PrerenderFormat::GreenScreen ||
                         job.format == PrerenderFormat::BlueScreen ||
                         job.format == PrerenderFormat::CustomColor);

    ChromaKeyEncoder    chromaEncoder;
    HWAlphaEncoder      hevcEncoder;
    ProResAlphaEncoder  proresEncoder;
    enum class ActiveEncoder { ChromaKey, HEVC, ProRes } activeEnc{};

    {
        bool opened = false;

        // Chroma key formats: render on solid background as normal H.264 .mp4
        if (!opened && useChromaKey) {
            auto ckOutputPath = job.outputPath;
            ckOutputPath.replace_extension(".mp4");
            if (chromaEncoder.open(ckOutputPath, renderW, renderH, job.fps, job.crf)) {
                activeEnc = ActiveEncoder::ChromaKey;
                opened = true;
                spdlog::info("SpinePrerenderer: using chroma-key H.264 encoder for '{}' [{}]",
                             job.animationName,
                             chromaEncoder.isHardwareAccelerated() ? "NVENC" : "CPU");
            }
        }

        // Legacy HEVC packed-alpha (for backwards compatibility)
        if (!opened && (job.format == PrerenderFormat::HEVCPackedAlpha || job.format == PrerenderFormat::Auto)) {
            auto hevcOutputPath = job.outputPath;
            hevcOutputPath.replace_extension(".mp4");
            if (hevcEncoder.open(hevcOutputPath, renderW, renderH, job.fps, job.crf)) {
                activeEnc = ActiveEncoder::HEVC;
                opened = true;
                spdlog::info("SpinePrerenderer: using HEVC packed-alpha encoder for '{}' [{}]",
                             job.animationName,
                             hevcEncoder.isHardwareAccelerated() ? "NVENC" : "CPU");
            }
        }

        // ProRes 4444 (fallback or explicitly requested)
        if (!opened) {
            auto proresOutputPath = job.outputPath;
            proresOutputPath.replace_extension(".mov");
            if (proresEncoder.open(proresOutputPath, renderW, renderH, job.fps, 4)) {
                activeEnc = ActiveEncoder::ProRes;
                opened = true;
                spdlog::info("SpinePrerenderer: using ProRes 4444 encoder for '{}'",
                             job.animationName);
            }
        }

        if (!opened) {
            result.error = "Failed to open any encoder (ChromaKey + HEVC + ProRes all failed)";
            spdlog::error("SpinePrerenderer: {}", result.error);
            readbackStaging.destroy();
            spineRenderer.shutdown();
            localCmdPool.destroy();
            return result;
        }
    }

    // Encoder function pointers for unified render loop
    auto encodeFrame = [&](const uint8_t* pixels) -> bool {
        switch (activeEnc) {
        case ActiveEncoder::ChromaKey: return chromaEncoder.writeFrame(pixels);
        case ActiveEncoder::HEVC:      return hevcEncoder.writeFrame(pixels);
        case ActiveEncoder::ProRes:    return proresEncoder.writeFrame(pixels);
        }
        return false;
    };
    auto finalizeEncoder = [&]() {
        switch (activeEnc) {
        case ActiveEncoder::ChromaKey: chromaEncoder.finalize(); break;
        case ActiveEncoder::HEVC:      hevcEncoder.finalize(); break;
        case ActiveEncoder::ProRes:    proresEncoder.finalize(); break;
        }
    };
    auto encoderError = [&]() -> std::string {
        switch (activeEnc) {
        case ActiveEncoder::ChromaKey: return chromaEncoder.lastError();
        case ActiveEncoder::HEVC:      return hevcEncoder.lastError();
        case ActiveEncoder::ProRes:    return proresEncoder.lastError();
        }
        return {};
    };

    // ── 7. Render loop ──────────────────────────────────────────────────
    const float fW = static_cast<float>(renderW);
    const float fH = static_cast<float>(renderH);
    const float cx = bx + bw * 0.5f;
    const float cy = by + bh * 0.5f;
    // Use HEIGHT-based fit to match COMPOSE's fitScale = canvasH / bh * 0.85.
    // The old min(fW/bw, fH/bh) caused width-limiting when renderW was capped
    // at 1080, making the character fill LESS than paddingFactor of the video
    // height.  The compositor's /0.9 compensation assumes exactly paddingFactor
    // fill, so using min() produced undersized characters.
    const float fitZoom = (fH / bh) * job.paddingFactor;

    glm::mat4 proj  = SpineRenderer::orthoProjection(fW, fH, cx, cy, fitZoom);
    glm::mat4 model = SpineRenderer::modelMatrix(0.0f, 0.0f);
    glm::mat4 mvp   = proj * model;

    std::vector<uint8_t> pixelBuf(static_cast<size_t>(imageSize));

    for (int64_t fi = 0; fi < result.frameCount; ++fi) {
        // Progress callback
        if (progress && !progress(fi, result.frameCount)) {
            result.error = "Cancelled by user";
            spdlog::info("SpinePrerenderer: render cancelled at frame {}/{}",
                         fi, result.frameCount);
            finalizeEncoder();
            readbackStaging.destroy();
            spineRenderer.shutdown();
            localCmdPool.destroy();
            return result;
        }

        // Evaluate animation at this frame's time
        float t = static_cast<float>(fi) / static_cast<float>(job.fps);
        engine.evaluateAtTime(t, job.isTalking ? t : 0.0f);

        // Extract meshes and render
        SpineRenderData renderData = engine.extractMeshes();
        if (fi == 0) {
            size_t totalVerts = 0, totalIdx = 0;
            for (auto& b : renderData.batches) {
                totalVerts += b.vertices.size();
                totalIdx   += b.indices.size();
            }
            spdlog::info("SpinePrerenderer: frame0 batches={} verts={} idx={}",
                         renderData.batches.size(), totalVerts, totalIdx);
        }
        spineRenderer.beginFrame();
        if (!renderData.batches.empty()) {
            spineRenderer.renderSkeleton(renderData, mvp, 1.0f);
        }
        spineRenderer.endFrame();
        spineRenderer.waitForFrame();

        // Readback framebuffer → staging buffer → CPU
        {
            auto& fbo = spineRenderer.framebuffer();
            VkCommandBuffer cmd = localCmdPool.beginSingleTime();

            // Transition color image to TRANSFER_SRC
            // After endFrame(), the FBO is in SHADER_READ_ONLY_OPTIMAL.
            // Transition directly to TRANSFER_SRC for the readback copy.
            VkImageMemoryBarrier barrier{};
            barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
            barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            barrier.oldLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            barrier.newLayout     = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            barrier.image         = fbo.colorImage();
            barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            vkCmdPipelineBarrier(cmd,
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT,
                0, 0, nullptr, 0, nullptr, 1, &barrier);

            // Copy image → staging buffer
            VkBufferImageCopy region{};
            region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.imageExtent = {renderW, renderH, 1};
            vkCmdCopyImageToBuffer(cmd, fbo.colorImage(),
                                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                   readbackStaging.handle(), 1, &region);

            // Transition back to SHADER_READ
            barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            barrier.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            barrier.newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            vkCmdPipelineBarrier(cmd,
                VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                0, 0, nullptr, 0, nullptr, 1, &barrier);

            // P1.3: route through GpuScheduler.  No fence — we just
            // wait for the queue to drain.  vkQueueWaitIdle still
            // requires direct queue access; the scheduler's queue
            // mutex protects the submit and the wait stays after.
            vkEndCommandBuffer(cmd);
            GpuSubmission sub{};
            sub.cmd   = cmd;
            sub.queue = GpuQueueKind::Graphics;
            sub.tag   = "SpinePrerenderer::readback";
            GpuContext::get().scheduler().submit(sub);
            {
                std::lock_guard qLock(gpu.graphicsQueueMutex());
                vkQueueWaitIdle(gpu.graphicsQueue());
            }
            localCmdPool.freeBuffer(cmd);
        }

        // Map staging buffer and copy to CPU
        void* mapped = nullptr;
        vmaMapMemory(gpu.allocator().handle(), readbackStaging.allocation(), &mapped);
        std::memcpy(pixelBuf.data(), mapped, imageSize);
        vmaUnmapMemory(gpu.allocator().handle(), readbackStaging.allocation());

        // Debug: check pixel content on first frame
        if (fi == 0) {
            size_t nz = 0;
            for (size_t pi = 0; pi < static_cast<size_t>(imageSize); ++pi)
                if (pixelBuf[pi] != 0) ++nz;
            spdlog::info("SpinePrerenderer: frame0 readback nonzero={}/{} ({:.1f}%)",
                         nz, imageSize, 100.0 * nz / imageSize);
        }

        // Chroma key: composite RGBA pixels over the solid background colour
        // before encoding.  This replaces the alpha channel with a flat
        // background that the user can key out in their editing software.
        if (useChromaKey) {
            constexpr float kInv255 = 1.0f / 255.0f;
            const float bgR = job.chromaKeyR * kInv255;
            const float bgG = job.chromaKeyG * kInv255;
            const float bgB = job.chromaKeyB * kInv255;
            const size_t pixelCount = static_cast<size_t>(renderW) * renderH;
            uint8_t* px = pixelBuf.data();
            for (size_t pi = 0; pi < pixelCount; ++pi) {
                uint8_t* p = px + pi * 4;
                float a = p[3] * kInv255;
                if (a >= 1.0f) continue;          // fully opaque — no blending needed
                if (a <= 0.0f) {
                    // fully transparent — set to background colour
                    p[0] = job.chromaKeyR;
                    p[1] = job.chromaKeyG;
                    p[2] = job.chromaKeyB;
                    p[3] = 255;
                    continue;
                }
                float invA = 1.0f - a;
                p[0] = static_cast<uint8_t>(static_cast<float>(p[0]) * a + bgR * invA * 255.0f + 0.5f);
                p[1] = static_cast<uint8_t>(static_cast<float>(p[1]) * a + bgG * invA * 255.0f + 0.5f);
                p[2] = static_cast<uint8_t>(static_cast<float>(p[2]) * a + bgB * invA * 255.0f + 0.5f);
                p[3] = 255;
            }
        }

        // Write frame to encoder
        if (!encodeFrame(pixelBuf.data())) {
            result.error = "Encode failed at frame " + std::to_string(fi)
                         + ": " + encoderError();
            spdlog::error("SpinePrerenderer: {}", result.error);
            finalizeEncoder();
            readbackStaging.destroy();
            spineRenderer.shutdown();
            localCmdPool.destroy();
            return result;
        }
    }

    // ── 8. Finalize ─────────────────────────────────────────────────────
    finalizeEncoder();
    readbackStaging.destroy();
    spineRenderer.shutdown();
    localCmdPool.destroy();

    // The actual output path depends on which encoder was used
    std::filesystem::path actualOutputPath;
    const char* encoderName = "unknown";
    switch (activeEnc) {
    case ActiveEncoder::ChromaKey:
        actualOutputPath = job.outputPath;
        actualOutputPath.replace_extension(".mp4");
        encoderName = "ChromaKey-H264";
        break;
    case ActiveEncoder::HEVC:
        actualOutputPath = job.outputPath;
        actualOutputPath.replace_extension(".mp4");
        encoderName = "HEVC-PackedAlpha";
        break;
    case ActiveEncoder::ProRes:
        actualOutputPath = job.outputPath;
        actualOutputPath.replace_extension(".mov");
        encoderName = "ProRes4444";
        break;
    }
    result.outputPath = actualOutputPath;

    // Get output file size
    std::error_code ec;
    result.fileSizeBytes = fs::file_size(actualOutputPath, ec);
    result.success = true;

    spdlog::info("SpinePrerenderer: completed '{}' / '{}' / '{}' \u2014 {} frames, {:.1f} KB [{}]",
                 job.characterName, job.outfit, job.animationName,
                 result.frameCount,
                 static_cast<double>(result.fileSizeBytes) / 1024.0,
                 encoderName);
    return result;
}

// ─── CPU rendering path (software triangle rasterizer fallback) ──────────────

PrerenderResult SpinePrerenderer::renderCPU(const PrerenderJob& job,
                                             PrerenderProgressFn progress)
{
    PrerenderResult result;
    result.outputPath = job.outputPath;

    // ── 1. Load skeleton ────────────────────────────────────────────────
    SpineEngine engine;
    auto paths = SpineEngine::resolvePaths(m_assetsDir, job.characterName,
                                            job.outfit, CharacterStance::Default);
    if (!paths.valid) {
        result.error = "Failed to resolve skeleton paths for " + job.characterName;
        spdlog::error("SpinePrerenderer: {}", result.error);
        return result;
    }

    if (!engine.loadSkeleton(paths.skelPath, paths.atlasPath)) {
        result.error = "Failed to load skeleton: " + paths.skelPath;
        spdlog::error("SpinePrerenderer: {}", result.error);
        return result;
    }

    // ── 2. Set animation and get duration ───────────────────────────────
    if (!engine.animation().hasAnimation(job.animationName)) {
        result.error = "Animation not found: " + job.animationName;
        spdlog::error("SpinePrerenderer: {}", result.error);
        return result;
    }

    engine.animation().setBodyAnimation(job.animationName, false);

    // If rendering a talking variant, start the talk track
    if (job.isTalking) {
        engine.animation().startTalking();
    }

    auto animInfos = engine.animation().listAnimations();
    float animDuration = 0.0f;
    for (const auto& ai : animInfos) {
        if (ai.name == job.animationName) {
            animDuration = ai.duration;
            break;
        }
    }

    if (animDuration <= 0.0f) {
        result.error = "Animation has zero duration: " + job.animationName;
        spdlog::warn("SpinePrerenderer: {}", result.error);
        return result;
    }

    result.duration = animDuration;
    result.frameCount = static_cast<int64_t>(std::ceil(animDuration * job.fps));
    if (result.frameCount < 1) result.frameCount = 1;

    // ── 3. Determine render dimensions from envelope bounds ─────────────
    // Sample bounds across the ENTIRE animation (same as GPU path).
    float envMinX =  1e9f, envMinY =  1e9f;
    float envMaxX = -1e9f, envMaxY = -1e9f;
    {
        const int samples = std::max(30, static_cast<int>(animDuration * 30.0f));
        for (int s = 0; s <= samples; ++s) {
            float t = (animDuration > 0.0f)
                ? (static_cast<float>(s) / static_cast<float>(samples)) * animDuration
                : 0.0f;
            engine.evaluateAtTime(t, job.isTalking ? t : 0.0f);
            float bx2, by2, bw2, bh2;
            engine.getBounds(bx2, by2, bw2, bh2);
            if (bx2 < envMinX)        envMinX = bx2;
            if (by2 < envMinY)        envMinY = by2;
            if (bx2 + bw2 > envMaxX)  envMaxX = bx2 + bw2;
            if (by2 + bh2 > envMaxY)  envMaxY = by2 + bh2;
        }
    }
    float bx = envMinX;
    float by = envMinY;
    float bw = envMaxX - envMinX;
    float bh = envMaxY - envMinY;
    if (bw < 1.0f || bh < 1.0f) {
        result.error = "Degenerate skeleton bounds";
        return result;
    }

    uint32_t renderW = static_cast<uint32_t>(std::ceil(bw * (1.0f / job.paddingFactor)));
    uint32_t renderH = static_cast<uint32_t>(std::ceil(bh * (1.0f / job.paddingFactor)));
    renderW = (renderW + 1) & ~1u;
    renderH = (renderH + 1) & ~1u;
    renderW = std::min(renderW, 4096u);
    renderH = std::min(renderH, 4096u);

    result.width  = renderW;
    result.height = renderH;

    // ── 4. Load atlas textures for CPU rasterization ────────────────────
    const auto& atlas = engine.atlas();
    const auto& atlasPages = atlas.pages();
    const std::string& atlasDir = atlas.directory();

    std::vector<std::vector<uint8_t>> pagePixels;
    std::vector<int> pageWidths, pageHeights;

    for (size_t pi = 0; pi < atlasPages.size(); ++pi) {
        const auto& page = atlasPages[pi];
        std::string texPath = atlasDir + "/" + page.texturePath;
        int pw = 0, ph = 0, ch = 0;
        uint8_t* rawPx = stbi_load(texPath.c_str(), &pw, &ph, &ch, 4);
        if (rawPx && pw > 0 && ph > 0) {
            const int total = pw * ph;
            std::vector<uint8_t> px(rawPx, rawPx + total * 4);
            stbi_image_free(rawPx);

            // Un-premultiply alpha for CPU blending
            if (page.pma) {
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
            pagePixels.push_back(std::move(px));
            pageWidths.push_back(pw);
            pageHeights.push_back(ph);
        } else {
            if (rawPx) stbi_image_free(rawPx);
            pagePixels.emplace_back();
            pageWidths.push_back(0);
            pageHeights.push_back(0);
        }
    }

    // ── 5. Open encoder ───────────────────────────────────────────────
    bool cpuUseChromaKey = (job.format == PrerenderFormat::GreenScreen ||
                            job.format == PrerenderFormat::BlueScreen ||
                            job.format == PrerenderFormat::CustomColor);

    ChromaKeyEncoder    cpuChromaEncoder;
    ProResAlphaEncoder  proresEncoder;
    bool cpuUseProRes = false;

    {
        bool opened = false;

        // Chroma key formats on CPU
        if (!opened && cpuUseChromaKey) {
            auto ckOutputPath = job.outputPath;
            ckOutputPath.replace_extension(".mp4");
            if (cpuChromaEncoder.open(ckOutputPath, renderW, renderH, job.fps, job.crf)) {
                opened = true;
                spdlog::info("SpinePrerenderer (CPU): using chroma-key H.264 encoder");
            }
        }

        // ProRes 4444 fallback
        if (!opened) {
            auto proresOutputPath = job.outputPath;
            proresOutputPath.replace_extension(".mov");
            if (proresEncoder.open(proresOutputPath, renderW, renderH, job.fps, 4)) {
                cpuUseProRes = true;
                opened = true;
                spdlog::info("SpinePrerenderer (CPU): using ProRes 4444 encoder");
            }
        }

        if (!opened) {
            result.error = "Failed to open any encoder (ChromaKey + ProRes both failed)";
            return result;
        }
    }

    auto encodeFrame = [&](const uint8_t* pixels) -> bool {
        if (cpuUseProRes) return proresEncoder.writeFrame(pixels);
        return cpuChromaEncoder.writeFrame(pixels);
    };
    auto finalizeEncoder = [&]() {
        if (cpuUseProRes) proresEncoder.finalize();
        else cpuChromaEncoder.finalize();
    };

    // ── 6. Render loop (software rasterizer) ────────────────────────────
    const float fW = static_cast<float>(renderW);
    const float fH = static_cast<float>(renderH);
    const float spineScale = std::min(fW / bw, fH / bh) * job.paddingFactor;
    const float offsetX = fW * 0.5f;
    const float offsetY = fH * 0.5f;
    const float spineCX = bx + bw * 0.5f;
    const float spineCY = by + bh * 0.5f;

    auto spineToPixel = [&](float sx, float sy, float& px, float& py) {
        px = (sx - spineCX) * spineScale + offsetX;
        py = -(sy - spineCY) * spineScale + offsetY;
    };

    std::vector<uint8_t> frameBuf(static_cast<size_t>(renderW) * renderH * 4);

    for (int64_t fi = 0; fi < result.frameCount; ++fi) {
        if (progress && !progress(fi, result.frameCount)) {
            result.error = "Cancelled";
            finalizeEncoder();
            return result;
        }

        // Clear frame to transparent
        std::memset(frameBuf.data(), 0, frameBuf.size());

        float t = static_cast<float>(fi) / static_cast<float>(job.fps);
        engine.evaluateAtTime(t, job.isTalking ? t : 0.0f);

        SpineRenderData renderData = engine.extractMeshes();

        // Software triangle rasterization (same as TimelineWorkspace::renderSpineClip)
        for (const auto& batch : renderData.batches) {
            if (batch.texturePageIndex < 0 ||
                batch.texturePageIndex >= static_cast<int>(pagePixels.size()))
                continue;

            const auto& texPx = pagePixels[batch.texturePageIndex];
            if (texPx.empty()) continue;
            const int texW = pageWidths[batch.texturePageIndex];
            const int texH = pageHeights[batch.texturePageIndex];
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
                int maxX = std::min(static_cast<int>(renderW) - 1,
                                    static_cast<int>(std::ceil(std::max({px0, px1, px2}))));
                int minY = std::max(0, static_cast<int>(std::floor(std::min({py0, py1, py2}))));
                int maxY = std::min(static_cast<int>(renderH) - 1,
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

                        int tx2 = std::clamp(static_cast<int>(u * texW), 0, texW - 1);
                        int ty2 = std::clamp(static_cast<int>(v * texH), 0, texH - 1);
                        const uint8_t* texel = texPx.data() + (ty2 * texW + tx2) * 4;

                        float sr = texel[0] / 255.0f * cr;
                        float sg = texel[1] / 255.0f * cg;
                        float sb = texel[2] / 255.0f * cb;
                        float sa = texel[3] / 255.0f * ca;
                        if (sa < 0.001f) continue;

                        uint8_t* dp = frameBuf.data() + (y * renderW + x) * 4;
                        float da = dp[3] / 255.0f;
                        float outA = sa + da * (1.0f - sa);
                        if (outA > 0.001f) {
                            dp[0] = static_cast<uint8_t>(std::clamp((sr * sa + (dp[0] / 255.0f) * da * (1.0f - sa)) / outA * 255.0f, 0.0f, 255.0f));
                            dp[1] = static_cast<uint8_t>(std::clamp((sg * sa + (dp[1] / 255.0f) * da * (1.0f - sa)) / outA * 255.0f, 0.0f, 255.0f));
                            dp[2] = static_cast<uint8_t>(std::clamp((sb * sa + (dp[2] / 255.0f) * da * (1.0f - sa)) / outA * 255.0f, 0.0f, 255.0f));
                            dp[3] = static_cast<uint8_t>(outA * 255.0f);
                        }
                    }
                }
            }
        }

        // Chroma key: composite RGBA pixels over the solid background colour
        if (cpuUseChromaKey) {
            constexpr float kInv255 = 1.0f / 255.0f;
            const float bgR = job.chromaKeyR * kInv255;
            const float bgG = job.chromaKeyG * kInv255;
            const float bgB = job.chromaKeyB * kInv255;
            const size_t pixelCount = static_cast<size_t>(renderW) * renderH;
            uint8_t* px = frameBuf.data();
            for (size_t pi = 0; pi < pixelCount; ++pi) {
                uint8_t* p = px + pi * 4;
                float a = p[3] * kInv255;
                if (a >= 1.0f) continue;
                if (a <= 0.0f) {
                    p[0] = job.chromaKeyR;
                    p[1] = job.chromaKeyG;
                    p[2] = job.chromaKeyB;
                    p[3] = 255;
                    continue;
                }
                float invA = 1.0f - a;
                p[0] = static_cast<uint8_t>(static_cast<float>(p[0]) * a + bgR * invA * 255.0f + 0.5f);
                p[1] = static_cast<uint8_t>(static_cast<float>(p[1]) * a + bgG * invA * 255.0f + 0.5f);
                p[2] = static_cast<uint8_t>(static_cast<float>(p[2]) * a + bgB * invA * 255.0f + 0.5f);
                p[3] = 255;
            }
        }

        // Write frame
        if (!encodeFrame(frameBuf.data())) {
            result.error = "Encode failed at frame " + std::to_string(fi);
            finalizeEncoder();
            return result;
        }
    }

    finalizeEncoder();

    std::filesystem::path actualOutputPathCPU;
    const char* encoderNameCPU = "unknown";
    if (cpuUseChromaKey) {
        actualOutputPathCPU = job.outputPath;
        actualOutputPathCPU.replace_extension(".mp4");
        encoderNameCPU = "ChromaKey-H264";
    } else {
        actualOutputPathCPU = job.outputPath;
        actualOutputPathCPU.replace_extension(".mov");
        encoderNameCPU = "ProRes4444";
    }
    result.outputPath = actualOutputPathCPU;

    std::error_code ec;
    result.fileSizeBytes = fs::file_size(actualOutputPathCPU, ec);
    result.success = true;

    spdlog::info("SpinePrerenderer (CPU): completed '{}' / '{}' / '{}' \u2014 {} frames, {:.1f} KB [{}]",
                 job.characterName, job.outfit, job.animationName,
                 result.frameCount,
                 static_cast<double>(result.fileSizeBytes) / 1024.0,
                 encoderNameCPU);
    return result;
}

} // namespace rt

#endif // ROUNDTABLE_HAS_SPINE
