/*
 * SequenceClip — Nested sequence clip.
 *
 * References another sequence (Timeline) within the project by index.
 * When rendered, the inner sequence is composited and used as a single
 * video layer in the parent timeline.
 */

#pragma once

#include "timeline/Clip.h"

namespace rt {

class SequenceClip : public Clip
{
public:
    SequenceClip();
    ~SequenceClip() override = default;

    /// Index of the referenced sequence in the Project's sequence list.
    [[nodiscard]] size_t sequenceIndex() const noexcept { return m_sequenceIndex; }
    void setSequenceIndex(size_t idx) noexcept { m_sequenceIndex = idx; }

    /// Display name of the nested sequence (informational, not authoritative).
    [[nodiscard]] const std::string& sequenceName() const noexcept { return m_sequenceName; }
    void setSequenceName(const std::string& name) { m_sequenceName = name; }
protected:

    [[nodiscard]] std::unique_ptr<Clip> createEmpty() const override;

    void copyOwnStateFrom(const Clip& src, CloneMode mode) override;
public:

private:
    size_t      m_sequenceIndex{0};
    std::string m_sequenceName;
};

} // namespace rt
