#include "NotificationCenter.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QMainWindow>
#include <QProgressBar>
#include <QPushButton>
#include <QSignalSpy>
#include <QStatusBar>
#include <QTest>
#include <QToolButton>

#include <memory>
#include <thread>

namespace {

int g_argc = 1;
char g_arg0[] = "test_notification_center";
char* g_argv[] = {g_arg0, nullptr};

class NotificationCenterTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        if (!QApplication::instance())
            m_app = std::make_unique<QApplication>(g_argc, g_argv);
    }

    std::unique_ptr<QApplication> m_app;
};

QPushButton* buttonWithText(QWidget* root, const QString& text)
{
    for (auto* button : root->findChildren<QPushButton*>()) {
        if (button->text() == text) return button;
    }
    return nullptr;
}

} // namespace

TEST_F(NotificationCenterTest, TracksUnreadHistoryAndStatusMessages)
{
    QMainWindow window;
    rt::NotificationCenter center(&window, window.statusBar());
    QSignalSpy counts(&center, &rt::NotificationCenter::countsChanged);

    center.postWarning(QStringLiteral("Low disk space"),
                       QStringLiteral("12 GB remaining"), false);
    EXPECT_EQ(center.notificationCount(), 1);
    EXPECT_EQ(center.unreadCount(), 1);
    EXPECT_EQ(center.statusButton()->text(), QStringLiteral("Alerts 1"));

    center.markAllRead();
    EXPECT_EQ(center.unreadCount(), 0);
    EXPECT_EQ(center.statusButton()->text(), QStringLiteral("Activity"));

    window.statusBar()->showMessage(QStringLiteral("Project saved"));
    EXPECT_EQ(center.notificationCount(), 2);
    EXPECT_EQ(center.unreadCount(), 1);
    EXPECT_GE(counts.count(), 3);

    center.clearNotifications();
    EXPECT_EQ(center.notificationCount(), 0);
    EXPECT_EQ(center.unreadCount(), 0);
}

TEST_F(NotificationCenterTest, TaskSupportsProgressCancelAndRetry)
{
    QMainWindow window;
    rt::NotificationCenter center(&window, window.statusBar());
    int cancelCalls = 0;
    int retryCalls = 0;

    const auto id = center.beginTask(
        QStringLiteral("Exporting video"), QStringLiteral("Rendering"), 150,
        [&cancelCalls]() { ++cancelCalls; },
        [&retryCalls]() { ++retryCalls; });

    ASSERT_TRUE(center.hasTask(id));
    EXPECT_EQ(center.activeTaskCount(), 1);
    EXPECT_EQ(center.statusButton()->text(), QStringLiteral("Tasks 1"));
    const auto progressBars = center.popupWidget()->findChildren<QProgressBar*>();
    ASSERT_EQ(progressBars.size(), 1);
    EXPECT_EQ(progressBars.front()->value(), 100);

    center.updateTask(id, 42, QStringLiteral("42% rendered"));
    EXPECT_EQ(progressBars.front()->value(), 42);
    center.cancelTask(id);
    center.cancelTask(id);
    EXPECT_EQ(cancelCalls, 1);

    center.finishTask(id, false, QStringLiteral("Encoder stopped"));
    EXPECT_FALSE(center.hasTask(id));
    EXPECT_EQ(center.activeTaskCount(), 0);
    EXPECT_EQ(center.notificationCount(), 1);

    auto* retry = buttonWithText(center.popupWidget(), QStringLiteral("Retry"));
    ASSERT_NE(retry, nullptr);
    retry->click();
    EXPECT_EQ(retryCalls, 1);
}

TEST_F(NotificationCenterTest, WorkerThreadCanPublishWithoutTouchingWidgets)
{
    QMainWindow window;
    rt::NotificationCenter center(&window, window.statusBar());

    std::thread worker([&center]() {
        center.postSuccess(QStringLiteral("Waveform ready"), {}, false);
    });
    worker.join();

    QTRY_COMPARE_WITH_TIMEOUT(center.notificationCount(), 1, 1000);
    EXPECT_EQ(center.unreadCount(), 1);
}
