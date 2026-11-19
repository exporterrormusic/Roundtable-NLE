/*
 * test_comfy_workflow.cpp — editor-workflow → API-prompt conversion and shot
 * parameter filling for the COMFYUI page (SCAIL "FINAL" workflows).
 */
#include <gtest/gtest.h>

#include "panels/comfy/ComfyWorkflow.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <iostream>

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


// ── H3 swap (Openjutsu's simple.api.json, trimmed to one node per role) ──────

namespace {

const char* kH3Workflow = R"JSON({
  "2":  {"class_type": "LoadImage", "inputs": {"image": ""}},
  "4":  {"class_type": "TextGenerate", "inputs": {"prompt": "describe", "clip": ["13", 0], "image": ["2", 0]}},
  "5":  {"class_type": "VHS_LoadVideo", "inputs": {"video": "", "force_rate": 24}},
  "13": {"class_type": "CLIPLoader", "inputs": {"clip_name": "qwen3vl_8b.safetensors"}},
  "11": {"class_type": "H3OneClickSwapPlan", "_meta": {"title": "Swap Plan"},
         "inputs": {"clip_seconds": 6, "resolution": "736p", "seed": 0, "roles": "",
                    "no_lip_sync": ["68", 0], "source_frames": ["5", 0], "picture_1": ["2", 0], "picture_1_text": ["4", 0]}},
  "28": {"class_type": "MiniMaxH3ChainLoopStart", "inputs": {"plan": ["11", 0]}},
  "29": {"class_type": "MiniMaxH3ChainCurrent", "inputs": {"state": ["28", 1]}},
  "44": {"class_type": "TextGenerate", "inputs": {"prompt": ["11", 4], "clip": ["13", 0], "image": ["5", 0]}},
  "38": {"class_type": "SAM3_VideoTrack", "inputs": {"images": ["5", 0], "clip_frames": ["29", 6]}},
  "39": {"class_type": "H3PeopleMask", "inputs": {"track_data": ["38", 0]}},
  "47": {"class_type": "SAM3_VideoTrack", "_meta": {"title": "SAM 3.1 track hats"}, "inputs": {"images": ["5", 0]}},
  "52": {"class_type": "MaskComposite", "_meta": {"title": "People + hats"},
         "inputs": {"destination": ["39", 0], "source": ["47", 0]}},
  "42": {"class_type": "GrowMaskWithBlur", "_meta": {"title": "Grow mask (~4% of the frame)"}, "inputs": {"expand": 40, "mask": ["52", 0]}},
  "68": {"class_type": "PrimitiveBoolean", "_meta": {"title": "No lip-sync in the prompt"}, "inputs": {"value": true}},
  "73": {"class_type": "AudioAdjustVolume", "_meta": {"title": "Mute the reference audio H3 hears"},
         "inputs": {"volume": -100, "audio": ["29", 12]}},
  "30": {"class_type": "MiniMaxH3ReferenceToVideo",
         "inputs": {"prompt": ["44", 0], "width": ["29", 8], "ref_images.ref_image_0": ["2", 0],
                    "ref_videos.ref_video_0": ["39", 0], "ref_video_audios.ref_video_audio_0": ["73", 0]}},
  "57": {"class_type": "MiniMaxH3ChainContext", "inputs": {"state": ["29", 0], "conditioning": ["30", 0], "mask": ["42", 0]}},
  "60": {"class_type": "SamplerCustomAdvanced", "inputs": {"guider": ["57", 0], "latent_image": ["57", 3]}},
  "66": {"class_type": "MiniMaxH3ChainLoopEnd", "inputs": {"flow": ["28", 0], "state": ["29", 0], "images": ["60", 0]}},
  "67": {"class_type": "MiniMaxH3ChainAssemble", "inputs": {"manifest": ["66", 0]}},
  "alpha_load": {"class_type": "VHS_LoadVideoPath", "inputs": {"video": ["67", 0]}},
  "alpha_save": {"class_type": "VHS_VideoCombine",
                 "inputs": {"images": ["alpha_load", 0], "format": "video/ProRes", "filename_prefix": "h3_alpha/swap"}}
})JSON";

ConvertResult convertH3()
{
    return convertWorkflow(QJsonDocument::fromJson(kH3Workflow).object());
}

bool linksTo(const QJsonObject& prompt, const QString& node, const QString& input, const QString& source, int slot)
{
    return prompt.value(node).toObject().value("inputs").toObject().value(input).toArray() == QJsonArray{source, slot};
}

/// The stash names of every node of `type`, sorted.
QStringList ofType(const QJsonObject& prompt, const QString& type)
{
    QStringList names;
    for (auto it = prompt.begin(); it != prompt.end(); ++it)
        if (it.value().toObject().value("class_type").toString() == type)
            names << it.value().toObject().value("inputs").toObject().value("name").toString();
    names.sort();
    return names;
}

} // namespace

TEST(ComfyWorkflowTest, ApiWorkflowWithSwapPlanIsTheH3Engine)
{
    const auto r = convertH3();
    ASSERT_TRUE(r.ok()) << r.error.toStdString();
    EXPECT_EQ(r.targets.engine, Engine::H3);
    EXPECT_EQ(r.targets.drivingVideo, "5");
    EXPECT_EQ(r.targets.referenceImage, "2");
    EXPECT_EQ(r.targets.runSettings, "11");
    EXPECT_EQ(r.targets.save, "alpha_save");
    EXPECT_EQ(convert().targets.engine, Engine::Scail);
}

TEST(ComfyWorkflowTest, H3ShotPromptMatchesScailCanvasAndKeepsLipSync)
{
    const auto r = convertH3();
    ShotParams p;
    p.seed = 0x100000007LL;   // wider than the Swap Plan's 32-bit seed
    const QJsonObject prompt = h3ShotPrompt(applyShotParams(r.prompt, r.targets, p), r.targets, true);

    for (const char* gone : {"68", "73", "47", "52"}) EXPECT_FALSE(prompt.contains(gone)) << gone;
    EXPECT_FALSE(inputsOf(prompt, "11").contains("no_lip_sync"));
    EXPECT_TRUE(linksTo(prompt, "30", "ref_video_audios.ref_video_audio_0", "29", 12));   // H3 hears the dialogue
    EXPECT_TRUE(linksTo(prompt, "42", "mask", "39", 0));                                   // people mask, no hats
    EXPECT_EQ(inputsOf(prompt, "42").value("expand").toInt(), kH3MaskGrow);                 // loose: ears fit

    EXPECT_EQ(inputsOf(prompt, "5").value("force_rate").toInt(), kH3Fps);
    EXPECT_EQ(inputsOf(prompt, "rt_h3_size").value("width").toInt(), 1280);
    EXPECT_EQ(inputsOf(prompt, "rt_h3_size").value("height").toInt(), 704);
    EXPECT_TRUE(linksTo(prompt, "rt_h3_size", "image", "5", 0));
    EXPECT_TRUE(linksTo(prompt, "38", "images", "rt_h3_size", 0));
    EXPECT_TRUE(linksTo(prompt, "44", "image", "rt_h3_size", 0));
    EXPECT_TRUE(linksTo(prompt, "11", "source_frames", "rt_h3_size", 0));
    EXPECT_EQ(inputsOf(prompt, "11").value("short_edge").toInt(), 704);
    EXPECT_EQ(inputsOf(prompt, "11").value("clip_seconds").toDouble(), 15.0);
    EXPECT_EQ(inputsOf(prompt, "11").value("seed").toInteger(), 7);

    // A long shot keeps the workflow's clip length and renders as a chain.
    EXPECT_EQ(inputsOf(h3ShotPrompt(r.prompt, r.targets, false), "11").value("clip_seconds").toInt(), 6);
}

TEST(ComfyWorkflowTest, H3PadsTheSoundtrackToTheLoadedFrames)
{
    const auto r = convertH3();
    QJsonObject graph = r.prompt;
    graph.insert("6", QJsonObject{{"class_type", "TrimAudioDuration"},
                                  {"inputs", QJsonObject{{"audio", QJsonArray{"5", 2}}, {"duration", 2048}}}});
    const QJsonObject prompt = h3ShotPrompt(graph, r.targets, true);

    EXPECT_TRUE(linksTo(prompt, "6", "audio", "rt_h3_soundtrack", 0));
    EXPECT_EQ(prompt.value("rt_h3_soundtrack").toObject().value("class_type").toString(), "H3SoundtrackOrSilence");
    EXPECT_TRUE(linksTo(prompt, "rt_h3_soundtrack", "audio", "5", 2));
    EXPECT_TRUE(linksTo(prompt, "rt_h3_soundtrack", "video_info", "5", 3));
    // Nothing reads the soundtrack → no extra node.
    EXPECT_FALSE(h3ShotPrompt(r.prompt, r.targets, true).contains("rt_h3_soundtrack"));
}

TEST(ComfyWorkflowTest, H3AsksForEyeColourAndUnmixesHairEdges)
{
    const auto r = convertH3();
    QJsonObject graph = r.prompt;
    QJsonObject describe = graph.value("4").toObject();
    describe.insert("_meta", QJsonObject{{"title", "Describe picture 1"}});
    graph.insert("4", describe);
    graph.insert("repair", QJsonObject{{"class_type", "RoundtableAlphaRepair"},
                                       {"inputs", QJsonObject{{"image", QJsonArray{"alpha_load", 0}}}}});
    const QJsonObject prompt = h3ShotPrompt(graph, r.targets, true);

    EXPECT_TRUE(inputsOf(prompt, "4").value("prompt").toString().contains("eye colour"));
    EXPECT_EQ(inputsOf(prompt, "4").value("max_length").toInt(), kH3DescribeTokens);
    EXPECT_EQ(inputsOf(prompt, "44").value("prompt"), QJsonValue(QJsonArray{"11", 4}));   // shot writer untouched
    EXPECT_EQ(inputsOf(prompt, "repair").value("edge_mode").toString(), "unmix");
    // SCAIL keeps the approved hard cut.
    EXPECT_FALSE(inputsOf(h3ShotPrompt(graph, convert().targets, true), "repair").contains("edge_mode"));
}

TEST(ComfyWorkflowTest, H3PassesRunEachModelOnceAndHandOverThroughDisk)
{
    const auto r = convertH3();
    const QJsonObject shot = h3ShotPrompt(r.prompt, r.targets, true);
    auto pass = [&](int k) { return h3StagePrompt(shot, r.targets, k, "key_0"); };

    const QJsonObject describe = pass(1), track = pass(2), encode = pass(3), generate = pass(4), finish = pass(5);
    for (const auto* g : {&describe, &track, &encode, &generate, &finish}) ASSERT_FALSE(g->isEmpty());

    // 1: both descriptions; nothing heavier.
    EXPECT_EQ(ofType(describe, "RoundtableStash"), (QStringList{"44_0", "4_0"}));
    EXPECT_TRUE(describe.contains("4") && describe.contains("44"));
    EXPECT_FALSE(describe.contains("38") || describe.contains("30") || describe.contains("60"));

    // 2: SAM reads the description back (the plan needs it) instead of running Qwen.
    EXPECT_EQ(ofType(track, "RoundtableStash"), QStringList{"38_0"});
    EXPECT_EQ(ofType(track, "RoundtableUnstash"), QStringList{"4_0"});
    EXPECT_FALSE(track.contains("4") || track.contains("44"));

    // 3: encoder + VAEs from the tracks and the shot list.
    EXPECT_EQ(ofType(encode, "RoundtableStash"), (QStringList{"57_0", "57_3"}));
    EXPECT_EQ(ofType(encode, "RoundtableUnstash"), (QStringList{"38_0", "44_0", "4_0"}));
    EXPECT_FALSE(encode.contains("38") || encode.contains("60"));

    // 4: H3 only; the loop state is recomputed (cheap), the conditioning read back.
    EXPECT_EQ(ofType(generate, "RoundtableStash"), QStringList{"67_0"});
    EXPECT_EQ(ofType(generate, "RoundtableUnstash"), (QStringList{"4_0", "57_0", "57_3"}));
    EXPECT_TRUE(generate.contains("60") && generate.contains("29"));
    EXPECT_FALSE(generate.contains("30") || generate.contains("38"));
    for (auto it = generate.begin(); it != generate.end(); ++it)
        EXPECT_NE(it.value().toObject().value("class_type").toString(), "TextGenerate");

    // 5: cut out the assembled video with alpha.
    EXPECT_EQ(ofType(finish, "RoundtableUnstash"), QStringList{"67_0"});
    EXPECT_TRUE(finish.contains("alpha_save"));
    EXPECT_FALSE(finish.contains("60") || finish.contains("67"));
    EXPECT_EQ(inputsOf(describe, "rt_stash_4_0").value("stage").toString(), h3StageName(1));
    EXPECT_EQ(inputsOf(describe, "rt_stash_4_0").value("artifact_key").toString(), "key_0");

    // Not H3 → no H3 passes.
    const auto scail = convert();
    EXPECT_TRUE(h3StagePrompt(scail.prompt, scail.targets, 1, "k").isEmpty());
}

// The real Openjutsu export: set ROUNDTABLE_H3_WORKFLOW to its simple.api.json.
TEST(ComfyWorkflowTest, LiveH3WorkflowSplitsIntoFivePasses)
{
    const QString path = qEnvironmentVariable("ROUNDTABLE_H3_WORKFLOW");
    if (path.isEmpty()) GTEST_SKIP() << "ROUNDTABLE_H3_WORKFLOW not set";
    QFile f(path);
    ASSERT_TRUE(f.open(QIODevice::ReadOnly));
    const auto r = convertWorkflow(QJsonDocument::fromJson(f.readAll()).object());
    ASSERT_TRUE(r.ok()) << r.error.toStdString();
    ASSERT_EQ(r.targets.engine, Engine::H3);
    const QJsonObject shot = h3ShotPrompt(r.prompt, r.targets, true);
    for (int k = 1; k <= kH3StageCount; ++k) {
        const QJsonObject pass = h3StagePrompt(shot, r.targets, k, "key_0");
        ASSERT_FALSE(pass.isEmpty()) << "pass " << k;
        QStringList types;
        for (auto it = pass.begin(); it != pass.end(); ++it) {
            const QJsonObject node = it.value().toObject();
            const QString type = node.value("class_type").toString();
            types << (type.startsWith("Roundtable") && type.endsWith("stash")
                ? type + ":" + node.value("inputs").toObject().value("name").toString() : type);
            // Every link resolves inside the pass.
            const QJsonObject in = node.value("inputs").toObject();
            for (auto i = in.begin(); i != in.end(); ++i) {
                const QJsonArray link = i.value().toArray();
                if (link.size() == 2 && link[0].isString())
                    EXPECT_TRUE(pass.contains(link[0].toString())) << "pass " << k << " " << it.key().toStdString()
                                                                   << "." << i.key().toStdString();
            }
        }
        types.sort();
        std::cout << "PASS " << k << " (" << h3StageName(k).toStdString() << "): "
                  << types.join(", ").toStdString() << "\n";
    }
}
