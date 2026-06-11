/*
 * test_shot_composer.cpp — tests for ShotPreset and ShotComposer.
 * Step 18: Shot Composer
 *
 * Tests cover:
 *   1. ShotPreset data model (backgrounds, characters, layer ordering, serialization)
 *   2. ShotPresetManager (save/load/delete presets, directory scanning)
 *   3. ShotComposer UI panel (widget existence, character/bg management,
 *      layer ordering, property binding, signals)
 */

#include <gtest/gtest.h>

#include "spine/ShotPreset.h"
#include "panels/characters/ShotComposer.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSignalSpy>
#include <QTest>

#include <filesystem>
#include <fstream>

static int    s_argc = 1;
static char   s_arg0[] = "test_shot_composer";
static char*  s_argv[] = { s_arg0, nullptr };

namespace {

/// Ensure a QApplication exists for the entire test process.
struct AppGuard {
    AppGuard() {
        if (!QApplication::instance())
            app = std::make_unique<QApplication>(s_argc, s_argv);
    }
    std::unique_ptr<QApplication> app;
};
static AppGuard s_guard;

/// Create a temporary directory for preset tests.
class TempDir {
public:
    TempDir() {
        m_path = std::filesystem::temp_directory_path() / "rt_test_presets";
        std::filesystem::create_directories(m_path);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(m_path, ec);
    }
    const std::filesystem::path& path() const { return m_path; }
private:
    std::filesystem::path m_path;
};

} // anonymous

using namespace rt;

// ═════════════════════════════════════════════════════════════════════════════
// ShotPreset — Data Model Tests
// ═════════════════════════════════════════════════════════════════════════════

class ShotPresetTest : public ::testing::Test {};

TEST_F(ShotPresetTest, DefaultConstruction)
{
    ShotPreset p;
    EXPECT_TRUE(p.name().empty());
    EXPECT_EQ(p.characterCount(), 0);
    EXPECT_EQ(p.backgroundCount(), 0);
    EXPECT_EQ(p.layerCount(), 0);
    EXPECT_FLOAT_EQ(p.cameraZoom(), 1.0f);
    EXPECT_FLOAT_EQ(p.cameraX(), 0.0f);
    EXPECT_FLOAT_EQ(p.cameraY(), 0.0f);
}

TEST_F(ShotPresetTest, NamedConstruction)
{
    ShotPreset p("Test Shot");
    EXPECT_EQ(p.name(), "Test Shot");
}

TEST_F(ShotPresetTest, AddCharacter)
{
    ShotPreset p("test");
    CharacterState ch;
    ch.characterName = "Modernia";
    ch.posX = 0.3f;

    int idx = p.addCharacter(ch);
    EXPECT_EQ(idx, 0);
    EXPECT_EQ(p.characterCount(), 1);
    EXPECT_EQ(p.layerCount(), 1);

    const auto* c = p.character(0);
    ASSERT_NE(c, nullptr);
    EXPECT_EQ(c->characterName, "Modernia");
    EXPECT_FLOAT_EQ(c->posX, 0.3f);
}

TEST_F(ShotPresetTest, AddMultipleCharacters)
{
    ShotPreset p;
    CharacterState c1;
    c1.characterName = "Rapi";
    CharacterState c2;
    c2.characterName = "Modernia";
    CharacterState c3;
    c3.characterName = "Dorothy";

    p.addCharacter(c1);
    p.addCharacter(c2);
    p.addCharacter(c3);

    EXPECT_EQ(p.characterCount(), 3);
    EXPECT_EQ(p.layerCount(), 3);
    EXPECT_EQ(p.character(0)->characterName, "Rapi");
    EXPECT_EQ(p.character(1)->characterName, "Modernia");
    EXPECT_EQ(p.character(2)->characterName, "Dorothy");
}

TEST_F(ShotPresetTest, RemoveCharacter)
{
    ShotPreset p;
    CharacterState c1, c2;
    c1.characterName = "A";
    c2.characterName = "B";
    p.addCharacter(c1);
    p.addCharacter(c2);

    EXPECT_TRUE(p.removeCharacter(0));
    EXPECT_EQ(p.characterCount(), 1);
    EXPECT_EQ(p.character(0)->characterName, "B");
    EXPECT_EQ(p.layerCount(), 1);
}

TEST_F(ShotPresetTest, RemoveCharacterInvalidIndex)
{
    ShotPreset p;
    EXPECT_FALSE(p.removeCharacter(0));
    EXPECT_FALSE(p.removeCharacter(-1));
}

TEST_F(ShotPresetTest, AddBackground)
{
    ShotPreset p;
    BackgroundState bg;
    bg.path = "backgrounds/city.png";
    bg.scale = 1.5f;

    int idx = p.addBackground(bg);
    EXPECT_EQ(idx, 0);
    EXPECT_EQ(p.backgroundCount(), 1);
    EXPECT_EQ(p.layerCount(), 1);

    const auto* b = p.background(0);
    ASSERT_NE(b, nullptr);
    EXPECT_EQ(b->path, "backgrounds/city.png");
    EXPECT_FLOAT_EQ(b->scale, 1.5f);
}

TEST_F(ShotPresetTest, RemoveBackground)
{
    ShotPreset p;
    BackgroundState bg1, bg2;
    bg1.path = "a.png";
    bg2.path = "b.png";
    p.addBackground(bg1);
    p.addBackground(bg2);

    EXPECT_TRUE(p.removeBackground(0));
    EXPECT_EQ(p.backgroundCount(), 1);
    EXPECT_EQ(p.background(0)->path, "b.png");
}

TEST_F(ShotPresetTest, MixedLayers)
{
    ShotPreset p;
    BackgroundState bg;
    bg.path = "bg.png";
    p.addBackground(bg);

    CharacterState ch;
    ch.characterName = "Rapi";
    p.addCharacter(ch);

    EXPECT_EQ(p.layerCount(), 2);
    EXPECT_EQ(p.backgroundCount(), 1);
    EXPECT_EQ(p.characterCount(), 1);

    // First layer should be background, second character
    const auto& order = p.layerOrder();
    EXPECT_EQ(order[0].type, LayerType::Background);
    EXPECT_EQ(order[0].index, 0);
    EXPECT_EQ(order[1].type, LayerType::Character);
    EXPECT_EQ(order[1].index, 0);
}

TEST_F(ShotPresetTest, SwapLayers)
{
    ShotPreset p;
    BackgroundState bg; bg.path = "bg";
    CharacterState ch;  ch.characterName = "ch";
    p.addBackground(bg);
    p.addCharacter(ch);

    EXPECT_TRUE(p.swapLayers(0, 1));
    const auto& order = p.layerOrder();
    EXPECT_EQ(order[0].type, LayerType::Character);
    EXPECT_EQ(order[1].type, LayerType::Background);
}

TEST_F(ShotPresetTest, MoveLayerUp)
{
    ShotPreset p;
    CharacterState c1, c2, c3;
    c1.characterName = "A";
    c2.characterName = "B";
    c3.characterName = "C";
    p.addCharacter(c1);
    p.addCharacter(c2);
    p.addCharacter(c3);

    // Move C (index 2) up
    EXPECT_TRUE(p.moveLayerUp(2));
    EXPECT_EQ(p.layerOrder()[1].index, 2); // C is now at position 1

    // Can't move first layer up
    EXPECT_FALSE(p.moveLayerUp(0));
}

TEST_F(ShotPresetTest, MoveLayerDown)
{
    ShotPreset p;
    CharacterState c1, c2;
    c1.characterName = "A";
    c2.characterName = "B";
    p.addCharacter(c1);
    p.addCharacter(c2);

    EXPECT_TRUE(p.moveLayerDown(0));
    EXPECT_EQ(p.layerOrder()[0].index, 1); // B is now first

    // Can't move last layer down
    EXPECT_FALSE(p.moveLayerDown(1));
}

TEST_F(ShotPresetTest, MoveLayerToFront)
{
    ShotPreset p;
    CharacterState c1, c2, c3;
    c1.characterName = "A";
    c2.characterName = "B";
    c3.characterName = "C";
    p.addCharacter(c1);
    p.addCharacter(c2);
    p.addCharacter(c3);

    EXPECT_TRUE(p.moveLayerToFront(2));  // Move C to front
    EXPECT_EQ(p.layerOrder()[0].index, 2);
}

TEST_F(ShotPresetTest, MoveLayerToBack)
{
    ShotPreset p;
    CharacterState c1, c2, c3;
    c1.characterName = "A";
    c2.characterName = "B";
    c3.characterName = "C";
    p.addCharacter(c1);
    p.addCharacter(c2);
    p.addCharacter(c3);

    EXPECT_TRUE(p.moveLayerToBack(0));  // Move A to back
    EXPECT_EQ(p.layerOrder()[2].index, 0);
}

TEST_F(ShotPresetTest, FindLayerIndex)
{
    ShotPreset p;
    BackgroundState bg; bg.path = "bg";
    CharacterState ch;  ch.characterName = "ch";
    p.addBackground(bg);
    p.addCharacter(ch);

    EXPECT_EQ(p.findLayerIndex({LayerType::Background, 0}), 0);
    EXPECT_EQ(p.findLayerIndex({LayerType::Character, 0}), 1);
    EXPECT_EQ(p.findLayerIndex({LayerType::Character, 5}), -1);
}

TEST_F(ShotPresetTest, Camera)
{
    ShotPreset p;
    p.setCameraZoom(2.0f);
    p.setCameraX(0.5f);
    p.setCameraY(-0.3f);

    EXPECT_FLOAT_EQ(p.cameraZoom(), 2.0f);
    EXPECT_FLOAT_EQ(p.cameraX(), 0.5f);
    EXPECT_FLOAT_EQ(p.cameraY(), -0.3f);
}

TEST_F(ShotPresetTest, CreateDefault)
{
    auto p = ShotPreset::createDefault("Rapi");
    EXPECT_EQ(p.name(), "Rapi - Default");
    EXPECT_EQ(p.characterCount(), 1);
    EXPECT_EQ(p.character(0)->characterName, "Rapi");
    EXPECT_FLOAT_EQ(p.character(0)->posX, 0.5f);
}

TEST_F(ShotPresetTest, NullAccessors)
{
    ShotPreset p;
    EXPECT_EQ(p.character(0), nullptr);
    EXPECT_EQ(p.character(-1), nullptr);
    EXPECT_EQ(p.background(0), nullptr);
    EXPECT_EQ(p.background(-1), nullptr);
}

TEST_F(ShotPresetTest, MutableCharacterAccess)
{
    ShotPreset p;
    CharacterState ch;
    ch.characterName = "Rapi";
    ch.posX = 0.5f;
    p.addCharacter(ch);

    auto* c = p.character(0);
    ASSERT_NE(c, nullptr);
    c->posX = 0.3f;
    EXPECT_FLOAT_EQ(p.character(0)->posX, 0.3f);
}

TEST_F(ShotPresetTest, CharacterStateDefaults)
{
    CharacterState ch;
    EXPECT_EQ(ch.outfit, "default");
    EXPECT_EQ(ch.stance, CharacterStance::Default);
    EXPECT_EQ(ch.animation, "idle");
    EXPECT_FALSE(ch.isTalking);
    EXPECT_FLOAT_EQ(ch.posX, 0.5f);
    EXPECT_FLOAT_EQ(ch.posY, 0.75f);
    EXPECT_FLOAT_EQ(ch.scale, 1.0f);
    EXPECT_FALSE(ch.flipX);
    EXPECT_FLOAT_EQ(ch.opacity, 1.0f);
    EXPECT_TRUE(ch.visible);
}

TEST_F(ShotPresetTest, BackgroundStateDefaults)
{
    BackgroundState bg;
    EXPECT_FLOAT_EQ(bg.posX, 0.5f);
    EXPECT_FLOAT_EQ(bg.posY, 0.5f);
    EXPECT_FLOAT_EQ(bg.scale, 1.0f);
    EXPECT_FLOAT_EQ(bg.opacity, 1.0f);
    EXPECT_TRUE(bg.visible);
}

// ═════════════════════════════════════════════════════════════════════════════
// ShotPreset — JSON Serialization Tests
// ═════════════════════════════════════════════════════════════════════════════

class ShotPresetSerializationTest : public ::testing::Test {};

TEST_F(ShotPresetSerializationTest, RoundTripEmpty)
{
    ShotPreset original("Empty Shot");
    auto json = original.toJson();
    EXPECT_FALSE(json.empty());

    auto loaded = ShotPreset::fromJson(json);
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->name(), "Empty Shot");
    EXPECT_EQ(loaded->characterCount(), 0);
    EXPECT_EQ(loaded->backgroundCount(), 0);
}

TEST_F(ShotPresetSerializationTest, RoundTripWithCharacter)
{
    ShotPreset original("Character Shot");
    CharacterState ch;
    ch.characterName = "Modernia";
    ch.outfit = "outfit_02";
    ch.stance = CharacterStance::Aim;
    ch.animation = "idle_02";
    ch.isTalking = true;
    ch.posX = 0.3f;
    ch.posY = 0.8f;
    ch.scale = 1.5f;
    ch.flipX = true;
    ch.opacity = 0.9f;
    ch.cropLeft = 0.1f;
    ch.cropRight = 0.2f;
    ch.visible = false;
    original.addCharacter(ch);

    auto json = original.toJson();
    auto loaded = ShotPreset::fromJson(json);
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->characterCount(), 1);

    const auto* c = loaded->character(0);
    ASSERT_NE(c, nullptr);
    EXPECT_EQ(c->characterName, "Modernia");
    EXPECT_EQ(c->outfit, "outfit_02");
    EXPECT_EQ(c->stance, CharacterStance::Aim);
    EXPECT_EQ(c->animation, "idle_02");
    EXPECT_TRUE(c->isTalking);
    EXPECT_NEAR(c->posX, 0.3f, 0.001f);
    EXPECT_NEAR(c->posY, 0.8f, 0.001f);
    EXPECT_NEAR(c->scale, 1.5f, 0.001f);
    EXPECT_TRUE(c->flipX);
    EXPECT_NEAR(c->opacity, 0.9f, 0.001f);
    EXPECT_NEAR(c->cropLeft, 0.1f, 0.001f);
    EXPECT_FALSE(c->visible);
}

TEST_F(ShotPresetSerializationTest, RoundTripWithBackground)
{
    ShotPreset original("BG Shot");
    BackgroundState bg;
    bg.path = "backgrounds/sunset.png";
    bg.posX = 0.4f;
    bg.scale = 2.0f;
    bg.opacity = 0.7f;
    bg.nativeWidth = 1920;
    bg.nativeHeight = 1080;
    bg.visible = false;
    original.addBackground(bg);

    auto json = original.toJson();
    auto loaded = ShotPreset::fromJson(json);
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->backgroundCount(), 1);

    const auto* b = loaded->background(0);
    ASSERT_NE(b, nullptr);
    EXPECT_EQ(b->path, "backgrounds/sunset.png");
    EXPECT_NEAR(b->posX, 0.4f, 0.001f);
    EXPECT_NEAR(b->scale, 2.0f, 0.001f);
    EXPECT_NEAR(b->opacity, 0.7f, 0.001f);
    EXPECT_EQ(b->nativeWidth, 1920);
    EXPECT_FALSE(b->visible);
}

TEST_F(ShotPresetSerializationTest, RoundTripMixed)
{
    ShotPreset original("Mixed");
    original.setCameraZoom(1.5f);
    original.setCameraX(0.2f);
    original.setCameraY(-0.1f);

    BackgroundState bg;
    bg.path = "bg.png";
    original.addBackground(bg);

    CharacterState c1, c2;
    c1.characterName = "A";
    c2.characterName = "B";
    original.addCharacter(c1);
    original.addCharacter(c2);

    // Reorder: ch_B, bg, ch_A
    original.moveLayerToFront(2); // B to front

    auto json = original.toJson();
    auto loaded = ShotPreset::fromJson(json);
    ASSERT_TRUE(loaded.has_value());

    EXPECT_NEAR(loaded->cameraZoom(), 1.5f, 0.001f);
    EXPECT_EQ(loaded->layerCount(), 3);
    EXPECT_EQ(loaded->layerOrder()[0].type, LayerType::Character);
    EXPECT_EQ(loaded->layerOrder()[0].index, 1); // B
}

TEST_F(ShotPresetSerializationTest, InvalidJsonReturnsNullopt)
{
    EXPECT_FALSE(ShotPreset::fromJson("").has_value());
    EXPECT_FALSE(ShotPreset::fromJson("not json").has_value());
    EXPECT_FALSE(ShotPreset::fromJson("[1,2,3]").has_value());
}

TEST_F(ShotPresetSerializationTest, SpecialCharactersInName)
{
    ShotPreset p("Shot \"with\" special\nchars");
    auto json = p.toJson();
    auto loaded = ShotPreset::fromJson(json);
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->name(), "Shot \"with\" special\nchars");
}

TEST_F(ShotPresetSerializationTest, ShowRoundTrip)
{
    ShotPreset original("Show Shot");
    original.setShow("Roundtable Talk");   // a shot belongs to one show
    CharacterState ch;
    ch.characterName = "Chime";
    original.addCharacter(ch);

    auto json = original.toJson();
    auto loaded = ShotPreset::fromJson(json);
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->show(), "Roundtable Talk");
    EXPECT_TRUE(loaded->hasShow("roundtable talk"));    // case-insensitive
    EXPECT_FALSE(loaded->hasShow("Kingdom Connection"));
}

TEST_F(ShotPresetSerializationTest, LegacyMultiShowMigratesToFirst)
{
    // Old files used a "shows" array; the first tag becomes the single show.
    const std::string legacy =
        R"({"name":"Legacy","shows":["Kingdom Connection","Roundtable Talk"]})";
    auto loaded = ShotPreset::fromJson(legacy);
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->show(), "Kingdom Connection");
}

TEST_F(ShotPresetSerializationTest, NoShowRoundTrip)
{
    ShotPreset original("Plain Shot");
    auto json = original.toJson();
    auto loaded = ShotPreset::fromJson(json);
    ASSERT_TRUE(loaded.has_value());
    EXPECT_TRUE(loaded->show().empty());
    EXPECT_FALSE(loaded->hasShow("Anything"));
}

TEST_F(ShotPresetSerializationTest, ShowNamespaceUnique)
{
    // The same name in two shows are distinct shots (per-show namespace).
    namespace fs = std::filesystem;
    fs::path dir = fs::temp_directory_path() / "rt_show_ns_test";
    fs::remove_all(dir);
    fs::create_directories(dir);

    ShotPresetManager mgr;
    mgr.scan(dir);

    ShotPreset a("CROWN (Default)");
    a.setShow("Roundtable Talk");
    ShotPreset b("CROWN (Default)");
    b.setShow("Kingdom Connection");
    EXPECT_TRUE(mgr.save(a));
    EXPECT_TRUE(mgr.save(b));

    // Two distinct presets despite the identical bare name.
    EXPECT_TRUE(mgr.hasPreset("Roundtable Talk", "CROWN (Default)"));
    EXPECT_TRUE(mgr.hasPreset("Kingdom Connection", "CROWN (Default)"));
    EXPECT_EQ(mgr.presetCount(), 2);

    auto la = mgr.load("Roundtable Talk", "CROWN (Default)");
    auto lb = mgr.load("Kingdom Connection", "CROWN (Default)");
    ASSERT_TRUE(la.has_value());
    ASSERT_TRUE(lb.has_value());
    EXPECT_EQ(la->show(), "Roundtable Talk");
    EXPECT_EQ(lb->show(), "Kingdom Connection");

    // A fresh scan finds both (in their subdirectories).
    ShotPresetManager mgr2;
    mgr2.scan(dir);
    EXPECT_EQ(mgr2.presetCount(), 2);
    EXPECT_TRUE(mgr2.hasPreset("Roundtable Talk", "CROWN (Default)"));
    EXPECT_TRUE(mgr2.hasPreset("Kingdom Connection", "CROWN (Default)"));

    fs::remove_all(dir);
}

TEST_F(ShotPresetSerializationTest, StanceRoundTrip)
{
    ShotPreset p;
    CharacterState c1, c2, c3;
    c1.characterName = "A"; c1.stance = CharacterStance::Default;
    c2.characterName = "B"; c2.stance = CharacterStance::Aim;
    c3.characterName = "C"; c3.stance = CharacterStance::Cover;
    p.addCharacter(c1);
    p.addCharacter(c2);
    p.addCharacter(c3);

    auto json = p.toJson();
    auto loaded = ShotPreset::fromJson(json);
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->character(0)->stance, CharacterStance::Default);
    EXPECT_EQ(loaded->character(1)->stance, CharacterStance::Aim);
    EXPECT_EQ(loaded->character(2)->stance, CharacterStance::Cover);
}

// ═════════════════════════════════════════════════════════════════════════════
// ShotPresetManager Tests
// ═════════════════════════════════════════════════════════════════════════════

class ShotPresetManagerTest : public ::testing::Test {
protected:
    TempDir m_tmpDir;
};

TEST_F(ShotPresetManagerTest, ScanEmptyDir)
{
    ShotPresetManager mgr;
    int count = mgr.scan(m_tmpDir.path());
    EXPECT_EQ(count, 0);
    EXPECT_EQ(mgr.presetCount(), 0);
}

TEST_F(ShotPresetManagerTest, SaveAndLoad)
{
    ShotPresetManager mgr;
    mgr.scan(m_tmpDir.path());

    auto preset = ShotPreset::createDefault("Rapi");
    EXPECT_TRUE(mgr.save(preset));
    EXPECT_TRUE(mgr.hasPreset("Rapi - Default"));

    auto loaded = mgr.load("Rapi - Default");
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->name(), "Rapi - Default");
    EXPECT_EQ(loaded->characterCount(), 1);
}

TEST_F(ShotPresetManagerTest, SaveAndReload)
{
    // Save with one manager
    {
        ShotPresetManager mgr;
        mgr.scan(m_tmpDir.path());
        mgr.save(ShotPreset::createDefault("Rapi"));
        mgr.save(ShotPreset::createDefault("Modernia"));
    }

    // Load with a new manager
    ShotPresetManager mgr2;
    int count = mgr2.scan(m_tmpDir.path());
    EXPECT_EQ(count, 2);
    EXPECT_EQ(mgr2.presetCount(), 2);
}

TEST_F(ShotPresetManagerTest, Remove)
{
    ShotPresetManager mgr;
    mgr.scan(m_tmpDir.path());
    mgr.save(ShotPreset::createDefault("Rapi"));

    EXPECT_TRUE(mgr.hasPreset("Rapi - Default"));
    EXPECT_TRUE(mgr.remove("Rapi - Default"));
    EXPECT_FALSE(mgr.hasPreset("Rapi - Default"));
    EXPECT_EQ(mgr.presetCount(), 0);
}

TEST_F(ShotPresetManagerTest, RemoveNonexistent)
{
    ShotPresetManager mgr;
    mgr.scan(m_tmpDir.path());
    EXPECT_FALSE(mgr.remove("does not exist"));
}

TEST_F(ShotPresetManagerTest, PresetNames)
{
    ShotPresetManager mgr;
    mgr.scan(m_tmpDir.path());
    mgr.save(ShotPreset::createDefault("Rapi"));
    mgr.save(ShotPreset::createDefault("Modernia"));

    auto names = mgr.presetNames();
    EXPECT_EQ(names.size(), 2u);
}

TEST_F(ShotPresetManagerTest, OverwritePreset)
{
    ShotPresetManager mgr;
    mgr.scan(m_tmpDir.path());

    auto preset1 = ShotPreset::createDefault("Rapi");
    mgr.save(preset1);

    // Modify and re-save with same name
    auto preset2 = ShotPreset::createDefault("Rapi");
    preset2.setCameraZoom(2.0f);
    mgr.save(preset2);

    EXPECT_EQ(mgr.presetCount(), 1); // Should still be 1
    auto loaded = mgr.load("Rapi - Default");
    ASSERT_TRUE(loaded.has_value());
    EXPECT_NEAR(loaded->cameraZoom(), 2.0f, 0.001f);
}

TEST_F(ShotPresetManagerTest, ScanNonexistentDir)
{
    ShotPresetManager mgr;
    int count = mgr.scan(m_tmpDir.path() / "nonexistent");
    EXPECT_EQ(count, 0);
}

TEST_F(ShotPresetManagerTest, LoadNonexistent)
{
    ShotPresetManager mgr;
    mgr.scan(m_tmpDir.path());
    auto loaded = mgr.load("does not exist");
    EXPECT_FALSE(loaded.has_value());
}

// ═════════════════════════════════════════════════════════════════════════════
// ShotComposer — UI Panel Tests
// ═════════════════════════════════════════════════════════════════════════════

class ShotComposerUITest : public ::testing::Test {
protected:
    void SetUp() override {
        m_panel = std::make_unique<ShotComposer>();
        m_panel->show();
        QApplication::processEvents();
    }

    std::unique_ptr<ShotComposer> m_panel;
};

TEST_F(ShotComposerUITest, Construction)
{
    EXPECT_NE(m_panel.get(), nullptr);
}

TEST_F(ShotComposerUITest, SizeHint)
{
    auto hint = m_panel->sizeHint();
    EXPECT_GT(hint.width(), 0);
    EXPECT_GT(hint.height(), 0);
}

TEST_F(ShotComposerUITest, WidgetsExist)
{
    EXPECT_NE(m_panel->libraryTabs(), nullptr);
    EXPECT_NE(m_panel->shotList(), nullptr);
    EXPECT_NE(m_panel->shotCombo(), nullptr);
    EXPECT_NE(m_panel->characterLibrary(), nullptr);
    EXPECT_NE(m_panel->backgroundLibrary(), nullptr);
    EXPECT_NE(m_panel->previewArea(), nullptr);
    EXPECT_NE(m_panel->shotNameEdit(), nullptr);
    EXPECT_NE(m_panel->layerList(), nullptr);
    EXPECT_NE(m_panel->mainSplitter(), nullptr);
}

TEST_F(ShotComposerUITest, ButtonsExist)
{
    EXPECT_NE(m_panel->newShotButton(), nullptr);
    EXPECT_NE(m_panel->saveShotButton(), nullptr);
    EXPECT_NE(m_panel->deleteShotButton(), nullptr);
    EXPECT_NE(m_panel->addCharBtn(), nullptr);
    EXPECT_NE(m_panel->addBgBtn(), nullptr);
    EXPECT_NE(m_panel->removeLayerBtn(), nullptr);
    EXPECT_NE(m_panel->layerUpBtn(), nullptr);
    EXPECT_NE(m_panel->layerDownBtn(), nullptr);
}

TEST_F(ShotComposerUITest, PropertyWidgetsExist)
{
    // Character property widgets
    EXPECT_NE(m_panel->posXSpin(), nullptr);
    EXPECT_NE(m_panel->posYSpin(), nullptr);
    EXPECT_NE(m_panel->scaleSpin(), nullptr);
    EXPECT_NE(m_panel->opacitySpin(), nullptr);
    EXPECT_NE(m_panel->outfitCombo(), nullptr);
    EXPECT_NE(m_panel->stanceCombo(), nullptr);
    EXPECT_NE(m_panel->animCombo(), nullptr);
    EXPECT_NE(m_panel->talkingCheck(), nullptr);
    EXPECT_NE(m_panel->flipXCheck(), nullptr);
    EXPECT_NE(m_panel->visibleCheck(), nullptr);

    // Background property widgets
    EXPECT_NE(m_panel->bgPosXSpin(), nullptr);
    EXPECT_NE(m_panel->bgPosYSpin(), nullptr);
    EXPECT_NE(m_panel->bgScaleSpin(), nullptr);
    EXPECT_NE(m_panel->bgOpacitySpin(), nullptr);

    // Camera
    EXPECT_NE(m_panel->cameraZoomSpin(), nullptr);
}

TEST_F(ShotComposerUITest, PropertiesInitiallyHidden)
{
    EXPECT_FALSE(m_panel->charPropsGroup()->isVisible());
    EXPECT_FALSE(m_panel->bgPropsGroup()->isVisible());
}

TEST_F(ShotComposerUITest, NewShot)
{
    m_panel->newShot("Test Shot");
    EXPECT_EQ(m_panel->currentShot().name(), "Test Shot");
    EXPECT_EQ(m_panel->shotNameEdit()->text(), "Test Shot");
    EXPECT_EQ(m_panel->currentShot().characterCount(), 0);
}

TEST_F(ShotComposerUITest, AddCharacterToShot)
{
    m_panel->newShot("Test Shot");
    int idx = m_panel->addCharacter("Modernia");
    EXPECT_EQ(idx, 0);
    EXPECT_EQ(m_panel->currentShot().characterCount(), 1);
    EXPECT_EQ(m_panel->layerList()->count(), 1);
}

TEST_F(ShotComposerUITest, AddMultipleCharacters)
{
    m_panel->newShot("Test Shot");
    m_panel->addCharacter("Rapi");
    m_panel->addCharacter("Modernia");
    m_panel->addCharacter("Dorothy");

    EXPECT_EQ(m_panel->currentShot().characterCount(), 3);
    EXPECT_EQ(m_panel->layerList()->count(), 3);
}

TEST_F(ShotComposerUITest, RemoveCharacterFromShot)
{
    m_panel->newShot("Test Shot");
    m_panel->addCharacter("Rapi");
    m_panel->addCharacter("Modernia");

    EXPECT_TRUE(m_panel->removeCharacter(0));
    EXPECT_EQ(m_panel->currentShot().characterCount(), 1);
    EXPECT_EQ(m_panel->layerList()->count(), 1);
}

TEST_F(ShotComposerUITest, AddBackground)
{
    m_panel->newShot("Test Shot");
    int idx = m_panel->addBackground("bg.png");
    EXPECT_EQ(idx, 0);
    EXPECT_EQ(m_panel->currentShot().backgroundCount(), 1);
    EXPECT_EQ(m_panel->layerList()->count(), 1);
}

TEST_F(ShotComposerUITest, RemoveBackground)
{
    m_panel->newShot("Test Shot");
    m_panel->addBackground("bg.png");

    EXPECT_TRUE(m_panel->removeBackground(0));
    EXPECT_EQ(m_panel->currentShot().backgroundCount(), 0);
}

TEST_F(ShotComposerUITest, SelectCharacterLayer)
{
    m_panel->newShot("Test Shot");
    m_panel->addCharacter("Modernia");

    m_panel->selectLayer(0);
    EXPECT_EQ(m_panel->selectedLayerIndex(), 0);
    EXPECT_TRUE(m_panel->charPropsGroup()->isVisible());
    EXPECT_FALSE(m_panel->bgPropsGroup()->isVisible());
}

TEST_F(ShotComposerUITest, SelectBackgroundLayer)
{
    m_panel->newShot("Test Shot");
    m_panel->addBackground("bg.png");

    m_panel->selectLayer(0);
    EXPECT_EQ(m_panel->selectedLayerIndex(), 0);
    EXPECT_FALSE(m_panel->charPropsGroup()->isVisible());
    EXPECT_TRUE(m_panel->bgPropsGroup()->isVisible());
}

TEST_F(ShotComposerUITest, CharacterPropertiesPopulated)
{
    m_panel->newShot("Test Shot");
    CharacterState ch;
    ch.characterName = "Rapi";
    ch.posX = 0.3f;
    ch.posY = 0.6f;
    ch.scale = 1.2f;
    ch.opacity = 0.8f;
    ch.flipX = true;
    ch.isTalking = true;
    m_panel->currentShot().addCharacter(ch);
    // Need to refresh the layer list and select
    m_panel->selectLayer(0);

    EXPECT_NEAR(m_panel->posXSpin()->value(), 30.0, 1.0);    // 0.3 × 100
    EXPECT_NEAR(m_panel->posYSpin()->value(), 60.0, 1.0);    // 0.6 × 100
    EXPECT_NEAR(m_panel->scaleSpin()->value(), 120.0, 1.0);   // 1.2 × 100
    EXPECT_NEAR(m_panel->opacitySpin()->value(), 80.0, 1.0);   // 0.8 × 100
    EXPECT_TRUE(m_panel->flipXCheck()->isChecked());
    EXPECT_TRUE(m_panel->talkingCheck()->isChecked());
}

TEST_F(ShotComposerUITest, BackgroundPropertiesPopulated)
{
    m_panel->newShot("Test Shot");
    BackgroundState bg;
    bg.path = "test.png";
    bg.posX = 0.4f;
    bg.scale = 2.0f;
    bg.opacity = 0.7f;
    m_panel->currentShot().addBackground(bg);
    m_panel->selectLayer(0);

    EXPECT_NEAR(m_panel->bgPosXSpin()->value(), 40.0, 1.0);   // 0.4 × 100
    EXPECT_NEAR(m_panel->bgScaleSpin()->value(), 200.0, 1.0);   // 2.0 × 100
    EXPECT_NEAR(m_panel->bgOpacitySpin()->value(), 70.0, 1.0);   // 0.7 × 100
}

TEST_F(ShotComposerUITest, EditCharacterProperties)
{
    m_panel->newShot("Test Shot");
    m_panel->addCharacter("Rapi");
    m_panel->selectLayer(0);

    m_panel->posXSpin()->setValue(20.0);    // 20% → internal 0.2
    m_panel->scaleSpin()->setValue(150.0);   // 150% → internal 1.5
    m_panel->flipXCheck()->setChecked(true);

    const auto* ch = m_panel->currentShot().character(0);
    ASSERT_NE(ch, nullptr);
    EXPECT_NEAR(ch->posX, 0.2f, 0.01f);
    EXPECT_NEAR(ch->scale, 1.5f, 0.01f);
    EXPECT_TRUE(ch->flipX);
}

TEST_F(ShotComposerUITest, EditBackgroundProperties)
{
    m_panel->newShot("Test Shot");
    m_panel->addBackground("bg.png");
    m_panel->selectLayer(0);

    m_panel->bgScaleSpin()->setValue(300.0);  // 300% → internal 3.0
    m_panel->bgOpacitySpin()->setValue(50.0);   //  50% → internal 0.5

    const auto* bg = m_panel->currentShot().background(0);
    ASSERT_NE(bg, nullptr);
    EXPECT_NEAR(bg->scale, 3.0f, 0.01f);
    EXPECT_NEAR(bg->opacity, 0.5f, 0.01f);
}

TEST_F(ShotComposerUITest, MoveLayerUp)
{
    m_panel->newShot("Test Shot");
    m_panel->addCharacter("A");
    m_panel->addCharacter("B");

    m_panel->selectLayer(1);  // Select B
    m_panel->moveSelectedLayerUp();

    EXPECT_EQ(m_panel->selectedLayerIndex(), 0);
    EXPECT_EQ(m_panel->currentShot().layerOrder()[0].index, 1); // B is now first
}

TEST_F(ShotComposerUITest, MoveLayerDown)
{
    m_panel->newShot("Test Shot");
    m_panel->addCharacter("A");
    m_panel->addCharacter("B");

    m_panel->selectLayer(0);  // Select A
    m_panel->moveSelectedLayerDown();

    EXPECT_EQ(m_panel->selectedLayerIndex(), 1);
    EXPECT_EQ(m_panel->currentShot().layerOrder()[0].index, 1); // B is now first
}

TEST_F(ShotComposerUITest, ShotChangedSignal)
{
    QSignalSpy spy(m_panel.get(), &ShotComposer::shotChanged);

    m_panel->newShot("Test Shot");
    EXPECT_GE(spy.count(), 1);

    spy.clear();
    m_panel->addCharacter("Rapi");
    EXPECT_GE(spy.count(), 1);
}

TEST_F(ShotComposerUITest, LayerSelectedSignal)
{
    m_panel->newShot("Test Shot");
    m_panel->addCharacter("Rapi");

    QSignalSpy spy(m_panel.get(), &ShotComposer::layerSelected);
    m_panel->selectLayer(0);
    EXPECT_GE(spy.count(), 1);
}

TEST_F(ShotComposerUITest, SetCurrentShot)
{
    auto preset = ShotPreset::createDefault("Rapi");
    preset.setCameraZoom(2.0f);

    m_panel->setCurrentShot(preset);
    EXPECT_EQ(m_panel->shotNameEdit()->text(), "Rapi - Default");
    EXPECT_NEAR(m_panel->cameraZoomSpin()->value(), 200.0, 1.0);  // 2.0 × 100
    EXPECT_EQ(m_panel->currentShot().characterCount(), 1);
}

TEST_F(ShotComposerUITest, CameraZoomProperty)
{
    m_panel->newShot("Test Shot");
    m_panel->cameraZoomSpin()->setValue(300.0);  // 300% → internal 3.0
    EXPECT_NEAR(m_panel->currentShot().cameraZoom(), 3.0f, 0.01f);
}

TEST_F(ShotComposerUITest, ShotNameProperty)
{
    m_panel->newShot("Test Shot");
    m_panel->shotNameEdit()->setText("My Custom Shot");
    EXPECT_EQ(m_panel->currentShot().name(), "My Custom Shot");
}

TEST_F(ShotComposerUITest, SwitchLayerSelection)
{
    m_panel->newShot("Test Shot");
    m_panel->addBackground("bg.png");
    m_panel->addCharacter("Rapi");

    // Select background
    m_panel->selectLayer(0);
    EXPECT_FALSE(m_panel->charPropsGroup()->isVisible());
    EXPECT_TRUE(m_panel->bgPropsGroup()->isVisible());

    // Switch to character
    m_panel->selectLayer(1);
    EXPECT_TRUE(m_panel->charPropsGroup()->isVisible());
    EXPECT_FALSE(m_panel->bgPropsGroup()->isVisible());
}

TEST_F(ShotComposerUITest, DeselectLayer)
{
    m_panel->newShot("Test Shot");
    m_panel->addCharacter("Rapi");
    m_panel->selectLayer(0);

    EXPECT_TRUE(m_panel->charPropsGroup()->isVisible());

    m_panel->selectLayer(-1);
    EXPECT_FALSE(m_panel->charPropsGroup()->isVisible());
    EXPECT_FALSE(m_panel->bgPropsGroup()->isVisible());
}

TEST_F(ShotComposerUITest, LibraryTabCount)
{
    // Characters, Custom (PNG puppets), Backgrounds, NikkeBKG, Videos —
    // the Custom tab was added with the puppet feature and this count
    // was never updated (this test group isn't in the CI core gate).
    EXPECT_EQ(m_panel->libraryTabs()->count(), 5);
}

TEST_F(ShotComposerUITest, StanceComboOptions)
{
    EXPECT_EQ(m_panel->stanceCombo()->count(), 3); // Default, Aim, Cover
    EXPECT_EQ(m_panel->stanceCombo()->itemText(0), "Default");
    EXPECT_EQ(m_panel->stanceCombo()->itemText(1), "Aim");
    EXPECT_EQ(m_panel->stanceCombo()->itemText(2), "Cover");
}

TEST_F(ShotComposerUITest, NoSpuriousSignalOnLoad)
{
    auto preset = ShotPreset::createDefault("Rapi");

    QSignalSpy spy(m_panel.get(), &ShotComposer::shotChanged);
    m_panel->setCurrentShot(preset);

    // Only one shotChanged on setCurrentShot, not extra from widgets
    EXPECT_EQ(spy.count(), 1);
}

TEST_F(ShotComposerUITest, PresetManagerIntegration)
{
    TempDir tmpDir;
    m_panel->setPresetsDirectory(tmpDir.path());

    // Save a preset
    m_panel->setCurrentShot(ShotPreset::createDefault("Rapi"));
    EXPECT_TRUE(m_panel->saveCurrentShot());

    EXPECT_TRUE(m_panel->presetManager().hasPreset("Rapi - Default"));
    // The shot lives in the No-Show namespace ("" show).
    EXPECT_TRUE(m_panel->presetManager().hasPreset("", "Rapi - Default"));
    EXPECT_EQ(m_panel->shotList()->count(), 1);   // shot strip stays in sync
}
