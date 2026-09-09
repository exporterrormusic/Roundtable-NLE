#include "NotificationCenter.h"

#include "MediaTaskQueue.h"
#include "Theme.h"

#include <QDateTime>
#include <QEvent>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QMainWindow>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QStatusBar>
#include <QThread>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <deque>
#include <unordered_map>

namespace rt {

namespace {

QPointer<NotificationCenter> g_currentCenter;

QString severityColor(NotificationCenter::Severity severity)
{
    const auto& colors = Theme::colors();
    switch (severity) {
    case NotificationCenter::Severity::Success: return Theme::hex(colors.success);
    case NotificationCenter::Severity::Warning: return Theme::hex(colors.warning);
    case NotificationCenter::Severity::Error:   return Theme::hex(colors.error);
    case NotificationCenter::Severity::Info:    return Theme::hex(colors.accent);
    }
    return Theme::hex(colors.accent);
}

QString severityLabel(NotificationCenter::Severity severity)
{
    switch (severity) {
    case NotificationCenter::Severity::Success: return QStringLiteral("DONE");
    case NotificationCenter::Severity::Warning: return QStringLiteral("WARNING");
    case NotificationCenter::Severity::Error:   return QStringLiteral("ERROR");
    case NotificationCenter::Severity::Info:    return QStringLiteral("INFO");
    }
    return QStringLiteral("INFO");
}

QString popupStyle()
{
    const auto& c = Theme::colors();
    return QStringLiteral(
        "QFrame#ActivityPopup { background:%1; border:1px solid %2; border-radius:8px; }"
        "QFrame#ActivityRow { background:%3; border:1px solid %2; border-radius:5px; }"
        "QLabel#ActivitySection { color:%4; font-size:11px; font-weight:600; }"
        "QLabel#ActivityTitle { color:%5; font-weight:600; }"
        "QLabel#ActivityDetail { color:%4; }"
        "QPushButton { padding:3px 8px; }"
        "QScrollArea { border:none; background:transparent; }"
        "QWidget#ActivityContent { background:transparent; }")
        .arg(Theme::hex(c.surface2), Theme::hex(c.border),
             Theme::hex(c.surface1), Theme::hex(c.textSecondary),
             Theme::hex(c.textPrimary));
}

} // namespace

struct NotificationCenter::State
{
    struct Notification {
        Id id{0};
        Severity severity{Severity::Info};
        QString title;
        QString message;
        QString actionText;
        std::function<void()> action;
        QDateTime created;
        bool read{false};
    };

    struct Task {
        Id id{0};
        QString title;
        QString detail;
        int progress{-1};
        std::function<void()> cancel;
        std::function<void()> retry;
        bool cancelRequested{false};
    };

    struct TaskWidgets {
        QFrame* row{nullptr};
        QLabel* title{nullptr};
        QLabel* detail{nullptr};
        QProgressBar* progress{nullptr};
        QPushButton* cancel{nullptr};
    };

    QMainWindow* window{nullptr};
    QStatusBar* statusBar{nullptr};
    QToolButton* button{nullptr};
    QFrame* popup{nullptr};
    QWidget* content{nullptr};
    QWidget* tasksContainer{nullptr};
    QWidget* notificationsContainer{nullptr};
    QLabel* tasksHeading{nullptr};
    QLabel* notificationsHeading{nullptr};
    QVBoxLayout* tasksLayout{nullptr};
    QVBoxLayout* notificationsLayout{nullptr};
    QFrame* toast{nullptr};
    QLabel* toastAccent{nullptr};
    QLabel* toastTitle{nullptr};
    QLabel* toastMessage{nullptr};
    QPushButton* toastAction{nullptr};
    QTimer* toastTimer{nullptr};
    QTimer* mediaPollTimer{nullptr};
    Id mediaTaskId{0};
    std::deque<Notification> notifications;
    std::unordered_map<Id, Task> tasks;
    std::unordered_map<Id, TaskWidgets> taskWidgets;
};

NotificationCenter::NotificationCenter(QMainWindow* window, QStatusBar* statusBar)
    : QObject(window)
    , m_state(std::make_unique<State>())
{
    Q_ASSERT(window);
    Q_ASSERT(statusBar);
    m_state->window = window;
    m_state->statusBar = statusBar;
    g_currentCenter = this;

    const auto& c = Theme::colors();
    m_state->button = new QToolButton(statusBar);
    m_state->button->setObjectName(QStringLiteral("ActivityCenterButton"));
    m_state->button->setText(tr("Activity"));
    m_state->button->setToolTip(tr("Background tasks and notifications"));
    m_state->button->setCursor(Qt::PointingHandCursor);
    m_state->button->setStyleSheet(QStringLiteral(
        "QToolButton { color:%1; background:transparent; border-left:1px solid %2;"
        " padding:2px 10px; }"
        "QToolButton:hover { background:%3; color:%4; }")
        .arg(Theme::hex(c.textSecondary), Theme::hex(c.border),
             Theme::hex(c.controlBgHover), Theme::hex(c.textPrimary)));
    statusBar->addPermanentWidget(m_state->button);

    m_state->popup = new QFrame(window, Qt::Popup);
    m_state->popup->setObjectName(QStringLiteral("ActivityPopup"));
    m_state->popup->setStyleSheet(popupStyle());
    m_state->popup->setFixedWidth(420);
    m_state->popup->setMaximumHeight(480);
    auto* popupLayout = new QVBoxLayout(m_state->popup);
    popupLayout->setContentsMargins(12, 10, 12, 12);
    popupLayout->setSpacing(8);

    auto* header = new QHBoxLayout;
    auto* heading = new QLabel(tr("Activity"), m_state->popup);
    QFont headingFont = heading->font();
    headingFont.setBold(true);
    headingFont.setPixelSize(15);
    heading->setFont(headingFont);
    auto* clear = new QPushButton(tr("Clear"), m_state->popup);
    clear->setObjectName(QStringLiteral("ActivityClearButton"));
    clear->setFlat(true);
    header->addWidget(heading);
    header->addStretch();
    header->addWidget(clear);
    popupLayout->addLayout(header);

    auto* scroll = new QScrollArea(m_state->popup);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_state->content = new QWidget(scroll);
    m_state->content->setObjectName(QStringLiteral("ActivityContent"));
    auto* contentLayout = new QVBoxLayout(m_state->content);
    contentLayout->setContentsMargins(0, 0, 0, 0);
    contentLayout->setSpacing(7);

    m_state->tasksHeading = new QLabel(tr("IN PROGRESS"), m_state->content);
    m_state->tasksHeading->setObjectName(QStringLiteral("ActivitySection"));
    contentLayout->addWidget(m_state->tasksHeading);
    m_state->tasksContainer = new QWidget(m_state->content);
    m_state->tasksLayout = new QVBoxLayout(m_state->tasksContainer);
    m_state->tasksLayout->setContentsMargins(0, 0, 0, 0);
    m_state->tasksLayout->setSpacing(6);
    contentLayout->addWidget(m_state->tasksContainer);

    m_state->notificationsHeading = new QLabel(tr("RECENT"), m_state->content);
    m_state->notificationsHeading->setObjectName(QStringLiteral("ActivitySection"));
    contentLayout->addWidget(m_state->notificationsHeading);
    m_state->notificationsContainer = new QWidget(m_state->content);
    m_state->notificationsLayout = new QVBoxLayout(m_state->notificationsContainer);
    m_state->notificationsLayout->setContentsMargins(0, 0, 0, 0);
    m_state->notificationsLayout->setSpacing(6);
    contentLayout->addWidget(m_state->notificationsContainer);
    contentLayout->addStretch();
    scroll->setWidget(m_state->content);
    popupLayout->addWidget(scroll, 1);

    m_state->toast = new QFrame(window);
    m_state->toast->setObjectName(QStringLiteral("ActivityToast"));
    m_state->toast->setFixedWidth(370);
    m_state->toast->hide();
    auto* toastLayout = new QHBoxLayout(m_state->toast);
    toastLayout->setContentsMargins(0, 0, 8, 0);
    toastLayout->setSpacing(8);
    m_state->toastAccent = new QLabel(m_state->toast);
    m_state->toastAccent->setFixedWidth(4);
    m_state->toastTitle = new QLabel(m_state->toast);
    m_state->toastTitle->setObjectName(QStringLiteral("ActivityTitle"));
    m_state->toastMessage = new QLabel(m_state->toast);
    m_state->toastMessage->setObjectName(QStringLiteral("ActivityDetail"));
    m_state->toastMessage->setWordWrap(true);
    auto* toastText = new QVBoxLayout;
    toastText->setContentsMargins(4, 8, 0, 8);
    toastText->setSpacing(2);
    toastText->addWidget(m_state->toastTitle);
    toastText->addWidget(m_state->toastMessage);
    m_state->toastAction = new QPushButton(m_state->toast);
    m_state->toastAction->hide();
    auto* closeToast = new QToolButton(m_state->toast);
    closeToast->setText(QStringLiteral("x"));
    closeToast->setAutoRaise(true);
    toastLayout->addWidget(m_state->toastAccent);
    toastLayout->addLayout(toastText, 1);
    toastLayout->addWidget(m_state->toastAction);
    toastLayout->addWidget(closeToast, 0, Qt::AlignTop);
    m_state->toastTimer = new QTimer(this);
    m_state->toastTimer->setSingleShot(true);
    m_state->toastTimer->setInterval(4500);

    connect(m_state->button, &QToolButton::clicked,
            this, &NotificationCenter::showCenter);
    connect(clear, &QPushButton::clicked,
            this, &NotificationCenter::clearNotifications);
    connect(closeToast, &QToolButton::clicked, m_state->toast, &QWidget::hide);
    connect(m_state->toastTimer, &QTimer::timeout, m_state->toast, &QWidget::hide);
    connect(statusBar, &QStatusBar::messageChanged, this,
            [this](const QString& message) {
                if (message.isEmpty() || message == QStringLiteral("Ready")) return;
                post(Severity::Info, tr("Status"), message, {}, {}, false);
            });

    window->installEventFilter(this);

    // Present the shared media queue as one low-noise aggregate task.
    m_state->mediaPollTimer = new QTimer(this);
    m_state->mediaPollTimer->setInterval(250);
    connect(m_state->mediaPollTimer, &QTimer::timeout, this, [this]() {
        const auto stats = MediaTaskQueue::instance().statistics();
        const size_t active = stats.queued + stats.running;
        if (active > 0) {
            const QString detail = tr("%1 media item%2 remaining")
                .arg(active).arg(active == 1 ? QString() : QStringLiteral("s"));
            if (m_state->mediaTaskId == 0)
                m_state->mediaTaskId = beginTask(tr("Preparing media"), detail, -1);
            else
                updateTask(m_state->mediaTaskId, -1, detail);
        } else if (m_state->mediaTaskId != 0) {
            const Id id = m_state->mediaTaskId;
            m_state->mediaTaskId = 0;
            finishTask(id, true, {}, false);
        }
    });
    m_state->mediaPollTimer->start();

    rebuildNotifications();
    updateStatusButton();
}

NotificationCenter::~NotificationCenter()
{
    if (g_currentCenter == this) g_currentCenter.clear();
}

NotificationCenter* NotificationCenter::current() noexcept
{
    return g_currentCenter.data();
}

NotificationCenter::Id NotificationCenter::post(
    Severity severity, QString title, QString message, QString actionText,
    std::function<void()> action, bool showToastFlag)
{
    const Id id = m_nextId.fetch_add(1, std::memory_order_relaxed);
    if (QThread::currentThread() != thread()) {
        QMetaObject::invokeMethod(this,
            [this, id, severity, title = std::move(title), message = std::move(message),
             actionText = std::move(actionText), action = std::move(action),
             showToastFlag]() mutable {
                postOnUi(id, severity, std::move(title), std::move(message),
                         std::move(actionText), std::move(action), showToastFlag);
            }, Qt::QueuedConnection);
    } else {
        postOnUi(id, severity, std::move(title), std::move(message),
                 std::move(actionText), std::move(action), showToastFlag);
    }
    return id;
}

NotificationCenter::Id NotificationCenter::postInfo(
    QString title, QString message, bool toast)
{
    return post(Severity::Info, std::move(title), std::move(message), {}, {}, toast);
}

NotificationCenter::Id NotificationCenter::postSuccess(
    QString title, QString message, bool toast)
{
    return post(Severity::Success, std::move(title), std::move(message), {}, {}, toast);
}

NotificationCenter::Id NotificationCenter::postWarning(
    QString title, QString message, bool toast)
{
    return post(Severity::Warning, std::move(title), std::move(message), {}, {}, toast);
}

NotificationCenter::Id NotificationCenter::postError(
    QString title, QString message, bool toast)
{
    return post(Severity::Error, std::move(title), std::move(message), {}, {}, toast);
}

void NotificationCenter::postOnUi(
    Id id, Severity severity, QString title, QString message, QString actionText,
    std::function<void()> action, bool showToastFlag)
{
    if (!m_state || title.trimmed().isEmpty()) return;
    if (!m_state->notifications.empty()) {
        const auto& newest = m_state->notifications.front();
        if (newest.severity == severity && newest.title == title &&
            newest.message == message) return;
    }

    State::Notification notification;
    notification.id = id;
    notification.severity = severity;
    notification.title = std::move(title);
    notification.message = std::move(message);
    notification.actionText = std::move(actionText);
    notification.action = std::move(action);
    notification.created = QDateTime::currentDateTime();
    m_state->notifications.push_front(std::move(notification));
    while (m_state->notifications.size() > 50) m_state->notifications.pop_back();

    const auto& added = m_state->notifications.front();
    if (showToastFlag)
        showToast(added.severity, added.title, added.message,
                  added.actionText, added.action);
    rebuildNotifications();
    updateStatusButton();
}

NotificationCenter::Id NotificationCenter::beginTask(
    QString title, QString detail, int progress, std::function<void()> cancel,
    std::function<void()> retry)
{
    const Id id = m_nextId.fetch_add(1, std::memory_order_relaxed);
    if (QThread::currentThread() != thread()) {
        QMetaObject::invokeMethod(this,
            [this, id, title = std::move(title), detail = std::move(detail),
             progress, cancel = std::move(cancel), retry = std::move(retry)]() mutable {
                beginTaskOnUi(id, std::move(title), std::move(detail), progress,
                              std::move(cancel), std::move(retry));
            }, Qt::QueuedConnection);
    } else {
        beginTaskOnUi(id, std::move(title), std::move(detail), progress,
                      std::move(cancel), std::move(retry));
    }
    return id;
}

void NotificationCenter::beginTaskOnUi(
    Id id, QString title, QString detail, int progress,
    std::function<void()> cancel, std::function<void()> retry)
{
    if (!m_state || title.trimmed().isEmpty()) return;
    State::Task task;
    task.id = id;
    task.title = std::move(title);
    task.detail = std::move(detail);
    task.progress = progress < 0 ? -1 : std::clamp(progress, 0, 100);
    task.cancel = std::move(cancel);
    task.retry = std::move(retry);
    m_state->tasks.emplace(id, std::move(task));

    State::TaskWidgets widgets;
    widgets.row = new QFrame(m_state->tasksContainer);
    widgets.row->setObjectName(QStringLiteral("ActivityRow"));
    auto* rowLayout = new QVBoxLayout(widgets.row);
    rowLayout->setContentsMargins(10, 8, 10, 8);
    rowLayout->setSpacing(5);
    auto* top = new QHBoxLayout;
    widgets.title = new QLabel(m_state->tasks.at(id).title, widgets.row);
    widgets.title->setObjectName(QStringLiteral("ActivityTitle"));
    widgets.cancel = new QPushButton(tr("Cancel"), widgets.row);
    widgets.cancel->setVisible(static_cast<bool>(m_state->tasks.at(id).cancel));
    top->addWidget(widgets.title, 1);
    top->addWidget(widgets.cancel);
    rowLayout->addLayout(top);
    widgets.detail = new QLabel(m_state->tasks.at(id).detail, widgets.row);
    widgets.detail->setObjectName(QStringLiteral("ActivityDetail"));
    widgets.detail->setWordWrap(true);
    rowLayout->addWidget(widgets.detail);
    widgets.progress = new QProgressBar(widgets.row);
    widgets.progress->setTextVisible(false);
    widgets.progress->setFixedHeight(7);
    rowLayout->addWidget(widgets.progress);
    connect(widgets.cancel, &QPushButton::clicked, this,
            [this, id]() { cancelTask(id); });
    m_state->tasksLayout->addWidget(widgets.row);
    m_state->taskWidgets.emplace(id, widgets);
    const auto& stored = m_state->tasks.at(id);
    updateTaskOnUi(id, stored.progress, stored.detail);
    updateStatusButton();
}

void NotificationCenter::updateTask(Id id, int progress, QString detail)
{
    if (QThread::currentThread() != thread()) {
        QMetaObject::invokeMethod(this,
            [this, id, progress, detail = std::move(detail)]() {
                updateTaskOnUi(id, progress, detail);
            }, Qt::QueuedConnection);
    } else {
        updateTaskOnUi(id, progress, detail);
    }
}

void NotificationCenter::updateTaskOnUi(Id id, int progress, const QString& detail)
{
    const auto taskIt = m_state->tasks.find(id);
    const auto widgetIt = m_state->taskWidgets.find(id);
    if (taskIt == m_state->tasks.end() || widgetIt == m_state->taskWidgets.end()) return;
    auto& task = taskIt->second;
    auto& widgets = widgetIt->second;
    task.progress = progress < 0 ? -1 : std::clamp(progress, 0, 100);
    if (!detail.isEmpty()) task.detail = detail;
    widgets.detail->setText(task.detail);
    widgets.detail->setVisible(!task.detail.isEmpty());
    if (task.progress < 0) {
        widgets.progress->setRange(0, 0);
    } else {
        widgets.progress->setRange(0, 100);
        widgets.progress->setValue(task.progress);
    }
    widgets.cancel->setEnabled(!task.cancelRequested);
}

void NotificationCenter::cancelTask(Id id)
{
    if (QThread::currentThread() != thread()) {
        QMetaObject::invokeMethod(this, [this, id]() { cancelTaskOnUi(id); },
                                  Qt::QueuedConnection);
    } else {
        cancelTaskOnUi(id);
    }
}

void NotificationCenter::cancelTaskOnUi(Id id)
{
    const auto found = m_state->tasks.find(id);
    if (found == m_state->tasks.end() || found->second.cancelRequested ||
        !found->second.cancel) return;

    found->second.cancelRequested = true;
    found->second.detail = tr("Cancelling...");
    const int progress = found->second.progress;
    const auto cancel = found->second.cancel;
    updateTaskOnUi(id, progress, found->second.detail);
    cancel();
}

void NotificationCenter::finishTask(
    Id id, bool success, QString message, bool addNotification)
{
    if (QThread::currentThread() != thread()) {
        QMetaObject::invokeMethod(this,
            [this, id, success, message = std::move(message), addNotification]() {
                finishTaskOnUi(id, success, message, addNotification);
            }, Qt::QueuedConnection);
    } else {
        finishTaskOnUi(id, success, message, addNotification);
    }
}

void NotificationCenter::finishTaskOnUi(
    Id id, bool success, const QString& message, bool addNotification)
{
    const auto taskIt = m_state->tasks.find(id);
    if (taskIt == m_state->tasks.end()) return;
    const QString title = taskIt->second.title;
    const auto retry = taskIt->second.retry;
    const auto widgetIt = m_state->taskWidgets.find(id);
    if (widgetIt != m_state->taskWidgets.end()) {
        widgetIt->second.row->deleteLater();
        m_state->taskWidgets.erase(widgetIt);
    }
    m_state->tasks.erase(taskIt);
    updateStatusButton();
    if (addNotification) {
        post(success ? Severity::Success : Severity::Error,
             title, message.isEmpty() ? (success ? tr("Completed") : tr("Failed"))
                                      : message,
             !success && retry ? tr("Retry") : QString(),
             !success ? retry : std::function<void()>());
    }
}

void NotificationCenter::showCenter()
{
    if (!m_state || !m_state->popup || !m_state->button) return;
    if (m_state->popup->isVisible()) {
        m_state->popup->hide();
        return;
    }
    markAllRead();
    m_state->popup->adjustSize();
    const QPoint buttonTopLeft = m_state->button->mapToGlobal(QPoint(0, 0));
    const int x = buttonTopLeft.x() + m_state->button->width() - m_state->popup->width();
    const int y = buttonTopLeft.y() - m_state->popup->height() - 6;
    m_state->popup->move(std::max(0, x), std::max(0, y));
    m_state->popup->show();
    m_state->popup->raise();
}

void NotificationCenter::clearNotifications()
{
    m_state->notifications.clear();
    rebuildNotifications();
    updateStatusButton();
}

void NotificationCenter::markAllRead()
{
    for (auto& notification : m_state->notifications) notification.read = true;
    rebuildNotifications();
    updateStatusButton();
}

int NotificationCenter::unreadCount() const
{
    return static_cast<int>(std::count_if(
        m_state->notifications.begin(), m_state->notifications.end(),
        [](const auto& item) { return !item.read; }));
}

int NotificationCenter::activeTaskCount() const
{
    return static_cast<int>(m_state->tasks.size());
}

int NotificationCenter::notificationCount() const
{
    return static_cast<int>(m_state->notifications.size());
}

bool NotificationCenter::hasTask(Id id) const
{
    return m_state->tasks.contains(id);
}

QToolButton* NotificationCenter::statusButton() const noexcept
{
    return m_state->button;
}

QWidget* NotificationCenter::popupWidget() const noexcept
{
    return m_state->popup;
}

void NotificationCenter::rebuildNotifications()
{
    while (QLayoutItem* item = m_state->notificationsLayout->takeAt(0)) {
        if (item->widget()) delete item->widget();
        delete item;
    }

    const bool showEmpty = m_state->notifications.empty() && m_state->tasks.empty();
    m_state->notificationsHeading->setVisible(!m_state->notifications.empty());
    m_state->notificationsContainer->setVisible(
        !m_state->notifications.empty() || showEmpty);
    m_state->tasksHeading->setVisible(!m_state->tasks.empty());
    m_state->tasksContainer->setVisible(!m_state->tasks.empty());

    if (m_state->notifications.empty()) {
        auto* empty = new QLabel(tr("No activity yet"), m_state->notificationsContainer);
        empty->setObjectName(QStringLiteral("ActivityDetail"));
        m_state->notificationsLayout->addWidget(empty);
        return;
    }

    for (auto& notification : m_state->notifications) {
        auto* row = new QFrame(m_state->notificationsContainer);
        row->setObjectName(QStringLiteral("ActivityRow"));
        auto* outer = new QHBoxLayout(row);
        outer->setContentsMargins(8, 7, 8, 7);
        outer->setSpacing(8);
        auto* accent = new QLabel(row);
        accent->setFixedSize(4, 36);
        accent->setStyleSheet(QStringLiteral("background:%1; border-radius:2px;")
                              .arg(severityColor(notification.severity)));
        outer->addWidget(accent);
        auto* text = new QVBoxLayout;
        text->setSpacing(2);
        auto* top = new QHBoxLayout;
        auto* title = new QLabel(notification.title, row);
        title->setObjectName(QStringLiteral("ActivityTitle"));
        auto* meta = new QLabel(
            severityLabel(notification.severity) + QStringLiteral("  ") +
            notification.created.toString(QStringLiteral("h:mm AP")), row);
        meta->setObjectName(QStringLiteral("ActivitySection"));
        top->addWidget(title, 1);
        top->addWidget(meta);
        text->addLayout(top);
        if (!notification.message.isEmpty()) {
            auto* detail = new QLabel(notification.message, row);
            detail->setObjectName(QStringLiteral("ActivityDetail"));
            detail->setWordWrap(true);
            text->addWidget(detail);
        }
        outer->addLayout(text, 1);
        if (!notification.actionText.isEmpty() && notification.action) {
            auto* action = new QPushButton(notification.actionText, row);
            const Id id = notification.id;
            connect(action, &QPushButton::clicked, this, [this, id]() {
                const auto found = std::find_if(
                    m_state->notifications.begin(), m_state->notifications.end(),
                    [id](const auto& item) { return item.id == id; });
                if (found != m_state->notifications.end() && found->action)
                    found->action();
                m_state->popup->hide();
            });
            outer->addWidget(action);
        }
        m_state->notificationsLayout->addWidget(row);
    }
}

void NotificationCenter::updateStatusButton()
{
    const int tasks = activeTaskCount();
    const int unread = unreadCount();
    QString text = tr("Activity");
    if (tasks > 0 && unread > 0)
        text = tr("Tasks %1 | Alerts %2").arg(tasks).arg(unread);
    else if (tasks > 0)
        text = tr("Tasks %1").arg(tasks);
    else if (unread > 0)
        text = tr("Alerts %1").arg(unread);
    m_state->button->setText(text);
    emit countsChanged(tasks, unread);
}

void NotificationCenter::showToast(
    Severity severity, const QString& title, const QString& message,
    const QString& actionText, const std::function<void()>& action)
{
    const auto& c = Theme::colors();
    m_state->toast->setStyleSheet(QStringLiteral(
        "QFrame#ActivityToast { background:%1; border:1px solid %2; border-radius:7px; }"
        "QLabel#ActivityTitle { color:%3; font-weight:600; }"
        "QLabel#ActivityDetail { color:%4; }")
        .arg(Theme::hex(c.surface3), Theme::hex(c.borderLight),
             Theme::hex(c.textPrimary), Theme::hex(c.textSecondary)));
    m_state->toastAccent->setStyleSheet(
        QStringLiteral("background:%1; border-radius:2px;")
            .arg(severityColor(severity)));
    m_state->toastTitle->setText(title);
    m_state->toastMessage->setText(message);
    m_state->toastMessage->setVisible(!message.isEmpty());
    QObject::disconnect(m_state->toastAction, nullptr, this, nullptr);
    if (!actionText.isEmpty() && action) {
        m_state->toastAction->setText(actionText);
        m_state->toastAction->show();
        connect(m_state->toastAction, &QPushButton::clicked, this,
                [this, action]() {
                    action();
                    m_state->toast->hide();
                });
    } else {
        m_state->toastAction->hide();
    }
    m_state->toast->adjustSize();
    positionOverlays();
    m_state->toast->show();
    m_state->toast->raise();
    m_state->toastTimer->start();
}

void NotificationCenter::positionOverlays()
{
    if (!m_state || !m_state->window || !m_state->toast) return;
    const int margin = 16;
    const int statusHeight = m_state->statusBar ? m_state->statusBar->height() : 0;
    const int x = m_state->window->width() - m_state->toast->width() - margin;
    const int y = m_state->window->height() - statusHeight -
                  m_state->toast->height() - margin;
    m_state->toast->move(std::max(margin, x), std::max(margin, y));
}

bool NotificationCenter::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == m_state->window &&
        (event->type() == QEvent::Resize || event->type() == QEvent::Move)) {
        positionOverlays();
    }
    return QObject::eventFilter(watched, event);
}

} // namespace rt
