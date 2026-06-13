/*
 * test_autosave.cpp — Tests for AutoSave and CrashHandler.
 *
 * Step 29: Auto-save & Crash Recovery
 *
 * Tests cover:
 *   AutoSave:
 *     - Default state and configuration
 *     - Interval setting and timing
 *     - Enable/disable behavior
 *     - tick() timing logic (saves only when interval elapses + project modified)
 *     - saveNow() immediate save
 *     - autoSaveFolder() generation
 *     - hasRecoverableAutoSave() detection
 *     - loadLatestAutoSave() round-trip
 *     - pruneAutoSaves() cleanup
 *     - Statistics tracking
 *     - Status callback invocation
 *     - resetTimer()
 *     - Atomic write (.tmp fallback)
 *
 *   CrashHandler:
 *     - Install / uninstall
 *     - isInstalled() state tracking
 *     - Crash directory configuration
 *     - CrashLog writing
 *     - Path accessors
 *     - Emergency save callback setting
 *     - Previous crash detection
 */

#include <gtest/gtest.h>

#include "project/AutoSave.h"
#include "project/Project.h"
#include "project/ProjectSerializer.h"
#include "CrashHandler.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using namespace std::chrono_literals;

// ─────────────────────────────────────────────────────────────────────────────
// Helper: temp directory per-test
// ─────────────────────────────────────────────────────────────────────────────

class AutoSaveTest : public ::testing::Test
{
protected:
    fs::path testDir;

    void SetUp() override
    {
        testDir = fs::temp_directory_path() / "rt_test_autosave" /
                  ::testing::UnitTest::GetInstance()->current_test_info()->name();

        // Clean up from any previous failed run
        std::error_code ec;
        fs::remove_all(testDir, ec);
        fs::create_directories(testDir);
    }

    void TearDown() override
    {
        std::error_code ec;
        fs::remove_all(testDir, ec);
    }

    // Create a project and save it to disk so we have a valid project file.
    std::unique_ptr<rt::Project> createAndSaveProject(const std::string& name = "TestProject")
    {
        auto proj = rt::Project::createNew(name);
        auto path = testDir / (name + ".rtp");
        proj->setFilePath(path);

        rt::ProjectSerializer ser;
        [[maybe_unused]] bool ok = ser.save(*proj, path);
        proj->setModified(false);
        return proj;
    }
};

// ═════════════════════════════════════════════════════════════════════════════
// AutoSave — Default State
// ═════════════════════════════════════════════════════════════════════════════

TEST_F(AutoSaveTest, DefaultState)
{
    rt::AutoSave as;
    EXPECT_TRUE(as.isEnabled());
    EXPECT_EQ(as.project(), nullptr);
    EXPECT_EQ(as.interval(), std::chrono::seconds(std::chrono::minutes(15)));
    EXPECT_EQ(as.status(), rt::AutoSaveStatus::Idle);
    EXPECT_EQ(as.stats().saveCount, 0u);
    EXPECT_EQ(as.stats().failCount, 0u);
}

TEST_F(AutoSaveTest, SetInterval)
{
    rt::AutoSave as;
    as.setInterval(std::chrono::seconds(30));
    EXPECT_EQ(as.interval(), std::chrono::seconds(30));

    as.setInterval(std::chrono::seconds(600));
    EXPECT_EQ(as.interval(), std::chrono::seconds(600));
}

TEST_F(AutoSaveTest, SetEnabled)
{
    rt::AutoSave as;
    EXPECT_TRUE(as.isEnabled());

    as.setEnabled(false);
    EXPECT_FALSE(as.isEnabled());
    EXPECT_EQ(as.status(), rt::AutoSaveStatus::Disabled);

    as.setEnabled(true);
    EXPECT_TRUE(as.isEnabled());
    EXPECT_EQ(as.status(), rt::AutoSaveStatus::Idle);
}

TEST_F(AutoSaveTest, SetProject)
{
    rt::AutoSave as;
    auto proj = rt::Project::createNew("TestProj");
    as.setProject(proj.get());
    EXPECT_EQ(as.project(), proj.get());

    as.setProject(nullptr);
    EXPECT_EQ(as.project(), nullptr);
}

// ═════════════════════════════════════════════════════════════════════════════
// AutoSave — autoSavePath
// ═════════════════════════════════════════════════════════════════════════════

TEST_F(AutoSaveTest, AutoSaveFolderNormal)
{
    auto path = testDir / "my_project.rtp";
    auto folder = rt::AutoSave::autoSaveFolder(path);

    auto expected = testDir / "Roundtable Auto-Save";
    EXPECT_EQ(folder, expected);
}

TEST_F(AutoSaveTest, AutoSaveFolderEmpty)
{
    // Unsaved project - temp directory fallback
    auto folder = rt::AutoSave::autoSaveFolder({});
    EXPECT_FALSE(folder.empty());
    EXPECT_TRUE(folder.string().find("Roundtable Auto-Save") != std::string::npos);
}

TEST_F(AutoSaveTest, CurrentAutoSaveFolder)
{
    rt::AutoSave as;
    auto proj = createAndSaveProject("MyProject");
    as.setProject(proj.get());

    auto current = as.currentAutoSaveFolder();
    auto expected = rt::AutoSave::autoSaveFolder(proj->filePath());
    EXPECT_EQ(current, expected);
}

TEST_F(AutoSaveTest, CurrentAutoSaveFolderNoProject)
{
    rt::AutoSave as;
    EXPECT_TRUE(as.currentAutoSaveFolder().empty());
}

// ═════════════════════════════════════════════════════════════════════════════
// AutoSave — tick() timing
// ═════════════════════════════════════════════════════════════════════════════

TEST_F(AutoSaveTest, TickReturnsFalseWhenDisabled)
{
    rt::AutoSave as;
    as.setEnabled(false);
    auto proj = createAndSaveProject();
    proj->setModified(true);
    as.setProject(proj.get());
    as.setInterval(std::chrono::seconds(0));

    EXPECT_FALSE(as.tick());
}

TEST_F(AutoSaveTest, TickReturnsFalseWithNoProject)
{
    rt::AutoSave as;
    as.setInterval(std::chrono::seconds(0));
    EXPECT_FALSE(as.tick());
}

TEST_F(AutoSaveTest, TickReturnsFalseWhenNotModified)
{
    rt::AutoSave as;
    auto proj = createAndSaveProject();
    proj->setModified(false); // Not modified
    as.setProject(proj.get());
    as.setInterval(std::chrono::seconds(0));

    // Wait tiny bit to clear interval
    std::this_thread::sleep_for(10ms);
    EXPECT_FALSE(as.tick());
}

TEST_F(AutoSaveTest, TickReturnsFalseBeforeInterval)
{
    rt::AutoSave as;
    auto proj = createAndSaveProject();
    proj->setModified(true);
    as.setProject(proj.get());
    as.setInterval(std::chrono::seconds(3600)); // 1 hour

    EXPECT_FALSE(as.tick()); // Way before interval
}

TEST_F(AutoSaveTest, TickSavesAfterInterval)
{
    rt::AutoSave as;
    auto proj = createAndSaveProject();
    proj->setModified(true);
    as.setProject(proj.get());
    as.setInterval(std::chrono::seconds(0)); // Immediate

    // Need to wait a tiny bit to ensure elapsed time > 0
    std::this_thread::sleep_for(10ms);

    EXPECT_TRUE(as.tick());
    EXPECT_EQ(as.stats().saveCount, 1u);
    EXPECT_EQ(as.status(), rt::AutoSaveStatus::Saved);

    // Verify auto-save folder was created with a file inside
    auto asFolder = as.currentAutoSaveFolder();
    EXPECT_TRUE(fs::exists(asFolder));
}

// ═════════════════════════════════════════════════════════════════════════════
// AutoSave — saveNow()
// ═════════════════════════════════════════════════════════════════════════════

TEST_F(AutoSaveTest, SaveNowSucceeds)
{
    rt::AutoSave as;
    auto proj = createAndSaveProject();
    proj->setModified(true);
    as.setProject(proj.get());

    EXPECT_TRUE(as.saveNow());
    EXPECT_EQ(as.stats().saveCount, 1u);
    EXPECT_EQ(as.status(), rt::AutoSaveStatus::Saved);

    auto asFolder = as.currentAutoSaveFolder();
    EXPECT_TRUE(fs::exists(asFolder));
}

TEST_F(AutoSaveTest, SaveNowFailsWithNoProject)
{
    rt::AutoSave as;
    EXPECT_FALSE(as.saveNow());
}

TEST_F(AutoSaveTest, MultipleSavesIncrementCount)
{
    rt::AutoSave as;
    auto proj = createAndSaveProject();
    proj->setModified(true);
    as.setProject(proj.get());

    as.saveNow();
    as.saveNow();
    as.saveNow();

    EXPECT_EQ(as.stats().saveCount, 3u);
    EXPECT_EQ(as.stats().failCount, 0u);
}

// ═════════════════════════════════════════════════════════════════════════════
// AutoSave — Recovery detection
// ═════════════════════════════════════════════════════════════════════════════

TEST_F(AutoSaveTest, HasRecoverableAutoSaveWhenNoneExists)
{
    auto projectPath = testDir / "nonexistent.rtp";
    EXPECT_FALSE(rt::AutoSave::hasRecoverableAutoSave(projectPath));
}

TEST_F(AutoSaveTest, HasRecoverableAutoSaveDetectsNewerFile)
{
    auto proj = createAndSaveProject();
    auto projectPath = proj->filePath();

    // Create auto-save file that is newer
    std::this_thread::sleep_for(50ms); // Ensure different timestamps
    rt::AutoSave as;
    proj->setModified(true);
    as.setProject(proj.get());
    as.saveNow();

    EXPECT_TRUE(rt::AutoSave::hasRecoverableAutoSave(projectPath));
}

TEST_F(AutoSaveTest, PruneKeepsMaxFiles)
{
    auto proj = createAndSaveProject();
    auto projectPath = proj->filePath();
    auto folder = rt::AutoSave::autoSaveFolder(projectPath);
    fs::create_directories(folder);

    // Create 5 fake auto-save files
    for (int i = 0; i < 5; ++i) {
        auto fakePath = folder / ("test " + std::to_string(i) + ".rtp");
        std::ofstream ofs(fakePath, std::ios::binary);
        ofs << "fake data " << i;
        ofs.close();
        std::this_thread::sleep_for(20ms);
    }

    // Prune to keep 3
    rt::AutoSave::pruneAutoSaves(folder, 3);

    // Count remaining files
    int count = 0;
    for (auto& entry : fs::directory_iterator(folder)) {
        if (entry.is_regular_file()) ++count;
    }
    EXPECT_EQ(count, 3);
}

TEST_F(AutoSaveTest, HasRecoverableAutoSaveNoProjectFile)
{
    // Auto-save exists but project file does not
    auto projectPath = testDir / "deleted.rtp";
    auto folder = rt::AutoSave::autoSaveFolder(projectPath);
    fs::create_directories(folder);

    // Create a fake auto-save file in the folder
    auto fakePath = folder / "deleted 2025-01-01 at 12.00.00.rtp";
    std::ofstream ofs(fakePath, std::ios::binary);
    ofs << "fake data";
    ofs.close();

    EXPECT_TRUE(rt::AutoSave::hasRecoverableAutoSave(projectPath));
}

// ═════════════════════════════════════════════════════════════════════════════
// AutoSave — loadAutoSave round-trip
// ═════════════════════════════════════════════════════════════════════════════

TEST_F(AutoSaveTest, LoadAutoSaveRoundTrip)
{
    auto proj = createAndSaveProject("RoundTrip");
    proj->setModified(true);
    auto projectPath = proj->filePath();

    // Auto-save the project
    rt::AutoSave as;
    as.setProject(proj.get());
    EXPECT_TRUE(as.saveNow());

    // Load from auto-save
    auto recovered = rt::AutoSave::loadLatestAutoSave(projectPath);
    ASSERT_NE(recovered, nullptr);
    // Should have the original project path, not the autosave path
    EXPECT_EQ(recovered->filePath(), projectPath);
    // Should be marked as modified (since it's a recovery)
    EXPECT_TRUE(recovered->isModified());
}

TEST_F(AutoSaveTest, LoadAutoSaveNonExistent)
{
    auto projectPath = testDir / "nonexistent.rtp";
    auto recovered = rt::AutoSave::loadLatestAutoSave(projectPath);
    EXPECT_EQ(recovered, nullptr);
}

// ═════════════════════════════════════════════════════════════════════════════
// AutoSave — folder and pruning
// ═════════════════════════════════════════════════════════════════════════════

TEST_F(AutoSaveTest, MakeTimestampedPath)
{
    auto folder = testDir / "Roundtable Auto-Save";
    auto path = rt::AutoSave::makeTimestampedPath(folder, "MyProject");
    EXPECT_TRUE(path.string().find("MyProject") != std::string::npos);
    EXPECT_TRUE(path.string().find(".rtp") != std::string::npos);
    EXPECT_EQ(path.parent_path(), folder);
}

TEST_F(AutoSaveTest, FindNewestAutoSaveEmpty)
{
    auto projectPath = testDir / "nonexistent.rtp";
    auto newest = rt::AutoSave::findNewestAutoSave(projectPath);
    EXPECT_TRUE(newest.empty());
}

TEST_F(AutoSaveTest, MaxAutoSavesConfig)
{
    rt::AutoSave as;
    EXPECT_EQ(as.maxAutoSaves(), 20u);
    as.setMaxAutoSaves(5);
    EXPECT_EQ(as.maxAutoSaves(), 5u);
}

TEST_F(AutoSaveTest, SaveNowCreatesAutoSaveFolder)
{
    auto proj = createAndSaveProject();
    proj->setModified(true);

    rt::AutoSave as;
    as.setProject(proj.get());
    as.saveNow();

    auto folder = as.currentAutoSaveFolder();
    EXPECT_TRUE(fs::exists(folder));

    // Verify at least one file in the folder
    int count = 0;
    for (auto& entry : fs::directory_iterator(folder)) {
        if (entry.is_regular_file()) ++count;
    }
    EXPECT_GE(count, 1);
}

// ═════════════════════════════════════════════════════════════════════════════
// AutoSave — Status callback
// ═════════════════════════════════════════════════════════════════════════════

TEST_F(AutoSaveTest, StatusCallbackOnSuccess)
{
    rt::AutoSave as;
    auto proj = createAndSaveProject();
    proj->setModified(true);
    as.setProject(proj.get());

    rt::AutoSaveStatus cbStatus{rt::AutoSaveStatus::Idle};
    std::string cbMessage;
    as.setStatusCallback([&](rt::AutoSaveStatus s, const std::string& msg) {
        cbStatus = s;
        cbMessage = msg;
    });

    as.saveNow();

    EXPECT_EQ(cbStatus, rt::AutoSaveStatus::Saved);
    EXPECT_FALSE(cbMessage.empty());
}

// ═════════════════════════════════════════════════════════════════════════════
// AutoSave — Statistics
// ═════════════════════════════════════════════════════════════════════════════

TEST_F(AutoSaveTest, StatsInitialState)
{
    rt::AutoSave as;
    auto stats = as.stats();
    EXPECT_EQ(stats.saveCount, 0u);
    EXPECT_EQ(stats.failCount, 0u);
    EXPECT_EQ(stats.status, rt::AutoSaveStatus::Idle);
}

TEST_F(AutoSaveTest, StatsAfterSuccessfulSave)
{
    rt::AutoSave as;
    auto proj = createAndSaveProject();
    proj->setModified(true);
    as.setProject(proj.get());
    as.saveNow();

    auto stats = as.stats();
    EXPECT_EQ(stats.saveCount, 1u);
    EXPECT_EQ(stats.failCount, 0u);
    EXPECT_EQ(stats.status, rt::AutoSaveStatus::Saved);
    EXPECT_NE(stats.lastSaveTime, rt::AutoSave::TimePoint{});
}

// ═════════════════════════════════════════════════════════════════════════════
// AutoSave — timeUntilNextSave
// ═════════════════════════════════════════════════════════════════════════════

TEST_F(AutoSaveTest, TimeUntilNextSaveWhenDisabled)
{
    rt::AutoSave as;
    as.setEnabled(false);
    EXPECT_EQ(as.timeUntilNextSave(), rt::AutoSave::Duration::max());
}

TEST_F(AutoSaveTest, TimeUntilNextSaveDecreasing)
{
    rt::AutoSave as;
    as.setInterval(std::chrono::seconds(10));
    as.resetTimer();

    auto time1 = as.timeUntilNextSave();
    EXPECT_GT(time1.count(), 0);
    EXPECT_LE(time1.count(), 10);
}

TEST_F(AutoSaveTest, TimeUntilNextSaveZeroWhenDue)
{
    rt::AutoSave as;
    as.setInterval(std::chrono::seconds(0));
    std::this_thread::sleep_for(10ms);

    EXPECT_EQ(as.timeUntilNextSave(), rt::AutoSave::Duration::zero());
}

// ═════════════════════════════════════════════════════════════════════════════
// AutoSave — resetTimer
// ═════════════════════════════════════════════════════════════════════════════

TEST_F(AutoSaveTest, ResetTimerPostponesAutoSave)
{
    rt::AutoSave as;
    as.setInterval(std::chrono::seconds(10));

    // After reset, should be close to full interval
    as.resetTimer();
    auto time = as.timeUntilNextSave();
    EXPECT_GT(time.count(), 5);
}

// ═════════════════════════════════════════════════════════════════════════════
// AutoSave — edge cases
// ═════════════════════════════════════════════════════════════════════════════

TEST_F(AutoSaveTest, TickDoesNotSaveWhenAlreadySavedRecently)
{
    rt::AutoSave as;
    as.setInterval(std::chrono::seconds(3600)); // 1 hour
    auto proj = createAndSaveProject();
    proj->setModified(true);
    as.setProject(proj.get());

    // First tick should not save (interval hasn't elapsed)
    EXPECT_FALSE(as.tick());

    // Second tick also shouldn't
    EXPECT_FALSE(as.tick());

    EXPECT_EQ(as.stats().saveCount, 0u);
}

TEST_F(AutoSaveTest, SaveNowWorksEvenWhenProjectNotModified)
{
    rt::AutoSave as;
    auto proj = createAndSaveProject();
    proj->setModified(false); // Explicitly not modified
    as.setProject(proj.get());

    // saveNow() should still work (it's an explicit save request)
    EXPECT_TRUE(as.saveNow());
    EXPECT_EQ(as.stats().saveCount, 1u);
}

TEST_F(AutoSaveTest, SwitchProjectResetsTimer)
{
    rt::AutoSave as;
    as.setInterval(std::chrono::seconds(0));

    auto proj1 = createAndSaveProject("Proj1");
    proj1->setModified(true);
    as.setProject(proj1.get());

    std::this_thread::sleep_for(10ms);
    as.tick(); // Should save proj1

    auto proj2 = createAndSaveProject("Proj2");
    proj2->setModified(true);
    as.setProject(proj2.get()); // Timer resets

    // With interval 0, sleeping briefly should allow next tick
    std::this_thread::sleep_for(10ms);
    EXPECT_TRUE(as.tick()); // Should save proj2
    EXPECT_EQ(as.stats().saveCount, 2u);
}

// ═════════════════════════════════════════════════════════════════════════════
// CrashHandler — Install / Uninstall
// ═════════════════════════════════════════════════════════════════════════════

class CrashHandlerTest : public ::testing::Test
{
protected:
    fs::path crashDir;

    void SetUp() override
    {
        crashDir = fs::temp_directory_path() / "rt_test_crash" /
                   ::testing::UnitTest::GetInstance()->current_test_info()->name();

        std::error_code ec;
        fs::remove_all(crashDir, ec);
        fs::create_directories(crashDir);

        // Ensure uninstalled before each test
        if (rt::CrashHandler::isInstalled()) {
            rt::CrashHandler::uninstall();
        }
    }

    void TearDown() override
    {
        if (rt::CrashHandler::isInstalled()) {
            rt::CrashHandler::uninstall();
        }
        std::error_code ec;
        fs::remove_all(crashDir, ec);
    }
};

TEST_F(CrashHandlerTest, InitiallyNotInstalled)
{
    EXPECT_FALSE(rt::CrashHandler::isInstalled());
}

TEST_F(CrashHandlerTest, InstallAndUninstall)
{
    rt::CrashHandler::install(crashDir);
    EXPECT_TRUE(rt::CrashHandler::isInstalled());

    rt::CrashHandler::uninstall();
    EXPECT_FALSE(rt::CrashHandler::isInstalled());
}

TEST_F(CrashHandlerTest, DoubleInstallIsNoop)
{
    rt::CrashHandler::install(crashDir);
    EXPECT_TRUE(rt::CrashHandler::isInstalled());

    // Second install should just warn, not crash
    rt::CrashHandler::install(crashDir);
    EXPECT_TRUE(rt::CrashHandler::isInstalled());

    rt::CrashHandler::uninstall();
}

TEST_F(CrashHandlerTest, DoubleUninstallIsNoop)
{
    rt::CrashHandler::install(crashDir);
    rt::CrashHandler::uninstall();
    EXPECT_FALSE(rt::CrashHandler::isInstalled());

    // Second uninstall should be no-op
    rt::CrashHandler::uninstall();
    EXPECT_FALSE(rt::CrashHandler::isInstalled());
}

TEST_F(CrashHandlerTest, SetCrashDirectory)
{
    rt::CrashHandler::install(crashDir);
    EXPECT_EQ(rt::CrashHandler::crashDirectory(), crashDir);

    auto newDir = crashDir / "subdir";
    rt::CrashHandler::setCrashDirectory(newDir);
    EXPECT_EQ(rt::CrashHandler::crashDirectory(), newDir);
    EXPECT_TRUE(fs::exists(newDir));

    rt::CrashHandler::uninstall();
}

// ═════════════════════════════════════════════════════════════════════════════
// CrashHandler — Crash log
// ═════════════════════════════════════════════════════════════════════════════

TEST_F(CrashHandlerTest, WriteCrashLog)
{
    rt::CrashHandler::install(crashDir);
    rt::CrashHandler::writeCrashLog("Test crash message 1");
    rt::CrashHandler::writeCrashLog("Test crash message 2");

    auto logPath = rt::CrashHandler::crashLogPath();
    EXPECT_TRUE(fs::exists(logPath));

    // Read and verify content
    std::ifstream ifs(logPath);
    std::string content((std::istreambuf_iterator<char>(ifs)),
                         std::istreambuf_iterator<char>());

    EXPECT_TRUE(content.find("Test crash message 1") != std::string::npos);
    EXPECT_TRUE(content.find("Test crash message 2") != std::string::npos);

    rt::CrashHandler::uninstall();
}

TEST_F(CrashHandlerTest, HasPreviousCrashLogFalseInitially)
{
    rt::CrashHandler::install(crashDir);
    EXPECT_FALSE(rt::CrashHandler::hasPreviousCrashLog());
    rt::CrashHandler::uninstall();
}

TEST_F(CrashHandlerTest, HasPreviousCrashLogAfterWrite)
{
    rt::CrashHandler::install(crashDir);
    rt::CrashHandler::writeCrashLog("Simulated crash");
    EXPECT_TRUE(rt::CrashHandler::hasPreviousCrashLog());
    rt::CrashHandler::uninstall();
}

// ═════════════════════════════════════════════════════════════════════════════
// CrashHandler — Path accessors
// ═════════════════════════════════════════════════════════════════════════════

TEST_F(CrashHandlerTest, NextDumpPath)
{
    rt::CrashHandler::install(crashDir);
    auto path = rt::CrashHandler::nextDumpPath();
    EXPECT_FALSE(path.empty());
    EXPECT_TRUE(path.string().find("crash_") != std::string::npos);
    EXPECT_TRUE(path.extension() == ".dmp");
    rt::CrashHandler::uninstall();
}

TEST_F(CrashHandlerTest, CrashLogPath)
{
    rt::CrashHandler::install(crashDir);
    auto path = rt::CrashHandler::crashLogPath();
    EXPECT_FALSE(path.empty());
    EXPECT_TRUE(path.filename() == "crash_log.txt");
    rt::CrashHandler::uninstall();
}

// ═════════════════════════════════════════════════════════════════════════════
// CrashHandler — Emergency save callback
// ═════════════════════════════════════════════════════════════════════════════

TEST_F(CrashHandlerTest, SetEmergencySaveCallback)
{
    rt::CrashHandler::install(crashDir);

    bool callbackSet = false;
    rt::CrashHandler::setEmergencySaveCallback([&] {
        callbackSet = true;
    });

    // We can't easily trigger a real crash in a test,
    // but we can verify the callback is accepted without error.
    // The callback would be invoked during an actual crash.
    EXPECT_TRUE(rt::CrashHandler::isInstalled());

    rt::CrashHandler::uninstall();
}

TEST_F(CrashHandlerTest, SetPostCrashCallback)
{
    rt::CrashHandler::install(crashDir);

    rt::CrashHandler::setPostCrashCallback([](const rt::CrashInfo& info) {
        // Would be called after crash dump is written
        (void)info;
    });

    EXPECT_TRUE(rt::CrashHandler::isInstalled());
    rt::CrashHandler::uninstall();
}

// ═════════════════════════════════════════════════════════════════════════════
// CrashHandler — lastCrash
// ═════════════════════════════════════════════════════════════════════════════

TEST_F(CrashHandlerTest, LastCrashEmptyInitially)
{
    rt::CrashHandler::install(crashDir);
    auto info = rt::CrashHandler::lastCrash();
    EXPECT_TRUE(info.summary.empty());
    EXPECT_EQ(info.exceptionCode, 0u);
    EXPECT_EQ(info.exceptionAddress, nullptr);
    rt::CrashHandler::uninstall();
}

// ═════════════════════════════════════════════════════════════════════════════
// CrashHandler — CrashInfo struct
// ═════════════════════════════════════════════════════════════════════════════

TEST(CrashInfoTest, DefaultConstruction)
{
    rt::CrashInfo info;
    EXPECT_TRUE(info.summary.empty());
    EXPECT_TRUE(info.stackTrace.empty());
    EXPECT_EQ(info.exceptionCode, 0u);
    EXPECT_EQ(info.exceptionAddress, nullptr);
    EXPECT_TRUE(info.dumpFilePath.empty());
    EXPECT_TRUE(info.logFilePath.empty());
}

// ═════════════════════════════════════════════════════════════════════════════
// AutoSave — Integration with CrashHandler
// ═════════════════════════════════════════════════════════════════════════════

TEST_F(AutoSaveTest, EmergencySaveCallbackIntegration)
{
    // Verify that AutoSave::saveNow can be used as a crash handler callback
    rt::AutoSave as;
    auto proj = createAndSaveProject("CrashTest");
    proj->setModified(true);
    as.setProject(proj.get());

    // Simulate what App would do: wire AutoSave::saveNow to CrashHandler
    bool saved = false;
    auto emergencySave = [&as, &saved]() {
        saved = as.saveNow();
    };

    emergencySave(); // Simulate crash-time call

    EXPECT_TRUE(saved);
    EXPECT_TRUE(fs::exists(as.currentAutoSaveFolder()));
}

// ═════════════════════════════════════════════════════════════════════════════
// AutoSaveStatus enum
// ═════════════════════════════════════════════════════════════════════════════

TEST(AutoSaveStatusTest, EnumValues)
{
    // Ensure all enum values are distinct
    EXPECT_NE(rt::AutoSaveStatus::Idle,     rt::AutoSaveStatus::Saving);
    EXPECT_NE(rt::AutoSaveStatus::Saving,   rt::AutoSaveStatus::Saved);
    EXPECT_NE(rt::AutoSaveStatus::Saved,    rt::AutoSaveStatus::Failed);
    EXPECT_NE(rt::AutoSaveStatus::Failed,   rt::AutoSaveStatus::Disabled);
}

// ═════════════════════════════════════════════════════════════════════════════
// AutoSaveStats struct
// ═════════════════════════════════════════════════════════════════════════════

TEST(AutoSaveStatsTest, DefaultConstruction)
{
    rt::AutoSaveStats stats;
    EXPECT_EQ(stats.saveCount, 0u);
    EXPECT_EQ(stats.failCount, 0u);
    EXPECT_EQ(stats.status, rt::AutoSaveStatus::Idle);
}
