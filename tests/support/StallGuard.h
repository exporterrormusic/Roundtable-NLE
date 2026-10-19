/*
 * StallGuard.h — turn a hang into a test failure.
 *
 * A test that loops over something that might block forever (a worker's
 * stop/join) calls tick() once per iteration. If no tick arrives for
 * `limit`, the guard prints what stalled and at which iteration, then
 * aborts the process: a thread blocked in join() cannot be failed any
 * other way, and aborting beats waiting for the ctest timeout.
 */
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>

namespace rt::test {

class StallGuard
{
public:
    explicit StallGuard(std::string what,
                        std::chrono::milliseconds limit = std::chrono::seconds(2))
        : m_what(std::move(what)), m_limit(limit),
          m_thread([this] { watch(); })
    {
    }

    ~StallGuard()
    {
        {
            std::lock_guard lock(m_mutex);
            m_done = true;
        }
        m_cv.notify_one();
        m_thread.join();
    }

    StallGuard(const StallGuard&) = delete;
    StallGuard& operator=(const StallGuard&) = delete;

    void tick() noexcept { m_ticks.fetch_add(1, std::memory_order_relaxed); }

private:
    void watch()
    {
        using clock = std::chrono::steady_clock;
        uint64_t lastTicks = m_ticks.load(std::memory_order_relaxed);
        auto lastProgress = clock::now();
        std::unique_lock lock(m_mutex);
        while (!m_cv.wait_for(lock, std::chrono::milliseconds(50), [this] { return m_done; })) {
            const uint64_t ticks = m_ticks.load(std::memory_order_relaxed);
            if (ticks != lastTicks) {
                lastTicks = ticks;
                lastProgress = clock::now();
            } else if (clock::now() - lastProgress > m_limit) {
                std::fprintf(stderr, "STALL: %s made no progress for %lld ms (iteration %llu)\n",
                             m_what.c_str(), static_cast<long long>(m_limit.count()),
                             static_cast<unsigned long long>(ticks));
                std::fflush(stderr);
                std::abort();
            }
        }
    }

    std::string m_what;
    std::chrono::milliseconds m_limit;
    std::atomic<uint64_t> m_ticks{0};
    std::mutex m_mutex;
    std::condition_variable m_cv;
    bool m_done{false};
    std::thread m_thread;   // last: starts after the members above exist
};

} // namespace rt::test
