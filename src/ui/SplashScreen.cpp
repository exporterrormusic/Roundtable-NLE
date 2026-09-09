/*
 * SplashScreen.cpp — Implementation.
 */

#include "SplashScreen.h"

#include <QApplication>
#include <QGuiApplication>
#include <QPainter>
#include <QScreen>
#include <QVBoxLayout>
#include <algorithm>

#ifdef _WIN32
#include <windows.h>
#endif

namespace rt {

static constexpr int kBorderStroke   = 6;    // white border thickness
static constexpr int kStatusBarHeight = 28;   // progress bar height

SplashScreen::SplashScreen(const QString &iconPath, const QString &version,
                           QWidget *parent)
    : QWidget(parent)
    , m_version(version)
{
    // Load icon at native size
    m_logo = QPixmap(iconPath);
    int iw = m_logo.isNull() ? 400 : m_logo.width();
    int ih = m_logo.isNull() ? 400 : m_logo.height();

    // Window = icon area + bottom bar + border on all sides
    int innerW = iw;
    int innerH = ih + kStatusBarHeight;
    setWindowFlags(Qt::Window | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
    setAttribute(Qt::WA_TranslucentBackground, false);
    setFixedSize(innerW + kBorderStroke * 2, innerH + kBorderStroke * 2);

    setStyleSheet("background-color: #0e0e14;");

    // ── Progress bar pinned to the bottom (no layout gaps) ─────────────
    m_progressBar = new QProgressBar(this);
    m_progressBar->setRange(0, 100);
    m_progressBar->setValue(0);
    m_progressBar->setTextVisible(true);
    m_progressBar->setFormat("Initializing...");
    m_progressBar->setAlignment(Qt::AlignCenter);
    m_progressBar->setStyleSheet(
        "QProgressBar {"
        "  background-color: #1a1a24;"
        "  border: none;"
        "  margin: 0px;"
        "  padding: 0px;"
        "  color: #ffffff;"
        "  font-size: 12px;"
        "  font-family: 'Segoe UI', sans-serif;"
        "}"
        "QProgressBar::chunk {"
        "  background-color: #4a90d9;"
        "}"
    );
    // Keep the bar attached to the image through Qt's layout system.  Manual
    // child geometry can briefly use the wrong coordinate space when Windows
    // moves a new top-level window onto a monitor with different DPI scaling.
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(kBorderStroke, kBorderStroke,
                               kBorderStroke, kBorderStroke);
    layout->setSpacing(0);
    layout->addSpacing(ih);
    m_progressBar->setFixedHeight(kStatusBarHeight);
    layout->addWidget(m_progressBar);

    // Center on primary screen
    if (auto *screen = QGuiApplication::primaryScreen()) {
        QRect screenGeom = screen->availableGeometry();
        move(screenGeom.center() - rect().center());
    }
}

void SplashScreen::setProgress(int percent)
{
    m_progress = std::clamp(percent, 0, 100);
    m_progressBar->setValue(m_progress);
    QApplication::processEvents();
}

void SplashScreen::setStatus(const QString &text)
{
    m_progressBar->setFormat(text);
    QApplication::processEvents();
}

void SplashScreen::finish(QWidget *mainWindow)
{
    // Forcefully dismiss the splash.  On Windows, a frameless topmost window
    // can be stubborn about hide() — we use the Win32 API to destroy the HWND
    // and then process events so Qt doesn't get confused.

    // First tell Qt the widget is hidden (updates internal state so the widget
    // can't interfere with modal dialogs that may be triggered by processEvents).
    hide();

#ifdef _WIN32
    if (HWND hwnd = reinterpret_cast<HWND>(winId())) {
        // Remove the topmost style so the splash stops blocking input.
        SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        // Destroy the window entirely so it can't block modal dialogs.
        DestroyWindow(hwnd);
    }
#endif

    // Show the main window BEFORE processing events — this ensures the main
    // window is on screen in case processEvents triggers any modal dialogs
    // (e.g. crash-recovery QMessageBox) that would otherwise block execution
    // before the main window is raised.
    if (mainWindow) {
#ifdef _WIN32
        if (HWND mwHwnd = reinterpret_cast<HWND>(mainWindow->winId())) {
            SetWindowPos(mwHwnd, HWND_TOP, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
        }
#else
        mainWindow->showNormal();
#endif
        mainWindow->raise();
        mainWindow->activateWindow();
    }

    QApplication::processEvents();
}

void SplashScreen::paintEvent(QPaintEvent * /*event*/)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    painter.setRenderHint(QPainter::Antialiasing);

    // ── Full dark background under everything ─────────────────────────
    painter.fillRect(rect(), QColor(14, 14, 20));

    // ── Inner content area (inside the border) ────────────────────────
    int innerL = kBorderStroke;
    int innerT = kBorderStroke;
    int innerW = width()  - kBorderStroke * 2;
    int innerH = height() - kBorderStroke * 2;

    // Icon area height = inner area minus bottom bar
    int iconAreaH = innerH - kStatusBarHeight;

    // Draw icon centered in the icon area
    if (!m_logo.isNull()) {
        int cx = innerL + (innerW - m_logo.width())  / 2;
        int cy = innerT + (iconAreaH - m_logo.height()) / 2;
        painter.drawPixmap(cx, cy, m_logo);

        // ── Version text ON the icon, bottom-right corner ───────────
        painter.setPen(Qt::white);
        QFont vf = painter.font();
        vf.setPixelSize(14);
        vf.setFamily("Segoe UI");
        vf.setBold(true);
        painter.setFont(vf);

        QString versionText = "v" + m_version;
        QFontMetrics fm(vf);
        int textW = fm.horizontalAdvance(versionText);
        int textH = fm.height();

        int padRight  = 10;
        int padBottom = 8;
        int tx = cx + m_logo.width()  - textW - padRight;
        int ty = cy + m_logo.height() - padBottom;

        // Semi-transparent dark pill behind text
        int pillPadX = 6;
        int pillPadY = 2;
        QRect pillRect(tx - pillPadX,
                       ty - textH + fm.descent() - pillPadY,
                       textW + pillPadX * 2,
                       textH + pillPadY * 2);
        painter.setBrush(QColor(0, 0, 0, 160));
        painter.setPen(Qt::NoPen);
        painter.drawRoundedRect(pillRect, 4, 4);

        painter.setPen(Qt::white);
        painter.drawText(tx, ty, versionText);
    }

    // ── White border on the EXTERIOR edge (content inset by border) ────
    int half = kBorderStroke / 2;
    QPen borderPen(Qt::white, kBorderStroke, Qt::SolidLine, Qt::SquareCap, Qt::MiterJoin);
    painter.setPen(borderPen);
    painter.setBrush(Qt::NoBrush);
    painter.drawRect(rect().adjusted(half, half, -half, -half));
}

void SplashScreen::mousePressEvent(QMouseEvent * /*event*/)
{
    // Clicking the splash immediately dismisses it and raises the main window.
    // This provides a manual escape if the auto-dismiss timer hasn't fired.
    finish(nullptr);
}

} // namespace rt
