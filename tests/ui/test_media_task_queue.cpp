#include "MediaTaskQueue.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

namespace {

template <typename Future>
void expectReady(Future& future)
{
    ASSERT_EQ(future.wait_for(2s), std::future_status::ready);
}

} // namespace

TEST(MediaTaskQueueTest, DeduplicatesOneDecodeAcrossOwners)
{
    rt::MediaTaskQueue queue(1);
    const auto firstOwner = queue.createOwner();
    const auto secondOwner = queue.createOwner();

    std::mutex gateMutex;
    std::condition_variable gate;
    bool release = false;
    std::promise<void> startedPromise;
    auto started = startedPromise.get_future();
    std::atomic<int> workCalls{0};
    std::promise<int> firstPromise;
    std::promise<int> secondPromise;
    auto first = firstPromise.get_future();
    auto second = secondPromise.get_future();

    ASSERT_TRUE(queue.submit<int>(
        firstOwner, "waveform:file.wav:480", rt::MediaTaskQueue::Priority::Background,
        [&](std::stop_token) {
            ++workCalls;
            startedPromise.set_value();
            std::unique_lock lock(gateMutex);
            gate.wait(lock, [&] { return release; });
            return rt::MediaTaskQueue::Result<int>::success(42);
        },
        [&](rt::MediaTaskQueue::Result<int> result) {
            firstPromise.set_value(result.succeeded() ? *result.value : -1);
        }));

    expectReady(started);
    ASSERT_TRUE(queue.submit<int>(
        secondOwner, "waveform:file.wav:480", rt::MediaTaskQueue::Priority::Visible,
        [&](std::stop_token) {
            ++workCalls;
            return rt::MediaTaskQueue::Result<int>::success(99);
        },
        [&](rt::MediaTaskQueue::Result<int> result) {
            secondPromise.set_value(result.succeeded() ? *result.value : -1);
        }));

    {
        std::lock_guard lock(gateMutex);
        release = true;
    }
    gate.notify_one();

    expectReady(first);
    expectReady(second);
    EXPECT_EQ(first.get(), 42);
    EXPECT_EQ(second.get(), 42);
    EXPECT_EQ(workCalls.load(), 1);

    const auto stats = queue.statistics();
    EXPECT_EQ(stats.submitted, 1u);
    EXPECT_EQ(stats.deduplicated, 1u);
    EXPECT_EQ(stats.completed, 1u);
}

TEST(MediaTaskQueueTest, CancelOwnerRemovesItsQueuedWork)
{
    rt::MediaTaskQueue queue(1);
    const auto blockerOwner = queue.createOwner();
    const auto canceledOwner = queue.createOwner();

    std::promise<void> releasePromise;
    auto release = releasePromise.get_future().share();
    std::promise<void> blockerStartedPromise;
    auto blockerStarted = blockerStartedPromise.get_future();
    std::promise<void> blockerDonePromise;
    auto blockerDone = blockerDonePromise.get_future();
    std::atomic<int> canceledWorkCalls{0};
    std::atomic<int> canceledCallbacks{0};

    ASSERT_TRUE(queue.submit<int>(
        blockerOwner, "blocker", rt::MediaTaskQueue::Priority::Interactive,
        [&](std::stop_token) {
            blockerStartedPromise.set_value();
            release.wait();
            return rt::MediaTaskQueue::Result<int>::success(1);
        },
        [&](rt::MediaTaskQueue::Result<int>) { blockerDonePromise.set_value(); }));
    expectReady(blockerStarted);

    ASSERT_TRUE(queue.submit<int>(
        canceledOwner, "obsolete-thumbnail", rt::MediaTaskQueue::Priority::Background,
        [&](std::stop_token) {
            ++canceledWorkCalls;
            return rt::MediaTaskQueue::Result<int>::success(2);
        },
        [&](rt::MediaTaskQueue::Result<int>) { ++canceledCallbacks; }));

    queue.cancelOwner(canceledOwner);
    releasePromise.set_value();
    expectReady(blockerDone);
    queue.shutdown();

    EXPECT_EQ(canceledWorkCalls.load(), 0);
    EXPECT_EQ(canceledCallbacks.load(), 0);
    EXPECT_GE(queue.statistics().canceled, 1u);
}

TEST(MediaTaskQueueTest, CancelOwnerSignalsRunningWork)
{
    rt::MediaTaskQueue queue(1);
    const auto owner = queue.createOwner();
    std::promise<void> startedPromise;
    auto started = startedPromise.get_future();
    std::promise<void> stoppedPromise;
    auto stopped = stoppedPromise.get_future();
    std::atomic<int> callbacks{0};

    ASSERT_TRUE(queue.submit<int>(
        owner, "running-decode", rt::MediaTaskQueue::Priority::Interactive,
        [&](std::stop_token stop) {
            startedPromise.set_value();
            while (!stop.stop_requested()) std::this_thread::yield();
            stoppedPromise.set_value();
            return rt::MediaTaskQueue::Result<int>::failure("canceled");
        },
        [&](rt::MediaTaskQueue::Result<int>) { ++callbacks; }));

    expectReady(started);
    queue.cancelOwner(owner);
    expectReady(stopped);
    queue.shutdown();

    EXPECT_EQ(callbacks.load(), 0);
}

TEST(MediaTaskQueueTest, RunsInteractiveWorkBeforeBackgroundWork)
{
    rt::MediaTaskQueue queue(1);
    const auto owner = queue.createOwner();

    std::promise<void> releasePromise;
    auto release = releasePromise.get_future().share();
    std::promise<void> blockerStartedPromise;
    auto blockerStarted = blockerStartedPromise.get_future();
    std::promise<void> allDonePromise;
    auto allDone = allDonePromise.get_future();
    std::mutex orderMutex;
    std::vector<std::string> order;
    std::atomic<int> completions{0};
    auto completed = [&](rt::MediaTaskQueue::Result<int>) {
        if (++completions == 3) allDonePromise.set_value();
    };

    ASSERT_TRUE(queue.submit<int>(
        owner, "blocker", rt::MediaTaskQueue::Priority::Interactive,
        [&](std::stop_token) {
            blockerStartedPromise.set_value();
            release.wait();
            std::lock_guard lock(orderMutex);
            order.emplace_back("blocker");
            return rt::MediaTaskQueue::Result<int>::success(0);
        }, completed));
    expectReady(blockerStarted);

    ASSERT_TRUE(queue.submit<int>(
        owner, "background", rt::MediaTaskQueue::Priority::Background,
        [&](std::stop_token) {
            std::lock_guard lock(orderMutex);
            order.emplace_back("background");
            return rt::MediaTaskQueue::Result<int>::success(1);
        }, completed));
    ASSERT_TRUE(queue.submit<int>(
        owner, "interactive", rt::MediaTaskQueue::Priority::Interactive,
        [&](std::stop_token) {
            std::lock_guard lock(orderMutex);
            order.emplace_back("interactive");
            return rt::MediaTaskQueue::Result<int>::success(2);
        }, completed));

    releasePromise.set_value();
    expectReady(allDone);

    ASSERT_EQ(order.size(), 3u);
    EXPECT_EQ(order[0], "blocker");
    EXPECT_EQ(order[1], "interactive");
    EXPECT_EQ(order[2], "background");
}
