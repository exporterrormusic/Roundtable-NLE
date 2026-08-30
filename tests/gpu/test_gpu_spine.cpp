/*
 * test_gpu_spine.cpp — Tests for Step 9: GPU Spine Renderer
 *
 * Tests the SpineRenderer in two modes:
 *   1. Structural tests (no GPU): verify push constants, projection matrices,
 *      model matrices, vertex format, configuration, and API contracts.
 *   2. GPU tests (require Vulkan): full init → texture upload → render →
 *      read back → validate. These are skipped if no Vulkan device is available.
 *
 * All GPU tests use offscreen rendering — no window/swapchain needed.
 */

#include <gtest/gtest.h>

#include <volk.h>          // Must come before any vulkan.h include
#include "SpineRenderer.h"
#include "vulkan/Instance.h"
#include "vulkan/Device.h"
#include "vulkan/Allocator.h"
#include "vulkan/CommandPool.h"
#include "spine/SpineEngine.h"
#include "spine/SpineAtlas.h"
#include "spine/ModelManager.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/epsilon.hpp>

#include <filesystem>
#include <cstring>
#include <vector>
#include <cmath>
#include <thread>
#include <future>

namespace fs = std::filesystem;

// ─── Helpers ────────────────────────────────────────────────────────────────

static std::string findAssetsDir()
{
    fs::path candidates[] = {
        // tests/gpu/test_gpu_spine.cpp → up 3 to repo root → assets/
        fs::path(__FILE__).parent_path().parent_path().parent_path() / "assets",
        fs::current_path() / "assets",
        fs::current_path().parent_path() / "assets",
        fs::current_path().parent_path().parent_path() / "assets",
    };
    for (auto& c : candidates) {
        if (fs::exists(c / "characters"))
            return c.string();
    }
    return {};
}

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
        // Initialize volk
        if (volkInitialize() != VK_SUCCESS)
            return false;

        rt::InstanceConfig cfg;
        cfg.appName = "test_gpu_spine";
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

class SpineRendererTest : public ::testing::Test {
protected:
    static void SetUpTestSuite()
    {
        // Try to initialize Vulkan once for all GPU tests
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

    void SetUp() override
    {
        assetsDir = findAssetsDir();
        hasAssets = !assetsDir.empty();
    }

    bool hasGPU() const { return g_vk != nullptr && g_vk->valid; }

    std::string assetsDir;
    bool hasAssets{false};
};


// ═════════════════════════════════════════════════════════════════════════════
//  STRUCTURAL TESTS (no GPU required)
// ═════════════════════════════════════════════════════════════════════════════

TEST_F(SpineRendererTest, DefaultConstruction)
{
    rt::SpineRenderer renderer;
    EXPECT_FALSE(renderer.isInitialized());
}

TEST_F(SpineRendererTest, PushConstantsSize)
{
    // Push constants must fit in 128 bytes (Vulkan minimum guarantee)
    EXPECT_LE(sizeof(rt::SpinePushConstants), 128u);

    // Verify layout: mat4 (64 bytes) + float (4 bytes)
    EXPECT_EQ(sizeof(rt::SpinePushConstants), 68u);
}

TEST_F(SpineRendererTest, VertexFormat)
{
    // SpineVertex must be tightly packed: x,y,u,v,r,g,b,a = 8 floats
    EXPECT_EQ(sizeof(rt::SpineVertex), 8u * sizeof(float));
    EXPECT_EQ(sizeof(rt::SpineVertex), 32u);

    // Verify offsets match shader layout
    EXPECT_EQ(offsetof(rt::SpineVertex, x), 0u);
    EXPECT_EQ(offsetof(rt::SpineVertex, u), 8u);
    EXPECT_EQ(offsetof(rt::SpineVertex, r), 16u);
}

TEST_F(SpineRendererTest, OrthoProjection_Identity)
{
    // At zoom=1.0, center at origin: maps viewport corners to NDC corners
    auto proj = rt::SpineRenderer::orthoProjection(1920.0f, 1080.0f);

    // Test that origin maps to center of NDC
    glm::vec4 origin = proj * glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
    EXPECT_NEAR(origin.x, 0.0f, 1e-5f);
    EXPECT_NEAR(origin.y, 0.0f, 1e-5f);

    // Right edge should map to +1
    glm::vec4 right = proj * glm::vec4(960.0f, 0.0f, 0.0f, 1.0f);
    EXPECT_NEAR(right.x, 1.0f, 1e-5f);

    // Top edge should map to -1 (Vulkan Y-flip)
    glm::vec4 top = proj * glm::vec4(0.0f, 540.0f, 0.0f, 1.0f);
    EXPECT_NEAR(top.y, -1.0f, 1e-5f);
}

TEST_F(SpineRendererTest, OrthoProjection_WithCenter)
{
    // Camera centered at (100, 200)
    auto proj = rt::SpineRenderer::orthoProjection(800.0f, 600.0f, 100.0f, 200.0f);

    // Center point should map to origin in NDC
    glm::vec4 center = proj * glm::vec4(100.0f, 200.0f, 0.0f, 1.0f);
    EXPECT_NEAR(center.x, 0.0f, 1e-5f);
    EXPECT_NEAR(center.y, 0.0f, 1e-5f);
}

TEST_F(SpineRendererTest, OrthoProjection_WithZoom)
{
    // Zoom 2x should halve the visible area
    auto proj1 = rt::SpineRenderer::orthoProjection(800.0f, 600.0f, 0.0f, 0.0f, 1.0f);
    auto proj2 = rt::SpineRenderer::orthoProjection(800.0f, 600.0f, 0.0f, 0.0f, 2.0f);

    // At zoom 2x, a point at half the edge should map to the same NDC as the full edge at zoom 1x
    glm::vec4 p1 = proj1 * glm::vec4(400.0f, 0.0f, 0.0f, 1.0f);
    glm::vec4 p2 = proj2 * glm::vec4(200.0f, 0.0f, 0.0f, 1.0f);
    EXPECT_NEAR(p1.x, p2.x, 1e-5f);
}

TEST_F(SpineRendererTest, ModelMatrix_Identity)
{
    auto model = rt::SpineRenderer::modelMatrix(0.0f, 0.0f);
    glm::vec4 p = model * glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
    EXPECT_NEAR(p.x, 0.0f, 1e-5f);
    EXPECT_NEAR(p.y, 0.0f, 1e-5f);
}

TEST_F(SpineRendererTest, ModelMatrix_Translation)
{
    auto model = rt::SpineRenderer::modelMatrix(100.0f, 200.0f);
    glm::vec4 p = model * glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
    EXPECT_NEAR(p.x, 100.0f, 1e-5f);
    EXPECT_NEAR(p.y, 200.0f, 1e-5f);
}

TEST_F(SpineRendererTest, ModelMatrix_Scale)
{
    auto model = rt::SpineRenderer::modelMatrix(0.0f, 0.0f, 2.0f, 3.0f);
    glm::vec4 p = model * glm::vec4(10.0f, 10.0f, 0.0f, 1.0f);
    EXPECT_NEAR(p.x, 20.0f, 1e-5f);
    EXPECT_NEAR(p.y, 30.0f, 1e-5f);
}

TEST_F(SpineRendererTest, ModelMatrix_Rotation)
{
    // 90-degree rotation should swap axes
    auto model = rt::SpineRenderer::modelMatrix(0.0f, 0.0f, 1.0f, 1.0f, 90.0f);
    glm::vec4 p = model * glm::vec4(1.0f, 0.0f, 0.0f, 1.0f);
    EXPECT_NEAR(p.x, 0.0f, 1e-5f);
    EXPECT_NEAR(p.y, 1.0f, 1e-5f);
}

TEST_F(SpineRendererTest, ModelMatrix_Combined)
{
    // Translate + scale: scale applied first, then translate
    auto model = rt::SpineRenderer::modelMatrix(50.0f, 100.0f, 2.0f, 2.0f);
    glm::vec4 p = model * glm::vec4(10.0f, 10.0f, 0.0f, 1.0f);
    EXPECT_NEAR(p.x, 70.0f, 1e-5f);   // 50 + 10*2
    EXPECT_NEAR(p.y, 120.0f, 1e-5f);  // 100 + 10*2
}

TEST_F(SpineRendererTest, ConfigDefaults)
{
    rt::SpineRendererConfig cfg;
    EXPECT_EQ(cfg.maxVertices, 65536u);
    EXPECT_EQ(cfg.maxIndices, 131072u);
    EXPECT_EQ(cfg.framesInFlight, 2u);
    EXPECT_EQ(cfg.renderWidth, 1920u);
    EXPECT_EQ(cfg.renderHeight, 1080u);
    EXPECT_EQ(cfg.colorFormat, VK_FORMAT_R8G8B8A8_UNORM);
}

TEST_F(SpineRendererTest, BlendModeEnum)
{
    // Ensure all blend modes have distinct values
    EXPECT_NE(static_cast<int>(rt::SpineBlendMode::Normal),
              static_cast<int>(rt::SpineBlendMode::Additive));
    EXPECT_NE(static_cast<int>(rt::SpineBlendMode::Additive),
              static_cast<int>(rt::SpineBlendMode::Multiply));
    EXPECT_NE(static_cast<int>(rt::SpineBlendMode::Multiply),
              static_cast<int>(rt::SpineBlendMode::Screen));
}

TEST_F(SpineRendererTest, RenderBatchLayout)
{
    rt::SpineRenderBatch batch;
    EXPECT_EQ(batch.texturePageIndex, -1);
    EXPECT_EQ(batch.blendMode, rt::SpineBlendMode::Normal);
    EXPECT_TRUE(batch.vertices.empty());
    EXPECT_TRUE(batch.indices.empty());
}

TEST_F(SpineRendererTest, RenderDataDefault)
{
    rt::SpineRenderData data;
    EXPECT_TRUE(data.batches.empty());
    EXPECT_EQ(data.boundsX, 0.0f);
    EXPECT_EQ(data.boundsY, 0.0f);
    EXPECT_EQ(data.boundsW, 0.0f);
    EXPECT_EQ(data.boundsH, 0.0f);
}

TEST_F(SpineRendererTest, StatsDefault)
{
    rt::SpineRenderStats stats;
    EXPECT_EQ(stats.drawCalls, 0u);
    EXPECT_EQ(stats.vertexCount, 0u);
    EXPECT_EQ(stats.indexCount, 0u);
    EXPECT_EQ(stats.skeletonCount, 0u);
    EXPECT_EQ(stats.gpuTimeMs, 0.0f);
}

TEST_F(SpineRendererTest, ShutdownWithoutInit)
{
    // Calling shutdown on uninitialized renderer should not crash
    rt::SpineRenderer renderer;
    renderer.shutdown();
    EXPECT_FALSE(renderer.isInitialized());
}

TEST_F(SpineRendererTest, BeginEndWithoutInit)
{
    // Uninitialized render operations must report failure instead of allowing
    // callers to treat stale framebuffer contents as a completed frame.
    rt::SpineRenderer renderer;
    EXPECT_FALSE(renderer.beginFrame());
    EXPECT_FALSE(renderer.endFrame());
    EXPECT_FALSE(renderer.waitForFrame());
    EXPECT_FALSE(renderer.isInitialized());
}

TEST_F(SpineRendererTest, RenderWithoutInit)
{
    // renderSkeleton on uninitialized renderer should not crash
    rt::SpineRenderer renderer;
    rt::SpineRenderData data;
    data.batches.push_back({0, rt::SpineBlendMode::Normal, {}, {}});
    EXPECT_FALSE(renderer.renderSkeleton(data, glm::mat4(1.0f)));
    EXPECT_FALSE(renderer.isInitialized());
}

TEST_F(SpineRendererTest, HasTextureBeforeInit)
{
    rt::SpineRenderer renderer;
    EXPECT_FALSE(renderer.hasTexture(0));
}

// ═════════════════════════════════════════════════════════════════════════════
//  GPU TESTS (require Vulkan device)
// ═════════════════════════════════════════════════════════════════════════════

TEST_F(SpineRendererTest, GPUInit)
{
    if (!hasGPU()) GTEST_SKIP() << "No Vulkan device available";

    rt::SpineRenderer renderer;
    ASSERT_TRUE(renderer.init(g_vk->device, g_vk->allocator, g_vk->cmdPool,
                               g_vk->device.graphicsQueue()));
    EXPECT_TRUE(renderer.isInitialized());

    // Framebuffer should be created
    EXPECT_EQ(renderer.framebuffer().width(), 1920u);
    EXPECT_EQ(renderer.framebuffer().height(), 1080u);

    renderer.shutdown();
    EXPECT_FALSE(renderer.isInitialized());
}

TEST_F(SpineRendererTest, GPUInitCustomConfig)
{
    if (!hasGPU()) GTEST_SKIP() << "No Vulkan device available";

    rt::SpineRendererConfig cfg;
    cfg.renderWidth  = 1280;
    cfg.renderHeight = 720;
    cfg.framesInFlight = 3;

    rt::SpineRenderer renderer;
    ASSERT_TRUE(renderer.init(g_vk->device, g_vk->allocator, g_vk->cmdPool,
                               g_vk->device.graphicsQueue(), cfg));
    EXPECT_TRUE(renderer.isInitialized());
    EXPECT_EQ(renderer.framebuffer().width(), 1280u);
    EXPECT_EQ(renderer.framebuffer().height(), 720u);

    renderer.shutdown();
}

TEST_F(SpineRendererTest, GPUDoubleInit)
{
    if (!hasGPU()) GTEST_SKIP() << "No Vulkan device available";

    rt::SpineRenderer renderer;
    ASSERT_TRUE(renderer.init(g_vk->device, g_vk->allocator, g_vk->cmdPool,
                               g_vk->device.graphicsQueue()));

    // Second init should succeed (returns true, already initialized)
    EXPECT_TRUE(renderer.init(g_vk->device, g_vk->allocator, g_vk->cmdPool,
                               g_vk->device.graphicsQueue()));

    renderer.shutdown();
}

TEST_F(SpineRendererTest, GPUResize)
{
    if (!hasGPU()) GTEST_SKIP() << "No Vulkan device available";

    rt::SpineRenderer renderer;
    ASSERT_TRUE(renderer.init(g_vk->device, g_vk->allocator, g_vk->cmdPool,
                               g_vk->device.graphicsQueue()));

    ASSERT_TRUE(renderer.resize(3840, 2160));
    EXPECT_EQ(renderer.framebuffer().width(), 3840u);
    EXPECT_EQ(renderer.framebuffer().height(), 2160u);

    renderer.shutdown();
}

TEST_F(SpineRendererTest, GPUTextureUpload)
{
    if (!hasGPU()) GTEST_SKIP() << "No Vulkan device available";

    rt::SpineRenderer renderer;
    ASSERT_TRUE(renderer.init(g_vk->device, g_vk->allocator, g_vk->cmdPool,
                               g_vk->device.graphicsQueue()));

    // Create a 4x4 test texture (RGBA8, solid white)
    const uint32_t w = 4, h = 4;
    std::vector<uint8_t> pixels(w * h * 4, 255);

    EXPECT_FALSE(renderer.hasTexture(0));
    ASSERT_TRUE(renderer.uploadAtlasTexture(0, pixels.data(), w, h, "test_texture"));
    EXPECT_TRUE(renderer.hasTexture(0));

    // Upload a second page
    ASSERT_TRUE(renderer.uploadAtlasTexture(1, pixels.data(), w, h, "test_texture_2"));
    EXPECT_TRUE(renderer.hasTexture(1));

    renderer.releaseTexture(0);
    EXPECT_FALSE(renderer.hasTexture(0));
    EXPECT_TRUE(renderer.hasTexture(1));

    renderer.releaseAllTextures();
    EXPECT_FALSE(renderer.hasTexture(1));

    renderer.shutdown();
}

TEST_F(SpineRendererTest, GPUEmptyFrame)
{
    if (!hasGPU()) GTEST_SKIP() << "No Vulkan device available";

    rt::SpineRenderer renderer;
    ASSERT_TRUE(renderer.init(g_vk->device, g_vk->allocator, g_vk->cmdPool,
                               g_vk->device.graphicsQueue()));

    // Empty frame — no draw calls, should still work
    renderer.beginFrame();
    renderer.endFrame();

    auto& stats = renderer.lastFrameStats();
    EXPECT_EQ(stats.drawCalls, 0u);
    EXPECT_EQ(stats.skeletonCount, 0u);

    renderer.shutdown();
}

TEST_F(SpineRendererTest, GPURenderSyntheticMesh)
{
    if (!hasGPU()) GTEST_SKIP() << "No Vulkan device available";

    rt::SpineRenderer renderer;
    ASSERT_TRUE(renderer.init(g_vk->device, g_vk->allocator, g_vk->cmdPool,
                               g_vk->device.graphicsQueue()));

    // Upload a test texture
    const uint32_t texW = 2, texH = 2;
    std::vector<uint8_t> pixels(texW * texH * 4, 255);
    ASSERT_TRUE(renderer.uploadAtlasTexture(0, pixels.data(), texW, texH));

    // Create synthetic render data — a simple quad
    rt::SpineRenderData data;
    rt::SpineRenderBatch batch;
    batch.texturePageIndex = 0;
    batch.blendMode = rt::SpineBlendMode::Normal;

    // Quad vertices (centered at origin, 100x100)
    batch.vertices = {
        {-50.0f, -50.0f,  0.0f, 0.0f,  1.0f, 1.0f, 1.0f, 1.0f},
        { 50.0f, -50.0f,  1.0f, 0.0f,  1.0f, 1.0f, 1.0f, 1.0f},
        { 50.0f,  50.0f,  1.0f, 1.0f,  1.0f, 1.0f, 1.0f, 1.0f},
        {-50.0f,  50.0f,  0.0f, 1.0f,  1.0f, 1.0f, 1.0f, 1.0f},
    };
    batch.indices = {0, 1, 2, 2, 3, 0};
    data.batches.push_back(batch);

    auto mvp = rt::SpineRenderer::orthoProjection(
        static_cast<float>(renderer.framebuffer().width()),
        static_cast<float>(renderer.framebuffer().height()));

    renderer.beginFrame();
    renderer.renderSkeleton(data, mvp, 1.0f);
    renderer.endFrame();

    // Wait for rendering to complete
    vkDeviceWaitIdle(g_vk->device.handle());

    auto& stats = renderer.lastFrameStats();
    EXPECT_EQ(stats.drawCalls, 1u);
    EXPECT_EQ(stats.skeletonCount, 1u);
    EXPECT_EQ(stats.vertexCount, 4u);
    EXPECT_EQ(stats.indexCount, 6u);

    renderer.shutdown();
}

TEST_F(SpineRendererTest, GPURenderMultipleBatches)
{
    if (!hasGPU()) GTEST_SKIP() << "No Vulkan device available";

    rt::SpineRenderer renderer;
    ASSERT_TRUE(renderer.init(g_vk->device, g_vk->allocator, g_vk->cmdPool,
                               g_vk->device.graphicsQueue()));

    const uint32_t texW = 2, texH = 2;
    std::vector<uint8_t> pixels(texW * texH * 4, 255);
    ASSERT_TRUE(renderer.uploadAtlasTexture(0, pixels.data(), texW, texH));

    // Create data with multiple batches (different blend modes)
    rt::SpineRenderData data;

    rt::SpineRenderBatch normalBatch;
    normalBatch.texturePageIndex = 0;
    normalBatch.blendMode = rt::SpineBlendMode::Normal;
    normalBatch.vertices = {
        {0, 0, 0, 0, 1, 1, 1, 1}, {10, 0, 1, 0, 1, 1, 1, 1}, {10, 10, 1, 1, 1, 1, 1, 1},
    };
    normalBatch.indices = {0, 1, 2};

    rt::SpineRenderBatch additiveBatch;
    additiveBatch.texturePageIndex = 0;
    additiveBatch.blendMode = rt::SpineBlendMode::Additive;
    additiveBatch.vertices = {
        {20, 0, 0, 0, 1, 1, 1, 0.5f}, {30, 0, 1, 0, 1, 1, 1, 0.5f}, {30, 10, 1, 1, 1, 1, 1, 0.5f},
    };
    additiveBatch.indices = {0, 1, 2};

    data.batches.push_back(normalBatch);
    data.batches.push_back(additiveBatch);

    auto mvp = rt::SpineRenderer::orthoProjection(1920.0f, 1080.0f);

    renderer.beginFrame();
    renderer.renderSkeleton(data, mvp);
    renderer.endFrame();

    vkDeviceWaitIdle(g_vk->device.handle());

    auto& stats = renderer.lastFrameStats();
    EXPECT_EQ(stats.drawCalls, 2u);
    EXPECT_EQ(stats.skeletonCount, 1u);
    EXPECT_EQ(stats.vertexCount, 6u);
    EXPECT_EQ(stats.indexCount, 6u);
    EXPECT_GT(stats.blendModeChanges, 0u);

    renderer.shutdown();
}

TEST_F(SpineRendererTest, GPURenderMultipleSkeletons)
{
    if (!hasGPU()) GTEST_SKIP() << "No Vulkan device available";

    rt::SpineRenderer renderer;
    ASSERT_TRUE(renderer.init(g_vk->device, g_vk->allocator, g_vk->cmdPool,
                               g_vk->device.graphicsQueue()));

    const uint32_t texW = 2, texH = 2;
    std::vector<uint8_t> pixels(texW * texH * 4, 255);
    ASSERT_TRUE(renderer.uploadAtlasTexture(0, pixels.data(), texW, texH));

    rt::SpineRenderBatch batch;
    batch.texturePageIndex = 0;
    batch.blendMode = rt::SpineBlendMode::Normal;
    batch.vertices = {
        {0, 0, 0, 0, 1, 1, 1, 1}, {10, 0, 1, 0, 1, 1, 1, 1}, {10, 10, 1, 1, 1, 1, 1, 1},
    };
    batch.indices = {0, 1, 2};

    rt::SpineRenderData data;
    data.batches.push_back(batch);

    auto proj = rt::SpineRenderer::orthoProjection(1920.0f, 1080.0f);

    renderer.beginFrame();

    // Render 5 skeletons at different positions
    for (int i = 0; i < 5; ++i) {
        auto model = rt::SpineRenderer::modelMatrix(
            static_cast<float>(i * 200 - 400), 0.0f);
        renderer.renderSkeleton(data, proj * model);
    }

    renderer.endFrame();
    vkDeviceWaitIdle(g_vk->device.handle());

    auto& stats = renderer.lastFrameStats();
    EXPECT_EQ(stats.skeletonCount, 5u);
    EXPECT_EQ(stats.drawCalls, 5u);
    EXPECT_EQ(stats.vertexCount, 15u);

    renderer.shutdown();
}

TEST_F(SpineRendererTest, GPUMultipleFrames)
{
    if (!hasGPU()) GTEST_SKIP() << "No Vulkan device available";

    rt::SpineRenderer renderer;
    ASSERT_TRUE(renderer.init(g_vk->device, g_vk->allocator, g_vk->cmdPool,
                               g_vk->device.graphicsQueue()));

    const uint32_t texW = 2, texH = 2;
    std::vector<uint8_t> pixels(texW * texH * 4, 255);
    ASSERT_TRUE(renderer.uploadAtlasTexture(0, pixels.data(), texW, texH));

    rt::SpineRenderBatch batch;
    batch.texturePageIndex = 0;
    batch.blendMode = rt::SpineBlendMode::Normal;
    batch.vertices = {
        {0, 0, 0, 0, 1, 1, 1, 1}, {10, 0, 1, 0, 1, 1, 1, 1}, {10, 10, 1, 1, 1, 1, 1, 1},
    };
    batch.indices = {0, 1, 2};

    rt::SpineRenderData data;
    data.batches.push_back(batch);
    auto mvp = rt::SpineRenderer::orthoProjection(1920.0f, 1080.0f);

    // Render 10 frames to test double-buffering
    for (int frame = 0; frame < 10; ++frame) {
        renderer.beginFrame();
        renderer.renderSkeleton(data, mvp);
        renderer.endFrame();
    }

    vkDeviceWaitIdle(g_vk->device.handle());

    // Last frame stats should be valid
    auto& stats = renderer.lastFrameStats();
    EXPECT_EQ(stats.drawCalls, 1u);

    renderer.shutdown();
}

#ifdef ROUNDTABLE_HAS_SPINE

TEST_F(SpineRendererTest, GPULoadAtlasTextures)
{
    if (!hasGPU()) GTEST_SKIP() << "No Vulkan device available";
    if (!hasAssets) GTEST_SKIP() << "No assets directory";

    // Load a real atlas
    auto paths = rt::SpineEngine::resolvePaths(assetsDir, "Crown", "default",
                                                rt::CharacterStance::Default);
    if (!paths.valid) GTEST_SKIP() << "Crown not found";

    rt::SpineAtlas atlas;
    ASSERT_TRUE(atlas.load(paths.atlasPath));

    rt::SpineRenderer renderer;
    ASSERT_TRUE(renderer.init(g_vk->device, g_vk->allocator, g_vk->cmdPool,
                               g_vk->device.graphicsQueue()));

    int loaded = renderer.loadAtlasTextures(atlas);
    EXPECT_GT(loaded, 0);

    for (size_t i = 0; i < atlas.pages().size(); ++i) {
        EXPECT_TRUE(renderer.hasTexture(static_cast<int>(i)));
    }

    renderer.shutdown();
}

TEST_F(SpineRendererTest, GPURenderRealSkeleton)
{
    if (!hasGPU()) GTEST_SKIP() << "No Vulkan device available";
    if (!hasAssets) GTEST_SKIP() << "No assets directory";

    auto paths = rt::SpineEngine::resolvePaths(assetsDir, "Crown", "default",
                                                rt::CharacterStance::Default);
    if (!paths.valid) GTEST_SKIP() << "Crown not found";

    // Load skeleton
    rt::SpineEngine engine;
    ASSERT_TRUE(engine.loadSkeleton(paths.skelPath, paths.atlasPath));

    // Set an animation
    auto anims = engine.animation().listAnimations();
    if (!anims.empty()) {
        engine.animation().setBodyAnimation(anims[0].name, false);
        engine.update(0.0f);
    }

    // Extract meshes
    auto renderData = engine.extractMeshes();
    EXPECT_GT(renderData.batches.size(), 0u);

    // Initialize renderer and load textures
    rt::SpineRenderer renderer;
    ASSERT_TRUE(renderer.init(g_vk->device, g_vk->allocator, g_vk->cmdPool,
                               g_vk->device.graphicsQueue()));
    renderer.loadAtlasTextures(engine.atlas());

    // Render frame
    auto mvp = rt::SpineRenderer::orthoProjection(1920.0f, 1080.0f);

    renderer.beginFrame();
    renderer.renderSkeleton(renderData, mvp);
    renderer.endFrame();

    vkDeviceWaitIdle(g_vk->device.handle());

    auto& stats = renderer.lastFrameStats();
    EXPECT_GT(stats.drawCalls, 0u);
    EXPECT_GT(stats.vertexCount, 0u);
    EXPECT_GT(stats.indexCount, 0u);
    EXPECT_EQ(stats.skeletonCount, 1u);

    renderer.shutdown();
}

TEST_F(SpineRendererTest, GPURenderMultipleRealSkeletons)
{
    if (!hasGPU()) GTEST_SKIP() << "No Vulkan device available";
    if (!hasAssets) GTEST_SKIP() << "No assets directory";

    // Load multiple characters
    std::vector<std::string> characters = {"Crown", "Chime", "Kilo", "Trony"};
    std::vector<rt::SpineEngine> engines;
    std::vector<rt::SpineRenderData> renderDatas;

    for (auto& name : characters) {
        auto paths = rt::SpineEngine::resolvePaths(assetsDir, name, "default",
                                                    rt::CharacterStance::Default);
        if (!paths.valid) continue;

        auto ver = rt::SpineEngine::detectVersion(paths.skelPath);
        if (ver.find("4.0") == 0) continue;  // Skip v4.0

        rt::SpineEngine eng;
        if (!eng.loadSkeleton(paths.skelPath, paths.atlasPath)) continue;

        auto anims = eng.animation().listAnimations();
        if (!anims.empty()) {
            eng.animation().setBodyAnimation(anims[0].name, false);
            eng.update(0.0f);
        }

        renderDatas.push_back(eng.extractMeshes());
        engines.push_back(std::move(eng));
    }

    if (engines.empty()) GTEST_SKIP() << "No loadable characters found";

    // Init renderer
    rt::SpineRenderer renderer;
    ASSERT_TRUE(renderer.init(g_vk->device, g_vk->allocator, g_vk->cmdPool,
                               g_vk->device.graphicsQueue()));

    // Load all atlas textures (from first engine for simplicity)
    for (auto& eng : engines) {
        renderer.loadAtlasTextures(eng.atlas());
    }

    // Render all skeletons in one frame
    auto proj = rt::SpineRenderer::orthoProjection(1920.0f, 1080.0f);

    renderer.beginFrame();
    for (size_t i = 0; i < renderDatas.size(); ++i) {
        float x = static_cast<float>(i) * 400.0f - 600.0f;
        auto model = rt::SpineRenderer::modelMatrix(x, 0.0f);
        renderer.renderSkeleton(renderDatas[i], proj * model);
    }
    renderer.endFrame();

    vkDeviceWaitIdle(g_vk->device.handle());

    auto& stats = renderer.lastFrameStats();
    EXPECT_EQ(stats.skeletonCount, static_cast<uint32_t>(renderDatas.size()));
    EXPECT_GT(stats.drawCalls, 0u);
    EXPECT_GT(stats.vertexCount, 0u);

    renderer.shutdown();
}

TEST_F(SpineRendererTest, GPUParallelSkeletonEvaluation)
{
    if (!hasGPU()) GTEST_SKIP() << "No Vulkan device available";
    if (!hasAssets) GTEST_SKIP() << "No assets directory";

    // Load Crown skeleton
    auto paths = rt::SpineEngine::resolvePaths(assetsDir, "Crown", "default",
                                                rt::CharacterStance::Default);
    if (!paths.valid) GTEST_SKIP() << "Crown not found";

    // Create multiple engines (simulating multiple characters on screen)
    constexpr int kNumCharacters = 4;
    std::vector<rt::SpineEngine> engines(kNumCharacters);

    for (int i = 0; i < kNumCharacters; ++i) {
        ASSERT_TRUE(engines[i].loadSkeleton(paths.skelPath, paths.atlasPath));
        auto anims = engines[i].animation().listAnimations();
        if (!anims.empty()) {
            engines[i].animation().setBodyAnimation(anims[0].name, true);
        }
    }

    // Parallel evaluation using std::async
    std::vector<std::future<rt::SpineRenderData>> futures;
    for (int i = 0; i < kNumCharacters; ++i) {
        futures.push_back(std::async(std::launch::async, [&engines, i]() {
            engines[i].update(0.016f);
            return engines[i].extractMeshes();
        }));
    }

    // Collect results
    std::vector<rt::SpineRenderData> results;
    for (auto& f : futures) {
        results.push_back(f.get());
    }

    EXPECT_EQ(results.size(), static_cast<size_t>(kNumCharacters));
    for (auto& data : results) {
        EXPECT_GT(data.batches.size(), 0u);
    }

    // Initialize renderer and render all
    rt::SpineRenderer renderer;
    ASSERT_TRUE(renderer.init(g_vk->device, g_vk->allocator, g_vk->cmdPool,
                               g_vk->device.graphicsQueue()));
    renderer.loadAtlasTextures(engines[0].atlas());

    auto proj = rt::SpineRenderer::orthoProjection(1920.0f, 1080.0f);

    renderer.beginFrame();
    for (size_t i = 0; i < results.size(); ++i) {
        float x = static_cast<float>(i) * 300.0f - 450.0f;
        auto model = rt::SpineRenderer::modelMatrix(x, 0.0f);
        renderer.renderSkeleton(results[i], proj * model);
    }
    renderer.endFrame();

    vkDeviceWaitIdle(g_vk->device.handle());

    auto& stats = renderer.lastFrameStats();
    EXPECT_EQ(stats.skeletonCount, static_cast<uint32_t>(kNumCharacters));

    renderer.shutdown();
}

#endif // ROUNDTABLE_HAS_SPINE
