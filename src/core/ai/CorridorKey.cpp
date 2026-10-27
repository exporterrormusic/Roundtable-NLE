/*
 * CorridorKey.cpp — see CorridorKey.h.
 */

#include "ai/CorridorKey.h"

#include "Constants.h"
#include "PathUtils.h"
#include "decode/VideoFrameMapping.h"
#include "effects/Effect.h"
#include "timeline/Track.h"
#include "timeline/Transition.h"
#include "timeline/VideoClip.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <numeric>
#include <sstream>

namespace rt::corridorkey {

namespace {

std::string trim(std::string_view text)
{
    size_t begin = 0, end = text.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(text[begin]))) ++begin;
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) --end;
    return std::string(text.substr(begin, end - begin));
}

template <typename T>
bool parseNumber(std::string_view text, T& out)
{
    const std::string clean = trim(text);
    if constexpr (std::is_floating_point_v<T>) {
        char* end = nullptr;
        const double value = std::strtod(clean.c_str(), &end);
        if (end == clean.c_str()) return false;
        out = static_cast<T>(value);
        return true;
    } else {
        const auto [ptr, ec] = std::from_chars(clean.data(), clean.data() + clean.size(), out);
        return ec == std::errc{} && ptr != clean.data();
    }
}

bool fileExists(const fs::path& path)
{
    std::error_code error;
    return fs::exists(path, error);
}

std::string number(double value, int decimals)
{
    char buffer[64];
    std::snprintf(buffer, sizeof buffer, "%.*f", decimals, value);
    return buffer;
}

uint64_t fnv1a(std::string_view text, uint64_t hash = 1469598103934665603ull)
{
    for (unsigned char c : text) {
        hash ^= c;
        hash *= 1099511628211ull;
    }
    return hash;
}

} // namespace

// ── Install layout ───────────────────────────────────────────────────────────

Layout layoutFor(const fs::path& appRoot)
{
    return {appRoot / "third_party" / "corridorkey",
            appRoot / "tools" / "corridorkey",
            appRoot / "runtimes" / "corridorkey"};
}

std::vector<fs::path> modelFiles(const Layout& layout)
{
    const fs::path models = layout.modelsDir();
    return {models / "weights" / "corridorkey_v2.safetensors",
            models / "weights" / "foundation_stats.safetensors",
            models / "vfms" / "cradio" / "C-RADIOv4-SO400M" / "config.json",
            models / "vfms" / "cradio" / "C-RADIOv4-SO400M" / "model.safetensors",
            models / "vfms" / "moge" / "model.pt",
            models / "vfms" / "rvm" / "pretrain_rvm_small16_256_204031069.npz"};
}

std::string stampText()
{
    return "setup=" + std::string(kSetupVersion) + "\ncode=" + std::string(kCodeCommit) + "\n";
}

bool isInstalled(const Layout& layout)
{
    std::ifstream stamp(layout.stampFile());
    std::string firstLine;
    if (!stamp || !std::getline(stamp, firstLine)) return false;
    if (trim(firstLine) != "setup=" + std::string(kSetupVersion)) return false;
    if (!fileExists(layout.venvPython()) || !fileExists(layout.worker())
        || !fileExists(layout.codeDir / "corridorkey" / "runtime.py"))
        return false;
    for (const auto& file : modelFiles(layout))
        if (!fileExists(file)) return false;
    return true;
}

// ── GPU check ────────────────────────────────────────────────────────────────

std::optional<GpuInfo> parseNvidiaSmiLine(std::string_view line)
{
    std::vector<std::string> fields;
    size_t start = 0;
    for (;;) {
        const size_t comma = line.find(',', start);
        fields.push_back(trim(line.substr(start, comma == std::string_view::npos
                                                     ? std::string_view::npos : comma - start)));
        if (comma == std::string_view::npos) break;
        start = comma + 1;
    }
    if (fields.size() < 4 || fields[0].empty()) return std::nullopt;
    GpuInfo gpu;
    gpu.name = fields[0];
    double driver = 0.0, memoryMiB = 0.0;
    if (!parseNumber(fields[1], gpu.computeCapability) || !parseNumber(fields[2], driver)
        || !parseNumber(fields[3], memoryMiB))
        return std::nullopt;
    gpu.driverMajor = static_cast<int>(driver);
    gpu.memoryGB = memoryMiB / 1024.0;
    return gpu;
}

std::string gpuProblem(const GpuInfo& gpu)
{
    if (gpu.computeCapability < 8.0)
        return "CorridorKey needs an NVIDIA RTX 30-series or newer graphics card. This computer has "
               + gpu.name + ".";
    if (gpu.memoryGB < 6.0)
        return "CorridorKey needs a graphics card with at least 6 GB of memory. " + gpu.name
               + " has " + number(gpu.memoryGB, 1) + " GB.";
    if (gpu.driverMajor < 570)
        return "CorridorKey needs NVIDIA driver 570 or newer (this computer has "
               + std::to_string(gpu.driverMajor) + "). Update the driver from nvidia.com, then try again.";
    return {};
}

double vramCapGB(double totalGB, Steadiness steadiness)
{
    const double cap = steadiness == Steadiness::Steadier ? std::clamp(totalGB * 0.65, 4.0, 16.0)
                                                          : std::clamp(totalGB * 0.35, 4.0, 8.0);
    return std::min(cap, std::max(1.0, totalGB - 2.0));
}

// ── Render planning ──────────────────────────────────────────────────────────

FrameRate exactFrameRate(double fps)
{
    if (!(fps > 0.0)) return {24, 1};
    for (int64_t den : {int64_t(1), int64_t(1001), int64_t(2), int64_t(4), int64_t(1000)}) {
        const int64_t num = std::llround(fps * double(den));
        if (num > 0 && std::abs(double(num) / double(den) - fps) < 1e-6) {
            const int64_t g = std::gcd(num, den);
            return {num / g, den / g};
        }
    }
    int64_t num = std::llround(fps * 1e6), den = 1'000'000;
    const int64_t g = std::gcd(num, den);
    return {num / g, den / g};
}

int64_t tickAlignedFrameStep(FrameRate rate)
{
    const int64_t ticksNum = kTicksPerSecond * rate.den;   // ticks per frame = ticksNum / num
    const int64_t step = rate.num / std::gcd(rate.num, ticksNum);
    return step > 0 && step <= 1000 ? step : 1;
}

int64_t framesToTicks(int64_t frames, FrameRate rate)
{
    const int64_t scaled = frames * kTicksPerSecond * rate.den;
    return (scaled + rate.num / 2) / rate.num;
}

Coverage transitionCoverage(const Track& track, const Clip& clip)
{
    Coverage coverage;
    for (const Transition& t : track.transitions()) {
        if (t.leftClipId == 0 || t.rightClipId == 0) continue;   // fades stay inside the clip
        int64_t start = 0, end = 0;
        t.getRange(start, end);
        if (t.rightClipId == clip.id())
            coverage.lead = std::max(coverage.lead, clip.timelineIn() - start);
        if (t.leftClipId == clip.id())
            coverage.tail = std::max(coverage.tail, end - clip.timelineOut());
    }
    return coverage;
}

namespace {

/// Every tick the clip is shown at, once per sequence frame, plus the last.
template <typename Fn>
void forEachShownTick(const VideoClip& clip, Coverage coverage, int64_t frameTicks, Fn&& fn)
{
    const int64_t first = clip.timelineIn() - std::max<int64_t>(0, coverage.lead);
    const int64_t end = clip.timelineOut() + std::max<int64_t>(0, coverage.tail);
    for (int64_t tick = first; tick < end; tick += std::max<int64_t>(1, frameTicks)) fn(tick);
    fn(std::max(first, end - 1));
}

} // namespace

Rect keyRectFor(const VideoClip& clip, int W, int H)
{
    // What the crop leaves visible plus a margin; even edges keep 4:2:0
    // chroma aligned.
    const int margin = 16;
    int x0 = static_cast<int>(std::floor(W * clip.cropLeft() / 100.0)) - margin;
    int x1 = static_cast<int>(std::ceil(W * (1.0 - clip.cropRight() / 100.0))) + margin;
    int y0 = static_cast<int>(std::floor(H * clip.cropTop() / 100.0)) - margin;
    int y1 = static_cast<int>(std::ceil(H * (1.0 - clip.cropBottom() / 100.0))) + margin;
    x0 = std::clamp(x0, 0, W) & ~1;
    y0 = std::clamp(y0, 0, H) & ~1;
    x1 = std::clamp(x1 + (x1 & 1), 0, W);
    y1 = std::clamp(y1 + (y1 & 1), 0, H);
    if (x1 - x0 < 64 || y1 - y0 < 64) return {};
    return {x0, y0, x1 - x0, y1 - y0};
}

bool renderCovers(const VideoClip& clip, const RenderRecord& record, Coverage coverage,
                  int64_t frameTicks)
{
    const int64_t frames = record.lastFrame - record.firstFrame + 1;
    bool covered = true;
    forEachShownTick(clip, coverage, frameTicks, [&](int64_t tick) {
        if (unclampedSourceTick(clip, tick) < 0) covered = false;   // before the render's first frame
        const MappedSourceFrame m = mapTickToSourceFrame(clip, tick, nullptr);
        if (m.lowerFrame < 0 || m.upperFrame > frames - 1) covered = false;
    });
    if (record.sourceWidth > 0 && record.sourceHeight > 0) {
        const Rect need = keyRectFor(clip, record.sourceWidth, record.sourceHeight);
        const Rect& have = record.keyRect;
        if (need.w == 0 || need.x < have.x || need.y < have.y || need.x + need.w > have.x + have.w
            || need.y + need.h > have.y + have.h)
            covered = false;
    }
    return covered;
}

PlanResult planRender(const VideoClip& clip, const VideoStreamInfo& info,
                      int64_t frameTicks, Coverage coverage)
{
    PlanResult result;
    auto fail = [&](std::string why) { result.problem = std::move(why); return result; };

    if (clip.mediaPath().empty()) return fail("This clip has no media file.");
    if (clip.isVideoCharacter()) return fail("CorridorKey renders camera footage, not character clips.");
    if (info.frameCount <= 1) return fail("CorridorKey renders video clips; this clip is a still image.");
    if (info.hasAlpha || info.packedAlpha)
        return fail("This clip already has transparency, so there is nothing to key.");
    if (info.rotation != 0)
        return fail("Rotated (portrait phone) footage is not supported yet.");
    if (info.isVFR)
        return fail("This footage has a variable frame rate. Convert it to a constant frame rate first.");
    const ColorConversion color = resolveColorConversion(info);
    if (color.isHdr) return fail("HDR footage is not supported yet.");
    if (info.width < 64 || info.height < 64) return fail("The footage is too small to key.");

    RenderPlan plan;
    plan.sourcePath = clip.mediaPath();
    plan.fps = exactFrameRate(clip.sourceFps() > 0.0 ? clip.sourceFps() : info.fps);
    plan.sourceWidth = static_cast<int>(info.width);
    plan.sourceHeight = static_cast<int>(info.height);
    plan.matrix = color.matrix;
    plan.fullRange = color.fullRange;

    // Every source frame the clip shows, transitions across its cuts
    // included, sampled once per sequence frame.
    int64_t lo = INT64_MAX, hi = INT64_MIN;
    forEachShownTick(clip, coverage, frameTicks, [&](int64_t tick) {
        const MappedSourceFrame m = mapTickToSourceFrame(clip, tick, &info);
        lo = std::min(lo, m.lowerFrame);
        hi = std::max(hi, m.upperFrame);
    });
    if (lo > hi) return fail("The clip is empty.");

    // Start on a whole number of ticks so the clip's in point shifts onto
    // the render exactly (at 29.97 that can start up to 4 frames early).
    const int64_t align = tickAlignedFrameStep(plan.fps);
    plan.firstFrame = std::max<int64_t>(0, lo) / align * align;
    plan.lastFrame = std::min<int64_t>(info.frameCount - 1, hi);
    plan.tickShift = framesToTicks(plan.firstFrame, plan.fps);

    // Key only what the crop leaves visible; the rest of the render is transparent.
    plan.keyRect = keyRectFor(clip, plan.sourceWidth, plan.sourceHeight);
    if (plan.keyRect.w == 0) return fail("The clip is cropped to almost nothing.");

    result.plan = plan;
    return result;
}

std::string renderKey(const RenderPlan& plan, uintmax_t sourceBytes, int64_t sourceModified)
{
    std::ostringstream text;
    text << plan.sourcePath << '|' << sourceBytes << '|' << sourceModified << '|'
         << plan.firstFrame << '|' << plan.lastFrame << '|' << plan.fps.num << '/' << plan.fps.den
         << '|' << plan.keyRect.x << ',' << plan.keyRect.y << ',' << plan.keyRect.w << ','
         << plan.keyRect.h << '|' << static_cast<int>(plan.matrix) << plan.fullRange << '|'
         << static_cast<int>(plan.steadiness) << '|' << kSetupVersion << '|' << kRenderFormat;
    char hex[17];
    std::snprintf(hex, sizeof hex, "%016llx", static_cast<unsigned long long>(fnv1a(text.str())));
    return std::string(hex, 8);
}

std::string renderFileName(const RenderPlan& plan, std::string_view key)
{
    std::string stem = pathToUtf8(utf8ToPath(plan.sourcePath).stem());
    for (char& c : stem)
        if (std::string_view("<>:\"/\\|?*").find(c) != std::string_view::npos) c = '_';
    return stem + " CK " + std::to_string(plan.firstFrame) + "-" + std::to_string(plan.lastFrame)
           + " " + std::string(key) + ".mov";
}

std::vector<std::string> extractArgs(const RenderPlan& plan, const fs::path& framesDir)
{
    // The app shows source frame f as the first picture at or after
    // (f - 1/2)/fps in the stream's own timestamps (VideoDecoder::
    // decodeForwardTo) — not counted from the file's first picture, which
    // often sits a frame in. The fps filter on those same timestamps
    // (-copyts) puts each picture in slot round(pts * fps), which is that
    // rule, and trim keeps slots first..last: render frame k is exactly what
    // the app shows as source frame first + k.
    const std::string rate = std::to_string(plan.fps.num) + "/" + std::to_string(plan.fps.den);
    std::vector<std::string> args{"-hide_banner", "-loglevel", "error", "-nostdin", "-y", "-copyts"};
    std::string fps = "fps=" + rate + ":round=near";
    if (plan.firstFrame > 3) {
        // A few frames early, in stream time, so the grid has room to settle.
        const double seconds = double(plan.firstFrame - 3) * double(plan.fps.den) / double(plan.fps.num);
        args.insert(args.end(), {"-seek_timestamp", "1", "-ss", number(seconds, 6)});
    } else {
        fps += ":start_time=0";   // slots before the first picture repeat it, as the app does
    }
    const char* matrix = plan.matrix == ColorMatrix::BT601    ? "bt601"
                       : plan.matrix == ColorMatrix::BT2020 ? "bt2020"
                                                            : "bt709";
    std::string filters = fps + ",trim=start_pts=" + std::to_string(plan.firstFrame)
                          + ":end_pts=" + std::to_string(plan.lastFrame + 1) + ",";
    const Rect& r = plan.keyRect;
    if (r.x != 0 || r.y != 0 || r.w != plan.sourceWidth || r.h != plan.sourceHeight)
        filters += "crop=" + std::to_string(r.w) + ":" + std::to_string(r.h) + ":"
                   + std::to_string(r.x) + ":" + std::to_string(r.y) + ",";
    filters += std::string("scale=in_color_matrix=") + matrix
               + ":in_range=" + (plan.fullRange ? "pc" : "tv") + ":out_range=pc,format=rgb24";
    args.insert(args.end(), {"-i", plan.sourcePath, "-map", "0:v:0", "-vf", filters,
                             "-fps_mode", "passthrough", "-compression_level", "1",
                             "-start_number", "0", pathToUtf8(framesDir / "%06d.png")});
    return args;
}

std::vector<std::string> workerArgs(const Layout& layout, const RenderPlan& plan,
                                    const fs::path& framesDir, const fs::path& output,
                                    const fs::path& tempDir, const fs::path& ffmpeg, double vramGB)
{
    return {"-I", "-u", pathToUtf8(layout.worker()),
            "--code", pathToUtf8(layout.codeDir),
            "--models", pathToUtf8(layout.modelsDir()),
            "--frames", pathToUtf8(framesDir),
            "--fps-num", std::to_string(plan.fps.num),
            "--fps-den", std::to_string(plan.fps.den),
            "--canvas", std::to_string(plan.sourceWidth) + "x" + std::to_string(plan.sourceHeight),
            "--offset", std::to_string(plan.keyRect.x) + "," + std::to_string(plan.keyRect.y),
            "--output", pathToUtf8(output),
            "--temp", pathToUtf8(tempDir),
            "--ffmpeg", pathToUtf8(ffmpeg),
            "--vram-gb", number(vramGB, 1)};
}

double estimateSeconds(const RenderPlan& plan, double secondsPerMegapixelFrame)
{
    const double megapixelFrames = double(plan.frameCount()) * plan.keyedMegapixels();
    return 25.0 + megapixelFrames * (secondsPerMegapixelFrame + 0.02);
}

std::string formatDuration(double seconds)
{
    if (seconds < 90.0) return "about " + std::to_string(std::max(5, int(std::ceil(seconds / 5.0)) * 5)) + " s";
    if (seconds < 90.0 * 60.0) return "about " + std::to_string(int(std::ceil(seconds / 60.0))) + " min";
    return "about " + number(seconds / 3600.0, 1) + " h";
}

// ── Worker protocol ──────────────────────────────────────────────────────────

WorkerEvent parseWorkerLine(std::string_view line)
{
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.remove_suffix(1);
    WorkerEvent event;
    auto rest = [&](std::string_view prefix) { return trim(line.substr(prefix.size())); };
    if (line.starts_with("RT_PROGRESS ")) {
        const std::string body = rest("RT_PROGRESS ");
        const size_t space = body.find(' ');
        if (!parseNumber(std::string_view(body).substr(0, space), event.progress)) {
            event.text = std::string(line);
            return event;
        }
        event.kind = WorkerEvent::Kind::Progress;
        event.progress = std::clamp(event.progress, 0.0, 1.0);
        event.text = space == std::string::npos ? std::string() : trim(std::string_view(body).substr(space));
    } else if (line.starts_with("RT_STATUS ")) {
        event.kind = WorkerEvent::Kind::Status;
        event.text = rest("RT_STATUS ");
    } else if (line.starts_with("RT_DONE ")) {
        event.kind = WorkerEvent::Kind::Done;
        event.text = rest("RT_DONE ");
    } else if (line.starts_with("RT_ERROR ")) {
        event.kind = WorkerEvent::Kind::Error;
        event.text = rest("RT_ERROR ");
    } else {
        event.text = std::string(line);
    }
    return event;
}

// ── The record beside a render ───────────────────────────────────────────────

fs::path recordPathFor(const fs::path& render)
{
    fs::path record = render;
    record += ".corridorkey";
    return record;
}

bool writeRecord(const fs::path& recordFile, const RenderRecord& r, std::string* error)
{
    std::ofstream out(recordFile, std::ios::binary | std::ios::trunc);
    if (!out) {
        if (error) *error = "Could not write " + pathToUtf8(recordFile);
        return false;
    }
    out << "corridorkey-render=1\n"
        << "source=" << r.sourcePath << "\n"
        << "source-codec=" << r.sourceCodec << "\n"
        << "source-duration=" << r.sourceDuration << "\n"
        << "fps=" << r.fps.num << "/" << r.fps.den << "\n"
        << "frames=" << r.firstFrame << "-" << r.lastFrame << "\n"
        << "tick-shift=" << r.tickShift << "\n"
        << "key-rect=" << r.keyRect.x << "," << r.keyRect.y << "," << r.keyRect.w << ","
        << r.keyRect.h << "\n"
        << "source-size=" << r.sourceWidth << "x" << r.sourceHeight << "\n"
        << "steadiness=" << (r.steadiness == Steadiness::Steadier ? "steadier" : "standard") << "\n"
        << "key=" << r.key << "\n"
        << "code=" << r.codeCommit << "\n";
    out.close();
    if (!out) {
        if (error) *error = "Could not write " + pathToUtf8(recordFile);
        return false;
    }
    return true;
}

std::optional<RenderRecord> readRecord(const fs::path& recordFile)
{
    std::ifstream in(recordFile, std::ios::binary);
    if (!in) return std::nullopt;
    RenderRecord r;
    bool header = false, source = false, frames = false;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string_view key = std::string_view(line).substr(0, eq);
        const std::string_view value = std::string_view(line).substr(eq + 1);
        if (key == "corridorkey-render") header = true;
        else if (key == "source") { r.sourcePath = std::string(value); source = !value.empty(); }
        else if (key == "source-codec") r.sourceCodec = std::string(value);
        else if (key == "source-duration") parseNumber(value, r.sourceDuration);
        else if (key == "fps") {
            const size_t slash = value.find('/');
            if (slash != std::string_view::npos) {
                parseNumber(value.substr(0, slash), r.fps.num);
                parseNumber(value.substr(slash + 1), r.fps.den);
            }
        } else if (key == "frames") {
            const size_t dash = value.find('-');
            frames = dash != std::string_view::npos
                     && parseNumber(value.substr(0, dash), r.firstFrame)
                     && parseNumber(value.substr(dash + 1), r.lastFrame);
        } else if (key == "tick-shift") parseNumber(value, r.tickShift);
        else if (key == "key-rect") {
            int v[4]{};
            size_t at = 0;
            for (int i = 0; i < 4; ++i) {
                const size_t comma = value.find(',', at);
                parseNumber(value.substr(at, comma == std::string_view::npos ? value.size() - at : comma - at), v[i]);
                at = comma == std::string_view::npos ? value.size() : comma + 1;
            }
            r.keyRect = {v[0], v[1], v[2], v[3]};
        } else if (key == "source-size") {
            const size_t x = value.find('x');
            if (x != std::string_view::npos) {
                parseNumber(value.substr(0, x), r.sourceWidth);
                parseNumber(value.substr(x + 1), r.sourceHeight);
            }
        } else if (key == "steadiness") {
            r.steadiness = value == "steadier" ? Steadiness::Steadier : Steadiness::Standard;
        } else if (key == "key") r.key = std::string(value);
        else if (key == "code") r.codeCommit = std::string(value);
    }
    if (!header || !source || !frames || r.fps.num <= 0 || r.fps.den <= 0) return std::nullopt;
    return r;
}

std::optional<RenderRecord> recordForMedia(const std::string& mediaPath)
{
    if (mediaPath.empty()) return std::nullopt;
    return readRecord(recordPathFor(utf8ToPath(mediaPath)));
}

RenderRecord makeRecord(const RenderPlan& plan, const VideoClip& clip, std::string_view key)
{
    RenderRecord r;
    r.sourcePath = plan.sourcePath;
    r.sourceCodec = clip.sourceCodecName();
    r.sourceDuration = clip.sourceDuration();
    r.fps = plan.fps;
    r.firstFrame = plan.firstFrame;
    r.lastFrame = plan.lastFrame;
    r.tickShift = plan.tickShift;
    r.keyRect = plan.keyRect;
    r.sourceWidth = plan.sourceWidth;
    r.sourceHeight = plan.sourceHeight;
    r.steadiness = plan.steadiness;
    r.key = std::string(key);
    r.codeCommit = std::string(kCodeCommit);
    return r;
}

// ── The clip swap ────────────────────────────────────────────────────────────

void useRender(VideoClip& clip, const std::string& render, const RenderRecord& record,
               uint64_t effectId)
{
    clip.setMediaPath(render);
    clip.setSourceIn(std::max<int64_t>(0, clip.sourceIn() - record.tickShift));
    clip.setSourceDuration(framesToTicks(record.lastFrame - record.firstFrame + 1, record.fps));
    clip.setSourceCodecName("prores");
    if (Effect* fx = clip.effects().effectById(effectId)) fx->setEnabled(false);
}

void useSource(VideoClip& clip, const RenderRecord& record, uint64_t effectId)
{
    clip.setMediaPath(record.sourcePath);
    clip.setSourceIn(clip.sourceIn() + record.tickShift);
    if (record.sourceDuration > 0) clip.setSourceDuration(record.sourceDuration);
    clip.setSourceCodecName(record.sourceCodec);
    if (Effect* fx = clip.effects().effectById(effectId)) fx->setEnabled(true);
}

} // namespace rt::corridorkey
