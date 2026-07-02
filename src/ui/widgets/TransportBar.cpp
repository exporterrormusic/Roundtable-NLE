/*
 * TransportBar.cpp — Transport control bar implementation.
 * Step 14
 */

#include "widgets/TransportBar.h"
#include "Theme.h"

#include "playback/PlaybackController.h"

#include <QHBoxLayout>
#include <QKeyEvent>
#include <QPainter>
#include <QStyle>

namespace rt {

// ═════════════════════════════════════════════════════════════════════════════
//  Construction
// ═════════════════════════════════════════════════════════════════════════════

TransportBar::TransportBar(QWidget* parent)
    : QWidget(parent)
{
    setupUI();

    m_pollTimer = new QTimer(this);
    m_pollTimer->setTimerType(Qt::PreciseTimer);
    connect(m_pollTimer, &QTimer::timeout, this, &TransportBar::onPollTimer);
}

TransportBar::~TransportBar()
{
    m_destroying.store(true, std::memory_order_release);

    // Stop the poll timer — prevents it firing during destruction
    if (m_pollTimer) {
        m_pollTimer->stop();
    }
}

// ═════════════════════════════════════════════════════════════════════════════
//  Setup
// ═════════════════════════════════════════════════════════════════════════════

void TransportBar::setupUI()
{
    setFixedHeight(kBarHeight);
    setFocusPolicy(Qt::StrongFocus);

    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(4, 0, 4, 0);
    layout->setSpacing(0);

    layout->addStretch();

    // ── Transport buttons (Premiere Pro style: flat, centered) ──────────
    m_btnGoStart     = new QPushButton(this);
    m_btnStepBack    = new QPushButton(this);
    m_btnStop        = new QPushButton(this);
    m_btnPlayPause   = new QPushButton(this);
    m_btnStepForward = new QPushButton(this);
    m_btnGoEnd       = new QPushButton(this);
    m_btnLoop        = new QPushButton(this);

    styleButton(m_btnGoStart,     QStringLiteral("\u23EE"), tr("Go to Start (Home)"));
    styleButton(m_btnStepBack,    QStringLiteral("\u23EA"), tr("Step Back (Left)"));
    styleButton(m_btnStop,        QStringLiteral("\u23F9"), tr("Stop (S)"));
    styleButton(m_btnPlayPause,   QStringLiteral("\u25B6"), tr("Play/Pause (Space)"));
    styleButton(m_btnStepForward, QStringLiteral("\u23E9"), tr("Step Forward (Right)"));
    styleButton(m_btnGoEnd,       QStringLiteral("\u23ED"), tr("Go to End (End)"));
    styleButton(m_btnLoop,        QStringLiteral("\U0001F501"), tr("Toggle Loop"));

    // Play button is larger and prominent (Premiere style)
    m_btnPlayPause->setFixedSize(28, 28);
    m_btnPlayPause->setStyleSheet(
        QString("QPushButton { background: transparent; border: none; "
                        "color: %1; font-size: %4px; border-radius: 14px; } "
                        "QPushButton:hover { background: %2; } "
                        "QPushButton:pressed { background: %3; }")
            .arg(Theme::hex(Theme::colors().textPrimary),
                 Theme::hex(Theme::colors().surface2),
                 Theme::hex(Theme::colors().surface3),
                 QString::number(Theme::typography().sizeBody)));

    m_btnLoop->setCheckable(true);

    layout->addWidget(m_btnGoStart);
    layout->addWidget(m_btnStepBack);
    layout->addWidget(m_btnPlayPause);
    layout->addWidget(m_btnStop);
    layout->addWidget(m_btnStepForward);
    layout->addWidget(m_btnGoEnd);

    layout->addSpacing(4);
    layout->addWidget(m_btnLoop);

    layout->addStretch();

    // ── Timecode display ────────────────────────────────────────────────
    layout->addSpacing(8);
    m_timecodeLabel = new QLabel(QStringLiteral("00:00:00:00"), this);
    m_timecodeLabel->setStyleSheet(
        QString("QLabel { font-family: 'Consolas', 'Courier New', monospace; "
                        "font-size: 13px; font-weight: bold; color: %1; "
                        "background: transparent; padding: 1px 4px; }")
            .arg(Theme::hex(Theme::colors().textPrimary)));
    m_timecodeLabel->setMinimumWidth(110);
    m_timecodeLabel->setAlignment(Qt::AlignCenter);
    layout->addWidget(m_timecodeLabel);

    // ── Duration / Remaining display (click to toggle) ──────────────────
    layout->addSpacing(2);
    m_durationLabel = new QLabel(QStringLiteral("/ 00:00:00:00"), this);
    m_durationLabel->setStyleSheet(
        QString("QLabel { font-family: 'Consolas', 'Courier New', monospace; "
                        "font-size: %2px; color: %1; background: transparent; "
                        "padding: 1px 2px; }")
            .arg(Theme::hex(Theme::colors().textDisabled),
                 QString::number(Theme::typography().sizeXxs)));
    m_durationLabel->setMinimumWidth(90);
    m_durationLabel->setCursor(Qt::PointingHandCursor);
    m_durationLabel->setToolTip(tr("Click to toggle Duration / Remaining"));
    m_durationLabel->installEventFilter(this);
    layout->addWidget(m_durationLabel);

    // ── Speed indicator ─────────────────────────────────────────────────
    layout->addSpacing(4);
    m_speedLabel = new QLabel(this);
    m_speedLabel->setStyleSheet(
        QString("QLabel { font-size: %2px; color: %1; }")
            .arg(Theme::hex(Theme::colors().accent),
                 QString::number(Theme::typography().sizeXxs)));
    m_speedLabel->setMinimumWidth(40);
    layout->addWidget(m_speedLabel);

    // ── Frame drop indicator (green/yellow/red dot) ─────────────────────
    layout->addSpacing(4);
    m_dropIndicator = new QLabel(this);
    m_dropIndicator->setFixedSize(10, 10);
    m_dropIndicator->setToolTip(tr("Dropped frames"));
    m_dropIndicator->hide();  // hidden until drops are detected
    layout->addWidget(m_dropIndicator);

    layout->addStretch();

    // ── Connect buttons ─────────────────────────────────────────────────
    connect(m_btnGoStart,     &QPushButton::clicked, this, [this]() {
        if (m_destroying.load(std::memory_order_acquire)) return;
        if (m_controller) m_controller->goToStart();
    });
    connect(m_btnStepBack,    &QPushButton::clicked, this, [this]() {
        if (m_destroying.load(std::memory_order_acquire)) return;
        if (m_controller) m_controller->stepBackward();
    });
    connect(m_btnStop,        &QPushButton::clicked, this, [this]() {
        if (m_destroying.load(std::memory_order_acquire)) return;
        if (m_controller) m_controller->stop();
    });
    connect(m_btnPlayPause,   &QPushButton::clicked, this, [this]() {
        if (m_destroying.load(std::memory_order_acquire)) return;
        if (m_controller) m_controller->togglePlayPause();
    });
    connect(m_btnStepForward, &QPushButton::clicked, this, [this]() {
        if (m_destroying.load(std::memory_order_acquire)) return;
        if (m_controller) m_controller->stepForward();
    });
    connect(m_btnGoEnd,       &QPushButton::clicked, this, [this]() {
        if (m_destroying.load(std::memory_order_acquire)) return;
        if (m_controller) m_controller->goToEnd();
    });
    connect(m_btnLoop,        &QPushButton::toggled, this, [this](bool checked) {
        if (m_destroying.load(std::memory_order_acquire)) return;
        if (m_controller) m_controller->setLoopEnabled(checked);
    });
}

void TransportBar::styleButton(QPushButton* btn, const QString& text, const QString& tooltip)
{
    btn->setText(text);
    btn->setToolTip(tooltip);
    btn->setFixedSize(24, 24);
    btn->setFocusPolicy(Qt::NoFocus);
    btn->setStyleSheet(
        QString("QPushButton { background: transparent; border: none; "
                        "color: %1; font-size: %6px; } "
                        "QPushButton:hover { color: %2; background: %3; border-radius: 12px; } "
                        "QPushButton:pressed { background: %4; } "
                        "QPushButton:checked { color: %5; }")
            .arg(Theme::hex(Theme::colors().textSecondary),
                 Theme::hex(Theme::colors().textBright),
                 Theme::hex(Theme::colors().surface2),
                 Theme::hex(Theme::colors().surface3),
                 Theme::hex(Theme::colors().accent),
                 QString::number(Theme::typography().sizeXs)));
}

// ═════════════════════════════════════════════════════════════════════════════
//  Controller attachment
// ═════════════════════════════════════════════════════════════════════════════

void TransportBar::setController(PlaybackController* controller)
{
    m_controller = controller;

    if (m_controller)
    {
        // Wire callbacks
        m_controller->onPositionChanged = [this](int64_t tick) {
            if (m_destroying.load(std::memory_order_acquire)) return;
            emit playheadChanged(tick);
        };

        m_controller->onStateChanged = [this](PlayState /*state*/) {
            if (m_destroying.load(std::memory_order_acquire)) return;
            updateButtonStates();
        };

        m_controller->onSpeedChanged = [this](double /*speed*/) {
            if (m_destroying.load(std::memory_order_acquire)) return;
            updateDisplay();
        };
    }
}

// ═════════════════════════════════════════════════════════════════════════════
//  Polling
// ═════════════════════════════════════════════════════════════════════════════

void TransportBar::startPolling(int intervalMs)
{
    m_pollTimer->start(intervalMs);
}

void TransportBar::stopPolling()
{
    m_pollTimer->stop();
}

void TransportBar::onPollTimer()
{
    if (!m_controller) return;

    // Skip polling when stopped/paused — nothing to update.
    if (!m_controller->isPlaying())
        return;

    (void)m_controller->pollPosition();
    updateDisplay();
}

// ═════════════════════════════════════════════════════════════════════════════
//  Display updates
// ═════════════════════════════════════════════════════════════════════════════

void TransportBar::updateDisplay()
{
    if (!m_controller) return;

    // Timecode
    std::string tc = m_controller->currentTimecodeString();
    m_timecodeLabel->setText(QString::fromStdString(tc));

    // Duration / Remaining display
    if (m_durationLabel && m_controller) {
        int64_t totalDur = m_controller->durationTicks();
        int64_t current  = m_controller->currentTick();
        double fps = m_controller->frameRate();
        if (m_showRemaining) {
            int64_t remaining = std::max(totalDur - current, int64_t(0));
            auto remTc = tickToTimecode(remaining, fps);
            m_durationLabel->setText(QStringLiteral("-%1").arg(QString::fromStdString(remTc.toString())));
        } else {
            auto durTc = tickToTimecode(totalDur, fps);
            m_durationLabel->setText(QStringLiteral("/ %1").arg(QString::fromStdString(durTc.toString())));
        }
    }

    // Speed indicator
    double speed = m_controller->shuttleSpeed();
    if (m_controller->state() == PlayState::Shuttling && speed != 1.0)
    {
        if (speed < 0.0)
            m_speedLabel->setText(QStringLiteral("\u25C0 %1x").arg(-speed, 0, 'g', 2));
        else
            m_speedLabel->setText(QStringLiteral("\u25B6 %1x").arg(speed, 0, 'g', 2));
    }
    else if (m_controller->state() == PlayState::Playing)
    {
        m_speedLabel->setText(QStringLiteral("\u25B6 1x"));
    }
    else
    {
        m_speedLabel->clear();
    }

    // ── Frame drop indicator ────────────────────────────────────────────
    if (m_dropIndicator && m_dropProvider) {
        int drops = m_dropProvider();
        if (drops < 0) {
            m_dropIndicator->hide();
        } else {
            // Green: no recent drops    Yellow: occasional drops    Red: dropping badly
            QString color;
            if (drops == 0)
                color = Theme::hex(Theme::colors().success);  // green
            else if (drops <= 5)
                color = Theme::hex(Theme::colors().warning);  // yellow
            else
                color = Theme::hex(Theme::colors().error);    // red
            m_dropIndicator->setStyleSheet(
                QString("QLabel { background: %1; border-radius: 5px; }").arg(color));
            m_dropIndicator->setToolTip(
                drops == 0 ? tr("No dropped frames")
                           : tr("Dropped frames: %1").arg(drops));
            m_dropIndicator->show();
        }
        m_lastDropCount = drops;
    }

    updateButtonStates();
}

void TransportBar::updateButtonStates()
{
    if (!m_controller) return;

    bool playing = m_controller->isPlaying();

    // Toggle play/pause icon
    m_btnPlayPause->setText(playing ? QStringLiteral("\u23F8") : QStringLiteral("\u25B6"));

    // Loop button state
    m_btnLoop->setChecked(m_controller->isLoopEnabled());
}

// ═════════════════════════════════════════════════════════════════════════════
//  Size hints
// ═════════════════════════════════════════════════════════════════════════════

QSize TransportBar::sizeHint() const
{
    return {600, kBarHeight};
}

QSize TransportBar::minimumSizeHint() const
{
    return {300, kBarHeight};
}

// ═════════════════════════════════════════════════════════════════════════════
//  Key handling — JKL shuttle and other transport keys
// ═════════════════════════════════════════════════════════════════════════════

void TransportBar::keyPressEvent(QKeyEvent* event)
{
    if (!m_controller)
    {
        QWidget::keyPressEvent(event);
        return;
    }

    bool handled = true;

    switch (event->key())
    {
    case Qt::Key_Space:
        m_controller->togglePlayPause();
        break;

    case Qt::Key_J:
        m_controller->shuttleReverse();
        break;

    case Qt::Key_K:
        m_controller->shuttlePause();
        break;

    case Qt::Key_L:
        if (event->modifiers() == Qt::NoModifier)
            m_controller->shuttleForward();
        else
            handled = false;
        break;

    case Qt::Key_Left:
        m_controller->stepBackward();
        break;

    case Qt::Key_Right:
        m_controller->stepForward();
        break;

    case Qt::Key_Up:
        m_controller->goToPrevEditPoint();
        break;

    case Qt::Key_Down:
        m_controller->goToNextEditPoint();
        break;

    case Qt::Key_Home:
        m_controller->goToStart();
        break;

    case Qt::Key_End:
        m_controller->goToEnd();
        break;

    default:
        handled = false;
        break;
    }

    if (handled)
        event->accept();
    else
        QWidget::keyPressEvent(event);
}

// ═════════════════════════════════════════════════════════════════════════════
//  Paint
// ═════════════════════════════════════════════════════════════════════════════

void TransportBar::paintEvent(QPaintEvent* event)
{
    static thread_local int s_paintDepth = 0;
    if (++s_paintDepth > 5) {
        --s_paintDepth;
        QWidget::paintEvent(event);
        return;
    }
    QPainter painter(this);
    painter.fillRect(rect(), Theme::colors().surface0);

    // Subtle top border (Premiere style)
    painter.setPen(Theme::colors().border);
    painter.drawLine(0, 0, width(), 0);

    --s_paintDepth;
}

bool TransportBar::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == m_durationLabel && event->type() == QEvent::MouseButtonPress) {
        m_showRemaining = !m_showRemaining;
        updateDisplay();
        return true;
    }
    return QWidget::eventFilter(watched, event);
}

} // namespace rt

