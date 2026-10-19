#include "ShutdownPhases.h"

#include "CrashHandler.h"
#include "HangWatchdog.h"
#include "threading/ThreadRegistry.h"

#include <spdlog/spdlog.h>

#include <chrono>
#include <cstdio>

namespace rt {

namespace {

int64_t steadyMs() noexcept
{
    // +1 so a reading is never 0, which marks "not recorded".
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count() + 1;
}

} // namespace

bool ShutdownManager::advanceTo(ShutdownPhase phase)
{
    const ShutdownPhase cur = m_phase.load(std::memory_order_relaxed);
    if (static_cast<int>(cur) >= static_cast<int>(phase))
        return false;

    const int64_t now = steadyMs();
    if (cur != ShutdownPhase::Running) {
        const int64_t started = m_phaseStartedMs[static_cast<size_t>(cur)].load();
        if (started != 0 && now - started > kSlowPhaseMs) {
            spdlog::warn("[SHUTDOWN] {} took {} ms; still running: {}",
                         phaseName(cur), now - started, ThreadRegistry::describeLive());
        }
    }
    m_phaseStartedMs[static_cast<size_t>(phase)].store(now);
    m_phase.store(phase, std::memory_order_release);
    return true;
}

void ShutdownManager::markCloseStarted() noexcept
{
    int64_t expected = 0;
    m_closeStartedMs.compare_exchange_strong(expected, steadyMs());
}

const char* ShutdownManager::phaseName(ShutdownPhase phase) noexcept
{
    switch (phase) {
    case ShutdownPhase::Running:            return "closing";
    case ShutdownPhase::Phase1_StopThreads: return "stopping threads";
    case ShutdownPhase::Phase2_Disconnect:  return "disconnecting";
    case ShutdownPhase::Phase3_DestroyQt:   return "destroying widgets";
    case ShutdownPhase::Phase4_DestroyGpu:  return "destroying GPU";
    case ShutdownPhase::Phase5_Done:        return "done";
    }
    return "?";
}

int64_t ShutdownManager::msSinceCloseStarted() const noexcept
{
    const int64_t started = m_closeStartedMs.load();
    return started == 0 ? -1 : steadyMs() - started;
}

int64_t ShutdownManager::msInCurrentPhase() const noexcept
{
    const ShutdownPhase cur = currentPhase();
    const int64_t started = cur == ShutdownPhase::Running
        ? m_closeStartedMs.load()
        : m_phaseStartedMs[static_cast<size_t>(cur)].load();
    return started == 0 ? 0 : steadyMs() - started;
}

std::string ShutdownManager::phaseTimes() const
{
    // Each phase runs until the next one starts; "closing" runs from the
    // close until phase 1. The last phase entered runs until now.
    std::array<int64_t, kPhaseCount> starts{};
    starts[0] = m_closeStartedMs.load();
    for (size_t i = 1; i < kPhaseCount; ++i)
        starts[i] = m_phaseStartedMs[i].load();

    std::string out;
    const size_t last = static_cast<size_t>(currentPhase());
    for (size_t i = 0; i < kPhaseCount && i < last; ++i) {
        if (starts[i] == 0) continue;
        size_t next = i + 1;
        while (next <= last && starts[next] == 0) ++next;
        if (next > last) break;
        if (!out.empty()) out += ", ";
        out += phaseName(static_cast<ShutdownPhase>(i));
        out += ' ';
        out += std::to_string(starts[next] - starts[i]);
    }
    return out.empty() ? out : out + " ms";
}

void logCleanShutdown()
{
    const auto& sm = ShutdownManager::instance();
    const int64_t sinceClose = sm.msSinceCloseStarted();
    std::string line = "=== SESSION END === clean shutdown";
    if (sinceClose >= 0)
        line += " " + std::to_string(sinceClose) + " ms after close";
    const std::string phases = sm.phaseTimes();
    if (!phases.empty())
        line += " (" + phases + ")";
    CrashHandler::writeCrashLog(line);
}

void logForcedShutdown(int afterSeconds)
{
    const auto& sm = ShutdownManager::instance();
    char head[256];
    std::snprintf(head, sizeof(head),
                  "SHUTDOWN HANG: forced exit %d s after close, stuck in '%s' for %.1f s",
                  afterSeconds, ShutdownManager::phaseName(sm.currentPhase()),
                  sm.msInCurrentPhase() / 1000.0);
    CrashHandler::writeCrashLog(head);
    CrashHandler::writeCrashLog("SHUTDOWN HANG: still running: " + ThreadRegistry::describeLive());
    HangWatchdog::logUiThreadStack("SHUTDOWN HANG: UI thread stack");
}

} // namespace rt
