/*
 * WorkerThread — one long-lived background thread.
 *
 * start(name, body) runs body(std::stop_token) on a std::jthread named
 * `name` (visible in debuggers and crash stacks) and lists it in the
 * ThreadRegistry until it has been joined. stop() asks it to stop and
 * joins; it is idempotent, also runs from the destructor, and logs a
 * warning when the join takes longer than a second. join() waits for a
 * thread that ends on its own (a one-shot job) without asking it to stop.
 *
 * Waiting: a worker that sleeps until it has work waits with
 *     std::condition_variable_any::wait(lock, stopToken, predicate)
 * which wakes for stop() itself. Never stop a worker by storing a
 * "running" flag outside the mutex its wait uses: a notify that lands
 * between the worker's check and its sleep is lost, and join() then
 * blocks forever. The same goes for handing it work: change what the
 * predicate reads under the mutex, then notify.
 *
 * The app's threading model — which tool for which job:
 *   - A long-lived loop (a cache writer, a decoder pool, a pipeline stage,
 *     a panel's renderer): a WorkerThread member, named. Stop it in the
 *     owner's destructor BEFORE the members its loop uses are destroyed
 *     (a member declared earlier is destroyed later).
 *   - A UI media job whose result goes back to a widget (decode, probe,
 *     waveform, thumbnail): MediaTaskQueue (ui/MediaTaskQueue.h), with an
 *     owner id to cancel by, deduplicated by key.
 *   - A one-shot job the owner waits for (scan, parse, detection): a
 *     WorkerThread and join(). Work running on a thread you don't own
 *     (std::async, QThreadPool, a QThread worker): list it for its
 *     duration with ThreadRegistry::Scope so a hang report can name it.
 *   - Results to the UI thread: QMetaObject::invokeMethod with a context
 *     object, and a QPointer guard checked on the UI thread before use.
 *   - Shutdown (core/ShutdownPhases.h, ui/App.cpp ~App): phase 1 stops
 *     threads that use shared services (media tasks, export, playback,
 *     compositor, audio, character scan); phase 2 drops the playback
 *     controller and MediaPool; phase 3 destroys the widgets, each stopping its
 *     own workers; phase 4 the GPU. crash_log.txt then gets
 *     "=== SESSION END === clean shutdown ..." or, from the forced-exit
 *     timers, "SHUTDOWN HANG: ..." with the live threads and UI stack.
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
    /// Wait for the thread to finish by itself (no stop request, no
    /// slow-stop warning). Safe to call more than once.
    void join();

    [[nodiscard]] bool running() const noexcept { return m_thread.joinable(); }
    [[nodiscard]] const std::string& name() const noexcept { return m_name; }

    /// Joins slower than this are logged as warnings.
    static constexpr int kSlowStopMs = 1000;

private:
    void finish(bool stopping);

    std::jthread m_thread;
    std::string m_name;
    uint64_t m_registryId{0};
};

/// Names the calling thread for debuggers and crash stacks (Windows).
void setCurrentThreadName(const std::string& name);

} // namespace rt
