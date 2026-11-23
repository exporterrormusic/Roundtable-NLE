/*
 * CudaContext.cpp — CUDA initialization
 *
 * Two implementations:
 *   1. ROUNDTABLE_HAS_CUDA: links against CUDA Toolkit (native API)
 *   2. Fallback: stub — CUDA features disabled, NVDEC via FFmpeg hwaccel
 */

#include "CudaContext.h"

#include <spdlog/spdlog.h>

#ifdef ROUNDTABLE_HAS_CUDA

#include <cuda.h>
#include <cuda_runtime.h>

namespace rt {

CudaContext::CudaContext() = default;

CudaContext::~CudaContext()
{
    shutdown();
}

bool CudaContext::init(int preferredDevice)
{
    if (m_available) return true;

    CUresult res = cuInit(0);
    if (res != CUDA_SUCCESS) {
        spdlog::warn("CudaContext: cuInit failed ({})", static_cast<int>(res));
        return false;
    }

    int deviceCount = 0;
    cuDeviceGetCount(&deviceCount);
    if (deviceCount == 0) {
        spdlog::warn("CudaContext: no CUDA devices found");
        return false;
    }

    m_device = (preferredDevice >= 0 && preferredDevice < deviceCount)
                   ? preferredDevice : 0;

    CUdevice cuDev;
    cuDeviceGet(&cuDev, m_device);

    // Get device info
    char name[256] = {};
    cuDeviceGetName(name, sizeof(name), cuDev);
    m_info.deviceIndex = m_device;
    m_info.name = name;

    cuDeviceGetAttribute(&m_info.computeMajor, CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MAJOR, cuDev);
    cuDeviceGetAttribute(&m_info.computeMinor, CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MINOR, cuDev);

    size_t totalMem = 0;
    cuDeviceTotalMem(&totalMem, cuDev);
    m_info.totalMemory = totalMem;

    // NVDEC/NVENC support is present on all modern NVIDIA GPUs (Kepler+)
    m_info.nvdecSupported = (m_info.computeMajor >= 3);
    m_info.nvencSupported = (m_info.computeMajor >= 3);

    // Create context — AUTO scheduling adapts to workload patterns,
    // better for mixed sync/async workloads like video editing
    CUcontext ctx;
#if CUDA_VERSION >= 13000
    // CUDA 13: cuCtxCreate gained a creation-params argument; nullptr gives
    // the same regular context as the CUDA 12 three-argument form.
    res = cuCtxCreate(&ctx, nullptr, CU_CTX_SCHED_AUTO, cuDev);
#else
    res = cuCtxCreate(&ctx, CU_CTX_SCHED_AUTO, cuDev);
#endif
    if (res != CUDA_SUCCESS) {
        spdlog::error("CudaContext: cuCtxCreate failed ({})", static_cast<int>(res));
        return false;
    }
    m_context = ctx;
    m_available = true;

    spdlog::info("CudaContext: initialized on {} (compute {}.{}, {:.0f} MB VRAM)",
                 m_info.name, m_info.computeMajor, m_info.computeMinor,
                 m_info.totalMemory / (1024.0 * 1024.0));

    return true;
}

void CudaContext::shutdown()
{
    if (m_context) {
        cuCtxDestroy(static_cast<CUcontext>(m_context));
        m_context = nullptr;
    }
    m_available = false;
    m_device = -1;
}

void CudaContext::pushContext()
{
    if (m_context)
        cuCtxPushCurrent(static_cast<CUcontext>(m_context));
}

void CudaContext::popContext()
{
    if (m_context) {
        CUcontext dummy;
        cuCtxPopCurrent(&dummy);
    }
}

} // namespace rt

#else // !ROUNDTABLE_HAS_CUDA

namespace rt {

CudaContext::CudaContext() = default;
CudaContext::~CudaContext() = default;

bool CudaContext::init(int /*preferredDevice*/)
{
    spdlog::info("CudaContext: CUDA Toolkit not available — "
                 "NVDEC via FFmpeg hwaccel still works");
    return false;
}

void CudaContext::shutdown() {}
void CudaContext::pushContext() {}
void CudaContext::popContext() {}

} // namespace rt

#endif // ROUNDTABLE_HAS_CUDA

