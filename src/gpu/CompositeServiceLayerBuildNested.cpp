/*
 * CompositeServiceLayerBuildNested.cpp - nested SequenceClip frame builder.
 * Extracted from CompositeServiceLayerBuild.cpp (modularization).
 *
 * Contains:
 *   - buildSequenceClipFrame() — recursive composite of a nested sequence's
 *     inner timeline into a clean CPU BGRA frame.
 *
 * Called from buildLayersForFrame() to reduce the size of the main timeline
 * traversal function. Behaviour is identical to the previous inline branch.
 */

#include "CompositeService.h"

#include "cache/FrameCache.h"
#include "timeline/SequenceClip.h"
#include "timeline/Timeline.h"
#include "project/Project.h"

namespace rt {

std::shared_ptr<CachedFrame> CompositeService::buildSequenceClipFrame(
    SequenceClip* seqClip, int64_t localTick,
    uint32_t outW, uint32_t outH, bool scrubMode,
    std::unique_lock<std::recursive_mutex>& lock)
{
    std::shared_ptr<CachedFrame> frame;

    if (!(m_project && seqClip->sequenceIndex() < m_project->sequenceCount()))
        return frame;

    auto* innerTimeline = m_project->sequence(seqClip->sequenceIndex());
    if (!innerTimeline || innerTimeline == m_timeline)
        return frame;

    // Map the local tick into the inner timeline, honoring
    // the clip's sourceIn so a trimmed nested sequence
    // (in/out set in the source monitor) shows the right
    // inner content instead of always starting at 0.
    int64_t innerTick = localTick + seqClip->sourceIn();
    if (innerTick < 0) innerTick = 0;

    // Force CPU display mode for the recursive composite
    // so it does the GPU→CPU readback INLINE while we
    // still hold the composite mutex. In GPU display mode
    // the inner composite returns a GPU-resident frame
    // backed by the SHARED composite output image; the
    // outer composite then immediately reuses that image,
    // racing the inner's deferred readback.
    const bool wasGpuMode = m_gpuDisplayMode;
    m_gpuDisplayMode = false;

    // Temporarily swap to the inner timeline and release
    // the lock so the recursive compositeFrame can acquire it.
    Timeline* outerTimeline = m_timeline;
    m_timeline = innerTimeline;
    lock.unlock();

    // Render the inner at the project's master
    // resolution rather than the (possibly scrubbed)
    // outer outW/outH.  Inner-layer decodes still
    // honour playbackTier internally — this only sets
    // the inner's output canvas size, keeping the
    // SequenceClip snapshot's dimensions consistent
    // between Full playback and scrubbed ticks so the
    // SequenceClip's transform produces stable geometry
    // every frame instead of subtly jittering as the
    // canvas shrinks/grows.  Falls back to outW/outH
    // when m_project is somehow null.
    uint32_t innerW = outW;
    uint32_t innerH = outH;
    if (innerTimeline) {
        // Each sequence has its own resolution — render the nested sequence at
        // ITS canvas size, not the parent/active sequence's.
        const auto& res = innerTimeline->settings().resolution();
        if (res.width > 0 && res.height > 0) {
            innerW = res.width;
            innerH = res.height;
        }
    }

    // isNestedRecursion=true so the inner composite
    // doesn't touch the outer's m_lastGoodComposite /
    // LRU / invalidate flag.  Without this, the inner
    // overwrites the cached frame the presenter reads,
    // producing the "nested sequence glitches to its
    // own first frame every other display tick" bug.
    auto innerFrame = compositeFrame(innerTick, innerW, innerH, scrubMode,
                                      /*isNestedRecursion=*/true);

    // Snapshot into a clean CPU-only BGRA frame. The
    // inner composite returns its shared m_lastGoodComposite,
    // which:
    //   • still has gpuReady/gpuImageView set (CPU display
    //     mode only changes presentation, tryCompositeOnGpu
    //     still tags the frame GPU-resident), AND
    //   • alternates between the GPU-composited frame and
    //     the single-layer fast path that returns the raw
    //     decoded frame.
    // Those two cases differ in channel order, so the old
    // unconditional needsSwapRB swapped only every other
    // frame → "every other frame inverted". A decoded /
    // read-back frame is ALWAYS straight BGRA (the project
    // convention), so copying pixels into a plain CPU
    // frame and treating it exactly like a normal video
    // layer (needsSwapRB=false) is correct for BOTH inner
    // paths and removes the GPU-aliasing race entirely.
    if (innerFrame && innerFrame->ensurePixels() &&
        !innerFrame->pixels.empty()) {
        auto cpu = std::make_shared<CachedFrame>();
        cpu->width              = innerFrame->width;
        cpu->height             = innerFrame->height;
        cpu->stride             = innerFrame->stride
            ? innerFrame->stride : innerFrame->width * 4;
        cpu->pixels             = innerFrame->pixels;
        cpu->unpackedAlpha      = true;
        cpu->premultipliedAlpha = innerFrame->premultipliedAlpha;
        frame = std::move(cpu);
    } else {
        frame = nullptr;
    }
    // NOTE: deliberately NOT setting fromNestedSequence —
    // the snapshot is plain BGRA, identical to any other
    // CPU video layer, so no R/B swap is needed.

    // Restore outer timeline and reacquire the lock
    lock.lock();
    m_timeline = outerTimeline;
    m_gpuDisplayMode = wasGpuMode;

    return frame;
}

} // namespace rt
