/*
 * HardwareDiagnostics — implementation.
 *
 * The hook table is a list of DLL basenames known to inject themselves
 * into Vulkan processes for capture / overlay purposes.  Each entry
 * names the source application and gives instructions for excluding
 * ROUNDTABLE.  When NVIDIA / OBS / Overwolf ship a new build that
 * renames or relocates these DLLs the table needs updating — it is
 * deliberately data-driven for that reason.
 *
 * The GPU classification table is keyed on NVIDIA's published deviceID
 * ranges (see https://pci-ids.ucw.cz/read/PC/10de).  Ranges, not exact
 * matches: some product lines (e.g. mobile Pascal) reuse the same
 * deviceID space.  AMD / Intel get a coarser classification because the
 * NVENC-session-cap concern doesn't apply to them.
 */

#include "HardwareDiagnostics.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstring>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <Psapi.h>
#pragma comment(lib, "psapi.lib")
#endif

namespace rt::HardwareDiagnostics {

namespace {

struct KnownHook
{
    const char* dllBaseLower;     ///< Lower-case basename match
    const char* sourceApp;        ///< Human-readable app name
    const char* exclusionHint;    ///< User-actionable instruction
};

// Order matters only for first-match logging.  Match is case-insensitive.
// dllBaseLower is compared as a substring of the lower-cased basename so
// versioned variants (discord_overlay_1.0.dll) still match.
constexpr KnownHook kKnownHooks[] = {
    { "nvspcap",
      "NVIDIA GeForce Experience / ShadowPlay overlay",
      "Open NVIDIA App → Settings → In-Game Overlay → toggle OFF, or add "
      "'roundtable.exe' to the per-app exclusions list. Also fixes Discord's "
      "GeForce overlay sharing this hook." },
    { "nvcamera",
      "NVIDIA Broadcast / RTX Voice capture",
      "Open NVIDIA Broadcast → Settings and exclude 'roundtable.exe' from "
      "the audio/video effect pipeline." },
    { "ow-graphics-hook",
      "Overwolf / OBS game capture hook",
      "In OBS: remove the 'Game Capture' source pointing at roundtable.exe, "
      "or switch it to 'Window Capture'. In Overwolf: disable the in-game "
      "overlay for the affected app." },
    { "obs-vulkan",
      "OBS Vulkan capture layer",
      "Disable the OBS Vulkan capture implicit layer or uninstall OBS's "
      "Game Capture component." },
    { "discord_overlay",
      "Discord overlay",
      "In Discord: User Settings → Game Overlay → toggle OFF, or remove "
      "'roundtable.exe' from the registered games list." },
    { "discord_hook",
      "Discord overlay (hook variant)",
      "In Discord: User Settings → Game Overlay → toggle OFF." },
    { "rtsshooks",
      "RivaTuner Statistics Server / MSI Afterburner overlay",
      "In RTSS: add 'roundtable.exe' to the application list and set "
      "'Application detection level' to None for that entry." },
    { "gameoverlayrenderer",
      "Steam in-game overlay",
      "In Steam: right-click ROUNDTABLE in your library (if added) → "
      "Properties → uncheck 'Enable the Steam Overlay'. Steam overlay rarely "
      "causes Vulkan crashes but is included for completeness." },
};

#ifdef _WIN32
std::string lowercase(const std::string& s)
{
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(::tolower(c)); });
    return out;
}

std::string basenameOf(const std::string& path)
{
    auto pos = path.find_last_of("\\/");
    return (pos == std::string::npos) ? path : path.substr(pos + 1);
}
#endif

} // namespace

std::vector<InjectedHook> scanLoadedHooks()
{
    std::vector<InjectedHook> found;

#ifdef _WIN32
    // EnumProcessModules can return ERROR_PARTIAL_COPY transiently while
    // modules are loading — accept whatever it returned and continue.
    HMODULE modules[1024];
    DWORD   bytesNeeded = 0;
    if (!EnumProcessModules(GetCurrentProcess(), modules, sizeof(modules),
                            &bytesNeeded)) {
        spdlog::debug("HardwareDiagnostics: EnumProcessModules failed (err {})",
                      GetLastError());
        return found;
    }

    const DWORD moduleCount = bytesNeeded / sizeof(HMODULE);
    for (DWORD i = 0; i < moduleCount && i < 1024; ++i) {
        char pathBuf[MAX_PATH] = {};
        if (!GetModuleFileNameExA(GetCurrentProcess(), modules[i],
                                  pathBuf, MAX_PATH)) {
            continue;
        }

        const std::string fullPath = pathBuf;
        const std::string baseLow  = lowercase(basenameOf(fullPath));

        for (const auto& known : kKnownHooks) {
            if (baseLow.find(known.dllBaseLower) != std::string::npos) {
                InjectedHook h;
                h.moduleBaseName = basenameOf(fullPath);
                h.fullPath       = fullPath;
                h.sourceApp      = known.sourceApp;
                h.exclusionHint  = known.exclusionHint;
                found.push_back(std::move(h));
                break;  // one classification per loaded DLL
            }
        }
    }
#endif

    return found;
}

GpuClassification classifyGpu(uint32_t vendorId,
                              uint32_t deviceId,
                              const std::string& deviceName,
                              uint64_t vramBytes)
{
    GpuClassification cls;
    cls.deviceName = deviceName;
    cls.vendorId   = vendorId;
    cls.deviceId   = deviceId;
    cls.vramBytes  = vramBytes;

    // ── NVIDIA — classify by deviceID ranges ────────────────────────────
    // Source: NVIDIA Vulkan deviceID is the lower 16 bits of PCI device
    // ID.  Ranges below are the documented Pascal / Turing / Ampere etc.
    // bands.  Pascal range covers GP100/GP102/GP104/GP106/GP107/GP108.
    if (vendorId == 0x10DE) {
        // 16-bit device IDs.  Ranges are not strictly contiguous but
        // these bounds cover the consumer SKUs that the support
        // population is likely on.
        if (deviceId >= 0x1380 && deviceId <= 0x13FF) {
            cls.arch = GpuArchitecture::NvidiaMaxwell;  // GTX 9xx
        } else if (
                   // GP102 (GTX 1080 Ti / Titan Xp) 1B00-1B7F
                   (deviceId >= 0x1B00 && deviceId <= 0x1B7F) ||
                   // GP104 (GTX 1080 / 1070 / 1070 Ti)   1B80-1BFF
                   (deviceId >= 0x1B80 && deviceId <= 0x1BFF) ||
                   // GP106 (GTX 1060)                    1C00-1C7F
                   (deviceId >= 0x1C00 && deviceId <= 0x1C7F) ||
                   // GP107 (GTX 1050 / 1050 Ti)          1C80-1CFF
                   (deviceId >= 0x1C80 && deviceId <= 0x1CFF) ||
                   // GP108 (GT 1030)                     1D00-1D7F
                   (deviceId >= 0x1D00 && deviceId <= 0x1D7F) ||
                   // GP100 (Tesla / Quadro GP100)        15F0-15FF
                   (deviceId >= 0x15F0 && deviceId <= 0x15FF)) {
            cls.arch = GpuArchitecture::NvidiaPascal;
            cls.hasStrictNvencSessionCap = true;
        } else if (deviceId == 0x1D81 || deviceId == 0x1DB1 ||
                   (deviceId >= 0x1DB0 && deviceId <= 0x1DBF)) {
            cls.arch = GpuArchitecture::NvidiaVolta;  // Titan V / GV100
        } else if (
                   // TU102 (RTX 2080 Ti / Titan RTX)     1E00-1E7F
                   (deviceId >= 0x1E00 && deviceId <= 0x1E7F) ||
                   // TU104 (RTX 2080 / 2070 Super)       1E80-1EFF
                   (deviceId >= 0x1E80 && deviceId <= 0x1EFF) ||
                   // TU106 (RTX 2070 / 2060 Super)       1F00-1F7F
                   (deviceId >= 0x1F00 && deviceId <= 0x1F7F) ||
                   // TU116 (GTX 1660 Ti / 1660 Super)    2180-21FF
                   (deviceId >= 0x2180 && deviceId <= 0x21FF) ||
                   // TU117 (GTX 1650 / 1650 Super)       1F80-1FFF
                   (deviceId >= 0x1F80 && deviceId <= 0x1FFF)) {
            cls.arch = GpuArchitecture::NvidiaTuring;
        } else if (
                   // GA102 (RTX 3090 / 3080)             2200-227F
                   (deviceId >= 0x2200 && deviceId <= 0x227F) ||
                   // GA104 (RTX 3070 / 3060 Ti)          2480-24FF
                   (deviceId >= 0x2480 && deviceId <= 0x24FF) ||
                   // GA106 (RTX 3060)                    2500-257F
                   (deviceId >= 0x2500 && deviceId <= 0x257F) ||
                   // GA107 (RTX 3050 / mobile 30 series) 2580-25FF
                   (deviceId >= 0x2580 && deviceId <= 0x25FF)) {
            cls.arch = GpuArchitecture::NvidiaAmpere;
        } else if (
                   // AD102 (RTX 4090)                    2680-26FF
                   (deviceId >= 0x2680 && deviceId <= 0x26FF) ||
                   // AD103 (RTX 4080)                    2700-277F
                   (deviceId >= 0x2700 && deviceId <= 0x277F) ||
                   // AD104 (RTX 4070 / Ti)               2780-27FF
                   (deviceId >= 0x2780 && deviceId <= 0x27FF) ||
                   // AD106 (RTX 4060 Ti)                 2800-287F
                   (deviceId >= 0x2800 && deviceId <= 0x287F) ||
                   // AD107 (RTX 4060)                    2880-28FF
                   (deviceId >= 0x2880 && deviceId <= 0x28FF)) {
            cls.arch = GpuArchitecture::NvidiaAda;
        } else if (deviceId >= 0x2B00 && deviceId <= 0x2BFF) {
            cls.arch = GpuArchitecture::NvidiaBlackwell;  // RTX 50xx
        } else {
            // Unknown NVIDIA device — best-effort fall-through.  Don't
            // light up the Pascal warning for unrecognised IDs.
            cls.arch = GpuArchitecture::Other;
        }
    } else if (vendorId == 0x1002 || vendorId == 0x1022) {
        cls.arch = GpuArchitecture::AmdRadeon;
    } else if (vendorId == 0x8086) {
        // Intel — distinguish Arc (discrete) from integrated by device-id
        // band.  Arc discrete is in the 0x56xx range.
        if (deviceId >= 0x5690 && deviceId <= 0x56FF) {
            cls.arch = GpuArchitecture::IntelArc;
        } else {
            cls.arch = GpuArchitecture::IntelIntegrated;
        }
    } else {
        cls.arch = GpuArchitecture::Other;
    }

    return cls;
}

MachineTier classifyMachine(const GpuClassification& gpu,
                            uint64_t totalRamBytes,
                            unsigned logicalCores)
{
    constexpr uint64_t kGiB = 1024ull * 1024ull * 1024ull;
    const uint64_t vram = gpu.vramBytes;
    const uint64_t ram  = totalRamBytes;
    const unsigned cores = logicalCores;

    // ── Base tier from VRAM (the binding resource) ──────────────────────
    MachineTier tier;
    if      (vram >= 16 * kGiB) tier = MachineTier::Workstation;
    else if (vram >=  8 * kGiB) tier = MachineTier::Performance;
    else if (vram >=  4 * kGiB) tier = MachineTier::Standard;
    else                        tier = MachineTier::Entry;

    // ── RAM / core gates can only pull the tier DOWN ────────────────────
    // A 16 GB-VRAM card paired with 8 GB RAM is not a Workstation; the
    // caches it would enable would thrash system memory.  cores==0 means
    // "unknown" — don't penalise for it.
    auto demoteTo = [&](MachineTier ceiling) {
        if (static_cast<int>(tier) > static_cast<int>(ceiling))
            tier = ceiling;
    };
    if (ram < 12 * kGiB || (cores != 0 && cores <= 4))
        demoteTo(MachineTier::Entry);
    else if (ram < 24 * kGiB || (cores != 0 && cores < 8))
        demoteTo(MachineTier::Standard);
    else if (ram < 40 * kGiB || (cores != 0 && cores < 12))
        demoteTo(MachineTier::Performance);

    return tier;
}

const char* machineTierName(MachineTier tier)
{
    switch (tier) {
    case MachineTier::Entry:       return "Entry";
    case MachineTier::Standard:    return "Standard";
    case MachineTier::Performance: return "Performance";
    case MachineTier::Workstation: return "Workstation";
    }
    return "Unknown";
}

void logAtStartup(const GpuClassification& gpu,
                  const std::vector<InjectedHook>& hooks)
{
    const char* archStr = "Unknown";
    switch (gpu.arch) {
    case GpuArchitecture::NvidiaMaxwell:    archStr = "NVIDIA Maxwell";    break;
    case GpuArchitecture::NvidiaPascal:     archStr = "NVIDIA Pascal";     break;
    case GpuArchitecture::NvidiaVolta:      archStr = "NVIDIA Volta";      break;
    case GpuArchitecture::NvidiaTuring:     archStr = "NVIDIA Turing";     break;
    case GpuArchitecture::NvidiaAmpere:     archStr = "NVIDIA Ampere";     break;
    case GpuArchitecture::NvidiaAda:        archStr = "NVIDIA Ada";        break;
    case GpuArchitecture::NvidiaBlackwell:  archStr = "NVIDIA Blackwell";  break;
    case GpuArchitecture::AmdRadeon:        archStr = "AMD Radeon";        break;
    case GpuArchitecture::IntelArc:         archStr = "Intel Arc";         break;
    case GpuArchitecture::IntelIntegrated:  archStr = "Intel iGPU";        break;
    case GpuArchitecture::Other:            archStr = "Other";             break;
    case GpuArchitecture::Unknown:          archStr = "Unknown";           break;
    }

    spdlog::warn("[HW-DIAG] GPU: {} (arch={}, vendor=0x{:04X}, device=0x{:04X}, vram={} MB)",
                 gpu.deviceName, archStr, gpu.vendorId, gpu.deviceId,
                 gpu.vramBytes / (1024ull * 1024ull));

    if (gpu.hasStrictNvencSessionCap) {
        spdlog::warn("[HW-DIAG] This GPU has a 2-concurrent-NVENC-session limit on "
                     "consumer SKUs (pre-driver-550). If Discord, OBS, or another "
                     "encoder app is running, HW export will fall back to CPU.");
    }

    if (hooks.empty()) {
        spdlog::warn("[HW-DIAG] No known overlay/capture hooks detected in this process.");
    } else {
        spdlog::warn("[HW-DIAG] {} injected overlay hook(s) detected:", hooks.size());
        for (const auto& h : hooks) {
            spdlog::warn("[HW-DIAG]   - {} ({})", h.moduleBaseName, h.sourceApp);
        }
    }
}

} // namespace rt::HardwareDiagnostics
