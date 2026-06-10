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
#include "media/VideoDecoder.h"
#include "media/FrameCache.h"
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

// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
// AVFrame â†’ QImage conversion helper  (YUV420P / NV12 / HW â†’ BGRA)
// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
#ifdef ROUNDTABLE_HAS_FFMPEG
// Thread-local cached SwsContext â€” safe with async video decode threads
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
/// content (e.g. 1080Ã—1888) passes through at full quality instead of being
/// squashed to ~411Ã—720 by the old 1280Ã—720 cap.
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

    // Use the AVFrame's actual pixel format â€” handles all formats correctly
    AVPixelFormat srcFmt = static_cast<AVPixelFormat>(avf->format);

    // If already BGRA at a reasonable size, create QImage directly
    if (srcFmt == AV_PIX_FMT_BGRA && w <= PREVIEW_MAX_WIDTH && h <= PREVIEW_MAX_HEIGHT) {
        QImage img(avf->data[0], w, h, avf->linesize[0],
                   QImage::Format_ARGB32);
        return img.copy();
    }

    // Downscale to preview dimensions during sws_scale.
    // This is the key optimisation for ProRes 4444 (YUVA444P10LE â†’ BGRA):
    // converting+scaling at 640Ã—360 is ~9Ã— less work than at 1920Ã—1080.
    int dstW = w, dstH = h;
    if (dstW > PREVIEW_MAX_WIDTH || dstH > PREVIEW_MAX_HEIGHT) {
        float scaleX = static_cast<float>(PREVIEW_MAX_WIDTH)  / static_cast<float>(w);
        float scaleY = static_cast<float>(PREVIEW_MAX_HEIGHT) / static_cast<float>(h);
        float scale  = std::min(scaleX, scaleY);
        dstW = std::max(1, static_cast<int>(w * scale));
        dstH = std::max(1, static_cast<int>(h * scale));
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

// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
// Construction


QImage ShotComposer::extractVideoThumbnail(const std::string& path)
{
    // Check cache
    auto it = m_videoFrameCache.find(path);
    if (it != m_videoFrameCache.end())
        return it->second;

    spdlog::info("ShotComposer: extractVideoThumbnail called for '{}'", path);

    // Find ffmpeg executable â€” check multiple locations
    QString ffmpegPath;
    QStringList searchPaths = {
        QStringLiteral("third_party/ffmpeg/bin/ffmpeg.exe"),
        QApplication::applicationDirPath() + "/../../../third_party/ffmpeg/bin/ffmpeg.exe",
        QApplication::applicationDirPath() + "/ffmpeg.exe",
        QStringLiteral("tools/ffmpeg/ffmpeg.exe"),
    };
    for (const auto& p : searchPaths) {
        spdlog::debug("ShotComposer: checking ffmpeg at '{}'", p.toStdString());
        if (QFileInfo::exists(p)) {
            ffmpegPath = QFileInfo(p).absoluteFilePath();
            spdlog::info("ShotComposer: found ffmpeg at '{}'", ffmpegPath.toStdString());
            break;
        }
    }
    if (ffmpegPath.isEmpty()) {
        spdlog::error("ShotComposer: ffmpeg not found in any search path, cannot extract video thumbnail");
        return {};
    }

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

    // Temp output file
    QString tempDir = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
    QString tempPath = tempDir + "/rt_video_thumb_" +
        QString::number(qHash(videoPath)) + ".png";

    // Run ffmpeg to extract first frame
    QStringList args = {
        "-y", "-i", videoPath,
        "-vframes", "1",
        "-f", "image2",
        tempPath
    };
    spdlog::info("ShotComposer: running: {} {}",
        ffmpegPath.toStdString(), args.join(' ').toStdString());

    QProcess proc;
    proc.setProcessChannelMode(QProcess::SeparateChannels);
#ifdef _WIN32
    proc.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* args) {
        args->flags |= CREATE_NO_WINDOW;
    });
#endif
    proc.start(ffmpegPath, args);

    if (!proc.waitForStarted(5000)) {
        spdlog::error("ShotComposer: ffmpeg failed to start: {}",
            proc.errorString().toStdString());
        return {};
    }

    if (!proc.waitForFinished(15000)) {
        spdlog::error("ShotComposer: ffmpeg timed out after 15s");
        proc.kill();
        return {};
    }

    if (proc.exitCode() != 0) {
        spdlog::warn("ShotComposer: ffmpeg exit code {}: {}",
            proc.exitCode(),
            proc.readAllStandardError().toStdString());
    }

    QImage frame(tempPath);
    if (!frame.isNull()) {
        frame = frame.convertToFormat(QImage::Format_ARGB32);
        // Chroma-key GREEN-suffixed media so thumbnails never show green
        {
            std::string fn = QFileInfo(videoPath).fileName().toUpper().toStdString();
            if (fn.find("GREEN") != std::string::npos) {
                chromaKeyInPlace(frame.bits(),
                                 static_cast<size_t>(frame.width()) * frame.height());
            }
        }
        m_videoFrameCache[path] = frame;
        spdlog::info("ShotComposer: extracted video thumbnail {}x{} from '{}'",
            frame.width(), frame.height(), path);
    } else {
        if (QFileInfo::exists(tempPath)) {
            spdlog::warn("ShotComposer: temp file exists at '{}' but failed to load as image",
                tempPath.toStdString());
        } else {
            spdlog::warn("ShotComposer: ffmpeg did not create output file at '{}'",
                tempPath.toStdString());
        }
    }

    // Clean up temp file
    QFile::remove(tempPath);

    return frame;
}

// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
// Video looping playback â€” uses VideoDecoder to decode frames in real-time
// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•

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

    // Detect GREEN-suffixed chroma-key files
    {
        std::string fn = pathToUtf8(resolvedPath.filename());
        std::transform(fn.begin(), fn.end(), fn.begin(),
                       [](unsigned char c) { return std::toupper(c); });
        state->needsChromaKey = (fn.find("GREEN") != std::string::npos);
    }

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

    spdlog::info("ShotComposer: opened video player for '{}' â€” {:.1f}s @ {:.1f}fps",
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
                // EOF â€” loop
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

// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
// Preview â€” composite ALL visible character layers into SpinePreviewWidget
// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•


} // namespace rt
