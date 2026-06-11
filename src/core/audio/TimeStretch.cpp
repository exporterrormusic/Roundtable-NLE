/*
 * TimeStretch.cpp — SoundTouch wrapper for real-time pitch-preserving time-stretch.
 *
 * SoundTouch handles all the WSOLA/TDHS internals. We just:
 *   1. Feed source samples at analysis rate (speed * output rate)
 *   2. Receive time-stretched output at the output rate
 *   3. Mix into the additive output buffer with volume/pan
 */

#include "audio/TimeStretch.h"

#ifdef ROUNDTABLE_HAS_SOUNDTOUCH
#include <SoundTouch.h>
#endif

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace rt {

TimeStretch::TimeStretch(uint32_t channels, uint32_t sampleRate)
    : m_channels(channels)
    , m_sampleRate(sampleRate)
{
#ifdef ROUNDTABLE_HAS_SOUNDTOUCH
    m_st = std::make_unique<soundtouch::SoundTouch>();
    m_st->setSampleRate(sampleRate);
    m_st->setChannels(channels);
    m_st->setTempo(1.0);
    m_st->setPitch(1.0);
    m_st->setRate(1.0);
    // Optimize for speech/music
    m_st->setSetting(SETTING_USE_QUICKSEEK, 0);
    m_st->setSetting(SETTING_USE_AA_FILTER, 1);
    m_st->setSetting(SETTING_SEQUENCE_MS, 40);
    m_st->setSetting(SETTING_SEEKWINDOW_MS, 15);
    m_st->setSetting(SETTING_OVERLAP_MS, 8);
#endif
}

TimeStretch::~TimeStretch() = default;

TimeStretch::TimeStretch(TimeStretch&& other) noexcept = default;
TimeStretch& TimeStretch::operator=(TimeStretch&& other) noexcept = default;

void TimeStretch::reset()
{
#ifdef ROUNDTABLE_HAS_SOUNDTOUCH
    if (m_st) m_st->clear();
#endif
    m_readPos = 0.0;
    m_initialized = false;
}

void TimeStretch::setSpeed(double speed)
{
    if (m_speed == speed) return;
    m_speed = speed;

#ifdef ROUNDTABLE_HAS_SOUNDTOUCH
    if (m_st) {
        // SoundTouch tempo = how much faster to play (>1 = faster, <1 = slower)
        // We handle reverse ourselves by feeding samples in reverse order
        const double absSpeed = std::abs(speed);
        m_st->setTempo(absSpeed > 0.01 ? absSpeed : 0.01);
    }
#endif
}

int64_t TimeStretch::process(const float* src, int64_t srcFrames,
                              int64_t srcStart,
                              float* output, unsigned long outFrames,
                              float volume, float pan, uint32_t outChannels,
                              uint32_t srcChannels,
                              const std::function<float(float)>& fadeEnvelope,
                              int64_t totalSrcFrames)
{
    if (!src || srcFrames <= 0 || outFrames == 0)
        return 0;

#ifndef ROUNDTABLE_HAS_SOUNDTOUCH
    // Fallback: simple sample copy without time-stretch
    return 0;
#else
    if (!m_st) return 0;

    const double absSpeed = std::abs(m_speed);
    const bool isReverse = (m_speed < 0.0);

    // --- 1x forward bypass (no processing needed) ---
    if (absSpeed > 0.999 && absSpeed < 1.001 && !isReverse) {
        const float panL = (srcChannels == 1)
            ? std::cos((pan + 1.0f) * 0.25f * 3.14159265f)
            : std::min(1.0f, 1.0f - pan);
        const float panR = (srcChannels == 1)
            ? std::sin((pan + 1.0f) * 0.25f * 3.14159265f)
            : std::min(1.0f, 1.0f + pan);

        for (unsigned long f = 0; f < outFrames; ++f) {
            const int64_t srcFrame = srcStart + static_cast<int64_t>(f);
            if (srcFrame < 0 || srcFrame >= srcFrames) continue;

            float vol = volume;
            if (fadeEnvelope && totalSrcFrames > 0)
                vol *= fadeEnvelope(static_cast<float>(srcFrame) / static_cast<float>(totalSrcFrames));

            const auto si = static_cast<size_t>(srcFrame * srcChannels);
            const auto oi = static_cast<size_t>(f * outChannels);

            if (srcChannels == 1) {
                const float s = src[si] * vol;
                if (outChannels >= 1) output[oi]     += s * panL;
                if (outChannels >= 2) output[oi + 1] += s * panR;
            } else if (srcChannels == 2) {
                if (outChannels >= 1) output[oi]     += src[si]     * vol * panL;
                if (outChannels >= 2) output[oi + 1] += src[si + 1] * vol * panR;
            } else {
                for (uint32_t c = 0; c < srcChannels && c < outChannels; ++c)
                    output[oi + c] += src[si + c] * vol;
            }
        }
        return static_cast<int64_t>(outFrames);
    }

    // --- Initialize on first call (or after reset) ---
    if (!m_initialized) {
        // Don't initialize if the clip isn't in range yet.
        // Initializing with a negative m_readPos would permanently stick
        // because the bounds check prevents feeding, so m_readPos never advances.
        if (srcStart < 0 || srcStart >= srcFrames)
            return 0;
        m_readPos = static_cast<double>(srcStart);
        m_st->clear();
        m_st->setChannels(srcChannels);
        m_st->setTempo(absSpeed > 0.01 ? absSpeed : 0.01);
        m_initialized = true;
    }

    // Pan gains
    const float panL = (srcChannels == 1)
        ? std::cos((pan + 1.0f) * 0.25f * 3.14159265f)
        : std::min(1.0f, 1.0f - pan);
    const float panR = (srcChannels == 1)
        ? std::sin((pan + 1.0f) * 0.25f * 3.14159265f)
        : std::min(1.0f, 1.0f + pan);

    // Scratch buffers (stack-allocated for small blocks)
    float feedBuf[kBlockSize * 2];   // max 2 channels for stack alloc
    float recvBuf[kBlockSize * 2];
    const bool useStackBuf = (srcChannels <= 2);

    // Heap fallback for >2 channels
    std::vector<float> heapFeed, heapRecv;
    if (!useStackBuf) {
        heapFeed.resize(static_cast<size_t>(kBlockSize * srcChannels));
        heapRecv.resize(static_cast<size_t>(kBlockSize * srcChannels));
    }

    float* feed = useStackBuf ? feedBuf : heapFeed.data();
    float* recv = useStackBuf ? recvBuf : heapRecv.data();

    unsigned long outWritten = 0;

    while (outWritten < outFrames) {
        // Try to receive processed samples from SoundTouch
        const unsigned long needed = std::min(
            static_cast<unsigned long>(kBlockSize), outFrames - outWritten);

        unsigned int received = m_st->receiveSamples(
            recv, static_cast<unsigned int>(needed));

        if (received > 0) {
            // Mix received samples into output with vol/pan
            for (unsigned int i = 0; i < received; ++i) {
                const size_t rIdx = static_cast<size_t>(i * srcChannels);
                const size_t oIdx = static_cast<size_t>((outWritten + i) * outChannels);

                // Approximate fade envelope position
                float vol = volume;
                if (fadeEnvelope && totalSrcFrames > 0) {
                    const auto p = static_cast<int64_t>(std::abs(m_readPos));
                    const float normPos = static_cast<float>(
                        std::min(p, totalSrcFrames)) / static_cast<float>(totalSrcFrames);
                    vol *= fadeEnvelope(normPos);
                }

                if (srcChannels == 1) {
                    const float s = recv[rIdx] * vol;
                    if (outChannels >= 1) output[oIdx]     += s * panL;
                    if (outChannels >= 2) output[oIdx + 1] += s * panR;
                } else if (srcChannels == 2) {
                    const float sL = recv[rIdx]     * vol;
                    const float sR = recv[rIdx + 1] * vol;
                    if (outChannels >= 1) output[oIdx]     += sL * panL;
                    if (outChannels >= 2) output[oIdx + 1] += sR * panR;
                } else {
                    for (uint32_t c = 0; c < srcChannels && c < outChannels; ++c)
                        output[oIdx + c] += recv[rIdx + c] * vol;
                }
            }
            outWritten += received;
            continue;
        }

        // Need to feed more source samples to SoundTouch
        const auto readStart = static_cast<int64_t>(std::round(m_readPos));

        // Check bounds
        if (!isReverse && (readStart < 0 || readStart >= srcFrames))
            break;
        if (isReverse && (readStart < 0 || readStart >= srcFrames))
            break;

        // How many frames can we feed?
        int canFeed;
        if (isReverse)
            canFeed = std::min(kBlockSize, static_cast<int>(readStart + 1));
        else
            canFeed = std::min(kBlockSize, static_cast<int>(srcFrames - readStart));

        if (canFeed <= 0) break;

        // Copy source samples into feed buffer (reverse if needed)
        for (int i = 0; i < canFeed; ++i) {
            const int64_t srcIdx = isReverse ? (readStart - i) : (readStart + i);
            const size_t fIdx = static_cast<size_t>(i * srcChannels);

            if (srcIdx >= 0 && srcIdx < srcFrames) {
                const size_t sIdx = static_cast<size_t>(srcIdx * srcChannels);
                for (uint32_t c = 0; c < srcChannels; ++c)
                    feed[fIdx + c] = src[sIdx + c];
            } else {
                for (uint32_t c = 0; c < srcChannels; ++c)
                    feed[fIdx + c] = 0.0f;
            }
        }

        // Feed to SoundTouch
        m_st->putSamples(feed, static_cast<unsigned int>(canFeed));

        // Advance read position
        if (isReverse)
            m_readPos -= canFeed;
        else
            m_readPos += canFeed;
    }

    return static_cast<int64_t>(static_cast<double>(outWritten) * absSpeed);
#endif // ROUNDTABLE_HAS_SOUNDTOUCH
}

} // namespace rt
