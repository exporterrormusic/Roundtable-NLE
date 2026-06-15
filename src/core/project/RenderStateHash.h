/*
 * RenderStateHash — hash of the compositing CONFIGURATION at a timeline tick.
 *
 * Foundation for the §4.6 slice-2 segment-render cache: the cache key for a
 * composited frame is (tick, tier, configHash).  The tick distinguishes
 * frames; the configHash makes a cached frame INVALIDATE the moment any edit
 * changes the state that produced it.  This file is that configHash.
 *
 * "Configuration" = everything that affects the composited pixels at a tick
 * EXCEPT the tick value itself, so identical configs across a clip's whole
 * span hash equal (the cache adds the tick separately).
 *
 * COMPLETENESS is the whole point: a missed field => a stale frame served
 * from cache after an edit.  We therefore hash the FULL serialized form of
 * every active clip (writeClip — the same routine that persists the project),
 * so no persisted field can be missed.  The cost is mild OVER-invalidation:
 * cosmetic fields (label, clip colour) also fold into the hash, so editing
 * them needlessly re-renders.  Over-invalidation is the SAFE direction
 * (wastes a re-render); under-invalidation (stale pixels) is the bug we must
 * never have.  For the same reason cross-track state (every track's mute/solo,
 * since soloing one track hides the others) and the sequence's pixel-affecting
 * Settings (resolution / fps / colour space) fold in globally.
 *
 * KNOWN COVERAGE GAPS — must be closed before the cache (slice 2b/2c) trusts
 * this hash for invalidation:
 *   - Nested SequenceClip inner-timeline edits: writeClip stores only the
 *     sequence reference, not its inner content.  Recurse into the referenced
 *     sequence (needs the Project) before relying on this for nested comps.
 *   - Global/app-level render prefs that bypass per-sequence Settings.
 */

#pragma once

#include <cstdint>

namespace rt {

class Timeline;

/// 64-bit hash of the compositing configuration governing `tick`.  Pure
/// (depends only on the timeline state), so it is deterministic and unit-
/// tested.  See the file header for what is / isn't covered.
[[nodiscard]] uint64_t hashCompositeConfigAt(const Timeline& timeline, int64_t tick);

} // namespace rt
