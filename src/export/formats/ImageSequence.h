/*
 * ImageSequence — writes individual frames as PNG/BMP/JPEG.
 */

#pragma once

#include "Encoder.h"

#include <filesystem>

namespace rt {

class ImageSequence : public Encoder
{
public:
    ImageSequence();
    ~ImageSequence() override;

    /// Set the output directory (must be called before init).
    void setOutputDirectory(const std::filesystem::path& dir);

    /// Set filename pattern (e.g., "frame_%06d"). Default: "frame_%06d".
    void setFilenamePattern(const std::string& pattern);

    bool init(const EncoderConfig& config) override;
    bool encodeFrame(const uint8_t* rgbaPixels, int64_t frameIndex) override;
    int  flush() override;
    void shutdown() override;

    [[nodiscard]] const EncodedPacket& lastPacket() const override { return m_lastPacket; }
    [[nodiscard]] const std::vector<EncodedPacket>& flushedPackets() const override { return m_flushedPackets; }
    [[nodiscard]] EncoderCodec codec() const noexcept override { return EncoderCodec::ImageSequence; }
    [[nodiscard]] int avCodecId() const noexcept override { return 0; }
    [[nodiscard]] bool isInitialized() const noexcept override { return m_initialized; }
    [[nodiscard]] bool isHardwareAccelerated() const noexcept override { return false; }
    [[nodiscard]] const std::string& lastError() const noexcept override { return m_lastError; }
    [[nodiscard]] int64_t framesEncoded() const noexcept override { return m_framesEncoded; }

private:
    std::filesystem::path      m_outputDir;
    std::string                m_filenamePattern{"frame_%06d"};
    EncoderConfig              m_config;
    EncodedPacket              m_lastPacket;
    std::vector<EncodedPacket> m_flushedPackets;
    std::string                m_lastError;
    int64_t                    m_framesEncoded{0};
    bool                       m_initialized{false};
};

} // namespace rt
