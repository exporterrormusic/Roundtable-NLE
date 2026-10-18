/*
 * LeafSchemas.h — record schemas for the small types clips and sequences
 * are built from: keyframes and keyframe tracks, masks, rich-text runs and
 * paragraphs, graphic appearance entries, transitions, markers, settings,
 * effects and the audio FX chain. See Schema.h for the rules; field ids are
 * append-only.
 */
#pragma once

#include "project/schema/Schema.h"

#include "audiofx/Dynamics.h"
#include "audiofx/ParametricEQ.h"
#include "effects/ColorGrading.h"
#include "project/Settings.h"
#include "timeline/GraphicLayer.h"
#include "timeline/Keyframe.h"
#include "timeline/KeyframeTrack.h"
#include "timeline/Marker.h"
#include "timeline/OpacityMask.h"
#include "timeline/Transition.h"

#include <memory>

namespace rt {
class Effect;
namespace audiofx { class FxChain; }
}

namespace rt::record {

#define RT_RECORD_SCHEMA(Type) \
    template <> struct SchemaOf<Type> { static const Schema<Type>& get(); }

RT_RECORD_SCHEMA(Keyframe<float>);
RT_RECORD_SCHEMA(KeyframeTrack<float>);
RT_RECORD_SCHEMA(MaskVertex);
RT_RECORD_SCHEMA(MaskGeometry);
RT_RECORD_SCHEMA(MaskPathKeyframe);
RT_RECORD_SCHEMA(OpacityMask);
RT_RECORD_SCHEMA(TextRunAppearance);
RT_RECORD_SCHEMA(TextStyleRun);
RT_RECORD_SCHEMA(TextParagraphStyle);
RT_RECORD_SCHEMA(FillEntry);
RT_RECORD_SCHEMA(StrokeEntry);
RT_RECORD_SCHEMA(ShadowEntry);
RT_RECORD_SCHEMA(Transition);
RT_RECORD_SCHEMA(Marker);
RT_RECORD_SCHEMA(Settings);
RT_RECORD_SCHEMA(ColorGrading::HslParams);
RT_RECORD_SCHEMA(audiofx::ParametricEQ::Band);
RT_RECORD_SCHEMA(audiofx::Dynamics::GateSettings);
RT_RECORD_SCHEMA(audiofx::Dynamics::CompSettings);
RT_RECORD_SCHEMA(audiofx::Dynamics::LimiterSettings);

#undef RT_RECORD_SCHEMA

/// An effect as a record: its type, then the fields for that type.
void writeEffect(RecordWriter& w, const Effect& effect);
/// The effect a record describes, or nullptr when its type is unknown to
/// this build (the effect is then left out rather than failing the load).
/// A malformed record fails `r`.
[[nodiscard]] std::unique_ptr<Effect> readEffect(RecordReader& r);

/// The audio FX chain as a list record of processors.
void writeFxChain(RecordWriter& w, uint32_t id, const audiofx::FxChain& chain);
/// Replaces `chain` with the processors in the current field of `r`.
/// Processor kinds this build doesn't know are left out.
bool readFxChain(RecordReader& r, audiofx::FxChain& chain);

} // namespace rt::record
