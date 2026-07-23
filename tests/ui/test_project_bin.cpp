/*
 * test_project_bin.cpp — Tests for Step 16: Project Bin
 *
 * Tests ThumbnailGenerator, ThumbnailGrid, and ProjectBin.
 */

#include <gtest/gtest.h>

#include "decode/ThumbnailGenerator.h"
#include "widgets/ThumbnailGrid.h"
#include "panels/project/ProjectBin.h"

#include <QApplication>
#include <QSignalSpy>
#include <QTabBar>
#include <QLineEdit>
#include <QSlider>
#include <QTest>

#include <filesystem>
#include <thread>
#include <atomic>
#include <chrono>

namespace fs = std::filesystem;

// ═══════════════════════════════════════════════════════════════════════════
//  QApplication fixture
// ═══════════════════════════════════════════════════════════════════════════

static int    s_argc = 1;
static char   s_arg0[] = "test_project_bin";
static char*  s_argv[] = { s_arg0, nullptr };

class ProjectBinTestEnv : public ::testing::Environment
{
public:
    void SetUp() override
    {
        if (!QApplication::instance())
            m_app = new QApplication(s_argc, s_argv);
    }
    void TearDown() override { /* QApplication lasts until process exit */ }
private:
    QApplication* m_app{nullptr};
};

static auto* g_env = ::testing::AddGlobalTestEnvironment(new ProjectBinTestEnv);

// ═══════════════════════════════════════════════════════════════════════════
//  MediaType detection tests
// ═══════════════════════════════════════════════════════════════════════════

TEST(ThumbnailMediaType, DetectsVideoExtensions)
{
    EXPECT_EQ(rt::ThumbnailGenerator::detectMediaType("clip.mp4"),  rt::MediaType::Video);
    EXPECT_EQ(rt::ThumbnailGenerator::detectMediaType("clip.MKV"),  rt::MediaType::Video);
    EXPECT_EQ(rt::ThumbnailGenerator::detectMediaType("clip.avi"),  rt::MediaType::Video);
    EXPECT_EQ(rt::ThumbnailGenerator::detectMediaType("demo.mov"),  rt::MediaType::Video);
    EXPECT_EQ(rt::ThumbnailGenerator::detectMediaType("test.webm"), rt::MediaType::Video);
}

TEST(ThumbnailMediaType, DetectsImageExtensions)
{
    EXPECT_EQ(rt::ThumbnailGenerator::detectMediaType("photo.png"),  rt::MediaType::Image);
    EXPECT_EQ(rt::ThumbnailGenerator::detectMediaType("photo.JPG"),  rt::MediaType::Image);
    EXPECT_EQ(rt::ThumbnailGenerator::detectMediaType("photo.jpeg"), rt::MediaType::Image);
    EXPECT_EQ(rt::ThumbnailGenerator::detectMediaType("photo.bmp"),  rt::MediaType::Image);
    EXPECT_EQ(rt::ThumbnailGenerator::detectMediaType("photo.tga"),  rt::MediaType::Image);
    EXPECT_EQ(rt::ThumbnailGenerator::detectMediaType("photo.webp"), rt::MediaType::Image);
}

TEST(ThumbnailMediaType, DetectsAudioExtensions)
{
    EXPECT_EQ(rt::ThumbnailGenerator::detectMediaType("track.wav"),  rt::MediaType::Audio);
    EXPECT_EQ(rt::ThumbnailGenerator::detectMediaType("track.MP3"),  rt::MediaType::Audio);
    EXPECT_EQ(rt::ThumbnailGenerator::detectMediaType("track.flac"), rt::MediaType::Audio);
    EXPECT_EQ(rt::ThumbnailGenerator::detectMediaType("track.ogg"),  rt::MediaType::Audio);
    EXPECT_EQ(rt::ThumbnailGenerator::detectMediaType("track.aac"),  rt::MediaType::Audio);
}

TEST(ThumbnailMediaType, DetectsSpineExtensions)
{
    EXPECT_EQ(rt::ThumbnailGenerator::detectMediaType("char.skel"),  rt::MediaType::Spine);
    EXPECT_EQ(rt::ThumbnailGenerator::detectMediaType("char.atlas"), rt::MediaType::Spine);
}

TEST(ThumbnailMediaType, DetectsUnknownExtensions)
{
    EXPECT_EQ(rt::ThumbnailGenerator::detectMediaType("file.xyz"),   rt::MediaType::Unknown);
    EXPECT_EQ(rt::ThumbnailGenerator::detectMediaType("readme.txt"), rt::MediaType::Unknown);
    EXPECT_EQ(rt::ThumbnailGenerator::detectMediaType("data.bin"),   rt::MediaType::Unknown);
}

// ═══════════════════════════════════════════════════════════════════════════
//  Extension helper tests
// ═══════════════════════════════════════════════════════════════════════════

TEST(ThumbnailExtensions, VideoExtensions)
{
    EXPECT_TRUE(rt::ThumbnailGenerator::isVideoExtension(".mp4"));
    EXPECT_TRUE(rt::ThumbnailGenerator::isVideoExtension(".mkv"));
    EXPECT_FALSE(rt::ThumbnailGenerator::isVideoExtension(".png"));
    EXPECT_FALSE(rt::ThumbnailGenerator::isVideoExtension(".wav"));
    EXPECT_FALSE(rt::ThumbnailGenerator::isVideoExtension("mp4")); // no dot
}

TEST(ThumbnailExtensions, ImageExtensions)
{
    EXPECT_TRUE(rt::ThumbnailGenerator::isImageExtension(".png"));
    EXPECT_TRUE(rt::ThumbnailGenerator::isImageExtension(".jpg"));
    EXPECT_TRUE(rt::ThumbnailGenerator::isImageExtension(".webp"));
    EXPECT_FALSE(rt::ThumbnailGenerator::isImageExtension(".mp4"));
    EXPECT_FALSE(rt::ThumbnailGenerator::isImageExtension(".wav"));
}

TEST(ThumbnailExtensions, AudioExtensions)
{
    EXPECT_TRUE(rt::ThumbnailGenerator::isAudioExtension(".wav"));
    EXPECT_TRUE(rt::ThumbnailGenerator::isAudioExtension(".mp3"));
    EXPECT_TRUE(rt::ThumbnailGenerator::isAudioExtension(".flac"));
    EXPECT_FALSE(rt::ThumbnailGenerator::isAudioExtension(".mp4"));
    EXPECT_FALSE(rt::ThumbnailGenerator::isAudioExtension(".png"));
}

// ═══════════════════════════════════════════════════════════════════════════
//  Thumbnail struct tests
// ═══════════════════════════════════════════════════════════════════════════

TEST(ThumbnailStruct, EmptyByDefault)
{
    rt::Thumbnail t;
    EXPECT_TRUE(t.empty());
    EXPECT_FALSE(t.valid);
    EXPECT_EQ(t.width, 0u);
    EXPECT_EQ(t.height, 0u);
    EXPECT_EQ(t.type, rt::MediaType::Unknown);
}

TEST(ThumbnailStruct, MemoryUsage)
{
    rt::Thumbnail t;
    t.width = 160;
    t.height = 120;
    t.stride = 640;
    t.pixels.resize(160 * 120 * 4, 0);
    t.valid = true;

    EXPECT_FALSE(t.empty());
    EXPECT_GT(t.memoryUsage(), 160u * 120u * 4u);
}

// ═══════════════════════════════════════════════════════════════════════════
//  ThumbnailKey / hash
// ═══════════════════════════════════════════════════════════════════════════

TEST(ThumbnailKey, Equality)
{
    rt::ThumbnailKey a{"path/to/file.mp4", 160};
    rt::ThumbnailKey b{"path/to/file.mp4", 160};
    rt::ThumbnailKey c{"path/to/other.mp4", 160};
    rt::ThumbnailKey d{"path/to/file.mp4", 320};

    EXPECT_EQ(a, b);
    EXPECT_FALSE(a == c);
    EXPECT_FALSE(a == d);
}

TEST(ThumbnailKey, HashConsistency)
{
    rt::ThumbnailKeyHash hash;
    rt::ThumbnailKey a{"path/to/file.mp4", 160};
    rt::ThumbnailKey b{"path/to/file.mp4", 160};
    rt::ThumbnailKey c{"path/to/other.mp4", 160};

    EXPECT_EQ(hash(a), hash(b));
    // Different keys should (usually) produce different hashes
    // Not guaranteed, but extremely likely for different paths
    EXPECT_NE(hash(a), hash(c));
}

// ═══════════════════════════════════════════════════════════════════════════
//  ThumbnailGenerator tests
// ═══════════════════════════════════════════════════════════════════════════

TEST(ThumbnailGenerator, ConstructionDefaults)
{
    rt::ThumbnailGenerator gen(1, 200, 150);
    EXPECT_EQ(gen.defaultWidth(), 200u);
    EXPECT_EQ(gen.defaultHeight(), 150u);
    EXPECT_EQ(gen.cacheCount(), 0u);
    EXPECT_EQ(gen.cacheMemoryUsed(), 0u);
    EXPECT_EQ(gen.pendingCount(), 0u);
}

TEST(ThumbnailGenerator, SetDefaultSize)
{
    rt::ThumbnailGenerator gen(1);
    gen.setDefaultSize(320, 240);
    EXPECT_EQ(gen.defaultWidth(), 320u);
    EXPECT_EQ(gen.defaultHeight(), 240u);

    // Zero clamps to defaults
    gen.setDefaultSize(0, 0);
    EXPECT_EQ(gen.defaultWidth(), 160u);
    EXPECT_EQ(gen.defaultHeight(), 120u);
}

TEST(ThumbnailGenerator, GenerateSyncPlaceholder)
{
    rt::ThumbnailGenerator gen(1, 80, 60);

    // Generate a placeholder for a non-existent file with unknown extension
    auto thumb = gen.generateSync(fs::path("fake_file.xyz"), 80);

    ASSERT_NE(thumb, nullptr);
    EXPECT_TRUE(thumb->valid);
    EXPECT_EQ(thumb->width, 80u);
    EXPECT_FALSE(thumb->empty());
    EXPECT_GT(thumb->pixels.size(), 0u);
}

TEST(ThumbnailGenerator, GenerateSyncVideoPlaceholder)
{
    // Without a MediaPool, video thumbnails fall back to placeholders
    rt::ThumbnailGenerator gen(1, 80, 60);

    auto thumb = gen.generateSync(fs::path("test_video.mp4"), 80);

    ASSERT_NE(thumb, nullptr);
    EXPECT_TRUE(thumb->valid);
    EXPECT_EQ(thumb->width, 80u);
    EXPECT_FALSE(thumb->empty());
}

TEST(ThumbnailGenerator, CacheAfterSync)
{
    rt::ThumbnailGenerator gen(1, 80, 60);

    EXPECT_FALSE(gen.isCached(fs::path("nonexistent.xyz"), 80));

    // generateSync for an unknown type should produce a placeholder and cache it
    auto thumb = gen.generateSync(fs::path("test.xyz"), 80);
    ASSERT_NE(thumb, nullptr);

    // The file doesn't exist on disk so canonical path fails:
    // it will cache by the original path
    EXPECT_EQ(gen.cacheCount(), 1u);
    EXPECT_GT(gen.cacheMemoryUsed(), 0u);
}

TEST(ThumbnailGenerator, ClearCache)
{
    rt::ThumbnailGenerator gen(1, 80, 60);
    (void)gen.generateSync(fs::path("a.xyz"), 80);
    (void)gen.generateSync(fs::path("b.xyz"), 80);
    EXPECT_EQ(gen.cacheCount(), 2u);

    gen.clearCache();
    EXPECT_EQ(gen.cacheCount(), 0u);
    EXPECT_EQ(gen.cacheMemoryUsed(), 0u);
}

TEST(ThumbnailGenerator, AsyncRequest)
{
    rt::ThumbnailGenerator gen(2, 80, 60);

    std::atomic<bool> called{false};
    std::shared_ptr<rt::Thumbnail> received;

    gen.requestThumbnail(
        fs::path("async_test.xyz"),
        [&](const fs::path&, std::shared_ptr<rt::Thumbnail> thumb) {
            received = thumb;
            called = true;
        },
        80);

    // Wait for callback (with timeout)
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!called && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

    EXPECT_TRUE(called);
    ASSERT_NE(received, nullptr);
    EXPECT_TRUE(received->valid);
}

TEST(ThumbnailGenerator, BatchRequest)
{
    rt::ThumbnailGenerator gen(2, 80, 60);

    std::atomic<int> count{0};
    std::vector<fs::path> files = {
        fs::path("batch1.xyz"),
        fs::path("batch2.xyz"),
        fs::path("batch3.xyz")
    };

    gen.requestBatch(files,
        [&](const fs::path&, std::shared_ptr<rt::Thumbnail>) {
            ++count;
        },
        80);

    // Wait for all callbacks
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (count < 3 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

    EXPECT_EQ(count.load(), 3);
}

TEST(ThumbnailGenerator, CancelAll)
{
    rt::ThumbnailGenerator gen(1, 80, 60);

    // Queue many requests
    for (int i = 0; i < 100; ++i)
    {
        gen.requestThumbnail(
            fs::path("cancel_" + std::to_string(i) + ".xyz"),
            [](const fs::path&, std::shared_ptr<rt::Thumbnail>) {},
            80);
    }

    gen.cancelAll();

    // After cancel, pending should be 0 or very low
    // (some may have already started processing)
    EXPECT_LE(gen.pendingCount(), 1u);
}

TEST(ThumbnailGenerator, CachedSyncReturnsFast)
{
    rt::ThumbnailGenerator gen(1, 80, 60);

    // First call generates
    auto t1 = gen.generateSync(fs::path("cached_test.xyz"), 80);
    ASSERT_NE(t1, nullptr);

    // Second call should return the cached version
    auto t2 = gen.generateSync(fs::path("cached_test.xyz"), 80);
    ASSERT_NE(t2, nullptr);

    // Should be the same pointer (from cache)
    EXPECT_EQ(t1.get(), t2.get());
}

TEST(ThumbnailGenerator, DifferentWidthsDifferentCache)
{
    rt::ThumbnailGenerator gen(1, 80, 60);

    auto t1 = gen.generateSync(fs::path("size_test.xyz"), 80);
    auto t2 = gen.generateSync(fs::path("size_test.xyz"), 160);

    ASSERT_NE(t1, nullptr);
    ASSERT_NE(t2, nullptr);
    EXPECT_NE(t1.get(), t2.get());
    EXPECT_EQ(gen.cacheCount(), 2u);
}

// ═══════════════════════════════════════════════════════════════════════════
//  ThumbnailGrid tests
// ═══════════════════════════════════════════════════════════════════════════

TEST(ThumbnailGrid, EmptyByDefault)
{
    rt::ThumbnailGrid grid;
    EXPECT_EQ(grid.itemCount(), 0);
    EXPECT_EQ(grid.visibleItemCount(), 0);
    EXPECT_EQ(grid.selectedIndex(), -1);
    EXPECT_EQ(grid.selectedItem(), nullptr);
}

TEST(ThumbnailGrid, AddAndRemoveItems)
{
    rt::ThumbnailGrid grid;

    grid.addItem(fs::path("video.mp4"));
    EXPECT_EQ(grid.itemCount(), 1);

    grid.addItem(fs::path("image.png"));
    EXPECT_EQ(grid.itemCount(), 2);

    EXPECT_TRUE(grid.removeItem(fs::path("video.mp4")));
    EXPECT_EQ(grid.itemCount(), 1);

    EXPECT_FALSE(grid.removeItem(fs::path("nonexistent.xyz")));
    EXPECT_EQ(grid.itemCount(), 1);
}

TEST(ThumbnailGrid, AddBatch)
{
    rt::ThumbnailGrid grid;

    std::vector<fs::path> files = {
        fs::path("a.mp4"),
        fs::path("b.png"),
        fs::path("c.wav")
    };
    grid.addItems(files);

    EXPECT_EQ(grid.itemCount(), 3);
    EXPECT_EQ(grid.visibleItemCount(), 3);
}

TEST(ThumbnailGrid, ClearItems)
{
    rt::ThumbnailGrid grid;
    grid.addItem(fs::path("a.mp4"));
    grid.addItem(fs::path("b.png"));
    grid.clearItems();
    EXPECT_EQ(grid.itemCount(), 0);
    EXPECT_EQ(grid.selectedIndex(), -1);
}

TEST(ThumbnailGrid, Selection)
{
    rt::ThumbnailGrid grid;
    grid.addItem(fs::path("a.mp4"));
    grid.addItem(fs::path("b.png"));
    grid.addItem(fs::path("c.wav"));

    grid.selectItem(1);
    EXPECT_EQ(grid.selectedIndex(), 1);
    ASSERT_NE(grid.selectedItem(), nullptr);
    EXPECT_EQ(grid.selectedItem()->filePath, fs::path("b.png"));
    EXPECT_TRUE(grid.selectedItem()->selected);

    grid.selectItem(2);
    EXPECT_EQ(grid.selectedIndex(), 2);
    EXPECT_FALSE(grid.items()[1].selected);
    EXPECT_TRUE(grid.items()[2].selected);

    grid.clearSelection();
    EXPECT_EQ(grid.selectedIndex(), -1);
}

TEST(ThumbnailGrid, SelectionOutOfRange)
{
    rt::ThumbnailGrid grid;
    grid.addItem(fs::path("a.mp4"));

    grid.selectItem(5);
    EXPECT_EQ(grid.selectedIndex(), -1);

    grid.selectItem(-3);
    EXPECT_EQ(grid.selectedIndex(), -1);
}

TEST(ThumbnailGrid, TextFilter)
{
    rt::ThumbnailGrid grid;
    grid.addItem(fs::path("intro_video.mp4"));
    grid.addItem(fs::path("outro_video.mp4"));
    grid.addItem(fs::path("background.png"));
    grid.addItem(fs::path("music.wav"));

    EXPECT_EQ(grid.visibleItemCount(), 4);

    grid.setFilter("video");
    EXPECT_EQ(grid.visibleItemCount(), 2);

    grid.setFilter("intro");
    EXPECT_EQ(grid.visibleItemCount(), 1);

    grid.setFilter("nonexistent");
    EXPECT_EQ(grid.visibleItemCount(), 0);

    grid.setFilter("");
    EXPECT_EQ(grid.visibleItemCount(), 4);
}

TEST(ThumbnailGrid, TextFilterCaseInsensitive)
{
    rt::ThumbnailGrid grid;
    grid.addItem(fs::path("MyVideo.MP4"));
    grid.addItem(fs::path("background.png"));

    grid.setFilter("myvideo");
    EXPECT_EQ(grid.visibleItemCount(), 1);

    grid.setFilter("MYVIDEO");
    EXPECT_EQ(grid.visibleItemCount(), 1);
}

TEST(ThumbnailGrid, TypeFilter)
{
    rt::ThumbnailGrid grid;
    grid.addItem(fs::path("clip.mp4"));
    grid.addItem(fs::path("photo.png"));
    grid.addItem(fs::path("track.wav"));
    grid.addItem(fs::path("track2.wav"));

    EXPECT_EQ(grid.visibleItemCount(), 4);

    grid.setTypeFilter(rt::MediaType::Audio);
    EXPECT_EQ(grid.visibleItemCount(), 2);

    grid.setTypeFilter(rt::MediaType::Video);
    EXPECT_EQ(grid.visibleItemCount(), 1);

    grid.setTypeFilter(rt::MediaType::Unknown); // All
    EXPECT_EQ(grid.visibleItemCount(), 4);
}

TEST(ThumbnailGrid, CombinedFilters)
{
    rt::ThumbnailGrid grid;
    grid.addItem(fs::path("intro_clip.mp4"));
    grid.addItem(fs::path("outro_clip.mp4"));
    grid.addItem(fs::path("intro_music.wav"));
    grid.addItem(fs::path("bg.png"));

    grid.setFilter("intro");
    EXPECT_EQ(grid.visibleItemCount(), 2);

    grid.setTypeFilter(rt::MediaType::Video);
    EXPECT_EQ(grid.visibleItemCount(), 1);
    EXPECT_TRUE(grid.items()[0].visible);    // intro_clip.mp4
    EXPECT_FALSE(grid.items()[1].visible);   // outro_clip.mp4
    EXPECT_FALSE(grid.items()[2].visible);   // intro_music.wav
    EXPECT_FALSE(grid.items()[3].visible);   // bg.png
}

TEST(ThumbnailGrid, Zoom)
{
    rt::ThumbnailGrid grid;

    EXPECT_FLOAT_EQ(grid.zoom(), 1.0f);
    int defaultWidth = grid.cellWidth();

    grid.setZoom(2.0f);
    EXPECT_FLOAT_EQ(grid.zoom(), 2.0f);
    EXPECT_GT(grid.cellWidth(), defaultWidth);

    grid.setZoom(0.5f);
    EXPECT_FLOAT_EQ(grid.zoom(), 0.5f);
    EXPECT_LT(grid.cellWidth(), defaultWidth);

    // Clamped range
    grid.setZoom(0.1f);
    EXPECT_FLOAT_EQ(grid.zoom(), 0.3f);

    grid.setZoom(10.0f);
    EXPECT_FLOAT_EQ(grid.zoom(), 3.0f);
}

TEST(ThumbnailGrid, AutoDetectsMediaType)
{
    rt::ThumbnailGrid grid;
    grid.addItem(fs::path("clip.mp4"));
    grid.addItem(fs::path("photo.png"));
    grid.addItem(fs::path("track.wav"));

    ASSERT_EQ(grid.items().size(), 3u);
    EXPECT_EQ(grid.items()[0].type, rt::MediaType::Video);
    EXPECT_EQ(grid.items()[1].type, rt::MediaType::Image);
    EXPECT_EQ(grid.items()[2].type, rt::MediaType::Audio);
}

TEST(ThumbnailGrid, ItemSignals)
{
    rt::ThumbnailGrid grid;
    grid.addItem(fs::path("clip.mp4"));
    grid.addItem(fs::path("photo.png"));

    QSignalSpy selSpy(&grid, &rt::ThumbnailGrid::itemSelected);

    grid.selectItem(0);
    EXPECT_EQ(selSpy.count(), 1);

    grid.selectItem(1);
    EXPECT_EQ(selSpy.count(), 2);

    // Re-select same index: no signal
    grid.selectItem(1);
    EXPECT_EQ(selSpy.count(), 2);
}

TEST(ThumbnailGrid, RemoveSelectedUpdatesIndex)
{
    rt::ThumbnailGrid grid;
    grid.addItem(fs::path("a.mp4"));
    grid.addItem(fs::path("b.png"));
    grid.addItem(fs::path("c.wav"));

    grid.selectItem(1);
    EXPECT_EQ(grid.selectedIndex(), 1);

    // Remove the selected item
    grid.removeItem(fs::path("b.png"));
    EXPECT_EQ(grid.selectedIndex(), -1);
    EXPECT_EQ(grid.itemCount(), 2);
}

TEST(ThumbnailGrid, RemoveBeforeSelectedShiftsIndex)
{
    rt::ThumbnailGrid grid;
    grid.addItem(fs::path("a.mp4"));
    grid.addItem(fs::path("b.png"));
    grid.addItem(fs::path("c.wav"));

    grid.selectItem(2);
    EXPECT_EQ(grid.selectedIndex(), 2);

    // Remove item before the selected one
    grid.removeItem(fs::path("a.mp4"));
    EXPECT_EQ(grid.selectedIndex(), 1);  // shifted down
}

// ═══════════════════════════════════════════════════════════════════════════
//  ProjectBin tests
// ═══════════════════════════════════════════════════════════════════════════

TEST(ProjectBin, Construction)
{
    rt::ProjectBin bin;
    EXPECT_EQ(bin.itemCount(), 0);
    EXPECT_NE(bin.grid(), nullptr);
    EXPECT_NE(bin.searchField(), nullptr);
    EXPECT_NE(bin.zoomSlider(), nullptr);
    EXPECT_EQ(bin.activeTabType(), rt::MediaType::Unknown); // All
}

TEST(ProjectBin, AddAndQueryFiles)
{
    rt::ProjectBin bin;

    std::vector<fs::path> files = {
        fs::path("clip1.mp4"),
        fs::path("clip2.mp4"),
        fs::path("photo.png"),
        fs::path("track.wav")
    };
    bin.addFiles(files);

    EXPECT_EQ(bin.itemCount(), 4);
    EXPECT_EQ(bin.allFiles().size(), 4u);
}

TEST(ProjectBin, FilesOfType)
{
    rt::ProjectBin bin;

    std::vector<fs::path> files = {
        fs::path("a.mp4"),
        fs::path("b.mp4"),
        fs::path("c.png"),
        fs::path("d.wav"),
        fs::path("e.wav")
    };
    bin.addFiles(files);

    auto videos = bin.filesOfType(rt::MediaType::Video);
    EXPECT_EQ(videos.size(), 2u);

    auto audio = bin.filesOfType(rt::MediaType::Audio);
    EXPECT_EQ(audio.size(), 2u);

    auto images = bin.filesOfType(rt::MediaType::Image);
    EXPECT_EQ(images.size(), 1u);
}

TEST(ProjectBin, RemoveFile)
{
    rt::ProjectBin bin;
    bin.addFiles({fs::path("a.mp4"), fs::path("b.png")});
    EXPECT_EQ(bin.itemCount(), 2);

    EXPECT_TRUE(bin.removeFile(fs::path("a.mp4")));
    EXPECT_EQ(bin.itemCount(), 1);

    EXPECT_FALSE(bin.removeFile(fs::path("nonexistent.xyz")));
    EXPECT_EQ(bin.itemCount(), 1);
}

TEST(ProjectBin, ClearAll)
{
    rt::ProjectBin bin;
    bin.addFiles({fs::path("a.mp4"), fs::path("b.png"), fs::path("c.wav")});
    EXPECT_EQ(bin.itemCount(), 3);

    bin.clearAll();
    EXPECT_EQ(bin.itemCount(), 0);
}

TEST(ProjectBin, RelinkUpdatesAllMatchingBinReferencesOnly)
{
    rt::ProjectBin bin;
    const fs::path oldPath("offline/source.mov");
    const fs::path newPath("fixed/source.mov");
    bin.addFiles({oldPath, oldPath, fs::path("already/online.mov")});

    EXPECT_EQ(bin.replacePathReferences(oldPath, newPath), 2);

    const auto files = bin.allFiles();
    EXPECT_EQ(std::count(files.begin(), files.end(), newPath), 2);
    EXPECT_EQ(std::count(files.begin(), files.end(), oldPath), 0);
    EXPECT_EQ(std::count(files.begin(), files.end(),
                         fs::path("already/online.mov")), 1);
}

TEST(ProjectBin, TabChangeFiltersGrid)
{
    rt::ProjectBin bin;
    bin.addFiles({
        fs::path("video.mp4"),
        fs::path("image.png"),
        fs::path("audio.wav")
    });

    EXPECT_EQ(bin.grid()->visibleItemCount(), 3);

    // Switch to Video tab (index 1)
    bin.findChild<QTabBar*>()->setCurrentIndex(1);
    EXPECT_EQ(bin.activeTabType(), rt::MediaType::Video);
    EXPECT_EQ(bin.grid()->visibleItemCount(), 1);

    // Switch to Audio tab (index 3)
    bin.findChild<QTabBar*>()->setCurrentIndex(3);
    EXPECT_EQ(bin.activeTabType(), rt::MediaType::Audio);
    EXPECT_EQ(bin.grid()->visibleItemCount(), 1);

    // Switch to All tab (index 0)
    bin.findChild<QTabBar*>()->setCurrentIndex(0);
    EXPECT_EQ(bin.activeTabType(), rt::MediaType::Unknown);
    EXPECT_EQ(bin.grid()->visibleItemCount(), 3);
}

TEST(ProjectBin, SearchFieldFiltersGrid)
{
    rt::ProjectBin bin;
    bin.addFiles({
        fs::path("intro_clip.mp4"),
        fs::path("outro_clip.mp4"),
        fs::path("photo.png")
    });

    EXPECT_EQ(bin.grid()->visibleItemCount(), 3);

    bin.searchField()->setText("clip");
    EXPECT_EQ(bin.grid()->visibleItemCount(), 2);

    bin.searchField()->setText("intro");
    EXPECT_EQ(bin.grid()->visibleItemCount(), 1);

    bin.searchField()->clear();
    EXPECT_EQ(bin.grid()->visibleItemCount(), 3);
}

TEST(ProjectBin, ZoomSliderAffectsGrid)
{
    rt::ProjectBin bin;

    int baseWidth = bin.grid()->cellWidth();

    bin.zoomSlider()->setValue(200); // 2x zoom
    EXPECT_GT(bin.grid()->cellWidth(), baseWidth);

    bin.zoomSlider()->setValue(50); // 0.5x zoom
    EXPECT_LT(bin.grid()->cellWidth(), baseWidth);
}

TEST(ProjectBin, DoubleClickSignal)
{
    rt::ProjectBin bin;
    bin.addFiles({fs::path("video.mp4")});

    QSignalSpy spy(&bin, &rt::ProjectBin::loadInSourceMonitor);

    // Simulate double-click via the grid signal
    emit bin.grid()->itemDoubleClicked(0, fs::path("video.mp4"));

    EXPECT_EQ(spy.count(), 1);
}

TEST(ProjectBin, SizeHint)
{
    rt::ProjectBin bin;
    QSize hint = bin.sizeHint();
    EXPECT_GT(hint.width(), 0);
    EXPECT_GT(hint.height(), 0);
}

// ═══════════════════════════════════════════════════════════════════════════
//  Placeholder color tests
// ═══════════════════════════════════════════════════════════════════════════

TEST(ThumbnailGenerator, PlaceholderHasCorrectDimensions)
{
    rt::ThumbnailGenerator gen(1, 100, 75);
    auto thumb = gen.generateSync(fs::path("test.xyz"), 100);

    ASSERT_NE(thumb, nullptr);
    EXPECT_EQ(thumb->width, 100u);
    EXPECT_EQ(thumb->stride, 400u); // 100 * 4
    EXPECT_EQ(thumb->pixels.size(), 100u * 75u * 4u);
}

TEST(ThumbnailGenerator, PlaceholderPixelsNotAllZero)
{
    rt::ThumbnailGenerator gen(1, 80, 60);
    auto thumb = gen.generateSync(fs::path("test.xyz"), 80);

    ASSERT_NE(thumb, nullptr);
    ASSERT_FALSE(thumb->pixels.empty());

    // Check that not all pixels are zero (should be colored)
    bool allZero = std::all_of(thumb->pixels.begin(), thumb->pixels.end(),
                               [](uint8_t b) { return b == 0; });
    EXPECT_FALSE(allZero);
}
