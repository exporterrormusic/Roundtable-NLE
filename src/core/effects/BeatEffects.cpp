/*
 * BeatEffects.cpp — beat-reactive effects: the pulse envelope and onsets.
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

void BeatReactEffect::setBeatTimes(std::vector<float> times)
{
    std::sort(times.begin(), times.end());
    m_beatTimes = std::move(times);
}

void BeatReactEffect::copyExtraStateFrom(const Effect& src)
{
    const auto& o = static_cast<const BeatReactEffect&>(src);
    m_beatTimes     = o.m_beatTimes;
    m_audioSourceId = o.m_audioSourceId;
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

} // namespace rt
