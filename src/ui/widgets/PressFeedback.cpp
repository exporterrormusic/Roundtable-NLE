/*
 * PressFeedback.cpp — see PressFeedback.h.
 */

#include "widgets/PressFeedback.h"

#include <QApplication>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QTimer>
#include <QToolButton>

namespace rt {

namespace {

constexpr const char* kDecisionProperty = "rtPressFx"; // 1 = overlay, 2 = skip

class PressOverlay : public QWidget
{
public:
    explicit PressOverlay(QWidget* parent) : QWidget(parent)
    {
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_NoSystemBackground);
        setFocusPolicy(Qt::NoFocus);
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        QPainterPath path;
        path.addRoundedRect(QRectF(rect()), 3.0, 3.0);
        p.fillPath(path, QColor(0, 0, 0, 64));
    }
};

} // namespace

PressFeedback::PressFeedback(QObject* parent)
    : QObject(parent)
{
    m_timer = new QTimer(this);
    m_timer->setInterval(16);
    connect(m_timer, &QTimer::timeout, this, &PressFeedback::sync);
}

void PressFeedback::install(QApplication* app)
{
    static PressFeedback* s_instance = nullptr;
    if (s_instance || !app) return;
    s_instance = new PressFeedback(app);
    app->installEventFilter(s_instance);
}

bool PressFeedback::wantsOverlay(QAbstractButton* button)
{
    const QVariant cached = button->property(kDecisionProperty);
    if (cached.isValid())
        return cached.toInt() == 1;

    // A local stylesheet (the button's own or an ancestor's) that sets a
    // background overrides the theme's :pressed rule unless it supplies
    // its own. No local background at all → the theme's :pressed applies.
    bool want = false;
    for (QWidget* w = button; w; w = w->parentWidget()) {
        const QString ss = w->styleSheet();
        if (ss.isEmpty()) continue;
        if (ss.contains(QLatin1String(":pressed"))) { want = false; break; }
        if (ss.contains(QLatin1String("background"))) want = true;
    }
    button->setProperty(kDecisionProperty, want ? 1 : 2);
    return want;
}

bool PressFeedback::eventFilter(QObject* obj, QEvent* event)
{
    if (event->type() != QEvent::MouseButtonPress
        && event->type() != QEvent::MouseButtonDblClick)
        return false;
    if (static_cast<QMouseEvent*>(event)->button() != Qt::LeftButton)
        return false;

    // Exact classes only: subclasses paint their own pressed state.
    const QMetaObject* mo = obj->metaObject();
    if (mo != &QPushButton::staticMetaObject && mo != &QToolButton::staticMetaObject)
        return false;
    auto* button = static_cast<QAbstractButton*>(obj);
    if (!button->isEnabled() || !wantsOverlay(button))
        return false;

    m_button = button;
    // The button sets its down state after this filter returns.
    QTimer::singleShot(0, this, &PressFeedback::sync);
    m_timer->start();
    return false;
}

void PressFeedback::sync()
{
    const bool down = m_button && m_button->isDown() && m_button->isVisible();
    if (down) {
        if (!m_overlay)
            m_overlay = new PressOverlay(m_button);
        else if (m_overlay->parentWidget() != m_button)
            m_overlay->setParent(m_button);
        m_overlay->setGeometry(m_button->rect());
        m_overlay->raise();
        m_overlay->show();
        return;
    }

    if (m_overlay)
        m_overlay->hide();
    // Keep polling while the mouse is held (the pointer can slide back
    // onto the button) or while a popup menu keeps the button down.
    if (QGuiApplication::mouseButtons() == Qt::NoButton) {
        m_timer->stop();
        m_button = nullptr;
    }
}

} // namespace rt
