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
#include "timeline/KeyframeTrack.h"
#include "effects/Effect.h"
#include "effects/EffectStack.h"
#include "effects/LUT.h"
#include "timeline/OpacityMask.h"

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
    fs::path p(path);

    // Wells-specific migration to HEVC stacked-alpha.
    {
        std::string filename = p.filename().string();
        std::string lower = filename;
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char c) { return std::tolower(c); });

        bool isWellsMute = (lower.find("wells-chrono-mute") != std::string::npos);
        bool isWellsTalk = (lower.find("wells-chrono-talk") != std::string::npos);

        if (isWellsMute || isWellsTalk) {
            fs::path hevcPath = p;
            std::string suffix = isWellsMute ? "WELLS-CHRONO-MUTE_HEVC.mp4"
                                             : "WELLS-CHRONO-TALK_HEVC.mp4";
            hevcPath.replace_filename(suffix);
            if (fs::exists(hevcPath)) {
                spdlog::info("ClipSerialization: Wells migration '{}' -> '{}'",
                             path, hevcPath.string());
                return hevcPath.string();
            }
        }
    }

    if (fs::exists(p)) return path;

    auto ext = p.extension().string();
    fs::path alt;
    if (ext == ".mov")       alt = p.replace_extension(".mp4");
    else if (ext == ".mp4")  alt = p.replace_extension(".mov");
    else if (ext == ".webm") alt = p.replace_extension(".mp4");
    else return path;

    if (fs::exists(alt)) {
        spdlog::info("ClipSerialization: re-resolved missing '{}' → '{}'",
                     path, alt.string());
        return alt.string();
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
}

std::unique_ptr<Clip> readClip(BinaryReader& r, uint32_t version)
{
    auto type = static_cast<ClipType>(r.readU8());
    uint64_t id = r.readU64();
    (void)id; // IDs are regenerated on load for safety

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
    default:
        return nullptr;
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
                clip->effects().addEffect(std::move(fx));
            } else {
                // Unknown effect type — skip its data
                for (uint32_t pi = 0; pi < paramCount; ++pi) {
                    KeyframeTrack<float> discard(0.0f);
                    readKeyframeTrack(r, discard, version);
                }
                if (fxType == EffectType::LUT)
                    r.readString(); // discard LUT path
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

    return clip;
}
} // namespace rt