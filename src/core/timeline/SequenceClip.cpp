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

std::unique_ptr<Clip> SequenceClip::clone() const
{
    auto copy = std::make_unique<SequenceClip>();
    copyBaseStateTo(*copy);

    // SequenceClip fields
    copy->m_sequenceIndex = m_sequenceIndex;
    copy->m_sequenceName  = m_sequenceName;

    return copy;
}

} // namespace rt
