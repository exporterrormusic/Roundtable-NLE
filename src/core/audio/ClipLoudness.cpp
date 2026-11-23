#include "audio/ClipLoudness.h"

#include "Constants.h"
#include "audio/AudioFile.h"
#include "audiofx/Loudness.h"
#include "timeline/AudioClip.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace rt {

std::optional<double> measureClipLoudness(const AudioClip& clip)
{
    AudioFile af;
    if (!af.open(clip.mediaPath(), clip.audioStreamIndex())) return std::nullopt;
    const auto& info = af.info();
    if (info.sampleRate == 0 || info.channels == 0) return std::nullopt;

    const double sr  = static_cast<double>(info.sampleRate);
    const int    ch  = static_cast<int>(info.channels);
    const double spd = std::max(std::abs(clip.speed()), 0.01);
    const int64_t start = std::max<int64_t>(
        0, static_cast<int64_t>(static_cast<double>(clip.sourceIn()) * sr / kTicksPerSecond));
    const int64_t count = static_cast<int64_t>(
        std::ceil(static_cast<double>(clip.duration()) * spd * sr / kTicksPerSecond));
    if (count <= 0) return std::nullopt;

    audiofx::FxChain chain = clip.buildProcessingChain();
    chain.prepare(sr, ch);
    const bool process = chain.isActive();
    audiofx::LoudnessMeter meter(sr, ch);

    constexpr int64_t kChunkFrames = 1 << 16;
    std::vector<float> buf;
    for (int64_t pos = start, end = start + count; pos < end;) {
        const int64_t got = af.readRegion(pos, std::min(kChunkFrames, end - pos), buf);
        if (got <= 0) break;
        const int64_t frames = std::min<int64_t>(got, static_cast<int64_t>(buf.size()) / ch);
        if (process) chain.process(buf.data(), static_cast<int>(frames));
        meter.add(buf.data(), frames);
        pos += got;
    }
    return meter.integrated();
}

float autoMatchVolume(double measuredLufs, double targetLufs)
{
    const double db = std::clamp(targetLufs - measuredLufs, -96.0, 24.0);
    return static_cast<float>(std::pow(10.0, db / 20.0));
}

} // namespace rt
