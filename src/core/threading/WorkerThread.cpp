#include "threading/WorkerThread.h"

#include "threading/ThreadRegistry.h"

#include <spdlog/spdlog.h>

#include <chrono>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace rt {

void setCurrentThreadName(const std::string& name)
{
#ifdef _WIN32
    const int length = MultiByteToWideChar(CP_UTF8, 0, name.c_str(), -1, nullptr, 0);
    if (length <= 0) return;
    std::wstring wide(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, name.c_str(), -1, wide.data(), length);
    SetThreadDescription(GetCurrentThread(), wide.c_str());
#else
    (void)name;
#endif
}

WorkerThread::WorkerThread(WorkerThread&& other) noexcept
    : m_thread(std::move(other.m_thread))
    , m_name(std::move(other.m_name))
    , m_registryId(std::exchange(other.m_registryId, 0))
{
}

WorkerThread& WorkerThread::operator=(WorkerThread&& other) noexcept
{
    if (this != &other) {
        stop();
        m_thread = std::move(other.m_thread);
        m_name = std::move(other.m_name);
        m_registryId = std::exchange(other.m_registryId, 0);
    }
    return *this;
}

void WorkerThread::start(std::string name, Body body)
{
    stop();
    m_name = std::move(name);
    m_registryId = ThreadRegistry::add(m_name);
    m_thread = std::jthread([threadName = m_name, body = std::move(body)](std::stop_token token) {
        setCurrentThreadName(threadName);
        body(std::move(token));
    });
}

void WorkerThread::requestStop() noexcept
{
    m_thread.request_stop();
}

void WorkerThread::stop()
{
    if (m_thread.joinable()) {
        m_thread.request_stop();
        if (m_thread.get_id() == std::this_thread::get_id()) {
            // Joining itself would throw; the thread ends on its own.
            spdlog::error("[THREAD] '{}' asked to stop itself; detaching", m_name);
            m_thread.detach();
        } else {
            const auto started = std::chrono::steady_clock::now();
            m_thread.join();
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started).count();
            if (ms >= kSlowStopMs)
                spdlog::warn("[THREAD] '{}' took {} ms to stop", m_name, ms);
        }
    }
    ThreadRegistry::remove(std::exchange(m_registryId, 0));
}

} // namespace rt
