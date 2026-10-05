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

/// API node ids of the nodes a shot fills in (empty = not found).
struct WorkflowSlots
{
    QString drivingVideo;    ///< "1. DRIVING VIDEO" (VHS_LoadVideo: video, force_rate)
    QString referenceImage;  ///< active "2. ... REFERENCE IMAGE" (LoadImage: image)
    QString expression;      ///< "5. FACIAL EXPRESSION" (value)
    QString runSettings;     ///< "6. RUN SETTINGS" (seed)
    QString save;            ///< "7. SAVE ..." (filename_prefix)
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

} // namespace rt::comfy
