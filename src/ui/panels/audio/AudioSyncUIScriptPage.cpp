/*
 * AudioSyncUIScriptPage.cpp - Script side-panel page for AudioSync.
 * Split from AudioSyncUI.cpp for maintainability.
 */
#include "panels/audio/AudioSync.h"
#include "Theme.h"
#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QEvent>
#include <QFrame>
#include <QGraphicsDropShadowEffect>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSplitter>
#include <QStackedWidget>
#include <QStyledItemDelegate>
#include <QVBoxLayout>
#include <spdlog/spdlog.h>
#include <algorithm>

namespace rt {

// ── Custom delegate: shows display name + URL subtitle ────────────────────
class ScriptHistoryDelegate : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override
    {
        // Draw selection background
        QStyleOptionViewItem opt = option;
        initStyleOption(&opt, index);

        painter->save();

        // Background
        if (opt.state & QStyle::State_Selected) {
            painter->fillRect(opt.rect, opt.palette.highlight());
        } else if (opt.state & QStyle::State_MouseOver) {
            QColor hover = Theme::colors().textBright;
            hover.setAlpha(15);
            painter->fillRect(opt.rect, hover);
        }

        QRect r = opt.rect.adjusted(12, 6, -12, -6);

        // Display name (top line)
        QString displayName = index.data(Qt::DisplayRole).toString();
        QFont nameFont = opt.font;
        nameFont.setPointSize(11);
        nameFont.setBold(false);
        painter->setFont(nameFont);
        painter->setPen(opt.palette.color(QPalette::Text));
        QRect nameRect(r.x(), r.y(), r.width(), 22);
        QString elidedName = painter->fontMetrics().elidedText(
            displayName, Qt::ElideRight, nameRect.width());
        painter->drawText(nameRect, Qt::AlignLeft | Qt::AlignVCenter, elidedName);

        // URL subtitle (bottom line)
        QString url = index.data(Qt::UserRole).toString();
        if (!url.isEmpty()) {
            QFont urlFont = opt.font;
            urlFont.setPointSize(8);
            painter->setFont(urlFont);
            painter->setPen(Theme::colors().textSecondary);
            QRect urlRect(r.x(), r.y() + 20, r.width(), 16);
            QString elidedUrl = painter->fontMetrics().elidedText(
                url, Qt::ElideRight, urlRect.width());
            painter->drawText(urlRect, Qt::AlignLeft | Qt::AlignVCenter, elidedUrl);
        }

        painter->restore();
    }

    QSize sizeHint(const QStyleOptionViewItem& option,
                   const QModelIndex& /*index*/) const override
    {
        return QSize(option.rect.width(), 44);
    }
};

// ── Custom delegate: rich script session list items ──────────────────────
// Each row shows: [status dot] [name + meta] [actions on hover]
class ScriptSessionDelegate : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override
    {
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);

        QStyleOptionViewItem opt = option;
        initStyleOption(&opt, index);

        QRect r = opt.rect;
        bool isActive = index.data(Qt::UserRole + 2).toBool();
        bool isHovered = opt.state & QStyle::State_MouseOver;

        // Background
        if (isActive) {
            painter->fillRect(r, QColor(42, 42, 80));
            // Left accent border for active item
            painter->fillRect(QRect(r.x(), r.y(), 3, r.height()), QColor(90, 122, 255));
        } else if (opt.state & QStyle::State_Selected) {
            painter->fillRect(r, opt.palette.highlight());
        } else if (isHovered) {
            painter->fillRect(r, QColor(34, 34, 64));
        }

        // Status dot color based on workflow state
        int state = index.data(Qt::UserRole + 3).toInt();
        QColor dotColor;
        switch (state) {
            case 3: dotColor = QColor(255, 152, 0);   break; // synced = orange
            case 2: dotColor = QColor(76, 175, 80);    break; // transcribed = green
            case 1: dotColor = QColor(74, 106, 255);   break; // loaded = blue
            default: dotColor = Theme::colors().textDisabled; break; // empty = gray
        }

        // Draw status dot
        int dotX = r.x() + 14;
        int dotY = r.y() + r.height() / 2 - 4;
        painter->setBrush(dotColor);
        painter->setPen(Qt::NoPen);
        painter->drawEllipse(QPoint(dotX, dotY), 4, 4);

        // Text area (after dot)
        QRect textR = r.adjusted(28, 8, -8, -8);

        // Display name
        QString displayName = index.data(Qt::DisplayRole).toString();
        QFont nameFont = opt.font;
        nameFont.setPointSize(11);
        nameFont.setBold(isActive);
        painter->setFont(nameFont);
        painter->setPen(isActive ? QColor(138, 164, 255) : opt.palette.color(QPalette::Text));
        QRect nameRect(textR.x(), textR.y(), textR.width(), 20);
        QString elidedName = painter->fontMetrics().elidedText(
            displayName, Qt::ElideRight, nameRect.width());
        painter->drawText(nameRect, Qt::AlignLeft | Qt::AlignVCenter, elidedName);

        // Meta info (lines + clips)
        QString meta = index.data(Qt::UserRole + 1).toString();
        if (!meta.isEmpty()) {
            QFont metaFont = opt.font;
            metaFont.setPointSize(9);
            painter->setFont(metaFont);
            painter->setPen(QColor(136, 136, 136));
            QRect metaRect(textR.x(), textR.y() + 18, textR.width(), 16);
            painter->drawText(metaRect, Qt::AlignLeft | Qt::AlignVCenter, meta);
        }

        painter->restore();
    }

    QSize sizeHint(const QStyleOptionViewItem& option,
                   const QModelIndex& /*index*/) const override
    {
        return QSize(option.rect.width(), 48);
    }
};

void AudioSync::setupScriptPage()
{
    const auto& c = Theme::colors();
    const auto& m = Theme::metrics();
    const auto& t = Theme::typography();
    const auto surf0 = Theme::hex(c.surface0);
    const auto surf1 = Theme::hex(c.surface1);
    const auto surf2 = Theme::hex(c.surface2);
    const auto surf3 = Theme::hex(c.surface3);
    const auto brd   = Theme::hex(c.border);
    const auto brdL  = Theme::hex(c.borderLight);
    const auto txt1  = Theme::hex(c.textPrimary);
    const auto txt2  = Theme::hex(c.textSecondary);
    const auto txt3  = Theme::hex(c.textTertiary);
    const auto txtD  = Theme::hex(c.textDisabled);
    const auto acc   = Theme::hex(c.accent);
    const auto accH  = Theme::hex(c.accentHover);
    const auto accS  = Theme::hex(c.accentSubtle);
    const auto accDm = Theme::hex(c.accentDim);
    const auto inp   = Theme::hex(c.inputBg);
    const auto inpB  = Theme::hex(c.inputBorder);
    const auto sucBg = Theme::hex(c.successBtnBg);
    const auto sucBH = Theme::hex(c.successBtnHover);
    const auto sucTx = Theme::hex(c.success);
    const auto danBg = Theme::hex(c.dangerBg);
    const auto danBH = Theme::hex(c.dangerBgHover);
    const auto danTx = Theme::hex(c.dangerText);
    const auto errC  = Theme::hex(c.error);
    const auto errBg = Theme::hex(c.errorBg);
    const auto warnC = Theme::hex(c.warning);
    const auto ctrlBd = Theme::hex(c.controlBorder);
    const auto scrTr = Theme::hex(c.scrollbarTrack);
    const auto scrTh = Theme::hex(c.scrollbarThumb);
    const auto rad   = QString::number(m.radiusSm);
    const auto radM  = QString::number(m.radiusMd);

    m_scriptPage = new QWidget;
    auto* scriptPageLayout = new QVBoxLayout(m_scriptPage);
    scriptPageLayout->setContentsMargins(m.spacingXxl, m.spacingXxl,
                                          m.spacingXxl, m.spacingXxl);
    scriptPageLayout->setSpacing(m.spacingMd);

    // ── Title row ────────────────────────────────────────────────────────
    auto* scriptTitleRow = new QHBoxLayout;
    auto* scriptTitle = new QLabel("Script Manager");
    scriptTitle->setStyleSheet(QStringLiteral(
        "font-size: 22px; font-weight: %1; color: %2;")
        .arg(t.weightBold)
        .arg(Theme::rgb(c.textPrimary)));
    scriptTitleRow->addWidget(scriptTitle, 1);

    auto* scriptCloseBtn = new QPushButton("\u2715");
    scriptCloseBtn->setFixedSize(36, 36);
    scriptCloseBtn->setCursor(Qt::PointingHandCursor);
    scriptCloseBtn->setStyleSheet(QStringLiteral(
        "QPushButton { background: transparent; border: none;"
        "  border-radius: %1px; font-size: 18px; color: %2; }"
        "QPushButton:hover { background: %3; color: %4; }")
        .arg(m.radiusMd)
        .arg(Theme::rgb(c.textTertiary))
        .arg(Theme::rgb(c.surface3))
        .arg(Theme::rgb(c.textPrimary)));
    connect(scriptCloseBtn, &QPushButton::clicked,
            this, &AudioSync::hideAudioSidePanel);
    scriptTitleRow->addWidget(scriptCloseBtn);
    scriptPageLayout->addLayout(scriptTitleRow);

    // ── Description card ─────────────────────────────────────────────────
    auto* descCard = new QFrame;
    descCard->setStyleSheet(QStringLiteral(
        "QFrame { background: %1; border: 1px solid %2;"
        "  border-radius: %3px; padding: %4px; }")
        .arg(Theme::rgb(c.surface1))
        .arg(Theme::rgb(c.borderLight))
        .arg(m.radiusMd)
        .arg(m.spacingMd));
    auto* descLayout = new QHBoxLayout(descCard);
    descLayout->setContentsMargins(m.spacingMd, m.spacingSm,
                                    m.spacingMd, m.spacingSm);
    auto* descIcon = new QLabel("\U0001F4DC");
    descIcon->setStyleSheet(QStringLiteral("font-size: 24px;"));
    descLayout->addWidget(descIcon);
    auto* descText = new QLabel(
        "Click <b>Load Script</b> to open a dialog where you paste a Google Docs URL\n"
        "or file path, and name the script session.\n"
        "Right-click any saved script below to rename, delete, or sync with GDrive.");
    descText->setWordWrap(true);
    descText->setStyleSheet(QStringLiteral(
        "font-size: %1px; color: %2; background: transparent; border: none;")
        .arg(t.sizeXs)
        .arg(Theme::rgb(c.textSecondary)));
    descLayout->addWidget(descText, 1);
    scriptPageLayout->addWidget(descCard);

    scriptPageLayout->addSpacing(4);

    // ── Button row ───────────────────────────────────────────────────────
    auto* btnRow = new QHBoxLayout;
    btnRow->setSpacing(m.spacingSm);

    m_loadScriptBtn = new QPushButton("\u25B6  Load Script");
    m_loadScriptBtn->setMinimumHeight(48);
    m_loadScriptBtn->setCursor(Qt::PointingHandCursor);
    m_loadScriptBtn->setStyleSheet(QStringLiteral(
        "QPushButton {"
        "  background: %1; color: %2; border: none;"
        "  border-radius: %3px; font-size: %4px;"
        "  font-weight: 700; padding: 12px 24px; }"
        "QPushButton:hover { background: %5; }"
        "QPushButton:pressed { background: %6; }")
        .arg(Theme::rgb(c.primaryBtnBg))
        .arg(Theme::hex(c.textBright))
        .arg(m.radiusMd)
        .arg(t.sizeCaption)
        .arg(Theme::rgb(c.primaryBtnHover))
        .arg(Theme::rgb(c.accent)));
    connect(m_loadScriptBtn, &QPushButton::clicked, this, &AudioSync::onLoadScriptClicked);
    btnRow->addWidget(m_loadScriptBtn, 1);

    scriptPageLayout->addLayout(btnRow);

    // ── Script status ────────────────────────────────────────────────────
    m_scriptStatus = new QLabel("No script loaded");
    m_scriptStatus->setStyleSheet(QStringLiteral(
        "font-size: 13px; color: %1; padding: 4px 0;")
        .arg(Theme::rgb(c.textTertiary)));
    scriptPageLayout->addWidget(m_scriptStatus);

    // ── Script format instructions card ──────────────────────────────────
    auto* formatCard = new QFrame;
    formatCard->setStyleSheet(QStringLiteral(
        "QFrame { background: %1; border: 1px solid %2;"
        "  border-radius: %3px; padding: %4px; }")
        .arg(Theme::rgb(c.surface1))
        .arg(Theme::rgb(c.borderLight))
        .arg(m.radiusMd)
        .arg(m.spacingMd));
    auto* formatLayout = new QVBoxLayout(formatCard);
    formatLayout->setContentsMargins(m.spacingMd, m.spacingSm,
                                     m.spacingMd, m.spacingSm);
    formatLayout->setSpacing(m.spacingXs);

    auto* formatTitle = new QLabel("\U0001F4CB  Script Format");
    formatTitle->setStyleSheet(QStringLiteral(
        "font-size: 13px; font-weight: 700; color: %1;"
        " background: transparent; border: none;")
        .arg(Theme::rgb(c.textPrimary)));
    formatLayout->addWidget(formatTitle);

    // ── Separator ────────────────────────────────────────────────────────
    auto* secSep = new QFrame;
    secSep->setFixedHeight(1);
    secSep->setStyleSheet(QStringLiteral(
        "background: %1; margin: %2px 0;")
        .arg(Theme::rgb(c.borderLight))
        .arg(m.spacingSm));
    scriptPageLayout->addWidget(secSep);

    // ── Script sessions section ─────────────────────────────────────────
    auto* sessionsLabel = new QLabel("SCRIPT SESSIONS");
    sessionsLabel->setStyleSheet(QStringLiteral(
        "font-size: %1px; font-weight: 700; color: %2;"
        " letter-spacing: 1.8px;")
        .arg(t.sizeXxs)
        .arg(Theme::rgb(c.textTertiary)));
    scriptPageLayout->addWidget(sessionsLabel);

    auto* sessionsHint = new QLabel(
        "Each script keeps its own audio files, clips, and matches.\n"
        "Click to switch between scripts. Right-click to rename, delete, or sync with GDrive.");
    sessionsHint->setWordWrap(true);
    sessionsHint->setStyleSheet(QStringLiteral(
        "font-size: %1px; color: %2; padding: 0 0 %3px 0;")
        .arg(t.sizeXxs)
        .arg(Theme::rgb(c.textDisabled))
        .arg(m.spacingSm));
    scriptPageLayout->addWidget(sessionsHint);

    // Session list widget
    m_scriptSessionList = new QListWidget;
    m_scriptSessionList->setMinimumHeight(100);
    m_scriptSessionList->setMaximumHeight(250);
    m_scriptSessionList->setContextMenuPolicy(Qt::DefaultContextMenu);
    m_scriptSessionList->setStyleSheet(QStringLiteral(
        "QListWidget {"
        "  background: %1; color: %2; border: 1px solid %3;"
        "  border-radius: %4px; padding: 4px;"
        "}"
        "QListWidget::item {"
        "  padding: 10px 14px; border-radius: %5px;"
        "}"
        "QListWidget::item:selected {"
        "  background: %6; color: %7;"
        "}"
        "QListWidget::item:hover {"
        "  background: %8;"
        "}")
        .arg(Theme::rgb(c.surface1))
        .arg(Theme::rgb(c.textPrimary))
        .arg(Theme::rgb(c.border))
        .arg(m.radiusMd)
        .arg(m.radiusSm)
        .arg(Theme::rgb(c.accentDim))
        .arg(Theme::rgb(c.textPrimary))
        .arg(Theme::rgb(c.surface3)));

    // Install custom delegate for rich display (status dot, name, meta)
    m_scriptSessionList->setItemDelegate(new ScriptSessionDelegate(m_scriptSessionList));

    connect(m_scriptSessionList, &QListWidget::itemClicked,
            this, [this](QListWidgetItem* item) {
        QString key = item->data(Qt::UserRole).toString();
        if (!key.isEmpty())
            switchToScript(key.toStdString());
    });

    m_scriptSessionList->installEventFilter(this);
    scriptPageLayout->addWidget(m_scriptSessionList, 1);

    // ── Collapsible script format section (at the very bottom) ──────────
    auto* fmtSep = new QFrame;
    fmtSep->setFixedHeight(1);
    fmtSep->setStyleSheet(QStringLiteral(
        "background: %1; margin: %2px 0;")
        .arg(Theme::rgb(c.borderLight))
        .arg(m.spacingSm));
    scriptPageLayout->addWidget(fmtSep);

    m_scriptFormatToggle = new QPushButton("\u25B6  SCRIPT FORMAT");
    m_scriptFormatToggle->setFlat(true);
    m_scriptFormatToggle->setCursor(Qt::PointingHandCursor);
    m_scriptFormatToggle->setStyleSheet(QStringLiteral(
        "QPushButton { background: transparent; border: none;"
        "  color: %1; font-size: %2px; font-weight: 700;"
        "  letter-spacing: 1.8px; text-align: left; padding: 8px 0; }"
        "QPushButton:hover { color: %3; }")
        .arg(Theme::rgb(c.textTertiary))
        .arg(t.sizeXxs)
        .arg(Theme::rgb(c.textSecondary)));
    connect(m_scriptFormatToggle, &QPushButton::clicked,
            this, [this]() {
        bool vis = m_scriptFormatBody->isVisible();
        m_scriptFormatBody->setVisible(!vis);
        QString arrow = vis ? "\u25B6" : "\u25BC";
        m_scriptFormatToggle->setText(arrow + "  SCRIPT FORMAT");
    });
    scriptPageLayout->addWidget(m_scriptFormatToggle);

    m_scriptFormatBody = new QFrame;
    m_scriptFormatBody->setVisible(false);
    m_scriptFormatBody->setStyleSheet(QStringLiteral(
        "QFrame { background: %1; border: 1px solid %2;"
        "  border-radius: %3px; padding: %4px; margin-top: 4px; }")
        .arg(Theme::rgb(c.surface1))
        .arg(Theme::rgb(c.borderLight))
        .arg(m.radiusMd)
        .arg(m.spacingSm));

    auto* fmtBodyLayout = new QVBoxLayout(m_scriptFormatBody);
    fmtBodyLayout->setContentsMargins(m.spacingMd, m.spacingSm,
                                       m.spacingMd, m.spacingSm);

    auto* fmtBodyLabel = new QLabel(
        "CHARACTER NAME: Dialogue text\n\n"
        "Examples:\n"
        "  WELLS: Hello everyone, welcome to the show.\n"
        "  ALICE: I can't believe we finally made it!\n\n"
        "The character name must be written in ALL CAPS.\n\n"
        "Lines that don't match this pattern are ignored.");
    fmtBodyLabel->setWordWrap(true);
    fmtBodyLabel->setStyleSheet(QStringLiteral(
        "font-size: %1px; color: %2; background: transparent;"
        " border: none; line-height: 1.6;")
        .arg(t.sizeXs)
        .arg(Theme::rgb(c.textSecondary)));
    fmtBodyLayout->addWidget(fmtBodyLabel);

    scriptPageLayout->addWidget(m_scriptFormatBody);

    scriptPageLayout->addStretch();
    m_audioSidePanelStack->addWidget(m_scriptPage);   // index 0

    // Populate session list from any existing sessions
    populateScriptSessionList();

    // --- IMPORT page (index 1) -------------------------------------------
}

} // namespace rt
