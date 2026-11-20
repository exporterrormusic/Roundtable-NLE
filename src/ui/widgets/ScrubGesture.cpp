/*
 * ScrubGesture.cpp — see ScrubGesture.h.
 */

#include "widgets/ScrubGesture.h"

#include <QApplication>
#include <QCursor>
#include <QGuiApplication>
#include <QScreen>

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace rt {

namespace {
// Distance from the screen edge at which the hidden cursor is recentred.
constexpr int kEdgeMarginPx = 40;
}

void ScrubGesture::press(QPoint globalPos)
{
    cancel();
    m_pressPos   = globalPos;
    m_lastPos    = globalPos;
    m_justWarped = false;
    m_state      = State::Armed;
}

double ScrubGesture::speedGain(double pxPerSecond) noexcept
{
    if (!(pxPerSecond > kSlowSpeed)) return kSlowGain;
    if (pxPerSecond >= kFastSpeed) return 1.0;
    const double t = (pxPerSecond - kSlowSpeed) / (kFastSpeed - kSlowSpeed);
    const double smooth = t * t * (3.0 - 2.0 * t);
    return kSlowGain + (1.0 - kSlowGain) * smooth;
}

double ScrubGesture::move(QPoint globalPos, quint64 timestampMs)
{
    if (m_state == State::Idle)
        return 0;

    if (m_state == State::Armed) {
        if (std::abs(globalPos.x() - m_pressPos.x()) < kDragThreshold)
            return 0;
        // Engage. Start measuring from HERE so the threshold distance
        // doesn't land on the value as a jump.
        m_state   = State::Active;
        m_lastPos = globalPos;
        // Speed is measured from here, starting at rest.
        m_lastTimeMs = timestampMs;
        m_speed      = 0.0;
        QApplication::setOverrideCursor(Qt::BlankCursor);
        return 0;
    }

    int dx = globalPos.x() - m_lastPos.x();

    QScreen* screen = QGuiApplication::screenAt(globalPos);
    const QRect geo = screen ? screen->geometry() : QRect();

    if (m_justWarped) {
        // A stale pre-warp event shows up as a jump of about half a screen.
        // Drop it (and keep the post-warp baseline) instead of applying it.
        const int stale = geo.isValid() ? geo.width() / 4 : 400;
        if (std::abs(dx) > stale)
            return 0;
        m_justWarped = false;
    }
    m_lastPos = globalPos;

    // Near the edge: recentre the (hidden) cursor so the scrub can go on
    // indefinitely.
    if (geo.isValid()
        && (globalPos.x() < geo.left() + kEdgeMarginPx
            || globalPos.x() > geo.right() - kEdgeMarginPx)) {
        const QPoint centre(geo.center().x(), globalPos.y());
        QCursor::setPos(screen, centre);
        m_lastPos    = centre;
        m_justWarped = true;
    }
    // Speed-scaled movement.  No timestamp: unscaled (synthesized events).
    if (timestampMs == 0)
        return static_cast<double>(dx);
    if (m_lastTimeMs != 0 && timestampMs > m_lastTimeMs) {
        const double dt = static_cast<double>(timestampMs - m_lastTimeMs);
        const double instant = std::abs(dx) * 1000.0 / dt;
        const double alpha = 1.0 - std::exp(-dt / kSpeedTimeConstantMs);
        m_speed += alpha * (instant - m_speed);
    }
    // (Same-millisecond events keep the current estimate.)
    if (timestampMs > m_lastTimeMs) m_lastTimeMs = timestampMs;
    return static_cast<double>(dx) * speedGain(m_speed);
}

bool ScrubGesture::release()
{
    if (m_state != State::Active) {
        m_state = State::Idle;
        return false;
    }
    endActive();
    QCursor::setPos(m_pressPos);
    return true;
}

void ScrubGesture::cancel()
{
    if (m_state == State::Active)
        endActive();
    m_state = State::Idle;
}

void ScrubGesture::endActive()
{
    QApplication::restoreOverrideCursor();
    m_state      = State::Idle;
    m_justWarped = false;
}

} // namespace rt
