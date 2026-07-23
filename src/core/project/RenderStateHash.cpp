/*
 * RenderStateHash.cpp — see RenderStateHash.h for the contract + coverage.
 */

#include "project/RenderStateHash.h"

#include "project/ClipSerialization.h"   // writeClip — full per-clip serialization
#include "project/BinaryIO.h"            // BinaryWriter
#include "project/Settings.h"
#include "project/Project.h"
#include "timeline/Timeline.h"
#include "timeline/Track.h"
#include "timeline/Clip.h"
#include "timeline/SequenceClip.h"
#include "timeline/Transition.h"

#include <vector>

namespace rt {

namespace {

// FNV-1a 64.  Cheap, dependency-free, good enough for a cache/invalidation
// key (not a security hash).
constexpr uint64_t kFnvOffset = 1469598103934665603ull;
constexpr uint64_t kFnvPrime  = 1099511628211ull;

// Bump to force every cached segment to re-render after the hash algorithm
// or its coverage changes (old keys can never collide with new ones).
// v2: nested-sequence inner config now folds in.
constexpr uint32_t kHashVersion = 3;

// Belt-and-braces ceiling on nested-sequence recursion; the explicit cycle
// guard (the ancestor stack) already prevents infinite loops, this just caps
// absurdly deep but acyclic nests.
constexpr int kMaxNestDepth = 16;

[[nodiscard]] uint64_t fnv1a(const uint8_t* data, size_t n)
{
    uint64_t h = kFnvOffset;
    for (size_t i = 0; i < n; ++i) {
        h ^= data[i];
        h *= kFnvPrime;
    }
    return h;
}

// Tracks that contribute pixels to the composite (video, incl. the pinned
// caption track which burns text in; dividers are visual separators only).
[[nodiscard]] bool isCompositingTrack(const Track& tr) noexcept
{
    return tr.type() == TrackType::Video && !tr.isDivider();
}

// Append the full compositing config governing `tick` of `timeline` into `w`.
// Recurses into active nested sequences when `project` is supplied; `ancestors`
// is the stack of timelines already being hashed up the recursion, used to break
// reference cycles (a sequence that nests itself, directly or transitively).
void appendCompositeConfig(BinaryWriter& w, const Timeline& timeline, int64_t tick,
                           const Project* project, int depth,
                           std::vector<const Timeline*>& ancestors)
{
    // ── Sequence settings that affect pixels ────────────────────────────
    const Settings& s = timeline.settings();
    w.writeU32(s.resolution().width);
    w.writeU32(s.resolution().height);
    w.writeF64(s.frameRate());
    w.writeU8(static_cast<uint8_t>(s.colorSpace()));

    // ── Cross-track mute/solo (soloing one track hides the others, so this
    //    affects EVERY tick's composite even for tracks not active here) ──
    for (size_t ti = 0; ti < timeline.trackCount(); ++ti) {
        const Track* tr = timeline.track(ti);
        if (!tr || !isCompositingTrack(*tr)) continue;
        w.writeU8(0x10);
        w.writeU32(static_cast<uint32_t>(ti));
        w.writeU8(tr->isMuted()  ? 1 : 0);
        w.writeU8(tr->isSoloed() ? 1 : 0);
    }

    // ── Active layers + transitions, in compositing (track) order ───────
    for (size_t ti = 0; ti < timeline.trackCount(); ++ti) {
        const Track* tr = timeline.track(ti);
        if (!tr || !isCompositingTrack(*tr)) continue;

        for (size_t ci = 0; ci < tr->clipCount(); ++ci) {
            const Clip* c = tr->clip(ci);
            if (!c) continue;
            // Half-open: a clip ending exactly at `tick` is no longer active.
            if (tick < c->timelineIn() || tick >= c->timelineOut()) continue;
            w.writeU8(0x01);                              // active-clip marker
            w.writeU32(static_cast<uint32_t>(ti));        // its track (layer order)
            writeClip(w, *c);                             // FULL serialized state

            // Nested sequence: writeClip captured only the reference, so fold
            // in the referenced sequence's inner config at the mapped tick —
            // otherwise an edit inside the nested sequence wouldn't invalidate
            // this composite.  Map the outer tick exactly as the compositor:
            // localTick = tick - timelineIn, innerTick = localTick + sourceIn
            // (see CompositeServiceLayerBuildNested::buildSequenceClipFrame).
            if (project && c->clipType() == ClipType::Sequence && depth < kMaxNestDepth) {
                const auto* seqClip = static_cast<const SequenceClip*>(c);
                const Timeline* inner =
                    seqClip->sequenceIndex() < project->sequenceCount()
                        ? project->sequence(seqClip->sequenceIndex())
                        : nullptr;
                // Cycle guard: skip if `inner` is already on the ancestor stack
                // (including the timeline we're currently hashing).
                bool onStack = (inner == &timeline);
                for (const Timeline* a : ancestors)
                    if (a == inner) { onStack = true; break; }
                if (inner && !onStack) {
                    int64_t innerTick = (tick - c->timelineIn()) + c->sourceIn();
                    if (innerTick < 0) innerTick = 0;
                    w.writeU8(0x03);                      // nested-config marker
                    ancestors.push_back(&timeline);
                    appendCompositeConfig(w, *inner, innerTick, project,
                                          depth + 1, ancestors);
                    ancestors.pop_back();
                }
            }
        }

        for (const Transition& t : tr->transitions()) {
            int64_t ts = 0, te = 0;
            t.getRange(ts, te);
            if (tick < ts || tick >= te) continue;
            w.writeU8(0x02);                              // active-transition marker
            w.writeU8(static_cast<uint8_t>(t.type));
            w.writeI64(t.duration);
            w.writeI64(t.editPointTick);
            w.writeU64(t.leftClipId);
            w.writeU64(t.rightClipId);
        }
    }
}

} // namespace

uint64_t hashCompositeConfigAt(const Timeline& timeline, int64_t tick,
                               const Project* project, uint32_t renderFlags)
{
    BinaryWriter w;
    w.writeU32(kHashVersion);
    w.writeU32(renderFlags);

    std::vector<const Timeline*> ancestors;
    appendCompositeConfig(w, timeline, tick, project, /*depth=*/0, ancestors);

    return fnv1a(w.data().data(), w.data().size());
}

} // namespace rt
