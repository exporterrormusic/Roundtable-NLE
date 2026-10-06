/*
 * test_manual_match_dialog.cpp — Premiere K-hold keys in the manual match /
 * take player dialog: hold K + tap L/J steps exactly one frame, K key-repeat
 * is ignored, and releasing K disarms the step.
 * (Slow motion on K + held J/L needs a live AudioEngine; not covered here.)
 */
#include <gtest/gtest.h>

#include "widgets/ManualMatchDialog.h"

#include <QApplication>
#include <QKeyEvent>

#include <memory>

using namespace rt;

namespace {

int g_argc = 1;
char g_arg0[] = "test_manual_match_dialog";
char* g_argv[] = {g_arg0, nullptr};

class ManualMatchDialogTest : public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        if (!QApplication::instance()) s_app = new QApplication(g_argc, g_argv);
    }

    void SetUp() override
    {
        ManualMatchDialog::AudioData data;
        data.sampleRate = 48000;
        data.pcm = SampleBuffer(std::vector<float>(48000 * 10, 0.0f));   // 10 s of silence
        m_dlg = std::make_unique<ManualMatchDialog>(
            "Announcer", "Line", 1, std::vector<std::string>{"take.wav"},
            std::unordered_map<std::string, ManualMatchDialog::AudioData>{{"take.wav", data}},
            std::vector<std::pair<double, double>>{}, std::vector<std::pair<double, double>>{},
            "take.wav", /*engine=*/nullptr, nullptr);
        m_dlg->loadInitialFile();
        // Playhead at 2.0 s via the edit-range API.
        m_dlg->setEditRange("Take Player", "Apply", 2.0, 4.0);
    }

    void key(QEvent::Type type, int k, bool autoRepeat = false)
    {
        QKeyEvent ev(type, k, Qt::NoModifier, QString(), autoRepeat);
        QApplication::sendEvent(m_dlg.get(), &ev);
    }

    static inline QApplication* s_app = nullptr;
    std::unique_ptr<ManualMatchDialog> m_dlg;
};

} // namespace

TEST_F(ManualMatchDialogTest, HoldKTapLStepsOneFrameForward)
{
    const double start = m_dlg->playhead();
    key(QEvent::KeyPress, Qt::Key_K);
    key(QEvent::KeyPress, Qt::Key_K, /*autoRepeat=*/true);   // holding K
    key(QEvent::KeyPress, Qt::Key_L);
    key(QEvent::KeyRelease, Qt::Key_L);
    EXPECT_NEAR(m_dlg->playhead(), start + 1.0 / 30.0, 1e-6);
    key(QEvent::KeyPress, Qt::Key_L);
    key(QEvent::KeyRelease, Qt::Key_L);
    EXPECT_NEAR(m_dlg->playhead(), start + 2.0 / 30.0, 1e-6);
}

TEST_F(ManualMatchDialogTest, HoldKTapJStepsOneFrameBack)
{
    const double start = m_dlg->playhead();
    key(QEvent::KeyPress, Qt::Key_K);
    key(QEvent::KeyPress, Qt::Key_J);
    key(QEvent::KeyRelease, Qt::Key_J);
    EXPECT_NEAR(m_dlg->playhead(), start - 1.0 / 30.0, 1e-6);
}

TEST_F(ManualMatchDialogTest, ReleasingKDisarmsFrameStep)
{
    key(QEvent::KeyPress, Qt::Key_K);
    key(QEvent::KeyRelease, Qt::Key_K);
    const double start = m_dlg->playhead();
    key(QEvent::KeyPress, Qt::Key_L);   // plain L = shuttle; no engine → no move
    key(QEvent::KeyRelease, Qt::Key_L);
    EXPECT_NEAR(m_dlg->playhead(), start, 1e-6);
}
