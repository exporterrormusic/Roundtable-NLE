/*
 * LegacyProjectWriterV47.cpp — the project writer as it was at format v47,
 * frozen, for tests only. It is independent of the v48 record schemas, so
 * "same v47 bytes before and after a v48 round trip" proves the schemas
 * dropped nothing; it also builds old-version files for migration tests.
 * Do not change it (beyond keeping it compiling).
 */

#include "support/LegacyProjectWriterV47.h"

#include "project/AssetDatabase.h"
#include "project/BinaryIO.h"
#include "project/Project.h"
#include "project/ProjectSerializer.h"
#include "project/Settings.h"
#include "timeline/AdjustmentClip.h"
#include "timeline/AudioClip.h"
#include "timeline/CaptionClip.h"
#include "timeline/Clip.h"
#include "timeline/GraphicClip.h"
#include "timeline/GraphicLayer.h"
#include "timeline/ImageClip.h"
#include "timeline/KeyframeTrack.h"
#include "timeline/Marker.h"
#include "timeline/OpacityMask.h"
#include "timeline/PngPuppetClip.h"
#include "timeline/SequenceClip.h"
#include "timeline/SpineClip.h"
#include "timeline/TierListClip.h"
#include "timeline/Timeline.h"
#include "timeline/TitleClip.h"
#include "timeline/Track.h"
#include "timeline/Transition.h"
#include "timeline/VideoClip.h"
#include "effects/BeatEffects.h"
#include "effects/ColorGrading.h"
#include "effects/Effect.h"
#include "effects/EffectStack.h"
#include "effects/LUT.h"
#include "audiofx/Dynamics.h"
#include "audiofx/FxChain.h"
#include "audiofx/ParametricEQ.h"

namespace rt::legacy_v47 {

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

namespace {


void writeMaskGeometry(BinaryWriter& w, const MaskGeometry& g)
{
    w.writeF32(g.centerX);
    w.writeF32(g.centerY);
    w.writeF32(g.width);
    w.writeF32(g.height);
    w.writeF32(g.rotation);
    w.writeU32(static_cast<uint32_t>(g.vertices.size()));
    for (const auto& v : g.vertices) {
        w.writeF32(v.x);
        w.writeF32(v.y);
        w.writeF32(v.inTanX);
        w.writeF32(v.inTanY);
        w.writeF32(v.outTanX);
        w.writeF32(v.outTanY);
    }
}

void writeMask(BinaryWriter& w, const OpacityMask& m)
{
    w.writeU8(static_cast<uint8_t>(m.shape));
    w.writeU8(m.inverted ? 1 : 0);
    w.writeString(m.name);
    writeMaskGeometry(w, m.base);
    w.writeU8(m.pathAnimated ? 1 : 0);
    w.writeU32(static_cast<uint32_t>(m.pathKeys.size()));
    for (const auto& k : m.pathKeys) {
        w.writeI64(k.time);
        writeMaskGeometry(w, k.geometry);
    }
    writeKeyframeTrack(w, m.feather);
    writeKeyframeTrack(w, m.maskOpacity);
    writeKeyframeTrack(w, m.expansion);
    w.writeU8(static_cast<uint8_t>(m.coordinateSpace));
    w.writeU64(m.maskId);
    w.writeU8(m.unassigned ? 1 : 0);  // v47
}

void writeAudioFx(BinaryWriter& w, const audiofx::FxChain& chain)
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

void writeSpineFields(BinaryWriter& w, const Clip& clip)
{
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
}

void writeVideoFields(BinaryWriter& w, const Clip& clip)
{
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
    // v34: persist source display rotation plus whether it came from actual
    // media metadata. Pre-v34 records had no rotation; treating their implicit
    // zero as authoritative would corrupt legacy masks on rotated footage.
    w.writeU32(static_cast<uint32_t>(vc.sourceRotation()));
    w.writeU8(vc.sourceMetadataAuthoritative() ? 1 : 0);
}

void writeAudioFields(BinaryWriter& w, const Clip& clip)
{
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
    // v27: chosen audio-stream ordinal (-1 = auto/best). Appended at the end
    // so the byte layout stays forward-compatible; no writeI32 exists so the
    // int round-trips through writeU32 (-1 <-> 0xFFFFFFFF).
    w.writeU32(static_cast<uint32_t>(ac.audioStreamIndex()));
}

void writeTitleFields(BinaryWriter& w, const Clip& clip)
{
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
}

void writeAdjustmentFields(BinaryWriter& w, const Clip& clip)
{
    auto& ac = static_cast<const AdjustmentClip&>(clip);
    // Former adjustment-only blend byte. The real blend mode is the shared
    // field saved with every clip; this slot stays for file compatibility
    // and carries the same value (clamped) for older builds.
    const int32_t blend = clip.blendMode();
    w.writeU8(static_cast<uint8_t>(blend < 0 ? 0 : (blend > 255 ? 255 : blend)));
    w.writeU8(ac.affectsSingleTrack() ? 1 : 0);
}

void writeImageFields(BinaryWriter& w, const Clip& clip)
{
    auto& ic = static_cast<const ImageClip&>(clip);
    w.writeString(ic.mediaPath());
    w.writeU64(ic.mediaId());
    w.writeU32(ic.sourceWidth());
    w.writeU32(ic.sourceHeight());
    w.writeF32(ic.cropLeft());
    w.writeF32(ic.cropRight());
    w.writeF32(ic.cropTop());
    w.writeF32(ic.cropBottom());
}

void writeSequenceFields(BinaryWriter& w, const Clip& clip)
{
    auto& sc = static_cast<const SequenceClip&>(clip);
    w.writeU32(static_cast<uint32_t>(sc.sequenceIndex()));
    w.writeString(sc.sequenceName());
}

void writeTextStyleRun(BinaryWriter& w, const TextStyleRun& run)
{
    w.writeU32(run.start);
    w.writeU32(run.length);
    w.writeString(run.fontFamily);
    w.writeF32(run.fontSize);
    w.writeU32(static_cast<uint32_t>(run.fontWeight));
    w.writeU8(run.italic ? 1 : 0);
    w.writeU8(run.allCaps ? 1 : 0);
    w.writeU8(run.smallCaps ? 1 : 0);
    w.writeF32(run.tracking);
    w.writeF32(run.baselineShift);
    w.writeU32(run.overrideMask);
    w.writeF32(run.leading);
    w.writeString(run.fontStyle);
    w.writeF32(run.kerning);
    w.writeF32(run.tabWidth);
    w.writeF32(run.tsume);
    w.writeU8(run.fauxBold ? 1 : 0);
    w.writeU8(run.fauxItalic ? 1 : 0);
    w.writeU8(run.underline ? 1 : 0);
    w.writeU8(run.superscript ? 1 : 0);
    w.writeU8(run.subscript ? 1 : 0);
    const auto& a = run.appearance;
    w.writeU8(a.fillEnabled ? 1 : 0); w.writeU32(a.fillColor);
    w.writeU8(a.strokeEnabled ? 1 : 0); w.writeU32(a.strokeColor);
    w.writeF32(a.strokeWidth);
    w.writeU8(static_cast<uint8_t>(a.strokePosition));
    w.writeU8(a.shadowEnabled ? 1 : 0); w.writeU32(a.shadowColor);
    w.writeF32(a.shadowDistance); w.writeF32(a.shadowAngle);
    w.writeF32(a.shadowSoftness); w.writeF32(a.shadowOpacity);
    w.writeU8(a.backgroundEnabled ? 1 : 0); w.writeU32(a.backgroundColor);
    w.writeF32(a.backgroundPadding);
}

void writeCaptionFields(BinaryWriter& w, const Clip& clip)
{
    auto& cc = static_cast<const CaptionClip&>(clip);
    w.writeString(cc.text());
    w.writeString(cc.speaker());
    w.writeString(cc.fontFamily());
    w.writeF32(cc.fontSize());
    w.writeU32(cc.textColor());
    w.writeU32(cc.bgColor());
    w.writeU8(static_cast<uint8_t>(cc.position()));
    // v31: burned-in style — bold toggle, outline, speaker label
    w.writeU8(cc.isBold() ? 1 : 0);
    w.writeU32(cc.outlineColor());
    w.writeF32(cc.outlineWidth());
    w.writeU8(cc.showSpeaker() ? 1 : 0);
    // v37: resolved per-character font styles (UTF-16 ranges).
    w.writeU32(static_cast<uint32_t>(cc.styleRuns().size()));
    for (const auto& run : cc.styleRuns()) writeTextStyleRun(w, run);
    w.writeString(cc.fontStyle());
    w.writeU8(cc.isItalic() ? 1 : 0);
    w.writeU8(cc.allCaps() ? 1 : 0);
    w.writeU8(cc.smallCaps() ? 1 : 0);
    w.writeU8(cc.underline() ? 1 : 0);
    w.writeU8(cc.superscript() ? 1 : 0);
    w.writeU8(cc.subscript() ? 1 : 0);
    w.writeU8(cc.fauxBold() ? 1 : 0);
    w.writeU8(cc.fauxItalic() ? 1 : 0);
    w.writeF32(cc.tracking());
    w.writeF32(cc.leading());
    w.writeU8(static_cast<uint8_t>(cc.alignment()));
    w.writeString(cc.trackStyleName());
    w.writeU32(static_cast<uint32_t>(cc.paragraphStyles().size()));
    for (const auto& paragraph : cc.paragraphStyles()) {
        w.writeU32(paragraph.start);
        w.writeU32(paragraph.length);
        w.writeU8(static_cast<uint8_t>(paragraph.alignment));
        w.writeU8(paragraph.rightToLeft ? 1 : 0);
    }
    w.writeF32(cc.confidence());
}

void writePngPuppetFields(BinaryWriter& w, const Clip& clip)
{
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
}

void writeGraphicFields(BinaryWriter& w, const Clip& clip)
{
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
            w.writeU32(static_cast<uint32_t>(tl->styleRuns().size()));
            for (const auto& run : tl->styleRuns())
                writeTextStyleRun(w, run);
            w.writeString(tl->fontStyle());
            w.writeF32(tl->kerning());
            w.writeF32(tl->tabWidth());
            w.writeF32(tl->tsume());
            w.writeU8(tl->fauxBold() ? 1 : 0);
            w.writeU8(tl->fauxItalic() ? 1 : 0);
            w.writeU8(tl->underline() ? 1 : 0);
            w.writeU8(tl->superscript() ? 1 : 0);
            w.writeU8(tl->subscript() ? 1 : 0);
            w.writeU8(tl->rightToLeft() ? 1 : 0);
            w.writeU8(tl->backgroundEnabled() ? 1 : 0);
            w.writeU32(tl->backgroundColor());
            w.writeF32(tl->backgroundPadding());
            w.writeU8(tl->maskWithText() ? 1 : 0);
            w.writeString(tl->linkedStyleName());
            w.writeU32(static_cast<uint32_t>(tl->paragraphStyles().size()));
            for (const auto& paragraph : tl->paragraphStyles()) {
                w.writeU32(paragraph.start);
                w.writeU32(paragraph.length);
                w.writeU8(static_cast<uint8_t>(paragraph.alignment));
                w.writeU8(paragraph.rightToLeft ? 1 : 0);
            }
        } else if (layer->layerType() == GraphicLayerType::Shape) {
            auto* sl = static_cast<const ShapeLayer*>(layer);
            w.writeU8(static_cast<uint8_t>(sl->shapeType()));
            w.writeF32(sl->shapeWidth());
            w.writeF32(sl->shapeHeight());
            w.writeF32(sl->cornerRadius());
            w.writeU32(sl->fillColor());
        }
    }
}

void writeTierListFields(BinaryWriter& w, const Clip& clip)
{
    auto& tc = static_cast<const TierListClip&>(clip);
    w.writeString(tc.title());
    // Grid config
    w.writeU8(static_cast<uint8_t>(tc.entryAspect()));
    w.writeF32(tc.customAspectW());
    w.writeF32(tc.customAspectH());
    w.writeF32(tc.topSafeMargin());
    w.writeF32(tc.rightSafeMargin());
    w.writeU32(tc.backgroundColor());
    // Tiers
    w.writeU32(static_cast<uint32_t>(tc.tiers().size()));
    for (const auto& t : tc.tiers()) {
        w.writeString(t.label);
        w.writeU32(t.color);
    }
    // Subbins
    w.writeU32(static_cast<uint32_t>(tc.subbins().size()));
    for (const auto& s : tc.subbins())
        w.writeString(s);
    // Entries
    w.writeU32(static_cast<uint32_t>(tc.entries().size()));
    for (const auto& e : tc.entries()) {
        w.writeU64(e.id);
        w.writeString(e.imagePath);
        w.writeString(e.title);
        w.writeString(e.subbin);
    }
    // Events
    w.writeU32(static_cast<uint32_t>(tc.events().size()));
    for (const auto& ev : tc.events()) {
        w.writeU8(static_cast<uint8_t>(ev.type));
        w.writeU64(ev.entryId);
        w.writeU32(static_cast<uint32_t>(ev.tier));
        w.writeU32(static_cast<uint32_t>(ev.index));
        w.writeU32(static_cast<uint32_t>(ev.fromIndex));
        w.writeU32(static_cast<uint32_t>(ev.toIndex));
        w.writeI64(ev.start);
        w.writeI64(ev.end);
    }
}

void writeTypeFields(BinaryWriter& w, const Clip& clip)
{
    switch (clip.clipType()) {
    case ClipType::Spine:      writeSpineFields(w, clip); break;
    case ClipType::Video:      writeVideoFields(w, clip); break;
    case ClipType::Audio:      writeAudioFields(w, clip); break;
    case ClipType::Title:      writeTitleFields(w, clip); break;
    case ClipType::Adjustment: writeAdjustmentFields(w, clip); break;
    case ClipType::Image:      writeImageFields(w, clip); break;
    case ClipType::Sequence:   writeSequenceFields(w, clip); break;
    case ClipType::Caption:    writeCaptionFields(w, clip); break;
    case ClipType::PngPuppet:  writePngPuppetFields(w, clip); break;
    case ClipType::Graphic:    writeGraphicFields(w, clip); break;
    case ClipType::TierList:   writeTierListFields(w, clip); break;
    default: break;
    }
}

} // namespace

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

    // Transform motion blur (v35+). 0 degrees is disabled, preserving the
    // byte-for-byte visual behaviour of projects created before v35.
    writeKeyframeTrack(w, const_cast<Clip&>(clip).shutterAngle());

    // Blend mode (v5+)
    w.writeU32(static_cast<uint32_t>(clip.blendMode()));

    // Maintain pitch flag (v7+)
    w.writeU8(clip.maintainPitch() ? 1 : 0);

    // AudioSync export back-link (v29+). No writeI32 exists, so the int
    // round-trips through writeU32 (-1 <-> 0xFFFFFFFF), same as v27's
    // audioStreamIndex.
    w.writeU32(static_cast<uint32_t>(clip.syncLine()));

    // Temporal interpolation mode (v32+).  The numeric enum value is part of
    // the project format and defaults to Frame Sampling in older projects.
    w.writeU8(static_cast<uint8_t>(clip.timeInterpolation()));

    // A/V link (v44+). Link ids live in the clip-id space (a clip's linkId
    // is its partner's id, or a split-derived id), and clip ids are
    // restored verbatim on load, so the value round-trips unchanged.
    w.writeU64(clip.linkId());

    // Type-specific fields — dispatched through the per-type registry
    writeTypeFields(w, clip);

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
            // Color Grading's non-keyframe scalar state (v41+). Curves and
            // HSL live outside Effect::m_params, so they need an explicit
            // payload or they silently reset when the project is reopened.
            if (fx.effectType() == EffectType::ColorGrading) {
                const auto& grading = static_cast<const ColorGrading&>(fx);
                w.writeU8(grading.basicEnabled() ? 1 : 0);
                w.writeU8(grading.creativeEnabled() ? 1 : 0);
                w.writeU8(grading.wheelsEnabled() ? 1 : 0);
                w.writeU8(grading.curvesEnabled() ? 1 : 0);
                w.writeU8(grading.hslEnabled() ? 1 : 0);
                w.writeU8(grading.vignetteEnabled() ? 1 : 0);
                const auto& hsl = grading.hslParams();
                w.writeF32(hsl.hueCenter); w.writeF32(hsl.hueWidth);
                w.writeF32(hsl.satMin); w.writeF32(hsl.satMax);
                w.writeF32(hsl.lumMin); w.writeF32(hsl.lumMax);
                w.writeF32(hsl.hueShift); w.writeF32(hsl.satAdjust);
                w.writeF32(hsl.lumAdjust);
                for (int ch = 0; ch < ColorGrading::CurveCount; ++ch) {
                    const auto& lut = grading.curveLUT(
                        static_cast<ColorGrading::CurveChannel>(ch));
                    for (float sample : lut) w.writeF32(sample);
                }
            }
            // Effect masks (v30+): masks limiting where this effect applies.
            w.writeU32(static_cast<uint32_t>(fx.maskCount()));
            for (const auto& m : fx.masks())
                writeMask(w, m);
        }
    }

    // ── Opacity masks (v11+; v30 record) ───────────────────────────────
    {
        const auto& masks = clip.masks();
        w.writeU32(static_cast<uint32_t>(masks.size()));
        for (const auto& m : masks)
            writeMask(w, m);
    }

    // ── Audio FX chain (v22+) — audio clips only ───────────────────────
    if (clip.clipType() == ClipType::Audio)
        writeAudioFx(w, static_cast<const AudioClip&>(clip).audioFx());
}

std::vector<uint8_t> serialize(const Project& project)
{
    BinaryWriter out;

    // ── Header ──────────────────────────────────────────────────────────
    out.writeBytes(ProjectSerializer::MAGIC, 8);
    out.writeU32(kVersion);
    // Section count placeholder — we'll count as we go
    size_t sectionCountPos = out.size();
    out.writeU32(0);
    // Reserved 16 bytes
    for (int i = 0; i < 16; ++i) out.writeU8(0);

    uint32_t sectionCount = 0;

    // ── Section: Settings ───────────────────────────────────────────────
    {
        BinaryWriter sec;
        // Project default template (NOT a sequence's settings — those are
        // written per-sequence in ProjectSerializer::Section_Sequences). Kept for the New
        // Sequence dialog defaults and for loading pre-v25 projects.
        const auto& s = project.defaultSettings();
        sec.writeU32(s.resolution().width);
        sec.writeU32(s.resolution().height);
        sec.writeF64(s.frameRate());
        sec.writeU8(static_cast<uint8_t>(s.colorSpace()));
        sec.writeU32(s.sampleRate());
        sec.writeU32(s.audioBitDepth());
        sec.writeU32(s.audioChannels());
        sec.writeString(s.exportSettings().codec);
        sec.writeU32(s.exportSettings().quality);
        sec.writeU32(s.exportSettings().audioBitrate);
        sec.writeString(s.exportSettings().outputPath);
        out.beginSection(ProjectSerializer::Section_Settings, sec.data());
        ++sectionCount;
    }

    // ── Section: Timeline metadata (backward compat — active sequence) ──
    {
        BinaryWriter sec;
        const Timeline* tl = project.timeline();
        sec.writeString(tl->name());
        sec.writeString(project.name());
        sec.writeI64(tl->playheadPosition());
        sec.writeI64(tl->inPoint());
        sec.writeI64(tl->outPoint());
        out.beginSection(ProjectSerializer::Section_Timeline, sec.data());
        ++sectionCount;
    }

    // ── Section: Tracks + Clips (backward compat — active sequence) ─────
    {
        BinaryWriter sec;
        const Timeline* tl = project.timeline();
        sec.writeU32(static_cast<uint32_t>(tl->trackCount()));

        for (size_t ti = 0; ti < tl->trackCount(); ++ti)
        {
            const Track* track = tl->track(ti);
            sec.writeU8(static_cast<uint8_t>(track->type()));
            sec.writeString(track->name());
            sec.writeU8(track->isLocked() ? 1 : 0);
            sec.writeU8(track->isMuted() ? 1 : 0);
            sec.writeU8(track->isSoloed() ? 1 : 0);
            sec.writeF32(track->height());
            sec.writeU8(track->isSyncLocked() ? 1 : 0); // v5+
            sec.writeU8(track->isDivider() ? 1 : 0);    // v18+
            sec.writeU8(track->isPermanentDivider() ? 1 : 0); // v20+
            sec.writeU8(track->isCaptionTrack() ? 1 : 0); // v21+

            // Clips for this track
            sec.writeU32(static_cast<uint32_t>(track->clipCount()));
            for (size_t ci = 0; ci < track->clipCount(); ++ci)
            {
                writeClip(sec, *track->clip(ci));
            }

            // Transitions for this track
            sec.writeU32(static_cast<uint32_t>(track->transitionCount()));
            for (size_t xi = 0; xi < track->transitionCount(); ++xi)
            {
                const Transition* t = track->transition(xi);
                sec.writeU8(static_cast<uint8_t>(t->type));
                sec.writeI64(t->duration);
                sec.writeI64(t->offset);
                sec.writeF32(t->param1);
                sec.writeF32(t->param2);
                // v16+: clip linkage + edit-point position. Without these
                // the transition collapses to tick 0 on reload and is
                // effectively lost.
                sec.writeU64(t->leftClipId);
                sec.writeU64(t->rightClipId);
                sec.writeI64(t->editPointTick);
            }
        }

        out.beginSection(ProjectSerializer::Section_Tracks, sec.data());
        ++sectionCount;
    }

    // ── Section: Markers (backward compat — active sequence) ────────────
    {
        BinaryWriter sec;
        const auto& markers = project.timeline()->markers();
        sec.writeU32(static_cast<uint32_t>(markers.size()));
        for (const auto& m : markers)
        {
            sec.writeI64(m.time);
            sec.writeString(m.label);
            sec.writeU32(m.color);
        }
        out.beginSection(ProjectSerializer::Section_Markers, sec.data());
        ++sectionCount;
    }

    // ── Section: Sequences (all sequences including non-active) ─────────
    {
        BinaryWriter sec;
        sec.writeU32(static_cast<uint32_t>(project.sequenceCount()));
        sec.writeU32(static_cast<uint32_t>(project.activeSequenceIndex()));

        for (size_t si = 0; si < project.sequenceCount(); ++si)
        {
            const Timeline* tl = project.sequence(si);

            // Sequence metadata
            sec.writeString(tl->name());
            sec.writeI64(tl->playheadPosition());
            sec.writeI64(tl->inPoint());
            sec.writeI64(tl->outPoint());

            // Tracks + clips
            sec.writeU32(static_cast<uint32_t>(tl->trackCount()));
            for (size_t ti = 0; ti < tl->trackCount(); ++ti)
            {
                const Track* track = tl->track(ti);
                sec.writeU8(static_cast<uint8_t>(track->type()));
                sec.writeString(track->name());
                sec.writeU8(track->isLocked() ? 1 : 0);
                sec.writeU8(track->isMuted() ? 1 : 0);
                sec.writeU8(track->isSoloed() ? 1 : 0);
                sec.writeF32(track->height());
                sec.writeU8(track->isSyncLocked() ? 1 : 0); // v5+
                sec.writeU8(track->isDivider() ? 1 : 0);    // v18+
                sec.writeU8(track->isPermanentDivider() ? 1 : 0); // v20+
                sec.writeU8(track->isCaptionTrack() ? 1 : 0); // v21+

                sec.writeU32(static_cast<uint32_t>(track->clipCount()));
                for (size_t ci = 0; ci < track->clipCount(); ++ci)
                    writeClip(sec, *track->clip(ci));

                sec.writeU32(static_cast<uint32_t>(track->transitionCount()));
                for (size_t xi = 0; xi < track->transitionCount(); ++xi)
                {
                    const Transition* t = track->transition(xi);
                    sec.writeU8(static_cast<uint8_t>(t->type));
                    sec.writeI64(t->duration);
                    sec.writeI64(t->offset);
                    sec.writeF32(t->param1);
                    sec.writeF32(t->param2);
                    // v16+: clip linkage + edit-point position
                    sec.writeU64(t->leftClipId);
                    sec.writeU64(t->rightClipId);
                    sec.writeI64(t->editPointTick);
                }
            }

            // Markers for this sequence
            const auto& markers = tl->markers();
            sec.writeU32(static_cast<uint32_t>(markers.size()));
            for (const auto& m : markers)
            {
                sec.writeI64(m.time);
                sec.writeString(m.label);
                sec.writeU32(m.color);
            }

            // Per-sequence settings (v25+) — resolution/fps/colour/audio are
            // independent per sequence, like Premiere Pro.
            const auto& ss = tl->settings();
            sec.writeU32(ss.resolution().width);
            sec.writeU32(ss.resolution().height);
            sec.writeF64(ss.frameRate());
            sec.writeU8(static_cast<uint8_t>(ss.colorSpace()));
            sec.writeU32(ss.sampleRate());
            sec.writeU32(ss.audioBitDepth());
            sec.writeU32(ss.audioChannels());
            sec.writeString(ss.exportSettings().codec);
            sec.writeU32(ss.exportSettings().quality);
            sec.writeU32(ss.exportSettings().audioBitrate);
            sec.writeString(ss.exportSettings().outputPath);
        }

        out.beginSection(ProjectSerializer::Section_Sequences, sec.data());
        ++sectionCount;
    }

    // ── Section: Asset entries ──────────────────────────────────────────
    {
        BinaryWriter sec;
        const AssetDatabase* db = project.assets();
        sec.writeU32(static_cast<uint32_t>(db->assetCount()));

        // Serialize all assets by type
        for (auto t : {AssetType::Character, AssetType::Background, AssetType::Audio,
                       AssetType::Video, AssetType::Image, AssetType::Font})
        {
            auto assets = db->findByType(t);
            for (const AssetEntry* a : assets)
            {
                sec.writeU64(a->id);
                sec.writeU8(static_cast<uint8_t>(a->type));
                sec.writeString(a->name);
                sec.writePath(a->path);
                sec.writePath(a->absolutePath);
                sec.writeU64(a->fileSize);
                sec.writeString(a->hash);
            }
        }

        out.beginSection(ProjectSerializer::Section_Assets, sec.data());
        ++sectionCount;
    }

    // ── Section: Character assets ───────────────────────────────────────
    {
        BinaryWriter sec;
        const auto& chars = project.assets()->characters();
        sec.writeU32(static_cast<uint32_t>(chars.size()));
        for (const auto& c : chars)
        {
            sec.writeString(c.name);
            sec.writePath(c.basePath);
            sec.writeU32(static_cast<uint32_t>(c.outfits.size()));
            for (const auto& o : c.outfits) sec.writeString(o);
            sec.writeU32(static_cast<uint32_t>(c.stances.size()));
            for (const auto& s : c.stances) sec.writeString(s);
        }
        out.beginSection(ProjectSerializer::Section_Characters, sec.data());
        ++sectionCount;
    }

    // ── Section: Bin state (v14+: rich per-instance items + folders) ────
    {
        BinaryWriter sec;

        // Rich bin items: each is an independent bin entry (footage can
        // appear multiple times as separate "master clips"). Falls back
        // to deriving from binFiles() for projects that never populated
        // the rich model.
        const auto& binItems = project.binItems();
        if (!binItems.empty()) {
            sec.writeU32(static_cast<uint32_t>(binItems.size()));
            for (const auto& it : binItems) {
                sec.writeU64(it.id);
                sec.writePath(it.path);
                sec.writeString(it.displayName);
                sec.writeU32(it.labelColor);
            }
        } else {
            const auto& binFiles = project.binFiles();
            sec.writeU32(static_cast<uint32_t>(binFiles.size()));
            uint64_t synthId = 1;
            for (const auto& f : binFiles) {
                sec.writeU64(synthId++);
                sec.writePath(f);
                sec.writeString(std::string{});      // no custom name
                sec.writeU32(0xFF888888u);           // default label
            }
        }

        const auto& binFolders = project.binFolders();
        sec.writeU32(static_cast<uint32_t>(binFolders.size()));
        for (const auto& bf : binFolders)
        {
            sec.writeString(bf.name);
            sec.writeU8(bf.expanded ? 1 : 0);
            sec.writeU32(static_cast<uint32_t>(bf.childKeys.size()));
            for (const auto& k : bf.childKeys)
                sec.writeString(k);
        }

        out.beginSection(ProjectSerializer::Section_BinState, sec.data());
        ++sectionCount;
    }

    // ── Section: AudioSync state (opaque blob from AudioSync panel) ─────
    {
        const auto& blob = project.audioSyncBlob();
        if (!blob.empty()) {
            out.beginSection(ProjectSerializer::Section_AudioSync, blob);
            ++sectionCount;
        }
    }

    // ── Section: Project metadata (show assignment) ─────────────────────
    if (!project.show().empty()) {
        BinaryWriter sec;
        sec.writeString(project.show());
        out.beginSection(ProjectSerializer::Section_ProjectMeta, sec.data());
        ++sectionCount;
    }

    // ── Section: Per-project workspace state ─────────────────────────────
    // Keep this separate from sequence content so future UI state can evolve
    // without changing the timeline record layout.
    {
        BinaryWriter sec;
        const auto& openIndices = project.openSequenceIndices();
        sec.writeU32(static_cast<uint32_t>(openIndices.size()));
        for (size_t index : openIndices)
            sec.writeU32(static_cast<uint32_t>(index));
        out.beginSection(ProjectSerializer::Section_WorkspaceState, sec.data());
        ++sectionCount;
    }

    // ── Patch section count ─────────────────────────────────────────────
    auto& d = const_cast<std::vector<uint8_t>&>(out.data());
    d[sectionCountPos]     = static_cast<uint8_t>(sectionCount);
    d[sectionCountPos + 1] = static_cast<uint8_t>(sectionCount >> 8);
    d[sectionCountPos + 2] = static_cast<uint8_t>(sectionCount >> 16);
    d[sectionCountPos + 3] = static_cast<uint8_t>(sectionCount >> 24);

    return out.data();
}

} // namespace rt::legacy_v47
