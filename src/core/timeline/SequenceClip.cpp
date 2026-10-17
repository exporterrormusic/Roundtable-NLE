/*
 * SequenceClip.cpp — Nested sequence clip implementation.
 */

#include "timeline/SequenceClip.h"

namespace rt {

SequenceClip::SequenceClip()
    : Clip(ClipType::Sequence)
{
    setLabel("Nested Sequence");
    // Color left at sentinel — theme tint applies.
}

std::unique_ptr<Clip> SequenceClip::createEmpty() const
{
    return std::make_unique<SequenceClip>();
}

void SequenceClip::copyOwnStateFrom(const Clip& src, CloneMode /*mode*/)
{
    const auto& o = static_cast<const SequenceClip&>(src);
    // SequenceClip fields
    m_sequenceIndex = o.m_sequenceIndex;
    m_sequenceName  = o.m_sequenceName;
}

} // namespace rt
