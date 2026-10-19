/*
 * test_worker_shutdown.cpp — stop-stress harness for background workers.
 *
 * Stage 12: several workers waited on a condition variable with no timeout
 * while their stop path set the "running" flag outside the waiter's mutex.
 * A stop that lands between the worker's check and its sleep loses its
 * wake-up, and join() then blocks forever (shutdown hangs until the 8 s
 * forced exit). These tests stop each worker thousands of times, right
 * after it starts (the worker is then in its first check) or right after
 * it was given work, and fail through StallGuard instead of hanging.
 *
 * The race window is tiny, so passing here does not prove a worker
 * correct; the fix is correct by construction (stop_token waits).
 */

#include <gtest/gtest.h>

#include "../support/StallGuard.h"

#include "cache/DiskFrameCache.h"
#include "cache/FrameCache.h"
#include "cache/SegmentRenderCache.h"
#include "playback/MediaPool.h"

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <system_error>

#ifdef _WIN32
#include <process.h>
#define RT_GETPID _getpid
#else
#include <unistd.h>
#define RT_GETPID getpid
#endif

namespace {

// ctest runs test processes in parallel: one directory per process.
std::filesystem::path tempDir(const char* name)
{
    auto dir = std::filesystem::temp_directory_path() / "rt_worker_shutdown"
             / (std::string(name) + "_" + std::to_string(RT_GETPID()));
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    return dir;
}

std::shared_ptr<rt::CachedFrame> smallFrame(uint64_t mediaId, int64_t frameNumber)
{
    auto f = std::make_shared<rt::CachedFrame>();
    f->mediaId = mediaId;
    f->frameNumber = frameNumber;
    f->width = 8;
    f->height = 8;
    f->stride = 8 * 4;
    f->pixels.assign(8 * 8 * 4, 0x7f);
    return f;
}

} // namespace

TEST(WorkerShutdown, DiskFrameCacheWriterStopsRightAfterStart)
{
    const auto dir = tempDir("disk_idle");
    rt::test::StallGuard guard("DiskFrameCache stop");
    for (int i = 0; i < 1500; ++i) {
        { rt::DiskFrameCache cache(dir, 64ULL * 1024 * 1024); }
        guard.tick();
    }
}

TEST(WorkerShutdown, DiskFrameCacheWriterStopsWhileWriting)
{
    const auto dir = tempDir("disk_busy");
    const auto media = dir / "media.bin";
    { std::ofstream(media, std::ios::binary) << "media"; }
    rt::test::StallGuard guard("DiskFrameCache stop after a write");
    for (int i = 0; i < 500; ++i) {
        {
            rt::DiskFrameCache cache(dir / "cache", 64ULL * 1024 * 1024);
            cache.registerMedia(1, media);
            cache.putAsync(smallFrame(1, i));
        }
        guard.tick();
    }
}

TEST(WorkerShutdown, SegmentRenderCacheDiskWriterStopsRightAfterStart)
{
    const auto dir = tempDir("segment");
    rt::test::StallGuard guard("SegmentRenderCache stop");
    for (int i = 0; i < 600; ++i) {
        {
            rt::SegmentRenderCache cache;
            cache.enableDiskCache(dir, 64ULL * 1024 * 1024);
            if (i % 2)
                cache.put(i * 1000, rt::ResolutionTier::Full, 0x1, smallFrame(0, i));
        }
        guard.tick();
    }
}

TEST(WorkerShutdown, MediaPoolWorkersStopRightAfterStart)
{
    // Open worker, prefetch workers and loop pre-decoders all start in the
    // constructor and stop in the destructor.
    rt::test::StallGuard guard("MediaPool stop");
    for (int i = 0; i < 300; ++i) {
        { rt::MediaPool pool; }
        guard.tick();
    }
}
