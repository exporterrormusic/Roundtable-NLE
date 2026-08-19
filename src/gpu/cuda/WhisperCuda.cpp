/*
 * WhisperCuda.cpp — CUDA backend for whisper.cpp acceleration.
 *
 * When ROUNDTABLE_HAS_CUDA is defined, queries the GPU for capabilities
 * and provides memory estimates for model loading.
 * Otherwise, stub implementation reports CUDA as unavailable.
 */

#include "cuda/WhisperCuda.h"

#include <spdlog/spdlog.h>

#ifdef ROUNDTABLE_HAS_CUDA
#include <cuda_runtime.h>
#endif

namespace rt {

WhisperCuda::WhisperCuda() = default;

WhisperCuda::~WhisperCuda()
{
    shutdown();
}

bool WhisperCuda::init()
{
#ifdef ROUNDTABLE_HAS_CUDA
    int deviceCount = 0;
    cudaError_t err = cudaGetDeviceCount(&deviceCount);
    if (err != cudaSuccess || deviceCount == 0) {
        spdlog::info("WhisperCuda: No CUDA devices found");
        m_info.available = false;
        return false;
    }

    // Use device 0 (primary GPU)
    cudaDeviceProp prop;
    err = cudaGetDeviceProperties(&prop, 0);
    if (err != cudaSuccess) {
        spdlog::warn("WhisperCuda: Failed to get device properties");
        m_info.available = false;
        return false;
    }

    m_info.deviceIndex = 0;
    m_info.deviceName  = prop.name;

    // Get free memory
    size_t freeMem = 0, totalMem = 0;
    cudaSetDevice(0);
    cudaMemGetInfo(&freeMem, &totalMem);
    m_info.freeMemoryMB = freeMem / (1024 * 1024);

    // Check compute capability
    int smMajor = prop.major;
    m_info.tensorCoresAvailable = (smMajor >= 7); // Volta+
    m_info.fp16Supported        = (smMajor >= 7);
    m_info.available            = true;

    spdlog::info("WhisperCuda: {} (SM {}.{}, {} MB free, tensor cores: {})",
                 m_info.deviceName, prop.major, prop.minor,
                 m_info.freeMemoryMB,
                 m_info.tensorCoresAvailable ? "yes" : "no");
    return true;

#else
    spdlog::info("WhisperCuda: CUDA not available at build time");
    m_info.available = false;
    return false;
#endif
}

void WhisperCuda::shutdown()
{
    m_info = WhisperCudaInfo{};
}

const char* WhisperCuda::recommendedComputeType() const noexcept
{
    if (m_info.fp16Supported)
        return "float16";
    return "float32";
}

size_t WhisperCuda::estimateModelVRAM(const std::string& modelSize) const noexcept
{
    // Approximate VRAM requirements for whisper.cpp models (in MB)
    if (modelSize == "tiny")     return 75;
    if (modelSize == "base")     return 150;
    if (modelSize == "small")    return 500;
    if (modelSize == "medium")   return 1500;
    if (modelSize == "large-v3-turbo") return 1700;
    if (modelSize == "large-v2") return 3000;
    if (modelSize == "large-v3") return 3000;
    return 500; // default estimate
}

bool WhisperCuda::canFitModel(const std::string& modelSize) const noexcept
{
    if (!m_info.available) return false;
    size_t required = estimateModelVRAM(modelSize);
    // Leave 500MB headroom for other GPU operations
    return m_info.freeMemoryMB > required + 500;
}

} // namespace rt
