/*
 * ComfyWorkflow.cpp — see header.
 */
#include "panels/comfy/ComfyWorkflow.h"

#include <QHash>
#include <QJsonArray>
#include <QRegularExpression>
#include <QSet>

#include <functional>
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

} // namespace rt::comfy
