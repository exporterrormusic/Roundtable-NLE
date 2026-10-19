/*
 * ThreadRegistry — which background threads are alive right now.
 *
 * Every WorkerThread is listed from start() until it has been joined, and
 * a one-shot task can list itself for its duration with a Scope. The
 * shutdown reports read it: a slow phase or a forced exit names the
 * threads still running, so a hung join is no longer anonymous.
 *
 * Thread-safe. The state is never destroyed, so it can still be read
 * while static destructors run at process exit.
 */
#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace rt {

class ThreadRegistry
{
public:
    struct Entry {
        uint64_t id{0};
        std::string name;
        std::chrono::steady_clock::time_point started;
    };

    /// List a thread; returns the id to remove it with.
    [[nodiscard]] static uint64_t add(std::string name);
    static void remove(uint64_t id) noexcept;

    [[nodiscard]] static std::vector<Entry> live();
    /// "name (3.2 s), other (0.4 s)" or "none". Waits at most `lockWait`
    /// for the registry lock (a hang report must not hang).
    [[nodiscard]] static std::string describeLive(
        std::chrono::milliseconds lockWait = std::chrono::milliseconds(100));

    /// Lists the calling one-shot task for the lifetime of the Scope.
    class Scope
    {
    public:
        explicit Scope(std::string name) : m_id(add(std::move(name))) {}
        ~Scope() { remove(m_id); }
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;

    private:
        uint64_t m_id;
    };

private:
    ThreadRegistry() = delete;
};

} // namespace rt
