/*
 * VideoFrameGrabber.cpp — see header.
 */
#include "panels/audio/VideoFrameGrabber.h"

#include "PathUtils.h"
#include "cache/FrameCache.h"
#include "decode/ConvertDecodedFrame.h"
#include "decode/VideoDecoder.h"

#include <QCoreApplication>
#include <QMetaObject>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <iterator>
#include <map>
#include <optional>

#ifdef ROUNDTABLE_HAS_FFMPEG
extern "C" {
#include <libswscale/swscale.h>
}
#endif

namespace rt {

namespace {
constexpr size_t kMaxOpenDecoders = 3;
constexpr size_t kMaxCachedStills = 400;
constexpr size_t kFrameCacheBytes = 48u * 1024u * 1024u;   ///< per open decoder
/// While walking forward from a keyframe, frames this close before the
/// target are converted + cached too (a backward scrub lands on them).
constexpr double kCacheLeadSec = 0.5;

// A deleted render can be recreated under the same name while an open decoder
// still reads the old file. Both decoded frames and posters need its revision.
std::string mediaSourceKey(const std::string& path)
{
    const auto file = utf8ToPath(path);
    std::error_code ec;
    const auto modified = std::filesystem::last_write_time(file, ec);
    if (ec) return {};
    const auto bytes = std::filesystem::file_size(file, ec);
    if (ec) return {};
    return path + "|" + std::to_string(modified.time_since_epoch().count()) + "|" + std::to_string(bytes);
}
} // namespace

struct VideoFrameGrabber::DecoderSlot
{
    VideoDecoder decoder;
    double       lastTime{-1.0};   ///< Timestamp of the last decoded frame (decoder cursor)
    uint64_t     lastUse{0};
    std::string  sourceKey;

    struct CachedImage
    {
        QImage image;
        double nextTs{-1.0};   ///< timestamp of the frame decoded right after (-1: unknown)
    };
    /// Converted frames keyed by timestamp (ms), all at `cacheSize`.
    std::map<int64_t, CachedImage> frames;
    QSize                          cacheSize;
    size_t                         cacheBytes{0};
    /// Key of the last converted frame IF it was also the last frame
    /// decoded — the next decoded frame fills in its nextTs.
    std::optional<int64_t>         pendingNext;

    void* swsCtx{nullptr};
    int   swsSrcW{0}, swsSrcH{0}, swsSrcFmt{-1}, swsDstW{0}, swsDstH{0};

    ~DecoderSlot()
    {
#ifdef ROUNDTABLE_HAS_FFMPEG
        if (swsCtx) sws_freeContext(static_cast<SwsContext*>(swsCtx));
#endif
    }

    static int64_t keyOf(double ts) { return static_cast<int64_t>(std::llround(ts * 1000.0)); }

    /// Cached frame displayed at `t`, or null.  A frame covers t from half a
    /// frame before its timestamp until half a frame before the NEXT frame;
    /// without a known successor only its own half-frame window counts (else
    /// a stale frame would be served while playback steps forward).
    QImage lookup(double t, double halfFrame) const
    {
        if (frames.empty()) return {};
        auto it = frames.upper_bound(keyOf(t + halfFrame));
        if (it == frames.begin()) return {};
        --it;
        const double ts = static_cast<double>(it->first) / 1000.0;
        const double end = it->second.nextTs >= 0.0 ? it->second.nextTs - halfFrame
                                                    : ts + halfFrame;
        return t < end ? it->second.image : QImage();
    }

    /// Called for EVERY decoded frame, converted or not.
    void noteDecoded(double ts)
    {
        if (pendingNext) {
            if (auto it = frames.find(*pendingNext); it != frames.end()) it->second.nextTs = ts;
            pendingNext.reset();
        }
    }

    void store(double ts, const QImage& image)
    {
        const auto key = keyOf(ts);
        pendingNext = key;
        if (frames.count(key)) return;
        frames.emplace(key, CachedImage{image, -1.0});
        cacheBytes += static_cast<size_t>(image.sizeInBytes());
        // Evict from whichever end is farther from the frame just stored.
        while (cacheBytes > kFrameCacheBytes && frames.size() > 1) {
            auto first = frames.begin();
            auto last  = std::prev(frames.end());
            auto victim = (key - first->first > last->first - key) ? first : last;
            cacheBytes -= static_cast<size_t>(victim->second.image.sizeInBytes());
            if (pendingNext && *pendingNext == victim->first) pendingNext.reset();
            frames.erase(victim);
        }
    }

    void resetCache(QSize size)
    {
        frames.clear();
        cacheBytes = 0;
        cacheSize  = size;
        pendingNext.reset();
    }
};

VideoFrameGrabber::VideoFrameGrabber()
{
    // Started in the body: every member the worker touches is constructed.
    m_thread.start("Video frame grabber", [this](std::stop_token) { workerLoop(); });
}

VideoFrameGrabber::~VideoFrameGrabber()
{
    {
        std::lock_guard lock(m_mutex);
        m_shutdown = true;
    }
    m_cv.notify_all();
    m_thread.stop();
}

bool VideoFrameGrabber::isVideoFile(const std::string& path)
{
    std::string ext = pathToUtf8(utf8ToPath(path).extension());
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext == ".mp4" || ext == ".mov" || ext == ".mxf" || ext == ".mkv" ||
           ext == ".m4v" || ext == ".avi" || ext == ".webm";
}

double VideoFrameGrabber::knownFps(const std::string& path) const
{
    const auto sourceKey = mediaSourceKey(path);
    std::lock_guard lock(m_mutex);
    auto it = m_fps.find(sourceKey);
    return it != m_fps.end() ? it->second : 0.0;
}

std::string VideoFrameGrabber::stillKey(const std::string& path, double timeSec, QSize size)
{
    char buf[64];
    std::snprintf(buf, sizeof(buf), "|%lld|%dx%d",
                  static_cast<long long>(std::llround(timeSec * 1000.0)),
                  size.width(), size.height());
    return path + buf;
}

void VideoFrameGrabber::requestStill(QObject* receiver, const std::string& path, double timeSec,
                                     QSize size, Callback callback)
{
    if (!receiver || !callback || path.empty() || size.isEmpty()) return;
    const auto sourceKey = mediaSourceKey(path);
    if (sourceKey.empty()) return;
    Request request{receiver, path, timeSec, size, std::move(callback), stillKey(sourceKey, timeSec, size), sourceKey};
    QImage cached;
    {
        std::lock_guard lock(m_mutex);
        auto it = m_stillCache.find(request.cacheKey);
        if (it != m_stillCache.end()) {
            cached = it->second;
        } else {
            m_stills.push_back(std::move(request));
        }
    }
    if (!cached.isNull()) {
        request.callback(cached);
        return;
    }
    m_cv.notify_one();
}

void VideoFrameGrabber::requestLive(QObject* receiver, const std::string& path, double timeSec,
                                    QSize size, Callback callback, int channel)
{
    if (!receiver || !callback || path.empty() || size.isEmpty()) return;
    const auto sourceKey = mediaSourceKey(path);
    if (sourceKey.empty()) return;
    {
        std::lock_guard lock(m_mutex);
        m_live[channel] = Request{receiver, path, timeSec, size, std::move(callback), {}, sourceKey};
    }
    m_cv.notify_one();
}

void VideoFrameGrabber::deliver(const Request& request, const QImage& image)
{
    if (image.isNull() || !QCoreApplication::instance()) return;
    QMetaObject::invokeMethod(QCoreApplication::instance(),
        [receiver = request.receiver, callback = request.callback, image,
         path = request.path, sourceKey = request.sourceKey]() {
            if (receiver && mediaSourceKey(path) == sourceKey) callback(image);
        }, Qt::QueuedConnection);
}

void VideoFrameGrabber::workerLoop()
{
    for (;;) {
        Request request;
        QImage cachedHit;
        {
            std::unique_lock lock(m_mutex);
            m_cv.wait(lock, [this]() { return m_shutdown || !m_live.empty() || !m_stills.empty(); });
            if (m_shutdown) break;
            if (!m_live.empty()) {
                // Round-robin across channels so one busy consumer (e.g.
                // playback) can't starve another (e.g. hover scrub).
                auto it = m_live.upper_bound(m_lastLiveChannel);
                if (it == m_live.end()) it = m_live.begin();
                m_lastLiveChannel = it->first;
                request = std::move(it->second);
                m_live.erase(it);
            } else {
                request = std::move(m_stills.front());
                m_stills.pop_front();
                auto it = m_stillCache.find(request.cacheKey);
                if (it != m_stillCache.end()) cachedHit = it->second;
            }
        }
        if (!request.receiver || mediaSourceKey(request.path) != request.sourceKey) continue;
        if (!cachedHit.isNull()) {
            deliver(request, cachedHit);
            continue;
        }

        QImage image;
        try {
            image = decode(request);
        } catch (const std::exception& e) {
            spdlog::warn("VideoFrameGrabber: decode failed for {}: {}", request.path, e.what());
        }

        if (!request.cacheKey.empty() && !image.isNull()) {
            std::lock_guard lock(m_mutex);
            if (m_stillCache.emplace(request.cacheKey, image).second) {
                m_stillCacheOrder.push_back(request.cacheKey);
                while (m_stillCacheOrder.size() > kMaxCachedStills) {
                    m_stillCache.erase(m_stillCacheOrder.front());
                    m_stillCacheOrder.pop_front();
                }
            }
        }
        deliver(request, image);
    }
    m_decoders.clear();   // decoders live and die on this thread
}

QImage VideoFrameGrabber::decode(const Request& request)
{
    auto& slot = m_decoders[request.path];
    if (slot && slot->sourceKey != request.sourceKey) slot.reset();
    if (!slot) {
        while (m_decoders.size() > kMaxOpenDecoders) {
            auto lru = m_decoders.end();
            for (auto it = m_decoders.begin(); it != m_decoders.end(); ++it) {
                if (it->second && (lru == m_decoders.end() || it->second->lastUse < lru->second->lastUse))
                    lru = it;
            }
            if (lru == m_decoders.end()) break;
            m_decoders.erase(lru);
        }
        auto fresh = std::make_unique<DecoderSlot>();
        fresh->sourceKey = request.sourceKey;
        // Slice-only threading: this decoder seeks constantly (poster frames
        // for many takes), which frame threading corrupts on H.264.
        if (!fresh->decoder.open(utf8ToPath(request.path), /*forceSoftware=*/false,
                                 /*maxThreads=*/0, /*sliceOnlyThreading=*/true)) {
            spdlog::warn("VideoFrameGrabber: cannot open {}", request.path);
            m_decoders.erase(request.path);
            return {};
        }
        if (const double fps = fresh->decoder.info().fps; fps > 0.0) {
            std::lock_guard lock(m_mutex);
            m_fps[request.sourceKey] = fps;
        }
        m_decoders[request.path] = std::move(fresh);
    }
    DecoderSlot& s = *m_decoders[request.path];
    s.lastUse = ++m_useCounter;

    const auto& info = s.decoder.info();
    const double frameDur = 1.0 / (info.fps > 0.0 ? info.fps : 30.0);
    const double halfFrame = 0.5 * frameDur;
    const double t = std::max(0.0, request.timeSec);

    if (s.cacheSize != request.size) s.resetCache(request.size);
    if (QImage hit = s.lookup(t, halfFrame); !hit.isNull()) return hit;

    // Converts the decoder's current frame and caches it.
    auto convert = [&](DecodedFrame& frame) -> QImage {
        const double ts = frame.timestamp;
        if (frame.isHardware) {
            DecodedFrame cpuFrame;
            if (!s.decoder.transferHardwareFrame(frame, cpuFrame)) return {};
            frame = std::move(cpuFrame);
        }
        if (frame.isHardware || !frame.data[0] || frame.width == 0 || frame.height == 0) return {};

        const int srcW = static_cast<int>(frame.width);
        const int srcH = static_cast<int>(frame.height);
        const double scale = std::min(static_cast<double>(request.size.width()) / srcW,
                                      static_cast<double>(request.size.height()) / srcH);
        const int dstW = std::max(2, static_cast<int>(std::lround(srcW * scale)) & ~1);
        const int dstH = std::max(2, static_cast<int>(std::lround(srcH * scale)) & ~1);

        CachedFrame bgra;
        if (!convertDecodedToBgra(frame, resolveDecodedAvFormat(frame), dstW, dstH, info,
                                  utf8ToPath(request.path),
                                  SwsCacheRef{s.swsCtx, s.swsSrcW, s.swsSrcH, s.swsSrcFmt,
                                              s.swsDstW, s.swsDstH},
                                  /*pool=*/nullptr, bgra) ||
            bgra.pixels.empty())
            return {};

        // BGRA bytes == QImage ARGB32 on little-endian.
        QImage image(bgra.pixels.data(), static_cast<int>(bgra.width), static_cast<int>(bgra.height),
                     static_cast<qsizetype>(bgra.stride), QImage::Format_ARGB32);
        QImage owned = image.copy();
        s.store(ts, owned);
        return owned;
    };

    // Short forward step (playback, slow scrub): keep decoding from the
    // cursor.  Anything else: keyframe seek, then decode forward.  Never
    // decodeAt(): its precise seek flushes the codec after reaching the
    // target, so on long-GOP H.264 the following decode skips ahead to the
    // next keyframe (or EOF) -- that made posters black and playback ~1 fps.
    // A size change drops the converted cache, not the decoder cursor.
    // If the current decoded frame covers t, seek back to recover it at
    // the new size. Advancing would skip that frame (or return no image at EOF).
    const bool forward = s.lastTime >= 0.0 && t > s.lastTime + halfFrame && t - s.lastTime < 2.0;
    if (!forward) {
        s.pendingNext.reset();
        if (!s.decoder.seek(t, SeekMode::Keyframe)) {
            s.lastTime = -1.0;
            return {};
        }
    }

    // Convert from here on.  Clamped to the duration so a request past the
    // end still converts the last frames (the fallback result below).
    const double convertFrom =
        (info.duration > 0.0 ? std::min(t, info.duration) : t) - kCacheLeadSec;

    QImage result;
    DecodedFrame frame;
    while (s.decoder.decodeNext(frame)) {
        s.lastTime = frame.timestamp;
        s.noteDecoded(frame.timestamp);
        const bool reached = frame.timestamp + halfFrame >= t;
        if (reached || frame.timestamp >= convertFrom) {
            QImage image = convert(frame);
            if (!image.isNull()) result = image;
        }
        if (reached) return result;
    }
    // Ran off the end (t past the last frame): show the last converted frame.
    s.lastTime = -1.0;
    s.pendingNext.reset();
    return result;
}

std::vector<QImage> VideoFrameGrabber::decodeAllFrames(const std::string& path, QSize size, int maxFrames)
{
    std::vector<QImage> frames;
    VideoDecoder decoder;
    if (!decoder.open(utf8ToPath(path), /*forceSoftware=*/true)) return frames;
    const auto& info = decoder.info();
    void* swsCtx = nullptr;
    int swsSrcW = 0, swsSrcH = 0, swsSrcFmt = -1, swsDstW = 0, swsDstH = 0;
    DecodedFrame frame;
    while (static_cast<int>(frames.size()) < maxFrames && decoder.decodeNext(frame)) {
        if (frame.isHardware || !frame.data[0] || frame.width == 0 || frame.height == 0) continue;
        const int srcW = static_cast<int>(frame.width);
        const int srcH = static_cast<int>(frame.height);
        const double scale = std::min({1.0, static_cast<double>(size.width()) / srcW,
                                       static_cast<double>(size.height()) / srcH});
        const int dstW = std::max(2, static_cast<int>(std::lround(srcW * scale)) & ~1);
        const int dstH = std::max(2, static_cast<int>(std::lround(srcH * scale)) & ~1);
        CachedFrame bgra;
        if (!convertDecodedToBgra(frame, resolveDecodedAvFormat(frame), dstW, dstH, info, utf8ToPath(path),
                                  SwsCacheRef{swsCtx, swsSrcW, swsSrcH, swsSrcFmt, swsDstW, swsDstH},
                                  /*pool=*/nullptr, bgra) ||
            bgra.pixels.empty())
            continue;
        frames.push_back(QImage(bgra.pixels.data(), static_cast<int>(bgra.width), static_cast<int>(bgra.height),
                                static_cast<qsizetype>(bgra.stride), QImage::Format_ARGB32).copy());
    }
#ifdef ROUNDTABLE_HAS_FFMPEG
    if (swsCtx) sws_freeContext(static_cast<SwsContext*>(swsCtx));
#endif
    return frames;
}

} // namespace rt
