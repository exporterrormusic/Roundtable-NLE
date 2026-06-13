#include "GpuResourceManager.h"

#include <gtest/gtest.h>

#include <chrono>

namespace rt {
namespace {

TEST(GpuResourceManagerTest, PlaybackPolicyIsBoundedAndAllowsHeldFrames)
{
    const auto policy = GpuResourceManager::playbackCompositePolicy();

    EXPECT_GT(policy.timeout, std::chrono::nanoseconds{0});
    EXPECT_LE(policy.timeout, std::chrono::milliseconds(16));
    EXPECT_TRUE(policy.allowHeldFrameOnTimeout);
    EXPECT_STREQ(policy.label, "playback-composite-fence");
}

TEST(GpuResourceManagerTest, ExactPolicyIsBoundedButDoesNotPreferHeldFrames)
{
    const auto policy = GpuResourceManager::exactCompositePolicy();

    EXPECT_GT(policy.timeout, GpuResourceManager::playbackCompositePolicy().timeout);
    EXPECT_FALSE(policy.allowHeldFrameOnTimeout);
    EXPECT_STREQ(policy.label, "exact-composite-fence");
}

TEST(GpuResourceManagerTest, DetectsWhenPresentationDrainWouldBeRequired)
{
    GpuResourceManager manager;
    auto* compute = reinterpret_cast<VkQueue>(static_cast<uintptr_t>(0x1));
    auto* graphics = reinterpret_cast<VkQueue>(static_cast<uintptr_t>(0x2));

    EXPECT_FALSE(manager.requiresPresentationDrain(VK_NULL_HANDLE, graphics));
    EXPECT_FALSE(manager.requiresPresentationDrain(compute, VK_NULL_HANDLE));
    EXPECT_FALSE(manager.requiresPresentationDrain(compute, compute));
    EXPECT_TRUE(manager.requiresPresentationDrain(compute, graphics));
}

TEST(GpuResourceManagerTest, TracksTelemetryCountersAndSnapshots)
{
    GpuResourceManager manager;

    manager.noteQueueIdleAvoided();
    manager.noteHeldFrame();
    manager.updateSnapshot({
        256u,
        1024u,
        3u,
        128u,
        512u,
        false
    });

    const auto stats = manager.stats();
    EXPECT_EQ(stats.queueIdleAvoided, 1u);
    EXPECT_EQ(stats.heldFrames, 1u);
    EXPECT_EQ(stats.lastSnapshot.textureBytesUsed, 256u);
    EXPECT_EQ(stats.lastSnapshot.textureBudgetBytes, 1024u);
    EXPECT_EQ(stats.lastSnapshot.textureEntries, 3u);
    EXPECT_EQ(stats.lastSnapshot.stagingBytesUsed, 128u);
    EXPECT_EQ(stats.lastSnapshot.stagingCapacityBytes, 512u);
    EXPECT_FALSE(stats.lastSnapshot.underVramPressure);
}

TEST(GpuResourceManagerTest, InvalidFenceWaitDoesNotBlock)
{
    GpuResourceManager manager;

    const auto outcome = manager.waitForFence(
        VK_NULL_HANDLE,
        VK_NULL_HANDLE,
        GpuResourceManager::playbackCompositePolicy());

    EXPECT_EQ(outcome, GpuWaitOutcome::InvalidHandle);
    EXPECT_STREQ(toString(outcome), "InvalidHandle");
    EXPECT_EQ(manager.stats().boundedFenceWaits, 0u);
}

} // namespace
} // namespace rt
