/*
 * Clip.cpp — Base clip implementation.
 * Step 3: Core Data Model
 */

#include "timeline/Clip.h"
#include "timeline/Position2D.h"

#include <algorithm>
#include <atomic>
#include <vector>

namespace rt {

// Thread-safe unique ID generation
static std::atomic<uint64_t> s_idCounter{1};

Clip::Clip(ClipType type)
    : m_type(type)
    , m_id(s_idCounter.fetch_add(1, std::memory_order_relaxed))
{
}

Clip::~Clip() = default;

void Clip::reserveId(uint64_t usedId) noexcept
{
    // Bump the shared counter so the next fetch_add yields > usedId.
    uint64_t cur = s_idCounter.load(std::memory_order_relaxed);
    while (cur <= usedId &&
           !s_idCounter.compare_exchange_weak(cur, usedId + 1,
                                              std::memory_order_relaxed)) {
        // retry with refreshed `cur`
    }
}

std::unique_ptr<Clip> Clip::clone() const
{
    auto copy = createEmpty();
    copy->copyBaseStateFrom(*this, CloneMode::Duplicate);
    copy->copyOwnStateFrom(*this, CloneMode::Duplicate);
    return copy;
}

std::unique_ptr<Clip> Clip::cloneForSnapshot() const
{
    auto copy = createEmpty();
    copy->copyBaseStateFrom(*this, CloneMode::Snapshot);
    copy->copyOwnStateFrom(*this, CloneMode::Snapshot);
    copy->m_id = m_id;
    return copy;
}

bool Clip::assignStateFrom(const Clip& src)
{
    if (src.m_type != m_type) return false;
    if (&src == this) return true;
    copyBaseStateFrom(src, CloneMode::Assign);
    copyOwnStateFrom(src, CloneMode::Assign);
    return true;
}

void Clip::copyBaseStateFrom(const Clip& src, CloneMode mode)
{
    m_label        = src.m_label;
    m_shotName     = src.m_shotName;
    m_layerId      = src.m_layerId;
    m_groupId      = src.m_groupId;
    m_linkId       = src.m_linkId;
    m_syncLine     = src.m_syncLine;
    m_color        = src.m_color;
    m_enabled      = src.m_enabled;
    m_offline      = src.m_offline;
    m_renderStatus = src.m_renderStatus;

    m_timelineIn        = src.m_timelineIn;
    m_duration          = src.m_duration;
    m_sourceIn          = src.m_sourceIn;
    m_speed             = src.m_speed;
    m_maintainPitch     = src.m_maintainPitch;
    m_timeInterpolation = src.m_timeInterpolation;
    m_speedRamp         = src.m_speedRamp;

    m_opacity      = src.m_opacity;
    m_posX         = src.m_posX;
    m_posY         = src.m_posY;
    m_scaleX       = src.m_scaleX;
    m_scaleY       = src.m_scaleY;
    m_rotation     = src.m_rotation;
    m_shutterAngle = src.m_shutterAngle;
    m_anchorX      = src.m_anchorX;
    m_anchorY      = src.m_anchorY;

    m_blendMode = src.m_blendMode;
    switch (mode) {
    case CloneMode::Duplicate:
        if (auto cloned = src.m_effects.clone())
            m_effects = std::move(*cloned);
        break;
    case CloneMode::Snapshot:
        m_effects = src.m_effects.cloneKeepingIds();
        break;
    case CloneMode::Assign:
        m_effects.assignStateFrom(src.m_effects);
        break;
    }
    m_masks   = src.m_masks;
    m_markers = src.m_markers;
}

int Clip::migrateLegacyMasksToSourceLocal(uint32_t sequenceWidth,
                                          uint32_t sequenceHeight,
                                          uint32_t sourceWidth,
                                          uint32_t sourceHeight,
                                          int sourceRotation)
{
    if (sequenceWidth == 0 || sequenceHeight == 0 ||
        sourceWidth == 0 || sourceHeight == 0)
        return 0;

    const auto transformAt = [this, sequenceWidth, sequenceHeight,
                              sourceWidth, sourceHeight,
                              sourceRotation](int64_t time) {
        LegacyMaskMigrationTransform transform;
        transform.sequenceWidth = sequenceWidth;
        transform.sequenceHeight = sequenceHeight;
        transform.sourceWidth = sourceWidth;
        transform.sourceHeight = sourceHeight;
        transform.sourceRotation = sourceRotation;
        const auto position = evaluatePosition2D(m_posX, m_posY, time);
        transform.positionX = position.first;
        transform.positionY = position.second;
        transform.scaleX = m_scaleX.evaluate(time);
        transform.scaleY = m_scaleY.evaluate(time);
        transform.rotation = m_rotation.evaluate(time);
        transform.anchorX = m_anchorX.evaluate(time);
        transform.anchorY = m_anchorY.evaluate(time);
        transform.containFit = isCharacter();
        if (isCharacter()) {
            constexpr float kCharacterComposeFit = 0.85f;
            transform.scaleX *= kCharacterComposeFit;
            transform.scaleY *= kCharacterComposeFit;
        }
        return transform;
    };

    // t=0 can be singular during a deliberate scale-from-zero animation.
    // Gather every meaningful edit/key time and choose the earliest one with
    // a finite inverse. No valid candidate means conversion must be deferred.
    std::vector<int64_t> candidates{0, m_duration};
    auto addTrackTimes = [&candidates](const KeyframeTrack<float>& track) {
        for (const auto& key : track.keyframes()) candidates.push_back(key.time);
    };
    addTrackTimes(m_posX);
    addTrackTimes(m_posY);
    addTrackTimes(m_scaleX);
    addTrackTimes(m_scaleY);
    addTrackTimes(m_rotation);
    addTrackTimes(m_anchorX);
    addTrackTimes(m_anchorY);
    auto addMaskTimes = [&candidates, &addTrackTimes](const OpacityMask& mask) {
        for (const auto& key : mask.pathKeys) candidates.push_back(key.time);
        addTrackTimes(mask.feather);
        addTrackTimes(mask.expansion);
    };
    for (const auto& mask : m_masks) addMaskTimes(mask);
    for (size_t ei = 0; ei < m_effects.effectCount(); ++ei)
        for (const auto& mask : m_effects.effect(ei).masks())
            addMaskTimes(mask);
    std::sort(candidates.begin(), candidates.end());
    candidates.erase(std::unique(candidates.begin(), candidates.end()),
                     candidates.end());

    LegacyMaskMigrationTransform baseTransform = transformAt(0);
    bool foundValidTransform =
        mask_migration_detail::isUsableTransform(baseTransform);
    if (!foundValidTransform) {
        for (const int64_t time : candidates) {
            if (time == 0) continue;
            const auto candidate = transformAt(time);
            if (mask_migration_detail::isUsableTransform(candidate)) {
                baseTransform = candidate;
                foundValidTransform = true;
                break;
            }
        }
    }
    if (!foundValidTransform) return 0;

    int migrated = 0;
    auto migrateList = [&](std::vector<OpacityMask>& masks) {
        for (auto& mask : masks)
            if (migrateLegacyMaskToSourceLocal(
                    mask, baseTransform, transformAt))
                ++migrated;
    };
    migrateList(m_masks);
    for (size_t ei = 0; ei < m_effects.effectCount(); ++ei)
        migrateList(m_effects.effect(ei).masks());
    return migrated;
}

} // namespace rt

