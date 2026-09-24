// HangWatchdog: a stalled watched thread gets its stack logged to
// crash_log.txt, followed by a "recovered" line once heartbeats resume.

#include <gtest/gtest.h>

#include "CrashHandler.h"
#include "HangWatchdog.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>

using namespace rt;

#if defined(_WIN32) && defined(_M_X64)

TEST(HangWatchdog, LogsStalledThreadStackAndRecovery)
{
    const auto dir = std::filesystem::temp_directory_path() / "rt_hang_watchdog_test";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    CrashHandler::install(dir);

    HangWatchdog::start();          // watches this thread
    HangWatchdog::heartbeat();      // arm

    // Stall past the 5 s threshold without heartbeating.
    std::this_thread::sleep_for(std::chrono::milliseconds(6500));

    HangWatchdog::heartbeat();      // recover
    std::this_thread::sleep_for(std::chrono::milliseconds(800));
    HangWatchdog::stop();

    std::ifstream in(dir / "crash_log.txt");
    std::stringstream ss;
    ss << in.rdbuf();
    const std::string log = ss.str();

    EXPECT_NE(log.find("UI HANG: main thread unresponsive"), std::string::npos) << log;
    EXPECT_NE(log.find("UI HANG: main thread recovered after"), std::string::npos) << log;
    // The sampled stack must show the thread parked in the sleep syscall —
    // proves the unwind walked the watched thread, not the watchdog's own.
    EXPECT_NE(log.find("NtDelayExecution"), std::string::npos) << log;
}

#endif
