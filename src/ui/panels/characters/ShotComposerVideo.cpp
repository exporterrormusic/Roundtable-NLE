/*
 * ShotComposerVideo.cpp - Video decoding, thumbnail extraction, and playback.
 * Split from ShotComposer.cpp.
 */

#include "panels/characters/ShotComposer.h"
#include "PathUtils.h"

#include "panels/characters/ShotComposerInternal.h"
#include "Theme.h"

#ifdef ROUNDTABLE_HAS_SPINE
#include "spine/ModelManager.h"
#include "spine/SpineEngine.h"
#include "spine/AnimationVideoCache.h"
#include "widgets/SpinePreviewWidget.h"
#endif

#ifdef ROUNDTABLE_HAS_FFMPEG
#include "decode/ConvertDecodedFrame.h"
#include "decode/VideoDecoder.h"
#include "cache/FrameCache.h"
#include "decode/PixelOps.h"
extern "C" {
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
}
#endif

#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QFormLayout>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QProcess>
#include <QResizeEvent>
#include <QScrollArea>
#include <QShortcut>
#include <QSlider>
#include <QStandardPaths>
#include <QStyledItemDelegate>
#include <QTabWidget>
#include <QJsonDocument>
#include <QJsonObject>
#include <QToolTip>
#include <QVBoxLayout>

#ifdef _WIN32
#include <windows.h>
#endif

#include <algorithm>
#include <cmath>
#include <fstream>
#include <map>
#include <set>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

#include <spdlog/spdlog.h>


namespace rt {

// ═════════════════════════════════════════════════════════════════════════════
// AVFrame → QImage conversion helper  (YUV420P / NV12 / HW → BGRA)
// ═════════════════════════════════════════════════════════════════════════════
#ifdef ROUNDTABLE_HAS_FFMPEG
// Thread-local cached SwsContext — safe with async video decode threads
static thread_local SwsContext* s_swsCtx    = nullptr;
static thread_local int         s_swsSrcW   = 0;
static thread_local int         s_swsSrcH   = 0;
static thread_local int         s_swsDstW   = 0;
static thread_local int         s_swsDstH   = 0;
static thread_local AVPixelFormat s_swsFmt  = AV_PIX_FMT_NONE;

/// Maximum preview decode resolution.
/// Frames are downscaled during sws_scale so we never convert at full ProRes
/// 4444 resolution (~8 MP) just for preview.  Cap each axis independently at
/// 1920 to support both landscape AND portrait content without aggressive
/// downscaling.  The WebM transcodes are already 1080px wide, so portrait
/// content (e.g. 1080×1888) passes through at full quality instead of being
/// squashed to ~411×720 by the old 1280×720 cap.
static constexpr int PREVIEW_MAX_WIDTH  = 1920;
static constexpr int PREVIEW_MAX_HEIGHT = 1920;

static QImage decodeFrameToQImage(DecodedFrame& df, VideoDecoder* decoder)
{
    // If hardware frame, transfer to CPU first
    DecodedFrame cpuFrame;
    DecodedFrame* src = &df;
    if (df.isHardware && decoder) {
        if (!decoder->transferHardwareFrame(df, cpuFrame))
            return {};
        src = &cpuFrame;
    }

    AVFrame* avf = src->avFrame;
    if (!avf || !avf->data[0])
        return {};

    int w = avf->width;
    int h = avf->height;
    if (w <= 0 || h <= 0)
        return {};

    // Use the AVFrame's actual pixel format — handles all formats correctly
    AVPixelFormat srcFmt = static_cast<AVPixelFormat>(avf->format);

    // If already BGRA at a reasonable size, create QImage directly
    if (srcFmt == AV_PIX_FMT_BGRA && w <= PREVIEW_MAX_WIDTH && h <= PREVIEW_MAX_HEIGHT) {
        QImage img(avf->data[0], w, h, avf->linesize[0],
                   QImage::Format_ARGB32);
        return img.copy();
    }

    // Downscale to preview dimensions during sws_scale.
    // This is the key optimisation for ProRes 4444 (YUVA444P10LE → BGRA):
    // converting+scaling at 640×360 is ~9× less work than at 1920×1080.
    int dstW = w, dstH = h;
    if (dstW > PREVIEW_MAX_WIDTH || dstH > PREVIEW_MAX_HEIGHT) {
        float scaleX = static_cast<float>(PREVIEW_MAX_WIDTH)  / static_cast<float>(w);
        float scaleY = static_cast<float>(PREVIEW_MAX_HEIGHT) / static_cast<float>(h);
        float scale  = std::min(scaleX, scaleY);
        dstW = std::max(1, static_cast<int>(w * scale));
        dstH = std::max(2, static_cast<int>(h * scale) & ~1);   // even: packed-alpha halves
    }

    // Re-use cached SwsContext when dimensions and format haven't changed
    if (s_swsCtx && (w != s_swsSrcW || h != s_swsSrcH ||
                     dstW != s_swsDstW || dstH != s_swsDstH || srcFmt != s_swsFmt)) {
        sws_freeContext(s_swsCtx);
        s_swsCtx = nullptr;
    }
    if (!s_swsCtx) {
        s_swsCtx = sws_getContext(
            w, h, srcFmt,
            dstW, dstH, AV_PIX_FMT_BGRA,
            SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
        s_swsSrcW = w;  s_swsSrcH = h;
        s_swsDstW = dstW; s_swsDstH = dstH;
        s_swsFmt  = srcFmt;
    }
    if (!s_swsCtx) return {};

    QImage result(dstW, dstH, QImage::Format_ARGB32);
    uint8_t* dstData[1] = { result.bits() };
    int dstStride[1] = { static_cast<int>(result.bytesPerLine()) };

    sws_scale(s_swsCtx, avf->data, avf->linesize, 0, h, dstData, dstStride);

    return result;
}
#endif

// ═════════════════════════════════════════════════════════════════════════════
// Construction


QImage ShotComposer::extractVideoThumbnail(const std::string& path)
{
    // Check cache
    auto it = m_videoFrameCache.find(path);
    if (it != m_videoFrameCache.end())
        return it->second;

    spdlog::info("ShotComposer: extractVideoThumbnail called for '{}'", path);


    // Resolve video path to absolute (try .webm first, fall back to .mov)
    QString videoPath = QString::fromStdString(path);
    auto resolveVideoPath = [](const QString& vp) -> QString {
        if (QFileInfo::exists(vp))
            return QFileInfo(vp).absoluteFilePath();
        QString alt = QApplication::applicationDirPath() + "/../../../" + vp;
        if (QFileInfo::exists(alt))
            return QFileInfo(alt).absoluteFilePath();
        return {};
    };

    QString resolved = resolveVideoPath(videoPath);
    // If not found, try .mov extension as fallback
    if (resolved.isEmpty()) {
        int dot = videoPath.lastIndexOf('.');
        if (dot >= 0) {
            QString movAlt = videoPath.left(dot) + ".mov";
            if (movAlt != videoPath)
                resolved = resolveVideoPath(movAlt);
        }
    }
    if (resolved.isEmpty()) {
        spdlog::error("ShotComposer: video file not found: '{}'", path);
        return {};
    }
    videoPath = resolved;

    // Decode the first frame in-process (software decoder: one frame does
    // not pay for a GPU decoder). This used to run ffmpeg.exe on the UI
    // thread -- a process spawn plus a temp PNG round trip, waiting up to
    // 5 s to start and 15 s to finish, per thumbnail.
    QImage frame;
#ifdef ROUNDTABLE_HAS_FFMPEG
    {
        VideoDecoder decoder;
        if (decoder.open(utf8ToPath(videoPath.toStdString()), /*forceSoftware=*/true)) {
            DecodedFrame df;
            if (decoder.decodeNext(df))
                frame = decodeFrameToQImage(df, &decoder);
        } else {
            spdlog::warn("ShotComposer: cannot open '{}' for a thumbnail: {}",
                         videoPath.toStdString(), decoder.lastError());
        }
    }
#endif
    if (!frame.isNull()) {
        frame = frame.convertToFormat(QImage::Format_ARGB32);
        // Chroma-key generated H264_Green cache entries and legacy
        // GREEN-suffixed media so thumbnails never expose the key colour.
        if (isGreenScreenMediaPath(utf8ToPath(path)))
            chromaKeyInPlace(frame.bits(),
                             static_cast<size_t>(frame.width()) * frame.height());
        m_videoFrameCache[path] = frame;
        spdlog::info("ShotComposer: extracted video thumbnail {}x{} from '{}'",
            frame.width(), frame.height(), path);
    } else {
        spdlog::warn("ShotComposer: no thumbnail frame decoded from '{}'",
                     videoPath.toStdString());
    }
    return frame;
}

// ═════════════════════════════════════════════════════════════════════════════
// Video looping playback — uses VideoDecoder to decode frames in real-time
// ═════════════════════════════════════════════════════════════════════════════

ShotComposer::VideoPlaybackState::~VideoPlaybackState()
{
    // Signal the worker thread to exit and wait for it
    stopWorker.store(true, std::memory_order_release);
    wakeWorker.notify_one();
    if (workerThread.joinable())
        workerThread.join();
}

std::shared_ptr<ShotComposer::VideoPlaybackState>
ShotComposer::getOrCreateVideoPlayer(const std::string& path)
{
#ifdef ROUNDTABLE_HAS_FFMPEG
    auto it = m_videoPlayers.find(path);
    if (it != m_videoPlayers.end() && it->second->decoder && it->second->decoder->isOpen())
        return it->second;

    // Resolve the video file path — use as-is (no forced .mov extension).
    QString qpath = QString::fromStdString(path);

    QString fileName = QFileInfo(qpath).fileName();
    QString appDir  = QApplication::applicationDirPath() + "/../../../";

    // Search order: direct path, assets/videos/, app-relative paths
    QStringList searchPaths = {
        qpath,
        QString("assets/videos/%1").arg(fileName),
        appDir + qpath,
        appDir + "assets/videos/" + fileName,
    };
    std::filesystem::path resolvedPath;
    for (const auto& sp : searchPaths) {
        if (QFileInfo::exists(sp)) {
            resolvedPath = sp.toStdString();
            break;
        }
    }
    if (resolvedPath.empty()) {
        spdlog::warn("ShotComposer: video file not found for '{}'", path);
        return nullptr;
    }

    auto state = std::make_shared<VideoPlaybackState>();
    state->decoder = std::make_unique<VideoDecoder>();
    if (!state->decoder->open(resolvedPath)) {
        spdlog::error("ShotComposer: failed to open video '{}': {}",
                      path, state->decoder->lastError());
        return nullptr;
    }

    state->needsChromaKey = isGreenScreenMediaPath(resolvedPath);

    const auto& info = state->decoder->info();
    state->duration = info.duration;
    state->fps = info.fps > 0 ? info.fps : 30.0;
    state->currentTime = 0.0;

    // Decode the first frame
    DecodedFrame df;
    if (state->decoder->decodeNext(df)) {
        QImage firstFrame = decodeFrameToQImage(df, state->decoder.get());
        if (!firstFrame.isNull()) {
            // Unpack packed-alpha immediately so the initial display is correct
            if (firstFrame.height() > firstFrame.width() &&
                (firstFrame.height() % 2 == 0) &&
                firstFrame.height() >= firstFrame.width() * 1.8) {
                firstFrame = unpackPackedAlpha(firstFrame.bits(),
                    static_cast<uint32_t>(firstFrame.width()),
                    static_cast<uint32_t>(firstFrame.height()));
            }
            // Chroma-key green-screen media
            if (state->needsChromaKey) {
                chromaKeyInPlace(firstFrame.bits(),
                    static_cast<size_t>(firstFrame.width()) * firstFrame.height());
            }
        }
        state->lastFrame = std::move(firstFrame);
    }

    spdlog::info("ShotComposer: opened video player for '{}' — {:.1f}s @ {:.1f}fps",
                 path, state->duration, state->fps);

    // Launch a persistent worker thread for this video player.
    // It sleeps until woken by advanceVideoPlayer(), decodes one frame,
    // then goes back to sleep. No per-frame thread creation overhead.
    std::weak_ptr<VideoPlaybackState> wp = state;
    state->workerThread = std::thread([wp]() {
        while (true) {
            auto sp = wp.lock();
            if (!sp) return;

            // Wait until signalled to decode (or stop)
            {
                std::unique_lock<std::mutex> lk(sp->workerMutex);
                sp->wakeWorker.wait(lk, [&sp]() {
                    return sp->decoding.load(std::memory_order_acquire)
                        || sp->stopWorker.load(std::memory_order_acquire);
                });
            }

            if (sp->stopWorker.load(std::memory_order_acquire))
                return;

            if (!sp->decoder) {
                sp->decoding.store(false, std::memory_order_release);
                continue;
            }

            DecodedFrame df;
            bool ok = sp->decoder->decodeNext(df);
            if (!ok) {
                // EOF — loop
                sp->decoder->seek(0.0, SeekMode::Keyframe);
                ok = sp->decoder->decodeNext(df);
            }
            if (ok) {
                QImage frame = decodeFrameToQImage(df, sp->decoder.get());
                if (!frame.isNull()) {
                    // Unpack packed-alpha on the worker thread (not the UI thread)
                    // to avoid blocking the 16ms paint timer.
                    if (frame.height() > frame.width() && (frame.height() % 2 == 0) &&
                        frame.height() >= frame.width() * 1.8) {
                        frame = unpackPackedAlpha(frame.bits(),
                            static_cast<uint32_t>(frame.width()),
                            static_cast<uint32_t>(frame.height()));
                    }
                    // Chroma-key green-screen media
                    if (sp->needsChromaKey) {
                        chromaKeyInPlace(frame.bits(),
                            static_cast<size_t>(frame.width()) * frame.height());
                    }
                    std::lock_guard<std::mutex> lk(sp->frameMutex);
                    sp->pendingFrame = std::move(frame);
                    sp->frameReady.store(true, std::memory_order_release);
                }
            }
            sp->decoding.store(false, std::memory_order_release);
        }
    });

    m_videoPlayers[path] = state;
    return state;
#else
    (void)path;
    return nullptr;
#endif
}

QImage ShotComposer::advanceVideoPlayer(
    const std::shared_ptr<ShotComposer::VideoPlaybackState>& player, float dt)
{
#ifdef ROUNDTABLE_HAS_FFMPEG
    if (!player || !player->decoder || !player->decoder->isOpen())
        return {};

    player->currentTime += static_cast<double>(dt);

    // Note: looping (seek) is handled by the background decode thread
    // to avoid racing with decodeNext(). Just wrap currentTime here.
    if (player->currentTime >= player->duration)
        player->currentTime = 0.0;

    // Check if a previously-dispatched async frame is ready
    QImage result;
    if (player->frameReady.load(std::memory_order_acquire)) {
        std::lock_guard<std::mutex> lk(player->frameMutex);
        result = std::move(player->pendingFrame);
        player->frameReady.store(false, std::memory_order_release);
        if (!result.isNull())
            player->lastFrame = result;
    }

    // Rate-limit: only kick off a new decode at the video's native frame rate
    double frameDuration = 1.0 / player->fps;
    player->frameAccum += static_cast<double>(dt);
    if (player->frameAccum < frameDuration * 0.8)
        return result;  // Return any frame we grabbed above, or empty
    player->frameAccum = 0.0;

    // If a decode is already in flight, skip
    if (player->decoding.load(std::memory_order_acquire))
        return result;

    // Signal the persistent worker thread to decode the next frame
    player->decoding.store(true, std::memory_order_release);
    player->wakeWorker.notify_one();

    return result;
#else
    (void)player; (void)dt;
    return {};
#endif
}

// ═════════════════════════════════════════════════════════════════════════════
// Preview — composite ALL visible character layers into SpinePreviewWidget
// ═════════════════════════════════════════════════════════════════════════════


} // namespace rt
