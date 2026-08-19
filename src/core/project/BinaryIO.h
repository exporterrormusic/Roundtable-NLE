/*
 * BinaryIO.h — Streaming binary read/write helpers.
 *
 * Little-endian, tag-length-value section format used by ProjectSerializer
 * and ClipSerialization. Extracted so that project-level code can use the
 * binary I/O primitives without depending on timeline/clip headers.
 */

#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "PathUtils.h"

namespace rt {

// ── Streaming binary write helper ──────────────────────────────────────────

class BinaryWriter
{
public:
    void writeU8(uint8_t v) { m_data.push_back(v); }

    void writeU32(uint32_t v)
    {
        m_data.push_back(static_cast<uint8_t>(v));
        m_data.push_back(static_cast<uint8_t>(v >> 8));
        m_data.push_back(static_cast<uint8_t>(v >> 16));
        m_data.push_back(static_cast<uint8_t>(v >> 24));
    }

    void writeU64(uint64_t v)
    {
        for (int i = 0; i < 8; ++i)
            m_data.push_back(static_cast<uint8_t>(v >> (i * 8)));
    }

    void writeI64(int64_t v) { writeU64(static_cast<uint64_t>(v)); }

    void writeF32(float v)
    {
        uint32_t bits;
        std::memcpy(&bits, &v, 4);
        writeU32(bits);
    }

    void writeF64(double v)
    {
        uint64_t bits;
        std::memcpy(&bits, &v, 8);
        writeU64(bits);
    }

    void writeString(const std::string& s)
    {
        writeU32(static_cast<uint32_t>(s.size()));
        m_data.insert(m_data.end(), s.begin(), s.end());
    }

    void writePath(const std::filesystem::path& p) { writeString(pathToUtf8(p)); }

    void writeBytes(const uint8_t* data, size_t size)
    {
        m_data.insert(m_data.end(), data, data + size);
    }

    [[nodiscard]] const std::vector<uint8_t>& data() const { return m_data; }
    [[nodiscard]] size_t size() const { return m_data.size(); }

    void beginSection(uint32_t tag, const std::vector<uint8_t>& sectionData)
    {
        writeU32(tag);
        writeU32(static_cast<uint32_t>(sectionData.size()));
        m_data.insert(m_data.end(), sectionData.begin(), sectionData.end());
    }

private:
    std::vector<uint8_t> m_data;
};

// ── Streaming binary read helper ───────────────────────────────────────────

class BinaryReader
{
public:
    // Project records use 32-bit lengths, but no legitimate individual text
    // field or collection should be able to consume unbounded memory.  These
    // limits are deliberately generous and do not change the on-disk format.
    static constexpr uint32_t kMaxStringBytes = 16u * 1024u * 1024u;
    static constexpr uint32_t kMaxCollectionItems = 1'000'000u;

    explicit BinaryReader(const uint8_t* data, size_t size)
        : m_data(data), m_size(size), m_pos(0) {}

    [[nodiscard]] bool hasRemaining(size_t n) const
    {
        return m_pos <= m_size && n <= (m_size - m_pos);
    }
    [[nodiscard]] bool ok() const { return m_ok; }
    [[nodiscard]] size_t position() const { return m_pos; }
    [[nodiscard]] size_t remaining() const
    {
        return m_pos <= m_size ? m_size - m_pos : 0;
    }
    [[nodiscard]] size_t errorPosition() const { return m_errorPos; }

    // Mark semantically invalid data (for example an unknown non-delimited
    // clip type) with the same persistent failure state as a short read.
    void invalidate()
    {
        if (m_ok) {
            m_ok = false;
            m_errorPos = m_pos;
        }
    }

    uint8_t readU8()
    {
        if (!requireRemaining(1)) return 0;
        return m_data[m_pos++];
    }

    uint32_t readU32()
    {
        if (!requireRemaining(4)) return 0;
        uint32_t v = static_cast<uint32_t>(m_data[m_pos])
                   | (static_cast<uint32_t>(m_data[m_pos + 1]) << 8)
                   | (static_cast<uint32_t>(m_data[m_pos + 2]) << 16)
                   | (static_cast<uint32_t>(m_data[m_pos + 3]) << 24);
        m_pos += 4;
        return v;
    }

    uint64_t readU64()
    {
        if (!requireRemaining(8)) return 0;
        uint64_t v = 0;
        for (int i = 0; i < 8; ++i)
            v |= static_cast<uint64_t>(m_data[m_pos + i]) << (i * 8);
        m_pos += 8;
        return v;
    }

    int64_t readI64() { return static_cast<int64_t>(readU64()); }

    float readF32()
    {
        uint32_t bits = readU32();
        float v;
        std::memcpy(&v, &bits, 4);
        return v;
    }

    double readF64()
    {
        uint64_t bits = readU64();
        double v;
        std::memcpy(&v, &bits, 8);
        return v;
    }

    std::string readString()
    {
        uint32_t len = readU32();
        if (!m_ok) return {};
        if (len > kMaxStringBytes || !hasRemaining(len)) {
            invalidate();
            return {};
        }
        if (len == 0) return {};
        std::string s(reinterpret_cast<const char*>(m_data + m_pos), len);
        m_pos += len;
        return s;
    }

    // Reads a file-controlled collection count and validates it before callers
    // reserve memory or enter a loop. minBytesPerItem is the smallest encoded
    // size of one item and prevents compact malicious buffers from requesting
    // large allocations even when the absolute cap is not exceeded.
    uint32_t readCount(uint32_t maxCount = kMaxCollectionItems,
                       size_t minBytesPerItem = 1)
    {
        const uint32_t count = readU32();
        if (!m_ok) return 0;
        if (count > maxCount ||
            (minBytesPerItem > 0 && count > remaining() / minBytesPerItem)) {
            invalidate();
            return 0;
        }
        return count;
    }

    // Returns a bounded reader for a length-delimited payload and advances the
    // parent. A short payload marks both the parent and returned reader failed.
    BinaryReader readSubReader(size_t n)
    {
        if (!requireRemaining(n)) {
            BinaryReader failed(nullptr, 0);
            failed.invalidate();
            return failed;
        }
        const uint8_t* childData = n == 0 ? nullptr : m_data + m_pos;
        BinaryReader child(childData, n);
        m_pos += n;
        return child;
    }

    // The stored string is UTF-8 (writePath uses pathToUtf8).  The raw
    // fs::path(std::string) ctor would decode it via the ANSI codepage and
    // mojibake non-ANSI characters (U+FF5C etc.) on pre-1903 Windows and in
    // test exes without the UTF-8 ACP manifest — always go through utf8ToPath.
    std::filesystem::path readPath() { return utf8ToPath(readString()); }

    void skip(size_t n)
    {
        if (!requireRemaining(n)) return;
        m_pos += n;
    }

private:
    bool requireRemaining(size_t n)
    {
        if (!m_ok) return false;
        if (!hasRemaining(n)) {
            invalidate();
            return false;
        }
        return true;
    }

    const uint8_t* m_data;
    size_t         m_size;
    size_t         m_pos;
    bool           m_ok{true};
    size_t         m_errorPos{0};
};

} // namespace rt
