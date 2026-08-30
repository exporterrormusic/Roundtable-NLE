#include <gtest/gtest.h>

#include "AudioMixdown.h"
#include "audio/AudioFile.h"
#include "command/CommandStack.h"
#include "panels/audio/AudioSync.h"
#include "panels/audio/AudioSyncFileName.h"
#include "panels/audio/VoiceGenerationPanel.h"
#include "panels/audio/VoiceGenerationService.h"
#include "widgets/ManualMatchDialog.h"

#include <QApplication>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QSettings>
#include <QTest>
#include <QTemporaryDir>
#include <QRegularExpression>
#include <QPushButton>
#include <QGroupBox>

#include <cmath>
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
};

std::unique_ptr<QApplication> VoiceGenerationTest::app;

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

TEST_F(VoiceGenerationTest, SavesCombinedApprovedClipsAsReusableMp3)
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
    EXPECT_EQ(QFileInfo(saved).suffix().toLower(), QStringLiteral("mp3"));
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

    QFile::remove(saved);
    QFile::remove(metadata);
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
    auto* panel = new rt::VoiceGenerationPanel(&service, true);
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
