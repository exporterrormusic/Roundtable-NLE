# ─── GPU Acceleration Library (Vulkan + CUDA) ────────────────────────────────

add_library(roundtable_gpu STATIC
    # Vulkan
    vulkan/Instance.cpp
    vulkan/Device.cpp
    vulkan/Allocator.cpp
    vulkan/Swapchain.cpp
    vulkan/Pipeline.cpp
    vulkan/CommandPool.cpp
    vulkan/Buffer.cpp
    vulkan/Texture.cpp
    vulkan/Framebuffer.cpp

    # Context
    GpuContext.cpp
    GpuScheduler.cpp

    # Renderers
    SpineRenderer.cpp
    Compositor.cpp
    CompositorInit.cpp
    TextRenderer.cpp
    WaveformRenderer.cpp
    TransitionRenderer.cpp
    EffectProcessor.cpp
    Nv12Converter.cpp
    Nv12ConverterPipeline.cpp
    Nv12ConverterYuv420p.cpp
    Nv12ConverterConvert.cpp
    Nv12ConverterP010.cpp
    Nv12ConverterYuva444p12.cpp
    GpuTextureCache.cpp
    VideoUploader.cpp
    ViewportLayout.cpp
    StagingRing.cpp
    GpuResourceManager.cpp

    # Composite engine (extracted from CompositeService)
    CompositeEngine.cpp
    CompositeEngineRenderGraph.cpp

    # A/B render-path verification harness (#18)
    diag/FrameSignature.cpp
    diag/FrameSignatureLog.cpp

    # Composite service (extracted from TimelineWorkspaceComposite.cpp)
    CompositeService.cpp
    CompositeServiceFrame.cpp
    CompositeServiceFramePrewarm.cpp
    CompositeServiceBlend.cpp
    CompositeServiceSpine.cpp
    CompositeServiceLayerBuild.cpp
    CompositeServiceLayerBuildVideo.cpp

    # GPU abstraction layer
    GpuWorkSubmission.cpp

    # GPU upload manager
    GpuUploadManager.cpp

    # GPU composite orchestration (delegates to CompositeEngine)
    CompositeServiceGpuOrchestration.cpp

    # CPU Compositor Safe Mode — removed in P2 of CLAUDE_IMPROVEMENT_PLAN.
    # Device-lost is now treated as fatal; no per-frame CPU fallback.

    # Render graph DAG (Phase 6 — DAG-based render passes)
    render_graph/GpuRenderGraph.cpp
)

target_include_directories(roundtable_gpu PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(roundtable_gpu PRIVATE WIN32_LEAN_AND_MEAN)

# PCH — enabled May 3, 2026.
target_precompile_headers(roundtable_gpu PRIVATE pch_gpu.h)

# P1.5: GpuScheduler.cpp is the only TU allowed to call vkQueueSubmit
# directly.  The pch_gpu.h macro guard replaces vkQueueSubmit elsewhere
# with a sentinel that fails to compile.
#
# IMPORTANT: target_precompile_headers builds ONE PCH per target, with
# the macro definition baked in.  Per-source COMPILE_DEFINITIONS arrives
# too late to opt out of a baked PCH symbol.  So we skip the PCH for
# this one source file — it includes <volk.h> and <spdlog/spdlog.h>
# directly, so it doesn't need the PCH.
set_source_files_properties(GpuScheduler.cpp PROPERTIES
    SKIP_PRECOMPILE_HEADERS TRUE
    COMPILE_DEFINITIONS "ROUNDTABLE_ALLOW_VK_QUEUE_SUBMIT")

# Use volk (runtime Vulkan loader — no Vulkan SDK install required)
target_link_libraries(roundtable_gpu PUBLIC
    roundtable_core
    volk::volk
    spdlog::spdlog
    glm::glm
)

# stb (header-only image loader)
if(TARGET stb)
    target_link_libraries(roundtable_gpu PUBLIC stb)
endif()

# Vulkan-Headers for type definitions
if(TARGET Vulkan::Headers)
    target_link_libraries(roundtable_gpu PUBLIC Vulkan::Headers)
endif()

# VMA — header-only Vulkan Memory Allocator
if(TARGET VulkanMemoryAllocator)
    target_link_libraries(roundtable_gpu PUBLIC VulkanMemoryAllocator)
elseif(TARGET GPUOpen::VulkanMemoryAllocator)
    target_link_libraries(roundtable_gpu PUBLIC GPUOpen::VulkanMemoryAllocator)
endif()

# FFmpeg (pre-built, shipped in third_party/)
if(FFMPEG_FOUND)
    target_link_libraries(roundtable_gpu PUBLIC ffmpeg::ffmpeg)
    target_compile_definitions(roundtable_gpu PUBLIC ROUNDTABLE_HAS_FFMPEG)
    message(STATUS "GPU layer: FFmpeg linked for hardware-accelerated video decode")
endif()

# CUDA (optional — graceful degradation if not available)
if(CUDAToolkit_FOUND)
    # Re-find in this scope to get CUDA::cudart / CUDA::cuda_driver targets
    find_package(CUDAToolkit QUIET)
    target_sources(roundtable_gpu PRIVATE
        cuda/CudaContext.cpp
        cuda/NvdecDecoder.cpp
        cuda/NvencEncoder.cpp
        cuda/CudaVulkanInterop.cpp
        cuda/WhisperCuda.cpp
    )
    target_link_libraries(roundtable_gpu PUBLIC
        CUDA::cudart
        CUDA::cuda_driver
    )
    target_compile_definitions(roundtable_gpu PUBLIC ROUNDTABLE_HAS_CUDA)
    message(STATUS "CUDA found — direct NVDEC/NVENC/Tensor acceleration enabled")
else()
    message(STATUS "CUDA Toolkit not installed — NVDEC via FFmpeg hwaccel (still hardware-accelerated)")
endif()
