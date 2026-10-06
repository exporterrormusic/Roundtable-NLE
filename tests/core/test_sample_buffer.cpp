/*
 * test_sample_buffer.cpp — SampleBuffer (heap / memory-mapped samples) and
 * AudioFile::readAllStreaming.
 */

#include <gtest/gtest.h>

#include "audio/AudioFile.h"
#include "audio/SampleBuffer.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

using namespace rt;

namespace {

namespace fs = std::filesystem;

class TempDir
{
public:
    TempDir()
    {
        const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
        m_path = fs::temp_directory_path() /
                 (std::string("rt_samplebuffer_") + info->test_suite_name() + "_" + info->name());
        fs::remove_all(m_path);
        fs::create_directories(m_path);
    }
    ~TempDir()
    {
        std::error_code ec;
        fs::remove_all(m_path, ec);
    }
    [[nodiscard]] const fs::path& path() const noexcept { return m_path; }

private:
    fs::path m_path;
};

std::vector<float> noise(size_t count, uint32_t seed)
{
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> dist(-1.0f, 1.0f);
    std::vector<float> v(count);
    for (float& x : v) x = dist(rng);
    return v;
}

/// `headerBytes` of filler, then the samples.
fs::path writeSamplesFile(const fs::path& path, const std::vector<float>& samples,
                          size_t headerBytes)
{
    std::ofstream out(path, std::ios::binary);
    const std::string header(headerBytes, 'H');
    out.write(header.data(), static_cast<std::streamsize>(header.size()));
    out.write(reinterpret_cast<const char*>(samples.data()),
              static_cast<std::streamsize>(samples.size() * sizeof(float)));
    return path;
}

} // namespace

TEST(SampleBuffer, HeapCopiesShareTheSamples)
{
    const auto values = noise(1000, 1);
    SampleBuffer a(values);
    SampleBuffer b = a;
    EXPECT_FALSE(a.isMapped());
    ASSERT_EQ(a.size(), values.size());
    EXPECT_EQ(a.data(), b.data());
    EXPECT_TRUE(std::equal(a.begin(), a.end(), values.begin()));

    a.reset();
    EXPECT_TRUE(a.empty());
    ASSERT_EQ(b.size(), values.size());   // b keeps the storage alive
    EXPECT_EQ(b[999], values[999]);

    EXPECT_TRUE(SampleBuffer(std::vector<float>{}).empty());
}

TEST(SampleBuffer, MapsSamplesAfterAHeader)
{
    TempDir dir;
    const auto values = noise(5000, 2);
    const auto path = writeSamplesFile(dir.path() / "samples.pcm", values, 40);

    SampleBuffer mapped = SampleBuffer::mapFile(path, 40, values.size());
    ASSERT_EQ(mapped.size(), values.size());
    EXPECT_TRUE(mapped.isMapped());
    EXPECT_TRUE(std::equal(mapped.begin(), mapped.end(), values.begin()));

    // Misaligned offset, too many samples, missing file: nothing mapped.
    EXPECT_TRUE(SampleBuffer::mapFile(path, 42, 10).empty());
    EXPECT_TRUE(SampleBuffer::mapFile(path, 40, values.size() + 1).empty());
    EXPECT_TRUE(SampleBuffer::mapFile(dir.path() / "missing.pcm", 0, 1).empty());
    EXPECT_TRUE(SampleBuffer::mapFile(path, 40, 0).empty());
}

TEST(SampleBuffer, MappedFileCanBeDeletedWhileMapped)
{
    // The cache folder may be cleaned while a session has files mapped:
    // the delete must not fail, and the mapped samples must stay readable.
    TempDir dir;
    const auto values = noise(4096, 3);
    const auto path = writeSamplesFile(dir.path() / "doomed.pcm", values, 0);

    SampleBuffer mapped = SampleBuffer::mapFile(path, 0, values.size());
    ASSERT_FALSE(mapped.empty());
    std::error_code ec;
    EXPECT_TRUE(fs::remove(path, ec)) << ec.message();
    EXPECT_TRUE(std::equal(mapped.begin(), mapped.end(), values.begin()));
}

TEST(SampleBuffer, MinMaxIsExactWithAndWithoutPeaks)
{
    const auto values = noise(100003, 4);   // not a whole number of blocks
    SampleBuffer buffer(values);
    std::mt19937 rng(5);
    std::uniform_int_distribution<size_t> pos(0, values.size());

    auto check = [&](const char* phase) {
        for (int i = 0; i < 400; ++i) {
            size_t first = pos(rng), last = pos(rng);
            if (first > last) std::swap(first, last);
            const auto [lo, hi] = buffer.minMax(first, last);
            if (first == last) {
                EXPECT_EQ(lo, 0.0f);
                EXPECT_EQ(hi, 0.0f);
                continue;
            }
            const auto [mn, mx] = std::minmax_element(values.begin() + static_cast<std::ptrdiff_t>(first),
                                                      values.begin() + static_cast<std::ptrdiff_t>(last));
            ASSERT_EQ(lo, *mn) << phase << " [" << first << ", " << last << ")";
            ASSERT_EQ(hi, *mx) << phase << " [" << first << ", " << last << ")";
        }
    };

    EXPECT_FALSE(buffer.hasBlockPeaks());
    check("scan");
    EXPECT_FALSE(buffer.hasBlockPeaks());   // minMax never builds them
    EXPECT_EQ(buffer.blockPeaks().size(),
              (values.size() + SampleBuffer::kPeakBlock - 1) / SampleBuffer::kPeakBlock);
    EXPECT_TRUE(buffer.hasBlockPeaks());
    EXPECT_TRUE(SampleBuffer(buffer).hasBlockPeaks());   // shared by copies
    check("peaks");

    // Past the end is clamped.
    const auto [lo, hi] = buffer.minMax(values.size() - 10, values.size() + 1000);
    const auto [mn, mx] = std::minmax_element(values.end() - 10, values.end());
    EXPECT_EQ(lo, *mn);
    EXPECT_EQ(hi, *mx);
}

TEST(AudioFileStreaming, ReadAllStreamingMatchesReadAll)
{
    TempDir dir;
    // 16-bit stereo WAV.
    const uint32_t rate = 22050;
    const uint32_t frames = rate * 3 + 17;
    const auto path = dir.path() / "tone.wav";
    {
        std::ofstream f(path, std::ios::binary);
        auto w32 = [&](uint32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); };
        auto w16 = [&](uint16_t v) { f.write(reinterpret_cast<const char*>(&v), 2); };
        f.write("RIFF", 4); w32(36 + frames * 4); f.write("WAVE", 4);
        f.write("fmt ", 4); w32(16); w16(1); w16(2); w32(rate); w32(rate * 4); w16(4); w16(16);
        f.write("data", 4); w32(frames * 4);
        for (uint32_t i = 0; i < frames; ++i) {
            const auto v = static_cast<int16_t>(12000 * std::sin(2.0 * 3.14159265358979 * 300.0 * i / rate));
            w16(static_cast<uint16_t>(v));
            w16(static_cast<uint16_t>(-v));
        }
    }

    AudioFile file;
    ASSERT_TRUE(file.open(path));
    const auto all = file.readAll();
    ASSERT_EQ(all.size(), static_cast<size_t>(frames) * 2);

    std::vector<float> streamed;
    int calls = 0;
    const int64_t total = file.readAllStreaming([&](const float* samples, int64_t n) {
        ++calls;
        streamed.insert(streamed.end(), samples, samples + n * 2);
        return true;
    });
    EXPECT_EQ(total, static_cast<int64_t>(frames));
    EXPECT_GT(calls, 1);   // delivered in chunks, not one buffer
    EXPECT_EQ(streamed, all);

    // A sink that stops early ends the read.
    int64_t seen = 0;
    EXPECT_EQ(file.readAllStreaming([&](const float*, int64_t n) { seen += n; return false; }), -1);
    EXPECT_GT(seen, 0);
    EXPECT_LT(seen, static_cast<int64_t>(frames));
}

TEST(AudioFileStreaming, ReadAllResampledMatchesAOneShotResample)
{
    // readAllResampled() now converts while it decodes instead of holding
    // the decoded file and a resampled copy. It must give exactly what the
    // one-shot path gives: readRegionResampled() over the whole file
    // resamples [0, end) in one go.
    TempDir dir;
    const uint32_t rate = 44100;
    const uint32_t frames = rate * 3 + 101;
    const auto path = dir.path() / "tone44.wav";
    {
        std::ofstream f(path, std::ios::binary);
        auto w32 = [&](uint32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); };
        auto w16 = [&](uint16_t v) { f.write(reinterpret_cast<const char*>(&v), 2); };
        f.write("RIFF", 4); w32(36 + frames * 4); f.write("WAVE", 4);
        f.write("fmt ", 4); w32(16); w16(1); w16(2); w32(rate); w32(rate * 4); w16(4); w16(16);
        f.write("data", 4); w32(frames * 4);
        std::mt19937 rng(11);
        std::uniform_int_distribution<int> noise(-800, 800);
        for (uint32_t i = 0; i < frames; ++i) {
            const auto v = static_cast<int16_t>(
                9000 * std::sin(2.0 * 3.14159265358979 * 523.0 * i / rate) + noise(rng));
            w16(static_cast<uint16_t>(v));
            w16(static_cast<uint16_t>(-v));
        }
    }

    for (const uint32_t target : {48000u, 16000u}) {
        AudioFile file;
        ASSERT_TRUE(file.open(path));
        const auto streamed = file.readAllResampled(target);
        ASSERT_FALSE(streamed.empty());

        std::vector<float> oneShot;
        const int64_t wanted = static_cast<int64_t>(
            std::ceil(static_cast<double>(frames) * target / rate)) + 16;
        file.readRegionResampled(0, wanted, target, oneShot);
        EXPECT_EQ(streamed, oneShot) << "target " << target;

        std::vector<float> chunks;
        int calls = 0;
        const int64_t delivered = file.readAllResampledStreaming(
            target, [&](const float* samples, int64_t n) {
                ++calls;
                chunks.insert(chunks.end(), samples, samples + n * 2);
                return true;
            });
        EXPECT_EQ(delivered, static_cast<int64_t>(streamed.size() / 2));
        EXPECT_GT(calls, 1);
        EXPECT_EQ(chunks, streamed);
    }
}
