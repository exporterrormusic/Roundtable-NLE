/*
 * ROUNDTABLE NLE v2 — Clip type unit tests
 * Step 3: All clip types + track/clip interaction
 */

#include <gtest/gtest.h>
#include "timeline/Track.h"
#include "timeline/Clip.h"
#include "timeline/SpineClip.h"
#include "timeline/VideoClip.h"
#include "timeline/AudioClip.h"
#include "timeline/TitleClip.h"
#include "timeline/AdjustmentClip.h"
#include "timeline/PngPuppetClip.h"

using namespace rt;

// ── SpineClip ───────────────────────────────────────────────────────────────

TEST(SpineClipTest, DefaultConstruction)
{
    SpineClip clip;
    EXPECT_EQ(clip.clipType(), ClipType::Spine);
    EXPECT_EQ(clip.label(), "Spine Clip");
    EXPECT_EQ(clip.animationName(), "idle");
    EXPECT_EQ(clip.outfit(), "default");
    EXPECT_EQ(clip.stance(), CharacterStance::Default);
    EXPECT_TRUE(clip.isLooping());
    EXPECT_FALSE(clip.isTalking());
    EXPECT_FLOAT_EQ(clip.animationSpeed(), 1.0f);
}

TEST(SpineClipTest, SetProperties)
{
    SpineClip clip;
    clip.setCharacterName("Modernia");
    clip.setOutfit("outfit_01");
    clip.setStance(CharacterStance::Aim);
    clip.setAnimationName("attack");
    clip.setLooping(false);
    clip.setTalking(true);
    clip.setAnimationSpeed(1.5f);
    clip.setCrop(0.1f, 0.2f, 0.3f, 0.4f);

    EXPECT_EQ(clip.characterName(), "Modernia");
    EXPECT_EQ(clip.outfit(), "outfit_01");
    EXPECT_EQ(clip.stance(), CharacterStance::Aim);
    EXPECT_EQ(clip.animationName(), "attack");
    EXPECT_FALSE(clip.isLooping());
    EXPECT_TRUE(clip.isTalking());
    EXPECT_FLOAT_EQ(clip.animationSpeed(), 1.5f);
    EXPECT_FLOAT_EQ(clip.cropLeft(), 0.1f);
    EXPECT_FLOAT_EQ(clip.cropRight(), 0.2f);
    EXPECT_FLOAT_EQ(clip.cropTop(), 0.3f);
    EXPECT_FLOAT_EQ(clip.cropBottom(), 0.4f);
}

TEST(SpineClipTest, Clone)
{
    SpineClip clip;
    clip.setCharacterName("Dorothy");
    clip.setTimelineIn(48000);
    clip.setDuration(96000);
    clip.setLabel("Test Clone");

    auto cloned = clip.clone();
    ASSERT_NE(cloned, nullptr);
    EXPECT_EQ(cloned->clipType(), ClipType::Spine);

    auto* sc = dynamic_cast<SpineClip*>(cloned.get());
    ASSERT_NE(sc, nullptr);
    EXPECT_EQ(sc->characterName(), "Dorothy");
    EXPECT_EQ(sc->timelineIn(), 48000);
    EXPECT_EQ(sc->duration(), 96000);
    EXPECT_EQ(sc->label(), "Test Clone");

    // Clone should have a different ID
    EXPECT_NE(cloned->id(), clip.id());
}

// ── PngPuppetClip ─────────────────────────────────────────────────────────────

TEST(PngPuppetClipTest, DefaultConstruction)
{
    PngPuppetClip clip;
    EXPECT_EQ(clip.clipType(), ClipType::PngPuppet);
    EXPECT_EQ(clip.variant(), "default");
    EXPECT_FALSE(clip.isTalking());
    EXPECT_GT(clip.blinkIntervalSeconds(), 0.0f);
    EXPECT_GT(clip.talkSwapSeconds(), 0.0f);
}

TEST(PngPuppetClipTest, IdleNeverOpensMouth)
{
    PngPuppetClip clip;            // talking == false by default
    // Across a long span the mouth must stay closed (faces 0 or 1 only).
    for (int i = 0; i < 2000; ++i) {
        const double t = i * 0.013;  // irregular sampling
        EXPECT_FALSE(clip.mouthOpenAt(t));
        const int face = clip.selectFace(t);
        EXPECT_TRUE(face == PngPuppetClip::MouthClosedEyesOpen ||
                    face == PngPuppetClip::MouthClosedEyesClosed);
    }
}

TEST(PngPuppetClipTest, TalkingOpensMouthSometimes)
{
    PngPuppetClip clip;
    clip.setTalking(true);
    int open = 0;
    for (int i = 0; i < 2000; ++i)
        if (clip.mouthOpenAt(i * clip.talkSwapSeconds())) ++open;
    // Lively but not stuck: a healthy fraction of frames are open.
    EXPECT_GT(open, 200);
    EXPECT_LT(open, 1800);
}

TEST(PngPuppetClipTest, BlinkHappensAndIsBounded)
{
    PngPuppetClip clip;
    clip.setBlinkIntervalSeconds(4.0f);
    clip.setBlinkDurationSeconds(0.12f);
    int closed = 0;
    const int N = 100000;
    const double dt = 0.001;
    for (int i = 0; i < N; ++i)
        if (clip.eyesClosedAt(i * dt)) ++closed;
    const double frac = static_cast<double>(closed) / N;
    // Expected duty cycle ≈ duration/interval = 0.03; allow generous slack.
    EXPECT_GT(frac, 0.005);
    EXPECT_LT(frac, 0.10);
}

TEST(PngPuppetClipTest, FrameSelectionIsDeterministic)
{
    PngPuppetClip clip;
    clip.setTalking(true);
    // Same time → same face, always (preview/export parity).
    for (int i = 0; i < 500; ++i) {
        const double t = i * 0.07;
        EXPECT_EQ(clip.selectFace(t), clip.selectFace(t));
    }
}

TEST(PngPuppetClipTest, SeedChangesBlinkPhase)
{
    PngPuppetClip a, b;
    a.setSeed(1);
    b.setSeed(99);
    // Different seeds should desynchronise blinking at some sampled time.
    bool differ = false;
    for (int i = 0; i < 5000 && !differ; ++i) {
        const double t = i * 0.01;
        if (a.eyesClosedAt(t) != b.eyesClosedAt(t)) differ = true;
    }
    EXPECT_TRUE(differ);
}

TEST(PngPuppetClipTest, CloneCopiesState)
{
    PngPuppetClip clip("Alice", "angry");
    clip.setTalking(true);
    clip.setFacePath(PngPuppetClip::MouthOpenEyesOpen, "assets/png_characters/Alice/angry/open_open.png");
    clip.setSeed(42);
    clip.setBreathAmplitude(12.0f);

    auto cloned = clip.clone();
    auto* pc = dynamic_cast<PngPuppetClip*>(cloned.get());
    ASSERT_NE(pc, nullptr);
    EXPECT_EQ(pc->characterName(), "Alice");
    EXPECT_EQ(pc->variant(), "angry");
    EXPECT_TRUE(pc->isTalking());
    EXPECT_EQ(pc->facePath(PngPuppetClip::MouthOpenEyesOpen),
              "assets/png_characters/Alice/angry/open_open.png");
    EXPECT_EQ(pc->seed(), 42u);
    EXPECT_FLOAT_EQ(pc->breathAmplitude(), 12.0f);
    EXPECT_NE(pc->id(), clip.id());
}

// ── VideoClip ───────────────────────────────────────────────────────────────

TEST(VideoClipTest, DefaultConstruction)
{
    VideoClip clip;
    EXPECT_EQ(clip.clipType(), ClipType::Video);
    EXPECT_EQ(clip.label(), "Video Clip");
    EXPECT_TRUE(clip.mediaPath().empty());
    EXPECT_EQ(clip.sourceWidth(), 0u);
    EXPECT_EQ(clip.sourceHeight(), 0u);
    EXPECT_FLOAT_EQ(clip.volume(), 1.0f);
}

TEST(VideoClipTest, SetProperties)
{
    VideoClip clip;
    clip.setMediaPath("/videos/intro.mp4");
    clip.setSourceResolution(1920, 1080);
    clip.setSourceFps(30.0);
    clip.setHasAudio(true);
    clip.setVolume(0.8f);

    EXPECT_EQ(clip.mediaPath(), "/videos/intro.mp4");
    EXPECT_EQ(clip.sourceWidth(), 1920u);
    EXPECT_EQ(clip.sourceHeight(), 1080u);
    EXPECT_DOUBLE_EQ(clip.sourceFps(), 30.0);
    EXPECT_TRUE(clip.hasAudio());
    EXPECT_FLOAT_EQ(clip.volume(), 0.8f);
}

TEST(VideoClipTest, Clone)
{
    VideoClip clip;
    clip.setMediaPath("/test.mp4");
    clip.setSourceResolution(3840, 2160);
    clip.setDuration(48000);

    auto cloned = clip.clone();
    auto* vc = dynamic_cast<VideoClip*>(cloned.get());
    ASSERT_NE(vc, nullptr);
    EXPECT_EQ(vc->mediaPath(), "/test.mp4");
    EXPECT_EQ(vc->sourceWidth(), 3840u);
    EXPECT_EQ(vc->sourceHeight(), 2160u);
    EXPECT_EQ(vc->duration(), 48000);
}

// ── AudioClip ───────────────────────────────────────────────────────────────

TEST(AudioClipTest, DefaultConstruction)
{
    AudioClip clip;
    EXPECT_EQ(clip.clipType(), ClipType::Audio);
    EXPECT_EQ(clip.sampleRate(), 48000u);
    EXPECT_EQ(clip.channels(), 2u);
    EXPECT_FLOAT_EQ(clip.volume().evaluate(0), 1.0f);
    EXPECT_FLOAT_EQ(clip.pan().evaluate(0), 0.0f);
}

TEST(AudioClipTest, VolumeKeyframes)
{
    AudioClip clip;
    clip.volume().addKeyframe(0, 0.0f);          // Start silent
    clip.volume().addKeyframe(48000, 1.0f);       // Fade in over 1 second

    EXPECT_FLOAT_EQ(clip.volume().evaluate(0), 0.0f);
    EXPECT_NEAR(clip.volume().evaluate(24000), 0.5f, 0.01f);
    EXPECT_FLOAT_EQ(clip.volume().evaluate(48000), 1.0f);
}

TEST(AudioClipTest, FadeInOut)
{
    AudioClip clip;
    clip.setFadeInDuration(4800);   // 100ms
    clip.setFadeOutDuration(9600);  // 200ms

    EXPECT_EQ(clip.fadeInDuration(), 4800);
    EXPECT_EQ(clip.fadeOutDuration(), 9600);
}

TEST(AudioClipTest, Clone)
{
    AudioClip clip;
    clip.setMediaPath("/audio/bgm.wav");
    clip.setSampleRate(44100);
    clip.setChannels(1);
    clip.setFadeInDuration(4800);
    clip.setAudioStreamIndex(1);   // copy/paste/undo must preserve the choice

    auto cloned = clip.clone();
    auto* ac = dynamic_cast<AudioClip*>(cloned.get());
    ASSERT_NE(ac, nullptr);
    EXPECT_EQ(ac->mediaPath(), "/audio/bgm.wav");
    EXPECT_EQ(ac->sampleRate(), 44100u);
    EXPECT_EQ(ac->channels(), 1u);
    EXPECT_EQ(ac->fadeInDuration(), 4800);
    EXPECT_EQ(ac->audioStreamIndex(), 1);
}

TEST(AudioClipTest, DefaultAudioStreamIsAuto)
{
    AudioClip clip;
    EXPECT_EQ(clip.audioStreamIndex(), -1);  // -1 = auto/best (legacy behavior)
}

// ── TitleClip ───────────────────────────────────────────────────────────────

TEST(TitleClipTest, DefaultConstruction)
{
    TitleClip clip;
    EXPECT_EQ(clip.clipType(), ClipType::Title);
    EXPECT_EQ(clip.text(), "Title");
    EXPECT_EQ(clip.fontFamily(), "Arial");
    EXPECT_FLOAT_EQ(clip.fontSize(), 72.0f);
    EXPECT_EQ(clip.alignment(), TextAlign::Center);
    EXPECT_EQ(clip.verticalAlignment(), TextVAlign::Middle);
}

TEST(TitleClipTest, SetProperties)
{
    TitleClip clip;
    clip.setText("Episode 1: The Beginning");
    clip.setFontFamily("Impact");
    clip.setFontSize(96.0f);
    clip.setBold(true);
    clip.setItalic(true);
    clip.setAlignment(TextAlign::Left);
    clip.setTextColor(0xFFFF0000);
    clip.setOutlineWidth(2.0f);

    EXPECT_EQ(clip.text(), "Episode 1: The Beginning");
    EXPECT_EQ(clip.fontFamily(), "Impact");
    EXPECT_FLOAT_EQ(clip.fontSize(), 96.0f);
    EXPECT_TRUE(clip.isBold());
    EXPECT_TRUE(clip.isItalic());
    EXPECT_EQ(clip.alignment(), TextAlign::Left);
    EXPECT_EQ(clip.textColor(), 0xFFFF0000u);
    EXPECT_FLOAT_EQ(clip.outlineWidth(), 2.0f);
}

TEST(TitleClipTest, Clone)
{
    TitleClip clip;
    clip.setText("Cloned Title");
    clip.setFontSize(48.0f);

    auto cloned = clip.clone();
    auto* tc = dynamic_cast<TitleClip*>(cloned.get());
    ASSERT_NE(tc, nullptr);
    EXPECT_EQ(tc->text(), "Cloned Title");
    EXPECT_FLOAT_EQ(tc->fontSize(), 48.0f);
}

// ── AdjustmentClip ──────────────────────────────────────────────────────────

TEST(AdjustmentClipTest, DefaultConstruction)
{
    AdjustmentClip clip;
    EXPECT_EQ(clip.clipType(), ClipType::Adjustment);
    EXPECT_EQ(clip.blendMode(), 0);
    EXPECT_FALSE(clip.affectsSingleTrack());
}

TEST(AdjustmentClipTest, Clone)
{
    AdjustmentClip clip;
    clip.setBlendMode(2);
    clip.setAffectsSingleTrack(true);

    auto cloned = clip.clone();
    auto* ac = dynamic_cast<AdjustmentClip*>(cloned.get());
    ASSERT_NE(ac, nullptr);
    EXPECT_EQ(ac->blendMode(), 2);
    EXPECT_TRUE(ac->affectsSingleTrack());
}

// ── Base Clip properties ────────────────────────────────────────────────────

TEST(ClipBaseTest, UniqueIds)
{
    SpineClip a, b, c;
    EXPECT_NE(a.id(), b.id());
    EXPECT_NE(b.id(), c.id());
    EXPECT_NE(a.id(), c.id());
}

TEST(ClipBaseTest, TimelinePosition)
{
    SpineClip clip;
    clip.setTimelineIn(48000);
    clip.setDuration(96000);

    EXPECT_EQ(clip.timelineIn(), 48000);
    EXPECT_EQ(clip.timelineOut(), 48000 + 96000);
    EXPECT_EQ(clip.duration(), 96000);
}

TEST(ClipBaseTest, SourceRange)
{
    SpineClip clip;
    clip.setSourceIn(12000);
    clip.setDuration(48000);

    EXPECT_EQ(clip.sourceIn(), 12000);
    EXPECT_EQ(clip.sourceOut(), 12000 + 48000);
}

TEST(ClipBaseTest, OpacityKeyframes)
{
    SpineClip clip;
    EXPECT_FLOAT_EQ(clip.opacity().evaluate(0), 1.0f);  // Default opacity = 1.0

    clip.opacity().addKeyframe(0, 0.0f);
    clip.opacity().addKeyframe(48000, 1.0f);
    EXPECT_NEAR(clip.opacity().evaluate(24000), 0.5f, 0.01f);
}

TEST(ClipBaseTest, EnabledFlag)
{
    SpineClip clip;
    EXPECT_TRUE(clip.isEnabled());
    clip.setEnabled(false);
    EXPECT_FALSE(clip.isEnabled());
}

// ── Track + Clip interaction ────────────────────────────────────────────────

TEST(TrackTest, AddAndRetrieveClips)
{
    Track track(TrackType::Video, "V1");

    auto clip1 = std::make_unique<SpineClip>();
    clip1->setTimelineIn(0);
    clip1->setDuration(48000);

    auto clip2 = std::make_unique<SpineClip>();
    clip2->setTimelineIn(48000);
    clip2->setDuration(48000);

    track.addClip(std::move(clip1));
    track.addClip(std::move(clip2));

    EXPECT_EQ(track.clipCount(), 2u);
    EXPECT_EQ(track.clip(0)->timelineIn(), 0);
    EXPECT_EQ(track.clip(1)->timelineIn(), 48000);
}

TEST(TrackTest, ClipsSortedByTimelineIn)
{
    Track track(TrackType::Video, "V1");

    auto clip1 = std::make_unique<SpineClip>();
    clip1->setTimelineIn(96000);
    clip1->setDuration(48000);

    auto clip2 = std::make_unique<SpineClip>();
    clip2->setTimelineIn(0);
    clip2->setDuration(48000);

    track.addClip(std::move(clip1));
    track.addClip(std::move(clip2));

    EXPECT_EQ(track.clip(0)->timelineIn(), 0);
    EXPECT_EQ(track.clip(1)->timelineIn(), 96000);
}

TEST(TrackTest, RemoveClip)
{
    Track track(TrackType::Video, "V1");
    auto clip = std::make_unique<SpineClip>();
    clip->setDuration(48000);
    track.addClip(std::move(clip));
    EXPECT_EQ(track.clipCount(), 1u);
    track.removeClip(0);
    EXPECT_EQ(track.clipCount(), 0u);
}

TEST(TrackTest, MoveClip)
{
    Track track(TrackType::Video, "V1");
    auto clip = std::make_unique<SpineClip>();
    clip->setTimelineIn(0);
    clip->setDuration(48000);
    track.addClip(std::move(clip));

    track.moveClip(0, 96000);
    EXPECT_EQ(track.clip(0)->timelineIn(), 96000);
}

TEST(TrackTest, ClipsAtTime)
{
    Track track(TrackType::Video, "V1");

    auto clip1 = std::make_unique<SpineClip>();
    clip1->setTimelineIn(0);
    clip1->setDuration(48000);

    auto clip2 = std::make_unique<SpineClip>();
    clip2->setTimelineIn(48000);
    clip2->setDuration(48000);

    track.addClip(std::move(clip1));
    track.addClip(std::move(clip2));

    auto at0 = track.clipsAtTime(0);
    EXPECT_EQ(at0.size(), 1u);

    auto at24k = track.clipsAtTime(24000);
    EXPECT_EQ(at24k.size(), 1u);

    auto at48k = track.clipsAtTime(48000);
    EXPECT_EQ(at48k.size(), 1u);
    EXPECT_EQ(at48k[0]->timelineIn(), 48000); // Should be second clip

    auto at96k = track.clipsAtTime(96000);
    EXPECT_EQ(at96k.size(), 0u); // Past end
}

TEST(TrackTest, Duration)
{
    Track track(TrackType::Video, "V1");
    EXPECT_EQ(track.duration(), 0);

    auto clip = std::make_unique<SpineClip>();
    clip->setTimelineIn(48000);
    clip->setDuration(48000);
    track.addClip(std::move(clip));

    EXPECT_EQ(track.duration(), 96000); // 48000 + 48000
}

TEST(TrackTest, Properties)
{
    Track track(TrackType::Video, "V1");

    EXPECT_FALSE(track.isLocked());
    EXPECT_FALSE(track.isMuted());
    EXPECT_FALSE(track.isSoloed());
    EXPECT_FLOAT_EQ(track.height(), 80.0f);

    track.setLocked(true);
    track.setMuted(true);
    track.setSoloed(true);
    track.setHeight(120.0f);

    EXPECT_TRUE(track.isLocked());
    EXPECT_TRUE(track.isMuted());
    EXPECT_TRUE(track.isSoloed());
    EXPECT_FLOAT_EQ(track.height(), 120.0f);
}

TEST(TrackTest, MixedClipTypes)
{
    Track track(TrackType::Video, "V1");

    auto spine = std::make_unique<SpineClip>();
    spine->setTimelineIn(0);
    spine->setDuration(48000);

    auto video = std::make_unique<VideoClip>();
    video->setTimelineIn(48000);
    video->setDuration(48000);

    auto title = std::make_unique<TitleClip>();
    title->setTimelineIn(96000);
    title->setDuration(24000);

    track.addClip(std::move(spine));
    track.addClip(std::move(video));
    track.addClip(std::move(title));

    EXPECT_EQ(track.clipCount(), 3u);
    EXPECT_EQ(track.clip(0)->clipType(), ClipType::Spine);
    EXPECT_EQ(track.clip(1)->clipType(), ClipType::Video);
    EXPECT_EQ(track.clip(2)->clipType(), ClipType::Title);
}
