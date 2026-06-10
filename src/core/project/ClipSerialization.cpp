/*
 * ClipSerialization.cpp — Per-clip binary serialization helpers.
 *
 * Extracted from ProjectSerializer.cpp.  Used by both serialize() and
 * deserialize() in ProjectSerializer.cpp via ClipSerialization.h.
 */

#include "project/ClipSerialization.h"
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
#include "timeline/KeyframeTrack.h"
#include "effects/Effect.h"
#include "effects/EffectStack.h"
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

// When loading a project, the stored video path may reference a .mov (ProRes)
// file that has since been re-rendered as .mp4 (HEVC), or vice versa.
// Try the alternate extension so format migrations don't break projects.
//
// 2026-05-28 Wells migration: remap all old Wells paths to HEVC
// stacked-alpha (NVDEC hardware decode, no chroma key needed).
static std::string resolveVideoPath(const std::string& path)
{
    if (path.empty()) return path;
    namespace fs = std::filesystem;
    // mediaPath strings are UTF-8 (the project file stores UTF-8).  The
    // fs::path(std::string) ctor decodes via the ANSI codepage and silently
    // mojibakes any non-ANSI character (U+FF5C etc.) — must use utf8ToPath.
    fs::path p = utf8ToPath(path);

    // Wells PROXY workflow: remap any old Wells path to the lightweight HEVC
    // packed-alpha for editing (fast NVDEC scrub/playback).  The export
    // renderer transparently substitutes the ProRes master at render time
    // (see FrameRenderer's wellsExportSource()).
    {
        std::string filename = pathToUtf8(p.filename());
        std::string lower = filename;
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char c) { return std::tolower(c); });

        // Map each Wells costume's old/master path to its lightweight stacked
        // HEVC proxy (skip paths already pointing at a _HEVC proxy).
        const char* hevcName = nullptr;
        if (lower.find("_hevc") == std::string::npos) {
            if      (lower.find("wells-chrono-mute") != std::string::npos) hevcName = "WELLS-CHRONO-MUTE_HEVC.mp4";
            else if (lower.find("wells-chrono-talk") != std::string::npos) hevcName = "WELLS-CHRONO-TALK_HEVC.mp4";
            else if (lower.find("wells-dress-mute")  != std::string::npos) hevcName = "WELLS-DRESS-MUTE_HEVC.mp4";
            else if (lower.find("wells-dress-talk")  != std::string::npos) hevcName = "WELLS-DRESS-TALK_HEVC.mp4";
        }
        if (hevcName) {
            fs::path hevcPath = p;
            hevcPath.replace_filename(hevcName);
            if (fs::exists(hevcPath)) {
                spdlog::info("ClipSerialization: Wells migration '{}' -> '{}'",
                             path, pathToUtf8(hevcPath));
                return pathToUtf8(hevcPath);
            }
        }
    }

    if (fs::exists(p)) return path;

    auto ext = pathToUtf8(p.extension());
    fs::path alt;
    if (ext == ".mov")       alt = p.replace_extension(".mp4");
    else if (ext == ".mp4")  alt = p.replace_extension(".mov");
    else if (ext == ".webm") alt = p.replace_extension(".mp4");
    else return path;

    if (fs::exists(alt)) {
        spdlog::info("ClipSerialization: re-resolved missing '{}' → '{}'",
                     path, pathToUtf8(alt));
        return pathToUtf8(alt);
    }
    return path;
}

void writeKeyframeTrack(BinaryWriter& w, const KeyframeTrack<float>& track)
{
    if (track.keyframeCount() == 0) {
        // Static track: write count=0 followed by the default value (v8+)
        w.writeU32(0);
        w.writeF32(track.defaultValue());
        return;
    }
    w.writeU32(static_cast<uint32_t>(track.keyframeCount()));
    for (size_t i = 0; i < track.keyframeCount(); ++i)
    {
        const auto& kf = track.keyframe(i);
        w.writeI64(kf.time);
        w.writeF32(kf.value);
        w.writeU8(static_cast<uint8_t>(kf.interp));
        w.writeF32(kf.bezierInX);
        w.writeF32(kf.bezierInY);
        w.writeF32(kf.bezierOutX);
        w.writeF32(kf.bezierOutY);
        // v15+: spatial interpolation + 2D path handles (motion path).
        w.writeU8(static_cast<uint8_t>(kf.spatialInterp));
        w.writeF32(kf.spatialInX);
        w.writeF32(kf.spatialInY);
        w.writeF32(kf.spatialOutX);
        w.writeF32(kf.spatialOutY);
    }
}

void readKeyframeTrack(BinaryReader& r, KeyframeTrack<float>& track, uint32_t version)
{
    uint32_t count = r.readU32();
    // Guard against corrupted files with absurd keyframe counts
    if (count > 100000) {
        spdlog::warn("ProjectSerializer: keyframe count {} exceeds limit, clamping", count);
        count = 100000;
    }

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

    for (uint32_t i = 0; i < count; ++i)
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

// ── Audio FX chain (v22+) ───────────────────────────────────────────────────
// Each processor is length-prefixed so unknown/older kinds re-sync cleanly.

static void writeAudioFx(BinaryWriter& w, const audiofx::FxChain& chain)
{
    w.writeU32(static_cast<uint32_t>(chain.size()));
    for (size_t i = 0; i < chain.size(); ++i) {
        const audiofx::AudioProcessor& p = chain.at(i);

        BinaryWriter pw;  // processor payload (length-prefixed below)
        uint8_t kind = 0xFF;

        if (const auto* eq = dynamic_cast<const audiofx::ParametricEQ*>(&p)) {
            kind = static_cast<uint8_t>(audiofx::ProcessorKind::ParametricEQ);
            pw.writeU8(eq->highPassOn() ? 1 : 0);
            pw.writeF32(eq->highPassFreq()); pw.writeF32(eq->highPassQ());
            pw.writeU8(eq->lowPassOn() ? 1 : 0);
            pw.writeF32(eq->lowPassFreq()); pw.writeF32(eq->lowPassQ());
            pw.writeU32(static_cast<uint32_t>(eq->bandCount()));
            for (int b = 0; b < eq->bandCount(); ++b) {
                const auto& bd = eq->band(b);
                pw.writeU8(bd.enabled ? 1 : 0);
                pw.writeU8(static_cast<uint8_t>(bd.type));
                pw.writeF32(bd.freqHz); pw.writeF32(bd.gainDb); pw.writeF32(bd.q);
            }
            pw.writeF32(eq->outputGainDb());
        } else if (const auto* dyn = dynamic_cast<const audiofx::Dynamics*>(&p)) {
            kind = static_cast<uint8_t>(audiofx::ProcessorKind::Dynamics);
            const auto& g = dyn->gate();
            pw.writeU8(g.enabled ? 1 : 0);
            pw.writeF32(g.thresholdDb); pw.writeF32(g.ratio); pw.writeF32(g.rangeDb);
            pw.writeF32(g.attackMs);    pw.writeF32(g.releaseMs);
            const auto& c = dyn->compressor();
            pw.writeU8(c.enabled ? 1 : 0);
            pw.writeF32(c.thresholdDb); pw.writeF32(c.ratio); pw.writeF32(c.kneeDb);
            pw.writeF32(c.makeupDb);    pw.writeF32(c.attackMs); pw.writeF32(c.releaseMs);
            const auto& l = dyn->limiter();
            pw.writeU8(l.enabled ? 1 : 0);
            pw.writeF32(l.ceilingDb); pw.writeF32(l.releaseMs);
        }

        w.writeU8(kind);
        w.writeU8(p.isEnabled() ? 1 : 0);
        w.writeU32(static_cast<uint32_t>(pw.size()));
        w.writeBytes(pw.data().data(), pw.size());
    }
}

static void readAudioFx(BinaryReader& r, audiofx::FxChain& chain)
{
    const uint32_t count = r.readU32();
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t kind    = r.readU8();
        const bool    enabled = r.readU8() != 0;
        const uint32_t payloadLen = r.readU32();
        const size_t endPos = r.position() + payloadLen;

        if (kind == static_cast<uint8_t>(audiofx::ProcessorKind::ParametricEQ)) {
            auto* eq = static_cast<audiofx::ParametricEQ*>(
                chain.add(audiofx::ProcessorKind::ParametricEQ));
            eq->setEnabled(enabled);
            const bool  hOn = r.readU8() != 0;
            const float hF = r.readF32(), hQ = r.readF32();
            eq->setHighPass(hOn, hF, hQ);
            const bool  lOn = r.readU8() != 0;
            const float lF = r.readF32(), lQ = r.readF32();
            eq->setLowPass(lOn, lF, lQ);
            const uint32_t bands = r.readU32();
            for (uint32_t b = 0; b < bands; ++b) {
                audiofx::ParametricEQ::Band bd;
                bd.enabled = r.readU8() != 0;
                bd.type    = static_cast<audiofx::Biquad::Type>(r.readU8());
                bd.freqHz  = r.readF32(); bd.gainDb = r.readF32(); bd.q = r.readF32();
                if (static_cast<int>(b) < eq->bandCount()) eq->setBand(static_cast<int>(b), bd);
            }
            eq->setOutputGainDb(r.readF32());
        } else if (kind == static_cast<uint8_t>(audiofx::ProcessorKind::Dynamics)) {
            auto* dyn = static_cast<audiofx::Dynamics*>(
                chain.add(audiofx::ProcessorKind::Dynamics));
            dyn->setEnabled(enabled);
            audiofx::Dynamics::GateSettings g;
            g.enabled = r.readU8() != 0;
            g.thresholdDb = r.readF32(); g.ratio = r.readF32(); g.rangeDb = r.readF32();
            g.attackMs = r.readF32();    g.releaseMs = r.readF32();
            dyn->setGate(g);
            audiofx::Dynamics::CompSettings c;
            c.enabled = r.readU8() != 0;
            c.thresholdDb = r.readF32(); c.ratio = r.readF32(); c.kneeDb = r.readF32();
            c.makeupDb = r.readF32();    c.attackMs = r.readF32(); c.releaseMs = r.readF32();
            dyn->setCompressor(c);
            audiofx::Dynamics::LimiterSettings l;
            l.enabled = r.readU8() != 0;
            l.ceilingDb = r.readF32(); l.releaseMs = r.readF32();
            dyn->setLimiter(l);
        }
        // Unknown kind or trailing future fields: re-sync to the payload end.
        if (r.position() < endPos) r.skip(endPos - r.position());
    }
}

// ── Serialize a single clip ─────────────────────────────────────────────────

void writeClip(BinaryWriter& w, const Clip& clip)
{
    // Common fields
    w.writeU8(static_cast<uint8_t>(clip.clipType()));
    w.writeU64(clip.id());
    w.writeString(clip.label());
    w.writeU32(clip.color());
    w.writeU8(clip.isEnabled() ? 1 : 0);
    w.writeI64(clip.timelineIn());
    w.writeI64(clip.duration());
    w.writeI64(clip.sourceIn());
    w.writeF64(clip.speed());

    // Shot group fields (v2+)
    w.writeU64(clip.groupId());
    w.writeString(clip.shotName());
    w.writeString(clip.layerId());

    // Keyframeable properties (6 tracks)
    writeKeyframeTrack(w, const_cast<Clip&>(clip).opacity());
    writeKeyframeTrack(w, const_cast<Clip&>(clip).positionX());
    writeKeyframeTrack(w, const_cast<Clip&>(clip).positionY());
    writeKeyframeTrack(w, const_cast<Clip&>(clip).scaleX());
    writeKeyframeTrack(w, const_cast<Clip&>(clip).scaleY());
    writeKeyframeTrack(w, const_cast<Clip&>(clip).rotation());

    // Speed ramp track (v5+)
    writeKeyframeTrack(w, const_cast<Clip&>(clip).speedRamp());

    // Anchor point tracks (v19+) — rotation/scale pivot offset. Old
    // projects load these as default 0 (legacy pivot at layer center).
    writeKeyframeTrack(w, const_cast<Clip&>(clip).anchorX());
    writeKeyframeTrack(w, const_cast<Clip&>(clip).anchorY());

    // Blend mode (v5+)
    w.writeU32(static_cast<uint32_t>(clip.blendMode()));

    // Maintain pitch flag (v7+)
    w.writeU8(clip.maintainPitch() ? 1 : 0);

    // Type-specific fields
    switch (clip.clipType())
    {
    case ClipType::Spine: {
        auto& sc = static_cast<const SpineClip&>(clip);
        w.writeString(sc.characterName());
        w.writeString(sc.outfit());
        w.writeU8(static_cast<uint8_t>(sc.stance()));
        w.writeString(sc.animationName());
        w.writeU8(sc.isLooping() ? 1 : 0);
        w.writeU8(sc.isTalking() ? 1 : 0);
        w.writeF32(sc.animationSpeed());
        w.writeU8(sc.useGlobalTime() ? 1 : 0);
        w.writeF32(sc.cropLeft());
        w.writeF32(sc.cropRight());
        w.writeF32(sc.cropTop());
        w.writeF32(sc.cropBottom());
        break;
    }
    case ClipType::Video: {
        auto& vc = static_cast<const VideoClip&>(clip);
        w.writeString(vc.mediaPath());
        w.writeU64(vc.mediaId());
        w.writeU32(vc.sourceWidth());
        w.writeU32(vc.sourceHeight());
        w.writeF64(vc.sourceFps());
        w.writeI64(vc.sourceDuration());
        w.writeU8(vc.hasAudio() ? 1 : 0);
        w.writeF32(vc.volume());
        w.writeF32(vc.cropLeft());
        w.writeF32(vc.cropRight());
        w.writeF32(vc.cropTop());
        w.writeF32(vc.cropBottom());
        // v9: video character metadata
        w.writeString(vc.characterName());
        w.writeU8(vc.isTalking() ? 1 : 0);
        w.writeString(vc.videoMutePath());
        w.writeString(vc.videoTalkPath());
        // v12: outfit + animation name for AnimationVideoCache characters
        w.writeString(vc.outfit());
        w.writeString(vc.animationName());
        break;
    }
    case ClipType::Audio: {
        auto& ac = static_cast<const AudioClip&>(clip);
        w.writeString(ac.mediaPath());
        w.writeU64(ac.mediaId());
        w.writeU32(ac.sampleRate());
        w.writeU32(static_cast<uint32_t>(ac.channels()));
        w.writeI64(ac.sourceDuration());
        writeKeyframeTrack(w, const_cast<AudioClip&>(ac).volume());
        writeKeyframeTrack(w, const_cast<AudioClip&>(ac).pan());
        w.writeI64(ac.fadeInDuration());
        w.writeI64(ac.fadeOutDuration());
        break;
    }
    case ClipType::Title: {
        auto& tc = static_cast<const TitleClip&>(clip);
        w.writeString(tc.text());
        w.writeString(tc.fontFamily());
        w.writeF32(tc.fontSize());
        w.writeU8(tc.isBold() ? 1 : 0);
        w.writeU8(tc.isItalic() ? 1 : 0);
        w.writeU8(static_cast<uint8_t>(tc.alignment()));
        w.writeU8(static_cast<uint8_t>(tc.verticalAlignment()));
        w.writeU32(tc.textColor());
        w.writeU32(tc.bgColor());
        w.writeU32(tc.outlineColor());
        w.writeF32(tc.outlineWidth());
        writeKeyframeTrack(w, const_cast<TitleClip&>(tc).tracking());
        writeKeyframeTrack(w, const_cast<TitleClip&>(tc).lineHeight());
        break;
    }
    case ClipType::Adjustment: {
        auto& ac = static_cast<const AdjustmentClip&>(clip);
        w.writeU8(ac.blendMode());
        w.writeU8(ac.affectsSingleTrack() ? 1 : 0);
        break;
    }
    case ClipType::Image: {
        auto& ic = static_cast<const ImageClip&>(clip);
        w.writeString(ic.mediaPath());
        w.writeU64(ic.mediaId());
        w.writeU32(ic.sourceWidth());
        w.writeU32(ic.sourceHeight());
        w.writeF32(ic.cropLeft());
        w.writeF32(ic.cropRight());
        w.writeF32(ic.cropTop());
        w.writeF32(ic.cropBottom());
        break;
    }
    case ClipType::Sequence: {
        auto& sc = static_cast<const SequenceClip&>(clip);
        w.writeU32(static_cast<uint32_t>(sc.sequenceIndex()));
        w.writeString(sc.sequenceName());
        break;
    }
    case ClipType::Caption: {
        auto& cc = static_cast<const CaptionClip&>(clip);
        w.writeString(cc.text());
        w.writeString(cc.speaker());
        w.writeString(cc.fontFamily());
        w.writeF32(cc.fontSize());
        w.writeU32(cc.textColor());
        w.writeU32(cc.bgColor());
        w.writeU8(static_cast<uint8_t>(cc.position()));
        break;
    }
    case ClipType::PngPuppet: {
        auto& pc = static_cast<const PngPuppetClip&>(clip);
        w.writeString(pc.characterName());
        w.writeString(pc.variant());
        for (int f = 0; f < PngPuppetClip::FaceCount; ++f)
            w.writeString(pc.facePath(f));
        w.writeU8(pc.isTalking() ? 1 : 0);
        w.writeU32(pc.seed());
        w.writeF32(pc.talkSwapSeconds());
        w.writeF32(pc.blinkIntervalSeconds());
        w.writeF32(pc.blinkDurationSeconds());
        w.writeF32(pc.breathAmplitude());
        w.writeF32(pc.breathSpeed());
        w.writeF32(pc.swayAmplitude());
        break;
    }
    case ClipType::Graphic: {
        auto& gc = static_cast<const GraphicClip&>(clip);
        w.writeU32(static_cast<uint32_t>(gc.layerCount()));
        for (size_t li = 0; li < gc.layerCount(); ++li) {
            const auto* layer = gc.layer(li);
            w.writeU8(static_cast<uint8_t>(layer->layerType()));
            w.writeString(layer->name());
            w.writeU8(layer->isVisible() ? 1 : 0);
            w.writeU8(layer->isLocked() ? 1 : 0);
            // Layer transform (8 keyframe tracks)
            writeKeyframeTrack(w, const_cast<GraphicLayer*>(layer)->transform().posX);
            writeKeyframeTrack(w, const_cast<GraphicLayer*>(layer)->transform().posY);
            writeKeyframeTrack(w, const_cast<GraphicLayer*>(layer)->transform().scaleX);
            writeKeyframeTrack(w, const_cast<GraphicLayer*>(layer)->transform().scaleY);
            writeKeyframeTrack(w, const_cast<GraphicLayer*>(layer)->transform().rotation);
            writeKeyframeTrack(w, const_cast<GraphicLayer*>(layer)->transform().anchorX);
            writeKeyframeTrack(w, const_cast<GraphicLayer*>(layer)->transform().anchorY);
            writeKeyframeTrack(w, const_cast<GraphicLayer*>(layer)->transform().opacity);
            // Appearance
            const auto& app = layer->appearance();
            w.writeU32(static_cast<uint32_t>(app.fills.size()));
            for (auto& f : app.fills) { w.writeU32(f.color); w.writeF32(f.opacity); w.writeU8(f.enabled ? 1 : 0); }
            w.writeU32(static_cast<uint32_t>(app.strokes.size()));
            for (auto& s : app.strokes) { w.writeU32(s.color); w.writeF32(s.width); w.writeU8(static_cast<uint8_t>(s.position)); w.writeF32(s.opacity); w.writeU8(s.enabled ? 1 : 0); }
            w.writeU32(static_cast<uint32_t>(app.shadows.size()));
            for (auto& s : app.shadows) { w.writeU32(s.color); w.writeF32(s.distance); w.writeF32(s.angle); w.writeF32(s.softness); w.writeF32(s.opacity); w.writeU8(s.enabled ? 1 : 0); }
            // Type-specific layer data
            if (layer->layerType() == GraphicLayerType::Text) {
                auto* tl = static_cast<const TextLayer*>(layer);
                w.writeString(tl->text());
                w.writeString(tl->fontFamily());
                w.writeF32(tl->fontSize());
                w.writeU32(static_cast<uint32_t>(tl->fontWeight()));
                w.writeU8(tl->isItalic() ? 1 : 0);
                w.writeU8(tl->allCaps() ? 1 : 0);
                w.writeU8(tl->smallCaps() ? 1 : 0);
                w.writeU8(static_cast<uint8_t>(tl->alignment()));
                w.writeU8(static_cast<uint8_t>(tl->vAlignment()));
                writeKeyframeTrack(w, const_cast<TextLayer*>(tl)->tracking());
                writeKeyframeTrack(w, const_cast<TextLayer*>(tl)->leading());
                writeKeyframeTrack(w, const_cast<TextLayer*>(tl)->baselineShift());
                w.writeF32(tl->boxWidth());
                w.writeF32(tl->boxHeight());
                w.writeU8(tl->useParagraphBox() ? 1 : 0);
            } else if (layer->layerType() == GraphicLayerType::Shape) {
                auto* sl = static_cast<const ShapeLayer*>(layer);
                w.writeU8(static_cast<uint8_t>(sl->shapeType()));
                w.writeF32(sl->shapeWidth());
                w.writeF32(sl->shapeHeight());
                w.writeF32(sl->cornerRadius());
                w.writeU32(sl->fillColor());
            }
        }
        break;
    }
    }

    // ── Effect stack (v10+) ────────────────────────────────────────────
    {
        const auto& stack = clip.effects();
        w.writeU32(static_cast<uint32_t>(stack.effectCount()));
        for (size_t ei = 0; ei < stack.effectCount(); ++ei) {
            const auto& fx = stack.effect(ei);
            w.writeU8(static_cast<uint8_t>(fx.effectType()));
            w.writeU8(fx.isEnabled() ? 1 : 0);
            w.writeU32(static_cast<uint32_t>(fx.paramCount()));
            for (size_t pi = 0; pi < fx.paramCount(); ++pi)
                writeKeyframeTrack(w, fx.param(pi).track);
            // LUT: save file path so it can be reloaded
            if (fx.effectType() == EffectType::LUT)
                w.writeString(static_cast<const LUT&>(fx).lutPath());
            // Beat-reactive effects (v24+): persist detected onsets + source.
            if (isBeatReactEffect(fx.effectType())) {
                const auto& be = static_cast<const BeatReactEffect&>(fx);
                w.writeU64(be.audioSourceId());
                const auto& bt = be.beatTimes();
                w.writeU32(static_cast<uint32_t>(bt.size()));
                for (float t : bt) w.writeF32(t);
            }
        }
    }

    // ── Opacity masks (v11+) ───────────────────────────────────────────
    {
        const auto& masks = clip.masks();
        w.writeU32(static_cast<uint32_t>(masks.size()));
        for (const auto& m : masks) {
            w.writeU8(static_cast<uint8_t>(m.shape));
            w.writeF32(m.centerX);
            w.writeF32(m.centerY);
            w.writeF32(m.width);
            w.writeF32(m.height);
            w.writeF32(m.rotation);
            w.writeF32(m.feather);
            w.writeF32(m.expansion);
            w.writeF32(m.maskOpacity);
            w.writeU8(m.inverted ? 1 : 0);
            w.writeString(m.name);
            w.writeU32(static_cast<uint32_t>(m.vertices.size()));
            for (const auto& v : m.vertices) {
                w.writeF32(v.x);
                w.writeF32(v.y);
                w.writeF32(v.inTanX);
                w.writeF32(v.inTanY);
                w.writeF32(v.outTanX);
                w.writeF32(v.outTanY);
            }
        }
    }

    // ── Audio FX chain (v22+) — audio clips only ───────────────────────
    if (clip.clipType() == ClipType::Audio)
        writeAudioFx(w, static_cast<const AudioClip&>(clip).audioFx());
}

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

    std::unique_ptr<Clip> clip;

    switch (type)
    {
    case ClipType::Spine:
        clip = std::make_unique<SpineClip>();
        break;
    case ClipType::Video:
        clip = std::make_unique<VideoClip>();
        break;
    case ClipType::Audio:
        clip = std::make_unique<AudioClip>();
        break;
    case ClipType::Title:
        clip = std::make_unique<TitleClip>();
        break;
    case ClipType::Adjustment:
        clip = std::make_unique<AdjustmentClip>();
        break;
    case ClipType::Image:
        clip = std::make_unique<ImageClip>();
        break;
    case ClipType::Graphic:
        clip = std::make_unique<GraphicClip>();
        break;
    case ClipType::Sequence:
        clip = std::make_unique<SequenceClip>();
        break;
    case ClipType::Caption:
        clip = std::make_unique<CaptionClip>();
        break;
    case ClipType::PngPuppet:
        clip = std::make_unique<PngPuppetClip>();
        break;
    default:
        return nullptr;
    }

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

    // Blend mode (v5+)
    if (version >= 5)
        clip->setBlendMode(static_cast<int32_t>(r.readU32()));

    // Maintain pitch flag (v7+)
    if (version >= 7)
        clip->setMaintainPitch(r.readU8() != 0);

    // Type-specific fields
    switch (type)
    {
    case ClipType::Spine: {
        auto* sc = static_cast<SpineClip*>(clip.get());
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
        break;
    }
    case ClipType::Video: {
        auto* vc = static_cast<VideoClip*>(clip.get());
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
        break;
    }
    case ClipType::Audio: {
        auto* ac = static_cast<AudioClip*>(clip.get());
        ac->setMediaPath(r.readString());
        ac->setMediaId(r.readU64());
        ac->setSampleRate(r.readU32());
        ac->setChannels(static_cast<uint16_t>(r.readU32()));
        ac->setSourceDuration(r.readI64());
        readKeyframeTrack(r, ac->volume(), version);
        readKeyframeTrack(r, ac->pan(), version);
        ac->setFadeInDuration(r.readI64());
        ac->setFadeOutDuration(r.readI64());
        break;
    }
    case ClipType::Title: {
        auto* tc = static_cast<TitleClip*>(clip.get());
        tc->setText(r.readString());
        tc->setFontFamily(r.readString());
        tc->setFontSize(r.readF32());
        tc->setBold(r.readU8() != 0);
        tc->setItalic(r.readU8() != 0);
        tc->setAlignment(static_cast<TextAlign>(r.readU8()));
        tc->setVerticalAlignment(static_cast<TextVAlign>(r.readU8()));
        tc->setTextColor(r.readU32());
        tc->setBgColor(r.readU32());
        tc->setOutlineColor(r.readU32());
        tc->setOutlineWidth(r.readF32());
        readKeyframeTrack(r, tc->tracking(), version);
        readKeyframeTrack(r, tc->lineHeight(), version);
        break;
    }
    case ClipType::Adjustment: {
        auto* ac = static_cast<AdjustmentClip*>(clip.get());
        ac->setBlendMode(r.readU8());
        ac->setAffectsSingleTrack(r.readU8() != 0);
        break;
    }
    case ClipType::Image: {
        auto* ic = static_cast<ImageClip*>(clip.get());
        ic->setMediaPath(r.readString());
        ic->setMediaId(r.readU64());
        uint32_t sw = r.readU32(), sh = r.readU32();
        ic->setSourceResolution(sw, sh);
        float cl = r.readF32(), cr = r.readF32(), ct = r.readF32(), cb = r.readF32();
        ic->setCrop(cl, cr, ct, cb);
        break;
    }
    case ClipType::Graphic: {
        auto* gc = static_cast<GraphicClip*>(clip.get());
        uint32_t layerCount = r.readU32();
        for (uint32_t li = 0; li < layerCount; ++li) {
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
            uint32_t fillCount = r.readU32();
            for (uint32_t fi = 0; fi < fillCount; ++fi) {
                FillEntry fe; fe.color = r.readU32(); fe.opacity = r.readF32(); fe.enabled = r.readU8() != 0;
                app.fills.push_back(fe);
            }
            app.strokes.clear();
            uint32_t strokeCount = r.readU32();
            for (uint32_t si = 0; si < strokeCount; ++si) {
                StrokeEntry se; se.color = r.readU32(); se.width = r.readF32(); se.position = static_cast<StrokePosition>(r.readU8()); se.opacity = r.readF32(); se.enabled = r.readU8() != 0;
                app.strokes.push_back(se);
            }
            app.shadows.clear();
            uint32_t shadowCount = r.readU32();
            for (uint32_t si = 0; si < shadowCount; ++si) {
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
        break;
    }
    case ClipType::Sequence: {
        auto* sc = static_cast<SequenceClip*>(clip.get());
        sc->setSequenceIndex(static_cast<size_t>(r.readU32()));
        sc->setSequenceName(r.readString());
        break;
    }
    case ClipType::Caption: {
        auto* cc = static_cast<CaptionClip*>(clip.get());
        cc->setText(r.readString());
        cc->setSpeaker(r.readString());
        cc->setFontFamily(r.readString());
        cc->setFontSize(r.readF32());
        cc->setTextColor(r.readU32());
        cc->setBgColor(r.readU32());
        cc->setPosition(static_cast<CaptionPosition>(r.readU8()));
        break;
    }
    case ClipType::PngPuppet: {
        auto* pc = static_cast<PngPuppetClip*>(clip.get());
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
        break;
    }
    default:
        break;
    }

    // ── Effect stack (v10+) ────────────────────────────────────────────
    if (version >= 10 && clip) {
        uint32_t fxCount = r.readU32();
        for (uint32_t ei = 0; ei < fxCount; ++ei) {
            auto fxType = static_cast<EffectType>(r.readU8());
            bool fxEnabled = r.readU8() != 0;
            uint32_t paramCount = r.readU32();
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
                if (fxType == EffectType::LUT) {
                    std::string lutPath = r.readString();
                    if (!lutPath.empty())
                        static_cast<LUT*>(fx.get())->loadCubeFile(lutPath);
                }
                if (version >= 24 && isBeatReactEffect(fxType)) {
                    auto* be = static_cast<BeatReactEffect*>(fx.get());
                    be->setAudioSourceId(r.readU64());
                    uint32_t n = r.readU32();
                    std::vector<float> times(n);
                    for (uint32_t i = 0; i < n; ++i) times[i] = r.readF32();
                    be->setBeatTimes(std::move(times));
                }
                clip->effects().addEffect(std::move(fx));
            } else {
                // Unknown effect type — skip its data
                for (uint32_t pi = 0; pi < paramCount; ++pi) {
                    KeyframeTrack<float> discard(0.0f);
                    readKeyframeTrack(r, discard, version);
                }
                if (fxType == EffectType::LUT)
                    r.readString(); // discard LUT path
                if (version >= 24 && isBeatReactEffect(fxType)) {
                    r.readU64();
                    uint32_t n = r.readU32();
                    for (uint32_t i = 0; i < n; ++i) r.readF32();
                }
            }
        }
    }

    // ── Opacity masks (v11+) ───────────────────────────────────────────
    if (version >= 11 && clip) {
        uint32_t maskCount = r.readU32();
        for (uint32_t mi = 0; mi < maskCount; ++mi) {
            OpacityMask m;
            m.shape       = static_cast<MaskShape>(r.readU8());
            m.centerX     = r.readF32();
            m.centerY     = r.readF32();
            m.width       = r.readF32();
            m.height      = r.readF32();
            m.rotation    = r.readF32();
            m.feather     = r.readF32();
            m.expansion   = r.readF32();
            m.maskOpacity = r.readF32();
            m.inverted    = r.readU8() != 0;
            m.name        = r.readString();
            uint32_t vertCount = r.readU32();
            m.vertices.resize(vertCount);
            for (uint32_t vi = 0; vi < vertCount; ++vi) {
                m.vertices[vi].x       = r.readF32();
                m.vertices[vi].y       = r.readF32();
                m.vertices[vi].inTanX  = r.readF32();
                m.vertices[vi].inTanY  = r.readF32();
                m.vertices[vi].outTanX = r.readF32();
                m.vertices[vi].outTanY = r.readF32();
            }
            clip->addMask(std::move(m));
        }
    }

    // ── Audio FX chain (v22+) — audio clips only ───────────────────────
    if (version >= 22 && clip && clip->clipType() == ClipType::Audio)
        readAudioFx(r, static_cast<AudioClip*>(clip.get())->audioFx());

    return clip;
}
} // namespace rt