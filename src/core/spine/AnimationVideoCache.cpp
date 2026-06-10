/*
 * AnimationVideoCache.cpp — pre-rendered Spine animation cache manager.
 *
 * Manages an on-disk cache of VP9+alpha WebM files for Spine character
 * animations.  On lookup miss, queues background pre-renders via
 * SpinePrerenderer.  On hit, returns decoded frames via MediaPool.
 */

#include "AnimationVideoCache.h"

#ifdef ROUNDTABLE_HAS_SPINE

#include "SpinePrerenderer.h"
#include "SpineEngine.h"
#include "SpineAnimation.h"

#include "PathUtils.h"
#include <spdlog/spdlog.h>

#include <algorithm>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace rt {

// ─── Construction / Destruction ──────────────────────────────────────────────

AnimationVideoCache::AnimationVideoCache(MediaPool* pool,
                                           const std::string& cacheDir,
                                           const std::string& assetsDir)
    : m_cacheDir(cacheDir)
    , m_assetsDir(assetsDir)
    , m_mediaPool(pool)
{
}

AnimationVideoCache::~AnimationVideoCache()
{
    // Signal all worker threads to stop and wait for them
    {
        std::lock_guard lock(m_mutex);
        m_stopWorker = true;
    }
    m_jobCv.notify_all();
    for (auto& t : m_workerThreads)
        if (t.joinable()) t.join();

    // Release media handles
    clear();
}

// ─── Key / Path helpers ──────────────────────────────────────────────────────

std::string AnimationVideoCache::makeKey(const std::string& charName,
                                          const std::string& outfit,
                                          const std::string& animName)
{
    return charName + "|" + outfit + "|" + animName;
}

std::filesystem::path AnimationVideoCache::cachePath(const std::string& charName,
                                                       const std::string& outfit,
                                                       const std::string& animName,
                                                       SpineCacheFormat fmt) const
{
    std::string fmtDir = formatDirName(fmt);
    const char* ext = (fmt == SpineCacheFormat::ProRes4444) ? ".mov" : ".mp4";
    return m_cacheDir / fmtDir / charName / outfit / (animName + ext);
}

// ─── Inventory scan ──────────────────────────────────────────────────────────

void AnimationVideoCache::scanCacheDirectory()
{
    std::lock_guard lock(m_mutex);

    if (!fs::exists(m_cacheDir)) {
        spdlog::info("AnimCache: cache directory does not exist yet: {}",
                     pathToUtf8(m_cacheDir));
        return;
    }

    int count = 0;
    int cleaned = 0;

    // Expect: {cacheDir}/{FormatDir}/{CharName}/{outfit}/{animName}.ext
    // Known format subdirectory names
    static const char* formatDirs[] = {"H264_Green", "H264_Blue", "H264_Custom", "ProRes"};

    for (const auto& fmtName : formatDirs) {
        fs::path fmtPath = m_cacheDir / fmtName;
        if (!fs::exists(fmtPath)) continue;

        for (const auto& charDir : fs::directory_iterator(fmtPath)) {
            if (!charDir.is_directory()) continue;
            std::string charName = pathToUtf8(charDir.path().filename());

            for (const auto& outfitDir : fs::directory_iterator(charDir.path())) {
                if (!outfitDir.is_directory()) continue;
                std::string outfit = pathToUtf8(outfitDir.path().filename());

                // First pass: find and clean up interrupted renders
                for (const auto& f : fs::directory_iterator(outfitDir.path())) {
                    if (!f.is_regular_file()) continue;
                    if (f.path().extension() != ".rendering") continue;

                    // Marker file found — the corresponding video is incomplete
                    for (const auto& ext : {".mov", ".webm", ".mp4"}) {
                        auto videoPath = f.path();
                        videoPath.replace_extension(ext);
                        std::error_code ec;
                        if (fs::exists(videoPath, ec)) {
                            spdlog::warn("AnimCache: removing incomplete render: {}",
                                         pathToUtf8(videoPath));
                            fs::remove(videoPath, ec);
                        }
                    }
                    {
                        std::error_code ec;
                        fs::remove(f.path(), ec);  // remove the marker itself
                    }
                    ++cleaned;
                }

                // Second pass: load valid cache entries (.mp4, .mov, .webm)
                for (const auto& videoFile : fs::directory_iterator(outfitDir.path())) {
                    if (!videoFile.is_regular_file()) continue;
                    auto ext = videoFile.path().extension();

                    if (ext != ".mp4" && ext != ".mov" && ext != ".webm") continue;

                    // Skip 0-byte files (failed pre-renders)
                    auto fileSize = fs::file_size(videoFile.path());
                    if (fileSize == 0) {
                        spdlog::warn("AnimCache: skipping 0-byte file: {}",
                                     pathToUtf8(videoFile.path()));
                        std::error_code removeEc;
                        fs::remove(videoFile.path(), removeEc);
                        continue;
                    }

                    std::string animName = pathToUtf8(videoFile.path().stem());
                    std::string key = makeKey(charName, outfit, animName);

                    // If we already have an entry for this anim, prefer
                    // .mp4 (HEVC packed-alpha) > .mov (ProRes) > .webm (VP9)
                    if (m_entries.count(key) > 0) {
                        auto existingExt = m_entries[key].videoPath.extension();
                        if (existingExt == ".mp4") continue;
                        if (existingExt == ".mov" && ext != ".mp4") continue;
                    }

                    AnimCacheEntry entry;
                    entry.characterName = charName;
                    entry.outfit        = outfit;
                    entry.animationName = animName;
                    entry.videoPath     = videoFile.path();
                    entry.fileSizeBytes = fileSize;

                    // Check skeleton modification time for staleness
                    auto skelPaths = SpineEngine::resolvePaths(
                        m_assetsDir, charName, outfit, CharacterStance::Default);
                    if (skelPaths.valid) {
                        std::error_code ec;
                        entry.skelModTime = fs::last_write_time(skelPaths.skelPath, ec);
                    }

                    m_entries[key] = std::move(entry);
                    ++count;
                }
            }
        }
    }

    if (cleaned > 0)
        spdlog::info("AnimCache: cleaned up {} interrupted render(s)", cleaned);
    spdlog::info("AnimCache: scanned {} cached animation videos", count);
}

// ─── Lookup ──────────────────────────────────────────────────────────────────

size_t AnimationVideoCache::entryCount() const
{
    std::lock_guard lock(m_mutex);
    return m_entries.size();
}

bool AnimationVideoCache::hasVideo(const std::string& characterName,
                                    const std::string& outfit,
                                    const std::string& animationName) const
{
    std::lock_guard lock(m_mutex);
    return m_entries.count(makeKey(characterName, outfit, animationName)) > 0;
}

const AnimCacheEntry* AnimationVideoCache::getEntry(const std::string& characterName,
                                                      const std::string& outfit,
                                                      const std::string& animationName) const
{
    std::lock_guard lock(m_mutex);
    auto it = m_entries.find(makeKey(characterName, outfit, animationName));
    if (it != m_entries.end()) return &it->second;
    return nullptr;
}

bool AnimationVideoCache::hasAnyForCharacter(const std::string& characterName) const
{
    std::lock_guard lock(m_mutex);
    for (const auto& [key, entry] : m_entries) {
        if (entry.characterName == characterName)
            return true;
    }
    return false;
}

size_t AnimationVideoCache::countForCharacter(const std::string& characterName) const
{
    std::lock_guard lock(m_mutex);
    size_t count = 0;
    for (const auto& [key, entry] : m_entries) {
        if (entry.characterName == characterName)
            ++count;
    }
    return count;
}

size_t AnimationVideoCache::countForCharacterOutfit(const std::string& characterName,
                                                      const std::string& outfit) const
{
    std::lock_guard lock(m_mutex);
    size_t count = 0;
    for (const auto& [key, entry] : m_entries) {
        if (entry.characterName == characterName && entry.outfit == outfit)
            ++count;
    }
    return count;
}

std::string AnimationVideoCache::codecForCharacterOutfit(const std::string& characterName,
                                                          const std::string& outfit) const
{
    std::lock_guard lock(m_mutex);
    for (const auto& [key, entry] : m_entries) {
        if (entry.characterName == characterName && entry.outfit == outfit) {
            auto ext = pathToUtf8(entry.videoPath.extension());
            // Normalize to lowercase for comparison
            for (auto& ch : ext) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            if (ext == ".mov")  return "ProRes 4444";
            if (ext == ".mxf")  return "DNxHD";
            if (ext == ".mp4")  return "H.264";
            if (ext == ".webm") return "VP9";
            return ext;  // fallback: raw extension
        }
    }
    return {};
}

MediaHandle AnimationVideoCache::getMediaHandleLocked(const std::string& characterName,
                                                      const std::string& outfit,
                                                      const std::string& animationName)
{
    // PRECONDITION: m_mutex held by caller.
    if (!m_mediaPool) return InvalidMedia;

    auto key = makeKey(characterName, outfit, animationName);
    auto it = m_entries.find(key);
    if (it == m_entries.end()) return InvalidMedia;

    auto& entry = it->second;
    if (entry.mediaHandle == 0) {
        entry.mediaHandle = m_mediaPool->open(entry.videoPath);
        if (entry.mediaHandle != 0) {
            // Fill in stream info
            const auto* info = m_mediaPool->getInfo(entry.mediaHandle);
            if (info) {
                entry.width      = info->width;
                entry.height     = info->height;
                entry.fps        = static_cast<int>(std::round(info->fps));
                entry.duration   = static_cast<float>(info->duration);
                entry.frameCount = info->frameCount;
            }
        }
    }
    return entry.mediaHandle;
}

MediaHandle AnimationVideoCache::getMediaHandle(const std::string& characterName,
                                                  const std::string& outfit,
                                                  const std::string& animationName)
{
    std::lock_guard lock(m_mutex);
    return getMediaHandleLocked(characterName, outfit, animationName);
}

std::shared_ptr<CachedFrame> AnimationVideoCache::getFrame(
    const std::string& characterName,
    const std::string& outfit,
    const std::string& animationName,
    int64_t frameNumber)
{
    // Hold m_mutex across BOTH the handle resolution and the decode.  If we
    // released the lock between the two (as the old getMediaHandle()-then-
    // decode form did), a concurrent removeEntry()/clear() could release the
    // MediaHandle out from under the in-flight decode → use-after-release in
    // MediaPool (D2).  m_mediaPool->getFrame() does not call back into this
    // cache, so holding m_mutex here cannot deadlock.  The cost is that
    // cache decodes serialise against entry mutations, which is acceptable:
    // getFrame() is only used for thumbnail generation, not the hot
    // per-frame composite path (which uses getMediaHandle() + MediaPool
    // directly).
    std::lock_guard lock(m_mutex);

    MediaHandle handle = getMediaHandleLocked(characterName, outfit, animationName);
    if (handle == InvalidMedia || !m_mediaPool) return nullptr;

    // Half tier with the nomH fix (uses nominal height h/2 for scale
    // computation) gives adequate quality: 960×960 for Bahamut, 548×959
    // for Wells.  Full tier exceeded the frame cache and caused thrashing.
    return m_mediaPool->getFrame(handle, frameNumber, ResolutionTier::Half, false);
}

// ─── Pre-rendering ───────────────────────────────────────────────────────────

void AnimationVideoCache::queueRender(const std::string& characterName,
                                        const std::string& outfit,
                                        const std::string& animationName,
                                        bool talking)
{
    // Talking variants use "{animName}_talk" as the cache key / filename
    const std::string cacheAnimName = talking ? (animationName + "_talk") : animationName;

    {
        std::lock_guard lock(m_mutex);
        const std::string key = makeKey(characterName, outfit, cacheAnimName);

        // Skip if already cached or in-progress
        if (m_entries.count(key) > 0 || m_pendingKeys.count(key) > 0) return;

        // If the user explicitly re-queues a render for an outfit that
        // was previously deleted (mid-render guard still active), clear
        // the guard so the new render result is accepted.
        m_deletingOutfits.erase(characterName + "|" + outfit);

        m_pendingKeys.insert(key);
        m_jobQueue.push_back({characterName, outfit, animationName, talking});

        // Lazy-start the worker thread pool.
        // Use only 1 worker for GPU-accelerated encoding: NVENC has a
        // hard session limit (3-5 on consumer GPUs) and multiple
        // simultaneous Vulkan render+readback operations on the shared
        // graphics queue can corrupt output.  NVENC is fast enough that
        // one GPU worker saturates the encoder; the bottleneck is the
        // Vulkan rendering, which serialises on the graphics queue anyway.
        if (m_workerThreads.empty()) {
            constexpr unsigned numWorkers = 1;
            spdlog::info("AnimCache: starting {} worker thread(s)", numWorkers);
            for (unsigned i = 0; i < numWorkers; ++i)
                m_workerThreads.emplace_back(&AnimationVideoCache::workerLoop, this);
        }
    }
    m_jobCv.notify_one();

    spdlog::info("AnimCache: queued pre-render '{}' / '{}' / '{}' (talk={})",
                 characterName, outfit, animationName, talking);
}

void AnimationVideoCache::queueAllAnimations(const std::string& characterName,
                                               const std::string& outfit)
{
    // Load skeleton to enumerate animations
    SpineEngine engine;
    auto paths = SpineEngine::resolvePaths(m_assetsDir, characterName,
                                            outfit, CharacterStance::Default);
    if (!paths.valid) {
        spdlog::warn("AnimCache: cannot resolve paths for '{}' / '{}'",
                     characterName, outfit);
        return;
    }

    if (!engine.loadSkeleton(paths.skelPath, paths.atlasPath)) {
        spdlog::warn("AnimCache: cannot load skeleton for '{}' / '{}'",
                     characterName, outfit);
        return;
    }

    auto anims = engine.animation().listAnimations();

    // Detect whether this skeleton has a talk animation at all
    bool hasTalkAnim = !engine.animation().detectTalkAnimation().empty();

    for (const auto& anim : anims) {
        // Skip zero-duration animations (talk_end)
        if (anim.duration <= 0.0f) continue;
        // Skip talk_start (very short, not a body animation)
        if (anim.name == "talk_start") continue;

        // Queue the normal (non-talking) version
        queueRender(characterName, outfit, anim.name, false);

        // Queue the talking version if the skeleton has a talk animation
        if (hasTalkAnim) {
            queueRender(characterName, outfit, anim.name, true);
        }
    }
}

bool AnimationVideoCache::isRendering() const
{
    std::lock_guard lock(m_mutex);
    return !m_pendingKeys.empty();
}

size_t AnimationVideoCache::pendingCount() const
{
    std::lock_guard lock(m_mutex);
    return m_pendingKeys.size();
}

bool AnimationVideoCache::hasPendingForCharacterOutfit(const std::string& characterName,
                                                        const std::string& outfit) const
{
    std::lock_guard lock(m_mutex);
    std::string prefix = characterName + "|" + outfit + "|";
    for (const auto& key : m_pendingKeys) {
        if (key.compare(0, prefix.size(), prefix) == 0)
            return true;
    }
    return false;
}

std::string AnimationVideoCache::currentJobDescription() const
{
    std::lock_guard lock(m_mutex);
    return m_currentJobDesc;
}

void AnimationVideoCache::waitForAll()
{
    // Wait until the job queue is drained and no keys are pending
    std::unique_lock lock(m_mutex);
    m_jobCv.wait(lock, [this]() {
        return m_pendingKeys.empty() || m_stopWorker;
    });
}

void AnimationVideoCache::cancelAll()
{
    std::lock_guard lock(m_mutex);
    m_jobQueue.clear();
    m_pendingKeys.clear();
    // Note: if a job is currently rendering, it will complete.
    // The worker thread will find the queue empty and block.
}

// ─── Concurrent background worker threads ────────────────────────────────────────
// Multiple threads process render jobs concurrently. Each thread owns its
// own SpinePrerenderer with a per-thread CommandPool. All workers use GPU
// rasterization when Vulkan is available; queue submissions are serialised
// through GpuContext::graphicsQueueMutex().

void AnimationVideoCache::workerLoop()
{
    spdlog::info("AnimCache: worker thread started (tid={}, GPU rasterization enabled)",
                 std::hash<std::thread::id>{}(std::this_thread::get_id()) % 10000);

    // Each worker owns its own prerenderer — never shared between threads.
    std::unique_ptr<SpinePrerenderer> prerenderer;

    while (true) {
        RenderJob job;

        // Wait for a job or stop signal
        {
            std::unique_lock lock(m_mutex);
            m_jobCv.wait(lock, [this]() {
                return !m_jobQueue.empty() || m_stopWorker;
            });

            if (m_stopWorker) {
                spdlog::info("AnimCache: worker thread stopping");
                break;
            }

            job = std::move(m_jobQueue.front());
            m_jobQueue.pop_front();
        }

        // Talking variants use "{animName}_talk" for the cache key + output file
        const std::string cacheAnimName = job.isTalking
            ? (job.animName + "_talk") : job.animName;
        const std::string key = makeKey(job.charName, job.outfit, cacheAnimName);

        // Update current job description for UI display, and snapshot the
        // config the render section reads UNLOCKED below.  setEncoderFormat /
        // setChromaKeyColor / setCompletionCallback can fire on the UI thread
        // at any time; snapshotting under m_mutex serialises against them so
        // the unlocked render reads (and the std::function call) can't tear
        // (D1 / D3).
        SpineCacheFormat    encoderFmt;
        uint8_t             chromaR, chromaG, chromaB;
        AnimCacheCompleteFn completeFn;
        {
            std::lock_guard lock(m_mutex);
            m_currentJobDesc = job.charName + " / " + job.outfit + " / "
                             + job.animName + (job.isTalking ? " [talk]" : "");
            encoderFmt = m_encoderFormat;
            chromaR    = m_chromaKeyR;
            chromaG    = m_chromaKeyG;
            chromaB    = m_chromaKeyB;
            completeFn = m_completeFn;
        }

        try {
            // Create prerenderer lazily (owned entirely by this thread)
            if (!prerenderer) {
                prerenderer = std::make_unique<SpinePrerenderer>();
                prerenderer->setAssetsDir(m_assetsDir);
                // All workers use GPU when available — each has its own
                // CommandPool and queue submissions are mutex-serialised.
                prerenderer->setForceCPU(false);
            }

            PrerenderJob renderJob;
            renderJob.characterName = job.charName;
            renderJob.outfit        = job.outfit;
            renderJob.animationName = job.animName;   // actual Spine animation name (no _talk suffix)
            renderJob.isTalking     = job.isTalking;  // talk track blending flag
            renderJob.outputPath    = cachePath(job.charName, job.outfit, cacheAnimName, encoderFmt);
            renderJob.fps           = 60;
            // QP=22 ~halves bitrate vs QP=18 (~80 Mbps vs ~157 Mbps for 1632x3840
            // packed-alpha @60fps).  Lower bitrate = less PCIe traffic and less
            // NVDEC bandwidth contention when multiple characters share a shot,
            // which was causing 200-350ms decode hiccups visible as character
            // flickering / swapping.  Packed alpha is R=G=B=A so chroma is
            // constant; quality is dominated by Y precision and QP=22 is
            // visually lossless for this use.
            // CRF — chroma key formats use slightly lower CRF (better quality)
            // since the output is normal (not packed) height so bitrate is lower.
            renderJob.crf           = 22;
            // Map SpineCacheFormat → PrerenderFormat
            switch (encoderFmt) {
            case SpineCacheFormat::GreenScreen:
                renderJob.format = PrerenderFormat::GreenScreen;
                break;
            case SpineCacheFormat::BlueScreen:
                renderJob.format = PrerenderFormat::BlueScreen;
                break;
            case SpineCacheFormat::CustomColor:
                renderJob.format = PrerenderFormat::CustomColor;
                break;
            default:
                renderJob.format = PrerenderFormat::ProRes4444;
                break;
            }
            // Propagate chroma key colour
            renderJob.chromaKeyR = chromaR;
            renderJob.chromaKeyG = chromaG;
            renderJob.chromaKeyB = chromaB;

            // Create .rendering marker so an interrupted render is
            // detected and cleaned up on next startup.
            auto markerPath = renderJob.outputPath;
            markerPath.replace_extension(".rendering");
            {
                std::error_code ec;
                fs::create_directories(renderJob.outputPath.parent_path(), ec);
                // Touch the marker file
                std::ofstream marker(markerPath, std::ios::trunc);
            }

            auto result = prerenderer->render(renderJob);

            // Remove the marker now that the render finished
            // (whether success or fail — a failed render leaves no usable .webm)
            {
                std::error_code ec;
                fs::remove(markerPath, ec);
            }

            {
                std::lock_guard lock(m_mutex);
                m_pendingKeys.erase(key);

                if (result.success) {
                    // Check if this outfit was deleted mid-render.
                    // If so, discard the rendered file and skip the entry
                    // so the deletion is truly persistent.
                    std::string outfitKey = job.charName + "|" + job.outfit;
                    if (m_deletingOutfits.count(outfitKey)) {
                        std::error_code rmEc;
                        fs::remove(result.outputPath, rmEc);
                        spdlog::info(
                            "AnimCache: discarded render for deleted outfit "
                            "'{}' / '{}' / '{}'",
                            job.charName, job.outfit, cacheAnimName);

                        // Clean up the deletion guard if no more pending
                        // keys exist for this outfit.
                        std::string prefix = outfitKey + "|";
                        bool hasMore = false;
                        for (const auto& pk : m_pendingKeys) {
                            if (pk.compare(0, prefix.size(), prefix) == 0) {
                                hasMore = true;
                                break;
                            }
                        }
                        if (!hasMore)
                            m_deletingOutfits.erase(outfitKey);
                    } else {
                        AnimCacheEntry entry;
                        entry.characterName = job.charName;
                        entry.outfit        = job.outfit;
                        entry.animationName = cacheAnimName;  // includes _talk suffix for talking variants
                        entry.videoPath     = result.outputPath;
                        entry.width         = result.width;
                        entry.height        = result.height;
                        entry.duration      = result.duration;
                        entry.frameCount    = result.frameCount;
                        entry.fps           = renderJob.fps;
                        entry.fileSizeBytes = result.fileSizeBytes;

                        // Record skeleton mod time
                        auto skelPaths = SpineEngine::resolvePaths(
                            m_assetsDir, job.charName, job.outfit,
                            CharacterStance::Default);
                        if (skelPaths.valid) {
                            std::error_code ec;
                            entry.skelModTime = fs::last_write_time(
                                skelPaths.skelPath, ec);
                        }

                        m_entries[key] = std::move(entry);

                        spdlog::info("AnimCache: rendered '{}' / '{}' / '{}' → {}x{}, {} frames",
                                     job.charName, job.outfit, cacheAnimName,
                                     result.width, result.height, result.frameCount);

                        // If an .mp4 was produced, clean up any old .mov for the same entry
                        // (migration from ProRes → HEVC packed-alpha saves ~10-20× space).
                        if (result.outputPath.extension() == ".mp4") {
                            auto oldMov = result.outputPath;
                            oldMov.replace_extension(".mov");
                            std::error_code rmEc;
                            if (fs::exists(oldMov, rmEc)) {
                                fs::remove(oldMov, rmEc);
                                spdlog::info("AnimCache: removed old .mov after HEVC migration: {}",
                                             pathToUtf8(oldMov));
                            }
                        }
                    }
                } else {
                    spdlog::warn("AnimCache: render failed for '{}': {}",
                                 key, result.error);
                }
            }

            // Notify waitForAll() and anyone else waiting
            m_jobCv.notify_all();

            // Clear current job description
            {
                std::lock_guard lock2(m_mutex);
                m_currentJobDesc.clear();
            }

            if (completeFn) {
                completeFn(job.charName, job.outfit, cacheAnimName,
                           result.success);
            }

        } catch (const std::exception& ex) {
            spdlog::error("AnimCache: render failed for '{}': {}", key, ex.what());
            // Clean up marker if exception thrown during render
            {
                auto failMarker = cachePath(job.charName, job.outfit, cacheAnimName, encoderFmt);
                failMarker.replace_extension(".rendering");
                std::error_code ec;
                fs::remove(failMarker, ec);
            }
            {
                std::lock_guard lock(m_mutex);
                m_pendingKeys.erase(key);
            }
            m_jobCv.notify_all();
            if (completeFn) {
                completeFn(job.charName, job.outfit, cacheAnimName, false);
            }
        }
    }

    spdlog::info("AnimCache: worker thread exited");
}

// ─── Maintenance ─────────────────────────────────────────────────────────────

void AnimationVideoCache::clear()
{
    std::lock_guard lock(m_mutex);
    if (m_mediaPool) {
        for (auto& [key, entry] : m_entries) {
            if (entry.mediaHandle != 0) {
                m_mediaPool->release(entry.mediaHandle);
                entry.mediaHandle = 0;
            }
        }
    }
    m_entries.clear();
}

void AnimationVideoCache::removeEntry(const std::string& characterName,
                                       const std::string& outfit,
                                       const std::string& animationName)
{
    std::lock_guard lock(m_mutex);
    auto key = makeKey(characterName, outfit, animationName);
    auto it = m_entries.find(key);
    if (it == m_entries.end()) return;

    // Release media handle
    if (m_mediaPool && it->second.mediaHandle != 0)
        m_mediaPool->release(it->second.mediaHandle);

    // Delete corrupt file on disk
    std::error_code ec;
    if (fs::exists(it->second.videoPath, ec))
        fs::remove(it->second.videoPath, ec);

    spdlog::info("AnimCache: removed bad entry '{}'", key);
    m_entries.erase(it);
}

void AnimationVideoCache::removeAllForCharacter(const std::string& characterName)
{
    std::lock_guard lock(m_mutex);

    // Collect keys to remove (only this character)
    std::vector<std::string> keysToRemove;

    // Collect the distinct outfit keys so we can guard in-flight renders
    std::unordered_set<std::string> outfitKeys;

    for (const auto& [key, entry] : m_entries) {
        if (entry.characterName == characterName) {
            keysToRemove.push_back(key);
            outfitKeys.insert(characterName + "|" + entry.outfit);
        }
    }

    // Mark all outfits as being deleted so in-flight worker threads
    // discard their results instead of re-adding entries.
    for (const auto& ok : outfitKeys)
        m_deletingOutfits.insert(ok);

    // Release media handles and erase entries
    for (const auto& key : keysToRemove) {
        auto it = m_entries.find(key);
        if (it == m_entries.end()) continue;
        if (m_mediaPool && it->second.mediaHandle != 0)
            m_mediaPool->release(it->second.mediaHandle);
        m_entries.erase(it);
    }

    // Drop any pending render jobs for this character
    for (auto it = m_jobQueue.begin(); it != m_jobQueue.end(); ) {
        if (it->charName == characterName) {
            const std::string cacheAnimName =
                it->isTalking ? (it->animName + "_talk") : it->animName;
            m_pendingKeys.erase(makeKey(characterName, it->outfit, cacheAnimName));
            it = m_jobQueue.erase(it);
        } else {
            ++it;
        }
    }

    // Clear any pending keys for jobs already dequeued by workers
    std::string prefix = characterName + "|";
    for (auto it = m_pendingKeys.begin(); it != m_pendingKeys.end(); ) {
        if (it->compare(0, prefix.size(), prefix) == 0)
            it = m_pendingKeys.erase(it);
        else
            ++it;
    }

    // Clean up deletion guards for outfits with no remaining in-flight renders
    for (auto it = m_deletingOutfits.begin(); it != m_deletingOutfits.end(); ) {
        if (it->compare(0, prefix.size(), prefix) != 0) {
            ++it;
            continue;
        }
        // Check if any pending keys remain for this outfit
        std::string outfitPrefix = *it + "|";
        bool hasInFlight = false;
        for (const auto& pk : m_pendingKeys) {
            if (pk.compare(0, outfitPrefix.size(), outfitPrefix) == 0) {
                hasInFlight = true;
                break;
            }
        }
        if (!hasInFlight)
            it = m_deletingOutfits.erase(it);
        else
            ++it;
    }

    // Delete the character cache directories across all format subdirectories
    static const char* formatDirs[] = {"H264_Green", "H264_Blue", "H264_Custom", "ProRes"};
    std::error_code ec;
    for (const auto& fmtName : formatDirs) {
        auto charCacheDir = m_cacheDir / fmtName / characterName;
        if (fs::exists(charCacheDir, ec)) {
            fs::remove_all(charCacheDir, ec);
            if (!ec)
                spdlog::info("AnimCache: deleted cache dir '{}' for '{}'",
                             fmtName, characterName);
            else
                spdlog::warn("AnimCache: failed to delete cache dir '{}': {}",
                             pathToUtf8(charCacheDir), ec.message());
        }
    }

    spdlog::info("AnimCache: removed {} cache entries for '{}'",
                 keysToRemove.size(), characterName);
}

void AnimationVideoCache::removeAllForCharacterOutfit(const std::string& characterName,
                                                      const std::string& outfit)
{
    std::lock_guard lock(m_mutex);

    // Mark this outfit as being deleted so any in-flight worker thread
    // that completes a render AFTER this point will discard the result
    // instead of re-adding it to m_entries.
    std::string outfitKey = characterName + "|" + outfit;
    m_deletingOutfits.insert(outfitKey);

    // Collect keys to remove (only this character/outfit pair)
    std::vector<std::string> keysToRemove;
    for (const auto& [key, entry] : m_entries) {
        if (entry.characterName == characterName && entry.outfit == outfit)
            keysToRemove.push_back(key);
    }

    // Release media handles and erase entries
    for (const auto& key : keysToRemove) {
        auto it = m_entries.find(key);
        if (it == m_entries.end()) continue;
        if (m_mediaPool && it->second.mediaHandle != 0)
            m_mediaPool->release(it->second.mediaHandle);
        m_entries.erase(it);
    }

    // Drop any pending render jobs for this character/outfit
    // Also zap stale pending keys (jobs already dequeued by a worker
    // but not yet completed will be caught by the m_deletingOutfits check).
    for (auto it = m_jobQueue.begin(); it != m_jobQueue.end(); ) {
        if (it->charName == characterName && it->outfit == outfit) {
            const std::string cacheAnimName =
                it->isTalking ? (it->animName + "_talk") : it->animName;
            m_pendingKeys.erase(makeKey(characterName, outfit, cacheAnimName));
            it = m_jobQueue.erase(it);
        } else {
            ++it;
        }
    }

    // Also clear any pending keys for jobs already dequeued by workers
    // (the in-flight worker will check m_deletingOutfits and discard).
    {
        std::string prefix = outfitKey + "|";
        for (auto it = m_pendingKeys.begin(); it != m_pendingKeys.end(); ) {
            if (it->compare(0, prefix.size(), prefix) == 0)
                it = m_pendingKeys.erase(it);
            else
                ++it;
        }
    }

    // Delete the outfit subdirectory across all format subdirectories.
    // Does NOT touch assets/characters/<character>/ — Spine / Live2D source
    // files remain intact.
    static const char* formatDirs[] = {"H264_Green", "H264_Blue", "H264_Custom", "ProRes"};
    std::error_code ec;
    for (const auto& fmtName : formatDirs) {
        auto outfitCacheDir = m_cacheDir / fmtName / characterName / outfit;
        if (fs::exists(outfitCacheDir, ec)) {
            fs::remove_all(outfitCacheDir, ec);
            if (!ec)
                spdlog::info("AnimCache: deleted outfit cache dir for '{}' / '{}' in '{}'",
                             characterName, outfit, fmtName);
            else
                spdlog::warn("AnimCache: failed to delete outfit cache dir '{}': {}",
                             pathToUtf8(outfitCacheDir), ec.message());
        }
    }

    // If no in-flight renders remain for this outfit, we can safely
    // remove the deletion guard.  If a render IS in-flight (already
    // dequeued by the worker), the worker will check m_deletingOutfits
    // and clean up after itself.
    {
        std::string prefix = outfitKey + "|";
        bool hasInFlight = false;
        for (const auto& pk : m_pendingKeys) {
            if (pk.compare(0, prefix.size(), prefix) == 0) {
                hasInFlight = true;
                break;
            }
        }
        if (!hasInFlight)
            m_deletingOutfits.erase(outfitKey);
    }

    spdlog::info("AnimCache: removed {} cache entries for '{}' / '{}'",
                 keysToRemove.size(), characterName, outfit);
}

void AnimationVideoCache::recheckStaleness()
{
    std::lock_guard lock(m_mutex);

    for (auto it = m_entries.begin(); it != m_entries.end(); ) {
        auto& entry = it->second;

        auto skelPaths = SpineEngine::resolvePaths(
            m_assetsDir, entry.characterName, entry.outfit,
            CharacterStance::Default);
        if (!skelPaths.valid) {
            ++it;
            continue;
        }

        std::error_code ec;
        auto currentModTime = fs::last_write_time(skelPaths.skelPath, ec);
        if (ec) {
            ++it;
            continue;
        }

        if (currentModTime > entry.skelModTime) {
            spdlog::info("AnimCache: stale entry '{}' / '{}' / '{}' — re-rendering",
                         entry.characterName, entry.outfit, entry.animationName);

            // Close media handle
            if (m_mediaPool && entry.mediaHandle != 0) {
                m_mediaPool->release(entry.mediaHandle);
            }

            // Remove entry and queue re-render
            std::string charName = entry.characterName;
            std::string outfit = entry.outfit;
            std::string animName = entry.animationName;
            it = m_entries.erase(it);

            // Queue via the job queue (will be processed by workerLoop)
            m_pendingKeys.insert(makeKey(charName, outfit, animName));
            m_jobQueue.push_back({charName, outfit, animName});

            // Ensure worker thread pool is running (1 worker: GPU session limits)
            if (m_workerThreads.empty()) {
                m_workerThreads.emplace_back(&AnimationVideoCache::workerLoop, this);
            }
        } else {
            ++it;
        }
    }
}

void AnimationVideoCache::migrateToProRes()
{
    std::lock_guard lock(m_mutex);

    std::vector<std::string> keysToMigrate;

    for (const auto& [key, entry] : m_entries) {
        if (entry.videoPath.extension() == ".mp4") {
            keysToMigrate.push_back(key);
        }
    }

    if (keysToMigrate.empty()) {
        spdlog::info("AnimCache: no legacy .mp4 files to migrate");
        return;
    }

    spdlog::info("AnimCache: migrating {} HEVC .mp4 cache files to ProRes 4444 .mov",
                 keysToMigrate.size());

    for (const auto& key : keysToMigrate) {
        auto it = m_entries.find(key);
        if (it == m_entries.end()) continue;

        auto& entry = it->second;

        // Save info for re-render
        std::string charName = entry.characterName;
        std::string outfit   = entry.outfit;
        std::string animName = entry.animationName;

        // Detect _talk suffix: cache stores talking variants as "anim_talk"
        // but the actual Spine animation is just "anim" with isTalking=true.
        bool isTalking = false;
        if (animName.size() > 5 && animName.substr(animName.size() - 5) == "_talk") {
            animName = animName.substr(0, animName.size() - 5);
            isTalking = true;
        }

        // Don't delete the .mp4 file yet — only remove it after the .mov
        // re-render succeeds.  The completion callback handles cleanup.
        // For now, remove the entry from m_entries so a new .mov entry
        // can be created on successful render.
        std::filesystem::path mp4Path = entry.videoPath;

        // Release media handle
        if (m_mediaPool && entry.mediaHandle != 0) {
            m_mediaPool->release(entry.mediaHandle);
        }

        m_entries.erase(it);

        // Queue re-render (will produce .mov ProRes now)
        std::string cacheAnimName = isTalking ? (animName + "_talk") : animName;
        std::string reKey = makeKey(charName, outfit, cacheAnimName);
        if (m_pendingKeys.count(reKey) == 0) {
            m_pendingKeys.insert(reKey);
            m_jobQueue.push_back({charName, outfit, animName, isTalking});
        }
    }

    // Ensure worker thread pool is running (1 worker: GPU session limits)
    if (!m_jobQueue.empty() && m_workerThreads.empty()) {
        m_workerThreads.emplace_back(&AnimationVideoCache::workerLoop, this);
    }

    // Notify workers
    m_jobCv.notify_all();
}

} // namespace rt

#endif // ROUNDTABLE_HAS_SPINE
