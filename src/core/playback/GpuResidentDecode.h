#pragma once

#include <atomic>

namespace rt::GpuResidentDecode {

/// Whether prefetch keeps decoded frames on the GPU (GPU-resident decode,
/// zero-copy NVDEC path) instead of producing CPU-pixel frames.  On by
/// default; CompositeService applies the ROUNDTABLE_GPU_RESIDENT_DECODE=0
/// kill switch at startup.  Lives in core so the decode path can read it
/// without depending on the GPU compositor.
inline std::atomic<bool>& flag() noexcept
{
    static std::atomic<bool> enabled{true};
    return enabled;
}

inline void setEnabled(bool on) noexcept
{
    flag().store(on, std::memory_order_release);
}

[[nodiscard]] inline bool enabled() noexcept
{
    return flag().load(std::memory_order_acquire);
}

} // namespace rt::GpuResidentDecode
