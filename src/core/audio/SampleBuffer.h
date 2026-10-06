/*
 * SampleBuffer — read-only float samples that copies share.
 *
 * The samples live either on the heap or in a read-only memory mapping of a
 * file (such as a decoded-audio cache). A mapping costs no committed memory:
 * the OS pages samples in when they are read and can drop them again under
 * memory pressure, so hours of audio can stay "loaded" cheaply.
 *
 * Copies share the storage, which lives until the last copy goes, so a
 * widget holding a copy can never be left pointing at freed samples.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <utility>
#include <vector>

namespace rt {

class SampleBuffer
{
public:
    SampleBuffer() = default;
    explicit SampleBuffer(std::vector<float> samples);

    /// Map `count` floats starting `byteOffset` bytes into `path` (a multiple
    /// of 4). Returns an empty buffer when the file cannot be mapped or is
    /// shorter than that. The file must not be modified while mapped.
    [[nodiscard]] static SampleBuffer mapFile(const std::filesystem::path& path,
                                              uint64_t byteOffset, uint64_t count);

    [[nodiscard]] const float* data() const noexcept { return m_data; }
    [[nodiscard]] size_t size() const noexcept { return m_size; }
    [[nodiscard]] bool empty() const noexcept { return m_size == 0; }
    [[nodiscard]] const float& operator[](size_t i) const noexcept { return m_data[i]; }
    [[nodiscard]] const float* begin() const noexcept { return m_data; }
    [[nodiscard]] const float* end() const noexcept { return m_data + m_size; }

    [[nodiscard]] bool isMapped() const noexcept;

    void reset() noexcept;

    /// Samples per block in blockPeaks().
    static constexpr size_t kPeakBlock = 256;

    struct Peak { float min; float max; };

    /// Min and max of each kPeakBlock-sample block (the last may be short),
    /// built on first call and shared by copies. Building reads every
    /// sample once, so call it off the UI thread (e.g. after decoding).
    const std::vector<Peak>& blockPeaks() const;

    /// True once blockPeaks() has been built.
    [[nodiscard]] bool hasBlockPeaks() const noexcept;

    /// Exact min and max of samples [first, last); {0, 0} when empty. Uses
    /// the block peaks for whole blocks once they are built, so a wide range
    /// reads few samples; it never builds them itself.
    [[nodiscard]] std::pair<float, float> minMax(size_t first, size_t last) const;

private:
    struct Storage;

    std::shared_ptr<Storage> m_storage;
    const float* m_data{nullptr};
    size_t       m_size{0};
};

} // namespace rt
