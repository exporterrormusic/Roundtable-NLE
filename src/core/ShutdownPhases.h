/*
 * ShutdownPhases.h — Ordered shutdown phase coordinator.
 *
 * Replaces ad-hoc teardown with a defined phase system so no component
 * is destroyed while another still references it.
 *
 * Usage:
 *   auto& sm = ShutdownManager::instance();
 *   sm.advanceTo(ShutdownPhase::Phase1_StopThreads);
 *   // ... stop threads ...
 *   sm.advanceTo(ShutdownPhase::Phase2_Disconnect);
 *   // ... disconnect signals ...
 *
 * It also times the shutdown so it can be reported (crash_log.txt):
 *   - markCloseStarted() when the main window has accepted its close;
 *   - each advanceTo() records when the phase starts, and warns (with the
 *     threads still running) when the phase it leaves took over a second;
 *   - logCleanShutdown() at the end of teardown writes
 *     "=== SESSION END === clean shutdown ..." with the phase times;
 *   - logForcedShutdown() from the forced-exit timers writes
 *     "SHUTDOWN HANG: ..." with the phase, the live threads and the UI
 *     thread's stack, before the process is killed.
 *
 * Thread-safe: the phase and times are atomics; advanceTo() runs on the
 * UI thread, the reports may run on a timer thread.
 */

#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <string>

namespace rt {

enum class ShutdownPhase : int {
    Running = 0,            // Normal operation
    Phase1_StopThreads,     // Stop background threads (FrameProducer, audio, prefetch)
    Phase2_Disconnect,      // Disconnect Qt signals, null back-pointers
    Phase3_DestroyQt,       // Destroy QWidget tree
    Phase4_DestroyGpu,      // Destroy GPU resources (VkDevice, VMA)
    Phase5_Done             // Complete
};

/// Global shutdown coordinator — singleton.
class ShutdownManager {
public:
    static ShutdownManager& instance();

    /// True if any shutdown phase has been entered.
    bool isShuttingDown() const {
        return m_phase.load() != ShutdownPhase::Running;
    }

    /// Current shutdown phase.
    ShutdownPhase currentPhase() const {
        return m_phase.load(std::memory_order_acquire);
    }

    /// True if we have passed (or are at) the given phase.
    bool havePassed(ShutdownPhase phase) const {
        return static_cast<int>(currentPhase()) >= static_cast<int>(phase);
    }

    /// Advance to next phase. Returns false if already past that phase.
    /// Idempotent — advancing to the same phase twice is a no-op.
    bool advanceTo(ShutdownPhase phase);

    /// The main window accepted its close; the shutdown clock starts.
    /// Only the first call counts.
    void markCloseStarted() noexcept;

    [[nodiscard]] static const char* phaseName(ShutdownPhase phase) noexcept;
    /// Milliseconds since markCloseStarted(), or -1 if it wasn't called.
    [[nodiscard]] int64_t msSinceCloseStarted() const noexcept;
    /// Milliseconds the current phase has been running.
    [[nodiscard]] int64_t msInCurrentPhase() const noexcept;
    /// "closing 120, stopping threads 40, ... ms" for the phases entered.
    [[nodiscard]] std::string phaseTimes() const;

    /// Phases slower than this are logged as warnings.
    static constexpr int64_t kSlowPhaseMs = 1000;

private:
    ShutdownManager() = default;
    ~ShutdownManager() = default;
    ShutdownManager(const ShutdownManager&) = delete;
    ShutdownManager& operator=(const ShutdownManager&) = delete;

    static constexpr size_t kPhaseCount = 6;

    std::atomic<ShutdownPhase> m_phase{ShutdownPhase::Running};
    std::atomic<int64_t> m_closeStartedMs{0};             // 0 = not marked
    std::array<std::atomic<int64_t>, kPhaseCount> m_phaseStartedMs{};
};

inline ShutdownManager& ShutdownManager::instance()
{
    static ShutdownManager s_instance;
    return s_instance;
}

/// End of a clean teardown: appends "=== SESSION END === clean shutdown
/// ..." with the total time and the phase times to crash_log.txt.
void logCleanShutdown();

/// From a forced-exit timer, just before the process is killed: appends
/// "SHUTDOWN HANG: forced exit ..." with the phase it is stuck in, the
/// threads still running, and the UI thread's stack to crash_log.txt.
void logForcedShutdown(int afterSeconds);

} // namespace rt
