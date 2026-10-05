/*
 * VideoFrameGrabber — small off-thread video frame source for Audio Sync's
 * video takes (poster frames on take chips/cards + live preview while a take
 * plays).  One worker thread owns its own decoders, so it never touches the
 * playback MediaPool or competes with the timeline's prefetch.
 *
 *   requestStill — cached, queued; used for poster frames.
 *   requestLive  — latest-wins PER CHANNEL; a newer live request replaces the
 *                  channel's pending one, so a slow decode drops frames instead
 *                  of lagging behind.  Channels keep independent consumers
 *                  (playback, hover-scrub, the take player) from starving each
 *                  other.
 *
 * Each open decoder keeps a small cache of recently converted frames, so
 * scrubbing back and forth over a stretch already seen is instant.
 *
 * Callbacks run on the GUI thread and are skipped if `receiver` was destroyed.
 */

#pragma once

#include <QImage>
#include <QPointer>
#include <QSize>

#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <map>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace rt {

class VideoFrameGrabber
{
public:
    using Callback = std::function<void(const QImage&)>;

    VideoFrameGrabber();
    ~VideoFrameGrabber();

    VideoFrameGrabber(const VideoFrameGrabber&) = delete;
    VideoFrameGrabber& operator=(const VideoFrameGrabber&) = delete;

    /// Poster frame of `path` at `timeSec`, scaled to fit `size`. Served from
    /// cache synchronously when available, otherwise decoded in order.
    void requestStill(QObject* receiver, const std::string& path, double timeSec,
                      QSize size, Callback callback);

    /// Live-request channels (see header comment).
    enum Channel : int { Playback = 0, HoverScrub = 1, Player = 2 };

    /// Frame for live preview; replaces the channel's pending live request.
    void requestLive(QObject* receiver, const std::string& path, double timeSec,
                     QSize size, Callback callback, int channel = Playback);

    /// Frame rate of `path` once any request has opened it, else 0.
    [[nodiscard]] double knownFps(const std::string& path) const;

    /// True for file extensions Audio Sync treats as video sources.
    [[nodiscard]] static bool isVideoFile(const std::string& path);

    /// Every frame of a short clip, in order, scaled to fit `size` (at most
    /// `maxFrames`).  Synchronous with its own decoder: call off the GUI
    /// thread.  Empty if the file can't be decoded.
    [[nodiscard]] static std::vector<QImage> decodeAllFrames(const std::string& path, QSize size,
                                                             int maxFrames = 600);

private:
    struct Request
    {
        QPointer<QObject> receiver;
        std::string       path;
        double            timeSec{0.0};
        QSize             size;
        Callback          callback;
        std::string       cacheKey;   // empty for live requests
    };
    struct DecoderSlot;

    void workerLoop();
    QImage decode(const Request& request);
    static std::string stillKey(const std::string& path, double timeSec, QSize size);
    static void deliver(const Request& request, const QImage& image);

    std::thread                 m_thread;
    mutable std::mutex          m_mutex;
    std::condition_variable     m_cv;
    bool                        m_shutdown{false};
    std::deque<Request>         m_stills;
    std::map<int, Request>      m_live;          ///< pending, one per channel
    int                         m_lastLiveChannel{-1};

    // Worker-thread only.
    std::unordered_map<std::string, std::unique_ptr<DecoderSlot>> m_decoders;
    uint64_t                    m_useCounter{0};

    // Guarded by m_mutex.
    std::unordered_map<std::string, QImage> m_stillCache;
    std::unordered_map<std::string, double> m_fps;
    std::deque<std::string>                 m_stillCacheOrder;
};

} // namespace rt
