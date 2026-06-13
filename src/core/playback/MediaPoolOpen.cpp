/*
 * MediaPoolOpen.cpp — File open/resolution and release for MediaPool.
 * Extracted from MediaPool.cpp for maintainability.
 */

#include "MediaPool.h"
#include "PathUtils.h"

#include <cstring>
#include <algorithm>
#include <chrono>
#include <spdlog/spdlog.h>

#include "GpuContext.h"
#include "Nv12Converter.h"
#include "cuda/CudaVulkanInterop.h"

#include <volk.h>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#endif

#ifdef ROUNDTABLE_HAS_FFMPEG
extern "C" {
#include <libswscale/swscale.h>
#include <libavutil/pixfmt.h>
}
#endif

namespace rt {

// ─── ProRes codec detection ────────────────────────────────────────────────
static bool isProResCodec(const std::string& codecName)
{
    return codecName.find("prores") != std::string::npos;
}

// ─── Open ───────────────────────────────────────────────────────────────────

MediaHandle MediaPool::open(const std::string& utf8Path)
{
    return open(utf8ToPath(utf8Path));
}

MediaHandle MediaPool::open(const std::filesystem::path& filePath)
{
    auto openT0 = std::chrono::steady_clock::now();

    // Canonicalize the path for dedup (filesystem I/O — outside lock)
    std::error_code ec;
    auto canonical = std::filesystem::canonical(filePath, ec);
    std::string key = ec ? pathToUtf8(filePath) : pathToUtf8(canonical);

    // Current on-disk mtime (one stat; canonical() above already did I/O on
    // this path).  Used to self-heal stale stream info when a file is
    // regenerated in place.  On a missing file canonical() fails → wtEc set →
    // haveWriteTime=false, so the self-heal never false-triggers.
    std::error_code wtEc;
    const auto currentWriteTime =
        std::filesystem::last_write_time(ec ? filePath : canonical, wtEc);
    const bool haveWriteTime = !wtEc;

    // ── Fast path: already open or known-failed? ─────────────────────
    bool needsReprobe = false;
    {
        std::lock_guard lock(m_mutex);

        // Return immediately for paths that already failed to open.
        // Prevents per-frame filesystem I/O for missing media files.
        if (m_failedPaths.count(key)) {
            return InvalidMedia;
        }

        auto pathIt = m_pathToHandle.find(key);
        if (pathIt != m_pathToHandle.end()) {
            auto entryIt = m_entries.find(pathIt->second);
            if (entryIt != m_entries.end()) {
                // Self-heal: if the file changed on disk since we cached it
                // (a converted/_H264 file re-rendered in place; or a change
                // the live file-swap watcher didn't catch), re-probe so
                // entry.info — hasAudio, dimensions, fps — is current instead
                // of stale for the whole session.
                const bool fileChanged = haveWriteTime
                    && entryIt->second.lastWriteTime != std::filesystem::file_time_type{}
                    && currentWriteTime != entryIt->second.lastWriteTime;
                if (!fileChanged) {
                    entryIt->second.refCount++;
                    spdlog::debug("MediaPool: reusing handle {} for '{}' (refCount={})",
                                  pathIt->second, pathToUtf8(filePath.filename()),
                                  entryIt->second.refCount);
                    return pathIt->second;
                }
                needsReprobe = true;
            }
        }
    }

    // File regenerated in place — re-point the SAME handle at the new content.
    // invalidatePath refreshes entry.info + reopens the decoder + evicts the
    // CPU/disk frame caches, keeping the handle valid so existing clip/handle
    // holders keep working.  Done OUTSIDE m_mutex (invalidatePath takes it).
    if (needsReprobe) {
        MediaHandle h = invalidatePath(filePath, /*reopenDecoder=*/true);
        if (h != InvalidMedia) {
            std::lock_guard lock(m_mutex);
            auto entryIt = m_entries.find(h);
            if (entryIt != m_entries.end()) {
                entryIt->second.refCount++;
                spdlog::warn("MediaPool: '{}' changed on disk — re-probed stale "
                             "stream info (handle {}, hasAudio now {})",
                             pathToUtf8(filePath.filename()), h,
                             entryIt->second.info.hasAudio);
                return h;
            }
        }
        // Entry vanished during re-probe — fall through to a fresh open below.
    }

    // ── Slow path: open decoder WITHOUT holding the mutex ────────────
    // The ffmpeg avformat_open_input() probe can take 50-200+ ms.
    // Holding m_mutex during the probe would block prefetch workers,
    // thumbnail generation, and other concurrent MediaPool callers.
    auto decoder = std::make_unique<VideoDecoder>();
    std::filesystem::path actualFilePath = filePath;
    bool directOpen = false;
    {
        namespace fs = std::filesystem;
        std::error_code existsEc;
        if (fs::exists(filePath, existsEc))
            directOpen = decoder->open(filePath, /*forceSoftware=*/false, /*maxThreads=*/4);
    }
    if (!directOpen) {
        // ── Search common asset directories for the file ────────────────
        namespace fs = std::filesystem;
        fs::path filename = filePath.filename();

        // Build a list of candidate paths to try
        std::vector<fs::path> candidates;

        // Alternate video extensions — prefer .mp4 (NVDEC hw decode) over .mov (ProRes sw decode)
        std::vector<fs::path> altExts;
        if (filePath.extension() == ".webm") {
            altExts.push_back(fs::path(filePath).replace_extension(".mp4"));
            altExts.push_back(fs::path(filePath).replace_extension(".mov"));
        } else if (filePath.extension() == ".mov") {
            altExts.push_back(fs::path(filePath).replace_extension(".mp4"));
            altExts.push_back(fs::path(filePath).replace_extension(".webm"));
        } else if (filePath.extension() == ".mp4") {
            altExts.push_back(fs::path(filePath).replace_extension(".webm"));
            altExts.push_back(fs::path(filePath).replace_extension(".mov"));
        }

        // Common image extensions — for presets that reference background
        // images as bare filenames without extension (e.g. "TABLE_LARGE_FINAL")
        const std::vector<fs::path> imgExts = {".png", ".jpg", ".jpeg",
                                               ".webp", ".bmp", ".tga"};
        for (const auto& imgExt : imgExts) {
            altExts.push_back(fs::path(filePath).replace_extension(imgExt));
        }

        // Asset search directories
        const fs::path searchDirs[] = {
            fs::path("assets") / "backgrounds",
            fs::path("assets") / "videos",
            fs::path("assets"),
        };

        // Original filename in each search dir
        for (const auto& dir : searchDirs)
            candidates.push_back(dir / filename);

        // Alternate extensions at original path and in search dirs
        for (const auto& altExt : altExts) {
            candidates.push_back(altExt);
            fs::path altName = altExt.filename();
            for (const auto& dir : searchDirs)
                candidates.push_back(dir / altName);
        }

        bool found = false;
        for (const auto& candidate : candidates) {
            std::error_code ec2;
            if (!fs::exists(candidate, ec2)) continue;
            decoder = std::make_unique<VideoDecoder>();
            if (decoder->open(candidate, /*forceSoftware=*/false, /*maxThreads=*/4)) {
                spdlog::info("MediaPool: resolved '{}' → '{}'",
                             pathToUtf8(filePath), pathToUtf8(candidate));
                actualFilePath = candidate;
                found = true;
                break;
            }
        }

        if (!found) {
            // Only cache as permanently failed if the file genuinely
            // doesn't exist anywhere on disk.  Decoder-open failures on
            // existing files (transient: locked by scanner, slow external
            // drive spin-up, NVDEC init race) must be retried — caching
            // them permanently makes the file unreachable for the rest of
            // the session.
            bool fileExistsOnDisk = false;
            {
                std::error_code ec3;
                fileExistsOnDisk = fs::exists(filePath, ec3);
                if (!fileExistsOnDisk) {
                    for (const auto& candidate : candidates) {
                        if (fs::exists(candidate, ec3)) {
                            fileExistsOnDisk = true;
                            break;
                        }
                    }
                }
            }
            if (!fileExistsOnDisk) {
                spdlog::error("MediaPool: failed to open '{}' (file missing, caching as failed)", pathToUtf8(filePath));
                std::lock_guard lock(m_mutex);
                m_failedPaths.insert(key);
            } else {
                spdlog::warn("MediaPool: failed to open '{}' (file exists but decoder failed — will retry)", pathToUtf8(filePath));
            }
            return InvalidMedia;
        }
    }

    // ── ProRes native decode (no transcoding) ──────────────────────────
    // ProRes 4444 is our preferred format for alpha video: intra-frame codec with
    // native YUVA alpha.  Software decode is fast enough for 1080p on modern CPUs.
    // No auto-transcode — keep the ProRes quality and instant random access.
    std::filesystem::path actualPath = actualFilePath;
    if (isProResCodec(decoder->info().codecName))
    {
        spdlog::info("MediaPool: using ProRes native decode for '{}' (sw, native alpha)",
                     pathToUtf8(filePath.filename()));
        // Fall through — use the software decoder as-is.
        // ProRes 4444 intra-frame: every frame is a keyframe, instant random access,
        // native YUVA444P10LE alpha.  No transcoding needed.
    }

    // Re-acquire lock to store the new entry.
    // Double-check dedup: another thread may have opened the same file
    // while we were probing without the lock.
    std::unique_lock<std::mutex> lock(m_mutex);

    {
        auto pathIt2 = m_pathToHandle.find(key);
        if (pathIt2 != m_pathToHandle.end()) {
            auto entryIt2 = m_entries.find(pathIt2->second);
            if (entryIt2 != m_entries.end()) {
                entryIt2->second.refCount++;
                spdlog::debug("MediaPool: race dedup — reusing handle {} for '{}'",
                              pathIt2->second, pathToUtf8(filePath.filename()));
                return pathIt2->second;
            }
        }
    }

    MediaHandle handle = m_nextHandle++;
    MediaEntry entry;
    entry.handle      = handle;
    entry.path        = actualPath;
    entry.info        = decoder->info();
    entry.packedAlpha = entry.info.packedAlpha;  // propagate packed-alpha detection
    entry.decoder     = std::move(decoder);
    entry.refCount    = 1;
    // Capture the actually-opened file's mtime so open() can detect a later
    // in-place regeneration and re-probe (see MediaEntry::lastWriteTime).
    {
        std::error_code wec;
        auto wt = std::filesystem::last_write_time(actualPath, wec);
        if (!wec) entry.lastWriteTime = wt;
    }

    spdlog::info("MediaPool: opened '{}' handle={} ({}x{}, {:.2f}fps, {:.1f}s, {} frames, hw={})",
                 pathToUtf8(actualPath.filename()), handle,
                 entry.info.width, entry.info.height,
                 entry.info.fps, entry.info.duration, entry.info.frameCount,
                 entry.decoder->isHardwareAccelerated() ? "yes" : "no");

    m_pathToHandle[key] = handle;

    // If we transcoded, also map the original path so future opens of the
    // .mov file find the transcoded entry directly.
    if (actualPath != filePath)
    {
        std::error_code ecOrig;
        auto origCanonical = std::filesystem::canonical(filePath, ecOrig);
        std::string origKey = ecOrig ? pathToUtf8(filePath) : pathToUtf8(origCanonical);
        m_pathToHandle[origKey] = handle;
    }

    // ── Premiere-style live replacement: don't hold a handle on stills ──
    // A still image is decoded exactly once and pinned in the cache, so the
    // probe decoder opened above is never consulted again.  Releasing its
    // OS file HANDLE here means a still added to the project (bin/timeline)
    // never locks the file in Explorer.  If the pinned cache entry is ever
    // evicted, decodeFrame()'s isStillImage branch transparently reopens
    // via close()+open(), decodes, then releases the handle again.
    const bool entryIsStill =
        (entry.info.duration <= 0.0 || entry.info.frameCount <= 1);
    if (entryIsStill && entry.decoder) {
        entry.decoder->close();
        spdlog::debug("MediaPool: released probe handle for still image '{}' "
                     "(handle={}) — live replace enabled",
                     pathToUtf8(entry.path.filename()), handle);
    }

    m_entries.emplace(handle, std::move(entry));

    // Register with disk cache for persistent cross-session caching
    if (m_diskCache)
        m_diskCache->registerMedia(handle, actualPath);

    {
        double openMs = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - openT0).count();
        if (openMs > 20.0)
            spdlog::info("[PERF] MediaPool::open '{}': {:.0f}ms",
                         pathToUtf8(filePath.filename()), openMs);
    }

    // Notify the live file-swap watcher about the just-opened file, OUTSIDE
    // m_mutex (the handler marshals to the GUI thread to arm a watch). This
    // covers every open path — timeline clips of any subtype, bin/source
    // previews, prewarm/lookahead opens — not just enumerable clip types.
    auto notify = m_onMediaOpened;
    lock.unlock();
    if (notify) notify(actualPath);

    return handle;
}

// ─── Release / Close ───────────────────────────────────────────────────────

void MediaPool::release(MediaHandle handle)
{
    std::lock_guard lock(m_mutex);

    auto it = m_entries.find(handle);
    if (it == m_entries.end()) return;

    it->second.refCount--;
    if (it->second.refCount <= 0) {
        spdlog::info("MediaPool: closing handle {} '{}'",
                     handle, pathToUtf8(it->second.path.filename()));

        // Remove from path map
        std::error_code ec;
        auto canonical = std::filesystem::canonical(it->second.path, ec);
        std::string key = ec ? pathToUtf8(it->second.path) : pathToUtf8(canonical);
        m_pathToHandle.erase(key);

        // Evict cached frames for this media
        m_cache->evictMedia(handle);

        // Free cached sws context and close decoder
#ifdef ROUNDTABLE_HAS_FFMPEG
        if (it->second.swsCtx)
            sws_freeContext(static_cast<SwsContext*>(it->second.swsCtx));
#endif
        m_entries.erase(it);

        // Also clean up the prefetch decoder for this handle
        {
            std::lock_guard pfLock(m_prefetchMutex);
            // Remove queued tasks for this handle
            m_prefetchQueue.erase(
                std::remove_if(m_prefetchQueue.begin(), m_prefetchQueue.end(),
                    [handle](const PrefetchTask& t) { return t.handle == handle; }),
                m_prefetchQueue.end());
        }
        // Cancel any pending scheduler work for this handle.
        m_scheduler.cancel(handle);

        // Clean up dedicated scrub decoder for this handle
        auto scrubIt = m_scrubDecoders.find(handle);
        if (scrubIt != m_scrubDecoders.end()) {
#ifdef ROUNDTABLE_HAS_FFMPEG
            if (scrubIt->second.swsCtx)
                sws_freeContext(static_cast<SwsContext*>(scrubIt->second.swsCtx));
#endif
            m_scrubDecoders.erase(scrubIt);
        }
    }
}

void MediaPool::closeAll()
{
    // Flush the prefetch queue first
    {
        std::lock_guard pfLock(m_prefetchMutex);
        m_prefetchQueue.clear();
    }
    // Cancel all pending scheduler work (e.g. on seek / project close).
    m_scheduler.cancelAll();

    std::lock_guard lock(m_mutex);
#ifdef ROUNDTABLE_HAS_FFMPEG
    for (auto& [h, entry] : m_entries) {
        if (entry.swsCtx)
            sws_freeContext(static_cast<SwsContext*>(entry.swsCtx));
    }
#endif
    m_entries.clear();
    m_pathToHandle.clear();
    m_cache->clear();
    // Clean up scrub decoders
#ifdef ROUNDTABLE_HAS_FFMPEG
    for (auto& [h, s] : m_scrubDecoders) {
        if (s.swsCtx)
            sws_freeContext(static_cast<SwsContext*>(s.swsCtx));
        s.swsCtx = nullptr;
    }
#endif
    m_scrubDecoders.clear();
}

MediaHandle MediaPool::invalidatePath(const std::filesystem::path& filePath,
                                         bool reopenDecoder)
{
    // Canonicalize identically to open() so we hit the same map key.
    std::error_code ec;
    auto canonical = std::filesystem::canonical(filePath, ec);
    std::string key = ec ? pathToUtf8(filePath) : pathToUtf8(canonical);

    // Flush queued prefetch work for the (about to be closed) handle.
    MediaHandle handle = InvalidMedia;
    {
        std::lock_guard lock(m_mutex);
        m_failedPaths.erase(key);  // allow a fresh open attempt
        auto pathIt = m_pathToHandle.find(key);
        if (pathIt == m_pathToHandle.end()) {
            spdlog::warn("MediaPool::invalidatePath: '{}' (key '{}') NOT in "
                         "open-handle map — nothing to invalidate (path-key "
                         "mismatch or not yet opened)",
                         pathToUtf8(filePath), key);
            return InvalidMedia;    // not open — next open() reads fresh
        }
        handle = pathIt->second;
    }

    {
        std::lock_guard pfLock(m_prefetchMutex);
        m_prefetchQueue.erase(
            std::remove_if(m_prefetchQueue.begin(), m_prefetchQueue.end(),
                [handle](const PrefetchTask& t) { return t.handle == handle; }),
            m_prefetchQueue.end());
    }
    m_scheduler.cancel(handle);

    // Drop the per-handle stale-frame fallback.  tryGetFrame() (the
    // compositor's non-blocking path) returns m_lastGoodFrame[handle] on a
    // cache miss instead of forcing a decode — so if we don't clear it
    // here, every post-invalidation composite keeps serving the OLD pixels
    // and the new file is never decoded (exactly the "doesn't refresh"
    // bug: the log shows invalidate + reopen but no subsequent decode).
    // Separate lock scope, taken BEFORE m_mutex, to avoid lock-order
    // inversion with the frame-access paths.
    {
        std::lock_guard lg(m_lastGoodMtx);
        m_lastGoodFrame.erase(handle);
    }

    std::lock_guard lock(m_mutex);
    auto it = m_entries.find(handle);
    if (it == m_entries.end())
        return handle;  // entry gone but GPU textures keyed by handle may
                        // still exist — let the caller evict them

    MediaEntry& entry = it->second;

    // Re-point the SAME handle at the (now changed) file rather than
    // erasing it.  Every consumer — the compositor's path→handle cache,
    // the Source Monitor, ImageClip/VideoClip — holds this handle by value;
    // erasing it would leave them resolving a dead handle (getFrame →
    // nullptr → they keep showing the last GPU texture, i.e. the OLD
    // image, which is exactly the "doesn't refresh" bug).  Keeping the
    // handle valid and re-opening its decoder means the very next
    // getFrame() transparently decodes the new file for all of them, with
    // zero path-string matching anywhere (which was fragile: the watcher
    // delivers 'G:\..\F.png' while the compositor keyed 'G:/../F.png').
    spdlog::warn("MediaPool: invalidating handle {} '{}' (file changed) — "
                 "evicting CPU+disk cache, {}decoder, "
                 "next getFrame re-decodes for all consumers",
                 handle, pathToUtf8(entry.path.filename()),
                 reopenDecoder ? "reopening " : "HANDLE released (not reopening) ");

    m_cache->evictMedia(handle);          // drops the pinned still frame too
    if (m_diskCache)
        m_diskCache->evictMedia(handle);  // mtime-keyed, but be explicit

#ifdef ROUNDTABLE_HAS_FFMPEG
    if (entry.swsCtx) {
        sws_freeContext(static_cast<SwsContext*>(entry.swsCtx));
        entry.swsCtx = nullptr;
    }
#endif
    entry.swsSrcW = entry.swsSrcH = 0;
    entry.swsSrcFmt = -1;
    entry.swsDstW = entry.swsDstH = 0;
    entry.lastDecodedFrame   = -1;
    entry.decodePathLogged   = 0;
    entry.loopPreDecodeStarted = false;

    // Reopen the decoder on the new file so video seeks work immediately
    // and entry.info reflects any new dimensions.  (Stills additionally
    // close()+open() inside decodeFrame()'s isStillImage branch, which is
    // harmless on an already-open decoder.)  If the file is momentarily
    // unreadable mid-write, getFrame() fails gracefully and a later
    // composite retries.
    if (entry.decoder) {
        entry.decoder->close();
        if (reopenDecoder) {
            if (entry.decoder->open(entry.path)) {
                entry.info        = entry.decoder->info();
                entry.packedAlpha = entry.info.packedAlpha;
                // Track the refreshed file's mtime so open() doesn't re-probe
                // the same change again on its next call.
                std::error_code wec;
                auto wt = std::filesystem::last_write_time(entry.path, wec);
                if (!wec) entry.lastWriteTime = wt;
            } else {
                spdlog::warn("MediaPool: invalidate reopen failed for '{}' "
                             "(handle {}) — will retry on next getFrame",
                             pathToUtf8(entry.path.filename()), handle);
            }
        } else {
            spdlog::warn("MediaPool: invalidate '{}' — HANDLE released, "
                         "decoder left closed (live-replace safe mode)",
                         pathToUtf8(entry.path.filename()));
        }
    }

    // Drop the dedicated scrub decoder so it reopens against the new file.
    auto scrubIt = m_scrubDecoders.find(handle);
    if (scrubIt != m_scrubDecoders.end()) {
#ifdef ROUNDTABLE_HAS_FFMPEG
        if (scrubIt->second.swsCtx)
            sws_freeContext(static_cast<SwsContext*>(scrubIt->second.swsCtx));
#endif
        m_scrubDecoders.erase(scrubIt);
    }
    // m_pathToHandle intentionally left intact — handle stays valid.
    return handle;  // caller evicts this media's GPU textures (keyed by
                    // mediaId): since the handle is preserved, the GPU
                    // texture cache key (handle,frame,tier) is unchanged
                    // and would otherwise serve the stale uploaded texture.
}

// ─── Async open worker ─────────────────────────────────────────────────────

void MediaPool::closePath(const std::filesystem::path& filePath)
{
    // Try the raw path string first (fileChanged delivers the exact path
    // that was passed to addPath).  Fall back to canonical if needed.
    // This avoids mismatches when canonical() fails because the file was
    // just deleted (Explorer overwrite) — canonical() returns the raw
    // path on error, but the stored key was created when the file existed.
    std::string rawKey = pathToUtf8(filePath);
    std::error_code ec;
    auto canonical = std::filesystem::canonical(filePath, ec);
    std::string canonicalKey = ec ? rawKey : pathToUtf8(canonical);

    MediaHandle handle = InvalidMedia;
    {
        std::lock_guard lock(m_mutex);
        // Try raw path first (most likely match from fileChanged/addPath)
        auto pathIt = m_pathToHandle.find(rawKey);
        if (pathIt == m_pathToHandle.end() && canonicalKey != rawKey)
            pathIt = m_pathToHandle.find(canonicalKey);
        if (pathIt == m_pathToHandle.end()) {
            spdlog::warn("MediaPool::closePath: '{}' NOT in open-handle map "
                         "(raw='{}' canonical='{}') — handle already released "
                         "or never opened",
                         pathToUtf8(filePath.filename()), rawKey, canonicalKey);
            return;
        }
        handle = pathIt->second;
    }

    // Cancel any queued prefetch tasks for this handle so a background
    // worker doesn't reopen the decoder while Explorer is mid-overwrite.
    {
        std::lock_guard pfLock(m_prefetchMutex);
        m_prefetchQueue.erase(
            std::remove_if(m_prefetchQueue.begin(), m_prefetchQueue.end(),
                [handle](const PrefetchTask& t) { return t.handle == handle; }),
            m_prefetchQueue.end());
    }
    m_scheduler.cancel(handle);

    // Close the main decoder to release the shared-mode file HANDLE.
    {
        std::lock_guard lock(m_mutex);
        auto it = m_entries.find(handle);
        if (it != m_entries.end() && it->second.decoder) {
            it->second.decoder->close();
            spdlog::warn("MediaPool::closePath: released HANDLE for '{}' "
                         "(handle={})", pathToUtf8(filePath.filename()), handle);
        } else {
            spdlog::warn("MediaPool::closePath: '{}' found in path map "
                         "(handle={}) but entry decoder already closed",
                         pathToUtf8(filePath.filename()), handle);
        }
    }

    // Close the dedicated scrub decoder too.
    auto scrubIt = m_scrubDecoders.find(handle);
    if (scrubIt != m_scrubDecoders.end()) {
#ifdef ROUNDTABLE_HAS_FFMPEG
        if (scrubIt->second.swsCtx)
            sws_freeContext(static_cast<SwsContext*>(scrubIt->second.swsCtx));
#endif
        m_scrubDecoders.erase(scrubIt);
    }
}

bool MediaPool::isPathOpen(const std::filesystem::path& filePath) const
{
    std::error_code ec;
    auto canonical = std::filesystem::canonical(filePath, ec);
    std::string key = ec ? pathToUtf8(filePath) : pathToUtf8(canonical);
    std::lock_guard lock(m_mutex);
    return m_pathToHandle.find(key) != m_pathToHandle.end();
}

std::vector<std::filesystem::path> MediaPool::openMediaPaths() const
{
    std::lock_guard lock(m_mutex);
    std::vector<std::filesystem::path> paths;
    paths.reserve(m_pathToHandle.size());
    for (const auto& kv : m_pathToHandle)
        paths.emplace_back(utf8ToPath(kv.first));
    return paths;
}

void MediaPool::setOnMediaOpened(std::function<void(std::filesystem::path)> cb)
{
    std::lock_guard lock(m_mutex);
    m_onMediaOpened = std::move(cb);
}

void MediaPool::clearFailedPaths()
{
    std::lock_guard lock(m_mutex);
    size_t count = m_failedPaths.size();
    m_failedPaths.clear();
    if (count > 0)
        spdlog::info("MediaPool: cleared failed-paths cache ({} entries)", count);
}

void MediaPool::openAsync(const std::filesystem::path& filePath)
{
    if (filePath.empty()) return;
    if (!m_openWorkerRunning.load(std::memory_order_acquire)) return;

    std::error_code ec;
    auto canonical = std::filesystem::canonical(filePath, ec);
    std::string key = ec ? pathToUtf8(filePath) : pathToUtf8(canonical);

    // Skip if already open or in flight or known-failed.
    {
        std::lock_guard mlk(m_mutex);
        if (m_pathToHandle.find(key) != m_pathToHandle.end()) return;
        if (m_failedPaths.count(key))                         return;
    }
    {
        std::lock_guard wlk(m_openWorkerMutex);
        if (!m_openWorkerInFlight.insert(key).second) return; // already queued
        m_openWorkerQueue.push_back(filePath);
    }
    m_openWorkerCv.notify_one();
}

void MediaPool::startOpenWorker()
{
    m_openWorkerRunning.store(true, std::memory_order_release);
    m_openWorker = std::thread([this]{ openWorkerLoop(); });
}

void MediaPool::stopOpenWorker()
{
    m_openWorkerRunning.store(false, std::memory_order_release);
    m_openWorkerCv.notify_all();
    if (m_openWorker.joinable())
        m_openWorker.join();
    std::lock_guard wlk(m_openWorkerMutex);
    m_openWorkerQueue.clear();
    m_openWorkerInFlight.clear();
}

void MediaPool::openWorkerLoop()
{
    while (m_openWorkerRunning.load(std::memory_order_acquire)) {
        std::filesystem::path path;
        {
            std::unique_lock wlk(m_openWorkerMutex);
            m_openWorkerCv.wait(wlk, [this]{
                return !m_openWorkerQueue.empty()
                    || !m_openWorkerRunning.load(std::memory_order_acquire);
            });
            if (!m_openWorkerRunning.load(std::memory_order_acquire))
                return;
            path = std::move(m_openWorkerQueue.front());
            m_openWorkerQueue.pop_front();
        }

        // Perform the actual open (heavy: NVDEC init etc).
        auto t0 = std::chrono::steady_clock::now();
        MediaHandle h = open(path);
        auto ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - t0).count();
        if (h != InvalidMedia) {
            spdlog::info("[PERF] MediaPool::openAsync '{}': {:.0f}ms (handle={})",
                         pathToUtf8(path.filename()), ms, h);
        } else {
            spdlog::warn("[PERF] MediaPool::openAsync '{}': FAILED after {:.0f}ms",
                         pathToUtf8(path.filename()), ms);
        }

        // Clear in-flight marker.
        std::error_code ec;
        auto canonical = std::filesystem::canonical(path, ec);
        std::string key = ec ? pathToUtf8(path) : pathToUtf8(canonical);
        std::lock_guard wlk(m_openWorkerMutex);
        m_openWorkerInFlight.erase(key);
    }
}

void MediaPool::setPackedAlpha(MediaHandle handle, bool packed)
{
    std::lock_guard lock(m_mutex);
    auto* entry = findEntry(handle);
    if (entry)
        entry->packedAlpha = packed;
}

} // namespace rt
