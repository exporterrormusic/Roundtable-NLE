/*
 * PrefetchTexturePool.cpp — see PrefetchTexturePool.h.
 */

#include "PrefetchTexturePool.h"

#include "GpuContext.h"
#include "vulkan/Texture.h"
#include "vulkan/Device.h"
#include "vulkan/Allocator.h"

#include <spdlog/spdlog.h>

namespace rt {

PrefetchTexturePool::~PrefetchTexturePool()
{
    // Drop every pooled Texture under lock. Each ~Texture calls
    // vmaDestroyImage / vkDestroyImageView / vkDestroySampler; those
    // require the VkDevice to still be alive. Lifetime invariant: this
    // pool lives inside MediaPool, which is destroyed before GpuContext
    // shuts down — so the device is valid here.
    clear();
}

void PrefetchTexturePool::sweepLocked(const PoolKey& key, uint64_t now)
{
    auto qit = m_quarantine.find(key);
    if (qit == m_quarantine.end()) return;
    auto& q = qit->second;
    auto& bucket = m_buckets[key];

    for (auto it = q.begin(); it != q.end(); ) {
        // Elapsed since release.  Epoch is monotonic and releaseEpoch was
        // sampled at (a past) release, so now >= releaseEpoch always.
        if (now - it->releaseEpoch >= kQuarantineFrames) {
            if (bucket.size() < kMaxPerShape)
                bucket.push_back(std::move(it->tex));
            // Else: past quarantine AND bucket full → safe to destroy now
            // (no GPU work references it).  unique_ptr dtor frees VMA memory.
            it = q.erase(it);
        } else {
            ++it;
        }
    }
}

std::unique_ptr<Texture> PrefetchTexturePool::acquire(
    uint32_t width, uint32_t height,
    VkFormat format, VkImageUsageFlags usage)
{
    std::lock_guard lk(m_mtx);
    PoolKey key{width, height, format, usage};

    // Promote any quarantined textures that have aged out before reusing.
    sweepLocked(key, GpuContext::get().compositeEpoch());

    auto it = m_buckets.find(key);
    if (it == m_buckets.end() || it->second.empty())
        return nullptr;  // caller creates a fresh Texture
    std::unique_ptr<Texture> tex = std::move(it->second.back());
    it->second.pop_back();
    return tex;
}

void PrefetchTexturePool::release(std::unique_ptr<Texture> tex,
                                  uint32_t width, uint32_t height,
                                  VkFormat format, VkImageUsageFlags usage)
{
    if (!tex) return;
    std::lock_guard lk(m_mtx);
    PoolKey key{width, height, format, usage};

    const uint64_t now = GpuContext::get().compositeEpoch();
    // Opportunistically age out earlier releases so quarantine doesn't grow
    // unboundedly when acquire isn't being called for this shape.
    sweepLocked(key, now);

    // NEVER recycle straight into the reusable bucket: the texture may still
    // be sampled by an in-flight compositor submit.  Quarantine it instead;
    // sweepLocked promotes it once kQuarantineFrames epochs have passed.
    m_quarantine[key].push_back(Quarantined{std::move(tex), now});
}

void PrefetchTexturePool::clear()
{
    std::lock_guard lk(m_mtx);
    m_buckets.clear();
    m_quarantine.clear();
}

size_t PrefetchTexturePool::totalEntries() const
{
    std::lock_guard lk(m_mtx);
    size_t n = 0;
    for (const auto& [k, bucket] : m_buckets)
        n += bucket.size();
    for (const auto& [k, q] : m_quarantine)
        n += q.size();
    return n;
}

// ─────────────────────────────────────────────────────────────────────────
// makePooledTexture — see header for contract.
// ─────────────────────────────────────────────────────────────────────────
std::shared_ptr<Texture> makePooledTexture(
    PrefetchTexturePool& pool,
    GpuContext&          ctx,
    uint32_t             width,
    uint32_t             height,
    VkFormat             format,
    VkImageUsageFlags    usage,
    std::vector<uint32_t> concurrentQueueFamilies)
{
    if (width == 0 || height == 0) return {};
    if (!ctx.isInitialized()) return {};

    std::unique_ptr<Texture> raw = pool.acquire(width, height, format, usage);
    if (!raw) {
        raw = std::make_unique<Texture>();
        TextureConfig cfg;
        cfg.width  = width;
        cfg.height = height;
        cfg.format = format;
        cfg.usage  = usage;
        cfg.concurrentQueueFamilies = std::move(concurrentQueueFamilies);
        if (!raw->create(ctx.allocator().handle(),
                         ctx.device().handle(), cfg)) {
            spdlog::warn("makePooledTexture: vmaCreateImage failed "
                         "({}x{}, format={}, usage=0x{:x})",
                         width, height,
                         static_cast<int>(format),
                         static_cast<uint32_t>(usage));
            return {};
        }
    }

    // Custom deleter returns the Texture to the pool instead of destroying
    // it. The deleter captures `pool` by reference — see the lifetime
    // invariant in PrefetchTexturePool.h.
    Texture* leaked = raw.release();
    return std::shared_ptr<Texture>(
        leaked,
        [&pool, width, height, format, usage](Texture* p) {
            if (!p) return;
            pool.release(std::unique_ptr<Texture>(p),
                         width, height, format, usage);
        });
}

} // namespace rt
