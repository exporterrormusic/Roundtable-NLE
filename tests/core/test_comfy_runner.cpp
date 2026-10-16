/*
 * test_comfy_runner.cpp — ComfyRunner against an in-process fake ComfyUI
 * HTTP server: upload → /prompt → /history poll → /view download, the
 * quality-preserving retries, staged batches, and validation errors.
 */
#include <gtest/gtest.h>

#include "panels/comfy/ComfyRunner.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>

#include <iostream>
#include <memory>

using namespace rt;

namespace {

int g_argc = 1;
char g_arg0[] = "test_comfy_runner";
char* g_argv[] = {g_arg0, nullptr};

/// Just enough of ComfyUI's HTTP API for the runner.
class FakeComfy : public QObject
{
public:
    bool        alive{true};
    bool        rejectPrompts{false};
    bool        failCacheDownloads{false};
    int         failFirstN{0};            ///< the first N queued prompts end in execution_error
    QStringList uploads;                  ///< uploaded file names, in order
    QHash<QString, QImage> uploadedImages;
    QList<QJsonObject> prompts;           ///< API prompts received
    QList<QJsonObject> metadata;
    int         interrupts{0};
    int         frees{0};
    QJsonObject freeRequest;
    QStringList released;

    FakeComfy()
    {
        server.listen(QHostAddress::LocalHost, 0);
        connect(&server, &QTcpServer::newConnection, this, [this]() {
            while (QTcpSocket* s = server.nextPendingConnection()) {
                auto buffer = std::make_shared<QByteArray>();
                connect(s, &QTcpSocket::readyRead, this, [this, s, buffer]() {
                    buffer->append(s->readAll());
                    const int headerEnd = buffer->indexOf("\r\n\r\n");
                    if (headerEnd < 0) return;
                    const QByteArray head = buffer->left(headerEnd);
                    static const QRegularExpression lenRe(QStringLiteral("(?i)content-length:\\s*(\\d+)"));
                    const auto m = lenRe.match(QString::fromLatin1(head));
                    const int length = m.hasMatch() ? m.captured(1).toInt() : 0;
                    if (buffer->size() < headerEnd + 4 + length) return;
                    const QByteArray body = buffer->mid(headerEnd + 4, length);
                    const QList<QByteArray> requestLine = head.left(head.indexOf("\r\n")).split(' ');
                    handle(s, QString::fromLatin1(requestLine.value(0)), QString::fromLatin1(requestLine.value(1)), body);
                    buffer->clear();
                });
                connect(s, &QTcpSocket::disconnected, s, &QObject::deleteLater);
            }
        });
    }

    [[nodiscard]] QString url() const { return QStringLiteral("http://127.0.0.1:%1").arg(server.serverPort()); }

private:
    void reply(QTcpSocket* s, int code, const QByteArray& body, const char* type = "application/json")
    {
        QByteArray out = "HTTP/1.1 " + QByteArray::number(code) + (code == 200 ? " OK" : " Error") + "\r\n";
        out += "Content-Type: " + QByteArray(type) + "\r\nContent-Length: " + QByteArray::number(body.size()) +
               "\r\nConnection: close\r\n\r\n" + body;
        s->write(out);
        s->disconnectFromHost();
    }

    void handle(QTcpSocket* s, const QString& method, const QString& path, const QByteArray& body)
    {
        if (!alive) { s->abort(); return; }
        if (path == QLatin1String("/system_stats")) { reply(s, 200, "{}"); return; }
        if (path == QLatin1String("/upload/image")) {
            static const QRegularExpression nameRe(QStringLiteral("filename=\"([^\"]+)\""));
            const QString name = nameRe.match(QString::fromUtf8(body)).captured(1);
            uploads << name;
            const int dataStart = body.indexOf("\r\n\r\n") + 4;
            const int dataEnd = body.indexOf("\r\n--", dataStart);
            uploadedImages.insert(name, QImage::fromData(body.mid(dataStart, dataEnd - dataStart)));
            reply(s, 200, QJsonDocument(QJsonObject{{"name", name}, {"subfolder", ""}, {"type", "input"}}).toJson());
            return;
        }
        if (path == QLatin1String("/prompt") && method == QLatin1String("POST")) {
            if (rejectPrompts) {
                reply(s, 400, R"({"error":{"message":"Prompt outputs failed validation","details":""},
                                   "node_errors":{"16":{"class_type":"UNETLoader","errors":[{"message":"Value not in list","details":"unet_name: x"}]}}})");
                return;
            }
            prompts << QJsonDocument::fromJson(body).object().value("prompt").toObject();
            metadata << QJsonDocument::fromJson(body).object().value("extra_data").toObject().value("extra_pnginfo").toObject();
            reply(s, 200, QJsonDocument(QJsonObject{{"prompt_id", QString("p%1").arg(prompts.size())}}).toJson());
            return;
        }
        if (path.startsWith(QLatin1String("/history/"))) {
            const QString id = path.mid(9);
            const int n = id.mid(1).toInt();
            QJsonObject entry;
            if (n <= failFirstN) {
                // Real shape: [["execution_start", {...}], ["execution_error", {...}]]
                // (append, because QJsonArray{QJsonArray{...}} is a copy, not a nesting).
                QJsonArray messages;
                messages.append(QJsonArray{"execution_start", QJsonObject{}});
                messages.append(QJsonArray{"execution_error",
                    QJsonObject{{"node_type", "GAPSCAIL2LongVideo"},
                                {"exception_message", "Allocation on device"}}});
                entry = QJsonObject{{"status", QJsonObject{{"status_str", "error"}, {"completed", false},
                                                           {"messages", messages}}},
                                    {"outputs", QJsonObject{}}};
            } else {
                for (const auto& node : prompts[n - 1]) {
                    const auto type = node.toObject().value("class_type").toString();
                    if (type == "RoundtablePrepare" || type == "RoundtableGenerate") {
                        const auto in = node.toObject().value("inputs").toObject();
                        entry = QJsonObject{{"status", QJsonObject{{"status_str", "success"}, {"completed", true}}},
                            {"outputs", QJsonObject{{"23", QJsonObject{
                                {"roundtable_stage", QJsonArray{type == "RoundtablePrepare" ? "prepared" : "generated"}},
                                {"artifact_key", QJsonArray{in.value("artifact_key")}}}}}}};
                        reply(s, 200, QJsonDocument(QJsonObject{{id, entry}}).toJson());
                        return;
                    }
                }
                const bool alpha = prompts[n - 1].value("25").toObject().value("class_type") == "VHS_VideoCombine";
                entry = QJsonObject{{"status", QJsonObject{{"status_str", "success"}, {"completed", true}}},
                    {"outputs", QJsonObject{{"25", QJsonObject{{"images", QJsonArray{
                        QJsonObject{{"filename", QString("out_%1").arg(n) + (alpha ? ".mov" : ".mp4")}, {"subfolder", "roundtable"},
                                    {"type", "output"}}}}}}}}};
            }
            reply(s, 200, QJsonDocument(QJsonObject{{id, entry}}).toJson());
            return;
        }
        if (path.startsWith(QLatin1String("/view"))) {
            if (failCacheDownloads && path.contains("frames.pt")) { reply(s, 500, "cache unavailable"); return; }
            reply(s, 200, "FAKE-MP4-BYTES", "video/mp4"); return;
        }
        if (path == QLatin1String("/interrupt")) { ++interrupts; reply(s, 200, "{}"); return; }
        if (path == QLatin1String("/free")) { ++frees; freeRequest = QJsonDocument::fromJson(body).object(); reply(s, 200, "{}"); return; }
        if (path == QLatin1String("/roundtable_batch/release")) {
            released << QJsonDocument::fromJson(body).object().value("artifact_key").toString();
            reply(s, 200, "{}"); return;
        }
        reply(s, 404, "{}");
    }

    QTcpServer server;
};

class ComfyRunnerTest : public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        if (!QCoreApplication::instance())
            app = std::make_unique<QCoreApplication>(g_argc, g_argv);
    }

    void SetUp() override
    {
        ASSERT_TRUE(dir.isValid());
        for (const char* name : {"001_INTRO_WIDE_L1.mp4", "002_INTRO_CU_L2.mp4"}) {
            QFile f(dir.filePath(name));
            ASSERT_TRUE(f.open(QIODevice::WriteOnly));
            f.write("x");
        }
        QImage reference(64, 36, QImage::Format_ARGB32);
        reference.fill(QColor(40, 80, 120, 128));
        ASSERT_TRUE(reference.save(dir.filePath("wide.png")));
        ASSERT_TRUE(reference.save(dir.filePath("cu.png")));
    }

    ComfyRunner::Config config(const FakeComfy& fake) const
    {
        ComfyRunner::Config c;
        c.url = fake.url();
        c.prompt = QJsonObject{
            {"60", QJsonObject{{"class_type", "VHS_LoadVideo"}, {"inputs", QJsonObject{{"video", "W65.mp4"}, {"force_rate", 16}}}}},
            {"10", QJsonObject{{"class_type", "LoadImage"}, {"inputs", QJsonObject{{"image", "Export-Wide.png"}}}}},
            {"87", QJsonObject{{"class_type", "PrimitiveStringMultiline"}, {"inputs", QJsonObject{{"value", "calm"}}}}},
            {"84", QJsonObject{{"class_type", "GAPSCAIL2RunSettings"}, {"inputs", QJsonObject{{"seed", 153}}}}},
            {"25", QJsonObject{{"class_type", "SaveVideo"}, {"inputs", QJsonObject{{"filename_prefix", "video/x"}}}}}};
        c.targets.drivingVideo = "60";
        c.targets.referenceImage = "10";
        c.targets.expression = "87";
        c.targets.runSettings = "84";
        c.targets.save = "25";
        c.wideReference = dir.filePath("wide.png");
        c.closeUpReference = dir.filePath("cu.png");
        c.rendersDir = dir.filePath("renders");
        c.pollIntervalMs = 30;
        return c;
    }

    std::vector<RenderJob> twoJobs() const
    {
        RenderJob a;
        a.key = "welcome";
        a.order = 1;
        a.label = "001 · INTRO · WIDE";
        a.drivingVideo = dir.filePath("001_INTRO_WIDE_L1.mp4");
        a.expression = "a smug grin";
        a.seed = 4242;
        a.fps = 16;
        a.frames = 40;
        RenderJob b = a;
        b.key = "debate";
        b.order = 2;
        b.label = "002 · INTRO · CU";
        b.drivingVideo = dir.filePath("002_INTRO_CU_L2.mp4");
        b.closeUp = true;
        b.expression = QString();   // keep the workflow's expression
        b.seed = -1;                // keep the workflow's seed
        return {a, b};
    }

    static bool waitFinished(ComfyRunner& runner, bool* stopped = nullptr)
    {
        bool done = false;
        QObject::connect(&runner, &ComfyRunner::finished, [&](int, int, bool s) {
            done = true;
            if (stopped) *stopped = s;
        });
        return QTest::qWaitFor([&]() { return done; }, 20000);
    }

    static QJsonObject inputs(const QJsonObject& prompt, const char* id)
    {
        return prompt.value(id).toObject().value("inputs").toObject();
    }

    QTemporaryDir dir;
    static std::unique_ptr<QCoreApplication> app;
};

std::unique_ptr<QCoreApplication> ComfyRunnerTest::app;

} // namespace

TEST_F(ComfyRunnerTest, RendersEveryShotAndFillsTheWorkflow)
{
    FakeComfy fake;
    ComfyRunner runner;
    QStringList rendered;
    QObject::connect(&runner, &ComfyRunner::jobRendered, [&](int, const QString& f) { rendered << f; });
    runner.start(config(fake), twoJobs());
    bool stopped = true;
    ASSERT_TRUE(waitFinished(runner, &stopped));
    EXPECT_FALSE(stopped);

    ASSERT_EQ(runner.jobs().size(), 2u);
    for (const auto& job : runner.jobs()) {
        EXPECT_EQ(job.state, RenderJob::State::Done) << job.status.toStdString();
        EXPECT_EQ(job.attempt, 0);
    }
    ASSERT_EQ(rendered.size(), 2);
    for (const QString& f : rendered) {
        QFile file(f);
        ASSERT_TRUE(file.open(QIODevice::ReadOnly)) << f.toStdString();
        EXPECT_EQ(file.readAll(), QByteArray("FAKE-MP4-BYTES"));
    }
    EXPECT_TRUE(QFileInfo(rendered[0]).fileName().startsWith("001_INTRO_WIDE_L1_s4242_16fps"));

    // Each reference image uploaded once, then each driving clip.
    EXPECT_EQ(fake.uploads, (QStringList{"rt_ref_wide_wide.png", "001_INTRO_WIDE_L1.mp4",
                                         "rt_ref_closeup_cu.png", "002_INTRO_CU_L2.mp4"}));
    ASSERT_EQ(fake.prompts.size(), 2);
    const QJsonObject& p1 = fake.prompts[0];
    EXPECT_EQ(inputs(p1, "60").value("video").toString(), "001_INTRO_WIDE_L1.mp4");
    EXPECT_EQ(inputs(p1, "60").value("force_rate").toInt(), 16);
    EXPECT_EQ(inputs(p1, "10").value("image").toString(), "rt_ref_wide_wide.png");
    EXPECT_EQ(inputs(p1, "87").value("value").toString(), "a smug grin");
    EXPECT_EQ(inputs(p1, "84").value("seed").toInteger(), 4242);
    EXPECT_EQ(inputs(p1, "25").value("filename_prefix").toString(), "roundtable/001_INTRO_WIDE_L1");
    const QJsonObject& p2 = fake.prompts[1];
    EXPECT_EQ(inputs(p2, "10").value("image").toString(), "rt_ref_closeup_cu.png");
    EXPECT_EQ(inputs(p2, "87").value("value").toString(), "calm");   // workflow's
    EXPECT_EQ(inputs(p2, "84").value("seed").toInt(), 153);          // workflow's
    EXPECT_TRUE(QTest::qWaitFor([&]() { return fake.frees == 1; }, 3000));   // VRAM back to editing
}

TEST_F(ComfyRunnerTest, ReferencesAreCappedAt1080pBeforeUploadWithoutChangingOriginals)
{
    QImage large(3840, 2160, QImage::Format_ARGB32);
    large.fill(QColor(40, 80, 120, 128));
    ASSERT_TRUE(large.save(dir.filePath("wide.png")));
    FakeComfy fake;
    ComfyRunner runner;
    runner.start(config(fake), twoJobs());
    ASSERT_TRUE(waitFinished(runner));
    const QImage wide = fake.uploadedImages.value("rt_ref_wide_wide.png");
    ASSERT_FALSE(wide.isNull());
    EXPECT_EQ(wide.size(), QSize(1920, 1080));
    EXPECT_EQ(wide.pixelColor(960, 540).alpha(), 128);
    EXPECT_EQ(fake.uploadedImages.value("rt_ref_closeup_cu.png").size(), QSize(64, 36));
    EXPECT_EQ(QImage(dir.filePath("wide.png")), large);
}

TEST_F(ComfyRunnerTest, StagedBatchGroupsReferencesWithoutChangingShotIdentity)
{
    FakeComfy fake;
    auto c = config(fake);
    c.uiWorkflow = QJsonObject{{"nodes", QJsonArray{}}};
    auto node = [](const char* type, QJsonObject in) { return QJsonObject{{"class_type", type}, {"inputs", in}}; };
    c.prompt.insert("16", node("UNETLoader", {{"unet_name", "wan.safetensors"}}));
    c.prompt.insert("19", node("CLIPLoader", {{"clip_name", "text.safetensors"}}));
    c.prompt.insert("20", node("VAELoader", {{"vae_name", "vae.safetensors"}}));
    c.prompt.insert("15", node("GAPMultiCharacterReference", {{"image_1", QJsonArray{"10", 0}}}));
    c.prompt.insert("22", node("CLIPVisionEncode", {{"image", QJsonArray{"15", 2}}}));
    c.prompt.insert("23", node("GAPSCAIL2LongVideo", {
        {"model", QJsonArray{"16", 0}}, {"clip", QJsonArray{"19", 0}}, {"vae", QJsonArray{"20", 0}},
        {"reference_image", QJsonArray{"15", 0}}, {"reference_image_mask", QJsonArray{"15", 1}},
        {"character_count", QJsonArray{"15", 3}}, {"clip_vision_output", QJsonArray{"22", 0}},
        {"width", 1280}, {"height", 704}, {"replacement_mode", false},
        {"seed", QJsonArray{"84", 2}}, {"base_prompt", QJsonArray{"87", 0}},
        {"pose_video", QJsonArray{"60", 0}}, {"steps", 8}}));
    c.prompt.insert("91", node("ToonOut Selected BG JNK", {{"image", QJsonArray{"23", 0}}, {"model_name", "toon.pth"}}));
    c.prompt.insert("24", node("CreateVideo", {{"images", QJsonArray{"91", 0}}, {"audio", QJsonArray{"60", 2}}, {"fps", 16.0}}));
    c.prompt.insert("25", node("SaveVideo", {{"video", QJsonArray{"24", 0}}, {"filename_prefix", "video/x"}}));
    auto jobs = twoJobs();
    std::swap(jobs[0], jobs[1]); // script starts close-up, rendering starts wide
    jobs.push_back(jobs[1]);
    jobs[2].key = "second-wide";
    jobs[2].seed = 77;
    jobs[2].expression = "serious";
    ComfyRunner runner;
    QList<int> delivered;
    const auto deliveryConnection = QObject::connect(&runner, &ComfyRunner::jobRendered, [&](int index, const QString&) {
        delivered << index;
        EXPECT_EQ(fake.prompts.size() - delivered.size(), 6); // no delivery before both earlier passes
    });
    runner.start(c, jobs);
    ASSERT_TRUE(waitFinished(runner));
    ASSERT_EQ(fake.prompts.size(), 9);
    EXPECT_EQ(delivered, (QList<int>{1, 2, 0}));
    const int order[] = {1, 2, 0};
    for (int i = 0; i < 3; ++i) {
        const auto& job = runner.jobs()[order[i]];
        EXPECT_EQ(job.key, jobs[order[i]].key);
        EXPECT_TRUE(job.prepared);
        EXPECT_TRUE(job.generated);
        EXPECT_EQ(job.state, RenderJob::State::Done);
        EXPECT_EQ(job.fpsUsed, 16);
        const auto prep = inputs(fake.prompts[i], "23");
        EXPECT_EQ(prep.value("artifact_key").toString(), job.artifactKey);
        EXPECT_EQ(inputs(fake.prompts[i + 3], "23").value("artifact_key").toString(), job.artifactKey);
        EXPECT_EQ(inputs(fake.prompts[i + 6], "rt_batch_media").value("artifact_key").toString(), job.artifactKey);
        EXPECT_EQ(prep.value("audio"), QJsonValue(QJsonArray{"60", 2}));
        EXPECT_EQ(prep.value("steps").toInt(), 8);
        EXPECT_FALSE(fake.prompts[i].contains("16"));
        EXPECT_FALSE(fake.prompts[i + 3].contains("19"));
        EXPECT_FALSE(fake.prompts[i + 3].contains("60"));
        EXPECT_FALSE(fake.prompts[i + 6].contains("16"));
        EXPECT_EQ(inputs(fake.prompts[i + 6], "25").value("audio"), QJsonValue(QJsonArray{"rt_batch_media", 1}));
        EXPECT_EQ(inputs(fake.prompts[i + 6], "25").value("profile"), "4444");
        EXPECT_EQ(inputs(fake.prompts[i + 6], "91").value("background_type"), "transparent");
        EXPECT_EQ(inputs(fake.prompts[i + 6], "rt_alpha_repair").value("image"), QJsonValue(QJsonArray{"91", 0}));
        EXPECT_TRUE(job.outputFile.endsWith(".mov"));
        const QString cache = QDir(c.rendersDir).absoluteFilePath("../ComfyUI Cache/" + QFileInfo(job.outputFile).completeBaseName());
        EXPECT_TRUE(QFileInfo::exists(cache + "/frames.pt"));
        EXPECT_TRUE(QFileInfo::exists(cache + "/media.pt"));
        EXPECT_TRUE(QFileInfo::exists(cache + "/recipe.json"));
        const auto original = fake.metadata[i + 6].value("roundtable_source_prompt").toObject();
        EXPECT_EQ(inputs(original, "23").value("steps").toInt(), 8);
        EXPECT_EQ(inputs(original, "60").value("video").toString(), QFileInfo(job.drivingVideo).fileName());
    }
    EXPECT_EQ(inputs(fake.prompts[0], "10").value("image").toString(), "rt_ref_wide_wide.png");
    EXPECT_EQ(inputs(fake.prompts[2], "10").value("image").toString(), "rt_ref_closeup_cu.png");
    ASSERT_TRUE(QTest::qWaitFor([&]() { return fake.frees == 1 && fake.released.size() == 3; }, 3000));
    EXPECT_FALSE(fake.freeRequest.value("free_memory").toBool());
    EXPECT_TRUE(fake.freeRequest.value("unload_models").toBool());
    EXPECT_EQ(fake.uploads.size(), 5); // two references and three driving clips, uploaded once
    // Even one shot uses the staged path so its raw frames can be retained.
    QObject::disconnect(deliveryConnection);
    FakeComfy single;
    c.url = single.url();
    runner.start(c, {jobs.front()});
    ASSERT_TRUE(waitFinished(runner));
    ASSERT_EQ(single.prompts.size(), 3);
    EXPECT_TRUE(runner.jobs().front().staged);
    FakeComfy cacheFailure;
    cacheFailure.failCacheDownloads = true;
    c.url = cacheFailure.url();
    runner.start(c, {jobs.front()});
    ASSERT_TRUE(waitFinished(runner));
    EXPECT_TRUE(QFileInfo::exists(runner.jobs().front().outputFile));
    EXPECT_TRUE(cacheFailure.released.empty()); // never delete the sole raw copy after an archive error
}

TEST_F(ComfyRunnerTest, LiveStagedBatch)
{
    const QString folder = qEnvironmentVariable("RT_COMFY_BATCH_PROBE");
    if (folder.isEmpty()) GTEST_SKIP() << "Set RT_COMFY_BATCH_PROBE to an isolated test folder.";
    QFile specification(folder + "/jobs.json");
    ASSERT_TRUE(specification.open(QIODevice::ReadOnly));
    const auto spec = QJsonDocument::fromJson(specification.readAll()).object();
    QFile workflow(spec.value("workflow").toString());
    ASSERT_TRUE(workflow.open(QIODevice::ReadOnly));
    const auto ui = QJsonDocument::fromJson(workflow.readAll()).object();
    const auto converted = comfy::convertUiWorkflow(ui);
    ASSERT_TRUE(converted.ok());
    ASSERT_FALSE(comfy::batchStagePrompt(converted.prompt, converted.targets, comfy::BatchStage::Prepare,
                                        "00000000000000000000000000000000_0").isEmpty());
    ComfyRunner::Config c;
    c.prompt = converted.prompt;
    c.uiWorkflow = ui;
    c.targets = converted.targets;
    c.wideReference = spec.value("wide").toString();
    c.closeUpReference = spec.value("close").toString();
    c.rendersDir = folder + "/renders";
    c.filenamePrefix = "diagnostics/roundtable_batch";
    c.pollIntervalMs = 500;
    std::vector<RenderJob> jobs;
    for (const auto& value : spec.value("jobs").toArray()) {
        const auto item = value.toObject();
        RenderJob j;
        j.key = item.value("key").toString().toStdString();
        j.label = QString::fromStdString(j.key);
        j.drivingVideo = item.value("video").toString();
        j.closeUp = item.value("close").toBool();
        j.expression = item.value("expression").toString();
        j.seed = item.value("seed").toInteger();
        j.fps = 16;
        jobs.push_back(j);
    }
    ComfyRunner runner;
    bool done = false;
    QObject::connect(&runner, &ComfyRunner::log, [&](const QString& message) {
        std::cout << message.toStdString() << std::endl;
    });
    QObject::connect(&runner, &ComfyRunner::finished, [&](int rendered, int failed, bool stopped) {
        EXPECT_FALSE(stopped);
        EXPECT_EQ(failed, 0);
        EXPECT_EQ(rendered, jobs.size());
        done = true;
    });
    runner.start(c, jobs);
    ASSERT_TRUE(QTest::qWaitFor([&]() { return done; }, 45 * 60 * 1000));
    for (const auto& job : runner.jobs()) {
        EXPECT_TRUE(job.prepared);
        EXPECT_TRUE(job.generated);
        EXPECT_EQ(job.state, RenderJob::State::Done);
        std::cout << "OUTPUT " << job.key << " " << job.outputFile.toStdString() << std::endl;
    }
}

TEST_F(ComfyRunnerTest, PortraitReferenceKeepsAspectRatioInside1080pBounds)
{
    QImage portrait(1800, 2400, QImage::Format_RGB32);
    portrait.fill(Qt::red);
    ASSERT_TRUE(portrait.save(dir.filePath("wide.png")));
    FakeComfy fake;
    ComfyRunner runner;
    auto jobs = twoJobs();
    jobs.resize(1);
    runner.start(config(fake), jobs);
    ASSERT_TRUE(waitFinished(runner));
    EXPECT_EQ(fake.uploadedImages.value("rt_ref_wide_wide.png").size(), QSize(810, 1080));
}

TEST_F(ComfyRunnerTest, UnreadableReferenceStopsBeforeSubmittingRender)
{
    QFile invalid(dir.filePath("wide.png"));
    ASSERT_TRUE(invalid.open(QIODevice::WriteOnly | QIODevice::Truncate));
    invalid.write("not an image");
    invalid.close();
    FakeComfy fake;
    ComfyRunner runner;
    runner.start(config(fake), twoJobs());
    ASSERT_TRUE(waitFinished(runner));
    EXPECT_TRUE(fake.uploads.isEmpty());
    EXPECT_TRUE(fake.prompts.isEmpty());
}

TEST_F(ComfyRunnerTest, RetriesPreserveRequestedQuality)
{
    FakeComfy fake;
    fake.failFirstN = 2;
    ComfyRunner runner;
    QStringList log;
    QObject::connect(&runner, &ComfyRunner::log, [&](const QString& l) { log << l; });
    auto jobs = twoJobs();
    jobs.resize(1);
    runner.start(config(fake), jobs);
    ASSERT_TRUE(waitFinished(runner));

    const RenderJob& job = runner.jobs()[0];
    EXPECT_EQ(job.state, RenderJob::State::Done);
    EXPECT_EQ(job.attempt, 2);
    EXPECT_EQ(job.fpsUsed, 16);
    ASSERT_EQ(fake.prompts.size(), 3);
    EXPECT_EQ(inputs(fake.prompts[0], "60").value("force_rate").toInt(), 16);
    EXPECT_EQ(inputs(fake.prompts[1], "60").value("force_rate").toInt(), 16);
    EXPECT_EQ(inputs(fake.prompts[2], "60").value("force_rate").toInt(), 16);
    EXPECT_TRUE(log.join('\n').contains("Allocation on device"));
}

TEST_F(ComfyRunnerTest, SkipsAShotAfterThreeFailuresAndCarriesOn)
{
    FakeComfy fake;
    fake.failFirstN = 3;   // job 1 fails every attempt; job 2 is fine
    ComfyRunner runner;
    runner.start(config(fake), twoJobs());
    ASSERT_TRUE(waitFinished(runner));
    EXPECT_EQ(runner.jobs()[0].state, RenderJob::State::Failed);
    EXPECT_TRUE(runner.jobs()[0].status.contains("Allocation on device"));
    EXPECT_EQ(runner.jobs()[1].state, RenderJob::State::Done);
    EXPECT_EQ(fake.prompts.size(), 4);
}

TEST_F(ComfyRunnerTest, ValidationErrorStopsTheBatch)
{
    FakeComfy fake;
    fake.rejectPrompts = true;
    ComfyRunner runner;
    bool stopped = false;
    runner.start(config(fake), twoJobs());
    ASSERT_TRUE(waitFinished(runner, &stopped));
    EXPECT_TRUE(stopped);
    EXPECT_EQ(runner.jobs()[0].state, RenderJob::State::Failed);
    EXPECT_TRUE(runner.jobs()[0].status.contains("unet_name")) << runner.jobs()[0].status.toStdString();
    EXPECT_EQ(runner.jobs()[1].state, RenderJob::State::Pending);   // never tried
}

TEST_F(ComfyRunnerTest, NoServerAndNoLauncherStopsInsteadOfRetrying)
{
    FakeComfy fake;
    fake.alive = false;
    ComfyRunner runner;
    bool stopped = false;
    runner.start(config(fake), twoJobs());
    ASSERT_TRUE(waitFinished(runner, &stopped));
    EXPECT_TRUE(stopped);
    EXPECT_TRUE(runner.jobs()[0].status.contains("no launcher"));
    EXPECT_TRUE(fake.prompts.isEmpty());
}

TEST_F(ComfyRunnerTest, LauncherRunsWithoutOpeningABrowser)
{
    const QString bat = dir.filePath("run_nvidia_gpu.bat");
    QFile f(bat);
    ASSERT_TRUE(f.open(QIODevice::WriteOnly));
    f.write(".\\python_embeded\\python.exe -s ComfyUI\\main.py --windows-standalone-build --disable-cuda-malloc\r\n"
            "echo If you see this and ComfyUI did not start try updating main.py things\r\n"
            "pause\r\n");
    f.close();
    EXPECT_EQ(ComfyRunner::serverCommandFromLauncher(bat),
              ".\\python_embeded\\python.exe -s ComfyUI\\main.py --windows-standalone-build "
              "--disable-cuda-malloc --disable-auto-launch");

    QFile g(bat);
    ASSERT_TRUE(g.open(QIODevice::WriteOnly));
    g.write("python main.py --auto-launch --port 8190\r\n");
    g.close();
    EXPECT_EQ(ComfyRunner::serverCommandFromLauncher(bat), "python main.py --port 8190 --disable-auto-launch");

    EXPECT_TRUE(ComfyRunner::serverCommandFromLauncher(dir.filePath("missing.bat")).isEmpty());
}

TEST_F(ComfyRunnerTest, SecondStartWaitsForTheServerAlreadyStarting)
{
    // Regression: a second start while ComfyUI was still loading discarded the
    // first QProcess (killing only cmd.exe), orphaning python with a dead
    // stdout — its custom node packs then failed to import.
    QTcpServer probe;
    ASSERT_TRUE(probe.listen(QHostAddress::LocalHost));
    const QString url = QStringLiteral("http://127.0.0.1:%1").arg(probe.serverPort());
    probe.close();   // nothing answers: the "server" never comes up

    const QString bat = dir.filePath("slow_start.bat");
    QFile f(bat);
    ASSERT_TRUE(f.open(QIODevice::WriteOnly));
    f.write("@ping -n 30 127.0.0.1 > nul\r\n");   // stays running, like a loading ComfyUI
    f.close();

    ComfyRunner runner;
    QStringList log;
    QObject::connect(&runner, &ComfyRunner::log, [&](const QString& l) { log << l; });
    runner.launchServer(url, bat);
    ASSERT_TRUE(QTest::qWaitFor([&]() { return log.join('\n').contains("Starting ComfyUI"); }, 5000));
    runner.launchServer(url, bat);
    ASSERT_TRUE(QTest::qWaitFor([&]() { return log.join('\n').contains("still starting"); }, 5000));
    EXPECT_EQ(log.filter("Starting ComfyUI").size(), 1);
}   // runner dtor ends the process tree it launched

TEST_F(ComfyRunnerTest, LauncherCdLineSetsServerWorkingDirectory)
{
    // A launcher kept outside the ComfyUI folder: its "cd /d" line (before
    // main.py) is where the server must run, like double-clicking the .bat.
    QDir root(dir.path());
    ASSERT_TRUE(root.mkpath("portable"));
    const QString portable = QDir::cleanPath(root.filePath("portable"));
    const QString bat = dir.filePath("roundtable_comfyui.bat");
    QFile f(bat);
    ASSERT_TRUE(f.open(QIODevice::WriteOnly));
    f.write(QString("@echo off\r\ncd /d \"%1\"\r\n%1\\python.exe -s %1\\main.py\r\ncd /d C:\\\r\n")
                .arg(QDir::toNativeSeparators(portable)).toLocal8Bit());
    f.close();
    EXPECT_EQ(QDir::cleanPath(ComfyRunner::serverWorkingDirFromLauncher(bat)), portable);

    // No cd line (stock run_nvidia_gpu.bat): the launcher's own folder.
    QFile g(bat);
    ASSERT_TRUE(g.open(QIODevice::WriteOnly));
    g.write(".\\python_embeded\\python.exe -s ComfyUI\\main.py\r\npause\r\n");
    g.close();
    EXPECT_EQ(QDir::cleanPath(ComfyRunner::serverWorkingDirFromLauncher(bat)), QDir::cleanPath(dir.path()));
}

TEST_F(ComfyRunnerTest, StopInterruptsTheRunningRender)
{
    FakeComfy fake;
    ComfyRunner runner;
    auto cfg = config(fake);
    cfg.pollIntervalMs = 60000;   // stay "rendering"
    runner.start(cfg, twoJobs());
    ASSERT_TRUE(QTest::qWaitFor([&]() { return fake.prompts.size() == 1; }, 10000));
    bool stopped = false;
    QObject::connect(&runner, &ComfyRunner::finished, [&](int, int, bool s) { stopped = s; });
    runner.stop();
    EXPECT_TRUE(stopped);
    EXPECT_FALSE(runner.isRunning());
    EXPECT_TRUE(QTest::qWaitFor([&]() { return fake.interrupts == 1; }, 3000));
}

// ─── ComfyUI event feed (WebSocket frames) ─────────────────────────────────

#include "panels/comfy/ComfyEventStream.h"

#include <QBuffer>
#include <QImage>
#include <QtEndian>

namespace {
QByteArray wsFrame(int opcode, const QByteArray& payload, bool fin = true)
{
    QByteArray f;
    f.append(static_cast<char>((fin ? 0x80 : 0x00) | opcode));
    if (payload.size() < 126) {
        f.append(static_cast<char>(payload.size()));
    } else {
        f.append(static_cast<char>(126));
        char len[2];
        qToBigEndian<quint16>(static_cast<quint16>(payload.size()), len);
        f.append(len, 2);
    }
    return f + payload;
}
} // namespace

TEST(ComfyEventStreamTest, ParsesProgressExecutingAndPreviewFrames)
{
    static int argc = 1;
    static char arg0[] = "test_comfy_runner";
    static char* argv[] = {arg0, nullptr};
    if (!QCoreApplication::instance()) new QCoreApplication(argc, argv);   // image plugins (JPEG)
    rt::ComfyEventStream stream;
    QStringList progress, executing;
    QImage preview;
    QObject::connect(&stream, &rt::ComfyEventStream::progress,
                     [&](const QString& id, const QString& node, int v, int max) {
        progress << QString("%1 %2 %3/%4").arg(id, node).arg(v).arg(max);
    });
    QObject::connect(&stream, &rt::ComfyEventStream::executing,
                     [&](const QString& id, const QString& node) { executing << id + ":" + node; });
    QObject::connect(&stream, &rt::ComfyEventStream::previewImage, [&](const QImage& img) { preview = img; });

    // Handshake response + first frame arrive split across reads.
    const QByteArray p1 = wsFrame(0x1, R"({"type":"progress","data":{"value":3,"max":8,"prompt_id":"abc","node":"23"}})");
    stream.feed("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n\r\n" + p1.left(5));
    EXPECT_TRUE(progress.isEmpty());
    stream.feed(p1.mid(5));
    ASSERT_EQ(progress.size(), 1);
    EXPECT_EQ(progress[0], "abc 23 3/8");

    // Fragmented text message; "executing" with node null = finished.
    const QByteArray msg = R"({"type":"executing","data":{"node":null,"prompt_id":"abc"}})";
    stream.feed(wsFrame(0x1, msg.left(10), /*fin=*/false) + wsFrame(0x0, msg.mid(10)));
    ASSERT_EQ(executing.size(), 1);
    EXPECT_EQ(executing[0], "abc:");

    // Binary PREVIEW_IMAGE (type 1, format 2 = PNG), > 125 bytes → 16-bit length.
    QImage img(32, 18, QImage::Format_RGB32);
    img.fill(Qt::green);
    QByteArray png;
    QBuffer buf(&png);
    buf.open(QIODevice::WriteOnly);
    img.save(&buf, "PNG");
    char head[8];
    qToBigEndian<quint32>(1, head);
    qToBigEndian<quint32>(2, head + 4);
    const QByteArray binary = QByteArray(head, 8) + png;
    stream.feed(wsFrame(0x2, binary));
    ASSERT_FALSE(preview.isNull());
    EXPECT_EQ(preview.size(), QSize(32, 18));
    // VideoHelperSuite animated preview: [1][1][1][frame index][16-byte node id][JPEG].
    QImage vhs(40, 22, QImage::Format_RGB32);
    vhs.fill(Qt::red);
    QByteArray jpg;
    QBuffer jbuf(&jpg);
    jbuf.open(QIODevice::WriteOnly);
    ASSERT_TRUE(vhs.save(&jbuf, "JPEG"));
    char vhsHead[16];
    qToBigEndian<quint32>(1, vhsHead);        // event PREVIEW_IMAGE
    qToBigEndian<quint32>(1, vhsHead + 4);
    qToBigEndian<quint32>(1, vhsHead + 8);
    qToBigEndian<quint32>(7, vhsHead + 12);   // frame index
    QByteArray nodeId(16, 0);
    nodeId[0] = 2;
    nodeId[1] = '2';
    nodeId[2] = '3';
    preview = QImage();
    stream.feed(wsFrame(0x2, QByteArray(vhsHead, 16) + nodeId + jpg));
    ASSERT_FALSE(preview.isNull());
    EXPECT_EQ(preview.size(), QSize(40, 22));

    // KJNodes preview override: JSON text, base64 JPEG (no mime = still)...
    preview = QImage();
    stream.feed(wsFrame(0x1, R"({"type":"kj_preview_override","data":{"node_id":"85","image":")" +
                                 jpg.toBase64() + R"(","w":40,"h":22}})"));
    ASSERT_FALSE(preview.isNull());
    EXPECT_EQ(preview.size(), QSize(40, 22));
    // ...or an animated clip handed on as bytes + fps.
    QByteArray clip, clipMime;
    double clipFps = 0.0;
    QObject::connect(&stream, &rt::ComfyEventStream::previewClip,
                     [&](const QByteArray& d, const QString& m, double f) {
        clip = d;
        clipMime = m.toLatin1();
        clipFps = f;
    });
    const QByteArray fakeMp4("\x00\x00\x00\x18" "ftypmp42 fake clip bytes", 28);
    stream.feed(wsFrame(0x1, R"({"type":"kj_preview_override","data":{"image":")" + fakeMp4.toBase64() +
                                 R"(","mime":"video/mp4","fps":2,"step":3,"total":8}})"));
    EXPECT_EQ(clip, fakeMp4);
    EXPECT_EQ(clipMime, "video/mp4");
    EXPECT_DOUBLE_EQ(clipFps, 2.0);
}

// Live check against a running ComfyUI (skipped unless ROUNDTABLE_LIVE_COMFY
// is set, e.g. "http://127.0.0.1:8188").  Confirms the hand-rolled WebSocket
// client really talks to ComfyUI's /ws.
TEST(ComfyEventStreamTest, LiveComfyUiHandshakeAndStatus)
{
    const QString url = qEnvironmentVariable("ROUNDTABLE_LIVE_COMFY");
    if (url.isEmpty()) GTEST_SKIP() << "set ROUNDTABLE_LIVE_COMFY to a running ComfyUI";
    static int argc = 1;
    static char arg0[] = "test_comfy_runner";
    static char* argv[] = {arg0, nullptr};
    if (!QCoreApplication::instance()) new QCoreApplication(argc, argv);   // sockets need an event loop
    rt::ComfyEventStream stream;
    QStringList types;
    QObject::connect(&stream, &rt::ComfyEventStream::textMessage, [&](const QString& t) { types << t; });
    stream.open(QUrl(url), QStringLiteral("roundtable-live-test"));
    EXPECT_TRUE(QTest::qWaitFor([&]() { return types.contains("status"); }, 8000))
        << "received: " << types.join(",").toStdString();
    EXPECT_TRUE(stream.isConnected());
}
