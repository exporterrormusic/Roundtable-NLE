/*
 * SharedFileIO — share-mode file opens + FFmpeg AVIO callbacks.
 *
 * On Windows we bypass the CRT entirely and drive Win32 ReadFile /
 * SetFilePointerEx directly from the AVIO callbacks. _open_osfhandle +
 * _fdopen LOOK like a clean wrapper around our shared-mode HANDLE, but
 * some MSVCRT builds silently re-acquire the share state when the CRT
 * touches the descriptor, which would defeat FILE_SHARE_DELETE. Using
 * the raw HANDLE end-to-end guarantees the share mode set at CreateFile
 * time is what the kernel sees for the entire lifetime of the open.
 * On POSIX, FILE* is fine (no exclusive sharing happens there).
 *
 * Used by VideoDecoder (video demux), AudioFile (FFmpeg audio fallback
 * AND the libsndfile virtual-IO path) — previously each carried a
 * verbatim copy of this machinery.
 */

#pragma once

#include <cstdint>
#include <cstdio>
#include <filesystem>

namespace rt {

#ifdef _WIN32
/// Win32 HANDLE, stored as void* so this header doesn't drag in windows.h.
using SharedFileHandle = void*;
#else
using SharedFileHandle = std::FILE*;
#endif

/// Open `p` read-only with full sharing (READ | WRITE | DELETE on Windows).
/// Returns nullptr on failure (normalized — never INVALID_HANDLE_VALUE).
/// `logTag` names the subsystem in the share-mode self-test log lines
/// ("VideoDecoder", "AudioFile", ...).
[[nodiscard]] SharedFileHandle openSharedReadHandle(
    const std::filesystem::path& p, const char* logTag);

/// Close a handle from openSharedReadHandle. Null-safe.
void closeSharedHandle(SharedFileHandle h);

#ifdef ROUNDTABLE_HAS_FFMPEG
/// AVIO buffer size (FFmpeg-recommended).
inline constexpr int kSharedAvioBufSize = 32 * 1024;

/// AVIO read/seek callbacks whose opaque is a SharedFileHandle.
/// Pass to avio_alloc_context() with the handle as `opaque`.
int sharedAvioRead(void* opaque, uint8_t* buf, int bufSize);
int64_t sharedAvioSeek(void* opaque, int64_t offset, int whence);
#endif

} // namespace rt
