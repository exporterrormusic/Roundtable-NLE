/*
 * test_record_io.cpp — the field-tagged record encoding (RecordIO.h) and the
 * schema tables on top of it (Schema.h): values survive bit-exactly, readers
 * skip what they don't know, missing fields keep defaults, a field of the
 * wrong kind is ignored, and malformed input fails cleanly.
 */

#include <gtest/gtest.h>

#include "project/schema/RecordIO.h"
#include "project/schema/Schema.h"

#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <set>
#include <string>
#include <utility>
#include <vector>

using namespace rt::record;

namespace {

struct Point
{
    float x{0.0f};
    float y{0.0f};
    bool operator==(const Point&) const = default;
};

enum class Shade : uint8_t { Light = 0, Dark = 7 };

struct Sample
{
    bool flag{false};
    int32_t offset{0};
    uint64_t big{0};
    double ratio{0.0};
    std::string label;
    Shade shade{Shade::Light};
    Point origin;
    std::vector<Point> path;
    std::vector<float> weights;
    std::array<float, 3> rgb{1.0f, 1.0f, 1.0f};
    std::vector<uint8_t> blob;
};

} // namespace

template <> struct rt::record::SchemaOf<Point>
{
    static const Schema<Point>& get()
    {
        static const Schema<Point> s("Point", {
            field<Point>(1, "x", [](const Point& p) { return p.x; },
                         [](Point& p, float v) { p.x = v; }),
            field<Point>(2, "y", [](const Point& p) { return p.y; },
                         [](Point& p, float v) { p.y = v; }),
        });
        return s;
    }
};

namespace {

const Schema<Sample>& sampleSchema()
{
    static const Schema<Sample> s("Sample", {
        field<Sample>(1, "flag", [](const Sample& o) { return o.flag; },
                      [](Sample& o, bool v) { o.flag = v; }),
        field<Sample>(2, "offset", [](const Sample& o) { return o.offset; },
                      [](Sample& o, int32_t v) { o.offset = v; }),
        field<Sample>(3, "big", [](const Sample& o) { return o.big; },
                      [](Sample& o, uint64_t v) { o.big = v; }),
        field<Sample>(4, "ratio", [](const Sample& o) { return o.ratio; },
                      [](Sample& o, double v) { o.ratio = v; }),
        field<Sample>(5, "label", [](const Sample& o) { return o.label; },
                      [](Sample& o, std::string v) { o.label = std::move(v); }),
        field<Sample>(6, "shade", [](const Sample& o) { return o.shade; },
                      [](Sample& o, Shade v) { o.shade = v; }),
        member<Sample>(7, "origin", [](Sample& o) -> Point& { return o.origin; }),
        member<Sample>(8, "path", [](Sample& o) -> std::vector<Point>& { return o.path; }),
        member<Sample>(9, "weights", [](Sample& o) -> std::vector<float>& { return o.weights; }),
        member<Sample>(10, "rgb", [](Sample& o) -> std::array<float, 3>& { return o.rgb; }),
        member<Sample>(11, "blob", [](Sample& o) -> std::vector<uint8_t>& { return o.blob; }),
    });
    return s;
}

Sample filledSample()
{
    Sample s;
    s.flag = true;
    s.offset = -123456;
    s.big = std::numeric_limits<uint64_t>::max();
    s.ratio = 29.97002997002997;
    s.label = std::string("na\0me \xE2\x80\x94 ok", 12);
    s.shade = Shade::Dark;
    s.origin = {1.5f, -2.25f};
    s.path = {{0.1f, 0.2f}, {0.3f, 0.4f}};
    s.weights = {-0.0f, 1.0f, 3.5e-30f};
    s.rgb = {0.25f, 0.5f, 0.75f};
    s.blob = {0x00, 0xFF, 0x80};
    return s;
}

std::vector<uint8_t> encode(const Sample& s)
{
    RecordWriter w;
    sampleSchema().write(w, s);
    return w.take();
}

bool decode(const std::vector<uint8_t>& bytes, Sample& out)
{
    RecordReader r(bytes);
    return sampleSchema().read(r, out);
}

} // namespace

TEST(RecordIO, EveryValueTypeRoundTripsExactly)
{
    const Sample in = filledSample();
    Sample out;
    ASSERT_TRUE(decode(encode(in), out));
    EXPECT_EQ(out.flag, in.flag);
    EXPECT_EQ(out.offset, in.offset);
    EXPECT_EQ(out.big, in.big);
    EXPECT_EQ(out.ratio, in.ratio);
    EXPECT_EQ(out.label, in.label);
    EXPECT_EQ(out.shade, in.shade);
    EXPECT_EQ(out.origin, in.origin);
    EXPECT_EQ(out.path, in.path);
    ASSERT_EQ(out.weights.size(), 3u);
    EXPECT_TRUE(std::signbit(out.weights[0]));   // -0.0 stays negative
    EXPECT_EQ(out.weights[2], in.weights[2]);
    EXPECT_EQ(out.rgb, in.rgb);
    EXPECT_EQ(out.blob, in.blob);
}

TEST(RecordIO, SignedAndUnsignedExtremesRoundTrip)
{
    for (int64_t v : {std::numeric_limits<int64_t>::min(), int64_t{-1}, int64_t{0},
                      int64_t{1}, std::numeric_limits<int64_t>::max()}) {
        RecordWriter w;
        w.writeSigned(3, v);
        RecordReader r(w.data());
        ASSERT_TRUE(r.next());
        int64_t back = 0;
        ASSERT_TRUE(r.readSigned(back));
        EXPECT_EQ(back, v);
    }
}

TEST(RecordIO, FloatBitsSurviveIncludingNaNPayloads)
{
    uint32_t bits = 0x7FC01234u;
    float nan = 0.0f;
    std::memcpy(&nan, &bits, sizeof nan);
    RecordWriter w;
    w.writeF32(1, nan);
    RecordReader r(w.data());
    ASSERT_TRUE(r.next());
    float back = 0.0f;
    ASSERT_TRUE(r.readF32(back));
    uint32_t backBits = 0;
    std::memcpy(&backBits, &back, sizeof backBits);
    EXPECT_EQ(backBits, bits);
}

TEST(RecordIO, UnknownFieldsAreSkipped)
{
    // A newer build wrote fields 50 (varint), 51 (string) and 52 (record)
    // around the known ones.
    RecordWriter w;
    w.writeVarint(50, 999);
    w.writeBool(1, true);
    w.writeString(51, "from the future");
    RecordWriter nested;
    nested.writeF64(1, 1.0);
    w.writeRecord(52, nested);
    w.writeString(5, "kept");
    Sample out;
    ASSERT_TRUE(decode(w.data(), out));
    EXPECT_TRUE(out.flag);
    EXPECT_EQ(out.label, "kept");
}

TEST(RecordIO, MissingFieldsKeepTheirDefaults)
{
    RecordWriter w;
    w.writeSigned(2, 42);
    Sample out;
    ASSERT_TRUE(decode(w.data(), out));
    EXPECT_EQ(out.offset, 42);
    EXPECT_EQ(out.rgb, (std::array<float, 3>{1.0f, 1.0f, 1.0f}));
    EXPECT_FALSE(out.flag);
}

TEST(RecordIO, AFieldOfTheWrongKindIsIgnored)
{
    RecordWriter w;
    w.writeString(2, "not a number");   // offset is a varint
    w.writeF32(4, 2.0f);                 // ratio is a double
    w.writeBool(1, true);
    Sample out;
    out.offset = 7;
    ASSERT_TRUE(decode(w.data(), out));
    EXPECT_EQ(out.offset, 7);
    EXPECT_EQ(out.ratio, 0.0);
    EXPECT_TRUE(out.flag);
}

TEST(RecordIO, AnOutOfRangeIntegerIsIgnored)
{
    RecordWriter w;
    w.writeSigned(2, int64_t{1} << 40);   // too big for int32
    Sample out;
    out.offset = 5;
    ASSERT_TRUE(decode(w.data(), out));
    EXPECT_EQ(out.offset, 5);
}

TEST(RecordIO, AnEmptyListClearsWhatTheObjectStartedWith)
{
    Sample in = filledSample();
    in.path.clear();
    Sample out;
    out.path = {{9.0f, 9.0f}};   // e.g. a constructor's default contents
    ASSERT_TRUE(decode(encode(in), out));
    EXPECT_TRUE(out.path.empty());
}

TEST(RecordIO, TruncationAnywhereFailsCleanly)
{
    const std::vector<uint8_t> bytes = encode(filledSample());
    for (size_t n = 1; n < bytes.size(); ++n) {
        std::vector<uint8_t> cut(bytes.begin(), bytes.begin() + static_cast<ptrdiff_t>(n));
        Sample out;
        RecordReader r(cut);
        // Either a clean prefix of whole fields or a failure; never a crash.
        (void)sampleSchema().read(r, out);
    }
    std::vector<uint8_t> cut(bytes.begin(), bytes.end() - 1);
    Sample out;
    EXPECT_FALSE(decode(cut, out));
}

TEST(RecordIO, MalformedNestedRecordFailsTheOuterRead)
{
    RecordWriter w;
    const uint8_t broken[] = {0x0D, 0x00};   // field 1 fixed32 with 1 byte
    w.writeBytes(7, broken, sizeof broken);
    Sample out;
    EXPECT_FALSE(decode(w.data(), out));
}

TEST(RecordIO, NestingDeeperThanTheLimitFails)
{
    std::vector<uint8_t> bytes;
    for (int depth = 0; depth < kMaxDepth + 2; ++depth) {
        RecordWriter w;
        w.writeBytes(7, bytes.data(), bytes.size());
        bytes = w.take();
    }
    // Step into each nested record as far as the data goes.
    int levels = 0;
    RecordReader cur(bytes);
    bool failed = false;
    while (cur.next()) {
        RecordReader child;
        if (!cur.readRecord(child)) { failed = true; break; }
        cur = child;
        ++levels;
    }
    EXPECT_TRUE(failed);
    EXPECT_LE(levels, kMaxDepth);
}

TEST(RecordIO, ZeroOrUnknownKindTagsFail)
{
    const uint8_t zeroId[] = {0x00, 0x01};          // field id 0
    RecordReader a(zeroId, sizeof zeroId);
    EXPECT_FALSE(a.next());
    EXPECT_FALSE(a.ok());
    const uint8_t badKind[] = {0x0B, 0x01};         // kind 3 (undefined)
    RecordReader b(badKind, sizeof badKind);
    EXPECT_FALSE(b.next());
    EXPECT_FALSE(b.ok());
}

TEST(RecordIO, SchemaRejectsDuplicateAndRetiredIds)
{
    auto get = [](const Point& p) { return p.x; };
    auto set = [](Point& p, float v) { p.x = v; };
    EXPECT_THROW((Schema<Point>("Dup", {field<Point>(1, "a", get, set),
                                        field<Point>(1, "b", get, set)})),
                 std::logic_error);
    EXPECT_THROW((Schema<Point>("Retired", {field<Point>(3, "a", get, set)}, {3})),
                 std::logic_error);
    EXPECT_NO_THROW((Schema<Point>("Fine", {field<Point>(2, "a", get, set)}, {1})));
}

TEST(RecordIO, TheFieldLogSeesEveryFieldWritten)
{
    struct Log final : FieldLog
    {
        std::set<std::pair<std::string, uint32_t>> seen;
        void wrote(const char* schema, uint32_t id) override { seen.insert({schema, id}); }
    } log;
    RecordWriter w(&log);
    sampleSchema().write(w, filledSample());
    for (const auto& f : sampleSchema().fields())
        EXPECT_TRUE(log.seen.count({"Sample", f.id})) << f.name;
    EXPECT_TRUE(log.seen.count({"Point", 1}));   // nested records log too
    EXPECT_TRUE(log.seen.count({"Point", 2}));
}

TEST(RecordIO, WritingIsDeterministic)
{
    EXPECT_EQ(encode(filledSample()), encode(filledSample()));
}
