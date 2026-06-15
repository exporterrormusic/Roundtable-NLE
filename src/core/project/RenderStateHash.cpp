/*
 * RenderStateHash.cpp — see RenderStateHash.h for the contract + coverage.
 */

#include "project/RenderStateHash.h"

#include "project/ClipSerialization.h"   // writeClip — full per-clip serialization
#include "project/BinaryIO.h"            // BinaryWriter
#include "project/Settings.h"
#include "timeline/Timeline.h"
#include "timeline/Track.h"
#include "timeline/Clip.h"
#include "timeline/Transition.h"

namespace rt {

namespace {

// FNV-1a 64.  Cheap, dependency-free, good enough for a cache/invalidation
// key (not a security hash).
constexpr uint64_t kFnvOffset = 1469598103934665603ull;
constexpr uint64_t kFnvPrime  = 1099511628211ull;

// Bump to force every cached segment to re-render after the hash algorithm
// or its coverage changes (old keys can never collide with new ones).
constexpr uint32_t kHashVersion = 1;

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

} // namespace

uint64_t hashCompositeConfigAt(const Timeline& timeline, int64_t tick)
{
    BinaryWriter w;
    w.writeU32(kHashVersion);

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

    return fnv1a(w.data().data(), w.data().size());
}

} // namespace rt
