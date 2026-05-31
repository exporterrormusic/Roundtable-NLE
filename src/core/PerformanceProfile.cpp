/*
 * PerformanceProfile.cpp — active-profile storage + machine factory.
 *
 * Phase 1: forMachine() returns the behaviour-neutral defaults.  The
 * MachineTier classifier (Phase 2) and Boost multiplier (Phase 3) slot in
 * here without changing any consumer.  See docs/BOOST_MODE_PLAN.md.
 */

#include "PerformanceProfile.h"
#include "HardwareDiagnostics.h"

#include <algorithm>

namespace rt {

namespace {
// Set once at startup before worker threads spawn; read-only thereafter.
PerformanceProfile g_active{};

constexpr size_t kMiB = 1024ull * 1024ull;
constexpr size_t kGiB = 1024ull * kMiB;
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
                                                  bool hasStrictNvencCap,
                                                  bool boost)
{
    using HardwareDiagnostics::MachineTier;

    PerformanceProfile p;
    p.boostEnabled = boost;

    // Hardware gate (always honoured): machines with a strict NVENC/NVDEC
    // session cap (Pascal consumer SKUs, pre-driver-550) must never run more
    // than 2 NVDEC workers, Boost or not.
    if (hasStrictNvencCap)
        p.nvdecWorkers = 2;

    // ── DEFAULT (adaptive) path ─────────────────────────────────────────
    // When Boost is OFF the cache override fields stay 0, which means
    // "let CacheCoordinator compute its existing VRAM/RAM-proportional
    // defaults".  Those formulas are already adaptive, so the default is
    // adaptive-but-conservative with zero behaviour change from before the
    // profile existed.  The Path-C-gated throughput knobs (prefetchAhead*,
    // nvdecWorkers, prefetchThreadCount) also stay at their conservative
    // baselines here — they cannot be raised safely until Path C decouples
    // the prefetch convert/upload work from the compositor's queue.
    if (!boost)
        return p;

    // ── BOOST (opt-in aggressive) path ──────────────────────────────────
    // Raise the cache working-set CEILINGS by tier.  These are the
    // "safe today" knobs: GpuTextureCache eviction is fence-gated (pin +
    // triple-buffered ring) so a larger working set cannot destroy an
    // in-flight texture, and CacheCoordinator's live VMA/RAM pressure
    // relief stays armed to back off if a target proves too high on a
    // contended machine.  See docs/BOOST_MODE_PLAN.md §4, §6.
    //
    // NOTE: Boost does NOT yet raise prefetchAheadFrames / nvdecWorkers /
    // prefetchThreadCount — those remain Path-C-gated (Phase 5/6).
    const auto tier = HardwareDiagnostics::classifyMachine(
        [&] { HardwareDiagnostics::GpuClassification g; g.vramBytes = deviceVramBytes; return g; }(),
        totalRamBytes, logicalCores);

    // Per-tier aggressive cache working set.  gpuTexMaxEntries is the real
    // ceiling; gpuTexCacheBudgetBytes is sized to accommodate it (~9 MB/
    // entry of 1080p BGRA + slack) so byte-pressure doesn't evict before
    // the entry cap does, then clamped to a sane fraction of VRAM.
    size_t gpuEntries = 0;
    size_t frameBudget = 0;
    switch (tier) {
    case MachineTier::Entry:       gpuEntries = 60;  frameBudget = 384 * kMiB; break;
    case MachineTier::Standard:    gpuEntries = 150; frameBudget = 1   * kGiB; break;
    case MachineTier::Performance: gpuEntries = 300; frameBudget = 2   * kGiB; break;
    case MachineTier::Workstation: gpuEntries = 450; frameBudget = 4   * kGiB; break;
    }

    p.gpuTexMaxEntries = gpuEntries;
    p.frameCacheMaxEntries = gpuEntries * 2;   // orphan-texture headroom (matches default shape)

    // Byte budget that lets the entry cap be the binding ceiling, clamped
    // so we never claim more than ~45% of VRAM for the texture cache.
    const size_t wantBudget = gpuEntries * (9 * kMiB);
    const size_t vramCeil = deviceVramBytes ? (deviceVramBytes * 45 / 100) : wantBudget;
    p.gpuTexCacheBudgetBytes = std::min(wantBudget, vramCeil);

    // CPU frame-cache budget, clamped so it never exceeds 25% of RAM.
    const size_t ramCeil = totalRamBytes ? (totalRamBytes / 4) : frameBudget;
    p.frameCacheBudgetBytes = std::min(frameBudget, ramCeil);

    // Background work scales with cores under Boost (helps thumbnail/proxy
    // generation keep up without starving decode).  Wired into consumers
    // incrementally — see BOOST_MODE_PLAN.md §4.
    if (logicalCores >= 16)      p.thumbnailThreads = 8;
    else if (logicalCores >= 8)  p.thumbnailThreads = 6;
    else if (logicalCores >= 6)  p.thumbnailThreads = 4;

    return p;
}

} // namespace rt
