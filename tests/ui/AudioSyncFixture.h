/*
 * AudioSyncFixture.h — shared setup for tests that drive the AudioSync panel:
 * a QApplication with throw-away QSettings, WAV writers and an event-loop
 * wait.
 */

#pragma once

#include <gtest/gtest.h>

#include "AudioMixdown.h"

#include <QApplication>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QSettings>
#include <QStandardPaths>
#include <QString>
#include <QTemporaryDir>

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

class AudioSyncFixture : public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        QStandardPaths::setTestModeEnabled(true);
        if (!QApplication::instance())
            app = std::make_unique<QApplication>(argc, argv);
        settingsDirectory = std::make_unique<QTemporaryDir>();
        ASSERT_TRUE(settingsDirectory->isValid());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                           settingsDirectory->path());
    }

    static void TearDownTestSuite() { settingsDirectory.reset(); }

    static QString writeWav(const QString& directory, const QString& name,
                            std::vector<float> samples, uint32_t sampleRate)
    {
        rt::MixdownResult result;
        result.sampleRate = sampleRate;
        result.channels = 1;
        result.totalFrames = static_cast<int64_t>(samples.size());
        result.duration = static_cast<double>(samples.size()) / sampleRate;
        result.samples = std::move(samples);
        const QString path = directory + QStringLiteral("/") + name;
        EXPECT_TRUE(rt::AudioMixdown::writeWav(
            result, std::filesystem::path(path.toStdWString())));
        return path;
    }

    /// A steady 220 Hz tone.
    static QString makeTone(const QString& directory, const QString& name, double seconds)
    {
        constexpr uint32_t sampleRate = 44100;
        std::vector<float> samples(static_cast<size_t>(seconds * sampleRate));
        for (size_t i = 0; i < samples.size(); ++i)
            samples[i] = 0.15f * static_cast<float>(
                std::sin(2.0 * 3.141592653589793 * 220.0 * double(i) / sampleRate));
        return writeWav(directory, name, std::move(samples), sampleRate);
    }

    /// Speech-like bursts (a voiced tone with a syllable-rate envelope) at the
    /// given [start, end) seconds over a faint, deterministic noise floor.
    static QString makeSpeech(const QString& directory, const QString& name,
                              double seconds,
                              const std::vector<std::pair<double, double>>& bursts)
    {
        constexpr uint32_t sampleRate = 44100;
        constexpr double pi = 3.141592653589793;
        std::vector<float> samples(static_cast<size_t>(seconds * sampleRate));
        uint32_t noise = 12345u;
        for (size_t i = 0; i < samples.size(); ++i) {
            noise = noise * 1664525u + 1013904223u;
            const double t = double(i) / sampleRate;
            double v = 0.002 * (double(noise >> 8) / double(1u << 24) - 0.5);
            for (const auto& [start, end] : bursts) {
                if (t < start || t >= end) continue;
                const double edge = std::min({1.0, (t - start) / 0.03, (end - t) / 0.03});
                const double syllable = 0.6 + 0.4 * std::sin(2.0 * pi * 4.0 * (t - start));
                v += 0.3 * edge * syllable *
                     (std::sin(2.0 * pi * 180.0 * t) + 0.5 * std::sin(2.0 * pi * 360.0 * t));
            }
            samples[i] = static_cast<float>(v);
        }
        return writeWav(directory, name, std::move(samples), sampleRate);
    }

    /// Spin the event loop until `done` or the deadline.
    static bool waitFor(const std::function<bool()>& done, int timeoutMs = 20000)
    {
        QElapsedTimer timer;
        timer.start();
        while (!done()) {
            if (timer.elapsed() > timeoutMs) return false;
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        }
        return true;
    }

    static inline int argc = 1;
    static inline char arg0[] = "audio_sync_test";
    static inline char* argv[] = {arg0, nullptr};
    static inline std::unique_ptr<QApplication> app;
    static inline std::unique_ptr<QTemporaryDir> settingsDirectory;
};
