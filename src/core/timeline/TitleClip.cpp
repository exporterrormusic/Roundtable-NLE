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

std::unique_ptr<Clip> TitleClip::createEmpty() const
{
    return std::make_unique<TitleClip>();
}

void TitleClip::copyOwnStateFrom(const Clip& src, CloneMode /*mode*/)
{
    const auto& o = static_cast<const TitleClip&>(src);
    // TitleClip-specific
    m_text         = o.m_text;
    m_fontFamily   = o.m_fontFamily;
    m_fontSize     = o.m_fontSize;
    m_bold         = o.m_bold;
    m_italic       = o.m_italic;
    m_align        = o.m_align;
    m_valign       = o.m_valign;
    m_textColor    = o.m_textColor;
    m_bgColor      = o.m_bgColor;
    m_outlineColor = o.m_outlineColor;
    m_outlineWidth = o.m_outlineWidth;
    m_tracking     = o.m_tracking;
    m_lineHeight   = o.m_lineHeight;
}

} // namespace rt
