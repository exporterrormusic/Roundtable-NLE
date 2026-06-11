/*
 * OmniShotDetector — AI-powered scene cut detection via OmniShotCut.
 *
 * Launches a Python subprocess to run OmniShotCut inference.
 * Uses std::thread + std::system for the subprocess bridge
 * (no Qt dependency — safe to compile into roundtable_core).
 *
 * This is the sole scene-cut detector (it superseded the old SceneDetector).
 */

#pragma once

#include <atomic>
#include <filesystem>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace rt {

/// A detected scene cut point.
struct DetectedCut
{
    int64_t frameNumber{0};
    double  timeSeconds{0.0};
    float   score{0.0f};
};

/// Progress callback: (percent 0-1, current frame, total frames).
using SceneDetectProgressFn = std::function<void(float percent, int64_t frame, int64_t total)>;

/// Completion callback: delivers the final list of detected cuts.
using SceneDetectCompleteFn = std::function<void(std::vector<DetectedCut> cuts)>;

/// Error callback.
using SceneDetectErrorFn = std::function<void(std::string message)>;

/// AI scene cut detector using OmniShotCut via Python subprocess.
class OmniShotDetector
{
public:
    OmniShotDetector() = default;
    ~OmniShotDetector();

    OmniShotDetector(const OmniShotDetector&) = delete;
    OmniShotDetector& operator=(const OmniShotDetector&) = delete;

    /// Start asynchronous detection on a video file.
    void detectAsync(const std::filesystem::path& mediaPath,
                     int64_t startFrame, int64_t endFrame,
                     float threshold,
                     SceneDetectProgressFn onProgress,
                     SceneDetectCompleteFn onComplete,
                     SceneDetectErrorFn onError);

    /// Cancel a running detection.
    void cancel();

    /// Is a detection currently running?
    [[nodiscard]] bool isRunning() const noexcept { return m_running.load(); }

    /// Check if OmniShotCut tools are installed and available.
    [[nodiscard]] static bool isAvailable();

private:
    void workerFunc(std::filesystem::path mediaPath,
                    int64_t startFrame, int64_t endFrame,
                    float threshold,
                    SceneDetectProgressFn onProgress,
                    SceneDetectCompleteFn onComplete,
                    SceneDetectErrorFn onError);

    std::thread       m_worker;
    std::atomic<bool> m_cancelled{false};
    std::atomic<bool> m_running{false};
};

} // namespace rt
