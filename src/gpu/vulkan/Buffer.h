/*
 * Buffer — VkBuffer + VMA allocation RAII wrapper.
 *
 * Step 2: Wraps vertex, index, uniform, and storage buffers with
 * automatic VMA memory allocation and mapping.
 */

#pragma once

#include <vulkan/vulkan.h>
#include <cstdint>

// Forward declare VMA types
struct VmaAllocator_T;
struct VmaAllocation_T;
using VmaAllocator  = VmaAllocator_T*;
using VmaAllocation = VmaAllocation_T*;

namespace rt {

class Allocator;

/// Buffer usage presets.
enum class BufferUsage : uint8_t
{
    Vertex,          // Vertex buffer (device-local, staged upload)
    Index,           // Index buffer (device-local, staged upload)
    Uniform,         // Uniform buffer (host-visible, coherent)
    Storage,         // Storage buffer (device-local, compute r/w)
    Staging,         // Staging buffer (host-visible, for upload transfers)
    Readback,        // Readback buffer (host-visible, for GPU→CPU download)
    StorageReadback, // Readback buffer a compute shader writes directly (host-visible)
    IndirectDraw,    // Indirect draw commands
    VertexDynamic,   // Vertex buffer (host-visible, for per-frame CPU writes)
    IndexDynamic     // Index buffer (host-visible, for per-frame CPU writes)
};

/// RAII wrapper for VkBuffer + VmaAllocation.
class Buffer
{
public:
    Buffer() = default;
    ~Buffer();

    Buffer(const Buffer&) = delete;
    Buffer& operator=(const Buffer&) = delete;
    Buffer(Buffer&&) noexcept;
    Buffer& operator=(Buffer&&) noexcept;

    /// Create a buffer with VMA allocation.
    bool create(VmaAllocator allocator, VkDeviceSize size, BufferUsage usage);

    /// Destroy buffer and free memory.
    void destroy();

    // ── Data upload ─────────────────────────────────────────────────────

    /// Map host-visible buffer memory. Returns nullptr for device-local.
    void* map();

    /// Unmap previously mapped memory.
    void unmap();

    /// Upload data to a host-visible buffer.
    void upload(const void* data, VkDeviceSize size, VkDeviceSize offset = 0);

    /// Flush mapped memory (for non-coherent buffers).
    void flush(VkDeviceSize size = VK_WHOLE_SIZE, VkDeviceSize offset = 0);

    /// Make device writes visible to host reads (for non-coherent buffers).
    void invalidate(VkDeviceSize size = VK_WHOLE_SIZE, VkDeviceSize offset = 0);

    // ── Accessors ───────────────────────────────────────────────────────

    [[nodiscard]] VkBuffer       handle()     const noexcept { return m_buffer; }
    [[nodiscard]] VmaAllocation  allocation() const noexcept { return m_allocation; }
    [[nodiscard]] VkDeviceSize   size()       const noexcept { return m_size; }
    [[nodiscard]] BufferUsage    usage()      const noexcept { return m_usage; }
    [[nodiscard]] bool           isMapped()   const noexcept { return m_mappedData != nullptr; }

    operator VkBuffer() const noexcept { return m_buffer; }

private:
    VkBuffer      m_buffer{VK_NULL_HANDLE};
    VmaAllocation m_allocation{nullptr};
    VmaAllocator  m_allocator{nullptr};
    VkDeviceSize  m_size{0};
    BufferUsage   m_usage{BufferUsage::Vertex};
    void*         m_mappedData{nullptr};
    bool          m_persistentlyMapped{false};
    bool          m_explicitMapActive{false};
};

} // namespace rt
