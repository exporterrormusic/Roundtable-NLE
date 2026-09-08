#pragma once

namespace rt {

/// Defines the synchronization boundary used before destroying GPU resources.
/// DeviceWide is the conservative global/application path. SessionScoped is
/// valid only when the owner drains every fence for work that can reference
/// the resources being destroyed.
enum class GpuTeardownMode {
    DeviceWide,
    SessionScoped,
};

} // namespace rt
