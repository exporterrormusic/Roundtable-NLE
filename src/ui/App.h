/*
 * App — Application-level services and initialization.
 *
 * Step 26: Main Window & Workspace
 *
 * Owns all core subsystems (audio engine, timeline, command stack, etc.)
 * and the MainWindow. Provides a single point of initialization and
 * shutdown for the entire application.
 *
 * Lifecycle:
 *   1. Construct App (creates QApplication internally or uses existing)
 *   2. Call init() — applies theme, creates subsystems
 *   3. Call createMainWindow() — builds the UI
 *   4. Call exec() — enters event loop
 *   5. Destruction cleans up in reverse order
 */

#pragma once

#include "HardwareDiagnostics.h"

#include <memory>
#include <string>
#include <thread>
#include <vector>

class QApplication;

namespace rt {

// Forward declarations
class AudioEngine;
class AVSyncClock;
class CachePolicy;
class CommandStack;
class MainWindow;
class MediaPool;
class MediaSourceService;
class ModelManager;
class PlaybackController;
class Timeline;
class ShortcutManager;

/// Application initialization and top-level service container.
class App
{
public:
    App();
    ~App();

    // Non-copyable, non-movable
    App(const App&) = delete;
    App& operator=(const App&) = delete;

    // ── Initialization ──────────────────────────────────────────────────

    /// Initialize core subsystems and apply theme.
    /// Must be called after QApplication exists.
    bool init();

    /// Create and show the main window.
    /// Call after init().
    bool createMainWindow();

private:
    /// Show the overlay-hook advisory dialog using the snapshot captured
    /// at startup.  Called from a deferred singleShot in createMainWindow.
    void showOverlayAdvisoryDialog();

public:

    // ── Accessors ───────────────────────────────────────────────────────

    [[nodiscard]] MainWindow*      mainWindow()      const noexcept { return m_mainWindow.get(); }
    [[nodiscard]] Timeline*        timeline()        const noexcept { return m_timeline.get(); }
    [[nodiscard]] CommandStack*    commandStack()    const noexcept { return m_commandStack.get(); }
    [[nodiscard]] ShortcutManager* shortcutManager() const noexcept { return m_shortcutManager.get(); }
    [[nodiscard]] AudioEngine*          audioEngine()          const noexcept { return m_audioEngine.get(); }
    [[nodiscard]] PlaybackController*   playbackController()   const noexcept { return m_playbackController.get(); }
    [[nodiscard]] AVSyncClock*          syncClock()            const noexcept { return m_syncClock.get(); }
    [[nodiscard]] MediaPool*             mediaPool()            const noexcept { return m_mediaPool.get(); }
    [[nodiscard]] MediaSourceService*    mediaSourceService()   const noexcept { return m_mediaSourceService.get(); }
    [[nodiscard]] ModelManager*         modelManager()         const noexcept { return m_modelManager.get(); }
    [[nodiscard]] bool                  isInitialized()        const noexcept { return m_initialized; }

    /// Snapshot of the GPU classification gathered at startup.
    [[nodiscard]] const HardwareDiagnostics::GpuClassification&
        diagnosticsGpu() const noexcept { return m_diagnosticsGpu; }
    /// Snapshot of injected overlay hooks found at startup.
    [[nodiscard]] const std::vector<HardwareDiagnostics::InjectedHook>&
        diagnosticsHooks() const noexcept { return m_diagnosticsHooks; }

    // ── Singleton-ish access (convenience) ──────────────────────────────

    /// Get the global App instance. Only valid after construction.
    [[nodiscard]] static App* instance() noexcept { return s_instance; }

private:
    static App* s_instance;

    bool m_initialized{false};

    // ── Cache policy (budgets + VRAM pressure + access/eviction policy) ─
    // One object, two documented sub-roles — see CachePolicy.h (merged
    // from the former CacheCoordinator + UnifiedCache).
    std::unique_ptr<CachePolicy> m_cachePolicy;

    // ── Core subsystems (owned) ─────────────────────────────────────────
    std::unique_ptr<Timeline>        m_timeline;
    std::unique_ptr<CommandStack>    m_commandStack;
    std::unique_ptr<ShortcutManager> m_shortcutManager;
    std::unique_ptr<AudioEngine>          m_audioEngine;
    std::unique_ptr<PlaybackController>   m_playbackController;
    std::unique_ptr<AVSyncClock>          m_syncClock;
    std::unique_ptr<MediaPool>             m_mediaPool;
    std::unique_ptr<MediaSourceService>    m_mediaSourceService;
    std::unique_ptr<ModelManager>         m_modelManager;

    // ── Async init ──────────────────────────────────────────────────────
    std::thread m_scanThread;

    // ── UI (owned) ──────────────────────────────────────────────────────
    std::unique_ptr<MainWindow>      m_mainWindow;

    // ── Hardware diagnostics ─────────────────────────────────────────────
    // Captured after GpuContext::init() so the post-show advisory dialog
    // can be raised against the same snapshot the startup log printed.
    HardwareDiagnostics::GpuClassification        m_diagnosticsGpu;
    std::vector<HardwareDiagnostics::InjectedHook> m_diagnosticsHooks;
};

} // namespace rt
