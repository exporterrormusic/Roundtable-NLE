/*
 * ExportIntegrity -- strict frame and finished-file validation for export.
 *
 * Export is intentionally fail-closed: a frame with an incomplete payload is
 * never handed to an encoder, and a staged container is never published until
 * every expected video frame decodes at the exact expected timestamp.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <chrono>
#include <filesystem>
#include <functional>
#include <string>

namespace rt {

struct CachedFrame;

struct ExportFrameValidation
{
    bool        valid{false};
    bool        useRgba16f{false};
    bool        needsBgraRepack{false};
    std::string error;
};

/// Validate the payload that will be handed to the encoder.  Exact dimensions
/// are required.  Padded BGRA rows are accepted, but the caller must repack
/// them when needsBgraRepack is true because Encoder::encodeFrame takes a
/// tightly-packed buffer.
[[nodiscard]] ExportFrameValidation validateExportFrame(
    const CachedFrame* frame, uint32_t expectedWidth,
    uint32_t expectedHeight, bool allowRgba16f);

struct ExportFileValidation
{
    bool        ok{false};
    int64_t     decodedFrames{0};
    std::string error;
};

using ExportValidationProgressFn = std::function<void(int64_t, int64_t)>;

/// Open and fully decode a staged export.  The decoded picture count,
/// dimensions, and presentation timestamp of every frame must exactly match
/// the requested constant-frame-rate timeline.
[[nodiscard]] ExportFileValidation validateExportFile(
    const std::filesystem::path& path, int64_t expectedFrames,
    uint32_t expectedWidth, uint32_t expectedHeight,
    int fpsNum, int fpsDen,
    const ExportValidationProgressFn& progress = nullptr);

size_t cleanupAbandonedStagedExports(
    const std::filesystem::path& directory,
    std::chrono::hours maxAge = std::chrono::hours(24));

/// Create a collision-resistant staging filename beside the destination.
/// Keeping both files on one volume lets publication be an atomic rename.
[[nodiscard]] std::filesystem::path makeStagedExportPath(
    const std::filesystem::path& destination, uint32_t jobId);

/// Atomically replace destination with stagedPath.  On failure, destination
/// is left untouched and stagedPath remains available for caller cleanup.
[[nodiscard]] bool publishStagedExport(
    const std::filesystem::path& stagedPath,
    const std::filesystem::path& destination,
    std::string& error);

} // namespace rt
