/*
 * ComfyWorkflow.cpp — see header.
 */
#include "panels/comfy/ComfyWorkflow.h"

#include <QHash>
#include <QJsonArray>
#include <QRegularExpression>
#include <QSet>

#include <functional>
#include <map>
#include <optional>

namespace rt::comfy {

namespace {

constexpr int kModeMuted    = 2;
constexpr int kModeBypassed = 4;

struct LinkInfo
{
    int     srcNode{-1};
    int     srcSlot{0};
    QString type;
};

bool isUiOnlyType(const QString& type)
{
    // "Note", "MarkdownNote" and custom-node note variants ("... Note").
    return type.endsWith(QLatin1String("Note"));
}

bool isControlAfterGenerate(const QJsonValue& v)
{
    if (!v.isString()) return false;
    const QString s = v.toString();
    return s == QLatin1String("fixed") || s == QLatin1String("increment") ||
           s == QLatin1String("decrement") || s == QLatin1String("randomize");
}

/// Leading "N." of a node title, or 0.
int titleNumber(const QString& title)
{
    static const QRegularExpression re(QStringLiteral(R"(^\s*(\d+)\s*\.)"));
    const auto m = re.match(title);
    return m.hasMatch() ? m.captured(1).toInt() : 0;
}

} // namespace

ConvertResult convertUiWorkflow(const QJsonObject& workflow)
{
    ConvertResult result;
    if (workflow.contains(QLatin1String("definitions")) &&
        !workflow.value(QLatin1String("definitions")).toObject().value(QLatin1String("subgraphs")).toArray().isEmpty()) {
        result.error = QStringLiteral("Workflows with subgraphs are not supported; unpack them in ComfyUI first.");
        return result;
    }
    const QJsonArray nodes = workflow.value(QLatin1String("nodes")).toArray();
    if (nodes.isEmpty()) {
        result.error = QStringLiteral("Not a ComfyUI editor workflow (no \"nodes\").");
        return result;
    }

    QHash<int, QJsonObject> byId;
    for (const auto& v : nodes) {
        const QJsonObject n = v.toObject();
        byId.insert(n.value(QLatin1String("id")).toInt(), n);
    }
    QHash<int, LinkInfo> links;
    for (const auto& v : workflow.value(QLatin1String("links")).toArray()) {
        const QJsonArray l = v.toArray();
        if (l.size() < 6) continue;
        links.insert(l.at(0).toInt(), LinkInfo{l.at(1).toInt(), l.at(2).toInt(), l.at(5).toString()});
    }

    // Follow bypassed nodes and reroutes back to a real producer.
    std::function<std::optional<LinkInfo>(LinkInfo, int)> resolve =
        [&](LinkInfo src, int depth) -> std::optional<LinkInfo> {
        if (depth > 64) return std::nullopt;
        auto it = byId.find(src.srcNode);
        if (it == byId.end()) return std::nullopt;
        const QJsonObject& n = it.value();
        const QString type = n.value(QLatin1String("type")).toString();
        const int mode = n.value(QLatin1String("mode")).toInt();
        if (mode == kModeMuted || isUiOnlyType(type)) return std::nullopt;   // not in the prompt
        if (type != QLatin1String("Reroute") && mode != kModeBypassed) return src;
        const QJsonArray outputs = n.value(QLatin1String("outputs")).toArray();
        const QString outType = src.srcSlot < outputs.size()
            ? outputs.at(src.srcSlot).toObject().value(QLatin1String("type")).toString() : src.type;
        for (const auto& iv : n.value(QLatin1String("inputs")).toArray()) {
            const QJsonObject in = iv.toObject();
            if (in.value(QLatin1String("link")).isNull()) continue;
            const QString inType = in.value(QLatin1String("type")).toString();
            if (type != QLatin1String("Reroute") && inType != outType && outType != QLatin1String("*"))
                continue;
            auto l = links.find(in.value(QLatin1String("link")).toInt());
            if (l == links.end()) continue;
            return resolve(l.value(), depth + 1);
        }
        return std::nullopt;
    };

    QSet<const QString*> claimedByTitle;
    for (auto it = byId.cbegin(); it != byId.cend(); ++it) {
        const QJsonObject& n = it.value();
        const QString type = n.value(QLatin1String("type")).toString();
        const int mode = n.value(QLatin1String("mode")).toInt();
        if (mode == kModeMuted || mode == kModeBypassed || type == QLatin1String("Reroute") ||
            isUiOnlyType(type))
            continue;
        if (type == QLatin1String("PrimitiveNode")) {
            result.warnings << QStringLiteral("Node %1: legacy Primitive nodes are not supported").arg(it.key());
            continue;
        }

        QJsonObject inputs;
        const QJsonValue widgetValues = n.value(QLatin1String("widgets_values"));
        const QJsonArray wvList = widgetValues.toArray();
        const QJsonObject wvDict = widgetValues.toObject();
        int wi = 0;
        for (const auto& iv : n.value(QLatin1String("inputs")).toArray()) {
            const QJsonObject in = iv.toObject();
            const QString name = in.value(QLatin1String("name")).toString();
            const bool isWidget = in.contains(QLatin1String("widget"));
            QJsonValue stored;
            if (isWidget) {
                if (widgetValues.isObject()) {
                    stored = wvDict.value(name);
                } else if (wi < wvList.size()) {
                    stored = wvList.at(wi++);
                    if (stored.isDouble() && wi < wvList.size() && isControlAfterGenerate(wvList.at(wi)))
                        ++wi;
                }
            }
            if (!in.value(QLatin1String("link")).isNull() && in.contains(QLatin1String("link"))) {
                auto l = links.find(in.value(QLatin1String("link")).toInt());
                if (l != links.end()) {
                    if (auto src = resolve(l.value(), 0)) {
                        inputs.insert(name, QJsonArray{QString::number(src->srcNode), src->srcSlot});
                        continue;
                    }
                }
                if (!isWidget) continue;   // dangling (e.g. from a muted node)
            }
            if (isWidget && name != QLatin1String("upload") && !stored.isUndefined())
                inputs.insert(name, stored);
        }

        QJsonObject apiNode;
        apiNode.insert(QLatin1String("class_type"), type);
        apiNode.insert(QLatin1String("inputs"), inputs);
        const QString title = n.value(QLatin1String("title")).toString();
        if (!title.isEmpty())
            apiNode.insert(QLatin1String("_meta"), QJsonObject{{QLatin1String("title"), title}});
        result.prompt.insert(QString::number(it.key()), apiNode);

        // Slots: numbered titles first, then node-type fallbacks.
        const QString id = QString::number(it.key());
        const int num = titleNumber(title);
        // A numbered title always wins; a type match only fills an empty slot
        // (and is replaced if the numbered node turns up later).
        auto claim = [&](QString& slot, int wantNum, const QString& wantType) {
            if (num == wantNum) {
                slot = id;
                claimedByTitle.insert(&slot);
            } else if (!claimedByTitle.contains(&slot) && slot.isEmpty() &&
                       !wantType.isEmpty() && type == wantType) {
                slot = id;
            }
        };
        claim(result.targets.drivingVideo, 1, QStringLiteral("VHS_LoadVideo"));
        claim(result.targets.referenceImage, 2, QStringLiteral("LoadImage"));
        claim(result.targets.expression, 5, QString());
        claim(result.targets.runSettings, 6, QString());
        claim(result.targets.save, 7, QStringLiteral("SaveVideo"));
    }

    if (result.prompt.isEmpty()) result.error = QStringLiteral("The workflow has no runnable nodes.");
    return result;
}

ConvertResult convertApiWorkflow(const QJsonObject& prompt)
{
    ConvertResult result;
    for (auto it = prompt.begin(); it != prompt.end(); ++it) {
        const QJsonObject node = it.value().toObject();
        const QString type = node.value(QLatin1String("class_type")).toString();
        if (type.isEmpty() || !node.value(QLatin1String("inputs")).isObject()) {
            result.error = QStringLiteral("Not a ComfyUI workflow (node %1 has no class_type/inputs).").arg(it.key());
            return result;
        }
        const QString id = it.key();
        auto claim = [&](QString& slot) { if (slot.isEmpty()) slot = id; };
        if (type == QLatin1String("VHS_LoadVideo")) claim(result.targets.drivingVideo);
        else if (type == QLatin1String("LoadImage")) claim(result.targets.referenceImage);
        else if (type == QLatin1String("H3OneClickSwapPlan")) {
            claim(result.targets.runSettings);
            result.targets.engine = Engine::H3;
        } else if (type == QLatin1String("SaveVideo")) claim(result.targets.save);
        else if (type == QLatin1String("VHS_VideoCombine") &&
                 node.value("inputs").toObject().value("format").toString() == QLatin1String("video/ProRes"))
            result.targets.save = id;   // the alpha master wins over any preview save
    }
    if (prompt.isEmpty()) result.error = QStringLiteral("The workflow has no runnable nodes.");
    else result.prompt = prompt;
    return result;
}

ConvertResult convertWorkflow(const QJsonObject& json)
{
    return json.contains(QLatin1String("nodes")) ? convertUiWorkflow(json) : convertApiWorkflow(json);
}

QJsonObject applyShotParams(QJsonObject prompt, const WorkflowSlots& targets,
                            const ShotParams& params, QStringList* missing)
{
    auto set = [&](const QString& nodeId, const char* slotName, const QString& key, const QJsonValue& value) {
        if (nodeId.isEmpty() || !prompt.contains(nodeId)) {
            if (missing) missing->append(QString::fromLatin1(slotName));
            return;
        }
        QJsonObject node = prompt.value(nodeId).toObject();
        QJsonObject inputs = node.value(QLatin1String("inputs")).toObject();
        inputs.insert(key, value);
        node.insert(QLatin1String("inputs"), inputs);
        prompt.insert(nodeId, node);
    };
    if (!params.videoName.isEmpty())
        set(targets.drivingVideo, "1. DRIVING VIDEO", QStringLiteral("video"), params.videoName);
    if (params.fps > 0)
        set(targets.drivingVideo, "1. DRIVING VIDEO", QStringLiteral("force_rate"), params.fps);
    if (!params.referenceImageName.isEmpty())
        set(targets.referenceImage, "2. REFERENCE IMAGE", QStringLiteral("image"), params.referenceImageName);
    if (!params.expression.isEmpty())
        set(targets.expression, "5. FACIAL EXPRESSION", QStringLiteral("value"), params.expression);
    if (params.seed >= 0)
        set(targets.runSettings, "6. RUN SETTINGS", QStringLiteral("seed"), static_cast<double>(params.seed));
    if (!params.filenamePrefix.isEmpty())
        set(targets.save, "7. SAVE", QStringLiteral("filename_prefix"), params.filenamePrefix);
    return prompt;
}

qint64 workflowSeed(const QJsonObject& prompt, const WorkflowSlots& targets)
{
    const QJsonValue v = prompt.value(targets.runSettings).toObject()
                             .value(QLatin1String("inputs")).toObject().value(QLatin1String("seed"));
    return v.isDouble() ? static_cast<qint64>(v.toDouble()) : -1;
}

QString workflowExpression(const QJsonObject& prompt, const WorkflowSlots& targets)
{
    return prompt.value(targets.expression).toObject()
        .value(QLatin1String("inputs")).toObject().value(QLatin1String("value")).toString();
}

QJsonObject alphaOutputPrompt(QJsonObject graph, const WorkflowSlots& targets)
{
    const auto save = graph.value(targets.save).toObject();
    if (save.value("class_type") != "SaveVideo") return graph;
    const auto saved = save.value("inputs").toObject();
    const auto source = [](const QJsonValue& value) {
        const auto link = value.toArray();
        return link.isEmpty() ? QString() : link[0].toString();
    };
    const QString create = source(saved.value("video"));
    if (graph.value(create).toObject().value("class_type") != "CreateVideo") return graph;
    const auto video = graph.value(create).toObject().value("inputs").toObject();
    const QString cleanup = source(video.value("images"));
    auto toon = graph.value(cleanup).toObject();
    if (toon.value("class_type") != "ToonOut Selected BG JNK") return graph;
    auto clean = toon.value("inputs").toObject();
    clean.insert("background_type", "transparent");
    toon.insert("inputs", clean);
    graph.insert(cleanup, toon);
    graph.insert("rt_alpha_repair", QJsonObject{{"class_type", "RoundtableAlphaRepair"},
        {"inputs", QJsonObject{{"image", QJsonArray{cleanup, 0}}}}});
    graph.insert(targets.save, QJsonObject{{"class_type", "VHS_VideoCombine"},
        {"inputs", QJsonObject{{"images", QJsonArray{"rt_alpha_repair", 0}},
            {"audio", video.value("audio")}, {"frame_rate", video.value("fps")},
            {"filename_prefix", saved.value("filename_prefix")}, {"format", "video/ProRes"},
            {"profile", "4444"}, {"loop_count", 0}, {"pingpong", false},
            {"save_output", true}, {"save_metadata", true}, {"trim_to_audio", false}}}});
    graph.remove(create);
    return graph;
}

QJsonObject batchStagePrompt(const QJsonObject& original, const WorkflowSlots& targets,
                            BatchStage stage, const QString& artifactKey)
{
    auto inputs = [&](const QString& id) { return original.value(id).toObject().value("inputs").toObject(); };
    auto type = [&](const QString& id) { return original.value(id).toObject().value("class_type").toString(); };
    auto source = [](const QJsonValue& value) {
        const auto link = value.toArray();
        return link.isEmpty() ? QString() : link[0].toString();
    };
    const QString create = source(inputs(targets.save).value("video"));
    const QString cleanup = source(inputs(create).value("images"));
    const QString generator = source(inputs(cleanup).value("image"));
    if (type(targets.save) != "SaveVideo" || type(create) != "CreateVideo" ||
        type(cleanup) != "ToonOut Selected BG JNK" || type(generator) != "GAPSCAIL2LongVideo") return {};
    const QJsonObject generation = inputs(generator);
    for (const char* name : {"reference_image", "reference_image_mask", "clip_vision_output", "character_count"})
        if (!generation.contains(QLatin1String(name))) return {};

    QJsonObject graph = original;
    auto put = [&](const QString& id, const QString& nodeType, const QJsonObject& in) {
        graph.insert(id, QJsonObject{{"class_type", nodeType}, {"inputs", in}});
    };
    const QString reference = "rt_batch_reference", media = "rt_batch_media";
    QString output;
    if (stage == BatchStage::Prepare) {
        QJsonObject ref;
        for (const char* name : {"reference_image", "reference_image_mask", "clip_vision_output", "character_count",
                                 "vae", "width", "height", "replacement_mode"})
            ref.insert(QLatin1String(name), generation.value(QLatin1String(name)));
        put(reference, "RoundtableReference", ref);
        QJsonObject prep = generation;
        for (const char* name : {"model", "vae", "reference_image", "reference_image_mask", "clip_vision_output", "character_count"})
            prep.remove(QLatin1String(name));
        prep.insert("reference", QJsonArray{reference, 0});
        prep.insert("artifact_key", artifactKey);
        prep.insert("audio", inputs(create).value("audio"));
        prep.insert("fps", inputs(create).value("fps"));
        put(generator, "RoundtablePrepare", prep);
        output = generator;
    } else if (stage == BatchStage::Generate) {
        put(generator, "RoundtableGenerate", QJsonObject{{"artifact_key", artifactKey},
            {"model", generation.value("model")}, {"vae", generation.value("vae")}});
        output = generator;
    } else {
        put(media, "RoundtableLoadFrames", QJsonObject{{"artifact_key", artifactKey}});
        QJsonObject clean = inputs(cleanup);
        clean.insert("image", QJsonArray{media, 0});
        clean.remove("scail_model");
        put("rt_batch_toon", "RoundtableToonModel", QJsonObject{{"model_name", clean.value("model_name")}});
        clean.insert("prepared_model", QJsonArray{"rt_batch_toon", 0});
        put(cleanup, type(cleanup), clean);
        QJsonObject video = inputs(create);
        video.insert("audio", QJsonArray{media, 1});
        video.insert("fps", QJsonArray{media, 2});
        put(create, type(create), video);
        output = targets.save;
        graph = alphaOutputPrompt(graph, targets);
    }
    // Keep only this phase's ancestors; otherwise ComfyUI also executes the
    // original output nodes and silently runs the entire workflow again.
    QJsonObject result;
    std::function<void(const QString&)> visit = [&](const QString& id) {
        if (result.contains(id) || !graph.contains(id)) return;
        const QJsonObject node = graph.value(id).toObject();
        result.insert(id, node);
        const QJsonObject in = node.value("inputs").toObject();
        for (auto it = in.begin(); it != in.end(); ++it) {
            const QJsonArray link = it.value().toArray();
            if (link.size() == 2 && link[0].isString()) visit(link[0].toString());
        }
    };
    visit(output);
    return result;
}

// ═════════════════════════════════════════════════════════════════════════
//  H3 swap
// ═════════════════════════════════════════════════════════════════════════

namespace {

bool isLink(const QJsonValue& value)
{
    const QJsonArray link = value.toArray();
    return link.size() == 2 && link[0].isString() && link[1].isDouble();
}

QString titleOf(const QJsonObject& node)
{
    return node.value("_meta").toObject().value("title").toString();
}

void setInput(QJsonObject& graph, const QString& id, const QString& name, const QJsonValue& value)
{
    QJsonObject node = graph.value(id).toObject();
    QJsonObject inputs = node.value("inputs").toObject();
    if (value.isUndefined()) inputs.remove(name); else inputs.insert(name, value);
    node.insert("inputs", inputs);
    graph.insert(id, node);
}

/// Every link to `from` (any output when `slot` < 0) becomes `to`; with an
/// undefined `to` the input is removed.
void relink(QJsonObject& graph, const QString& from, int slot, const QJsonValue& to)
{
    for (const QString& id : graph.keys()) {
        const QJsonObject inputs = graph.value(id).toObject().value("inputs").toObject();
        for (auto it = inputs.begin(); it != inputs.end(); ++it) {
            const QJsonArray link = it.value().toArray();
            if (isLink(link) && link[0].toString() == from && (slot < 0 || link[1].toInt() == slot))
                setInput(graph, id, it.key(), to);
        }
    }
}

/// The model pass whose cost a node carries (0 = cheap, rerun wherever needed).
int h3HeavyStage(const QString& type)
{
    if (type == QLatin1String("TextGenerate")) return 1;
    if (type == QLatin1String("SAM3_VideoTrack")) return 2;
    if (type == QLatin1String("MiniMaxH3ReferenceToVideo") || type == QLatin1String("H3SourceSlice") ||
        type == QLatin1String("MiniMaxH3FantasticVideoEditLatent") || type == QLatin1String("MiniMaxH3ChainContext"))
        return 3;
    if (type == QLatin1String("MiniMaxH3ChainAssemble")) return 4;
    return 0;
}

} // namespace

QJsonObject h3ShotPrompt(QJsonObject graph, const WorkflowSlots& targets, bool singleClip)
{
    if (targets.engine != Engine::H3) return graph;

    // Lip-sync on: the shots are dialogue. Openjutsu's export has No lip-sync
    // switched on; take its nodes out the way its dropNoLipSync() does.
    static const QRegularExpression mute(QStringLiteral(R"(^Mute the (clip|reference) audio H3 hears$)"));
    static const QRegularExpression noLipSync(
        QStringLiteral(R"(^(No lip-sync in the prompt|SAM: faces|SAM 3\.1 track faces|Faces \(shown on the mannequins?\))$)"));
    // The hat pass costs ~45 s a clip; Openjutsu has it off by default.
    static const QRegularExpression hat(QStringLiteral(R"(\bhats?\b)"), QRegularExpression::CaseInsensitiveOption);
    for (const QString& id : graph.keys()) {
        const QJsonObject node = graph.value(id).toObject();
        const QString title = titleOf(node);
        if (mute.match(title).hasMatch()) {
            relink(graph, id, -1, node.value("inputs").toObject().value("audio"));
            graph.remove(id);
        } else if (title == QLatin1String("People + hats")) {
            relink(graph, id, -1, node.value("inputs").toObject().value("destination"));
            graph.remove(id);
        }
    }
    for (const QString& id : graph.keys()) {
        const QString title = titleOf(graph.value(id).toObject());
        if (noLipSync.match(title).hasMatch() || hat.match(title).hasMatch()) {
            relink(graph, id, -1, QJsonValue(QJsonValue::Undefined));
            graph.remove(id);
        }
    }

    // The describer's sentence is all H3 reads about the character's look, and
    // Openjutsu's asks for no eye colour: H3 then repaints the eyes its own way.
    static const QRegularExpression describePicture(QStringLiteral(R"(^Describe picture \d+$)"));
    for (const QString& id : graph.keys()) {
        const QJsonObject node = graph.value(id).toObject();
        const QString type = node.value("class_type").toString();
        if (type == QLatin1String("GrowMaskWithBlur") && titleOf(node).startsWith(QLatin1String("Grow mask")))
            setInput(graph, id, QStringLiteral("expand"), kH3MaskGrow);
        else if (type == QLatin1String("TextGenerate") && describePicture.match(titleOf(node)).hasMatch()) {
            setInput(graph, id, QStringLiteral("prompt"), QString::fromLatin1(kH3DescribePrompt));
            setInput(graph, id, QStringLiteral("max_length"), kH3DescribeTokens);
        }
        // H3 frames reach ToonOut through 4:2:0 H.264, which tints thin dark
        // hair strands green; the hard green cut would delete them.
        else if (type == QLatin1String("RoundtableAlphaRepair"))
            setInput(graph, id, QStringLiteral("edge_mode"), QStringLiteral("unmix"));
    }

    // 24 fps, cropped to the SCAIL canvas before anything reads the frames,
    // so SAM, the masks, the mannequin and the render share one shape.
    if (graph.contains(targets.drivingVideo)) {
        setInput(graph, targets.drivingVideo, QStringLiteral("force_rate"), kH3Fps);
        const QString crop = QStringLiteral("rt_h3_size");
        relink(graph, targets.drivingVideo, 0, QJsonArray{crop, 0});
        graph.insert(crop, QJsonObject{{"class_type", "ImageScale"},
            {"inputs", QJsonObject{{"image", QJsonArray{targets.drivingVideo, 0}}, {"upscale_method", "bilinear"},
                                   {"width", kH3Width}, {"height", kH3Height}, {"crop", "center"}}},
            {"_meta", QJsonObject{{"title", "Roundtable size (centre crop)"}}}});

        // A shot's AAC track can end a frame before its picture; Source Timeline
        // then refuses the last frame ("audio window 0:56 exceeds 0:55"). Pad
        // every reader's soundtrack with silence to the loaded frames, as
        // Openjutsu's own loader does (silenceWhenSoundless in lib/comfy.ts).
        const QString soundtrack = QStringLiteral("rt_h3_soundtrack");
        bool heard = false;
        for (const QString& id : graph.keys()) {
            const QJsonObject inputs = graph.value(id).toObject().value("inputs").toObject();
            for (auto it = inputs.begin(); it != inputs.end(); ++it)
                if (it.value().toArray() == QJsonArray{targets.drivingVideo, 2}) heard = true;
        }
        if (heard) {
            relink(graph, targets.drivingVideo, 2, QJsonArray{soundtrack, 0});
            graph.insert(soundtrack, QJsonObject{{"class_type", "H3SoundtrackOrSilence"},
                {"inputs", QJsonObject{{"audio", QJsonArray{targets.drivingVideo, 2}},
                                       {"video_info", QJsonArray{targets.drivingVideo, 3}}}},
                {"_meta", QJsonObject{{"title", "Soundtrack (silence if none)"}}}});
        }
    }
    if (graph.contains(targets.runSettings)) {
        QJsonObject plan = graph.value(targets.runSettings).toObject().value("inputs").toObject();
        plan.insert("short_edge", std::min(kH3Width, kH3Height));
        if (singleClip) plan.insert("clip_seconds", 15.0);
        // The Swap Plan's seed is 32-bit; ours can be wider.
        if (plan.value("seed").isDouble())
            plan.insert("seed", static_cast<double>(static_cast<quint64>(plan.value("seed").toDouble()) % 0x100000000ULL));
        QJsonObject node = graph.value(targets.runSettings).toObject();
        node.insert("inputs", plan);
        graph.insert(targets.runSettings, node);
    }
    return graph;
}

QString h3StageName(int stage)
{
    static const char* names[] = {"", "described", "tracked", "encoded", "generated", "finished"};
    return stage > 0 && stage <= kH3StageCount ? QString::fromLatin1(names[stage]) : QString();
}

QJsonObject h3StagePrompt(const QJsonObject& prompt, const WorkflowSlots& targets, int stage, const QString& artifactKey)
{
    if (targets.engine != Engine::H3 || stage < 1 || stage > kH3StageCount || !prompt.contains(targets.save)) return {};
    auto heavy = [&](const QString& id) {
        return h3HeavyStage(prompt.value(id).toObject().value("class_type").toString());
    };
    auto stashName = [](const QString& id, int slot) {
        QString name = id + QLatin1Char('_') + QString::number(slot);
        static const QRegularExpression unsafe(QStringLiteral("[^A-Za-z0-9_]"));
        return name.replace(unsafe, QStringLiteral("_"));
    };

    // Build the passes from the last back: each one tells the earlier passes
    // which of their outputs it reads, so only those are written to disk.
    std::map<std::pair<QString, int>, int> needed;   // (node, output) → its pass
    for (int k = kH3StageCount; k >= stage; --k) {
        QJsonObject graph;
        bool ok = true;
        std::function<void(const QString&)> visit = [&](const QString& id) {
            if (graph.contains(id) || !ok) return;
            QJsonObject node = prompt.value(id).toObject();
            if (node.isEmpty()) { ok = false; return; }
            QJsonObject inputs = node.value("inputs").toObject();
            graph.insert(id, node);
            for (auto it = inputs.begin(); it != inputs.end(); ++it) {
                if (!isLink(it.value())) continue;
                const QJsonArray link = it.value().toArray();
                const QString source = link[0].toString();
                const int slot = link[1].toInt(), pass = heavy(source);
                if (pass > k) { ok = false; return; }   // a later model feeding an earlier pass
                if (pass > 0 && pass < k) {
                    needed[{source, slot}] = pass;
                    const QString load = QStringLiteral("rt_unstash_") + stashName(source, slot);
                    graph.insert(load, QJsonObject{{"class_type", "RoundtableUnstash"},
                        {"inputs", QJsonObject{{"artifact_key", artifactKey}, {"name", stashName(source, slot)}}}});
                    it.value() = QJsonArray{load, 0};
                } else {
                    visit(source);
                }
            }
            node.insert("inputs", inputs);
            graph.insert(id, node);
        };
        bool any = false;
        if (k == kH3StageCount) {
            visit(targets.save);
            any = true;
        } else {
            for (const auto& [output, pass] : needed) {
                if (pass != k) continue;
                any = true;
                graph.insert(QStringLiteral("rt_stash_") + stashName(output.first, output.second),
                    QJsonObject{{"class_type", "RoundtableStash"},
                        {"inputs", QJsonObject{{"value", QJsonArray{output.first, output.second}},
                                               {"artifact_key", artifactKey},
                                               {"name", stashName(output.first, output.second)},
                                               {"stage", h3StageName(k)}}}});
                visit(output.first);
            }
        }
        if (!ok || !any) return {};
        if (k == stage) return graph;
    }
    return {};
}

} // namespace rt::comfy
