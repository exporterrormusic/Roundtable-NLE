/*
 * TimelinePanelMedia.cpp - Waveform loading and video thumbnail generation.
 * Split from TimelinePanel.cpp.
 */

#include "panels/timeline/TimelinePanel.h"
#include "PathUtils.h"
#include "panels/timeline/TimelinePanelInternal.h"

#include "timeline/Timeline.h"
#include "timeline/Track.h"
#include "timeline/Clip.h"
#include "timeline/AudioClip.h"
#include "timeline/VideoClip.h"
#include "audio/AudioFile.h"
#include "decode/VideoDecoder.h"
#include "widgets/TimelineTrackWidget.h"

#include <QImage>
#include <QPainter>
#include <QPixmap>
#include <QPointer>

#include <spdlog/spdlog.h>

#include <filesystem>
#include <thread>

namespace rt {

// Waveform cache key = (media path, audio-stream ordinal). Two clips on
// different audio streams of the same file must NOT share one waveform, so
// the ordinal is folded into the path-keyed waveform caches. -1 (auto/best)
// keeps the legacy single-stream key shape distinct from explicit picks.
static std::string waveformCacheKey(const std::string& path, int audioStreamOrdinal)
{
    return path + "|" + std::to_string(audioStreamOrdinal);
}

static std::string normalizedWaveformPath(const std::string& path)
{
    const auto generic = utf8ToPath(path).lexically_normal().generic_u8string();
    return std::string(reinterpret_cast<const char*>(generic.data()), generic.size());
}

static bool waveformKeyMatchesPath(const std::string& key,
                                   const std::string& normalizedPath)
{
    const auto ordinalSeparator = key.rfind('|');
    if (ordinalSeparator == std::string::npos) return false;
    return normalizedWaveformPath(key.substr(0, ordinalSeparator)) == normalizedPath;
}

// Mirror of MediaPool::open() / ThumbnailGenerator path-fallback search.
// loadThumbnails() opens VideoDecoder directly (it does not go through
// MediaPool), so bare filenames like "MARIAN_WALL_HD.png" miss the asset
// search dirs and fail.  Probe the same directories MediaPool does.
static std::filesystem::path resolveThumbnailPath(const std::filesystem::path& path)
{
    namespace fs = std::filesystem;
    std::error_code ec;
    if (fs::exists(path, ec)) return path;
    fs::path filename = path.filename();
    const fs::path searchDirs[] = {
        fs::path("assets") / "backgrounds",
        fs::path("assets") / "characters",
        fs::path("assets") / "videos",
        fs::path("assets"),
    };
    for (const auto& dir : searchDirs) {
        fs::path candidate = dir / filename;
        if (fs::exists(candidate, ec)) return candidate;
    }
    // Try appending common image extensions for bare filenames (e.g.
    // "TABLE_LARGE_FINAL" -> "TABLE_LARGE_FINAL.png" in backgrounds/)
    if (filename.extension().empty()) {
        const fs::path imgExts[] = {".png", ".jpg", ".jpeg"};
        for (const auto& imgExt : imgExts) {
            for (const auto& dir : searchDirs) {
                fs::path candidate = dir / fs::path(pathToUtf8(filename) + pathToUtf8(imgExt));
                if (fs::exists(candidate, ec)) return candidate;
            }
        }
    }
    return path;  // fall through; caller will log a warning
}

void TimelinePanel::loadWaveforms()
{
    // Don't clear - keep existing peaks for already-loaded clips.
    // Stale entries for deleted clips are harmless (never looked up again).
    if (!m_timeline) return;

    for (size_t ti = 0; ti < m_timeline->trackCount(); ++ti)
    {
        auto* track = m_timeline->track(ti);
        if (!track || track->type() != TrackType::Audio) continue;

        for (size_t ci = 0; ci < track->clipCount(); ++ci)
        {
            auto* clip = track->clip(ci);
            auto* audioClip = dynamic_cast<AudioClip*>(clip);
            if (!audioClip || !clip->isEnabled()) continue;

            const auto& path = audioClip->mediaPath();
            if (path.empty()) continue;

            // Skip if already cached by clip ID
            if (m_waveformPeaks.count(clip->id())) continue;

            // Fast path: another clip with the same source file AND the same
            // audio stream was already decoded (e.g. after a split) - just
            // copy peaks. Keyed by (path, ordinal) so a clip pointing at a
            // different stream of the same file gets its own waveform.
            const int ord = audioClip->audioStreamIndex();
            const std::string key = waveformCacheKey(path, ord);
            auto byPathIt = m_waveformByPath.find(key);
            if (byPathIt != m_waveformByPath.end()) {
                m_waveformPeaks[clip->id()] = byPathIt->second;
                continue;
            }

            if (!m_failedWaveformPaths.count(key)) {
                queueWaveformLoad(path, ord);
            }
        }
    }
}

void TimelinePanel::invalidateClipWaveform(uint64_t clipId)
{
    // Erase this clip's cached peaks (keyed by clip id) and re-run the loader,
    // which re-queues a decode under the clip's CURRENT (path, ordinal) key.
    // For a stream change the key always differs, so a fresh decode is queued
    // and applyWaveformPeaks() repaints when it lands.
    m_waveformPeaks.erase(clipId);
    loadWaveforms();
}

void TimelinePanel::refreshMediaWaveform(const std::filesystem::path& changedPath)
{
    if (changedPath.empty()) return;

    const auto changedGeneric = changedPath.lexically_normal().generic_u8string();
    const std::string changedKey(
        reinterpret_cast<const char*>(changedGeneric.data()), changedGeneric.size());

    // Retire outstanding decodes so an old result cannot repopulate this
    // cache after the media is replaced. Clearing the pending set also lets
    // loadWaveforms() immediately requeue any unrelated retired work.
    ++m_waveformLoadGeneration;
    m_pendingWaveformPaths.clear();

    for (auto it = m_waveformByPath.begin(); it != m_waveformByPath.end(); ) {
        if (waveformKeyMatchesPath(it->first, changedKey))
            it = m_waveformByPath.erase(it);
        else
            ++it;
    }
    for (auto it = m_failedWaveformPaths.begin(); it != m_failedWaveformPaths.end(); ) {
        if (waveformKeyMatchesPath(*it, changedKey))
            it = m_failedWaveformPaths.erase(it);
        else
            ++it;
    }

    if (m_timeline) {
        for (size_t ti = 0; ti < m_timeline->trackCount(); ++ti) {
            auto* track = m_timeline->track(ti);
            if (!track || track->type() != TrackType::Audio) continue;
            for (size_t ci = 0; ci < track->clipCount(); ++ci) {
                auto* audio = dynamic_cast<AudioClip*>(track->clip(ci));
                if (audio && normalizedWaveformPath(audio->mediaPath()) == changedKey)
                    m_waveformPeaks.erase(audio->id());
            }
        }
    }

    loadWaveforms();
    for (auto tw : m_trackWidgets)
        if (tw) tw->update();
}

void TimelinePanel::queueWaveformLoad(const std::string& path, int audioStreamOrdinal)
{
    const std::string key = waveformCacheKey(path, audioStreamOrdinal);
    if (path.empty() || m_waveformByPath.count(key) ||
        m_pendingWaveformPaths.count(key) || m_failedWaveformPaths.count(key)) {
        return;
    }

    m_pendingWaveformPaths.insert(key);
    QPointer<TimelinePanel> self(this);
    const uint64_t generation = m_waveformLoadGeneration;

    std::thread([self, path, key, audioStreamOrdinal, generation]() {
        constexpr int64_t kPeakWindowFrames = 480; // 10ms at 48 kHz

        AudioFile file;
        if (!file.open(path, audioStreamOrdinal)) {
            if (!self) {
                return;
            }
            QMetaObject::invokeMethod(self, [self, path, key, generation]() {
                if (!self || self->m_waveformLoadGeneration != generation) {
                    return;
                }
                self->m_pendingWaveformPaths.erase(key);
                self->m_failedWaveformPaths.insert(key);
                spdlog::warn("loadWaveforms: failed to open '{}'", path);
            }, Qt::QueuedConnection);
            return;
        }

        std::vector<float> peaks;
        const size_t numPeaks = file.buildPeakEnvelopeResampled(
            48000, kPeakWindowFrames, peaks);
        if (numPeaks == 0 || peaks.empty()) {
            if (!self) {
                return;
            }
            QMetaObject::invokeMethod(self, [self, key, generation]() {
                if (!self || self->m_waveformLoadGeneration != generation) {
                    return;
                }
                self->m_pendingWaveformPaths.erase(key);
                self->m_failedWaveformPaths.insert(key);
            }, Qt::QueuedConnection);
            return;
        }

        if (!self) {
            return;
        }

        QMetaObject::invokeMethod(self, [self, generation, key,
                                         peaks = std::move(peaks)]() mutable {
            if (!self) {
                return;
            }
            self->applyWaveformPeaks(generation, key, std::move(peaks));
        }, Qt::QueuedConnection);
    }).detach();
}

void TimelinePanel::applyWaveformPeaks(uint64_t generation,
                                       const std::string& key,
                                       std::vector<float> peaks)
{
    if (m_waveformLoadGeneration != generation) {
        return;
    }

    m_pendingWaveformPaths.erase(key);
    if (peaks.empty()) {
        m_failedWaveformPaths.insert(key);
        return;
    }

    m_failedWaveformPaths.erase(key);
    auto it = m_waveformByPath.find(key);
    if (it == m_waveformByPath.end()) {
        it = m_waveformByPath.emplace(key, std::move(peaks)).first;
    } else {
        it->second = std::move(peaks);
    }

    if (m_timeline) {
        for (size_t ti = 0; ti < m_timeline->trackCount(); ++ti) {
            auto* track = m_timeline->track(ti);
            if (!track || track->type() != TrackType::Audio) continue;

            for (size_t ci = 0; ci < track->clipCount(); ++ci) {
                auto* clip = track->clip(ci);
                auto* audioClip = dynamic_cast<AudioClip*>(clip);
                if (!audioClip || !clip->isEnabled()) continue;
                // Match by (path, ordinal) — only clips on this exact stream
                // of this file get these peaks.
                if (waveformCacheKey(audioClip->mediaPath(),
                                     audioClip->audioStreamIndex()) != key) continue;
                m_waveformPeaks[clip->id()] = it->second;
            }
        }
    }

    for (auto tw : m_trackWidgets) {
        tw->update();
    }

    spdlog::info("loadWaveforms: loaded {} peaks asynchronously for '{}'",
                 it->second.size(), key);
}

void TimelinePanel::loadThumbnails()
{
    // Don't clear - keep existing thumbnails for already-loaded clips.
    if (!m_timeline) return;

    for (size_t ti = 0; ti < m_timeline->trackCount(); ++ti)
    {
        auto* track = m_timeline->track(ti);
        if (!track || track->type() != TrackType::Video) continue;

        for (size_t ci = 0; ci < track->clipCount(); ++ci)
        {
            auto* clip = track->clip(ci);
            if (!clip || clip->clipType() != ClipType::Video) continue;

            // Skip if already cached by clip ID
            if (m_thumbnailCache.count(clip->id())) continue;

            auto* videoClip = dynamic_cast<VideoClip*>(clip);
            if (!videoClip) continue;

            const auto& path = videoClip->mediaPath();
            if (path.empty()) continue;

            // Fast path: another clip with the same source was already
            // decoded (e.g. after a split) - reuse the cached thumbnail.
            auto byPathIt = m_thumbnailByPath.find(path);
            if (byPathIt != m_thumbnailByPath.end()) {
                m_thumbnailCache[clip->id()] = byPathIt->second;
                continue;
            }

            // Otherwise decode on a background thread (see queueThumbnailLoad).
            // Decoding the first frame inline used to cost ~150ms per clip
            // (VideoDecoder open + decode) and froze the UI thread for many
            // seconds when opening a project with dozens of video clips.
            if (!m_failedThumbnailPaths.count(path))
                queueThumbnailLoad(path);
        }
    }
}

void TimelinePanel::refreshMediaThumbnail(const std::filesystem::path& changedPath)
{
    if (changedPath.empty()) return;

    auto pathKey = [](const std::string& path) {
        const auto generic = utf8ToPath(path).lexically_normal().generic_u8string();
        return std::string(reinterpret_cast<const char*>(generic.data()), generic.size());
    };
    const auto changedGeneric = changedPath.lexically_normal().generic_u8string();
    const std::string changedKey(
        reinterpret_cast<const char*>(changedGeneric.data()), changedGeneric.size());

    // Retire every outstanding result so a decode that began before the file
    // swap cannot put the old thumbnail back after this method returns.
    ++m_thumbnailLoadGeneration;
    m_pendingThumbnailPaths.clear();

    for (auto it = m_thumbnailByPath.begin(); it != m_thumbnailByPath.end(); ) {
        if (pathKey(it->first) == changedKey)
            it = m_thumbnailByPath.erase(it);
        else
            ++it;
    }
    for (auto it = m_failedThumbnailPaths.begin(); it != m_failedThumbnailPaths.end(); ) {
        if (pathKey(*it) == changedKey)
            it = m_failedThumbnailPaths.erase(it);
        else
            ++it;
    }

    if (m_timeline) {
        for (size_t ti = 0; ti < m_timeline->trackCount(); ++ti) {
            auto* track = m_timeline->track(ti);
            if (!track) continue;
            for (size_t ci = 0; ci < track->clipCount(); ++ci) {
                auto* video = dynamic_cast<VideoClip*>(track->clip(ci));
                if (video && pathKey(video->mediaPath()) == changedKey)
                    m_thumbnailCache.erase(video->id());
            }
        }
    }

    loadThumbnails();
    for (auto tw : m_trackWidgets)
        if (tw) tw->update();
}

void TimelinePanel::queueThumbnailLoad(const std::string& path)
{
    if (path.empty() || m_thumbnailByPath.count(path) ||
        m_pendingThumbnailPaths.count(path) || m_failedThumbnailPaths.count(path)) {
        return;
    }

    m_pendingThumbnailPaths.insert(path);
    QPointer<TimelinePanel> self(this);
    const uint64_t generation = m_thumbnailLoadGeneration;

    std::thread([self, path, generation]() {
        // Resolve the path the same way MediaPool does (bare filenames ->
        // asset search dirs).  Pure filesystem + decode work, no GUI calls.
        const std::filesystem::path resolved =
            resolveThumbnailPath(path);

        QImage img;  // built off-thread; QPixmap conversion happens on UI thread
        {
            VideoDecoder decoder;
            DecodedFrame frame;
            if (decoder.open(resolved) &&
                decoder.decodeAt(0.0, frame) &&
                frame.width > 0 && frame.height > 0) {
                const int bytesPerRow = frame.linesize[0];
                if (frame.format == PixelFormat::BGRA) {
                    img = QImage(frame.data[0], frame.width, frame.height,
                                 bytesPerRow, QImage::Format_RGBA8888_Premultiplied).copy();
                } else if (frame.format == PixelFormat::RGBA) {
                    img = QImage(frame.data[0], frame.width, frame.height,
                                 bytesPerRow, QImage::Format_RGBA8888).copy();
                }
                // YUV-only formats are left null -> treated as failed below.
            }
        }

        if (!self) return;
        QMetaObject::invokeMethod(self, [self, generation, path,
                                         img = std::move(img)]() mutable {
            if (!self) return;
            self->applyThumbnail(generation, path, img);
        }, Qt::QueuedConnection);
    }).detach();
}

void TimelinePanel::applyThumbnail(uint64_t generation, const std::string& path,
                                   const QImage& image)
{
    // Stale result from a previous project/timeline - discard.
    if (m_thumbnailLoadGeneration != generation) return;

    m_pendingThumbnailPaths.erase(path);

    if (image.isNull()) {
        m_failedThumbnailPaths.insert(path);
        spdlog::debug("loadThumbnails: failed to decode thumbnail for '{}'", path);
        return;
    }

    // Scale + convert to QPixmap on the UI thread (QPixmap is GUI-thread-only).
    constexpr int kThumbHeight = 80;
    QPixmap thumb = QPixmap::fromImage(
        image.scaledToHeight(kThumbHeight, Qt::SmoothTransformation));
    if (thumb.isNull()) {
        m_failedThumbnailPaths.insert(path);
        return;
    }

    m_failedThumbnailPaths.erase(path);
    m_thumbnailByPath[path] = thumb;  // store by path for future splits

    // Propagate to every video clip sharing this source path.
    if (m_timeline) {
        for (size_t ti = 0; ti < m_timeline->trackCount(); ++ti) {
            auto* track = m_timeline->track(ti);
            if (!track || track->type() != TrackType::Video) continue;
            for (size_t ci = 0; ci < track->clipCount(); ++ci) {
                auto* clip = track->clip(ci);
                if (!clip || clip->clipType() != ClipType::Video) continue;
                auto* videoClip = dynamic_cast<VideoClip*>(clip);
                if (videoClip && videoClip->mediaPath() == path)
                    m_thumbnailCache[clip->id()] = thumb;
            }
        }
    }

    for (auto tw : m_trackWidgets)
        if (tw) tw->update();

    spdlog::info("loadThumbnails: thumbnail for '{}' ({}x{}) applied asynchronously",
                 path, thumb.width(), thumb.height());
}

} // namespace rt
