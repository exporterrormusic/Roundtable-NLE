/*
 * ChromaKey.cpp — Ultra Key chroma keyer implementation.
 */

#include "effects/ChromaKey.h"

namespace rt {

ChromaKey::ChromaKey()
    : Effect(EffectType::ChromaKey)
{
    // Key color (RGB) — green screen default
    addParam("Key Color R",     0.0f,    0.0f, 1.0f);
    addParam("Key Color G",     1.0f,    0.0f, 1.0f);
    addParam("Key Color B",     0.0f,    0.0f, 1.0f);

    // Output / preset
    addParam("Output",          0.0f,    0.0f, 5.0f);   // 0=Composite, 1=Alpha Matte,
                                                         // 2=Color, 3=Original,
                                                         // 4=Removed Color, 5=Spill Map
    addParam("Setting",         0.0f,    0.0f, 4.0f);   // 4=ComfyUI Green

    // Matte Generation
    addParam("Transparency",    0.0f,    0.0f, 100.0f);
    addParam("Highlight",      10.0f,    0.0f, 100.0f);
    addParam("Shadow",         50.0f,    0.0f, 100.0f);
    addParam("Tolerance",      50.0f,    0.0f, 100.0f);
    addParam("Pedestal",       10.0f,    0.0f, 100.0f);

    // Matte Cleanup
    addParam("Choke",           0.0f,    0.0f, 100.0f);
    addParam("Soften",          0.0f,    0.0f, 100.0f);
    addParam("Contrast",        0.0f,    0.0f, 100.0f);
    addParam("Mid Point",      50.0f,    0.0f, 100.0f);

    // Spill Suppression
    addParam("Desaturate",     25.0f,    0.0f, 50.0f);
    addParam("Range",          50.0f,    0.0f, 100.0f);
    addParam("Spill",          50.0f,    0.0f, 100.0f);
    addParam("Luma",           50.0f,    0.0f, 100.0f);

    // Foreground Color Correction
    addParam("Saturation",    100.0f,    0.0f, 200.0f);
    addParam("Hue",             0.0f, -180.0f, 180.0f);
    addParam("Luminance",     100.0f,    0.0f, 200.0f);
}

std::unique_ptr<Effect> ChromaKey::clone() const
{
    auto copy = std::make_unique<ChromaKey>();
    copy->m_enabled = m_enabled;
    for (size_t i = 0; i < m_params.size(); ++i)
        copy->m_params[i].track = m_params[i].track;
    return copy;
}

void ChromaKey::applyPreset(int preset)
{
    // Preset values: Tolerance, Shadow, Highlight, Spill, Pedestal
    switch (preset) {
    case 0: // Default
        m_params[Transparency].track.setDefaultValue(0.0f);
        m_params[Highlight].track.setDefaultValue(10.0f);
        m_params[Shadow].track.setDefaultValue(50.0f);
        m_params[Tolerance].track.setDefaultValue(50.0f);
        m_params[Pedestal].track.setDefaultValue(10.0f);
        m_params[Spill].track.setDefaultValue(50.0f);
        break;
    case 1: // Relaxed
        m_params[Transparency].track.setDefaultValue(0.0f);
        m_params[Highlight].track.setDefaultValue(5.0f);
        m_params[Shadow].track.setDefaultValue(40.0f);
        m_params[Tolerance].track.setDefaultValue(30.0f);
        m_params[Pedestal].track.setDefaultValue(5.0f);
        m_params[Spill].track.setDefaultValue(30.0f);
        break;
    case 2: // Aggressive
        m_params[Transparency].track.setDefaultValue(0.0f);
        m_params[Highlight].track.setDefaultValue(20.0f);
        m_params[Shadow].track.setDefaultValue(70.0f);
        m_params[Tolerance].track.setDefaultValue(80.0f);
        m_params[Pedestal].track.setDefaultValue(20.0f);
        m_params[Spill].track.setDefaultValue(75.0f);
        break;
    case 4:
        applyFlatGreenPreset();
        break;
    default: // Custom — no changes
        break;
    }
}

void ChromaKey::applyFlatGreenPreset()
{
    m_params[Setting].track.setDefaultValue(4.0f); // ComfyUI Green
    m_params[KeyColorR].track.setDefaultValue(0.0f);
    m_params[KeyColorG].track.setDefaultValue(1.0f);
    m_params[KeyColorB].track.setDefaultValue(0.0f);
    m_params[Transparency].track.setDefaultValue(0.0f);
    m_params[Highlight].track.setDefaultValue(0.0f);
    m_params[Shadow].track.setDefaultValue(0.0f);
    m_params[Tolerance].track.setDefaultValue(50.0f);
    m_params[Pedestal].track.setDefaultValue(10.0f);
    // The current shader rounds 20 to a one-pixel erosion radius.
    m_params[Choke].track.setDefaultValue(20.0f);
    m_params[Soften].track.setDefaultValue(0.0f);
    m_params[Contrast].track.setDefaultValue(0.0f);
    m_params[MidPoint].track.setDefaultValue(50.0f);
    m_params[Spill].track.setDefaultValue(100.0f);
    m_params[SpillRange].track.setDefaultValue(5.0f);
    m_params[Desaturate].track.setDefaultValue(25.0f);
    m_params[Luma].track.setDefaultValue(0.0f); // don't turn removed green into a bright rim
}

} // namespace rt
