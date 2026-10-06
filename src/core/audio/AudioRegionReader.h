/*
 * AudioRegionReader — block-wise AudioFile::readRegionResampled().
 *
 * readRegionResampled() decodes and rate-converts a whole region into one
 * buffer, so its memory grows with the region. This reader delivers the
 * same samples a block at a time: it decodes the same source span
 * sequentially and runs one resampler across all of it, so memory stays
 * proportional to the block size and block boundaries leave no seam.
 */

#pragma once

#include <cstdint>
#include <memory>
#include <vector>

namespace rt {

class AudioFile;

class AudioRegionReader
{
public:
    AudioRegionReader();
    ~AudioRegionReader();

    AudioRegionReader(const AudioRegionReader&) = delete;
    AudioRegionReader& operator=(const AudioRegionReader&) = delete;

    /// Start reading frames [startFrame, startFrame + numFrames) of `file`
    /// in the targetSampleRate domain (the arguments readRegionResampled
    /// takes). `file` must stay open and unused by anyone else until the
    /// reader is done. Returns false when there is nothing to read.
    bool begin(AudioFile& file, int64_t startFrame, int64_t numFrames,
               uint32_t targetSampleRate);

    /// The next up-to-`maxFrames` frames, interleaved, into `out` (resized).
    /// Returns the frame count; 0 once the region is exhausted.
    int64_t read(int64_t maxFrames, std::vector<float>& out);

    /// Channel count of the delivered frames (the file's).
    [[nodiscard]] uint16_t channels() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace rt
