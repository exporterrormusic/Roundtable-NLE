/*
 * RecordIO.h — the field-tagged record encoding of project files (v48+).
 *
 * A record is a sequence of fields. Each field is a tag followed by its
 * payload; the tag is a varint of (fieldId << 3 | kind):
 *
 *   kind 0  varint    unsigned LEB128 (bools, unsigned ints, enums;
 *                     signed ints zig-zag encoded)
 *   kind 1  fixed64   8 bytes little-endian (double)
 *   kind 2  bytes     varint length + that many bytes (strings, blobs,
 *                     nested records, lists, packed floats)
 *   kind 5  fixed32   4 bytes little-endian (float)
 *
 * (the same numbering as protobuf). A reader can step over any field
 * without knowing it, so fields can be added without breaking older
 * readers, and a missing field simply keeps its default. Which field means
 * what is defined by the schema tables (Schema.h), not here.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace rt::record {

enum class Kind : uint8_t
{
    Varint  = 0,
    Fixed64 = 1,
    Bytes   = 2,
    Fixed32 = 5,
};

/// Largest field id a tag can carry.
inline constexpr uint32_t kMaxFieldId = (1u << 29) - 1;

/// Records nest (a clip holds tracks holds keyframes); deeper than this is
/// treated as corrupt rather than risking the stack.
inline constexpr int kMaxDepth = 64;

/// Optional observer of the fields a write emits (schema name, field id):
/// the coverage test uses it to prove every schema field is exercised.
class FieldLog
{
public:
    virtual ~FieldLog() = default;
    virtual void wrote(const char* schema, uint32_t fieldId) = 0;
};

class RecordWriter
{
public:
    RecordWriter() = default;
    explicit RecordWriter(FieldLog* log) : m_log(log) {}

    void writeVarint(uint32_t id, uint64_t value);
    void writeSigned(uint32_t id, int64_t value);   ///< zig-zag varint
    void writeBool(uint32_t id, bool value) { writeVarint(id, value ? 1u : 0u); }
    void writeF32(uint32_t id, float value);
    void writeF64(uint32_t id, double value);
    void writeBytes(uint32_t id, const uint8_t* data, size_t size);
    void writeString(uint32_t id, std::string_view text);
    void writeFloats(uint32_t id, const float* values, size_t count);   ///< packed
    void writeRecord(uint32_t id, const RecordWriter& nested);

    /// A writer for a nested record, sharing this writer's field log.
    [[nodiscard]] RecordWriter nested() const { return RecordWriter(m_log); }

    [[nodiscard]] FieldLog* log() const noexcept { return m_log; }
    [[nodiscard]] const std::vector<uint8_t>& data() const noexcept { return m_data; }
    [[nodiscard]] std::vector<uint8_t> take() noexcept { return std::move(m_data); }
    [[nodiscard]] size_t size() const noexcept { return m_data.size(); }

private:
    void putTag(uint32_t id, Kind kind);
    void putVarint(uint64_t value);

    std::vector<uint8_t> m_data;
    FieldLog* m_log{nullptr};
};

/// Reads one record. next() steps from field to field; the typed accessors
/// read the current field and return false when its kind doesn't fit (the
/// caller then treats the field as unknown). Truncated or malformed input
/// makes next() return false with ok() false, and stays failed.
class RecordReader
{
public:
    RecordReader() = default;
    RecordReader(const uint8_t* data, size_t size, int depth = 0)
        : m_pos(data), m_end(data + size), m_depth(depth)
    {
        if (depth > kMaxDepth) m_ok = false;
    }
    explicit RecordReader(const std::vector<uint8_t>& data)
        : RecordReader(data.data(), data.size()) {}

    /// Move to the next field (skipping whatever of the current one was not
    /// read). False at the end of the record or on malformed input.
    bool next();

    [[nodiscard]] uint32_t id() const noexcept { return m_id; }
    [[nodiscard]] Kind kind() const noexcept { return m_kind; }
    [[nodiscard]] bool ok() const noexcept { return m_ok; }
    /// Marks the record failed (a field's content was invalid).
    void fail() noexcept { m_ok = false; }

    bool readVarint(uint64_t& out) const;
    bool readSigned(int64_t& out) const;
    bool readBool(bool& out) const;
    bool readF32(float& out) const;
    bool readF64(double& out) const;
    bool readString(std::string& out) const;
    bool readBytes(std::vector<uint8_t>& out) const;
    bool readFloats(std::vector<float>& out) const;
    /// The current field as a nested record.
    bool readRecord(RecordReader& out) const;

private:
    const uint8_t* m_pos{nullptr};
    const uint8_t* m_end{nullptr};
    // The current field: its payload (bytes kind: the bytes; varint/fixed:
    // the raw encoded value).
    const uint8_t* m_value{nullptr};
    size_t         m_valueSize{0};
    uint64_t       m_varint{0};
    uint32_t       m_id{0};
    Kind           m_kind{Kind::Varint};
    int            m_depth{0};
    bool           m_ok{true};
};

} // namespace rt::record
