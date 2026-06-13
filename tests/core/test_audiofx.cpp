/*
 * test_audiofx.cpp — unit tests for the src/core/audiofx DSP module.
 *
 * Verifies the building blocks (Biquad), the ParametricEQ, the unified
 * Dynamics processor (gate / compressor / limiter), and the FxChain
 * container. Signals are synthetic so the tests are deterministic and
 * headless-safe (core group).
 */

#include <gtest/gtest.h>

#include <cmath>
#include <vector>

#include "audiofx/AudioProcessor.h"
#include "audiofx/Biquad.h"
#include "audiofx/Dynamics.h"
#include "audiofx/FxChain.h"
#include "audiofx/ParametricEQ.h"

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

} // namespace
} // namespace rt::audiofx
