/*
 * Muxer.cpp — FFmpeg-based multiplexer.
 */

#include "Muxer.h"
#include "Encoder.h"
#include "AudioMixdown.h"
#include "PathUtils.h"

#include <spdlog/spdlog.h>

#ifdef ROUNDTABLE_HAS_FFMPEG
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libavutil/channel_layout.h>
#include <libswresample/swresample.h>
}
#endif

namespace rt {

const char* containerFormatName(ContainerFormat fmt) noexcept
{
    switch (fmt) {
        case ContainerFormat::MP4:  return "MP4";
        case ContainerFormat::MOV:  return "MOV";
        case ContainerFormat::MKV:  return "MKV";
        case ContainerFormat::WebM: return "WebM";
        case ContainerFormat::AVI:  return "AVI";
        default:                    return "Unknown";
    }
}

const char* containerFormatExtension(ContainerFormat fmt) noexcept
{
    switch (fmt) {
        case ContainerFormat::MP4:  return ".mp4";
        case ContainerFormat::MOV:  return ".mov";
        case ContainerFormat::MKV:  return ".mkv";
        case ContainerFormat::WebM: return ".webm";
        case ContainerFormat::AVI:  return ".avi";
        default:                    return ".mp4";
    }
}

Muxer::Muxer() = default;

Muxer::~Muxer()
{
    if (m_isOpen) close();
}

#ifdef ROUNDTABLE_HAS_FFMPEG

bool Muxer::open(const MuxerConfig& config, bool deferHeader)
{
    if (m_isOpen) close();
    m_config = config;
    m_videoPtsWritten = 0;
    m_audioPtsWritten = 0;
    m_headerWritten = false;

    // Determine format short name
    const char* fmtName = nullptr;
    switch (config.format) {
        case ContainerFormat::MP4:  fmtName = "mp4"; break;
        case ContainerFormat::MOV:  fmtName = "mov"; break;
        case ContainerFormat::MKV:  fmtName = "matroska"; break;
        case ContainerFormat::WebM: fmtName = "webm"; break;
        case ContainerFormat::AVI:  fmtName = "avi"; break;
        default: fmtName = "mp4"; break;
    }

    int ret = avformat_alloc_output_context2(&m_fmtCtx, nullptr, fmtName,
                                              pathToUtf8(config.outputPath).c_str());
    if (ret < 0 || !m_fmtCtx) {
        m_lastError = "Muxer: Failed to create output context";
        spdlog::error("{}", m_lastError);
        return false;
    }

    // Add video stream
    const AVCodec* vcodec = avcodec_find_encoder(static_cast<AVCodecID>(config.videoCodecId));
    if (!vcodec) vcodec = avcodec_find_encoder(AV_CODEC_ID_H264);

    m_videoStream = avformat_new_stream(m_fmtCtx, vcodec);
    if (!m_videoStream) {
        m_lastError = "Muxer: Failed to create video stream";
        avformat_free_context(m_fmtCtx); m_fmtCtx = nullptr;
        return false;
    }
    m_videoStream->id = 0;
    m_videoStream->time_base = {config.videoFpsDen, config.videoFpsNum};
    m_videoStream->avg_frame_rate = {config.videoFpsNum, config.videoFpsDen};
    m_videoStream->r_frame_rate   = {config.videoFpsNum, config.videoFpsDen};

    auto* vpar = m_videoStream->codecpar;

    if (config.videoCodecContext) {
        // Copy the full opened codec context — crucially this brings
        // extradata (SPS/PPS / hvcC / AV1 seq header), pixel format,
        // profile/level, color metadata and sample aspect ratio into
        // the container. Without this the file plays in lenient
        // demuxers (VLC) but is rejected by strict editors and players.
        int pret = avcodec_parameters_from_context(vpar, config.videoCodecContext);
        if (pret < 0) {
            m_lastError = "Muxer: Failed to copy codec parameters from encoder";
            spdlog::error("{}", m_lastError);
            avformat_free_context(m_fmtCtx); m_fmtCtx = nullptr;
            return false;
        }
        if (config.videoCodecContext->extradata_size <= 0)
            spdlog::warn("Muxer: encoder produced no extradata — strict "
                         "players may still reject the file (check that "
                         "AV_CODEC_FLAG_GLOBAL_HEADER was set before open)");
    } else {
        // Fallback: minimal params (e.g. smart-render passthrough with
        // no live encoder context available).
        vpar->codec_type = AVMEDIA_TYPE_VIDEO;
        vpar->codec_id   = vcodec ? vcodec->id : AV_CODEC_ID_H264;
        vpar->width      = static_cast<int>(config.videoWidth);
        vpar->height     = static_cast<int>(config.videoHeight);
    }

    // Add audio stream if enabled
    if (config.hasAudio) {
        m_audioStream = avformat_new_stream(m_fmtCtx, nullptr);
        if (m_audioStream) {
            m_audioStream->id = 1;
            m_audioStream->time_base = {1, static_cast<int>(config.audioSampleRate)};

            auto* apar = m_audioStream->codecpar;
            apar->codec_type    = AVMEDIA_TYPE_AUDIO;
            apar->codec_id      = AV_CODEC_ID_AAC;
            apar->sample_rate   = static_cast<int>(config.audioSampleRate);
            apar->ch_layout.nb_channels = config.audioChannels;
            apar->bit_rate      = config.audioBitrate;
        }
    }

    // Open output file
    if (!(m_fmtCtx->oformat->flags & AVFMT_NOFILE)) {
        ret = avio_open(&m_fmtCtx->pb, pathToUtf8(config.outputPath).c_str(), AVIO_FLAG_WRITE);
        if (ret < 0) {
            m_lastError = "Muxer: Failed to open output file: " + pathToUtf8(config.outputPath);
            spdlog::error("{}", m_lastError);
            avformat_free_context(m_fmtCtx); m_fmtCtx = nullptr;
            return false;
        }
    }

    m_isOpen = true;

    // Write header (unless caller wants to defer it)
    if (!deferHeader) {
        if (!writeHeader()) return false;
    }

    spdlog::info("Muxer: Opened {} ({})", pathToUtf8(config.outputPath),
                 containerFormatName(config.format));
    return true;
}

bool Muxer::writeHeader()
{
    if (!m_fmtCtx) return false;
    if (m_headerWritten) return true;  // already done

    int ret = avformat_write_header(m_fmtCtx, nullptr);
    if (ret < 0) {
        m_lastError = "Muxer: Failed to write header";
        spdlog::error("{}", m_lastError);
        if (!(m_fmtCtx->oformat->flags & AVFMT_NOFILE)) avio_closep(&m_fmtCtx->pb);
        avformat_free_context(m_fmtCtx); m_fmtCtx = nullptr;
        m_isOpen = false;
        return false;
    }
    m_headerWritten = true;
    spdlog::info("Muxer: Header written — stream time_base={}/{} avg_fps={}/{}",
                 m_videoStream->time_base.num, m_videoStream->time_base.den,
                 m_videoStream->avg_frame_rate.num, m_videoStream->avg_frame_rate.den);
    return true;
}

bool Muxer::writeVideoPacket(const EncodedPacket& packet)
{
    if (!m_isOpen || !m_videoStream) return false;

    AVPacket avpkt;
    memset(&avpkt, 0, sizeof(avpkt));
    avpkt.data     = const_cast<uint8_t*>(packet.data);
    avpkt.size     = packet.size;
    avpkt.pts      = packet.pts;
    avpkt.dts      = packet.dts;
    avpkt.duration = packet.duration;
    avpkt.stream_index = m_videoStream->index;
    if (packet.isKeyframe) avpkt.flags |= AV_PKT_FLAG_KEY;

    av_packet_rescale_ts(&avpkt, {m_config.videoFpsDen, m_config.videoFpsNum},
                          m_videoStream->time_base);

    int ret = av_interleaved_write_frame(m_fmtCtx, &avpkt);
    if (ret < 0) {
        m_lastError = "Muxer: Failed to write video packet";
        return false;
    }

    m_videoPtsWritten = packet.pts;
    return true;
}

// ── Persistent AAC encoder for writeAudioData() ─────────────────────────────

struct Muxer::AudioEncoder {
    AVCodecContext* ctx{nullptr};
    SwrContext*     swr{nullptr};
    AVFrame*        frame{nullptr};
    AVPacket*       pkt{nullptr};
    int64_t         pts{0};
};

void Muxer::initAudioEncoder(uint32_t sampleRate, uint16_t channels)
{
#ifdef ROUNDTABLE_HAS_FFMPEG
    if (m_audioEnc || !m_audioStream) return;

    const AVCodec* acodec = avcodec_find_encoder(AV_CODEC_ID_AAC);
    if (!acodec) { spdlog::error("Muxer: AAC encoder not found"); return; }

    auto* enc = new AudioEncoder;
    enc->ctx = avcodec_alloc_context3(acodec);
    if (!enc->ctx) { delete enc; return; }

    enc->ctx->sample_fmt  = AV_SAMPLE_FMT_FLTP;
    enc->ctx->sample_rate = static_cast<int>(sampleRate);
    enc->ctx->bit_rate    = m_config.audioBitrate > 0 ? m_config.audioBitrate : 192000;
    av_channel_layout_default(&enc->ctx->ch_layout, channels);
    enc->ctx->time_base   = {1, enc->ctx->sample_rate};

    if (m_fmtCtx->oformat->flags & AVFMT_GLOBALHEADER)
        enc->ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    if (avcodec_open2(enc->ctx, acodec, nullptr) < 0) {
        avcodec_free_context(&enc->ctx);
        delete enc;
        spdlog::error("Muxer: Failed to open AAC encoder");
        return;
    }

    avcodec_parameters_from_context(m_audioStream->codecpar, enc->ctx);

    // Resample interleaved float → planar float
    AVChannelLayout srcLayout{}, dstLayout{};
    av_channel_layout_default(&srcLayout, channels);
    av_channel_layout_default(&dstLayout, channels);
    swr_alloc_set_opts2(&enc->swr,
                        &dstLayout, AV_SAMPLE_FMT_FLTP, enc->ctx->sample_rate,
                        &srcLayout, AV_SAMPLE_FMT_FLT,  static_cast<int>(sampleRate),
                        0, nullptr);
    if (!enc->swr || swr_init(enc->swr) < 0) {
        spdlog::error("Muxer: Failed to init SwrContext for writeAudioData");
        avcodec_free_context(&enc->ctx);
        if (enc->swr) swr_free(&enc->swr);
        delete enc;
        return;
    }

    enc->frame = av_frame_alloc();
    enc->frame->format      = AV_SAMPLE_FMT_FLTP;
    enc->frame->sample_rate = enc->ctx->sample_rate;
    av_channel_layout_copy(&enc->frame->ch_layout, &enc->ctx->ch_layout);
    enc->frame->nb_samples  = enc->ctx->frame_size;
    av_frame_get_buffer(enc->frame, 0);

    enc->pkt = av_packet_alloc();
    enc->pts = 0;

    m_audioEnc = enc;
    spdlog::info("Muxer: AAC encoder initialized ({}Hz, {}ch)", sampleRate, channels);
#endif
}

void Muxer::flushAudioEncoder()
{
#ifdef ROUNDTABLE_HAS_FFMPEG
    if (!m_audioEnc || !m_audioEnc->ctx) return;

    avcodec_send_frame(m_audioEnc->ctx, nullptr);
    while (avcodec_receive_packet(m_audioEnc->ctx, m_audioEnc->pkt) == 0) {
        m_audioEnc->pkt->stream_index = m_audioStream->index;
        av_packet_rescale_ts(m_audioEnc->pkt, m_audioEnc->ctx->time_base,
                              m_audioStream->time_base);
        av_interleaved_write_frame(m_fmtCtx, m_audioEnc->pkt);
        av_packet_unref(m_audioEnc->pkt);
    }

    av_packet_free(&m_audioEnc->pkt);
    av_frame_free(&m_audioEnc->frame);
    swr_free(&m_audioEnc->swr);
    avcodec_free_context(&m_audioEnc->ctx);
    delete m_audioEnc;
    m_audioEnc = nullptr;
#endif
}

bool Muxer::writeAudioData(const float* samples, int64_t frameCount,
                            uint32_t sampleRate, uint16_t channels)
{
#ifdef ROUNDTABLE_HAS_FFMPEG
    if (!m_isOpen || !m_audioStream || !samples || frameCount <= 0)
        return false;

    // Lazily init the AAC encoder on first call
    if (!m_audioEnc)
        initAudioEncoder(sampleRate, channels);
    if (!m_audioEnc)
        return false;

    const int frameSize = m_audioEnc->ctx->frame_size;
    int64_t   samplesRead = 0;

    while (samplesRead < frameCount) {
        int64_t remaining = frameCount - samplesRead;
        int     chunk     = static_cast<int>(std::min<int64_t>(remaining, frameSize));

        const uint8_t* srcData[] = {
            reinterpret_cast<const uint8_t*>(samples + samplesRead * channels)
        };
        av_frame_make_writable(m_audioEnc->frame);
        m_audioEnc->frame->nb_samples = chunk;
        swr_convert(m_audioEnc->swr, m_audioEnc->frame->data, chunk,
                    srcData, chunk);

        m_audioEnc->frame->pts = m_audioEnc->pts;
        m_audioEnc->pts += chunk;

        int ret = avcodec_send_frame(m_audioEnc->ctx, m_audioEnc->frame);
        if (ret < 0) {
            m_lastError = "Muxer: AAC encode error";
            return false;
        }

        while (avcodec_receive_packet(m_audioEnc->ctx, m_audioEnc->pkt) == 0) {
            m_audioEnc->pkt->stream_index = m_audioStream->index;
            av_packet_rescale_ts(m_audioEnc->pkt, m_audioEnc->ctx->time_base,
                                  m_audioStream->time_base);
            av_interleaved_write_frame(m_fmtCtx, m_audioEnc->pkt);
            av_packet_unref(m_audioEnc->pkt);
        }

        samplesRead += chunk;
    }

    m_audioPtsWritten = m_audioEnc->pts;
    return true;
#else
    (void)samples; (void)frameCount; (void)sampleRate; (void)channels;
    return true;
#endif
}

bool Muxer::writeAudioPacket(const EncodedPacket& packet)
{
    if (!m_isOpen || !m_audioStream) return false;

    AVPacket avpkt;
    memset(&avpkt, 0, sizeof(avpkt));
    avpkt.data     = const_cast<uint8_t*>(packet.data);
    avpkt.size     = packet.size;
    avpkt.pts      = packet.pts;
    avpkt.dts      = packet.dts;
    avpkt.duration = packet.duration;
    avpkt.stream_index = m_audioStream->index;

    int ret = av_interleaved_write_frame(m_fmtCtx, &avpkt);
    if (ret < 0) {
        m_lastError = "Muxer: Failed to write audio packet";
        return false;
    }

    m_audioPtsWritten = packet.pts;
    return true;
}

bool Muxer::close()
{
    if (!m_isOpen) return true;

    // Flush any pending audio frames from the persistent encoder
    flushAudioEncoder();

    if (m_headerWritten && m_fmtCtx) {
        av_write_trailer(m_fmtCtx);
    }

    if (m_fmtCtx) {
        if (!(m_fmtCtx->oformat->flags & AVFMT_NOFILE)) {
            avio_closep(&m_fmtCtx->pb);
        }
        avformat_free_context(m_fmtCtx);
        m_fmtCtx = nullptr;
    }

    m_videoStream = nullptr;
    m_audioStream = nullptr;
    m_isOpen = false;
    m_headerWritten = false;

    spdlog::info("Muxer: Closed");
    return true;
}

// static
bool Muxer::muxFile(const std::filesystem::path& outputPath,
                     const std::vector<EncodedPacket>& videoPackets,
                     const MixdownResult* audioResult,
                     const MuxerConfig& config)
{
    Muxer mux;
    MuxerConfig cfg = config;
    cfg.outputPath = outputPath;
    cfg.hasAudio = audioResult != nullptr && audioResult->isValid();

    // Open with deferred header so we can set up audio codec params first
    if (!mux.open(cfg, /*deferHeader=*/cfg.hasAudio)) return false;

    // ── Set up the persistent AAC encoder BEFORE writing the header ──────
    // initAudioEncoder() copies codec params (incl. extradata) onto the
    // audio stream, so the deferred header is written with correct params.
    // This is the same encoder that does the actual audio encoding below —
    // no throwaway context.
    if (cfg.hasAudio && mux.m_audioStream && audioResult) {
        mux.initAudioEncoder(audioResult->sampleRate, audioResult->channels);
        if (!mux.m_audioEnc) {
            spdlog::warn("Muxer: Could not init AAC encoder, writing video-only");
            cfg.hasAudio = false;
        }
    }

    // Now write the header with proper codec params
    if (!mux.m_headerWritten) {
        if (!mux.writeHeader()) return false;
    }

    // ── Write video packets ──────────────────────────────────────────────
    for (const auto& pkt : videoPackets) {
        if (!mux.writeVideoPacket(pkt)) {
            spdlog::error("Muxer: Failed at video pts {}", pkt.pts);
            mux.close();
            return false;
        }
    }

    // ── Encode + write audio (encoder already initialized above) ─────────
    if (cfg.hasAudio && mux.m_audioEnc && audioResult) {
        if (!mux.writeAudioData(audioResult->samples.data(),
                                 audioResult->totalFrames,
                                 audioResult->sampleRate,
                                 audioResult->channels)) {
            spdlog::warn("Muxer: Audio encoding had errors but video is intact");
        }
    }

    return mux.close();
}

#else // !ROUNDTABLE_HAS_FFMPEG

bool Muxer::open(const MuxerConfig&, bool) { m_lastError = "FFmpeg not available"; return false; }
bool Muxer::writeHeader() { m_lastError = "FFmpeg not available"; return false; }
bool Muxer::writeVideoPacket(const EncodedPacket&) { return false; }
bool Muxer::writeAudioData(const float*, int64_t, uint32_t, uint16_t) { return false; }
bool Muxer::writeAudioPacket(const EncodedPacket&) { return false; }
bool Muxer::close() { m_isOpen = false; return true; }
bool Muxer::muxFile(const std::filesystem::path&, const std::vector<EncodedPacket>&,
                     const MixdownResult*, const MuxerConfig&) { return false; }

#endif // ROUNDTABLE_HAS_FFMPEG

} // namespace rt
