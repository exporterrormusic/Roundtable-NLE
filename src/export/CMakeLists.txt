# ─── Export Pipeline Library ──────────────────────────────────────────────────

add_library(roundtable_export STATIC
    Encoder.cpp
    FfmpegEncoderBase.cpp
    Rgba16fPack.cpp
    RenderQueue.cpp
    SmartRenderAnalyzer.cpp
    PacketDemuxer.cpp
    AudioMixdown.cpp
    Muxer.cpp
    formats/H264Encoder.cpp
    formats/H265Encoder.cpp
    formats/AV1Encoder.cpp
    formats/ProResEncoder.cpp
    formats/DNxHREncoder.cpp
    formats/ImageSequence.cpp
)

target_include_directories(roundtable_export PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(roundtable_export PUBLIC
    roundtable_core
    roundtable_gpu
    spdlog::spdlog
)
