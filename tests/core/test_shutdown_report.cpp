/*
 * test_shutdown_report.cpp — the shutdown reports in crash_log.txt.
 *
 * ShutdownManager is a process-wide singleton whose phases only move
 * forward, so the whole shutdown is driven once, in order, in one test.
 */

#include <gtest/gtest.h>

#include "CrashHandler.h"
#include "HangWatchdog.h"
#include "ShutdownPhases.h"
#include "threading/WorkerThread.h"

#include <spdlog/sinks/ringbuffer_sink.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

using namespace std::chrono_literals;

namespace {

std::string readLog(const std::filesystem::path& dir)
{
    std::ifstream in(dir / "crash_log.txt");
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

} // namespace

TEST(ShutdownReport, ForcedAndCleanShutdownReportsTellWhereTheTimeWent)
{
    const auto dir = std::filesystem::temp_directory_path() / "rt_shutdown_report_test";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    rt::CrashHandler::install(dir);
    rt::HangWatchdog::start();   // records this thread as the UI thread

    auto sink = std::make_shared<spdlog::sinks::ringbuffer_sink_mt>(32);
    auto& sinks = spdlog::default_logger()->sinks();
    sinks.push_back(sink);

    auto& sm = rt::ShutdownManager::instance();
    sm.markCloseStarted();
    sm.advanceTo(rt::ShutdownPhase::Phase1_StopThreads);

    // A worker that never stops, and a forced-exit report taken from
    // another thread while this ("UI") thread is blocked.
    rt::WorkerThread stuck;
    stuck.start("stuck-in-test", [](std::stop_token stop) {
        std::mutex m;
        std::condition_variable_any cv;
        std::unique_lock lock(m);
        cv.wait(lock, stop, [] { return false; });
    });
    std::thread timer([] {
        std::this_thread::sleep_for(200ms);
        rt::logForcedShutdown(8);
    });
    std::this_thread::sleep_for(1200ms);   // also makes phase 1 "slow"
    timer.join();
    stuck.stop();

    sm.advanceTo(rt::ShutdownPhase::Phase2_Disconnect);   // leaving a slow phase warns
    sm.advanceTo(rt::ShutdownPhase::Phase3_DestroyQt);
    sm.advanceTo(rt::ShutdownPhase::Phase4_DestroyGpu);
    sm.advanceTo(rt::ShutdownPhase::Phase5_Done);
    rt::logCleanShutdown();

    sinks.erase(std::find(sinks.begin(), sinks.end(), sink));
    rt::HangWatchdog::stop();

    const std::string log = readLog(dir);
    EXPECT_NE(log.find("SHUTDOWN HANG: forced exit 8 s after close, stuck in 'stopping threads'"),
              std::string::npos) << log;
    EXPECT_NE(log.find("SHUTDOWN HANG: still running: "), std::string::npos) << log;
    EXPECT_NE(log.find("stuck-in-test ("), std::string::npos) << log;
    EXPECT_NE(log.find("SHUTDOWN HANG: UI thread stack"), std::string::npos) << log;
    // The stack is this thread's, parked in its sleep.
    const bool sleepFrame = log.find("NtDelayExecution") != std::string::npos
                         || log.find("ZwDelayExecution") != std::string::npos;
    EXPECT_TRUE(sleepFrame) << log;

    EXPECT_NE(log.find("=== SESSION END === clean shutdown "), std::string::npos) << log;
    EXPECT_NE(log.find(" ms after close (closing "), std::string::npos) << log;
    EXPECT_NE(log.find("stopping threads "), std::string::npos) << log;
    EXPECT_NE(log.find("destroying GPU "), std::string::npos) << log;

    const auto lines = sink->last_formatted();
    const bool slowPhaseWarned = std::any_of(lines.begin(), lines.end(), [](const std::string& l) {
        return l.find("[SHUTDOWN] stopping threads took") != std::string::npos;
    });
    EXPECT_TRUE(slowPhaseWarned);
}
