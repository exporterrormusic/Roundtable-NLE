/*
 * CrashHandler implementation — Windows SEH + MiniDump.
 */

#include "CrashHandler.h"
#include "WorkerBreadcrumb.h"

#include <spdlog/spdlog.h>

#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <DbgHelp.h>
#pragma comment(lib, "dbghelp.lib")
#include <excpt.h>   // _resetstkoflw()
#include <stdlib.h>  // _set_purecall_handler, _set_invalid_parameter_handler
#endif

namespace rt {

// ── Global state (singleton-style) ──────────────────────────────────────────

namespace {

struct CrashHandlerState
{
    bool                                installed{false};
    std::filesystem::path               crashDir;
    wchar_t                             crashDirW[MAX_PATH + 1]{}; // stack-safe copy
    CrashHandler::EmergencySaveCallback emergencySave;
    CrashHandler::PostCrashCallback     postCrash;
    CrashInfo                           lastCrashInfo;
    std::mutex                          mu;

#ifdef _WIN32
    LPTOP_LEVEL_EXCEPTION_FILTER        previousFilter{nullptr};
#endif
};

CrashHandlerState& state()
{
    static CrashHandlerState s;
    return s;
}

// Lock-free flag set by App::~App at the start of Phase 1.  The SEH
// path consults this to decide whether to write the crash_marker.txt
// (which triggers the next-launch recovery dialog).  See header.
std::atomic<bool>& shutdownInProgressFlag() noexcept
{
    static std::atomic<bool> s{false};
    return s;
}

// ── Timestamp helper ────────────────────────────────────────────────────────

std::string formatTimestamp(const char* fmt)
{
    auto             now = std::chrono::system_clock::now();
    std::time_t      t   = std::chrono::system_clock::to_time_t(now);
    std::tm          tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    std::ostringstream oss;
    oss << std::put_time(&tm, fmt);
    return oss.str();
}

std::string timestamp()  { return formatTimestamp("%Y%m%d_%H%M%S"); }
std::string timestampHuman() { return formatTimestamp("%Y-%m-%d %H:%M:%S"); }

// ── Crash log writer ────────────────────────────────────────────────────────

/// Low-level crash log append using Win32 WriteFile.
/// Avoids std::ofstream heap allocations which can fail in a corrupted heap.
void appendCrashLogRaw(const std::filesystem::path& logPath, const std::string& msg)
{
    HANDLE hFile = CreateFileW(
        logPath.wstring().c_str(),
        FILE_APPEND_DATA,
        FILE_SHARE_READ,
        nullptr,
        OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (hFile == INVALID_HANDLE_VALUE)
        return;
    SetFilePointer(hFile, 0, nullptr, FILE_END);
    DWORD written = 0;
    WriteFile(hFile, msg.data(), static_cast<DWORD>(msg.size()), &written, nullptr);
    FlushFileBuffers(hFile);
    CloseHandle(hFile);
}

/// Heap-safe fallback: writes to crash_log.txt using ONLY stack buffers and
/// raw Win32 calls.  No std::string, no std::filesystem, no heap allocation.
/// Call this when heap corruption is suspected (e.g., in a purecall handler
/// or after an access violation that may have corrupted the heap).
void appendCrashLogRawStackSafe(const wchar_t* logDir, const char* msg)
{
    // Build path: logDir\crash_log.txt using stack buffer
    wchar_t fullPath[MAX_PATH + 32];
    wchar_t* p = fullPath;
    size_t dirLen = 0;
    while (logDir[dirLen] && dirLen < MAX_PATH) { ++dirLen; }
    for (size_t i = 0; i < dirLen && i < MAX_PATH; ++i)
        *p++ = logDir[i];
    *p++ = L'\\';
    wcscpy_s(p, 32, L"crash_log.txt");

    HANDLE hFile = CreateFileW(fullPath, FILE_APPEND_DATA, FILE_SHARE_READ,
                                nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) {
        // Directory may not exist — try to create it
        CreateDirectoryW(logDir, nullptr);
        hFile = CreateFileW(fullPath, FILE_APPEND_DATA, FILE_SHARE_READ,
                            nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hFile == INVALID_HANDLE_VALUE) return;
    }
    SetFilePointer(hFile, 0, nullptr, FILE_END);

    // Build timestamp on stack: YYYY-MM-DD HH:MM:SS
    wchar_t timeBuf[32];
    timeBuf[0] = L'[';
    {
        SYSTEMTIME st;
        GetLocalTime(&st);
        swprintf_s(timeBuf + 1, 30, L"%04d-%02d-%02d %02d:%02d:%02d",
                   st.wYear, st.wMonth, st.wDay,
                   st.wHour, st.wMinute, st.wSecond);
    }
    size_t timeLen = wcslen(timeBuf);
    timeBuf[timeLen] = L']';
    timeBuf[timeLen + 1] = L' ';
    timeBuf[timeLen + 2] = L'\0';

    // Write timestamp prefix without heap allocation
    DWORD written = 0;
    char narrowTime[64];
    int narrowLen = WideCharToMultiByte(CP_UTF8, 0, timeBuf, -1,
                                         narrowTime, 64, nullptr, nullptr);
    if (narrowLen > 1) {
        WriteFile(hFile, narrowTime, narrowLen - 1, &written, nullptr);
    }

    // Write message
    if (msg && *msg) {
        DWORD msgLen = static_cast<DWORD>(strlen(msg));
        WriteFile(hFile, msg, msgLen, &written, nullptr);
    }

    // Write newline
    const char newline[] = "\n";
    WriteFile(hFile, newline, 1, &written, nullptr);

    FlushFileBuffers(hFile);
    CloseHandle(hFile);
}

/// Build a timestamped crash log line and write it via the low-level path.
void appendCrashLog(const std::filesystem::path& logPath, const std::string& msg)
{
    try {
        std::filesystem::create_directories(logPath.parent_path());
        std::string line = "[" + timestampHuman() + "] " + msg + "\n";
        appendCrashLogRaw(logPath, line);
    } catch (...) {
        // Best effort — we're in a crash handler, can't throw.
    }
}

// ── Platform-specific crash handlers ────────────────────────────────────────

#ifdef _WIN32

std::string getModuleNameFromAddress(void* addr)
{
    HMODULE hMod = nullptr;
    if (GetModuleHandleExA(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCSTR>(addr), &hMod)) {
        char modPath[MAX_PATH] = {};
        if (GetModuleFileNameA(hMod, modPath, MAX_PATH)) {
            std::string fullPath(modPath);
            auto pos = fullPath.find_last_of("\\/");
            return (pos != std::string::npos) ? fullPath.substr(pos + 1) : fullPath;
        }
    }
    return "unknown";
}

std::string exceptionCodeToString(DWORD code)
{
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:         return "ACCESS_VIOLATION";
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:    return "ARRAY_BOUNDS_EXCEEDED";
    case EXCEPTION_BREAKPOINT:               return "BREAKPOINT";
    case EXCEPTION_DATATYPE_MISALIGNMENT:    return "DATATYPE_MISALIGNMENT";
    case EXCEPTION_FLT_DENORMAL_OPERAND:     return "FLT_DENORMAL_OPERAND";
    case EXCEPTION_FLT_DIVIDE_BY_ZERO:       return "FLT_DIVIDE_BY_ZERO";
    case EXCEPTION_FLT_INEXACT_RESULT:       return "FLT_INEXACT_RESULT";
    case EXCEPTION_FLT_INVALID_OPERATION:    return "FLT_INVALID_OPERATION";
    case EXCEPTION_FLT_OVERFLOW:             return "FLT_OVERFLOW";
    case EXCEPTION_FLT_STACK_CHECK:          return "FLT_STACK_CHECK";
    case EXCEPTION_FLT_UNDERFLOW:            return "FLT_UNDERFLOW";
    case EXCEPTION_ILLEGAL_INSTRUCTION:      return "ILLEGAL_INSTRUCTION";
    case EXCEPTION_IN_PAGE_ERROR:            return "IN_PAGE_ERROR";
    case EXCEPTION_INT_DIVIDE_BY_ZERO:       return "INT_DIVIDE_BY_ZERO";
    case EXCEPTION_INT_OVERFLOW:             return "INT_OVERFLOW";
    case EXCEPTION_INVALID_DISPOSITION:      return "INVALID_DISPOSITION";
    case EXCEPTION_NONCONTINUABLE_EXCEPTION: return "NONCONTINUABLE_EXCEPTION";
    case EXCEPTION_PRIV_INSTRUCTION:         return "PRIV_INSTRUCTION";
    case EXCEPTION_SINGLE_STEP:              return "SINGLE_STEP";
    case EXCEPTION_STACK_OVERFLOW:           return "STACK_OVERFLOW";
    default:
    {
        std::ostringstream oss;
        oss << "UNKNOWN(0x" << std::hex << code << ")";
        return oss.str();
    }
    }
}

bool writeMiniDump(const std::filesystem::path& dumpPath,
                   EXCEPTION_POINTERS*          exInfo)
{
    HANDLE hFile = CreateFileW(
        dumpPath.wstring().c_str(),
        GENERIC_WRITE,
        0,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);

    if (hFile == INVALID_HANDLE_VALUE)
        return false;

    MINIDUMP_EXCEPTION_INFORMATION mdei;
    mdei.ThreadId          = GetCurrentThreadId();
    mdei.ExceptionPointers = exInfo;
    mdei.ClientPointers    = FALSE;

    BOOL result = MiniDumpWriteDump(
        GetCurrentProcess(),
        GetCurrentProcessId(),
        hFile,
        MiniDumpWithDataSegs,
        exInfo ? &mdei : nullptr,
        nullptr,
        nullptr);

    CloseHandle(hFile);
    return result != FALSE;
}

/// Call a noexcept-ish callback inside SEH __try/__except so that an AV
/// inside the callback doesn't kill the crash handler.  This is a free
/// function (no C++ try/catch) so MSVC won't complain about mixing SEH
/// with C++ EH in the caller.
template <typename Fn>
inline void sehCall(Fn&& fn)
{
    __try { fn(); } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

/// If `modBase` is the leaf name of a known third-party DLL injected
/// into our process (driver shim, overlay capture, game-platform
/// overlay, ...), returns a short human label.  Otherwise nullptr.
/// Crashes that originate inside these modules are nearly always
/// caused by the injected component, not by our code — distinguishing
/// them in crash_log.txt prevents wasting time hunting bugs we
/// cannot fix.
///
/// Stack-safe: no heap, no exceptions, no CRT TLS — safe to call from
/// VEH / SEH context where the heap may be unusable.
static const char* identifyThirdPartyHook(const char* modBase) noexcept
{
    if (!modBase || !*modBase) return nullptr;

    // ASCII-lowercase the basename into a stack buffer.  Windows file
    // names are case-insensitive, so "NvOglv64.DLL" must match the
    // "nvoglv64" needle below.
    char lower[96] = {};
    size_t n = 0;
    for (; modBase[n] && n + 1 < sizeof(lower); ++n) {
        char c = modBase[n];
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        lower[n] = c;
    }
    lower[n] = '\0';

    struct Entry { const char* needle; const char* label; };
    static constexpr Entry kHooks[] = {
        // NVIDIA drivers + overlay components.  nvoglv64 in a Vulkan
        // app is almost always the GFE / NVIDIA-App overlay rendering
        // its UI through OpenGL, not our code.
        { "nvoglv64",            "NVIDIA OpenGL driver"                         },
        { "nvspcap",             "NVIDIA GeForce Experience / ShadowPlay overlay" },
        { "nvapp",               "NVIDIA App overlay"                           },
        { "nvspbridge",          "NVIDIA Streamer bridge"                       },
        // AMD
        { "amdihk",              "AMD Adrenaline overlay"                       },
        { "amdvlk",              "AMD Vulkan driver"                            },
        // Capture / streaming hooks
        { "ow-graphics-hook",    "Overwolf graphics hook"                       },
        { "graphics-hook",       "OBS Studio game-capture hook"                 },
        { "obs-hook",            "OBS game-capture hook"                        },
        { "fraps",               "Fraps capture"                                },
        { "rtsshooks",           "RivaTuner Statistics Server / MSI Afterburner"},
        // Store / chat overlays
        { "discordhook",         "Discord overlay"                              },
        { "gameoverlayrenderer", "Steam overlay"                                },
        { "eosovh",              "Epic Online Services overlay"                 },
        { "uplay_r",             "Ubisoft Connect overlay"                      },
    };

    for (const auto& e : kHooks) {
        // Minimal stack-safe substring search (avoids strstr — locale
        // tables can heap-allocate on first use).
        for (size_t i = 0; lower[i]; ++i) {
            size_t k = 0;
            while (e.needle[k] && lower[i + k] == e.needle[k]) ++k;
            if (!e.needle[k]) return e.label;
        }
    }
    return nullptr;
}

// ── Vectored Exception Handler (VEH) for stack overflow ──────────────────
// SetUnhandledExceptionFilter runs on the faulting thread's remaining stack.
// When the fault is STACK_OVERFLOW, there is practically NO stack left —
// even the SEH handler cannot run.  A VEH fires before that, while the
// guard page is still present, giving us a chance to call _resetstkoflw()
// and restore a usable stack BEFORE the SEH handler is invoked.
static LONG WINAPI stackOverflowVectoredHandler(EXCEPTION_POINTERS* exInfo)
{
    if (exInfo && exInfo->ExceptionRecord &&
        exInfo->ExceptionRecord->ExceptionCode == EXCEPTION_STACK_OVERFLOW)
    {
        _resetstkoflw();
        return EXCEPTION_CONTINUE_SEARCH;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

// ── Last-chance VEH (registered as last handler) ────────────────────────
// On Windows 8+, SetUnhandledExceptionFilter is unreliable — WER may
// intercept the exception before the filter runs.  This VEH runs BEFORE
// the SEH __try/__except chain (all VEH handlers are first-chance).
// IMPORTANT: We must NOT terminate here — we only LOG the exception and
// return EXCEPTION_CONTINUE_SEARCH so the SEH handler chain still runs.
// If we terminate, we break normal operation for exceptions that the
// application handles via __try/__except (e.g., during static init).
//
// C++ exceptions (0xE06D7363) and debugger exceptions are passed through
// silently since they're handled by std::terminate or the debugger.
static LONG WINAPI lastChanceVectoredHandler(EXCEPTION_POINTERS* exInfo)
{
    if (!exInfo || !exInfo->ExceptionRecord)
        return EXCEPTION_CONTINUE_SEARCH;

    DWORD code = exInfo->ExceptionRecord->ExceptionCode;

    // Pass through C++ exceptions, debugger events, and guard page
    // exceptions (normal stack growth — OS handles it transparently).
    if (code == 0xE06D7363)
        return EXCEPTION_CONTINUE_SEARCH;
    if (code == EXCEPTION_BREAKPOINT || code == EXCEPTION_SINGLE_STEP)
        return EXCEPTION_CONTINUE_SEARCH;
    // OutputDebugString raises these benign "exceptions" continuously while
    // a debugger or DebugView is attached. They are NOT crashes — logging
    // them floods crash_log.txt and buries real faults (e.g. heap
    // corruption). DBG_PRINTEXCEPTION_C / DBG_PRINTEXCEPTION_WIDE_C.
    if (code == 0x40010006 || code == 0x4001000A)
        return EXCEPTION_CONTINUE_SEARCH;
    if (code == EXCEPTION_GUARD_PAGE)
        return EXCEPTION_CONTINUE_EXECUTION;  // OS must retry after expanding stack
    if (code == 0x406D1388)  // Thread name exception (SetThreadDescription)
        return EXCEPTION_CONTINUE_SEARCH;

    // Log using the stack-safe writer.  We're in the middle of an exception
    // so heap operations are unsafe.  The log may be incomplete if the
    // crash_dir_w isn't populated yet (before CrashHandler::install()).
    //
    // Best-effort: resolve the module that owns ExceptionAddress so the log
    // line lets us tell apart "our code", "NVIDIA driver", and "injected
    // hook DLL" (NVIDIA App / OBS / Discord overlay).  GetModuleHandleExA +
    // GetModuleBaseNameA are documented safe to call from a VEH and use
    // only OS-internal allocators, not our CRT heap.  modBase is left
    // empty on failure (no extra noise in the log).
    char modBase[64] = {};
    uintptr_t modOff = 0;
    {
        HMODULE hMod = nullptr;
        if (GetModuleHandleExA(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCSTR>(exInfo->ExceptionRecord->ExceptionAddress),
                &hMod) && hMod) {
            char modPath[MAX_PATH] = {};
            if (GetModuleFileNameA(hMod, modPath, MAX_PATH)) {
                const char* base = modPath;
                for (const char* p = modPath; *p; ++p) {
                    if (*p == '\\' || *p == '/') base = p + 1;
                }
                size_t n = 0;
                while (base[n] && n + 1 < sizeof(modBase)) {
                    modBase[n] = base[n]; ++n;
                }
                modBase[n] = '\0';
            }
            modOff = reinterpret_cast<uintptr_t>(
                         exInfo->ExceptionRecord->ExceptionAddress)
                   - reinterpret_cast<uintptr_t>(hMod);
        }
    }

    // 0x000006BA (= RPC_S_SERVER_UNAVAILABLE / 1722) is a software-raised
    // exception observed coming from third-party injected DLLs (NVIDIA
    // GeForce Experience overlay, OBS hook, Discord overlay) when the
    // Vulkan ICD is mid-TDR.  Tag it distinctly so users / support can
    // identify the injecting module from the log without a minidump.
    const char* tag = "SEH";
    if (code == 0x000006BA)         tag = "SEH(HOOK_RPC_UNAVAIL)";
    else if (code == 0xC0000374)    tag = "SEH(HEAP_CORRUPTION)";
    else if (code == EXCEPTION_ACCESS_VIOLATION) tag = "SEH(ACCESS_VIOLATION)";

    // If the faulting module is a known third-party hook DLL (driver
    // overlay, capture hook, game-platform overlay) prefix the line
    // with [HOOK:<label>] so we don't waste time chasing bugs in our
    // own code that are actually third-party.  See identifyThirdPartyHook.
    const char* hookLabel = identifyThirdPartyHook(modBase);

    char msg[384];
    uintptr_t addr = reinterpret_cast<uintptr_t>(
        exInfo->ExceptionRecord->ExceptionAddress);
    if (modBase[0]) {
        if (hookLabel) {
            snprintf(msg, sizeof(msg),
                     "%s [HOOK:%s]: CODE=0x%08X ADDR=0x%llX MOD=%s+0x%llX TID=%lu",
                     tag, hookLabel, code,
                     static_cast<unsigned long long>(addr),
                     modBase, static_cast<unsigned long long>(modOff),
                     GetCurrentThreadId());
        } else {
            snprintf(msg, sizeof(msg),
                     "%s: CODE=0x%08X ADDR=0x%llX MOD=%s+0x%llX TID=%lu",
                     tag, code, static_cast<unsigned long long>(addr),
                     modBase, static_cast<unsigned long long>(modOff),
                     GetCurrentThreadId());
        }
    } else {
        snprintf(msg, sizeof(msg),
                 "%s: CODE=0x%08X ADDR=0x%llX TID=%lu",
                 tag, code, static_cast<unsigned long long>(addr),
                 GetCurrentThreadId());
    }
    appendCrashLogRawStackSafe(state().crashDirW, msg);

    // Continue searching — SEH handlers and the unhandled exception
    // filter will still process this exception normally.
    return EXCEPTION_CONTINUE_SEARCH;
}

LONG WINAPI crashExceptionFilter(EXCEPTION_POINTERS* exInfo)
{
    auto& s = state();

    // ── STACK_OVERFLOW recovery ──────────────────────────────────────
    // The VEH above should have reset the stack already, but as a safety
    // net, call _resetstkoflw() again here if this is a stack overflow.
    // This ensures the crash handler has a usable stack even if the VEH
    // wasn't installed or didn't fire for some reason.
    if (exInfo && exInfo->ExceptionRecord &&
        exInfo->ExceptionRecord->ExceptionCode == EXCEPTION_STACK_OVERFLOW)
    {
        _resetstkoflw();
    }

    // Build crash info
    CrashInfo info;
    if (exInfo && exInfo->ExceptionRecord) {
        info.exceptionCode    = exInfo->ExceptionRecord->ExceptionCode;
        info.exceptionAddress = exInfo->ExceptionRecord->ExceptionAddress;

        // Identify the module containing the crash address
        std::string moduleName = getModuleNameFromAddress(info.exceptionAddress);
        const char* hookLabel = identifyThirdPartyHook(moduleName.c_str());

        HMODULE hMod = nullptr;
        GetModuleHandleExA(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCSTR>(info.exceptionAddress), &hMod);

        auto addrOffset = reinterpret_cast<uintptr_t>(info.exceptionAddress)
                        - reinterpret_cast<uintptr_t>(hMod);

        info.summary = "Unhandled exception: " +
                       exceptionCodeToString(info.exceptionCode) +
                       " at address " + [&]{
                           std::ostringstream oss;
                           oss << "0x" << std::hex
                               << reinterpret_cast<uintptr_t>(info.exceptionAddress)
                               << " in " << moduleName
                               << " (offset 0x" << addrOffset << ")";
                           if (hookLabel)
                               oss << " [HOOK: " << hookLabel
                                   << " — third-party, not our code]";
                           return oss.str();
                       }();
    } else {
        info.summary = "Unhandled exception (no exception record available)";
    }

    // ── Prepare paths ─────────────────────────────────────────────────────
    std::filesystem::path dumpDir = s.crashDir;
    try {
        std::filesystem::create_directories(dumpDir);
    } catch (...) {
    }
    auto logPath = dumpDir / "crash_log.txt";
    info.logFilePath = logPath;

    // 1. Attempt emergency auto-save (SEH-protected via helper)
    if (s.emergencySave) {
        sehCall([&] { s.emergencySave(); });
    }

    // 2. Write crash log FIRST (before minidump — if MiniDumpWriteDump
    //    crashes with a nested exception the process dies immediately).
    //    Use the stack-safe path to avoid heap allocation failures that
    //    can occur when the heap is corrupted by the crash.
    appendCrashLogRawStackSafe(s.crashDirW, info.summary.c_str());

    // 2a. Last breadcrumb FOR THIS (faulting) thread (set by
    //     convertDecodedToCacheGpu phases on prefetch workers).
    //     Per-thread TLS, so "idle" means the crash was NOT on a worker
    //     thread that participates in the breadcrumb scheme — a useful
    //     signal in itself (typically points at a UI-thread shutdown
    //     race rather than a Vulkan worker bug).  readLastWorkerStep()
    //     is noexcept and reads its own TLS slot; safe in SEH.
    {
        char buf[256];
        snprintf(buf, sizeof(buf), "  Last step (this thread): %s",
                 readLastWorkerStep());
        appendCrashLogRawStackSafe(s.crashDirW, buf);
    }

    // 3. Write minidump (SEH-protected via helper)
    auto dumpPath = dumpDir / ("crash_" + timestamp() + ".dmp");
    {
        bool dumpOk = false;
        sehCall([&] { dumpOk = writeMiniDump(dumpPath, exInfo); });
        if (dumpOk)
            info.dumpFilePath = dumpPath;
    }
    if (!info.dumpFilePath.empty()) {
        appendCrashLog(logPath, "  Dump written: " + info.dumpFilePath.string());
    }

    // 4. Capture and write stack trace (SEH-protected via helper)
    {
        sehCall([&] { SymInitialize(GetCurrentProcess(), NULL, TRUE); });

        void* stack[64] = {};
        WORD frames = CaptureStackBackTrace(0, 64, stack, NULL);

        for (WORD i = 0; i < frames; ++i) {
            DWORD64 addr = (DWORD64)(stack[i]);
            std::string mn = getModuleNameFromAddress(stack[i]);

            HMODULE hm = nullptr;
            GetModuleHandleExA(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                (LPCSTR)stack[i], &hm);

            std::string symName;
            sehCall([&] {
                struct SymbolInfo {
                    SYMBOL_INFO base;
                    char name[256];
                };
                SymbolInfo sym{};
                sym.base.SizeOfStruct = sizeof(SYMBOL_INFO);
                sym.base.MaxNameLen   = 255;
                if (SymFromAddr(GetCurrentProcess(), addr, NULL, &sym.base))
                    symName = sym.base.Name;
            });

            std::ostringstream oss;
            oss << "  [" << i << "] 0x" << std::hex << addr;
            if (!symName.empty())
                oss << " " << symName;
            if (!mn.empty() && hm)
                oss << " (" << mn << "+0x" << std::hex << (addr - (DWORD64)hm) << ")";
            appendCrashLog(logPath, oss.str());
        }
        sehCall([&] { SymCleanup(GetCurrentProcess()); });
    }

    s.lastCrashInfo = info;

    // ── Crash marker (Phase 7.A) ──────────────────────────────────────
    // Write a marker file so the next launch can detect this crash and
    // offer recovery options (reset dock layout, workspace, etc.).
    //
    // EXCEPTION: if App::~App has already started its shutdown sequence,
    // a worker-thread AV during teardown is a known race we don't want
    // to surface to the user.  The dump + log entries are still written
    // for forensics, but the marker (which drives the next-launch
    // recovery dialog) is suppressed.  Without this, every shutdown-time
    // background-thread crash would falsely tell the user their last
    // session crashed.
    if (shutdownInProgressFlag().load(std::memory_order_acquire)) {
        appendCrashLogRawStackSafe(s.crashDirW,
            "  (shutdown in progress — suppressing crash marker)");
    } else {
        sehCall([&] { CrashHandler::writeCrashMarker(info); });
    }

    // 5. Post-crash callback (SEH-protected via helper)
    if (s.postCrash) {
        sehCall([&] { s.postCrash(info); });
    }

    // Terminate immediately — we already wrote our own dump and log.
    // Handing to WER via EXCEPTION_CONTINUE_SEARCH keeps the exe locked
    // for an indeterminate time, blocking rebuilds.
    TerminateProcess(GetCurrentProcess(), exInfo->ExceptionRecord->ExceptionCode);

    // Unreachable, but keep the fallback for safety.
    return EXCEPTION_CONTINUE_SEARCH;
}

#else
// POSIX stub — signals
#include <csignal>
#include <cstring>
#include <unistd.h>
#include <execinfo.h>

namespace {
struct sigaction s_oldSigsegv, s_oldSigabrt, s_oldSigfpe, s_oldSigbus;

void crashSignalHandler(int sig, siginfo_t* info, void* /*context*/)
{
    auto& s = state();

    CrashInfo ci;
    ci.summary = std::string("Signal ") + strsignal(sig);
    if (info) {
        ci.exceptionAddress = info->si_addr;
    }

    // Emergency save
    if (s.emergencySave) {
        try { s.emergencySave(); } catch (...) {}
    }

    // Collect backtrace
    void* frames[64];
    int   nFrames = backtrace(frames, 64);
    char** syms   = backtrace_symbols(frames, nFrames);
    if (syms) {
        std::ostringstream oss;
        for (int i = 0; i < nFrames; ++i)
            oss << syms[i] << "\n";
        ci.stackTrace = oss.str();
        free(syms);
    }

    // Write crash log
    auto logPath = s.crashDir / "crash_log.txt";
    ci.logFilePath = logPath;
    appendCrashLog(logPath, ci.summary);
    if (!ci.stackTrace.empty())
        appendCrashLog(logPath, ci.stackTrace);

    s.lastCrashInfo = ci;

    // Crash marker (Phase 7.A)
    try { CrashHandler::writeCrashMarker(ci); } catch (...) {}

    if (s.postCrash) {
        try { s.postCrash(ci); } catch (...) {}
    }

    // Re-raise to get default behavior (core dump, etc.)
    struct sigaction sa{};
    sa.sa_handler = SIG_DFL;
    sigaction(sig, &sa, nullptr);
    raise(sig);
}
} // anon
#endif

} // anonymous namespace

// ── Public API ──────────────────────────────────────────────────────────────

void CrashHandler::install(const std::filesystem::path& crashDir)
{
    auto& s = state();
    std::lock_guard lock(s.mu);

    if (s.installed) {
        spdlog::warn("CrashHandler::install() — already installed");
        return;
    }

    s.crashDir = crashDir;
    // Populate stack-safe wchar_t copy of crash directory
    {
        std::wstring ws = crashDir.wstring();
        size_t len = ws.copy(s.crashDirW, MAX_PATH);
        s.crashDirW[len] = L'\0';
    }

    try {
        std::filesystem::create_directories(crashDir);
    } catch (const std::exception& e) {
        spdlog::error("CrashHandler: failed to create crash dir '{}': {}",
                       crashDir.string(), e.what());
    }

    installPlatformHandler();
    s.installed = true;

    // ── Non-SEH crash handlers ─────────────────────────────────────────
    // These catch termination paths that bypass SetUnhandledExceptionFilter.
    // ALL handlers use appendCrashLogRawStackSafe() to avoid heap allocation
    // failures that occur when the heap is corrupted.

    // 1. Pure virtual function call (e.g. calling virtual methods on a
    //    partially-destroyed object).  The default handler calls abort()
    //    which terminates without SEH, leaving NO crash log.
#ifdef _WIN32
    _set_purecall_handler([]() {
        appendCrashLogRawStackSafe(state().crashDirW,
            "PURE VIRTUAL FUNCTION CALL — likely use-after-free "
            "on a partially-destroyed object");
        TerminateProcess(GetCurrentProcess(), 0xC0000005);
    });

    // 2. CRT invalid parameter handler (catches assertions from CRT
    //    functions like isdigit() or fgets() with invalid args).
    _set_invalid_parameter_handler([](
        const wchar_t* expr, const wchar_t* func,
        const wchar_t* file, unsigned int line, uintptr_t /*reserved*/)
    {
        // Build message on stack without heap allocation
        char msg[512];
        int pos = snprintf(msg, sizeof(msg), "CRT INVALID PARAMETER:");
        if (pos > 0 && expr) {
            char narrow[128];
            WideCharToMultiByte(CP_UTF8, 0, expr, -1, narrow, 128, nullptr, nullptr);
            pos += snprintf(msg + pos, sizeof(msg) - pos, " expr=%s", narrow);
        }
        if (pos > 0 && func) {
            char narrow[128];
            WideCharToMultiByte(CP_UTF8, 0, func, -1, narrow, 128, nullptr, nullptr);
            pos += snprintf(msg + pos, sizeof(msg) - pos, " func=%s", narrow);
        }
        if (pos > 0 && file) {
            char narrow[128];
            WideCharToMultiByte(CP_UTF8, 0, file, -1, narrow, 128, nullptr, nullptr);
            pos += snprintf(msg + pos, sizeof(msg) - pos, " file=%s:%u", narrow, line);
        }
        appendCrashLogRawStackSafe(state().crashDirW, msg);
    });
#endif

    // 3. std::terminate handler (catches unhandled C++ exceptions that
    //    escape main(), or exceptions thrown during stack unwinding).
    std::set_terminate([]() {
        // Write crash log FIRST using stack-safe path.
        // Do NOT re-throw — calling throw; inside a terminate handler
        // causes recursive std::terminate (undefined behavior), which
        // would prevent this log from ever being written.
        // Write to TWO locations: the configured crashDir (via state)
        // and a hardcoded fallback in case state is corrupted.
        const wchar_t* dir = state().crashDirW;
        appendCrashLogRawStackSafe(dir,
            "std::terminate called — unhandled C++ exception or "
            "exception during stack unwinding");
        // Also write a fallback to a known path to verify handler runs
        HANDLE hFb = CreateFileW(L"C:\\roundtable_terminate_marker.txt",
                                  GENERIC_WRITE, 0, nullptr,
                                  CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hFb != INVALID_HANDLE_VALUE) {
            const char msg[] = "terminate handler fired\n";
            DWORD wrote = 0;
            WriteFile(hFb, msg, sizeof(msg) - 1, &wrote, nullptr);
            CloseHandle(hFb);
        }
        // Terminate immediately — nothing safe left to do.
#ifdef _WIN32
        TerminateProcess(GetCurrentProcess(), 0xC0000005);
#else
        abort();
#endif
    });

    // Write startup marker to verify crash log is writable.
    // Uses the stack-safe raw Win32 path — no heap allocation.
    appendCrashLogRawStackSafe(s.crashDirW, "=== SESSION START ===");

    spdlog::info("CrashHandler installed — crash dir: {}", crashDir.string());
}

void CrashHandler::uninstall()
{
    auto& s = state();
    std::lock_guard lock(s.mu);

    if (!s.installed) return;

    uninstallPlatformHandler();
    s.installed = false;

    spdlog::info("CrashHandler uninstalled");
}

bool CrashHandler::isInstalled() noexcept
{
    return state().installed;
}

void CrashHandler::setCrashDirectory(const std::filesystem::path& dir)
{
    auto& s = state();
    std::lock_guard lock(s.mu);
    s.crashDir = dir;
    try {
        std::filesystem::create_directories(dir);
    } catch (...) {
    }
}

const std::filesystem::path& CrashHandler::crashDirectory() noexcept
{
    return state().crashDir;
}

void CrashHandler::setEmergencySaveCallback(EmergencySaveCallback cb)
{
    auto& s = state();
    std::lock_guard lock(s.mu);
    s.emergencySave = std::move(cb);
}

void CrashHandler::setPostCrashCallback(PostCrashCallback cb)
{
    auto& s = state();
    std::lock_guard lock(s.mu);
    s.postCrash = std::move(cb);
}

void CrashHandler::writeCrashLog(const std::string& message)
{
    auto& s = state();
    appendCrashLog(s.crashDir / "crash_log.txt", message);
}

void CrashHandler::notifyShutdownStarted() noexcept
{
    shutdownInProgressFlag().store(true, std::memory_order_release);
}

bool CrashHandler::isShutdownInProgress() noexcept
{
    return shutdownInProgressFlag().load(std::memory_order_acquire);
}

std::filesystem::path CrashHandler::nextDumpPath()
{
    auto& s = state();
    return s.crashDir / ("crash_" + timestamp() + ".dmp");
}

std::filesystem::path CrashHandler::crashLogPath()
{
    return state().crashDir / "crash_log.txt";
}

CrashInfo CrashHandler::lastCrash()
{
    auto& s = state();
    std::lock_guard lock(s.mu);
    return s.lastCrashInfo;
}

bool CrashHandler::hasPreviousCrashLog()
{
    auto logPath = crashLogPath();
    return std::filesystem::exists(logPath) &&
           std::filesystem::file_size(logPath) > 0;
}

// ── Platform handler install / uninstall ────────────────────────────────────

// ── Crash marker (Phase 7.A) ────────────────────────────────────────────────

std::filesystem::path CrashHandler::crashMarkerPath()
{
    return state().crashDir / "crash_marker.txt";
}

void CrashHandler::writeCrashMarker(const CrashInfo& info)
{
    try {
        auto path = crashMarkerPath();
        std::ofstream marker(path);
        if (!marker.is_open()) return;

        marker << "crash_time=" << timestamp() << "\n";
        marker << "exception=0x" << std::hex << info.exceptionCode << "\n";
        marker << "address=0x" << std::hex
               << reinterpret_cast<uintptr_t>(info.exceptionAddress) << "\n";
        marker << "summary=" << info.summary << "\n";
        marker << "dump=" << info.dumpFilePath.string() << "\n";
        marker << "log=" << info.logFilePath.string() << "\n";
        marker.close();
    } catch (...) {
        // Best-effort — marker is non-critical
    }
}

bool CrashHandler::hasCrashMarker()
{
    return std::filesystem::exists(crashMarkerPath());
}

CrashInfo CrashHandler::readCrashMarker()
{
    CrashInfo info;
    try {
        auto path = crashMarkerPath();
        if (!std::filesystem::exists(path)) return info;

        std::ifstream marker(path);
        if (!marker.is_open()) return info;

        std::string line;
        while (std::getline(marker, line)) {
            if (line.rfind("summary=", 0) == 0)
                info.summary = line.substr(8);
            else if (line.rfind("exception=0x", 0) == 0)
                info.exceptionCode =
                    static_cast<uint32_t>(std::stoul(line.substr(11), nullptr, 16));
            else if (line.rfind("dump=", 0) == 0)
                info.dumpFilePath = line.substr(5);
            else if (line.rfind("log=", 0) == 0)
                info.logFilePath = line.substr(4);
        }
        marker.close();
    } catch (...) {
    }
    return info;
}

void CrashHandler::clearCrashMarker()
{
    std::error_code ec;
    std::filesystem::remove(crashMarkerPath(), ec);
}

void CrashHandler::installPlatformHandler()
{
#ifdef _WIN32
    // Install VEH first (catches stack overflow before the stack is gone)
    // so _resetstkoflw() can be called to restore a usable stack before
    // the SEH filter runs.
    AddVectoredExceptionHandler(/*FirstHandler=*/1, stackOverflowVectoredHandler);

    // Install last-chance VEH — catches SEH that SetUnhandledExceptionFilter
    // misses on Windows 8+ (WER may intercept before the filter runs).
    // Registered as last handler (FirstHandler=0) so __try/__except blocks
    // and C++ exception handling run first.
    AddVectoredExceptionHandler(/*FirstHandler=*/0, lastChanceVectoredHandler);

    state().previousFilter = SetUnhandledExceptionFilter(crashExceptionFilter);
#else
    struct sigaction sa{};
    sa.sa_sigaction = crashSignalHandler;
    sa.sa_flags     = SA_SIGINFO | SA_RESETHAND;
    sigemptyset(&sa.sa_mask);

    sigaction(SIGSEGV, &sa, &s_oldSigsegv);
    sigaction(SIGABRT, &sa, &s_oldSigabrt);
    sigaction(SIGFPE,  &sa, &s_oldSigfpe);
    sigaction(SIGBUS,  &sa, &s_oldSigbus);
#endif
}

void CrashHandler::uninstallPlatformHandler()
{
#ifdef _WIN32
    SetUnhandledExceptionFilter(state().previousFilter);
    state().previousFilter = nullptr;
#else
    sigaction(SIGSEGV, &s_oldSigsegv, nullptr);
    sigaction(SIGABRT, &s_oldSigabrt, nullptr);
    sigaction(SIGFPE,  &s_oldSigfpe,  nullptr);
    sigaction(SIGBUS,  &s_oldSigbus,  nullptr);
#endif
}

// ── Crash log deduplication ───────────────────────────────────────────────

void CrashHandler::deduplicateCrashLog()
{
    auto logPath = crashLogPath();
    if (!std::filesystem::exists(logPath)) return;

    // Read all lines
    std::ifstream in(logPath);
    if (!in.is_open()) return;

    std::vector<std::string> lines;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty()) lines.push_back(line);
    }
    in.close();

    if (lines.size() < 50) return;  // Not worth deduplicating small logs

    // Build deduplicated output.  Consecutive crash entry lines with
    // identical signatures (after stripping timestamp + TID) get collapsed
    // into one line with a "(repeated N times)" suffix.
    std::vector<std::string> out;

    // Helper: extract the "stable signature" of a crash log line — the
    // part that stays the same across repeated crashes of the same bug.
    // Removes the timestamp prefix and thread-id suffix so consecutive
    // identical crashes compare equal.
    auto crashSignature = [](const std::string& s) -> std::string {
        // Only hash crash entry lines (they start with '[' or have a tag).
        // SESSION START and other markers pass through as-is.
        if (s.empty()) return {};

        // Strip "[YYYY-MM-DD HH:MM:SS] " prefix
        std::string body = s;
        if (body.size() > 22 && body[0] == '[' &&
            body[4] == '-' && body[7] == '-' &&
            body[10] == ' ' && body[13] == ':' && body[16] == ':') {
            body = body.substr(22);
        }

        // Strip " TID=NNNNN" suffix (variable thread ID)
        auto tidPos = body.rfind(" TID=");
        if (tidPos != std::string::npos) {
            // Verify the rest after TID= is all digits before stripping
            auto after = body.substr(tidPos + 5);
            bool allDigits = !after.empty();
            for (char c : after) { if (c < '0' || c > '9') { allDigits = false; break; } }
            if (allDigits) body = body.substr(0, tidPos);
        }

        return body;
    };

    size_t runCount = 0;
    std::string runSig;
    size_t runStartIdx = 0;  // index in `lines` of first line in current run

    auto flushRun = [&]() {
        if (runCount <= 1) {
            // Single line — just copy
            for (size_t i = runStartIdx; i < runStartIdx + runCount; ++i)
                out.push_back(lines[i]);
        } else {
            // Collapsed run — keep first line with repeat count
            out.push_back(lines[runStartIdx] +
                          "  (repeated " + std::to_string(runCount) + " times)");
        }
        runCount = 0;
        runSig.clear();
    };

    for (size_t i = 0; i < lines.size(); ++i) {
        const auto& l = lines[i];

        // SESSION START markers always break runs and pass through
        if (l.find("SESSION START") != std::string::npos) {
            flushRun();
            out.push_back(l);
            continue;
        }

        // Lines that don't look like crash entries break runs
        if (l.empty() || l[0] != '[') {
            flushRun();
            out.push_back(l);
            continue;
        }

        std::string sig = crashSignature(l);

        // Only deduplicate lines that actually contain crash info
        bool isCrashLine = (sig.find("CODE=0x") != std::string::npos ||
                            sig.find("SEH(") == 0 ||
                            sig.find("Unhandled exception") != std::string::npos ||
                            sig.find("std::terminate") != std::string::npos);

        if (!isCrashLine) {
            flushRun();
            out.push_back(l);
            continue;
        }

        if (runCount > 0 && sig == runSig) {
            // Same crash as previous line — extend the run
            ++runCount;
        } else {
            flushRun();
            runSig = sig;
            runStartIdx = i;
            runCount = 1;
        }
    }
    flushRun();

    // Write back only if we actually reduced the size
    if (out.size() < lines.size()) {
        std::ofstream ofs(logPath, std::ios::trunc);
        if (ofs.is_open()) {
            for (const auto& ol : out)
                ofs << ol << '\n';
        }
    }
}

} // namespace rt
