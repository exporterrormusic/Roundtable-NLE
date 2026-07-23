/*
 * test_animation_video_cache.cpp — Regression tests for the
 * AnimationVideoCache deletion-vs-in-flight-render race.
 *
 * A render job is "in flight" once a worker has dequeued it from
 * m_jobQueue but its key is still in m_pendingKeys.  Deleting a
 * character/outfit while such a render is running must leave a guard in
 * m_deletingOutfits so the worker discards its result instead of
 * re-adding an entry after the deletion.
 *
 * Historical bugs covered:
 *   - removeAllForCharacter derived guards from completed entries only,
 *     missing an outfit whose FIRST render was still in flight.
 *   - Both removers wiped in-flight pending keys, which made their own
 *     guard-cleanup passes conclude "no in-flight renders" and erase the
 *     guards they had just set — defeating the mechanism entirely.
 *
 * These tests use AnimationVideoCacheTestPeer (friend of the cache) to
 * simulate queued/in-flight state without spinning up render workers.
 */

#include <gtest/gtest.h>

#include "spine/AnimationVideoCache.h"

#include <filesystem>
#include <mutex>
#include <string>

namespace fs = std::filesystem;

namespace rt {

struct AnimationVideoCacheTestPeer
{
    /// Simulate a completed render in the inventory.
    static void addEntry(AnimationVideoCache& c,
                         const std::string& charName,
                         const std::string& outfit,
                         const std::string& anim)
    {
        std::lock_guard lock(c.m_mutex);
        AnimCacheEntry e;
        e.characterName = charName;
        e.outfit        = outfit;
        e.animationName = anim;
        c.m_entries[AnimationVideoCache::makeKey(charName, outfit, anim)] =
            std::move(e);
    }

    /// Simulate an IN-FLIGHT render: dequeued by a worker (not in the
    /// job queue) but not yet completed (key still pending).
    static void addInFlight(AnimationVideoCache& c,
                            const std::string& charName,
                            const std::string& outfit,
                            const std::string& anim)
    {
        std::lock_guard lock(c.m_mutex);
        c.m_pendingKeys.insert(
            AnimationVideoCache::makeKey(charName, outfit, anim));
    }

    /// Simulate a QUEUED render: still in the job queue, key pending.
    static void addQueued(AnimationVideoCache& c,
                          const std::string& charName,
                          const std::string& outfit,
                          const std::string& anim)
    {
        std::lock_guard lock(c.m_mutex);
        c.m_pendingKeys.insert(
            AnimationVideoCache::makeKey(charName, outfit, anim));
        c.m_jobQueue.push_back({charName, outfit, anim, false});
    }

    static bool isGuarded(AnimationVideoCache& c,
                          const std::string& charName,
                          const std::string& outfit)
    {
        std::lock_guard lock(c.m_mutex);
        return c.m_deletingOutfits.count(charName + "|" + outfit) > 0;
    }

    static bool hasPendingKey(AnimationVideoCache& c,
                              const std::string& charName,
                              const std::string& outfit,
                              const std::string& anim)
    {
        std::lock_guard lock(c.m_mutex);
        return c.m_pendingKeys.count(
                   AnimationVideoCache::makeKey(charName, outfit, anim)) > 0;
    }

    static size_t jobQueueSize(AnimationVideoCache& c)
    {
        std::lock_guard lock(c.m_mutex);
        return c.m_jobQueue.size();
    }
};

} // namespace rt

namespace {

using rt::AnimationVideoCache;
using Peer = rt::AnimationVideoCacheTestPeer;

class AnimationVideoCacheDeletionRace : public ::testing::Test
{
protected:
    void SetUp() override
    {
        m_tmpDir = fs::temp_directory_path() / "rt_avc_race_test";
        std::error_code ec;
        fs::create_directories(m_tmpDir / "converted", ec);
        m_cache = std::make_unique<AnimationVideoCache>(
            nullptr,
            (m_tmpDir / "converted").string(),
            (m_tmpDir / "assets").string());
    }

    void TearDown() override
    {
        m_cache.reset();
        std::error_code ec;
        fs::remove_all(m_tmpDir, ec);
    }

    fs::path m_tmpDir;
    std::unique_ptr<AnimationVideoCache> m_cache;
};

// The original P0: deleting a character while its outfit's FIRST render
// is in flight (no completed entries yet) must still set the guard.
TEST_F(AnimationVideoCacheDeletionRace,
       RemoveAllForCharacter_GuardsInFlightOutfitWithoutEntries)
{
    Peer::addInFlight(*m_cache, "Hero", "casual", "idle");

    m_cache->removeAllForCharacter("Hero");

    EXPECT_TRUE(Peer::isGuarded(*m_cache, "Hero", "casual"));
    // The in-flight key must survive: the worker erases it on completion
    // and the guard-cleanup logic uses it to detect in-flight renders.
    EXPECT_TRUE(Peer::hasPendingKey(*m_cache, "Hero", "casual", "idle"));
}

// The subtler half of the bug: the remover wiped in-flight pending keys
// and then its guard-cleanup pass concluded "no in-flight renders" and
// erased the guard it had just set.
TEST_F(AnimationVideoCacheDeletionRace,
       RemoveAllForCharacter_GuardSurvivesForOutfitWithEntries)
{
    Peer::addEntry(*m_cache, "Hero", "formal", "idle");
    Peer::addInFlight(*m_cache, "Hero", "formal", "walk");

    m_cache->removeAllForCharacter("Hero");

    EXPECT_EQ(m_cache->entryCount(), 0u);
    EXPECT_TRUE(Peer::isGuarded(*m_cache, "Hero", "formal"));
}

// No in-flight render → no guard should linger.
TEST_F(AnimationVideoCacheDeletionRace,
       RemoveAllForCharacter_NoInFlight_NoGuard)
{
    Peer::addEntry(*m_cache, "Hero", "formal", "idle");

    m_cache->removeAllForCharacter("Hero");

    EXPECT_EQ(m_cache->entryCount(), 0u);
    EXPECT_FALSE(Peer::isGuarded(*m_cache, "Hero", "formal"));
}

// Queued-but-not-dequeued jobs are simply dropped (queue + key), and do
// not leave a guard behind; other characters are untouched.
TEST_F(AnimationVideoCacheDeletionRace,
       RemoveAllForCharacter_DropsQueuedJobs_LeavesOthersAlone)
{
    Peer::addQueued(*m_cache, "Hero", "casual", "run");
    Peer::addEntry(*m_cache, "Villain", "cape", "idle");
    Peer::addInFlight(*m_cache, "Villain", "cape", "laugh");

    m_cache->removeAllForCharacter("Hero");

    EXPECT_EQ(Peer::jobQueueSize(*m_cache), 0u);
    EXPECT_FALSE(Peer::hasPendingKey(*m_cache, "Hero", "casual", "run"));
    EXPECT_FALSE(Peer::isGuarded(*m_cache, "Hero", "casual"));

    EXPECT_EQ(m_cache->entryCount(), 1u);
    EXPECT_TRUE(Peer::hasPendingKey(*m_cache, "Villain", "cape", "laugh"));
    EXPECT_FALSE(Peer::isGuarded(*m_cache, "Villain", "cape"));
}

TEST_F(AnimationVideoCacheDeletionRace,
       RemoveAllForCharacterOutfit_GuardSurvivesWithInFlight)
{
    Peer::addEntry(*m_cache, "Hero", "casual", "idle");
    Peer::addInFlight(*m_cache, "Hero", "casual", "walk");

    m_cache->removeAllForCharacterOutfit("Hero", "casual");

    EXPECT_EQ(m_cache->entryCount(), 0u);
    EXPECT_TRUE(Peer::isGuarded(*m_cache, "Hero", "casual"));
    EXPECT_TRUE(Peer::hasPendingKey(*m_cache, "Hero", "casual", "walk"));
}

TEST_F(AnimationVideoCacheDeletionRace,
       RemoveAllForCharacterOutfit_NoInFlight_GuardCleared)
{
    Peer::addEntry(*m_cache, "Hero", "casual", "idle");
    Peer::addQueued(*m_cache, "Hero", "casual", "walk");

    m_cache->removeAllForCharacterOutfit("Hero", "casual");

    EXPECT_EQ(m_cache->entryCount(), 0u);
    EXPECT_EQ(Peer::jobQueueSize(*m_cache), 0u);
    EXPECT_FALSE(Peer::isGuarded(*m_cache, "Hero", "casual"));
}

// Re-queuing an outfit whose guard is still active (render in flight at
// deletion time) must clear the guard so the in-flight result is
// accepted instead of silently discarded.
TEST_F(AnimationVideoCacheDeletionRace,
       QueueRender_ClearsGuardForReQueuedOutfit)
{
    Peer::addInFlight(*m_cache, "Hero", "casual", "idle");
    m_cache->removeAllForCharacter("Hero");
    ASSERT_TRUE(Peer::isGuarded(*m_cache, "Hero", "casual"));

    m_cache->queueRender("Hero", "casual", "idle");

    EXPECT_FALSE(Peer::isGuarded(*m_cache, "Hero", "casual"));
}

} // namespace
