/*
 * test_worker_thread.cpp — rt::WorkerThread and rt::ThreadRegistry.
 */

#include <gtest/gtest.h>

#include "../support/StallGuard.h"

#include "threading/ThreadRegistry.h"
#include "threading/WorkerThread.h"

#include <spdlog/sinks/ringbuffer_sink.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

using namespace std::chrono_literals;

namespace {

size_t liveNamed(const std::string& prefix)
{
    const auto entries = rt::ThreadRegistry::live();
    return static_cast<size_t>(std::count_if(entries.begin(), entries.end(),
        [&](const rt::ThreadRegistry::Entry& e) { return e.name.rfind(prefix, 0) == 0; }));
}

void idleUntilStopped(std::stop_token stop)
{
    std::mutex mutex;
    std::condition_variable_any cv;
    std::unique_lock lock(mutex);
    cv.wait(lock, stop, [] { return false; });
}

} // namespace

TEST(WorkerThread, StopWakesAWorkerWaitingOnItsStopToken)
{
    // The worker may be anywhere in its first wait when stop() arrives;
    // a stop_token wait never misses it.
    rt::test::StallGuard guard("WorkerThread stop");
    for (int i = 0; i < 2000; ++i) {
        rt::WorkerThread worker;
        worker.start("stop-wakes", idleUntilStopped);
        worker.stop();
        guard.tick();
    }
    EXPECT_EQ(liveNamed("stop-wakes"), 0u);
}

TEST(WorkerThread, RegistryListsAWorkerUntilItIsJoined)
{
    rt::WorkerThread a;
    rt::WorkerThread b;
    a.start("registry-a", idleUntilStopped);
    b.start("registry-b", idleUntilStopped);
    EXPECT_TRUE(a.running());
    EXPECT_EQ(liveNamed("registry-a"), 1u);
    EXPECT_EQ(liveNamed("registry-b"), 1u);

    a.stop();
    EXPECT_FALSE(a.running());
    EXPECT_EQ(liveNamed("registry-a"), 0u);
    EXPECT_EQ(liveNamed("registry-b"), 1u);
    a.stop();   // idempotent
    EXPECT_NE(rt::ThreadRegistry::describeLive().find("registry-b ("), std::string::npos);

    b.stop();
    EXPECT_EQ(liveNamed("registry-b"), 0u);
}

TEST(WorkerThread, MovedAndDestroyedWorkersKeepTheRegistryExact)
{
    {
        std::vector<rt::WorkerThread> workers;
        for (int i = 0; i < 4; ++i) {
            rt::WorkerThread worker;
            worker.start("moved-" + std::to_string(i), idleUntilStopped);
            workers.push_back(std::move(worker));
            EXPECT_FALSE(worker.running());   // NOLINT: moved-from is empty
        }
        EXPECT_EQ(liveNamed("moved-"), 4u);
        rt::WorkerThread replacement;
        replacement.start("moved-replacement", idleUntilStopped);
        workers[0] = std::move(replacement);   // stops moved-0 first
        EXPECT_EQ(liveNamed("moved-0"), 0u);
        EXPECT_EQ(liveNamed("moved-"), 4u);
    }
    EXPECT_EQ(liveNamed("moved-"), 0u);
}

TEST(WorkerThread, StartingAgainStopsThePreviousThread)
{
    rt::WorkerThread worker;
    worker.start("restart-first", idleUntilStopped);
    worker.start("restart-second", idleUntilStopped);
    EXPECT_EQ(liveNamed("restart-first"), 0u);
    EXPECT_EQ(liveNamed("restart-second"), 1u);
    EXPECT_EQ(worker.name(), "restart-second");
}

TEST(WorkerThread, JoinWaitsForAOneShotJobWithoutAskingItToStop)
{
    bool finished = false;
    bool askedToStop = true;
    rt::WorkerThread worker;
    worker.start("one-shot-job", [&](std::stop_token stop) {
        std::this_thread::sleep_for(50ms);
        askedToStop = stop.stop_requested();
        finished = true;
    });
    worker.join();
    EXPECT_TRUE(finished);
    EXPECT_FALSE(askedToStop);
    EXPECT_FALSE(worker.running());
    EXPECT_EQ(liveNamed("one-shot-job"), 0u);
    worker.join();   // idempotent
}

TEST(WorkerThread, ScopeListsAOneShotTask)
{
    {
        rt::ThreadRegistry::Scope scope("one-shot-task");
        EXPECT_EQ(liveNamed("one-shot-task"), 1u);
    }
    EXPECT_EQ(liveNamed("one-shot-task"), 0u);
    EXPECT_EQ(rt::ThreadRegistry::describeLive().find("one-shot-task"), std::string::npos);
}

TEST(WorkerThread, ASlowStopIsLoggedAsAWarning)
{
    auto sink = std::make_shared<spdlog::sinks::ringbuffer_sink_mt>(32);
    auto& sinks = spdlog::default_logger()->sinks();
    sinks.push_back(sink);
    {
        rt::WorkerThread worker;
        worker.start("slow-stopper", [](std::stop_token) {
            std::this_thread::sleep_for(1100ms);   // ignores the stop request
        });
        worker.stop();
    }
    sinks.erase(std::find(sinks.begin(), sinks.end(), sink));

    const auto lines = sink->last_formatted();
    const bool logged = std::any_of(lines.begin(), lines.end(), [](const std::string& line) {
        return line.find("'slow-stopper' took") != std::string::npos
            && line.find("warn") != std::string::npos;
    });
    EXPECT_TRUE(logged);
}

#ifdef _WIN32
TEST(WorkerThread, TheThreadCarriesItsName)
{
    std::wstring seen;
    rt::WorkerThread worker;
    worker.start("named-worker", [&seen](std::stop_token) {
        PWSTR description = nullptr;
        if (SUCCEEDED(GetThreadDescription(GetCurrentThread(), &description))) {
            seen = description;
            LocalFree(description);
        }
    });
    worker.stop();
    EXPECT_EQ(seen, L"named-worker");
}
#endif
