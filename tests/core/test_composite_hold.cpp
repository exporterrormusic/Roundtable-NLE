/*
 * test_composite_hold.cpp — what the compositor holds between frames:
 * the settle window, the stale fallback, static-stack reuse and every way
 * of forgetting them (CompositeHold).
 */

#include <gtest/gtest.h>

#include "Constants.h"
#include "cache/FrameCache.h"
#include "playback/CompositeHold.h"

#include <chrono>
#include <memory>
#include <vector>

using namespace rt;
using namespace std::chrono_literals;

namespace {

std::shared_ptr<CachedFrame> bgraFrame()
{
    auto frame = std::make_shared<CachedFrame>();
    frame->width = 16;
    frame->height = 9;
    frame->pixels.assign(16 * 9 * 4, 0);
    return frame;
}

std::shared_ptr<CachedFrame> nv12Frame()
{
    auto frame = std::make_shared<CachedFrame>();
    frame->width = 16;
    frame->height = 9;
    frame->nv12 = std::shared_ptr<const uint8_t>(new uint8_t[216](),
                                                 std::default_delete<uint8_t[]>());
    frame->nv12Size = 216;
    return frame;
}

const std::vector<uint64_t> kShotA{1, 2};
const std::vector<uint64_t> kShotB{1, 3};

auto ids(const std::vector<uint64_t>& v)
{
    return [v] { return v; };
}

} // namespace

TEST(CompositeHold, FirstIncompleteFrameArmsTheSettleWindowWithNothingToHold)
{
    CompositeHold hold;
    const auto t0 = CompositeHold::Clock::now();
    auto first = hold.settle(t0, 1000, ids(kShotA));
    EXPECT_TRUE(first.withinWindow);
    EXPECT_TRUE(first.firstView);
    EXPECT_EQ(first.frame, nullptr);

    auto later = hold.settle(t0 + 100ms, 1000, ids(kShotA));
    EXPECT_TRUE(later.withinWindow);
    EXPECT_FALSE(later.firstView);

    auto expired = hold.settle(t0 + 300ms, 1000, ids(kShotA));
    EXPECT_FALSE(expired.withinWindow) << "the window is 250 ms";
}

TEST(CompositeHold, HoldsTheLastCompleteFrameOnlyForTheSameShotNearby)
{
    CompositeHold hold;
    auto good = bgraFrame();
    hold.rememberFresh(good, 1000, kShotA, std::nullopt, /*complete=*/true);
    const auto now = CompositeHold::Clock::now();

    auto same = hold.settle(now, 1800, ids(kShotA));
    EXPECT_TRUE(same.withinWindow);
    EXPECT_EQ(same.frame, good);

    auto otherShot = hold.settle(now, 1800, ids(kShotB));
    EXPECT_FALSE(otherShot.withinWindow);
    EXPECT_EQ(otherShot.frame, nullptr);

    auto farAway = hold.settle(now, 1000 + 2 * kTicksPerSecond, ids(kShotA));
    EXPECT_FALSE(farAway.withinWindow) << "a seek is not the same shot";

    auto expired = hold.settle(now + 300ms, 1800, ids(kShotA));
    EXPECT_FALSE(expired.withinWindow);
}

TEST(CompositeHold, IncompleteCompositesDoNotRestartTheWindow)
{
    CompositeHold hold;
    const auto t0 = CompositeHold::Clock::now();
    ASSERT_TRUE(hold.settle(t0, 1000, ids(kShotA)).firstView);
    hold.rememberFresh(bgraFrame(), 1000, kShotA, std::nullopt, /*complete=*/false);
    EXPECT_FALSE(hold.settle(t0 + 300ms, 1000, ids(kShotA)).withinWindow)
        << "a partial composite must not extend the hold";
}

TEST(CompositeHold, ClipIdsAreOnlyAskedForWhenThereIsAFrameToCompare)
{
    CompositeHold hold;
    int asked = 0;
    auto counting = [&] { ++asked; return kShotA; };
    (void)hold.settle(CompositeHold::Clock::now(), 1000, counting);
    EXPECT_EQ(asked, 0);
    hold.rememberFresh(bgraFrame(), 1000, kShotA, std::nullopt, true);
    (void)hold.settle(CompositeHold::Clock::now(), 1000, counting);
    EXPECT_EQ(asked, 1);
}

TEST(CompositeHold, StaleFallbackReusesOnlyACloseFrameOfTheSameShot)
{
    CompositeHold hold;
    EXPECT_EQ(hold.staleFallback(1000, kShotA).kind, CompositeHold::Stale::Kind::None);

    auto good = bgraFrame();
    hold.rememberFresh(good, 10000, kShotA, std::nullopt, true);

    auto close = hold.staleFallback(10000 + CompositeHold::kMaxStaleTicks, kShotA);
    EXPECT_EQ(close.kind, CompositeHold::Stale::Kind::Reuse);
    EXPECT_EQ(close.frame, good);
    EXPECT_EQ(close.delta, CompositeHold::kMaxStaleTicks);

    auto old = hold.staleFallback(10000 + CompositeHold::kMaxStaleTicks + 1, kShotA);
    EXPECT_EQ(old.kind, CompositeHold::Stale::Kind::TooOld);
    EXPECT_EQ(old.frame, nullptr);
    EXPECT_EQ(old.lastTick, 10000);
    EXPECT_EQ(hold.lastGood(), good) << "too old is not forgotten";

    auto otherShot = hold.staleFallback(10000, kShotB);
    EXPECT_EQ(otherShot.kind, CompositeHold::Stale::Kind::DifferentShot);
    EXPECT_EQ(hold.lastGood(), nullptr) << "another shot's frame is dropped";
}

TEST(CompositeHold, StaticStacksAreReusedByKey)
{
    CompositeHold hold;
    auto frame = bgraFrame();
    hold.rememberFresh(frame, 1000, kShotA, 42u, true);
    EXPECT_EQ(hold.staticHit(43, 2000, kShotB), nullptr);
    EXPECT_EQ(hold.staticHit(42, 2000, kShotB), frame);
    // The hit is now the last good frame, for tick 2000 and shot B.
    auto stale = hold.staleFallback(2000, kShotB);
    EXPECT_EQ(stale.kind, CompositeHold::Stale::Kind::Reuse);
    EXPECT_EQ(stale.lastTick, 2000);

    hold.rememberFresh(bgraFrame(), 3000, kShotA, std::nullopt, true);
    EXPECT_EQ(hold.staticHit(42, 3000, kShotA), nullptr)
        << "a composite without a static key forgets the static frame";
}

TEST(CompositeHold, Nv12FramesNeverBecomeTheLastGoodFrame)
{
    CompositeHold hold;
    auto good = bgraFrame();
    hold.rememberFresh(good, 1000, kShotA, std::nullopt, true);
    auto nv12 = nv12Frame();
    hold.rememberStaticOnly(nv12, 7u);
    EXPECT_EQ(hold.staticHit(7, 1500, kShotB), nv12);
    EXPECT_EQ(hold.lastGood(), good);
    hold.rememberStaticOnly(nv12, std::nullopt);
    EXPECT_EQ(hold.staticHit(7, 1500, kShotB), nullptr);
}

TEST(CompositeHold, RememberingTheSameFrameAgainIsReported)
{
    CompositeHold hold;
    auto frame = bgraFrame();
    EXPECT_FALSE(hold.rememberFresh(frame, 1000, kShotA, std::nullopt, true));
    EXPECT_TRUE(hold.rememberFresh(frame, 1000, kShotA, std::nullopt, true));
}

TEST(CompositeHold, AHeldFrameIsRecognisedOnlyAtAnotherTick)
{
    CompositeHold hold;
    auto frame = bgraFrame();
    EXPECT_FALSE(hold.isHeldFromAnotherTick(frame, 1000));
    hold.rememberFresh(frame, 1000, kShotA, std::nullopt, true);
    EXPECT_FALSE(hold.isHeldFromAnotherTick(frame, 1000)) << "its own tick";
    EXPECT_TRUE(hold.isHeldFromAnotherTick(frame, 1800));
    EXPECT_FALSE(hold.isHeldFromAnotherTick(bgraFrame(), 1800)) << "another frame";
}

TEST(CompositeHold, AnAdoptedSegmentFrameKeepsThePreviousClipIds)
{
    CompositeHold hold;
    hold.rememberFresh(bgraFrame(), 1000, kShotA, std::nullopt, true);
    auto segment = bgraFrame();
    hold.adoptLastGood(segment, 1100);
    auto stale = hold.staleFallback(1100, kShotA);
    EXPECT_EQ(stale.kind, CompositeHold::Stale::Kind::Reuse);
    EXPECT_EQ(stale.frame, segment);
}

TEST(CompositeHold, ClearAllForgetsEverythingAndRearmsTheWindow)
{
    CompositeHold hold;
    hold.rememberFresh(bgraFrame(), 1000, kShotA, 42u, true);
    hold.clearAll();
    EXPECT_EQ(hold.lastGood(), nullptr);
    EXPECT_EQ(hold.staticHit(42, 1000, kShotA), nullptr);
    EXPECT_TRUE(hold.settle(CompositeHold::Clock::now(), 1000, ids(kShotA)).firstView);
}

TEST(CompositeHold, ClearingLastGoodKeepsTheStaticFrame)
{
    CompositeHold hold;
    auto frame = bgraFrame();
    hold.rememberFresh(frame, 1000, kShotA, 42u, true);
    hold.clearLastGoodAndSettle();
    EXPECT_EQ(hold.lastGood(), nullptr);
    EXPECT_TRUE(hold.settle(CompositeHold::Clock::now(), 1000, ids(kShotA)).firstView);
    EXPECT_EQ(hold.staticHit(42, 1000, kShotA), frame);
}

TEST(CompositeHold, RangeInvalidationDropsTheLastGoodFrameOnlyInsideTheRange)
{
    CompositeHold hold;
    auto frame = bgraFrame();
    hold.rememberFresh(frame, 5000, kShotA, 42u, true);
    hold.invalidateRange(6000, 9000);
    EXPECT_EQ(hold.lastGood(), frame);
    EXPECT_EQ(hold.staticHit(42, 5000, kShotA), nullptr) << "the static frame always goes";
    EXPECT_FALSE(hold.settle(CompositeHold::Clock::now(), 5000, ids(kShotA)).firstView);

    hold.invalidateRange(4000, 5000);
    EXPECT_EQ(hold.lastGood(), nullptr);
    EXPECT_TRUE(hold.settle(CompositeHold::Clock::now(), 5000, ids(kShotA)).firstView);
}

TEST(CompositeHold, TheCpuCopyOfTheLastGoodFrameHasNoGpuHandles)
{
    CompositeHold hold;
    auto frame = bgraFrame();
    frame->gpuReady = true;
    frame->gpuImageView = 11;
    frame->gpuSampler = 12;
    frame->gpuSemaphore = 13;
    hold.rememberFresh(frame, 1000, kShotA, std::nullopt, true);
    auto cpu = hold.lastGoodWithoutGpu();
    ASSERT_EQ(cpu, frame);
    EXPECT_FALSE(cpu->gpuReady);
    EXPECT_EQ(cpu->gpuImageView, 0u);
    EXPECT_EQ(cpu->gpuSampler, 0u);
    EXPECT_EQ(cpu->gpuSemaphore.load(), 0u);
}
