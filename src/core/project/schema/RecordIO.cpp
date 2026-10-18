/*
 * RecordIO.cpp — see RecordIO.h.
 */

#include "project/schema/RecordIO.h"

#include <cstring>

namespace rt::record {

namespace {

uint64_t zigzag(int64_t v) noexcept
{
    return (static_cast<uint64_t>(v) << 1) ^ static_cast<uint64_t>(v >> 63);
}

int64_t unzigzag(uint64_t v) noexcept
{
    return static_cast<int64_t>(v >> 1) ^ -static_cast<int64_t>(v & 1u);
}

/// Decode a varint at `pos`; false when it runs past `end` or over 10 bytes.
bool getVarint(const uint8_t*& pos, const uint8_t* end, uint64_t& out) noexcept
{
    out = 0;
    for (int shift = 0; shift < 64; shift += 7) {
        if (pos >= end) return false;
        const uint8_t byte = *pos++;
        out |= static_cast<uint64_t>(byte & 0x7Fu) << shift;
        if ((byte & 0x80u) == 0) return true;
    }
    return false;
}

} // namespace

// ── Writer ──────────────────────────────────────────────────────────────

void RecordWriter::putVarint(uint64_t value)
{
    while (value >= 0x80u) {
        m_data.push_back(static_cast<uint8_t>(value | 0x80u));
        value >>= 7;
    }
    m_data.push_back(static_cast<uint8_t>(value));
}

void RecordWriter::putTag(uint32_t id, Kind kind)
{
    putVarint((static_cast<uint64_t>(id) << 3) | static_cast<uint64_t>(kind));
}

void RecordWriter::writeVarint(uint32_t id, uint64_t value)
{
    putTag(id, Kind::Varint);
    putVarint(value);
}

void RecordWriter::writeSigned(uint32_t id, int64_t value)
{
    writeVarint(id, zigzag(value));
}

void RecordWriter::writeF32(uint32_t id, float value)
{
    putTag(id, Kind::Fixed32);
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof bits);
    for (int i = 0; i < 4; ++i) m_data.push_back(static_cast<uint8_t>(bits >> (8 * i)));
}

void RecordWriter::writeF64(uint32_t id, double value)
{
    putTag(id, Kind::Fixed64);
    uint64_t bits = 0;
    std::memcpy(&bits, &value, sizeof bits);
    for (int i = 0; i < 8; ++i) m_data.push_back(static_cast<uint8_t>(bits >> (8 * i)));
}

void RecordWriter::writeBytes(uint32_t id, const uint8_t* data, size_t size)
{
    putTag(id, Kind::Bytes);
    putVarint(size);
    if (size) m_data.insert(m_data.end(), data, data + size);
}

void RecordWriter::writeString(uint32_t id, std::string_view text)
{
    writeBytes(id, reinterpret_cast<const uint8_t*>(text.data()), text.size());
}

void RecordWriter::writeFloats(uint32_t id, const float* values, size_t count)
{
    putTag(id, Kind::Bytes);
    putVarint(count * 4);
    for (size_t i = 0; i < count; ++i) {
        uint32_t bits = 0;
        std::memcpy(&bits, &values[i], sizeof bits);
        for (int b = 0; b < 4; ++b) m_data.push_back(static_cast<uint8_t>(bits >> (8 * b)));
    }
}

void RecordWriter::writeRecord(uint32_t id, const RecordWriter& nested)
{
    writeBytes(id, nested.m_data.data(), nested.m_data.size());
}

// ── Reader ──────────────────────────────────────────────────────────────

bool RecordReader::next()
{
    if (!m_ok || m_pos >= m_end) return false;
    uint64_t tag = 0;
    if (!getVarint(m_pos, m_end, tag)) { m_ok = false; return false; }
    const uint64_t id = tag >> 3;
    const auto kind = static_cast<Kind>(tag & 7u);
    if (id == 0 || id > kMaxFieldId) { m_ok = false; return false; }
    m_id = static_cast<uint32_t>(id);
    m_kind = kind;
    switch (kind) {
    case Kind::Varint:
        if (!getVarint(m_pos, m_end, m_varint)) { m_ok = false; return false; }
        return true;
    case Kind::Fixed64:
    case Kind::Fixed32: {
        const size_t n = kind == Kind::Fixed64 ? 8u : 4u;
        if (static_cast<size_t>(m_end - m_pos) < n) { m_ok = false; return false; }
        m_value = m_pos;
        m_valueSize = n;
        m_pos += n;
        return true;
    }
    case Kind::Bytes: {
        uint64_t size = 0;
        if (!getVarint(m_pos, m_end, size)
            || size > static_cast<uint64_t>(m_end - m_pos)) {
            m_ok = false;
            return false;
        }
        m_value = m_pos;
        m_valueSize = static_cast<size_t>(size);
        m_pos += size;
        return true;
    }
    }
    m_ok = false;   // a kind this format doesn't define: can't step over it
    return false;
}

bool RecordReader::readVarint(uint64_t& out) const
{
    if (m_kind != Kind::Varint) return false;
    out = m_varint;
    return true;
}

bool RecordReader::readSigned(int64_t& out) const
{
    if (m_kind != Kind::Varint) return false;
    out = unzigzag(m_varint);
    return true;
}

bool RecordReader::readBool(bool& out) const
{
    if (m_kind != Kind::Varint) return false;
    out = m_varint != 0;
    return true;
}

bool RecordReader::readF32(float& out) const
{
    if (m_kind != Kind::Fixed32) return false;
    uint32_t bits = 0;
    for (int i = 0; i < 4; ++i) bits |= static_cast<uint32_t>(m_value[i]) << (8 * i);
    std::memcpy(&out, &bits, sizeof out);
    return true;
}

bool RecordReader::readF64(double& out) const
{
    if (m_kind != Kind::Fixed64) return false;
    uint64_t bits = 0;
    for (int i = 0; i < 8; ++i) bits |= static_cast<uint64_t>(m_value[i]) << (8 * i);
    std::memcpy(&out, &bits, sizeof out);
    return true;
}

bool RecordReader::readString(std::string& out) const
{
    if (m_kind != Kind::Bytes) return false;
    out.assign(reinterpret_cast<const char*>(m_value), m_valueSize);
    return true;
}

bool RecordReader::readBytes(std::vector<uint8_t>& out) const
{
    if (m_kind != Kind::Bytes) return false;
    out.assign(m_value, m_value + m_valueSize);
    return true;
}

bool RecordReader::readFloats(std::vector<float>& out) const
{
    if (m_kind != Kind::Bytes || m_valueSize % 4 != 0) return false;
    out.resize(m_valueSize / 4);
    for (size_t i = 0; i < out.size(); ++i) {
        uint32_t bits = 0;
        for (int b = 0; b < 4; ++b)
            bits |= static_cast<uint32_t>(m_value[i * 4 + b]) << (8 * b);
        std::memcpy(&out[i], &bits, sizeof bits);
    }
    return true;
}

bool RecordReader::readRecord(RecordReader& out) const
{
    if (m_kind != Kind::Bytes) return false;
    out = RecordReader(m_value, m_valueSize, m_depth + 1);
    return out.ok();
}

} // namespace rt::record
