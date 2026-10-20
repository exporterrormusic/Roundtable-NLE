/*
 * test_audio_sample_loader.cpp — AudioSampleLoader with a fake decoder on a
 * private MediaTaskQueue: files decode one at a time in request order,
 * nothing is queued twice, failed files are not retried, waiting actions
 * run once the decodes finish (a repeat is dropped), cancel drops them, and
 * destroying the loader mid-decode is safe.
 */

#include <gtest/gtest.h>

#include "panels/audio/AudioSampleLoader.h"
#include "MediaTaskQueue.h"
#include "../support/StallGuard.h"

#include <QCoreApplication>
#include <QTest>

#include <atomic>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

using rt::AudioSampleData;
using rt::AudioSampleLoader;

namespace {

void ensureApp()
{
    static int argc = 1;
    static char name[] = "test_audio_sample_loader";
    static char* argv[] = {name, nullptr};
    if (!QCoreApplication::instance()) new QCoreApplication(argc, argv);
}

AudioSampleData samplesOf(float value)
{
    AudioSampleData data;
    data.sampleRate = 16000;
    data.samples = rt::SampleBuffer(std::vector<float>(160, value));
    return data;
}

/// Holds fake decodes of chosen files until released or stopped.
struct DecodeGate
{
    std::mutex mutex;
    std::condition_variable_any cv;
    std::set<std::string> held;
    std::set<std::string> entered;

    void hold(const std::string& path)
    {
        std::lock_guard lock(mutex);
        held.insert(path);
    }
    void release(const std::string& path)
    {
        {
            std::lock_guard lock(mutex);
            held.erase(path);
        }
        cv.notify_all();
    }
    /// False when stopped while held.
    bool pass(const std::string& path, const std::stop_token& stop)
    {
        std::unique_lock lock(mutex);
        entered.insert(path);
        cv.notify_all();
        return cv.wait(lock, stop, [&] { return held.count(path) == 0; });
    }
    bool waitEntered(const std::string& path)
    {
        return QTest::qWaitFor([&] {
            std::lock_guard lock(mutex);
            return entered.count(path) > 0;
        }, 10000);
    }
};

/// The owner side: which files are imported and which are installed.
struct Owner
{
    std::set<std::string> imported;
    std::map<std::string, float> installed;   ///< path → first sample

    AudioSampleLoader::Hooks hooks()
    {
        return {
            [this](const std::string& path) { return imported.count(path) > 0; },
            [this](const std::string& path) { return installed.count(path) > 0; },
            [this](const std::string& path, AudioSampleData data) {
                installed[path] = data.samples.empty() ? -1.0f : data.samples.data()[0];
            }};
    }
};

class AudioSampleLoaderTest : public ::testing::Test
{
protected:
    void SetUp() override { ensureApp(); }

    /// Decodes record their order and how many run at once; "bad*" fails.
    AudioSampleLoader::DecodeFn recordingDecoder()
    {
        return [this](const std::string& path, const std::stop_token& stop)
                   -> std::optional<AudioSampleData> {
            const int now = ++running;
            int seen = maxRunning.load();
            while (now > seen && !maxRunning.compare_exchange_weak(seen, now)) {}
            {
                std::lock_guard lock(orderMutex);
                order.push_back(path);
            }
            const bool passed = gate.pass(path, stop);
            --running;
            if (!passed || path.rfind("bad", 0) == 0) return std::nullopt;
            return samplesOf(static_cast<float>(path.size()));
        };
    }

    std::vector<std::string> decodedOrder()
    {
        std::lock_guard lock(orderMutex);
        return order;
    }

    DecodeGate gate;
    std::atomic<int> running{0};
    std::atomic<int> maxRunning{0};
    std::mutex orderMutex;
    std::vector<std::string> order;
    Owner owner;
    rt::MediaTaskQueue tasks{4};   // last: its workers stop before the gate goes
};

} // namespace

TEST_F(AudioSampleLoaderTest, FilesDecodeOneAtATimeInRequestOrder)
{
    owner.imported = {"a.wav", "bb.wav", "ccc.wav"};
    AudioSampleLoader loader(owner.hooks(), recordingDecoder(), &tasks);
    loader.request({"a.wav", "bb.wav", "ccc.wav"});
    EXPECT_TRUE(loader.pending());
    ASSERT_TRUE(QTest::qWaitFor([&] { return !loader.pending(); }, 10000));

    EXPECT_EQ(decodedOrder(), (std::vector<std::string>{"a.wav", "bb.wav", "ccc.wav"}));
    EXPECT_EQ(maxRunning.load(), 1);
    EXPECT_EQ(owner.installed, (std::map<std::string, float>{
        {"a.wav", 5.0f}, {"bb.wav", 6.0f}, {"ccc.wav", 7.0f}}));
}

TEST_F(AudioSampleLoaderTest, LoadedQueuedOrDecodingFilesAreNotQueuedAgain)
{
    owner.imported = {"a.wav", "b.wav", "c.wav"};
    owner.installed["c.wav"] = 1.0f;
    gate.hold("a.wav");
    AudioSampleLoader loader(owner.hooks(), recordingDecoder(), &tasks);
    loader.request({"a.wav", "b.wav", "c.wav"});
    ASSERT_TRUE(gate.waitEntered("a.wav"));
    EXPECT_TRUE(loader.pending("a.wav"));
    EXPECT_TRUE(loader.pending("b.wav"));
    EXPECT_FALSE(loader.pending("c.wav"));

    loader.request({"a.wav", "b.wav", "c.wav"});
    gate.release("a.wav");
    ASSERT_TRUE(QTest::qWaitFor([&] { return !loader.pending(); }, 10000));
    EXPECT_EQ(decodedOrder(), (std::vector<std::string>{"a.wav", "b.wav"}));
}

TEST_F(AudioSampleLoaderTest, AFailedFileIsNotRetriedUntilForgotten)
{
    owner.imported = {"bad.wav", "good.wav"};
    AudioSampleLoader loader(owner.hooks(), recordingDecoder(), &tasks);
    loader.request({"bad.wav", "good.wav"});
    ASSERT_TRUE(QTest::qWaitFor([&] { return !loader.pending(); }, 10000));
    EXPECT_TRUE(loader.failed("bad.wav"));
    EXPECT_EQ(owner.installed.count("good.wav"), 1u);

    loader.request({"bad.wav"});
    EXPECT_FALSE(loader.pending());
    loader.loadNow({"bad.wav"});
    EXPECT_EQ(decodedOrder().size(), 2u);

    loader.forgetFailure("bad.wav");   // imported again
    loader.request({"bad.wav"});
    ASSERT_TRUE(QTest::qWaitFor([&] { return !loader.pending(); }, 10000));
    EXPECT_EQ(decodedOrder(), (std::vector<std::string>{"bad.wav", "good.wav", "bad.wav"}));
}

TEST_F(AudioSampleLoaderTest, FilesNoLongerImportedAreSkippedAndTheirResultsDropped)
{
    owner.imported = {"a.wav", "b.wav"};
    gate.hold("a.wav");
    AudioSampleLoader loader(owner.hooks(), recordingDecoder(), &tasks);
    loader.request({"a.wav", "b.wav"});
    ASSERT_TRUE(gate.waitEntered("a.wav"));

    owner.imported.clear();   // another script opened
    gate.release("a.wav");
    ASSERT_TRUE(QTest::qWaitFor([&] { return !loader.pending(); }, 10000));
    EXPECT_EQ(decodedOrder(), (std::vector<std::string>{"a.wav"}));
    EXPECT_TRUE(owner.installed.empty());
}

TEST_F(AudioSampleLoaderTest, AWaitingActionRunsOnceTheDecodesFinish)
{
    owner.imported = {"a.wav"};
    AudioSampleLoader loader(owner.hooks(), recordingDecoder(), &tasks);

    int ranNow = 0;
    loader.whenReady({}, QStringLiteral("Auto-Sync"), [&] { ++ranNow; });
    EXPECT_EQ(ranNow, 1);   // nothing to wait for

    gate.hold("a.wav");
    int ran = 0;
    int repeat = 0;
    std::vector<std::string> installedWhenRun;
    loader.whenReady({"a.wav"}, QStringLiteral("Auto-Sync"), [&] {
        ++ran;
        for (const auto& [path, value] : owner.installed) installedWhenRun.push_back(path);
    });
    loader.whenReady({"a.wav"}, QStringLiteral("Auto-Sync"), [&] { ++repeat; });   // double click
    EXPECT_EQ(ran, 0);
    gate.release("a.wav");
    ASSERT_TRUE(QTest::qWaitFor([&] { return ran > 0; }, 10000));
    EXPECT_EQ(ran, 1);
    EXPECT_EQ(repeat, 0);
    EXPECT_EQ(installedWhenRun, (std::vector<std::string>{"a.wav"}));
}

TEST_F(AudioSampleLoaderTest, CancelDropsTheQueueAndTheWaitingActions)
{
    owner.imported = {"a.wav", "b.wav"};
    gate.hold("a.wav");
    AudioSampleLoader loader(owner.hooks(), recordingDecoder(), &tasks);
    int ran = 0;
    loader.whenReady({"a.wav", "b.wav"}, QStringLiteral("Close Gaps"), [&] { ++ran; });
    ASSERT_TRUE(gate.waitEntered("a.wav"));

    loader.cancel();
    EXPECT_FALSE(loader.pending());
    ASSERT_TRUE(QTest::qWaitFor([&] { return running.load() == 0; }, 10000));   // stop reached it
    QTest::qWait(50);
    EXPECT_EQ(ran, 0);
    EXPECT_EQ(decodedOrder(), (std::vector<std::string>{"a.wav"}));
    EXPECT_FALSE(loader.failed("a.wav"));   // cut short, not failed

    // Asking again starts over.
    gate.release("a.wav");
    loader.request({"a.wav", "b.wav"});
    ASSERT_TRUE(QTest::qWaitFor([&] { return !loader.pending(); }, 10000));
    EXPECT_EQ(owner.installed.size(), 2u);
}

TEST_F(AudioSampleLoaderTest, DestroyingTheLoaderMidDecodeIsSafe)
{
    owner.imported = {"a.wav", "b.wav"};
    gate.hold("a.wav");
    auto loader = std::make_unique<AudioSampleLoader>(owner.hooks(), recordingDecoder(), &tasks);
    loader->request({"a.wav", "b.wav"});
    ASSERT_TRUE(gate.waitEntered("a.wav"));

    {
        rt::test::StallGuard guard("AudioSampleLoader destroyed mid-decode");
        loader.reset();
    }
    ASSERT_TRUE(QTest::qWaitFor([&] { return running.load() == 0; }, 10000));
    QTest::qWait(50);
    EXPECT_TRUE(owner.installed.empty());
    EXPECT_EQ(decodedOrder(), (std::vector<std::string>{"a.wav"}));
}
