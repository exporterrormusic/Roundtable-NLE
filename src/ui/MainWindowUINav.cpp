/*
 * MainWindowUINav.cpp — Navigation-rail & page-switching extracted from
 * MainWindowUI.cpp.
 *
 * Contains: setupPageTabs(), setCurrentPage(), currentPage(),
 * toggleNavRail(), onPageTabChanged().
 */

#include "MainWindow.h"
#include "panels/timeline/TimelineWorkspace.h"

#include "Theme.h"
#include "Settings.h"

// Delegated panel headers (for page-switch refresh)
#include "panels/monitors/ProgramMonitor.h"
#include "panels/timeline/TimelinePanel.h"
#include "panels/characters/CharacterShotPanel.h"
#include "panels/characters/ShotComposer.h"
#include "panels/project/ProjectPanel.h"
#include "spine/ShotPreset.h"

#include <QButtonGroup>
#include <QCoreApplication>
#include <QFrame>
#include <QLabel>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QStackedWidget>
#include <QTimer>
#include <QTransform>
#include <QVBoxLayout>
#include <QWidget>

#include <array>

namespace rt {

// ═════════════════════════════════════════════════════════════════════════════
// Tab system setup
// ═════════════════════════════════════════════════════════════════════════════

void MainWindow::setupPageTabs()
{
    const auto& c = Theme::colors();
    const auto& m = Theme::metrics();

    // Central widget: HBox with nav rail on left, content on right
    auto* centralContainer = new QWidget(this);
    auto* hbox = new QHBoxLayout(centralContainer);
    hbox->setContentsMargins(0, 0, 0, 0);
    hbox->setSpacing(0);

    // ── Vertical nav rail (left sidebar) ────────────────────────────────
    m_navRail = new QWidget(centralContainer);
    m_navRail->setObjectName("MainNavRail");
    m_navRail->setFixedWidth(150);
    m_navRail->setStyleSheet(QStringLiteral(
        "#MainNavRail { background: rgb(%1,%2,%3); }")
        .arg(std::max(0, c.surface0.red()   - 4))
        .arg(std::max(0, c.surface0.green() - 4))
        .arg(std::max(0, c.surface0.blue()  - 4)));

    // Separator strip between main nav rail and content — accent glow edge
    auto* navSeparator = new QWidget(centralContainer);
    navSeparator->setFixedWidth(3);
    navSeparator->setStyleSheet(QStringLiteral(
        "background: qlineargradient(x1:0, y1:0, x2:1, y2:0,"
        "  stop:0 %1, stop:0.5 %2, stop:1 %3);")
        .arg(Theme::rgb(c.accentDim))
        .arg(Theme::rgb(c.accent))
        .arg(Theme::rgb(c.surface1)));

    auto* railLayout = new QVBoxLayout(m_navRail);
    railLayout->setContentsMargins(8, m.spacingXl, 8, m.spacingXl);
    railLayout->setSpacing(0);
    railLayout->setAlignment(Qt::AlignTop);

    // Nav button style — icon + label stacked vertically (matches ProjectPanel rail)
    QString navBtnStyle = QStringLiteral(
        "QPushButton { background: transparent; border: none;"
        "  border-radius: %1px; color: %2; font-size: 42px;"
        "  padding: 12px 0; }"
        "QPushButton:hover { background: %3; color: %4; }"
        "QPushButton:pressed { background: %5; color: white; }"
        "QPushButton:checked { background: %6; color: %7; }")
        .arg(m.radiusXl)
        .arg(Theme::rgb(c.textTertiary))
        .arg(Theme::rgb(c.surface3))
        .arg(Theme::rgb(c.textPrimary))
        .arg(Theme::rgb(c.accentDim))
        .arg(Theme::rgb(c.accentDim))
        .arg(Theme::rgb(c.accent));

    struct NavEntry { const char* icon; const char* label; };
    NavEntry entries[] = {
        {"\U0001F4C1", "PROJECTS"},
        {"\U0001F464", "CHARACTERS"},
        {"\U0001F3B5", "AUDIO"},
        {"\U0001F39E", "TIMELINE"},
        {"\U0001F4E4", "EXPORT"}
    };

    m_navGroup = new QButtonGroup(this);
    m_navGroup->setExclusive(true);

    // Divider block between nav entries — single container widget
    // wrapping a 1px line to prevent DPI sub-pixel drift.
    auto makeDivider = [&]() {
        auto* div = new QWidget;
        div->setFixedHeight(17);
        auto* lay = new QVBoxLayout(div);
        lay->setContentsMargins(16, 0, 16, 0);
        lay->setSpacing(0);
        lay->addStretch();
        auto* line = new QFrame;
        line->setFixedHeight(1);
        line->setStyleSheet(QStringLiteral("background: %1;").arg(Theme::rgb(c.border)));
        lay->addWidget(line);
        lay->addStretch();
        return div;
    };

    for (int i = 0; i < 5; ++i) {
        auto* btn = new QPushButton(QString::fromUtf8(entries[i].icon));
        btn->setCheckable(true);
        btn->setFixedSize(128, 84);
        btn->setCursor(Qt::PointingHandCursor);
        btn->setStyleSheet(navBtnStyle);
        m_navButtons[i] = btn;
        m_navGroup->addButton(btn, i);
        railLayout->addWidget(btn, 0, Qt::AlignHCenter);

        railLayout->addSpacing(4);

        auto* lbl = new QLabel(QString::fromUtf8(entries[i].label));
        lbl->setAlignment(Qt::AlignCenter);
        lbl->setFixedHeight(20);
        lbl->setStyleSheet(QStringLiteral(
            "font-size: 15px; color: %1; font-weight: 800;")
            .arg(Theme::rgb(c.textPrimary)));
        railLayout->addWidget(lbl, 0, Qt::AlignHCenter);

        // Add a divider after each entry except the last
        if (i < 4) {
            railLayout->addWidget(makeDivider());
        }
    }

    railLayout->addStretch();

    // ── Collapse button at the bottom of the nav rail ────────────────
    m_navCollapseBtn = new QPushButton;
    m_navCollapseBtn->setFixedSize(128, 70);
    m_navCollapseBtn->setCursor(Qt::PointingHandCursor);
    m_navCollapseBtn->setToolTip(QStringLiteral("Collapse sidebar"));
    {
        // Render a large « glyph into a pixmap, centered in the button.
        QFont chevFont;
        chevFont.setPixelSize(112);   // proportional to nav buttons
        chevFont.setWeight(QFont::Bold);
        QFontMetrics chevFm(chevFont);
        QString chevText = QStringLiteral("\u00AB");
        int chevW = chevFm.horizontalAdvance(chevText);
        int chevH = chevFm.height();
        qreal dpr = devicePixelRatioF();
        QPixmap chevPix(static_cast<int>(chevW * dpr),
                        static_cast<int>(chevH * dpr));
        chevPix.setDevicePixelRatio(dpr);
        chevPix.fill(Qt::transparent);
        {
            QPainter p(&chevPix);
            p.setFont(chevFont);
            p.setPen(c.textSecondary);
            p.drawText(0, chevFm.ascent(), chevText);
        }
        // Crop to tight bounding rect of the visible glyph so Qt
        // centres it properly (font metrics include leading/descent
        // padding that shifts the glyph upward otherwise).
        {
            QImage img = chevPix.toImage();
            int top = img.height(), bottom = 0;
            for (int y = 0; y < img.height(); ++y) {
                for (int x = 0; x < img.width(); ++x) {
                    if (qAlpha(img.pixel(x, y)) > 0) {
                        if (y < top) top = y;
                        if (y > bottom) bottom = y;
                        break;
                    }
                }
            }
            if (top <= bottom) {
                chevPix = chevPix.copy(0, top, chevPix.width(), bottom - top + 1);
            }
        }
        int finalW = static_cast<int>(chevPix.width() / dpr);
        int finalH = static_cast<int>(chevPix.height() / dpr);
        m_navCollapseBtn->setIcon(QIcon(chevPix));
        m_navCollapseBtn->setIconSize(QSize(finalW, finalH));
    }
    m_navCollapseBtn->setStyleSheet(QStringLiteral(
        "QPushButton { background: %1; border: none; border-radius: %2px;"
        "  padding: 0; min-height: 70px; max-height: 70px;"
        "  min-width: 128px; max-width: 128px; }"
        "QPushButton:hover { background: %3; }")
        .arg(Theme::rgb(c.surface2))
        .arg(m.radiusMd)
        .arg(Theme::rgb(c.surface3)));
    railLayout->addWidget(m_navCollapseBtn, 0, Qt::AlignHCenter);

    // Small square expand button — hidden initially, shown when collapsed
    m_navExpandBtn = new QPushButton(QStringLiteral("\u00BB"));  // »
    m_navExpandBtn->setFixedSize(32, 32);
    m_navExpandBtn->setCursor(Qt::PointingHandCursor);
    m_navExpandBtn->setToolTip(QStringLiteral("Expand sidebar"));
    m_navExpandBtn->setStyleSheet(QStringLiteral(
        "QPushButton { background: %1; border: none; border-radius: %2px;"
        "  color: %3; font-size: 16px; font-weight: bold; }"
        "QPushButton:hover { background: %4; color: %5; }")
        .arg(Theme::rgb(c.surface2))
        .arg(m.radiusSm)
        .arg(Theme::rgb(c.textSecondary))
        .arg(Theme::rgb(c.surface3))
        .arg(Theme::rgb(c.textPrimary)));
    m_navExpandBtn->hide();
    railLayout->addWidget(m_navExpandBtn, 0, Qt::AlignHCenter);

    connect(m_navCollapseBtn, &QPushButton::clicked, this, &MainWindow::toggleNavRail);
    connect(m_navExpandBtn, &QPushButton::clicked, this, &MainWindow::toggleNavRail);

    hbox->addWidget(m_navRail);
    hbox->addWidget(navSeparator);

    // ── Stacked widget for page content ─────────────────────────────────
    m_pageStack = new QStackedWidget(centralContainer);
    m_pageStack->setFrameShape(QFrame::NoFrame);
    m_pageStack->setLineWidth(0);
    m_pageStack->setMidLineWidth(0);
    m_pageStack->setStyleSheet(QStringLiteral(
        "QStackedWidget { background: %1; border: none; padding: 0; margin: 0; }")
        .arg(Theme::hex(c.surface1)));
    // Zero internal layout margins so child pages start at (0,0) and
    // their sub-rails align vertically with the main nav rail.
    if (auto* lay = m_pageStack->layout())
        lay->setContentsMargins(0, 0, 0, 0);
    hbox->addWidget(m_pageStack, 1);

    // Wire button group to page switching
    connect(m_navGroup, &QButtonGroup::idClicked,
            this, &MainWindow::onPageTabChanged);

    // Select the first button by default — show the Projects page on startup
    setCurrentPage(Page::Projects);

    setCentralWidget(centralContainer);
}

// ═════════════════════════════════════════════════════════════════════════════
// Page navigation
// ═════════════════════════════════════════════════════════════════════════════

void MainWindow::setCurrentPage(Page page)
{
    int idx = static_cast<int>(page);
    if (idx >= 0 && idx < 5 && m_navButtons[idx]) {
        m_navButtons[idx]->setChecked(true);
        onPageTabChanged(idx);   // QButtonGroup::idClicked only fires on user clicks
    }
}

Page MainWindow::currentPage() const noexcept
{
    int id = m_navGroup ? m_navGroup->checkedId() : 0;
    return static_cast<Page>(id);
}

void MainWindow::toggleNavRail()
{
    m_navCollapsed = !m_navCollapsed;

    const auto& c = Theme::colors();
    const auto& m = Theme::metrics();

    // Per-page accent colors for collapsed vertical-text buttons
    static const QColor kPageColors[] = {
        QColor(255, 200, 40),   // PROJECTS  — yellow
        QColor(180, 100, 255),  // CHARACTERS — purple
        QColor(80, 210, 120),   // AUDIO     — green
        QColor(70, 150, 255),   // TIMELINE  — blue
        QColor(240, 70, 70)     // EXPORT    — red
    };
    static const char* kPageLabels[] = {
        "PROJECTS", "CHARACTERS", "AUDIO", "TIMELINE", "EXPORT"
    };

    if (m_navCollapsed) {
        constexpr int kBtnW = 28;

        // Measure the actual expanded entry stride from live button
        // positions.  At this point the buttons are still at their
        // expanded 150×120 size — geometry is valid and fully laid out.
        int kTargetH = m_navButtons[1]->y() - m_navButtons[0]->y();
        if (kTargetH <= 0) {
            // Fallback formula (should never be needed at runtime)
            QFont lblFont;
            lblFont.setPixelSize(16);
            lblFont.setWeight(QFont::ExtraBold);
            int lblH = QFontMetrics(lblFont).height();
            kTargetH = 120 + 2 + lblH + 2 + m.spacingSm + 2 + 1 + 2 + m.spacingSm + 2;
        }
        spdlog::debug("NavRail collapse: measured stride = {}", kTargetH);

        auto* layout = m_navRail->layout();
        auto* vlay = static_cast<QVBoxLayout*>(layout);

        // In compact mode each button takes the full stride height.
        // All spacers, dividers, and labels are hidden — the divider
        // line is drawn as a CSS bottom-border on each button instead.
        // This guarantees pixel-perfect alignment with the sub-rail
        // regardless of spacing arithmetic.
        // In compact mode labels and dividers are hidden.  The
        // remaining gap between consecutive buttons is 2 spacer items
        // (spacingSm each) plus 3 inter-item layout spacings.
        const int kGap     = 3 * vlay->spacing() + 2 * m.spacingSm; // 3×2 + 2×6 = 18
        const int kCompactH = kTargetH - kGap + 2;  // +2 fine-tune

        for (int i = 0; i < 5; ++i) {
            QColor col = kPageColors[i];

            // Render vertical text (rotated 90° CW) into a QPixmap.
            QFont font;
            font.setPixelSize(11);
            font.setWeight(QFont::ExtraBold);
            font.setLetterSpacing(QFont::AbsoluteSpacing, 1.5);
            QFontMetrics fm(font);

            QString label = QString::fromUtf8(kPageLabels[i]);
            int textW = fm.horizontalAdvance(label);
            int textH = fm.height();

            // Use the compact button height for the rotated text pixmap.
            int pixW = kCompactH;
            int pad  = (pixW - textW) / 2;

            qreal dpr = devicePixelRatioF();
            QPixmap hPix(static_cast<int>(pixW * dpr),
                         static_cast<int>(textH * dpr));
            hPix.setDevicePixelRatio(dpr);
            hPix.fill(Qt::transparent);
            {
                QPainter p(&hPix);
                p.setFont(font);
                p.setPen(col);
                p.drawText(pad, fm.ascent(), label);
            }

            // Rotate 90° clockwise → vertical pixmap
            QTransform rot;
            rot.rotate(90);
            QPixmap vPix = hPix.transformed(rot, Qt::SmoothTransformation);
            vPix.setDevicePixelRatio(dpr);

            m_navButtons[i]->setText(QString());
            m_navButtons[i]->setIcon(QIcon(vPix));
            m_navButtons[i]->setIconSize(QSize(kBtnW, kCompactH));
            m_navButtons[i]->setFixedSize(kBtnW, kCompactH);

            QString css = QStringLiteral(
                "QPushButton { background: rgba(%1,%2,%3,30); border: none;"
                "  border-radius: %4px; padding: 0; }"
                "QPushButton:hover { background: rgba(%1,%2,%3,55); }"
                "QPushButton:pressed { background: rgba(%1,%2,%3,90); }"
                "QPushButton:checked { background: rgba(%1,%2,%3,80); }")
                .arg(col.red()).arg(col.green()).arg(col.blue())
                .arg(m.radiusSm);

            m_navButtons[i]->setStyleSheet(css);
            m_navButtons[i]->show();
        }

        // Hide everything except nav buttons and expand button.
        for (int i = 0; i < layout->count(); ++i) {
            auto* w = layout->itemAt(i)->widget();
            if (!w) continue;
            bool isNavBtn = false;
            for (int j = 0; j < 5; ++j)
                if (w == m_navButtons[j]) { isNavBtn = true; break; }
            if (isNavBtn || w == m_navExpandBtn) continue;
            w->hide();
        }

        // Configure expand button: vertical "EXPAND" text in red
        {
            QColor redCol(240, 70, 70);
            QFont efont;
            efont.setPixelSize(11);
            efont.setWeight(QFont::ExtraBold);
            efont.setLetterSpacing(QFont::AbsoluteSpacing, 1.5);
            QFontMetrics efm(efont);

            QString elabel = QStringLiteral("EXPAND");
            int etextW = efm.horizontalAdvance(elabel);
            int etextH = efm.height();
            int epixW  = kCompactH;
            int epad   = (epixW - etextW) / 2;

            qreal edpr = devicePixelRatioF();
            QPixmap ehPix(static_cast<int>(epixW * edpr),
                          static_cast<int>(etextH * edpr));
            ehPix.setDevicePixelRatio(edpr);
            ehPix.fill(Qt::transparent);
            {
                QPainter p(&ehPix);
                p.setFont(efont);
                p.setPen(redCol);
                p.drawText(epad, efm.ascent(), elabel);
            }

            QTransform erot;
            erot.rotate(90);
            QPixmap evPix = ehPix.transformed(erot, Qt::SmoothTransformation);
            evPix.setDevicePixelRatio(edpr);

            m_navExpandBtn->setText(QString());
            m_navExpandBtn->setIcon(QIcon(evPix));
            m_navExpandBtn->setIconSize(QSize(kBtnW, kCompactH));
            m_navExpandBtn->setFixedSize(kBtnW, kCompactH);

            QString ecss = QStringLiteral(
                "QPushButton { background: rgba(240,70,70,30); border: none;"
                "  border-radius: %1px; padding: 0; }"
                "QPushButton:hover { background: rgba(240,70,70,55); }"
                "QPushButton:pressed { background: rgba(240,70,70,90); }")
                .arg(m.radiusSm);
            m_navExpandBtn->setStyleSheet(ecss);
        }

        m_navExpandBtn->show();
        m_navRail->setFixedWidth(44);
    } else {
        // (Spacers and layout spacing were never modified in compact mode,
        //  so nothing to restore here — just proceed with button geometry.)

        auto* layout = m_navRail->layout();

        // Restore original emoji + full-size nav buttons
        static const char* kPageIcons[] = {
            "\U0001F4C1", "\U0001F464", "\U0001F3B5",
            "\U0001F39E", "\U0001F4E4"
        };

        QString navBtnStyle = QStringLiteral(
            "QPushButton { background: transparent; border: none;"
            "  border-radius: %1px; color: %2; font-size: 42px;"
            "  padding: 12px 0; }"
            "QPushButton:hover { background: %3; color: %4; }"
            "QPushButton:pressed { background: %5; color: white; }"
            "QPushButton:checked { background: %6; color: %7; }")
            .arg(m.radiusXl)
            .arg(Theme::rgb(c.textTertiary))
            .arg(Theme::rgb(c.surface3))
            .arg(Theme::rgb(c.textPrimary))
            .arg(Theme::rgb(c.accentDim))
            .arg(Theme::rgb(c.accentDim))
            .arg(Theme::rgb(c.accent));

        for (int i = 0; i < 5; ++i) {
            m_navButtons[i]->setIcon(QIcon());   // clear icon
            m_navButtons[i]->setText(QString::fromUtf8(kPageIcons[i]));
            m_navButtons[i]->setFixedSize(128, 84);
            m_navButtons[i]->setStyleSheet(navBtnStyle);
        }

        // Restore collapse button to expanded size
        m_navCollapseBtn->setFixedSize(128, 70);

        // Show everything, hide expand button
        for (int i = 0; i < layout->count(); ++i) {
            auto* w = layout->itemAt(i)->widget();
            if (!w) continue;
            if (w == m_navExpandBtn) continue;
            w->show();
        }
        m_navExpandBtn->hide();
        m_navRail->setFixedWidth(150);
    }
}

void MainWindow::onPageTabChanged(int index)
{
    m_pageStack->setCurrentIndex(index);
    emit pageChanged(index);

    // When switching TO the Timeline page, force the Program Monitor to
    // re-composite so it doesn't show a stale frame from Shots/Export.
    // The VulkanViewport native HWND can retain stale content while hidden,
    // and the ExportPanel shares the same compositeFrame() callback which
    // may have overwritten the GPU compositor output.
    // Use requestRefresh() (not refresh()) so m_editSettleCounter is set,
    // giving the compositor ~240ms of retry window for late decodes after
    // a timeline-edit-heavy operation like audio export.
    if (index == static_cast<int>(Page::Timeline)) {
        if (auto* pm = programMonitor())
            pm->requestRefresh();
        // The segment cache may have changed while we were away (e.g. an export
        // populated it via write-through), so re-probe and repaint the render
        // bar — otherwise it keeps showing its stale pre-export state.
        if (m_timelineWorkspace)
            m_timelineWorkspace->refreshRenderBar();
    }

    // When switching TO the Projects page, refresh the known-shows list so the
    // right-click "Assign to Show" submenu reflects shows added in COMPOSE.
    // Use ShotComposer::knownShows() (merges the _shows.json registry AND every
    // show used as a tag on shots) — not ShotPresetManager::knownShows(), which
    // is registry-only and misses shows that only exist as shot tags.
    if (index == static_cast<int>(Page::Projects) && m_projectPanel &&
        m_characterShotPanel && m_characterShotPanel->shotComposer()) {
        m_projectPanel->setKnownShows(
            m_characterShotPanel->shotComposer()->knownShows());
    }

    // Log page switch
    static const char* pageNames[] = {"PROJECTS", "CHARACTERS", "AUDIO", "TIMELINE", "EXPORT"};
    if (index >= 0 && index < 5)
        spdlog::debug("Switched to {} page", pageNames[index]);
}

// ═════════════════════════════════════════════════════════════════════════════

} // namespace rt
