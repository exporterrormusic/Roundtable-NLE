/*
 * test_passthrough_eligibility.cpp — Phase 4.2 export 16F passthrough predicate.
 *
 * evaluatePassthroughAt is a pure walk of the timeline at a tick: eligible only
 * when exactly one full-frame, opaque, identity-transform, un-effected,
 * un-retimed plain video clip is active — no second layer, transition, caption,
 * or effecting adjustment.  Source-level gates (bit depth, dims, alpha, VFR)
 * are applied by the compositor, not here, so this suite covers the
 * timeline-level conditions only.
 */

#include <gtest/gtest.h>

#include "timeline/PassthroughEligibility.h"
#include "timeline/Timeline.h"
#include "timeline/Track.h"
#include "timeline/VideoClip.h"
#include "timeline/SpineClip.h"
#include "timeline/CaptionClip.h"
#include "timeline/AdjustmentClip.h"
#include "timeline/Transition.h"
#include "effects/Effect.h"
#include "effects/EffectStack.h"

namespace rt {
namespace {

VideoClip* addVideoClip(Track* tr, int64_t in, int64_t dur)
{
    auto c = std::make_unique<VideoClip>("test.mp4");
    c->setTimelineIn(in);
    c->setDuration(dur);
    return static_cast<VideoClip*>(tr->addClip(std::move(c)));
}

void addDissolve(Track* tr, int64_t editPoint, int64_t dur)
{
    Transition t;
    t.type          = TransitionType::CrossDissolve;
    t.editPointTick = editPoint;
    t.duration      = dur;
    t.leftClipId    = 1;
    t.rightClipId   = 2;
    tr->addTransition(t);
}

// ── Eligible base case ──────────────────────────────────────────────────────
TEST(PassthroughEligibility, SinglePlainVideoClipIsEligible)
{
    Timeline tl;
    auto* v1 = tl.addVideoTrack("V1");
    auto* clip = addVideoClip(v1, 1000, 48000);

    auto r = evaluatePassthroughAt(tl, 5000);
    EXPECT_TRUE(r.eligible);
    EXPECT_EQ(r.clip, clip);
    EXPECT_EQ(r.localTick, 5000 - 1000);
}

TEST(PassthroughEligibility, InactiveTickIsIneligible)
{
    Timeline tl;
    auto* v1 = tl.addVideoTrack("V1");
    addVideoClip(v1, 1000, 48000);            // [1000, 49000)

    EXPECT_FALSE(evaluatePassthroughAt(tl, 500).eligible);     // before
    EXPECT_FALSE(evaluatePassthroughAt(tl, 49000).eligible);   // half-open end
}

// ── Wrong clip type ─────────────────────────────────────────────────────────
TEST(PassthroughEligibility, SpineClipIsIneligible)
{
    Timeline tl;
    auto* v1 = tl.addVideoTrack("V1");
    auto c = std::make_unique<SpineClip>();
    c->setTimelineIn(0); c->setDuration(48000);
    v1->addClip(std::move(c));

    EXPECT_FALSE(evaluatePassthroughAt(tl, 1000).eligible);
}

TEST(PassthroughEligibility, VideoCharacterIsIneligible)
{
    Timeline tl;
    auto* v1 = tl.addVideoTrack("V1");
    auto* clip = addVideoClip(v1, 0, 48000);
    clip->setCharacterName("Wells");          // contain-fit 0.85×, never 1:1

    EXPECT_FALSE(evaluatePassthroughAt(tl, 1000).eligible);
}

// ── Second layer ────────────────────────────────────────────────────────────
TEST(PassthroughEligibility, TwoOverlappingLayersIneligible)
{
    Timeline tl;
    auto* v1 = tl.addVideoTrack("V1");
    auto* v2 = tl.addVideoTrack("V2");
    addVideoClip(v1, 0, 48000);
    addVideoClip(v2, 0, 48000);

    EXPECT_FALSE(evaluatePassthroughAt(tl, 1000).eligible);
}

// ── Per-clip compositing extras ─────────────────────────────────────────────
TEST(PassthroughEligibility, NonOpaqueIneligible)
{
    Timeline tl;
    auto* v1 = tl.addVideoTrack("V1");
    auto* clip = addVideoClip(v1, 0, 48000);
    clip->opacity().setDefaultValue(0.5f);

    EXPECT_FALSE(evaluatePassthroughAt(tl, 1000).eligible);
}

TEST(PassthroughEligibility, ScaledIneligible)
{
    Timeline tl;
    auto* v1 = tl.addVideoTrack("V1");
    auto* clip = addVideoClip(v1, 0, 48000);
    clip->scaleX().setDefaultValue(1.5f);

    EXPECT_FALSE(evaluatePassthroughAt(tl, 1000).eligible);
}

TEST(PassthroughEligibility, EffectIneligible)
{
    Timeline tl;
    auto* v1 = tl.addVideoTrack("V1");
    auto* clip = addVideoClip(v1, 0, 48000);
    clip->effects().addEffect(createEffect(EffectType::ColorCorrect));

    EXPECT_FALSE(evaluatePassthroughAt(tl, 1000).eligible);
}

TEST(PassthroughEligibility, BlendModeIneligible)
{
    Timeline tl;
    auto* v1 = tl.addVideoTrack("V1");
    auto* clip = addVideoClip(v1, 0, 48000);
    clip->setBlendMode(1);                    // Multiply

    EXPECT_FALSE(evaluatePassthroughAt(tl, 1000).eligible);
}

TEST(PassthroughEligibility, RetimeIneligible)
{
    Timeline tl;
    auto* v1 = tl.addVideoTrack("V1");
    auto* clip = addVideoClip(v1, 0, 48000);
    clip->setSpeed(2.0);

    EXPECT_FALSE(evaluatePassthroughAt(tl, 1000).eligible);
}

TEST(PassthroughEligibility, AnimatedSpeedRampIneligible)
{
    Timeline tl;
    auto* v1 = tl.addVideoTrack("V1");
    auto* clip = addVideoClip(v1, 0, 48000);
    clip->speedRamp().addKeyframe(0, 1.0f);   // animated ramp (value 1.0, but non-static)

    EXPECT_FALSE(evaluatePassthroughAt(tl, 1000).eligible);
}

// ── Transition ──────────────────────────────────────────────────────────────
TEST(PassthroughEligibility, ActiveTransitionIneligible)
{
    Timeline tl;
    auto* v1 = tl.addVideoTrack("V1");
    addVideoClip(v1, 0,     48000);
    addVideoClip(v1, 48000, 48000);
    addDissolve(v1, 48000, 12000);            // [42000, 54000)

    EXPECT_FALSE(evaluatePassthroughAt(tl, 48000).eligible);   // inside the dissolve
    EXPECT_TRUE (evaluatePassthroughAt(tl, 10000).eligible);   // well clear of it
}

// ── Caption overlay ─────────────────────────────────────────────────────────
TEST(PassthroughEligibility, ActiveCaptionIneligible)
{
    Timeline tl;
    auto* v1 = tl.addVideoTrack("V1");
    addVideoClip(v1, 0, 48000);
    auto* cap = tl.addCaptionTrack();
    auto cc = std::make_unique<CaptionClip>();
    cc->setText("hello");
    cc->setTimelineIn(0); cc->setDuration(48000);
    cap->addClip(std::move(cc));

    EXPECT_FALSE(evaluatePassthroughAt(tl, 1000).eligible);
}

// ── Adjustment layer ────────────────────────────────────────────────────────
TEST(PassthroughEligibility, AdjustmentWithEffectIneligible)
{
    Timeline tl;
    auto* v1 = tl.addVideoTrack("V1");
    auto* v2 = tl.addVideoTrack("V2");
    addVideoClip(v1, 0, 48000);
    auto adj = std::make_unique<AdjustmentClip>();
    adj->setTimelineIn(0); adj->setDuration(48000);
    adj->effects().addEffect(createEffect(EffectType::ColorCorrect));
    v2->addClip(std::move(adj));

    EXPECT_FALSE(evaluatePassthroughAt(tl, 1000).eligible);
}

TEST(PassthroughEligibility, NoOpAdjustmentStillEligible)
{
    Timeline tl;
    auto* v1 = tl.addVideoTrack("V1");
    auto* v2 = tl.addVideoTrack("V2");
    auto* clip = addVideoClip(v1, 0, 48000);
    auto adj = std::make_unique<AdjustmentClip>();
    adj->setTimelineIn(0); adj->setDuration(48000);   // no effects → invisible
    v2->addClip(std::move(adj));

    auto r = evaluatePassthroughAt(tl, 1000);
    EXPECT_TRUE(r.eligible);
    EXPECT_EQ(r.clip, clip);
}

} // namespace
} // namespace rt
