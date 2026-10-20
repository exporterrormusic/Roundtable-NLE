/*
 * CompositeHold.h -- what the compositor remembers between frames so it can
 * answer a frame whose sources are not all decoded yet.
 *
 *   last good   the newest complete composite, its tick and the visual clips
 *               it showed. Held while a new frame's sources decode (settle
 *               window) and reused for at most kMaxStaleTicks inside one
 *               shot; never across a shot change.
 *   static      the newest composite of a stack whose sources are all still
 *               images, keyed by the evaluated layer state; reused while the
 *               state is unchanged.
 *   settle      when the last complete composite was produced. Partial
 *               composites inside kSettleWindowMs of it hold the last good
 *               frame instead of showing layers popping in one by one.
 *
 * Thread-safe: the presenter and UI threads read and clear it while the
 * FrameProducer composites. No Timeline or GPU dependency; callers pass the
 * clip ids they computed.
 */

#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

namespace rt {

struct CachedFrame;

class CompositeHold
{
public:
    using Clock = std::chrono::steady_clock;

    /// 250 ms covers one cold NVDEC decoder open (~150-200 ms, see the
    /// prewarmUpcomingShots comment) plus headroom for a couple of peers to
    /// decode in parallel under the NVDEC-concurrency cap. Past it the
    /// partial composite is accepted, so a permanently unresolvable clip
    /// (missing file, codec mismatch) cannot freeze playback.
    static constexpr int kSettleWindowMs = 250;
    /// The last good frame may stand in for an empty layer stack only this
    /// close to its own tick: 1 frame at 24 fps, ~2.5 frames at 60 fps.
    static constexpr int64_t kMaxStaleTicks = 48000 / 24;

    // ── Last good frame ─────────────────────────────────────────────────
    [[nodiscard]] std::shared_ptr<CachedFrame> lastGood() const;
    /// The last good frame for a CPU-only consumer: clears its GPU handles
    /// (a modal dialog or a failed GPU composite must not touch the GPU).
    [[nodiscard]] std::shared_ptr<CachedFrame> lastGoodWithoutGpu();
    /// `frame` is the held last good frame and was composited for a tick
    /// other than `tick`: a best-effort answer that held the previous frame.
    [[nodiscard]] bool isHeldFromAnotherTick(const std::shared_ptr<CachedFrame>& frame,
                                             int64_t tick) const;
    /// A frame served from the segment cache becomes the last good frame;
    /// the clip ids of the previous one are kept.
    void adoptLastGood(std::shared_ptr<CachedFrame> frame, int64_t tick);

    // ── Forgetting ──────────────────────────────────────────────────────
    /// Last good, static and settle: a full cache invalidation.
    void clearAll();
    /// Last good and settle; the static frame survives.
    void clearLastGoodAndSettle();
    /// An edit inside [fromTick, toTick]: the static frame always goes; the
    /// last good frame (and settle) only if its tick is in the range.
    void invalidateRange(int64_t fromTick, int64_t toTick);
    void clearStatic();

    // ── Answers for a frame that cannot be composited yet ───────────────
    struct Settle
    {
        bool withinWindow{false};               ///< hold instead of compositing
        bool firstView{false};                  ///< this call armed the clock
        std::shared_ptr<CachedFrame> frame;     ///< null: nothing to hold yet
    };
    /// A partial composite at `tick`: hold the last good frame while inside
    /// the settle window and still on the same shot. `currentClipIds` is
    /// called (under the lock) only when there is a held frame to compare.
    [[nodiscard]] Settle settle(Clock::time_point now, int64_t tick,
                                const std::function<std::vector<uint64_t>()>& currentClipIds);

    struct Stale
    {
        enum class Kind {
            None,           ///< no last good frame
            DifferentShot,  ///< it showed another shot; it has been dropped
            Reuse,          ///< show `frame`
            TooOld,         ///< same shot but more than kMaxStaleTicks away
        };
        Kind kind{Kind::None};
        std::shared_ptr<CachedFrame> frame;
        int64_t lastTick{-1};
        int64_t delta{0};
    };
    /// No layer resolved at `tick` but clips are present.
    [[nodiscard]] Stale staleFallback(int64_t tick,
                                      const std::vector<uint64_t>& currentClipIds);

    // ── Static stacks ───────────────────────────────────────────────────
    /// The static composite for `key`, or null. A hit that has BGRA pixels
    /// also becomes the last good frame.
    [[nodiscard]] std::shared_ptr<CachedFrame> staticHit(
        uint64_t key, int64_t tick, std::vector<uint64_t> clipIds);

    // ── After a fresh composite ─────────────────────────────────────────
    /// Remember a BGRA composite as last good (and static, when `staticKey`
    /// is set). `complete`: every active clip resolved, which re-arms the
    /// settle clock. Returns whether `frame` already was the last good one.
    bool rememberFresh(std::shared_ptr<CachedFrame> frame, int64_t tick,
                       std::vector<uint64_t> clipIds,
                       std::optional<uint64_t> staticKey, bool complete);
    /// An NV12 export frame has no BGRA pixels, so it never stands in as
    /// the last good frame; it can still be the static one.
    void rememberStaticOnly(std::shared_ptr<CachedFrame> frame,
                            std::optional<uint64_t> staticKey);

private:
    mutable std::mutex m_mutex;
    std::shared_ptr<CachedFrame> m_lastGood;
    int64_t m_lastGoodTick{-1};
    /// Visual clips that produced m_lastGood. The complete set must match
    /// before a stale fallback is allowed: requiring only one overlapping id
    /// would let a persistent overlay authorize an unrelated prior shot.
    std::vector<uint64_t> m_lastGoodClipIds;
    std::shared_ptr<CachedFrame> m_static;
    uint64_t m_staticKey{0};
    Clock::time_point m_lastCompleteAt{};
};

/// Same set of visual clip ids. `current` must be sorted and unique.
[[nodiscard]] bool sameVisualClipSet(const std::vector<uint64_t>& current,
                                     const std::vector<uint64_t>& previous);

} // namespace rt
