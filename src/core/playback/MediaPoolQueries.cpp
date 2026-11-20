/*
 * MediaPoolQueries.cpp — Queries and internal helpers for MediaPool.
 * Extracted from MediaPool.cpp for maintainability.
 */

#include "MediaPool.h"
#include "playback/FrameFallbackPolicy.h"

#include <algorithm>
#include <cmath>

namespace rt {

void MediaPool::setProjectFps(double fps)
{
    m_projectFps.store(fps > 0.0 ? fps : 30.0, std::memory_order_relaxed);
}

void MediaPool::setPlaybackSpeed(double speed)
{
    m_playbackSpeed.store(speed, std::memory_order_relaxed);
}

void MediaPool::noteClipFrameRate(MediaHandle handle, int64_t frame,
                                  double exactPos,
                                  double sourceFramesPerSecond,
                                  bool bothNeighbours,
                                  const VideoStreamInfo* info)
{
    if (handle == InvalidMedia || !std::isfinite(exactPos) ||
        !std::isfinite(sourceFramesPerSecond))
        return;
    ClipRateHint hint;
    hint.frame = frame;
    hint.exactPos = exactPos;
    hint.sourceFramesPerSecond = std::abs(sourceFramesPerSecond);
    hint.bothNeighbours = bothNeighbours;
    hint.sourceFps = info ? info->fps : 0.0;
    hint.keyframes = info ? info->keyframes : nullptr;
    hint.at = std::chrono::steady_clock::now();
    std::lock_guard lock(m_rateHintMutex);
    m_rateHints[handle] = std::move(hint);
}

std::optional<MediaPool::ClipRateHint>
MediaPool::freshRateHint(MediaHandle handle) const
{
    std::lock_guard lock(m_rateHintMutex);
    const auto it = m_rateHints.find(handle);
    if (it == m_rateHints.end() || it->second.sourceFramesPerSecond <= 0.0 ||
        std::chrono::steady_clock::now() - it->second.at >= kRateHintTtl)
        return std::nullopt;
    return it->second;
}

bool MediaPool::keyframeOnly(const ClipRateHint& hint) const
{
    if (!hint.keyframes || !hint.keyframes->usable() || hint.sourceFps <= 0.0)
        return false;
    const double absShuttle =
        std::abs(m_playbackSpeed.load(std::memory_order_relaxed));
    // Source seconds per timeline second.
    const double speed = hint.sourceFramesPerSecond / hint.sourceFps
                       * std::max(1.0, absShuttle);
    return speed >= kKeyframeOnlyPlaybackSpeed;
}

std::optional<int64_t> MediaPool::playbackKeyframeFor(MediaHandle handle,
                                                     int64_t frame) const
{
    const auto hint = freshRateHint(handle);
    if (!hint || !keyframeOnly(*hint)) return std::nullopt;
    return hint->keyframes->atOrBefore(frame);
}

int64_t MediaPool::playbackFallbackDistance(MediaHandle handle) const
{
    const auto hint = freshRateHint(handle);
    const double projFps = m_projectFps.load(std::memory_order_relaxed);
    if (!hint || projFps <= 0.0) return kMaxPlaybackFallbackDistanceFrames;
    const double absShuttle =
        std::abs(m_playbackSpeed.load(std::memory_order_relaxed));
    const double step = hint->sourceFramesPerSecond / projFps
                      * std::max(1.0, absShuttle);
    int64_t distance = std::max<int64_t>(
        kMaxPlaybackFallbackDistanceFrames,
        static_cast<int64_t>(std::ceil(
            static_cast<double>(kMaxPlaybackFallbackDistanceFrames) * step)));
    if (keyframeOnly(*hint))
        distance = std::max(distance, hint->keyframes->maxGap * 2);
    return distance;
}

DecodeAheadRate MediaPool::decodeAheadRate(MediaHandle handle,
                                           double sourceFps) const
{
    const double projFps = m_projectFps.load(std::memory_order_relaxed);
    const double absShuttle =
        std::abs(m_playbackSpeed.load(std::memory_order_relaxed));

    DecodeAheadRate rate;
    if (const auto hint = freshRateHint(handle); hint && projFps > 0.0) {
        // The compositor told us how this clip walks the file: exact
        // fractional rate + phase (covers Speed/Duration and fps ratio).
        // Shuttle below 1x still shows every frame the clip maps to,
        // just more slowly, so it never makes the plan denser.
        rate.step = hint->sourceFramesPerSecond / projFps
                  * std::max(1.0, absShuttle);
        rate.anchorFrame = hint->frame;
        rate.anchorPos = hint->exactPos;
        rate.bothNeighbours = hint->bothNeighbours;
        if (keyframeOnly(*hint)) {
            rate.keyframesOnly = hint->keyframes;
            rate.bothNeighbours = false;
        }
        return rate;
    }

    // No fresh hint (Source Monitor, a file the sequence is not showing):
    // the original integer strides.  A 60 fps clip in a 30 fps sequence only
    // needs every 2nd frame; at N x shuttle only every Nth.
    const int srcStride = (sourceFps > projFps * 1.4)
        ? std::max(1, static_cast<int>(std::round(sourceFps / projFps)))
        : 1;
    const int speedStride = (absShuttle > 1.4)
        ? std::max(1, static_cast<int>(std::lround(absShuttle)))
        : 1;
    rate.step = static_cast<double>(std::max(1, srcStride * speedStride));
    return rate;
}

void MediaPool::setDiskCache(std::shared_ptr<DiskFrameCache> cache)
{
    m_diskCache = std::move(cache);
}

DiskFrameCache* MediaPool::diskCache() const noexcept
{
    return m_diskCache.get();
}

FrameScheduler& MediaPool::scheduler() noexcept
{
    return m_scheduler;
}

const FrameScheduler& MediaPool::scheduler() const noexcept
{
    return m_scheduler;
}

// ─── Queries ─────────────────────────────────────────────────────────────────

const VideoStreamInfo* MediaPool::getInfo(MediaHandle handle) const
{
    std::lock_guard lock(m_mutex);
    auto* entry = findEntry(handle);
    return entry ? &entry->info : nullptr;
}

std::filesystem::path MediaPool::getPath(MediaHandle handle) const
{
    std::lock_guard lock(m_mutex);
    auto* entry = findEntry(handle);
    return entry ? entry->path : std::filesystem::path{};
}

bool MediaPool::isValid(MediaHandle handle) const
{
    std::lock_guard lock(m_mutex);
    return m_entries.find(handle) != m_entries.end();
}

size_t MediaPool::openCount() const
{
    std::lock_guard lock(m_mutex);
    return m_entries.size();
}

// ─── Internal ────────────────────────────────────────────────────────────────

MediaEntry* MediaPool::findEntry(MediaHandle handle)
{
    auto it = m_entries.find(handle);
    return it != m_entries.end() ? &it->second : nullptr;
}

const MediaEntry* MediaPool::findEntry(MediaHandle handle) const
{
    auto it = m_entries.find(handle);
    return it != m_entries.end() ? &it->second : nullptr;
}

} // namespace rt
