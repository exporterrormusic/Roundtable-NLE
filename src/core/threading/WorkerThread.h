/*
 * WorkerThread — one long-lived background thread.
 *
 * start(name, body) runs body(std::stop_token) on a std::jthread named
 * `name` (visible in debuggers and crash stacks) and lists it in the
 * ThreadRegistry until it has been joined. stop() asks it to stop and
 * joins; it is idempotent, also runs from the destructor, and logs a
 * warning when the join takes longer than a second.
 *
 * Waiting: a worker that sleeps until it has work waits with
 *     std::condition_variable_any::wait(lock, stopToken, predicate)
 * which wakes for stop() itself. Never stop a worker by storing a
 * "running" flag outside the mutex its wait uses: a notify that lands
 * between the worker's check and its sleep is lost, and join() then
 * blocks forever.
 */
#pragma once

#include <cstdint>
#include <functional>
#include <stop_token>
#include <string>
#include <thread>

namespace rt {

class WorkerThread
{
public:
    using Body = std::function<void(std::stop_token)>;

    WorkerThread() = default;
    ~WorkerThread() { stop(); }

    WorkerThread(WorkerThread&& other) noexcept;
    WorkerThread& operator=(WorkerThread&& other) noexcept;
    WorkerThread(const WorkerThread&) = delete;
    WorkerThread& operator=(const WorkerThread&) = delete;

    /// Run `body` on a new thread. A running thread is stopped first.
    void start(std::string name, Body body);

    /// Ask the thread to stop (fires its stop_token) without waiting.
    void requestStop() noexcept;
    /// requestStop() and join. Safe to call more than once.
    void stop();

    [[nodiscard]] bool running() const noexcept { return m_thread.joinable(); }
    [[nodiscard]] const std::string& name() const noexcept { return m_name; }

    /// Joins slower than this are logged as warnings.
    static constexpr int kSlowStopMs = 1000;

private:
    std::jthread m_thread;
    std::string m_name;
    uint64_t m_registryId{0};
};

/// Names the calling thread for debuggers and crash stacks (Windows).
void setCurrentThreadName(const std::string& name);

} // namespace rt
