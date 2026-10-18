/*
 * Schema.h — what each field of a saved record means (project format v48+).
 *
 * Every saved type has ONE table of fields. Each field has a stable id, a
 * name, and how to get the value from the object and put it back:
 *
 *     const record::Schema<Marker>& markerSchema()
 *     {
 *         static const record::Schema<Marker> s("Marker", {
 *             record::field<Marker>(1, "time",
 *                 [](const Marker& m) { return m.time; },
 *                 [](Marker& m, int64_t v) { m.time = v; }),
 *             ...
 *         });
 *         return s;
 *     }
 *
 * The writer emits every field; the reader applies the fields it knows, in
 * whatever order they come, skips the ones it doesn't, and leaves a missing
 * field at the object's default. So:
 *
 *   - To save something new, add ONE field with the next unused id. Old
 *     files load with the default; no version bump, no reader branch.
 *   - Never reuse or renumber an id. To drop a field, delete it and add its
 *     id to the schema's retired list so it can't come back meaning
 *     something else.
 *   - Changing what a value MEANS (units, sign) is not a new field: keep
 *     the id, bump ProjectSerializer::FORMAT_VERSION and convert old files
 *     after loading, as the v45 vignette fix does.
 *
 * Value types map to the record encoding (RecordIO.h) by these rules:
 *   bool, integers, enums   varint (signed: zig-zag)
 *   float / double          fixed32 / fixed64 (bit-exact)
 *   std::string, path       bytes (paths as UTF-8)
 *   vector<uint8_t>         bytes          vector<float>, array<float,N>: packed
 *   a type with a schema    nested record  (specialise SchemaOf<V>)
 *   vector<V>, array<V,N>   list record: each element is field 1
 */
#pragma once

#include "project/schema/RecordIO.h"
#include "PathUtils.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace rt::record {

template <class T> class Schema;

/// Types saved as nested records specialise this with
/// `static const Schema<V>& get();`.
template <class V> struct SchemaOf;

template <class V>
concept HasSchema = requires { { SchemaOf<V>::get() } -> std::same_as<const Schema<V>&>; };

template <class V> struct IsVector : std::false_type {};
template <class E, class A> struct IsVector<std::vector<E, A>> : std::true_type {};
template <class V> struct IsArray : std::false_type {};
template <class E, size_t N> struct IsArray<std::array<E, N>> : std::true_type {};
template <class V> struct IsFloatArray : std::false_type {};
template <size_t N> struct IsFloatArray<std::array<float, N>> : std::true_type {};

// ── Value codecs ────────────────────────────────────────────────────────
// readValue returns false when the field's kind doesn't fit V (the caller
// treats the field as unknown and keeps the default); a malformed nested
// record fails the reader instead.

template <class V> void writeValue(RecordWriter& w, uint32_t id, const V& value);
template <class V> bool readValue(RecordReader& r, V& value);

namespace detail {

/// Element field id inside a list record.
inline constexpr uint32_t kListElement = 1;

template <class V>
void writeList(RecordWriter& w, uint32_t id, const V& list)
{
    RecordWriter n = w.nested();
    for (const auto& element : list) writeValue(n, kListElement, element);
    w.writeRecord(id, n);
}

template <class V>
bool readList(RecordReader& r, V& list)
{
    RecordReader child;
    if (r.kind() != Kind::Bytes) return false;
    if (!r.readRecord(child)) { r.fail(); return true; }
    using E = typename V::value_type;
    if constexpr (IsVector<V>::value) list.clear();
    size_t index = 0;
    while (child.next()) {
        if (child.id() != kListElement) continue;
        E element{};
        if constexpr (IsArray<V>::value) {
            if (index < list.size()) element = list[index];
        }
        if (!readValue(child, element)) continue;
        if constexpr (IsVector<V>::value) {
            list.push_back(std::move(element));
        } else if (index < list.size()) {
            list[index] = std::move(element);
        }
        ++index;
    }
    if (!child.ok()) r.fail();
    return true;
}

} // namespace detail

template <class V>
void writeValue(RecordWriter& w, uint32_t id, const V& value)
{
    if constexpr (std::is_same_v<V, bool>) {
        w.writeBool(id, value);
    } else if constexpr (std::is_enum_v<V>) {
        writeValue(w, id, static_cast<std::underlying_type_t<V>>(value));
    } else if constexpr (std::is_integral_v<V> && std::is_signed_v<V>) {
        w.writeSigned(id, static_cast<int64_t>(value));
    } else if constexpr (std::is_integral_v<V>) {
        w.writeVarint(id, static_cast<uint64_t>(value));
    } else if constexpr (std::is_same_v<V, float>) {
        w.writeF32(id, value);
    } else if constexpr (std::is_same_v<V, double>) {
        w.writeF64(id, value);
    } else if constexpr (std::is_same_v<V, std::string>) {
        w.writeString(id, value);
    } else if constexpr (std::is_same_v<V, std::filesystem::path>) {
        w.writeString(id, pathToUtf8(value));
    } else if constexpr (std::is_same_v<V, std::vector<uint8_t>>) {
        w.writeBytes(id, value.data(), value.size());
    } else if constexpr (std::is_same_v<V, std::vector<float>>) {
        w.writeFloats(id, value.data(), value.size());
    } else if constexpr (IsFloatArray<V>::value) {
        w.writeFloats(id, value.data(), value.size());
    } else if constexpr (HasSchema<V>) {
        RecordWriter n = w.nested();
        SchemaOf<V>::get().write(n, value);
        w.writeRecord(id, n);
    } else if constexpr (IsVector<V>::value || IsArray<V>::value) {
        detail::writeList(w, id, value);
    } else {
        static_assert(sizeof(V) == 0, "no record encoding for this type");
    }
}

template <class V>
bool readValue(RecordReader& r, V& value)
{
    if constexpr (std::is_same_v<V, bool>) {
        return r.readBool(value);
    } else if constexpr (std::is_enum_v<V>) {
        std::underlying_type_t<V> raw{};
        if (!readValue(r, raw)) return false;
        value = static_cast<V>(raw);
        return true;
    } else if constexpr (std::is_integral_v<V> && std::is_signed_v<V>) {
        int64_t raw = 0;
        if (!r.readSigned(raw)) return false;
        if (raw < static_cast<int64_t>(std::numeric_limits<V>::min())
            || raw > static_cast<int64_t>(std::numeric_limits<V>::max()))
            return false;
        value = static_cast<V>(raw);
        return true;
    } else if constexpr (std::is_integral_v<V>) {
        uint64_t raw = 0;
        if (!r.readVarint(raw)) return false;
        if (raw > static_cast<uint64_t>(std::numeric_limits<V>::max())) return false;
        value = static_cast<V>(raw);
        return true;
    } else if constexpr (std::is_same_v<V, float>) {
        return r.readF32(value);
    } else if constexpr (std::is_same_v<V, double>) {
        return r.readF64(value);
    } else if constexpr (std::is_same_v<V, std::string>) {
        return r.readString(value);
    } else if constexpr (std::is_same_v<V, std::filesystem::path>) {
        std::string utf8;
        if (!r.readString(utf8)) return false;
        value = utf8ToPath(utf8);
        return true;
    } else if constexpr (std::is_same_v<V, std::vector<uint8_t>>) {
        return r.readBytes(value);
    } else if constexpr (std::is_same_v<V, std::vector<float>>) {
        return r.readFloats(value);
    } else if constexpr (IsFloatArray<V>::value) {
        std::vector<float> floats;
        if (!r.readFloats(floats)) return false;
        for (size_t i = 0; i < floats.size() && i < value.size(); ++i) value[i] = floats[i];
        return true;
    } else if constexpr (HasSchema<V>) {
        if (r.kind() != Kind::Bytes) return false;
        RecordReader child;
        if (!r.readRecord(child) || !SchemaOf<V>::get().read(child, value)) r.fail();
        return true;
    } else if constexpr (IsVector<V>::value || IsArray<V>::value) {
        return detail::readList(r, value);
    } else {
        static_assert(sizeof(V) == 0, "no record encoding for this type");
    }
}

// ── Fields and schemas ──────────────────────────────────────────────────

template <class T>
struct FieldDef
{
    uint32_t    id;
    const char* name;
    void (*write)(RecordWriter& w, uint32_t id, const T& object);
    /// False: the field's kind doesn't fit (skipped as unknown).
    bool (*read)(RecordReader& r, T& object);
};

/// A value got through a getter and put back through a setter. The value
/// type is the getter's return type (decayed); the setter takes it by value.
template <class T, class Get, class Set>
FieldDef<T> field(uint32_t id, const char* name, Get, Set)
{
    using V = std::decay_t<decltype(std::declval<Get>()(std::declval<const T&>()))>;
    return {id, name,
            [](RecordWriter& w, uint32_t fid, const T& o) { writeValue(w, fid, Get{}(o)); },
            [](RecordReader& r, T& o) -> bool {
                V v{};
                if (!readValue(r, v)) return false;
                Set{}(o, std::move(v));
                return true;
            }};
}

/// A member reached by reference, read in place (keyframe tracks, lists
/// that have no setter). `Ref` takes T& and returns a reference; writing
/// only reads through it.
template <class T, class Ref>
FieldDef<T> member(uint32_t id, const char* name, Ref)
{
    return {id, name,
            [](RecordWriter& w, uint32_t fid, const T& o) {
                writeValue(w, fid, Ref{}(const_cast<T&>(o)));
            },
            [](RecordReader& r, T& o) -> bool { return readValue(r, Ref{}(o)); }};
}

/// Hand-written encoding for a field the rules above can't express.
template <class T>
FieldDef<T> custom(uint32_t id, const char* name,
                   void (*write)(RecordWriter&, uint32_t, const T&),
                   bool (*read)(RecordReader&, T&))
{
    return {id, name, write, read};
}

template <class T>
class Schema
{
public:
    Schema(const char* name, std::vector<FieldDef<T>> fields,
           std::vector<uint32_t> retired = {})
        : m_name(name), m_fields(std::move(fields)), m_retired(std::move(retired))
    {
        for (size_t i = 0; i < m_fields.size(); ++i) {
            const uint32_t id = m_fields[i].id;
            if (id == 0 || id > kMaxFieldId)
                throw std::logic_error(std::string(name) + ": field id out of range");
            for (size_t j = 0; j < i; ++j)
                if (m_fields[j].id == id)
                    throw std::logic_error(std::string(name) + ": field id "
                                           + std::to_string(id) + " used twice");
            for (uint32_t gone : m_retired)
                if (gone == id)
                    throw std::logic_error(std::string(name) + ": field id "
                                           + std::to_string(id) + " is retired");
        }
    }

    void write(RecordWriter& w, const T& object) const
    {
        for (const auto& f : m_fields) {
            f.write(w, f.id, object);
            if (FieldLog* log = w.log()) log->wrote(m_name, f.id);
        }
    }

    /// Apply the record's fields to `object`. False on malformed input.
    bool read(RecordReader& r, T& object) const
    {
        while (r.next()) {
            const FieldDef<T>* f = find(r.id());
            if (!f) continue;              // unknown (newer build): skipped
            (void)f->read(r, object);      // wrong kind: skipped, default kept
        }
        return r.ok();
    }

    [[nodiscard]] const char* name() const noexcept { return m_name; }
    [[nodiscard]] const std::vector<FieldDef<T>>& fields() const noexcept { return m_fields; }

private:
    [[nodiscard]] const FieldDef<T>* find(uint32_t id) const noexcept
    {
        for (const auto& f : m_fields)
            if (f.id == id) return &f;
        return nullptr;
    }

    const char* m_name;
    std::vector<FieldDef<T>> m_fields;
    std::vector<uint32_t> m_retired;
};

} // namespace rt::record
