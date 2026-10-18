/*
 * Buffer.cpp — VkBuffer + VMA allocation wrapper.
 * Step 2: Vulkan Initialization
 */

#include <volk.h>
#include "vulkan/Buffer.h"

#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
#include <vk_mem_alloc.h>

#include <spdlog/spdlog.h>

#include <cstring>
#include <utility>

namespace rt {

// ── Destructor ──────────────────────────────────────────────────────────────

Buffer::~Buffer()
{
    destroy();
}

// ── Move constructor ────────────────────────────────────────────────────────

Buffer::Buffer(Buffer&& other) noexcept
    : m_buffer(other.m_buffer)
    , m_allocation(other.m_allocation)
    , m_allocator(other.m_allocator)
    , m_size(other.m_size)
    , m_usage(other.m_usage)
    , m_mappedData(other.m_mappedData)
    , m_persistentlyMapped(other.m_persistentlyMapped)
    , m_explicitMapActive(other.m_explicitMapActive)
{
    other.m_buffer     = VK_NULL_HANDLE;
    other.m_allocation = nullptr;
    other.m_allocator  = nullptr;
    other.m_size       = 0;
    other.m_mappedData = nullptr;
    other.m_persistentlyMapped = false;
    other.m_explicitMapActive = false;
}

// ── Move assignment ─────────────────────────────────────────────────────────

Buffer& Buffer::operator=(Buffer&& other) noexcept
{
    if (this != &other)
    {
        destroy();
        m_buffer     = other.m_buffer;
        m_allocation = other.m_allocation;
        m_allocator  = other.m_allocator;
        m_size       = other.m_size;
        m_usage      = other.m_usage;
        m_mappedData = other.m_mappedData;
        m_persistentlyMapped = other.m_persistentlyMapped;
        m_explicitMapActive = other.m_explicitMapActive;

        other.m_buffer     = VK_NULL_HANDLE;
        other.m_allocation = nullptr;
        other.m_allocator  = nullptr;
        other.m_size       = 0;
        other.m_mappedData = nullptr;
        other.m_persistentlyMapped = false;
        other.m_explicitMapActive = false;
    }
    return *this;
}

// ── create ──────────────────────────────────────────────────────────────────

bool Buffer::create(VmaAllocator allocator, VkDeviceSize size, BufferUsage usage)
{
    destroy();

    m_allocator = allocator;
    m_size      = size;
    m_usage     = usage;
    m_mappedData = nullptr;
    m_persistentlyMapped = false;
    m_explicitMapActive = false;

    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size  = size;

    VmaAllocationCreateInfo allocCreateInfo{};

    switch (usage)
    {
    case BufferUsage::Vertex:
        bufferInfo.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                           VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                           VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
        allocCreateInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        break;

    case BufferUsage::Index:
        bufferInfo.usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                           VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                           VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
        allocCreateInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        break;

    case BufferUsage::Uniform:
        bufferInfo.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT
                         | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        allocCreateInfo.usage = VMA_MEMORY_USAGE_AUTO;
        allocCreateInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                                VMA_ALLOCATION_CREATE_MAPPED_BIT;
        break;

    case BufferUsage::Storage:
        bufferInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                           VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                           VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
        allocCreateInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        break;

    case BufferUsage::Staging:
        bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        allocCreateInfo.usage = VMA_MEMORY_USAGE_AUTO;
        allocCreateInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                                VMA_ALLOCATION_CREATE_MAPPED_BIT;
        break;

    case BufferUsage::Readback:
        bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        allocCreateInfo.usage = VMA_MEMORY_USAGE_AUTO;
        allocCreateInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT |
                                VMA_ALLOCATION_CREATE_MAPPED_BIT;
        break;

    case BufferUsage::StorageReadback:
        bufferInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                           VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        // System memory: with plain AUTO a storage buffer can land in
        // device-local host-visible memory (ReBAR), which the CPU reads
        // uncached — slower per byte than the BGRA staging readback.
        allocCreateInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
        allocCreateInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT |
                                VMA_ALLOCATION_CREATE_MAPPED_BIT;
        break;

    case BufferUsage::IndirectDraw:
        bufferInfo.usage = VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
                           VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                           VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        allocCreateInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        break;

    case BufferUsage::VertexDynamic:
        bufferInfo.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
        allocCreateInfo.usage = VMA_MEMORY_USAGE_AUTO;
        allocCreateInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                                VMA_ALLOCATION_CREATE_MAPPED_BIT;
        break;

    case BufferUsage::IndexDynamic:
        bufferInfo.usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
        allocCreateInfo.usage = VMA_MEMORY_USAGE_AUTO;
        allocCreateInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                                VMA_ALLOCATION_CREATE_MAPPED_BIT;
        break;
    }

    VmaAllocationInfo allocInfo{};
    VkResult result = vmaCreateBuffer(m_allocator, &bufferInfo, &allocCreateInfo,
                                       &m_buffer, &m_allocation, &allocInfo);
    if (result != VK_SUCCESS)
    {
        spdlog::error("Failed to create buffer (size: {} bytes, VkResult: {})",
                      size, static_cast<int>(result));
        return false;
    }

    // If allocation was created with MAPPED_BIT, store the pointer
    if (allocInfo.pMappedData) {
        m_mappedData = allocInfo.pMappedData;
        m_persistentlyMapped = true;
    }

    return true;
}

// ── destroy ─────────────────────────────────────────────────────────────────

void Buffer::destroy()
{
    if (m_buffer != VK_NULL_HANDLE && m_allocator != nullptr)
    {
        if (m_explicitMapActive)
        {
            vmaUnmapMemory(m_allocator, m_allocation);
            m_explicitMapActive = false;
            m_mappedData = nullptr;
        }
        vmaDestroyBuffer(m_allocator, m_buffer, m_allocation);
        m_buffer     = VK_NULL_HANDLE;
        m_allocation = nullptr;
    }
    m_allocator = nullptr;
    m_size      = 0;
    m_persistentlyMapped = false;
    m_explicitMapActive = false;
    m_mappedData = nullptr;
}

// ── map ─────────────────────────────────────────────────────────────────────

void* Buffer::map()
{
    if (m_mappedData) return m_mappedData;

    VkResult result = vmaMapMemory(m_allocator, m_allocation, &m_mappedData);
    if (result != VK_SUCCESS)
    {
        spdlog::error("Failed to map buffer memory");
        return nullptr;
    }

    m_explicitMapActive = true;

    return m_mappedData;
}

// ── unmap ───────────────────────────────────────────────────────────────────

void Buffer::unmap()
{
    if (m_persistentlyMapped) return;

    if (m_mappedData && m_explicitMapActive)
    {
        vmaUnmapMemory(m_allocator, m_allocation);
        m_mappedData = nullptr;
        m_explicitMapActive = false;
    }
}

// ── upload ──────────────────────────────────────────────────────────────────

void Buffer::upload(const void* data, VkDeviceSize size, VkDeviceSize offset)
{
    void* mapped = map();
    if (!mapped)
    {
        spdlog::error("Cannot upload: buffer is not mappable");
        return;
    }

    std::memcpy(static_cast<char*>(mapped) + offset, data, static_cast<size_t>(size));
}

// ── flush ───────────────────────────────────────────────────────────────────

void Buffer::flush(VkDeviceSize size, VkDeviceSize offset)
{
    vmaFlushAllocation(m_allocator, m_allocation, offset, size);
}

void Buffer::invalidate(VkDeviceSize size, VkDeviceSize offset)
{
    vmaInvalidateAllocation(m_allocator, m_allocation, offset, size);
}

} // namespace rt

