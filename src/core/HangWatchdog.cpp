/*
 * HangWatchdog implementation — see HangWatchdog.h.
 */

#include "HangWatchdog.h"
#include "CrashHandler.h"

#include <spdlog/spdlog.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>

#if defined(_WIN32) && defined(_M_X64)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <DbgHelp.h>
#pragma comment(lib, "dbghelp.lib")
#define RT_HANG_WATCHDOG_ENABLED 1
#endif

namespace rt {

#ifdef RT_HANG_WATCHDOG_ENABLED

namespace {

constexpr uint64_t kStallThresholdMs  = 5000;
constexpr uint64_t kResampleIntervalMs = 10000;
constexpr int      kMaxSamples        = 4;
constexpr int      kMaxFrames         = 48;
constexpr auto     kPollInterval      = std::chrono::milliseconds(250);

// Interrupt time excluding system sleep, so a laptop waking from sleep
// doesn't read as a multi-hour UI stall.
uint64_t nowMs() noexcept
{
    ULONGLONG t100ns = 0;
    QueryUnbiasedInterruptTime(&t100ns);
    return static_cast<uint64_t>(t100ns / 10000);
}

std::atomic<uint64_t> g_lastBeatMs{0};   // 0 = not armed yet
DWORD                 g_uiThreadId{0};
std::jthread          g_thread;
std::mutex            g_lifecycleMutex;

// Suspend the UI thread, unwind its stack into `frames`, resume it.
// Only PODs here: __try cannot coexist with C++ unwinding in one function.
// No heap, no locks, no dbghelp while the thread is suspended — it may
// hold the heap or loader lock, and touching either would deadlock us.
int captureThreadStack(HANDLE thread, DWORD64* frames, int maxFrames) noexcept
{
    if (SuspendThread(thread) == static_cast<DWORD>(-1))
        return 0;

    int n = 0;
    CONTEXT ctx{};
    ctx.ContextFlags = CONTEXT_FULL;
    if (GetThreadContext(thread, &ctx)) {
        __try {
            while (n < maxFrames && ctx.Rip != 0) {
                frames[n++] = ctx.Rip;
                DWORD64 imageBase = 0;
                PRUNTIME_FUNCTION fn =
                    RtlLookupFunctionEntry(ctx.Rip, &imageBase, nullptr);
                if (!fn) {
                    // Leaf function: return address is at [rsp].
                    ctx.Rip = *reinterpret_cast<const DWORD64*>(ctx.Rsp);
                    ctx.Rsp += 8;
                } else {
                    PVOID   handlerData = nullptr;
                    DWORD64 establisher = 0;
                    RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, ctx.Rip, fn,
                                     &ctx, &handlerData, &establisher, nullptr);
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            // Unreadable stack memory — keep the frames gathered so far.
        }
    }

    ResumeThread(thread);
    return n;
}

std::string moduleNameFor(DWORD64 addr, HMODULE* outModule)
{
    HMODULE mod = nullptr;
    *outModule = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                                | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCSTR>(addr), &mod) || !mod)
        return "?";
    *outModule = mod;
    char path[MAX_PATH] = {};
    if (!GetModuleFileNameA(mod, path, MAX_PATH))
        return "?";
    const char* base = path;
    for (const char* p = path; *p; ++p)
        if (*p == '\\' || *p == '/') base = p + 1;
    return base;
}

// dbghelp is initialised lazily on the watchdog thread, with the exe's
// folder on the search path so roundtable.pdb is found beside the exe
// (the working directory is the app root, not build/bin/Release).
bool ensureSymbols()
{
    static bool attempted = false;
    static bool ok = false;
    if (attempted) return ok;
    attempted = true;

    wchar_t exePath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::wstring dir(exePath);
    const auto slash = dir.find_last_of(L"\\/");
    if (slash != std::wstring::npos) dir.resize(slash);

    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES
                  | SYMOPT_FAIL_CRITICAL_ERRORS);
    ok = SymInitializeW(GetCurrentProcess(), dir.c_str(), TRUE) != FALSE;
    return ok;
}

std::string describeFrame(int index, DWORD64 addr)
{
    HMODULE mod = nullptr;
    const std::string modName = moduleNameFor(addr, &mod);

    std::string symName;
    std::string lineInfo;
    if (ensureSymbols()) {
        struct { SYMBOL_INFO info; char name[256]; } sym{};
        sym.info.SizeOfStruct = sizeof(SYMBOL_INFO);
        sym.info.MaxNameLen   = sizeof(sym.name);
        DWORD64 displacement  = 0;
        if (SymFromAddr(GetCurrentProcess(), addr, &displacement, &sym.info))
            symName = sym.info.Name;

        IMAGEHLP_LINE64 line{};
        line.SizeOfStruct = sizeof(line);
        DWORD lineDisp = 0;
        if (SymGetLineFromAddr64(GetCurrentProcess(), addr, &lineDisp, &line)
            && line.FileName) {
            const char* file = line.FileName;
            for (const char* p = line.FileName; *p; ++p)
                if (*p == '\\' || *p == '/') file = p + 1;
            lineInfo = std::string(file) + ":" + std::to_string(line.LineNumber);
        }
    }

    char buf[160];
    std::snprintf(buf, sizeof(buf), "  [%2d] %s+0x%llX", index, modName.c_str(),
                  static_cast<unsigned long long>(
                      mod ? addr - reinterpret_cast<DWORD64>(mod) : addr));
    std::string out = buf;
    if (!symName.empty()) out += "  " + symName;
    if (!lineInfo.empty()) out += "  (" + lineInfo + ")";
    return out;
}

void logStackSample(HANDLE uiThread, uint64_t stalledMs, int sampleIndex)
{
    DWORD64 frames[kMaxFrames] = {};
    const int n = captureThreadStack(uiThread, frames, kMaxFrames);

    char header[160];
    std::snprintf(header, sizeof(header),
                  "UI HANG: main thread unresponsive for %.1fs (sample %d, %d frames):",
                  stalledMs / 1000.0, sampleIndex, n);
    spdlog::warn("[UI-HANG] {}", header);
    CrashHandler::writeCrashLog(header);
    for (int i = 0; i < n; ++i)
        CrashHandler::writeCrashLog(describeFrame(i, frames[i]));
}

void watchdogLoop(std::stop_token stop)
{
    HANDLE uiThread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT
                                     | THREAD_QUERY_INFORMATION,
                                 FALSE, g_uiThreadId);
    if (!uiThread) {
        spdlog::warn("HangWatchdog: OpenThread failed ({}) — disabled",
                     GetLastError());
        return;
    }

    bool     inStall        = false;
    uint64_t stallStartMs   = 0;   // heartbeat time the stall began from
    uint64_t nextSampleAtMs = 0;
    int      samples        = 0;

    while (!stop.stop_requested()) {
        std::this_thread::sleep_for(kPollInterval);
        if (CrashHandler::isShutdownInProgress()) break;

        const uint64_t lastBeat = g_lastBeatMs.load(std::memory_order_acquire);
        if (lastBeat == 0) continue;               // not armed yet
        const uint64_t now     = nowMs();
        const uint64_t stalled = now > lastBeat ? now - lastBeat : 0;

        if (inStall && lastBeat != stallStartMs) {
            // Heartbeat moved: the stall is over.
            const uint64_t totalMs = lastBeat - stallStartMs;
            char msg[96];
            std::snprintf(msg, sizeof(msg),
                          "UI HANG: main thread recovered after %.1fs",
                          totalMs / 1000.0);
            spdlog::warn("[UI-HANG] {}", msg);
            CrashHandler::writeCrashLog(msg);
            inStall = false;
            continue;
        }

        if (stalled < kStallThresholdMs || IsDebuggerPresent())
            continue;

        if (!inStall) {
            inStall        = true;
            stallStartMs   = lastBeat;
            samples        = 0;
            nextSampleAtMs = now;
        }
        if (samples < kMaxSamples && now >= nextSampleAtMs) {
            logStackSample(uiThread, stalled, ++samples);
            nextSampleAtMs = now + kResampleIntervalMs;
        }
    }

    CloseHandle(uiThread);
}

} // namespace

void HangWatchdog::start()
{
    std::lock_guard lock(g_lifecycleMutex);
    if (g_thread.joinable()) return;
    g_uiThreadId = GetCurrentThreadId();
    g_lastBeatMs.store(0, std::memory_order_release);
    g_thread = std::jthread(watchdogLoop);
    SetThreadDescription(g_thread.native_handle(), L"HangWatchdog");
}

void HangWatchdog::heartbeat() noexcept
{
    g_lastBeatMs.store(nowMs(), std::memory_order_release);
}

void HangWatchdog::stop()
{
    std::lock_guard lock(g_lifecycleMutex);
    if (!g_thread.joinable()) return;
    g_thread.request_stop();
    g_thread.join();
}

#else  // !RT_HANG_WATCHDOG_ENABLED

void HangWatchdog::start() {}
void HangWatchdog::heartbeat() noexcept {}
void HangWatchdog::stop() {}

#endif

} // namespace rt
