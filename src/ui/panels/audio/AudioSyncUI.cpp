/*
 * AudioSyncUI.cpp - UI construction (setupUi) for AudioSync panel.
 * Split from AudioSync.cpp for maintainability.
 */

#include "panels/audio/AudioSync.h"

#include "command/CommandStack.h"
#include "command/LambdaCommand.h"
#include "Theme.h"

#include <QComboBox>
#include <QFrame>
#include <QGraphicsDropShadowEffect>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QPainter>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSplitter>
#include <QStyledItemDelegate>
#include <QTimer>
#include <QVBoxLayout>

#include <spdlog/spdlog.h>

#include <algorithm>

// ── Delegate to paint UNMATCHED items with red background ───────────────────
class UnmatchedDelegate : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override
    {
        if (index.data(Qt::UserRole).toString() == "unmatched") {
            painter->save();
            QColor bg = rt::Theme::colors().error;
            if (option.state & QStyle::State_Selected)
                bg = bg.lighter(120);
            else if (option.state & QStyle::State_MouseOver)
                bg = bg.lighter(110);
            painter->fillRect(option.rect, bg);
            QFont f = option.font;
            f.setWeight(QFont::Bold);
            painter->setFont(f);
            painter->setPen(rt::Theme::colors().textBright);
            QRect textRect = option.rect.adjusted(14, 0, -14, 0);
            painter->drawText(textRect, Qt::AlignVCenter | Qt::AlignLeft,
                              index.data(Qt::DisplayRole).toString());
            painter->restore();
        } else {
            QStyledItemDelegate::paint(painter, option, index);
        }
    }
};

namespace rt {

void AudioSync::setupUi()
{
    const auto& c = Theme::colors();
    const auto& m = Theme::metrics();
    const auto& t = Theme::typography();

    auto* rootLayout = new QHBoxLayout(this);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(0);

    // Theme colour shortcuts
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

    // â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
    //  ICON RAIL  (left sidebar, matching ProjectPanel style)
    // â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
    m_audioIconRail = new QWidget;
    m_audioIconRail->setObjectName("AudioIconRail");
    m_audioIconRail->setFixedWidth(150);
    m_audioIconRail->setStyleSheet(QStringLiteral(
        "#AudioIconRail { background: %1;"
        "  border-right: 1px solid %2; }")
        .arg(Theme::rgb(c.surface1))
        .arg(Theme::rgb(c.border)));

    auto* railLayout = new QVBoxLayout(m_audioIconRail);
    railLayout->setContentsMargins(8, m.spacingXl, 8, m.spacingXl);
    railLayout->setSpacing(0);

    // Rail button style (matches main nav rail exactly)
    QString railBtnStyle = QStringLiteral(
        "QPushButton { background: transparent; border: none;"
        "  border-radius: %1px; color: %2; font-size: 42px;"
        "  padding: 12px 0; }"
        "QPushButton:hover { background: %3; color: %4; }"
        "QPushButton:pressed { background: %5; color: %6; }"
        "QPushButton:checked { background: %7; color: %8; }")
        .arg(m.radiusXl)
        .arg(Theme::rgb(c.textTertiary))
        .arg(Theme::rgb(c.surface3))
        .arg(Theme::rgb(c.textPrimary))
        .arg(Theme::rgb(c.accentDim))
        .arg(Theme::hex(c.textBright))
        .arg(Theme::rgb(c.accentDim))
        .arg(Theme::rgb(c.accent));

    auto makeRailBtn = [&](const QString& icon, const QString& label,
                           const QString& tip, bool checkable = false) -> QPushButton*
    {
        auto* btn = new QPushButton(icon);
        btn->setToolTip(tip);
        btn->setFixedSize(128, 84);
        btn->setCursor(Qt::PointingHandCursor);
        btn->setCheckable(checkable);
        btn->setStyleSheet(railBtnStyle);
        railLayout->addWidget(btn, 0, Qt::AlignHCenter);

        railLayout->addSpacing(4);

        auto* lbl = new QLabel(label);
        lbl->setAlignment(Qt::AlignCenter);
        lbl->setFixedHeight(20);
        lbl->setStyleSheet(QStringLiteral(
            "font-size: %1px; color: %2; font-weight: 800;")
            .arg(t.sizeCaption)
            .arg(Theme::rgb(c.textPrimary)));
        railLayout->addWidget(lbl, 0, Qt::AlignHCenter);

        return btn;
    };

    // Helper: add a divider between rail entries — single container
    // widget wrapping a 1px line to prevent DPI drift.
    auto addRailDivider = [&]() {
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
        railLayout->addWidget(div);
    };

    m_scriptRailBtn = makeRailBtn("\U0001F4DC", "SCRIPT", "Load / manage script", true);
    connect(m_scriptRailBtn, &QPushButton::clicked,
            this, [this]() { toggleAudioSidePanel(0); });

    addRailDivider();

    m_importRailBtn = makeRailBtn("\U0001F3B5", "IMPORT", "Import audio files", true);
    connect(m_importRailBtn, &QPushButton::clicked,
            this, [this]() { toggleAudioSidePanel(1); });

    addRailDivider();

    m_transcribeRailBtn = makeRailBtn("\u26A1", "TRANSCRIBE", "Transcribe audio", true);
    connect(m_transcribeRailBtn, &QPushButton::clicked,
            this, [this]() { toggleAudioSidePanel(2); });

    addRailDivider();

    m_matchRailBtn = makeRailBtn("\U0001F517", "MATCH", "Character filter & matching", true);
    connect(m_matchRailBtn, &QPushButton::clicked,
            this, [this]() { toggleAudioSidePanel(3); });

    addRailDivider();

    m_audioSettingsRailBtn = makeRailBtn("\u2699", "SETTINGS", "Audio sync settings", true);
    connect(m_audioSettingsRailBtn, &QPushButton::clicked,
            this, [this]() { toggleAudioSidePanel(4); });

    railLayout->addStretch();
    rootLayout->addWidget(m_audioIconRail);

    // â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
    //  SIDE PANEL  (inline expanding column, like ProjectPanel)
    // â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
    m_audioSidePanel = new QWidget;
    m_audioSidePanel->setObjectName("AudioSidePanel");
    m_audioSidePanel->setMinimumWidth(0);
    m_audioSidePanel->setMaximumWidth(0);
    m_audioSidePanel->setVisible(false);
    m_audioSidePanel->setStyleSheet(QStringLiteral(
        "#AudioSidePanel {"
        "  background: %1;"
        "  border-right: 1px solid %2;"
        "}")
        .arg(Theme::rgb(c.surface1))
        .arg(Theme::rgb(c.borderLight)));

    auto* sidePanelLayout = new QVBoxLayout(m_audioSidePanel);
    sidePanelLayout->setContentsMargins(0, 0, 0, 0);
    sidePanelLayout->setSpacing(0);

    m_audioSidePanelStack = new QStackedWidget;
    sidePanelLayout->addWidget(m_audioSidePanelStack);

    // --- SCRIPT page (index 0) - delegated to setupScriptPage() ----------
    setupScriptPage();

    // --- IMPORT page (index 1) -------------------------------------------
    m_importPage = new QWidget;
    auto* importPageLayout = new QVBoxLayout(m_importPage);
    importPageLayout->setContentsMargins(m.spacingXxl, m.spacingXxl,
                                          m.spacingXxl, m.spacingXxl);
    importPageLayout->setSpacing(m.spacingXl);

    // Title row
    auto* importTitleRow = new QHBoxLayout;
    auto* importTitle = new QLabel("Import Audio");
    importTitle->setStyleSheet(QStringLiteral(
        "font-size: 20px; font-weight: %1; color: %2;")
        .arg(t.weightBold)
        .arg(Theme::rgb(c.textPrimary)));
    importTitleRow->addWidget(importTitle, 1);

    auto* importCloseBtn = new QPushButton("\u2715");
    importCloseBtn->setFixedSize(36, 36);
    importCloseBtn->setCursor(Qt::PointingHandCursor);
    importCloseBtn->setStyleSheet(QStringLiteral(
        "QPushButton { background: transparent; border: none;"
        "  border-radius: %1px; font-size: 18px; color: %2; }"
        "QPushButton:hover { background: %3; color: %4; }")
        .arg(m.radiusMd)
        .arg(Theme::rgb(c.textTertiary))
        .arg(Theme::rgb(c.surface3))
        .arg(Theme::rgb(c.textPrimary)));
    connect(importCloseBtn, &QPushButton::clicked,
            this, &AudioSync::hideAudioSidePanel);
    importTitleRow->addWidget(importCloseBtn);
    importPageLayout->addLayout(importTitleRow);

    // Import button
    m_importAudioBtn = new QPushButton("\U0001F4C1  Import Audio Files...");
    m_importAudioBtn->setMinimumHeight(48);
    m_importAudioBtn->setCursor(Qt::PointingHandCursor);
    m_importAudioBtn->setStyleSheet(QStringLiteral(
        "QPushButton {"
        "  background: %1; color: %2; border: none;"
        "  border-radius: %3px; font-size: %4px;"
        "  font-weight: 700; padding: 12px 24px; }"
        "QPushButton:hover { background: %5; }"
        "QPushButton:pressed { background: %6; }")
        .arg(Theme::rgb(c.primaryBtnBg))
        .arg(Theme::hex(c.textBright))
        .arg(m.radiusMd)
        .arg(t.sizeBody)
        .arg(Theme::rgb(c.primaryBtnHover))
        .arg(Theme::rgb(c.accent)));
    connect(m_importAudioBtn, &QPushButton::clicked, this, &AudioSync::onImportAudioClicked);
    importPageLayout->addWidget(m_importAudioBtn);

    // Audio status
    m_audioStatus = new QLabel("No files imported");
    m_audioStatus->setStyleSheet(QStringLiteral(
        "font-size: %1px; color: %2;")
        .arg(t.sizeSmall)
        .arg(Theme::rgb(c.textTertiary)));
    importPageLayout->addWidget(m_audioStatus);

    // Audio file list header with sort
    auto* filesHeaderRow = new QHBoxLayout;
    auto* filesLabel = new QLabel("IMPORTED FILES");
    filesLabel->setStyleSheet(QStringLiteral(
        "font-size: 13px; font-weight: 700; color: %1;"
        " letter-spacing: 1.5px;")
        .arg(Theme::rgb(c.textTertiary)));
    filesHeaderRow->addWidget(filesLabel, 1);

    m_audioSortCombo = new QComboBox;
    m_audioSortCombo->addItems({"Name", "Date", "Size"});
    m_audioSortCombo->setFixedHeight(28);
    m_audioSortCombo->setStyleSheet(QStringLiteral(
        "QComboBox { background: %1; color: %2; border: 1px solid %3;"
        "  padding: 2px 8px; border-radius: %4px; font-size: %5px; }"
        "QComboBox::drop-down { width: 20px; border: none; }"
        "QComboBox::down-arrow { image: url(none); width: 0; height: 0;"
        "  border-left: 4px solid transparent; border-right: 4px solid transparent;"
        "  border-top: 5px solid %6; }"
        "QComboBox QAbstractItemView {"
        "  background: %7; color: %8; border: 1px solid %9;"
        "  selection-background-color: %10; outline: none; }")
        .arg(Theme::rgb(c.surface2), Theme::rgb(c.textSecondary),
             Theme::rgb(c.border), QString::number(m.radiusSm),
             QString::number(t.sizeXs),
             Theme::rgb(c.textTertiary), Theme::rgb(c.surface2),
             Theme::rgb(c.textPrimary), Theme::rgb(c.border))
        .arg(Theme::rgb(c.accentDim)));
    connect(m_audioSortCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int idx) { sortAudioFileList(idx); });
    filesHeaderRow->addWidget(m_audioSortCombo);
    importPageLayout->addLayout(filesHeaderRow);

    m_audioFileList = new QListWidget;
    m_audioFileList->setTextElideMode(Qt::ElideNone);
    m_audioFileList->setWordWrap(true);
    m_audioFileList->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_audioFileList->setStyleSheet(
        QString("QListWidget { background: %1; border: 1px solid %2; "
        "border-radius: %3px; font-size: %4px; }"
        "QListWidget::item { padding: 10px 12px; border-bottom: 1px solid %5; }"
        "QListWidget::item:last { border-bottom: none; }"
        "QListWidget::item:selected { background: %6; }")
        .arg(inp, inpB, radM, QString::number(t.sizeSmall),
             Theme::rgb(c.borderLight), accS));
    m_audioFileList->installEventFilter(this);
    m_audioFileList->setContextMenuPolicy(Qt::DefaultContextMenu);
    importPageLayout->addWidget(m_audioFileList, 1);

    // Remove button
    m_removeAudioBtn = new QPushButton("\U0001F5D1  Remove Selected");
    m_removeAudioBtn->setMinimumHeight(44);
    m_removeAudioBtn->setCursor(Qt::PointingHandCursor);
    m_removeAudioBtn->setStyleSheet(
        QString("QPushButton { background: %1; color: %2; border: 1px solid %3;"
        "  border-radius: %4px; font-size: %5px; font-weight: 600;"
        "  padding: 8px 16px; }"
        "QPushButton:hover { background: %6; }")
        .arg(Theme::rgb(c.surface2))
        .arg(Theme::rgb(c.dangerText))
        .arg(Theme::rgb(c.border))
        .arg(m.radiusMd)
        .arg(t.sizeSmall)
        .arg(Theme::rgb(c.surface3)));
    connect(m_removeAudioBtn, &QPushButton::clicked, this, [this]() {
        if (!m_audioFileList) return;
        auto selected = m_audioFileList->selectedItems();
        if (selected.isEmpty()) return;

        // Collect rows in descending order so removal doesn't shift indices
        std::vector<int> rows;
        rows.reserve(static_cast<size_t>(selected.size()));
        for (auto* item : selected)
            rows.push_back(m_audioFileList->row(item));
        std::sort(rows.begin(), rows.end(), std::greater<int>());

        QPointer<QListWidget> safeList = m_audioFileList;
        for (int row : rows) {
            if (row < 0 || row >= static_cast<int>(m_audioPaths.size())) continue;
            std::string removed = m_audioPaths[static_cast<size_t>(row)];
            m_audioPaths.erase(m_audioPaths.begin() + row);
            m_audioSamples.erase(removed);
            if (safeList) {
                QListWidgetItem* item = safeList->takeItem(row);
                if (item) {
                    spdlog::debug("AudioSync: Deleting audio file list item at row {} (remove)", row);
                    delete item;
                }
            }
            spdlog::info("AudioSync: Removed audio: {}", removed);
        }

        m_audioStatus->setText(m_audioPaths.empty() ? "No files imported"
            : QString("%1 file(s) imported").arg(m_audioPaths.size()));
        m_clips.clear();
        m_allTranscriptionResults.clear();
        m_transcriptionDone = false;
        m_syncDone = false;
        populateCards();
        if (m_audioPaths.empty()) {
            m_audioImported = false;
            m_audioPath.clear();
        }
        updateWorkflowState();
    });
    importPageLayout->addWidget(m_removeAudioBtn);

    m_audioSidePanelStack->addWidget(m_importPage);   // index 1

    // --- TRANSCRIBE page (index 2) ---------------------------------------
    m_transcribePage = new QWidget;
    auto* transcribePageLayout = new QVBoxLayout(m_transcribePage);
    transcribePageLayout->setContentsMargins(m.spacingXxl, m.spacingXxl,
                                              m.spacingXxl, m.spacingXxl);
    transcribePageLayout->setSpacing(m.spacingXl);

    // Title row
    auto* transcribeTitleRow = new QHBoxLayout;
    auto* transcribeTitle = new QLabel("Transcribe");
    transcribeTitle->setStyleSheet(QStringLiteral(
        "font-size: 20px; font-weight: %1; color: %2;")
        .arg(t.weightBold)
        .arg(Theme::rgb(c.textPrimary)));
    transcribeTitleRow->addWidget(transcribeTitle, 1);

    auto* transcribeCloseBtn = new QPushButton("\u2715");
    transcribeCloseBtn->setFixedSize(36, 36);
    transcribeCloseBtn->setCursor(Qt::PointingHandCursor);
    transcribeCloseBtn->setStyleSheet(QStringLiteral(
        "QPushButton { background: transparent; border: none;"
        "  border-radius: %1px; font-size: 18px; color: %2; }"
        "QPushButton:hover { background: %3; color: %4; }")
        .arg(m.radiusMd)
        .arg(Theme::rgb(c.textTertiary))
        .arg(Theme::rgb(c.surface3))
        .arg(Theme::rgb(c.textPrimary)));
    connect(transcribeCloseBtn, &QPushButton::clicked,
            this, &AudioSync::hideAudioSidePanel);
    transcribeTitleRow->addWidget(transcribeCloseBtn);
    transcribePageLayout->addLayout(transcribeTitleRow);

    // Description
    auto* transcribeDesc = new QLabel(
        "Run Whisper AI transcription on imported audio files.\n"
        "This generates timestamped text segments for sync.");
    transcribeDesc->setWordWrap(true);
    transcribeDesc->setStyleSheet(QStringLiteral(
        "font-size: 13px; color: %1;")
        .arg(Theme::rgb(c.textTertiary)));
    transcribePageLayout->addWidget(transcribeDesc);

    // Model selector
    auto* modelLabel = new QLabel("WHISPER MODEL");
    modelLabel->setStyleSheet(QStringLiteral(
        "font-size: 13px; font-weight: 700; color: %1;"
        " letter-spacing: 1.5px;")
        .arg(Theme::rgb(c.textTertiary)));
    transcribePageLayout->addWidget(modelLabel);

    m_modelCombo = new QComboBox;
    m_modelCombo->addItems({"tiny", "base", "small", "medium", "large-v2", "large-v3"});
    m_modelCombo->setCurrentIndex(2);  // default to "small" for better accuracy
    m_modelCombo->setMinimumHeight(44);
    m_modelCombo->setStyleSheet(QStringLiteral(
        "QComboBox {"
        "  background: %1; color: %2; border: 1px solid %3;"
        "  padding: 10px 14px; border-radius: %4px; font-size: %5px;"
        "}"
        "QComboBox:hover { border-color: %6; }"
        "QComboBox::drop-down {"
        "  subcontrol-origin: padding; subcontrol-position: center right;"
        "  width: 32px; border: none;"
        "}"
        "QComboBox::down-arrow { image: none; border: none; }"
        "QComboBox QAbstractItemView {"
        "  background: %7; color: %8; border: 1px solid %9;"
        "  border-radius: %10px; padding: 4px;"
        "  selection-background-color: %11;"
        "  outline: none;"
        "}"
        "QComboBox QAbstractItemView::item {"
        "  padding: 8px 12px; min-height: 28px;"
        "}"
        "QComboBox QAbstractItemView::item:hover { background: %12; }")
        .arg(inp, txt1, inpB, radM,
             QString::number(t.sizeSmall),
             Theme::rgb(c.accent),
             Theme::rgb(c.surface2), txt1, Theme::rgb(c.border))
        .arg(radM, Theme::rgb(c.accentDim),
             Theme::rgb(c.surface3)));
    transcribePageLayout->addWidget(m_modelCombo);

    // Transcribe button
    m_transcribeBtn = new QPushButton("\u26A1  Transcribe All");
    m_transcribeBtn->setMinimumHeight(48);
    m_transcribeBtn->setCursor(Qt::PointingHandCursor);
    m_transcribeBtn->setStyleSheet(QStringLiteral(
        "QPushButton {"
        "  background: %1; color: %2; border: none;"
        "  border-radius: %3px; font-size: %4px;"
        "  font-weight: 700; padding: 12px 24px; }"
        "QPushButton:hover { background: %5; }"
        "QPushButton:pressed { background: %6; }")
        .arg(Theme::rgb(c.primaryBtnBg))
        .arg(Theme::hex(c.textBright))
        .arg(m.radiusMd)
        .arg(t.sizeBody)
        .arg(Theme::rgb(c.primaryBtnHover))
        .arg(Theme::rgb(c.accent)));
    connect(m_transcribeBtn, &QPushButton::clicked, this, &AudioSync::onTranscribeClicked);
    transcribePageLayout->addWidget(m_transcribeBtn);

    // Progress bar
    m_progressBar = new QProgressBar;
    m_progressBar->setRange(0, 100);
    m_progressBar->setValue(0);
    m_progressBar->setVisible(false);
    m_progressBar->setFixedHeight(12);
    m_progressBar->setStyleSheet(
        QString("QProgressBar { background: %1; border: none; border-radius: %3px; }"
        "QProgressBar::chunk { background: %2; border-radius: %3px; }").arg(inp, sucTx, radM));
    transcribePageLayout->addWidget(m_progressBar);

    // Transcribe status
    m_transcribeStatus = new QLabel;
    m_transcribeStatus->setWordWrap(true);
    m_transcribeStatus->setStyleSheet(QStringLiteral(
        "font-size: %1px; color: %2;")
        .arg(t.sizeSmall)
        .arg(Theme::rgb(c.textTertiary)));
    transcribePageLayout->addWidget(m_transcribeStatus);

    // File list with per-file transcription status indicators
    auto* transcribeFilesLabel = new QLabel("FILES");
    transcribeFilesLabel->setStyleSheet(QStringLiteral(
        "font-size: 13px; font-weight: 700; color: %1;"
        " letter-spacing: 1.5px;")
        .arg(Theme::rgb(c.textTertiary)));
    transcribePageLayout->addWidget(transcribeFilesLabel);

    m_transcribeFileList = new QListWidget;
    m_transcribeFileList->setStyleSheet(
        QString("QListWidget { background: %1; border: 1px solid %2; "
        "border-radius: %3px; font-size: 13px; }"
        "QListWidget::item { padding: 6px 10px; border-bottom: 1px solid %4; }"
        "QListWidget::item:last { border-bottom: none; }")
        .arg(inp, inpB, radM, Theme::rgb(c.borderLight)));
    m_transcribeFileList->installEventFilter(this);
    m_transcribeFileList->setContextMenuPolicy(Qt::DefaultContextMenu);
    transcribePageLayout->addWidget(m_transcribeFileList, 1);

    // Clear transcription buttons
    auto* clearBtnLayout = new QHBoxLayout;
    clearBtnLayout->setSpacing(m.spacingSm);

    m_clearSelectedTranscriptionBtn = new QPushButton("Clear Selected");
    m_clearSelectedTranscriptionBtn->setCursor(Qt::PointingHandCursor);
    m_clearSelectedTranscriptionBtn->setStyleSheet(QStringLiteral(
        "QPushButton {"
        "  background: %1; color: %2; border: 1px solid %3;"
        "  border-radius: %4px; font-size: 13px;"
        "  padding: 6px 14px; }"
        "QPushButton:hover { background: %5; }")
        .arg(Theme::rgb(c.surface2))
        .arg(Theme::rgb(c.textSecondary))
        .arg(Theme::rgb(c.border))
        .arg(m.radiusSm)
        .arg(Theme::rgb(c.surface3)));
    connect(m_clearSelectedTranscriptionBtn, &QPushButton::clicked, this, [this] {
        auto* item = m_transcribeFileList->currentItem();
        if (!item) return;
        int idx = m_transcribeFileList->row(item);
        clearTranscriptionForFile(static_cast<size_t>(idx));
    });
    clearBtnLayout->addWidget(m_clearSelectedTranscriptionBtn);

    m_clearAllTranscriptionsBtn = new QPushButton("Clear All");
    m_clearAllTranscriptionsBtn->setCursor(Qt::PointingHandCursor);
    m_clearAllTranscriptionsBtn->setStyleSheet(QStringLiteral(
        "QPushButton {"
        "  background: %1; color: %2; border: 1px solid %3;"
        "  border-radius: %4px; font-size: 13px;"
        "  padding: 6px 14px; }"
        "QPushButton:hover { background: %5; }")
        .arg(Theme::rgb(c.surface2))
        .arg(Theme::rgb(c.textSecondary))
        .arg(Theme::rgb(c.border))
        .arg(m.radiusSm)
        .arg(Theme::rgb(c.surface3)));
    connect(m_clearAllTranscriptionsBtn, &QPushButton::clicked, this, &AudioSync::clearAllTranscriptions);
    clearBtnLayout->addWidget(m_clearAllTranscriptionsBtn);

    transcribePageLayout->addLayout(clearBtnLayout);

    m_audioSidePanelStack->addWidget(m_transcribePage);   // index 2

    // --- MATCH page (index 3) â€” character filter list â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    m_matchPage = new QWidget;
    auto* matchPageLayout = new QVBoxLayout(m_matchPage);
    matchPageLayout->setContentsMargins(m.spacingMd, m.spacingLg,
                                         m.spacingMd, m.spacingLg);
    matchPageLayout->setSpacing(m.spacingMd);

    // Title row
    auto* matchTitleRow = new QHBoxLayout;
    auto* matchTitle = new QLabel("Characters");
    matchTitle->setStyleSheet(QStringLiteral(
        "font-size: %1px; font-weight: %2; color: %3;")
        .arg(t.sizeBody)
        .arg(t.weightBold)
        .arg(Theme::rgb(c.textPrimary)));
    matchTitleRow->addWidget(matchTitle, 1);

    auto* matchCloseBtn = new QPushButton("\u2715");
    matchCloseBtn->setFixedSize(28, 28);
    matchCloseBtn->setCursor(Qt::PointingHandCursor);
    matchCloseBtn->setStyleSheet(QStringLiteral(
        "QPushButton { background: transparent; border: none;"
        "  border-radius: %1px; font-size: 18px; color: %2; }"
        "QPushButton:hover { background: %3; color: %4; }")
        .arg(m.radiusMd)
        .arg(Theme::rgb(c.textTertiary))
        .arg(Theme::rgb(c.surface3))
        .arg(Theme::rgb(c.textPrimary)));
    connect(matchCloseBtn, &QPushButton::clicked,
            this, &AudioSync::hideAudioSidePanel);
    matchTitleRow->addWidget(matchCloseBtn);
    matchPageLayout->addLayout(matchTitleRow);

    // Character filter list
    m_charFilterList = new QListWidget;
    m_charFilterList->setSelectionMode(QAbstractItemView::SingleSelection);
    m_charFilterList->addItem("ALL");
    m_charFilterList->setCurrentRow(0);
    m_charFilterList->setStyleSheet(QStringLiteral(
        "QListWidget {"
        "  background: %1; border: 1px solid %2;"
        "  border-radius: %3px; font-size: %4px; font-weight: 600; outline: none;"
        "}"
        "QListWidget::item {"
        "  padding: 10px 14px; border-bottom: 1px solid %5;"
        "  color: %6;"
        "}"
        "QListWidget::item:last { border-bottom: none; }"
        "QListWidget::item:selected {"
        "  background: %7; color: %8;"
        "  font-weight: 700;"
        "}"
        "QListWidget::item:hover:!selected {"
        "  background: %9;"
        "}")
        .arg(Theme::rgb(c.inputBg))
        .arg(Theme::rgb(c.inputBorder))
        .arg(m.radiusMd)
        .arg(t.sizeBody)
        .arg(Theme::rgb(c.borderLight))
        .arg(Theme::rgb(c.textPrimary))
        .arg(Theme::rgb(c.accentDim))
        .arg(Theme::rgb(c.accent))
        .arg(Theme::rgb(c.surface3)));
    // Use a delegate to paint UNMATCHED items with red bg + white text
    m_charFilterList->setItemDelegate(new class UnmatchedDelegate(m_charFilterList));
    connect(m_charFilterList, &QListWidget::currentRowChanged, this, [this](int) {
        populateCards();
    });
    matchPageLayout->addWidget(m_charFilterList, 1);

    m_audioSidePanelStack->addWidget(m_matchPage);   // index 3

    // --- SETTINGS page (index 4) -----------------------------------------
    m_audioSettingsPage = new QWidget;
    auto* settingsPageLayout = new QVBoxLayout(m_audioSettingsPage);
    settingsPageLayout->setContentsMargins(m.spacingXxl, m.spacingXxl,
                                            m.spacingXxl, m.spacingXxl);
    settingsPageLayout->setSpacing(m.spacingXl);

    // Title row
    auto* settingsTitleRow = new QHBoxLayout;
    auto* settingsTitle = new QLabel("Audio Settings");
    settingsTitle->setStyleSheet(QStringLiteral(
        "font-size: 20px; font-weight: %1; color: %2;")
        .arg(t.weightBold)
        .arg(Theme::rgb(c.textPrimary)));
    settingsTitleRow->addWidget(settingsTitle, 1);

    auto* settingsCloseBtn = new QPushButton("\u2715");
    settingsCloseBtn->setFixedSize(36, 36);
    settingsCloseBtn->setCursor(Qt::PointingHandCursor);
    settingsCloseBtn->setStyleSheet(QStringLiteral(
        "QPushButton { background: transparent; border: none;"
        "  border-radius: %1px; font-size: 18px; color: %2; }"
        "QPushButton:hover { background: %3; color: %4; }")
        .arg(m.radiusMd)
        .arg(Theme::rgb(c.textTertiary))
        .arg(Theme::rgb(c.surface3))
        .arg(Theme::rgb(c.textPrimary)));
    connect(settingsCloseBtn, &QPushButton::clicked,
            this, &AudioSync::hideAudioSidePanel);
    settingsTitleRow->addWidget(settingsCloseBtn);
    settingsPageLayout->addLayout(settingsTitleRow);

    // Retakes section
    auto* retakesLabel = new QLabel("MATCHING OPTIONS");
    retakesLabel->setStyleSheet(QStringLiteral(
        "font-size: 13px; font-weight: 700; color: %1;"
        " letter-spacing: 1.5px;")
        .arg(Theme::rgb(c.textTertiary)));
    settingsPageLayout->addWidget(retakesLabel);

    m_retakesCheck = new QCheckBox("Allow retakes (multiple audio segments per script line)");
    m_retakesCheck->setStyleSheet(
        QString("QCheckBox { color: %1; font-size: %2px; spacing: 10px; }"
        "QCheckBox::indicator { width: 20px; height: 20px; }")
        .arg(txt1).arg(t.sizeSmall));
    m_retakesCheck->setToolTip("Allow script lines to match multiple audio segments");
    settingsPageLayout->addWidget(m_retakesCheck);

    settingsPageLayout->addStretch();
    m_audioSidePanelStack->addWidget(m_audioSettingsPage);   // index 4

    rootLayout->addWidget(m_audioSidePanel);

    // â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
    //  CONTENT AREA  (match workspace + action bar)
    // â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
    m_audioContentArea = new QWidget;
    auto* contentLayout = new QVBoxLayout(m_audioContentArea);
    contentLayout->setContentsMargins(0, 0, 0, 0);
    contentLayout->setSpacing(0);

    // â”€â”€ Status bar (compact, at top of content) â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    auto* statusBar = new QWidget;
    statusBar->setFixedHeight(36);
    statusBar->setStyleSheet(
        QString("QWidget { background: %1; border-bottom: 1px solid %2; }").arg(surf0, brd));
    auto* statusLayout = new QHBoxLayout(statusBar);
    statusLayout->setContentsMargins(12, 0, 12, 0);
    statusLayout->setSpacing(8);

    m_smartBarIcon = new QLabel(QStringLiteral("\u25CF"));
    m_smartBarIcon->setFixedWidth(18);
    m_smartBarIcon->setStyleSheet(QString("QLabel { color: %1; font-size: %2px; border: none; }")
        .arg(txtD).arg(t.sizeSmall));
    statusLayout->addWidget(m_smartBarIcon);

    m_smartBarLabel = new QLabel("Load a script to begin");
    m_smartBarLabel->setStyleSheet(
        QString("QLabel { color: %1; font-size: 13px; border: none; }").arg(txt2));
    statusLayout->addWidget(m_smartBarLabel, 1);

    m_syncStatus = new QLabel("0 / 0");
    m_syncStatus->setStyleSheet(QString("QLabel { color: %1; font-size: %2px; border: none; }")
        .arg(txtD).arg(t.sizeXs));
    statusLayout->addWidget(m_syncStatus);

    contentLayout->addWidget(statusBar);

    // â”€â”€ MAIN SPLIT PANE  (Left: tabs+list | Right: card scroll) â”€â”€â”€â”€â”€â”€â”€â”€â”€
    m_splitter = new QSplitter(Qt::Horizontal);
    m_splitter->setStyleSheet(
        QString("QSplitter { background: %1; }"
        "QSplitter::handle { background: %2; width: 3px; }").arg(surf0, brd));

    // â”€â”€ LEFT PANE â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    auto* leftPane = new QWidget;
    leftPane->setStyleSheet(QString("QWidget { background: %1; }").arg(surf1));
    auto* leftLayout = new QVBoxLayout(leftPane);
    leftLayout->setContentsMargins(0, 0, 0, 0);
    leftLayout->setSpacing(0);

    // Character tabs removed â€” filter now in MATCH side panel (m_charFilterList)

    // Script line list
    m_leftScriptList = new QListWidget;
    m_leftScriptList->setSelectionMode(QAbstractItemView::SingleSelection);
    m_leftScriptList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_leftScriptList->setWordWrap(false);
    m_leftScriptList->setTextElideMode(Qt::ElideNone);
    m_leftScriptList->setStyleSheet(
        "QListWidget { background: " + surf0 + "; border: none; outline: none; padding-top: 4px; }"
        "QListWidget::item { background: transparent; border: none; padding: 0px; margin: 4px 8px; }"
        "QListWidget::item:selected { background: transparent; }"
        "QListWidget::item:hover { background: transparent; }");
    connect(m_leftScriptList, &QListWidget::currentRowChanged, this, [this](int row) {
        // Restore previous left card style
        if (m_selectedLeftCard) {
            m_selectedLeftCard->setGraphicsEffect(nullptr);
            m_selectedLeftCard = nullptr;
        }
        if (row >= 0 && row < static_cast<int>(m_cardScriptLineNums.size())) {
            scrollToCard(m_cardScriptLineNums[static_cast<size_t>(row)]);
            // Select the corresponding clip for spacebar playback
            auto idx = static_cast<size_t>(row);
            if (idx < m_cardClipIndices.size() && m_cardClipIndices[idx] >= 0)
                m_selectedClipIdx = m_cardClipIndices[idx];
            // Highlight the left card
            auto* item = m_leftScriptList->item(row);
            auto* w = m_leftScriptList->itemWidget(item);
            auto* frame = qobject_cast<QFrame*>(w);
            if (frame) {
                m_selectedLeftCard = frame;
                auto* glow = new QGraphicsDropShadowEffect(frame);
                glow->setBlurRadius(14);
                glow->setColor(Theme::colors().accent);
                glow->setOffset(0, 0);
                frame->setGraphicsEffect(glow);
            }
        }
    });
    m_leftScriptList->installEventFilter(this);
    leftLayout->addWidget(m_leftScriptList, 1);

    // Orphan clips section
    m_leftOrphanLabel = new QLabel("  Orphan Clips");
    m_leftOrphanLabel->setFixedHeight(28);
    m_leftOrphanLabel->setStyleSheet(
        "QLabel { color: " + errC + "; font-size: " + QString::number(t.sizeXs) + "px; font-weight: bold; "
        "background: " + errBg + "; border: none; border-top: 2px solid " + errC + "; }");
    m_leftOrphanLabel->setVisible(false);
    leftLayout->addWidget(m_leftOrphanLabel);

    m_leftOrphanList = new QListWidget;
    m_leftOrphanList->setMaximumHeight(120);
    m_leftOrphanList->setStyleSheet(
        "QListWidget { background: " + surf0 + "; border: none; font-size: " + QString::number(t.sizeXs) + "px; }"
        "QListWidget::item { padding: 4px 8px; }"
        "QListWidget::item:selected { background: " + surf2 + "; }");
    m_leftOrphanList->setVisible(false);
    leftLayout->addWidget(m_leftOrphanList);

    m_splitter->addWidget(leftPane);

    // â”€â”€ RIGHT PANE  (continuous scroll of cards) â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    m_rightScrollArea = new QScrollArea;
    m_rightScrollArea->setWidgetResizable(true);
    m_rightScrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_rightScrollArea->setStyleSheet(
        "QScrollArea { background: " + surf1 + "; border: none; }"
        "QScrollBar:vertical { background: " + scrTr + "; width: 16px; border: none; }"
        "QScrollBar::handle:vertical { background: " + scrTh + "; border-radius: 7px; min-height: 40px; }"
        "QScrollBar::handle:vertical:hover { background: " + txt3 + "; }"
        "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }");

    m_rightScrollContent = new QWidget;
    m_rightScrollContent->setStyleSheet("QWidget { background: " + surf1 + "; }");
    m_rightLayout = new QVBoxLayout(m_rightScrollContent);
    m_rightLayout->setContentsMargins(12, 12, 12, 12);
    m_rightLayout->setSpacing(10);
    m_rightLayout->addStretch();

    m_rightScrollArea->setWidget(m_rightScrollContent);

    // Sync right scroll â†’ left list selection
    connect(m_rightScrollArea->verticalScrollBar(), &QScrollBar::valueChanged,
            this, [this]() { syncLeftListFromScroll(); });

    m_splitter->addWidget(m_rightScrollArea);
    m_splitter->setSizes({420, 480});

    contentLayout->addWidget(m_splitter, 1);

    // â”€â”€ BOTTOM ACTION BAR â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    m_audioActionBar = new QWidget;
    m_audioActionBar->setFixedHeight(56);
    m_audioActionBar->setStyleSheet(
        QString("QWidget { background: %1; border-top: 1px solid %2; }").arg(surf0, brd));

    auto* actionBarLayout = new QHBoxLayout(m_audioActionBar);
    actionBarLayout->setContentsMargins(12, 0, 12, 0);
    actionBarLayout->setSpacing(10);

    auto makeActionBtn = [&](const QString& text, const QColor& bg,
                             const QColor& hoverBg, const QColor& textColor) -> QPushButton*
    {
        auto* btn = new QPushButton(text);
        btn->setMinimumHeight(36);
        btn->setCursor(Qt::PointingHandCursor);
        btn->setStyleSheet(QStringLiteral(
            "QPushButton {"
            "  background: %1; color: %2; border: none;"
            "  border-radius: %3px; font-size: 13px;"
            "  font-weight: 600; padding: 6px 16px; }"
            "QPushButton:hover { background: %4; }"
            "QPushButton:disabled { background: %5; color: %6; }")
            .arg(Theme::rgb(bg))
            .arg(Theme::rgb(textColor))
            .arg(m.radiusMd)
            .arg(Theme::rgb(hoverBg))
            .arg(Theme::rgb(c.surface2))
            .arg(Theme::rgb(c.textDisabled)));
        return btn;
    };

    m_syncActionBtn = makeActionBtn(
        "\U0001F504  Auto-Sync", c.accentDim, c.accentHover, c.textPrimary);
    connect(m_syncActionBtn, &QPushButton::clicked, this, &AudioSync::onAutoSyncClicked);
    actionBarLayout->addWidget(m_syncActionBtn);

    // Rebuild routine shared by all bulk action handlers \u2014 defers via
    // QTimer::singleShot so the clicked widget isn't destroyed mid-signal.
    auto bulkRebuild = [this]() {
        QTimer::singleShot(0, this, [this]() {
            if (m_destroying.load(std::memory_order_acquire)) return;
            populateCards();
            updateWorkflowState();
        });
    };

    m_confirmAllActionBtn = makeActionBtn(
        "\u2713  Confirm All", c.successBtnBg, c.successBtnHover, c.textPrimary);
    connect(m_confirmAllActionBtn, &QPushButton::clicked, this, [this, bulkRebuild]() {
        runClipsMutationWithUndo("Confirm all audio matches",
            [this]() {
                for (auto& clip : m_clips) {
                    if (clip.matchState == 1)
                        clip.matchState = 2;
                }
            },
            bulkRebuild);
    });
    actionBarLayout->addWidget(m_confirmAllActionBtn);

    m_unconfirmAllActionBtn = makeActionBtn(
        "\u21A9  Unconfirm All", c.surface3, c.surface2, c.textPrimary);
    connect(m_unconfirmAllActionBtn, &QPushButton::clicked, this, [this, bulkRebuild]() {
        runClipsMutationWithUndo("Unconfirm all audio matches",
            [this]() {
                for (auto& clip : m_clips) {
                    if (clip.matchState == 2)
                        clip.matchState = 1;
                }
            },
            bulkRebuild);
    });
    actionBarLayout->addWidget(m_unconfirmAllActionBtn);

    m_clearActionBtn = makeActionBtn(
        "\u2715  Clear Matches", c.surface3, c.surface2, c.textPrimary);
    connect(m_clearActionBtn, &QPushButton::clicked, this, [this, bulkRebuild]() {
        runClipsMutationWithUndo("Clear all audio matches",
            [this]() {
                for (auto& clip : m_clips) {
                    clip.matchState = 0;
                    clip.confidence = 0.0f;
                    clip.scriptLineNumber = -1;
                    clip.scriptSegment.clear();
                }
            },
            bulkRebuild);
    });
    actionBarLayout->addWidget(m_clearActionBtn);

    auto* closeGapsBtn = makeActionBtn(
        "\u2194  Close Gaps", c.surface3, c.surface2, c.textPrimary);
    connect(closeGapsBtn, &QPushButton::clicked, this, [this, bulkRebuild]() {
        runClipsMutationWithUndo("Close inter-clip gaps",
            [this]() { closeInterClipGaps(); },
            bulkRebuild);
    });
    actionBarLayout->addWidget(closeGapsBtn);

    actionBarLayout->addStretch();

    m_statusLabel = new QLabel;
    m_statusLabel->setStyleSheet(
        QString("QLabel { color: %1; font-size: %2px; border: none; }")
        .arg(txtD).arg(t.sizeXs));
    actionBarLayout->addWidget(m_statusLabel);

    m_exportActionBtn = makeActionBtn(
        "\U0001F4E4  Export \u2192", c.successBtnBg, c.successBtnHover, c.textPrimary);
    connect(m_exportActionBtn, &QPushButton::clicked, this, &AudioSync::onExportClicked);
    actionBarLayout->addWidget(m_exportActionBtn);

    contentLayout->addWidget(m_audioActionBar);

    rootLayout->addWidget(m_audioContentArea, 1);

    // Legacy / hidden compat widgets are no longer created.
    // All member pointers default to nullptr; references are null-guarded.
}

} // namespace rt
