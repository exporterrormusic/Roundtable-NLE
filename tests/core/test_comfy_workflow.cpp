/*
 * test_comfy_workflow.cpp — editor-workflow → API-prompt conversion and shot
 * parameter filling for the COMFYUI page (SCAIL "FINAL" workflows).
 */
#include <gtest/gtest.h>

#include "panels/comfy/ComfyWorkflow.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

using namespace rt::comfy;

namespace {

// A trimmed graph with every construct the V10 workflow uses.
const char* kWorkflow = R"JSON({
  "nodes": [
    {"id": 27, "type": "MarkdownNote", "mode": 0, "widgets_values": ["# notes"]},
    {"id": 16, "type": "UNETLoader", "mode": 0,
     "inputs": [{"name": "unet_name", "widget": {"name": "unet_name"}, "link": null},
                {"name": "weight_dtype", "widget": {"name": "weight_dtype"}, "link": null}],
     "outputs": [{"name": "MODEL", "type": "MODEL"}],
     "widgets_values": ["scail.safetensors", "default"]},
    {"id": 74, "type": "EasyCache", "mode": 4, "title": "EasyCache - DISABLED",
     "inputs": [{"name": "model", "type": "MODEL", "link": 1},
                {"name": "reuse_threshold", "widget": {"name": "reuse_threshold"}, "link": null}],
     "outputs": [{"name": "MODEL", "type": "MODEL"}],
     "widgets_values": [0.15]},
    {"id": 50, "type": "Reroute", "mode": 0,
     "inputs": [{"name": "", "type": "*", "link": 2}],
     "outputs": [{"name": "", "type": "MODEL"}]},
    {"id": 84, "type": "GAPSCAIL2RunSettings", "mode": 0, "title": "6. RUN SETTINGS - RESOLUTION + SEED",
     "inputs": [{"name": "width", "widget": {"name": "width"}, "link": null},
                {"name": "height", "widget": {"name": "height"}, "link": null},
                {"name": "seed", "widget": {"name": "seed"}, "link": null}],
     "outputs": [{"name": "width", "type": "INT"}, {"name": "height", "type": "INT"}, {"name": "seed", "type": "INT"}],
     "widgets_values": [1280, 704, 153, "fixed"]},
    {"id": 87, "type": "PrimitiveStringMultiline", "mode": 0, "title": "5. FACIAL EXPRESSION (e.g. an annoyed expression)",
     "inputs": [{"name": "value", "widget": {"name": "value"}, "link": null}],
     "outputs": [{"name": "STRING", "type": "STRING"}],
     "widgets_values": ["a happy expression"]},
    {"id": 89, "type": "PrimitiveStringMultiline", "mode": 4, "title": "3. BASE PROMPT",
     "inputs": [{"name": "value", "widget": {"name": "value"}, "link": null}],
     "outputs": [{"name": "STRING", "type": "STRING"}], "widgets_values": ["old prompt"]},
    {"id": 90, "type": "LoadImage", "mode": 4, "title": "2. ONE REFERENCE IMAGE",
     "inputs": [{"name": "image", "widget": {"name": "image"}, "link": null},
                {"name": "upload", "widget": {"name": "upload"}, "link": null}],
     "outputs": [{"name": "IMAGE", "type": "IMAGE"}], "widgets_values": ["Medium.png", "image"]},
    {"id": 10, "type": "LoadImage", "mode": 0, "title": "2. ONE REFERENCE IMAGE",
     "inputs": [{"name": "image", "widget": {"name": "image"}, "link": null},
                {"name": "upload", "widget": {"name": "upload"}, "link": null}],
     "outputs": [{"name": "IMAGE", "type": "IMAGE"}], "widgets_values": ["Export-Wide.png", "image"]},
    {"id": 60, "type": "VHS_LoadVideo", "mode": 0, "title": "1. DRIVING VIDEO - 16 FPS",
     "inputs": [{"name": "meta_batch", "type": "VHS_BatchManager", "link": null},
                {"name": "video", "widget": {"name": "video"}, "link": null},
                {"name": "force_rate", "widget": {"name": "force_rate"}, "link": null},
                {"name": "frame_load_cap", "widget": {"name": "frame_load_cap"}, "link": null}],
     "outputs": [{"name": "IMAGE", "type": "IMAGE"}],
     "widgets_values": {"video": "W65.mp4", "force_rate": 16, "frame_load_cap": 0,
                        "videopreview": {"hidden": false}}},
    {"id": 23, "type": "GAPSCAIL2LongVideo", "mode": 0,
     "inputs": [{"name": "model", "type": "MODEL", "link": 3},
                {"name": "pose_video", "type": "IMAGE", "link": 4},
                {"name": "reference_image", "type": "IMAGE", "link": 5},
                {"name": "mask", "type": "MASK", "link": 9},
                {"name": "base_prompt", "widget": {"name": "base_prompt"}, "link": 6},
                {"name": "seed", "widget": {"name": "seed"}, "link": 7},
                {"name": "steps", "widget": {"name": "steps"}, "link": null}],
     "outputs": [{"name": "IMAGE", "type": "IMAGE"}],
     "widgets_values": ["stored prompt", 105, "fixed", 8]},
    {"id": 70, "type": "SomeMaskNode", "mode": 2,
     "outputs": [{"name": "MASK", "type": "MASK"}], "widgets_values": []},
    {"id": 25, "type": "SaveVideo", "mode": 0, "title": "7. SAVE FINAL VIDEO",
     "inputs": [{"name": "video", "type": "VIDEO", "link": 8},
                {"name": "filename_prefix", "widget": {"name": "filename_prefix"}, "link": null},
                {"name": "format", "widget": {"name": "format"}, "link": null},
                {"name": "format.codec", "widget": {"name": "format.codec"}, "link": null}],
     "widgets_values": ["video/SCAIL_2_FINAL_V10", "auto", "auto"]}
  ],
  "links": [
    [1, 16, 0, 74, 0, "MODEL"],
    [2, 74, 0, 50, 0, "MODEL"],
    [3, 50, 0, 23, 0, "MODEL"],
    [4, 60, 0, 23, 1, "IMAGE"],
    [5, 10, 0, 23, 2, "IMAGE"],
    [6, 87, 0, 23, 4, "STRING"],
    [7, 84, 2, 23, 5, "INT"],
    [8, 23, 0, 25, 0, "VIDEO"],
    [9, 70, 0, 23, 3, "MASK"]
  ]
})JSON";

ConvertResult convert()
{
    return convertUiWorkflow(QJsonDocument::fromJson(kWorkflow).object());
}

QJsonObject inputsOf(const QJsonObject& prompt, const char* id)
{
    return prompt.value(QLatin1String(id)).toObject().value(QLatin1String("inputs")).toObject();
}

} // namespace

TEST(ComfyWorkflowTest, DropsNotesMutedAndBypassedNodes)
{
    const auto r = convert();
    ASSERT_TRUE(r.ok()) << r.error.toStdString();
    const QStringList ids = r.prompt.keys();
    for (const char* gone : {"27", "74", "50", "89", "90", "70"})
        EXPECT_FALSE(ids.contains(QLatin1String(gone))) << gone;
    for (const char* kept : {"16", "84", "87", "10", "60", "23", "25"})
        EXPECT_TRUE(ids.contains(QLatin1String(kept))) << kept;
    EXPECT_EQ(r.prompt.value("23").toObject().value("class_type").toString(), "GAPSCAIL2LongVideo");
}

TEST(ComfyWorkflowTest, RewiresThroughBypassAndReroute)
{
    const auto r = convert();
    // model: UNET → (bypassed EasyCache) → Reroute → SCAIL  ⇒  UNET directly.
    EXPECT_EQ(inputsOf(r.prompt, "23").value("model").toArray(), (QJsonArray{"16", 0}));
    // A link from a muted node is dropped rather than left dangling.
    EXPECT_FALSE(inputsOf(r.prompt, "23").contains("mask"));
}

TEST(ComfyWorkflowTest, MapsWidgetsSkippingControlValuesAndPreferringLinks)
{
    const auto r = convert();
    const auto scail = inputsOf(r.prompt, "23");
    EXPECT_EQ(scail.value("base_prompt").toArray(), (QJsonArray{"87", 0}));   // linked widget
    EXPECT_EQ(scail.value("seed").toArray(), (QJsonArray{"84", 2}));
    EXPECT_EQ(scail.value("steps").toInt(), 8);   // after the skipped "fixed"

    const auto settings = inputsOf(r.prompt, "84");
    EXPECT_EQ(settings.value("width").toInt(), 1280);
    EXPECT_EQ(settings.value("seed").toInt(), 153);
    EXPECT_FALSE(settings.contains("control_after_generate"));

    const auto image = inputsOf(r.prompt, "10");
    EXPECT_EQ(image.value("image").toString(), "Export-Wide.png");
    EXPECT_FALSE(image.contains("upload"));

    const auto video = inputsOf(r.prompt, "60");   // VHS stores a dict
    EXPECT_EQ(video.value("video").toString(), "W65.mp4");
    EXPECT_EQ(video.value("force_rate").toInt(), 16);
    EXPECT_FALSE(video.contains("videopreview"));

    EXPECT_EQ(inputsOf(r.prompt, "25").value("format.codec").toString(), "auto");
}

TEST(ComfyWorkflowTest, FindsNumberedSlotsIgnoringBypassedDuplicates)
{
    const auto r = convert();
    EXPECT_EQ(r.targets.drivingVideo, "60");
    EXPECT_EQ(r.targets.referenceImage, "10");   // not the bypassed "2." node
    EXPECT_EQ(r.targets.expression, "87");
    EXPECT_EQ(r.targets.runSettings, "84");
    EXPECT_EQ(r.targets.save, "25");
    EXPECT_EQ(workflowSeed(r.prompt, r.targets), 153);
    EXPECT_EQ(workflowExpression(r.prompt, r.targets), "a happy expression");
}

TEST(ComfyWorkflowTest, AppliesShotParameters)
{
    const auto r = convert();
    ShotParams p;
    p.videoName = "007_STORY_CU_L12.mp4";
    p.fps = 12;
    p.referenceImageName = "closeup_ref.png";
    p.expression = "a smug grin";
    p.seed = 4242;
    p.filenamePrefix = "roundtable/007_STORY_CU_L12";
    QStringList missing;
    const QJsonObject prompt = applyShotParams(r.prompt, r.targets, p, &missing);
    EXPECT_TRUE(missing.isEmpty());
    EXPECT_EQ(inputsOf(prompt, "60").value("video").toString(), "007_STORY_CU_L12.mp4");
    EXPECT_EQ(inputsOf(prompt, "60").value("force_rate").toInt(), 12);
    EXPECT_EQ(inputsOf(prompt, "60").value("frame_load_cap").toInt(), 0);   // untouched
    EXPECT_EQ(inputsOf(prompt, "10").value("image").toString(), "closeup_ref.png");
    EXPECT_EQ(inputsOf(prompt, "87").value("value").toString(), "a smug grin");
    EXPECT_EQ(inputsOf(prompt, "84").value("seed").toInteger(), 4242);
    EXPECT_EQ(inputsOf(prompt, "25").value("filename_prefix").toString(), "roundtable/007_STORY_CU_L12");
    // The original prompt is not modified.
    EXPECT_EQ(inputsOf(r.prompt, "84").value("seed").toInt(), 153);
}

TEST(ComfyWorkflowTest, ReportsMissingSlots)
{
    QJsonObject bare{{"nodes", QJsonArray{QJsonObject{{"id", 1}, {"type", "KSampler"}, {"mode", 0}}}}};
    const auto r = convertUiWorkflow(bare);
    ASSERT_TRUE(r.ok());
    ShotParams p;
    p.videoName = "x.mp4";
    p.seed = 1;
    QStringList missing;
    (void)applyShotParams(r.prompt, r.targets, p, &missing);
    EXPECT_TRUE(missing.contains("1. DRIVING VIDEO"));
    EXPECT_TRUE(missing.contains("6. RUN SETTINGS"));
}

TEST(ComfyWorkflowTest, RejectsNonWorkflows)
{
    EXPECT_FALSE(convertUiWorkflow(QJsonObject{{"1", QJsonObject{}}}).ok());
}

