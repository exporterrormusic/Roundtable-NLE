#include "audiofx/EssentialSound.h"

#include "audiofx/Dynamics.h"
#include "audiofx/ParametricEQ.h"
#include "audiofx/RepairFx.h"
#include "audiofx/Reverb.h"

#include <algorithm>

namespace rt::audiofx {

namespace {

/// 0-10 slider -> 0-1.
float unit(float amount) { return std::clamp(amount, 0.0f, 10.0f) / 10.0f; }

struct BandDef { Biquad::Type type; float freq; float gainDb; float q; };

ParametricEQ* addEq(FxChain& c)
{
    return static_cast<ParametricEQ*>(c.add(ProcessorKind::ParametricEQ));
}

void setBands(ParametricEQ& eq, std::initializer_list<BandDef> bands, float scale, int first = 0)
{
    int i = first;
    for (const BandDef& b : bands) {
        if (i >= eq.bandCount()) break;
        ParametricEQ::Band band;
        band.enabled = true;
        band.type = b.type;
        band.freqHz = b.freq;
        band.gainDb = b.gainDb * scale;
        band.q = b.q;
        eq.setBand(i++, band);
    }
}

constexpr auto Peak = Biquad::Type::Peaking;
constexpr auto LowShelf = Biquad::Type::LowShelf;
constexpr auto HighShelf = Biquad::Type::HighShelf;

} // namespace

const char* soundTypeName(SoundType t) noexcept
{
    switch (t) {
    case SoundType::None:     return "None";
    case SoundType::Dialogue: return "Dialogue";
    case SoundType::Music:    return "Music";
    case SoundType::SFX:      return "SFX";
    case SoundType::Ambience: return "Ambience";
    case SoundType::Count:    break;
    }
    return "None";
}

const char* clarityEqPresetName(ClarityEqPreset p) noexcept
{
    switch (p) {
    case ClarityEqPreset::Subtle:          return "Subtle";
    case ClarityEqPreset::PodcastVoice:    return "Podcast Voice";
    case ClarityEqPreset::WarmVoice:       return "Warm Voice";
    case ClarityEqPreset::BackgroundVoice: return "Background Voice";
    case ClarityEqPreset::Telephone:       return "On the Telephone";
    case ClarityEqPreset::OldRadio:        return "Old Radio";
    case ClarityEqPreset::Intercom:        return "Intercom";
    case ClarityEqPreset::OutsideTheRoom:  return "Outside the Room";
    case ClarityEqPreset::Count:           break;
    }
    return "Subtle";
}

const char* spaceReverbPresetName(SpaceReverbPreset p) noexcept
{
    switch (p) {
    case SpaceReverbPreset::SmallRoom:  return "Small Room";
    case SpaceReverbPreset::LargeRoom:  return "Large Room";
    case SpaceReverbPreset::Auditorium: return "Auditorium";
    case SpaceReverbPreset::Church:     return "Church";
    case SpaceReverbPreset::Outside:    return "Outside";
    case SpaceReverbPreset::Distant:    return "Distant";
    case SpaceReverbPreset::Count:      break;
    }
    return "Small Room";
}

float defaultLoudnessTarget(SoundType t) noexcept
{
    switch (t) {
    case SoundType::Music:    return -25.0f;
    case SoundType::SFX:      return -21.0f;
    case SoundType::Ambience: return -30.0f;
    default:                  return -23.0f;
    }
}

FxChain compileEssentialSound(const EssentialSoundSettings& s)
{
    FxChain c;
    if (s.type == SoundType::None) return c;
    const bool dialogue = s.type == SoundType::Dialogue;

    // ── Repair (Dialogue) ───────────────────────────────────────────────
    if (dialogue && s.reduceRumble) {
        // 40 Hz .. 160 Hz high-pass, steeper as the slider goes up.
        auto* eq = addEq(c);
        const float k = unit(s.rumbleAmount);
        eq->setHighPass(true, 40.0f + 120.0f * k, 0.707f + 0.3f * k);
    }
    if (dialogue && s.deHum) {
        auto* p = static_cast<ParamProcessor*>(c.add(ProcessorKind::DeHum));
        p->setParam(DeHum::MainsFrequency, s.hum60Hz ? 1.0f : 0.0f);
        p->setParam(DeHum::Harmonics, 2.0f + 6.0f * unit(s.deHumAmount));
        p->setParam(DeHum::Depth, 40.0f + 60.0f * unit(s.deHumAmount));
    }
    if (dialogue && s.deEss) {
        auto* p = static_cast<ParamProcessor*>(c.add(ProcessorKind::DeEsser));
        const float k = unit(s.deEssAmount);
        p->setParam(DeEsser::Threshold, -18.0f - 22.0f * k);
        p->setParam(DeEsser::MaxReduction, 3.0f + 12.0f * k);
    }

    // ── Clarity (Dialogue) ──────────────────────────────────────────────
    if (dialogue && s.dynamics) {
        auto* d = static_cast<Dynamics*>(c.add(ProcessorKind::Dynamics));
        const float k = unit(s.dynamicsAmount);
        Dynamics::GateSettings g;            // gentle expander for room tone
        g.enabled = true;
        g.thresholdDb = -55.0f + 10.0f * k;
        g.ratio = 1.5f + k;
        g.rangeDb = -6.0f - 9.0f * k;
        d->setGate(g);
        Dynamics::CompSettings cs;
        cs.enabled = true;
        cs.thresholdDb = -12.0f - 14.0f * k;
        cs.ratio = 1.5f + 3.5f * k;
        cs.kneeDb = 8.0f;
        cs.attackMs = 8.0f;
        cs.releaseMs = 140.0f;
        cs.makeupDb = 1.0f + 6.0f * k;
        d->setCompressor(cs);
        Dynamics::LimiterSettings l;
        l.enabled = true;
        l.ceilingDb = -1.0f;
        d->setLimiter(l);
    }
    if (dialogue && (s.clarityEq || s.enhanceSpeech)) {
        auto* eq = addEq(c);
        int nextBand = 0;
        if (s.clarityEq) {
            const float k = unit(s.eqAmount);
            switch (static_cast<ClarityEqPreset>(std::clamp(s.eqPreset, 0,
                        static_cast<int>(ClarityEqPreset::Count) - 1))) {
            case ClarityEqPreset::Subtle:
                setBands(*eq, {{Peak, 250.0f, -1.5f, 1.0f}, {Peak, 3500.0f, 2.0f, 1.0f}}, k);
                nextBand = 2;
                break;
            case ClarityEqPreset::PodcastVoice:
                eq->setHighPass(true, 80.0f, 0.707f);
                setBands(*eq, {{LowShelf, 120.0f, 3.0f, 0.7f}, {Peak, 300.0f, -2.5f, 1.0f},
                               {Peak, 5000.0f, 3.0f, 0.8f}}, k);
                nextBand = 3;
                break;
            case ClarityEqPreset::WarmVoice:
                setBands(*eq, {{LowShelf, 200.0f, 3.0f, 0.7f}, {HighShelf, 8000.0f, -2.0f, 0.7f}}, k);
                nextBand = 2;
                break;
            case ClarityEqPreset::BackgroundVoice:
                eq->setHighPass(true, 200.0f, 0.707f);
                eq->setLowPass(true, 6000.0f, 0.707f);
                setBands(*eq, {{Peak, 2500.0f, -3.0f, 0.8f}}, k);
                nextBand = 1;
                break;
            case ClarityEqPreset::Telephone:
                eq->setHighPass(true, 400.0f, 1.0f);
                eq->setLowPass(true, 3200.0f, 1.0f);
                setBands(*eq, {{Peak, 1500.0f, 5.0f, 1.0f}}, k);
                nextBand = 1;
                break;
            case ClarityEqPreset::OldRadio:
                eq->setHighPass(true, 300.0f, 1.0f);
                eq->setLowPass(true, 4500.0f, 1.0f);
                setBands(*eq, {{Peak, 1800.0f, 6.0f, 1.0f}}, k);
                nextBand = 1;
                break;
            case ClarityEqPreset::Intercom:
                eq->setHighPass(true, 500.0f, 1.0f);
                eq->setLowPass(true, 2800.0f, 1.0f);
                setBands(*eq, {{Peak, 1200.0f, 7.0f, 1.2f}}, k);
                nextBand = 1;
                break;
            case ClarityEqPreset::OutsideTheRoom:
                eq->setLowPass(true, 900.0f, 0.707f);
                eq->setOutputGainDb(-4.0f * k);
                break;
            case ClarityEqPreset::Count:
                break;
            }
        }
        if (s.enhanceSpeech) {
            // Presence + air, a gentle low-mid cut for mud.
            setBands(*eq, {{Peak, 3000.0f, 3.0f, 0.9f}, {HighShelf, 10000.0f, 2.0f, 0.7f},
                           {Peak, 400.0f, -1.5f, 1.2f}}, 1.0f, nextBand);
        }
    }

    // ── Creative reverb (Dialogue, SFX, Ambience) ───────────────────────
    if (s.reverb && s.type != SoundType::Music) {
        auto* r = static_cast<ParamProcessor*>(c.add(ProcessorKind::Reverb));
        const float k = unit(s.reverbAmount);
        struct Space { float room, decay, pre, damp, er, hiCut, dry; };
        static const Space spaces[] = {
            {25, 0.5f,  4, 55, 45, 9000, 100},   // Small Room
            {55, 1.1f, 10, 50, 40, 8000, 100},   // Large Room
            {80, 2.0f, 20, 45, 35, 7500, 100},   // Auditorium
            {100, 3.6f, 35, 35, 30, 7000, 95},   // Church
            {40, 0.7f, 25, 75, 60, 5000, 100},   // Outside (sparse, dark)
            {70, 1.4f, 30, 60, 55, 6000, 60},    // Distant (less direct sound)
        };
        const Space& sp = spaces[std::clamp(s.reverbPreset, 0,
                                            static_cast<int>(SpaceReverbPreset::Count) - 1)];
        r->setParam(Reverb::RoomSize, sp.room);
        r->setParam(Reverb::Decay, sp.decay);
        r->setParam(Reverb::PreDelay, sp.pre);
        r->setParam(Reverb::Damping, sp.damp);
        r->setParam(Reverb::EarlyReflections, sp.er);
        r->setParam(Reverb::HighCut, sp.hiCut);
        r->setParam(Reverb::Dry, sp.dry);
        r->setParam(Reverb::Wet, 60.0f * k);
    }

    // ── Ambience width ──────────────────────────────────────────────────
    if (s.type == SoundType::Ambience && s.stereoWidth) {
        auto* w = static_cast<ParamProcessor*>(c.add(ProcessorKind::StereoWidth));
        w->setParam(StereoWidth::Width, 100.0f + 10.0f * std::clamp(s.widthAmount, 0.0f, 10.0f));
    }
    return c;
}

std::vector<std::pair<int64_t, int64_t>>
computeDuckingRanges(std::vector<std::pair<int64_t, int64_t>> dialogue, int64_t mergeGap,
                     int64_t fade)
{
    std::vector<std::pair<int64_t, int64_t>> out;
    std::sort(dialogue.begin(), dialogue.end());
    for (auto [a, b] : dialogue) {
        if (b <= a) continue;
        a -= fade;
        b += fade;
        if (!out.empty() && a <= out.back().second + mergeGap)
            out.back().second = std::max(out.back().second, b);
        else
            out.push_back({a, b});
    }
    return out;
}

} // namespace rt::audiofx
