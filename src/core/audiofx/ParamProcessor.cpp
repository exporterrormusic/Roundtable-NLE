#include "audiofx/ParamProcessor.h"

#include <algorithm>

namespace rt::audiofx {

ParamProcessor::ParamProcessor(const char* typeName, const ParamSpec* specs, int count)
    : AudioProcessor(typeName), m_specs(specs), m_specCount(count)
{
    m_params.resize(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) m_params[static_cast<size_t>(i)] = specs[i].def;
}

void ParamProcessor::setParam(int i, float v)
{
    if (i < 0 || i >= m_specCount) return;
    const ParamSpec& s = m_specs[i];
    if (!(v == v)) v = s.def;   // NaN guard
    v = std::clamp(v, s.min, s.max);
    if (s.kind != ParamKind::Number) v = static_cast<float>(static_cast<int>(v + 0.5f));
    m_params[static_cast<size_t>(i)] = v;
    onParamsChanged();
}

void ParamProcessor::setParams(const std::vector<float>& values)
{
    for (int i = 0; i < m_specCount; ++i) {
        const ParamSpec& s = m_specs[i];
        float v = i < static_cast<int>(values.size()) ? values[static_cast<size_t>(i)] : s.def;
        if (!(v == v)) v = s.def;
        v = std::clamp(v, s.min, s.max);
        if (s.kind != ParamKind::Number) v = static_cast<float>(static_cast<int>(v + 0.5f));
        m_params[static_cast<size_t>(i)] = v;
    }
    onParamsChanged();
}

void ParamProcessor::resetToDefaults()
{
    for (int i = 0; i < m_specCount; ++i) m_params[static_cast<size_t>(i)] = m_specs[i].def;
    onParamsChanged();
}

} // namespace rt::audiofx
