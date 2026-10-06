/*
 * SampleBuffer.cpp — heap or memory-mapped read-only samples.
 */

#include "audio/SampleBuffer.h"

#include <algorithm>
#include <atomic>
#include <limits>
#include <mutex>

#ifdef _WIN32
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #include <windows.h>
#else
  #include <fcntl.h>
  #include <sys/mman.h>
  #include <sys/stat.h>
  #include <unistd.h>
#endif

namespace rt {

struct SampleBuffer::Storage
{
    std::vector<float> heap;
    void*  view{nullptr};       ///< mapped view (whole file), or null for heap
    size_t viewBytes{0};

    mutable std::once_flag     peaksOnce;
    mutable std::vector<Peak>  peaks;
    mutable std::atomic<bool>  peaksReady{false};

    Storage() = default;
    Storage(const Storage&) = delete;
    Storage& operator=(const Storage&) = delete;

    ~Storage()
    {
        if (!view) return;
#ifdef _WIN32
        UnmapViewOfFile(view);
#else
        munmap(view, viewBytes);
#endif
    }
};

SampleBuffer::SampleBuffer(std::vector<float> samples)
{
    if (samples.empty()) return;
    m_storage = std::make_shared<Storage>();
    m_storage->heap = std::move(samples);
    m_data = m_storage->heap.data();
    m_size = m_storage->heap.size();
}

SampleBuffer SampleBuffer::mapFile(const std::filesystem::path& path,
                                   uint64_t byteOffset, uint64_t count)
{
    SampleBuffer buffer;
    if (count == 0 || byteOffset % sizeof(float) != 0) return buffer;
    const uint64_t needed = byteOffset + count * sizeof(float);

    void* view = nullptr;
    size_t viewBytes = 0;
#ifdef _WIN32
    // Share delete: the cache file can still be deleted or replaced while
    // mapped (the mapping keeps the old contents until it is released).
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return buffer;
    LARGE_INTEGER fileSize{};
    if (!GetFileSizeEx(file, &fileSize) ||
        static_cast<uint64_t>(fileSize.QuadPart) < needed ||
        static_cast<uint64_t>(fileSize.QuadPart) > std::numeric_limits<size_t>::max()) {
        CloseHandle(file);
        return buffer;
    }
    HANDLE mapping = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
    CloseHandle(file);   // the mapping keeps the file open
    if (!mapping) return buffer;
    view = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
    CloseHandle(mapping);   // the view keeps the mapping alive
    if (!view) return buffer;
    viewBytes = static_cast<size_t>(fileSize.QuadPart);
#else
    const int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) return buffer;
    struct stat st{};
    if (fstat(fd, &st) != 0 || static_cast<uint64_t>(st.st_size) < needed) {
        ::close(fd);
        return buffer;
    }
    viewBytes = static_cast<size_t>(st.st_size);
    view = mmap(nullptr, viewBytes, PROT_READ, MAP_SHARED, fd, 0);
    ::close(fd);   // the mapping keeps the file open
    if (view == MAP_FAILED) return buffer;
#endif

    buffer.m_storage = std::make_shared<Storage>();
    buffer.m_storage->view = view;
    buffer.m_storage->viewBytes = viewBytes;
    buffer.m_data = reinterpret_cast<const float*>(static_cast<const char*>(view) + byteOffset);
    buffer.m_size = static_cast<size_t>(count);
    return buffer;
}

bool SampleBuffer::isMapped() const noexcept
{
    return m_storage && m_storage->view;
}

void SampleBuffer::reset() noexcept
{
    m_storage.reset();
    m_data = nullptr;
    m_size = 0;
}

const std::vector<SampleBuffer::Peak>& SampleBuffer::blockPeaks() const
{
    static const std::vector<Peak> kNone;
    if (!m_storage) return kNone;
    const Storage& storage = *m_storage;
    std::call_once(storage.peaksOnce, [&] {
        storage.peaks.resize((m_size + kPeakBlock - 1) / kPeakBlock);
        for (size_t b = 0; b < storage.peaks.size(); ++b) {
            const float* first = m_data + b * kPeakBlock;
            const float* last  = m_data + std::min(m_size, (b + 1) * kPeakBlock);
            const auto [mn, mx] = std::minmax_element(first, last);
            storage.peaks[b] = {*mn, *mx};
        }
        storage.peaksReady.store(true, std::memory_order_release);
    });
    return storage.peaks;
}

bool SampleBuffer::hasBlockPeaks() const noexcept
{
    return m_storage && m_storage->peaksReady.load(std::memory_order_acquire);
}

std::pair<float, float> SampleBuffer::minMax(size_t first, size_t last) const
{
    last = std::min(last, m_size);
    if (first >= last) return {0.0f, 0.0f};

    float mn = std::numeric_limits<float>::infinity();
    float mx = -std::numeric_limits<float>::infinity();
    auto scan = [&](size_t from, size_t to) {
        for (size_t i = from; i < to; ++i) {
            mn = std::min(mn, m_data[i]);
            mx = std::max(mx, m_data[i]);
        }
    };

    if (last - first < 2 * kPeakBlock || !hasBlockPeaks()) {
        scan(first, last);
        return {mn, mx};
    }

    const size_t headEnd   = (first + kPeakBlock - 1) / kPeakBlock * kPeakBlock;
    const size_t tailStart = last / kPeakBlock * kPeakBlock;
    scan(first, headEnd);
    const auto& peaks = m_storage->peaks;
    for (size_t b = headEnd / kPeakBlock; b < tailStart / kPeakBlock; ++b) {
        mn = std::min(mn, peaks[b].min);
        mx = std::max(mx, peaks[b].max);
    }
    scan(tailStart, last);
    return {mn, mx};
}

} // namespace rt
