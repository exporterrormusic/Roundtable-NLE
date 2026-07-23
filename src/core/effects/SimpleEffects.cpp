/*
 * SimpleEffects.cpp — Minimal effect implementations consolidated here.
 *
 * Effects with only a constructor (addParam calls) and clone() are grouped
 * into this single translation unit to reduce build overhead.
 */

#include "effects/Blur.h"
#include "effects/Glow.h"
#include "effects/Sharpen.h"
#include "effects/Vignette.h"
#include "effects/Letterbox.h"
#include "effects/FillChannel.h"
#include "effects/Tint.h"

namespace rt {

// ── Blur ──────────────────────────────────────────────────────────────────

Blur::Blur()
    : Effect(EffectType::Blur)
{
    addParam("Radius", 15.0f, 0.0f, 100.0f);
}

std::unique_ptr<Effect> Blur::clone() const
{
    auto copy = std::make_unique<Blur>();
    copy->m_enabled = m_enabled;
    for (size_t i = 0; i < m_params.size(); ++i)
        copy->m_params[i].track = m_params[i].track;
    return copy;
}

// ── Glow ──────────────────────────────────────────────────────────────────

Glow::Glow()
    : Effect(EffectType::Glow)
{
    addParam("Threshold", 0.8f, 0.0f, 1.0f);
    addParam("Intensity", 1.0f, 0.0f, 5.0f);
    addParam("Radius",    10.0f, 1.0f, 50.0f);
}

std::unique_ptr<Effect> Glow::clone() const
{
    auto copy = std::make_unique<Glow>();
    copy->m_enabled = m_enabled;
    for (size_t i = 0; i < m_params.size(); ++i)
        copy->m_params[i].track = m_params[i].track;
    return copy;
}

// ── Sharpen ───────────────────────────────────────────────────────────────

Sharpen::Sharpen()
    : Effect(EffectType::Sharpen)
{
    addParam("Amount",    1.0f, 0.0f, 5.0f);
    addParam("Radius",    1.0f, 0.1f, 10.0f);
    addParam("Threshold", 0.0f, 0.0f, 1.0f);
}

std::unique_ptr<Effect> Sharpen::clone() const
{
    auto copy = std::make_unique<Sharpen>();
    copy->m_enabled = m_enabled;
    for (size_t i = 0; i < m_params.size(); ++i)
        copy->m_params[i].track = m_params[i].track;
    return copy;
}

// ── Vignette ──────────────────────────────────────────────────────────────

Vignette::Vignette()
    : Effect(EffectType::Vignette)
{
    addParam("Intensity",  1.0f, 0.0f, 2.0f);
    addParam("Roundness",  0.5f, 0.0f, 1.0f);
    addParam("Softness",   0.5f, 0.01f, 1.0f);
    addParam("Center X",   0.0f, -1.0f, 1.0f);
    addParam("Center Y",   0.0f, -1.0f, 1.0f);
}

std::unique_ptr<Effect> Vignette::clone() const
{
    auto copy = std::make_unique<Vignette>();
    copy->m_enabled = m_enabled;
    for (size_t i = 0; i < m_params.size(); ++i)
        copy->m_params[i].track = m_params[i].track;
    return copy;
}

// ── Letterbox ─────────────────────────────────────────────────────────────

Letterbox::Letterbox()
    : Effect(EffectType::Letterbox)
{
    addParam("Aspect Width", 2.39f, 1.0f, 4.0f);
    addParam("Aspect Height", 1.0f,  1.0f, 4.0f);
    addParam("Bar Opacity",  1.0f,  0.0f, 1.0f);
    addParam("Bar R",        0.0f,  0.0f, 1.0f);
    addParam("Bar G",        0.0f,  0.0f, 1.0f);
    addParam("Bar B",        0.0f,  0.0f, 1.0f);
    addParam("Feather",      0.0f,  0.0f, 0.1f);
}

std::unique_ptr<Effect> Letterbox::clone() const
{
    auto copy = std::make_unique<Letterbox>();
    copy->m_enabled = m_enabled;
    for (size_t i = 0; i < m_params.size(); ++i)
        copy->m_params[i].track = m_params[i].track;
    return copy;
}

// ── FillLeftWithRight ─────────────────────────────────────────────────────

// Tint (Premiere-style luminance remap)

Tint::Tint()
    : Effect(EffectType::Tint)
{
    addParam("Map Black To R", 0.0f, 0.0f, 1.0f);
    addParam("Map Black To G", 0.0f, 0.0f, 1.0f);
    addParam("Map Black To B", 0.0f, 0.0f, 1.0f);
    addParam("Map White To R", 1.0f, 0.0f, 1.0f);
    addParam("Map White To G", 1.0f, 0.0f, 1.0f);
    addParam("Map White To B", 1.0f, 0.0f, 1.0f);
    addParam("Amount to Tint", 100.0f, 0.0f, 100.0f);
}

std::unique_ptr<Effect> Tint::clone() const
{
    auto copy = std::make_unique<Tint>();
    copy->m_enabled = m_enabled;
    for (size_t i = 0; i < m_params.size(); ++i)
        copy->m_params[i].track = m_params[i].track;
    return copy;
}

// FillLeftWithRight

FillLeftWithRight::FillLeftWithRight()
    : Effect(EffectType::FillLeftWithRight)
{
}

std::unique_ptr<Effect> FillLeftWithRight::clone() const
{
    auto copy = std::make_unique<FillLeftWithRight>();
    copy->m_enabled = m_enabled;
    return copy;
}

// ── FillRightWithLeft ─────────────────────────────────────────────────────

FillRightWithLeft::FillRightWithLeft()
    : Effect(EffectType::FillRightWithLeft)
{
}

std::unique_ptr<Effect> FillRightWithLeft::clone() const
{
    auto copy = std::make_unique<FillRightWithLeft>();
    copy->m_enabled = m_enabled;
    return copy;
}

} // namespace rt
