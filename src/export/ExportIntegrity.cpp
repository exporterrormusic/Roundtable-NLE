/*
 * ExportIntegrity.cpp -- fail-closed export validation and publication.
 */

#include "ExportIntegrity.h"

#include "PathUtils.h"
#include "cache/FrameCache.h"

#include <atomic>
#include <algorithm>
#include <chrono>
#include <limits>
#include <system_error>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

#ifdef ROUNDTABLE_HAS_FFMPEG
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/mathematics.h>
}
#endif

namespace rt {

namespace {

bool validBufferLayout(size_t bufferSize, uint32_t stride,
                       uint32_t minimumRowBytes, uint32_t height) noexcept
{
    if (stride < minimumRowBytes || height == 0)
        return false;
    const size_t strideSize = static_cast<size_t>(stride);
    const size_t heightSize = static_cast<size_t>(height);
    if (heightSize > std::numeric_limits<size_t>::max() / strideSize)
        return false;
    return bufferSize >= strideSize * heightSize;
}

#ifdef ROUNDTABLE_HAS_FFMPEG
std::string ffmpegError(int error)
{
    char buffer[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(error, buffer, sizeof(buffer));
    return buffer;
}
#endif

} // namespace

ExportFrameValidation validateExportFrame(
    const CachedFrame* frame, uint32_t expectedWidth,
    uint32_t expectedHeight, bool allowRgba16f)
{
    ExportFrameValidation result;
    if (!frame) {
        result.error = "compositor returned no frame";
        return result;
    }
    if (expectedWidth == 0 || expectedHeight == 0 ||
        frame->width != expectedWidth || frame->height != expectedHeight) {
        result.error = "compositor returned unexpected frame dimensions";
        return result;
    }
    if (expectedWidth > std::numeric_limits<uint32_t>::max() / 8u) {
        result.error = "export frame dimensions overflow row size";
        return result;
    }

    const uint32_t bgraRowBytes = expectedWidth * 4u;
    const bool validBgra = validBufferLayout(
        frame->pixels.size(), frame->stride, bgraRowBytes, expectedHeight);

    const uint32_t rgba16fRowBytes = expectedWidth * 8u;
    const bool validRgba16f =
        allowRgba16f && frame->depth == 16 &&
        validBufferLayout(frame->rgba16f.size(), frame->rgba16fStride,
                          rgba16fRowBytes, expectedHeight);

    if (validRgba16f) {
        result.valid = true;
        result.useRgba16f = true;
        return result;
    }
    if (validBgra) {
        result.valid = true;
        result.needsBgraRepack = frame->stride != bgraRowBytes;
        return result;
    }

    result.error = allowRgba16f && frame->depth == 16
        ? "compositor returned incomplete BGRA and RGBA16F payloads"
        : "compositor returned an incomplete BGRA payload";
    return result;
}

ExportFileValidation validateExportFile(
    const std::filesystem::path& path, int64_t expectedFrames,
    uint32_t expectedWidth, uint32_t expectedHeight,
    int fpsNum, int fpsDen, const ExportValidationProgressFn& progress)
{
    ExportFileValidation result;
    if (path.empty() || expectedFrames <= 0 || expectedWidth == 0 ||
        expectedHeight == 0 || fpsNum <= 0 || fpsDen <= 0) {
        result.error = "invalid export validation parameters";
        return result;
    }

#ifndef ROUNDTABLE_HAS_FFMPEG
    (void)path;
    result.error = "FFmpeg is unavailable; export cannot be validated";
    return result;
#else
    AVFormatContext* format = nullptr;
    AVCodecContext* decoder = nullptr;
    AVPacket* packet = nullptr;
    AVFrame* frame = nullptr;

    auto cleanup = [&]() {
        av_frame_free(&frame);
        av_packet_free(&packet);
        avcodec_free_context(&decoder);
        avformat_close_input(&format);
    };
    auto fail = [&](std::string message) {
        result.error = std::move(message);
        cleanup();
        return result;
    };

    int ret = avformat_open_input(&format, pathToUtf8(path).c_str(), nullptr, nullptr);
    if (ret < 0)
        return fail("cannot open staged export: " + ffmpegError(ret));
    ret = avformat_find_stream_info(format, nullptr);
    if (ret < 0)
        return fail("cannot read staged export stream information: " + ffmpegError(ret));

    const int videoIndex = av_find_best_stream(
        format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (videoIndex < 0)
        return fail("staged export has no decodable video stream");
    AVStream* stream = format->streams[videoIndex];
    if (!stream || !stream->codecpar)
        return fail("staged export has invalid video stream parameters");
    if (stream->codecpar->width != static_cast<int>(expectedWidth) ||
        stream->codecpar->height != static_cast<int>(expectedHeight)) {
        return fail("staged export dimensions do not match the requested export");
    }

    const AVCodec* codec = avcodec_find_decoder(stream->codecpar->codec_id);
    if (!codec)
        return fail("no decoder is available to validate the staged export");
    decoder = avcodec_alloc_context3(codec);
    if (!decoder)
        return fail("cannot allocate export validation decoder");
    ret = avcodec_parameters_to_context(decoder, stream->codecpar);
    if (ret < 0)
        return fail("cannot configure export validation decoder: " + ffmpegError(ret));
    decoder->err_recognition = AV_EF_CRCCHECK | AV_EF_BITSTREAM |
                               AV_EF_BUFFER | AV_EF_EXPLODE | AV_EF_CAREFUL;
    // Let FFmpeg use the codec's safe frame/slice threading.  Validation is
    // still a complete independent decode; only the decoder execution is
    // parallelized.
    decoder->thread_count = 0;
    decoder->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
    ret = avcodec_open2(decoder, codec, nullptr);
    if (ret < 0)
        return fail("cannot open export validation decoder: " + ffmpegError(ret));

    packet = av_packet_alloc();
    frame = av_frame_alloc();
    if (!packet || !frame)
        return fail("cannot allocate export validation buffers");

    const AVRational expectedTimeBase{fpsDen, fpsNum};
    auto drain = [&]() -> bool {
        while (true) {
            const int receiveRet = avcodec_receive_frame(decoder, frame);
            if (receiveRet == AVERROR(EAGAIN) || receiveRet == AVERROR_EOF)
                return true;
            if (receiveRet < 0) {
                result.error = "video decode failed during export validation: " +
                               ffmpegError(receiveRet);
                return false;
            }
            if ((frame->flags & AV_FRAME_FLAG_CORRUPT) != 0 ||
                frame->decode_error_flags != 0) {
                result.error = "decoder reported a corrupt exported frame";
                av_frame_unref(frame);
                return false;
            }
            if (frame->width != static_cast<int>(expectedWidth) ||
                frame->height != static_cast<int>(expectedHeight)) {
                result.error = "decoded export frame has unexpected dimensions";
                av_frame_unref(frame);
                return false;
            }
            if (result.decodedFrames >= expectedFrames) {
                result.error = "staged export contains more video frames than requested";
                av_frame_unref(frame);
                return false;
            }

            const int64_t timestamp =
                frame->best_effort_timestamp != AV_NOPTS_VALUE
                    ? frame->best_effort_timestamp
                    : frame->pts;
            if (timestamp == AV_NOPTS_VALUE ||
                av_compare_ts(timestamp, stream->time_base,
                              result.decodedFrames, expectedTimeBase) != 0) {
                result.error = "staged export has a missing or discontinuous frame timestamp";
                av_frame_unref(frame);
                return false;
            }
            ++result.decodedFrames;
            if (progress && (result.decodedFrames == expectedFrames
                || result.decodedFrames == 1
                || result.decodedFrames % std::max<int64_t>(1, expectedFrames / 100) == 0))
                progress(result.decodedFrames, expectedFrames);
            av_frame_unref(frame);
        }
    };

    int readRet = 0;
    while ((readRet = av_read_frame(format, packet)) >= 0) {
        if (packet->stream_index == videoIndex) {
            ret = avcodec_send_packet(decoder, packet);
            if (ret < 0) {
                av_packet_unref(packet);
                return fail("cannot submit an exported packet for validation: " +
                            ffmpegError(ret));
            }
            if (!drain()) {
                av_packet_unref(packet);
                return fail(result.error);
            }
        }
        av_packet_unref(packet);
    }
    if (readRet != AVERROR_EOF)
        return fail("staged export ended with a container read error: " +
                    ffmpegError(readRet));

    ret = avcodec_send_packet(decoder, nullptr);
    if (ret < 0 && ret != AVERROR_EOF)
        return fail("cannot flush export validation decoder: " + ffmpegError(ret));
    if (!drain())
        return fail(result.error);

    if (result.decodedFrames != expectedFrames) {
        return fail("staged export decoded " +
                    std::to_string(result.decodedFrames) + " of " +
                    std::to_string(expectedFrames) + " expected video frames");
    }

    cleanup();
    result.ok = true;
    return result;
#endif
}

size_t cleanupAbandonedStagedExports(const std::filesystem::path& directory,
                                      std::chrono::hours maxAge)
{
    if (directory.empty()) return 0;
    std::error_code ec;
    if (!std::filesystem::is_directory(directory, ec) || ec) return 0;
    const auto cutoff = std::filesystem::file_time_type::clock::now() - maxAge;
    size_t removed = 0;
    for (std::filesystem::directory_iterator it(directory, ec), end;
         !ec && it != end; it.increment(ec)) {
        const auto& entry = *it;
        if (!entry.is_regular_file(ec) || ec) { ec.clear(); continue; }
        const std::string name = pathToUtf8(entry.path().filename());
        if (name.find(".roundtable-job") == std::string::npos
            || !name.ends_with(".partial"))
            continue;
        const auto modified = entry.last_write_time(ec);
        if (ec) { ec.clear(); continue; }
        if (modified >= cutoff) continue;
        if (std::filesystem::remove(entry.path(), ec) && !ec) ++removed;
        ec.clear();
    }
    return removed;
}

std::filesystem::path makeStagedExportPath(
    const std::filesystem::path& destination, uint32_t jobId)
{
    if (destination.empty())
        return {};

    static std::atomic<uint64_t> counter{0};
    const uint64_t serial = counter.fetch_add(1, std::memory_order_relaxed);
    const uint64_t clock = static_cast<uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    const std::string suffix = ".roundtable-job" + std::to_string(jobId) +
                               "-" + std::to_string(clock) + "-" +
                               std::to_string(serial) + ".partial";
    auto filename = destination.filename().native();
    filename.append(std::filesystem::path(suffix).native());
    return destination.parent_path() / std::filesystem::path(filename);
}

bool publishStagedExport(const std::filesystem::path& stagedPath,
                         const std::filesystem::path& destination,
                         std::string& error)
{
    error.clear();
    std::error_code ec;
    if (stagedPath.empty() || destination.empty() ||
        !std::filesystem::is_regular_file(stagedPath, ec) || ec) {
        error = "staged export does not exist or is not a regular file";
        return false;
    }
    const auto stagedSize = std::filesystem::file_size(stagedPath, ec);
    if (ec || stagedSize == 0) {
        error = "staged export is empty or unreadable";
        return false;
    }

#ifdef _WIN32
    const std::wstring staged = stagedPath.wstring();
    const std::wstring final = destination.wstring();
    const DWORD attrs = GetFileAttributesW(final.c_str());
    const DWORD attributeError = attrs == INVALID_FILE_ATTRIBUTES
        ? GetLastError() : ERROR_SUCCESS;
    BOOL ok = FALSE;
    if (attrs != INVALID_FILE_ATTRIBUTES) {
        if ((attrs & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            error = "export destination is a directory";
            return false;
        }
        // ReplaceFileW itself is atomic on one volume.  Its documented flags
        // do not include a write-through option (that flag belongs to
        // MoveFileExW), so pass zero here for broad Windows compatibility.
        ok = ReplaceFileW(final.c_str(), staged.c_str(), nullptr,
                          0, nullptr, nullptr);
    } else if (attributeError == ERROR_FILE_NOT_FOUND ||
               attributeError == ERROR_PATH_NOT_FOUND) {
        ok = MoveFileExW(staged.c_str(), final.c_str(), MOVEFILE_WRITE_THROUGH);
    } else {
        error = "cannot inspect export destination: " +
                std::system_category().message(
                    static_cast<int>(attributeError));
        return false;
    }
    if (!ok) {
        const DWORD winError = GetLastError();
        error = "atomic export publication failed: " +
                std::system_category().message(static_cast<int>(winError));
        return false;
    }
#else
    std::filesystem::rename(stagedPath, destination, ec);
    if (ec) {
        error = "atomic export publication failed: " + ec.message();
        return false;
    }
#endif
    return true;
}

} // namespace rt
