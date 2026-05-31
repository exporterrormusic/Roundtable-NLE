# ─── Core Engine Library (no UI dependency) ──────────────────────────────────

add_library(roundtable_core STATIC
    # Project
    project/Project.cpp
    project/ProjectSerializer.cpp
    project/ProjectSerializerIO.cpp
    project/ClipSerialization.cpp
    project/AssetDatabase.cpp
    project/Settings.cpp
    project/AutoSave.cpp

    # Crash handler
    CrashHandler.cpp
    WorkerBreadcrumb.cpp

    # Hardware diagnostics (overlay-hook detection + GPU classification)
    HardwareDiagnostics.cpp

    # Performance profile (machine-adaptive resource knobs / Boost mode)
    PerformanceProfile.cpp

    # Timeline
    timeline/Timeline.cpp
    timeline/Track.cpp
    timeline/Clip.cpp
    timeline/SpineClip.cpp
    timeline/VideoClip.cpp
    timeline/AudioClip.cpp
    timeline/ImageClip.cpp
    timeline/TitleClip.cpp
    timeline/GraphicLayer.cpp
    timeline/GraphicClip.cpp
    timeline/AdjustmentClip.cpp
    timeline/SequenceClip.cpp
    timeline/Transition.cpp
    timeline/Keyframe.cpp
    timeline/KeyframeTrack.cpp
    timeline/MediaRelinker.cpp
    timeline/Marker.cpp
    timeline/EditOperations.cpp
    timeline/EditOperationsSelection.cpp
    timeline/EditOperationsTrim.cpp
    timeline/TimelineLayoutEngine.cpp

    # Command
    command/CommandStack.cpp
    command/CompoundCommand.cpp
    command/commands/ClipCommands.cpp
    command/commands/TrackCommands.cpp
    command/commands/TransitionCmds.cpp
    command/commands/KeyframeCmds.cpp
    command/commands/MarkerCommands.cpp

    # Media
    media/MediaPool.cpp
    media/MediaPoolOpen.cpp
    media/MediaPoolFrame.cpp
    media/MediaPoolPerf.cpp
    media/MediaPoolQueries.cpp
    media/MediaPoolDecode.cpp
    media/MediaPoolPrefetch.cpp
    media/MediaPoolPrefetchLoop.cpp
    media/MediaPoolPrefetchSchedule.cpp
    media/MediaPoolPrefetchConvert.cpp
    media/MediaPoolPrefetchConvertGpu.cpp
    media/MediaPoolPrefetchDecode.cpp
    media/PrefetchTexturePool.cpp
    media/VideoDecoder.cpp
    media/VideoDecoderInit.cpp
    media/VideoDecoderDecode.cpp
    media/AudioEngine.cpp
    media/TimeStretch.cpp
    media/AudioFile.cpp
    media/FrameCache.cpp
    media/CacheCoordinator.cpp
    media/UnifiedCache.cpp
    media/DiskFrameCache.cpp
    media/WaveformCache.cpp
    media/AVSyncClock.cpp
    media/PlaybackController.cpp
    media/PlaybackScheduler.cpp
    media/FrameClock.cpp
    media/FrameProducer.cpp
    media/FramePresenter.cpp
    media/FrameScheduler.cpp
    media/ThumbnailGenerator.cpp
    media/AudioPlaybackService.cpp
    media/AudioPlaybackServiceLoad.cpp
    media/AudioPlaybackServiceCache.cpp
    media/OmniShotDetector.cpp

    # Spine
    spine/SpineEngine.cpp
    spine/SpineAtlas.cpp
    spine/SpineAnimation.cpp
    spine/ModelManager.cpp
    spine/ShotPreset.cpp
    spine/AnimationCache.cpp
    spine/SpinePrerenderer.cpp
    spine/AnimationVideoCache.cpp

    # Media (alpha encoding for pre-rendered animation cache)
    media/VP9AlphaEncoder.cpp
    media/HWAlphaEncoder.cpp
    media/ChromaKeyEncoder.cpp
    media/ProResEncoder.cpp

    # Effects
    effects/EffectStack.cpp
    effects/ColorCorrect.cpp
    effects/ChromaKey.cpp
    effects/Transform2D.cpp
    effects/LUT.cpp
    effects/ColorGrading.cpp
    effects/Ots.cpp
    effects/Flip.cpp
    effects/SimpleEffects.cpp

    # Commands (effects)
    command/commands/EffectCommands.cpp

    # AI
    ai/Transcriber.cpp
    ai/ScriptMatcher.cpp

    # Subtitle
    SrtIO.cpp
)

# /EHa for FrameProducer.cpp only — catch(...) catches SEH exceptions
# (access violations, etc.) so the producer thread survives GPU crashes
# instead of aborting silently via std::terminate.
if(MSVC)
    set_source_files_properties(media/FrameProducer.cpp PROPERTIES COMPILE_FLAGS "/EHa")
endif()

target_include_directories(roundtable_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# PCH — enabled May 3, 2026. Re-testing LNK2011 with current MSVC/CMake.
target_precompile_headers(roundtable_core PRIVATE pch_core.h)

# GPU include path for SpinePrerenderer (uses GPU SpineRenderer for offscreen rendering)
# No link dependency — symbols resolve at final executable link time since both are static libs.
target_include_directories(roundtable_core PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../gpu)

# WIN32_LEAN_AND_MEAN for our code only (not applied globally to avoid breaking PortAudio)
target_compile_definitions(roundtable_core PRIVATE WIN32_LEAN_AND_MEAN)

target_link_libraries(roundtable_core PUBLIC
    spdlog::spdlog
    glm::glm
)

# FFmpeg — needed by media layer (VideoDecoder, FrameCache, MediaPool)
if(FFMPEG_FOUND)
    target_link_libraries(roundtable_core PUBLIC ffmpeg::ffmpeg)
    target_compile_definitions(roundtable_core PUBLIC ROUNDTABLE_HAS_FFMPEG)
endif()

# PortAudio — real-time audio output (WASAPI)
if(PORTAUDIO_FOUND)
    target_link_libraries(roundtable_core PUBLIC ${PORTAUDIO_TARGET})
    target_compile_definitions(roundtable_core PUBLIC ROUNDTABLE_HAS_PORTAUDIO)
endif()

# libsndfile — audio file I/O (WAV, FLAC, AIFF, etc.)
if(SNDFILE_FOUND)
    target_link_libraries(roundtable_core PUBLIC sndfile)
    target_compile_definitions(roundtable_core PUBLIC ROUNDTABLE_HAS_SNDFILE)
endif()

# spine-cpp — Spine 4.1 C++ runtime (skeleton loading, animation, mesh extraction)
if(SPINE_CPP_FOUND)
    target_link_libraries(roundtable_core PUBLIC spine-cpp)
    target_compile_definitions(roundtable_core PUBLIC ROUNDTABLE_HAS_SPINE)

    # SpinePrerenderer needs GPU headers (Vulkan, VMA, stb) for its GPU path.
    # These are PRIVATE — only needed to compile SpinePrerenderer.cpp.
    # Actual symbol resolution happens at final exe link time when both
    # roundtable_core and roundtable_gpu are linked together.
    if(TARGET Vulkan::Headers)
        target_link_libraries(roundtable_core PRIVATE Vulkan::Headers)
    endif()
    if(TARGET volk::volk)
        target_link_libraries(roundtable_core PRIVATE volk::volk)
    endif()
    if(TARGET VulkanMemoryAllocator)
        target_link_libraries(roundtable_core PRIVATE VulkanMemoryAllocator)
    elseif(TARGET GPUOpen::VulkanMemoryAllocator)
        target_link_libraries(roundtable_core PRIVATE GPUOpen::VulkanMemoryAllocator)
    endif()
    if(TARGET stb)
        target_link_libraries(roundtable_core PRIVATE stb)
    endif()
endif()

# whisper.cpp — speech-to-text transcription
if(WHISPER_FOUND)
    target_link_libraries(roundtable_core PUBLIC whisper)
    target_compile_definitions(roundtable_core PUBLIC ROUNDTABLE_HAS_WHISPER)
endif()

# SoundTouch — pitch-preserving time-stretch
if(SOUNDTOUCH_FOUND)
    target_link_libraries(roundtable_core PUBLIC SoundTouch)
    target_compile_definitions(roundtable_core PUBLIC ROUNDTABLE_HAS_SOUNDTOUCH)
endif()

# dbghelp — Windows crash dump (MiniDumpWriteDump)
if(WIN32)
    target_link_libraries(roundtable_core PRIVATE dbghelp)
endif()
