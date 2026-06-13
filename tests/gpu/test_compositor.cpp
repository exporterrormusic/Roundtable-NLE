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
#include "TransitionRenderer.h"
#include "vulkan/Instance.h"
#include "vulkan/Device.h"
#include "vulkan/Allocator.h"
#include "vulkan/CommandPool.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/epsilon.hpp>

#include <cstring>
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

        valid = true;
        return true;
    }

    void shutdown()
    {
        if (!valid) return;
        cmdPool.destroy();
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
