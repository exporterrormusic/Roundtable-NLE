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
                                                  bool hasStrictNvencCap)
{
    using HardwareDiagnostics::MachineTier;

    PerformanceProfile p;

    // Hardware gate (always honoured): machines with a strict NVENC/NVDEC
    // session cap (Pascal consumer SKUs, pre-driver-550) must never run more
    // than 2 NVDEC workers.
    if (hasStrictNvencCap)
        p.nvdecWorkers = 2;

    // ── Modest, default machine adaptation ──────────────────────────────
    // There is no opt-in "Boost" mode: the default simply scales the cache
    // WORKING SET to the machine tier so a capable GPU keeps more recent
    // frames resident (smoother scrub-back over already-seen footage) than
    // the one-size-fits-the-weakest-machine floor — WITHOUT the aggressive
    // sizing that thrashed VRAM on a busy timeline.  Cache fields left at 0
    // fall through to CachePolicy's existing conservative formula.
    //
    // Deliberately conservative:
    //   • Only the cache working set scales — the throughput knobs
    //     (prefetchAheadFrames, nvdecWorkers, prefetchThreadCount) stay at
    //     the baseline.  Raising those safely needs the prefetch/compositor
    //     queue-decoupling work (parked in git stash; see
    //     the retired BOOST_MODE_PLAN doc in git history), which is not landed.
    //   • Entry/Standard tiers stay on the floor (little headroom to spare).
    //   • The byte budget is sized with generous slack so the ENTRY-COUNT
    //     cap is the binding limit, never byte-pressure — that avoids the
    //     near-budget eviction thrash that an over-tight budget caused.
    const auto tier = HardwareDiagnostics::classifyMachine(
        [&] { HardwareDiagnostics::GpuClassification g; g.vramBytes = deviceVramBytes; return g; }(),
        totalRamBytes, logicalCores);

    // Size the GPU cache by a BYTE budget directly (not entry count) — frame
    // textures vary wildly (1080p ≈ 8 MB, a tall packed-alpha clip ≈ 16 MB),
    // so an entry count can't predict VRAM use.  The entry cap is set high
    // enough that the byte budget is the binding limit.  Values are a modest
    // step above CachePolicy's ~2 GB / ~1 GB floor, with lots of VRAM/RAM
    // headroom left.
    size_t gpuBudget    = 0;   // 0 ⇒ CachePolicy's conservative default
    size_t gpuEntryCap  = 0;
    size_t frameBudget  = 0;
    size_t frameEntries = 0;
    switch (tier) {
    case MachineTier::Entry:       break;   // floor (little headroom to spare)
    case MachineTier::Standard:    break;   // ≈ floor
    case MachineTier::Performance: gpuBudget = 3 * kGiB; gpuEntryCap = 400; frameBudget = 2 * kGiB; frameEntries = 600; break;
    case MachineTier::Workstation: gpuBudget = 5 * kGiB; gpuEntryCap = 700; frameBudget = 4 * kGiB; frameEntries = 900; break;
    }

    if (gpuBudget != 0) {
        // Hard safety ceiling: keep the texture cache well below the VRAM
        // paging danger zone.  ~25% of VRAM (≈6 GB on a 24 GB card) leaves
        // ample room for decoder surfaces + the swapchain + live frames +
        // driver overhead, so total VMA-tracked VRAM stays clear of the OS
        // budget that previously triggered driver paging + stutter.
        const size_t vramCeil = deviceVramBytes ? (deviceVramBytes * 25 / 100)
                                                : gpuBudget;
        p.gpuTexCacheBudgetBytes = std::min(gpuBudget, vramCeil);
        p.gpuTexMaxEntries       = gpuEntryCap;     // ceiling; byte budget binds first
        p.frameCacheMaxEntries   = frameEntries;    // bounds orphan-texture VRAM
    }
    if (frameBudget != 0) {
        // RAM is plentiful, but the FrameCache also pins GPU-co-owned VkImages
        // until the compositor consumes them, so it is bounded too (≤~12% RAM).
        const size_t ramCeil = totalRamBytes ? (totalRamBytes / 8) : frameBudget;
        p.frameCacheBudgetBytes = std::min(frameBudget, ramCeil);
    }

    // Background work scales modestly with cores (thumbnail/waveform gen).
    if (logicalCores >= 16)      p.thumbnailThreads = 4;
    else if (logicalCores >= 8)  p.thumbnailThreads = 3;

    return p;
}

} // namespace rt
