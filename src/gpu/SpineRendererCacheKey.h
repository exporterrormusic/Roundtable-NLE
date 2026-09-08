#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

namespace rt {

/// Stable identity for a reusable Spine surface. Clips that reference the
/// same immutable character asset share instance zero across edits. A second
/// simultaneous copy of that asset uses instance one, preventing both layers
/// from aliasing the same mutable framebuffer.
inline std::string spineRendererContentKey(
    const std::string& characterAssetKey, uint32_t simultaneousInstance)
{
    return characterAssetKey + "|instance=" +
           std::to_string(simultaneousInstance);
}

/// Exact identity of one persistent Spine GPU surface and atlas set.
///
/// Keeping the string in the unordered-map key makes hash collisions benign:
/// equality still compares the complete character/outfit/stance identity.
struct SpineRendererCacheKey
{
    uint32_t width{0};
    uint32_t height{0};
    std::string contentKey;

    [[nodiscard]] bool operator==(
        const SpineRendererCacheKey& other) const noexcept
    {
        return width == other.width && height == other.height &&
               contentKey == other.contentKey;
    }
};

struct SpineRendererCacheKeyHash
{
    [[nodiscard]] size_t operator()(
        const SpineRendererCacheKey& key) const noexcept
    {
        size_t seed = std::hash<std::string>{}(key.contentKey);
        const auto mix = [&seed](size_t value) {
            seed ^= value + static_cast<size_t>(0x9e3779b9u) +
                    (seed << 6u) + (seed >> 2u);
        };
        mix(std::hash<uint32_t>{}(key.width));
        mix(std::hash<uint32_t>{}(key.height));
        return seed;
    }
};

} // namespace rt
