/*
 * ClipSchemas.cpp — see ClipSchemas.h. Field ids are append-only: add new
 * fields at the end of a table with the next id, never renumber.
 *
 * Where a setter rewrites other state (the "ForAll" text setters and
 * caption styles reshape character runs), the runs field sits LAST in its
 * table so the saved runs are applied after everything they depend on.
 */

#include "project/schema/ClipSchemas.h"

#include "project/VideoPathResolve.h"
#include "project/schema/LeafSchemas.h"

#include "effects/Effect.h"
#include "effects/EffectStack.h"
#include "timeline/AdjustmentClip.h"
#include "timeline/AudioClip.h"
#include "timeline/CaptionClip.h"
#include "timeline/Clip.h"
#include "timeline/GraphicClip.h"
#include "timeline/GraphicLayer.h"
#include "timeline/ImageClip.h"
#include "timeline/PngPuppetClip.h"
#include "timeline/SequenceClip.h"
#include "timeline/SpineClip.h"
#include "timeline/TierListClip.h"
#include "timeline/VideoClip.h"

#include <spdlog/spdlog.h>

#include <string>
#include <utility>
#include <vector>

namespace rt::record {

#define RT_MEMBER(Type, id, name) \
    member<Type>(id, #name, [](Type& o) -> auto& { return o.name; })
// A getter/setter pair of the same name, value type taken from the getter.
#define RT_PROP(Type, id, get, set) \
    field<Type>(id, #get, [](const Type& o) { return o.get(); }, \
                [](Type& o, auto v) { o.set(std::move(v)); })
#define RT_TRACK(Type, id, name) \
    member<Type>(id, #name, [](Type& o) -> KeyframeTrack<float>& { return o.name(); })

// ── Tier list pieces ────────────────────────────────────────────────────

template <> struct SchemaOf<TierDef> { static const Schema<TierDef>& get(); };
template <> struct SchemaOf<TierEntry> { static const Schema<TierEntry>& get(); };
template <> struct SchemaOf<TierEvent> { static const Schema<TierEvent>& get(); };

const Schema<TierDef>& SchemaOf<TierDef>::get()
{
    static const Schema<TierDef> s("TierDef", {
        RT_MEMBER(TierDef, 1, label),
        RT_MEMBER(TierDef, 2, color),
    });
    return s;
}

const Schema<TierEntry>& SchemaOf<TierEntry>::get()
{
    static const Schema<TierEntry> s("TierEntry", {
        RT_MEMBER(TierEntry, 1, id),
        RT_MEMBER(TierEntry, 2, imagePath),
        RT_MEMBER(TierEntry, 3, title),
        RT_MEMBER(TierEntry, 4, subbin),
    });
    return s;
}

const Schema<TierEvent>& SchemaOf<TierEvent>::get()
{
    static const Schema<TierEvent> s("TierEvent", {
        field<TierEvent>(1, "type", [](const TierEvent& e) { return e.type; },
            [](TierEvent& e, TierEventType v) {
                // An unknown event type (damaged file) would be UB downstream.
                e.type = static_cast<uint8_t>(v) <= static_cast<uint8_t>(TierEventType::Reorder)
                    ? v : TierEventType::Popup;
            }),
        RT_MEMBER(TierEvent, 2, entryId),
        RT_MEMBER(TierEvent, 3, tier),
        RT_MEMBER(TierEvent, 4, index),
        RT_MEMBER(TierEvent, 5, fromIndex),
        RT_MEMBER(TierEvent, 6, toIndex),
        RT_MEMBER(TierEvent, 7, start),
        RT_MEMBER(TierEvent, 8, end),
    });
    return s;
}

namespace {

// ── Per-type schemas ────────────────────────────────────────────────────

const Schema<SpineClip>& spineSchema()
{
    using C = SpineClip;
    static const Schema<C> s("SpineClip", {
        RT_PROP(C, 1, characterName, setCharacterName),
        RT_PROP(C, 2, outfit, setOutfit),
        RT_PROP(C, 3, stance, setStance),
        RT_PROP(C, 4, animationName, setAnimationName),
        RT_PROP(C, 5, isLooping, setLooping),
        RT_PROP(C, 6, isTalking, setTalking),
        RT_PROP(C, 7, animationSpeed, setAnimationSpeed),
        RT_PROP(C, 8, useGlobalTime, setUseGlobalTime),
        field<C>(9, "cropLeft", [](const C& c) { return c.cropLeft(); },
            [](C& c, float v) { c.setCrop(v, c.cropRight(), c.cropTop(), c.cropBottom()); }),
        field<C>(10, "cropRight", [](const C& c) { return c.cropRight(); },
            [](C& c, float v) { c.setCrop(c.cropLeft(), v, c.cropTop(), c.cropBottom()); }),
        field<C>(11, "cropTop", [](const C& c) { return c.cropTop(); },
            [](C& c, float v) { c.setCrop(c.cropLeft(), c.cropRight(), v, c.cropBottom()); }),
        field<C>(12, "cropBottom", [](const C& c) { return c.cropBottom(); },
            [](C& c, float v) { c.setCrop(c.cropLeft(), c.cropRight(), c.cropTop(), v); }),
    });
    return s;
}

const Schema<VideoClip>& videoSchema()
{
    using C = VideoClip;
    static const Schema<C> s("VideoClip", {
        field<C>(1, "mediaPath", [](const C& c) { return c.mediaPath(); },
            [](C& c, std::string v) { c.setMediaPath(resolveVideoPath(v)); }),
        RT_PROP(C, 2, mediaId, setMediaId),
        field<C>(3, "sourceWidth", [](const C& c) { return c.sourceWidth(); },
            [](C& c, uint32_t v) { c.setSourceResolution(v, c.sourceHeight()); }),
        field<C>(4, "sourceHeight", [](const C& c) { return c.sourceHeight(); },
            [](C& c, uint32_t v) { c.setSourceResolution(c.sourceWidth(), v); }),
        RT_PROP(C, 5, sourceFps, setSourceFps),
        RT_PROP(C, 6, sourceDuration, setSourceDuration),
        RT_PROP(C, 7, hasAudio, setHasAudio),
        RT_PROP(C, 8, volume, setVolume),
        field<C>(9, "cropLeft", [](const C& c) { return c.cropLeft(); },
            [](C& c, float v) { c.setCrop(v, c.cropRight(), c.cropTop(), c.cropBottom()); }),
        field<C>(10, "cropRight", [](const C& c) { return c.cropRight(); },
            [](C& c, float v) { c.setCrop(c.cropLeft(), v, c.cropTop(), c.cropBottom()); }),
        field<C>(11, "cropTop", [](const C& c) { return c.cropTop(); },
            [](C& c, float v) { c.setCrop(c.cropLeft(), c.cropRight(), v, c.cropBottom()); }),
        field<C>(12, "cropBottom", [](const C& c) { return c.cropBottom(); },
            [](C& c, float v) { c.setCrop(c.cropLeft(), c.cropRight(), c.cropTop(), v); }),
        RT_PROP(C, 13, characterName, setCharacterName),
        RT_PROP(C, 14, isTalking, setTalking),
        field<C>(15, "videoMutePath", [](const C& c) { return c.videoMutePath(); },
            [](C& c, std::string v) { c.setVideoMutePath(resolveVideoPath(v)); }),
        field<C>(16, "videoTalkPath", [](const C& c) { return c.videoTalkPath(); },
            [](C& c, std::string v) { c.setVideoTalkPath(resolveVideoPath(v)); }),
        RT_PROP(C, 17, outfit, setOutfit),
        RT_PROP(C, 18, animationName, setAnimationName),
        RT_PROP(C, 19, sourceRotation, setSourceRotation),
        RT_PROP(C, 20, sourceMetadataAuthoritative, setSourceMetadataAuthoritative),
    });
    return s;
}

const Schema<AudioClip>& audioSchema()
{
    using C = AudioClip;
    static const Schema<C> s("AudioClip", {
        RT_PROP(C, 1, mediaPath, setMediaPath),
        RT_PROP(C, 2, mediaId, setMediaId),
        RT_PROP(C, 3, sampleRate, setSampleRate),
        RT_PROP(C, 4, channels, setChannels),
        RT_PROP(C, 5, sourceDuration, setSourceDuration),
        RT_TRACK(C, 6, volume),
        RT_TRACK(C, 7, pan),
        RT_PROP(C, 8, fadeInDuration, setFadeInDuration),
        RT_PROP(C, 9, fadeOutDuration, setFadeOutDuration),
        RT_PROP(C, 10, audioStreamIndex, setAudioStreamIndex),
        custom<C>(11, "audioFx",
            [](RecordWriter& w, uint32_t id, const C& c) { writeFxChain(w, id, c.audioFx()); },
            [](RecordReader& r, C& c) { return readFxChain(r, c.audioFx()); }),
    });
    return s;
}

const Schema<AdjustmentClip>& adjustmentSchema()
{
    using C = AdjustmentClip;
    static const Schema<C> s("AdjustmentClip", {
        RT_PROP(C, 1, affectsSingleTrack, setAffectsSingleTrack),
    });
    return s;
}

const Schema<ImageClip>& imageSchema()
{
    using C = ImageClip;
    static const Schema<C> s("ImageClip", {
        RT_PROP(C, 1, mediaPath, setMediaPath),
        RT_PROP(C, 2, mediaId, setMediaId),
        field<C>(3, "sourceWidth", [](const C& c) { return c.sourceWidth(); },
            [](C& c, uint32_t v) { c.setSourceResolution(v, c.sourceHeight()); }),
        field<C>(4, "sourceHeight", [](const C& c) { return c.sourceHeight(); },
            [](C& c, uint32_t v) { c.setSourceResolution(c.sourceWidth(), v); }),
        field<C>(5, "cropLeft", [](const C& c) { return c.cropLeft(); },
            [](C& c, float v) { c.setCrop(v, c.cropRight(), c.cropTop(), c.cropBottom()); }),
        field<C>(6, "cropRight", [](const C& c) { return c.cropRight(); },
            [](C& c, float v) { c.setCrop(c.cropLeft(), v, c.cropTop(), c.cropBottom()); }),
        field<C>(7, "cropTop", [](const C& c) { return c.cropTop(); },
            [](C& c, float v) { c.setCrop(c.cropLeft(), c.cropRight(), v, c.cropBottom()); }),
        field<C>(8, "cropBottom", [](const C& c) { return c.cropBottom(); },
            [](C& c, float v) { c.setCrop(c.cropLeft(), c.cropRight(), c.cropTop(), v); }),
    });
    return s;
}

const Schema<SequenceClip>& sequenceSchema()
{
    using C = SequenceClip;
    static const Schema<C> s("SequenceClip", {
        field<C>(1, "sequenceIndex",
            [](const C& c) { return static_cast<uint64_t>(c.sequenceIndex()); },
            [](C& c, uint64_t v) { c.setSequenceIndex(static_cast<size_t>(v)); }),
        RT_PROP(C, 2, sequenceName, setSequenceName),
    });
    return s;
}

const Schema<CaptionClip>& captionSchema()
{
    using C = CaptionClip;
    static const Schema<C> s("CaptionClip", {
        RT_PROP(C, 1, text, setText),
        RT_PROP(C, 2, speaker, setSpeaker),
        RT_PROP(C, 3, fontFamily, setFontFamily),
        RT_PROP(C, 4, fontSize, setFontSize),
        RT_PROP(C, 5, textColor, setTextColor),
        RT_PROP(C, 6, bgColor, setBgColor),
        RT_PROP(C, 7, position, setPosition),
        RT_PROP(C, 8, isBold, setBold),
        RT_PROP(C, 9, outlineColor, setOutlineColor),
        RT_PROP(C, 10, outlineWidth, setOutlineWidth),
        RT_PROP(C, 11, showSpeaker, setShowSpeaker),
        RT_PROP(C, 12, fontStyle, setFontStyle),
        RT_PROP(C, 13, isItalic, setItalic),
        RT_PROP(C, 14, allCaps, setAllCaps),
        RT_PROP(C, 15, smallCaps, setSmallCaps),
        RT_PROP(C, 16, underline, setUnderline),
        field<C>(17, "superscript", [](const C& c) { return c.superscript(); },
            [](C& c, bool v) { c.setScript(v, c.subscript()); }),
        field<C>(18, "subscript", [](const C& c) { return c.subscript(); },
            [](C& c, bool v) { c.setScript(c.superscript(), v); }),
        field<C>(19, "fauxBold", [](const C& c) { return c.fauxBold(); },
            [](C& c, bool v) { c.setFauxStyles(v, c.fauxItalic()); }),
        field<C>(20, "fauxItalic", [](const C& c) { return c.fauxItalic(); },
            [](C& c, bool v) { c.setFauxStyles(c.fauxBold(), v); }),
        RT_PROP(C, 21, tracking, setTracking),
        RT_PROP(C, 22, leading, setLeading),
        RT_PROP(C, 23, alignment, setAlignment),
        RT_PROP(C, 24, trackStyleName, setTrackStyleName),
        RT_PROP(C, 25, paragraphStyles, setParagraphStyles),
        RT_PROP(C, 26, confidence, setConfidence),
        RT_PROP(C, 27, styleRuns, setStyleRuns),   // last: see the file comment
    });
    return s;
}

const Schema<PngPuppetClip>& puppetSchema()
{
    using C = PngPuppetClip;
    static const Schema<C> s("PngPuppetClip", {
        RT_PROP(C, 1, characterName, setCharacterName),
        RT_PROP(C, 2, variant, setVariant),
        field<C>(3, "facePaths",
            [](const C& c) {
                std::vector<std::string> faces;
                for (int f = 0; f < PngPuppetClip::FaceCount; ++f) faces.push_back(c.facePath(f));
                return faces;
            },
            [](C& c, std::vector<std::string> faces) {
                for (size_t f = 0; f < faces.size() && f < PngPuppetClip::FaceCount; ++f)
                    c.setFacePath(static_cast<int>(f), faces[f]);
            }),
        RT_PROP(C, 4, isTalking, setTalking),
        RT_PROP(C, 5, seed, setSeed),
        RT_PROP(C, 6, talkSwapSeconds, setTalkSwapSeconds),
        RT_PROP(C, 7, blinkIntervalSeconds, setBlinkIntervalSeconds),
        RT_PROP(C, 8, blinkDurationSeconds, setBlinkDurationSeconds),
        RT_PROP(C, 9, breathAmplitude, setBreathAmplitude),
        RT_PROP(C, 10, breathSpeed, setBreathSpeed),
        RT_PROP(C, 11, swayAmplitude, setSwayAmplitude),
    });
    return s;
}

const Schema<TextLayer>& textLayerSchema()
{
    using L = TextLayer;
    static const Schema<L> s("TextLayer", {
        RT_PROP(L, 1, text, setText),
        RT_PROP(L, 2, fontFamily, setFontFamily),
        RT_PROP(L, 3, fontSize, setFontSize),
        RT_PROP(L, 4, fontWeight, setFontWeight),
        RT_PROP(L, 5, isItalic, setItalic),
        RT_PROP(L, 6, allCaps, setAllCaps),
        RT_PROP(L, 7, smallCaps, setSmallCaps),
        RT_PROP(L, 8, alignment, setAlignment),
        RT_PROP(L, 9, vAlignment, setVAlignment),
        RT_TRACK(L, 10, tracking),
        RT_TRACK(L, 11, leading),
        RT_TRACK(L, 12, baselineShift),
        RT_PROP(L, 13, boxWidth, setBoxWidth),
        RT_PROP(L, 14, boxHeight, setBoxHeight),
        RT_PROP(L, 15, useParagraphBox, setUseParagraphBox),
        RT_PROP(L, 16, fontStyle, setFontStyleForAll),
        RT_PROP(L, 17, kerning, setKerningForAll),
        RT_PROP(L, 18, tabWidth, setTabWidthForAll),
        RT_PROP(L, 19, tsume, setTsumeForAll),
        field<L>(20, "fauxBold", [](const L& l) { return l.fauxBold(); },
            [](L& l, bool v) { l.setFauxStylesForAll(v, l.fauxItalic()); }),
        field<L>(21, "fauxItalic", [](const L& l) { return l.fauxItalic(); },
            [](L& l, bool v) { l.setFauxStylesForAll(l.fauxBold(), v); }),
        RT_PROP(L, 22, underline, setUnderlineForAll),
        field<L>(23, "superscript", [](const L& l) { return l.superscript(); },
            [](L& l, bool v) { l.setScriptForAll(v, l.subscript()); }),
        field<L>(24, "subscript", [](const L& l) { return l.subscript(); },
            [](L& l, bool v) { l.setScriptForAll(l.superscript(), v); }),
        RT_PROP(L, 25, rightToLeft, setRightToLeft),
        field<L>(26, "backgroundEnabled", [](const L& l) { return l.backgroundEnabled(); },
            [](L& l, bool v) {
                l.setBackgroundForAll(v, l.backgroundColor(), l.backgroundPadding());
            }),
        field<L>(27, "backgroundColor", [](const L& l) { return l.backgroundColor(); },
            [](L& l, uint32_t v) {
                l.setBackgroundForAll(l.backgroundEnabled(), v, l.backgroundPadding());
            }),
        field<L>(28, "backgroundPadding", [](const L& l) { return l.backgroundPadding(); },
            [](L& l, float v) {
                l.setBackgroundForAll(l.backgroundEnabled(), l.backgroundColor(), v);
            }),
        RT_PROP(L, 29, maskWithText, setMaskWithText),
        RT_PROP(L, 30, linkedStyleName, setLinkedStyleName),
        RT_PROP(L, 31, paragraphStyles, setParagraphStyles),
        RT_PROP(L, 32, styleRuns, setStyleRuns),   // last: see the file comment
    });
    return s;
}

const Schema<ShapeLayer>& shapeLayerSchema()
{
    using L = ShapeLayer;
    static const Schema<L> s("ShapeLayer", {
        RT_PROP(L, 1, shapeType, setShapeType),
        RT_PROP(L, 2, shapeWidth, setShapeWidth),
        RT_PROP(L, 3, shapeHeight, setShapeHeight),
        RT_PROP(L, 4, cornerRadius, setCornerRadius),
        RT_PROP(L, 5, fillColor, setFillColor),
    });
    return s;
}

/// A field holding a sub-record of a derived type's own schema, written only
/// when the object is that type.
template <class Base, class Derived, const Schema<Derived>& (*Get)(),
          bool (*Is)(const Base&)>
FieldDef<Base> subRecord(uint32_t id, const char* name)
{
    return when<Base>(Is, custom<Base>(id, name,
        [](RecordWriter& w, uint32_t fid, const Base& o) {
            RecordWriter n = w.nested();
            Get().write(n, static_cast<const Derived&>(o));
            w.writeRecord(fid, n);
        },
        [](RecordReader& r, Base& o) -> bool {
            if (!Is(o) || r.kind() != Kind::Bytes) return false;
            RecordReader child;
            if (!r.readRecord(child) || !Get().read(child, static_cast<Derived&>(o))) r.fail();
            return true;
        }));
}

bool isTextLayer(const GraphicLayer& l) { return l.layerType() == GraphicLayerType::Text; }
bool isShapeLayer(const GraphicLayer& l) { return l.layerType() == GraphicLayerType::Shape; }

const Schema<GraphicLayer>& layerSchema()
{
    using L = GraphicLayer;
    static const Schema<L> s("GraphicLayer", {
        custom<L>(1, "layerType",
            [](RecordWriter& w, uint32_t id, const L& l) { writeValue(w, id, l.layerType()); },
            // Read first, by the graphic clip, to create the layer.
            [](RecordReader&, L&) { return true; }),
        RT_PROP(L, 2, name, setName),
        RT_PROP(L, 3, isVisible, setVisible),
        RT_PROP(L, 4, isLocked, setLocked),
        member<L>(5, "posX", [](L& l) -> auto& { return l.transform().posX; }),
        member<L>(6, "posY", [](L& l) -> auto& { return l.transform().posY; }),
        member<L>(7, "scaleX", [](L& l) -> auto& { return l.transform().scaleX; }),
        member<L>(8, "scaleY", [](L& l) -> auto& { return l.transform().scaleY; }),
        member<L>(9, "rotation", [](L& l) -> auto& { return l.transform().rotation; }),
        member<L>(10, "anchorX", [](L& l) -> auto& { return l.transform().anchorX; }),
        member<L>(11, "anchorY", [](L& l) -> auto& { return l.transform().anchorY; }),
        member<L>(12, "opacity", [](L& l) -> auto& { return l.transform().opacity; }),
        member<L>(13, "fills", [](L& l) -> auto& { return l.appearance().fills; }),
        member<L>(14, "strokes", [](L& l) -> auto& { return l.appearance().strokes; }),
        member<L>(15, "shadows", [](L& l) -> auto& { return l.appearance().shadows; }),
        subRecord<L, TextLayer, textLayerSchema, isTextLayer>(16, "text"),
        subRecord<L, ShapeLayer, shapeLayerSchema, isShapeLayer>(17, "shape"),
    });
    return s;
}

const Schema<GraphicClip>& graphicSchema()
{
    using C = GraphicClip;
    static const Schema<C> s("GraphicClip", {
        custom<C>(1, "layers",
            [](RecordWriter& w, uint32_t id, const C& c) {
                RecordWriter list = w.nested();
                for (size_t i = 0; i < c.layerCount(); ++i) {
                    RecordWriter n = list.nested();
                    layerSchema().write(n, *c.layer(i));
                    list.writeRecord(detail::kListElement, n);
                }
                w.writeRecord(id, list);
            },
            [](RecordReader& r, C& c) -> bool {
                RecordReader list;
                if (r.kind() != Kind::Bytes) return false;
                if (!r.readRecord(list)) { r.fail(); return true; }
                while (c.layerCount() > 0) (void)c.removeLayer(c.layerCount() - 1);
                while (list.next()) {
                    RecordReader item;
                    if (list.id() != detail::kListElement || !list.readRecord(item)) continue;
                    RecordReader scan = item;
                    GraphicLayerType type = GraphicLayerType::Text;
                    bool known = false;
                    while (scan.next())
                        if (scan.id() == 1 && readValue(scan, type)) { known = true; break; }
                    std::unique_ptr<GraphicLayer> layer;
                    if (known && type == GraphicLayerType::Text)
                        layer = std::make_unique<TextLayer>();
                    else if (known && type == GraphicLayerType::Shape)
                        layer = std::make_unique<ShapeLayer>();
                    else
                        continue;   // a layer kind this build doesn't know
                    if (!layerSchema().read(item, *layer)) { list.fail(); break; }
                    c.addLayer(std::move(layer));
                }
                if (!list.ok()) r.fail();
                return true;
            }),
    });
    return s;
}

const Schema<TierListClip>& tierListSchema()
{
    using C = TierListClip;
    static const Schema<C> s("TierListClip", {
        RT_PROP(C, 1, title, setTitle),
        field<C>(2, "entryAspect", [](const C& c) { return c.entryAspect(); },
            [](C& c, TierEntryAspect v) {
                c.setEntryAspect(static_cast<uint8_t>(v) <= static_cast<uint8_t>(TierEntryAspect::Custom)
                    ? v : TierEntryAspect::Banner);
            }),
        field<C>(3, "customAspectW", [](const C& c) { return c.customAspectW(); },
            [](C& c, float v) { c.setCustomAspect(v, c.customAspectH()); }),
        field<C>(4, "customAspectH", [](const C& c) { return c.customAspectH(); },
            [](C& c, float v) { c.setCustomAspect(c.customAspectW(), v); }),
        RT_PROP(C, 5, topSafeMargin, setTopSafeMargin),
        RT_PROP(C, 6, rightSafeMargin, setRightSafeMargin),
        RT_PROP(C, 7, backgroundColor, setBackgroundColor),
        member<C>(8, "tiers", [](C& c) -> auto& { return c.tiers(); }),
        member<C>(9, "subbins", [](C& c) -> auto& { return c.subbins(); }),
        member<C>(10, "entries", [](C& c) -> auto& { return c.entries(); }),
        member<C>(11, "events", [](C& c) -> auto& { return c.events(); }),
    });
    return s;
}

// ── The clip record ─────────────────────────────────────────────────────

template <ClipType T>
bool isType(const Clip& c) { return c.clipType() == T; }

const Schema<Clip>& clipSchema()
{
    using C = Clip;
    static const Schema<C> s("Clip", {
        custom<C>(1, "type",
            [](RecordWriter& w, uint32_t id, const C& c) { writeValue(w, id, c.clipType()); },
            // Read first, by readClip, to create the clip.
            [](RecordReader&, C&) { return true; }),
        field<C>(2, "id", [](const C& c) { return c.id(); },
            [](C& c, uint64_t v) {
                // Transitions and A/V links reference clips by id: keep it,
                // and keep later clips from reusing it.
                if (v == 0) return;
                c.setId(v);
                Clip::reserveId(v);
            }),
        RT_PROP(C, 3, label, setLabel),
        RT_PROP(C, 4, color, setColor),
        RT_PROP(C, 5, isEnabled, setEnabled),
        RT_PROP(C, 6, timelineIn, setTimelineIn),
        RT_PROP(C, 7, duration, setDuration),
        RT_PROP(C, 8, sourceIn, setSourceIn),
        RT_PROP(C, 9, speed, setSpeed),
        RT_PROP(C, 10, groupId, setGroupId),
        RT_PROP(C, 11, shotName, setShotName),
        RT_PROP(C, 12, layerId, setLayerId),
        RT_TRACK(C, 13, opacity),
        RT_TRACK(C, 14, positionX),
        RT_TRACK(C, 15, positionY),
        RT_TRACK(C, 16, scaleX),
        RT_TRACK(C, 17, scaleY),
        RT_TRACK(C, 18, rotation),
        RT_TRACK(C, 19, speedRamp),
        RT_TRACK(C, 20, anchorX),
        RT_TRACK(C, 21, anchorY),
        RT_TRACK(C, 22, shutterAngle),
        RT_PROP(C, 23, blendMode, setBlendMode),
        RT_PROP(C, 24, maintainPitch, setMaintainPitch),
        RT_PROP(C, 25, syncLine, setSyncLine),
        field<C>(26, "timeInterpolation", [](const C& c) { return c.timeInterpolation(); },
            [](C& c, TimeInterpolation v) {
                if (static_cast<uint8_t>(v) <= static_cast<uint8_t>(TimeInterpolation::OpticalFlow))
                    c.setTimeInterpolation(v);
            }),
        RT_PROP(C, 27, linkId, setLinkId),
        custom<C>(28, "effects",
            [](RecordWriter& w, uint32_t id, const C& c) {
                RecordWriter list = w.nested();
                for (size_t i = 0; i < c.effects().effectCount(); ++i) {
                    RecordWriter n = list.nested();
                    writeEffect(n, c.effects().effect(i));
                    list.writeRecord(detail::kListElement, n);
                }
                w.writeRecord(id, list);
            },
            [](RecordReader& r, C& c) -> bool {
                RecordReader list;
                if (r.kind() != Kind::Bytes) return false;
                if (!r.readRecord(list)) { r.fail(); return true; }
                while (c.effects().effectCount() > 0)
                    (void)c.effects().removeEffect(c.effects().effectCount() - 1);
                while (list.next()) {
                    RecordReader item;
                    if (list.id() != detail::kListElement || !list.readRecord(item)) continue;
                    if (auto effect = readEffect(item))
                        c.effects().addEffect(std::move(effect));
                    else if (!item.ok())
                        list.fail();
                }
                if (!list.ok()) r.fail();
                return true;
            }),
        member<C>(29, "masks", [](C& c) -> auto& { return c.masks(); }),
        subRecord<C, SpineClip, spineSchema, isType<ClipType::Spine>>(30, "spine"),
        subRecord<C, VideoClip, videoSchema, isType<ClipType::Video>>(31, "video"),
        subRecord<C, AudioClip, audioSchema, isType<ClipType::Audio>>(32, "audio"),
        subRecord<C, AdjustmentClip, adjustmentSchema, isType<ClipType::Adjustment>>(33, "adjustment"),
        subRecord<C, ImageClip, imageSchema, isType<ClipType::Image>>(34, "image"),
        subRecord<C, SequenceClip, sequenceSchema, isType<ClipType::Sequence>>(35, "sequence"),
        subRecord<C, CaptionClip, captionSchema, isType<ClipType::Caption>>(36, "caption"),
        subRecord<C, PngPuppetClip, puppetSchema, isType<ClipType::PngPuppet>>(37, "pngPuppet"),
        subRecord<C, GraphicClip, graphicSchema, isType<ClipType::Graphic>>(38, "graphic"),
        subRecord<C, TierListClip, tierListSchema, isType<ClipType::TierList>>(39, "tierList"),
    });
    return s;
}

std::unique_ptr<Clip> makeClip(ClipType type)
{
    switch (type) {
    case ClipType::Spine:      return std::make_unique<SpineClip>();
    case ClipType::Video:      return std::make_unique<VideoClip>();
    case ClipType::Audio:      return std::make_unique<AudioClip>();
    case ClipType::Adjustment: return std::make_unique<AdjustmentClip>();
    case ClipType::Image:      return std::make_unique<ImageClip>();
    case ClipType::Sequence:   return std::make_unique<SequenceClip>();
    case ClipType::Caption:    return std::make_unique<CaptionClip>();
    case ClipType::PngPuppet:  return std::make_unique<PngPuppetClip>();
    case ClipType::Graphic:    return std::make_unique<GraphicClip>();
    case ClipType::TierList:   return std::make_unique<TierListClip>();
    default:                   return nullptr;   // Title: loads as Graphic, never saved
    }
}

} // namespace

void writeClip(RecordWriter& w, const Clip& clip)
{
    clipSchema().write(w, clip);
}

std::unique_ptr<Clip> readClip(RecordReader& r)
{
    RecordReader scan = r;
    std::unique_ptr<Clip> clip;
    bool sawType = false;
    while (scan.next()) {
        ClipType type{};
        if (scan.id() == 1 && readValue(scan, type)) {
            sawType = true;
            clip = makeClip(type);
            if (!clip)
                spdlog::warn("Project: left out a clip of unknown type {}",
                             static_cast<int>(type));
            break;
        }
    }
    if (!scan.ok()) { r.fail(); return nullptr; }
    if (!clip) {
        if (!sawType) spdlog::warn("Project: left out a clip with no type");
        return nullptr;
    }
    if (!clipSchema().read(r, *clip)) return nullptr;
    return clip;
}

std::vector<uint8_t> clipBytes(const Clip& clip)
{
    RecordWriter w;
    writeClip(w, clip);
    return w.take();
}

#undef RT_TRACK
#undef RT_PROP
#undef RT_MEMBER

} // namespace rt::record
