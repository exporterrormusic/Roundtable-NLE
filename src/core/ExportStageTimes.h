/*
 * ExportStageTimes.h - cumulative wall time of each export stage, for
 * profiling (RenderPolicyTest.OptionalExportProfile). Each stage adds the
 * microseconds it took; reset() starts a new measurement. Cheap enough to
 * leave on: one steady_clock read pair and a relaxed add per stage.
 */

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>

namespace rt {

struct ExportStageTimes
{
    /// Inline exact decode of an export source frame (MediaPool forceExact).
    std::atomic<uint64_t> exactDecodeUs{0};
    /// Waiting for the export composite's GPU work before the readback.
    std::atomic<uint64_t> readbackWaitUs{0};
    /// Copying the composite out of staging memory.
    std::atomic<uint64_t> readbackCopyUs{0};
    /// Encoder: BGRA → YUV conversion on the CPU.
    std::atomic<uint64_t> encodeConvertUs{0};
    /// Encoder: uploading the YUV frame to a hardware encoder.
    std::atomic<uint64_t> encodeUploadUs{0};
    /// Encoder: sending the frame and receiving packets.
    std::atomic<uint64_t> encodeSendUs{0};

    void reset() noexcept
    {
        for (auto* c : {&exactDecodeUs, &readbackWaitUs, &readbackCopyUs,
                        &encodeConvertUs, &encodeUploadUs, &encodeSendUs})
            c->store(0, std::memory_order_relaxed);
    }
};

[[nodiscard]] inline ExportStageTimes& exportStageTimes() noexcept
{
    static ExportStageTimes times;
    return times;
}

/// Adds the time from construction to destruction to one stage.
class ExportStageTimer
{
public:
    explicit ExportStageTimer(std::atomic<uint64_t>& stage) noexcept
        : m_stage(stage), m_start(std::chrono::steady_clock::now()) {}
    ~ExportStageTimer()
    {
        const auto us = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - m_start).count();
        m_stage.fetch_add(static_cast<uint64_t>(us), std::memory_order_relaxed);
    }
    ExportStageTimer(const ExportStageTimer&) = delete;
    ExportStageTimer& operator=(const ExportStageTimer&) = delete;

private:
    std::atomic<uint64_t>& m_stage;
    std::chrono::steady_clock::time_point m_start;
};

} // namespace rt
