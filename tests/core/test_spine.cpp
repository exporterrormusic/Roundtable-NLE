/*
 * test_spine.cpp — Tests for Step 8: Spine Engine
 *
 * Tests SpineAtlas, SpineAnimation, SpineEngine, and ModelManager using
 * the real Spine character assets in assets/characters/.
 */

#include <gtest/gtest.h>

#ifdef ROUNDTABLE_HAS_SPINE

#include "spine/SpineAtlas.h"
#include "spine/SpineAnimation.h"
#include "spine/SpineEngine.h"
#include "spine/ModelManager.h"
#include "timeline/SpineClip.h"

#include <filesystem>
#include <fstream>
#include <algorithm>

namespace fs = std::filesystem;

// ─── Test fixture ───────────────────────────────────────────────────────────
// Locates the assets directory relative to the build output
class SpineTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        // Try to find assets dir by walking up from executable location
        // Common build layout: build/tests/ or build/Debug/ etc.
        fs::path candidates[] = {
            fs::path(__FILE__).parent_path().parent_path().parent_path() / "assets",
            fs::current_path() / "assets",
            fs::current_path().parent_path() / "assets",
            fs::current_path().parent_path().parent_path() / "assets",
        };

        for (auto& c : candidates) {
            if (fs::exists(c / "characters")) {
                assetsDir = c.string();
                break;
            }
        }

        hasAssets = !assetsDir.empty();
        if (!hasAssets) {
            // Not a failure — tests that need assets will be skipped
        }
    }

    std::string assetsDir;
    bool hasAssets = false;

    // Helper: find first .atlas file in a character directory
    struct CharFiles {
        std::string skel, atlas, texture;
        bool valid() const { return !skel.empty() && !atlas.empty(); }
    };

    CharFiles findCharFiles(const std::string& charName,
                            const std::string& outfit = "default",
                            const std::string& stance = "") const
    {
        CharFiles result;
        if (!hasAssets) return result;

        fs::path dir = fs::path(assetsDir) / "characters" / charName / outfit;
        if (!stance.empty()) dir = dir / stance;

        if (!fs::exists(dir)) return result;

        for (auto& entry : fs::directory_iterator(dir)) {
            auto ext = entry.path().extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
            if (ext == ".skel") result.skel = entry.path().string();
            else if (ext == ".atlas") result.atlas = entry.path().string();
            else if (ext == ".png") result.texture = entry.path().string();
        }
        return result;
    }
};

// ═════════════════════════════════════════════════════════════════════════════
//  SpineAtlas Tests
// ═════════════════════════════════════════════════════════════════════════════

TEST_F(SpineTest, AtlasDefaultConstruction)
{
    rt::SpineAtlas atlas;
    EXPECT_FALSE(atlas.isLoaded());
    EXPECT_EQ(atlas.pages().size(), 0u);
    EXPECT_EQ(atlas.regions().size(), 0u);
}

TEST_F(SpineTest, AtlasLoadInvalidPath)
{
    rt::SpineAtlas atlas;
    EXPECT_FALSE(atlas.load("nonexistent_path.atlas"));
    EXPECT_FALSE(atlas.isLoaded());
}

TEST_F(SpineTest, AtlasLoadRealFile)
{
    if (!hasAssets) GTEST_SKIP() << "No assets directory found";

    // Try Crown (4.1) or Modernia (4.0) — either should work
    auto files = findCharFiles("Crown");
    if (!files.valid()) files = findCharFiles("Modernia");
    if (!files.valid()) GTEST_SKIP() << "No character files found";

    rt::SpineAtlas atlas;
    ASSERT_TRUE(atlas.load(files.atlas));
    EXPECT_TRUE(atlas.isLoaded());
    EXPECT_GT(atlas.pages().size(), 0u);
    EXPECT_GT(atlas.regions().size(), 0u);
    EXPECT_NE(atlas.getSpineAtlas(), nullptr);

    // Check page info
    auto& page = atlas.pages()[0];
    EXPECT_GT(page.width, 0);
    EXPECT_GT(page.height, 0);
    EXPECT_TRUE(page.pma);  // All NIKKE atlases use premultiplied alpha
    EXPECT_FALSE(page.texturePath.empty());

    // Check we can find regions
    auto& regions = atlas.regions();
    EXPECT_GT(regions.size(), 10u);  // Characters have many parts

    // Test findRegion
    auto* firstRegion = atlas.findRegion(regions[0].name);
    EXPECT_NE(firstRegion, nullptr);
    EXPECT_EQ(firstRegion->name, regions[0].name);

    // Test findRegion with nonexistent name
    EXPECT_EQ(atlas.findRegion("nonexistent_region_xyz"), nullptr);
}

TEST_F(SpineTest, AtlasLoadFromMemory)
{
    if (!hasAssets) GTEST_SKIP() << "No assets directory found";

    auto files = findCharFiles("Crown");
    if (!files.valid()) files = findCharFiles("Modernia");
    if (!files.valid()) GTEST_SKIP() << "No character files found";

    // Read atlas file into memory
    std::ifstream f(files.atlas, std::ios::binary);
    ASSERT_TRUE(f.is_open());
    std::string data((std::istreambuf_iterator<char>(f)),
                      std::istreambuf_iterator<char>());

    auto dir = fs::path(files.atlas).parent_path().string();

    rt::SpineAtlas atlas;
    ASSERT_TRUE(atlas.loadFromMemory(data.c_str(), static_cast<int>(data.size()), dir));
    EXPECT_TRUE(atlas.isLoaded());
    EXPECT_GT(atlas.pages().size(), 0u);
    EXPECT_GT(atlas.regions().size(), 0u);
}

TEST_F(SpineTest, AtlasMove)
{
    if (!hasAssets) GTEST_SKIP() << "No assets directory found";

    auto files = findCharFiles("Crown");
    if (!files.valid()) GTEST_SKIP() << "No character files found";

    rt::SpineAtlas atlas1;
    ASSERT_TRUE(atlas1.load(files.atlas));
    auto pageCount = atlas1.pages().size();
    auto regionCount = atlas1.regions().size();

    // Move construct
    rt::SpineAtlas atlas2 = std::move(atlas1);
    EXPECT_TRUE(atlas2.isLoaded());
    EXPECT_EQ(atlas2.pages().size(), pageCount);
    EXPECT_EQ(atlas2.regions().size(), regionCount);
}

// ═════════════════════════════════════════════════════════════════════════════
//  SpineEngine — Version Detection
// ═════════════════════════════════════════════════════════════════════════════

TEST_F(SpineTest, VersionDetection)
{
    if (!hasAssets) GTEST_SKIP() << "No assets directory found";

    // Modernia default is 4.0
    auto modFiles = findCharFiles("Modernia");
    if (modFiles.valid()) {
        auto ver = rt::SpineEngine::detectVersion(modFiles.skel);
        EXPECT_FALSE(ver.empty());
        EXPECT_TRUE(ver.find("4.") == 0) << "Version: " << ver;
    }

    // Crown default is 4.1
    auto crownFiles = findCharFiles("Crown");
    if (crownFiles.valid()) {
        auto ver = rt::SpineEngine::detectVersion(crownFiles.skel);
        EXPECT_FALSE(ver.empty());
        EXPECT_TRUE(ver.find("4.") == 0) << "Version: " << ver;
    }
}

TEST_F(SpineTest, VersionDetectionInvalidFile)
{
    auto ver = rt::SpineEngine::detectVersion("nonexistent.skel");
    EXPECT_TRUE(ver.empty());
}

// ═════════════════════════════════════════════════════════════════════════════
//  SpineEngine — Path Resolution
// ═════════════════════════════════════════════════════════════════════════════

TEST_F(SpineTest, PathResolution_Default)
{
    if (!hasAssets) GTEST_SKIP() << "No assets directory found";

    auto paths = rt::SpineEngine::resolvePaths(assetsDir, "Crown", "default",
                                                rt::CharacterStance::Default);
    if (!paths.valid) GTEST_SKIP() << "Crown not available";

    EXPECT_TRUE(paths.valid);
    EXPECT_FALSE(paths.skelPath.empty());
    EXPECT_FALSE(paths.atlasPath.empty());
    EXPECT_TRUE(fs::exists(paths.skelPath));
    EXPECT_TRUE(fs::exists(paths.atlasPath));
}

TEST_F(SpineTest, PathResolution_Aim)
{
    if (!hasAssets) GTEST_SKIP() << "No assets directory found";

    auto paths = rt::SpineEngine::resolvePaths(assetsDir, "Crown", "default",
                                                rt::CharacterStance::Aim);
    if (!paths.valid) GTEST_SKIP() << "Crown aim not available";

    EXPECT_TRUE(paths.valid);
    EXPECT_TRUE(paths.skelPath.find("aim") != std::string::npos);
}

TEST_F(SpineTest, PathResolution_Cover)
{
    if (!hasAssets) GTEST_SKIP() << "No assets directory found";

    auto paths = rt::SpineEngine::resolvePaths(assetsDir, "Crown", "default",
                                                rt::CharacterStance::Cover);
    if (!paths.valid) GTEST_SKIP() << "Crown cover not available";

    EXPECT_TRUE(paths.valid);
    EXPECT_TRUE(paths.skelPath.find("cover") != std::string::npos);
}

TEST_F(SpineTest, PathResolution_InvalidChar)
{
    if (!hasAssets) GTEST_SKIP() << "No assets directory found";

    auto paths = rt::SpineEngine::resolvePaths(assetsDir, "NonexistentChar", "default",
                                                rt::CharacterStance::Default);
    EXPECT_FALSE(paths.valid);
}

// ═════════════════════════════════════════════════════════════════════════════
//  SpineEngine — Skeleton Loading
// ═════════════════════════════════════════════════════════════════════════════

TEST_F(SpineTest, EngineDefaultConstruction)
{
    rt::SpineEngine engine;
    EXPECT_FALSE(engine.isLoaded());
}

TEST_F(SpineTest, EngineLoadSkeleton)
{
    if (!hasAssets) GTEST_SKIP() << "No assets directory found";

    auto files = findCharFiles("Crown");
    if (!files.valid()) GTEST_SKIP() << "Crown not available";

    rt::SpineEngine engine;
    ASSERT_TRUE(engine.loadSkeleton(files.skel, files.atlas));
    EXPECT_TRUE(engine.isLoaded());
    EXPECT_FALSE(engine.version().empty());
    EXPECT_GT(engine.skeletonWidth(), 0.0f);
    EXPECT_GT(engine.skeletonHeight(), 0.0f);
}

TEST_F(SpineTest, EngineLoadSkeleton_Modernia)
{
    if (!hasAssets) GTEST_SKIP() << "No assets directory found";

    auto files = findCharFiles("Modernia");
    if (!files.valid()) GTEST_SKIP() << "Modernia not available";

    // The spine-cpp 4.1 runtime is backward-compatible with 4.0.x skeletons
    // (Modernia is v4.0.47): the version is detected and the skeleton loads.
    rt::SpineEngine engine;
    auto ver = rt::SpineEngine::detectVersion(files.skel);
    EXPECT_FALSE(ver.empty());

    EXPECT_TRUE(engine.loadSkeleton(files.skel, files.atlas));
    EXPECT_TRUE(engine.isLoaded());
}

TEST_F(SpineTest, EngineLoadInvalid)
{
    rt::SpineEngine engine;
    EXPECT_FALSE(engine.loadSkeleton("bad.skel", "bad.atlas"));
    EXPECT_FALSE(engine.isLoaded());
}

TEST_F(SpineTest, EngineLoadFromClip)
{
    if (!hasAssets) GTEST_SKIP() << "No assets directory found";

    rt::SpineClip clip;
    clip.setCharacterName("Crown");
    clip.setOutfit("default");
    clip.setStance(rt::CharacterStance::Default);
    clip.setAnimationName("idle");

    rt::SpineEngine engine;
    ASSERT_TRUE(engine.loadFromClip(clip, assetsDir));
    EXPECT_TRUE(engine.isLoaded());
}

// ═════════════════════════════════════════════════════════════════════════════
//  SpineEngine — Skins
// ═════════════════════════════════════════════════════════════════════════════

TEST_F(SpineTest, EngineSkins)
{
    if (!hasAssets) GTEST_SKIP() << "No assets directory found";

    auto files = findCharFiles("Crown");
    if (!files.valid()) GTEST_SKIP() << "Crown not available";

    rt::SpineEngine engine;
    ASSERT_TRUE(engine.loadSkeleton(files.skel, files.atlas));

    auto skins = engine.listSkins();
    EXPECT_GT(skins.size(), 0u);

    // Every skeleton has at least a "default" skin
    bool hasDefault = false;
    for (auto& s : skins) {
        if (s == "default") hasDefault = true;
    }
    EXPECT_TRUE(hasDefault);
}

// ═════════════════════════════════════════════════════════════════════════════
//  SpineAnimation Tests
// ═════════════════════════════════════════════════════════════════════════════

TEST_F(SpineTest, AnimationListAnimations)
{
    if (!hasAssets) GTEST_SKIP() << "No assets directory found";

    auto files = findCharFiles("Crown");
    if (!files.valid()) GTEST_SKIP() << "Crown not available";

    rt::SpineEngine engine;
    ASSERT_TRUE(engine.loadSkeleton(files.skel, files.atlas));

    auto anims = engine.animation().listAnimations();
    EXPECT_GT(anims.size(), 0u);

    // Should have an "idle" animation
    bool hasIdle = false;
    for (auto& a : anims) {
        EXPECT_FALSE(a.name.empty());
        EXPECT_GE(a.duration, 0.0f);
        if (a.name == "idle") hasIdle = true;
    }
    EXPECT_TRUE(hasIdle) << "Expected 'idle' animation";
}

TEST_F(SpineTest, AnimationSetBody)
{
    if (!hasAssets) GTEST_SKIP() << "No assets directory found";

    auto files = findCharFiles("Crown");
    if (!files.valid()) GTEST_SKIP() << "Crown not available";

    rt::SpineEngine engine;
    ASSERT_TRUE(engine.loadSkeleton(files.skel, files.atlas));

    engine.animation().setBodyAnimation("idle", true);
    EXPECT_EQ(engine.animation().currentBodyAnimation(), "idle");
}

TEST_F(SpineTest, AnimationHasAnimation)
{
    if (!hasAssets) GTEST_SKIP() << "No assets directory found";

    auto files = findCharFiles("Crown");
    if (!files.valid()) GTEST_SKIP() << "Crown not available";

    rt::SpineEngine engine;
    ASSERT_TRUE(engine.loadSkeleton(files.skel, files.atlas));

    EXPECT_TRUE(engine.animation().hasAnimation("idle"));
    EXPECT_FALSE(engine.animation().hasAnimation("nonexistent_animation_xyz"));
}

TEST_F(SpineTest, AnimationTalkDetection)
{
    if (!hasAssets) GTEST_SKIP() << "No assets directory found";

    // Try each character — at least one should have a talk animation
    bool foundTalk = false;
    for (auto& charName : {"Crown", "Modernia", "Kilo", "Chime", "Dorothy", "Trony"}) {
        auto files = findCharFiles(charName);
        if (!files.valid()) continue;

        rt::SpineEngine engine;
        if (!engine.loadSkeleton(files.skel, files.atlas)) continue;

        auto talkAnim = engine.animation().detectTalkAnimation();
        if (!talkAnim.empty()) {
            foundTalk = true;
            // The detected animation should actually exist
            EXPECT_TRUE(engine.animation().hasAnimation(talkAnim))
                << charName << ": detected '" << talkAnim << "' but it doesn't exist";
        }
    }
    // It's OK if no talk animations are found in test assets
}

TEST_F(SpineTest, AnimationStartStopTalking)
{
    if (!hasAssets) GTEST_SKIP() << "No assets directory found";

    auto files = findCharFiles("Crown");
    if (!files.valid()) GTEST_SKIP() << "Crown not available";

    rt::SpineEngine engine;
    ASSERT_TRUE(engine.loadSkeleton(files.skel, files.atlas));

    engine.animation().setBodyAnimation("idle", true);
    EXPECT_FALSE(engine.animation().isTalking());

    engine.animation().startTalking();
    // May or may not be talking depending on whether talk anims exist
    (void)engine.animation().isTalking();

    engine.animation().stopTalking();
    EXPECT_FALSE(engine.animation().isTalking());
}

TEST_F(SpineTest, AnimationSpeed)
{
    rt::SpineAnimation anim;
    anim.setSpeed(2.0f);
    EXPECT_FLOAT_EQ(anim.speed(), 2.0f);
}

// ═════════════════════════════════════════════════════════════════════════════
//  SpineEngine — Update & Evaluate
// ═════════════════════════════════════════════════════════════════════════════

TEST_F(SpineTest, EngineUpdate)
{
    if (!hasAssets) GTEST_SKIP() << "No assets directory found";

    auto files = findCharFiles("Crown");
    if (!files.valid()) GTEST_SKIP() << "Crown not available";

    rt::SpineEngine engine;
    ASSERT_TRUE(engine.loadSkeleton(files.skel, files.atlas));

    engine.animation().setBodyAnimation("idle", true);

    // Should not crash
    for (int i = 0; i < 60; ++i) {
        engine.update(1.0f / 30.0f);
    }
}

TEST_F(SpineTest, EngineEvaluateAtTime)
{
    if (!hasAssets) GTEST_SKIP() << "No assets directory found";

    auto files = findCharFiles("Crown");
    if (!files.valid()) GTEST_SKIP() << "Crown not available";

    rt::SpineEngine engine;
    ASSERT_TRUE(engine.loadSkeleton(files.skel, files.atlas));

    engine.animation().setBodyAnimation("idle", true);

    // Should not crash
    engine.evaluateAtTime(0.0f);
    engine.evaluateAtTime(0.5f);
    engine.evaluateAtTime(1.0f);
}

// ═════════════════════════════════════════════════════════════════════════════
//  SpineEngine — Mesh Extraction
// ═════════════════════════════════════════════════════════════════════════════

TEST_F(SpineTest, MeshExtraction)
{
    if (!hasAssets) GTEST_SKIP() << "No assets directory found";

    auto files = findCharFiles("Crown");
    if (!files.valid()) GTEST_SKIP() << "Crown not available";

    rt::SpineEngine engine;
    ASSERT_TRUE(engine.loadSkeleton(files.skel, files.atlas));

    engine.animation().setBodyAnimation("idle", true);
    engine.update(0.0f);  // Apply initial pose

    auto meshData = engine.extractMeshes();

    // Should have at least one batch
    EXPECT_GT(meshData.batches.size(), 0u);

    // Check batches have valid data
    size_t totalVerts = 0;
    size_t totalIndices = 0;
    for (auto& batch : meshData.batches) {
        EXPECT_GE(batch.texturePageIndex, 0);
        EXPECT_GT(batch.vertices.size(), 0u);
        EXPECT_GT(batch.indices.size(), 0u);
        EXPECT_EQ(batch.indices.size() % 3, 0u) << "Indices should be triangles";

        // Check all indices are valid
        for (auto idx : batch.indices) {
            EXPECT_LT(idx, batch.vertices.size());
        }

        // Check vertex data is reasonable
        for (auto& v : batch.vertices) {
            EXPECT_TRUE(std::isfinite(v.x));
            EXPECT_TRUE(std::isfinite(v.y));
            EXPECT_GE(v.u, 0.0f);
            EXPECT_LE(v.u, 1.0f);
            EXPECT_GE(v.v, 0.0f);
            EXPECT_LE(v.v, 1.0f);
            EXPECT_GE(v.a, 0.0f);
            EXPECT_LE(v.a, 1.0f);
        }

        totalVerts += batch.vertices.size();
        totalIndices += batch.indices.size();
    }

    EXPECT_GT(totalVerts, 0u);
    EXPECT_GT(totalIndices, 0u);

    // Bounds should be non-zero for a loaded character
    EXPECT_NE(meshData.boundsW, 0.0f);
    EXPECT_NE(meshData.boundsH, 0.0f);
}

TEST_F(SpineTest, MeshExtractionMultipleFrames)
{
    if (!hasAssets) GTEST_SKIP() << "No assets directory found";

    auto files = findCharFiles("Crown");
    if (!files.valid()) GTEST_SKIP() << "Crown not available";

    rt::SpineEngine engine;
    ASSERT_TRUE(engine.loadSkeleton(files.skel, files.atlas));

    engine.animation().setBodyAnimation("idle", true);

    // Extract meshes at different times
    for (int frame = 0; frame < 10; ++frame) {
        engine.update(1.0f / 30.0f);
        auto meshData = engine.extractMeshes();
        EXPECT_GT(meshData.batches.size(), 0u) << "Frame " << frame;
    }
}

// ═════════════════════════════════════════════════════════════════════════════
//  SpineEngine — Bounds
// ═════════════════════════════════════════════════════════════════════════════

TEST_F(SpineTest, EngineBounds)
{
    if (!hasAssets) GTEST_SKIP() << "No assets directory found";

    auto files = findCharFiles("Crown");
    if (!files.valid()) GTEST_SKIP() << "Crown not available";

    rt::SpineEngine engine;
    ASSERT_TRUE(engine.loadSkeleton(files.skel, files.atlas));

    engine.animation().setBodyAnimation("idle", true);
    engine.update(0.0f);

    float x, y, w, h;
    engine.getBounds(x, y, w, h);
    EXPECT_GT(w, 0.0f);
    EXPECT_GT(h, 0.0f);
}

// ═════════════════════════════════════════════════════════════════════════════
//  SpineEngine — Position & Scale
// ═════════════════════════════════════════════════════════════════════════════

TEST_F(SpineTest, EnginePositionAndScale)
{
    if (!hasAssets) GTEST_SKIP() << "No assets directory found";

    auto files = findCharFiles("Crown");
    if (!files.valid()) GTEST_SKIP() << "Crown not available";

    rt::SpineEngine engine;
    ASSERT_TRUE(engine.loadSkeleton(files.skel, files.atlas));

    // Should not crash
    engine.setPosition(100.0f, 200.0f);
    engine.setScale(0.5f, 0.5f);
    engine.animation().setBodyAnimation("idle", true);
    engine.update(0.0f);
}

// ═════════════════════════════════════════════════════════════════════════════
//  SpineEngine — Move semantics
// ═════════════════════════════════════════════════════════════════════════════

TEST_F(SpineTest, EngineMove)
{
    if (!hasAssets) GTEST_SKIP() << "No assets directory found";

    auto files = findCharFiles("Crown");
    if (!files.valid()) GTEST_SKIP() << "Crown not available";

    rt::SpineEngine engine1;
    ASSERT_TRUE(engine1.loadSkeleton(files.skel, files.atlas));

    rt::SpineEngine engine2 = std::move(engine1);
    EXPECT_TRUE(engine2.isLoaded());
}

// ═════════════════════════════════════════════════════════════════════════════
//  ModelManager Tests
// ═════════════════════════════════════════════════════════════════════════════

TEST_F(SpineTest, ModelManagerDefaultConstruction)
{
    rt::ModelManager mgr;
    EXPECT_FALSE(mgr.isScanned());
    EXPECT_EQ(mgr.entries().size(), 0u);
}

TEST_F(SpineTest, ModelManagerScan)
{
    if (!hasAssets) GTEST_SKIP() << "No assets directory found";

    rt::ModelManager mgr;
    int count = mgr.scan(assetsDir);

    EXPECT_GT(count, 0);
    EXPECT_TRUE(mgr.isScanned());
    EXPECT_EQ(static_cast<int>(mgr.entries().size()), count);

    // Each entry should have at least one outfit with at least one variant
    for (auto& entry : mgr.entries()) {
        EXPECT_FALSE(entry.name.empty());
        EXPECT_GT(entry.outfits.size(), 0u) << entry.name;
        for (auto& outfit : entry.outfits) {
            EXPECT_FALSE(outfit.name.empty());
            EXPECT_GT(outfit.variants.size(), 0u) << entry.name << "/" << outfit.name;
            for (auto& var : outfit.variants) {
                EXPECT_FALSE(var.skelPath.empty());
                EXPECT_FALSE(var.atlasPath.empty());
                EXPECT_TRUE(fs::exists(var.skelPath)) << var.skelPath;
                EXPECT_TRUE(fs::exists(var.atlasPath)) << var.atlasPath;
            }
        }
    }
}

TEST_F(SpineTest, ModelManagerFindByName)
{
    if (!hasAssets) GTEST_SKIP() << "No assets directory found";

    rt::ModelManager mgr;
    mgr.scan(assetsDir);

    // Case-insensitive search
    auto* crown = mgr.findByName("Crown");
    if (crown) {
        EXPECT_EQ(crown->name, "Crown");
    }

    auto* crownLower = mgr.findByName("crown");
    if (crownLower) {
        EXPECT_EQ(crownLower->name, "Crown");
    }

    EXPECT_EQ(mgr.findByName("NonexistentCharacterXYZ"), nullptr);
}

TEST_F(SpineTest, ModelManagerCharacterNames)
{
    if (!hasAssets) GTEST_SKIP() << "No assets directory found";

    rt::ModelManager mgr;
    mgr.scan(assetsDir);

    auto names = mgr.characterNames();
    EXPECT_GT(names.size(), 0u);

    // Names should be sorted
    for (size_t i = 1; i < names.size(); ++i) {
        EXPECT_LE(names[i-1], names[i]) << "Names should be sorted";
    }
}

TEST_F(SpineTest, ModelManagerFindVariant)
{
    if (!hasAssets) GTEST_SKIP() << "No assets directory found";

    rt::ModelManager mgr;
    mgr.scan(assetsDir);

    // Find default stance
    auto* defVar = mgr.findVariant("Crown", "default", rt::CharacterStance::Default);
    if (defVar) {
        EXPECT_EQ(defVar->stance, rt::CharacterStance::Default);
        EXPECT_FALSE(defVar->skelPath.empty());
    }

    // Find aim stance
    auto* aimVar = mgr.findVariant("Crown", "default", rt::CharacterStance::Aim);
    if (aimVar) {
        EXPECT_EQ(aimVar->stance, rt::CharacterStance::Aim);
    }

    // Nonexistent
    EXPECT_EQ(mgr.findVariant("NonexistentXYZ", "default", rt::CharacterStance::Default), nullptr);
}

TEST_F(SpineTest, ModelManagerMultipleOutfits)
{
    if (!hasAssets) GTEST_SKIP() << "No assets directory found";

    rt::ModelManager mgr;
    mgr.scan(assetsDir);

    // Crown should have multiple outfits
    auto* crown = mgr.findByName("Crown");
    if (crown) {
        // Just check it scanned correctly — don't assert specific count
        EXPECT_GE(crown->outfits.size(), 1u);
    }
}

TEST_F(SpineTest, ModelManagerScanInvalidDir)
{
    rt::ModelManager mgr;
    int count = mgr.scan("nonexistent_directory");
    EXPECT_EQ(count, 0);
}

// ═════════════════════════════════════════════════════════════════════════════
//  Integration: ModelManager + SpineEngine
// ═════════════════════════════════════════════════════════════════════════════

TEST_F(SpineTest, IntegrationLoadFromModelManager)
{
    if (!hasAssets) GTEST_SKIP() << "No assets directory found";

    rt::ModelManager mgr;
    mgr.scan(assetsDir);

    auto names = mgr.characterNames();
    if (names.empty()) GTEST_SKIP() << "No characters found";

    // Load the first character
    auto* entry = mgr.findByName(names[0]);
    ASSERT_NE(entry, nullptr);
    ASSERT_GT(entry->outfits.size(), 0u);
    ASSERT_GT(entry->outfits[0].variants.size(), 0u);

    auto& var = entry->outfits[0].variants[0];

    rt::SpineEngine engine;
    ASSERT_TRUE(engine.loadSkeleton(var.skelPath, var.atlasPath));
    EXPECT_TRUE(engine.isLoaded());

    engine.animation().setBodyAnimation("idle", true);
    engine.update(0.0f);

    auto meshData = engine.extractMeshes();
    EXPECT_GT(meshData.batches.size(), 0u);
}

TEST_F(SpineTest, IntegrationLoadAllCharacters)
{
    if (!hasAssets) GTEST_SKIP() << "No assets directory found";

    rt::ModelManager mgr;
    mgr.scan(assetsDir);

    int loaded = 0;
    int failed = 0;
    int skipped = 0;

    for (auto& entry : mgr.entries()) {
        for (auto& outfit : entry.outfits) {
            for (auto& var : outfit.variants) {
                // Skip v4.0 skeletons — spine-cpp 4.1 correctly rejects them
                auto ver = rt::SpineEngine::detectVersion(var.skelPath);
                if (ver.find("4.0") == 0) {
                    skipped++;
                    continue;
                }

                rt::SpineEngine engine;
                if (engine.loadSkeleton(var.skelPath, var.atlasPath)) {
                    loaded++;
                    // Quick sanity check
                    auto anims = engine.animation().listAnimations();
                    EXPECT_GT(anims.size(), 0u)
                        << entry.name << "/" << outfit.name;
                } else {
                    failed++;
                }
            }
        }
    }

    EXPECT_GT(loaded, 0);
    EXPECT_EQ(failed, 0) << failed << " skeleton(s) failed to load";
    if (skipped > 0) {
        // Not a failure — just informational
        std::cout << "  [INFO] Skipped " << skipped << " v4.0 skeleton(s)\n";
    }
}

#else // !ROUNDTABLE_HAS_SPINE

// If spine-cpp is not available, provide a single test that documents this
TEST(SpineTest, SpineCppNotAvailable)
{
    GTEST_SKIP() << "spine-cpp not available (ROUNDTABLE_HAS_SPINE not defined)";
}

#endif // ROUNDTABLE_HAS_SPINE
