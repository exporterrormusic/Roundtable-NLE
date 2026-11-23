#include "effects/Echo.h"

#include "Constants.h"
#include "effects/Effect.h"
#include "effects/EffectStack.h"

#include <algorithm>
#include <cmath>

namespace rt {

namespace {

// Compositor BlendMode values (gpu/Compositor.h); core can't include gpu.
constexpr int32_t kNormal  = 0;
constexpr int32_t kScreen  = 2;
constexpr int32_t kAdd     = 3;
constexpr int32_t kDarken  = 11;
constexpr int32_t kLighten = 12;

// Echo parameter indices (EffectRegistry entry, append only).
enum : size_t { EchoTime = 0, NumberOfEchoes, StartingIntensity, Decay, Operator };

} // namespace

std::vector<EchoCopy> planEcho(const EffectStack& stack, int64_t localTick,
                               int64_t clipDuration, int32_t clipBlendMode)
{
    const Effect* echo = nullptr;
    for (size_t i = 0; i < stack.effectCount(); ++i) {
        const Effect& e = stack.effect(i);
        if (e.isEnabled() && e.effectType() == EffectType::Echo) { echo = &e; break; }
    }
    if (!echo || echo->paramCount() <= Operator) return {};

    const double echoSeconds = echo->evalParam(EchoTime, localTick);
    const int count = std::clamp(
        static_cast<int>(std::lround(echo->evalParam(NumberOfEchoes, localTick))), 0, kMaxEchoes);
    const float start = std::clamp(echo->evalParam(StartingIntensity, localTick), 0.0f, 1.0f);
    const float decay = std::clamp(echo->evalParam(Decay, localTick), 0.0f, 1.0f);
    const auto op = static_cast<EchoOperator>(std::clamp(
        static_cast<int>(std::lround(echo->evalParam(Operator, localTick))), 0,
        static_cast<int>(EchoOperator::Count) - 1));
    const int64_t stepTicks = static_cast<int64_t>(std::llround(echoSeconds * kTicksPerSecond));

    // Copies in k order (current first), skipping invisible / out-of-clip ones.
    std::vector<EchoCopy> copies;
    float intensity = start;
    for (int k = 0; k <= count; ++k, intensity *= decay) {
        const int64_t offset = stepTicks * k;
        const int64_t t = localTick + offset;
        if (k > 0 && (stepTicks == 0 || t < 0 || t >= clipDuration)) continue;
        if (intensity <= 0.0f && k > 0) continue;
        copies.push_back({k, offset, intensity, kNormal});
    }
    if (copies.empty()) return copies;

    // Bottom-to-top order. "In Front" draws the echoes over the current
    // frame; every other operator builds up from the oldest echo, with the
    // current frame on top.
    if (op != EchoOperator::CompositeInFront)
        std::reverse(copies.begin(), copies.end());

    int32_t stackBlend = kNormal;
    switch (op) {
    case EchoOperator::Add:     stackBlend = kAdd;     break;
    case EchoOperator::Maximum: stackBlend = kLighten; break;
    case EchoOperator::Minimum: stackBlend = kDarken;  break;
    case EchoOperator::Screen:  stackBlend = kScreen;  break;
    default:                    stackBlend = kNormal;  break;
    }
    for (size_t i = 0; i < copies.size(); ++i) {
        EchoCopy& c = copies[i];
        if (i == 0) {
            c.blendMode = clipBlendMode;   // bottom copy lands like the clip would
        } else {
            c.blendMode = stackBlend;
        }
        // Blend: equal-weight running average (layer i covers 1/(i+1)).
        if (op == EchoOperator::Blend)
            c.opacity *= 1.0f / static_cast<float>(i + 1);
    }
    return copies;
}

} // namespace rt
