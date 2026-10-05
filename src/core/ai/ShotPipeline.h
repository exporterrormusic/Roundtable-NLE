/*
 * ShotPipeline — data + pure logic for the green-screen → ComfyUI (SCAIL)
 * show pipeline: per-recording WIDE / CLOSE-UP framing, per-shot settings,
 * and the ffmpeg command that exports one framed 1080p shot.
 *
 * Framing uses Premiere-style numbers so they match the owner's old manual
 * workflow: `scale` is the source's size relative to its native pixels
 * (0.5 = a 3840-wide recording exactly fills a 1920-wide frame), and the
 * centre is where in the SOURCE the output frame is centred (0..1).
 * Anything outside the source is filled with pure green so it keys out with
 * the rest of the background.
 */

#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace rt {

class BinaryWriter;
class BinaryReader;

/// Frame rate the SCAIL workflow drives at unless a shot is switched to 12.
inline constexpr int kScailDefaultFps = 16;
inline constexpr int kScailReducedFps = 12;

struct ShotFraming
{
    double scale{0.5};
    double centerX{0.5};
    double centerY{0.5};

    bool operator==(const ShotFraming&) const = default;
};

/// Default WIDE / CLOSE-UP framing for one source recording.
struct RecordingFraming
{
    // 65 % = the owner's Premiere WIDE ("W65"); 50 % (whole 4K frame) left
    // the subject ~30 % smaller than the workflow was tuned for.
    ShotFraming wide{0.65, 0.5, 0.5};
    ShotFraming closeUp{0.9, 0.5, 0.4};

    bool operator==(const RecordingFraming&) const = default;
};

/// One ComfyUI render of a shot. Every render is kept so a re-roll never
/// throws away a take the owner liked better.
struct RenderVersion
{
    std::string file;            ///< rendered clip on disk (UTF-8 path)
    int64_t     seed{-1};
    std::string expression;
    int         fps{0};
    double      tailPadSec{0.0}; ///< source padding this render includes (trim on import)
    int64_t     renderedAt{0};   ///< unix seconds
    // Manual in/out on the render (seconds in the rendered file), set on the
    // SHOTS page. trimOut < 0 = automatic: the file's end minus tailPadSec.
    double      trimIn{0.0};
    double      trimOut{-1.0};

    /// Out point used on the timeline for a file `durationSec` long.
    [[nodiscard]] double effectiveOut(double durationSec) const
    {
        const double out = trimOut >= 0.0 ? trimOut : durationSec - tailPadSec;
        return out > trimIn ? out : durationSec;
    }

    bool operator==(const RenderVersion&) const = default;
};

/// Per-shot (per script line) settings. Unset values fall back to the
/// recording / pipeline defaults.
struct ShotSettings
{
    bool        hasFraming{false};   ///< true → `framing` overrides the recording default
    ShotFraming framing;
    int         padFrames{-1};       ///< -1 → ShowPipelineState::defaultPadFrames
    std::string expression;          ///< empty → script ACTION → default expression
    int         fps{0};              ///< 0 → kScailDefaultFps; else 12 or 16

    // Last export (what import must trim back off).
    std::string exportedFile;
    double      exportedTailPadSec{0.0};

    int64_t                    seed{-1};        ///< -1 → ShowPipelineState::seed
    std::vector<RenderVersion> renders;
    int                        chosenRender{-1}; ///< index into renders; -1 → newest

    /// The render the timeline should use (chosen, else newest), or nullptr.
    [[nodiscard]] const RenderVersion* activeRender() const;

    bool operator==(const ShotSettings&) const = default;
};

struct ShowPipelineState
{
    std::map<std::string, RecordingFraming> recordings;  ///< key: source path (UTF-8)
    std::map<std::string, ShotSettings>     shots;       ///< key: ScriptMatcher::normalize(dialogue)
    int         defaultPadFrames{0};
    std::string defaultExpression;
    int         frameCapThreshold{120}; ///< frames @16 fps before suggesting 12; 0 = no check
    std::string outputDir;

    // ComfyUI render settings (per show)
    std::string workflowFile;           ///< editor-format workflow .json (e.g. SCAIL 2 FINAL V10)
    std::string wideReferenceImage;
    std::string closeUpReferenceImage;
    int64_t     seed{-1};               ///< -1 → the workflow's own seed

    // Timeline assembly (per show)
    std::string background;             ///< video/image under every shot (e.g. "bkg loop.mp4")
    double      closeUpBackgroundZoom{0.0}; ///< background zoom under CLOSE-UPs; 0 = auto
                                            ///< (the recording's CLOSE-UP / WIDE framing scale)
    double      segmentGapSec{3.0};     ///< room left for the stinger between segments

    bool operator==(const ShowPipelineState&) const = default;

    /// Framing for a shot: its own override, else its recording's WIDE or
    /// CLOSE-UP default, else the built-in default.
    [[nodiscard]] ShotFraming framingFor(const std::string& shotKey,
                                         const std::string& sourceFile,
                                         bool closeUp) const;
    /// Tail padding for a shot, in frames at the shot's render fps.
    [[nodiscard]] int padFramesFor(const std::string& shotKey) const;
    /// Render fps for a shot (16 unless switched to 12).
    [[nodiscard]] int fpsFor(const std::string& shotKey) const;
    /// Seed for a shot: its re-roll seed, else the show seed, else -1 (workflow's).
    [[nodiscard]] int64_t seedFor(const std::string& shotKey) const;
    /// Frames SCAIL will generate for a take of `durationSec` (incl. tail pad).
    [[nodiscard]] int renderFramesFor(const std::string& shotKey, double durationSec) const;

    void write(BinaryWriter& w) const;
    /// Reads a block written by write(); unknown newer fields are skipped.
    void read(BinaryReader& r);
};

/// Where the output frame sits after scaling the source and padding it by one
/// output frame on every side (so any centre is reachable).
struct CropPlacement
{
    int scaledW{0};
    int scaledH{0};
    int cropX{0};   ///< in the padded image
    int cropY{0};
};

[[nodiscard]] CropPlacement computeCropPlacement(int srcW, int srcH, const ShotFraming& framing,
                                                 int outW = 1920, int outH = 1080);

/// One framed shot to export from a source recording.
struct ShotExportJob
{
    std::string sourceFile;
    double      start{0.0};        ///< seconds in the source
    double      end{0.0};
    double      tailPadSec{0.0};   ///< extra source time after `end`
    int         srcW{3840};
    int         srcH{2160};
    ShotFraming framing;
    int         outW{1920};
    int         outH{1080};
    std::string outputFile;
    std::string videoEncoder{"h264_nvenc"};
};

/// ffmpeg arguments (without the executable) for one shot.
[[nodiscard]] std::vector<std::string> buildShotExportArgs(const ShotExportJob& job);

/// Everything that decides an exported shot's pixels: source identity (path,
/// size, mtime), in/out, tail pad, source size, framing, output size and
/// path.  Equal signature = the existing export can be reused.  The encoder
/// is deliberately excluded (an NVENC → software fallback changes nothing).
[[nodiscard]] std::string shotExportSignature(const ShotExportJob& job, int64_t sourceBytes,
                                              int64_t sourceModifiedMs);

// ─── Timeline assembly ──────────────────────────────────────────────────────

/// Scale (relative to the compositor's default "cover" fit) and vertical
/// offset that make a render fill the output WIDTH and sit on the bottom edge
/// (the channel-URL bar covers any gap at the top).
struct FillWidthPlacement
{
    float scale{1.0f};
    float posYPx{0.0f};   ///< pixels from centre, + = down
};
[[nodiscard]] FillWidthPlacement fillWidthBottom(int srcW, int srcH, int outW = 1920, int outH = 1080);

/// One rendered shot to lay out.
struct AssemblyShot
{
    std::string segment;
    bool        closeUp{false};
    double      renderDurationSec{0.0};   ///< whole rendered file
    double      tailPadSec{0.0};          ///< trimmed off the end (when renderOutSec < 0)
    double      renderInSec{0.0};         ///< manual in point on the render
    double      renderOutSec{-1.0};       ///< manual out point; < 0 = duration − tailPadSec
};

struct AssemblyPlan
{
    struct Shot       { size_t index; int64_t timelineIn; int64_t duration; };
    struct Background { bool closeUp; size_t firstShot; int64_t timelineIn; int64_t duration; int64_t sourceIn; };
    struct Marker     { int64_t time; std::string label; };

    std::vector<Shot>       shots;
    std::vector<Background> backgrounds;
    std::vector<Marker>     markers;
    int64_t                 end{0};
};

/// Lay the shots end to end from `startTick` (ticks = kTicksPerSecond):
/// a `segmentGapSec` gap (for the stinger) before every segment after the
/// first, a marker at each segment start, and one background clip per run of
/// same-type shots (split where the loop wraps). The background plays as ONE
/// continuous loop from `startTick`, so cutting WIDE/CLOSE-UP never jumps it
/// in time. `backgroundSec` <= 0 means a still image.
[[nodiscard]] AssemblyPlan planAssembly(const std::vector<AssemblyShot>& shots, int64_t startTick,
                                        double segmentGapSec, double backgroundSec);

/// "007_STORY_CU_L12.mp4" — sortable, readable, unique per line.
[[nodiscard]] std::string shotFileName(int order, const std::string& segment,
                                       const std::string& shot, int lineNumber);

} // namespace rt
