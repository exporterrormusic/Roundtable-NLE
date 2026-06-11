/*
 * AudioSyncCards.cpp - Script list and waveform card builders for AudioSync.
 * Split from AudioSync.cpp for maintainability.
 */

#include "panels/audio/AudioSync.h"

#include "ai/ScriptMatcher.h"
#include "command/CommandStack.h"
#include "command/LambdaCommand.h"
#include "audio/AudioEngine.h"
#include "widgets/MiniWaveformWidget.h"
#include "Theme.h"

#include <QComboBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QScrollBar>
#include <QTimer>
#include <QVBoxLayout>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <filesystem>

namespace rt {

void AudioSync::populateLeftList()
{
    if (!m_leftScriptList) return;
    m_leftScriptList->blockSignals(true);
    m_leftScriptList->clear();
    m_cardScriptLineNums.clear();

    if (!m_script) return;

    // Current character filter from MATCH side panel list
    QString filterChar;
    bool filterUnmatched = false;
    if (m_charFilterList && m_charFilterList->currentRow() > 0) {
        auto* item = m_charFilterList->currentItem();
        if (item) {
            if (item->text() == "UNMATCHED")
                filterUnmatched = true;
            else
                filterChar = item->text();
        }
    }

    // Build match state map: lineNumber â†’ best match state
    std::unordered_map<int, int> lineMatchState;
    for (const auto& clip : m_clips) {
        if (clip.scriptLineNumber > 0) {
            int existing = lineMatchState.count(clip.scriptLineNumber)
                ? lineMatchState[clip.scriptLineNumber] : 0;
            lineMatchState[clip.scriptLineNumber] = std::max(existing, clip.matchState);
        }
    }

    int displayIdx = 0;
    for (const auto& line : m_script->lines) {
        // Apply character filter
        if (!filterChar.isEmpty()) {
            if (QString::fromStdString(line.character) != filterChar)
                continue;
        }

        // Apply unmatched filter â€” show only lines without a matched clip
        if (filterUnmatched) {
            if (lineMatchState.count(line.lineNumber) > 0)
                continue;
        }
        ++displayIdx;

        int matchState = 0;
        auto it = lineMatchState.find(line.lineNumber);
        if (it != lineMatchState.end())
            matchState = it->second;

        // Status icon
        QString icon;
        QString iconColor;
        const auto& _tc = Theme::colors();
        switch (matchState) {
        case 2:  icon = QStringLiteral("\u2713"); iconColor = Theme::hex(_tc.success); break;
        case 1:  icon = "?"; iconColor = Theme::hex(_tc.warning); break;
        default: icon = QStringLiteral("\u25CB"); iconColor = Theme::hex(_tc.textDisabled); break;
        }

        QColor cc = characterColor(line.character);
        QString charName = QString::fromStdString(line.character);
        QString dialogueText = QString::fromStdString(line.dialogue);

        // â”€â”€ Build custom card widget: [ #N | CHAR | dialogue text ] â”€â”€â”€
        // Number box colored by match state
        QString numBg;
        switch (matchState) {
        case 2:  numBg = Theme::hex(_tc.successBtnBg); break;
        case 1:  numBg = Theme::hex(_tc.warning); break;
        default: numBg = Theme::hex(_tc.error); break;
        }

        // Card background with subtle match tint
        QString cardBg, cardBorder;
        switch (matchState) {
        case 2:  cardBg = Theme::hex(_tc.successBg); cardBorder = Theme::hex(_tc.success); break;
        case 1:  cardBg = Theme::hex(_tc.warningBg); cardBorder = Theme::hex(_tc.warning); break;
        default: cardBg = Theme::hex(_tc.errorBg); cardBorder = Theme::hex(_tc.error); break;
        }

        // Hover variant: slightly brighter background + border
        QString hoverBg, hoverBorder;
        switch (matchState) {
        case 2:  hoverBg = Theme::hex(_tc.successBg.lighter(120)); hoverBorder = Theme::hex(_tc.success.lighter(120)); break;
        case 1:  hoverBg = Theme::hex(_tc.warningBg.lighter(120)); hoverBorder = Theme::hex(_tc.warning.lighter(120)); break;
        default: hoverBg = Theme::hex(_tc.surface3); hoverBorder = Theme::hex(_tc.border); break;
        }

        auto* cardWidget = new QFrame;
        cardWidget->setObjectName(QString("leftCard_%1").arg(line.lineNumber));
        cardWidget->setCursor(Qt::PointingHandCursor);
        cardWidget->setStyleSheet(
            QString("QFrame#leftCard_%1 { background: %2; border: 1px solid %3; border-radius: %6px; }"
                    "QFrame#leftCard_%1:hover { background: %4; border: 1px solid %5; }")
                .arg(line.lineNumber).arg(cardBg, cardBorder, hoverBg, hoverBorder).arg(Theme::metrics().radiusMd));

        auto* rowLayout = new QHBoxLayout(cardWidget);
        rowLayout->setContentsMargins(0, 0, 0, 0);
        rowLayout->setSpacing(0);

        // Number badge â€” colored by match state
        auto* numLabel = new QLabel(QString::number(displayIdx));
        numLabel->setFixedWidth(36);
        numLabel->setAlignment(Qt::AlignCenter);
        numLabel->setStyleSheet(
            QString("QLabel { color: %2; font-size: 14px; font-weight: bold; "
                    "background: %1; border: none; "
                    "border-top-left-radius: %3px; border-bottom-left-radius: %3px; "
                    "padding: 4px 0px; }")
                .arg(numBg, Theme::hex(_tc.textBright)).arg(Theme::metrics().radiusMd));
        rowLayout->addWidget(numLabel);

        // Character name tag â€” neutral bg, colored text
        auto* charLabel = new QLabel(charName);
        charLabel->setFixedWidth(80);
        charLabel->setAlignment(Qt::AlignCenter);
        charLabel->setStyleSheet(
            QString("QLabel { color: %1; font-size: 13px; font-weight: bold; "
                    "background: %2; border: none; padding: 4px 4px; }")
                .arg(cc.lighter(130).name(), Theme::hex(_tc.surface2)));
        rowLayout->addWidget(charLabel);

        // Script dialogue text
        auto* textLabel = new QLabel(dialogueText);
        textLabel->setWordWrap(true);
        textLabel->setStyleSheet(
            QString("QLabel { color: %1; font-size: 14px; "
                    "background: transparent; border: none; "
                    "padding: 6px 10px; }").arg(Theme::hex(_tc.textPrimary)));
        textLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        rowLayout->addWidget(textLabel, 1);

        // â”€â”€ Insert into list â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
        auto* item = new QListWidgetItem(m_leftScriptList);
        item->setData(Qt::UserRole, line.lineNumber);
        item->setSizeHint(QSize(0, std::max(cardWidget->sizeHint().height(), 42) + 6));
        m_leftScriptList->setItemWidget(item, cardWidget);

        m_cardScriptLineNums.push_back(line.lineNumber);
    }

    // Orphan clips hidden â€” manual matching is the preferred workflow.
    if (m_leftOrphanLabel) m_leftOrphanLabel->setVisible(false);
    if (m_leftOrphanList) {
        m_leftOrphanList->clear();
        m_leftOrphanList->setVisible(false);
    }

    m_leftScriptList->blockSignals(false);
    updateSmartBar();
}

// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
//  Right pane â€” continuous scroll of script-line cards
// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•


} // namespace rt
