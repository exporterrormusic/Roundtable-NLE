/*
 * ClipSerializationLegacy.cpp — reading clips from positional project files
 * (format v1–47). FROZEN: v48+ files use the record schemas
 * (schema/ClipSchemas.h); nothing new is ever added here. The v47 writer
 * lives on as a test oracle (tests/support/LegacyProjectWriterV47).
 */

#include "project/ClipSerializationLegacy.h"
#include "project/VideoPathResolve.h"
#include "timeline/Clip.h"
#include "timeline/SpineClip.h"
#include "timeline/VideoClip.h"
#include "timeline/AudioClip.h"
#include "timeline/TitleClip.h"
#include "timeline/GraphicClip.h"
#include "timeline/AdjustmentClip.h"
#include "timeline/ImageClip.h"
#include "timeline/SequenceClip.h"
#include "timeline/CaptionClip.h"
#include "timeline/PngPuppetClip.h"
#include "timeline/TierListClip.h"
#include "timeline/KeyframeTrack.h"
#include "effects/Effect.h"
#include "effects/EffectStack.h"
#include "effects/ColorGrading.h"
#include "effects/LUT.h"
#include "effects/BeatEffects.h"
#include "timeline/OpacityMask.h"
#include "audiofx/FxChain.h"
#include "audiofx/ParametricEQ.h"
#include "audiofx/Dynamics.h"
#include "PathUtils.h"

#include <spdlog/spdlog.h>
#include <algorithm>
#include <cctype>
#include <filesystem>

namespace rt {


void readKeyframeTrack(BinaryReader& r, KeyframeTrack<float>& track, uint32_t version)
{
    const size_t bytesPerKeyframe = version >= 15 ? 46u : 29u;
    const uint32_t count = r.readCount(100000, bytesPerKeyframe);

    // v8+: count=0 means static track — read default value
    if (count == 0) {
        if (version >= 8) {
            track.setDefaultValue(r.readF32());
        }
        // For versions < 8, count=0 shouldn't appear, but handle gracefully
        return;
    }

    while (track.keyframeCount() > 0)
        track.removeKeyframe(0);

    for (uint32_t i = 0; i < count && r.ok(); ++i)
    {
        int64_t time = r.readI64();
        float value  = r.readF32();
        auto interp  = static_cast<InterpMode>(r.readU8());
        float biX    = r.readF32();
        float biY    = r.readF32();
        float boX    = r.readF32();
        float boY    = r.readF32();

        track.addKeyframe(time, value, interp);
        auto& kf = track.keyframe(track.keyframeCount() - 1);
        if (kf.time == time)
        {
            kf.bezierInX  = biX;
            kf.bezierInY  = biY;
            kf.bezierOutX = boX;
            kf.bezierOutY = boY;
        }
        // v15+: spatial interpolation + 2D motion-path handles.
        if (version >= 15)
        {
            auto spInterp = static_cast<InterpMode>(r.readU8());
            float spInX   = r.readF32();
            float spInY   = r.readF32();
            float spOutX  = r.readF32();
            float spOutY  = r.readF32();
            if (kf.time == time)
            {
                kf.spatialInterp = spInterp;
                kf.spatialInX    = spInX;
                kf.spatialInY    = spInY;
                kf.spatialOutX   = spOutX;
                kf.spatialOutY   = spOutY;
            }
        }
    }

    // Backward compat (v7 and older): old format stored static defaults as a
    // single keyframe at time 0.  Convert to the new representation.
    if (version < 8 && track.keyframeCount() == 1 && track.keyframe(0).time == 0) {
        track.setDefaultValue(track.keyframe(0).value);
        track.removeKeyframe(0);
    }
}

// ── Mask serialization ──────────────────────────────────────────────────────
// v30 record: shape/inverted/name + base geometry + Mask Path keyframes +
// keyframeable feather/opacity/expansion tracks.  Legacy v11–29 records
// (plain scalars) are converted on read in readMask().

static void readMaskGeometry(BinaryReader& r, MaskGeometry& g)
{
    g.centerX  = r.readF32();
    g.centerY  = r.readF32();
    g.width    = r.readF32();
    g.height   = r.readF32();
    g.rotation = r.readF32();
    const uint32_t vertCount = r.readCount(100000, 24);
    g.vertices.resize(vertCount);
    for (uint32_t vi = 0; vi < vertCount && r.ok(); ++vi) {
        g.vertices[vi].x       = r.readF32();
        g.vertices[vi].y       = r.readF32();
        g.vertices[vi].inTanX  = r.readF32();
        g.vertices[vi].inTanY  = r.readF32();
        g.vertices[vi].outTanX = r.readF32();
        g.vertices[vi].outTanY = r.readF32();
    }
}

static OpacityMask readMask(BinaryReader& r, uint32_t version)
{
    OpacityMask m;
    if (version >= 30) {
        m.shape    = static_cast<MaskShape>(r.readU8());
        m.inverted = r.readU8() != 0;
        m.name     = r.readString();
        readMaskGeometry(r, m.base);
        m.pathAnimated = r.readU8() != 0;
        const uint32_t keyCount = r.readCount(100000, 32);
        m.pathKeys.resize(keyCount);
        for (uint32_t ki = 0; ki < keyCount && r.ok(); ++ki) {
            m.pathKeys[ki].time = r.readI64();
            readMaskGeometry(r, m.pathKeys[ki].geometry);
        }
        readKeyframeTrack(r, m.feather, version);
        readKeyframeTrack(r, m.maskOpacity, version);
        readKeyframeTrack(r, m.expansion, version);
        if (version >= 33) {
            const uint8_t rawSpace = r.readU8();
            m.coordinateSpace = rawSpace
                    <= static_cast<uint8_t>(MaskCoordinateSpace::LegacySequenceFrame)
                ? static_cast<MaskCoordinateSpace>(rawSpace)
                : MaskCoordinateSpace::SourceLocal;
        } else {
            m.coordinateSpace = MaskCoordinateSpace::LegacySequenceFrame;
        }
        if (version >= 33) {
            m.maskId = r.readU64();
            if (m.maskId == 0) m.maskId = allocateOpacityMaskId();
            reserveOpacityMaskId(m.maskId);
        }
        if (version >= 47)
            m.unassigned = r.readU8() != 0;
        return m;
    }

    // Legacy record (v11–29): plain scalars → static geometry + track defaults.
    m.shape         = static_cast<MaskShape>(r.readU8());
    m.base.centerX  = r.readF32();
    m.base.centerY  = r.readF32();
    m.base.width    = r.readF32();
    m.base.height   = r.readF32();
    m.base.rotation = r.readF32();
    m.feather.setDefaultValue(r.readF32());
    m.expansion.setDefaultValue(r.readF32());
    m.maskOpacity.setDefaultValue(r.readF32());
    m.inverted      = r.readU8() != 0;
    m.name          = r.readString();
    const uint32_t vertCount = r.readCount(100000, 24);
    m.base.vertices.resize(vertCount);
    for (uint32_t vi = 0; vi < vertCount && r.ok(); ++vi) {
        m.base.vertices[vi].x       = r.readF32();
        m.base.vertices[vi].y       = r.readF32();
        m.base.vertices[vi].inTanX  = r.readF32();
        m.base.vertices[vi].inTanY  = r.readF32();
        m.base.vertices[vi].outTanX = r.readF32();
        m.base.vertices[vi].outTanY = r.readF32();
    }
    m.coordinateSpace = MaskCoordinateSpace::LegacySequenceFrame;
    return m;
}

// ── Audio FX chain (v22+) ───────────────────────────────────────────────────
// Each processor is length-prefixed so unknown/older kinds re-sync cleanly.

static void readAudioFx(BinaryReader& r, audiofx::FxChain& chain)
{
    const uint32_t count = r.readCount(1024, 6);
    for (uint32_t i = 0; i < count && r.ok(); ++i) {
        const uint8_t kind    = r.readU8();
        const bool    enabled = r.readU8() != 0;
        const uint32_t payloadLen = r.readU32();
        BinaryReader payload = r.readSubReader(payloadLen);

        if (kind == static_cast<uint8_t>(audiofx::ProcessorKind::ParametricEQ)) {
            auto* eq = static_cast<audiofx::ParametricEQ*>(
                chain.add(audiofx::ProcessorKind::ParametricEQ));
            eq->setEnabled(enabled);
            const bool  hOn = payload.readU8() != 0;
            const float hF = payload.readF32(), hQ = payload.readF32();
            eq->setHighPass(hOn, hF, hQ);
            const bool  lOn = payload.readU8() != 0;
            const float lF = payload.readF32(), lQ = payload.readF32();
            eq->setLowPass(lOn, lF, lQ);
            const uint32_t bands = payload.readCount(64, 14);
            for (uint32_t b = 0; b < bands && payload.ok(); ++b) {
                audiofx::ParametricEQ::Band bd;
                bd.enabled = payload.readU8() != 0;
                bd.type    = static_cast<audiofx::Biquad::Type>(payload.readU8());
                bd.freqHz  = payload.readF32(); bd.gainDb = payload.readF32(); bd.q = payload.readF32();
                if (static_cast<int>(b) < eq->bandCount()) eq->setBand(static_cast<int>(b), bd);
            }
            eq->setOutputGainDb(payload.readF32());
        } else if (kind == static_cast<uint8_t>(audiofx::ProcessorKind::Dynamics)) {
            auto* dyn = static_cast<audiofx::Dynamics*>(
                chain.add(audiofx::ProcessorKind::Dynamics));
            dyn->setEnabled(enabled);
            audiofx::Dynamics::GateSettings g;
            g.enabled = payload.readU8() != 0;
            g.thresholdDb = payload.readF32(); g.ratio = payload.readF32(); g.rangeDb = payload.readF32();
            g.attackMs = payload.readF32();    g.releaseMs = payload.readF32();
            dyn->setGate(g);
            audiofx::Dynamics::CompSettings c;
            c.enabled = payload.readU8() != 0;
            c.thresholdDb = payload.readF32(); c.ratio = payload.readF32(); c.kneeDb = payload.readF32();
            c.makeupDb = payload.readF32();    c.attackMs = payload.readF32(); c.releaseMs = payload.readF32();
            dyn->setCompressor(c);
            audiofx::Dynamics::LimiterSettings l;
            l.enabled = payload.readU8() != 0;
            l.ceilingDb = payload.readF32(); l.releaseMs = payload.readF32();
            dyn->setLimiter(l);
        }
        if (!payload.ok()) {
            r.invalidate();
            return;
        }
        // Unknown kinds and trailing future fields remain safely bounded by
        // the payload sub-reader and need no special re-synchronization.
    }
}

// ═════════════════════════════════════════════════════════════════════════
//  Per-type serializer registry
//
//  Each clip type contributes a factory and a type-specific field reader,
//  registered in ONE table below. readClip handles the fields common to all
//  clips and dispatches the type-specific payload through the registry.
// ═════════════════════════════════════════════════════════════════════════

namespace {

struct TypeSerializer
{
    std::unique_ptr<Clip> (*make)();
    void (*readFields)(BinaryReader& r, Clip& clip, uint32_t version);
};

template <typename T>
std::unique_ptr<Clip> makeClip() { return std::make_unique<T>(); }

// ── Spine ──────────────────────────────────────────────────────────────────

void readSpineFields(BinaryReader& r, Clip& clip, uint32_t)
{
    auto* sc = static_cast<SpineClip*>(&clip);
    sc->setCharacterName(r.readString());
    sc->setOutfit(r.readString());
    sc->setStance(static_cast<CharacterStance>(r.readU8()));
    sc->setAnimationName(r.readString());
    sc->setLooping(r.readU8() != 0);
    sc->setTalking(r.readU8() != 0);
    sc->setAnimationSpeed(r.readF32());
    sc->setUseGlobalTime(r.readU8() != 0);
    float cl = r.readF32(), cr = r.readF32(), ct = r.readF32(), cb = r.readF32();
    sc->setCrop(cl, cr, ct, cb);
}

// ── Video ──────────────────────────────────────────────────────────────────

void readVideoFields(BinaryReader& r, Clip& clip, uint32_t version)
{
    auto* vc = static_cast<VideoClip*>(&clip);
    vc->setMediaPath(resolveVideoPath(r.readString()));
    vc->setMediaId(r.readU64());
    uint32_t sw = r.readU32(), sh = r.readU32();
    vc->setSourceResolution(sw, sh);
    vc->setSourceFps(r.readF64());
    vc->setSourceDuration(r.readI64());
    vc->setHasAudio(r.readU8() != 0);
    vc->setVolume(r.readF32());
    if (version >= 3) {
        float cl = r.readF32(), cr = r.readF32(), ct = r.readF32(), cb = r.readF32();
        vc->setCrop(cl, cr, ct, cb);
    }
    if (version >= 9) {
        vc->setCharacterName(r.readString());
        vc->setTalking(r.readU8() != 0);
        vc->setVideoMutePath(resolveVideoPath(r.readString()));
        vc->setVideoTalkPath(resolveVideoPath(r.readString()));
    }
    if (version >= 12) {
        vc->setOutfit(r.readString());
        vc->setAnimationName(r.readString());
    }
    if (version >= 34) {
        vc->setSourceRotation(static_cast<int>(r.readU32()));
        vc->setSourceMetadataAuthoritative(r.readU8() != 0);
    }
}

// ── Audio ──────────────────────────────────────────────────────────────────

void readAudioFields(BinaryReader& r, Clip& clip, uint32_t version)
{
    auto* ac = static_cast<AudioClip*>(&clip);
    ac->setMediaPath(r.readString());
    ac->setMediaId(r.readU64());
    ac->setSampleRate(r.readU32());
    ac->setChannels(static_cast<uint16_t>(r.readU32()));
    ac->setSourceDuration(r.readI64());
    readKeyframeTrack(r, ac->volume(), version);
    readKeyframeTrack(r, ac->pan(), version);
    ac->setFadeInDuration(r.readI64());
    ac->setFadeOutDuration(r.readI64());
    // v27: chosen audio-stream ordinal. Pre-v27 projects leave it at -1
    // (auto/best), so they load byte-identically in effect.
    if (version >= 27)
        ac->setAudioStreamIndex(static_cast<int32_t>(r.readU32()));
}

// ── Title ──────────────────────────────────────────────────────────────────

// TitleClip is creation-orphaned — the only source of one is a legacy project
// file. Loading MIGRATES each Title record into a GraphicClip with a single
// TextLayer (the registry maps ClipType::Title to a GraphicClip factory), so
// the legacy render/UI surface is never exercised after load and the clip
// re-saves as a modern Graphic. Base-clip state (id — which transitions
// reference —, transform, effects, masks) reads through the common path
// directly into the GraphicClip, so nothing is hand-copied.
// Fidelity notes: alignment maps 1:1 (center/middle titles — the default —
// render identically); a non-transparent legacy background becomes a
// full-frame rect layer behind the text; legacy frame-width word-wrap is not
// carried over (explicit '\n' line breaks are preserved).
void readTitleFields(BinaryReader& r, Clip& clip, uint32_t version)
{
    auto* gc = static_cast<GraphicClip*>(&clip);

    const std::string text       = r.readString();
    const std::string fontFamily = r.readString();
    const float       fontSize   = r.readF32();
    const bool        bold       = r.readU8() != 0;
    const bool        italic     = r.readU8() != 0;
    const auto        align      = r.readU8();   // TextAlign: L/C/R
    const auto        valign     = r.readU8();   // TextVAlign: T/M/B
    const uint32_t    textColor  = r.readU32();
    const uint32_t    bgColor    = r.readU32();
    const uint32_t    outlineCol = r.readU32();
    const float       outlineW   = r.readF32();

    // Background first so it stacks behind the text layer.
    if ((bgColor >> 24) != 0) {
        ShapeLayer* bg = gc->addShapeLayer(ShapeType::Rectangle);
        bg->setName("Background");
        bg->setShapeWidth(8192.0f);   // covers any frame; legacy fill was full-frame
        bg->setShapeHeight(8192.0f);
        bg->setFillColor(bgColor);
    }

    TextLayer* tl = gc->addTextLayer(text);
    tl->setFontFamily(fontFamily);
    tl->setFontSize(fontSize);
    tl->setFontWeight(bold ? 700 : 400);
    tl->setItalic(italic);
    // TextAlign L/C/R and TextVAlign T/M/B share GTextAlign/GTextVAlign order.
    tl->setAlignment(static_cast<GTextAlign>(std::min<uint8_t>(align, 2)));
    tl->setVAlignment(static_cast<GTextVAlign>(std::min<uint8_t>(valign, 2)));
    tl->appearance().fills.push_back({textColor, 1.0f, true});
    if (outlineW > 0.01f)
        tl->appearance().strokes.push_back(
            {outlineCol, outlineW, StrokePosition::Outer, 1.0f, true});

    readKeyframeTrack(r, tl->tracking(), version);
    // Legacy lineHeight was a MULTIPLIER (default 1.2); leading is additive px.
    KeyframeTrack<float> lineHeight{1.2f};
    readKeyframeTrack(r, lineHeight, version);
    const float mult = lineHeight.evaluate(0);
    if (mult > 1.21f || mult < 1.19f)
        tl->leading().addKeyframe(0, (mult - 1.2f) * fontSize);
}

// ── Adjustment ─────────────────────────────────────────────────────────────

void readAdjustmentFields(BinaryReader& r, Clip& clip, uint32_t)
{
    auto* ac = static_cast<AdjustmentClip*>(&clip);
    (void)r.readU8();   // former adjustment-only blend byte: unused
    ac->setAffectsSingleTrack(r.readU8() != 0);
}

// ── Image ──────────────────────────────────────────────────────────────────

void readImageFields(BinaryReader& r, Clip& clip, uint32_t)
{
    auto* ic = static_cast<ImageClip*>(&clip);
    ic->setMediaPath(r.readString());
    ic->setMediaId(r.readU64());
    uint32_t sw = r.readU32(), sh = r.readU32();
    ic->setSourceResolution(sw, sh);
    float cl = r.readF32(), cr = r.readF32(), ct = r.readF32(), cb = r.readF32();
    ic->setCrop(cl, cr, ct, cb);
}

// ── Sequence (nest) ────────────────────────────────────────────────────────

void readSequenceFields(BinaryReader& r, Clip& clip, uint32_t)
{
    auto* sc = static_cast<SequenceClip*>(&clip);
    sc->setSequenceIndex(static_cast<size_t>(r.readU32()));
    sc->setSequenceName(r.readString());
}

// ── Caption ────────────────────────────────────────────────────────────────

static TextStyleRun readTextStyleRun(BinaryReader& r, uint32_t version)
{
    TextStyleRun run;
    run.start = r.readU32();
    run.length = r.readU32();
    run.fontFamily = r.readString();
    run.fontSize = r.readF32();
    run.fontWeight = static_cast<int>(r.readU32());
    run.italic = r.readU8() != 0;
    if (version >= 38) {
        run.allCaps = r.readU8() != 0;
        run.smallCaps = r.readU8() != 0;
        run.tracking = r.readF32();
        run.baselineShift = r.readF32();
        run.overrideMask = version >= 40 ? r.readU32() : r.readU8();
    }
    if (version >= 39) run.leading = r.readF32();
    if (version >= 40) {
        run.fontStyle = r.readString();
        run.kerning = r.readF32();
        run.tabWidth = r.readF32();
        run.tsume = r.readF32();
        run.fauxBold = r.readU8() != 0;
        run.fauxItalic = r.readU8() != 0;
        run.underline = r.readU8() != 0;
        run.superscript = r.readU8() != 0;
        run.subscript = r.readU8() != 0;
        auto& a = run.appearance;
        a.fillEnabled = r.readU8() != 0; a.fillColor = r.readU32();
        a.strokeEnabled = r.readU8() != 0; a.strokeColor = r.readU32();
        a.strokeWidth = r.readF32();
        a.strokePosition = static_cast<StrokePosition>(r.readU8());
        a.shadowEnabled = r.readU8() != 0; a.shadowColor = r.readU32();
        a.shadowDistance = r.readF32(); a.shadowAngle = r.readF32();
        a.shadowSoftness = r.readF32(); a.shadowOpacity = r.readF32();
        a.backgroundEnabled = r.readU8() != 0;
        a.backgroundColor = r.readU32();
        a.backgroundPadding = r.readF32();
    }
    return run;
}

void readCaptionFields(BinaryReader& r, Clip& clip, uint32_t version)
{
    auto* cc = static_cast<CaptionClip*>(&clip);
    cc->setText(r.readString());
    cc->setSpeaker(r.readString());
    cc->setFontFamily(r.readString());
    cc->setFontSize(r.readF32());
    cc->setTextColor(r.readU32());
    cc->setBgColor(r.readU32());
    cc->setPosition(static_cast<CaptionPosition>(r.readU8()));
    if (version >= 31) {
        cc->setBold(r.readU8() != 0);
        cc->setOutlineColor(r.readU32());
        cc->setOutlineWidth(r.readF32());
        cc->setShowSpeaker(r.readU8() != 0);
    } // pre-v31 defaults (bold, no outline, no speaker) match the old look
    std::vector<TextStyleRun> loadedRuns;
    if (version >= 37) {
        const uint32_t count = r.readCount(100000, 21);
        loadedRuns.reserve(count);
        for (uint32_t i = 0; i < count && r.ok(); ++i)
            loadedRuns.push_back(readTextStyleRun(r, version));
    }
    if (version >= 40) {
        cc->setFontStyle(r.readString());
        cc->setItalic(r.readU8() != 0);
        cc->setAllCaps(r.readU8() != 0);
        cc->setSmallCaps(r.readU8() != 0);
        cc->setUnderline(r.readU8() != 0);
        const bool superscript = r.readU8() != 0;
        const bool subscript = r.readU8() != 0;
        cc->setScript(superscript, subscript);
        const bool fauxBold = r.readU8() != 0;
        const bool fauxItalic = r.readU8() != 0;
        cc->setFauxStyles(fauxBold, fauxItalic);
        cc->setTracking(r.readF32());
        cc->setLeading(r.readF32());
        cc->setAlignment(static_cast<GTextAlign>(r.readU8()));
        cc->setTrackStyleName(r.readString());
        const uint32_t paragraphCount = r.readCount(100000, 10);
        std::vector<TextParagraphStyle> paragraphs;
        paragraphs.reserve(paragraphCount);
        for (uint32_t pi = 0; pi < paragraphCount && r.ok(); ++pi) {
            TextParagraphStyle paragraph;
            paragraph.start = r.readU32();
            paragraph.length = r.readU32();
            paragraph.alignment = static_cast<GTextAlign>(r.readU8());
            paragraph.rightToLeft = r.readU8() != 0;
            paragraphs.push_back(paragraph);
        }
        cc->setParagraphStyles(std::move(paragraphs));
    }
    if (version >= 37) cc->setStyleRuns(std::move(loadedRuns));
    if (version >= 42) cc->setConfidence(r.readF32());
}

// ── PngPuppet ──────────────────────────────────────────────────────────────

void readPngPuppetFields(BinaryReader& r, Clip& clip, uint32_t)
{
    auto* pc = static_cast<PngPuppetClip*>(&clip);
    pc->setCharacterName(r.readString());
    pc->setVariant(r.readString());
    for (int f = 0; f < PngPuppetClip::FaceCount; ++f)
        pc->setFacePath(f, r.readString());
    pc->setTalking(r.readU8() != 0);
    pc->setSeed(r.readU32());
    pc->setTalkSwapSeconds(r.readF32());
    pc->setBlinkIntervalSeconds(r.readF32());
    pc->setBlinkDurationSeconds(r.readF32());
    pc->setBreathAmplitude(r.readF32());
    pc->setBreathSpeed(r.readF32());
    pc->setSwayAmplitude(r.readF32());
}

// ── Graphic ────────────────────────────────────────────────────────────────

void readGraphicFields(BinaryReader& r, Clip& clip, uint32_t version)
{
    auto* gc = static_cast<GraphicClip*>(&clip);
    const uint32_t layerCount = r.readCount(100000, 7);
    for (uint32_t li = 0; li < layerCount && r.ok(); ++li) {
        auto layerType = static_cast<GraphicLayerType>(r.readU8());
        std::string layerName = r.readString();
        bool visible = r.readU8() != 0;
        bool locked  = r.readU8() != 0;
        std::unique_ptr<GraphicLayer> layer;
        if (layerType == GraphicLayerType::Text)
            layer = std::make_unique<TextLayer>();
        else
            layer = std::make_unique<ShapeLayer>();
        layer->setName(layerName);
        layer->setVisible(visible);
        layer->setLocked(locked);
        // Layer transform (8 keyframe tracks)
        readKeyframeTrack(r, layer->transform().posX, version);
        readKeyframeTrack(r, layer->transform().posY, version);
        readKeyframeTrack(r, layer->transform().scaleX, version);
        readKeyframeTrack(r, layer->transform().scaleY, version);
        readKeyframeTrack(r, layer->transform().rotation, version);
        readKeyframeTrack(r, layer->transform().anchorX, version);
        readKeyframeTrack(r, layer->transform().anchorY, version);
        readKeyframeTrack(r, layer->transform().opacity, version);
        // Appearance
        auto& app = layer->appearance();
        app.fills.clear();
        const uint32_t fillCount = r.readCount(100000, 9);
        for (uint32_t fi = 0; fi < fillCount && r.ok(); ++fi) {
            FillEntry fe; fe.color = r.readU32(); fe.opacity = r.readF32(); fe.enabled = r.readU8() != 0;
            app.fills.push_back(fe);
        }
        app.strokes.clear();
        const uint32_t strokeCount = r.readCount(100000, 14);
        for (uint32_t si = 0; si < strokeCount && r.ok(); ++si) {
            StrokeEntry se; se.color = r.readU32(); se.width = r.readF32(); se.position = static_cast<StrokePosition>(r.readU8()); se.opacity = r.readF32(); se.enabled = r.readU8() != 0;
            app.strokes.push_back(se);
        }
        app.shadows.clear();
        const uint32_t shadowCount = r.readCount(100000, 21);
        for (uint32_t si = 0; si < shadowCount && r.ok(); ++si) {
            ShadowEntry se; se.color = r.readU32(); se.distance = r.readF32(); se.angle = r.readF32(); se.softness = r.readF32(); se.opacity = r.readF32(); se.enabled = r.readU8() != 0;
            app.shadows.push_back(se);
        }
        // Type-specific
        if (layerType == GraphicLayerType::Text) {
            auto* tl = static_cast<TextLayer*>(layer.get());
            tl->setText(r.readString());
            tl->setFontFamily(r.readString());
            tl->setFontSize(r.readF32());
            tl->setFontWeight(static_cast<int>(r.readU32()));
            tl->setItalic(r.readU8() != 0);
            tl->setAllCaps(r.readU8() != 0);
            tl->setSmallCaps(r.readU8() != 0);
            tl->setAlignment(static_cast<GTextAlign>(r.readU8()));
            tl->setVAlignment(static_cast<GTextVAlign>(r.readU8()));
            readKeyframeTrack(r, tl->tracking(), version);
            readKeyframeTrack(r, tl->leading(), version);
            readKeyframeTrack(r, tl->baselineShift(), version);
            tl->setBoxWidth(r.readF32());
            tl->setBoxHeight(r.readF32());
            tl->setUseParagraphBox(r.readU8() != 0);
            std::vector<TextStyleRun> loadedRuns;
            if (version >= 36) {
                const uint32_t runCount = r.readCount(100000, 21);
                loadedRuns.reserve(runCount);
                for (uint32_t ri = 0; ri < runCount && r.ok(); ++ri)
                    loadedRuns.push_back(readTextStyleRun(r, version));
            }
            if (version >= 40) {
                tl->setFontStyleForAll(r.readString());
                tl->setKerningForAll(r.readF32());
                tl->setTabWidthForAll(r.readF32());
                tl->setTsumeForAll(r.readF32());
                const bool fauxBold = r.readU8() != 0;
                const bool fauxItalic = r.readU8() != 0;
                tl->setFauxStylesForAll(fauxBold, fauxItalic);
                tl->setUnderlineForAll(r.readU8() != 0);
                const bool superscript = r.readU8() != 0;
                const bool subscript = r.readU8() != 0;
                tl->setScriptForAll(superscript, subscript);
                tl->setRightToLeft(r.readU8() != 0);
                const bool backgroundEnabled = r.readU8() != 0;
                const uint32_t backgroundColor = r.readU32();
                const float backgroundPadding = r.readF32();
                tl->setBackgroundForAll(backgroundEnabled, backgroundColor,
                                        backgroundPadding);
                tl->setMaskWithText(r.readU8() != 0);
                tl->setLinkedStyleName(r.readString());
                const uint32_t paragraphCount = r.readCount(100000, 10);
                std::vector<TextParagraphStyle> paragraphs;
                paragraphs.reserve(paragraphCount);
                for (uint32_t pi = 0; pi < paragraphCount && r.ok(); ++pi) {
                    TextParagraphStyle paragraph;
                    paragraph.start = r.readU32();
                    paragraph.length = r.readU32();
                    paragraph.alignment = static_cast<GTextAlign>(r.readU8());
                    paragraph.rightToLeft = r.readU8() != 0;
                    paragraphs.push_back(paragraph);
                }
                tl->setParagraphStyles(std::move(paragraphs));
            }
            if (version >= 36)
                tl->setStyleRuns(std::move(loadedRuns));
        } else if (layerType == GraphicLayerType::Shape) {
            auto* sl = static_cast<ShapeLayer*>(layer.get());
            sl->setShapeType(static_cast<ShapeType>(r.readU8()));
            sl->setShapeWidth(r.readF32());
            sl->setShapeHeight(r.readF32());
            sl->setCornerRadius(r.readF32());
            sl->setFillColor(r.readU32());
        }
        gc->addLayer(std::move(layer));
    }
}

// ── TierList ─────────────────────────────────────────────────────────────────

void readTierListFields(BinaryReader& r, Clip& clip, uint32_t version)
{
    // TierList fields first appear in v28; a TierList clip in an older file
    // means corruption/hand-editing — leave the ctor defaults, read nothing.
    if (version < 28) return;

    auto* tc = static_cast<TierListClip*>(&clip);
    tc->setTitle(r.readString());
    {
        // Clamp out-of-range enum bytes (corrupt file) to a valid default —
        // an invalid enum value is UB in every switch downstream.
        uint8_t aspect = r.readU8();
        if (aspect > static_cast<uint8_t>(TierEntryAspect::Custom))
            aspect = static_cast<uint8_t>(TierEntryAspect::Banner);
        tc->setEntryAspect(static_cast<TierEntryAspect>(aspect));
    }
    const float caw = r.readF32();
    const float cah = r.readF32();
    tc->setCustomAspect(caw, cah);
    tc->setTopSafeMargin(r.readF32());
    tc->setRightSafeMargin(r.readF32());
    tc->setBackgroundColor(r.readU32());
    // Tiers
    tc->tiers().clear();
    const uint32_t tierCount = r.readCount(100000, 8);
    for (uint32_t i = 0; i < tierCount && r.ok(); ++i) {
        TierDef t;
        t.label = r.readString();
        t.color = r.readU32();
        tc->tiers().push_back(std::move(t));
    }
    // Subbins
    tc->subbins().clear();
    const uint32_t subCount = r.readCount(100000, 4);
    for (uint32_t i = 0; i < subCount && r.ok(); ++i)
        tc->subbins().push_back(r.readString());
    // Entries
    tc->entries().clear();
    const uint32_t entryCount = r.readCount(100000, 20);
    for (uint32_t i = 0; i < entryCount && r.ok(); ++i) {
        TierEntry e;
        e.id        = r.readU64();
        e.imagePath = r.readString();
        e.title     = r.readString();
        e.subbin    = r.readString();
        tc->entries().push_back(std::move(e));
    }
    // Events
    tc->events().clear();
    const uint32_t eventCount = r.readCount(100000, 41);
    for (uint32_t i = 0; i < eventCount && r.ok(); ++i) {
        TierEvent ev;
        uint8_t evType = r.readU8();
        if (evType > static_cast<uint8_t>(TierEventType::Reorder))
            evType = static_cast<uint8_t>(TierEventType::Popup);
        ev.type      = static_cast<TierEventType>(evType);
        ev.entryId   = r.readU64();
        ev.tier      = static_cast<int32_t>(r.readU32());
        ev.index     = static_cast<int32_t>(r.readU32());
        ev.fromIndex = static_cast<int32_t>(r.readU32());
        ev.toIndex   = static_cast<int32_t>(r.readU32());
        ev.start     = r.readI64();
        ev.end       = r.readI64();
        tc->events().push_back(ev);
    }
}

// ── The registry ───────────────────────────────────────────────────────────
// ONE entry per ClipType that positional files can contain.

struct RegistryEntry { ClipType type; TypeSerializer s; };

constexpr RegistryEntry kClipSerializers[] = {
    { ClipType::Spine,      { makeClip<SpineClip>,      readSpineFields      } },
    { ClipType::Video,      { makeClip<VideoClip>,      readVideoFields      } },
    { ClipType::Audio,      { makeClip<AudioClip>,      readAudioFields      } },
    // Title records MIGRATE to GraphicClip on load (see readTitleFields).
    { ClipType::Title,      { makeClip<GraphicClip>,    readTitleFields      } },
    { ClipType::Adjustment, { makeClip<AdjustmentClip>, readAdjustmentFields } },
    { ClipType::Image,      { makeClip<ImageClip>,      readImageFields      } },
    { ClipType::Sequence,   { makeClip<SequenceClip>,   readSequenceFields   } },
    { ClipType::Caption,    { makeClip<CaptionClip>,    readCaptionFields    } },
    { ClipType::PngPuppet,  { makeClip<PngPuppetClip>,  readPngPuppetFields  } },
    { ClipType::Graphic,    { makeClip<GraphicClip>,    readGraphicFields    } },
    { ClipType::TierList,   { makeClip<TierListClip>,   readTierListFields   } },
};

const TypeSerializer* serializerFor(ClipType type)
{
    for (const auto& e : kClipSerializers)
        if (e.type == type)
            return &e.s;
    return nullptr;
}

} // namespace

// ── Serialize a single clip ─────────────────────────────────────────────────

std::unique_ptr<Clip> readClip(BinaryReader& r, uint32_t version)
{
    auto type = static_cast<ClipType>(r.readU8());
    uint64_t id = r.readU64();

    std::string label = r.readString();
    uint32_t color    = r.readU32();
    bool enabled      = r.readU8() != 0;
    int64_t tlIn      = r.readI64();
    int64_t dur       = r.readI64();
    int64_t srcIn     = r.readI64();
    double speed      = r.readF64();

    const TypeSerializer* ts = serializerFor(type);
    if (!ts) {
        r.invalidate();
        return nullptr;
    }
    std::unique_ptr<Clip> clip = ts->make();

    // Restore the saved clip id so transitions (which reference clips by id)
    // survive reload.  Previously ids were regenerated, leaving every
    // transition's leftClipId/rightClipId stale and relying on a fragile
    // geometry relink that fails whenever editPointTick drifts from a clip
    // boundary (e.g. after trimming) — which is why transitions "often" went
    // missing on reopen.  Bump the global counter so later clips don't reuse
    // this id.  id 0 only appears in pre-id legacy saves; keep the fresh id
    // there and let relinkTransitionsByGeometry recover those.
    if (id != 0) {
        clip->setId(id);
        Clip::reserveId(id);
    }

    clip->setLabel(label);
    // Migration: legacy projects baked the old per-type ctor color into m_color
    // for every clip, which prevented theme-based per-type tinting (image=purple,
    // video-character=orange, etc.). Reset such legacy values to the sentinel so
    // TimelineTrackWidget can apply the new theme tints. Users can still pick a
    // custom color afterwards.
    switch (color) {
        case 0xFF44BB88:  // VideoClip teal green
        case 0xFF88CC44:  // AudioClip green
        case 0xFF6B9EFF:  // SpineClip soft blue
        case 0xFF8844CC:  // ImageClip purple
        case 0xFFCC66FF:  // TitleClip purple
        case 0xFF44BBFF:  // GraphicClip teal/cyan
        case 0xFFFFAA44:  // AdjustmentClip orange
        case 0xFF44CCAA:  // CaptionClip teal
        case 0xFF7B68EE:  // SequenceClip slate blue
            clip->setColor(0xFF888888);
            break;
        default:
            clip->setColor(color);
            break;
    }
    clip->setEnabled(enabled);
    clip->setTimelineIn(tlIn);
    clip->setDuration(dur);
    clip->setSourceIn(srcIn);
    clip->setSpeed(speed);

    // Shot group fields (v2+)
    if (version >= 2) {
        clip->setGroupId(r.readU64());
        clip->setShotName(r.readString());
        clip->setLayerId(r.readString());
    }

    // Read 6 keyframe tracks
    readKeyframeTrack(r, clip->opacity(), version);
    readKeyframeTrack(r, clip->positionX(), version);
    readKeyframeTrack(r, clip->positionY(), version);
    readKeyframeTrack(r, clip->scaleX(), version);
    readKeyframeTrack(r, clip->scaleY(), version);
    readKeyframeTrack(r, clip->rotation(), version);

    // Speed ramp track (v5+)
    if (version >= 5)
        readKeyframeTrack(r, clip->speedRamp(), version);

    // Anchor point tracks (v19+). Pre-v19 projects leave the tracks at
    // the constructed default of 0, which makes the renderer pivot
    // around the layer center — identical to the pre-anchor behavior.
    if (version >= 19) {
        readKeyframeTrack(r, clip->anchorX(), version);
        readKeyframeTrack(r, clip->anchorY(), version);
    }

    // Transform motion blur (v35+). Pre-v35 projects keep the constructed
    // 0-degree default, so they render exactly as they did before.
    if (version >= 35)
        readKeyframeTrack(r, clip->shutterAngle(), version);

    // Blend mode (v5+)
    if (version >= 5)
        clip->setBlendMode(static_cast<int32_t>(r.readU32()));

    // Maintain pitch flag (v7+)
    if (version >= 7)
        clip->setMaintainPitch(r.readU8() != 0);

    // AudioSync export back-link (v29+). Pre-v29 clips keep the -1 default
    // (= not export-owned), so the first re-export on a legacy timeline
    // falls back to the full/clean path.
    if (version >= 29)
        clip->setSyncLine(static_cast<int32_t>(r.readU32()));

    // Temporal interpolation mode (v32+).  Invalid values from a damaged or
    // future project safely retain the constructed Frame Sampling default.
    if (version >= 32) {
        const uint8_t mode = r.readU8();
        if (mode <= static_cast<uint8_t>(TimeInterpolation::OpticalFlow))
            clip->setTimeInterpolation(static_cast<TimeInterpolation>(mode));
    }

    // A/V link (v44+). Pre-v44 projects never saved links, so their clips
    // load unlinked (0), exactly as they always have.
    if (version >= 44)
        clip->setLinkId(r.readU64());

    // Type-specific fields — dispatched through the per-type registry
    ts->readFields(r, *clip, version);

    // ── Effect stack (v10+) ────────────────────────────────────────────
    if (version >= 10 && clip) {
        const uint32_t fxCount = r.readCount(1024, 6);
        for (uint32_t ei = 0; ei < fxCount && r.ok(); ++ei) {
            auto fxType = static_cast<EffectType>(r.readU8());
            bool fxEnabled = r.readU8() != 0;
            const uint32_t paramCount = r.readCount(4096, 4);
            auto fx = createEffect(fxType);
            if (fx) {
                fx->setEnabled(fxEnabled);
                // Read keyframe tracks — match stored count to actual params
                size_t toRead = std::min(static_cast<size_t>(paramCount), fx->paramCount());
                for (size_t pi = 0; pi < toRead; ++pi)
                    readKeyframeTrack(r, fx->param(pi).track, version);
                // Skip extra params if the stored effect had more than current code
                for (size_t pi = toRead; pi < paramCount; ++pi) {
                    KeyframeTrack<float> discard(0.0f);
                    readKeyframeTrack(r, discard, version);
                }
                // v45 made the vignette Premiere-exact: Amount was inverted
                // (positive darkened) and Roundness was 0..100 around 50.
                // Remap so old vignettes keep their darken/lighten intent.
                if (version < 45 && (fxType == EffectType::ColorGrading
                                     || fxType == EffectType::LumetriColor)) {
                    auto remap = [&](size_t idx, auto fn) {
                        if (idx >= toRead) return;
                        auto& trk = fx->param(idx).track;
                        // The default is only stored for static tracks; a
                        // keyframed track keeps the (already new-scale)
                        // built-in default.
                        if (trk.keyframeCount() == 0)
                            trk.setDefaultValue(fn(trk.defaultValue()));
                        for (size_t k = 0; k < trk.keyframeCount(); ++k)
                            trk.keyframe(k).value = fn(trk.keyframe(k).value);
                    };
                    remap(ColorGrading::VignetteAmount, [](float v) { return -v; });
                    remap(ColorGrading::VignetteRoundness,
                          [](float v) { return (v - 50.0f) * 2.0f; });
                }
                if (fxType == EffectType::LUT) {
                    std::string lutPath = r.readString();
                    if (!lutPath.empty())
                        static_cast<LUT*>(fx.get())->loadCubeFile(lutPath);
                }
                if (version >= 24 && isBeatReactEffect(fxType)) {
                    auto* be = static_cast<BeatReactEffect*>(fx.get());
                    be->setAudioSourceId(r.readU64());
                    const uint32_t n = r.readCount(1000000, 4);
                    std::vector<float> times(n);
                    for (uint32_t i = 0; i < n && r.ok(); ++i) times[i] = r.readF32();
                    be->setBeatTimes(std::move(times));
                }
                if (version >= 41 && fxType == EffectType::ColorGrading) {
                    auto* grading = static_cast<ColorGrading*>(fx.get());
                    grading->setBasicEnabled(r.readU8() != 0);
                    grading->setCreativeEnabled(r.readU8() != 0);
                    grading->setWheelsEnabled(r.readU8() != 0);
                    grading->setCurvesEnabled(r.readU8() != 0);
                    grading->setHslEnabled(r.readU8() != 0);
                    grading->setVignetteEnabled(r.readU8() != 0);
                    ColorGrading::HslParams hsl;
                    hsl.hueCenter = r.readF32(); hsl.hueWidth = r.readF32();
                    hsl.satMin = r.readF32(); hsl.satMax = r.readF32();
                    hsl.lumMin = r.readF32(); hsl.lumMax = r.readF32();
                    hsl.hueShift = r.readF32(); hsl.satAdjust = r.readF32();
                    hsl.lumAdjust = r.readF32();
                    grading->setHslParams(hsl);
                    for (int ch = 0; ch < ColorGrading::CurveCount; ++ch) {
                        ColorGrading::CurveLUT lut{};
                        for (float& sample : lut) sample = r.readF32();
                        grading->setCurveLUT(
                            static_cast<ColorGrading::CurveChannel>(ch), lut);
                    }
                }
                // Effect masks (v30+)
                if (version >= 30) {
                    const uint32_t fxMaskCount = r.readCount(10000, 68);
                    for (uint32_t mi = 0; mi < fxMaskCount && r.ok(); ++mi)
                        fx->addMask(readMask(r, version));
                }
                clip->effects().addEffect(std::move(fx));
            } else {
                // Supported-format effects are not individually length-delimited,
                // so an unknown type cannot be skipped without risking drift into
                // the next record. Treat it as corruption instead.
                r.invalidate();
                return nullptr;
            }
        }
    }

    // ── Opacity masks (v11+; v30 record) ───────────────────────────────
    if (version >= 11 && clip) {
        const uint32_t maskCount = r.readCount(10000, version >= 30 ? 68 : 42);
        for (uint32_t mi = 0; mi < maskCount && r.ok(); ++mi)
            clip->addMask(readMask(r, version));
    }

    // ── Audio FX chain (v22+) — audio clips only ───────────────────────
    if (version >= 22 && clip && clip->clipType() == ClipType::Audio)
        readAudioFx(r, static_cast<AudioClip*>(clip.get())->audioFx());

    return clip;
}
} // namespace rt
