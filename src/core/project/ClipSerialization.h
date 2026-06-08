/*
 * ClipSerialization.h — Internal helpers shared between ProjectSerializer.cpp
 *                       and ClipSerialization.cpp.
 *
 * NOT a public header — only #included by the serializer implementation files.
 */

#pragma once

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "PathUtils.h"

#include "timeline/Clip.h"
#include "timeline/KeyframeTrack.h"

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
    explicit BinaryReader(const uint8_t* data, size_t size)
        : m_data(data), m_size(size), m_pos(0) {}

    [[nodiscard]] bool hasRemaining(size_t n) const { return m_pos + n <= m_size; }
    [[nodiscard]] size_t position() const { return m_pos; }
    [[nodiscard]] size_t remaining() const { return m_size - m_pos; }

    uint8_t readU8()
    {
        if (!hasRemaining(1)) return 0;
        return m_data[m_pos++];
    }

    uint32_t readU32()
    {
        if (!hasRemaining(4)) return 0;
        uint32_t v = static_cast<uint32_t>(m_data[m_pos])
                   | (static_cast<uint32_t>(m_data[m_pos + 1]) << 8)
                   | (static_cast<uint32_t>(m_data[m_pos + 2]) << 16)
                   | (static_cast<uint32_t>(m_data[m_pos + 3]) << 24);
        m_pos += 4;
        return v;
    }

    uint64_t readU64()
    {
        if (!hasRemaining(8)) return 0;
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
        if (!hasRemaining(len)) return "";
        std::string s(reinterpret_cast<const char*>(m_data + m_pos), len);
        m_pos += len;
        return s;
    }

    std::filesystem::path readPath() { return std::filesystem::path(readString()); }

    void skip(size_t n) { m_pos = std::min(m_pos + n, m_size); }

private:
    const uint8_t* m_data;
    size_t         m_size;
    size_t         m_pos;
};

// ── Clip-level serialization (implemented in ClipSerialization.cpp) ────────

void writeKeyframeTrack(BinaryWriter& w, const KeyframeTrack<float>& track);
void readKeyframeTrack(BinaryReader& r, KeyframeTrack<float>& track, uint32_t version = 1);

void writeClip(BinaryWriter& w, const Clip& clip);
std::unique_ptr<Clip> readClip(BinaryReader& r, uint32_t version = 1);

} // namespace rt
