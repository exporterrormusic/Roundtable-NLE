/*
 * AudioSyncCards.cpp - Script list and waveform card builders for AudioSync.
 * Split from AudioSync.cpp for maintainability.
 */

#include "panels/audio/AudioSync.h"
#include "panels/audio/VideoFrameGrabber.h"

#include "ai/ScriptMatcher.h"
#include "command/CommandStack.h"
#include "command/LambdaCommand.h"
#include "audio/AudioEngine.h"
#include "widgets/MiniWaveformWidget.h"
#include "widgets/VideoFrameView.h"
#include "widgets/ScriptTextCopy.h"
#include "Theme.h"

#include <QComboBox>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QPixmap>
#include <QPushButton>
#include <QScrollBar>
#include <QTimer>
#include <QVBoxLayout>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <filesystem>

namespace rt {

namespace {
// Match-state colouring of one left-list row (card frame + number badge).
void styleLeftCard(QFrame* cardWidget, QLabel* numLabel, int lineNumber, int matchState)
{
    const auto& _tc = Theme::colors();
    QString numBg, cardBg, cardBorder, hoverBg, hoverBorder;
    switch (matchState) {
    case 2:
        numBg = Theme::hex(_tc.successBtnBg);
        cardBg = Theme::hex(_tc.successBg); cardBorder = Theme::hex(_tc.success);
        hoverBg = Theme::hex(_tc.successBg.lighter(120)); hoverBorder = Theme::hex(_tc.success.lighter(120));
        break;
    case 1:
        numBg = Theme::hex(_tc.warning);
        cardBg = Theme::hex(_tc.warningBg); cardBorder = Theme::hex(_tc.warning);
        hoverBg = Theme::hex(_tc.warningBg.lighter(120)); hoverBorder = Theme::hex(_tc.warning.lighter(120));
        break;
    default:
        numBg = Theme::hex(_tc.error);
        cardBg = Theme::hex(_tc.errorBg); cardBorder = Theme::hex(_tc.error);
        hoverBg = Theme::hex(_tc.surface3); hoverBorder = Theme::hex(_tc.border);
        break;
    }
    cardWidget->setStyleSheet(
        QString("QFrame#leftCard_%1 { background: %2; border: 1px solid %3; border-radius: %6px; }"
                "QFrame#leftCard_%1:hover { background: %4; border: 1px solid %5; }")
            .arg(lineNumber).arg(cardBg, cardBorder, hoverBg, hoverBorder).arg(Theme::metrics().radiusMd));
    if (numLabel) {
        numLabel->setStyleSheet(
            QString("QLabel { color: %2; font-size: %4px; font-weight: bold; "
                    "background: %1; border: none; "
                    "border-top-left-radius: %3px; border-bottom-left-radius: %3px; "
                    "padding: 4px 0px; }")
                .arg(numBg, Theme::hex(_tc.textBright)).arg(Theme::metrics().radiusMd)
                .arg(Theme::typography().sizeSmall));
    }
    cardWidget->setProperty("matchState", matchState);
}
} // namespace

void AudioSync::populateLeftList()
{
    if (!m_leftScriptList) return;
    m_leftBuild = PendingLeftBuild{};   // a rebuild replaces one still streaming
    if (!m_script) {
        m_leftScriptList->blockSignals(true);
        m_leftScriptList->clear();
        m_cardScriptLineNums.clear();
        m_leftScriptList->blockSignals(false);
        return;
    }

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

    // Build match state map: lineNumber → best match state
    std::unordered_map<int, int> lineMatchState;
    for (const auto& clip : m_clips) {
        if (clip.scriptLineNumber > 0) {
            int existing = lineMatchState.count(clip.scriptLineNumber)
                ? lineMatchState[clip.scriptLineNumber] : 0;
            lineMatchState[clip.scriptLineNumber] = std::max(existing, clip.matchState);
        }
    }

    m_leftScriptList->blockSignals(true);
    m_leftScriptList->clear();
    m_cardScriptLineNums.clear();

    for (size_t lineIndex = 0; lineIndex < m_script->lines.size(); ++lineIndex) {
        const auto& line = m_script->lines[lineIndex];
        // Apply character filter
        if (!filterChar.isEmpty()) {
            if (QString::fromStdString(line.character) != filterChar)
                continue;
        }

        // Apply unmatched filter — show only lines without a matched clip
        if (filterUnmatched) {
            if (lineMatchState.count(line.lineNumber) > 0)
                continue;
        }
        // Items (row index, line number) exist for every row up front, so
        // left row i stays right-pane card i; the row widgets -- a few
        // style sheets each, ~3-4 ms a row -- stream in like the cards.
        auto* item = new QListWidgetItem(m_leftScriptList);
        item->setData(Qt::UserRole, line.lineNumber);
        item->setSizeHint(QSize(0, 42 + 6));   // until its widget is built
        m_cardScriptLineNums.push_back(line.lineNumber);
        m_leftBuild.lineIndices.push_back(lineIndex);
    }

    m_leftBuild.script = m_script.get();
    m_leftBuild.active = !m_leftBuild.lineIndices.empty();
    // The first screen of rows now (it is what shows), the rest in slices.
    const int wanted = 2 * std::max(m_leftScriptList->viewport()->height(), 600);
    int builtHeight = 0;
    while (m_leftBuild.active && builtHeight < wanted) {
        buildLeftRow(m_leftBuild.next, lineMatchState);
        if (auto* item = m_leftScriptList->item(static_cast<int>(m_leftBuild.next)))
            builtHeight += item->sizeHint().height();
        m_leftBuild.active = ++m_leftBuild.next < m_leftBuild.lineIndices.size();
    }
    if (m_leftBuild.active && m_cardBuildTimer && !m_cardBuildTimer->isActive())
        m_cardBuildTimer->start();

    // Orphan clips hidden — manual matching is the preferred workflow.
    if (m_leftOrphanLabel) m_leftOrphanLabel->setVisible(false);
    if (m_leftOrphanList) {
        m_leftOrphanList->clear();
        m_leftOrphanList->setVisible(false);
    }

    m_leftScriptList->blockSignals(false);
    updateSmartBar();
}

void AudioSync::buildLeftRow(size_t row, const std::unordered_map<int, int>& lineMatchState)
{
    QListWidgetItem* item = m_leftScriptList->item(static_cast<int>(row));
    if (!item || row >= m_leftBuild.lineIndices.size()) return;
    const auto& line = m_script->lines[m_leftBuild.lineIndices[row]];
    const int displayIdx = static_cast<int>(row) + 1;

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

    // ── Build custom card widget: [ #N | CHAR | dialogue text ] ───
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
    cardWidget->setProperty("matchState", matchState);  // refreshLeftListStates()
    cardWidget->setObjectName(QString("leftCard_%1").arg(line.lineNumber));
    cardWidget->setCursor(Qt::PointingHandCursor);
    cardWidget->setStyleSheet(
        QString("QFrame#leftCard_%1 { background: %2; border: 1px solid %3; border-radius: %6px; }"
                "QFrame#leftCard_%1:hover { background: %4; border: 1px solid %5; }")
            .arg(line.lineNumber).arg(cardBg, cardBorder, hoverBg, hoverBorder).arg(Theme::metrics().radiusMd));

    auto* rowLayout = new QHBoxLayout(cardWidget);
    rowLayout->setContentsMargins(0, 0, 0, 0);
    rowLayout->setSpacing(0);

    // Number badge — colored by match state
    auto* numLabel = new QLabel(QString::number(displayIdx));
    numLabel->setFixedWidth(36);
    numLabel->setAlignment(Qt::AlignCenter);
    numLabel->setStyleSheet(
        QString("QLabel { color: %2; font-size: %4px; font-weight: bold; "
                "background: %1; border: none; "
                "border-top-left-radius: %3px; border-bottom-left-radius: %3px; "
                "padding: 4px 0px; }")
            .arg(numBg, Theme::hex(_tc.textBright)).arg(Theme::metrics().radiusMd)
            .arg(Theme::typography().sizeSmall));
    rowLayout->addWidget(numLabel);

    // Character name tag — neutral bg, colored text
    if (m_script->isShotScript()) {
        // One announcer, so the tag shows the shot instead.
        rowLayout->addWidget(createShotTag(line, 80));
    } else {
        auto* charLabel = new QLabel(charName);
        charLabel->setFixedWidth(80);
        charLabel->setAlignment(Qt::AlignCenter);
        charLabel->setStyleSheet(
            QString("QLabel { color: %1; font-size: 13px; font-weight: bold; "
                    "background: %2; border: none; padding: 4px 4px; }")
                .arg(cc.lighter(130).name(), Theme::hex(_tc.surface2)));
        rowLayout->addWidget(charLabel);
    }

    // Script dialogue text
    auto* textLabel = new QLabel(dialogueText);
    textLabel->setWordWrap(true);
    enableScriptTextCopy(textLabel, dialogueText);
    addGenerateVoiceAction(textLabel, line.lineNumber, charName, dialogueText,
                           QString::fromStdString(line.segment));
    textLabel->setStyleSheet(
        QString("QLabel { color: %1; font-size: %2px; "
                "background: transparent; border: none; "
                "padding: 6px 10px; }").arg(Theme::hex(_tc.textPrimary))
            .arg(Theme::typography().sizeSmall));
    textLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    rowLayout->addWidget(textLabel, 1);

    // ── Insert into list ────────────────────────────────────────
    item->setSizeHint(QSize(0, std::max(cardWidget->sizeHint().height(), 42) + 6));
    m_leftScriptList->setItemWidget(item, cardWidget);
}

void AudioSync::buildPendingLeftRows(int budgetMs)
{
    if (!m_leftBuild.active || m_destroying.load(std::memory_order_acquire)) return;
    if (!m_leftScriptList || !m_script || m_script.get() != m_leftBuild.script ||
        m_leftScriptList->count() != static_cast<int>(m_leftBuild.lineIndices.size())) {
        m_leftBuild = PendingLeftBuild{};   // stale: the list was rebuilt or cleared
        return;
    }
    std::unordered_map<int, int> lineMatchState;   // fresh: states change between slices
    for (const auto& clip : m_clips) {
        if (clip.scriptLineNumber > 0) {
            int& st = lineMatchState[clip.scriptLineNumber];
            st = std::max(st, clip.matchState);
        }
    }
    QElapsedTimer slice;
    slice.start();
    do {
        buildLeftRow(m_leftBuild.next, lineMatchState);
        m_leftBuild.active = ++m_leftBuild.next < m_leftBuild.lineIndices.size();
    } while (m_leftBuild.active && (budgetMs < 0 || slice.elapsed() < budgetMs));
}

void AudioSync::refreshLeftListStates()
{
    if (!m_leftScriptList || !m_script) { populateLeftList(); return; }

    // Filters change which rows exist when a state changes — full rebuild.
    if (m_charFilterList && m_charFilterList->currentRow() > 0) {
        populateLeftList();
        return;
    }
    std::unordered_map<int, int> lineMatchState;
    for (const auto& clip : m_clips) {
        if (clip.scriptLineNumber > 0) {
            int& st = lineMatchState[clip.scriptLineNumber];
            st = std::max(st, clip.matchState);
        }
    }
    const int rows = m_leftScriptList->count();
    if (rows != static_cast<int>(m_script->lines.size()) ||
        m_cardScriptLineNums.size() != static_cast<size_t>(rows)) {
        populateLeftList();
        return;
    }
    // Restyle only rows whose state changed.  Rebuilding hundreds of
    // stylesheet'd row widgets on every confirm/reject click was a big part
    // of the MATCH tab's click lag.
    for (int r = 0; r < rows; ++r) {
        auto* item = m_leftScriptList->item(r);
        auto* frame = qobject_cast<QFrame*>(m_leftScriptList->itemWidget(item));
        const int lineNumber = item ? item->data(Qt::UserRole).toInt() : -1;
        if (lineNumber != m_script->lines[static_cast<size_t>(r)].lineNumber) {
            populateLeftList();
            return;
        }
        if (!frame) {
            // Not built yet: it takes the current state when it is.
            if (m_leftBuild.active && static_cast<size_t>(r) >= m_leftBuild.next) continue;
            populateLeftList();
            return;
        }
        auto it = lineMatchState.find(lineNumber);
        const int state = (it != lineMatchState.end()) ? it->second : 0;
        if (frame->property("matchState").toInt() == state) continue;
        QLabel* numLabel = nullptr;
        if (auto* lay = frame->layout(); lay && lay->count() > 0)
            numLabel = qobject_cast<QLabel*>(lay->itemAt(0)->widget());
        styleLeftCard(frame, numLabel, lineNumber, state);
    }
    updateSmartBar();
}

// ═════════════════════════════════════════════════════════════════════════
//  Right pane — continuous scroll of script-line cards
// ═════════════════════════════════════════════════════════════════════════

// ═════════════════════════════════════════════════════════════════════════
//  Shot-table scripts — per-line shot tag + in-app pick for blank cells
// ═════════════════════════════════════════════════════════════════════════

// ═════════════════════════════════════════════════════════════════════════
//  Video takes — preview frames + take chips (video sources only)
// ═════════════════════════════════════════════════════════════════════════

VideoFrameGrabber* AudioSync::frameGrabber()
{
    if (!m_frameGrabber) m_frameGrabber = std::make_unique<VideoFrameGrabber>();
    return m_frameGrabber.get();
}

void AudioSync::requestTakePreviewFrame(size_t clipIdx, double timeSec)
{
    auto it = m_takePreviewLabels.find(clipIdx);
    if (it == m_takePreviewLabels.end() || !it->second || clipIdx >= m_clips.size()) return;
    QPointer<VideoFrameView> view = it->second;
    frameGrabber()->requestLive(view, m_clips[clipIdx].sourceFile, timeSec, view->requestSize(),
                                [view](const QImage& image) {
                                    if (view) view->setFrame(image);
                                }, VideoFrameGrabber::Playback);
}

void AudioSync::scheduleCardRebuild(std::vector<int> lineNumbers)
{
    QTimer::singleShot(0, this, [this, lines = std::move(lineNumbers)]() {
        if (!m_destroying.load(std::memory_order_acquire)) rebuildCardsForLines(lines);
    });
}

void AudioSync::setHighlightedCard(QWidget* card)
{
    m_highlightedCard = card;
    if (!card) {
        if (m_cardHighlight) m_cardHighlight->hide();
        return;
    }
    if (!m_cardHighlight) {
        m_cardHighlight = new QFrame;
        m_cardHighlight->setObjectName("cardHighlightOverlay");
        m_cardHighlight->setAttribute(Qt::WA_TransparentForMouseEvents);
        m_cardHighlight->setStyleSheet(
            QString("QFrame#cardHighlightOverlay { background: transparent; "
                    "border: 2px solid %1; border-radius: %2px; }")
                .arg(Theme::hex(Theme::colors().accent))
                .arg(Theme::metrics().radiusMd));
    }
    // Reparent onto the card; its Resize events (card->installEventFilter(this))
    // keep the overlay's geometry in sync — see eventFilter().
    if (m_cardHighlight->parentWidget() != card) m_cardHighlight->setParent(card);
    m_cardHighlight->setGeometry(card->rect());
    m_cardHighlight->raise();
    m_cardHighlight->show();
}

void AudioSync::styleTakeChip(QPushButton* chip, bool chosen, bool shown)
{
    const auto& tc = Theme::colors();
    chip->setText((chosen ? QStringLiteral("\u2713 ") : QString()) +
                  chip->property("takeLabel").toString());
    const QString border = chosen ? Theme::hex(tc.success)
                         : shown  ? Theme::hex(tc.accent)
                                  : Theme::hex(tc.controlBorder);
    chip->setStyleSheet(
        QString("QPushButton { background: %1; color: %2; border: 2px solid %3; "
                "border-radius: %4px; font-size: %5px; font-weight: %6; padding: 2px 6px; }"
                "QPushButton:hover { border-color: %7; }")
            .arg(Theme::hex(shown ? tc.surface3 : tc.surface2),
                 Theme::hex(tc.textPrimary), border)
            .arg(Theme::metrics().radiusSm)
            .arg(Theme::typography().sizeXs)
            .arg(shown ? 700 : 400)
            .arg(Theme::hex(tc.accent)));
}

QWidget* AudioSync::createTakeStrip(int lineNumber, const std::vector<size_t>& clipIndices,
                                    size_t shownClipIdx)
{
    const auto& tc = Theme::colors();
    auto* strip = new QWidget;
    strip->setStyleSheet("background: transparent; border: none;");
    auto* row = new QHBoxLayout(strip);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(6);

    auto* title = new QLabel(QStringLiteral("TAKES"));
    title->setStyleSheet(QString("QLabel { color: %1; font-size: %2px; font-weight: bold; }")
                             .arg(Theme::hex(tc.textSecondary))
                             .arg(Theme::typography().sizeXs));
    row->addWidget(title);

    const QSize thumbSize(64, 36);
    int takeNumber = 0;
    for (size_t ci : clipIndices) {
        if (ci >= m_clips.size()) continue;
        const SyncClip& clip = m_clips[ci];
        ++takeNumber;
        const bool chosen = (clip.matchState == 2);
        const bool shown  = (ci == shownClipIdx);
        const int  secs   = static_cast<int>(clip.start);

        auto* chip = new QPushButton;
        chip->setCursor(Qt::PointingHandCursor);
        chip->setIconSize(thumbSize);
        chip->setToolTip(QString("Take %1 of %2\n%3\n%4s – %5s · %6% of the line\n"
                                 "Click to view this take; CONFIRM picks it.")
            .arg(takeNumber).arg(clipIndices.size())
            .arg(QFileInfo(QString::fromStdString(clip.sourceFile)).fileName())
            .arg(clip.start, 0, 'f', 1).arg(clip.end, 0, 'f', 1)
            .arg(static_cast<int>(clip.confidence * 100.0f)));
        chip->setObjectName("takeChip");
        chip->setProperty("takeClipIndex", static_cast<int>(ci));
        chip->setProperty("shownTake", shown);
        chip->setProperty("takeLabel", QString("%1  %2:%3").arg(takeNumber)
            .arg(secs / 60).arg(secs % 60, 2, 10, QLatin1Char('0')));
        styleTakeChip(chip, chosen, shown);

        QPointer<QPushButton> chipGuard(chip);
        const double posterT = clip.start + 0.35 * std::max(0.0, clip.end - clip.start);
        frameGrabber()->requestStill(chip, clip.sourceFile, posterT, thumbSize,
                                     [chipGuard](const QImage& image) {
                                         if (chipGuard) chipGuard->setIcon(QIcon(QPixmap::fromImage(image)));
                                     });

        const int clipId = clip.id;
        connect(chip, &QPushButton::clicked, this, [this, lineNumber, clipId]() {
            m_displayedTakeId[lineNumber] = clipId;
            if (m_playingClipIdx >= 0) stopPlayback();
            // Only this line's card changes. Deferred: the rebuild deletes this chip.
            scheduleCardRebuild({lineNumber});
        });
        row->addWidget(chip);
    }
    row->addStretch();
    return strip;
}

bool AudioSync::isShotScript() const
{
    return m_script && m_script->isShotScript();
}

QString AudioSync::activeScriptName() const
{
    auto it = m_scriptSessions.find(m_activeScriptKey);
    return it != m_scriptSessions.end() ? QString::fromStdString(it->second.displayName) : QString();
}

std::vector<ShowShot> AudioSync::showShots() const
{
    std::vector<ShowShot> out;
    if (!isShotScript()) return out;
    out.reserve(m_script->lines.size());
    for (const auto& line : m_script->lines) {
        ShowShot s;
        s.lineNumber = line.lineNumber;
        s.dialogue   = line.dialogue;
        s.action     = line.action;
        s.segment    = line.segment;
        s.shot       = effectiveShot(line);
        s.key        = ScriptMatcher::normalize(line.dialogue);
        for (const auto& clip : m_clips) {
            if (clip.scriptLineNumber == line.lineNumber && clip.matchState == 2) {
                s.hasTake = true;
                s.take = clip;
                break;
            }
        }
        out.push_back(std::move(s));
    }
    return out;
}

std::string AudioSync::effectiveShot(const ScriptLine& line) const
{
    if (!line.shot.empty()) return line.shot;
    auto it = m_shotOverrides.find(ScriptMatcher::normalize(line.dialogue));
    return it != m_shotOverrides.end() ? it->second : std::string{};
}

void AudioSync::setShotOverride(const ScriptLine& line, const std::string& shot)
{
    const std::string key = ScriptMatcher::normalize(line.dialogue);
    auto it = m_shotOverrides.find(key);
    const std::string before = (it != m_shotOverrides.end()) ? it->second : std::string{};
    if (before == shot) return;

    const std::string sessionKey = m_activeScriptKey;
    auto apply = [this, key, sessionKey](const std::string& value) {
        if (m_destroying.load(std::memory_order_acquire)) return;
        const bool active = (sessionKey == m_activeScriptKey);
        auto sit = m_scriptSessions.find(sessionKey);
        if (!active && sit == m_scriptSessions.end()) return;
        auto& overrides = active ? m_shotOverrides : sit->second.shotOverrides;
        if (value.empty()) overrides.erase(key);
        else               overrides[key] = value;
        if (active) populateScriptList();
    };
    apply(shot);
    if (m_commandStack) {
        m_commandStack->pushWithoutExecute(std::make_unique<LambdaCommand>(
            "Set Shot",
            [apply, shot]() { apply(shot); },
            [apply, before]() { apply(before); }));
    }
}

QWidget* AudioSync::createShotTag(const ScriptLine& line, int width)
{
    const auto& tc = Theme::colors();
    const std::string shot = effectiveShot(line);

    if (!line.shot.empty()) {
        auto* label = new QLabel(QString::fromStdString(shot));
        label->setFixedWidth(width);
        label->setAlignment(Qt::AlignCenter);
        label->setStyleSheet(
            QString("QLabel { color: %1; font-size: 13px; font-weight: bold; "
                    "background: %2; border: none; padding: 4px 4px; }")
                .arg(characterColor(shot).lighter(130).name(), Theme::hex(tc.surface2)));
        return label;
    }

    // Blank SHOT cell in the doc: red until picked, then still clickable.
    const bool missing = shot.empty();
    auto* btn = new QPushButton(missing ? QStringLiteral("SET SHOT") : QString::fromStdString(shot));
    btn->setFixedWidth(width);
    btn->setCursor(Qt::PointingHandCursor);
    btn->setToolTip(missing
        ? QStringLiteral("SHOT cell is blank in the script. Click to pick the shot.")
        : QStringLiteral("SHOT cell is blank in the script; picked here. Click to change."));
    btn->setStyleSheet(missing
        ? QString("QPushButton { color: %1; background: %2; font-size: 12px; font-weight: bold; "
                  "border: none; border-radius: %3px; padding: 4px 2px; }"
                  "QPushButton:hover { background: %4; }")
              .arg(Theme::hex(tc.textBright), Theme::hex(tc.error))
              .arg(Theme::metrics().radiusSm)
              .arg(Theme::hex(tc.error.lighter(120)))
        : QString("QPushButton { color: %1; background: %2; font-size: 13px; font-weight: bold; "
                  "border: 1px dashed %3; border-radius: %4px; padding: 3px 2px; }"
                  "QPushButton:hover { background: %5; }")
              .arg(characterColor(shot).lighter(130).name(), Theme::hex(tc.surface2),
                   Theme::hex(tc.border))
              .arg(Theme::metrics().radiusSm)
              .arg(Theme::hex(tc.surface3)));

    connect(btn, &QPushButton::clicked, this, [this, btn, line, shot]() {
        const QPoint menuPos = btn->mapToGlobal(QPoint(0, btn->height()));
        QMenu menu(this);  // not btn: a rebuild while open would delete it
        for (const char* option : {"WIDE", "CLOSE-UP"}) {
            QAction* act = menu.addAction(QString::fromLatin1(option));
            act->setCheckable(true);
            act->setChecked(shot == option);
            act->setData(QString::fromLatin1(option));
        }
        QAction* chosen = menu.exec(menuPos);
        if (!chosen) return;
        const std::string picked = chosen->data().toString().toStdString();
        // Deferred: the rebuild deletes this button, which is still in its
        // own clicked() handler here.
        QTimer::singleShot(0, this, [this, line, picked]() { setShotOverride(line, picked); });
    });
    return btn;
}

} // namespace rt
