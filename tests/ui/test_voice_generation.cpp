#include <gtest/gtest.h>

#include "AudioMixdown.h"
#include "audio/AudioFile.h"
#include "command/CommandStack.h"
#include "panels/audio/AudioSync.h"
#include "panels/audio/AudioSyncFileName.h"
#include "panels/audio/VoiceGenerationPanel.h"
#include "panels/audio/VoiceGenerationService.h"
#include "panels/audio/VoiceReferenceLibrary.h"
#include "widgets/ManualMatchDialog.h"

#include <QApplication>
#include <QClipboard>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QSettings>
#include <QTest>
#include <QTemporaryDir>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QTextEdit>
#include <QComboBox>
#include <QElapsedTimer>
#include <QAction>
#include <QPushButton>
#include <QGroupBox>
#include <QLabel>
#include <QMessageBox>
#include <QTimer>

#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>

namespace {

int g_argc = 1;
char g_arg0[] = "test_voice_generation";
char* g_argv[] = {g_arg0, nullptr};

class VoiceGenerationTest : public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        QStandardPaths::setTestModeEnabled(true);
        if (!QApplication::instance())
            app = std::make_unique<QApplication>(g_argc, g_argv);
        settingsDirectory = std::make_unique<QTemporaryDir>();
        ASSERT_TRUE(settingsDirectory->isValid());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                           settingsDirectory->path());
    }

    static QString makeTone(const QString& directory, const QString& name,
                            double seconds)
    {
        constexpr uint32_t sampleRate = 44100;
        rt::MixdownResult result;
        result.sampleRate = sampleRate;
        result.channels = 1;
        result.totalFrames = static_cast<int64_t>(seconds * sampleRate);
        result.duration = seconds;
        result.samples.resize(static_cast<size_t>(result.totalFrames));
        for (int64_t i = 0; i < result.totalFrames; ++i)
            result.samples[static_cast<size_t>(i)] =
                0.15f * std::sin(2.0 * 3.141592653589793 * 220.0 * i / sampleRate);
        const QString path = directory + QStringLiteral("/") + name;
        EXPECT_TRUE(rt::AudioMixdown::writeWav(
            result, std::filesystem::path(path.toStdWString())));
        return path;
    }

    static std::unique_ptr<QApplication> app;
    static std::unique_ptr<QTemporaryDir> settingsDirectory;
};

std::unique_ptr<QApplication> VoiceGenerationTest::app;
std::unique_ptr<QTemporaryDir> VoiceGenerationTest::settingsDirectory;

rt::SyncClip clip(int id, const QString& path, const char* character,
                  double start, double end, int matchState, const char* text)
{
    rt::SyncClip value;
    value.id = id;
    value.sourceFile = path.toUtf8().toStdString();
    value.character = character;
    value.start = start;
    value.end = end;
    value.transcript = text;
    value.editedText = text;
    value.matchState = matchState;
    value.confidence = matchState == 2 ? 1.0f : 0.75f;
    value.scriptLineNumber = matchState == 0 ? -1 : id;
    return value;
}

class ScopedCrisperConsentSettings
{
public:
    ScopedCrisperConsentSettings()
        : oldOrganization(QCoreApplication::organizationName())
        , oldApplication(QCoreApplication::applicationName())
    {
        QCoreApplication::setOrganizationName(QStringLiteral("RoundtableTests"));
        QCoreApplication::setApplicationName(QStringLiteral("CrisperConsentTests"));
        settings = std::make_unique<QSettings>();
        hadConsent = settings->contains(consentKey);
        oldConsent = settings->value(consentKey);
        hadLegacy = settings->contains(legacyKey);
        oldLegacy = settings->value(legacyKey);
        settings->remove(consentKey);
        settings->remove(legacyKey);
        settings->sync();
    }

    ~ScopedCrisperConsentSettings()
    {
        if (hadConsent) settings->setValue(consentKey, oldConsent);
        else settings->remove(consentKey);
        if (hadLegacy) settings->setValue(legacyKey, oldLegacy);
        else settings->remove(legacyKey);
        settings->sync();
        settings.reset();
        QCoreApplication::setOrganizationName(oldOrganization);
        QCoreApplication::setApplicationName(oldApplication);
    }

    const QString consentKey{
        QStringLiteral("transcription/crisperWhisperPersonalConsent")};
    const QString legacyKey{
        QStringLiteral("transcription/crisperWhisperPersonalAccepted")};
    std::unique_ptr<QSettings> settings;

private:
    QString oldOrganization;
    QString oldApplication;
    bool hadConsent{false};
    QVariant oldConsent;
    bool hadLegacy{false};
    QVariant oldLegacy;
};

} // namespace

TEST(AudioSyncFileNameTest, PreservesMultiWordCharacterBeforeFixSuffix)
{
    EXPECT_EQ(rt::extractCharacterName("C:/audio/ONLY ONE FIX.wav"), "Only one");
    EXPECT_EQ(rt::extractCharacterName("C:/audio/ONLY ONE-fixed-v2.wav"), "Only one");
    EXPECT_EQ(rt::extractCharacterName("C:/audio/ONLY ONE.wav"), "Only one");
    EXPECT_EQ(rt::extractCharacterName("C:/audio/MR X FIX.wav"), "Mr x");
    EXPECT_EQ(rt::extractCharacterName("C:/audio/ALICE TAKE 2.wav"), "Alice");
}

TEST_F(VoiceGenerationTest, ManualMatchDialogStaysAboveOnlyItsApplication)
{
    QWidget parent;
    rt::ManualMatchDialog dialog(
        "Alice", "Test dialogue", 1, {}, {}, {}, {}, {}, nullptr, &parent);

    EXPECT_EQ(dialog.parentWidget(), &parent);
    EXPECT_EQ(dialog.windowModality(), Qt::ApplicationModal);
    EXPECT_FALSE(dialog.windowFlags().testFlag(Qt::WindowStaysOnTopHint));
}

TEST_F(VoiceGenerationTest, ExposesOnlyConfirmedClipsAsApprovedReferences)
{
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    const QString approvedPath = makeTone(
        temporary.path(), QStringLiteral("approved.wav"), 4.0);
    const QString unmatchedPath = makeTone(
        temporary.path(), QStringLiteral("unmatched.wav"), 4.0);

    const std::vector<rt::SyncClip> clips{
        clip(1, approvedPath, "Alice", 0.0, 1.0, 2, "approved words"),
        clip(2, approvedPath, "Alice", 1.0, 2.0, 1, "tentative words"),
        clip(3, unmatchedPath, "Alice", 0.0, 1.0, 0, "unmatched words")
    };

    const auto references = rt::AudioSync::approvedVoiceReferenceCandidates(clips);
    ASSERT_EQ(references.size(), 1);
    EXPECT_EQ(references.front().transcript, QStringLiteral("approved words"));
    EXPECT_FLOAT_EQ(references.front().confidence, 1.0f);
}

TEST_F(VoiceGenerationTest, ScriptDialogueCanBeSelectedAndCopied)
{
    rt::AudioSync audioSync;
    ASSERT_TRUE(audioSync.loadScript(
        R"({"lines":[{"character":"Alice","dialogue":"Paste this into TTS."}]})"));

    auto* list = audioSync.scriptListWidget();
    ASSERT_NE(list, nullptr);
    ASSERT_EQ(list->count(), 1);
    auto* card = list->itemWidget(list->item(0));
    ASSERT_NE(card, nullptr);

    QLabel* dialogue = nullptr;
    for (auto* label : card->findChildren<QLabel*>()) {
        if (label->property("scriptTextCopyEnabled").toBool()) {
            dialogue = label;
            break;
        }
    }
    ASSERT_NE(dialogue, nullptr);
    EXPECT_TRUE(dialogue->textInteractionFlags().testFlag(Qt::TextSelectableByMouse));
    EXPECT_TRUE(dialogue->textInteractionFlags().testFlag(Qt::TextSelectableByKeyboard));

    auto* copyAction = dialogue->findChild<QAction*>(
        QStringLiteral("copyScriptLineTextAction"));
    ASSERT_NE(copyAction, nullptr);
    QGuiApplication::clipboard()->clear();
    copyAction->trigger();
    EXPECT_EQ(QGuiApplication::clipboard()->text(),
              QStringLiteral("Paste this into TTS."));
}

TEST_F(VoiceGenerationTest, ReusesAutomaticTranscriptForManualReferenceRange)
{
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    const QString source = makeTone(
        temporary.path(), QStringLiteral("manual-reference.wav"), 2.0);

    rt::AudioSync audioSync;
    audioSync.attachGeneratedAudio(
        source, QStringLiteral("Alice"),
        QStringLiteral("words produced by automatic transcription"),
        -1, QString(), 2.0);

    EXPECT_EQ(audioSync.voiceTranscriptForRange(source, 0.0, 1.5),
              QStringLiteral("words produced by automatic transcription"));
    EXPECT_TRUE(audioSync.voiceTranscriptForRange(source, 2.1, 3.0).isEmpty());
}

#ifdef ROUNDTABLE_HAS_CRISPERWHISPER
TEST_F(VoiceGenerationTest, UnknownCrisperConsentDoesNotPromptAtConstruction)
{
    ScopedCrisperConsentSettings consent;
    bool promptShown = false;
    QTimer modalGuard;
    modalGuard.setSingleShot(true);
    QObject::connect(&modalGuard, &QTimer::timeout, [&promptShown]() {
        for (QWidget* widget : QApplication::topLevelWidgets()) {
            if (auto* messageBox = qobject_cast<QMessageBox*>(widget)) {
                promptShown = true;
                messageBox->reject();
            }
        }
    });
    modalGuard.start(0);

    rt::AudioSync audioSync;
    modalGuard.stop();

    ASSERT_NE(audioSync.transcriptionModelCombo(), nullptr);
    EXPECT_EQ(audioSync.transcriptionModelCombo()->currentData().toString(),
              QStringLiteral("large-v3-turbo"));
    EXPECT_FALSE(promptShown);
}

TEST_F(VoiceGenerationTest, DecliningCrisperSelectionPersistsAndFallsBack)
{
    ScopedCrisperConsentSettings consent;
    rt::AudioSync audioSync;
    auto* combo = audioSync.transcriptionModelCombo();
    ASSERT_NE(combo, nullptr);
    const int crisperIndex = combo->findData(
        QStringLiteral("crisperwhisper-2-large-personal"));
    ASSERT_GE(crisperIndex, 0);

    QTimer::singleShot(0, []() {
        for (QWidget* widget : QApplication::topLevelWidgets()) {
            if (auto* messageBox = qobject_cast<QMessageBox*>(widget))
                messageBox->reject();
        }
    });
    combo->setCurrentIndex(crisperIndex);

    EXPECT_EQ(combo->currentData().toString(), QStringLiteral("large-v3-turbo"));
    consent.settings->sync();
    EXPECT_EQ(consent.settings->value(consent.consentKey).toString(),
              QStringLiteral("declined"));
    EXPECT_FALSE(consent.settings->value(consent.legacyKey).toBool());
}

TEST_F(VoiceGenerationTest, AcceptedCrisperConsentRestoresItsDefault)
{
    ScopedCrisperConsentSettings consent;
    consent.settings->setValue(consent.consentKey, QStringLiteral("accepted"));
    consent.settings->sync();

    rt::AudioSync audioSync;

    ASSERT_NE(audioSync.transcriptionModelCombo(), nullptr);
    EXPECT_EQ(audioSync.transcriptionModelCombo()->currentData().toString(),
              QStringLiteral("crisperwhisper-2-large-personal"));
}
#endif

TEST_F(VoiceGenerationTest, SavesCombinedApprovedClipsAsReusableFlac)
{
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    const QString firstPath = makeTone(
        temporary.path(), QStringLiteral("first.wav"), 0.8);
    const QString secondPath = makeTone(
        temporary.path(), QStringLiteral("second.wav"), 0.8);
    const QString unmatchedPath = makeTone(
        temporary.path(), QStringLiteral("unmatched.wav"), 0.8);

    const std::vector<rt::SyncClip> clips{
        clip(1, firstPath, "Alice", 0.0, 0.8, 2, "first approved line"),
        clip(2, secondPath, "Alice", 0.0, 0.8, 2, "second approved line"),
        clip(3, unmatchedPath, "Alice", 0.0, 0.8, 1, "not approved")
    };

    QString saved;
    QString error;
    ASSERT_TRUE(rt::AudioSync::saveApprovedVoiceReferenceClips(
        clips, QStringLiteral("Alice"), &saved, &error)) << error.toStdString();
    EXPECT_TRUE(QFileInfo::exists(saved));
    EXPECT_EQ(QFileInfo(saved).suffix().toLower(), QStringLiteral("flac"));
    EXPECT_GT(QFileInfo(saved).size(), 1000);

    const QString metadata = QFileInfo(saved).absolutePath() + QStringLiteral("/")
        + QFileInfo(saved).completeBaseName() + QStringLiteral(".json");
    ASSERT_TRUE(QFileInfo::exists(metadata));
    QFile metadataFile(metadata);
    ASSERT_TRUE(metadataFile.open(QIODevice::ReadOnly));
    const QByteArray contents = metadataFile.readAll();
    EXPECT_TRUE(contents.contains("first approved line"));
    EXPECT_TRUE(contents.contains("second approved line"));
    EXPECT_FALSE(contents.contains("not approved"));

    rt::AudioFile encoded;
    ASSERT_TRUE(encoded.open(saved.toUtf8().toStdString()));
    EXPECT_GT(encoded.info().duration, 1.5);
    encoded.close();

    const auto savedReference = rt::VoiceReferenceLibrary::newestFor(QStringLiteral("alice"));
    ASSERT_TRUE(savedReference.has_value());
    EXPECT_EQ(savedReference->path, saved);
    EXPECT_TRUE(savedReference->transcript.contains(QStringLiteral("second approved line")));

    QFile::remove(saved);
    QFile::remove(metadata);
    rt::VoiceReferenceLibrary::invalidate();
}

TEST_F(VoiceGenerationTest, TtsRailRefreshesCharactersAfterProjectRestore)
{
    QCoreApplication::setOrganizationName(QStringLiteral("RoundtableTests"));
    QCoreApplication::setApplicationName(QStringLiteral("VoiceGenerationTests"));
    QSettings settings;
    settings.setValue(
        QStringLiteral("transcription/crisperWhisperPersonalAccepted"), true);
    settings.sync();
    ASSERT_TRUE(settings.value(
        QStringLiteral("transcription/crisperWhisperPersonalAccepted")).toBool());

    std::vector<uint8_t> savedState;
    {
        rt::AudioSync source;
        ASSERT_TRUE(source.loadScript(
            "ALICE: First restored line\nBOB: Second restored line",
            "memory://voice-character-test"));
        savedState = source.serializeToBlob();
    }
    ASSERT_FALSE(savedState.empty());

    rt::VoiceGenerationService service;
    rt::AudioSync restored;
    auto* panel = new rt::VoiceGenerationPanel(&service);
    panel->setAudioSync(&restored);
    restored.setVoiceGenerationPanel(panel);
    restored.deserializeFromBlob(savedState);

    EXPECT_NE(restored.voiceGenerationButton(), nullptr);
    EXPECT_TRUE(panel->availableCharacters().contains(QStringLiteral("Alice")));
    EXPECT_TRUE(panel->availableCharacters().contains(QStringLiteral("Bob")));

    restored.showVoiceGenerationPanel();
    EXPECT_EQ(restored.audioSidePanelMode(), 5);
    EXPECT_EQ(panel->generateButton()->text(), QStringLiteral("Generate Draft"));
    EXPECT_FALSE(panel->listenButton()->isEnabled());
    EXPECT_FALSE(panel->approveSyncButton()->isEnabled());
    EXPECT_FALSE(panel->approveImportButton()->isEnabled());
    ASSERT_NE(panel->unloadModelButton(), nullptr);
    EXPECT_EQ(panel->unloadModelButton()->text(),
              QStringLiteral("Unload Model / Free VRAM"));
    EXPECT_FALSE(panel->unloadModelButton()->isEnabled());
    ASSERT_NE(panel->manualReferenceGroup(), nullptr);
    ASSERT_NE(panel->manualReferenceContent(), nullptr);
    EXPECT_TRUE(panel->manualReferenceContent()->isHidden());
    panel->manualReferenceGroup()->setChecked(true);
    EXPECT_FALSE(panel->manualReferenceContent()->isHidden());
    panel->manualReferenceGroup()->setChecked(false);
    EXPECT_TRUE(panel->manualReferenceContent()->isHidden());
}

TEST_F(VoiceGenerationTest, ApprovesDraftBesideImportedReferenceWithUniqueCharacterName)
{
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    const QString source = makeTone(
        temporary.path(), QStringLiteral("Chime.wav"), 0.5);
    const QString draft = makeTone(
        temporary.path(), QStringLiteral("audition-draft.wav"), 0.5);

    rt::VoiceGenerationRequest request;
    request.character = QStringLiteral("Chime");
    request.references.push_back({source, QStringLiteral("known words"), 0.0, 0.5});

    rt::VoiceGenerationService service;
    QString error;
    const QString approved = service.approveDraft(request, draft, &error);
    ASSERT_FALSE(approved.isEmpty()) << error.toStdString();
    EXPECT_EQ(QFileInfo(approved).absolutePath(), QFileInfo(source).absolutePath());
    EXPECT_TRUE(QRegularExpression(
        QStringLiteral(R"(^CHIME-\d{8}-\d{6}-\d{3}\.wav$)"))
                    .match(QFileInfo(approved).fileName()).hasMatch());
    EXPECT_TRUE(QFileInfo::exists(approved));
    EXPECT_FALSE(QFileInfo::exists(draft));
}

TEST_F(VoiceGenerationTest, ApprovedSyncMatchesOnlyTheSelectedCharacter)
{
    QCoreApplication::setOrganizationName(QStringLiteral("RoundtableTests"));
    QCoreApplication::setApplicationName(QStringLiteral("VoiceGenerationTests"));
    QSettings settings;
    settings.setValue(
        QStringLiteral("transcription/crisperWhisperPersonalAccepted"), true);
    settings.sync();

    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    const QString generated = makeTone(
        temporary.path(), QStringLiteral("CHIME-generated.wav"), 0.8);

    rt::AudioSync audioSync;
    ASSERT_TRUE(audioSync.loadScript(
        "CHIME: Welcome back to the show\nCROWN: Welcome back to the show",
        "memory://generated-sync-test"));
    EXPECT_TRUE(audioSync.syncGeneratedAudio(
        generated, QStringLiteral("chime"),
        QStringLiteral("Welcome back to the show"),
        -1, QStringLiteral("UNTITLED"), 0.8));

    ASSERT_EQ(audioSync.clipCount(), 1);
    EXPECT_EQ(QString::fromStdString(audioSync.clip(0).character),
              QStringLiteral("Chime"));
    EXPECT_EQ(audioSync.clip(0).matchState, 2);
    EXPECT_EQ(audioSync.clip(0).scriptLineNumber, 1);
}

TEST_F(VoiceGenerationTest, AutoSyncStaysTentativeAndMatchChangesAreUndoable)
{
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    QCoreApplication::setOrganizationName(QStringLiteral("RoundtableTests"));
    QCoreApplication::setApplicationName(QStringLiteral("VoiceGenerationTests"));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(
        QSettings::IniFormat, QSettings::UserScope, temporary.path());
    QSettings settings;
    settings.setValue(
        QStringLiteral("transcription/crisperWhisperPersonalAccepted"), true);
    settings.sync();
    ASSERT_EQ(settings.status(), QSettings::NoError);
    const QString approved = makeTone(
        temporary.path(), QStringLiteral("ALICE-approved.wav"), 0.5);
    const QString fixed = makeTone(
        temporary.path(), QStringLiteral("ALICE-fixed.wav"), 0.5);

    rt::AudioSync audioSync;
    rt::CommandStack history;
    audioSync.setCommandStack(&history);
    ASSERT_TRUE(audioSync.loadScript(
        "ALICE: This line was already reviewed\n"
        "ALICE: This replacement line is an exact match",
        "memory://auto-sync-tentative-test"));
    audioSync.attachGeneratedAudio(
        approved, QStringLiteral("Alice"),
        QStringLiteral("This line was already reviewed"),
        1, QStringLiteral("Alice: This line was already reviewed"), 0.5);
    audioSync.attachGeneratedAudio(
        fixed, QStringLiteral("Alice"),
        QStringLiteral("This replacement line is an exact match"),
        -1, QString(), 0.5);

    audioSync.runAutoSync();

    ASSERT_EQ(audioSync.clipCount(), 2);
    const rt::SyncClip* approvedClip = nullptr;
    const rt::SyncClip* fixedClip = nullptr;
    for (int i = 0; i < audioSync.clipCount(); ++i) {
        const auto& candidate = audioSync.clip(i);
        if (candidate.sourceFile == approved.toUtf8().toStdString())
            approvedClip = &candidate;
        if (candidate.sourceFile == fixed.toUtf8().toStdString())
            fixedClip = &candidate;
    }

    ASSERT_NE(approvedClip, nullptr);
    EXPECT_EQ(approvedClip->matchState, 2);
    EXPECT_EQ(approvedClip->scriptLineNumber, 1);
    ASSERT_NE(fixedClip, nullptr);
    EXPECT_EQ(fixedClip->scriptLineNumber, 2);
    EXPECT_FLOAT_EQ(fixedClip->confidence, 1.0f);
    EXPECT_EQ(fixedClip->matchState, 1);

    ASSERT_TRUE(history.canUndo());
    EXPECT_EQ(history.undoDescription(), "Auto-sync audio matches");
    QTest::keyClick(&audioSync, Qt::Key_Z, Qt::ControlModifier);

    bool foundUnmatchedFixed = false;
    for (int i = 0; i < audioSync.clipCount(); ++i) {
        const auto& candidate = audioSync.clip(i);
        if (candidate.sourceFile == fixed.toUtf8().toStdString()) {
            foundUnmatchedFixed = true;
            EXPECT_EQ(candidate.matchState, 0);
            EXPECT_EQ(candidate.scriptLineNumber, -1);
        }
    }
    EXPECT_TRUE(foundUnmatchedFixed);

    QTest::keyClick(
        &audioSync, Qt::Key_Z,
        Qt::ControlModifier | Qt::ShiftModifier);

    QPushButton* confirmFixed = nullptr;
    for (auto* button : audioSync.findChildren<QPushButton*>()) {
        if (button->text() == QStringLiteral("\u2713  CONFIRM")) {
            confirmFixed = button;
            break;
        }
    }
    ASSERT_NE(confirmFixed, nullptr);
    confirmFixed->click();
    EXPECT_EQ(history.undoDescription(), "Confirm audio match");
    QTest::keyClick(&audioSync, Qt::Key_Z, Qt::ControlModifier);

    bool foundTentativeFixed = false;
    for (int i = 0; i < audioSync.clipCount(); ++i) {
        const auto& candidate = audioSync.clip(i);
        if (candidate.sourceFile == fixed.toUtf8().toStdString()) {
            foundTentativeFixed = true;
            EXPECT_EQ(candidate.matchState, 1);
            EXPECT_EQ(candidate.scriptLineNumber, 2);
        }
    }
    EXPECT_TRUE(foundTentativeFixed);

    // The history entries remain reusable across repeated redo/undo cycles.
    QTest::keyClick(
        &audioSync, Qt::Key_Z,
        Qt::ControlModifier | Qt::ShiftModifier);
    QTest::keyClick(&audioSync, Qt::Key_Z, Qt::ControlModifier);
    QTest::keyClick(&audioSync, Qt::Key_Z, Qt::ControlModifier);
    ASSERT_EQ(audioSync.clipCount(), 2);
    for (int i = 0; i < audioSync.clipCount(); ++i) {
        const auto& candidate = audioSync.clip(i);
        if (candidate.sourceFile == fixed.toUtf8().toStdString()) {
            EXPECT_EQ(candidate.matchState, 0);
            EXPECT_EQ(candidate.scriptLineNumber, -1);
        }
    }
}

// ─── Service lifecycle (fake worker) ─────────────────────────────────────────

namespace {

QString fakeWorkerPython()
{
    return QString::fromUtf8(ROUNDTABLE_CRISPERWHISPER_PYTHON_PATH);
}

/// Launch the protocol-compatible fake worker for any engine key.
rt::VoiceGenerationService::LaunchOverride fakeLauncher(
    std::function<QStringList(const QString&)> extraArguments)
{
    return [extraArguments](const QString& provider) {
        QStringList arguments{QString::fromUtf8(ROUNDTABLE_FAKE_VOICE_WORKER),
                              QStringLiteral("--name"), provider};
        arguments += extraArguments(provider);
        return rt::VoiceGenerationService::WorkerLaunch{
            fakeWorkerPython(), arguments, QString()};
    };
}

rt::VoiceGenerationRequest fakeRequest(const QString& provider, const QString& id)
{
    rt::VoiceGenerationRequest request;
    request.requestId = id;
    request.provider = provider;
    request.text = QStringLiteral("Hello from the fake worker");
    request.character = QStringLiteral("Alice");
    return request;
}

/// "<event> <name> <monotonic seconds>" lines written by the fake worker.
double loggedTime(const QString& logPath, const QString& event, const QString& name)
{
    QFile file(logPath);
    if (!file.open(QIODevice::ReadOnly)) return -1.0;
    for (const auto& line : QString::fromUtf8(file.readAll()).split('\n')) {
        const auto parts = line.trimmed().split(' ');
        if (parts.size() == 3 && parts[0] == event && parts[1] == name)
            return parts[2].toDouble();
    }
    return -1.0;
}

int loggedCount(const QString& logPath, const QString& event)
{
    QFile file(logPath);
    if (!file.open(QIODevice::ReadOnly)) return 0;
    int count = 0;
    for (const auto& line : QString::fromUtf8(file.readAll()).split('\n')) {
        if (line.startsWith(event + QLatin1Char(' '))) ++count;
    }
    return count;
}

/// Records service outcomes by request id / error text.
struct ServiceOutcomes
{
    explicit ServiceOutcomes(rt::VoiceGenerationService& service)
    {
        QObject::connect(&service, &rt::VoiceGenerationService::generationFinished,
            [this](const rt::VoiceGenerationRequest& request, const QString& path, double) {
                finished << request.requestId;
                QFile::remove(path);
            });
        QObject::connect(&service, &rt::VoiceGenerationService::generationFailed,
            [this](const rt::VoiceGenerationRequest&, const QString& error) {
                failures << error;
            });
    }
    QStringList finished;
    QStringList failures;
};

} // namespace

class VoiceServiceLifecycleTest : public VoiceGenerationTest
{
protected:
    void SetUp() override
    {
        if (!QFileInfo::exists(fakeWorkerPython()))
            GTEST_SKIP() << "Python runtime for the fake voice worker is not installed";
    }
};

TEST_F(VoiceServiceLifecycleTest, PersistentWorkerServesQueuedRequestsThenUnloads)
{
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    const QString log = temporary.filePath(QStringLiteral("worker.log"));
    rt::VoiceGenerationService service;
    service.setLaunchOverrideForTesting(fakeLauncher([&log](const QString&) {
        return QStringList{QStringLiteral("--log"), log};
    }));
    ServiceOutcomes outcomes(service);

    service.enqueue(fakeRequest(QStringLiteral("breeze"), QStringLiteral("first")));
    service.enqueue(fakeRequest(QStringLiteral("breeze"), QStringLiteral("second")));
    EXPECT_TRUE(service.isBusy());
    ASSERT_TRUE(QTest::qWaitFor([&] { return outcomes.finished.size() == 2; }, 20000))
        << outcomes.failures.join(QStringLiteral("; ")).toStdString();
    EXPECT_EQ(outcomes.finished, (QStringList{QStringLiteral("first"), QStringLiteral("second")}));
    EXPECT_TRUE(outcomes.failures.isEmpty());
    EXPECT_FALSE(service.isBusy());
    EXPECT_TRUE(service.isModelResident());
    EXPECT_EQ(loggedCount(log, QStringLiteral("start")), 1);  // one model load for both

    service.unloadModel();
    ASSERT_TRUE(QTest::qWaitFor([&] { return !service.isModelResident(); }, 10000));
    // The idle worker received the shutdown request and exited on its own.
    EXPECT_EQ(loggedCount(log, QStringLiteral("exit")), 1);
}

TEST_F(VoiceServiceLifecycleTest, LoadTimeoutOfUnloadedWorkerDoesNotKillItsReplacement)
{
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    const QString flag = temporary.filePath(QStringLiteral("ready.flag"));
    rt::VoiceGenerationService service;
    service.setLoadTimeoutForTesting(2000);
    service.setLaunchOverrideForTesting(fakeLauncher([&flag](const QString&) {
        return QStringList{QStringLiteral("--ready-flag"), flag};
    }));
    ServiceOutcomes outcomes(service);

    QElapsedTimer clock;
    clock.start();
    service.enqueue(fakeRequest(QStringLiteral("breeze"), QStringLiteral("abandoned")));
    QTest::qWait(800);
    service.unloadModel();  // the first worker's load timer is still pending
    service.enqueue(fakeRequest(QStringLiteral("breeze"), QStringLiteral("kept")));

    // Past the first worker's deadline, before the replacement's.
    QTest::qWait(static_cast<int>(std::max<qint64>(0, 2300 - clock.elapsed())));
    EXPECT_TRUE(service.isBusy());
    EXPECT_TRUE(service.isModelResident());

    QFile ready(flag);
    ASSERT_TRUE(ready.open(QIODevice::WriteOnly));
    ready.close();
    ASSERT_TRUE(QTest::qWaitFor([&] {
        return outcomes.finished.contains(QStringLiteral("kept")); }, 10000))
        << outcomes.failures.join(QStringLiteral("; ")).toStdString();
    EXPECT_FALSE(outcomes.failures.join(QStringLiteral(" "))
                     .contains(QStringLiteral("did not finish loading")));
    service.shutdown();
}

TEST_F(VoiceServiceLifecycleTest, WorkerCrashFailsTheRequestWithItsLastMessage)
{
    rt::VoiceGenerationService service;
    service.setLaunchOverrideForTesting(fakeLauncher([](const QString&) {
        return QStringList{QStringLiteral("--crash-on-generate")};
    }));
    ServiceOutcomes outcomes(service);

    service.enqueue(fakeRequest(QStringLiteral("breeze"), QStringLiteral("crash")));
    ASSERT_TRUE(QTest::qWaitFor([&] { return !outcomes.failures.isEmpty(); }, 20000));
    EXPECT_TRUE(outcomes.failures.front().contains(QStringLiteral("exited unexpectedly (code 3)")))
        << outcomes.failures.front().toStdString();
    EXPECT_TRUE(outcomes.failures.front().contains(QStringLiteral("fake worker failure detail")));
    EXPECT_FALSE(service.isBusy());
    EXPECT_TRUE(QTest::qWaitFor([&] { return !service.isModelResident(); }, 5000));
}

TEST_F(VoiceServiceLifecycleTest, SwitchingEnginesStartsNewWorkerOnlyAfterOldOneExits)
{
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    const QString log = temporary.filePath(QStringLiteral("worker.log"));
    rt::VoiceGenerationService service;
    service.setLaunchOverrideForTesting(fakeLauncher([&log](const QString&) {
        return QStringList{QStringLiteral("--log"), log,
                           QStringLiteral("--shutdown-delay"), QStringLiteral("0.6")};
    }));
    ServiceOutcomes outcomes(service);

    service.enqueue(fakeRequest(QStringLiteral("omnivoice"), QStringLiteral("a")));
    ASSERT_TRUE(QTest::qWaitFor([&] { return outcomes.finished.size() == 1; }, 20000));

    QElapsedTimer blocking;
    blocking.start();
    service.enqueue(fakeRequest(QStringLiteral("fish-s2"), QStringLiteral("b")));
    // The old worker takes 0.6 s to exit; the UI thread must not wait for it.
    EXPECT_LT(blocking.elapsed(), 300);
    ASSERT_TRUE(QTest::qWaitFor([&] { return outcomes.finished.size() == 2; }, 20000))
        << outcomes.failures.join(QStringLiteral("; ")).toStdString();

    const double oldExit = loggedTime(log, QStringLiteral("exit"), QStringLiteral("omnivoice"));
    const double newStart = loggedTime(log, QStringLiteral("start"), QStringLiteral("fish-s2"));
    ASSERT_GT(oldExit, 0.0);
    ASSERT_GT(newStart, 0.0);
    EXPECT_LE(oldExit, newStart);  // never two models resident at once
    service.shutdown();
}

// ─── Script line → TTS panel ─────────────────────────────────────────────────

TEST_F(VoiceGenerationTest, ScriptLineActionLinksTheDraftToThatLine)
{
    rt::VoiceGenerationService service;
    rt::AudioSync audioSync;
    ASSERT_TRUE(audioSync.loadScript(
        "ALICE: First line\nBOB: Second line", "memory://voice-line-link-test"));
    auto* panel = new rt::VoiceGenerationPanel(&service);
    panel->setAudioSync(&audioSync);
    audioSync.setVoiceGenerationPanel(panel);  // AudioSync owns the panel

    auto* list = audioSync.scriptListWidget();
    ASSERT_NE(list, nullptr);
    ASSERT_EQ(list->count(), 2);
    QAction* generateVoice = nullptr;
    for (auto* label : list->itemWidget(list->item(1))->findChildren<QLabel*>()) {
        if (auto* action = label->findChild<QAction*>(
                QStringLiteral("generateVoiceForLineAction"))) {
            generateVoice = action;
            break;
        }
    }
    ASSERT_NE(generateVoice, nullptr);

    QSignalSpy requested(&audioSync, &rt::AudioSync::voiceLineRequested);
    generateVoice->trigger();
    ASSERT_EQ(requested.count(), 1);
    EXPECT_EQ(audioSync.audioSidePanelMode(), 5);
    EXPECT_EQ(panel->linkedScriptLine(), requested.at(0).at(0).toInt());
    EXPECT_GE(panel->linkedScriptLine(), 0);
    EXPECT_EQ(panel->currentText(), QStringLiteral("Second line"));
    EXPECT_EQ(panel->currentCharacter().compare(QStringLiteral("Bob"), Qt::CaseInsensitive), 0);

    // A line belongs to one character: picking another one unlinks it.
    QComboBox* character = nullptr;
    for (auto* combo : panel->findChildren<QComboBox*>()) {
        if (combo->isEditable()) character = combo;
    }
    ASSERT_NE(character, nullptr);
    character->setCurrentText(QStringLiteral("Alice"));
    EXPECT_EQ(panel->linkedScriptLine(), -1);
}
