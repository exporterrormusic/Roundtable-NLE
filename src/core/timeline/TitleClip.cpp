/*
 * TitleClip.cpp — Title/text clip implementation.
 * Step 3: Core Data Model
 */

#include "timeline/TitleClip.h"

namespace rt {

TitleClip::TitleClip()
    : Clip(ClipType::Title)
{
    m_label = "Title";
    // Color left at sentinel — theme tint applies.
}

std::unique_ptr<Clip> TitleClip::clone() const
{
    auto copy = std::make_unique<TitleClip>();
    copyBaseStateTo(*copy);

    // TitleClip-specific
    copy->m_text         = m_text;
    copy->m_fontFamily   = m_fontFamily;
    copy->m_fontSize     = m_fontSize;
    copy->m_bold         = m_bold;
    copy->m_italic       = m_italic;
    copy->m_align        = m_align;
    copy->m_valign       = m_valign;
    copy->m_textColor    = m_textColor;
    copy->m_bgColor      = m_bgColor;
    copy->m_outlineColor = m_outlineColor;
    copy->m_outlineWidth = m_outlineWidth;
    copy->m_tracking     = m_tracking;
    copy->m_lineHeight   = m_lineHeight;

    return copy;
}

} // namespace rt
