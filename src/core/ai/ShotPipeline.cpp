/*
 * ShotPipeline.cpp — see header.
 */
#include "ai/ShotPipeline.h"

#include "Constants.h"
#include "project/BinaryIO.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

namespace rt {

namespace {

constexpr uint32_t kPipelineBlockVersion = 8;   // v8: timeline assembly template

void writeFraming(BinaryWriter& w, const ShotFraming& f)
{
    w.writeF64(f.scale);
    w.writeF64(f.centerX);
    w.writeF64(f.centerY);
}

ShotFraming readFraming(BinaryReader& r)
{
    ShotFraming f;
    f.scale   = r.readF64();
    f.centerX = r.readF64();
    f.centerY = r.readF64();
    if (!(f.scale > 0.01 && f.scale < 20.0)) f.scale = 0.5;   // corrupt → sane
    return f;
}

int evenRound(double v)
{
    return std::max(2, static_cast<int>(std::lround(v / 2.0)) * 2);
}

std::string formatSeconds(double s)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.3f", std::max(0.0, s));
    return buf;
}

} // namespace

// ─── ShotSettings / ShowPipelineState ───────────────────────────────────────

const RenderVersion* ShotSettings::activeRender() const
{
    if (renders.empty()) return nullptr;
    if (chosenRender >= 0 && chosenRender < static_cast<int>(renders.size()))
        return &renders[static_cast<size_t>(chosenRender)];
    return &renders.back();
}

ShotReview ShotSettings::reviewFor(const RenderVersion* render) const
{
    if (review == ShotReview::Retake) return review;
    return render && render->file == reviewedRender ? review : ShotReview::Unreviewed;
}

void ShotSettings::setFreshSeed(uint32_t candidate, int64_t fallbackSeed)
{
    int64_t next = candidate & 0x7fffffffU;
    while (next == seed || next == fallbackSeed ||
           std::any_of(renders.begin(), renders.end(), [next](const RenderVersion& r) { return r.seed == next; }))
        next = (next + 1) & 0x7fffffffLL;
    seed = next;
}

ShotFraming ShowPipelineState::framingFor(const std::string& shotKey,
                                          const std::string& sourceFile,
                                          bool closeUp) const
{
    auto s = shots.find(shotKey);
    if (s != shots.end() && s->second.hasFraming) return s->second.framing;
    auto rec = recordings.find(sourceFile);
    const RecordingFraming defaults = rec != recordings.end() ? rec->second : RecordingFraming{};
    return closeUp ? defaults.closeUp : defaults.wide;
}

int ShowPipelineState::padFramesFor(const std::string& shotKey) const
{
    auto s = shots.find(shotKey);
    if (s != shots.end() && s->second.padFrames >= 0) return s->second.padFrames;
    return std::max(0, defaultPadFrames);
}

int ShowPipelineState::fpsFor(const std::string& shotKey) const
{
    auto s = shots.find(shotKey);
    if (s != shots.end() && s->second.fps == kScailReducedFps) return kScailReducedFps;
    return kScailDefaultFps;
}

int64_t ShowPipelineState::seedFor(const std::string& shotKey) const
{
    auto s = shots.find(shotKey);
    if (s != shots.end() && s->second.seed >= 0) return s->second.seed;
    return seed;
}

int ShowPipelineState::renderFramesFor(const std::string& shotKey, double durationSec) const
{
    const int fps = fpsFor(shotKey);
    return static_cast<int>(std::ceil(std::max(0.0, durationSec) * fps - 1e-6)) + padFramesFor(shotKey);
}

void ShowPipelineState::write(BinaryWriter& w) const
{
    w.writeU32(kPipelineBlockVersion);

    w.writeU32(static_cast<uint32_t>(recordings.size()));
    for (const auto& [path, rec] : recordings) {
        w.writeString(path);
        writeFraming(w, rec.wide);
        writeFraming(w, rec.closeUp);
    }

    w.writeU32(static_cast<uint32_t>(shots.size()));
    for (const auto& [key, s] : shots) {
        w.writeString(key);
        w.writeU8(s.hasFraming ? 1 : 0);
        writeFraming(w, s.framing);
        w.writeI64(s.padFrames);
        w.writeString(s.expression);
        w.writeI64(s.fps);
        w.writeString(s.exportedFile);
        w.writeF64(s.exportedTailPadSec);
        // v2
        w.writeI64(s.seed);
        w.writeU32(static_cast<uint32_t>(s.renders.size()));
        for (const auto& r : s.renders) {
            w.writeString(r.file);
            w.writeI64(r.seed);
            w.writeString(r.expression);
            w.writeI64(r.fps);
            w.writeF64(r.tailPadSec);
            w.writeI64(r.renderedAt);
            // v4
            w.writeF64(r.trimIn);
            w.writeF64(r.trimOut);
        }
        w.writeI64(s.chosenRender);
    }

    w.writeI64(defaultPadFrames);
    w.writeString(defaultExpression);
    w.writeI64(frameCapThreshold);
    w.writeString(outputDir);
    // v2
    w.writeString(workflowFile);
    w.writeString(wideReferenceImage);
    w.writeString(closeUpReferenceImage);
    w.writeI64(seed);
    // v3
    w.writeString(background);
    w.writeF64(closeUpBackgroundZoom);
    w.writeF64(segmentGapSec);
    // Append to preserve the layout of older pipeline blocks.
    w.writeU32(static_cast<uint32_t>(shots.size()));
    for (const auto& [key, s] : shots) {
        w.writeString(key);
        w.writeU8(static_cast<uint8_t>(s.review));
        w.writeString(s.reviewedRender);
    }
    w.writeF64(closeUpBackgroundOffsetX);
    w.writeF64(closeUpBackgroundOffsetY);
    w.writeU32(backgroundFramingRevision);
    w.writeString(timelineTemplate);
}

void ShowPipelineState::read(BinaryReader& r)
{
    *this = ShowPipelineState{};
    const uint32_t version = r.readU32();
    if (version == 0) return;

    const uint32_t recCount = r.readCount();
    for (uint32_t i = 0; i < recCount; ++i) {
        std::string path = r.readString();
        RecordingFraming rec;
        rec.wide    = readFraming(r);
        rec.closeUp = readFraming(r);
        recordings[std::move(path)] = rec;
    }

    const uint32_t shotCount = r.readCount();
    for (uint32_t i = 0; i < shotCount; ++i) {
        std::string key = r.readString();
        ShotSettings s;
        s.hasFraming         = r.readU8() != 0;
        s.framing            = readFraming(r);
        s.padFrames          = static_cast<int>(r.readI64());
        s.expression         = r.readString();
        s.fps                = static_cast<int>(r.readI64());
        s.exportedFile       = r.readString();
        s.exportedTailPadSec = r.readF64();
        if (version >= 2) {
            s.seed = r.readI64();
            const uint32_t renderCount = r.readCount();
            for (uint32_t k = 0; k < renderCount; ++k) {
                RenderVersion rv;
                rv.file       = r.readString();
                rv.seed       = r.readI64();
                rv.expression = r.readString();
                rv.fps        = static_cast<int>(r.readI64());
                rv.tailPadSec = r.readF64();
                rv.renderedAt = r.readI64();
                if (version >= 4) {
                    rv.trimIn  = r.readF64();
                    rv.trimOut = r.readF64();
                }
                s.renders.push_back(std::move(rv));
            }
            s.chosenRender = static_cast<int>(r.readI64());
        }
        shots[std::move(key)] = std::move(s);
    }

    defaultPadFrames  = static_cast<int>(r.readI64());
    defaultExpression = r.readString();
    frameCapThreshold = static_cast<int>(r.readI64());
    outputDir         = r.readString();
    if (version >= 2) {
        workflowFile          = r.readString();
        wideReferenceImage    = r.readString();
        closeUpReferenceImage = r.readString();
        seed                  = r.readI64();
    }
    if (version >= 3) {
        background            = r.readString();
        closeUpBackgroundZoom = r.readF64();
        segmentGapSec         = r.readF64();
    }
    if (version >= 5) {
        const uint32_t count = r.readCount();
        for (uint32_t i = 0; i < count; ++i) {
            const std::string key = r.readString();
            const uint8_t review = r.readU8();
            const std::string file = r.readString();
            if (auto it = shots.find(key); it != shots.end()) {
                it->second.review = review <= static_cast<uint8_t>(ShotReview::Retake)
                    ? static_cast<ShotReview>(review) : ShotReview::Unreviewed;
                it->second.reviewedRender = file;
            }
        }
    }
    if (version >= 6) {
        closeUpBackgroundOffsetX = r.readF64();
        closeUpBackgroundOffsetY = r.readF64();
    }
    if (version >= 7) backgroundFramingRevision = r.readU32();
    if (version >= 8) timelineTemplate = r.readString();
}

// ─── Framing math + export command ──────────────────────────────────────────

CropPlacement computeCropPlacement(int srcW, int srcH, const ShotFraming& framing,
                                   int outW, int outH)
{
    CropPlacement p;
    p.scaledW = evenRound(srcW * framing.scale);
    p.scaledH = evenRound(srcH * framing.scale);
    // The scaled source sits at (outW, outH) inside a canvas padded by one
    // output frame on each side; centre the crop on the framing centre.
    const double cx = outW + framing.centerX * p.scaledW - outW / 2.0;
    const double cy = outH + framing.centerY * p.scaledH - outH / 2.0;
    p.cropX = std::clamp(static_cast<int>(std::lround(cx)), 0, p.scaledW + outW);
    p.cropY = std::clamp(static_cast<int>(std::lround(cy)), 0, p.scaledH + outH);
    return p;
}

std::string shotExportSignature(const ShotExportJob& job, int64_t sourceBytes, int64_t sourceModifiedMs)
{
    // %.17g round-trips doubles exactly; any real edit changes the string.
    char buf[512];
    std::snprintf(buf, sizeof(buf), "v1|%lld|%lld|%.17g|%.17g|%.17g|%dx%d|%.17g|%.17g|%.17g|%dx%d|",
                  static_cast<long long>(sourceBytes), static_cast<long long>(sourceModifiedMs),
                  job.start, job.end, job.tailPadSec, job.srcW, job.srcH,
                  job.framing.scale, job.framing.centerX, job.framing.centerY, job.outW, job.outH);
    return std::string(buf) + job.sourceFile + "|" + job.outputFile;
}

std::vector<std::string> buildShotExportArgs(const ShotExportJob& job)
{
    const CropPlacement p = computeCropPlacement(job.srcW, job.srcH, job.framing, job.outW, job.outH);
    char filter[512];
    std::snprintf(filter, sizeof(filter),
                  "scale=%d:%d:flags=lanczos,"
                  "pad=%d:%d:%d:%d:color=0x00FF00,"
                  "crop=%d:%d:%d:%d,setsar=1,format=yuv420p",
                  p.scaledW, p.scaledH,
                  p.scaledW + 2 * job.outW, p.scaledH + 2 * job.outH, job.outW, job.outH,
                  job.outW, job.outH, p.cropX, p.cropY);

    std::vector<std::string> args{
        "-y", "-hide_banner", "-loglevel", "error", "-nostdin",
        // -ss before -i + re-encode is frame-accurate and avoids decoding
        // the whole recording up to the take.
        "-ss", formatSeconds(job.start),
        "-i", job.sourceFile,
        "-t", formatSeconds(job.end - job.start + job.tailPadSec),
        "-map", "0:v:0", "-map", "0:a:0?",
        "-vf", filter,
        "-c:v", job.videoEncoder,
    };
    if (job.videoEncoder == "h264_nvenc") {
        for (const char* a : {"-preset", "p5", "-rc", "vbr", "-cq", "16", "-b:v", "0"})
            args.emplace_back(a);
    } else {
        for (const char* a : {"-b:v", "20M"}) args.emplace_back(a);
    }
    for (const char* a : {"-c:a", "aac", "-b:a", "192k", "-movflags", "+faststart"})
        args.emplace_back(a);
    args.push_back(job.outputFile);
    return args;
}

FillWidthPlacement fillWidthBottom(int srcW, int srcH, int outW, int outH)
{
    FillWidthPlacement p;
    if (srcW <= 0 || srcH <= 0 || outW <= 0 || outH <= 0) return p;
    const double widthFit = static_cast<double>(outW) / srcW;
    const double coverFit = std::max(widthFit, static_cast<double>(outH) / srcH);
    p.scale = static_cast<float>(widthFit / coverFit);
    const double scaledH = srcH * widthFit;
    p.posYPx = static_cast<float>((outH - scaledH) / 2.0);   // push down to the bottom edge
    return p;
}

AssemblyPlan planAssembly(const std::vector<AssemblyShot>& shots, int64_t startTick,
                          double segmentGapSec, double backgroundSec)
{
    auto ticks = [](double sec) { return static_cast<int64_t>(std::llround(sec * kTicksPerSecond)); };
    AssemblyPlan plan;
    int64_t t = startTick;
    std::string segment;
    const int64_t bgLen = backgroundSec > 0 ? std::max<int64_t>(1, ticks(backgroundSec)) : 0;

    // Every shot restarts the background. Only a shot longer than the source
    // needs additional loop pieces; adjacent shots never share a clip.
    auto addBackground = [&](size_t firstShot, int64_t from, int64_t to) {
        const bool closeUp = shots[firstShot].closeUp;
        while (from < to) {
            int64_t len = to - from;
            int64_t sourceIn = 0;
            if (bgLen > 0) {
                len = std::min(len, bgLen);
            }
            plan.backgrounds.push_back({closeUp, firstShot, from, len, sourceIn});
            from += len;
        }
    };

    for (size_t i = 0; i < shots.size(); ++i) {
        const AssemblyShot& s = shots[i];
        if (i == 0 || s.segment != segment) {
            if (i > 0) t += ticks(std::max(0.0, segmentGapSec));
            segment = s.segment;
            plan.markers.push_back({t, segment});
        }
        const double out = s.renderOutSec >= 0.0 ? s.renderOutSec : s.renderDurationSec - s.tailPadSec;
        const int64_t dur = std::max<int64_t>(1, ticks(out - std::max(0.0, s.renderInSec)));
        plan.shots.push_back({i, t, dur});
        addBackground(i, t, t + dur);
        t += dur;
    }
    plan.end = t;
    return plan;
}

std::string shotFileName(int order, const std::string& segment,
                         const std::string& shot, int lineNumber)
{
    auto clean = [](const std::string& s) {
        std::string out;
        for (unsigned char c : s) {
            if (std::isalnum(c)) out += static_cast<char>(std::toupper(c));
            else if ((c == ' ' || c == '-' || c == '_') && !out.empty() && out.back() != '-')
                out += '-';
        }
        while (!out.empty() && out.back() == '-') out.pop_back();
        return out.empty() ? std::string("X") : out;
    };
    const std::string shotTag = shot == "CLOSE-UP" ? std::string("CU") : clean(shot);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%03d_", order);
    return buf + clean(segment) + "_" + shotTag + "_L" + std::to_string(lineNumber) + ".mp4";
}

} // namespace rt
