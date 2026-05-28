// ROUNDTABLE NLE — entry point

#include <QApplication>
#include <QGuiApplication>
#include <QScreen>
#include <QDir>
#include <QFileInfo>
#include <QIcon>
#include <QLoggingCategory>
#include <QStyleFactory>
#include <QTimer>

#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <filesystem>

#ifdef _WIN32
#include <windows.h>
#include <shlobj.h>
#endif

#include "ui/App.h"
#include "ui/MainWindow.h"
#include "ui/SplashScreen.h"
#include "ui/UiScale.h"
#include "ui/NoElideTabStyle.h"
#include "ui/QtHelpers.h"
#include "CrashHandler.h"

// FFmpeg log suppression
#ifdef ROUNDTABLE_HAS_FFMPEG
extern "C" {
#include <libavutil/log.h>
}
#endif

// ── Event loop runner (simple wrapper for consistency)
static int runEventLoop(QApplication& app) { return app.exec(); }

int main(int argc, char* argv[])
{
    // Per-Monitor DPI v2 (must be set before ANY window is created)
#ifdef _WIN32
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
#endif

    // High DPI scaling
    QApplication::setHighDpiScaleFactorRoundingPolicy(
        Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);

    // Suppress harmless Qt warnings
    rt::installQtMessageFilter();

    // Enable debug heap: tracks allocations for leak detection at exit.
    // Full CHECK_ALWAYS_DF is too slow for startup (10-50x) — we skip it
    // here and rely on Windows' heap FailFast for corruption detection.
#ifdef _DEBUG
#include <crtdbg.h>
    _CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);
#endif

    // ── Unified log root ──────────────────────────────────────────────
    // All crash logs, minidumps, and spdlog output go to a single location:
    //   logs/  (alongside the executable)
    //
    // Detection logic: walk up from argv[0]'s parent directory looking for
    // CMakeLists.txt (dev build with build subdirectory).  If found, use
    // the project root as the log root.  Otherwise use the exe's own
    // directory (installed build — installer creates a logs/ folder).
    // NEVER use %LOCALAPPDATA% — logs must be easy to find.
    std::filesystem::path logRoot;
    {
        auto exeDir = std::filesystem::path(argv[0]).parent_path();
        auto probe = exeDir;
        bool foundProjectRoot = false;
        for (int i = 0; i < 5; ++i) {
            if (std::filesystem::exists(probe / "CMakeLists.txt")) {
                logRoot = probe / "logs";
                foundProjectRoot = true;
                break;
            }
            auto parent = probe.parent_path();
            if (parent == probe) break;
            probe = parent;
        }
        if (!foundProjectRoot) {
            // Installed build — logs go next to the exe.
            logRoot = exeDir / "logs";
        }
    }
    std::filesystem::create_directories(logRoot);

    // Crash handler — install with unified log root as crash directory
    rt::CrashHandler::install(logRoot);
    spdlog::info("Crash logs → {}", logRoot.string());

    // Deduplicate the crash log from previous sessions so it doesn't
    // balloon during crash loops (e.g., paint recursion or TDR storms).
    rt::CrashHandler::deduplicateCrashLog();

    // Logging: console + perf_log.txt (in unified log root)
    {
        auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
        std::shared_ptr<spdlog::sinks::basic_file_sink_mt> file_sink;
        try {
            auto logPath = logRoot / "perf_log.txt";
            file_sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(
                logPath.string(), /*truncate=*/true);
        } catch (const std::exception& e) {
            spdlog::warn("Could not create file logger: {}", e.what());
        }
        auto logger = std::make_shared<spdlog::logger>("multi",
            file_sink
                ? spdlog::sinks_init_list{console_sink, file_sink}
                : spdlog::sinks_init_list{console_sink});

        // Release builds default to `warn` so the 891+ spdlog::info calls
        // on the happy path don't flood perf_log.txt / the console.
        // Override with ROUNDTABLE_LOG_LEVEL=trace|debug|info|warn|err|off
        // for support-channel bug reports.
        auto logLevel = spdlog::level::warn;
        if (const char* env = std::getenv("ROUNDTABLE_LOG_LEVEL")) {
            auto parsed = spdlog::level::from_str(env);
            if (parsed != spdlog::level::off || std::string(env) == "off")
                logLevel = parsed;
        }
        logger->set_level(logLevel);
        logger->set_pattern("[%H:%M:%S.%e] [%^%l%$] %v");
        logger->flush_on(spdlog::level::warn);
        spdlog::set_default_logger(logger);
        spdlog::flush_every(std::chrono::seconds(1));
    }
    spdlog::info("ROUNDTABLE NLE v{} starting", ROUNDTABLE_VERSION);

    // Windows timer resolution (1ms for accurate QTimer)
#ifdef _WIN32
    timeBeginPeriod(1);

    // Prevent OBS graphics hook from injecting into Roundtable's Vulkan
    // swapchain.  OBS's ow-graphics-hook64.dll can cause ACCESS_VIOLATION
    // when it tries to hook Vulkan present calls during playback, as seen
    // in crash logs (CODE=0xC0000005 at ow-graphics-hook64.dll+0x89b1).
    // This env var tells the OBS hook DLL to skip this process entirely.
    // Also set DISABLE_OGL_HOOK as a belt-and-suspenders measure — some
    // OBS versions check both names.
    ::SetEnvironmentVariableW(L"DISABLE_VULKAN_HOOK", L"1");
    ::SetEnvironmentVariableW(L"DISABLE_OGL_HOOK", L"1");
#endif

    // Silence FFmpeg logging
#ifdef ROUNDTABLE_HAS_FFMPEG
    av_log_set_level(AV_LOG_QUIET);
#endif

    // Qt Application
    QApplication qtApp(argc, argv);
    qtApp.setStyle(new NoElideTabStyle(qtApp.style()->objectName()));
    qtApp.setApplicationName("ROUNDTABLE NLE");
    qtApp.setApplicationVersion(ROUNDTABLE_VERSION);
    qtApp.setOrganizationName("Roundtable");

    // Initialise global UI scale from primary screen
    if (auto* primary = QGuiApplication::primaryScreen()) {
        const double primaryLogical  = primary->logicalDotsPerInch();
        const double primaryPhysical = primary->physicalDotsPerInch();
        const double primaryDpr      = primary->devicePixelRatio();
        double baseline = 96.0;
        baseline = std::max(baseline, primaryLogical);
        baseline = std::max(baseline, primaryPhysical);
        baseline = std::max(baseline, 96.0 * primaryDpr);
        rt::UiScale::setBaselineDpi(baseline);
        rt::UiScale::updateForScreen(primary);
        spdlog::info("UiScale: baseline={:.0f} factor={:.2f} (primary logicalDpi={:.0f} physDpi={:.0f} dpr={:.2f})",
                     baseline, rt::UiScale::factor(),
                     primaryLogical, primaryPhysical, primaryDpr);
    }

#ifdef _WIN32
    // AppUserModelID for taskbar icon stability
    {
        using SetAppIdFn = HRESULT (WINAPI*)(PCWSTR);
        HMODULE shell32 = LoadLibraryW(L"shell32.dll");
        if (shell32) {
            auto setAppId = reinterpret_cast<SetAppIdFn>(
                GetProcAddress(shell32, "SetCurrentProcessExplicitAppUserModelID"));
            if (setAppId)
                setAppId(L"Roundtable.NLE");
            FreeLibrary(shell32);
        }
    }
#endif

    // Set CWD to project root (PORTABILITY)
    QString projectRoot = rt::findProjectRoot();
    QDir::setCurrent(projectRoot);
    std::filesystem::current_path(projectRoot.toStdString());
    spdlog::info("Project root: {}", projectRoot.toStdString());

    // Application icon
    QIcon appIcon;
    {
        QString icoPath = QDir(projectRoot).filePath("build/icon.ico");
        QString pngPath = QDir(projectRoot).filePath("icon.png");
        if (QFileInfo::exists(icoPath))
            appIcon = QIcon(icoPath);
        else if (QFileInfo::exists(pngPath))
            appIcon = QIcon(pngPath);
        if (!appIcon.isNull())
            qtApp.setWindowIcon(appIcon);
    }

    // Register .rtp file association (Windows, per-user)
#ifdef _WIN32
    {
        QString exePath = QCoreApplication::applicationFilePath();
        exePath.replace('/', '\\');

        auto setReg = [](const QString& key, const QString& value) {
            HKEY hKey;
            std::wstring wKey = key.toStdWString();
            std::wstring wVal = value.toStdWString();
            if (RegCreateKeyExW(HKEY_CURRENT_USER, wKey.c_str(), 0, nullptr,
                                0, KEY_SET_VALUE, nullptr, &hKey, nullptr) == ERROR_SUCCESS) {
                RegSetValueExW(hKey, nullptr, 0, REG_SZ,
                               reinterpret_cast<const BYTE*>(wVal.c_str()),
                               static_cast<DWORD>((wVal.size() + 1) * sizeof(wchar_t)));
                RegCloseKey(hKey);
            }
        };

        setReg("Software\\Classes\\.rtp", "Roundtable.Project");
        setReg("Software\\Classes\\Roundtable.Project", "Roundtable Project File");
        setReg("Software\\Classes\\Roundtable.Project\\DefaultIcon",
               "\"" + exePath + "\",0");
        setReg("Software\\Classes\\Roundtable.Project\\shell\\open\\command",
               "\"" + exePath + "\" \"%1\"");

        SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    }
#endif

    // ── Splash screen ────────────────────────────────────────────────────
    rt::SplashScreen splash(
        QDir(projectRoot).filePath("icon.png"), ROUNDTABLE_VERSION);
    splash.show();
    splash.setStatus("Initializing...");
    splash.setProgress(5);

    // App services
    rt::App app;

    splash.setStatus("Loading subsystems...");
    splash.setProgress(15);
    if (!app.init())
    {
        spdlog::critical("Failed to initialize application services");
        return 1;
    }
    splash.setProgress(60);

    splash.setStatus("Building interface...");
    if (!app.createMainWindow())
    {
        spdlog::critical("Failed to create main window");
        return 1;
    }
    splash.setProgress(90);

    // Enforce icon on main window
    if (auto* mw = app.mainWindow()) {
        if (!appIcon.isNull())
            mw->setWindowIcon(appIcon);
#ifdef _WIN32
        HWND hwnd = reinterpret_cast<HWND>(mw->winId());
        HICON hSmall = static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr),
            MAKEINTRESOURCEW(1), IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR));
        HICON hBig = static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr),
            MAKEINTRESOURCEW(1), IMAGE_ICON, 32, 32, LR_DEFAULTCOLOR));
        if (hSmall)
            SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(hSmall));
        if (hBig)
            SendMessageW(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(hBig));
#endif
    }

    // ── Handle --capture-workspace (continued from createMainWindow) ────
    // If we captured the workspace, skip the splash fade and timers —
    // the app will quit as soon as the event loop starts.
    bool capturing = false;
    for (int i = 1; i < argc; ++i) {
        if (QString::fromLocal8Bit(argv[i]) == "--capture-workspace") {
            capturing = true;
            break;
        }
    }
    if (capturing) {
        splash.setStatus("Done");
        splash.setProgress(100);
        splash.finish(nullptr);
        spdlog::info("Workspace captured — entering event loop for clean exit");
        int result = runEventLoop(qtApp);
        spdlog::info("ROUNDTABLE NLE shutdown complete");
        return result;
    }

    splash.setStatus("Ready");
    splash.setProgress(100);

    // Fade out splash and raise main window
    splash.finish(app.mainWindow());

    // Check for auto-save recovery (after splash is fully dismissed so the
    // modal QMessageBox it shows won't be blocked by a lingering splash).
    QTimer::singleShot(0, app.mainWindow(), &rt::MainWindow::checkCrashRecovery);

    // Check for updates (deferred to avoid slowing startup)
    QTimer::singleShot(3000, app.mainWindow(), &rt::MainWindow::onCheckForUpdatesSilent);

    spdlog::info("ROUNDTABLE NLE ready — entering event loop");

    int result = runEventLoop(qtApp);

    spdlog::info("ROUNDTABLE NLE shutdown complete");
    return result;
}
