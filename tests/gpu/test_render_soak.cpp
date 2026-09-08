/*
 * Production-context renderer soak test.
 *
 * The shared provider models live preview. RenderGpuResources models an
 * isolated export session. Both workers churn resolution generations while
 * submitting real compositor, effect, transition, Spine, and NV12 work.
 *
 * Set ROUNDTABLE_RENDER_SOAK_ROUNDS to increase the default two sweeps.
 * For synchronization validation in Release builds, also set:
 *   ROUNDTABLE_VALIDATION=1
 *   ROUNDTABLE_VALIDATION_FATAL=0
 */

#include <gtest/gtest.h>

#include "Compositor.h"
#include "EffectProcessor.h"
#include "GpuContext.h"
#include "Nv12Converter.h"
#include "RenderGpuResources.h"
#include "SpineRenderer.h"
#include "TransitionRenderer.h"
#include "effects/EffectStack.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <future>
#include <sstream>
#include <string>
#include <vector>

namespace {

struct Resolution
{
    uint32_t width;
    uint32_t height;
};

struct SweepResult
{
    bool ok{true};
    std::string error;
};

SweepResult failed(const char* stage, Resolution resolution)
{
    std::ostringstream message;
    message << stage << " failed at " << resolution.width << 'x'
            << resolution.height;
    return {false, message.str()};
}

int soakRounds()
{
    constexpr int kDefaultRounds = 2;
    constexpr int kMaximumRounds = 50;
    const char* value = std::getenv("ROUNDTABLE_RENDER_SOAK_ROUNDS");
    if (!value || !*value)
        return kDefaultRounds;

    char* end = nullptr;
    const long parsed = std::strtol(value, &end, 10);
    if (end == value || *end != '\0')
        return kDefaultRounds;
    return std::clamp(static_cast<int>(parsed), 2, kMaximumRounds);
}

template <typename ResourceProvider>
SweepResult runRenderSweep(ResourceProvider& resources, int round)
{
    // Five sizes exceed every three/four-entry render-helper cache. Keeping
    // dimensions small makes the normal regression run practical while still
    // exercising the same pipelines, descriptors, fences, and eviction paths.
    constexpr std::array<Resolution, 5> kRenderResolutions{{
        {64, 36}, {80, 46}, {96, 54}, {112, 64}, {128, 72}
    }};

    rt::EffectStack::EffectSnapshot neutralColor;
    neutralColor.type = rt::EffectType::ColorCorrect;
    neutralColor.params = {
        0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f
    };
    const std::vector<rt::EffectStack::EffectSnapshot> effects{neutralColor};

    for (size_t i = 0; i < kRenderResolutions.size(); ++i) {
        const Resolution resolution = kRenderResolutions[i];

        auto compositor = resources.compositor(
            resolution.width, resolution.height);
        if (!compositor)
            return failed("compositor creation", resolution);
        compositor->clearLayers();
        if (!compositor->compositeSync())
            return failed("compositor submission", resolution);

        auto effectProcessor = resources.effectProcessor(
            resolution.width, resolution.height);
        if (!effectProcessor)
            return failed("effect-processor creation", resolution);
        if (!effectProcessor->processSync(
                compositor->outputDescriptorInfo(), effects)) {
            return failed("effect submission", resolution);
        }

        auto transition = resources.transitionRenderer(
            resolution.width, resolution.height);
        if (!transition)
            return failed("transition creation", resolution);
        if (!transition->renderSync(
                rt::TransitionSourceInfo{compositor->outputDescriptorInfo()},
                rt::TransitionSourceInfo{effectProcessor->outputDescriptorInfo()},
                rt::GpuTransitionType::Dissolve,
                static_cast<float>((round + static_cast<int>(i)) % 10) / 10.0f)) {
            return failed("transition submission", resolution);
        }

        auto spine = resources.spineRenderer(
            resolution.width, resolution.height);
        if (!spine)
            return failed("Spine renderer creation", resolution);
        if (!spine->beginFrame() || !spine->endFrame() ||
            !spine->waitForFrame()) {
            return failed("Spine submission", resolution);
        }

        // Materialize the final transition once per sweep. This verifies the
        // last descriptor chain rather than treating successful submission as
        // sufficient evidence.
        if (i + 1 == kRenderResolutions.size()) {
            std::vector<uint8_t> pixels;
            if (!transition->readbackOutput(pixels) ||
                pixels.size() != static_cast<size_t>(resolution.width) *
                                 resolution.height * 4u) {
                return failed("transition readback", resolution);
            }
        }
    }

    // Nine converter sizes exceed both the shared (8) and isolated (6)
    // generation limits. Each converter performs a real upload, dispatch, and
    // readback so eviction overlaps production-style converter use.
    constexpr std::array<Resolution, 9> kDecodeResolutions{{
        {32, 18}, {40, 22}, {48, 28}, {56, 32}, {64, 36},
        {72, 42}, {80, 46}, {88, 50}, {96, 54}
    }};
    for (const Resolution resolution : kDecodeResolutions) {
        auto converter = resources.nv12Converter(
            resolution.width, resolution.height);
        if (!converter)
            return failed("NV12 converter creation", resolution);

        std::vector<uint8_t> yPlane(
            static_cast<size_t>(resolution.width) * resolution.height, 128u);
        std::vector<uint8_t> uvPlane(
            static_cast<size_t>(resolution.width) * resolution.height / 2u,
            128u);
        std::vector<uint8_t> pixels;
        if (!converter->convertAndReadbackNV12Scaled(
                yPlane.data(), static_cast<int>(resolution.width),
                uvPlane.data(), static_cast<int>(resolution.width),
                resolution.width, resolution.height,
                resolution.width, resolution.height, pixels) ||
            pixels.size() != static_cast<size_t>(resolution.width) *
                             resolution.height * 4u) {
            return failed("NV12 conversion", resolution);
        }
    }

    return {};
}

void expectWorkingSetWithinLimits(
    const rt::GpuGenerationWorkingSetStats& stats,
    size_t nv12EntryLimit)
{
    constexpr size_t kMiB = 1024ull * 1024ull;
    EXPECT_LE(stats.compositors.residentEntries, 3u);
    EXPECT_LE(stats.compositors.estimatedBytes, 192u * kMiB);
    EXPECT_LE(stats.effectProcessors.residentEntries, 4u);
    EXPECT_LE(stats.effectProcessors.estimatedBytes, 192u * kMiB);
    EXPECT_LE(stats.spineRenderers.residentEntries, 6u);
    EXPECT_LE(stats.spineRenderers.estimatedBytes, 768u * kMiB);
    EXPECT_LE(stats.transitionRenderers.residentEntries, 3u);
    EXPECT_LE(stats.transitionRenderers.estimatedBytes, 128u * kMiB);
    EXPECT_LE(stats.nv12Converters.residentEntries, nv12EntryLimit);
}

} // namespace

TEST(RenderSoakTest, ConcurrentPreviewExportResolutionChurnStaysBounded)
{
    auto& gpu = rt::GpuContext::get();
    if (!gpu.init(VK_NULL_HANDLE))
        GTEST_SKIP() << "Vulkan context unavailable";

    struct ContextShutdown
    {
        rt::GpuContext& context;
        ~ContextShutdown() { context.shutdown(); }
    } contextShutdown{gpu};

    rt::RenderGpuResources isolatedExport;
    ASSERT_TRUE(isolatedExport.init());

    const int rounds = soakRounds();
    const uint64_t deviceWaitsBefore =
        gpu.scheduler().deviceWaitIdleCalls();
    const uint64_t queueWaitsBefore =
        gpu.scheduler().queueWaitIdleCalls();

    rt::MemoryStats warmWorkingSet{};
    rt::MemoryStats finalWorkingSet{};
    for (int round = 0; round < rounds; ++round) {
        auto preview = std::async(std::launch::async, [&gpu, round] {
            return runRenderSweep(gpu, round);
        });
        auto exportRender = std::async(
            std::launch::async, [&isolatedExport, round] {
                return runRenderSweep(isolatedExport, round);
            });

        const SweepResult previewResult = preview.get();
        const SweepResult exportResult = exportRender.get();
        ASSERT_TRUE(previewResult.ok) << previewResult.error;
        ASSERT_TRUE(exportResult.ok) << exportResult.error;

        const rt::MemoryStats current = gpu.allocator().queryStats();
        if (round == 0)
            warmWorkingSet = current;
        finalWorkingSet = current;

        expectWorkingSetWithinLimits(gpu.generationCacheStats(), 8u);
        expectWorkingSetWithinLimits(
            isolatedExport.generationCacheStats(), 6u);
    }

    // The first sweep warms every lazy resource. Repeating the identical
    // churn must settle near that allocation level instead of growing once
    // per generation switch. Leave headroom for allocator bookkeeping and
    // driver-dependent lazy allocations that may appear on the second pass.
    constexpr uint64_t kGrowthTolerance = 32ull * 1024ull * 1024ull;
    EXPECT_LE(finalWorkingSet.totalUsedBytes,
              warmWorkingSet.totalUsedBytes + kGrowthTolerance);
    EXPECT_LE(finalWorkingSet.allocationCount,
              warmWorkingSet.allocationCount + 64u);

    EXPECT_EQ(gpu.scheduler().deviceWaitIdleCalls(), deviceWaitsBefore);
    EXPECT_EQ(gpu.scheduler().queueWaitIdleCalls(), queueWaitsBefore);
    EXPECT_EQ(gpu.gpuState(), rt::GpuState::Healthy);
    EXPECT_EQ(gpu.vkInstance().validationErrorCount(), 0u);

    const uint64_t usedBeforeExportShutdown =
        gpu.allocator().queryStats().totalUsedBytes;
    isolatedExport.shutdown(rt::GpuTeardownMode::SessionScoped);
    EXPECT_LE(gpu.allocator().queryStats().totalUsedBytes,
              usedBeforeExportShutdown);
    EXPECT_EQ(gpu.scheduler().deviceWaitIdleCalls(), deviceWaitsBefore);
}
