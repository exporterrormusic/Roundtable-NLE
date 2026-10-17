/*
 * ChromaKey.cpp — Ultra Key chroma keyer implementation.
 */

#include "effects/ChromaKey.h"

namespace rt {

ChromaKey::ChromaKey()
    : Effect(EffectType::ChromaKey)
{
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
