/*
 * FrameSignatureLog — flag-gated CSV logger for the A/B render-path harness (#18).
 *
 * Enable by setting the env var ROUNDTABLE_FRAMEHASH (to 1, or to a .csv path).
 * When enabled, each composite path calls capture() with its OUTPUT texture; a
 * GPU FrameSignature is computed (32-byte readback) and one CSV row is written:
 *
 *     tag,frame,width,height,s0,s1,s2,s3,s4,s5,s6,s7
 *
 * Run preview and export over the same project, then diff rows with matching
 * (frame) across tag="preview" vs tag="export". Identical signatures => the two
 * paths produced bit-identical frames (the #18 goal). When the env var is unset
 * this is a no-op (callers gate on enabled() to avoid all overhead).
 */

#pragma once

#include <volk.h>

#include <cstdint>

namespace rt {

class FrameSignatureLog
{
public:
    static FrameSignatureLog& get();

    [[nodiscard]] bool enabled() const noexcept { return m_enabled; }

    /// Compute the GPU signature of `view` and append a CSV row. `tag` labels
    /// the path ("preview"/"export"); `frame` is the frame index or tick.
    /// Thread-safe (serialized internally). No-op if disabled or init fails.
    /// The texture's composite writes must already be complete.
    void capture(const char* tag, int64_t frame,
                 VkImageView view, VkSampler sampler,
                 uint32_t width, uint32_t height,
                 VkImageLayout layout = VK_IMAGE_LAYOUT_GENERAL);

private:
    FrameSignatureLog();
    ~FrameSignatureLog();
    FrameSignatureLog(const FrameSignatureLog&) = delete;
    FrameSignatureLog& operator=(const FrameSignatureLog&) = delete;

    struct Impl;
    Impl* m_impl{nullptr};
    bool  m_enabled{false};
};

} // namespace rt
