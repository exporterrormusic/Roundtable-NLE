#include "audiofx/AudioPresets.h"

#include "audiofx/Delay.h"
#include "audiofx/Distortion.h"
#include "audiofx/Dynamics.h"
#include "audiofx/ParametricEQ.h"
#include "audiofx/Reverb.h"

#include <initializer_list>
#include <utility>

namespace rt::audiofx {

namespace {

using P = AudioPreset;

// ── Recipe helpers ───────────────────────────────────────────────────────────

struct BandDef { Biquad::Type type; float freq; float gainDb; float q; };

/// EQ with optional high-pass / low-pass (0 Hz = off) and up to 8 bands.
void addEq(FxChain& c, float hpHz, float lpHz, std::initializer_list<BandDef> bands,
           float outDb = 0.0f, float filterQ = 0.707f)
{
    auto* eq = static_cast<ParametricEQ*>(c.add(ProcessorKind::ParametricEQ));
    if (hpHz > 0.0f) eq->setHighPass(true, hpHz, filterQ);
    if (lpHz > 0.0f) eq->setLowPass(true, lpHz, filterQ);
    int i = 0;
    for (const BandDef& b : bands) {
        if (i >= eq->bandCount()) break;
        ParametricEQ::Band band;
        band.enabled = true;
        band.type = b.type;
        band.freqHz = b.freq;
        band.gainDb = b.gainDb;
        band.q = b.q;
        eq->setBand(i++, band);
    }
    if (outDb != 0.0f) eq->setOutputGainDb(outDb);
}

/// A ParamProcessor of `kind` with the listed {param, value} overrides.
void addParams(FxChain& c, ProcessorKind kind, std::initializer_list<std::pair<int, double>> values)
{
    auto* p = static_cast<ParamProcessor*>(c.add(kind));
    for (const auto& [index, value] : values) p->setParam(index, static_cast<float>(value));
}

void addReverb(FxChain& c, std::initializer_list<std::pair<int, double>> v)
{
    addParams(c, ProcessorKind::Reverb, v);
}

void addDelay(FxChain& c, std::initializer_list<std::pair<int, double>> v)
{
    addParams(c, ProcessorKind::Delay, v);
}

void addDistortion(FxChain& c, std::initializer_list<std::pair<int, double>> v)
{
    addParams(c, ProcessorKind::Distortion, v);
}

void addCompressor(FxChain& c, float thresholdDb, float ratio, float makeupDb)
{
    auto* d = static_cast<Dynamics*>(c.add(ProcessorKind::Dynamics));
    Dynamics::CompSettings s;
    s.enabled = true;
    s.thresholdDb = thresholdDb;
    s.ratio = ratio;
    s.makeupDb = makeupDb;
    s.attackMs = 5.0f;
    s.releaseMs = 120.0f;
    d->setCompressor(s);
}

constexpr auto Peak = Biquad::Type::Peaking;
constexpr auto LowShelf = Biquad::Type::LowShelf;
constexpr auto HighShelf = Biquad::Type::HighShelf;

using R = Reverb;
using D = Delay;
using X = Distortion;

} // namespace

const char* audioPresetName(AudioPreset p) noexcept
{
    switch (p) {
    case P::MakeCloseUp:    return "Make Close Up";
    case P::MakeMediumShot: return "Make Medium Shot";
    case P::MakeDistant:    return "Make Distant";
    case P::SmallRoom:      return "Small Room";
    case P::LargeHall:      return "Large Hall";
    case P::Cathedral:      return "Cathedral";
    case P::NextRoom:       return "From the Next Room";
    case P::Underwater:     return "Underwater";
    case P::Telephone:      return "Telephone";
    case P::OldRadio:       return "Old Radio";
    case P::WalkieTalkie:   return "Walkie-Talkie";
    case P::Megaphone:      return "Megaphone";
    case P::Echo:           return "Echo (Canyon)";
    case P::Slapback:       return "Slapback Echo";
    case P::PodcastVoice:   return "Podcast Voice";
    case P::Count:          break;
    }
    return "Unknown";
}

const char* audioPresetCategory(AudioPreset p) noexcept
{
    switch (p) {
    case P::MakeCloseUp: case P::MakeMediumShot: case P::MakeDistant:
        return "Distance";
    case P::SmallRoom: case P::LargeHall: case P::Cathedral: case P::NextRoom: case P::Underwater:
        return "Spaces";
    case P::Telephone: case P::OldRadio: case P::WalkieTalkie: case P::Megaphone:
        return "Devices";
    case P::Echo: case P::Slapback:
        return "Echo";
    case P::PodcastVoice:
        return "Voice";
    case P::Count:
        break;
    }
    return "";
}

void applyAudioPreset(FxChain& c, AudioPreset p)
{
    switch (p) {
    // ── Distance: proximity lows + presence for close, less direct sound,
    //    more (pre-delayed) room and fewer highs as the source moves away.
    case P::MakeCloseUp:
        addEq(c, 60.0f, 0.0f, {{LowShelf, 160.0f, 2.5f, 0.7f}, {Peak, 4000.0f, 2.0f, 1.0f}});
        addReverb(c, {{R::RoomSize, 15}, {R::Decay, 0.3f}, {R::PreDelay, 0}, {R::Damping, 60},
                      {R::EarlyReflections, 20}, {R::HighCut, 9000}, {R::Wet, 6}});
        break;
    case P::MakeMediumShot:
        addEq(c, 90.0f, 0.0f, {{HighShelf, 6000.0f, -2.0f, 0.7f}});
        addReverb(c, {{R::RoomSize, 40}, {R::Decay, 0.7f}, {R::PreDelay, 8}, {R::Damping, 55},
                      {R::EarlyReflections, 40}, {R::HighCut, 8000}, {R::Wet, 18}});
        break;
    case P::MakeDistant:
        addEq(c, 180.0f, 6500.0f, {{LowShelf, 250.0f, -4.0f, 0.7f}});
        addReverb(c, {{R::RoomSize, 70}, {R::Decay, 1.4f}, {R::PreDelay, 30}, {R::Damping, 60},
                      {R::EarlyReflections, 55}, {R::HighCut, 6000}, {R::Dry, 55}, {R::Wet, 40}});
        break;

    // ── Spaces
    case P::SmallRoom:
        addReverb(c, {{R::RoomSize, 25}, {R::Decay, 0.5f}, {R::PreDelay, 4}, {R::Damping, 55},
                      {R::EarlyReflections, 45}, {R::Wet, 20}});
        break;
    case P::LargeHall:
        addReverb(c, {{R::RoomSize, 85}, {R::Decay, 2.6f}, {R::PreDelay, 25}, {R::Damping, 45},
                      {R::Diffusion, 85}, {R::EarlyReflections, 35}, {R::Wet, 30}});
        break;
    case P::Cathedral:
        addReverb(c, {{R::RoomSize, 100}, {R::Decay, 4.0f}, {R::PreDelay, 40}, {R::Damping, 35},
                      {R::Diffusion, 90}, {R::EarlyReflections, 30}, {R::Dry, 80}, {R::Wet, 38}});
        break;
    case P::NextRoom:
        addEq(c, 100.0f, 700.0f, {}, -4.0f);
        addReverb(c, {{R::RoomSize, 35}, {R::Decay, 0.5f}, {R::Damping, 70},
                      {R::EarlyReflections, 40}, {R::HighCut, 3000}, {R::Dry, 80}, {R::Wet, 20}});
        break;
    case P::Underwater:
        addEq(c, 0.0f, 350.0f, {{LowShelf, 120.0f, 3.0f, 0.7f}}, 0.0f, 1.2f);
        addReverb(c, {{R::RoomSize, 60}, {R::Decay, 1.0f}, {R::Damping, 90},
                      {R::HighCut, 1500}, {R::Wet, 35}});
        break;

    // ── Devices: narrow band-pass + resonance + some grit
    case P::Telephone:
        addEq(c, 400.0f, 3200.0f, {{Peak, 1500.0f, 4.0f, 1.0f}}, 0.0f, 1.0f);
        addDistortion(c, {{X::Type, X::SoftClip}, {X::Drive, 8}, {X::Tone, 3500}, {X::Mix, 60}, {X::Output, -3}});
        addCompressor(c, -24.0f, 4.0f, 6.0f);
        break;
    case P::OldRadio:
        addEq(c, 300.0f, 4500.0f, {{Peak, 1800.0f, 5.0f, 1.0f}}, 0.0f, 1.0f);
        addDistortion(c, {{X::Type, X::Tube}, {X::Drive, 14}, {X::Tone, 5000}, {X::Mix, 50}, {X::Output, -4}});
        break;
    case P::WalkieTalkie:
        addEq(c, 500.0f, 2800.0f, {{Peak, 1200.0f, 6.0f, 1.2f}}, 0.0f, 1.0f);
        addDistortion(c, {{X::Type, X::HardClip}, {X::Drive, 20}, {X::Tone, 3000}, {X::Mix, 40}, {X::Output, -6}});
        addCompressor(c, -25.0f, 6.0f, 6.0f);
        break;
    case P::Megaphone:
        addEq(c, 600.0f, 4000.0f, {{Peak, 2000.0f, 8.0f, 1.5f}}, 0.0f, 1.0f);
        addDistortion(c, {{X::Type, X::HardClip}, {X::Drive, 22}, {X::Tone, 4500}, {X::Mix, 70}, {X::Output, -8}});
        break;

    // ── Echo
    case P::Echo:
        addDelay(c, {{D::Time, 450}, {D::Feedback, 45}, {D::HighCut, 4500}, {D::Wet, 40}});
        addReverb(c, {{R::RoomSize, 60}, {R::Decay, 1.2f}, {R::Wet, 10}});
        break;
    case P::Slapback:
        addDelay(c, {{D::Time, 110}, {D::Feedback, 0}, {D::HighCut, 7000}, {D::Wet, 35}});
        break;

    // ── Voice
    case P::PodcastVoice:
        addEq(c, 80.0f, 0.0f, {{LowShelf, 120.0f, 3.0f, 0.7f}, {Peak, 300.0f, -2.0f, 1.0f},
                               {Peak, 5000.0f, 3.0f, 0.8f}});
        static_cast<Dynamics*>(c.add(ProcessorKind::Dynamics))->loadVoicePreset();
        break;

    case P::Count:
        break;
    }
}

bool applyAudioFxCode(FxChain& chain, int code)
{
    if (code >= kPresetCodeBase) {
        const int idx = code - kPresetCodeBase;
        if (idx >= static_cast<int>(AudioPreset::Count)) return false;
        applyAudioPreset(chain, static_cast<AudioPreset>(idx));
        return true;
    }
    if (code < 0 || code >= static_cast<int>(ProcessorKind::Count)) return false;
    const auto kind = static_cast<ProcessorKind>(code);
    AudioProcessor* p = chain.add(kind);
    if (kind == ProcessorKind::Dynamics && p)
        static_cast<Dynamics*>(p)->loadVoicePreset();
    return p != nullptr;
}

const char* audioFxCodeName(int code) noexcept
{
    if (code >= kPresetCodeBase) {
        const int idx = code - kPresetCodeBase;
        if (idx >= static_cast<int>(AudioPreset::Count)) return "Unknown";
        return audioPresetName(static_cast<AudioPreset>(idx));
    }
    if (code < 0 || code >= static_cast<int>(ProcessorKind::Count)) return "Unknown";
    return processorDisplayName(static_cast<ProcessorKind>(code));
}

} // namespace rt::audiofx
