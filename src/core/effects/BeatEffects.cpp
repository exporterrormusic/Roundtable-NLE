/*
 * BeatEffects.cpp — beat-reactive base (pulse envelope) + the reactions.
 */

#include "effects/BeatEffects.h"

#include "Constants.h"

#include <algorithm>
#include <cmath>

namespace rt {

// ── BeatReactEffect base ─────────────────────────────────────────────────────

BeatReactEffect::BeatReactEffect(EffectType type)
    : Effect(type)
{
}

void BeatReactEffect::addBeatParams()
{
    // Indices BPM..Mode — order must match BeatParam.
    addParam("BPM",      120.0f, 1.0f,   300.0f);
    addParam("Offset",   0.0f,  -2.0f,   2.0f);
    addParam("Rate",     1.0f,   0.25f,  4.0f);
    addParam("Attack",   0.01f,  0.0f,   0.5f);
    addParam("Decay",    0.25f,  0.01f,  2.0f);
    addParam("Mode",     0.0f,   0.0f,   1.0f);  // 0 = manual BPM, 1 = detected
}

void BeatReactEffect::setBeatTimes(std::vector<float> times)
{
    std::sort(times.begin(), times.end());
    m_beatTimes = std::move(times);
}

void BeatReactEffect::copyBeatStateTo(BeatReactEffect& dst) const
{
    dst.m_enabled = m_enabled;
    for (size_t i = 0; i < m_params.size(); ++i)
        dst.m_params[i].track = m_params[i].track;
    dst.m_beatTimes     = m_beatTimes;
    dst.m_audioSourceId = m_audioSourceId;
}

float BeatReactEffect::computePulse(int64_t clipLocalTick) const
{
    const float t      = static_cast<float>(ticksToSeconds(clipLocalTick));
    const float attack = std::max(m_params[Attack].track.evaluate(clipLocalTick), 0.0001f);
    const float decay  = std::max(m_params[Decay].track.evaluate(clipLocalTick), 0.0001f);
    const float mode   = m_params[Mode].track.evaluate(clipLocalTick);

    // Time since the most recent beat.
    float dt;
    if (mode >= 0.5f && !m_beatTimes.empty()) {
        // Auto: last detected onset at or before t.
        auto it = std::upper_bound(m_beatTimes.begin(), m_beatTimes.end(), t);
        if (it == m_beatTimes.begin())
            return 0.0f;                       // before the first onset
        dt = t - *(it - 1);
    } else {
        const float bpm    = std::max(m_params[BPM].track.evaluate(clipLocalTick), 1.0f);
        const float rate   = std::max(m_params[Rate].track.evaluate(clipLocalTick), 0.01f);
        const float offset = m_params[Offset].track.evaluate(clipLocalTick);
        const float period = (60.0f / bpm) / rate;
        float phase = t - offset;
        phase -= std::floor(phase / period) * period;   // positive modulo
        dt = phase;
    }

    if (dt < 0.0f) return 0.0f;
    if (dt < attack) return dt / attack;               // linear rise
    return std::clamp(std::exp(-(dt - attack) / decay), 0.0f, 1.0f);  // eased fall
}

// ── Reactions ────────────────────────────────────────────────────────────────

#define RT_BEAT_CLONE(Klass)                          \
    std::unique_ptr<Effect> Klass::clone() const      \
    {                                                 \
        auto copy = std::make_unique<Klass>();        \
        copyBeatStateTo(*copy);                       \
        return copy;                                  \
    }

BeatZoom::BeatZoom() : BeatReactEffect(EffectType::BeatZoom)
{
    addParam("Amount", 0.06f, 0.0f, 0.5f);   // fractional scale-up at full pulse
    addBeatParams();
}
RT_BEAT_CLONE(BeatZoom)

BeatFlash::BeatFlash() : BeatReactEffect(EffectType::BeatFlash)
{
    addParam("Amount", 0.6f, 0.0f, 2.0f);    // brightness gain at full pulse
    addBeatParams();
}
RT_BEAT_CLONE(BeatFlash)

BeatShake::BeatShake() : BeatReactEffect(EffectType::BeatShake)
{
    addParam("Amount", 20.0f, 0.0f, 150.0f); // max offset in px at full pulse
    addBeatParams();
}
RT_BEAT_CLONE(BeatShake)

BeatChroma::BeatChroma() : BeatReactEffect(EffectType::BeatChroma)
{
    addParam("Amount", 12.0f, 0.0f, 60.0f);  // channel split in px at full pulse
    addBeatParams();
}
RT_BEAT_CLONE(BeatChroma)

BeatDrop::BeatDrop() : BeatReactEffect(EffectType::BeatDrop)
{
    addParam("Amount", 1.0f, 0.0f, 2.0f);    // unitless punch (drives all three)
    addBeatParams();
}
RT_BEAT_CLONE(BeatDrop)

#undef RT_BEAT_CLONE

} // namespace rt
