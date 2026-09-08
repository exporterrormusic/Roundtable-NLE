/*
 * PerformanceProfile.cpp — active-profile storage + machine factory.
 *
 * forMachine() classifies the machine into a tier and scales the cache
 * working set modestly to fit it (one adaptive default — no opt-in mode).
 * Throughput knobs stay at the conservative baseline.
 */

#include "PerformanceProfile.h"
#include "HardwareDiagnostics.h"

#include <algorithm>

namespace rt {

namespace {
// Set once at startup before worker threads spawn; read-only thereafter.
PerformanceProfile g_active{};

} // namespace

const PerformanceProfile& perfProfile()
{
    return g_active;
}

void setPerfProfile(const PerformanceProfile& profile)
{
    g_active = profile;
}

PerformanceProfile PerformanceProfile::forMachine(size_t deviceVramBytes,
                                                  size_t totalRamBytes,
                                                  unsigned logicalCores,
                                                  bool hasStrictNvencCap)
{
    using HardwareDiagnostics::MachineTier;

    PerformanceProfile p;

    // Hardware gate (always honoured): machines with a strict NVENC/NVDEC
    // session cap (Pascal consumer SKUs, pre-driver-550) must never run more
    // than 2 NVDEC workers.
    if (hasStrictNvencCap)
        p.nvdecWorkers = 2;

    // Machine tier still controls bounded background work and the first-run
    // preview-resolution default. Decoded-frame retention deliberately does
    // not scale upward with installed VRAM.
    const auto tier = HardwareDiagnostics::classifyMachine(
        [&] { HardwareDiagnostics::GpuClassification g; g.vramBytes = deviceVramBytes; return g; }(),
        totalRamBytes, logicalCores);

    // Decoded-frame retention is a bounded editing working set, not a reward
    // for owning a larger GPU. Export creates an isolated compositor while
    // decoder surfaces, effects, Spine atlases, and display resources remain
    // live, so the former multi-gigabyte workstation override multiplied the
    // same cache category and could consume double-digit VRAM. Leave these at
    // zero so CachePolicy supplies its resolution-agnostic bounded defaults.

    // Background work scales modestly with cores (thumbnail/waveform gen).
    if (logicalCores >= 16)      p.thumbnailThreads = 4;
    else if (logicalCores >= 8)  p.thumbnailThreads = 3;

    // Disk write-behind queue: each queued frame pins its source GPU texture
    // (~8 MB) until written, so the cap scales with the same VRAM headroom
    // as the texture-cache budgets above.  Consumed by DiskFrameCache.
    switch (tier) {
    case MachineTier::Entry:
    case MachineTier::Standard:    break;                          // baseline 30
    case MachineTier::Performance: p.diskWriteQueueDepth = 40; break;
    case MachineTier::Workstation: p.diskWriteQueueDepth = 50; break;
    }

    // First-run playback-resolution default (the Program Monitor dropdown
    // persists the user's choice in QSettings thereafter): workstations can
    // afford Full-res preview decode; every other tier keeps the 1/2 default.
    if (tier == MachineTier::Workstation)
        p.editProxyScale = 1.0f;

    return p;
}

} // namespace rt
