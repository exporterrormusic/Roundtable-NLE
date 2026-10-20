/*
 * test_transcription_runner.cpp — TranscriptionRunner with a fake transcribe
 * function: files run in order, each is reported once on the owner's
 * thread, a failing file does not stop the rest, cancel stops after the
 * current file, and destroying the runner mid-run is safe.
 */

#include <gtest/gtest.h>

#include "panels/audio/TranscriptionRunner.h"
#include "../support/StallGuard.h"

#include <QCoreApplication>
#include <QTest>
#include <QThread>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

using rt::TranscriptionResult;
using rt::TranscriptionRunner;
using rt::TranscriptionStatus;

namespace {

void ensureApp()
{
    static int argc = 1;
    static char name[] = "test_transcription_runner";
    static char* argv[] = {name, nullptr};
    if (!QCoreApplication::instance()) new QCoreApplication(argc, argv);
}

TranscriptionResult success(const std::string& text)
{
    TranscriptionResult result;
    result.status = TranscriptionStatus::Success;
    rt::TranscriptionSegment segment;
    segment.text = text;
    result.segments.push_back(segment);
    return result;
}

TranscriptionResult failure(const std::string& message)
{
    TranscriptionResult result;
    result.status = TranscriptionStatus::Failed;
    result.error.message = message;
    return result;
}

TranscriptionResult cancelled()
{
    TranscriptionResult result;
    result.status = TranscriptionStatus::Cancelled;
    return result;
}

/// Everything a runner reported, in order, as "event:path[:detail]".
struct Recorder
{
    std::vector<std::string> events;
    std::vector<std::string> finishedTexts;
    bool offThread{false};
    int finishedCount{0};
    bool lastCancelled{false};

    void attach(TranscriptionRunner& runner)
    {
        QObject::connect(&runner, &TranscriptionRunner::fileStarted, &runner,
            [this](const std::string& path, size_t number, size_t total) {
                note("started:" + path + ":" + std::to_string(number) + "/" + std::to_string(total));
            });
        QObject::connect(&runner, &TranscriptionRunner::fileProgress, &runner,
            [this](const std::string& path, float percent, const QString& status) {
                note("progress:" + path + ":" + std::to_string(static_cast<int>(percent))
                     + ":" + status.toStdString());
            });
        QObject::connect(&runner, &TranscriptionRunner::fileFinished, &runner,
            [this](const std::string& path, const TranscriptionResult& result) {
                note("finished:" + path);
                finishedTexts.push_back(result.segments.empty() ? "" : result.segments.front().text);
            });
        QObject::connect(&runner, &TranscriptionRunner::fileFailed, &runner,
            [this](const std::string& path, const QString& error) {
                note("failed:" + path + ":" + error.toStdString());
            });
        QObject::connect(&runner, &TranscriptionRunner::finished, &runner,
            [this](bool wasCancelled) {
                note(wasCancelled ? "end:cancelled" : "end");
                ++finishedCount;
                lastCancelled = wasCancelled;
            });
    }

    void note(std::string event)
    {
        if (QThread::currentThread() != QCoreApplication::instance()->thread())
            offThread = true;
        events.push_back(std::move(event));
    }

    [[nodiscard]] bool waitForEnd(int count = 1) const
    {
        return QTest::qWaitFor([&] { return finishedCount >= count; }, 10000);
    }
};

/// Blocks a fake transcribe call until released or stopped.
struct Gate
{
    std::mutex mutex;
    std::condition_variable_any cv;
    bool open{false};
    std::atomic<bool> entered{false};

    /// True when released, false when stopped.
    bool wait(std::stop_token stop)
    {
        entered = true;
        std::unique_lock lock(mutex);
        return cv.wait(lock, stop, [this] { return open; });
    }

    void release()
    {
        {
            std::lock_guard lock(mutex);
            open = true;
        }
        cv.notify_all();
    }
};

class TranscriptionRunnerTest : public ::testing::Test
{
protected:
    void SetUp() override { ensureApp(); }
};

} // namespace

TEST_F(TranscriptionRunnerTest, FilesRunInOrderAndEachIsReportedOnceOnTheOwnerThread)
{
    TranscriptionRunner runner;
    Recorder recorder;
    recorder.attach(runner);

    std::mutex mutex;
    std::vector<std::string> calls;
    ASSERT_TRUE(runner.start({"a.wav", "b.wav", "c.wav"},
        [&](const std::string& path, std::stop_token, const TranscriptionRunner::ProgressFn& progress) {
            {
                std::lock_guard lock(mutex);
                calls.push_back(path);
            }
            progress(50.0f, "half");
            return success("text of " + path);
        }));
    EXPECT_TRUE(runner.isActive());
    EXPECT_FALSE(runner.isCancelling());
    ASSERT_TRUE(recorder.waitForEnd());

    EXPECT_EQ(calls, (std::vector<std::string>{"a.wav", "b.wav", "c.wav"}));
    EXPECT_EQ(recorder.events, (std::vector<std::string>{
        "started:a.wav:1/3", "progress:a.wav:50:half", "finished:a.wav",
        "started:b.wav:2/3", "progress:b.wav:50:half", "finished:b.wav",
        "started:c.wav:3/3", "progress:c.wav:50:half", "finished:c.wav",
        "end"}));
    EXPECT_EQ(recorder.finishedTexts,
              (std::vector<std::string>{"text of a.wav", "text of b.wav", "text of c.wav"}));
    EXPECT_FALSE(recorder.offThread);
    EXPECT_FALSE(runner.isActive());
    EXPECT_EQ(runner.total(), 3u);
    EXPECT_EQ(runner.completed(), 3u);
    EXPECT_TRUE(runner.hadSuccess());
    EXPECT_TRUE(runner.error().isEmpty());
}

TEST_F(TranscriptionRunnerTest, AFailingFileDoesNotStopTheRest)
{
    TranscriptionRunner runner;
    Recorder recorder;
    recorder.attach(runner);

    ASSERT_TRUE(runner.start({"a.wav", "b.wav", "c.wav"},
        [](const std::string& path, std::stop_token, const TranscriptionRunner::ProgressFn&) {
            return path == "b.wav" ? failure("could not decode b") : success(path);
        }));
    ASSERT_TRUE(recorder.waitForEnd());

    EXPECT_EQ(recorder.events, (std::vector<std::string>{
        "started:a.wav:1/3", "finished:a.wav",
        "started:b.wav:2/3", "failed:b.wav:could not decode b",
        "started:c.wav:3/3", "finished:c.wav",
        "end"}));
    EXPECT_EQ(runner.completed(), 3u);
    EXPECT_TRUE(runner.hadSuccess());
    EXPECT_EQ(runner.error().toStdString(), "could not decode b");
}

TEST_F(TranscriptionRunnerTest, ThrownExceptionsAndEmptyErrorsBecomeFailures)
{
    TranscriptionRunner runner;
    Recorder recorder;
    recorder.attach(runner);

    ASSERT_TRUE(runner.start({"a.wav", "b.wav", "c.wav", "d.wav"},
        [](const std::string& path, std::stop_token, const TranscriptionRunner::ProgressFn&)
            -> TranscriptionResult {
            if (path == "a.wav") throw std::runtime_error("model exploded");
            if (path == "b.wav") throw 42;
            if (path == "c.wav") return failure("");
            return cancelled();   // the backend gave up by itself
        }));
    ASSERT_TRUE(recorder.waitForEnd());

    EXPECT_EQ(recorder.events, (std::vector<std::string>{
        "started:a.wav:1/4", "failed:a.wav:Transcription exception: model exploded",
        "started:b.wav:2/4", "failed:b.wav:Unknown transcription exception",
        "started:c.wav:3/4", "failed:c.wav:Transcription failed",
        "started:d.wav:4/4",
        "end"}));
    EXPECT_FALSE(runner.hadSuccess());
    EXPECT_EQ(runner.completed(), 4u);
    EXPECT_EQ(runner.error().toStdString(), "Transcription failed");
}

TEST_F(TranscriptionRunnerTest, ABackendCancelWithoutAnEarlierErrorReadsAsFailed)
{
    TranscriptionRunner runner;
    Recorder recorder;
    recorder.attach(runner);

    ASSERT_TRUE(runner.start({"a.wav"},
        [](const std::string&, std::stop_token, const TranscriptionRunner::ProgressFn&) {
            return cancelled();
        }));
    ASSERT_TRUE(recorder.waitForEnd());

    EXPECT_EQ(recorder.events, (std::vector<std::string>{"started:a.wav:1/1", "end"}));
    EXPECT_FALSE(recorder.lastCancelled);
    EXPECT_EQ(runner.error().toStdString(), "Transcription failed");
}

TEST_F(TranscriptionRunnerTest, CancelStopsAfterTheCurrentFileAndKeepsFinishedOnes)
{
    std::atomic<int> backendCancels{0};
    TranscriptionRunner runner([&] { ++backendCancels; });
    Recorder recorder;
    recorder.attach(runner);

    Gate gate;
    std::atomic<int> calls{0};
    ASSERT_TRUE(runner.start({"a.wav", "b.wav", "c.wav"},
        [&](const std::string& path, std::stop_token stop, const TranscriptionRunner::ProgressFn&) {
            ++calls;
            if (path == "a.wav") return success(path);
            return gate.wait(stop) ? success(path) : cancelled();
        }));
    ASSERT_TRUE(QTest::qWaitFor([&] { return gate.entered.load(); }, 10000));
    ASSERT_TRUE(QTest::qWaitFor([&] {
        return std::find(recorder.events.begin(), recorder.events.end(), "started:b.wav:2/3")
               != recorder.events.end();
    }, 10000));

    runner.cancel();
    EXPECT_TRUE(runner.isCancelling());
    EXPECT_EQ(backendCancels.load(), 1);
    runner.cancel();   // a second click does nothing
    EXPECT_EQ(backendCancels.load(), 1);
    ASSERT_TRUE(recorder.waitForEnd());

    EXPECT_EQ(calls.load(), 2);   // c.wav never started
    EXPECT_EQ(recorder.events, (std::vector<std::string>{
        "started:a.wav:1/3", "finished:a.wav", "started:b.wav:2/3", "end:cancelled"}));
    EXPECT_TRUE(recorder.lastCancelled);
    EXPECT_EQ(runner.completed(), 1u);   // b.wav was cut short, not completed
    EXPECT_TRUE(runner.hadSuccess());
    EXPECT_TRUE(runner.error().isEmpty());
    EXPECT_FALSE(runner.isActive());
}

TEST_F(TranscriptionRunnerTest, AStartWhileRunningOrWithNothingToDoIsRefused)
{
    TranscriptionRunner runner;
    Recorder recorder;
    recorder.attach(runner);
    const auto passThrough = [](const std::string& path, std::stop_token,
                                const TranscriptionRunner::ProgressFn&) { return success(path); };

    EXPECT_FALSE(runner.start({}, passThrough));
    EXPECT_FALSE(runner.start({"a.wav"}, {}));
    EXPECT_FALSE(runner.isActive());

    Gate gate;
    ASSERT_TRUE(runner.start({"a.wav"},
        [&](const std::string& path, std::stop_token stop, const TranscriptionRunner::ProgressFn&) {
            gate.wait(stop);
            return success(path);
        }));
    EXPECT_FALSE(runner.start({"b.wav"}, passThrough));
    gate.release();
    ASSERT_TRUE(recorder.waitForEnd());
    EXPECT_EQ(recorder.events, (std::vector<std::string>{"started:a.wav:1/1", "finished:a.wav", "end"}));

    // Idle again: the next run starts, with fresh figures.
    ASSERT_TRUE(runner.start({"b.wav", "c.wav"}, passThrough));
    ASSERT_TRUE(recorder.waitForEnd(2));
    EXPECT_EQ(runner.total(), 2u);
    EXPECT_EQ(runner.completed(), 2u);
    EXPECT_EQ(runner.runId(), 2u);
}

TEST_F(TranscriptionRunnerTest, DestroyingTheRunnerMidRunStopsTheFileAndReportsNothing)
{
    std::atomic<int> backendCancels{0};
    auto runner = std::make_unique<TranscriptionRunner>([&] { ++backendCancels; });
    Recorder recorder;
    recorder.attach(*runner);

    Gate gate;
    std::atomic<bool> sawStop{false};
    ASSERT_TRUE(runner->start({"a.wav", "b.wav"},
        [&](const std::string& path, std::stop_token stop, const TranscriptionRunner::ProgressFn& progress) {
            progress(10.0f, "working");
            if (!gate.wait(stop)) sawStop = true;
            return success(path);
        }));
    ASSERT_TRUE(QTest::qWaitFor([&] { return gate.entered.load(); }, 10000));

    {
        rt::test::StallGuard guard("TranscriptionRunner destroyed mid-run");
        runner.reset();
        guard.tick();
    }
    EXPECT_TRUE(sawStop.load());
    EXPECT_EQ(backendCancels.load(), 1);

    const size_t before = recorder.events.size();
    QCoreApplication::processEvents();
    QTest::qWait(50);
    EXPECT_EQ(recorder.events.size(), before);
    EXPECT_EQ(recorder.finishedCount, 0);
}

TEST_F(TranscriptionRunnerTest, DestroyingAnIdleRunnerDoesNotCallTheBackend)
{
    std::atomic<int> backendCancels{0};
    {
        TranscriptionRunner runner([&] { ++backendCancels; });
        Recorder recorder;
        recorder.attach(runner);
        ASSERT_TRUE(runner.start({"a.wav"},
            [](const std::string& path, std::stop_token, const TranscriptionRunner::ProgressFn&) {
                return success(path);
            }));
        ASSERT_TRUE(recorder.waitForEnd());
    }
    EXPECT_EQ(backendCancels.load(), 0);
}
