#include "AppPaths.h"

#include <cstdlib>
#include <system_error>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace rt::AppPaths {

namespace fs = std::filesystem;

namespace {

fs::path executableDirectory()
{
#ifdef _WIN32
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
        const DWORD length = GetModuleFileNameW(
            nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) return {};
        if (length < buffer.size()) {
            buffer.resize(length);
            return fs::path(buffer).parent_path();
        }
        buffer.resize(buffer.size() * 2);  // path longer than the buffer
    }
#else
    std::error_code error;
    const auto self = fs::read_symlink("/proc/self/exe", error);
    return error ? fs::path{} : self.parent_path();
#endif
}

bool isDirectory(const fs::path& path)
{
    std::error_code error;
    return fs::is_directory(path, error);
}

bool isFile(const fs::path& path)
{
    std::error_code error;
    return fs::is_regular_file(path, error);
}

fs::path environmentRoot()
{
#ifdef _WIN32
    const wchar_t* value = _wgetenv(L"ROUNDTABLE_ROOT");
#else
    const char* value = std::getenv("ROUNDTABLE_ROOT");
#endif
    if (!value || !*value) return {};
    const fs::path root(value);
    return isDirectory(root / "assets") ? root : fs::path{};
}

fs::path resolveRoot()
{
    if (auto root = environmentRoot(); !root.empty()) return root;

    const fs::path exeDir = executableDirectory();
    if (!exeDir.empty()) {
        // Dev build: stray assets/ folders can exist under build/ (created
        // by earlier runs with the wrong working directory), so only the
        // source checkout — marked by CMakeLists.txt — counts here.
        fs::path probe = exeDir;
        for (int depth = 0; depth < 5; ++depth) {
            if (isFile(probe / "CMakeLists.txt") && isDirectory(probe / "assets"))
                return probe;
            const fs::path parent = probe.parent_path();
            if (parent == probe) break;
            probe = parent;
        }
        // Installed build: assets/ ships beside the exe.
        if (isDirectory(exeDir / "assets")) return exeDir;
    }

    std::error_code error;
    const fs::path current = fs::current_path(error);
    return error ? fs::path(".") : current;
}

} // namespace

const fs::path& root()
{
    static const fs::path resolved = resolveRoot();
    return resolved;
}

fs::path assets()
{
    return root() / "assets";
}

bool makeRootCurrentDirectory()
{
    std::error_code error;
    fs::current_path(root(), error);
    return !error;
}

} // namespace rt::AppPaths
