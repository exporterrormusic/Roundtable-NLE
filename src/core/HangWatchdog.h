/*
 * HangWatchdog — logs where the UI thread is stuck when it stops responding.
 *
 * A background thread watches a heartbeat that the UI event loop bumps
 * (main.cpp drives heartbeat() from a QTimer).  When the heartbeat goes
 * stale for kStallThreshold, the watchdog briefly suspends the UI thread,
 * unwinds its stack, and appends the symbolized frames to crash_log.txt
 * ("UI HANG ..." entries).  While the stall lasts it re-samples every
 * kResampleInterval (up to kMaxSamples), then logs the total duration once
 * the heartbeat resumes.  A hang that recovers on its own otherwise leaves
 * no trace at all — perf_log just goes quiet.
 *
 * Windows x64 only; start()/heartbeat()/stop() are no-ops elsewhere.
 * Disabled while a debugger is attached (breakpoints look like hangs).
 */

#pragma once

namespace rt {

class HangWatchdog
{
public:
    /// Start watching the CALLING thread (call from the UI thread).  The
    /// watchdog stays disarmed until the first heartbeat() so startup work
    /// done before the event loop runs isn't reported as a hang.
    static void start();

    /// Mark the UI thread as alive.  Cheap (one atomic store).
    static void heartbeat() noexcept;

    /// Stop and join the watchdog thread.  Call at the start of shutdown —
    /// teardown legitimately blocks the UI thread for long stretches.
    static void stop();

private:
    HangWatchdog() = delete;
};

} // namespace rt
