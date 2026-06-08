/*
 * ImageSequence.cpp — Writes individual frames as PNG/BMP/JPEG/TIFF/EXR.
 *
 * Uses stb_image_write for PNG/BMP/JPEG (no FFmpeg dependency needed).
 * EXR/TIFF use FFmpeg if available.
 */

#include "formats/ImageSequence.h"
#include "PathUtils.h"

#include <spdlog/spdlog.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

#define STB_IMAGE_WRITE_IMPLEMENTATION
// Only define once — if already defined elsewhere, guard it
#ifndef STB_IMAGE_WRITE_ALREADY_DEFINED
#include <stb_image_write.h>
#define STB_IMAGE_WRITE_ALREADY_DEFINED
#endif

namespace rt {

namespace {
// stb_image_write callback that appends encoded bytes to a std::vector,
// so the file itself is written via a wide-path-safe std::ofstream rather
// than stb's internal fopen() (which uses the ANSI codepage on Windows and
// cannot open paths with non-CP_ACP characters).
void appendToBuffer(void* context, void* data, int size)
{
    auto* buf = static_cast<std::vector<uint8_t>*>(context);
    const auto* bytes = static_cast<const uint8_t*>(data);
    buf->insert(buf->end(), bytes, bytes + size);
}
} // namespace

ImageSequence::ImageSequence() = default;
ImageSequence::~ImageSequence() { shutdown(); }

void ImageSequence::setOutputDirectory(const std::filesystem::path& dir)
{
    m_outputDir = dir;
}

void ImageSequence::setFilenamePattern(const std::string& pattern)
{
    m_filenamePattern = pattern;
}

bool ImageSequence::init(const EncoderConfig& config)
{
    m_config = config;
    m_framesEncoded = 0;

    if (m_outputDir.empty()) {
        m_lastError = "ImageSequence: Output directory not set";
        return false;
    }

    std::error_code ec;
    std::filesystem::create_directories(m_outputDir, ec);
    if (ec) {
        m_lastError = "ImageSequence: Cannot create output dir: " + ec.message();
        return false;
    }

    m_initialized = true;
    spdlog::info("ImageSequence: {}x{} format={} to {}",
                 config.width, config.height,
                 static_cast<int>(config.imageFormat), pathToUtf8(m_outputDir));
    return true;
}

bool ImageSequence::encodeFrame(const uint8_t* rgbaPixels, int64_t frameIndex)
{
    if (!m_initialized) return false;

    // Build filename
    char numBuf[64];
    std::snprintf(numBuf, sizeof(numBuf), m_filenamePattern.c_str(),
                  static_cast<int>(frameIndex));

    const char* ext = ".png";
    switch (m_config.imageFormat) {
        case ImageFormat::PNG:  ext = ".png"; break;
        case ImageFormat::BMP:  ext = ".bmp"; break;
        case ImageFormat::JPEG: ext = ".jpg"; break;
        case ImageFormat::TIFF: ext = ".tiff"; break;
        case ImageFormat::EXR:  ext = ".exr"; break;
        default: break;
    }

    auto path = m_outputDir / (std::string(numBuf) + ext);
    int w = static_cast<int>(m_config.width);
    int h = static_cast<int>(m_config.height);
    int ok = 0;

    // Encode into memory, then write the bytes through std::ofstream, which
    // takes the std::filesystem::path directly and is wide-path-safe on
    // Windows.  stb's filename-based writers go through fopen() (ANSI) and
    // crash/fail on paths containing non-CP_ACP characters.
    std::vector<uint8_t> encoded;
    switch (m_config.imageFormat) {
        case ImageFormat::PNG:
            ok = stbi_write_png_to_func(appendToBuffer, &encoded, w, h, 4, rgbaPixels, w * 4);
            break;
        case ImageFormat::BMP:
            ok = stbi_write_bmp_to_func(appendToBuffer, &encoded, w, h, 4, rgbaPixels);
            break;
        case ImageFormat::JPEG:
            ok = stbi_write_jpg_to_func(appendToBuffer, &encoded, w, h, 4, rgbaPixels,
                                        m_config.jpegQuality);
            break;
        default:
            // TIFF/EXR would need FFmpeg or dedicated library
            spdlog::warn("ImageSequence: {} format not implemented, using PNG",
                         static_cast<int>(m_config.imageFormat));
            ok = stbi_write_png_to_func(appendToBuffer, &encoded, w, h, 4, rgbaPixels, w * 4);
            break;
    }

    if (ok) {
        std::ofstream out(path, std::ios::binary);
        if (out.is_open()) {
            out.write(reinterpret_cast<const char*>(encoded.data()),
                      static_cast<std::streamsize>(encoded.size()));
            ok = out.good() ? 1 : 0;
        } else {
            ok = 0;
        }
    }

    if (!ok) {
        m_lastError = "ImageSequence: Failed to write " + pathToUtf8(path);
        spdlog::error("{}", m_lastError);
        return false;
    }

    ++m_framesEncoded;
    return true;
}

int ImageSequence::flush()
{
    return 0; // No buffering — each frame is immediately written
}

void ImageSequence::shutdown()
{
    m_initialized = false;
    m_flushedPackets.clear();
}

} // namespace rt
