/*
 * ScrubGesture.cpp — see ScrubGesture.h.
 */

#include "widgets/ScrubGesture.h"

#include <QApplication>
#include <QCursor>
#include <QGuiApplication>
#include <QScreen>

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

int ScrubGesture::move(QPoint globalPos)
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
    return dx;
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
