/*
 * test_project_format.cpp — the project file format holds everything it saves.
 *
 *   - An "everything" project sets every saved field to a non-default value
 *     (every clip type, effects with their extra state, masks, the audio FX
 *     chain, transitions, markers, two sequences with their own settings, the
 *     bin, assets, the AudioSync blob, open tabs, the show). Saving it,
 *     loading that and saving again gives the same bytes.
 *   - The corpus test does the same for every real project file it finds
 *     (ROUNDTABLE_PROJECT_CORPUS, else projects/ and artifacts/ in the source
 *     tree). Those files are private and never committed; without them the
 *     test is skipped.
 *
 * Not saved, so not set here: characters (rebuilt from the asset scan on
 * load) and Title clips (they load as Graphic clips).
 */

#include <gtest/gtest.h>

#include "project/Project.h"
#include "project/ProjectSerializer.h"
#include "project/AssetDatabase.h"
#include "project/RenderStateHash.h"
#include "timeline/Timeline.h"
#include "timeline/Track.h"
#include "timeline/Transition.h"
#include "timeline/SpineClip.h"
#include "timeline/VideoClip.h"
#include "timeline/AudioClip.h"
#include "timeline/AdjustmentClip.h"
#include "timeline/ImageClip.h"
#include "timeline/SequenceClip.h"
#include "timeline/CaptionClip.h"
#include "timeline/PngPuppetClip.h"
#include "timeline/GraphicClip.h"
#include "timeline/GraphicLayer.h"
#include "timeline/TierListClip.h"
#include "timeline/OpacityMask.h"
#include "effects/Effect.h"
#include "effects/EffectStack.h"
#include "effects/ColorGrading.h"
#include "effects/LUT.h"
#include "effects/BeatEffects.h"
#include "audiofx/FxChain.h"
#include "audiofx/ParametricEQ.h"
#include "audiofx/Dynamics.h"
#include "PathUtils.h"
#include "project/BinaryIO.h"
#include "support/LegacyProjectWriterV47.h"
#include "project/schema/LeafSchemas.h"
#include "project/schema/ClipSchemas.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <set>
#include <utility>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <vector>

using namespace rt;
namespace fs = std::filesystem;

namespace {

constexpr int64_t kSec = 48000;   // ticks per second used by these fixtures

/// Keyframes with every per-keyframe field set (temporal and spatial).
void animate(KeyframeTrack<float>& track, float from, float to)
{
    track.addKeyframe(0, from, InterpMode::Bezier);
    track.addKeyframe(kSec, to, InterpMode::Linear);
    auto& k = track.keyframe(0);
    k.bezierInX = 0.2f;   k.bezierInY = 0.9f;
    k.bezierOutX = 0.4f;  k.bezierOutY = 0.1f;
    k.spatialInterp = InterpMode::ContinuousBezier;
    k.spatialInX = -12.0f; k.spatialInY = 4.0f;
    k.spatialOutX = 12.0f; k.spatialOutY = -4.0f;
}

void setEveryCommonField(Clip& clip, uint64_t linkId)
{
    clip.setLabel("Everything " + std::to_string(static_cast<int>(clip.clipType())));
    clip.setColor(0xFF123456u);
    clip.setEnabled(false);
    clip.setSourceIn(kSec / 4);
    clip.setSpeed(1.5);
    clip.setGroupId(77);
    clip.setShotName("Shot A");
    clip.setLayerId("layer-7");
    animate(clip.opacity(), 0.25f, 0.75f);
    animate(clip.positionX(), -40.0f, 40.0f);
    animate(clip.positionY(), 10.0f, -10.0f);
    animate(clip.scaleX(), 0.5f, 1.25f);
    animate(clip.scaleY(), 0.6f, 1.1f);
    animate(clip.rotation(), -15.0f, 15.0f);
    animate(clip.speedRamp(), 1.0f, 2.0f);
    clip.anchorX().setDefaultValue(32.0f);
    clip.anchorY().setDefaultValue(-18.0f);
    clip.shutterAngle().setDefaultValue(180.0f);
    clip.setBlendMode(5);
    clip.setMaintainPitch(false);
    clip.setSyncLine(12);
    clip.setTimeInterpolation(TimeInterpolation::FrameBlending);
    clip.setLinkId(linkId);
}

OpacityMask everyMaskField(MaskShape shape)
{
    OpacityMask m;
    m.shape = shape;
    m.inverted = true;
    m.name = "Mask " + std::to_string(static_cast<int>(shape));
    m.base.centerX = 0.3f;  m.base.centerY = 0.6f;
    m.base.width = 0.4f;    m.base.height = 0.2f;
    m.base.rotation = 30.0f;
    MaskVertex a; a.x = 0.1f; a.y = 0.1f; a.inTanX = -0.01f; a.inTanY = 0.02f;
    a.outTanX = 0.05f; a.outTanY = 0.03f;
    MaskVertex b; b.x = 0.9f; b.y = 0.2f;
    MaskVertex c; c.x = 0.5f; c.y = 0.9f;
    m.base.vertices = {a, b, c};
    m.pathAnimated = true;
    m.addPathKey(0, m.base);
    MaskGeometry shifted = m.base;
    shifted.centerX += 0.1f;
    for (auto& v : shifted.vertices) v.x += 0.05f;
    m.addPathKey(kSec, shifted);
    animate(m.feather, 4.0f, 24.0f);
    m.maskOpacity.setDefaultValue(0.8f);
    m.expansion.setDefaultValue(-3.0f);
    m.coordinateSpace = MaskCoordinateSpace::SourceLocal;
    m.unassigned = true;
    return m;
}

void addEveryEffect(Clip& clip, const fs::path& cubeFile)
{
    auto cc = createEffect(EffectType::ColorCorrect);
    animate(cc->param(0).track, -0.2f, 0.4f);
    cc->param(1).track.setDefaultValue(0.3f);
    cc->setEnabled(false);
    cc->addMask(everyMaskField(MaskShape::Rectangle));
    clip.effects().addEffect(std::move(cc));

    auto lut = createEffect(EffectType::LUT);
    EXPECT_TRUE(static_cast<LUT*>(lut.get())->loadCubeFile(pathToUtf8(cubeFile)));
    clip.effects().addEffect(std::move(lut));

    auto beat = createEffect(EffectType::BeatZoom);
    auto* be = static_cast<BeatReactEffect*>(beat.get());
    be->setAudioSourceId(4242);
    be->setBeatTimes({0.5f, 1.0f, 1.75f});
    clip.effects().addEffect(std::move(beat));

    auto grading = std::make_unique<ColorGrading>();
    grading->setBasicEnabled(false);
    grading->setCreativeEnabled(false);
    grading->setWheelsEnabled(false);
    grading->setCurvesEnabled(true);
    grading->setHslEnabled(true);
    grading->setVignetteEnabled(true);
    ColorGrading::HslParams hsl;
    hsl.hueCenter = 210.0f; hsl.hueWidth = 35.0f;
    hsl.satMin = 15.0f;     hsl.satMax = 90.0f;
    hsl.lumMin = 5.0f;      hsl.lumMax = 80.0f;
    hsl.hueShift = -30.0f;  hsl.satAdjust = 22.0f; hsl.lumAdjust = -8.0f;
    grading->setHslParams(hsl);
    for (int ch = 0; ch < ColorGrading::CurveCount; ++ch) {
        auto curve = grading->curveLUT(static_cast<ColorGrading::CurveChannel>(ch));
        curve[100] = 0.3f + 0.1f * static_cast<float>(ch);
        grading->setCurveLUT(static_cast<ColorGrading::CurveChannel>(ch), curve);
    }
    grading->param(ColorGrading::VignetteAmount).track.setDefaultValue(-1.5f);
    clip.effects().addEffect(std::move(grading));
}

TextStyleRun everyRunField(uint32_t start)
{
    TextStyleRun run;
    run.start = start;
    run.length = 4;
    run.fontFamily = "Georgia";
    run.fontSize = 52.0f;
    run.fontWeight = 900;
    run.italic = true;
    run.allCaps = true;
    run.smallCaps = true;
    run.tracking = 5.5f;
    run.baselineShift = 3.0f;
    run.overrideMask = 0x7FFFu;
    run.leading = 12.0f;
    run.fontStyle = "Bold Italic";
    run.kerning = 3.0f;
    run.tabWidth = 84.0f;
    run.tsume = 12.0f;
    run.fauxBold = true;
    run.fauxItalic = true;
    run.underline = true;
    run.superscript = true;
    run.subscript = false;
    auto& a = run.appearance;
    a.fillEnabled = true;        a.fillColor = 0xFF55AAFFu;
    a.strokeEnabled = true;      a.strokeColor = 0xFF010203u;
    a.strokeWidth = 4.0f;        a.strokePosition = StrokePosition::Outer;
    a.shadowEnabled = true;      a.shadowColor = 0x99000000u;
    a.shadowDistance = 6.0f;     a.shadowAngle = 30.0f;
    a.shadowSoftness = 2.0f;     a.shadowOpacity = 0.6f;
    a.backgroundEnabled = true;  a.backgroundColor = 0x66000000u;
    a.backgroundPadding = 7.0f;
    return run;
}

std::unique_ptr<VideoClip> everyVideoField(int64_t at, uint64_t linkId,
                                           const fs::path& cubeFile)
{
    auto v = std::make_unique<VideoClip>();
    v->setTimelineIn(at);
    v->setDuration(kSec);
    setEveryCommonField(*v, linkId);
    v->setMediaPath("media/never-on-disk/interview.mp4");
    v->setMediaId(9001);
    v->setSourceResolution(3840, 2160);
    v->setSourceFps(29.97);
    v->setSourceDuration(20 * kSec);
    v->setHasAudio(true);
    v->setVolume(0.7f);
    v->setCrop(1.0f, 2.0f, 3.0f, 4.0f);
    v->setCharacterName("Wells");
    v->setTalking(true);
    v->setVideoMutePath("media/never-on-disk/mute.mp4");
    v->setVideoTalkPath("media/never-on-disk/talk.mp4");
    v->setOutfit("Dress");
    v->setAnimationName("idle");
    v->setSourceRotation(90);
    v->setSourceMetadataAuthoritative(true);
    addEveryEffect(*v, cubeFile);
    v->addMask(everyMaskField(MaskShape::Ellipse));
    v->addMask(everyMaskField(MaskShape::FreeDrawBezier));
    return v;
}

std::unique_ptr<GraphicClip> everyGraphicField(int64_t at)
{
    auto g = std::make_unique<GraphicClip>();
    g->setTimelineIn(at);
    g->setDuration(kSec);

    auto text = std::make_unique<TextLayer>();
    text->setName("Headline");
    text->setVisible(false);
    text->setLocked(true);
    text->setText("BREAKING NEWS");
    text->setFontFamily("Futura");
    text->setFontSize(88.0f);
    text->setFontWeight(700);
    text->setItalic(true);
    text->setAllCaps(true);
    text->setSmallCaps(true);
    text->setAlignment(GTextAlign::Right);
    text->setVAlignment(GTextVAlign::Bottom);
    animate(text->tracking(), 1.0f, 3.0f);
    text->leading().setDefaultValue(6.0f);
    text->baselineShift().setDefaultValue(-2.0f);
    text->setBoxWidth(800.0f);
    text->setBoxHeight(200.0f);
    text->setUseParagraphBox(true);
    text->setFontStyleForAll("Condensed Bold");
    text->setKerningForAll(2.0f);
    text->setTabWidthForAll(72.0f);
    text->setTsumeForAll(8.0f);
    text->setFauxStylesForAll(true, true);
    text->setUnderlineForAll(true);
    text->setScriptForAll(false, true);
    text->setRightToLeft(true);
    text->setBackgroundForAll(true, 0x77224466u, 9.0f);
    text->setMaskWithText(true);
    text->setLinkedStyleName("Breaking News Style");
    text->setStyleRuns({everyRunField(0), everyRunField(9)});
    TextParagraphStyle paragraph;
    paragraph.start = 0;
    paragraph.length = 13;
    paragraph.alignment = GTextAlign::Justify;
    paragraph.rightToLeft = true;
    text->setParagraphStyles({paragraph});
    animate(text->transform().posX, -100.0f, 100.0f);
    text->transform().posY.setDefaultValue(20.0f);
    text->transform().scaleX.setDefaultValue(1.2f);
    text->transform().scaleY.setDefaultValue(0.9f);
    text->transform().rotation.setDefaultValue(5.0f);
    text->transform().anchorX.setDefaultValue(3.0f);
    text->transform().anchorY.setDefaultValue(-3.0f);
    text->transform().opacity.setDefaultValue(0.85f);
    text->appearance().fills = {{0xFFFF8800u, 0.9f, true}};
    text->appearance().strokes = {{0xFF112233u, 5.0f, StrokePosition::Center, 0.7f, false}};
    ShadowEntry shadow;
    shadow.color = 0xAA000000u; shadow.distance = 12.0f; shadow.angle = 45.0f;
    shadow.softness = 8.0f;     shadow.opacity = 0.5f;   shadow.enabled = true;
    text->appearance().shadows = {shadow};
    g->addLayer(std::move(text));

    auto shape = std::make_unique<ShapeLayer>();
    shape->setName("Lower Third BG");
    shape->setShapeType(ShapeType::RoundedRect);
    shape->setShapeWidth(640.0f);
    shape->setShapeHeight(160.0f);
    shape->setCornerRadius(24.0f);
    shape->setFillColor(0xFF224466u);
    g->addLayer(std::move(shape));
    return g;
}

std::unique_ptr<TierListClip> everyTierListField(int64_t at)
{
    auto t = std::make_unique<TierListClip>();
    t->setTimelineIn(at);
    t->setDuration(kSec);
    t->setTitle("Best Openings");
    t->setEntryAspect(TierEntryAspect::Custom);
    t->setCustomAspect(4.0f, 3.0f);
    t->setTopSafeMargin(0.12f);
    t->setRightSafeMargin(0.08f);
    t->setBackgroundColor(0xFF202830u);
    t->tiers() = {{"S", 0xFFFF0000u}, {"A", 0xFFFF8800u}};
    t->subbins() = {"2024", "2025"};
    t->entries() = {{11, "tier/one.png", "One", "2024"},
                    {12, "tier/two.png", "Two", "2025"}};
    TierEvent drop;
    drop.type = TierEventType::Drop;
    drop.entryId = 11; drop.tier = 1; drop.index = 0;
    drop.start = kSec / 10; drop.end = kSec / 2;
    TierEvent reorder;
    reorder.type = TierEventType::Reorder;
    reorder.tier = 1; reorder.fromIndex = 0; reorder.toIndex = 1;
    reorder.start = kSec / 2; reorder.end = kSec;
    t->events() = {drop, reorder};
    return t;
}

void setEverySettingsField(Settings& s, uint32_t w, uint32_t h, double fps)
{
    s.setResolution(w, h);
    s.setFrameRate(fps);
    s.setColorSpace(ColorSpace::Rec709);
    s.setAudioFormat(AudioFormat{96000, 24, 6});
    ExportSettings es;
    es.codec = "hevc_nvenc";
    es.quality = 18;
    es.audioBitrate = 320;
    es.outputPath = "C:/output/" + std::to_string(w) + ".mp4";
    s.setExportSettings(es);
}

/// A .cube file for the LUT effect (it keeps its path only when the file
/// loads). One per process: ctest runs these tests in parallel processes.
fs::path writeCubeFile()
{
    static const fs::path path = fs::temp_directory_path() /
        ("roundtable_format_test_" + std::to_string(std::random_device{}()) + ".cube");
    std::ofstream out(path);
    out << "LUT_3D_SIZE 2\n";
    for (int i = 0; i < 8; ++i)
        out << (i & 1) << ' ' << ((i >> 1) & 1) << ' ' << ((i >> 2) & 1) << '\n';
    return path;
}

std::unique_ptr<Project> makeEverythingProject()
{
    const fs::path cube = writeCubeFile();
    auto p = Project::createNew("Everything Project");
    setEverySettingsField(p->defaultSettings(), 2560, 1440, 50.0);

    // ── Sequence 1: every clip type ─────────────────────────────────────
    Timeline* tl = p->timeline();
    tl->setName("Main");
    tl->setPlayheadPosition(3 * kSec);
    tl->setInPoint(kSec);
    tl->setOutPoint(9 * kSec);
    Settings s1;
    setEverySettingsField(s1, 3840, 2160, 59.94);
    tl->setSettings(s1);

    Track* v1 = tl->track(0);
    v1->setSyncLocked(false);
    v1->setHeight(96.0f);
    auto video = everyVideoField(0, 5000, cube);
    const uint64_t videoId = video->id();
    v1->addClip(std::move(video));
    auto image = std::make_unique<ImageClip>();
    image->setTimelineIn(kSec);
    image->setDuration(kSec);
    image->setMediaPath("media/never-on-disk/still.png");
    image->setMediaId(31);
    image->setSourceResolution(1000, 800);
    image->setCrop(5.0f, 6.0f, 7.0f, 8.0f);
    const uint64_t imageId = image->id();
    v1->addClip(std::move(image));
    Transition t;
    t.type = TransitionType::FilmDissolve;
    t.duration = kSec / 5;
    t.offset = -kSec / 10;
    t.param1 = 0.25f;
    t.param2 = 0.75f;
    t.leftClipId = videoId;
    t.rightClipId = imageId;
    t.editPointTick = kSec;
    v1->addTransition(t);

    Track* v2 = tl->addVideoTrack("Video 2");
    v2->setMuted(true);
    v2->setSoloed(true);
    auto spine = std::make_unique<SpineClip>();
    spine->setTimelineIn(0);
    spine->setDuration(kSec);
    spine->setCharacterName("Modernia");
    spine->setOutfit("outfit_01");
    spine->setStance(CharacterStance::Aim);
    spine->setAnimationName("talk_01");
    spine->setLooping(false);
    spine->setTalking(true);
    spine->setAnimationSpeed(1.5f);
    spine->setUseGlobalTime(false);
    spine->setCrop(0.1f, 0.2f, 0.3f, 0.05f);
    v2->addClip(std::move(spine));
    v2->addClip(everyGraphicField(kSec));
    v2->addClip(everyTierListField(2 * kSec));
    auto puppet = std::make_unique<PngPuppetClip>();
    puppet->setTimelineIn(3 * kSec);
    puppet->setDuration(kSec);
    puppet->setCharacterName("Mascot");
    puppet->setVariant("winter");
    for (int f = 0; f < PngPuppetClip::FaceCount; ++f)
        puppet->setFacePath(f, "puppets/mascot/face" + std::to_string(f) + ".png");
    puppet->setTalking(true);
    puppet->setSeed(1234567u);
    puppet->setTalkSwapSeconds(0.11f);
    puppet->setBlinkIntervalSeconds(3.25f);
    puppet->setBlinkDurationSeconds(0.18f);
    puppet->setBreathAmplitude(6.5f);
    puppet->setBreathSpeed(0.3f);
    puppet->setSwayAmplitude(2.75f);
    v2->addClip(std::move(puppet));
    auto nest = std::make_unique<SequenceClip>();
    nest->setTimelineIn(4 * kSec);
    nest->setDuration(kSec);
    nest->setSequenceIndex(1);
    nest->setSequenceName("B-Roll");
    v2->addClip(std::move(nest));

    Track* v3 = tl->addVideoTrack("Video 3");
    v3->setLocked(true);
    auto adj = std::make_unique<AdjustmentClip>();
    adj->setTimelineIn(0);
    adj->setDuration(2 * kSec);
    adj->setBlendMode(3);
    adj->setAffectsSingleTrack(true);
    v3->addClip(std::move(adj), TrackMutationPolicy::BypassLock);

    Track* captions = tl->addCaptionTrack();
    auto cap = std::make_unique<CaptionClip>();
    cap->setTimelineIn(kSec);
    cap->setDuration(kSec);
    cap->setText("Hello there.");
    cap->setSpeaker("Wells");
    cap->setFontFamily("Verdana");
    cap->setFontSize(40.0f);
    cap->setTextColor(0xFFFFEE00u);
    cap->setBgColor(0x99000000u);
    cap->setPosition(CaptionPosition::Top);
    cap->setBold(false);
    cap->setOutlineColor(0xFF112233u);
    cap->setOutlineWidth(3.0f);
    cap->setShowSpeaker(true);
    cap->setStyleRuns({everyRunField(6)});
    cap->setFontStyle("Oblique");
    cap->setItalic(true);
    cap->setAllCaps(true);
    cap->setSmallCaps(true);
    cap->setUnderline(true);
    cap->setScript(true, false);
    cap->setFauxStyles(true, true);
    cap->setTracking(2.5f);
    cap->setLeading(10.0f);
    cap->setAlignment(GTextAlign::Left);
    cap->setTrackStyleName("Interview captions");
    TextParagraphStyle capParagraph;
    capParagraph.start = 0;
    capParagraph.length = 12;
    capParagraph.alignment = GTextAlign::Right;
    capParagraph.rightToLeft = true;
    cap->setParagraphStyles({capParagraph});
    cap->setConfidence(0.83f);
    captions->addClip(std::move(cap));

    Track* a1 = nullptr;
    for (size_t i = 0; i < tl->trackCount(); ++i)
        if (tl->track(i)->type() == TrackType::Audio) { a1 = tl->track(i); break; }
    EXPECT_NE(a1, nullptr);
    auto audio = std::make_unique<AudioClip>();
    audio->setTimelineIn(0);
    audio->setDuration(kSec);
    setEveryCommonField(*audio, 5000);
    audio->setMediaPath("media/never-on-disk/interview.wav");
    audio->setMediaId(9002);
    audio->setSampleRate(44100);
    audio->setChannels(1);
    audio->setSourceDuration(20 * kSec);
    animate(audio->volume(), 0.5f, 1.0f);
    audio->pan().setDefaultValue(-0.3f);
    audio->setFadeInDuration(kSec / 20);
    audio->setFadeOutDuration(kSec / 10);
    audio->setAudioStreamIndex(2);
    auto* eq = static_cast<audiofx::ParametricEQ*>(
        audio->audioFx().add(audiofx::ProcessorKind::ParametricEQ));
    eq->setHighPass(true, 90.0f, 0.8f);
    eq->setLowPass(true, 12000.0f, 0.9f);
    audiofx::ParametricEQ::Band band;
    band.enabled = true;
    band.type = audiofx::Biquad::Type::Peaking;
    band.freqHz = 3200.0f; band.gainDb = 4.5f; band.q = 1.3f;
    eq->setBand(0, band);
    eq->setOutputGainDb(-1.5f);
    auto* dyn = static_cast<audiofx::Dynamics*>(
        audio->audioFx().add(audiofx::ProcessorKind::Dynamics));
    dyn->loadVoicePreset();
    dyn->setEnabled(false);
    a1->addClip(std::move(audio));

    tl->addMarker(0, "Start", 0xFF00FF00u);
    tl->addMarker(2 * kSec, "Mid", 0xFFFF0000u);

    // ── Sequence 2: its own settings, a clip and a marker ───────────────
    Timeline* broll = p->addSequence("B-Roll");
    broll->setPlayheadPosition(kSec / 2);
    broll->setInPoint(0);
    broll->setOutPoint(5 * kSec);
    Settings s2;
    setEverySettingsField(s2, 1080, 1920, 25.0);
    broll->setSettings(s2);
    auto still = std::make_unique<ImageClip>();
    still->setTimelineIn(0);
    still->setDuration(kSec);
    still->setMediaPath("media/never-on-disk/broll.png");
    broll->track(0)->addClip(std::move(still));
    broll->addMarker(kSec, "B marker", 0xFF0000FFu);

    // ── Project-level state ─────────────────────────────────────────────
    p->setShow("Roundtable Talk");
    p->setBinItems({{1, "media/never-on-disk/interview.mp4", "Interview A", 0xFF00AAFFu},
                    {2, "media/never-on-disk/interview.mp4", "Interview B", 0xFFFFAA00u}});
    p->setBinFiles({"media/never-on-disk/interview.mp4", "media/never-on-disk/interview.mp4"});
    p->setBinFolders({{"Footage", false, {"media/never-on-disk/interview.mp4"}},
                      {"Sequences", true, {"B-Roll"}}});
    p->setSourceMarkersFor("media/never-on-disk/interview.mp4",
                           {{3 * kSec, "Laugh", 0xFFFF0000u}, {kSec, "Take 2", 0xFF00FF00u}});
    p->setAudioSyncBlob({0x41, 0x53, 0x00, 0x01, 0xFE, 0x7F});
    AssetEntry asset;
    asset.type = AssetType::Audio;
    asset.name = "voice.wav";
    asset.path = "audio/voice.wav";
    asset.absolutePath = "C:/never-on-disk/audio/voice.wav";
    asset.fileSize = 1024000;
    asset.hash = "abc123";
    p->assets()->addAsset(std::move(asset));
    p->setOpenSequenceIndices({0, 1});
    p->setActiveSequence(0);
    p->setModified(false);
    return p;
}

/// Where two byte strings first differ, for a readable failure.
std::string firstDifference(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b)
{
    const size_t n = std::min(a.size(), b.size());
    size_t i = 0;
    while (i < n && a[i] == b[i]) ++i;
    return "sizes " + std::to_string(a.size()) + " vs " + std::to_string(b.size()) +
           ", first difference at byte " + std::to_string(i);
}

std::vector<uint8_t> readFile(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

uint32_t fileVersion(const std::vector<uint8_t>& data)
{
    if (data.size() < 12) return 0;
    return static_cast<uint32_t>(data[8]) | (static_cast<uint32_t>(data[9]) << 8) |
           (static_cast<uint32_t>(data[10]) << 16) | (static_cast<uint32_t>(data[11]) << 24);
}

std::vector<fs::path> corpusFiles()
{
    std::vector<fs::path> roots;
    if (const char* env = std::getenv("ROUNDTABLE_PROJECT_CORPUS"); env && *env) {
        roots.push_back(utf8ToPath(env));
    } else {
        roots.push_back(fs::path(ROUNDTABLE_SOURCE_ROOT) / "projects");
        roots.push_back(fs::path(ROUNDTABLE_SOURCE_ROOT) / "artifacts");
    }
    std::vector<fs::path> files;
    for (const auto& root : roots) {
        std::error_code ec;
        if (!fs::is_directory(root, ec)) continue;
        for (fs::recursive_directory_iterator it(root, ec), end; it != end; it.increment(ec)) {
            if (ec) break;
            if (it->is_regular_file(ec) && it->path().extension() == ".rtp")
                files.push_back(it->path());
        }
    }
    std::sort(files.begin(), files.end());
    return files;
}

} // namespace

TEST(ProjectFormat, EverythingProjectSavesTheSameBytesAfterAReload)
{
    const ProjectSerializer serializer;
    const auto project = makeEverythingProject();
    const std::vector<uint8_t> saved = serializer.serialize(*project);

    const auto loaded = serializer.deserialize(saved);
    ASSERT_NE(loaded, nullptr);
    const std::vector<uint8_t> resaved = serializer.serialize(*loaded);
    EXPECT_EQ(saved, resaved) << firstDifference(saved, resaved);

    ASSERT_EQ(loaded->sequenceCount(), project->sequenceCount());
    for (size_t i = 0; i < project->sequenceCount(); ++i)
        EXPECT_EQ(hashTimelineState(*loaded->sequence(i)),
                  hashTimelineState(*project->sequence(i)))
            << "sequence " << i;
}

TEST(ProjectFormat, SourceMonitorClipMarkersSurviveASaveAndReload)
{
    const ProjectSerializer serializer;
    const auto loaded = serializer.deserialize(serializer.serialize(*makeEverythingProject()));
    ASSERT_NE(loaded, nullptr);

    const auto markers = loaded->sourceMarkersFor("media/never-on-disk/interview.mp4");
    ASSERT_EQ(markers.size(), 2u);
    EXPECT_EQ(markers[0].time, kSec);           // kept sorted by time
    EXPECT_EQ(markers[0].label, "Take 2");
    EXPECT_EQ(markers[0].color, 0xFF00FF00u);
    EXPECT_EQ(markers[1].time, 3 * kSec);
    EXPECT_EQ(markers[1].label, "Laugh");
    EXPECT_TRUE(loaded->sourceMarkersFor("media/never-on-disk/other.mp4").empty());
}

TEST(ProjectFormat, ClipMarkersAreKeptPerMediaFile)
{
    Project project;
    project.setSourceMarkersFor("media/a.mp4", {{2 * kSec, "b", 0xFF4444FFu}, {kSec, "a", 0xFF4444FFu}});
    project.setSourceMarkersFor("media/b.mp4", {{kSec, "other", 0xFF4444FFu}});
    ASSERT_EQ(project.sourceMarkers().size(), 2u);

    // The same file spelled differently finds the same markers.
    const auto a = project.sourceMarkersFor("media/./a.mp4");
    ASSERT_EQ(a.size(), 2u);
    EXPECT_EQ(a[0].label, "a");

    // Replacing keeps one entry per file; an empty list removes the entry.
    project.setSourceMarkersFor("media/a.mp4", {{kSec, "only", 0xFF4444FFu}});
    EXPECT_EQ(project.sourceMarkersFor("media/a.mp4").size(), 1u);
    project.setSourceMarkersFor("media/a.mp4", {});
    EXPECT_TRUE(project.sourceMarkersFor("media/a.mp4").empty());
    ASSERT_EQ(project.sourceMarkers().size(), 1u);
    EXPECT_EQ(project.sourceMarkers()[0].path, std::filesystem::path("media/b.mp4"));
}

TEST(ProjectFormat, EverythingProjectUsesEveryClipType)
{
    // The round trip above only proves what the project contains; make sure
    // it contains a clip of every type that is saved as itself.
    const auto project = makeEverythingProject();
    std::map<ClipType, int> seen;
    for (size_t si = 0; si < project->sequenceCount(); ++si) {
        const Timeline* tl = project->sequence(si);
        for (size_t ti = 0; ti < tl->trackCount(); ++ti)
            for (size_t ci = 0; ci < tl->track(ti)->clipCount(); ++ci)
                ++seen[tl->track(ti)->clip(ci)->clipType()];
    }
    for (ClipType type : {ClipType::Spine, ClipType::Video, ClipType::Audio,
                          ClipType::Adjustment, ClipType::Image, ClipType::Sequence,
                          ClipType::Caption, ClipType::PngPuppet, ClipType::Graphic,
                          ClipType::TierList})
        EXPECT_GT(seen[type], 0) << "clip type " << static_cast<int>(type);
}

TEST(ProjectFormat, CorpusProjectsSaveTheSameBytesAfterAReload)
{
    const std::vector<fs::path> files = corpusFiles();
    if (files.empty())
        GTEST_SKIP() << "no project corpus (set ROUNDTABLE_PROJECT_CORPUS)";

    const ProjectSerializer serializer;
    std::map<uint32_t, int> versions;
    int failures = 0;
    for (const fs::path& file : files) {
        SCOPED_TRACE(pathToUtf8(file));
        const std::vector<uint8_t> original = readFile(file);
        ++versions[fileVersion(original)];

        const auto first = serializer.deserialize(original);
        ASSERT_NE(first, nullptr);
        const std::vector<uint8_t> saved = serializer.serialize(*first);
        const auto second = serializer.deserialize(saved);
        ASSERT_NE(second, nullptr);
        const std::vector<uint8_t> resaved = serializer.serialize(*second);
        if (saved != resaved) {
            ++failures;
            ADD_FAILURE() << firstDifference(saved, resaved);
            // Keep both saves for a look at what moved.
            const fs::path dump = fs::temp_directory_path() / "roundtable_corpus_diff";
            fs::create_directories(dump);
            std::ofstream(dump / "first.rtp", std::ios::binary)
                .write(reinterpret_cast<const char*>(saved.data()),
                       static_cast<std::streamsize>(saved.size()));
            std::ofstream(dump / "second.rtp", std::ios::binary)
                .write(reinterpret_cast<const char*>(resaved.data()),
                       static_cast<std::streamsize>(resaved.size()));
        }
    }

    std::string summary;
    for (const auto& [version, count] : versions)
        summary += " v" + std::to_string(version) + "x" + std::to_string(count);
    std::printf("[corpus] %zu projects:%s, %d not stable\n", files.size(),
                summary.c_str(), failures);
}

// ═════════════════════════════════════════════════════════════════════════
//  Leaf schemas (v48 records): each type survives a record round trip, and
//  re-encoding what was read gives the same bytes, so the reader takes in
//  every field the writer emits.
// ═════════════════════════════════════════════════════════════════════════

namespace {

template <class V>
std::vector<uint8_t> recordBytes(const V& value)
{
    record::RecordWriter w;
    record::SchemaOf<V>::get().write(w, value);
    return w.take();
}

template <class V>
void readRecordInto(const std::vector<uint8_t>& bytes, V& out)
{
    record::RecordReader r(bytes);
    EXPECT_TRUE(record::SchemaOf<V>::get().read(r, out));
}

std::vector<uint8_t> legacyTrackBytes(const KeyframeTrack<float>& track)
{
    BinaryWriter w;
    legacy_v47::writeKeyframeTrack(w, track);
    return w.data();
}

std::vector<uint8_t> effectBytes(const Effect& effect)
{
    record::RecordWriter w;
    record::writeEffect(w, effect);
    return w.take();
}

} // namespace

TEST(LeafSchemas, KeyframeTracksKeepEveryKeyframeField)
{
    KeyframeTrack<float> animated(0.5f);
    animate(animated, -2.0f, 7.0f);
    KeyframeTrack<float> still(3.25f);
    for (const KeyframeTrack<float>* track : {&animated, &still}) {
        const auto bytes = recordBytes(*track);
        KeyframeTrack<float> back(99.0f);
        back.addKeyframe(5, 5.0f);   // whatever was there is replaced
        readRecordInto(bytes, back);
        EXPECT_EQ(legacyTrackBytes(back), legacyTrackBytes(*track));
        EXPECT_EQ(back.defaultValue(), track->defaultValue());
        EXPECT_EQ(recordBytes(back), bytes);
    }
}

TEST(LeafSchemas, MasksKeepEveryFieldAndTheirId)
{
    for (MaskShape shape : {MaskShape::Ellipse, MaskShape::Rectangle, MaskShape::FreeDrawBezier}) {
        const OpacityMask mask = everyMaskField(shape);
        const auto bytes = recordBytes(mask);
        OpacityMask back;
        readRecordInto(bytes, back);
        EXPECT_EQ(back.maskId, mask.maskId);
        EXPECT_EQ(back.shape, shape);
        EXPECT_EQ(back.name, mask.name);
        EXPECT_TRUE(back.unassigned);
        ASSERT_EQ(back.pathKeys.size(), 2u);
        EXPECT_EQ(back.pathKeys[1].geometry.vertices.size(), 3u);
        EXPECT_EQ(legacyTrackBytes(back.feather), legacyTrackBytes(mask.feather));
        EXPECT_EQ(recordBytes(back), bytes);
    }
    // A reloaded id is reserved: masks made afterwards never reuse it.
    OpacityMask saved = everyMaskField(MaskShape::Ellipse);
    saved.maskId = 1u << 30;
    OpacityMask back;
    readRecordInto(recordBytes(saved), back);
    EXPECT_GT(OpacityMask{}.maskId, saved.maskId);
}

TEST(LeafSchemas, TextRunsAndParagraphsRoundTrip)
{
    const TextStyleRun run = everyRunField(3);
    TextStyleRun runBack;
    readRecordInto(recordBytes(run), runBack);
    EXPECT_EQ(runBack, run);

    TextParagraphStyle paragraph;
    paragraph.start = 2;
    paragraph.length = 9;
    paragraph.alignment = GTextAlign::Justify;
    paragraph.rightToLeft = true;
    TextParagraphStyle paragraphBack;
    readRecordInto(recordBytes(paragraph), paragraphBack);
    EXPECT_EQ(paragraphBack, paragraph);
}

TEST(LeafSchemas, SettingsTransitionsAndMarkersRoundTrip)
{
    Settings settings;
    setEverySettingsField(settings, 1080, 1920, 23.976);
    Settings settingsBack;
    readRecordInto(recordBytes(settings), settingsBack);
    EXPECT_EQ(settingsBack, settings);
    EXPECT_EQ(settingsBack.exportSettings().outputPath, settings.exportSettings().outputPath);

    Transition t;
    t.type = TransitionType::WipeUp;
    t.duration = 4800; t.offset = -2400;
    t.leftClipId = 3; t.rightClipId = 4; t.editPointTick = 96000;
    t.param1 = 0.3f; t.param2 = 0.6f;
    Transition tb;
    readRecordInto(recordBytes(t), tb);
    EXPECT_EQ(recordBytes(tb), recordBytes(t));
    EXPECT_EQ(tb.editPointTick, 96000);
    EXPECT_EQ(tb.type, TransitionType::WipeUp);

    Marker m{12345, "Cue", 0xFF00FF00u};
    Marker mb;
    readRecordInto(recordBytes(m), mb);
    EXPECT_EQ(mb.time, m.time);
    EXPECT_EQ(mb.label, m.label);
    EXPECT_EQ(mb.color, m.color);
}

TEST(LeafSchemas, EveryEffectKeepsItsParametersAndExtraState)
{
    VideoClip host;
    addEveryEffect(host, writeCubeFile());
    ASSERT_GE(host.effects().effectCount(), 4u);
    for (size_t i = 0; i < host.effects().effectCount(); ++i) {
        const Effect& fx = host.effects().effect(i);
        SCOPED_TRACE(fx.name());
        const auto bytes = effectBytes(fx);
        record::RecordReader r(bytes);
        const auto back = record::readEffect(r);
        ASSERT_NE(back, nullptr);
        EXPECT_TRUE(r.ok());
        EXPECT_EQ(back->effectType(), fx.effectType());
        EXPECT_EQ(back->isEnabled(), fx.isEnabled());
        ASSERT_EQ(back->paramCount(), fx.paramCount());
        for (size_t p = 0; p < fx.paramCount(); ++p)
            EXPECT_EQ(legacyTrackBytes(back->param(p).track), legacyTrackBytes(fx.param(p).track));
        EXPECT_EQ(back->maskCount(), fx.maskCount());
        EXPECT_EQ(effectBytes(*back), bytes);
    }
}

TEST(LeafSchemas, AnEffectOfAnUnknownTypeIsLeftOut)
{
    record::RecordWriter w;
    w.writeVarint(1, 250);         // a type from a newer build
    w.writeBool(2, true);
    record::RecordReader r(w.data());
    EXPECT_EQ(record::readEffect(r), nullptr);
    EXPECT_TRUE(r.ok());
}

TEST(LeafSchemas, AFieldForAnotherEffectTypeIsSkipped)
{
    // A damaged type byte loaded a LUT's record as a plain effect, and the
    // LUT path setter then wrote LUT data into it (memory damage, then a
    // crash or a hang). Fields for another type must be skipped on read.
    LUT lut;
    ASSERT_TRUE(lut.loadCubeFile(pathToUtf8(writeCubeFile())));
    std::vector<uint8_t> bytes = effectBytes(lut);
    ASSERT_GE(bytes.size(), 2u);
    ASSERT_EQ(bytes[0], 0x08);   // field 1 (type), varint
    ASSERT_EQ(bytes[1], static_cast<uint8_t>(EffectType::LUT));
    bytes[1] = static_cast<uint8_t>(EffectType::FillRightWithLeft);

    record::RecordReader r(bytes);
    const auto back = record::readEffect(r);
    ASSERT_NE(back, nullptr);
    EXPECT_EQ(back->effectType(), EffectType::FillRightWithLeft);
    EXPECT_TRUE(r.ok());
}

TEST(LeafSchemas, TheAudioFxChainKeepsEveryProcessor)
{
    const auto project = makeEverythingProject();
    const AudioClip* audio = nullptr;
    const Timeline* tl = project->timeline();
    for (size_t ti = 0; ti < tl->trackCount() && !audio; ++ti)
        for (size_t ci = 0; ci < tl->track(ti)->clipCount(); ++ci)
            if (tl->track(ti)->clip(ci)->clipType() == ClipType::Audio)
                audio = static_cast<const AudioClip*>(tl->track(ti)->clip(ci));
    ASSERT_NE(audio, nullptr);

    record::RecordWriter w;
    record::writeFxChain(w, 1, audio->audioFx());
    record::RecordReader r(w.data());
    ASSERT_TRUE(r.next());
    audiofx::FxChain back;
    back.add(audiofx::ProcessorKind::Dynamics);   // replaced, not appended to
    ASSERT_TRUE(record::readFxChain(r, back));
    ASSERT_EQ(back.size(), 2u);
    const auto* eq = dynamic_cast<const audiofx::ParametricEQ*>(&back.at(0));
    ASSERT_NE(eq, nullptr);
    EXPECT_TRUE(eq->lowPassOn());
    EXPECT_FLOAT_EQ(eq->band(0).gainDb, 4.5f);
    const auto* dyn = dynamic_cast<const audiofx::Dynamics*>(&back.at(1));
    ASSERT_NE(dyn, nullptr);
    EXPECT_FALSE(dyn->isEnabled());
    EXPECT_FLOAT_EQ(dyn->compressor().makeupDb, 4.0f);

    record::RecordWriter again;
    record::writeFxChain(again, 1, back);
    EXPECT_EQ(again.data(), w.data());
}

// ═════════════════════════════════════════════════════════════════════════
//  Clip schemas: every clip, taken through a v48 record and back, writes the
//  same legacy (v47) bytes as before. The legacy writer is independent of
//  the schemas, so this proves no saved field was dropped or changed.
// ═════════════════════════════════════════════════════════════════════════

namespace {

std::vector<uint8_t> legacyClipBytes(const Clip& clip)
{
    BinaryWriter w;
    legacy_v47::writeClip(w, clip);
    return w.data();
}

/// Every clip of `project` through a record; returns how many went through.
size_t expectClipsSurviveRecords(const Project& project)
{
    size_t count = 0;
    for (size_t si = 0; si < project.sequenceCount(); ++si) {
        const Timeline* tl = project.sequence(si);
        for (size_t ti = 0; ti < tl->trackCount(); ++ti) {
            for (size_t ci = 0; ci < tl->track(ti)->clipCount(); ++ci) {
                const Clip& clip = *tl->track(ti)->clip(ci);
                const std::vector<uint8_t> bytes = record::clipBytes(clip);
                record::RecordReader r(bytes);
                const auto back = record::readClip(r);
                if (!back) {
                    ADD_FAILURE() << "clip " << clip.id() << " (type "
                                  << static_cast<int>(clip.clipType()) << ") did not load";
                    continue;
                }
                EXPECT_TRUE(r.ok());
                const auto expected = legacyClipBytes(clip);
                const auto actual = legacyClipBytes(*back);
                EXPECT_TRUE(actual == expected)
                    << "clip " << clip.id() << " type " << static_cast<int>(clip.clipType())
                    << ": " << firstDifference(expected, actual);
                EXPECT_TRUE(record::clipBytes(*back) == bytes) << "clip " << clip.id();
                ++count;
            }
        }
    }
    return count;
}

} // namespace

TEST(ClipSchemas, EveryClipOfTheEverythingProjectSurvivesARecord)
{
    const auto project = makeEverythingProject();
    EXPECT_EQ(expectClipsSurviveRecords(*project), 11u);
}

TEST(ClipSchemas, EveryClipOfTheCorpusSurvivesARecord)
{
    const std::vector<fs::path> files = corpusFiles();
    if (files.empty())
        GTEST_SKIP() << "no project corpus (set ROUNDTABLE_PROJECT_CORPUS)";
    const ProjectSerializer serializer;
    size_t clips = 0;
    for (const fs::path& file : files) {
        SCOPED_TRACE(pathToUtf8(file));
        const auto project = serializer.deserialize(readFile(file));
        ASSERT_NE(project, nullptr);
        clips += expectClipsSurviveRecords(*project);
        if (HasFailure()) break;   // one file's report is enough
    }
    std::printf("[corpus] %zu clips through records\n", clips);
}

TEST(ClipSchemas, AClipOfAnUnknownTypeIsLeftOut)
{
    record::RecordWriter w;
    w.writeVarint(1, 200);   // a clip type from a newer build
    w.writeVarint(2, 77);
    record::RecordReader r(w.data());
    EXPECT_EQ(record::readClip(r), nullptr);
    EXPECT_TRUE(r.ok());

    record::RecordWriter untyped;
    untyped.writeString(3, "no type field");
    record::RecordReader u(untyped.data());
    EXPECT_EQ(record::readClip(u), nullptr);
}

TEST(ClipSchemas, AReloadedClipIdIsNotHandedOutAgain)
{
    VideoClip clip;
    clip.setId(uint64_t{1} << 40);
    const std::vector<uint8_t> bytes = record::clipBytes(clip);
    record::RecordReader r(bytes);
    const auto back = record::readClip(r);
    ASSERT_NE(back, nullptr);
    EXPECT_EQ(back->id(), clip.id());
    EXPECT_GT(VideoClip{}.id(), clip.id());
}

// ═════════════════════════════════════════════════════════════════════════
//  The v48 switch: whatever a project held, a v48 save and reload holds the
//  same. Checked through the frozen v47 writer (independent of the schemas):
//  its bytes for the project before and after the v48 trip must match.
// ═════════════════════════════════════════════════════════════════════════

namespace {

struct SweepTotals
{
    size_t projects = 0;
    size_t v47Bytes = 0;
    size_t v48Bytes = 0;
    double v47LoadMs = 0.0;
    double v48LoadMs = 0.0;
};

double millisecondsSince(std::chrono::steady_clock::time_point start)
{
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
        .count();
}

void expectV48KeepsEverything(const Project& project, SweepTotals& totals)
{
    const ProjectSerializer serializer;
    const std::vector<uint8_t> before = legacy_v47::serialize(project);
    const std::vector<uint8_t> v48 = serializer.serialize(project);
    ASSERT_EQ(fileVersion(v48), ProjectSerializer::FORMAT_VERSION);

    auto start = std::chrono::steady_clock::now();
    const auto reloaded = serializer.deserialize(v48);
    totals.v48LoadMs += millisecondsSince(start);
    ASSERT_NE(reloaded, nullptr);

    const std::vector<uint8_t> after = legacy_v47::serialize(*reloaded);
    EXPECT_TRUE(after == before) << firstDifference(before, after);
    ASSERT_EQ(reloaded->sequenceCount(), project.sequenceCount());
    for (size_t i = 0; i < project.sequenceCount(); ++i)
        EXPECT_EQ(hashTimelineState(*reloaded->sequence(i)),
                  hashTimelineState(*project.sequence(i)))
            << "sequence " << i;
    EXPECT_TRUE(serializer.serialize(*reloaded) == v48) << "v48 save is not stable";

    start = std::chrono::steady_clock::now();
    (void)serializer.deserialize(before);
    totals.v47LoadMs += millisecondsSince(start);
    totals.v47Bytes += before.size();
    totals.v48Bytes += v48.size();
    ++totals.projects;
}

} // namespace

TEST(ProjectFormat, TheEverythingProjectKeepsEverythingThroughV48)
{
    SweepTotals totals;
    expectV48KeepsEverything(*makeEverythingProject(), totals);
}

TEST(ProjectFormat, CorpusProjectsKeepEverythingThroughV48)
{
    const std::vector<fs::path> files = corpusFiles();
    if (files.empty())
        GTEST_SKIP() << "no project corpus (set ROUNDTABLE_PROJECT_CORPUS)";
    const ProjectSerializer serializer;
    SweepTotals totals;
    for (const fs::path& file : files) {
        SCOPED_TRACE(pathToUtf8(file));
        const auto project = serializer.deserialize(readFile(file));
        ASSERT_NE(project, nullptr);
        expectV48KeepsEverything(*project, totals);
        if (HasFailure()) break;   // one file's report is enough
    }
    std::printf("[corpus] %zu projects through v48: v47 %.1f MB loaded in %.0f ms, "
                "v48 %.1f MB loaded in %.0f ms\n",
                totals.projects, totals.v47Bytes / 1048576.0, totals.v47LoadMs,
                totals.v48Bytes / 1048576.0, totals.v48LoadMs);
}

TEST(ProjectFormat, SavingTheEverythingProjectWritesEveryField)
{
    // A schema field the everything project never writes is untested: add
    // data for it to makeEverythingProject().
    struct Log final : record::FieldLog
    {
        std::set<std::pair<std::string, uint32_t>> seen;
        void wrote(const char* schema, uint32_t id) override { seen.insert({schema, id}); }
    } log;
    const ProjectSerializer serializer;
    (void)serializer.serialize(*makeEverythingProject(), &log);

    std::string missing;
    size_t fields = 0;
    for (const record::SchemaInfo& schema : record::registeredSchemas()) {
        for (uint32_t id : schema.fieldIds) {
            ++fields;
            if (!log.seen.count({schema.name, id}))
                missing += std::string(" ") + schema.name + "." + std::to_string(id);
        }
    }
    EXPECT_TRUE(missing.empty()) << "never written:" << missing;
    EXPECT_GT(fields, 300u);
}

TEST(ProjectFormat, CorruptFilesFailToLoadInsteadOfThrowing)
{
    // Random single-byte damage, v48 and v47 alike: the loader returns a
    // project or nothing, never throws (a corrupt path string used to throw
    // out of the path conversion).
    const ProjectSerializer serializer;
    const auto project = makeEverythingProject();
    for (const std::vector<uint8_t>& bytes :
         {serializer.serialize(*project), legacy_v47::serialize(*project)}) {
        SCOPED_TRACE(fileVersion(bytes));
        uint32_t seed = 20261018u;
        auto nextRandom = [&seed] { seed = seed * 1664525u + 1013904223u; return seed >> 8; };
        int rejected = 0;
        for (int i = 0; i < 3000; ++i) {
            std::vector<uint8_t> broken = bytes;
            const size_t pos = 12 + nextRandom() % (broken.size() - 12);
            broken[pos] ^= static_cast<uint8_t>(1 + nextRandom() % 255);
            std::unique_ptr<Project> loaded;
            EXPECT_NO_THROW(loaded = serializer.deserialize(broken)) << "byte " << pos;
            if (!loaded) ++rejected;
        }
        EXPECT_GT(rejected, 0);
    }
}

TEST(ProjectFormat, ASectionFromANewerBuildIsSkipped)
{
    const ProjectSerializer serializer;
    const auto project = makeEverythingProject();
    std::vector<uint8_t> bytes = serializer.serialize(*project);
    const std::vector<uint8_t> extra = {0x7F, 0x00, 0x00, 0x00, 3, 0, 0, 0, 0x08, 0x2A, 0x00};
    bytes.insert(bytes.end(), extra.begin(), extra.end());   // tag 0x7F, 3 bytes
    bytes[12] = static_cast<uint8_t>(bytes[12] + 1);          // one more section
    const auto loaded = serializer.deserialize(bytes);
    ASSERT_NE(loaded, nullptr);
    EXPECT_TRUE(legacy_v47::serialize(*loaded) == legacy_v47::serialize(*project));
}
