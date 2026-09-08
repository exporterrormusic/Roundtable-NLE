#include <gtest/gtest.h>

#include "PerformanceProfile.h"
#include "cache/CachePolicy.h"

namespace {

constexpr size_t GiB(size_t n) { return n * 1024ull * 1024ull * 1024ull; }

TEST(PerformanceProfileTest, WorkstationDoesNotHoardDecodedFrames)
{
    const auto profile = rt::PerformanceProfile::forMachine(
        GiB(24), GiB(64), 24, false);
    EXPECT_EQ(profile.gpuTexCacheBudgetBytes, 0u);
    EXPECT_EQ(profile.gpuTexMaxEntries, 0u);
    EXPECT_EQ(profile.frameCacheBudgetBytes, 0u);
    EXPECT_EQ(profile.frameCacheMaxEntries, 0u);
}

TEST(PerformanceProfileTest, DefaultVramWorkingSetIsBounded)
{
    const auto previous = rt::perfProfile();
    rt::setPerfProfile({});

    rt::CachePolicy policy;
    EXPECT_EQ(policy.recommendedGpuTexCacheBudget(GiB(24)), GiB(1));
    EXPECT_EQ(policy.recommendedGpuTexCacheMaxEntries(GiB(24)), 120u);
    EXPECT_EQ(policy.recommendedFrameCacheMaxEntries(GiB(24)), 168u);
    EXPECT_LT(policy.recommendedGpuTexCacheBudget(GiB(8)), GiB(1));

    rt::setPerfProfile(previous);
}

} // namespace
