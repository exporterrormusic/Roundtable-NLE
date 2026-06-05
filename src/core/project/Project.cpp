/*
 * Project.cpp — Top-level project container implementation.
 * Step 5: Project Serialization
 */

#include "project/Project.h"
#include "project/AssetDatabase.h"
#include "timeline/Timeline.h"
#include "timeline/Track.h"
#include "timeline/Clip.h"
#include "command/CommandStack.h"

#include <spdlog/spdlog.h>
#include <algorithm>

namespace rt {

Project::Project()
    : m_assets(std::make_unique<AssetDatabase>())
    , m_commands(std::make_unique<CommandStack>())
{
    // Create a default sequence
    m_sequences.push_back(std::make_unique<Timeline>());

    // Mark the project dirty whenever a command is executed
    m_commands->setChangeCallback([this]() {
        m_modified.store(true, std::memory_order_relaxed);
    });
}

Project::~Project() = default;

Project::Project(Project&& o) noexcept
    : m_name(std::move(o.m_name))
    , m_show(std::move(o.m_show))
    , m_filePath(std::move(o.m_filePath))
    , m_modified(o.m_modified.load(std::memory_order_relaxed))
    , m_formatVersion(o.m_formatVersion)
    , m_settings(std::move(o.m_settings))
    , m_sequences(std::move(o.m_sequences))
    , m_activeSequence(o.m_activeSequence)
    , m_assets(std::move(o.m_assets))
    , m_commands(std::move(o.m_commands))
    , m_binFiles(std::move(o.m_binFiles))
    , m_binFolders(std::move(o.m_binFolders))
    , m_binItems(std::move(o.m_binItems))
{}

Project& Project::operator=(Project&& o) noexcept
{
    if (this != &o) {
        m_name            = std::move(o.m_name);
        m_show            = std::move(o.m_show);
        m_filePath        = std::move(o.m_filePath);
        m_modified.store(o.m_modified.load(std::memory_order_relaxed), std::memory_order_relaxed);
        m_formatVersion   = o.m_formatVersion;
        m_settings        = std::move(o.m_settings);
        m_sequences       = std::move(o.m_sequences);
        m_activeSequence  = o.m_activeSequence;
        m_assets          = std::move(o.m_assets);
        m_commands        = std::move(o.m_commands);
        m_binFiles        = std::move(o.m_binFiles);
        m_binFolders      = std::move(o.m_binFolders);
        m_binItems        = std::move(o.m_binItems);
    }
    return *this;
}

// ── Sequence accessors ──────────────────────────────────────────────────────

Timeline* Project::timeline() noexcept
{
    if (m_activeSequence < m_sequences.size())
        return m_sequences[m_activeSequence].get();
    if (!m_sequences.empty())
        return m_sequences[0].get();
    return nullptr;
}

const Timeline* Project::timeline() const noexcept
{
    if (m_activeSequence < m_sequences.size())
        return m_sequences[m_activeSequence].get();
    if (!m_sequences.empty())
        return m_sequences[0].get();
    return nullptr;
}

Timeline* Project::sequence(size_t index) noexcept
{
    return index < m_sequences.size() ? m_sequences[index].get() : nullptr;
}

const Timeline* Project::sequence(size_t index) const noexcept
{
    return index < m_sequences.size() ? m_sequences[index].get() : nullptr;
}

Timeline* Project::setActiveSequence(size_t index)
{
    if (index < m_sequences.size()) {
        m_activeSequence = index;
        spdlog::info("Project: switched to sequence {} '{}'",
                     index, m_sequences[index]->name());
        return m_sequences[index].get();
    }
    return timeline();
}

Timeline* Project::addSequence(const std::string& name)
{
    auto tl = std::make_unique<Timeline>();
    std::string seqName = name.empty() ? nextSequenceName() : name;
    tl->setName(seqName);
    tl->addVideoTrack("Video 1");
    tl->addAudioTrack("Audio 1");
    m_sequences.push_back(std::move(tl));
    m_modified = true;
    spdlog::info("Project: added sequence '{}' (total: {})", seqName, m_sequences.size());
    return m_sequences.back().get();
}

Timeline* Project::addSequence(std::unique_ptr<Timeline> seq)
{
    if (!seq) return nullptr;
    std::string seqName = seq->name();
    m_sequences.push_back(std::move(seq));
    m_modified = true;
    spdlog::info("Project: added pre-built sequence '{}' (total: {})", seqName, m_sequences.size());
    return m_sequences.back().get();
}

Timeline* Project::duplicateSequence(size_t srcIndex)
{
    if (srcIndex >= m_sequences.size()) return nullptr;

    const Timeline* src = m_sequences[srcIndex].get();
    auto dup = src->clone();
    dup->setName(src->name() + " Copy");

    m_sequences.push_back(std::move(dup));
    m_modified = true;
    spdlog::info("Project: duplicated sequence '{}' → '{}'",
                 src->name(), m_sequences.back()->name());
    return m_sequences.back().get();
}

bool Project::removeSequence(size_t index)
{
    if (m_sequences.empty() || index >= m_sequences.size())
        return false;

    // If this is the last sequence, replace it with a fresh default instead of
    // leaving the project with zero sequences (which would break the UI).
    if (m_sequences.size() == 1) {
        m_sequences[0] = std::make_unique<Timeline>();
        m_sequences[0]->setName(nextSequenceName());
        m_activeSequence = 0;
        m_modified = true;
        spdlog::info("Project: replaced last sequence with new empty sequence");
        return true;
    }

    std::string name = m_sequences[index]->name();
    m_sequences.erase(m_sequences.begin() + static_cast<ptrdiff_t>(index));

    // Adjust active index if needed
    if (m_activeSequence >= m_sequences.size())
        m_activeSequence = m_sequences.size() - 1;
    else if (m_activeSequence > index)
        --m_activeSequence;

    m_modified = true;
    spdlog::info("Project: removed sequence '{}' (remaining: {})", name, m_sequences.size());
    return true;
}

std::unique_ptr<Timeline> Project::extractSequence(size_t index)
{
    if (m_sequences.size() <= 1 || index >= m_sequences.size())
        return nullptr;

    auto seq = std::move(m_sequences[index]);
    m_sequences.erase(m_sequences.begin() + static_cast<ptrdiff_t>(index));

    if (m_activeSequence >= m_sequences.size())
        m_activeSequence = m_sequences.size() - 1;
    else if (m_activeSequence > index)
        --m_activeSequence;

    m_modified = true;
    spdlog::info("Project: extracted sequence '{}' (remaining: {})", seq->name(), m_sequences.size());
    return seq;
}

void Project::insertSequence(size_t index, std::unique_ptr<Timeline> seq)
{
    if (!seq) return;
    if (index > m_sequences.size()) index = m_sequences.size();
    std::string name = seq->name();
    m_sequences.insert(m_sequences.begin() + static_cast<ptrdiff_t>(index), std::move(seq));
    // Adjust active index if insertion is before/at it
    if (m_activeSequence >= index && m_sequences.size() > 1)
        ++m_activeSequence;
    m_modified = true;
    spdlog::info("Project: inserted sequence '{}' at index {} (total: {})", name, index, m_sequences.size());
}

std::string Project::nextSequenceName() const
{
    int maxNum = 0;
    for (const auto& seq : m_sequences) {
        const auto& n = seq->name();
        if (n.rfind("Sequence ", 0) == 0) {
            try {
                int num = std::stoi(n.substr(9));
                maxNum = std::max(maxNum, num);
            } catch (...) {}
        }
    }
    return "Sequence " + std::to_string(maxNum + 1);
}

std::unique_ptr<Project> Project::createNew(const std::string& name)
{
    auto project = std::make_unique<Project>();
    project->setName(name);

    // The default constructor already creates a default sequence.
    // Add default tracks to it.
    project->timeline()->setName("Sequence 1");
    project->timeline()->addVideoTrack("Video 1");
    project->timeline()->addAudioTrack("Audio 1");

    project->setModified(false);
    spdlog::info("Created new project '{}'", name);
    return project;
}

} // namespace rt

