#include "audiofx/FxChain.h"

#include "audiofx/Dynamics.h"
#include "audiofx/ParametricEQ.h"

#include <algorithm>

namespace rt::audiofx {

const char* processorKindName(ProcessorKind k) noexcept
{
    switch (k) {
    case ProcessorKind::ParametricEQ: return "ParametricEQ";
    case ProcessorKind::Dynamics:     return "Dynamics";
    }
    return "Unknown";
}

std::unique_ptr<AudioProcessor> createProcessor(ProcessorKind k)
{
    switch (k) {
    case ProcessorKind::ParametricEQ: return std::make_unique<ParametricEQ>();
    case ProcessorKind::Dynamics:     return std::make_unique<Dynamics>();
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

void FxChain::process(float* data, int frames) noexcept
{
    for (auto& p : m_processors) {
        if (p->isEnabled() && p->isActive())
            p->process(data, frames);
    }
}

bool FxChain::isActive() const noexcept
{
    return std::any_of(m_processors.begin(), m_processors.end(),
                       [](const auto& p) { return p->isEnabled() && p->isActive(); });
}

} // namespace rt::audiofx
