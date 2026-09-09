/*
 * NotificationCenter -- application-wide activity history, progress, and
 * non-blocking feedback presented from the MainWindow status bar.
 */

#pragma once

#include <QObject>
#include <QString>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>

class QMainWindow;
class QStatusBar;
class QToolButton;
class QWidget;

namespace rt {

class NotificationCenter final : public QObject
{
    Q_OBJECT

public:
    using Id = uint64_t;

    enum class Severity : uint8_t {
        Info,
        Success,
        Warning,
        Error,
    };
    Q_ENUM(Severity)

    explicit NotificationCenter(QMainWindow* window, QStatusBar* statusBar);
    ~NotificationCenter() override;

    NotificationCenter(const NotificationCenter&) = delete;
    NotificationCenter& operator=(const NotificationCenter&) = delete;

    /// The center attached to the active MainWindow, if one exists.
    [[nodiscard]] static NotificationCenter* current() noexcept;

    Id post(Severity severity,
            QString title,
            QString message = {},
            QString actionText = {},
            std::function<void()> action = {},
            bool showToast = true);

    Id postInfo(QString title, QString message = {}, bool showToast = true);
    Id postSuccess(QString title, QString message = {}, bool showToast = true);
    Id postWarning(QString title, QString message = {}, bool showToast = true);
    Id postError(QString title, QString message = {}, bool showToast = true);

    /// progress: -1 is indeterminate; otherwise it is clamped to 0..100.
    Id beginTask(QString title,
                 QString detail = {},
                 int progress = -1,
                 std::function<void()> cancel = {},
                 std::function<void()> retry = {});
    void updateTask(Id id, int progress, QString detail = {});
    void cancelTask(Id id);
    void finishTask(Id id, bool success, QString message = {},
                    bool addNotification = true);

    void showCenter();
    void clearNotifications();
    void markAllRead();

    [[nodiscard]] int unreadCount() const;
    [[nodiscard]] int activeTaskCount() const;
    [[nodiscard]] int notificationCount() const;
    [[nodiscard]] bool hasTask(Id id) const;
    [[nodiscard]] QToolButton* statusButton() const noexcept;
    [[nodiscard]] QWidget* popupWidget() const noexcept;

signals:
    void countsChanged(int activeTasks, int unreadNotifications);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    struct State;
    std::unique_ptr<State> m_state;
    std::atomic<Id> m_nextId{1};

    void postOnUi(Id id, Severity severity, QString title, QString message,
                  QString actionText, std::function<void()> action,
                  bool showToast);
    void beginTaskOnUi(Id id, QString title, QString detail, int progress,
                       std::function<void()> cancel,
                       std::function<void()> retry);
    void updateTaskOnUi(Id id, int progress, const QString& detail);
    void cancelTaskOnUi(Id id);
    void finishTaskOnUi(Id id, bool success, const QString& message,
                        bool addNotification);
    void rebuildNotifications();
    void updateStatusButton();
    void positionOverlays();
    void showToast(Severity severity, const QString& title,
                   const QString& message, const QString& actionText,
                   const std::function<void()>& action);
};

} // namespace rt
