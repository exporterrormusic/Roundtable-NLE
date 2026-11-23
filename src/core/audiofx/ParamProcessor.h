/*
 * ParamProcessor — base for audio effects whose whole state is a flat list of
 * float parameters described by a static table (Reverb, Delay, Distortion…).
 *
 * The table gives each parameter its name, range, default, display step and
 * kind, so one generic path handles everything around the DSP:
 *   - save/load writes the float list (FxChain schema field "params");
 *   - Effect Controls builds its rows from the table;
 *   - clone() copies the list.
 *
 * Parameters are addressed by INDEX and the tables are APPEND ONLY: a project
 * saved by an older build simply has fewer values, and the missing ones take
 * their defaults (same rule as the video EffectRegistry).
 */

#pragma once

#include "audiofx/AudioProcessor.h"

#include <vector>

namespace rt::audiofx {

enum class ParamKind
{
    Number,   ///< scrubbable number
    Toggle,   ///< 0 = off, 1 = on
    Choice,   ///< integer index into `choices`
};

struct ParamSpec
{
    const char*        name;
    float              def;
    float              min;
    float              max;
    float              step{1.0f};
    int                decimals{0};
    const char*        suffix{""};
    ParamKind          kind{ParamKind::Number};
    const char* const* choices{nullptr};   ///< Choice only, `choiceCount` entries
    int                choiceCount{0};
};

class ParamProcessor : public AudioProcessor
{
public:
    [[nodiscard]] int              paramCount() const noexcept { return m_specCount; }
    [[nodiscard]] const ParamSpec& spec(int i) const noexcept { return m_specs[i]; }
    [[nodiscard]] float            param(int i) const noexcept { return m_params[static_cast<size_t>(i)]; }
    [[nodiscard]] const std::vector<float>& params() const noexcept { return m_params; }

    /// Clamp to the spec range and recompute derived coefficients.
    void setParam(int i, float v);

    /// Replace every value. Missing trailing values keep their defaults and
    /// extra values (from a newer build) are ignored.
    void setParams(const std::vector<float>& values);

    /// Reset every parameter to its table default.
    void resetToDefaults();

protected:
    ParamProcessor(const char* typeName, const ParamSpec* specs, int count);

    /// Recompute coefficients / delay lengths from m_params. Called after any
    /// parameter change and from prepare(). Control thread.
    virtual void onParamsChanged() = 0;

    /// Copy enabled flag + parameter values into a fresh instance (for clone()).
    template <class T>
    [[nodiscard]] std::unique_ptr<AudioProcessor> cloneAs() const
    {
        auto c = std::make_unique<T>();
        c->setParams(m_params);
        c->setEnabled(isEnabled());
        return c;
    }

    const ParamSpec*   m_specs;
    int                m_specCount;
    std::vector<float> m_params;
};

} // namespace rt::audiofx
