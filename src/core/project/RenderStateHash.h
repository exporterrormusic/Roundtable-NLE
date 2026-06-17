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
 * NESTED SEQUENCES: writeClip stores only the SequenceClip's reference, not the
 * referenced sequence's inner content — so an edit INSIDE a nested sequence
 * would not, on its own, change the parent's hash (→ stale cached frame).  When
 * a Project is supplied, hashCompositeConfigAt recurses into each active nested
 * sequence (mapping the outer tick through the clip's timelineIn + sourceIn,
 * exactly as the compositor does) and folds its inner config in.  Cycles and
 * runaway depth are guarded.  Pass the Project for correct nested invalidation;
 * omit it only for flat single-sequence timelines (e.g. unit tests).
 *
 * KNOWN COVERAGE GAPS — must be closed before the cache trusts this hash:
 *   - Global/app-level render prefs that bypass per-sequence Settings.
 */

#pragma once

#include <cstdint>

namespace rt {

class Timeline;
class Project;

/// 64-bit hash of the compositing configuration governing `tick`.  Pure
/// (depends only on timeline/project state), so it is deterministic and unit-
/// tested.  See the file header for what is / isn't covered.  Supply `project`
/// so edits inside active NESTED sequences invalidate the parent's hash; pass
/// nullptr (the default) for a flat timeline with no nested sequences.
[[nodiscard]] uint64_t hashCompositeConfigAt(const Timeline& timeline, int64_t tick,
                                             const Project* project = nullptr);

} // namespace rt
