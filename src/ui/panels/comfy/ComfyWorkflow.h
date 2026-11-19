/*
 * ComfyWorkflow — turns a ComfyUI workflow saved from the editor (the
 * "nodes"/"links" graph format) into the API "prompt" format the /prompt
 * endpoint runs, and fills in one shot's parameters.
 *
 * Conversion rules (matching ComfyUI's own "Export (API)"):
 *   - Notes / markdown nodes and muted nodes are dropped.
 *   - Bypassed nodes are removed and their consumers rewired to the bypassed
 *     node's same-typed input (Reroutes are followed the same way).
 *   - Widget values map, in order, onto the node's widget inputs; a value
 *     like "fixed"/"randomize" right after a numeric widget is the editor's
 *     control_after_generate helper and is skipped. A widget input that is
 *     linked takes the link instead of its stored value.
 *
 * The SCAIL "FINAL" workflows mark the user-facing nodes with numbered
 * titles; targets are found by those titles (so V11 keeps working), with
 * node-type fallbacks.
 */

#pragma once

#include <QJsonObject>
#include <QString>
#include <QStringList>

namespace rt::comfy {

/// SCAIL draws the character from scratch on green; H3 (Openjutsu's one-click
/// swap) redraws only the tracked person, following a grey mannequin of them.
enum class Engine { Scail, H3 };

/// API node ids of the nodes a shot fills in (empty = not found).
struct WorkflowSlots
{
    QString drivingVideo;    ///< "1. DRIVING VIDEO" (VHS_LoadVideo: video, force_rate)
    QString referenceImage;  ///< active "2. ... REFERENCE IMAGE" (LoadImage: image)
    QString expression;      ///< "5. FACIAL EXPRESSION" (value)
    QString runSettings;     ///< "6. RUN SETTINGS" (seed); H3: the Swap Plan
    QString save;            ///< "7. SAVE ..." (filename_prefix)
    Engine  engine{Engine::Scail};
};

struct ConvertResult
{
    QJsonObject   prompt;
    WorkflowSlots targets;
    QStringList   warnings;
    QString       error;     ///< non-empty → conversion failed

    [[nodiscard]] bool ok() const noexcept { return error.isEmpty(); }
};

[[nodiscard]] ConvertResult convertUiWorkflow(const QJsonObject& workflow);

/// An "Export (API)" workflow, used as is; slots by node type. A workflow with
/// an H3OneClickSwapPlan is the H3 engine.
[[nodiscard]] ConvertResult convertApiWorkflow(const QJsonObject& prompt);

/// Either format.
[[nodiscard]] ConvertResult convertWorkflow(const QJsonObject& json);

/// One shot's inputs. Empty / negative fields leave the workflow's value.
struct ShotParams
{
    QString videoName;          ///< file name in ComfyUI's input folder
    int     fps{0};
    QString referenceImageName; ///< file name in ComfyUI's input folder
    QString expression;
    qint64  seed{-1};
    QString filenamePrefix;
};

/// Returns a copy of `prompt` with the shot's values written into the targets.
/// `missing` receives the names of targets that were needed but not found.
[[nodiscard]] QJsonObject applyShotParams(QJsonObject prompt, const WorkflowSlots& targets,
                                          const ShotParams& params, QStringList* missing = nullptr);

/// The seed stored in the workflow's run-settings node (-1 if none).
[[nodiscard]] qint64 workflowSeed(const QJsonObject& prompt, const WorkflowSlots& targets);

/// The expression text stored in the workflow (empty if none).
[[nodiscard]] QString workflowExpression(const QJsonObject& prompt, const WorkflowSlots& targets);

enum class BatchStage { Prepare, Generate, Finish };
/// Preserve ToonOut RGBA and apply the approved interior repair before ProRes 4444 output.
/// Unrelated custom workflows are returned unchanged.
[[nodiscard]] QJsonObject alphaOutputPrompt(QJsonObject prompt, const WorkflowSlots& targets);
/// Empty for workflows that do not have the supported SCAIL -> ToonOut -> SaveVideo path.
[[nodiscard]] QJsonObject batchStagePrompt(const QJsonObject& prompt, const WorkflowSlots& targets,
                                          BatchStage stage, const QString& artifactKey);

// ── H3 swap ──────────────────────────────────────────────────────────────
constexpr int kH3Fps = 24;
/// The SCAIL workflow's canvas, so both engines deliver the same frame.
constexpr int kH3Width = 1280;
constexpr int kH3Height = 704;
/// Edit-mask grow at SAM's 1008 px: Openjutsu's "loose" (owner's 160), so
/// added ears fit; the background is cut out afterwards anyway.
constexpr int kH3MaskGrow = 160;
/// Reference describer (Qwen3-VL): Openjutsu's sentence plus exact colours —
/// eye colour above all, which H3 otherwise invents.
constexpr const char* kH3DescribePrompt =
    "Describe the person in this picture for a casting sheet, in one sentence of about 50 words: "
    "apparent age and gender, eye colour, hair colour and style, facial hair, skin tone, build, and "
    "their complete outfit including footwear. Name every colour precisely (for example slate blue "
    "eyes, jet-black hair, pale pink skin). No preamble, no background.";
constexpr int kH3DescribeTokens = 160;
/// Longest shot H3 renders as ONE clip (15 s floored to its 17k+5 grid).
constexpr int kH3MaxSingleClipFrames = 345;
/// describe (Qwen3-VL 8B) → track (SAM 3.1) → encode (text encoder + VAEs)
/// → generate (H3) → finish (ToonOut alpha): each model loads once per batch.
constexpr int kH3StageCount = 5;

/// The H3 settings every shot gets: 24 fps, a 1280×704 centre crop, lip-sync
/// to the dialogue, no hat pass, and (singleClip) one clip for the whole shot.
[[nodiscard]] QJsonObject h3ShotPrompt(QJsonObject prompt, const WorkflowSlots& targets, bool singleClip);

/// One pass (1..kH3StageCount) of a single-clip shot. Each pass saves the
/// outputs later passes use (RoundtableStash) and reads earlier ones back
/// (RoundtableUnstash) instead of running their models. Empty = unsupported.
[[nodiscard]] QJsonObject h3StagePrompt(const QJsonObject& prompt, const WorkflowSlots& targets,
                                       int stage, const QString& artifactKey);
/// What RoundtableStash reports for a finished pass ("described", ...).
[[nodiscard]] QString h3StageName(int stage);

} // namespace rt::comfy
