/*
 * test_animation_cache.cpp — Tests for the Spine AnimationCache.
 *
 * Step 27: Spine Animation Cache
 *
 * Tests cover:
 *   - AnimationCacheKey construction, equality, hashing, toString, toHash
 *   - CachedAnimationEntry frame access (index, time-based, memory)
 *   - AnimationCache put/get/contains, LRU eviction, memory budget
 *   - Cache statistics (hits, misses, eviction count)
 *   - Disk persistence (save, load, scan, invalidate)
 *   - Character-based eviction
 *   - Validator callback
 *   - Thread safety (basic concurrent access)
 */

#include <gtest/gtest.h>

#include "spine/AnimationCache.h"
#include "timeline/SpineClip.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace {

// ─── Helpers ─────────────────────────────────────────────────────────────────

/// Create a dummy cached animation entry with given parameters.
std::shared_ptr<rt::CachedAnimationEntry> makeEntry(
    const std::string& character,
    const std::string& animation,
    uint32_t width = 64,
    uint32_t height = 64,
    size_t frameCount = 4,
    float duration = 1.0f,
    float fps = 30.0f,
    bool talking = false,
    const std::string& outfit = "default",
    const std::string& stance = "default")
{
    auto entry = std::make_shared<rt::CachedAnimationEntry>();
    entry->key.character = character;
    entry->key.outfit    = outfit;
    entry->key.stance    = stance;
    entry->key.animation = animation;
    entry->key.talking   = talking;
    entry->key.width     = width;
    entry->key.height    = height;
    entry->key.fps       = fps;
    entry->duration      = duration;
    entry->fps           = fps;

    size_t pixelBytes = static_cast<size_t>(width) * height * 4;

    for (size_t i = 0; i < frameCount; ++i) {
        rt::CachedAnimFrame frame;
        frame.width  = width;
        frame.height = height;
        frame.pixels.resize(pixelBytes);

        // Fill with a recognizable pattern (frame index in first byte)
        std::memset(frame.pixels.data(), static_cast<int>(i & 0xFF), pixelBytes);

        entry->frames.push_back(std::move(frame));
    }

    return entry;
}

/// Create a temporary directory for disk cache tests.
class TempCacheDir
{
public:
    TempCacheDir()
    {
        m_path = fs::temp_directory_path() / ("rt_test_anim_cache_" + std::to_string(
            std::hash<std::thread::id>{}(std::this_thread::get_id())));
        fs::create_directories(m_path);
    }

    ~TempCacheDir()
    {
        std::error_code ec;
        fs::remove_all(m_path, ec);
    }

    [[nodiscard]] const fs::path& path() const { return m_path; }

private:
    fs::path m_path;
};

} // anonymous namespace


// ═════════════════════════════════════════════════════════════════════════════
// AnimationCacheKey tests
// ═════════════════════════════════════════════════════════════════════════════

TEST(AnimationCacheKeyTest, DefaultConstruction)
{
    rt::AnimationCacheKey key;
    EXPECT_TRUE(key.character.empty());
    EXPECT_TRUE(key.outfit.empty());
    EXPECT_TRUE(key.stance.empty());
    EXPECT_TRUE(key.animation.empty());
    EXPECT_FALSE(key.talking);
    EXPECT_EQ(key.width, 0u);
    EXPECT_EQ(key.height, 0u);
    EXPECT_FLOAT_EQ(key.fps, 30.0f);
}

TEST(AnimationCacheKeyTest, FromSpineClip)
{
    rt::SpineClip clip;
    clip.setCharacterName("Modernia");
    clip.setOutfit("outfit_01");
    clip.setStance(rt::CharacterStance::Aim);
    clip.setAnimationName("reload");
    clip.setTalking(true);

    auto key = rt::AnimationCacheKey::fromClip(clip, 1920, 1080, 24.0f);

    EXPECT_EQ(key.character, "Modernia");
    EXPECT_EQ(key.outfit, "outfit_01");
    EXPECT_EQ(key.stance, "aim");
    EXPECT_EQ(key.animation, "reload");
    EXPECT_TRUE(key.talking);
    EXPECT_EQ(key.width, 1920u);
    EXPECT_EQ(key.height, 1080u);
    EXPECT_FLOAT_EQ(key.fps, 24.0f);
}

TEST(AnimationCacheKeyTest, FromSpineClipDefaultStance)
{
    rt::SpineClip clip;
    clip.setCharacterName("Crown");
    clip.setStance(rt::CharacterStance::Default);

    auto key = rt::AnimationCacheKey::fromClip(clip, 640, 480);
    EXPECT_EQ(key.stance, "default");
}

TEST(AnimationCacheKeyTest, FromSpineClipCoverStance)
{
    rt::SpineClip clip;
    clip.setCharacterName("Crown");
    clip.setStance(rt::CharacterStance::Cover);

    auto key = rt::AnimationCacheKey::fromClip(clip, 640, 480);
    EXPECT_EQ(key.stance, "cover");
}

TEST(AnimationCacheKeyTest, Equality)
{
    rt::AnimationCacheKey a;
    a.character = "Mod";
    a.outfit = "default";
    a.stance = "default";
    a.animation = "idle";
    a.talking = false;
    a.width = 100;
    a.height = 100;
    a.fps = 30.0f;

    rt::AnimationCacheKey b = a;
    EXPECT_EQ(a, b);

    // Different character
    b = a; b.character = "Crown";
    EXPECT_FALSE(a == b);

    // Different outfit
    b = a; b.outfit = "outfit_01";
    EXPECT_FALSE(a == b);

    // Different stance
    b = a; b.stance = "aim";
    EXPECT_FALSE(a == b);

    // Different animation
    b = a; b.animation = "walk";
    EXPECT_FALSE(a == b);

    // Different talking
    b = a; b.talking = true;
    EXPECT_FALSE(a == b);

    // Different width
    b = a; b.width = 200;
    EXPECT_FALSE(a == b);

    // Different height
    b = a; b.height = 200;
    EXPECT_FALSE(a == b);

    // Different fps
    b = a; b.fps = 60.0f;
    EXPECT_FALSE(a == b);

    // Very close fps should be equal
    b = a; b.fps = 30.005f;
    EXPECT_EQ(a, b);
}

TEST(AnimationCacheKeyTest, ToString)
{
    rt::AnimationCacheKey key;
    key.character = "Modernia";
    key.outfit = "default";
    key.stance = "default";
    key.animation = "idle";
    key.talking = false;
    key.width = 1920;
    key.height = 1080;
    key.fps = 30.0f;

    std::string s = key.toString();
    EXPECT_EQ(s, "Modernia|default|default|idle|0|1920x1080@30.0");
}

TEST(AnimationCacheKeyTest, ToStringTalking)
{
    rt::AnimationCacheKey key;
    key.character = "Crown";
    key.outfit = "outfit_01";
    key.stance = "aim";
    key.animation = "talk_01";
    key.talking = true;
    key.width = 640;
    key.height = 480;
    key.fps = 24.0f;

    std::string s = key.toString();
    EXPECT_EQ(s, "Crown|outfit_01|aim|talk_01|1|640x480@24.0");
}

TEST(AnimationCacheKeyTest, ToHashDeterministic)
{
    rt::AnimationCacheKey key;
    key.character = "Modernia";
    key.outfit = "default";
    key.stance = "default";
    key.animation = "idle";
    key.width = 1920;
    key.height = 1080;

    std::string h1 = key.toHash();
    std::string h2 = key.toHash();
    EXPECT_EQ(h1, h2);
    EXPECT_EQ(h1.size(), 12u);
}

TEST(AnimationCacheKeyTest, ToHashDiffers)
{
    rt::AnimationCacheKey a;
    a.character = "Modernia";
    a.animation = "idle";
    a.width = 100;
    a.height = 100;

    rt::AnimationCacheKey b = a;
    b.animation = "walk";

    EXPECT_NE(a.toHash(), b.toHash());
}

TEST(AnimationCacheKeyTest, HashFunctor)
{
    rt::AnimationCacheKeyHash hasher;

    rt::AnimationCacheKey a;
    a.character = "Modernia";
    a.animation = "idle";
    a.width = 100;
    a.height = 100;

    rt::AnimationCacheKey b = a;

    // Same keys => same hash
    EXPECT_EQ(hasher(a), hasher(b));

    // Different keys => likely different hash (no guarantee, but test reasonable distribution)
    b.animation = "walk";
    EXPECT_NE(hasher(a), hasher(b));
}

// ═════════════════════════════════════════════════════════════════════════════
// CachedAnimFrame tests
// ═════════════════════════════════════════════════════════════════════════════

TEST(CachedAnimFrameTest, MemoryUsage)
{
    rt::CachedAnimFrame frame;
    frame.width = 64;
    frame.height = 64;
    frame.pixels.resize(64 * 64 * 4);

    EXPECT_GE(frame.memoryUsage(), 64u * 64 * 4);
}

// ═════════════════════════════════════════════════════════════════════════════
// CachedAnimationEntry tests
// ═════════════════════════════════════════════════════════════════════════════

TEST(CachedAnimationEntryTest, FrameCount)
{
    auto entry = makeEntry("Mod", "idle", 32, 32, 10);
    EXPECT_EQ(entry->frameCount(), 10u);
}

TEST(CachedAnimationEntryTest, FrameAccessByIndex)
{
    auto entry = makeEntry("Mod", "idle", 32, 32, 5);

    // Valid indices
    for (size_t i = 0; i < 5; ++i) {
        auto* f = entry->frame(i);
        ASSERT_NE(f, nullptr);
        EXPECT_EQ(f->width, 32u);
        EXPECT_EQ(f->height, 32u);
        // Check pattern
        EXPECT_EQ(f->pixels[0], static_cast<uint8_t>(i));
    }

    // Out of range → clamped to last
    auto* last = entry->frame(999);
    ASSERT_NE(last, nullptr);
    EXPECT_EQ(last->pixels[0], 4u); // last frame index = 4
}

TEST(CachedAnimationEntryTest, FrameAccessEmpty)
{
    rt::CachedAnimationEntry entry;
    EXPECT_EQ(entry.frame(0), nullptr);
    EXPECT_EQ(entry.frameAtTime(0.5f), nullptr);
}

TEST(CachedAnimationEntryTest, FrameAtTimeBasic)
{
    auto entry = makeEntry("Mod", "idle", 32, 32, 4, 2.0f);
    // 4 frames over 2 seconds => each frame = 0.5s

    auto* f0 = entry->frameAtTime(0.0f);
    ASSERT_NE(f0, nullptr);
    EXPECT_EQ(f0->pixels[0], 0u);

    auto* f1 = entry->frameAtTime(0.5f);
    ASSERT_NE(f1, nullptr);
    EXPECT_EQ(f1->pixels[0], 1u);

    auto* f2 = entry->frameAtTime(1.0f);
    ASSERT_NE(f2, nullptr);
    EXPECT_EQ(f2->pixels[0], 2u);
}

TEST(CachedAnimationEntryTest, FrameAtTimeWraps)
{
    auto entry = makeEntry("Mod", "idle", 32, 32, 4, 2.0f);

    // t=2.5 should wrap to t=0.5 → frame 1
    auto* f = entry->frameAtTime(2.5f);
    ASSERT_NE(f, nullptr);
    EXPECT_EQ(f->pixels[0], 1u);
}

TEST(CachedAnimationEntryTest, MemoryUsageMultipleFrames)
{
    auto entry = makeEntry("Mod", "idle", 64, 64, 10);
    size_t perFrame = 64 * 64 * 4;
    EXPECT_GE(entry->memoryUsage(), perFrame * 10);
}

// ═════════════════════════════════════════════════════════════════════════════
// AnimationCache — basic operations
// ═════════════════════════════════════════════════════════════════════════════

TEST(AnimationCacheTest, DefaultState)
{
    rt::AnimationCache cache;
    EXPECT_EQ(cache.entryCount(), 0u);
    EXPECT_EQ(cache.memoryUsed(), 0u);
    EXPECT_EQ(cache.memoryBudget(), 512ULL * 1024 * 1024);
    EXPECT_TRUE(cache.keys().empty());
}

TEST(AnimationCacheTest, PutAndGet)
{
    rt::AnimationCache cache;
    auto entry = makeEntry("Modernia", "idle");

    cache.put(entry);

    EXPECT_EQ(cache.entryCount(), 1u);
    EXPECT_TRUE(cache.containsInMemory(entry->key));

    auto retrieved = cache.get(entry->key);
    ASSERT_NE(retrieved, nullptr);
    EXPECT_EQ(retrieved->key, entry->key);
    EXPECT_EQ(retrieved->frameCount(), entry->frameCount());
}

TEST(AnimationCacheTest, GetReturnsNullForMiss)
{
    rt::AnimationCache cache;

    rt::AnimationCacheKey key;
    key.character = "Nobody";
    key.animation = "nothing";

    EXPECT_EQ(cache.get(key), nullptr);
    EXPECT_FALSE(cache.containsInMemory(key));
}

TEST(AnimationCacheTest, PutOverwrite)
{
    rt::AnimationCache cache;
    auto e1 = makeEntry("Mod", "idle", 32, 32, 2);
    auto e2 = makeEntry("Mod", "idle", 32, 32, 5); // same key, more frames

    cache.put(e1);
    EXPECT_EQ(cache.entryCount(), 1u);
    EXPECT_EQ(cache.get(e1->key)->frameCount(), 2u);

    cache.put(e2);
    EXPECT_EQ(cache.entryCount(), 1u);
    EXPECT_EQ(cache.get(e2->key)->frameCount(), 5u);
}

TEST(AnimationCacheTest, MultipleEntries)
{
    rt::AnimationCache cache;

    auto e1 = makeEntry("Modernia", "idle");
    auto e2 = makeEntry("Crown", "idle");
    auto e3 = makeEntry("Modernia", "walk");

    cache.put(e1);
    cache.put(e2);
    cache.put(e3);

    EXPECT_EQ(cache.entryCount(), 3u);
    EXPECT_NE(cache.get(e1->key), nullptr);
    EXPECT_NE(cache.get(e2->key), nullptr);
    EXPECT_NE(cache.get(e3->key), nullptr);
}

TEST(AnimationCacheTest, ContainsChecksMemory)
{
    rt::AnimationCache cache;
    auto entry = makeEntry("Mod", "idle");

    EXPECT_FALSE(cache.contains(entry->key));
    cache.put(entry);
    EXPECT_TRUE(cache.contains(entry->key));
}

// ═════════════════════════════════════════════════════════════════════════════
// AnimationCache — eviction
// ═════════════════════════════════════════════════════════════════════════════

TEST(AnimationCacheTest, EvictSingleKey)
{
    rt::AnimationCache cache;
    auto entry = makeEntry("Mod", "idle");

    cache.put(entry);
    EXPECT_EQ(cache.entryCount(), 1u);

    cache.evict(entry->key);
    EXPECT_EQ(cache.entryCount(), 0u);
    EXPECT_EQ(cache.get(entry->key), nullptr);
}

TEST(AnimationCacheTest, EvictCharacter)
{
    rt::AnimationCache cache;

    cache.put(makeEntry("Modernia", "idle"));
    cache.put(makeEntry("Modernia", "walk"));
    cache.put(makeEntry("Crown", "idle"));

    EXPECT_EQ(cache.entryCount(), 3u);

    cache.evictCharacter("Modernia");
    EXPECT_EQ(cache.entryCount(), 1u);

    auto keys = cache.keys();
    EXPECT_EQ(keys.size(), 1u);
    EXPECT_EQ(keys[0].character, "Crown");
}

TEST(AnimationCacheTest, ClearMemory)
{
    rt::AnimationCache cache;

    cache.put(makeEntry("Mod", "idle"));
    cache.put(makeEntry("Crown", "idle"));

    EXPECT_EQ(cache.entryCount(), 2u);
    EXPECT_GT(cache.memoryUsed(), 0u);

    cache.clearMemory();

    EXPECT_EQ(cache.entryCount(), 0u);
    EXPECT_EQ(cache.memoryUsed(), 0u);
}

// ═════════════════════════════════════════════════════════════════════════════
// AnimationCache — LRU eviction under memory budget
// ═════════════════════════════════════════════════════════════════════════════

TEST(AnimationCacheTest, LruEvictionOnBudget)
{
    rt::AnimationCache cache;

    // Each entry: 64x64x4 pixels * 4 frames = 65536 bytes + overhead
    auto e1 = makeEntry("A", "idle");
    auto e2 = makeEntry("B", "idle");
    auto e3 = makeEntry("C", "idle");

    size_t perEntry = e1->memoryUsage();

    // Budget fits exactly 2 entries
    cache.setMemoryBudget(perEntry * 2 + 100);

    cache.put(e1);
    cache.put(e2);
    EXPECT_EQ(cache.entryCount(), 2u);

    // Adding a third should evict the LRU (e1)
    cache.put(e3);
    EXPECT_EQ(cache.entryCount(), 2u);
    EXPECT_EQ(cache.get(e1->key), nullptr);  // evicted
    EXPECT_NE(cache.get(e2->key), nullptr);   // still present
    EXPECT_NE(cache.get(e3->key), nullptr);   // just added
}

TEST(AnimationCacheTest, LruPromotionOnAccess)
{
    rt::AnimationCache cache;

    auto e1 = makeEntry("A", "idle");
    auto e2 = makeEntry("B", "idle");
    auto e3 = makeEntry("C", "idle");

    size_t perEntry = e1->memoryUsage();
    cache.setMemoryBudget(perEntry * 2 + 100);

    cache.put(e1);  // LRU order: [e1]
    cache.put(e2);  // LRU order: [e2, e1]

    // Access e1 → promotes it to MRU
    (void)cache.get(e1->key);  // LRU order: [e1, e2]

    // Adding e3 should evict e2 (now LRU)
    cache.put(e3);  // LRU order: [e3, e1]
    EXPECT_NE(cache.get(e1->key), nullptr);   // still present (was promoted)
    EXPECT_EQ(cache.get(e2->key), nullptr);   // evicted
    EXPECT_NE(cache.get(e3->key), nullptr);   // just added
}

TEST(AnimationCacheTest, SetMemoryBudgetTriggersEviction)
{
    rt::AnimationCache cache;

    auto e1 = makeEntry("A", "idle");
    auto e2 = makeEntry("B", "idle");
    auto e3 = makeEntry("C", "idle");

    cache.put(e1);
    cache.put(e2);
    cache.put(e3);
    EXPECT_EQ(cache.entryCount(), 3u);

    // Shrink budget to fit only 1
    size_t perEntry = e1->memoryUsage();
    cache.setMemoryBudget(perEntry + 100);

    EXPECT_EQ(cache.entryCount(), 1u);
    // The most recently used (e3) should survive
    EXPECT_NE(cache.get(e3->key), nullptr);
}

// ═════════════════════════════════════════════════════════════════════════════
// AnimationCache — statistics
// ═════════════════════════════════════════════════════════════════════════════

TEST(AnimationCacheTest, StatsHitsMisses)
{
    rt::AnimationCache cache;

    auto entry = makeEntry("Mod", "idle");
    cache.put(entry);

    // 3 hits
    (void)cache.get(entry->key);
    (void)cache.get(entry->key);
    (void)cache.get(entry->key);

    // 2 misses
    rt::AnimationCacheKey missKey;
    missKey.character = "Nobody";
    missKey.animation = "none";
    (void)cache.get(missKey);
    (void)cache.get(missKey);

    auto s = cache.stats();
    EXPECT_EQ(s.hitCount, 3u);
    EXPECT_EQ(s.missCount, 2u);
    EXPECT_DOUBLE_EQ(s.hitRate(), 3.0 / 5.0);
    EXPECT_EQ(s.entryCount, 1u);
    EXPECT_EQ(s.totalFrames, 4u);  // 4 frames per entry
    EXPECT_GT(s.memoryUsed, 0u);
}

TEST(AnimationCacheTest, StatsEvictionCount)
{
    rt::AnimationCache cache;

    auto e1 = makeEntry("A", "idle");
    size_t perEntry = e1->memoryUsage();
    cache.setMemoryBudget(perEntry + 100);

    cache.put(e1);
    cache.put(makeEntry("B", "idle"));  // evicts A
    cache.put(makeEntry("C", "idle"));  // evicts B

    auto s = cache.stats();
    EXPECT_EQ(s.evictionCount, 2u);
}

TEST(AnimationCacheTest, StatsHitRateZero)
{
    rt::AnimationCacheStats stats;
    EXPECT_DOUBLE_EQ(stats.hitRate(), 0.0);
}

// ═════════════════════════════════════════════════════════════════════════════
// AnimationCache — validation callback
// ═════════════════════════════════════════════════════════════════════════════

TEST(AnimationCacheTest, ValidatorRejectsCacheHit)
{
    rt::AnimationCache cache;
    auto entry = makeEntry("Mod", "idle");
    cache.put(entry);

    // Validator that rejects "Mod" character
    cache.setValidator([](const rt::AnimationCacheKey& k) {
        return k.character != "Mod";
    });

    // Get should fail because validator rejects it
    auto result = cache.get(entry->key);
    EXPECT_EQ(result, nullptr);

    // Entry should be evicted
    EXPECT_EQ(cache.entryCount(), 0u);
}

TEST(AnimationCacheTest, ValidatorAcceptsCacheHit)
{
    rt::AnimationCache cache;
    auto entry = makeEntry("Crown", "idle");
    cache.put(entry);

    cache.setValidator([](const rt::AnimationCacheKey& k) {
        return k.character == "Crown";
    });

    auto result = cache.get(entry->key);
    EXPECT_NE(result, nullptr);
}

// ═════════════════════════════════════════════════════════════════════════════
// AnimationCache — disk persistence
// ═════════════════════════════════════════════════════════════════════════════

TEST(AnimationCacheTest, SaveAndLoadFromDisk)
{
    TempCacheDir tmpDir;
    rt::AnimationCache cache;
    cache.setCacheDirectory(tmpDir.path());

    auto entry = makeEntry("Modernia", "idle", 32, 32, 3, 1.0f, 30.0f);
    cache.put(entry);

    // Save
    EXPECT_TRUE(cache.saveToDisk(entry->key));

    // Verify files on disk
    EXPECT_TRUE(fs::exists(tmpDir.path()));

    // Clear memory and reload
    cache.clearMemory();
    EXPECT_EQ(cache.entryCount(), 0u);

    // Scan disk
    cache.scanDiskCache();
    EXPECT_TRUE(cache.containsOnDisk(entry->key));

    // Load from disk
    EXPECT_TRUE(cache.loadFromDisk(entry->key));
    EXPECT_EQ(cache.entryCount(), 1u);

    // Verify loaded data matches
    auto loaded = cache.get(entry->key);
    ASSERT_NE(loaded, nullptr);
    EXPECT_EQ(loaded->frameCount(), 3u);
    EXPECT_EQ(loaded->key.character, "Modernia");
    EXPECT_EQ(loaded->key.animation, "idle");
    EXPECT_EQ(loaded->key.width, 32u);
    EXPECT_EQ(loaded->key.height, 32u);
    EXPECT_FLOAT_EQ(loaded->duration, 1.0f);
}

TEST(AnimationCacheTest, LoadFromDiskPixelDataIntegrity)
{
    TempCacheDir tmpDir;
    rt::AnimationCache cache;
    cache.setCacheDirectory(tmpDir.path());

    auto entry = makeEntry("Crown", "walk", 16, 16, 2, 0.5f, 30.0f);

    // Frame 0 pixels are all 0, frame 1 pixels are all 1
    cache.put(entry);
    EXPECT_TRUE(cache.saveToDisk(entry->key));

    cache.clearMemory();
    cache.scanDiskCache();
    EXPECT_TRUE(cache.loadFromDisk(entry->key));

    auto loaded = cache.get(entry->key);
    ASSERT_NE(loaded, nullptr);
    ASSERT_EQ(loaded->frameCount(), 2u);

    // Verify pixel data
    EXPECT_EQ(loaded->frames[0].pixels[0], 0u);
    EXPECT_EQ(loaded->frames[1].pixels[0], 1u);
}

TEST(AnimationCacheTest, SaveReturnsfalseForMissingKey)
{
    rt::AnimationCache cache;
    rt::AnimationCacheKey key;
    key.character = "Nobody";
    key.animation = "none";
    EXPECT_FALSE(cache.saveToDisk(key));
}

TEST(AnimationCacheTest, LoadFromDiskReturnsFalseForMissing)
{
    TempCacheDir tmpDir;
    rt::AnimationCache cache;
    cache.setCacheDirectory(tmpDir.path());

    rt::AnimationCacheKey key;
    key.character = "Nobody";
    key.animation = "none";
    EXPECT_FALSE(cache.loadFromDisk(key));
}

TEST(AnimationCacheTest, InvalidateCharacterFromDisk)
{
    TempCacheDir tmpDir;
    rt::AnimationCache cache;
    cache.setCacheDirectory(tmpDir.path());

    auto e1 = makeEntry("Modernia", "idle", 16, 16, 2);
    auto e2 = makeEntry("Modernia", "walk", 16, 16, 2);
    auto e3 = makeEntry("Crown", "idle", 16, 16, 2);

    cache.put(e1);
    cache.put(e2);
    cache.put(e3);

    cache.saveToDisk(e1->key);
    cache.saveToDisk(e2->key);
    cache.saveToDisk(e3->key);

    // Invalidate Modernia (memory + disk)
    cache.invalidateCharacter("Modernia");

    // Memory: only Crown remains
    EXPECT_EQ(cache.entryCount(), 1u);
    EXPECT_NE(cache.get(e3->key), nullptr);

    // Disk: Crown's dir should still exist, Modernia's should be gone
    auto diskKeys = cache.diskKeys();
    EXPECT_EQ(diskKeys.size(), 1u);
    if (!diskKeys.empty())
        EXPECT_EQ(diskKeys[0].character, "Crown");
}

TEST(AnimationCacheTest, ScanDiskCache)
{
    TempCacheDir tmpDir;

    {
        // First cache instance: save some entries
        rt::AnimationCache cache;
        cache.setCacheDirectory(tmpDir.path());

        cache.put(makeEntry("Mod", "idle", 16, 16, 2));
        cache.put(makeEntry("Crown", "walk", 16, 16, 2));

        for (auto& k : cache.keys())
            cache.saveToDisk(k);
    }

    {
        // Second cache instance: should find entries on disk
        rt::AnimationCache cache;
        cache.setCacheDirectory(tmpDir.path());
        cache.scanDiskCache();

        auto diskKeys = cache.diskKeys();
        EXPECT_EQ(diskKeys.size(), 2u);
    }
}

TEST(AnimationCacheTest, CacheDirectory)
{
    rt::AnimationCache cache;
    EXPECT_TRUE(cache.cacheDirectory().empty());

    cache.setCacheDirectory("/some/path");
    EXPECT_EQ(cache.cacheDirectory(), "/some/path");
}

// ═════════════════════════════════════════════════════════════════════════════
// AnimationCache — memory accounting
// ═════════════════════════════════════════════════════════════════════════════

TEST(AnimationCacheTest, MemoryUsedConsistedWithEntries)
{
    rt::AnimationCache cache;

    auto e1 = makeEntry("A", "idle");
    auto e2 = makeEntry("B", "idle");
    size_t m1 = e1->memoryUsage();
    size_t m2 = e2->memoryUsage();

    cache.put(e1);
    EXPECT_EQ(cache.memoryUsed(), m1);

    cache.put(e2);
    EXPECT_EQ(cache.memoryUsed(), m1 + m2);

    cache.evict(e1->key);
    EXPECT_EQ(cache.memoryUsed(), m2);

    cache.clearMemory();
    EXPECT_EQ(cache.memoryUsed(), 0u);
}

// ═════════════════════════════════════════════════════════════════════════════
// AnimationCache — keys() and diskKeys()
// ═════════════════════════════════════════════════════════════════════════════

TEST(AnimationCacheTest, KeysReturnsAllMemoryKeys)
{
    rt::AnimationCache cache;

    cache.put(makeEntry("A", "idle"));
    cache.put(makeEntry("B", "walk"));

    auto keys = cache.keys();
    EXPECT_EQ(keys.size(), 2u);

    bool foundA = false, foundB = false;
    for (auto& k : keys) {
        if (k.character == "A") foundA = true;
        if (k.character == "B") foundB = true;
    }
    EXPECT_TRUE(foundA);
    EXPECT_TRUE(foundB);
}

// ═════════════════════════════════════════════════════════════════════════════
// AnimationCache — clearAll (including disk)
// ═════════════════════════════════════════════════════════════════════════════

TEST(AnimationCacheTest, ClearAllRemovesDiskCache)
{
    TempCacheDir tmpDir;
    rt::AnimationCache cache;
    cache.setCacheDirectory(tmpDir.path());

    auto entry = makeEntry("Mod", "idle", 16, 16, 2);
    cache.put(entry);
    cache.saveToDisk(entry->key);

    EXPECT_TRUE(fs::exists(tmpDir.path()));

    cache.clearAll();

    EXPECT_EQ(cache.entryCount(), 0u);
    EXPECT_EQ(cache.memoryUsed(), 0u);
    EXPECT_TRUE(cache.diskKeys().empty());
}

// ═════════════════════════════════════════════════════════════════════════════
// AnimationCache — thread safety (basic)
// ═════════════════════════════════════════════════════════════════════════════

TEST(AnimationCacheTest, ConcurrentPutAndGet)
{
    rt::AnimationCache cache;

    constexpr int numThreads = 4;
    constexpr int opsPerThread = 50;

    std::vector<std::thread> threads;

    for (int t = 0; t < numThreads; ++t) {
        threads.emplace_back([&cache, t]() {
            for (int i = 0; i < opsPerThread; ++i) {
                std::string character = "Char_" + std::to_string(t) + "_" + std::to_string(i);
                auto entry = makeEntry(character, "idle", 8, 8, 1);
                cache.put(entry);
                (void)cache.get(entry->key);
                (void)cache.containsInMemory(entry->key);
            }
        });
    }

    for (auto& th : threads)
        th.join();

    // No crashes or data corruption
    EXPECT_GT(cache.entryCount(), 0u);
}

// ═════════════════════════════════════════════════════════════════════════════
// AnimationCache — edge cases
// ═════════════════════════════════════════════════════════════════════════════

TEST(AnimationCacheTest, PutNullptrIgnored)
{
    rt::AnimationCache cache;
    cache.put(nullptr);
    EXPECT_EQ(cache.entryCount(), 0u);
}

TEST(AnimationCacheTest, EvictNonexistentKeyNoOp)
{
    rt::AnimationCache cache;

    rt::AnimationCacheKey key;
    key.character = "Nobody";

    // Should not crash
    cache.evict(key);
    cache.evictCharacter("Nobody");
    EXPECT_EQ(cache.entryCount(), 0u);
}

TEST(AnimationCacheTest, ZeroBudgetEvictsEverything)
{
    rt::AnimationCache cache;
    cache.put(makeEntry("A", "idle"));
    cache.put(makeEntry("B", "idle"));

    cache.setMemoryBudget(0);
    EXPECT_EQ(cache.entryCount(), 0u);
    EXPECT_EQ(cache.memoryUsed(), 0u);
}

TEST(AnimationCacheTest, DifferentTalkingStatesDifferentKeys)
{
    rt::AnimationCache cache;

    auto e1 = makeEntry("Mod", "idle", 32, 32, 2, 1.0f, 30.0f, false);
    auto e2 = makeEntry("Mod", "idle", 32, 32, 2, 1.0f, 30.0f, true);

    cache.put(e1);
    cache.put(e2);

    // They should be different cache entries
    EXPECT_EQ(cache.entryCount(), 2u);
}

TEST(AnimationCacheTest, DifferentResolutionsDifferentKeys)
{
    rt::AnimationCache cache;

    auto e1 = makeEntry("Mod", "idle", 64, 64, 2);
    auto e2 = makeEntry("Mod", "idle", 128, 128, 2);

    cache.put(e1);
    cache.put(e2);

    EXPECT_EQ(cache.entryCount(), 2u);
}

TEST(AnimationCacheTest, DifferentFpsDifferentKeys)
{
    rt::AnimationCache cache;

    auto e1 = makeEntry("Mod", "idle", 32, 32, 2, 1.0f, 30.0f);
    auto e2 = makeEntry("Mod", "idle", 32, 32, 2, 1.0f, 60.0f);

    cache.put(e1);
    cache.put(e2);

    EXPECT_EQ(cache.entryCount(), 2u);
}

TEST(AnimationCacheTest, DifferentOutfitsDifferentKeys)
{
    rt::AnimationCache cache;

    auto e1 = makeEntry("Mod", "idle", 32, 32, 2, 1.0f, 30.0f, false, "default");
    auto e2 = makeEntry("Mod", "idle", 32, 32, 2, 1.0f, 30.0f, false, "outfit_01");

    cache.put(e1);
    cache.put(e2);

    EXPECT_EQ(cache.entryCount(), 2u);
}

TEST(AnimationCacheTest, DifferentStancesDifferentKeys)
{
    rt::AnimationCache cache;

    auto e1 = makeEntry("Mod", "idle", 32, 32, 2, 1.0f, 30.0f, false, "default", "default");
    auto e2 = makeEntry("Mod", "idle", 32, 32, 2, 1.0f, 30.0f, false, "default", "aim");

    cache.put(e1);
    cache.put(e2);

    EXPECT_EQ(cache.entryCount(), 2u);
}
