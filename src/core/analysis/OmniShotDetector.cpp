/* OmniShotDetector — AI scene cut detection via OmniShotCut subprocess.
 * No Qt dependency — pure Win32 CreateProcess with stdout pipe for progress. */
#include "analysis/OmniShotDetector.h"
#include "PathUtils.h"
#include <spdlog/spdlog.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace rt {

namespace {

std::string readAll(HANDLE h) {
    std::string out;
    char buf[4096];
    DWORD avail = 0;
    while (PeekNamedPipe(h, nullptr, 0, nullptr, &avail, nullptr) && avail > 0) {
        DWORD read = 0;
        if (ReadFile(h, buf, std::min(avail, (DWORD)sizeof(buf) - 1), &read, nullptr) && read > 0) {
            out.append(buf, read);
        } else break;
    }
    return out;
}

double getJsonNum(const std::string& j, const std::string& k) {
    auto p = j.find("\"" + k + "\": ");
    if (p == std::string::npos) return 0.0;
    p += k.size() + 4;
    auto e = j.find_first_of(",}\n", p);
    try { return std::stod(j.substr(p, e == std::string::npos ? j.size() : e - p)); }
    catch (...) { return 0.0; }
}

std::string getJsonStr(const std::string& j, const std::string& k) {
    auto p = j.find("\"" + k + "\": \"");
    if (p == std::string::npos) return {};
    p += k.size() + 5;
    auto e = j.find('"', p);
    return e == std::string::npos ? std::string{} : j.substr(p, e - p);
}

std::vector<DetectedCut> parseCuts(const std::string& j) {
    std::vector<DetectedCut> cuts;
    auto a = j.find("\"cuts\":");
    if (a == std::string::npos) return cuts;
    a = j.find('[', a);
    if (a == std::string::npos) return cuts;
    auto ae = j.find(']', a);
    if (ae == std::string::npos) return cuts;
    std::string arr = j.substr(a, ae - a + 1);
    size_t pos = 0;
    while ((pos = arr.find('{', pos)) != std::string::npos) {
        auto oe = arr.find('}', pos);
        if (oe == std::string::npos) break;
        std::string obj = arr.substr(pos, oe - pos + 1);
        DetectedCut c;
        c.frameNumber = static_cast<int64_t>(getJsonNum(obj, "frame"));
        c.timeSeconds = getJsonNum(obj, "time");
        c.score = static_cast<float>(getJsonNum(obj, "score"));
        cuts.push_back(c);
        pos = oe + 1;
    }
    return cuts;
}

int parseProgress(const std::string& line) {
    // "Progress: 42%"
    auto p = line.find("Progress: ");
    if (p == std::string::npos) return -1;
    p += 10;
    try { return std::stoi(line.substr(p, line.find('%', p) - p)); }
    catch (...) { return -1; }
}

} // anon

OmniShotDetector::~OmniShotDetector() { cancel(); if (m_worker.joinable()) m_worker.join(); }
void OmniShotDetector::cancel() { m_cancelled.store(true); }
bool OmniShotDetector::isAvailable() {
    // Check both install dir and user data dir
    std::ifstream t1("tools/omnishotcut/detect.py");
    if (t1.good()) return true;
    std::ifstream t2(std::string(getenv("LOCALAPPDATA") ? getenv("LOCALAPPDATA") : "") + "/ROUNDTABLE/tools/omnishotcut/detect.py");
    return t2.good();
}

void OmniShotDetector::detectAsync(
    const std::filesystem::path& mp, int64_t sf, int64_t ef, float th,
    SceneDetectProgressFn onP, SceneDetectCompleteFn onC, SceneDetectErrorFn onE)
{
    if (m_worker.joinable()) m_worker.join();
    m_cancelled.store(false); m_running.store(true);
    m_worker = std::thread(&OmniShotDetector::workerFunc, this, mp, sf, ef, th,
                           std::move(onP), std::move(onC), std::move(onE));
}

void OmniShotDetector::workerFunc(
    std::filesystem::path mp, int64_t, int64_t, float,
    SceneDetectProgressFn onP, SceneDetectCompleteFn onC, SceneDetectErrorFn onE)
{
    spdlog::info("OmniShot: {}", pathToUtf8(mp));

    std::string outPath = pathToUtf8(std::filesystem::temp_directory_path() / "omni_out.json");
    std::string errPath = pathToUtf8(std::filesystem::temp_directory_path() / "omni_err.txt");
    std::string batPath = pathToUtf8(std::filesystem::temp_directory_path() / "omni_run.bat");
    std::remove(outPath.c_str());
    std::remove(errPath.c_str());

    // Build a .bat file so the command runs in cmd.exe (proper PATH, stderr redirect)
    {
        std::ofstream bat(batPath);
        bat << "@echo off\r\n";
        bat << "cd /d \"" << pathToUtf8(std::filesystem::current_path()) << "\"\r\n";
        bat << "python tools/omnishotcut/detect.py"
            << " --input \"" << pathToUtf8(mp) << "\""
            << " --checkpoint tools/omnishotcut/checkpoints/OmniShotCut_ckpt.pth"
            << " --output \"" << outPath << "\""
            << " 2>\"" << errPath << "\"\r\n";
    }

    std::string cmdStr = "cmd /c \"\"" + batPath + "\"\"";
    spdlog::info("OmniShot: cmd={}", batPath);

    // ── Create stdout pipe ──
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE hRead = nullptr, hWrite = nullptr;
    CreatePipe(&hRead, &hWrite, &sa, 0);
    SetHandleInformation(hRead, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si{}; si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = hWrite;
    PROCESS_INFORMATION pi{};

    if (!CreateProcessA(nullptr, cmdStr.data(), nullptr, nullptr, TRUE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        CloseHandle(hRead); CloseHandle(hWrite);
        std::remove(batPath.c_str());
        if (onE) onE("Failed to launch Python (err " + std::to_string(GetLastError()) + ")");
        m_running.store(false); return;
    }
    CloseHandle(hWrite);

    // ── Create a Windows Job Object so ALL descendants (cmd → python →
    // ffmpeg) are killed together, not just the immediate child. ──────
    HANDLE hJob = CreateJobObjectW(nullptr, nullptr);
    if (hJob) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli{};
        jeli.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(hJob, JobObjectExtendedLimitInformation,
                                &jeli, sizeof(jeli));
        AssignProcessToJobObject(hJob, pi.hProcess);
    }

    // ── Poll for stdout progress ──
    auto t0 = std::chrono::steady_clock::now();
    int lastPct = 0;
    while (!m_cancelled.load()) {
        if (WaitForSingleObject(pi.hProcess, 200) == WAIT_OBJECT_0) break;
        std::string out = readAll(hRead);
        if (!out.empty()) {
            // Parse progress lines
            std::istringstream iss(out);
            std::string line;
            while (std::getline(iss, line)) {
                int p = parseProgress(line);
                if (p >= 0 && p > lastPct) { lastPct = p; if (onP) onP(p / 100.0f, 0, 100); }
            }
        }
    }

    // Read any remaining output
    std::string outRemain = readAll(hRead);
    CloseHandle(hRead);

    // ── Check for cancellation BEFORE closing process handles ─────
    if (m_cancelled.load()) {
        // Close the job object — KILL_ON_JOB_CLOSE flag terminates the
        // entire process tree (cmd.exe + python.exe + any ffmpeg children
        // spawned by detect.py) so nothing gets orphaned.
        if (hJob) { CloseHandle(hJob); hJob = nullptr; }
        WaitForSingleObject(pi.hProcess, 5000);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        std::remove(outPath.c_str()); std::remove(batPath.c_str());
        m_running.store(false);
        return;
    }

    DWORD ec = 0; GetExitCodeProcess(pi.hProcess, &ec);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    if (hJob) CloseHandle(hJob); // clean up job (children should already be gone)
    if (onP) onP(1.0f, 100, 100);

    // Read error file
    std::string errText;
    { std::ifstream ef(errPath); if (ef.is_open()) { std::stringstream ss; ss << ef.rdbuf(); errText = ss.str(); } }

    std::ifstream in(outPath);
    if (!in.is_open()) {
        std::string msg = "OmniShotCut failed (code " + std::to_string(ec) + ")";
        if (!errText.empty()) { msg += ":\n" + errText; spdlog::warn("OmniShot: {}", errText); }
        if (onE) onE(msg);
        std::remove(outPath.c_str()); std::remove(errPath.c_str()); std::remove(batPath.c_str());
        m_running.store(false); return;
    }

    std::stringstream buf; buf << in.rdbuf(); in.close();
    std::remove(outPath.c_str()); std::remove(errPath.c_str()); std::remove(batPath.c_str());

    std::string json = buf.str();
    std::string err = getJsonStr(json, "error");
    if (!err.empty()) { if (onE) onE(err); m_running.store(false); return; }
    auto cuts = parseCuts(json);
    spdlog::info("OmniShot: {} cuts", cuts.size());
    if (onC) onC(std::move(cuts));
    m_running.store(false);
}

} // namespace rt
