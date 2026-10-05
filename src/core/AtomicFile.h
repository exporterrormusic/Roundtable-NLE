/*
 * AtomicFile.h — Replace a file's contents all-or-nothing.
 *
 * Settings and preset files used to be rewritten in place (truncate, then
 * write): a crash, full disk or kill mid-write left them empty or cut off.
 * Project files have their own staged writer with backups
 * (ProjectSerializerIO.cpp); this is the lean version for everything else.
 */

#pragma once

#include <filesystem>
#include <string_view>

namespace rt {

/// Write `data` to `path` so readers (and a crash) only ever see the old
/// or the new complete file: staged beside it, flushed, then swapped in
/// with one atomic rename. On failure logs, leaves the old file untouched
/// and returns false.
bool writeFileAtomically(const std::filesystem::path& path, std::string_view data);

} // namespace rt
