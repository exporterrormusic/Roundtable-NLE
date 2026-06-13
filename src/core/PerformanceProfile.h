/*
 * PerformanceProfile — single owner of every runtime performance knob.
 *
 * Historically these values lived as scattered `static constexpr` literals
 * across MediaPool, CachePolicy, DiskFrameCache, ThumbnailGenerator and
 * the audio waveform path — each tuned independently and documented with its
 * own multi-paragraph rationale.  This struct consolidates them so the whole
 * machine-adaptive policy (and the future "Boost" toggle) is one object that
 * can be derived from detected hardware and applied in one place.
 *
 * See docs/BOOST_MODE_PLAN.md for the full design.
 *
 * Phase 1 (this revision): the struct's defaults reproduce the previously
 * hardcoded values EXACTLY, so wiring consumers to read from perfProfile()
 * is behaviour-neutral.  The factory and tier classifier (Phase 2) and the
 * Boost multiplier (Phase 3) build on top without touching the consumers.
 *
 * Threading note: the active profile is set once at startup, before the
 * prefetch/worker threads spawn, and read-only thereafter.  A future hot
 * "Boost" toggle (Phase 3) must publish a new profile via an atomic snapshot
 * rather than mutating the live one in place — see setPerfProfile().
 */

#pragma once

#include <cstddef>

namespace rt {

struct PerformanceProfile
{
    // ── Decode / prefetch throughput ──────────────────────────────────────
    // Source of truth was MediaPool.h:445-447.  nvdecWorkers is ALWAYS
    // clamped by the GPU's real NVENC/NVDEC concurrent-session cap
    // (HardwareDiagnostics::hasStrictNvencSessionCap) — the profile never
    // raises it past what the silicon allows.
    int prefetchAheadFrames  = 12;   ///< lookahead window (was PREFETCH_AHEAD_COUNT)
    int prefetchThreadCount  = 10;   ///< total prefetch workers (was PREFETCH_THREAD_COUNT)
    int nvdecWorkers         = 2;    ///< workers [0..n) eligible for NVDEC (was PREFETCH_NVDEC_WORKERS)

    // ── Cache working sets ────────────────────────────────────────────────
    // 0 == "let CachePolicy compute the adaptive default" (its existing
    // recommended*() methods stay the policy).  Phase 3 fills these in to
    // override on capable machines.
    size_t frameCacheBudgetBytes  = 0;
    size_t gpuTexCacheBudgetBytes = 0;
    size_t gpuTexMaxEntries       = 0;
    size_t frameCacheMaxEntries   = 0;
    size_t diskCacheBudgetBytes   = 0;

    // ── Background work ───────────────────────────────────────────────────
    size_t thumbnailThreads    = 2;   ///< ThumbnailGenerator default thread count
    size_t diskWriteQueueDepth = 30;  ///< DiskFrameCache kMaxWriteQueue

    // ── Quality / editing ─────────────────────────────────────────────────
    float editProxyScale = 0.5f;      ///< full-res ProRes forced to 1/2 today

    /// Build a profile adapted to the given machine.  Scales the cache
    /// working set modestly by detected tier (capable machines keep more
    /// recent frames resident); throughput knobs stay at the conservative
    /// baseline.  There is no opt-in mode — this single adaptive default
    /// replaces the former locked-low constants.
    ///
    /// Primitive parameters (not a GpuClassification) keep this header free of
    /// Vulkan/HardwareDiagnostics coupling and make the policy unit-testable.
    static PerformanceProfile forMachine(size_t deviceVramBytes,
                                         size_t totalRamBytes,
                                         unsigned logicalCores,
                                         bool hasStrictNvencCap);
};

/// Process-global active profile.  Defaults to the historically-tuned values
/// so reads before setPerfProfile() are safe and behaviour-neutral.
[[nodiscard]] const PerformanceProfile& perfProfile();

/// Install the active profile.  Call once at startup after hardware is known.
/// (Phase 3 hot-toggle will replace this with an atomic-snapshot publish.)
void setPerfProfile(const PerformanceProfile& profile);

} // namespace rt
