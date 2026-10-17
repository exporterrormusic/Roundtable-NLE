/*
 * EffectStack.cpp — implements EffectStack + Effect base class.
 *
 * Step 22: Effects System
 */

#include "effects/EffectStack.h"
#include "effects/Effect.h"
#include "effects/EffectRegistry.h"
#include "effects/BeatEffects.h"
#include "effects/ChromaKey.h"
#include "effects/ColorGrading.h"
#include "effects/LUT.h"
#include "effects/Ots.h"

#include "Constants.h"

#include <algorithm>
#include <cmath>

namespace rt {

// =============================================================================
//  Effect base — static ID counter + implementation
// =============================================================================

uint64_t Effect::s_nextId = 1;

Effect::Effect(EffectType type)
    : m_type(type)
    , m_id(s_nextId++)
{
    const auto& def = effectDef(type);
    m_params.reserve(def.params.size());
    for (const auto& p : def.params) {
        EffectParam param;
        param.name   = p.name;
        param.track  = KeyframeTrack<float>(p.def);
        param.minVal = p.min;
        param.maxVal = p.max;
        m_params.push_back(std::move(param));
    }
}

std::unique_ptr<Effect> Effect::clone() const
{
    auto copy = createEffect(m_type);
    if (!copy)
        copy = std::make_unique<Effect>(m_type);
    copy->m_enabled = m_enabled;
    const size_t n = std::min(m_params.size(), copy->m_params.size());
    for (size_t i = 0; i < n; ++i)
        copy->m_params[i].track = m_params[i].track;
    copy->copyExtraStateFrom(*this);
    return copy;
}

Effect::~Effect() = default;

EffectParam* Effect::findParam(const std::string& n)
{
    for (auto& p : m_params)
        if (p.name == n) return &p;
    return nullptr;
}

const EffectParam* Effect::findParam(const std::string& n) const
{
    for (auto& p : m_params)
        if (p.name == n) return &p;
    return nullptr;
}

std::vector<float> Effect::evalAllParams(int64_t time) const
{
    std::vector<float> vals;
    vals.reserve(m_params.size());
    for (auto& p : m_params)
        vals.push_back(p.track.evaluate(time));
    return vals;
}

// ---- Factory ----------------------------------------------------------------

std::unique_ptr<Effect> createEffect(EffectType type)
{
    // Only effects with state beyond their parameters have a class of their
    // own; every other effect is a plain Effect built from its table entry.
    switch (type) {
    case EffectType::ChromaKey:    return std::make_unique<ChromaKey>();
    case EffectType::LUT:          return std::make_unique<LUT>();
    case EffectType::ColorGrading:
    case EffectType::LumetriColor: return std::make_unique<ColorGrading>();  // legacy loads as Color Grading
    case EffectType::OtsLeft:
    case EffectType::OtsRight:
    case EffectType::OtsIntro:     return std::make_unique<Ots>(type);
    default:                       break;
    }
    if (static_cast<size_t>(type) >= static_cast<size_t>(EffectType::Count))
        return nullptr;   // not an effect type
    if (isBeatReactEffect(type))
        return std::make_unique<BeatReactEffect>(type);
    return std::make_unique<Effect>(type);
}

// =============================================================================
//  EffectStack
// =============================================================================

EffectStack::EffectStack() = default;
EffectStack::~EffectStack() = default;

void EffectStack::addEffect(std::unique_ptr<Effect> effect)
{
    m_effects.push_back(std::move(effect));
}

void EffectStack::insertEffect(size_t index, std::unique_ptr<Effect> effect)
{
    if (index >= m_effects.size())
        m_effects.push_back(std::move(effect));
    else
        m_effects.insert(m_effects.begin() + static_cast<ptrdiff_t>(index), std::move(effect));
}

std::unique_ptr<Effect> EffectStack::removeEffect(size_t index)
{
    if (index >= m_effects.size()) return nullptr;
    auto ptr = std::move(m_effects[index]);
    m_effects.erase(m_effects.begin() + static_cast<ptrdiff_t>(index));
    return ptr;
}

void EffectStack::moveEffect(size_t fromIndex, size_t toIndex)
{
    if (fromIndex >= m_effects.size() || toIndex >= m_effects.size()) return;
    if (fromIndex == toIndex) return;

    auto effect = std::move(m_effects[fromIndex]);
    m_effects.erase(m_effects.begin() + static_cast<ptrdiff_t>(fromIndex));
    m_effects.insert(
        m_effects.begin() + static_cast<ptrdiff_t>(std::min(toIndex, m_effects.size())),
        std::move(effect));
}

const Effect* EffectStack::effectById(uint64_t id) const
{
    for (auto& e : m_effects)
        if (e->id() == id) return e.get();
    return nullptr;
}

Effect* EffectStack::effectById(uint64_t id)
{
    for (auto& e : m_effects)
        if (e->id() == id) return e.get();
    return nullptr;
}

size_t EffectStack::indexOf(uint64_t effectId) const
{
    for (size_t i = 0; i < m_effects.size(); ++i)
        if (m_effects[i]->id() == effectId) return i;
    return m_effects.size();
}

bool EffectStack::hasActiveEffects() const
{
    for (auto& e : m_effects)
        if (e->isEnabled()) return true;
    return false;
}

std::vector<EffectStack::EffectSnapshot> EffectStack::evaluate(int64_t time) const
{
    std::vector<EffectSnapshot> result;
    for (auto& e : m_effects) {
        if (!e->isEnabled()) continue;
        if (e->effectType() == EffectType::ColorGrading) {
            auto* grading = static_cast<const ColorGrading*>(e.get());
            const auto masks = e->maskCount() > 0
                ? evaluateMaskStates(e->masks(), time)
                : std::vector<MaskRenderState>{};

            auto appendPass = [&](EffectType type, std::vector<float> params) {
                result.push_back({type, std::move(params), e->id(), masks});
            };

            // The unified color effect is evaluated as a small sequence of
            // GPU passes.  This keeps every control functional while staying
            // inside Vulkan's guaranteed 128-byte push-constant limit.
            appendPass(EffectType::ColorGrading, grading->evalGpuParams(time));
            if (grading->curvesEnabled() && !grading->curvesIdentity())
                appendPass(EffectType::ColorGrading, grading->evalCurveGpuParams());

            const auto& hsl = grading->hslParams();
            if (grading->hslEnabled()
                && (std::abs(hsl.hueShift) > 0.001f
                    || std::abs(hsl.satAdjust) > 0.001f
                    || std::abs(hsl.lumAdjust) > 0.001f)) {
                appendPass(EffectType::ColorGrading, grading->evalHslGpuParams());
            }

            // Sharpen needs neighbouring pixels, so it uses the existing
            // unsharp-mask shader as a final Creative-section pass.
            if (grading->creativeEnabled()) {
                const float sharpen = grading->param(ColorGrading::Sharpen).track.evaluate(time);
                if (sharpen > 0.001f)
                    appendPass(EffectType::Sharpen,
                               {sharpen / 20.0f, 1.5f, 0.015f});
            }
            continue;

        } else if (isProceduralTimeEffect(e->effectType())) {
            // Glitch family: forward a continuous clock so the shader can
            // animate itself. `time` here is already clip-local (the caller
            // passes tick - clip->timelineIn()), so this auto-fits clip length.
            auto params = e->evalAllParams(time);
            if (params.size() <= kGlitchTimeSlot)
                params.resize(kGlitchTimeSlot + 1, 0.0f);
            params[kGlitchTimeSlot] = static_cast<float>(ticksToSeconds(time));
            result.push_back({e->effectType(), std::move(params)});

        } else if (isBeatReactEffect(e->effectType())) {
            // Beat family: the CPU computes the 0–1 pulse envelope (from the
            // tempo grid or detected onsets) and forwards it + the clock so the
            // shader just scales its reaction by pulse * Amount.
            auto* be = static_cast<const BeatReactEffect*>(e.get());
            auto params = e->evalAllParams(time);
            if (params.size() <= kGlitchTimeSlot)
                params.resize(kGlitchTimeSlot + 1, 0.0f);
            params[kBeatPulseSlot]  = be->computePulse(time);
            params[kGlitchTimeSlot] = static_cast<float>(ticksToSeconds(time));
            result.push_back({e->effectType(), std::move(params)});

        } else {
            result.push_back({e->effectType(), e->evalAllParams(time)});
        }

        // Attach identity + evaluated effect masks (Premiere: masks limit
        // where this effect applies). Snapshot here so the render thread
        // never reads the live OpacityMask objects.
        result.back().effectId = e->id();
        if (e->maskCount() > 0)
            result.back().masks = evaluateMaskStates(e->masks(), time);
        if (e->effectType() == EffectType::LUT)
            result.back().lut = static_cast<const LUT*>(e.get())->cube();
    }
    return result;
}

std::unique_ptr<EffectStack> EffectStack::clone() const
{
    auto copy = std::make_unique<EffectStack>();
    for (auto& e : m_effects)
        copy->addEffect(e->cloneWithMasks());
    return copy;
}

EffectStack EffectStack::cloneKeepingIds() const
{
    EffectStack copy;
    copy.m_effects.reserve(m_effects.size());
    for (const auto& e : m_effects)
        copy.m_effects.push_back(e->cloneKeepingId());
    return copy;
}

void EffectStack::assignStateFrom(const EffectStack& src)
{
    std::vector<std::unique_ptr<Effect>> next;
    next.reserve(src.m_effects.size());
    for (const auto& want : src.m_effects) {
        auto it = std::find_if(m_effects.begin(), m_effects.end(),
            [&](const std::unique_ptr<Effect>& have) {
                return have && have->id() == want->id()
                    && have->effectType() == want->effectType();
            });
        if (it != m_effects.end()) {
            (*it)->assignStateFrom(*want);
            next.push_back(std::move(*it));
        } else {
            next.push_back(want->cloneKeepingId());
        }
    }
    m_effects = std::move(next);
}

} // namespace rt
