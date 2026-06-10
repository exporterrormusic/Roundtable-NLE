# check_path_string.ps1 - guard against std::filesystem::path::string() in src/.
#
# path::string() converts via the process ANSI code page: it THROWS
# std::system_error on un-encodable characters and silently mojibakes UTF-8
# byte strings.  The codebase was fully swept to rt::pathToUtf8/utf8ToPath
# (src/core/PathUtils.h) on 2026-06-09; this check keeps new call sites out.
# roundtable.manifest forces ACP=UTF-8 in the SHIPPED app, but the test
# executables don't carry the manifest and pre-1903 Windows ignores it -
# so the helpers remain the only safe spelling.
#
# The pattern is intentionally simple (literal ".string()"): every
# string()-named method on a path-like object is wrong here, and non-path
# .string() methods don't exist in this codebase.  Mention it in comments as
# path::string() to stay checker-safe.
#
# NOTE: keep this file pure ASCII - PowerShell 5.1 reads BOM-less files as
# ANSI, and UTF-8 punctuation bytes parse as stray smart-quotes.

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot

$hits = Get-ChildItem -Path (Join-Path $root "src") -Recurse -Include *.cpp, *.h |
    Select-String -Pattern '\.string\(\)'

if ($hits) {
    Write-Host "ERROR: .string() on std::filesystem::path is banned in src/ -" -ForegroundColor Red
    Write-Host "use rt::pathToUtf8 / rt::utf8ToPath from core/PathUtils.h instead:" -ForegroundColor Red
    foreach ($h in $hits) {
        Write-Host ("  {0}:{1}: {2}" -f $h.Path, $h.LineNumber, $h.Line.Trim())
    }
    exit 1
}

Write-Host "check_path_string OK - no path::string() call sites in src/."
exit 0
