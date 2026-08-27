/*
 * test_compositor.cpp — Tests for Step 10: GPU Compositor & Transitions
 *
 * Tests the Compositor and TransitionRenderer:
 *   1. Structural tests (no GPU): verify data structures, blend modes,
 *      transform helpers, push constants, and configuration.
 *   2. GPU tests (require Vulkan): full init → compose → readback → validate.
 *
 * All GPU tests use offscreen compute — no window/swapchain needed.
 */

#include <gtest/gtest.h>

#include <volk.h>
#include "Compositor.h"
#include "CompositeServiceLayerBuild.h"
#include "EffectProcessor.h"
#include "GpuContext.h"
#include "TemporalInterpolator.h"
#include "TransitionRenderer.h"
#include "vulkan/Instance.h"
#include "vulkan/Device.h"
#include "vulkan/Allocator.h"
#include "vulkan/CommandPool.h"
#include "timeline/AdjustmentClip.h"
#include "effects/Blur.h"
#include "effects/ColorCorrect.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/epsilon.hpp>

#include <cstring>
#include <mutex>
#include <vector>
#include <cmath>
#include <algorithm>
#include <numeric>

// ─── Vulkan context for GPU tests ───────────────────────────────────────────

struct TestVulkanContext
{
    rt::Instance    instance;
    rt::Device      device;
    rt::Allocator   allocator;
    rt::CommandPool cmdPool;
    std::mutex      graphicsQueueMutex;
    std::mutex      computeQueueMutex;
    std::mutex      transferQueueMutex;
    bool            valid{false};

    bool init()
    {
        if (volkInitialize() != VK_SUCCESS)
            return false;

        rt::InstanceConfig cfg;
        cfg.appName = "test_compositor";
        cfg.enableValidation = false;
        if (!instance.create(cfg))
            return false;

        volkLoadInstance(instance.handle());

        if (!device.create(instance))
            return false;

        volkLoadDevice(device.handle());

        if (!allocator.create(instance, device))
            return false;

        if (!cmdPool.create(device.handle(),
                            device.queueFamilies().graphics.value()))
            return false;

        // CommandPool::endSingleTime routes submissions through the global
        // scheduler. Bind this fixture's device/queues so synchronous GPU
        // tests do not hang at their first submit.
        if (!rt::GpuContext::get().scheduler().init(
                device.handle(),
                device.graphicsQueue(), &graphicsQueueMutex,
                device.computeQueue(), &computeQueueMutex,
                device.transferQueue(), &transferQueueMutex))
            return false;

        valid = true;
        return true;
    }

    void shutdown()
    {
        if (!valid) return;
        cmdPool.destroy();
        rt::GpuContext::get().scheduler().shutdown();
        allocator.destroy();
        device.destroy();
        instance.destroy();
        valid = false;
    }
};

static TestVulkanContext* g_vk = nullptr;

// ═════════════════════════════════════════════════════════════════════════════
//  TEST FIXTURE
// ═════════════════════════════════════════════════════════════════════════════

class CompositorTest : public ::testing::Test {
protected:
    static void SetUpTestSuite()
    {
        static TestVulkanContext ctx;
        if (ctx.init()) {
            g_vk = &ctx;
        }
    }

    static void TearDownTestSuite()
    {
        if (g_vk) {
            g_vk->shutdown();
            g_vk = nullptr;
        }
    }

    bool hasGPU() const { return g_vk != nullptr && g_vk->valid; }
};

// ═════════════════════════════════════════════════════════════════════════════
//  STRUCTURAL TESTS (no GPU required)
// ═════════════════════════════════════════════════════════════════════════════

// ── BlendMode enum ──────────────────────────────────────────────────────────

TEST_F(CompositorTest, BlendModeValues)
{
    // Must match composite.comp shader constants
    EXPECT_EQ(static_cast<int>(rt::BlendMode::Normal),   0);
    EXPECT_EQ(static_cast<int>(rt::BlendMode::Multiply), 1);
    EXPECT_EQ(static_cast<int>(rt::BlendMode::Screen),   2);
    EXPECT_EQ(static_cast<int>(rt::BlendMode::Add),      3);
}

// ── TransitionType enum ─────────────────────────────────────────────────────

TEST_F(CompositorTest, TransitionTypeValues)
{
    EXPECT_EQ(static_cast<int>(rt::GpuTransitionType::Dissolve),  0);
    EXPECT_EQ(static_cast<int>(rt::GpuTransitionType::FadeBlack), 1);
    EXPECT_EQ(static_cast<int>(rt::GpuTransitionType::WipeLeft),  2);
    EXPECT_EQ(static_cast<int>(rt::GpuTransitionType::WipeRight), 3);
    EXPECT_EQ(static_cast<int>(rt::GpuTransitionType::WipeUp),    4);
    EXPECT_EQ(static_cast<int>(rt::GpuTransitionType::WipeDown),  5);
}

// ── Constants ───────────────────────────────────────────────────────────────

TEST_F(CompositorTest, MaxLayersConstant)
{
    EXPECT_EQ(rt::kMaxCompositorLayers, 32u);
}

TEST_F(CompositorTest, WorkgroupSizeConstant)
{
    EXPECT_EQ(rt::kCompositeWorkgroupSize, 16u);
}

// ── LayerParamsGPU layout ───────────────────────────────────────────────────

TEST_F(CompositorTest, LayerParamsGPU_Alignment)
{
    // Must be 16-byte aligned for std430
    EXPECT_EQ(alignof(rt::LayerParamsGPU), 16u);
}

TEST_F(CompositorTest, LayerParamsGPU_DefaultValues)
{
    rt::LayerParamsGPU params{};
    EXPECT_EQ(params.layerCount, 0);

    for (uint32_t i = 0; i < rt::kMaxCompositorLayers; ++i)
    {
        EXPECT_FLOAT_EQ(params.opacity[i], 0.0f);
        EXPECT_EQ(params.motionSampleCount[i], 0);
        EXPECT_EQ(params.blendMode[i], 0);
        EXPECT_EQ(params.enabled[i], 0);
    }
}

// ── CompositorLayer defaults ────────────────────────────────────────────────

TEST_F(CompositorTest, CompositorLayer_Defaults)
{
    rt::CompositorLayer layer;
    EXPECT_FLOAT_EQ(layer.opacity, 1.0f);
    EXPECT_EQ(layer.blendMode, rt::BlendMode::Normal);
    EXPECT_TRUE(layer.enabled);
    EXPECT_EQ(layer.transform, glm::mat4(1.0f));
    EXPECT_EQ(layer.motionTransformStart, glm::mat4(1.0f));
    EXPECT_EQ(layer.motionTransformEnd, glm::mat4(1.0f));
    EXPECT_EQ(layer.motionSampleCount, 1);
}

// ── CompositorConfig defaults ───────────────────────────────────────────────

TEST_F(CompositorTest, CompositorConfig_Defaults)
{
    rt::CompositorConfig cfg;
    EXPECT_EQ(cfg.outputWidth, 1920u);
    EXPECT_EQ(cfg.outputHeight, 1080u);
    EXPECT_EQ(cfg.outputFormat, VK_FORMAT_R8G8B8A8_UNORM);
}

// ── TransitionConfig defaults ───────────────────────────────────────────────

TEST_F(CompositorTest, TransitionConfig_Defaults)
{
    rt::TransitionConfig cfg;
    EXPECT_EQ(cfg.outputWidth, 1920u);
    EXPECT_EQ(cfg.outputHeight, 1080u);
    EXPECT_EQ(cfg.outputFormat, VK_FORMAT_R8G8B8A8_UNORM);
    EXPECT_FLOAT_EQ(cfg.wipeSoftness, 0.02f);
}

// ── Push constants size ─────────────────────────────────────────────────────

TEST_F(CompositorTest, TransitionPushConstants_Size)
{
    // Must be 32 bytes (matching shader layout)
    EXPECT_EQ(sizeof(rt::TransitionPushConstants), 32u);
}

TEST_F(CompositorTest, CompositePushConstants_Size)
{
    // Includes the internal-RGBA/final-BGRA output switch and shader padding.
    EXPECT_EQ(sizeof(rt::CompositePushConstants), 32u);
}

TEST_F(CompositorTest, IntermediateOutputEncodingCanBeSelected)
{
    rt::Compositor compositor;
    EXPECT_TRUE(compositor.outputSwizzleRB());
    EXPECT_FALSE(compositor.preserveAlpha());

    compositor.setOutputSwizzleRB(false);
    compositor.setPreserveAlpha(true);
    EXPECT_FALSE(compositor.outputSwizzleRB());
    EXPECT_TRUE(compositor.preserveAlpha());
}

TEST_F(CompositorTest, AdjustmentStackIsScheduledOnceAtBoundary)
{
    std::vector<rt::LayerInfo> layers(2); // two ordinary layers below it
    layers[0].clipId = 10;
    layers[1].clipId = 20;

    rt::AdjustmentClip adjustment;
    adjustment.effects().addEffect(std::make_unique<rt::ColorCorrect>());
    adjustment.effects().addEffect(std::make_unique<rt::Blur>());

    rt::appendAdjustmentLayerBoundary(layers, adjustment, 0);

    ASSERT_EQ(layers.size(), 3u);
    EXPECT_TRUE(layers[2].isAdjustmentLayer);
    EXPECT_EQ(layers[2].effects.size(), 2u);
    EXPECT_FLOAT_EQ(layers[2].opacity, 1.0f);
    EXPECT_TRUE(layers[0].effects.empty());
    EXPECT_TRUE(layers[1].effects.empty());
}

TEST_F(CompositorTest, AdjustmentCrossDissolveEvaluatesEffectStrength)
{
    rt::AdjustmentClip adjustment;
    adjustment.opacity().setDefaultValue(0.8f);

    rt::Transition fadeIn;
    fadeIn.type = rt::TransitionType::CrossDissolve;
    fadeIn.duration = 100;
    fadeIn.editPointTick = 1000;
    fadeIn.leftClipId = 0;
    fadeIn.rightClipId = adjustment.id();

    const std::vector<rt::Transition> incoming{fadeIn};
    EXPECT_FLOAT_EQ(rt::adjustmentLayerStrengthAtTick(
                        adjustment, 0, 1000, incoming),
                    0.0f);
    EXPECT_NEAR(rt::adjustmentLayerStrengthAtTick(
                    adjustment, 50, 1050, incoming),
                0.4f, 0.0001f);
    EXPECT_NEAR(rt::adjustmentLayerStrengthAtTick(
                    adjustment, 100, 1100, incoming),
                0.8f, 0.0001f);

    rt::Transition fadeOut;
    fadeOut.type = rt::TransitionType::CrossDissolve;
    fadeOut.duration = 100;
    fadeOut.editPointTick = 2000;
    fadeOut.leftClipId = adjustment.id();
    fadeOut.rightClipId = 0;

    const std::vector<rt::Transition> outgoing{fadeOut};
    EXPECT_NEAR(rt::adjustmentLayerStrengthAtTick(
                    adjustment, 900, 1900, outgoing),
                0.8f, 0.0001f);
    EXPECT_NEAR(rt::adjustmentLayerStrengthAtTick(
                    adjustment, 950, 1950, outgoing),
                0.4f, 0.0001f);
    EXPECT_FLOAT_EQ(rt::adjustmentLayerStrengthAtTick(
                        adjustment, 1000, 2000, outgoing),
                    0.0f);
}

// ── CompositorStats defaults ────────────────────────────────────────────────

TEST_F(CompositorTest, CompositorStats_Defaults)
{
    rt::CompositorStats stats;
    EXPECT_EQ(stats.layerCount, 0u);
    EXPECT_EQ(stats.enabledLayers, 0u);
    EXPECT_FLOAT_EQ(stats.gpuTimeMs, 0.0f);
    EXPECT_EQ(stats.outputWidth, 0u);
    EXPECT_EQ(stats.outputHeight, 0u);
}

// ── TransitionStats defaults ────────────────────────────────────────────────

TEST_F(CompositorTest, TransitionStats_Defaults)
{
    rt::TransitionStats stats;
    EXPECT_EQ(stats.type, rt::GpuTransitionType::Dissolve);
    EXPECT_FLOAT_EQ(stats.progress, 0.0f);
    EXPECT_FLOAT_EQ(stats.gpuTimeMs, 0.0f);
}

// ── Default construction ────────────────────────────────────────────────────

TEST_F(CompositorTest, Compositor_DefaultConstruction)
{
    rt::Compositor compositor;
    EXPECT_FALSE(compositor.isInitialized());
    EXPECT_EQ(compositor.layerCount(), 0u);
}

TEST_F(CompositorTest, TransitionRenderer_DefaultConstruction)
{
    rt::TransitionRenderer transition;
    EXPECT_FALSE(transition.isInitialized());
}

// ── Transform helpers ───────────────────────────────────────────────────────

TEST_F(CompositorTest, IdentityTransform)
{
    auto m = rt::Compositor::identityTransform();
    EXPECT_EQ(m, glm::mat4(1.0f));
}

TEST_F(CompositorTest, BuildLayerTransform_Identity)
{
    // Full-screen layer at origin with scale 1
    auto m = rt::Compositor::buildLayerTransform(0.0f, 0.0f, 1.0f, 1.0f);

    // Apply to origin UV — should map to (0,0)
    glm::vec4 origin = m * glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
    EXPECT_NEAR(origin.x / origin.w, 0.0f, 1e-5f);
    EXPECT_NEAR(origin.y / origin.w, 0.0f, 1e-5f);

    // Apply to (1,1) — should map to (1,1)
    glm::vec4 corner = m * glm::vec4(1.0f, 1.0f, 0.0f, 1.0f);
    EXPECT_NEAR(corner.x / corner.w, 1.0f, 1e-5f);
    EXPECT_NEAR(corner.y / corner.w, 1.0f, 1e-5f);
}

TEST_F(CompositorTest, BuildLayerTransform_HalfSize)
{
    // Layer covers half the screen at origin
    auto m = rt::Compositor::buildLayerTransform(0.0f, 0.0f, 0.5f, 0.5f);

    // The transform maps output UV to layer UV.
    // A 0.5-scale layer at (0,0) means: output UV (0,0) → layer UV (0,0)
    // And output UV (0.5, 0.5) → layer UV (1,1) (edge of layer)
    glm::vec4 edge = m * glm::vec4(0.5f, 0.5f, 0.0f, 1.0f);
    EXPECT_NEAR(edge.x / edge.w, 1.0f, 1e-5f);
    EXPECT_NEAR(edge.y / edge.w, 1.0f, 1e-5f);
}

TEST_F(CompositorTest, BuildLayerTransform_Offset)
{
    // Layer at (0.25, 0.25) with scale 0.5
    auto m = rt::Compositor::buildLayerTransform(0.25f, 0.25f, 0.5f, 0.5f);

    // Output UV (0.25, 0.25) should map to layer UV (0, 0)
    glm::vec4 start = m * glm::vec4(0.25f, 0.25f, 0.0f, 1.0f);
    EXPECT_NEAR(start.x / start.w, 0.0f, 1e-5f);
    EXPECT_NEAR(start.y / start.w, 0.0f, 1e-5f);

    // Output UV (0.75, 0.75) should map to layer UV (1, 1)
    glm::vec4 end = m * glm::vec4(0.75f, 0.75f, 0.0f, 1.0f);
    EXPECT_NEAR(end.x / end.w, 1.0f, 1e-5f);
    EXPECT_NEAR(end.y / end.w, 1.0f, 1e-5f);
}

// ═════════════════════════════════════════════════════════════════════════════
//  GPU TESTS (require Vulkan device)
// ═════════════════════════════════════════════════════════════════════════════

// ── Helper: create a solid-color texture ────────────────────────────────────

static rt::Texture createSolidTexture(uint32_t width, uint32_t height,
                                       uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    rt::Texture tex;
    std::vector<uint8_t> pixels(width * height * 4);
    for (size_t i = 0; i < width * height; ++i)
    {
        pixels[i * 4 + 0] = r;
        pixels[i * 4 + 1] = g;
        pixels[i * 4 + 2] = b;
        pixels[i * 4 + 3] = a;
    }

    rt::TextureConfig cfg;
    cfg.width  = width;
    cfg.height = height;
    cfg.format = VK_FORMAT_R8G8B8A8_UNORM;
    cfg.usage  = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

    tex.createFromData(g_vk->allocator.handle(), g_vk->device.handle(),
                       cfg, pixels.data(), pixels.size(),
                       g_vk->cmdPool, g_vk->device.graphicsQueue());
    return tex;
}

static rt::Texture createLeftHalfMaskTexture(uint32_t width, uint32_t height)
{
    rt::Texture tex;
    std::vector<uint8_t> pixels(width * height * 4);
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            const uint8_t value = x < width / 2 ? 255 : 0;
            const size_t i = (static_cast<size_t>(y) * width + x) * 4;
            pixels[i + 0] = value;
            pixels[i + 1] = value;
            pixels[i + 2] = value;
            pixels[i + 3] = value;
        }
    }

    rt::TextureConfig cfg;
    cfg.width = width;
    cfg.height = height;
    cfg.format = VK_FORMAT_R8G8B8A8_UNORM;
    cfg.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

    tex.createFromData(g_vk->allocator.handle(), g_vk->device.handle(),
                       cfg, pixels.data(), pixels.size(),
                       g_vk->cmdPool, g_vk->device.graphicsQueue());
    return tex;
}

static rt::Texture createRgbaTexture(uint32_t width, uint32_t height,
                                     const std::vector<uint8_t>& pixels)
{
    rt::Texture tex;
    if (pixels.size() != static_cast<size_t>(width) * height * 4)
        return tex;

    rt::TextureConfig cfg;
    cfg.width = width;
    cfg.height = height;
    cfg.format = VK_FORMAT_R8G8B8A8_UNORM;
    cfg.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    tex.createFromData(g_vk->allocator.handle(), g_vk->device.handle(),
                       cfg, pixels.data(), pixels.size(),
                       g_vk->cmdPool, g_vk->device.graphicsQueue());
    return tex;
}

static bool renderTemporalPixels(rt::TemporalInterpolator& interpolator,
                                 rt::Compositor& compositor,
                                 const rt::Texture& sourceA,
                                 const rt::Texture& sourceB,
                                 uint32_t width, uint32_t height,
                                 float phase, int32_t mode,
                                 uint32_t layerIndex,
                                 std::vector<uint8_t>& pixels)
{
    rt::Texture output;
    rt::TextureConfig outputCfg;
    outputCfg.width = width;
    outputCfg.height = height;
    outputCfg.format = VK_FORMAT_R8G8B8A8_UNORM;
    outputCfg.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    if (!output.create(g_vk->allocator.handle(), g_vk->device.handle(),
                       outputCfg)) {
        return false;
    }

    VkCommandBuffer cmd = g_vk->cmdPool.beginSingleTime();
    if (cmd == VK_NULL_HANDLE) return false;
    output.transitionLayout(cmd, VK_IMAGE_LAYOUT_UNDEFINED,
                            VK_IMAGE_LAYOUT_GENERAL);
    const bool recorded = interpolator.render(
        cmd, output.imageView(), sourceA.descriptorInfo(),
        sourceB.descriptorInfo(), width, height, phase, mode,
        false, false, 0, layerIndex);
    output.transitionLayout(cmd, VK_IMAGE_LAYOUT_GENERAL,
                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    g_vk->cmdPool.endSingleTime(cmd, g_vk->device.graphicsQueue());
    if (!recorded) return false;

    std::vector<rt::CompositorLayer> layers(1);
    layers[0].textureInfo = output.descriptorInfo();
    layers[0].transform = rt::Compositor::identityTransform();
    compositor.setLayers(layers);
    const bool composited = compositor.compositeSync();
    const bool readBack = composited && compositor.readbackOutput(pixels);
    output.destroy();
    return readBack;
}

// ── Compositor init/shutdown ────────────────────────────────────────────────

TEST_F(CompositorTest, GPU_Compositor_InitShutdown)
{
    if (!hasGPU()) GTEST_SKIP() << "No Vulkan device";

    rt::Compositor compositor;
    rt::CompositorConfig cfg;
    cfg.outputWidth  = 64;
    cfg.outputHeight = 64;

    ASSERT_TRUE(compositor.init(g_vk->device, g_vk->allocator, g_vk->cmdPool,
                                g_vk->device.graphicsQueue(), cfg));
    EXPECT_TRUE(compositor.isInitialized());
    EXPECT_EQ(compositor.outputWidth(), 64u);
    EXPECT_EQ(compositor.outputHeight(), 64u);

    compositor.shutdown();
    EXPECT_FALSE(compositor.isInitialized());
}

TEST_F(CompositorTest, GPU_Compositor_DoubleInit)
{
    if (!hasGPU()) GTEST_SKIP() << "No Vulkan device";

    rt::Compositor compositor;
    rt::CompositorConfig cfg;
    cfg.outputWidth  = 32;
    cfg.outputHeight = 32;

    ASSERT_TRUE(compositor.init(g_vk->device, g_vk->allocator, g_vk->cmdPool,
                                g_vk->device.graphicsQueue(), cfg));

    // Second init should succeed (returns true, already initialized)
    EXPECT_TRUE(compositor.init(g_vk->device, g_vk->allocator, g_vk->cmdPool,
                                g_vk->device.graphicsQueue(), cfg));

    compositor.shutdown();
}

// ── Compositor: empty composite ─────────────────────────────────────────────

TEST_F(CompositorTest, GPU_Compositor_EmptyComposite)
{
    if (!hasGPU()) GTEST_SKIP() << "No Vulkan device";

    rt::Compositor compositor;
    rt::CompositorConfig cfg;
    cfg.outputWidth  = 16;
    cfg.outputHeight = 16;

    ASSERT_TRUE(compositor.init(g_vk->device, g_vk->allocator, g_vk->cmdPool,
                                g_vk->device.graphicsQueue(), cfg));

    // Composite with no layers
    ASSERT_TRUE(compositor.compositeSync());

    // Read back — should be transparent black
    std::vector<uint8_t> pixels;
    ASSERT_TRUE(compositor.readbackOutput(pixels));
    EXPECT_EQ(pixels.size(), 16u * 16u * 4u);

    // All pixels should be (0,0,0,0)
    bool allBlack = true;
    for (size_t i = 0; i < pixels.size(); ++i)
    {
        if (pixels[i] != 0) { allBlack = false; break; }
    }
    EXPECT_TRUE(allBlack) << "Empty composite should produce transparent black";

    compositor.shutdown();
}

TEST_F(CompositorTest, GPU_Compositor_RepeatedReadback)
{
    if (!hasGPU()) GTEST_SKIP() << "No Vulkan device";

    rt::Compositor compositor;
    rt::CompositorConfig cfg;
    cfg.outputWidth  = 16;
    cfg.outputHeight = 16;

    ASSERT_TRUE(compositor.init(g_vk->device, g_vk->allocator, g_vk->cmdPool,
                                g_vk->device.graphicsQueue(), cfg));

    auto tex = createSolidTexture(16, 16, 255, 0, 0, 255);

    std::vector<rt::CompositorLayer> layers(1);
    layers[0].textureInfo = tex.descriptorInfo();
    layers[0].transform   = rt::Compositor::identityTransform();
    layers[0].opacity     = 1.0f;
    layers[0].blendMode   = rt::BlendMode::Normal;
    layers[0].enabled     = true;

    compositor.setLayers(layers);
    ASSERT_TRUE(compositor.compositeSync());

    std::vector<uint8_t> firstReadback;
    std::vector<uint8_t> secondReadback;
    ASSERT_TRUE(compositor.readbackOutput(firstReadback));
    ASSERT_TRUE(compositor.readbackOutput(secondReadback));

    EXPECT_EQ(firstReadback.size(), 16u * 16u * 4u);
    EXPECT_EQ(secondReadback.size(), firstReadback.size());
    EXPECT_EQ(secondReadback, firstReadback);

    tex.destroy();
    compositor.shutdown();
}

// ── Compositor: single solid layer ──────────────────────────────────────────

TEST_F(CompositorTest, GPU_Compositor_SingleSolidLayer)
{
    if (!hasGPU()) GTEST_SKIP() << "No Vulkan device";

    rt::Compositor compositor;
    rt::CompositorConfig cfg;
    cfg.outputWidth  = 16;
    cfg.outputHeight = 16;

    ASSERT_TRUE(compositor.init(g_vk->device, g_vk->allocator, g_vk->cmdPool,
                                g_vk->device.graphicsQueue(), cfg));

    // Create a solid red texture
    auto redTex = createSolidTexture(16, 16, 255, 0, 0, 255);

    // Set one layer
    std::vector<rt::CompositorLayer> layers(1);
    layers[0].textureInfo = redTex.descriptorInfo();
    layers[0].transform   = rt::Compositor::identityTransform();
    layers[0].opacity     = 1.0f;
    layers[0].blendMode   = rt::BlendMode::Normal;
    layers[0].enabled     = true;

    compositor.setLayers(layers);
    EXPECT_EQ(compositor.layerCount(), 1u);

    ASSERT_TRUE(compositor.compositeSync());

    // Read back
    std::vector<uint8_t> pixels;
    ASSERT_TRUE(compositor.readbackOutput(pixels));

    // Check center pixel is red
    size_t centerIdx = (8 * 16 + 8) * 4;
    EXPECT_GE(pixels[centerIdx + 0], 250u);  // R
    EXPECT_LE(pixels[centerIdx + 1], 5u);    // G
    EXPECT_LE(pixels[centerIdx + 2], 5u);    // B
    EXPECT_GE(pixels[centerIdx + 3], 250u);  // A

    redTex.destroy();
    compositor.shutdown();
}

// ── Compositor: clip-local mask follows transform ──────────────────────────

TEST_F(CompositorTest, GPU_Compositor_TransformMotionBlurCreatesTemporalTrail)
{
    if (!hasGPU()) GTEST_SKIP() << "No Vulkan device";

    constexpr uint32_t kWidth = 32;
    constexpr uint32_t kHeight = 16;
    rt::Compositor compositor;
    rt::CompositorConfig cfg;
    cfg.outputWidth = kWidth;
    cfg.outputHeight = kHeight;
    ASSERT_TRUE(compositor.init(g_vk->device, g_vk->allocator, g_vk->cmdPool,
                                g_vk->device.graphicsQueue(), cfg));

    // Four-pixel opaque white stripe on transparent black.
    std::vector<uint8_t> source(kWidth * kHeight * 4, 0);
    for (uint32_t y = 0; y < kHeight; ++y) {
        for (uint32_t x = 14; x <= 17; ++x) {
            const size_t i = (static_cast<size_t>(y) * kWidth + x) * 4;
            source[i + 0] = 255;
            source[i + 1] = 255;
            source[i + 2] = 255;
            source[i + 3] = 255;
        }
    }
    auto stripe = createRgbaTexture(kWidth, kHeight, source);

    std::vector<rt::CompositorLayer> layers(1);
    layers[0].textureInfo = stripe.descriptorInfo();
    layers[0].transform = rt::Compositor::identityTransform();

    compositor.setLayers(layers);
    ASSERT_TRUE(compositor.compositeSync());
    std::vector<uint8_t> sharp;
    ASSERT_TRUE(compositor.readbackOutput(sharp));

    layers[0].motionTransformStart = rt::Compositor::buildLayerTransform(
        -0.25f, 0.0f, 1.0f, 1.0f);
    layers[0].motionTransformEnd = rt::Compositor::buildLayerTransform(
         0.25f, 0.0f, 1.0f, 1.0f);
    layers[0].motionSampleCount = 8;
    compositor.setLayers(layers);
    ASSERT_TRUE(compositor.compositeSync());
    std::vector<uint8_t> blurred;
    ASSERT_TRUE(compositor.readbackOutput(blurred));

    const auto valueAt = [](const std::vector<uint8_t>& pixels, uint32_t x) {
        const size_t i = (static_cast<size_t>(8) * kWidth + x) * 4;
        return std::max({pixels[i], pixels[i + 1], pixels[i + 2]});
    };
    EXPECT_LE(valueAt(sharp, 9), 5u);
    EXPECT_GE(valueAt(sharp, 15), 250u);
    EXPECT_GT(valueAt(blurred, 9), 20u);
    EXPECT_LT(valueAt(blurred, 9), 120u);
    EXPECT_GT(valueAt(blurred, 23), 20u);
    EXPECT_LT(valueAt(blurred, 23), 120u);
    EXPECT_LT(valueAt(blurred, 15), 150u);

    int litColumns = 0;
    for (uint32_t x = 0; x < kWidth; ++x)
        if (valueAt(blurred, x) > 10u) ++litColumns;
    EXPECT_GE(litColumns, 14);

    stripe.destroy();
    compositor.shutdown();
}

TEST_F(CompositorTest, GPU_EffectProcessor_ResizeThenTwoEffects)
{
    if (!hasGPU()) GTEST_SKIP() << "No Vulkan device";

    constexpr uint32_t kWidth = 37;
    constexpr uint32_t kHeight = 19;
    rt::EffectProcessor processor;
    rt::EffectProcessorConfig cfg;
    cfg.width = 64;
    cfg.height = 64;
    ASSERT_TRUE(processor.init(
        g_vk->device, g_vk->allocator, g_vk->cmdPool,
        g_vk->device.graphicsQueue(), cfg));

    // Adjustment processors start at a default size and then resize to the
    // preview/export frame. That used to leave effect #2's sampled binding
    // pointing at the destroyed pre-resize image view.
    ASSERT_TRUE(processor.resize(kWidth, kHeight));

    std::vector<uint8_t> sourcePixels(kWidth * kHeight * 4);
    for (uint32_t y = 0; y < kHeight; ++y) {
        for (uint32_t x = 0; x < kWidth; ++x) {
            const size_t i = (static_cast<size_t>(y) * kWidth + x) * 4;
            sourcePixels[i + 0] = static_cast<uint8_t>((x * 7 + y * 3) & 0xff);
            sourcePixels[i + 1] = static_cast<uint8_t>((x * 2 + y * 11) & 0xff);
            sourcePixels[i + 2] = static_cast<uint8_t>((x * 13 + y * 5) & 0xff);
            sourcePixels[i + 3] = 255;
        }
    }
    auto source = createRgbaTexture(kWidth, kHeight, sourcePixels);

    const std::vector<float> neutralColorCorrect = {
        0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f};
    std::vector<rt::EffectStack::EffectSnapshot> effects(2);
    effects[0].type = rt::EffectType::ColorCorrect;
    effects[0].params = neutralColorCorrect;
    effects[1].type = rt::EffectType::ColorCorrect;
    effects[1].params = neutralColorCorrect;

    VkCommandBuffer cmd = g_vk->cmdPool.beginSingleTime();
    ASSERT_NE(cmd, VK_NULL_HANDLE);
    ASSERT_TRUE(processor.process(cmd, source.descriptorInfo(), effects));
    g_vk->cmdPool.endSingleTime(cmd, g_vk->device.graphicsQueue());

    std::vector<uint8_t> outputPixels;
    ASSERT_TRUE(processor.readbackOutput(outputPixels));
    EXPECT_EQ(outputPixels, sourcePixels);

    source.destroy();
    processor.shutdown();
}

TEST_F(CompositorTest, GPU_Compositor_ClipLocalMaskFollowsLayerTransform)
{
    if (!hasGPU()) GTEST_SKIP() << "No Vulkan device";

    rt::Compositor compositor;
    rt::CompositorConfig cfg;
    cfg.outputWidth = 32;
    cfg.outputHeight = 16;

    ASSERT_TRUE(compositor.init(g_vk->device, g_vk->allocator, g_vk->cmdPool,
                                g_vk->device.graphicsQueue(), cfg));

    auto redTex = createSolidTexture(16, 16, 255, 0, 0, 255);
    auto maskTex = createLeftHalfMaskTexture(16, 16);

    std::vector<rt::CompositorLayer> layers(1);
    layers[0].textureInfo = redTex.descriptorInfo();
    // Put the source in the output's right half. Its local mask should move
    // and scale with it, revealing x=[16,24) and hiding x=[24,32).
    layers[0].transform = rt::Compositor::buildLayerTransform(
        0.5f, 0.0f, 0.5f, 1.0f);
    layers[0].hasMask = true;
    layers[0].maskTextureInfo = maskTex.descriptorInfo();

    compositor.setLayers(layers);
    ASSERT_TRUE(compositor.compositeSync());

    std::vector<uint8_t> pixels;
    ASSERT_TRUE(compositor.readbackOutput(pixels));
    const auto colorAt = [&pixels](uint32_t x, uint32_t y) {
        const size_t i = (static_cast<size_t>(y) * 32 + x) * 4;
        return std::max({pixels[i], pixels[i + 1], pixels[i + 2]});
    };

    EXPECT_LE(colorAt(8, 8), 5u);     // outside the translated layer
    EXPECT_GE(colorAt(20, 8), 250u);  // revealed by local left-half mask
    EXPECT_LE(colorAt(28, 8), 5u);    // hidden by local right-half mask

    // Move the same layer into the output's left half (without rebuilding or
    // repositioning its mask texture). The revealed half must travel with the
    // source to x=[0,8). buildLayerTransform position is the layer's output-UV
    // origin, so the left half starts at 0 rather than -0.5.
    layers[0].transform = rt::Compositor::buildLayerTransform(
        0.0f, 0.0f, 0.5f, 1.0f);
    compositor.setLayers(layers);
    ASSERT_TRUE(compositor.compositeSync());
    ASSERT_TRUE(compositor.readbackOutput(pixels));
    EXPECT_GE(colorAt(4, 8), 250u);
    EXPECT_LE(colorAt(12, 8), 5u);
    EXPECT_LE(colorAt(20, 8), 5u);

    maskTex.destroy();
    redTex.destroy();
    compositor.shutdown();
}

TEST_F(CompositorTest, GPU_Compositor_PremultipliedMaskAttenuatesRgb)
{
    if (!hasGPU()) GTEST_SKIP() << "No Vulkan device";

    rt::Compositor compositor;
    rt::CompositorConfig cfg;
    cfg.outputWidth = 16;
    cfg.outputHeight = 16;

    ASSERT_TRUE(compositor.init(g_vk->device, g_vk->allocator, g_vk->cmdPool,
                                g_vk->device.graphicsQueue(), cfg));

    // An opaque white texture is valid in both straight-alpha and PMA form.
    // A 50% mask must turn it into 50% premultiplied white before blending;
    // otherwise flattening the result over black leaves a bright white halo.
    auto whiteTex = createSolidTexture(16, 16, 255, 255, 255, 255);
    auto halfMask = createSolidTexture(16, 16, 128, 128, 128, 128);

    std::vector<rt::CompositorLayer> layers(1);
    layers[0].textureInfo = whiteTex.descriptorInfo();
    layers[0].transform = rt::Compositor::identityTransform();
    layers[0].isPMA = true;
    layers[0].hasMask = true;
    layers[0].maskTextureInfo = halfMask.descriptorInfo();

    compositor.setLayers(layers);
    ASSERT_TRUE(compositor.compositeSync());

    std::vector<uint8_t> pixels;
    ASSERT_TRUE(compositor.readbackOutput(pixels));
    const size_t center = (8 * 16 + 8) * 4;
    EXPECT_NEAR(pixels[center + 0], 128, 3);
    EXPECT_NEAR(pixels[center + 1], 128, 3);
    EXPECT_NEAR(pixels[center + 2], 128, 3);
    EXPECT_GE(pixels[center + 3], 252);

    halfMask.destroy();
    whiteTex.destroy();
    compositor.shutdown();
}

TEST_F(CompositorTest, GPU_TemporalInterpolator_InitShutdown)
{
    if (!hasGPU()) GTEST_SKIP() << "No Vulkan GPU available";

    rt::TemporalInterpolator interpolator;
    EXPECT_TRUE(interpolator.init(g_vk->device));
    EXPECT_TRUE(interpolator.isInitialized());
    interpolator.shutdown();
    EXPECT_FALSE(interpolator.isInitialized());
}

TEST_F(CompositorTest, GPU_TemporalInterpolator_FrameBlendingIsDirectMix)
{
    if (!hasGPU()) GTEST_SKIP() << "No Vulkan GPU available";

    constexpr uint32_t kSize = 8;
    rt::TemporalInterpolator interpolator;
    ASSERT_TRUE(interpolator.init(g_vk->device));

    auto blackTex = createSolidTexture(kSize, kSize, 0, 0, 0, 255);
    auto whiteTex = createSolidTexture(kSize, kSize, 255, 255, 255, 255);

    rt::Compositor compositor;
    rt::CompositorConfig compositorCfg;
    compositorCfg.outputWidth = kSize;
    compositorCfg.outputHeight = kSize;
    ASSERT_TRUE(compositor.init(g_vk->device, g_vk->allocator, g_vk->cmdPool,
                                g_vk->device.graphicsQueue(), compositorCfg));

    std::vector<uint8_t> pixels;
    ASSERT_TRUE(renderTemporalPixels(interpolator, compositor,
                                     blackTex, whiteTex,
                                     kSize, kSize, 0.25f, 1, 0, pixels));
    const size_t center =
        (static_cast<size_t>(kSize / 2) * kSize + kSize / 2) * 4;
    EXPECT_NEAR(pixels[center + 0], 64, 3);
    EXPECT_NEAR(pixels[center + 1], 64, 3);
    EXPECT_NEAR(pixels[center + 2], 64, 3);
    EXPECT_GE(pixels[center + 3], 252);

    compositor.shutdown();
    whiteTex.destroy();
    blackTex.destroy();
    interpolator.shutdown();
}

TEST_F(CompositorTest, GPU_TemporalInterpolator_OpticalFlowRejectsSceneCut)
{
    if (!hasGPU()) GTEST_SKIP() << "No Vulkan GPU available";

    constexpr uint32_t kSize = 8;
    rt::TemporalInterpolator interpolator;
    ASSERT_TRUE(interpolator.init(g_vk->device));
    auto blackTex = createSolidTexture(kSize, kSize, 0, 0, 0, 255);
    auto whiteTex = createSolidTexture(kSize, kSize, 255, 255, 255, 255);

    rt::Compositor compositor;
    rt::CompositorConfig cfg;
    cfg.outputWidth = kSize;
    cfg.outputHeight = kSize;
    ASSERT_TRUE(compositor.init(g_vk->device, g_vk->allocator, g_vk->cmdPool,
                                g_vk->device.graphicsQueue(), cfg));

    for (const auto [phase, expectFirstFrame] :
         {std::pair{0.25f, true}, std::pair{0.75f, false}}) {
        std::vector<uint8_t> pixels;
        ASSERT_TRUE(renderTemporalPixels(interpolator, compositor,
                                         blackTex, whiteTex, kSize, kSize,
                                         phase, 2, 1, pixels));
        const size_t center =
            (static_cast<size_t>(kSize / 2) * kSize + kSize / 2) * 4;
        const int expected = expectFirstFrame ? 0 : 255;
        EXPECT_NEAR(pixels[center + 0], expected, 5);
        EXPECT_NEAR(pixels[center + 1], expected, 5);
        EXPECT_NEAR(pixels[center + 2], expected, 5);
        EXPECT_GE(pixels[center + 3], 252);
    }

    compositor.shutdown();
    whiteTex.destroy();
    blackTex.destroy();
    interpolator.shutdown();
}

TEST_F(CompositorTest, GPU_TemporalInterpolator_TracksTwelvePixelMotion)
{
    if (!hasGPU()) GTEST_SKIP() << "No Vulkan GPU available";

    constexpr uint32_t kWidth = 64;
    constexpr uint32_t kHeight = 64;
    constexpr int kObjectWidth = 20;
    constexpr int kObjectHeight = 24;
    constexpr int kStartY = 20;
    constexpr int kStartA = 8;
    constexpr int kStartB = 20; // Deliberately beyond the old +/-10px reach.
    constexpr int kStartMid = (kStartA + kStartB) / 2;

    const auto makeFrame = [](int objectX) {
        std::vector<uint8_t> pixels(kWidth * kHeight * 4);
        for (uint32_t y = 0; y < kHeight; ++y) {
            for (uint32_t x = 0; x < kWidth; ++x) {
                const size_t i = (static_cast<size_t>(y) * kWidth + x) * 4;
                pixels[i + 0] = 8;
                pixels[i + 1] = 10;
                pixels[i + 2] = 12;
                pixels[i + 3] = 255;
                if (static_cast<int>(x) >= objectX &&
                    static_cast<int>(x) < objectX + kObjectWidth &&
                    static_cast<int>(y) >= kStartY &&
                    static_cast<int>(y) < kStartY + kObjectHeight) {
                    const int rx = static_cast<int>(x) - objectX;
                    const int ry = static_cast<int>(y) - kStartY;
                    const uint32_t hash = static_cast<uint32_t>(
                        rx * 73 + ry * 151 + rx * ry * 17);
                    const uint8_t value =
                        static_cast<uint8_t>(64 + hash % 192);
                    pixels[i + 0] = value;
                    pixels[i + 1] = value;
                    pixels[i + 2] = value;
                }
            }
        }
        return pixels;
    };

    const auto frameA = makeFrame(kStartA);
    const auto frameB = makeFrame(kStartB);
    const auto expected = makeFrame(kStartMid);
    auto texA = createRgbaTexture(kWidth, kHeight, frameA);
    auto texB = createRgbaTexture(kWidth, kHeight, frameB);

    rt::TemporalInterpolator interpolator;
    ASSERT_TRUE(interpolator.init(g_vk->device));
    rt::Compositor compositor;
    rt::CompositorConfig cfg;
    cfg.outputWidth = kWidth;
    cfg.outputHeight = kHeight;
    ASSERT_TRUE(compositor.init(g_vk->device, g_vk->allocator, g_vk->cmdPool,
                                g_vk->device.graphicsQueue(), cfg));

    std::vector<uint8_t> blended;
    std::vector<uint8_t> optical;
    ASSERT_TRUE(renderTemporalPixels(interpolator, compositor, texA, texB,
                                     kWidth, kHeight, 0.5f, 1, 2, blended));
    ASSERT_TRUE(renderTemporalPixels(interpolator, compositor, texA, texB,
                                     kWidth, kHeight, 0.5f, 2, 3, optical));

    const auto regionMae = [&expected](const std::vector<uint8_t>& actual) {
        uint64_t error = 0;
        uint64_t samples = 0;
        for (int y = kStartY - 4; y < kStartY + kObjectHeight + 4; ++y) {
            for (int x = kStartA - 4; x < kStartB + kObjectWidth + 4; ++x) {
                const size_t i = (static_cast<size_t>(y) * kWidth + x) * 4;
                for (int channel = 0; channel < 3; ++channel) {
                    error += static_cast<uint64_t>(std::abs(
                        static_cast<int>(actual[i + channel]) -
                        static_cast<int>(expected[i + channel])));
                    ++samples;
                }
            }
        }
        return static_cast<double>(error) / static_cast<double>(samples);
    };

    const double blendError = regionMae(blended);
    const double opticalError = regionMae(optical);
    const double nearestAError = regionMae(frameA);
    const double nearestBError = regionMae(frameB);
    const double nearestError = std::min(nearestAError, nearestBError);
    EXPECT_LT(opticalError, blendError * 0.65)
        << "optical MAE=" << opticalError << ", blend MAE=" << blendError;
    EXPECT_LT(opticalError, nearestError * 0.80)
        << "optical MAE=" << opticalError
        << ", nearest-A MAE=" << nearestAError
        << ", nearest-B MAE=" << nearestBError;

    compositor.shutdown();
    texB.destroy();
    texA.destroy();
    interpolator.shutdown();
}

// ── Compositor: half-opacity layer ──────────────────────────────────────────

TEST_F(CompositorTest, GPU_Compositor_HalfOpacity)
{
    if (!hasGPU()) GTEST_SKIP() << "No Vulkan device";

    rt::Compositor compositor;
    rt::CompositorConfig cfg;
    cfg.outputWidth  = 16;
    cfg.outputHeight = 16;

    ASSERT_TRUE(compositor.init(g_vk->device, g_vk->allocator, g_vk->cmdPool,
                                g_vk->device.graphicsQueue(), cfg));

    auto whiteTex = createSolidTexture(16, 16, 255, 255, 255, 255);

    std::vector<rt::CompositorLayer> layers(1);
    layers[0].textureInfo = whiteTex.descriptorInfo();
    layers[0].transform   = rt::Compositor::identityTransform();
    layers[0].opacity     = 0.5f;
    layers[0].blendMode   = rt::BlendMode::Normal;
    layers[0].enabled     = true;

    compositor.setLayers(layers);
    ASSERT_TRUE(compositor.compositeSync());

    std::vector<uint8_t> pixels;
    ASSERT_TRUE(compositor.readbackOutput(pixels));

    // With 50% opacity over transparent black, alpha compositing gives:
    // result.a = 0.5, result.rgb = white
    size_t ci = (8 * 16 + 8) * 4;
    EXPECT_NEAR(pixels[ci + 3], 128u, 10u);  // A ≈ 128 (50% of 255)

    whiteTex.destroy();
    compositor.shutdown();
}

// ── Compositor: two layers ──────────────────────────────────────────────────

TEST_F(CompositorTest, GPU_Compositor_TwoLayers)
{
    if (!hasGPU()) GTEST_SKIP() << "No Vulkan device";

    rt::Compositor compositor;
    rt::CompositorConfig cfg;
    cfg.outputWidth  = 16;
    cfg.outputHeight = 16;

    ASSERT_TRUE(compositor.init(g_vk->device, g_vk->allocator, g_vk->cmdPool,
                                g_vk->device.graphicsQueue(), cfg));

    auto blueTex  = createSolidTexture(16, 16, 0, 0, 255, 255);
    auto greenTex = createSolidTexture(16, 16, 0, 255, 0, 255);

    // Blue on bottom, green on top (both fully opaque)
    std::vector<rt::CompositorLayer> layers(2);
    layers[0].textureInfo = blueTex.descriptorInfo();
    layers[0].transform   = rt::Compositor::identityTransform();
    layers[0].opacity     = 1.0f;
    layers[0].enabled     = true;

    layers[1].textureInfo = greenTex.descriptorInfo();
    layers[1].transform   = rt::Compositor::identityTransform();
    layers[1].opacity     = 1.0f;
    layers[1].enabled     = true;

    compositor.setLayers(layers);
    EXPECT_EQ(compositor.layerCount(), 2u);

    ASSERT_TRUE(compositor.compositeSync());

    std::vector<uint8_t> pixels;
    ASSERT_TRUE(compositor.readbackOutput(pixels));

    // Top layer (green) should dominate
    size_t ci = (8 * 16 + 8) * 4;
    EXPECT_LE(pixels[ci + 0], 5u);    // R ≈ 0
    EXPECT_GE(pixels[ci + 1], 250u);  // G ≈ 255
    EXPECT_LE(pixels[ci + 2], 5u);    // B ≈ 0 (green covers blue)
    EXPECT_GE(pixels[ci + 3], 250u);  // A ≈ 255

    blueTex.destroy();
    greenTex.destroy();
    compositor.shutdown();
}

// ── Compositor: disabled layer ──────────────────────────────────────────────

TEST_F(CompositorTest, GPU_Compositor_DisabledLayer)
{
    if (!hasGPU()) GTEST_SKIP() << "No Vulkan device";

    rt::Compositor compositor;
    rt::CompositorConfig cfg;
    cfg.outputWidth  = 16;
    cfg.outputHeight = 16;

    ASSERT_TRUE(compositor.init(g_vk->device, g_vk->allocator, g_vk->cmdPool,
                                g_vk->device.graphicsQueue(), cfg));

    auto redTex = createSolidTexture(16, 16, 255, 0, 0, 255);

    std::vector<rt::CompositorLayer> layers(1);
    layers[0].textureInfo = redTex.descriptorInfo();
    layers[0].transform   = rt::Compositor::identityTransform();
    layers[0].opacity     = 1.0f;
    layers[0].enabled     = false; // DISABLED

    compositor.setLayers(layers);
    ASSERT_TRUE(compositor.compositeSync());

    std::vector<uint8_t> pixels;
    ASSERT_TRUE(compositor.readbackOutput(pixels));

    // Disabled layer should produce transparent black
    size_t ci = (8 * 16 + 8) * 4;
    EXPECT_LE(pixels[ci + 0], 5u);
    EXPECT_LE(pixels[ci + 1], 5u);
    EXPECT_LE(pixels[ci + 2], 5u);
    EXPECT_LE(pixels[ci + 3], 5u);

    redTex.destroy();
    compositor.shutdown();
}

// ── Compositor: additive blend ──────────────────────────────────────────────

TEST_F(CompositorTest, GPU_Compositor_AddBlend)
{
    if (!hasGPU()) GTEST_SKIP() << "No Vulkan device";

    rt::Compositor compositor;
    rt::CompositorConfig cfg;
    cfg.outputWidth  = 16;
    cfg.outputHeight = 16;

    ASSERT_TRUE(compositor.init(g_vk->device, g_vk->allocator, g_vk->cmdPool,
                                g_vk->device.graphicsQueue(), cfg));

    auto redTex  = createSolidTexture(16, 16, 128, 0, 0, 255);
    auto blueTex = createSolidTexture(16, 16, 0, 0, 128, 255);

    std::vector<rt::CompositorLayer> layers(2);
    layers[0].textureInfo = redTex.descriptorInfo();
    layers[0].transform   = rt::Compositor::identityTransform();
    layers[0].opacity     = 1.0f;
    layers[0].blendMode   = rt::BlendMode::Normal;
    layers[0].enabled     = true;

    layers[1].textureInfo = blueTex.descriptorInfo();
    layers[1].transform   = rt::Compositor::identityTransform();
    layers[1].opacity     = 1.0f;
    layers[1].blendMode   = rt::BlendMode::Add;
    layers[1].enabled     = true;

    compositor.setLayers(layers);
    ASSERT_TRUE(compositor.compositeSync());

    std::vector<uint8_t> pixels;
    ASSERT_TRUE(compositor.readbackOutput(pixels));

    // Additive blend: R=128+0, G=0, B=0+128 → should have both red and blue
    size_t ci = (8 * 16 + 8) * 4;
    EXPECT_GE(pixels[ci + 0], 100u);  // R significant
    EXPECT_GE(pixels[ci + 2], 100u);  // B significant

    redTex.destroy();
    blueTex.destroy();
    compositor.shutdown();
}

// ── Compositor: clearLayers ─────────────────────────────────────────────────

TEST_F(CompositorTest, GPU_Compositor_ClearLayers)
{
    if (!hasGPU()) GTEST_SKIP() << "No Vulkan device";

    rt::Compositor compositor;
    rt::CompositorConfig cfg;
    cfg.outputWidth  = 16;
    cfg.outputHeight = 16;

    ASSERT_TRUE(compositor.init(g_vk->device, g_vk->allocator, g_vk->cmdPool,
                                g_vk->device.graphicsQueue(), cfg));

    auto tex = createSolidTexture(16, 16, 255, 255, 255, 255);

    std::vector<rt::CompositorLayer> layers(1);
    layers[0].textureInfo = tex.descriptorInfo();
    layers[0].transform   = rt::Compositor::identityTransform();
    layers[0].opacity     = 1.0f;
    layers[0].enabled     = true;

    compositor.setLayers(layers);
    EXPECT_EQ(compositor.layerCount(), 1u);

    compositor.clearLayers();
    EXPECT_EQ(compositor.layerCount(), 0u);

    tex.destroy();
    compositor.shutdown();
}

// ── Compositor: resize ──────────────────────────────────────────────────────

TEST_F(CompositorTest, GPU_Compositor_Resize)
{
    if (!hasGPU()) GTEST_SKIP() << "No Vulkan device";

    rt::Compositor compositor;
    rt::CompositorConfig cfg;
    cfg.outputWidth  = 32;
    cfg.outputHeight = 32;

    ASSERT_TRUE(compositor.init(g_vk->device, g_vk->allocator, g_vk->cmdPool,
                                g_vk->device.graphicsQueue(), cfg));

    EXPECT_EQ(compositor.outputWidth(), 32u);
    EXPECT_EQ(compositor.outputHeight(), 32u);

    ASSERT_TRUE(compositor.resize(64, 48));
    EXPECT_EQ(compositor.outputWidth(), 64u);
    EXPECT_EQ(compositor.outputHeight(), 48u);

    // Same size should be a no-op
    ASSERT_TRUE(compositor.resize(64, 48));

    compositor.shutdown();
}

// ── Compositor: stats ───────────────────────────────────────────────────────

TEST_F(CompositorTest, GPU_Compositor_Stats)
{
    if (!hasGPU()) GTEST_SKIP() << "No Vulkan device";

    rt::Compositor compositor;
    rt::CompositorConfig cfg;
    cfg.outputWidth  = 16;
    cfg.outputHeight = 16;

    ASSERT_TRUE(compositor.init(g_vk->device, g_vk->allocator, g_vk->cmdPool,
                                g_vk->device.graphicsQueue(), cfg));

    auto tex = createSolidTexture(16, 16, 255, 0, 0, 255);

    std::vector<rt::CompositorLayer> layers(2);
    layers[0].textureInfo = tex.descriptorInfo();
    layers[0].transform   = rt::Compositor::identityTransform();
    layers[0].opacity     = 1.0f;
    layers[0].enabled     = true;

    layers[1].textureInfo = tex.descriptorInfo();
    layers[1].enabled     = false; // disabled

    compositor.setLayers(layers);
    ASSERT_TRUE(compositor.compositeSync());

    auto stats = compositor.stats();
    EXPECT_EQ(stats.layerCount, 2u);
    EXPECT_EQ(stats.enabledLayers, 1u);
    EXPECT_EQ(stats.outputWidth, 16u);
    EXPECT_EQ(stats.outputHeight, 16u);
    EXPECT_GE(stats.gpuTimeMs, 0.0f);  // Should have some timing

    tex.destroy();
    compositor.shutdown();
}

// ── Compositor: output descriptor info ──────────────────────────────────────

TEST_F(CompositorTest, GPU_Compositor_OutputDescriptor)
{
    if (!hasGPU()) GTEST_SKIP() << "No Vulkan device";

    rt::Compositor compositor;
    rt::CompositorConfig cfg;
    cfg.outputWidth  = 16;
    cfg.outputHeight = 16;

    ASSERT_TRUE(compositor.init(g_vk->device, g_vk->allocator, g_vk->cmdPool,
                                g_vk->device.graphicsQueue(), cfg));

    auto info = compositor.outputDescriptorInfo();
    EXPECT_NE(info.imageView, VK_NULL_HANDLE);
    EXPECT_NE(info.sampler, VK_NULL_HANDLE);

    compositor.shutdown();
}

// ═════════════════════════════════════════════════════════════════════════════
//  TRANSITION RENDERER GPU TESTS
// ═════════════════════════════════════════════════════════════════════════════

TEST_F(CompositorTest, GPU_Transition_InitShutdown)
{
    if (!hasGPU()) GTEST_SKIP() << "No Vulkan device";

    rt::TransitionRenderer transition;
    rt::TransitionConfig cfg;
    cfg.outputWidth  = 32;
    cfg.outputHeight = 32;

    ASSERT_TRUE(transition.init(g_vk->device, g_vk->allocator, g_vk->cmdPool,
                                g_vk->device.graphicsQueue(), cfg));
    EXPECT_TRUE(transition.isInitialized());

    transition.shutdown();
    EXPECT_FALSE(transition.isInitialized());
}

TEST_F(CompositorTest, GPU_Transition_Dissolve_Start)
{
    if (!hasGPU()) GTEST_SKIP() << "No Vulkan device";

    rt::TransitionRenderer transition;
    rt::TransitionConfig cfg;
    cfg.outputWidth  = 16;
    cfg.outputHeight = 16;

    ASSERT_TRUE(transition.init(g_vk->device, g_vk->allocator, g_vk->cmdPool,
                                g_vk->device.graphicsQueue(), cfg));

    auto redTex  = createSolidTexture(16, 16, 255, 0, 0, 255);
    auto blueTex = createSolidTexture(16, 16, 0, 0, 255, 255);

    // Progress = 0: should show 100% A (red)
    ASSERT_TRUE(transition.renderSync(
        rt::TransitionSourceInfo{redTex.descriptorInfo()}, rt::TransitionSourceInfo{blueTex.descriptorInfo()},
        rt::GpuTransitionType::Dissolve, 0.0f));

    std::vector<uint8_t> pixels;
    ASSERT_TRUE(transition.readbackOutput(pixels));

    size_t ci = (8 * 16 + 8) * 4;
    EXPECT_GE(pixels[ci + 0], 250u);  // Red
    EXPECT_LE(pixels[ci + 2], 5u);    // No blue

    redTex.destroy();
    blueTex.destroy();
    transition.shutdown();
}

TEST_F(CompositorTest, GPU_Transition_Dissolve_End)
{
    if (!hasGPU()) GTEST_SKIP() << "No Vulkan device";

    rt::TransitionRenderer transition;
    rt::TransitionConfig cfg;
    cfg.outputWidth  = 16;
    cfg.outputHeight = 16;

    ASSERT_TRUE(transition.init(g_vk->device, g_vk->allocator, g_vk->cmdPool,
                                g_vk->device.graphicsQueue(), cfg));

    auto redTex  = createSolidTexture(16, 16, 255, 0, 0, 255);
    auto blueTex = createSolidTexture(16, 16, 0, 0, 255, 255);

    // Progress = 1: should show 100% B (blue)
    ASSERT_TRUE(transition.renderSync(
        rt::TransitionSourceInfo{redTex.descriptorInfo()}, rt::TransitionSourceInfo{blueTex.descriptorInfo()},
        rt::GpuTransitionType::Dissolve, 1.0f));

    std::vector<uint8_t> pixels;
    ASSERT_TRUE(transition.readbackOutput(pixels));

    size_t ci = (8 * 16 + 8) * 4;
    EXPECT_LE(pixels[ci + 0], 5u);    // No red
    EXPECT_GE(pixels[ci + 2], 250u);  // Blue

    redTex.destroy();
    blueTex.destroy();
    transition.shutdown();
}

TEST_F(CompositorTest, GPU_Transition_Dissolve_Mid)
{
    if (!hasGPU()) GTEST_SKIP() << "No Vulkan device";

    rt::TransitionRenderer transition;
    rt::TransitionConfig cfg;
    cfg.outputWidth  = 16;
    cfg.outputHeight = 16;

    ASSERT_TRUE(transition.init(g_vk->device, g_vk->allocator, g_vk->cmdPool,
                                g_vk->device.graphicsQueue(), cfg));

    auto redTex  = createSolidTexture(16, 16, 255, 0, 0, 255);
    auto blueTex = createSolidTexture(16, 16, 0, 0, 255, 255);

    // Progress = 0.5: should be a mix
    ASSERT_TRUE(transition.renderSync(
        rt::TransitionSourceInfo{redTex.descriptorInfo()}, rt::TransitionSourceInfo{blueTex.descriptorInfo()},
        rt::GpuTransitionType::Dissolve, 0.5f));

    std::vector<uint8_t> pixels;
    ASSERT_TRUE(transition.readbackOutput(pixels));

    size_t ci = (8 * 16 + 8) * 4;
    // Both R and B should be approximately half
    EXPECT_NEAR(pixels[ci + 0], 128u, 20u);  // R ≈ 50%
    EXPECT_NEAR(pixels[ci + 2], 128u, 20u);  // B ≈ 50%

    redTex.destroy();
    blueTex.destroy();
    transition.shutdown();
}

TEST_F(CompositorTest, GPU_Transition_TwoIndependentDissolvesInOneSubmission)
{
    if (!hasGPU()) GTEST_SKIP() << "No Vulkan device";

    rt::TransitionRenderer transition;
    rt::TransitionConfig cfg;
    cfg.outputWidth  = 16;
    cfg.outputHeight = 16;

    ASSERT_TRUE(transition.init(g_vk->device, g_vk->allocator, g_vk->cmdPool,
                                g_vk->device.graphicsQueue(), cfg));

    auto redTex   = createSolidTexture(16, 16, 255, 0, 0, 255);
    auto blueTex  = createSolidTexture(16, 16, 0, 0, 255, 255);
    auto whiteTex = createSolidTexture(16, 16, 255, 255, 255, 255);
    auto greenTex = createSolidTexture(16, 16, 0, 255, 0, 255);

    // Record both dissolves before submitting, matching two connected clip
    // pairs on different tracks in one composite frame.
    VkCommandBuffer cmd = g_vk->cmdPool.beginSingleTime();
    ASSERT_TRUE(transition.render(
        cmd,
        rt::TransitionSourceInfo{redTex.descriptorInfo()},
        rt::TransitionSourceInfo{blueTex.descriptorInfo()},
        rt::GpuTransitionType::Dissolve, 0.5f,
        -1, 0.0f, -1.0f, 0, 0));
    ASSERT_TRUE(transition.render(
        cmd,
        rt::TransitionSourceInfo{whiteTex.descriptorInfo()},
        rt::TransitionSourceInfo{greenTex.descriptorInfo()},
        rt::GpuTransitionType::Dissolve, 0.25f,
        -1, 0.0f, -1.0f, 0, 1));
    g_vk->cmdPool.endSingleTime(cmd, g_vk->device.graphicsQueue());

    std::vector<uint8_t> first;
    std::vector<uint8_t> second;
    ASSERT_TRUE(transition.readbackOutput(first, 0, 0));
    ASSERT_TRUE(transition.readbackOutput(second, 0, 1));

    const size_t ci = (8 * 16 + 8) * 4;
    EXPECT_NEAR(first[ci + 0], 128u, 20u);
    EXPECT_LE(first[ci + 1], 5u);
    EXPECT_NEAR(first[ci + 2], 128u, 20u);

    EXPECT_NEAR(second[ci + 0], 191u, 20u);
    EXPECT_GE(second[ci + 1], 250u);
    EXPECT_NEAR(second[ci + 2], 191u, 20u);

    redTex.destroy();
    blueTex.destroy();
    whiteTex.destroy();
    greenTex.destroy();
    transition.shutdown();
}

TEST_F(CompositorTest, GPU_Transition_FadeBlack_Start)
{
    if (!hasGPU()) GTEST_SKIP() << "No Vulkan device";

    rt::TransitionRenderer transition;
    rt::TransitionConfig cfg;
    cfg.outputWidth  = 16;
    cfg.outputHeight = 16;

    ASSERT_TRUE(transition.init(g_vk->device, g_vk->allocator, g_vk->cmdPool,
                                g_vk->device.graphicsQueue(), cfg));

    auto whiteTex = createSolidTexture(16, 16, 255, 255, 255, 255);
    auto greenTex = createSolidTexture(16, 16, 0, 255, 0, 255);

    // Progress = 0: should show 100% A (white)
    ASSERT_TRUE(transition.renderSync(
        rt::TransitionSourceInfo{whiteTex.descriptorInfo()}, rt::TransitionSourceInfo{greenTex.descriptorInfo()},
        rt::GpuTransitionType::FadeBlack, 0.0f));

    std::vector<uint8_t> pixels;
    ASSERT_TRUE(transition.readbackOutput(pixels));

    size_t ci = (8 * 16 + 8) * 4;
    EXPECT_GE(pixels[ci + 0], 250u);  // R ≈ 255
    EXPECT_GE(pixels[ci + 1], 250u);  // G ≈ 255
    EXPECT_GE(pixels[ci + 2], 250u);  // B ≈ 255

    whiteTex.destroy();
    greenTex.destroy();
    transition.shutdown();
}

TEST_F(CompositorTest, GPU_Transition_FadeBlack_Mid)
{
    if (!hasGPU()) GTEST_SKIP() << "No Vulkan device";

    rt::TransitionRenderer transition;
    rt::TransitionConfig cfg;
    cfg.outputWidth  = 16;
    cfg.outputHeight = 16;

    ASSERT_TRUE(transition.init(g_vk->device, g_vk->allocator, g_vk->cmdPool,
                                g_vk->device.graphicsQueue(), cfg));

    auto whiteTex = createSolidTexture(16, 16, 255, 255, 255, 255);
    auto greenTex = createSolidTexture(16, 16, 0, 255, 0, 255);

    // Progress = 0.5: should be fully black
    ASSERT_TRUE(transition.renderSync(
        rt::TransitionSourceInfo{whiteTex.descriptorInfo()}, rt::TransitionSourceInfo{greenTex.descriptorInfo()},
        rt::GpuTransitionType::FadeBlack, 0.5f));

    std::vector<uint8_t> pixels;
    ASSERT_TRUE(transition.readbackOutput(pixels));

    size_t ci = (8 * 16 + 8) * 4;
    EXPECT_LE(pixels[ci + 0], 5u);  // R ≈ 0
    EXPECT_LE(pixels[ci + 1], 5u);  // G ≈ 0
    EXPECT_LE(pixels[ci + 2], 5u);  // B ≈ 0

    whiteTex.destroy();
    greenTex.destroy();
    transition.shutdown();
}

TEST_F(CompositorTest, GPU_Transition_FadeBlack_End)
{
    if (!hasGPU()) GTEST_SKIP() << "No Vulkan device";

    rt::TransitionRenderer transition;
    rt::TransitionConfig cfg;
    cfg.outputWidth  = 16;
    cfg.outputHeight = 16;

    ASSERT_TRUE(transition.init(g_vk->device, g_vk->allocator, g_vk->cmdPool,
                                g_vk->device.graphicsQueue(), cfg));

    auto whiteTex = createSolidTexture(16, 16, 255, 255, 255, 255);
    auto greenTex = createSolidTexture(16, 16, 0, 255, 0, 255);

    // Progress = 1: should show 100% B (green)
    ASSERT_TRUE(transition.renderSync(
        rt::TransitionSourceInfo{whiteTex.descriptorInfo()}, rt::TransitionSourceInfo{greenTex.descriptorInfo()},
        rt::GpuTransitionType::FadeBlack, 1.0f));

    std::vector<uint8_t> pixels;
    ASSERT_TRUE(transition.readbackOutput(pixels));

    size_t ci = (8 * 16 + 8) * 4;
    EXPECT_LE(pixels[ci + 0], 5u);    // R ≈ 0
    EXPECT_GE(pixels[ci + 1], 250u);  // G ≈ 255
    EXPECT_LE(pixels[ci + 2], 5u);    // B ≈ 0

    whiteTex.destroy();
    greenTex.destroy();
    transition.shutdown();
}

TEST_F(CompositorTest, GPU_Transition_Wipe)
{
    if (!hasGPU()) GTEST_SKIP() << "No Vulkan device";

    rt::TransitionRenderer transition;
    rt::TransitionConfig cfg;
    cfg.outputWidth  = 64;
    cfg.outputHeight = 64;
    cfg.wipeSoftness = 0.01f; // Very sharp edge for testing

    ASSERT_TRUE(transition.init(g_vk->device, g_vk->allocator, g_vk->cmdPool,
                                g_vk->device.graphicsQueue(), cfg));

    auto redTex  = createSolidTexture(64, 64, 255, 0, 0, 255);
    auto blueTex = createSolidTexture(64, 64, 0, 0, 255, 255);

    // Wipe right at 50%: left half should be B (blue), right half should be A (red)
    ASSERT_TRUE(transition.renderSync(
        rt::TransitionSourceInfo{redTex.descriptorInfo()}, rt::TransitionSourceInfo{blueTex.descriptorInfo()},
        rt::GpuTransitionType::WipeRight, 0.5f));

    std::vector<uint8_t> pixels;
    ASSERT_TRUE(transition.readbackOutput(pixels));

    // Check left quarter (should be mostly blue = source B revealed)
    size_t leftIdx = (32 * 64 + 8) * 4;
    EXPECT_LE(pixels[leftIdx + 0], 30u);    // Low red
    EXPECT_GE(pixels[leftIdx + 2], 220u);   // High blue

    // Check right quarter (should be mostly red = source A still visible)
    size_t rightIdx = (32 * 64 + 56) * 4;
    EXPECT_GE(pixels[rightIdx + 0], 220u);  // High red
    EXPECT_LE(pixels[rightIdx + 2], 30u);   // Low blue

    redTex.destroy();
    blueTex.destroy();
    transition.shutdown();
}

TEST_F(CompositorTest, GPU_Transition_Resize)
{
    if (!hasGPU()) GTEST_SKIP() << "No Vulkan device";

    rt::TransitionRenderer transition;
    rt::TransitionConfig cfg;
    cfg.outputWidth  = 32;
    cfg.outputHeight = 32;

    ASSERT_TRUE(transition.init(g_vk->device, g_vk->allocator, g_vk->cmdPool,
                                g_vk->device.graphicsQueue(), cfg));

    EXPECT_EQ(transition.outputWidth(), 32u);
    EXPECT_EQ(transition.outputHeight(), 32u);

    ASSERT_TRUE(transition.resize(64, 48));
    EXPECT_EQ(transition.outputWidth(), 64u);
    EXPECT_EQ(transition.outputHeight(), 48u);

    transition.shutdown();
}

TEST_F(CompositorTest, GPU_Transition_Stats)
{
    if (!hasGPU()) GTEST_SKIP() << "No Vulkan device";

    rt::TransitionRenderer transition;
    rt::TransitionConfig cfg;
    cfg.outputWidth  = 16;
    cfg.outputHeight = 16;

    ASSERT_TRUE(transition.init(g_vk->device, g_vk->allocator, g_vk->cmdPool,
                                g_vk->device.graphicsQueue(), cfg));

    auto texA = createSolidTexture(16, 16, 255, 0, 0, 255);
    auto texB = createSolidTexture(16, 16, 0, 255, 0, 255);

    ASSERT_TRUE(transition.renderSync(
        rt::TransitionSourceInfo{texA.descriptorInfo()}, rt::TransitionSourceInfo{texB.descriptorInfo()},
        rt::GpuTransitionType::Dissolve, 0.3f));

    auto stats = transition.stats();
    EXPECT_EQ(stats.type, rt::GpuTransitionType::Dissolve);
    EXPECT_NEAR(stats.progress, 0.3f, 0.01f);
    EXPECT_GE(stats.gpuTimeMs, 0.0f);

    texA.destroy();
    texB.destroy();
    transition.shutdown();
}

TEST_F(CompositorTest, GPU_Transition_OutputDescriptor)
{
    if (!hasGPU()) GTEST_SKIP() << "No Vulkan device";

    rt::TransitionRenderer transition;
    rt::TransitionConfig cfg;
    cfg.outputWidth  = 16;
    cfg.outputHeight = 16;

    ASSERT_TRUE(transition.init(g_vk->device, g_vk->allocator, g_vk->cmdPool,
                                g_vk->device.graphicsQueue(), cfg));

    auto info = transition.outputDescriptorInfo();
    EXPECT_NE(info.imageView, VK_NULL_HANDLE);
    EXPECT_NE(info.sampler, VK_NULL_HANDLE);

    transition.shutdown();
}

// ═════════════════════════════════════════════════════════════════════════════
//  COMPOSITOR INTEGRATION TESTS
// ═════════════════════════════════════════════════════════════════════════════

TEST_F(CompositorTest, GPU_Compositor_MultiBlendModes)
{
    if (!hasGPU()) GTEST_SKIP() << "No Vulkan device";

    rt::Compositor compositor;
    rt::CompositorConfig cfg;
    cfg.outputWidth  = 16;
    cfg.outputHeight = 16;

    ASSERT_TRUE(compositor.init(g_vk->device, g_vk->allocator, g_vk->cmdPool,
                                g_vk->device.graphicsQueue(), cfg));

    auto grayTex = createSolidTexture(16, 16, 128, 128, 128, 255);

    // Test each blend mode compiles and runs without error
    rt::BlendMode modes[] = {
        rt::BlendMode::Normal,
        rt::BlendMode::Multiply,
        rt::BlendMode::Screen,
        rt::BlendMode::Add
    };

    for (auto mode : modes)
    {
        std::vector<rt::CompositorLayer> layers(2);
        layers[0].textureInfo = grayTex.descriptorInfo();
        layers[0].transform   = rt::Compositor::identityTransform();
        layers[0].opacity     = 1.0f;
        layers[0].blendMode   = rt::BlendMode::Normal;
        layers[0].enabled     = true;

        layers[1].textureInfo = grayTex.descriptorInfo();
        layers[1].transform   = rt::Compositor::identityTransform();
        layers[1].opacity     = 1.0f;
        layers[1].blendMode   = mode;
        layers[1].enabled     = true;

        compositor.setLayers(layers);
        ASSERT_TRUE(compositor.compositeSync())
            << "Failed for blend mode " << static_cast<int>(mode);

        std::vector<uint8_t> pixels;
        ASSERT_TRUE(compositor.readbackOutput(pixels));
        EXPECT_EQ(pixels.size(), 16u * 16u * 4u);
    }

    grayTex.destroy();
    compositor.shutdown();
}

TEST_F(CompositorTest, GPU_Compositor_MaxLayers)
{
    if (!hasGPU()) GTEST_SKIP() << "No Vulkan device";

    rt::Compositor compositor;
    rt::CompositorConfig cfg;
    cfg.outputWidth  = 8;
    cfg.outputHeight = 8;

    ASSERT_TRUE(compositor.init(g_vk->device, g_vk->allocator, g_vk->cmdPool,
                                g_vk->device.graphicsQueue(), cfg));

    auto tex = createSolidTexture(8, 8, 10, 10, 10, 255);

    // Fill all 32 layers
    std::vector<rt::CompositorLayer> layers(rt::kMaxCompositorLayers);
    for (auto& layer : layers)
    {
        layer.textureInfo = tex.descriptorInfo();
        layer.transform   = rt::Compositor::identityTransform();
        layer.opacity     = 1.0f;
        layer.blendMode   = rt::BlendMode::Normal;
        layer.enabled     = true;
    }

    compositor.setLayers(layers);
    EXPECT_EQ(compositor.layerCount(), rt::kMaxCompositorLayers);

    ASSERT_TRUE(compositor.compositeSync());

    std::vector<uint8_t> pixels;
    ASSERT_TRUE(compositor.readbackOutput(pixels));
    EXPECT_EQ(pixels.size(), 8u * 8u * 4u);

    tex.destroy();
    compositor.shutdown();
}

TEST_F(CompositorTest, GPU_AllTransitionTypes)
{
    if (!hasGPU()) GTEST_SKIP() << "No Vulkan device";

    rt::TransitionRenderer transition;
    rt::TransitionConfig cfg;
    cfg.outputWidth  = 16;
    cfg.outputHeight = 16;

    ASSERT_TRUE(transition.init(g_vk->device, g_vk->allocator, g_vk->cmdPool,
                                g_vk->device.graphicsQueue(), cfg));

    auto texA = createSolidTexture(16, 16, 255, 0, 0, 255);
    auto texB = createSolidTexture(16, 16, 0, 0, 255, 255);

    rt::GpuTransitionType types[] = {
        rt::GpuTransitionType::Dissolve,
        rt::GpuTransitionType::FadeBlack,
        rt::GpuTransitionType::WipeLeft,
        rt::GpuTransitionType::WipeRight,
        rt::GpuTransitionType::WipeUp,
        rt::GpuTransitionType::WipeDown,
    };

    for (auto type : types)
    {
        ASSERT_TRUE(transition.renderSync(
            rt::TransitionSourceInfo{texA.descriptorInfo()}, rt::TransitionSourceInfo{texB.descriptorInfo()},
            type, 0.5f))
            << "Failed for transition type " << static_cast<int>(type);

        std::vector<uint8_t> pixels;
        ASSERT_TRUE(transition.readbackOutput(pixels));
        EXPECT_EQ(pixels.size(), 16u * 16u * 4u);
    }

    texA.destroy();
    texB.destroy();
    transition.shutdown();
}
