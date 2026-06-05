/*
 * ROUNDTABLE NLE v2 — Project & Serialization unit tests
 * Step 5: Project file I/O validation
 *
 * Tests: Settings, Project factory, AssetDatabase, ProjectSerializer
 *        round-trip and file I/O.
 */

#include <gtest/gtest.h>
#include <filesystem>

#include "project/Project.h"
#include "project/Settings.h"
#include "project/AssetDatabase.h"
#include "project/ProjectSerializer.h"
#include "timeline/Timeline.h"
#include "timeline/Track.h"
#include "timeline/Clip.h"
#include "timeline/SpineClip.h"
#include "timeline/VideoClip.h"
#include "timeline/AudioClip.h"
#include "timeline/TitleClip.h"
#include "timeline/AdjustmentClip.h"
#include "timeline/Marker.h"
#include "timeline/Transition.h"
#include "timeline/KeyframeTrack.h"
#include "command/CommandStack.h"
#include "audiofx/FxChain.h"
#include "audiofx/ParametricEQ.h"
#include "audiofx/Dynamics.h"

using namespace rt;

// ═══════════════════════════════════════════════════════════════════════════
// Settings tests
// ═══════════════════════════════════════════════════════════════════════════

TEST(SettingsTest, DefaultValues)
{
    Settings s;
    EXPECT_EQ(s.resolution().width, 1920u);
    EXPECT_EQ(s.resolution().height, 1080u);
    EXPECT_DOUBLE_EQ(s.frameRate(), 30.0);
    EXPECT_EQ(s.colorSpace(), ColorSpace::sRGB);
    EXPECT_EQ(s.sampleRate(), 48000u);
    EXPECT_EQ(s.audioBitDepth(), 16u);
    EXPECT_EQ(s.audioChannels(), 2u);
    EXPECT_EQ(s.exportSettings().codec, "h264_nvenc");
    EXPECT_EQ(s.exportSettings().quality, 23u);
}

TEST(SettingsTest, Setters)
{
    Settings s;
    s.setResolution(3840, 2160);
    EXPECT_EQ(s.resolution().width, 3840u);
    EXPECT_EQ(s.resolution().height, 2160u);

    s.setFrameRate(60.0);
    EXPECT_DOUBLE_EQ(s.frameRate(), 60.0);

    s.setColorSpace(ColorSpace::Rec709);
    EXPECT_EQ(s.colorSpace(), ColorSpace::Rec709);

    AudioFormat af{96000, 24, 6};
    s.setAudioFormat(af);
    EXPECT_EQ(s.sampleRate(), 96000u);
    EXPECT_EQ(s.audioBitDepth(), 24u);
    EXPECT_EQ(s.audioChannels(), 6u);
}

TEST(SettingsTest, TicksPerFrame)
{
    Settings s;
    s.setFrameRate(30.0);
    EXPECT_EQ(s.ticksPerFrame(), 1600); // 48000 / 30

    s.setFrameRate(24.0);
    EXPECT_EQ(s.ticksPerFrame(), 2000); // 48000 / 24
}

TEST(SettingsTest, Equality)
{
    Settings a, b;
    EXPECT_EQ(a, b);

    b.setFrameRate(60.0);
    EXPECT_NE(a, b);

    a.setFrameRate(60.0);
    EXPECT_EQ(a, b);
}

// ═══════════════════════════════════════════════════════════════════════════
// Project tests
// ═══════════════════════════════════════════════════════════════════════════

TEST(ProjectTest, Construction)
{
    Project p;
    EXPECT_EQ(p.name(), "Untitled");
    EXPECT_FALSE(p.isModified());
    EXPECT_NE(p.timeline(), nullptr);
    EXPECT_NE(p.assets(), nullptr);
    EXPECT_NE(p.commandStack(), nullptr);
    EXPECT_EQ(p.formatVersion(), 2u);
}

TEST(ProjectTest, CreateNewFactory)
{
    auto p = Project::createNew("Test Project");
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(p->name(), "Test Project");
    EXPECT_FALSE(p->isModified());

    // Should have default tracks: Video 1 + Audio 1
    EXPECT_EQ(p->timeline()->trackCount(), 2u);
    EXPECT_EQ(p->timeline()->track(0)->type(), TrackType::Video);
    EXPECT_EQ(p->timeline()->track(1)->type(), TrackType::Audio);
    EXPECT_EQ(p->timeline()->track(0)->name(), "Video 1");
    EXPECT_EQ(p->timeline()->track(1)->name(), "Audio 1");
}

TEST(ProjectTest, ModifiedFlag)
{
    Project p;
    EXPECT_FALSE(p.isModified());
    p.setModified(true);
    EXPECT_TRUE(p.isModified());
    p.setModified(false);
    EXPECT_FALSE(p.isModified());
}

TEST(ProjectTest, NameAndPath)
{
    Project p;
    p.setName("My Project");
    EXPECT_EQ(p.name(), "My Project");

    p.setFilePath("C:/test/project.rtp");
    EXPECT_EQ(p.filePath(), std::filesystem::path("C:/test/project.rtp"));
}

// ═══════════════════════════════════════════════════════════════════════════
// AssetDatabase tests
// ═══════════════════════════════════════════════════════════════════════════

TEST(AssetDatabaseTest, AddAndFind)
{
    AssetDatabase db;
    EXPECT_EQ(db.assetCount(), 0u);

    AssetEntry e;
    e.type = AssetType::Audio;
    e.name = "test.wav";
    e.path = "audio/test.wav";
    uint64_t id = db.addAsset(std::move(e));

    EXPECT_EQ(db.assetCount(), 1u);
    EXPECT_GT(id, 0u);

    const AssetEntry* found = db.findById(id);
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found->name, "test.wav");
    EXPECT_EQ(found->type, AssetType::Audio);
}

TEST(AssetDatabaseTest, FindByType)
{
    AssetDatabase db;

    AssetEntry a1; a1.type = AssetType::Audio; a1.name = "a1.wav";
    AssetEntry a2; a2.type = AssetType::Audio; a2.name = "a2.wav";
    AssetEntry v1; v1.type = AssetType::Video; v1.name = "v1.mp4";
    db.addAsset(std::move(a1));
    db.addAsset(std::move(a2));
    db.addAsset(std::move(v1));

    auto audios = db.findByType(AssetType::Audio);
    EXPECT_EQ(audios.size(), 2u);

    auto videos = db.findByType(AssetType::Video);
    EXPECT_EQ(videos.size(), 1u);

    auto images = db.findByType(AssetType::Image);
    EXPECT_EQ(images.size(), 0u);
}

TEST(AssetDatabaseTest, Remove)
{
    AssetDatabase db;
    AssetEntry e; e.type = AssetType::Image; e.name = "bg.png";
    uint64_t id = db.addAsset(std::move(e));
    EXPECT_EQ(db.assetCount(), 1u);

    db.removeAsset(id);
    EXPECT_EQ(db.assetCount(), 0u);
    EXPECT_EQ(db.findById(id), nullptr);
}

TEST(AssetDatabaseTest, RemoveNonExistent)
{
    AssetDatabase db;
    // Should not crash
    db.removeAsset(999);
    EXPECT_EQ(db.assetCount(), 0u);
}

// ═══════════════════════════════════════════════════════════════════════════
// ProjectSerializer tests
// ═══════════════════════════════════════════════════════════════════════════

class SerializerTest : public ::testing::Test
{
protected:
    ProjectSerializer serializer;

    /// Build a project with various data for round-trip testing
    std::unique_ptr<Project> makeTestProject()
    {
        auto p = Project::createNew("Serializer Test");
        auto* tl = p->timeline();

        // Modify settings
        p->settings().setResolution(3840, 2160);
        p->settings().setFrameRate(60.0);
        p->settings().setColorSpace(ColorSpace::Rec709);

        AudioFormat af{96000, 24, 2};
        p->settings().setAudioFormat(af);

        ExportSettings es;
        es.codec = "hevc_nvenc";
        es.quality = 18;
        es.audioBitrate = 320;
        es.outputPath = "C:/output/video.mp4";
        p->settings().setExportSettings(es);

        tl->setName("Main Sequence");
        tl->setPlayheadPosition(48000);
        tl->setInPoint(0);
        tl->setOutPoint(480000);

        // Add a SpineClip to Video 1
        auto spine = std::make_unique<SpineClip>();
        spine->setCharacterName("Modernia");
        spine->setOutfit("outfit_01");
        spine->setStance(CharacterStance::Aim);
        spine->setAnimationName("talk_01");
        spine->setLooping(false);
        spine->setTalking(true);
        spine->setAnimationSpeed(1.5f);
        spine->setCrop(0.1f, 0.2f, 0.0f, 0.05f);
        spine->setTimelineIn(0);
        spine->setDuration(96000);
        spine->setLabel("Modernia Talk");
        spine->setColor(0xFFFF0000);
        tl->track(0)->addClip(std::move(spine));

        // Add a VideoClip
        auto video = std::make_unique<VideoClip>();
        video->setMediaPath("videos/intro.mp4");
        video->setMediaId(42);
        video->setSourceResolution(1920, 1080);
        video->setSourceFps(30.0);
        video->setSourceDuration(240000);
        video->setHasAudio(true);
        video->setVolume(0.8f);
        video->setTimelineIn(96000);
        video->setDuration(48000);
        video->setLabel("Intro Video");
        tl->track(0)->addClip(std::move(video));

        // Add a TitleClip
        auto title = std::make_unique<TitleClip>();
        title->setText("ROUNDTABLE");
        title->setFontFamily("Montserrat");
        title->setFontSize(96.0f);
        title->setBold(true);
        title->setItalic(false);
        title->setAlignment(TextAlign::Center);
        title->setVerticalAlignment(TextVAlign::Bottom);
        title->setTextColor(0xFFFFFFFF);
        title->setBgColor(0x80000000);
        title->setOutlineColor(0xFF000000);
        title->setOutlineWidth(2.5f);
        title->setTimelineIn(144000);
        title->setDuration(72000);
        title->setLabel("Title Card");
        tl->track(0)->addClip(std::move(title));

        // Add AudioClips to Audio 1
        auto audio1 = std::make_unique<AudioClip>();
        audio1->setMediaPath("audio/voice.wav");
        audio1->setMediaId(10);
        audio1->setSampleRate(44100);
        audio1->setChannels(1);
        audio1->setSourceDuration(192000);
        audio1->setFadeInDuration(2400);
        audio1->setFadeOutDuration(4800);
        audio1->setTimelineIn(0);
        audio1->setDuration(192000);
        audio1->setLabel("Voice Over");
        tl->track(1)->addClip(std::move(audio1));

        // Add an AdjustmentClip to a new video track
        auto* vt2 = tl->addVideoTrack("Video 2");
        vt2->setLocked(true);
        vt2->setMuted(false);
        vt2->setSoloed(true);
        vt2->setHeight(120.0f);

        auto adj = std::make_unique<AdjustmentClip>();
        adj->setBlendMode(3);
        adj->setAffectsSingleTrack(true);
        adj->setTimelineIn(24000);
        adj->setDuration(48000);
        adj->setLabel("Color Grade");
        vt2->addClip(std::move(adj));

        // Add a transition to Video 1
        Transition t;
        t.type = TransitionType::CrossDissolve;
        t.duration = 4800;
        t.offset = -2400;
        t.param1 = 0.5f;
        t.param2 = 0.0f;
        tl->track(0)->addTransition(t);

        // Add markers
        tl->addMarker(0, "Start", 0xFF00FF00);
        tl->addMarker(96000, "Mid Point", 0xFFFF0000);
        tl->addMarker(480000, "End", 0xFF0000FF);

        // Add assets
        AssetEntry ae;
        ae.type = AssetType::Audio;
        ae.name = "voice.wav";
        ae.path = "audio/voice.wav";
        ae.absolutePath = "C:/project/audio/voice.wav";
        ae.fileSize = 1024000;
        ae.hash = "abc123";
        p->assets()->addAsset(std::move(ae));

        AssetEntry ve;
        ve.type = AssetType::Video;
        ve.name = "intro.mp4";
        ve.path = "videos/intro.mp4";
        ve.absolutePath = "C:/project/videos/intro.mp4";
        ve.fileSize = 50000000;
        ve.hash = "def456";
        p->assets()->addAsset(std::move(ve));

        p->setModified(false);
        return p;
    }
};

TEST_F(SerializerTest, RoundTripInMemory)
{
    auto original = makeTestProject();
    auto data = serializer.serialize(*original);

    // Verify magic header
    ASSERT_GE(data.size(), 32u);
    EXPECT_EQ(data[0], 'R');
    EXPECT_EQ(data[1], 'N');
    EXPECT_EQ(data[2], 'D');
    EXPECT_EQ(data[3], 'T');
    EXPECT_EQ(data[4], 'B');
    EXPECT_EQ(data[5], 'L');
    EXPECT_EQ(data[6], 'v');
    EXPECT_EQ(data[7], '2');

    auto loaded = serializer.deserialize(data);
    ASSERT_NE(loaded, nullptr);

    // Settings
    EXPECT_EQ(loaded->settings().resolution().width, 3840u);
    EXPECT_EQ(loaded->settings().resolution().height, 2160u);
    EXPECT_DOUBLE_EQ(loaded->settings().frameRate(), 60.0);
    EXPECT_EQ(loaded->settings().colorSpace(), ColorSpace::Rec709);
    EXPECT_EQ(loaded->settings().sampleRate(), 96000u);
    EXPECT_EQ(loaded->settings().audioBitDepth(), 24u);
    EXPECT_EQ(loaded->settings().exportSettings().codec, "hevc_nvenc");
    EXPECT_EQ(loaded->settings().exportSettings().quality, 18u);
    EXPECT_EQ(loaded->settings().exportSettings().audioBitrate, 320u);
    EXPECT_EQ(loaded->settings().exportSettings().outputPath, "C:/output/video.mp4");

    // Timeline metadata
    EXPECT_EQ(loaded->name(), "Serializer Test");
    EXPECT_EQ(loaded->timeline()->name(), "Main Sequence");
    EXPECT_EQ(loaded->timeline()->playheadPosition(), 48000);
    EXPECT_EQ(loaded->timeline()->inPoint(), 0);
    EXPECT_EQ(loaded->timeline()->outPoint(), 480000);

    // Tracks — on load the V/A divider invariant regroups all video tracks
    // above audio tracks, so the saved order (Video 1, Audio 1, Video 2) loads
    // back as Video 1, Video 2, Audio 1. All track fields are preserved; only
    // the indices change.
    ASSERT_EQ(loaded->timeline()->trackCount(), 3u);
    EXPECT_EQ(loaded->timeline()->track(0)->type(), TrackType::Video);
    EXPECT_EQ(loaded->timeline()->track(0)->name(), "Video 1");
    EXPECT_EQ(loaded->timeline()->track(1)->type(), TrackType::Video);
    EXPECT_EQ(loaded->timeline()->track(1)->name(), "Video 2");
    EXPECT_EQ(loaded->timeline()->track(2)->type(), TrackType::Audio);
    EXPECT_EQ(loaded->timeline()->track(2)->name(), "Audio 1");

    // Track properties (Video 2 — locked, soloed, height 120 — is now at index 1)
    EXPECT_TRUE(loaded->timeline()->track(1)->isLocked());
    EXPECT_FALSE(loaded->timeline()->track(1)->isMuted());
    EXPECT_TRUE(loaded->timeline()->track(1)->isSoloed());
    EXPECT_FLOAT_EQ(loaded->timeline()->track(1)->height(), 120.0f);

    // Modified flag
    EXPECT_FALSE(loaded->isModified());
}

TEST_F(SerializerTest, AudioFxChainRoundTrip)
{
    auto p = Project::createNew("FX Test");
    auto* tl = p->timeline();

    auto audio = std::make_unique<AudioClip>();
    audio->setMediaPath("audio/voice.wav");
    audio->setTimelineIn(0);
    audio->setDuration(96000);

    // Build a chain: EQ (HPF + 2 bands + trim) then Dynamics (all 3 stages).
    auto* eq = static_cast<audiofx::ParametricEQ*>(
        audio->audioFx().add(audiofx::ProcessorKind::ParametricEQ));
    eq->setHighPass(true, 90.0f, 0.8f);
    audiofx::ParametricEQ::Band b;
    b.enabled = true; b.type = audiofx::Biquad::Type::Peaking;
    b.freqHz = 3200.0f; b.gainDb = 4.5f; b.q = 1.3f;
    eq->setBand(0, b);
    eq->setOutputGainDb(-1.5f);

    auto* dyn = static_cast<audiofx::Dynamics*>(
        audio->audioFx().add(audiofx::ProcessorKind::Dynamics));
    dyn->loadVoicePreset();
    dyn->setEnabled(false);  // exercise per-processor enabled flag

    tl->track(1)->addClip(std::move(audio));

    auto data = serializer.serialize(*p);
    auto loaded = serializer.deserialize(data);
    ASSERT_NE(loaded, nullptr);

    // Audio clip lands on the audio track (index 1).
    const Track* atrack = nullptr;
    for (size_t i = 0; i < loaded->timeline()->trackCount(); ++i) {
        if (loaded->timeline()->track(i)->type() == TrackType::Audio) {
            atrack = loaded->timeline()->track(i);
            break;
        }
    }
    ASSERT_NE(atrack, nullptr);
    ASSERT_GE(atrack->clipCount(), 1u);
    const auto* aclip = dynamic_cast<const AudioClip*>(atrack->clip(0));
    ASSERT_NE(aclip, nullptr);

    const auto& chain = aclip->audioFx();
    ASSERT_EQ(chain.size(), 2u);

    const auto* leq = dynamic_cast<const audiofx::ParametricEQ*>(&chain.at(0));
    ASSERT_NE(leq, nullptr);
    EXPECT_TRUE(leq->highPassOn());
    EXPECT_FLOAT_EQ(leq->highPassFreq(), 90.0f);
    EXPECT_FLOAT_EQ(leq->highPassQ(), 0.8f);
    EXPECT_TRUE(leq->band(0).enabled);
    EXPECT_FLOAT_EQ(leq->band(0).freqHz, 3200.0f);
    EXPECT_FLOAT_EQ(leq->band(0).gainDb, 4.5f);
    EXPECT_FLOAT_EQ(leq->band(0).q, 1.3f);
    EXPECT_FLOAT_EQ(leq->outputGainDb(), -1.5f);

    const auto* ldyn = dynamic_cast<const audiofx::Dynamics*>(&chain.at(1));
    ASSERT_NE(ldyn, nullptr);
    EXPECT_FALSE(ldyn->isEnabled());
    EXPECT_TRUE(ldyn->gate().enabled);
    EXPECT_TRUE(ldyn->compressor().enabled);
    EXPECT_FLOAT_EQ(ldyn->compressor().makeupDb, 4.0f);  // from loadVoicePreset
    EXPECT_TRUE(ldyn->limiter().enabled);
}

TEST_F(SerializerTest, SpineClipRoundTrip)
{
    auto original = makeTestProject();
    auto data = serializer.serialize(*original);
    auto loaded = serializer.deserialize(data);
    ASSERT_NE(loaded, nullptr);

    // Video 1 should have 3 clips; first is SpineClip
    ASSERT_GE(loaded->timeline()->track(0)->clipCount(), 1u);
    auto* clip = loaded->timeline()->track(0)->clip(0);
    ASSERT_EQ(clip->clipType(), ClipType::Spine);

    auto* sc = static_cast<SpineClip*>(clip);
    EXPECT_EQ(sc->characterName(), "Modernia");
    EXPECT_EQ(sc->outfit(), "outfit_01");
    EXPECT_EQ(sc->stance(), CharacterStance::Aim);
    EXPECT_EQ(sc->animationName(), "talk_01");
    EXPECT_FALSE(sc->isLooping());
    EXPECT_TRUE(sc->isTalking());
    EXPECT_FLOAT_EQ(sc->animationSpeed(), 1.5f);
    EXPECT_FLOAT_EQ(sc->cropLeft(), 0.1f);
    EXPECT_FLOAT_EQ(sc->cropRight(), 0.2f);
    EXPECT_FLOAT_EQ(sc->cropTop(), 0.0f);
    EXPECT_FLOAT_EQ(sc->cropBottom(), 0.05f);
    EXPECT_EQ(sc->timelineIn(), 0);
    EXPECT_EQ(sc->duration(), 96000);
    EXPECT_EQ(sc->label(), "Modernia Talk");
    EXPECT_EQ(sc->color(), 0xFFFF0000u);
}

TEST_F(SerializerTest, ProjectShowRoundTrip)
{
    auto original = makeTestProject();
    original->setShow("Roundtable Talk");
    auto data = serializer.serialize(*original);
    auto loaded = serializer.deserialize(data);
    ASSERT_NE(loaded, nullptr);
    EXPECT_EQ(loaded->show(), "Roundtable Talk");
}

TEST_F(SerializerTest, ProjectShowDefaultsEmpty)
{
    // A project with no show assigned should deserialize with an empty show
    // (and old files, which lack the section entirely, behave the same).
    auto original = makeTestProject();
    auto data = serializer.serialize(*original);
    auto loaded = serializer.deserialize(data);
    ASSERT_NE(loaded, nullptr);
    EXPECT_TRUE(loaded->show().empty());
}

TEST_F(SerializerTest, VideoClipRoundTrip)
{
    auto original = makeTestProject();
    auto data = serializer.serialize(*original);
    auto loaded = serializer.deserialize(data);
    ASSERT_NE(loaded, nullptr);

    ASSERT_GE(loaded->timeline()->track(0)->clipCount(), 2u);
    auto* clip = loaded->timeline()->track(0)->clip(1);
    ASSERT_EQ(clip->clipType(), ClipType::Video);

    auto* vc = static_cast<VideoClip*>(clip);
    EXPECT_EQ(vc->mediaPath(), "videos/intro.mp4");
    EXPECT_EQ(vc->mediaId(), 42u);
    EXPECT_EQ(vc->sourceWidth(), 1920u);
    EXPECT_EQ(vc->sourceHeight(), 1080u);
    EXPECT_DOUBLE_EQ(vc->sourceFps(), 30.0);
    EXPECT_EQ(vc->sourceDuration(), 240000);
    EXPECT_TRUE(vc->hasAudio());
    EXPECT_FLOAT_EQ(vc->volume(), 0.8f);
    EXPECT_EQ(vc->timelineIn(), 96000);
    EXPECT_EQ(vc->duration(), 48000);
}

TEST_F(SerializerTest, TitleClipRoundTrip)
{
    auto original = makeTestProject();
    auto data = serializer.serialize(*original);
    auto loaded = serializer.deserialize(data);
    ASSERT_NE(loaded, nullptr);

    ASSERT_GE(loaded->timeline()->track(0)->clipCount(), 3u);
    auto* clip = loaded->timeline()->track(0)->clip(2);
    ASSERT_EQ(clip->clipType(), ClipType::Title);

    auto* tc = static_cast<TitleClip*>(clip);
    EXPECT_EQ(tc->text(), "ROUNDTABLE");
    EXPECT_EQ(tc->fontFamily(), "Montserrat");
    EXPECT_FLOAT_EQ(tc->fontSize(), 96.0f);
    EXPECT_TRUE(tc->isBold());
    EXPECT_FALSE(tc->isItalic());
    EXPECT_EQ(tc->alignment(), TextAlign::Center);
    EXPECT_EQ(tc->verticalAlignment(), TextVAlign::Bottom);
    EXPECT_EQ(tc->textColor(), 0xFFFFFFFFu);
    EXPECT_EQ(tc->bgColor(), 0x80000000u);
    EXPECT_EQ(tc->outlineColor(), 0xFF000000u);
    EXPECT_FLOAT_EQ(tc->outlineWidth(), 2.5f);
}

TEST_F(SerializerTest, AudioClipRoundTrip)
{
    auto original = makeTestProject();
    auto data = serializer.serialize(*original);
    auto loaded = serializer.deserialize(data);
    ASSERT_NE(loaded, nullptr);

    // Audio 1 regroups below the video tracks on load (index 2, not 1).
    ASSERT_GE(loaded->timeline()->track(2)->clipCount(), 1u);
    auto* clip = loaded->timeline()->track(2)->clip(0);
    ASSERT_EQ(clip->clipType(), ClipType::Audio);

    auto* ac = static_cast<AudioClip*>(clip);
    EXPECT_EQ(ac->mediaPath(), "audio/voice.wav");
    EXPECT_EQ(ac->mediaId(), 10u);
    EXPECT_EQ(ac->sampleRate(), 44100u);
    EXPECT_EQ(ac->channels(), 1u);
    EXPECT_EQ(ac->sourceDuration(), 192000);
    EXPECT_EQ(ac->fadeInDuration(), 2400);
    EXPECT_EQ(ac->fadeOutDuration(), 4800);
}

TEST_F(SerializerTest, AdjustmentClipRoundTrip)
{
    auto original = makeTestProject();
    auto data = serializer.serialize(*original);
    auto loaded = serializer.deserialize(data);
    ASSERT_NE(loaded, nullptr);

    // The AdjustmentClip lives on "Video 2", which regroups to index 1 on load.
    ASSERT_GE(loaded->timeline()->trackCount(), 3u);
    ASSERT_GE(loaded->timeline()->track(1)->clipCount(), 1u);
    auto* clip = loaded->timeline()->track(1)->clip(0);
    ASSERT_EQ(clip->clipType(), ClipType::Adjustment);

    auto* ac = static_cast<AdjustmentClip*>(clip);
    EXPECT_EQ(ac->blendMode(), 3u);
    EXPECT_TRUE(ac->affectsSingleTrack());
    EXPECT_EQ(ac->timelineIn(), 24000);
    EXPECT_EQ(ac->duration(), 48000);
}

TEST_F(SerializerTest, TransitionRoundTrip)
{
    auto original = makeTestProject();
    auto data = serializer.serialize(*original);
    auto loaded = serializer.deserialize(data);
    ASSERT_NE(loaded, nullptr);

    ASSERT_EQ(loaded->timeline()->track(0)->transitionCount(), 1u);
    const Transition* t = loaded->timeline()->track(0)->transition(0);
    ASSERT_NE(t, nullptr);
    EXPECT_EQ(t->type, TransitionType::CrossDissolve);
    EXPECT_EQ(t->duration, 4800);
    EXPECT_EQ(t->offset, -2400);
    EXPECT_FLOAT_EQ(t->param1, 0.5f);
    EXPECT_FLOAT_EQ(t->param2, 0.0f);
}

TEST_F(SerializerTest, MarkerRoundTrip)
{
    auto original = makeTestProject();
    auto data = serializer.serialize(*original);
    auto loaded = serializer.deserialize(data);
    ASSERT_NE(loaded, nullptr);

    auto& markers = loaded->timeline()->markers();
    ASSERT_EQ(markers.size(), 3u);
    EXPECT_EQ(markers[0].time, 0);
    EXPECT_EQ(markers[0].label, "Start");
    EXPECT_EQ(markers[0].color, 0xFF00FF00u);
    EXPECT_EQ(markers[1].time, 96000);
    EXPECT_EQ(markers[1].label, "Mid Point");
    EXPECT_EQ(markers[2].time, 480000);
    EXPECT_EQ(markers[2].label, "End");
}

TEST_F(SerializerTest, AssetRoundTrip)
{
    auto original = makeTestProject();
    auto data = serializer.serialize(*original);
    auto loaded = serializer.deserialize(data);
    ASSERT_NE(loaded, nullptr);

    EXPECT_EQ(loaded->assets()->assetCount(), 2u);

    auto audios = loaded->assets()->findByType(AssetType::Audio);
    ASSERT_EQ(audios.size(), 1u);
    EXPECT_EQ(audios[0]->name, "voice.wav");
    EXPECT_EQ(audios[0]->hash, "abc123");
    EXPECT_EQ(audios[0]->fileSize, 1024000u);

    auto videos = loaded->assets()->findByType(AssetType::Video);
    ASSERT_EQ(videos.size(), 1u);
    EXPECT_EQ(videos[0]->name, "intro.mp4");
}

TEST_F(SerializerTest, EmptyProject)
{
    Project p;
    // Remove default tracks for a truly empty project
    while (p.timeline()->trackCount() > 0)
        p.timeline()->removeTrack(0);

    auto data = serializer.serialize(p);
    auto loaded = serializer.deserialize(data);
    ASSERT_NE(loaded, nullptr);

    EXPECT_EQ(loaded->timeline()->trackCount(), 0u);
    EXPECT_EQ(loaded->assets()->assetCount(), 0u);
    EXPECT_EQ(loaded->timeline()->markers().size(), 0u);
}

TEST_F(SerializerTest, InvalidMagic)
{
    std::vector<uint8_t> data(64, 0);
    data[0] = 'X'; // Wrong magic
    auto result = serializer.deserialize(data);
    EXPECT_EQ(result, nullptr);
}

TEST_F(SerializerTest, TooSmall)
{
    std::vector<uint8_t> data(16, 0);
    auto result = serializer.deserialize(data);
    EXPECT_EQ(result, nullptr);
}

TEST_F(SerializerTest, FutureVersion)
{
    std::vector<uint8_t> data(64, 0);
    // Write correct magic
    data[0] = 'R'; data[1] = 'N'; data[2] = 'D'; data[3] = 'T';
    data[4] = 'B'; data[5] = 'L'; data[6] = 'v'; data[7] = '2';
    // Write future version (999)
    data[8] = 0xE7; data[9] = 0x03; data[10] = 0; data[11] = 0;
    auto result = serializer.deserialize(data);
    EXPECT_EQ(result, nullptr);
}

TEST_F(SerializerTest, FileRoundTrip)
{
    auto original = makeTestProject();

    auto tempPath = std::filesystem::temp_directory_path() / "test_roundtable.rtp";

    // Save
    bool saved = serializer.save(*original, tempPath);
    ASSERT_TRUE(saved);
    EXPECT_TRUE(std::filesystem::exists(tempPath));

    // Load
    auto loaded = serializer.load(tempPath);
    ASSERT_NE(loaded, nullptr);

    // Verify key data survived
    EXPECT_EQ(loaded->name(), "Serializer Test");
    EXPECT_EQ(loaded->settings().resolution().width, 3840u);
    EXPECT_EQ(loaded->timeline()->trackCount(), 3u);
    EXPECT_FALSE(loaded->isModified());
    EXPECT_EQ(loaded->filePath(), tempPath);

    // Clean up
    std::filesystem::remove(tempPath);
}

TEST_F(SerializerTest, KeyframeTrackRoundTrip)
{
    auto p = Project::createNew("KF Test");

    // Add a spine clip with animated opacity
    auto spine = std::make_unique<SpineClip>();
    spine->setCharacterName("Test");
    spine->setTimelineIn(0);
    spine->setDuration(96000);
    spine->opacity().addKeyframe(0, 0.0f, InterpMode::Linear);
    spine->opacity().addKeyframe(24000, 1.0f, InterpMode::Bezier);
    spine->opacity().addKeyframe(48000, 0.5f, InterpMode::Hold);
    p->timeline()->track(0)->addClip(std::move(spine));

    auto data = serializer.serialize(*p);
    auto loaded = serializer.deserialize(data);
    ASSERT_NE(loaded, nullptr);

    auto* clip = loaded->timeline()->track(0)->clip(0);
    ASSERT_NE(clip, nullptr);

    auto& opTrack = clip->opacity();
    // 3 explicit keyframes survive the round-trip
    ASSERT_GE(opTrack.keyframeCount(), 3u);
    EXPECT_FLOAT_EQ(opTrack.keyframe(0).value, 0.0f);
    EXPECT_FLOAT_EQ(opTrack.keyframe(1).value, 1.0f);
    EXPECT_EQ(opTrack.keyframe(1).interp, InterpMode::Bezier);
    EXPECT_FLOAT_EQ(opTrack.keyframe(2).value, 0.5f);
    EXPECT_EQ(opTrack.keyframe(2).interp, InterpMode::Hold);
}
