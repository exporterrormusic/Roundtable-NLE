/*
 * SharedFileIO.cpp — share-mode file opens + FFmpeg AVIO callbacks.
 * See SharedFileIO.h for the rationale.
 */

#include "decode/SharedFileIO.h"
#include "PathUtils.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cerrno>
#include <limits>
#include <new>

#ifdef _WIN32
#include <windows.h>
#endif

#ifdef ROUNDTABLE_HAS_FFMPEG
extern "C" {
#include <libavformat/avio.h>
#include <libavutil/error.h>
}
#endif

namespace rt {

SharedFileHandle openSharedReadHandle(const std::filesystem::path& p,
                                      const char* logTag)
{
#ifdef _WIN32
    HANDLE h = ::CreateFileW(p.wstring().c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return nullptr;

    // Share-mode self-test. If h was really opened with FILE_SHARE_DELETE,
    // we can immediately reopen the SAME file from our own process with
    // DELETE access (kernel-enforced). If this fails, our first open
    // didn't actually get FILE_SHARE_DELETE — that's a bug we own. If it
    // succeeds, the file is shared-delete-able and any "locked" complaint
    // from Explorer must be coming from a different process or a Windows
    // service (Defender, indexer, etc.).
    HANDLE h2 = ::CreateFileW(p.wstring().c_str(),
        DELETE | SYNCHRONIZE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h2 != INVALID_HANDLE_VALUE) {
        spdlog::debug("{}: share-mode self-test PASSED '{}'",
                      logTag, pathToUtf8(p));
        ::CloseHandle(h2);
    } else {
        DWORD err = ::GetLastError();
        spdlog::warn("{}: share-mode self-test FAILED '{}' "
                     "GetLastError={} — FILE_SHARE_DELETE not active",
                     logTag, pathToUtf8(p), err);
    }
    return static_cast<SharedFileHandle>(h);
#else
    (void)logTag;
    return std::fopen(pathToUtf8(p).c_str(), "rb");
#endif
}

void closeSharedHandle(SharedFileHandle h)
{
    if (!h) return;
#ifdef _WIN32
    ::CloseHandle(static_cast<HANDLE>(h));
#else
    std::fclose(h);
#endif
}

bool readSharedFileBytes(const std::filesystem::path& p,
                         std::vector<std::uint8_t>& out,
                         const char* logTag)
{
    out.clear();
    SharedFileHandle raw = openSharedReadHandle(p, logTag);
    if (!raw) return false;

    struct HandleGuard {
        SharedFileHandle handle;
        ~HandleGuard() { closeSharedHandle(handle); }
    } guard{raw};

#ifdef _WIN32
    HANDLE h = static_cast<HANDLE>(raw);
    LARGE_INTEGER size{};
    if (!::GetFileSizeEx(h, &size) || size.QuadPart <= 0)
        return false;
    if (static_cast<unsigned long long>(size.QuadPart) >
        static_cast<unsigned long long>(std::numeric_limits<std::size_t>::max()))
        return false;

    try {
        out.resize(static_cast<std::size_t>(size.QuadPart));
    } catch (const std::bad_alloc&) {
        out.clear();
        return false;
    }

    std::size_t done = 0;
    while (done < out.size()) {
        const std::size_t remaining = out.size() - done;
        const DWORD request = static_cast<DWORD>(std::min<std::size_t>(
            remaining, static_cast<std::size_t>(std::numeric_limits<DWORD>::max())));
        DWORD got = 0;
        if (!::ReadFile(h, out.data() + done, request, &got, nullptr) || got == 0) {
            out.clear();
            return false;
        }
        done += got;
    }
#else
    std::FILE* fp = raw;
    if (std::fseek(fp, 0, SEEK_END) != 0) return false;
    const long size = std::ftell(fp);
    if (size <= 0 || std::fseek(fp, 0, SEEK_SET) != 0) return false;
    try {
        out.resize(static_cast<std::size_t>(size));
    } catch (const std::bad_alloc&) {
        out.clear();
        return false;
    }
    if (std::fread(out.data(), 1, out.size(), fp) != out.size()) {
        out.clear();
        return false;
    }
#endif

    return true;
}

#ifdef ROUNDTABLE_HAS_FFMPEG

int sharedAvioRead(void* opaque, uint8_t* buf, int bufSize)
{
#ifdef _WIN32
    HANDLE h = static_cast<HANDLE>(opaque);
    DWORD got = 0;
    if (!::ReadFile(h, buf, static_cast<DWORD>(bufSize), &got, nullptr))
        return AVERROR(EIO);
    if (got == 0) return AVERROR_EOF;
    return static_cast<int>(got);
#else
    FILE* fp = static_cast<FILE*>(opaque);
    size_t n = std::fread(buf, 1, static_cast<size_t>(bufSize), fp);
    if (n == 0) {
        if (std::feof(fp)) return AVERROR_EOF;
        return AVERROR(errno ? errno : EIO);
    }
    return static_cast<int>(n);
#endif
}

int64_t sharedAvioSeek(void* opaque, int64_t offset, int whence)
{
#ifdef _WIN32
    HANDLE h = static_cast<HANDLE>(opaque);
    if (whence == AVSEEK_SIZE) {
        LARGE_INTEGER sz;
        if (!::GetFileSizeEx(h, &sz)) return AVERROR(EIO);
        return sz.QuadPart;
    }
    DWORD mode = FILE_BEGIN;
    switch (whence & ~AVSEEK_FORCE) {
        case SEEK_SET: mode = FILE_BEGIN;   break;
        case SEEK_CUR: mode = FILE_CURRENT; break;
        case SEEK_END: mode = FILE_END;     break;
        default: return AVERROR(EINVAL);
    }
    LARGE_INTEGER off;    off.QuadPart    = offset;
    LARGE_INTEGER newPos; newPos.QuadPart = 0;
    if (!::SetFilePointerEx(h, off, &newPos, mode))
        return AVERROR(EIO);
    return newPos.QuadPart;
#else
    FILE* fp = static_cast<FILE*>(opaque);
    if (whence == AVSEEK_SIZE) {
        long cur = std::ftell(fp);
        if (std::fseek(fp, 0, SEEK_END) != 0) return AVERROR(errno);
        long sz = std::ftell(fp);
        std::fseek(fp, cur, SEEK_SET);
        return sz < 0 ? AVERROR(errno) : sz;
    }
    int stdWhence = SEEK_SET;
    switch (whence & ~AVSEEK_FORCE) {
        case SEEK_SET: stdWhence = SEEK_SET; break;
        case SEEK_CUR: stdWhence = SEEK_CUR; break;
        case SEEK_END: stdWhence = SEEK_END; break;
        default: return AVERROR(EINVAL);
    }
    if (std::fseek(fp, static_cast<long>(offset), stdWhence) != 0)
        return AVERROR(errno);
    return std::ftell(fp);
#endif
}

#endif // ROUNDTABLE_HAS_FFMPEG

} // namespace rt
