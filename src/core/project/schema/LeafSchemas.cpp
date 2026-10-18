/*
 * LeafSchemas.cpp — see LeafSchemas.h. Field ids are append-only: add new
 * fields at the end of a table with the next id, never renumber.
 */

#include "project/schema/LeafSchemas.h"

#include "audiofx/FxChain.h"
#include "effects/BeatEffects.h"
#include "effects/Effect.h"
#include "effects/LUT.h"

#include <limits>
#include <utility>
#include <vector>

namespace rt::record {

// A plain struct member, read and written in place.
#define RT_MEMBER(Type, id, name) \
    member<Type>(id, #name, [](Type& o) -> auto& { return o.name; })

// ── Keyframes ───────────────────────────────────────────────────────────

const Schema<Keyframe<float>>& SchemaOf<Keyframe<float>>::get()
{
    using K = Keyframe<float>;
    static const Schema<K> s("Keyframe", {
        RT_MEMBER(K, 1, time),
        RT_MEMBER(K, 2, value),
        RT_MEMBER(K, 3, interp),
        RT_MEMBER(K, 4, bezierInX),
        RT_MEMBER(K, 5, bezierInY),
        RT_MEMBER(K, 6, bezierOutX),
        RT_MEMBER(K, 7, bezierOutY),
        RT_MEMBER(K, 8, spatialInterp),
        RT_MEMBER(K, 9, spatialInX),
        RT_MEMBER(K, 10, spatialInY),
        RT_MEMBER(K, 11, spatialOutX),
        RT_MEMBER(K, 12, spatialOutY),
    });
    return s;
}

const Schema<KeyframeTrack<float>>& SchemaOf<KeyframeTrack<float>>::get()
{
    using T = KeyframeTrack<float>;
    static const Schema<T> s("KeyframeTrack", {
        field<T>(1, "defaultValue",
            [](const T& t) { return t.defaultValue(); },
            [](T& t, float v) { t.setDefaultValue(v); }),
        field<T>(2, "keyframes",
            [](const T& t) -> const std::vector<Keyframe<float>>& { return t.keyframes(); },
            [](T& t, std::vector<Keyframe<float>> keys) {
                while (t.keyframeCount() > 0) t.removeKeyframe(t.keyframeCount() - 1);
                for (const auto& k : keys) t.restoreKeyframe(k);
            }),
    });
    return s;
}

// ── Masks ───────────────────────────────────────────────────────────────

const Schema<MaskVertex>& SchemaOf<MaskVertex>::get()
{
    static const Schema<MaskVertex> s("MaskVertex", {
        RT_MEMBER(MaskVertex, 1, x),
        RT_MEMBER(MaskVertex, 2, y),
        RT_MEMBER(MaskVertex, 3, inTanX),
        RT_MEMBER(MaskVertex, 4, inTanY),
        RT_MEMBER(MaskVertex, 5, outTanX),
        RT_MEMBER(MaskVertex, 6, outTanY),
    });
    return s;
}

const Schema<MaskGeometry>& SchemaOf<MaskGeometry>::get()
{
    static const Schema<MaskGeometry> s("MaskGeometry", {
        RT_MEMBER(MaskGeometry, 1, centerX),
        RT_MEMBER(MaskGeometry, 2, centerY),
        RT_MEMBER(MaskGeometry, 3, width),
        RT_MEMBER(MaskGeometry, 4, height),
        RT_MEMBER(MaskGeometry, 5, rotation),
        RT_MEMBER(MaskGeometry, 6, vertices),
    });
    return s;
}

const Schema<MaskPathKeyframe>& SchemaOf<MaskPathKeyframe>::get()
{
    static const Schema<MaskPathKeyframe> s("MaskPathKeyframe", {
        RT_MEMBER(MaskPathKeyframe, 1, time),
        RT_MEMBER(MaskPathKeyframe, 2, geometry),
    });
    return s;
}

const Schema<OpacityMask>& SchemaOf<OpacityMask>::get()
{
    using M = OpacityMask;
    static const Schema<M> s("OpacityMask", {
        RT_MEMBER(M, 1, shape),
        RT_MEMBER(M, 2, inverted),
        RT_MEMBER(M, 3, name),
        RT_MEMBER(M, 4, base),
        RT_MEMBER(M, 5, pathAnimated),
        RT_MEMBER(M, 6, pathKeys),
        RT_MEMBER(M, 7, feather),
        RT_MEMBER(M, 8, maskOpacity),
        RT_MEMBER(M, 9, expansion),
        field<M>(10, "coordinateSpace",
            [](const M& m) { return m.coordinateSpace; },
            [](M& m, MaskCoordinateSpace v) {
                // An unknown space (damaged file) falls back to clip-local.
                m.coordinateSpace = static_cast<uint8_t>(v)
                        <= static_cast<uint8_t>(MaskCoordinateSpace::LegacySequenceFrame)
                    ? v : MaskCoordinateSpace::SourceLocal;
            }),
        field<M>(11, "maskId",
            [](const M& m) { return m.maskId; },
            [](M& m, uint64_t v) {
                // Keep the saved id (effect targets reference it) and stop
                // new masks from reusing it.
                if (v == 0) return;
                m.maskId = v;
                reserveOpacityMaskId(v);
            }),
        RT_MEMBER(M, 12, unassigned),
    });
    return s;
}

// ── Rich text ───────────────────────────────────────────────────────────

const Schema<TextRunAppearance>& SchemaOf<TextRunAppearance>::get()
{
    using A = TextRunAppearance;
    static const Schema<A> s("TextRunAppearance", {
        RT_MEMBER(A, 1, fillEnabled),
        RT_MEMBER(A, 2, fillColor),
        RT_MEMBER(A, 3, strokeEnabled),
        RT_MEMBER(A, 4, strokeColor),
        RT_MEMBER(A, 5, strokeWidth),
        RT_MEMBER(A, 6, strokePosition),
        RT_MEMBER(A, 7, shadowEnabled),
        RT_MEMBER(A, 8, shadowColor),
        RT_MEMBER(A, 9, shadowDistance),
        RT_MEMBER(A, 10, shadowAngle),
        RT_MEMBER(A, 11, shadowSoftness),
        RT_MEMBER(A, 12, shadowOpacity),
        RT_MEMBER(A, 13, backgroundEnabled),
        RT_MEMBER(A, 14, backgroundColor),
        RT_MEMBER(A, 15, backgroundPadding),
    });
    return s;
}

const Schema<TextStyleRun>& SchemaOf<TextStyleRun>::get()
{
    using R = TextStyleRun;
    static const Schema<R> s("TextStyleRun", {
        RT_MEMBER(R, 1, start),
        RT_MEMBER(R, 2, length),
        RT_MEMBER(R, 3, fontFamily),
        RT_MEMBER(R, 4, fontSize),
        RT_MEMBER(R, 5, fontWeight),
        RT_MEMBER(R, 6, italic),
        RT_MEMBER(R, 7, allCaps),
        RT_MEMBER(R, 8, smallCaps),
        RT_MEMBER(R, 9, tracking),
        RT_MEMBER(R, 10, baselineShift),
        RT_MEMBER(R, 11, overrideMask),
        RT_MEMBER(R, 12, leading),
        RT_MEMBER(R, 13, fontStyle),
        RT_MEMBER(R, 14, kerning),
        RT_MEMBER(R, 15, tabWidth),
        RT_MEMBER(R, 16, tsume),
        RT_MEMBER(R, 17, fauxBold),
        RT_MEMBER(R, 18, fauxItalic),
        RT_MEMBER(R, 19, underline),
        RT_MEMBER(R, 20, superscript),
        RT_MEMBER(R, 21, subscript),
        RT_MEMBER(R, 22, appearance),
    });
    return s;
}

const Schema<TextParagraphStyle>& SchemaOf<TextParagraphStyle>::get()
{
    using P = TextParagraphStyle;
    static const Schema<P> s("TextParagraphStyle", {
        RT_MEMBER(P, 1, start),
        RT_MEMBER(P, 2, length),
        RT_MEMBER(P, 3, alignment),
        RT_MEMBER(P, 4, rightToLeft),
    });
    return s;
}

// ── Graphic appearance entries ──────────────────────────────────────────

const Schema<FillEntry>& SchemaOf<FillEntry>::get()
{
    static const Schema<FillEntry> s("FillEntry", {
        RT_MEMBER(FillEntry, 1, color),
        RT_MEMBER(FillEntry, 2, opacity),
        RT_MEMBER(FillEntry, 3, enabled),
    });
    return s;
}

const Schema<StrokeEntry>& SchemaOf<StrokeEntry>::get()
{
    static const Schema<StrokeEntry> s("StrokeEntry", {
        RT_MEMBER(StrokeEntry, 1, color),
        RT_MEMBER(StrokeEntry, 2, width),
        RT_MEMBER(StrokeEntry, 3, position),
        RT_MEMBER(StrokeEntry, 4, opacity),
        RT_MEMBER(StrokeEntry, 5, enabled),
    });
    return s;
}

const Schema<ShadowEntry>& SchemaOf<ShadowEntry>::get()
{
    static const Schema<ShadowEntry> s("ShadowEntry", {
        RT_MEMBER(ShadowEntry, 1, color),
        RT_MEMBER(ShadowEntry, 2, distance),
        RT_MEMBER(ShadowEntry, 3, angle),
        RT_MEMBER(ShadowEntry, 4, softness),
        RT_MEMBER(ShadowEntry, 5, opacity),
        RT_MEMBER(ShadowEntry, 6, enabled),
    });
    return s;
}

// ── Sequence pieces ─────────────────────────────────────────────────────

const Schema<Transition>& SchemaOf<Transition>::get()
{
    static const Schema<Transition> s("Transition", {
        RT_MEMBER(Transition, 1, type),
        RT_MEMBER(Transition, 2, duration),
        RT_MEMBER(Transition, 3, offset),
        RT_MEMBER(Transition, 4, param1),
        RT_MEMBER(Transition, 5, param2),
        RT_MEMBER(Transition, 6, leftClipId),
        RT_MEMBER(Transition, 7, rightClipId),
        RT_MEMBER(Transition, 8, editPointTick),
    });
    return s;
}

const Schema<Marker>& SchemaOf<Marker>::get()
{
    static const Schema<Marker> s("Marker", {
        RT_MEMBER(Marker, 1, time),
        RT_MEMBER(Marker, 2, label),
        RT_MEMBER(Marker, 3, color),
    });
    return s;
}

const Schema<Settings>& SchemaOf<Settings>::get()
{
    using S = Settings;
    static const Schema<S> s("Settings", {
        field<S>(1, "width",
            [](const S& o) { return o.resolution().width; },
            [](S& o, uint32_t v) { o.setResolution(v, o.resolution().height); }),
        field<S>(2, "height",
            [](const S& o) { return o.resolution().height; },
            [](S& o, uint32_t v) { o.setResolution(o.resolution().width, v); }),
        field<S>(3, "frameRate",
            [](const S& o) { return o.frameRate(); },
            [](S& o, double v) { o.setFrameRate(v); }),
        field<S>(4, "colorSpace",
            [](const S& o) { return o.colorSpace(); },
            [](S& o, ColorSpace v) { o.setColorSpace(v); }),
        field<S>(5, "sampleRate",
            [](const S& o) { return o.audioFormat().sampleRate; },
            [](S& o, uint32_t v) { auto a = o.audioFormat(); a.sampleRate = v; o.setAudioFormat(a); }),
        field<S>(6, "bitDepth",
            [](const S& o) { return o.audioFormat().bitDepth; },
            [](S& o, uint32_t v) { auto a = o.audioFormat(); a.bitDepth = v; o.setAudioFormat(a); }),
        field<S>(7, "channels",
            [](const S& o) { return o.audioFormat().channels; },
            [](S& o, uint32_t v) { auto a = o.audioFormat(); a.channels = v; o.setAudioFormat(a); }),
        member<S>(8, "exportCodec", [](S& o) -> auto& { return o.exportSettings().codec; }),
        member<S>(9, "exportQuality", [](S& o) -> auto& { return o.exportSettings().quality; }),
        member<S>(10, "exportAudioBitrate",
                  [](S& o) -> auto& { return o.exportSettings().audioBitrate; }),
        member<S>(11, "exportOutputPath",
                  [](S& o) -> auto& { return o.exportSettings().outputPath; }),
    });
    return s;
}

// ── Effects ─────────────────────────────────────────────────────────────

const Schema<ColorGrading::HslParams>& SchemaOf<ColorGrading::HslParams>::get()
{
    using H = ColorGrading::HslParams;
    static const Schema<H> s("HslParams", {
        RT_MEMBER(H, 1, hueCenter),
        RT_MEMBER(H, 2, hueWidth),
        RT_MEMBER(H, 3, satMin),
        RT_MEMBER(H, 4, satMax),
        RT_MEMBER(H, 5, lumMin),
        RT_MEMBER(H, 6, lumMax),
        RT_MEMBER(H, 7, hueShift),
        RT_MEMBER(H, 8, satAdjust),
        RT_MEMBER(H, 9, lumAdjust),
    });
    return s;
}

namespace {

bool isLut(const Effect& e) { return e.effectType() == EffectType::LUT; }
bool isBeat(const Effect& e) { return isBeatReactEffect(e.effectType()); }
bool isGrading(const Effect& e) { return e.effectType() == EffectType::ColorGrading; }

const ColorGrading& grading(const Effect& e) { return static_cast<const ColorGrading&>(e); }
ColorGrading& grading(Effect& e) { return static_cast<ColorGrading&>(e); }

const Schema<Effect>& effectSchema()
{
    using E = Effect;
    static const Schema<E> s("Effect", {
        custom<E>(1, "type",
            [](RecordWriter& w, uint32_t id, const E& e) {
                writeValue(w, id, e.effectType());
            },
            // Read first, by readEffect, to create the effect.
            [](RecordReader&, E&) { return true; }),
        field<E>(2, "enabled",
            [](const E& e) { return e.isEnabled(); },
            [](E& e, bool v) { e.setEnabled(v); }),
        custom<E>(3, "params",
            // Parameter values by registry index (EffectRegistry: append-only).
            [](RecordWriter& w, uint32_t id, const E& e) {
                RecordWriter list = w.nested();
                for (size_t i = 0; i < e.paramCount(); ++i)
                    writeValue(list, detail::kListElement, e.param(i).track);
                w.writeRecord(id, list);
            },
            [](RecordReader& r, E& e) -> bool {
                RecordReader list;
                if (r.kind() != Kind::Bytes) return false;
                if (!r.readRecord(list)) { r.fail(); return true; }
                size_t index = 0;
                while (list.next()) {
                    if (list.id() != detail::kListElement) continue;
                    if (index < e.paramCount()) {
                        (void)readValue(list, e.param(index).track);
                    }   // a parameter this build doesn't have: dropped
                    ++index;
                }
                if (!list.ok()) r.fail();
                return true;
            }),
        member<E>(4, "masks", [](E& e) -> auto& { return e.masks(); }),
        when<E>(isLut, field<E>(5, "lutPath",
            [](const E& e) { return static_cast<const LUT&>(e).lutPath(); },
            [](E& e, std::string path) {
                if (!path.empty()) static_cast<LUT&>(e).loadCubeFile(path);
            })),
        when<E>(isBeat, field<E>(6, "beatAudioSourceId",
            [](const E& e) { return static_cast<const BeatReactEffect&>(e).audioSourceId(); },
            [](E& e, uint64_t v) { static_cast<BeatReactEffect&>(e).setAudioSourceId(v); })),
        when<E>(isBeat, field<E>(7, "beatTimes",
            [](const E& e) -> const std::vector<float>& {
                return static_cast<const BeatReactEffect&>(e).beatTimes();
            },
            [](E& e, std::vector<float> v) {
                static_cast<BeatReactEffect&>(e).setBeatTimes(std::move(v));
            })),
        when<E>(isGrading, field<E>(8, "gradingBasicEnabled",
            [](const E& e) { return grading(e).basicEnabled(); },
            [](E& e, bool v) { grading(e).setBasicEnabled(v); })),
        when<E>(isGrading, field<E>(9, "gradingCreativeEnabled",
            [](const E& e) { return grading(e).creativeEnabled(); },
            [](E& e, bool v) { grading(e).setCreativeEnabled(v); })),
        when<E>(isGrading, field<E>(10, "gradingWheelsEnabled",
            [](const E& e) { return grading(e).wheelsEnabled(); },
            [](E& e, bool v) { grading(e).setWheelsEnabled(v); })),
        when<E>(isGrading, field<E>(11, "gradingCurvesEnabled",
            [](const E& e) { return grading(e).curvesEnabled(); },
            [](E& e, bool v) { grading(e).setCurvesEnabled(v); })),
        when<E>(isGrading, field<E>(12, "gradingHslEnabled",
            [](const E& e) { return grading(e).hslEnabled(); },
            [](E& e, bool v) { grading(e).setHslEnabled(v); })),
        when<E>(isGrading, field<E>(13, "gradingVignetteEnabled",
            [](const E& e) { return grading(e).vignetteEnabled(); },
            [](E& e, bool v) { grading(e).setVignetteEnabled(v); })),
        when<E>(isGrading, field<E>(14, "gradingHsl",
            [](const E& e) -> const ColorGrading::HslParams& { return grading(e).hslParams(); },
            [](E& e, ColorGrading::HslParams v) { grading(e).setHslParams(v); })),
        when<E>(isGrading, field<E>(15, "gradingCurves",
            [](const E& e) {
                std::vector<ColorGrading::CurveLUT> curves;
                for (int ch = 0; ch < ColorGrading::CurveCount; ++ch)
                    curves.push_back(grading(e).curveLUT(
                        static_cast<ColorGrading::CurveChannel>(ch)));
                return curves;
            },
            [](E& e, std::vector<ColorGrading::CurveLUT> curves) {
                for (size_t ch = 0; ch < curves.size() && ch < ColorGrading::CurveCount; ++ch)
                    grading(e).setCurveLUT(static_cast<ColorGrading::CurveChannel>(ch),
                                           curves[ch]);
            })),
    });
    return s;
}

} // namespace

void writeEffect(RecordWriter& w, const Effect& effect)
{
    effectSchema().write(w, effect);
}

std::unique_ptr<Effect> readEffect(RecordReader& r)
{
    // The type decides what to create; find it before applying the rest.
    RecordReader scan = r;
    std::unique_ptr<Effect> effect;
    while (scan.next()) {
        EffectType type{};
        if (scan.id() == 1 && readValue(scan, type)) {
            if (static_cast<uint32_t>(type) < static_cast<uint32_t>(EffectType::Count))
                effect = createEffect(type);
            break;
        }
    }
    if (!scan.ok()) { r.fail(); return nullptr; }
    if (!effect) return nullptr;
    if (!effectSchema().read(r, *effect)) return nullptr;
    return effect;
}

// ── Audio FX chain ──────────────────────────────────────────────────────

const Schema<audiofx::ParametricEQ::Band>& SchemaOf<audiofx::ParametricEQ::Band>::get()
{
    using B = audiofx::ParametricEQ::Band;
    static const Schema<B> s("EqBand", {
        RT_MEMBER(B, 1, enabled),
        RT_MEMBER(B, 2, type),
        RT_MEMBER(B, 3, freqHz),
        RT_MEMBER(B, 4, gainDb),
        RT_MEMBER(B, 5, q),
    });
    return s;
}

const Schema<audiofx::Dynamics::GateSettings>& SchemaOf<audiofx::Dynamics::GateSettings>::get()
{
    using G = audiofx::Dynamics::GateSettings;
    static const Schema<G> s("GateSettings", {
        RT_MEMBER(G, 1, enabled),
        RT_MEMBER(G, 2, thresholdDb),
        RT_MEMBER(G, 3, ratio),
        RT_MEMBER(G, 4, rangeDb),
        RT_MEMBER(G, 5, attackMs),
        RT_MEMBER(G, 6, releaseMs),
    });
    return s;
}

const Schema<audiofx::Dynamics::CompSettings>& SchemaOf<audiofx::Dynamics::CompSettings>::get()
{
    using C = audiofx::Dynamics::CompSettings;
    static const Schema<C> s("CompSettings", {
        RT_MEMBER(C, 1, enabled),
        RT_MEMBER(C, 2, thresholdDb),
        RT_MEMBER(C, 3, ratio),
        RT_MEMBER(C, 4, kneeDb),
        RT_MEMBER(C, 5, makeupDb),
        RT_MEMBER(C, 6, attackMs),
        RT_MEMBER(C, 7, releaseMs),
    });
    return s;
}

const Schema<audiofx::Dynamics::LimiterSettings>&
SchemaOf<audiofx::Dynamics::LimiterSettings>::get()
{
    using L = audiofx::Dynamics::LimiterSettings;
    static const Schema<L> s("LimiterSettings", {
        RT_MEMBER(L, 1, enabled),
        RT_MEMBER(L, 2, ceilingDb),
        RT_MEMBER(L, 3, releaseMs),
    });
    return s;
}

namespace {

using audiofx::AudioProcessor;
using audiofx::Dynamics;
using audiofx::ParametricEQ;
using audiofx::ProcessorKind;

const Schema<ParametricEQ>& eqSchema()
{
    using Q = ParametricEQ;
    static const Schema<Q> s("ParametricEQ", {
        field<Q>(1, "highPassOn", [](const Q& q) { return q.highPassOn(); },
            [](Q& q, bool v) { q.setHighPass(v, q.highPassFreq(), q.highPassQ()); }),
        field<Q>(2, "highPassFreq", [](const Q& q) { return q.highPassFreq(); },
            [](Q& q, float v) { q.setHighPass(q.highPassOn(), v, q.highPassQ()); }),
        field<Q>(3, "highPassQ", [](const Q& q) { return q.highPassQ(); },
            [](Q& q, float v) { q.setHighPass(q.highPassOn(), q.highPassFreq(), v); }),
        field<Q>(4, "lowPassOn", [](const Q& q) { return q.lowPassOn(); },
            [](Q& q, bool v) { q.setLowPass(v, q.lowPassFreq(), q.lowPassQ()); }),
        field<Q>(5, "lowPassFreq", [](const Q& q) { return q.lowPassFreq(); },
            [](Q& q, float v) { q.setLowPass(q.lowPassOn(), v, q.lowPassQ()); }),
        field<Q>(6, "lowPassQ", [](const Q& q) { return q.lowPassQ(); },
            [](Q& q, float v) { q.setLowPass(q.lowPassOn(), q.lowPassFreq(), v); }),
        field<Q>(7, "bands",
            [](const Q& q) {
                std::vector<ParametricEQ::Band> bands;
                for (int b = 0; b < q.bandCount(); ++b) bands.push_back(q.band(b));
                return bands;
            },
            [](Q& q, std::vector<ParametricEQ::Band> bands) {
                for (size_t b = 0; b < bands.size() && static_cast<int>(b) < q.bandCount(); ++b)
                    q.setBand(static_cast<int>(b), bands[b]);
            }),
        field<Q>(8, "outputGainDb", [](const Q& q) { return q.outputGainDb(); },
            [](Q& q, float v) { q.setOutputGainDb(v); }),
    });
    return s;
}

const Schema<Dynamics>& dynamicsSchema()
{
    using D = Dynamics;
    static const Schema<D> s("Dynamics", {
        field<D>(1, "gate", [](const D& d) -> const D::GateSettings& { return d.gate(); },
            [](D& d, D::GateSettings v) { d.setGate(v); }),
        field<D>(2, "compressor", [](const D& d) -> const D::CompSettings& { return d.compressor(); },
            [](D& d, D::CompSettings v) { d.setCompressor(v); }),
        field<D>(3, "limiter", [](const D& d) -> const D::LimiterSettings& { return d.limiter(); },
            [](D& d, D::LimiterSettings v) { d.setLimiter(v); }),
    });
    return s;
}

bool isEq(const AudioProcessor& p) { return dynamic_cast<const ParametricEQ*>(&p) != nullptr; }
bool isDynamics(const AudioProcessor& p) { return dynamic_cast<const Dynamics*>(&p) != nullptr; }

ProcessorKind kindOf(const AudioProcessor& p)
{
    return isEq(p) ? ProcessorKind::ParametricEQ : ProcessorKind::Dynamics;
}

const Schema<AudioProcessor>& processorSchema()
{
    using A = AudioProcessor;
    static const Schema<A> s("AudioProcessor", {
        custom<A>(1, "kind",
            [](RecordWriter& w, uint32_t id, const A& p) {
                w.writeVarint(id, static_cast<uint64_t>(kindOf(p)));
            },
            // Read first, by readFxChain, to create the processor.
            [](RecordReader&, A&) { return true; }),
        field<A>(2, "enabled", [](const A& p) { return p.isEnabled(); },
            [](A& p, bool v) { p.setEnabled(v); }),
        when<A>(isEq, custom<A>(3, "eq",
            [](RecordWriter& w, uint32_t id, const A& p) {
                RecordWriter n = w.nested();
                eqSchema().write(n, static_cast<const ParametricEQ&>(p));
                w.writeRecord(id, n);
            },
            [](RecordReader& r, A& p) -> bool {
                auto* eq = dynamic_cast<ParametricEQ*>(&p);
                RecordReader child;
                if (!eq || r.kind() != Kind::Bytes) return false;
                if (!r.readRecord(child) || !eqSchema().read(child, *eq)) r.fail();
                return true;
            })),
        when<A>(isDynamics, custom<A>(4, "dynamics",
            [](RecordWriter& w, uint32_t id, const A& p) {
                RecordWriter n = w.nested();
                dynamicsSchema().write(n, static_cast<const Dynamics&>(p));
                w.writeRecord(id, n);
            },
            [](RecordReader& r, A& p) -> bool {
                auto* dyn = dynamic_cast<Dynamics*>(&p);
                RecordReader child;
                if (!dyn || r.kind() != Kind::Bytes) return false;
                if (!r.readRecord(child) || !dynamicsSchema().read(child, *dyn)) r.fail();
                return true;
            })),
    });
    return s;
}

} // namespace

void writeFxChain(RecordWriter& w, uint32_t id, const audiofx::FxChain& chain)
{
    RecordWriter list = w.nested();
    for (size_t i = 0; i < chain.size(); ++i) {
        const AudioProcessor& p = chain.at(i);
        if (!isEq(p) && !isDynamics(p)) continue;   // nothing to save it as
        RecordWriter n = list.nested();
        processorSchema().write(n, p);
        list.writeRecord(detail::kListElement, n);
    }
    w.writeRecord(id, list);
}

bool readFxChain(RecordReader& r, audiofx::FxChain& chain)
{
    RecordReader list;
    if (r.kind() != Kind::Bytes) return false;
    if (!r.readRecord(list)) { r.fail(); return true; }
    chain.clear();
    while (list.next()) {
        RecordReader item;
        if (list.id() != detail::kListElement || !list.readRecord(item)) continue;
        RecordReader scan = item;
        uint64_t kind = std::numeric_limits<uint64_t>::max();
        while (scan.next())
            if (scan.id() == 1 && scan.readVarint(kind)) break;
        if (kind != static_cast<uint64_t>(ProcessorKind::ParametricEQ)
            && kind != static_cast<uint64_t>(ProcessorKind::Dynamics))
            continue;   // a kind this build doesn't know
        AudioProcessor* p = chain.add(static_cast<ProcessorKind>(kind));
        if (p && !processorSchema().read(item, *p)) list.fail();
    }
    if (!list.ok()) r.fail();
    return true;
}

#undef RT_MEMBER

} // namespace rt::record
