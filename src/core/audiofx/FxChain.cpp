#include "audiofx/FxChain.h"

#include "audiofx/Delay.h"
#include "audiofx/Distortion.h"
#include "audiofx/Dynamics.h"
#include "audiofx/ParametricEQ.h"
#include "audiofx/HologramVoice.h"
#include "audiofx/PitchShifter.h"
#include "audiofx/RepairFx.h"
#include "audiofx/Reverb.h"

#include <algorithm>
#include <atomic>
#include <cstring>

namespace rt::audiofx {

const char* processorKindName(ProcessorKind k) noexcept
{
    switch (k) {
    case ProcessorKind::ParametricEQ: return "ParametricEQ";
    case ProcessorKind::Dynamics:     return "Dynamics";
    case ProcessorKind::Reverb:       return "Reverb";
    case ProcessorKind::Delay:        return "Delay";
    case ProcessorKind::Distortion:   return "Distortion";
    case ProcessorKind::DeEsser:      return "DeEsser";
    case ProcessorKind::DeHum:        return "DeHum";
    case ProcessorKind::StereoWidth:  return "StereoWidth";
    case ProcessorKind::PitchShifter: return "PitchShifter";
    case ProcessorKind::HologramVoice: return "HologramVoice";
    case ProcessorKind::Count:        break;
    }
    return "Unknown";
}

const char* processorDisplayName(ProcessorKind k) noexcept
{
    switch (k) {
    case ProcessorKind::ParametricEQ: return "Parametric EQ";
    case ProcessorKind::Dynamics:     return "Dynamics";
    case ProcessorKind::Reverb:       return "Studio Reverb";
    case ProcessorKind::Delay:        return "Analog Delay";
    case ProcessorKind::Distortion:   return "Distortion";
    case ProcessorKind::DeEsser:      return "DeEsser";
    case ProcessorKind::DeHum:        return "DeHum";
    case ProcessorKind::StereoWidth:  return "Stereo Width";
    case ProcessorKind::PitchShifter: return "Pitch Shifter";
    case ProcessorKind::HologramVoice: return "Hologram Voice";
    case ProcessorKind::Count:        break;
    }
    return "Unknown";
}

ProcessorKind processorKindOf(const AudioProcessor& p) noexcept
{
    const char* name = p.typeName();
    if (!name) return ProcessorKind::Count;
    for (int i = 0; i < static_cast<int>(ProcessorKind::Count); ++i) {
        const auto k = static_cast<ProcessorKind>(i);
        if (std::strcmp(name, processorKindName(k)) == 0) return k;
    }
    return ProcessorKind::Count;
}

std::unique_ptr<AudioProcessor> createProcessor(ProcessorKind k)
{
    switch (k) {
    case ProcessorKind::ParametricEQ: return std::make_unique<ParametricEQ>();
    case ProcessorKind::Dynamics:     return std::make_unique<Dynamics>();
    case ProcessorKind::Reverb:       return std::make_unique<Reverb>();
    case ProcessorKind::Delay:        return std::make_unique<Delay>();
    case ProcessorKind::Distortion:   return std::make_unique<Distortion>();
    case ProcessorKind::DeEsser:      return std::make_unique<DeEsser>();
    case ProcessorKind::DeHum:        return std::make_unique<DeHum>();
    case ProcessorKind::StereoWidth:  return std::make_unique<StereoWidth>();
    case ProcessorKind::PitchShifter: return std::make_unique<PitchShifter>();
    case ProcessorKind::HologramVoice: return std::make_unique<HologramVoice>();
    case ProcessorKind::Count:        break;
    }
    return nullptr;
}

AudioProcessor* FxChain::add(std::unique_ptr<AudioProcessor> p)
{
    if (!p) return nullptr;
    p->prepare(m_sampleRate, m_channels);
    AudioProcessor* raw = p.get();
    m_processors.push_back(std::move(p));
    return raw;
}

AudioProcessor* FxChain::add(ProcessorKind kind)
{
    return add(createProcessor(kind));
}

std::unique_ptr<AudioProcessor> FxChain::remove(size_t index)
{
    if (index >= m_processors.size()) return nullptr;
    auto p = std::move(m_processors[index]);
    m_processors.erase(m_processors.begin() + static_cast<ptrdiff_t>(index));
    return p;
}

void FxChain::move(size_t from, size_t to)
{
    const size_t n = m_processors.size();
    if (from >= n || to >= n || from == to) return;
    auto p = std::move(m_processors[from]);
    m_processors.erase(m_processors.begin() + static_cast<ptrdiff_t>(from));
    m_processors.insert(m_processors.begin() + static_cast<ptrdiff_t>(to), std::move(p));
}

FxChain FxChain::clone() const
{
    FxChain copy;
    copy.m_sampleRate = m_sampleRate;
    copy.m_channels   = m_channels;
    copy.m_processors.reserve(m_processors.size());
    for (const auto& p : m_processors)
        copy.m_processors.push_back(p->clone());
    return copy;
}

void FxChain::prepare(double sampleRate, int channels)
{
    m_sampleRate = sampleRate > 0.0 ? sampleRate : 48000.0;
    m_channels   = std::max(1, channels);
    for (auto& p : m_processors)
        p->prepare(m_sampleRate, m_channels);
}

void FxChain::reset() noexcept
{
    for (auto& p : m_processors)
        p->reset();
}

bool allFinite(const float* data, size_t count) noexcept
{
    // Branch-free accumulate: x - x is 0 for finite x and NaN for NaN / Inf.
    float acc = 0.0f;
    for (size_t i = 0; i < count; ++i) acc += data[i] - data[i];
    return acc == 0.0f;
}

namespace {
std::atomic<uint64_t> g_nonFiniteResets{0};
std::atomic<int>      g_lastNonFiniteKind{static_cast<int>(ProcessorKind::Count)};
} // namespace

uint64_t nonFiniteResetCount() noexcept
{
    return g_nonFiniteResets.load(std::memory_order_relaxed);
}

ProcessorKind lastNonFiniteProcessor() noexcept
{
    return static_cast<ProcessorKind>(g_lastNonFiniteKind.load(std::memory_order_relaxed));
}

void FxChain::process(float* data, int frames) noexcept
{
    if (frames <= 0) return;
    const size_t count = static_cast<size_t>(frames) * static_cast<size_t>(m_channels);
    for (auto& p : m_processors) {
        if (!p->isEnabled() || !p->isActive()) continue;
        p->process(data, frames);
        if (!allFinite(data, count)) {
            // Clear the processor's state so the bad value cannot keep
            // circulating, and drop this one block rather than pass it on.
            p->reset();
            std::fill_n(data, count, 0.0f);
            g_lastNonFiniteKind.store(static_cast<int>(processorKindOf(*p)),
                                      std::memory_order_relaxed);
            g_nonFiniteResets.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

bool FxChain::isActive() const noexcept
{
    return std::any_of(m_processors.begin(), m_processors.end(),
                       [](const auto& p) { return p->isEnabled() && p->isActive(); });
}

int FxChain::latencyFrames() const noexcept
{
    int total = 0;
    for (const auto& p : m_processors)
        if (p->isEnabled() && p->isActive()) total += p->latencyFrames();
    return total;
}

} // namespace rt::audiofx
