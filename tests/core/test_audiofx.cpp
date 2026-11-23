/*
 * test_audiofx.cpp — unit tests for the src/core/audiofx DSP module.
 *
 * Verifies the building blocks (Biquad), the ParametricEQ, the unified
 * Dynamics processor (gate / compressor / limiter), and the FxChain
 * container. Signals are synthetic so the tests are deterministic and
 * headless-safe (core group).
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <random>
#include <string>
#include <vector>

#include "audiofx/AudioPresets.h"
#include "audiofx/RepairFx.h"
#include "audiofx/Loudness.h"
#include "audiofx/PitchShifter.h"
#include "audiofx/EssentialSound.h"
#include "audiofx/AudioProcessor.h"
#include "audiofx/Delay.h"
#include "audiofx/Distortion.h"
#include "audiofx/Reverb.h"
#include "audiofx/Biquad.h"
#include "audiofx/Dynamics.h"
#include "audiofx/FxChain.h"
#include "audiofx/ParametricEQ.h"
#include "audiofx/ParamProcessor.h"

namespace rt::audiofx {
namespace {

constexpr double kSR = 48000.0;
constexpr float  kPi = 3.14159265358979323846f;

/// Interleaved sine. `channels` copies of the same tone.
std::vector<float> sine(float freq, float amp, int frames, int channels)
{
    std::vector<float> out(static_cast<size_t>(frames) * channels);
    for (int f = 0; f < frames; ++f) {
        const float v = amp * std::sin(2.0f * kPi * freq * f / static_cast<float>(kSR));
        for (int c = 0; c < channels; ++c)
            out[static_cast<size_t>(f) * channels + c] = v;
    }
    return out;
}

/// RMS over channel 0, skipping the first `skip` frames (settling time).
float rms(const std::vector<float>& buf, int channels, int skip = 0)
{
    const int frames = static_cast<int>(buf.size() / channels);
    double acc = 0.0;
    int n = 0;
    for (int f = skip; f < frames; ++f) {
        const float v = buf[static_cast<size_t>(f) * channels];
        acc += static_cast<double>(v) * v;
        ++n;
    }
    return n > 0 ? static_cast<float>(std::sqrt(acc / n)) : 0.0f;
}

float peakAbs(const std::vector<float>& buf, int channels, int skip = 0)
{
    const int frames = static_cast<int>(buf.size() / channels);
    float pk = 0.0f;
    for (int f = skip; f < frames; ++f)
        pk = std::max(pk, std::fabs(buf[static_cast<size_t>(f) * channels]));
    return pk;
}

// ── dB helpers ───────────────────────────────────────────────────────────────

TEST(AudioFxHelpers, DbLinearRoundTrip)
{
    EXPECT_NEAR(dbToLinear(0.0f), 1.0f, 1e-6f);
    EXPECT_NEAR(dbToLinear(-6.0206f), 0.5f, 1e-3f);
    EXPECT_NEAR(linearToDb(1.0f), 0.0f, 1e-4f);
    EXPECT_NEAR(linearToDb(0.5f), -6.0206f, 1e-2f);
    EXPECT_LE(linearToDb(0.0f), -100.0f);  // floored, not -inf
}

// ── Biquad ───────────────────────────────────────────────────────────────────

TEST(Biquad, IdentityPassesThrough)
{
    Biquad b;
    b.setIdentity();
    for (float x : {0.1f, -0.5f, 0.9f, -0.3f})
        EXPECT_NEAR(b.processSample(x), x, 1e-6f);
}

TEST(Biquad, LowPassAttenuatesHighFrequencies)
{
    Biquad b;
    b.setCoefficients(Biquad::Type::LowPass, kSR, 500.0, 0.707, 0.0);

    auto low  = sine(100.0f, 1.0f, 4096, 1);
    auto high = sine(8000.0f, 1.0f, 4096, 1);

    Biquad bl = b, bh = b;
    for (float& s : low)  s = bl.processSample(s);
    for (float& s : high) s = bh.processSample(s);

    // High frequency well below the corner must be far more attenuated.
    EXPECT_LT(rms(high, 1, 1024), rms(low, 1, 1024) * 0.25f);
    EXPECT_GT(rms(low, 1, 1024), 0.5f);  // passband roughly intact
}

TEST(Biquad, PeakingBoostsAtCenter)
{
    Biquad b;
    b.setCoefficients(Biquad::Type::Peaking, kSR, 1000.0, 1.0, 12.0);  // +12 dB

    auto tone = sine(1000.0f, 0.2f, 8192, 1);
    Biquad f = b;
    for (float& s : tone) s = f.processSample(s);

    // ~+12 dB ≈ x4 amplitude; allow generous settling tolerance.
    EXPECT_GT(rms(tone, 1, 4096), 0.2f * 0.707f * 3.0f);
}

// ── ParametricEQ ─────────────────────────────────────────────────────────────

TEST(ParametricEQ, FlatIsInactiveAndTransparent)
{
    ParametricEQ eq;
    eq.prepare(kSR, 2);
    EXPECT_FALSE(eq.isActive());

    auto buf = sine(440.0f, 0.5f, 1024, 2);
    auto orig = buf;
    eq.process(buf.data(), 1024);
    for (size_t i = 0; i < buf.size(); ++i)
        EXPECT_NEAR(buf[i], orig[i], 1e-6f);
}

TEST(ParametricEQ, HighPassRemovesLowFrequencies)
{
    ParametricEQ eq;
    eq.prepare(kSR, 2);
    eq.setHighPass(true, 300.0f, 0.707f);
    EXPECT_TRUE(eq.isActive());

    auto buf = sine(40.0f, 0.8f, 8192, 2);
    const float before = rms(buf, 2);
    eq.process(buf.data(), 8192);
    EXPECT_LT(rms(buf, 2, 2048), before * 0.3f);
}

TEST(ParametricEQ, OutputGainScales)
{
    ParametricEQ eq;
    eq.prepare(kSR, 2);
    eq.setOutputGainDb(-6.0206f);  // half amplitude
    EXPECT_TRUE(eq.isActive());

    auto buf = sine(1000.0f, 0.5f, 4096, 2);
    eq.process(buf.data(), 4096);
    EXPECT_NEAR(rms(buf, 2), 0.5f * 0.707f * 0.5f, 0.02f);
}

TEST(ParametricEQ, VoicePresetIsActive)
{
    ParametricEQ eq;
    eq.prepare(kSR, 2);
    eq.loadVoicePreset();
    EXPECT_TRUE(eq.isActive());
}

// ── Dynamics: gate ───────────────────────────────────────────────────────────

TEST(Dynamics, GateSilencesQuietSignal)
{
    Dynamics dyn;
    dyn.prepare(kSR, 2);
    Dynamics::GateSettings g;
    g.enabled = true; g.thresholdDb = -30.0f; g.ratio = 8.0f;
    g.rangeDb = -60.0f; g.attackMs = 1.0f; g.releaseMs = 50.0f;
    dyn.setGate(g);
    EXPECT_TRUE(dyn.isActive());

    // -50 dB tone is well below the -30 dB threshold.
    const float amp = dbToLinear(-50.0f);
    auto buf = sine(440.0f, amp, 16384, 2);
    const float before = rms(buf, 2);
    dyn.process(buf.data(), 16384);
    EXPECT_LT(rms(buf, 2, 8192), before * 0.2f);
    EXPECT_LT(dyn.gateReductionDb(), -10.0f);
}

TEST(Dynamics, GatePassesLoudSignal)
{
    Dynamics dyn;
    dyn.prepare(kSR, 2);
    Dynamics::GateSettings g;
    g.enabled = true; g.thresholdDb = -30.0f; g.ratio = 8.0f;
    g.rangeDb = -60.0f; g.attackMs = 1.0f; g.releaseMs = 50.0f;
    dyn.setGate(g);

    auto buf = sine(440.0f, 0.5f, 8192, 2);  // -6 dB, well above threshold
    const float before = rms(buf, 2);
    dyn.process(buf.data(), 8192);
    EXPECT_NEAR(rms(buf, 2, 4096), before, before * 0.1f);
}

// ── Dynamics: compressor ─────────────────────────────────────────────────────

TEST(Dynamics, CompressorReducesLoudSignal)
{
    Dynamics dyn;
    dyn.prepare(kSR, 2);
    Dynamics::CompSettings c;
    c.enabled = true; c.thresholdDb = -24.0f; c.ratio = 4.0f; c.kneeDb = 0.0f;
    c.makeupDb = 0.0f; c.attackMs = 5.0f; c.releaseMs = 100.0f;
    dyn.setCompressor(c);
    EXPECT_TRUE(dyn.isActive());

    auto buf = sine(440.0f, 1.0f, 24000, 2);  // 0 dB peak
    dyn.process(buf.data(), 24000);

    // Peak driven near 0 dB, threshold -24, ratio 4 ->
    // expected reduction ≈ 24*(1 - 1/4) = 18 dB at the peaks.
    EXPECT_LT(peakAbs(buf, 2, 12000), 0.5f);   // clearly compressed
    EXPECT_GT(peakAbs(buf, 2, 12000), 0.02f);  // not silenced
    EXPECT_LT(dyn.compReductionDb(), -6.0f);
}

TEST(Dynamics, CompressorMakeupRestoresLevel)
{
    Dynamics dyn;
    dyn.prepare(kSR, 2);
    Dynamics::CompSettings c;
    c.enabled = true; c.thresholdDb = -24.0f; c.ratio = 4.0f; c.kneeDb = 0.0f;
    c.makeupDb = 12.0f; c.attackMs = 5.0f; c.releaseMs = 100.0f;
    dyn.setCompressor(c);

    auto buf = sine(440.0f, 1.0f, 24000, 2);
    auto noMakeup = buf;

    Dynamics dyn2;
    dyn2.prepare(kSR, 2);
    c.makeupDb = 0.0f;
    dyn2.setCompressor(c);
    dyn2.process(noMakeup.data(), 24000);
    dyn.process(buf.data(), 24000);

    EXPECT_GT(peakAbs(buf, 2, 12000), peakAbs(noMakeup, 2, 12000));
}

// ── Dynamics: limiter ────────────────────────────────────────────────────────

TEST(Dynamics, LimiterCapsPeak)
{
    Dynamics dyn;
    dyn.prepare(kSR, 2);
    Dynamics::LimiterSettings l;
    l.enabled = true; l.ceilingDb = -0.3f; l.releaseMs = 50.0f;
    dyn.setLimiter(l);
    EXPECT_TRUE(dyn.isActive());

    auto buf = sine(440.0f, 1.0f, 16384, 2);  // 0 dB
    dyn.process(buf.data(), 16384);

    const float ceiling = dbToLinear(-0.3f);
    // After settling, no peak should exceed the ceiling (small tolerance).
    EXPECT_LE(peakAbs(buf, 2, 4096), ceiling * 1.05f);
}

TEST(Dynamics, InactiveWhenAllStagesOff)
{
    Dynamics dyn;
    dyn.prepare(kSR, 2);
    EXPECT_FALSE(dyn.isActive());

    auto buf = sine(440.0f, 0.5f, 1024, 2);
    auto orig = buf;
    dyn.process(buf.data(), 1024);
    for (size_t i = 0; i < buf.size(); ++i)
        EXPECT_FLOAT_EQ(buf[i], orig[i]);
}

// ── FxChain ──────────────────────────────────────────────────────────────────

TEST(FxChain, AddRemoveMove)
{
    FxChain chain;
    chain.prepare(kSR, 2);
    auto* eq  = chain.add(ProcessorKind::ParametricEQ);
    auto* dyn = chain.add(ProcessorKind::Dynamics);
    ASSERT_EQ(chain.size(), 2u);
    EXPECT_EQ(&chain.at(0), eq);
    EXPECT_EQ(&chain.at(1), dyn);

    chain.move(0, 1);
    EXPECT_EQ(&chain.at(0), dyn);
    EXPECT_EQ(&chain.at(1), eq);

    auto removed = chain.remove(0);
    EXPECT_EQ(removed.get(), dyn);
    EXPECT_EQ(chain.size(), 1u);
}

TEST(FxChain, InactiveChainIsTransparent)
{
    FxChain chain;
    chain.prepare(kSR, 2);
    chain.add(ProcessorKind::ParametricEQ);  // flat
    chain.add(ProcessorKind::Dynamics);      // all stages off
    EXPECT_FALSE(chain.isActive());

    auto buf = sine(440.0f, 0.5f, 2048, 2);
    auto orig = buf;
    chain.process(buf.data(), 2048);
    for (size_t i = 0; i < buf.size(); ++i)
        EXPECT_NEAR(buf[i], orig[i], 1e-6f);
}

TEST(FxChain, CloneProducesIndependentEquivalentChain)
{
    FxChain src;
    src.prepare(kSR, 2);
    auto* eq = static_cast<ParametricEQ*>(src.add(ProcessorKind::ParametricEQ));
    eq->setHighPass(true, 120.0f, 0.7f);
    eq->setOutputGainDb(-3.0f);
    auto* dyn = static_cast<Dynamics*>(src.add(ProcessorKind::Dynamics));
    dyn->loadVoicePreset();

    FxChain copy = src.clone();
    ASSERT_EQ(copy.size(), 2u);
    copy.prepare(kSR, 2);

    // Same input through both chains must yield identical output.
    auto a = sine(440.0f, 0.6f, 4096, 2);
    auto b = a;
    src.process(a.data(), 4096);
    copy.process(b.data(), 4096);
    for (size_t i = 0; i < a.size(); ++i)
        EXPECT_NEAR(a[i], b[i], 1e-5f);

    // Mutating the clone must not affect the original.
    static_cast<ParametricEQ&>(copy.at(0)).setOutputGainDb(-40.0f);
    EXPECT_FLOAT_EQ(static_cast<ParametricEQ&>(src.at(0)).outputGainDb(), -3.0f);
}

TEST(FxChain, AppliesProcessorsInOrder)
{
    FxChain chain;
    chain.prepare(kSR, 2);
    auto* eq = static_cast<ParametricEQ*>(chain.add(ProcessorKind::ParametricEQ));
    eq->setOutputGainDb(-6.0206f);  // halve
    EXPECT_TRUE(chain.isActive());

    auto buf = sine(1000.0f, 0.8f, 4096, 2);
    const float before = rms(buf, 2);
    chain.process(buf.data(), 4096);
    EXPECT_NEAR(rms(buf, 2), before * 0.5f, 0.02f);
}

// ── Reverb / Delay / Distortion / presets ───────────────────────────────────

std::vector<float> impulse(int frames, int channels)
{
    std::vector<float> v(static_cast<size_t>(frames) * channels, 0.0f);
    for (int c = 0; c < channels; ++c) v[static_cast<size_t>(c)] = 1.0f;
    return v;
}

bool allFinite(const std::vector<float>& v)
{
    for (float x : v) if (!std::isfinite(x)) return false;
    return true;
}

/// RMS of channel `ch` over frames [from, to).
float rmsRange(const std::vector<float>& buf, int channels, int from, int to, int ch = 0)
{
    double acc = 0.0;
    for (int f = from; f < to; ++f) {
        const float x = buf[static_cast<size_t>(f) * channels + ch];
        acc += static_cast<double>(x) * x;
    }
    return static_cast<float>(std::sqrt(acc / std::max(1, to - from)));
}

/// Processing in one call and in odd-sized blocks must match exactly
/// (export processes per block, playback per window).
void expectBlockInvariant(const AudioProcessor& proto)
{
    const int frames = 48000;
    auto in = sine(330.0f, 0.5f, frames, 2);
    for (int f = 0; f < frames; f += 997) in[static_cast<size_t>(f) * 2] += 0.4f;   // transients

    auto a = proto.clone();
    a->prepare(kSR, 2);
    auto whole = in;
    a->process(whole.data(), frames);

    auto b = proto.clone();
    b->prepare(kSR, 2);
    auto blocks = in;
    for (int off = 0; off < frames;) {
        const int n = std::min(513, frames - off);
        b->process(blocks.data() + static_cast<size_t>(off) * 2, n);
        off += n;
    }
    ASSERT_EQ(whole.size(), blocks.size());
    for (size_t i = 0; i < whole.size(); ++i) ASSERT_FLOAT_EQ(whole[i], blocks[i]) << "at " << i;
}

TEST(Reverb, DryOnlyIsInactive)
{
    Reverb r;
    r.setParam(Reverb::Wet, 0.0f);
    EXPECT_FALSE(r.isActive());
    r.setParam(Reverb::Wet, 10.0f);
    EXPECT_TRUE(r.isActive());
}

TEST(Reverb, ImpulseProducesDecayingTail)
{
    Reverb r;
    r.setParam(Reverb::Decay, 1.0f);
    r.setParam(Reverb::Dry, 0.0f);
    r.setParam(Reverb::Wet, 100.0f);
    r.prepare(kSR, 2);
    const int frames = static_cast<int>(kSR * 2.5);
    auto buf = impulse(frames, 2);
    r.process(buf.data(), frames);
    ASSERT_TRUE(allFinite(buf));

    const float early = rmsRange(buf, 2, 2400, 12000);            // 50–250 ms
    const float mid   = rmsRange(buf, 2, 48000, 52800);           // ~1.0 s
    const float late  = rmsRange(buf, 2, frames - 4800, frames);  // ~2.4 s
    EXPECT_GT(early, 1e-4f);
    EXPECT_LT(mid, early * 0.25f);   // RT60 = 1 s
    EXPECT_LT(late, mid);
    // Stereo: left and right tails are decorrelated, not identical.
    EXPECT_NE(buf[24000 * 2], buf[24000 * 2 + 1]);
}

TEST(Reverb, LongerDecayRingsLonger)
{
    auto tailAt = [](float decay) {
        Reverb r;
        r.setParam(Reverb::Decay, decay);
        r.setParam(Reverb::Dry, 0.0f);
        r.setParam(Reverb::Wet, 100.0f);
        r.prepare(kSR, 2);
        const int frames = 72000;
        auto buf = impulse(frames, 2);
        r.process(buf.data(), frames);
        return rmsRange(buf, 2, 48000, 72000);
    };
    EXPECT_GT(tailAt(3.0f), tailAt(0.5f) * 10.0f);
}

TEST(Reverb, BlockSizeDoesNotChangeOutput)
{
    Reverb r;
    r.setParam(Reverb::Wet, 40.0f);
    expectBlockInvariant(r);
}

TEST(Delay, EchoArrivesAtDelayTimeWithFeedbackGain)
{
    Delay d;
    d.setParam(Delay::Time, 100.0f);      // 4800 samples
    d.setParam(Delay::Feedback, 50.0f);
    d.setParam(Delay::Dry, 0.0f);
    d.setParam(Delay::Wet, 100.0f);
    d.setParam(Delay::LowCut, 20.0f);
    d.setParam(Delay::HighCut, 20000.0f);
    d.prepare(kSR, 1);
    const int frames = 20000;
    std::vector<float> buf(frames, 0.0f);
    buf[0] = 1.0f;
    d.process(buf.data(), frames);

    EXPECT_NEAR(buf[0], 0.0f, 1e-6f);
    EXPECT_NEAR(buf[4800], 1.0f, 1e-6f);   // first repeat: raw delayed input
    // Second repeat ≈ feedback × filtered first repeat.
    float second = 0.0f;
    for (int i = 9590; i < 9640; ++i) second = std::max(second, std::fabs(buf[static_cast<size_t>(i)]));
    EXPECT_GT(second, 0.3f);
    EXPECT_LT(second, 0.6f);
}

TEST(Delay, PingPongAlternatesSides)
{
    Delay d;
    d.setParam(Delay::Time, 50.0f);       // 2400 samples
    d.setParam(Delay::Feedback, 60.0f);
    d.setParam(Delay::PingPong, 1.0f);
    d.setParam(Delay::Dry, 0.0f);
    d.setParam(Delay::Wet, 100.0f);
    d.setParam(Delay::LowCut, 20.0f);       // open the feedback filters so the
    d.setParam(Delay::HighCut, 20000.0f);   // impulse peak isn't smeared
    d.prepare(kSR, 2);
    const int frames = 8000;
    auto buf = impulse(frames, 2);
    d.process(buf.data(), frames);
    auto peak = [&](int at, int ch) {
        float m = 0.0f;
        for (int f = at - 5; f < at + 60; ++f)
            m = std::max(m, std::fabs(buf[static_cast<size_t>(f) * 2 + ch]));
        return m;
    };
    EXPECT_GT(peak(2400, 0), 0.5f);   // first echo left
    EXPECT_LT(peak(2400, 1), 1e-4f);
    EXPECT_GT(peak(4800, 1), 0.2f);   // second echo right
}

TEST(Delay, BlockSizeDoesNotChangeOutput)
{
    Delay d;
    d.setParam(Delay::Saturation, 40.0f);
    d.setParam(Delay::PingPong, 1.0f);
    expectBlockInvariant(d);
}

TEST(Distortion, OutputStaysBoundedAndFinite)
{
    for (int mode = 0; mode < Distortion::ModeCount; ++mode) {
        Distortion x;
        x.setParam(Distortion::Type, static_cast<float>(mode));
        x.setParam(Distortion::Drive, 48.0f);
        x.setParam(Distortion::Output, 0.0f);
        x.prepare(kSR, 2);
        auto buf = sine(220.0f, 1.0f, 9600, 2);
        x.process(buf.data(), 9600);
        ASSERT_TRUE(allFinite(buf)) << mode;
        for (float v : buf) ASSERT_LT(std::fabs(v), 2.5f) << mode;
        EXPECT_GT(rms(buf, 2, 2400), 0.2f) << mode;
    }
}

TEST(Distortion, BlockSizeDoesNotChangeOutput)
{
    Distortion x;
    x.setParam(Distortion::Type, Distortion::BitCrush);
    x.setParam(Distortion::Downsample, 5.0f);
    expectBlockInvariant(x);
}

TEST(ParamProcessor, ClampsAndFillsMissingWithDefaults)
{
    Reverb r;
    r.setParam(Reverb::Decay, 99.0f);
    EXPECT_FLOAT_EQ(r.param(Reverb::Decay), 4.0f);   // capped (4 s window warm-up)
    r.setParams({80.0f});                             // older save: only first value
    EXPECT_FLOAT_EQ(r.param(Reverb::RoomSize), 80.0f);
    EXPECT_FLOAT_EQ(r.param(Reverb::Wet), 30.0f);
    Distortion x;
    x.setParam(Distortion::Type, 2.4f);
    EXPECT_FLOAT_EQ(x.param(Distortion::Type), 2.0f); // choices snap to integers
}

TEST(FxChain, EveryKindRoundTripsThroughKindOf)
{
    for (int k = 0; k < static_cast<int>(ProcessorKind::Count); ++k) {
        auto p = createProcessor(static_cast<ProcessorKind>(k));
        ASSERT_NE(p, nullptr);
        EXPECT_EQ(static_cast<int>(processorKindOf(*p)), k);
        EXPECT_EQ(static_cast<int>(processorKindOf(*p->clone())), k);
    }
}

TEST(AudioPresets, EveryPresetAddsActiveFiniteProcessing)
{
    for (int i = 0; i < static_cast<int>(AudioPreset::Count); ++i) {
        const char* name = audioPresetName(static_cast<AudioPreset>(i));
        FxChain chain;
        chain.prepare(kSR, 2);
        ASSERT_TRUE(applyAudioFxCode(chain, kPresetCodeBase + i)) << name;
        EXPECT_FALSE(chain.empty()) << name;
        EXPECT_TRUE(chain.isActive()) << name;
        auto buf = sine(440.0f, 0.5f, 24000, 2);
        chain.process(buf.data(), 24000);
        EXPECT_TRUE(allFinite(buf)) << name;
        for (float v : buf) ASSERT_LT(std::fabs(v), 4.0f) << name;
    }
    FxChain chain;
    EXPECT_FALSE(applyAudioFxCode(chain, kPresetCodeBase + static_cast<int>(AudioPreset::Count)));
    EXPECT_FALSE(applyAudioFxCode(chain, static_cast<int>(ProcessorKind::Count)));
    EXPECT_TRUE(chain.empty());
}

TEST(AudioPresets, DistantIsDarkerThanCloseUp)
{
    // A bright tone loses more level through "Make Distant" than "Close Up".
    auto levelAfter = [](AudioPreset p) {
        FxChain chain;
        chain.prepare(kSR, 2);
        applyAudioPreset(chain, p);
        auto buf = sine(9000.0f, 0.3f, 48000, 2);
        chain.process(buf.data(), 48000);
        return rms(buf, 2, 24000);
    };
    EXPECT_LT(levelAfter(AudioPreset::MakeDistant), levelAfter(AudioPreset::MakeCloseUp) * 0.7f);
}


// ── Loudness (BS.1770) ──────────────────────────────────────────────────────

TEST(Loudness, ReferenceToneReadsItsLevel)
{
    // A 1 kHz stereo sine at -20 dBFS peak per channel reads about -20 LUFS
    // (BS.1770 K-weighting is ~0 dB at 1 kHz; two channels add 3 dB, the
    // sine's RMS takes 3 dB off).
    auto buf = sine(1000.0f, 0.1f, 48000 * 5, 2);
    const auto l = integratedLoudness(buf.data(), 48000 * 5, 2, kSR);
    ASSERT_TRUE(l.has_value());
    EXPECT_NEAR(*l, -20.0, 0.3);

    // 6 dB quieter reads 6 LU lower.
    auto quiet = sine(1000.0f, 0.05f, 48000 * 5, 2);
    EXPECT_NEAR(*integratedLoudness(quiet.data(), 48000 * 5, 2, kSR), *l - 6.02, 0.1);
}

TEST(Loudness, SilenceIsGatedAndBlocksDoNotMatter)
{
    std::vector<float> silence(48000 * 2 * 2, 0.0f);
    EXPECT_FALSE(integratedLoudness(silence.data(), 48000 * 2, 2, kSR).has_value());

    // Tone + equal length of silence: the gate ignores the silence.
    auto tone = sine(1000.0f, 0.1f, 48000 * 3, 2);
    std::vector<float> mixed = tone;
    mixed.insert(mixed.end(), static_cast<size_t>(48000 * 3 * 2), 0.0f);
    const double a = *integratedLoudness(tone.data(), 48000 * 3, 2, kSR);
    const double b = *integratedLoudness(mixed.data(), 48000 * 6, 2, kSR);
    // Within 0.35 LU: the 400 ms blocks straddling the tone/silence edge
    // pass the gates with partial energy, as BS.1770 specifies.
    EXPECT_NEAR(a, b, 0.35);

    LoudnessMeter m(kSR, 2);
    for (int64_t off = 0; off < 48000 * 3;) {
        const int64_t n = std::min<int64_t>(777, 48000 * 3 - off);
        m.add(tone.data() + static_cast<size_t>(off) * 2, n);
        off += n;
    }
    EXPECT_DOUBLE_EQ(*m.integrated(), a);
}

// ── Repair processors ───────────────────────────────────────────────────────

TEST(RepairFx, DeEsserTurnsDownSibilanceOnly)
{
    auto level = [](float freq) {
        DeEsser d;
        d.setParam(DeEsser::Threshold, -40.0f);
        d.setParam(DeEsser::MaxReduction, 12.0f);
        d.prepare(kSR, 2);
        auto buf = sine(freq, 0.5f, 24000, 2);
        const float before = rms(buf, 2, 4800);
        d.process(buf.data(), 24000);
        return rms(buf, 2, 4800) / before;
    };
    EXPECT_LT(level(8000.0f), 0.5f);    // loud sibilance: down several dB
    EXPECT_GT(level(300.0f), 0.97f);    // voice body untouched
}

TEST(RepairFx, DeHumNotchesMainsHarmonics)
{
    auto level = [](float freq) {
        DeHum h;   // defaults: 60 Hz, 4 harmonics, 100 %
        h.prepare(kSR, 1);
        auto buf = sine(freq, 0.5f, 48000, 1);
        const float before = rms(buf, 1, 12000);
        h.process(buf.data(), 48000);
        return rms(buf, 1, 12000) / before;
    };
    EXPECT_LT(level(60.0f), 0.1f);
    EXPECT_LT(level(180.0f), 0.1f);
    EXPECT_GT(level(1000.0f), 0.98f);
}

TEST(RepairFx, StereoWidthZeroIsMono)
{
    StereoWidth w;
    w.setParam(StereoWidth::Width, 0.0f);
    EXPECT_TRUE(w.isActive());
    w.prepare(kSR, 2);
    std::vector<float> buf = {1.0f, 0.0f, 0.2f, -0.6f};
    w.process(buf.data(), 2);
    EXPECT_FLOAT_EQ(buf[0], buf[1]);
    EXPECT_FLOAT_EQ(buf[2], buf[3]);
    EXPECT_FLOAT_EQ(buf[0], 0.5f);
}

// ── Essential Sound ─────────────────────────────────────────────────────────

TEST(EssentialSound, CompilesOnlyWhatTheTypeAllows)
{
    EssentialSoundSettings s;
    EXPECT_TRUE(compileEssentialSound(s).empty());   // None

    s.type = SoundType::Dialogue;
    s.reduceRumble = s.deHum = s.deEss = s.dynamics = s.clarityEq = s.enhanceSpeech = true;
    s.reverb = true;
    const FxChain d = compileEssentialSound(s);
    std::vector<ProcessorKind> kinds;
    for (size_t i = 0; i < d.size(); ++i) kinds.push_back(processorKindOf(d.at(i)));
    const std::vector<ProcessorKind> expected = {
        ProcessorKind::ParametricEQ, ProcessorKind::DeHum, ProcessorKind::DeEsser,
        ProcessorKind::Dynamics, ProcessorKind::ParametricEQ, ProcessorKind::Reverb};
    EXPECT_EQ(kinds, expected);
    EXPECT_TRUE(d.isActive());

    // Music never gets repair / clarity / reverb (loudness + ducking only).
    s.type = SoundType::Music;
    EXPECT_TRUE(compileEssentialSound(s).empty());

    s = {};
    s.type = SoundType::Ambience;
    s.stereoWidth = true;
    const FxChain a = compileEssentialSound(s);
    ASSERT_EQ(a.size(), 1u);
    EXPECT_EQ(processorKindOf(a.at(0)), ProcessorKind::StereoWidth);
}

TEST(EssentialSound, EveryPresetProcessesFinite)
{
    for (int p = 0; p < static_cast<int>(ClarityEqPreset::Count); ++p) {
        for (int r = 0; r < static_cast<int>(SpaceReverbPreset::Count); ++r) {
            EssentialSoundSettings s;
            s.type = SoundType::Dialogue;
            s.clarityEq = true; s.eqPreset = p;
            s.reverb = true; s.reverbPreset = r; s.reverbAmount = 10.0f;
            FxChain c = compileEssentialSound(s);
            c.prepare(kSR, 2);
            auto buf = sine(440.0f, 0.5f, 9600, 2);
            c.process(buf.data(), 9600);
            for (float v : buf) ASSERT_TRUE(std::isfinite(v)) << p << "/" << r;
        }
    }
}

TEST(EssentialSound, DuckingRangesMergeAndPad)
{
    const auto r = computeDuckingRanges({{1000, 2000}, {100, 200}, {2300, 3000}}, 400, 50);
    ASSERT_EQ(r.size(), 2u);
    EXPECT_EQ(r[0], (std::pair<int64_t, int64_t>{50, 250}));
    EXPECT_EQ(r[1], (std::pair<int64_t, int64_t>{950, 3050}));   // 2050 and 2250 joined
}


// ── Pitch Shifter ───────────────────────────────────────────────────────────

namespace {

/// Frequency estimate from positive-going zero crossings on channel 0.
double zeroCrossingHz(const std::vector<float>& buf, int channels, int from, int to)
{
    int crossings = 0, firstX = -1, lastX = -1;
    for (int f = from + 1; f < to; ++f) {
        const float a = buf[static_cast<size_t>(f - 1) * channels];
        const float b = buf[static_cast<size_t>(f) * channels];
        if (a < 0.0f && b >= 0.0f) {
            if (firstX < 0) firstX = f;
            lastX = f;
            ++crossings;
        }
    }
    if (crossings < 2) return 0.0;
    return (crossings - 1) * kSR / static_cast<double>(lastX - firstX);
}

} // namespace

TEST(PitchShifter, ZeroIsInactiveWithNoLatency)
{
    PitchShifter p;
    p.prepare(kSR, 2);
    EXPECT_FALSE(p.isActive());
    EXPECT_EQ(p.latencyFrames(), 0);
    p.setParam(PitchShifter::Cents, 50.0f);
    EXPECT_TRUE(p.isActive());
    EXPECT_GT(p.latencyFrames(), 0);
}

TEST(PitchShifter, OctaveUpDoublesTheFrequency)
{
    PitchShifter p;
    p.setParam(PitchShifter::Semitones, 12.0f);
    p.prepare(kSR, 2);
    const int frames = 48000 * 2;
    auto buf = sine(220.0f, 0.5f, frames, 2);
    p.process(buf.data(), frames);
    const int from = p.latencyFrames() + 4800;
    EXPECT_NEAR(zeroCrossingHz(buf, 2, from, frames), 440.0, 6.0);
}

TEST(PitchShifter, OutputIsTheInputDelayedByItsLatency)
{
    // Short bursts every 0.25 s; a pitch shift keeps timing, so the burst
    // envelope must come out latencyFrames() later - at every pitch (the
    // SoundTouch offset varies with it; the latency is measured).
    for (float st : {-12.0f, -5.0f, 3.0f, 12.0f}) {
        SCOPED_TRACE(st);
        PitchShifter p;
        p.setParam(PitchShifter::Semitones, st);
        p.prepare(kSR, 1);
        const int frames = 48000 * 3;
        std::vector<float> in(frames, 0.0f);
        for (int start = 4800; start + 480 < frames; start += 12000)
            for (int i = 0; i < 480; ++i)
                in[static_cast<size_t>(start + i)] = 0.5f * std::sin(2.0f * kPi * 440.0f * i / 48000.0f);
        auto out = in;
        for (int off = 0; off < frames;) {   // odd block sizes, like a host
            const int n = std::min(1000, frames - off);
            p.process(out.data() + off, n);
            off += n;
        }
        const int L = p.latencyFrames();
        ASSERT_GT(L, 0);
        auto env = [](const std::vector<float>& v) {
            std::vector<float> e(v.size() / 16, 0.0f);
            for (size_t b = 0; b < e.size(); ++b) {
                double acc = 0;
                for (size_t i = 0; i < 16; ++i) acc += v[b * 16 + i] * v[b * 16 + i];
                e[b] = static_cast<float>(std::sqrt(acc / 16));
            }
            return e;
        };
        const auto ei = env(in), eo = env(out);
        int bestLag = 0;
        double best = -1;
        for (int lag = 0; lag < 800; ++lag) {
            double acc = 0;
            for (size_t b = 0; b + static_cast<size_t>(lag) < eo.size(); ++b)
                acc += ei[b] * eo[b + static_cast<size_t>(lag)];
            if (acc > best) { best = acc; bestLag = lag; }
        }
        // Within 1 ms of the declared latency.
        EXPECT_NEAR(bestLag * 16, L, 48) << "declared " << L << " measured " << bestLag * 16;
    }
}

TEST(PitchShifter, ChainReportsLatencyOnlyWhenActive)
{
    FxChain chain;
    chain.prepare(kSR, 2);
    auto* p = static_cast<ParamProcessor*>(chain.add(ProcessorKind::PitchShifter));
    EXPECT_EQ(chain.latencyFrames(), 0);
    p->setParam(PitchShifter::Semitones, -5.0f);
    EXPECT_GT(chain.latencyFrames(), 0);
    chain.at(0).setEnabled(false);
    EXPECT_EQ(chain.latencyFrames(), 0);
}

// ── Stress: no effect may ever output NaN / Inf ─────────────────────────────
// A non-finite sample in a track bus poisons the whole playback mix (the
// owner lost all sound mid-edit, 2026-11-22). Every processor and preset is
// run with random and extreme settings, harsh inputs, settings changed
// mid-stream and a long silent tail; every output sample must stay finite.

namespace stress {

enum class Input { Silence, LoudNoise, Dc, Impulses, HotSine, Count };

const char* inputName(Input in)
{
    switch (in) {
    case Input::Silence:   return "silence";
    case Input::LoudNoise: return "loud noise (+-4)";
    case Input::Dc:        return "DC 1.0";
    case Input::Impulses:  return "impulses";
    case Input::HotSine:   return "hot sine";
    default:               return "?";
    }
}

void fill(std::vector<float>& buf, Input in, int frames, int ch, int64_t start,
          std::mt19937& rng)
{
    std::uniform_real_distribution<float> n(-4.0f, 4.0f);
    for (int f = 0; f < frames; ++f)
        for (int c = 0; c < ch; ++c) {
            const int64_t t = start + f;
            float v = 0.0f;
            switch (in) {
            case Input::LoudNoise: v = n(rng); break;
            case Input::Dc:        v = 1.0f; break;
            case Input::Impulses:  v = (t % 4801 == 0) ? 1.0f : 0.0f; break;
            case Input::HotSine:   v = 2.0f * std::sin(2.0f * kPi * 97.0f * float(t) / 48000.0f); break;
            default: break;
            }
            buf[static_cast<size_t>(f) * ch + c] = v;
        }
}

void randomize(FxChain& chain, std::mt19937& rng, int mode)
{
    for (size_t i = 0; i < chain.size(); ++i) {
        auto* pp = dynamic_cast<ParamProcessor*>(&chain.at(i));
        if (!pp) continue;
        for (int k = 0; k < pp->paramCount(); ++k) {
            const ParamSpec& sp = pp->spec(k);
            float v = sp.def;
            if (mode == 1) v = sp.min;
            else if (mode == 2) v = sp.max;
            else if (mode >= 3) {
                std::uniform_real_distribution<float> d(sp.min, sp.max);
                const int r = static_cast<int>(rng() % 4);
                v = r == 0 ? sp.min : r == 1 ? sp.max : d(rng);
            }
            pp->setParam(k, v);
        }
    }
}

/// Runs `chain`; returns a failure description or empty when all finite.
std::string run(FxChain& chain, Input in, int ch, int block, std::mt19937& rng, int mode)
{
    chain.prepare(48000.0, ch);
    std::vector<float> buf(static_cast<size_t>(block) * ch);
    const int64_t signalFrames = 48000 * 2;          // 2 s of input
    const int64_t totalFrames  = signalFrames + 48000 * 6;    // + 6 s silent tail
    for (int64_t pos = 0; pos < totalFrames; pos += block) {
        if (pos < signalFrames) fill(buf, in, block, ch, pos, rng);
        else std::fill(buf.begin(), buf.end(), 0.0f);
        if (mode >= 3 && pos > 0 && pos % (48000 / 2) < block)   // change settings mid-stream
            randomize(chain, rng, mode);
        chain.process(buf.data(), block);
        for (size_t i = 0; i < buf.size(); ++i)
            if (!std::isfinite(buf[i]))
                return std::string("non-finite at frame ") + std::to_string(pos + int64_t(i) / ch)
                     + " input=" + inputName(in) + " ch=" + std::to_string(ch)
                     + " block=" + std::to_string(block) + " mode=" + std::to_string(mode);
    }
    return {};
}

} // namespace stress

TEST(AudioFxStress, EveryProcessorStaysFinite)
{
    std::mt19937 rng(1234);
    std::vector<std::string> failures;
    for (int k = 0; k < static_cast<int>(ProcessorKind::Count); ++k) {
        const auto kind = static_cast<ProcessorKind>(k);
        for (int mode = 0; mode < 6; ++mode)
            for (int in = 0; in < static_cast<int>(stress::Input::Count); ++in)
                for (int ch : {1, 2}) {
                    FxChain chain;
                    chain.add(kind);
                    stress::randomize(chain, rng, mode);
                    const int block = (mode % 2) ? 64 : 512;
                    const std::string f = stress::run(chain, static_cast<stress::Input>(in),
                                                      ch, block, rng, mode);
                    if (!f.empty()) {
                        failures.push_back(std::string(processorKindName(kind)) + ": " + f);
                        break;
                    }
                }
    }
    for (const auto& f : failures) ADD_FAILURE() << f;
}

TEST(AudioFxStress, EveryPresetStaysFinite)
{
    std::mt19937 rng(99);
    for (int p = 0; p < static_cast<int>(AudioPreset::Count); ++p)
        for (int in = 0; in < static_cast<int>(stress::Input::Count); ++in) {
            FxChain chain;
            applyAudioPreset(chain, static_cast<AudioPreset>(p));
            const std::string f = stress::run(chain, static_cast<stress::Input>(in), 2, 512, rng, 0);
            EXPECT_TRUE(f.empty()) << audioPresetName(static_cast<AudioPreset>(p)) << ": " << f;
        }
}

namespace {
/// Test processor: outputs NaN while `poison` is set; counts resets.
class NanProcessor final : public AudioProcessor
{
public:
    NanProcessor() : AudioProcessor("NanTest") {}
    bool poison{true};
    int  resets{0};
    void prepare(double sr, int ch) override { m_sampleRate = sr; m_channels = ch; }
    void reset() noexcept override { ++resets; poison = false; }
    void process(float* d, int frames) noexcept override
    {
        if (poison) d[frames / 2 * m_channels] = std::numeric_limits<float>::quiet_NaN();
    }
    bool isActive() const noexcept override { return true; }
    std::unique_ptr<AudioProcessor> clone() const override { return std::make_unique<NanProcessor>(); }
};
} // namespace

TEST(AudioFxStress, ChainResetsAProcessorThatOutputsNan)
{
    FxChain chain;
    auto* nan = static_cast<NanProcessor*>(chain.add(std::make_unique<NanProcessor>()));
    chain.prepare(48000.0, 2);
    nan->poison = true;   // prepare() does not reset this test processor's flag
    const uint64_t before = nonFiniteResetCount();

    auto block = sine(440.0f, 0.5f, 256, 2);
    chain.process(block.data(), 256);
    EXPECT_TRUE(rt::audiofx::allFinite(block.data(), block.size()));
    EXPECT_TRUE(std::all_of(block.begin(), block.end(), [](float v) { return v == 0.0f; }))
        << "the poisoned block is dropped";
    EXPECT_EQ(nan->resets, 1);
    EXPECT_EQ(nonFiniteResetCount(), before + 1);

    // Next block: the processor was reset, so audio passes again.
    auto next = sine(440.0f, 0.5f, 256, 2);
    const auto expected = next;
    chain.process(next.data(), 256);
    EXPECT_EQ(next, expected);
    EXPECT_EQ(nonFiniteResetCount(), before + 1);
}


} // namespace
} // namespace rt::audiofx
