/*
 * AudioSyncPopulateCards.cpp - Right-pane waveform card population.
 * Extracted from AudioSyncCards.cpp for modularity.
 */
#include "panels/audio/AudioSync.h"
#include "panels/audio/VideoFrameGrabber.h"
#include "PathUtils.h"
#include "ai/ScriptMatcher.h"
#include "command/CommandStack.h"
#include "command/LambdaCommand.h"
#include "audio/AudioEngine.h"
#include "widgets/MiniWaveformWidget.h"
#include "widgets/VideoFrameView.h"
#include "widgets/ScriptTextCopy.h"
#include "Theme.h"

#include <QApplication>
#include <QCheckBox>
#include <QElapsedTimer>
#include <QComboBox>
#include <QFrame>
#include <QGraphicsDropShadowEffect>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPixmap>
#include <QPointer>
#include <QPushButton>
#include <QScrollBar>
#include <QTimer>
#include <QVBoxLayout>

#include <spdlog/spdlog.h>
#include <algorithm>
#include <functional>
#include <memory>

namespace rt {

namespace {
// QComboBox that defers filling its (potentially huge) item list until the
// user actually opens the dropdown. Pre-filling every "Reassign…" combo with
// all script lines was O(N²) for N lines — the dominant cost when opening the
// Match tab on large projects. The list is built once, on first popup.
class LazyComboBox : public QComboBox {
public:
    using QComboBox::QComboBox;
    std::function<void()> firstPopup;
    /// Keep firstPopup and re-run it on EVERY popup (lists that must reflect
    /// the current clips, since cards are no longer all rebuilt together).
    bool refillEveryPopup{false};
    void showPopup() override {
        if (firstPopup) {
            if (refillEveryPopup) {
                auto fn = firstPopup;
                fn();
            } else {
                auto fn = std::move(firstPopup);
                firstPopup = nullptr;
                fn();
            }
        }
        QComboBox::showPopup();
    }
};
} // namespace

// Per-rebuild data shared by every card.
struct AudioSync::CardBuildContext {
    struct ScriptLineDisplay {
        int displayNum;
        int lineNumber;
        std::string character;
        std::string dialogue;
    };

    /// scriptLineNumber → clip indices
    std::unordered_map<int, std::vector<size_t>> lineToClips;
    /// Display-index → script line mapping for the Reassign dropdowns.
    /// Shared so each card's combo and its slot reference one copy instead
    /// of copying the whole vector per card (was O(N²) memory).
    std::shared_ptr<std::vector<ScriptLineDisplay>> displayLines;
};

AudioSync::CardBuildContext AudioSync::makeCardBuildContext() const
{
    CardBuildContext ctx;
    for (size_t i = 0; i < m_clips.size(); ++i) {
        if (m_clips[i].scriptLineNumber > 0)
            ctx.lineToClips[m_clips[i].scriptLineNumber].push_back(i);
    }
    ctx.displayLines = std::make_shared<std::vector<CardBuildContext::ScriptLineDisplay>>();
    if (m_script) {
        int idx = 0;
        for (const auto& line : m_script->lines) {
            ++idx;
            ctx.displayLines->push_back({idx, line.lineNumber, line.character, line.dialogue});
        }
    }
    return ctx;
}

void AudioSync::populateCards()
{
    // Save scroll position to restore after rebuild
    int savedScrollPos = 0;
    if (m_rightScrollArea && m_rightScrollArea->verticalScrollBar())
        savedScrollPos = m_rightScrollArea->verticalScrollBar()->value();

    // Take focus off any card widget before tearing down. Otherwise Qt
    // re-targets focus to whichever button survives — typically a button on
    // the LAST card added — and QScrollArea auto-scrolls to make that
    // newly-focused widget visible, slamming the view to the bottom right
    // after a manual match.
    if (auto* fw = QApplication::focusWidget()) {
        if (m_rightScrollContent && m_rightScrollContent->isAncestorOf(fw))
            m_rightScrollArea->setFocus(Qt::OtherFocusReason);
    }
    // Suppress scroll-bar value-change side effects (syncLeftListFromScroll)
    // while the layout churns through clear → repopulate.
    QScrollBar* rightSB = m_rightScrollArea ? m_rightScrollArea->verticalScrollBar() : nullptr;
    if (rightSB) rightSB->blockSignals(true);

    // A full rebuild replaces any build still streaming in.
    cancelCardBuild();

    // ── Card widget pool (Phase 5.A) ──────────────────────────────────
    // Hide existing cards for reuse instead of destroy+recreate.
    m_cardPool.releaseAll();
    m_cardWaveforms.clear();
    m_cardWidgets.clear();
    m_cardClipIndices.clear();
    m_takePreviewLabels.clear();
    setHighlightedCard(nullptr);
    m_selectedLeftCard = nullptr;

    // Empty the layout. Pooled cards stay parented (hidden): detaching and
    // re-attaching them re-ran the whole style-sheet cascade on every card
    // twice per rebuild. Anything else (the empty-state label) is deleted.
    if (m_rightLayout) {
        while (m_rightLayout->count() > 0) {
            auto* item = m_rightLayout->takeAt(0);
            if (auto* w = item->widget()) {
                if (std::find(m_cardPool.pool.begin(), m_cardPool.pool.end(), w)
                        == m_cardPool.pool.end())
                    w->deleteLater();
            }
            delete item;
        }
    }

    // Suppress repaints during bulk widget creation
    if (m_rightScrollContent)
        m_rightScrollContent->setUpdatesEnabled(false);

    if (!m_script) {
        // No script loaded — show placeholder
        if (m_rightLayout) {
            auto* placeholder = new QLabel("Load a script and import audio to begin.");
            placeholder->setAlignment(Qt::AlignCenter);
            placeholder->setStyleSheet(
                QString("QLabel { color: %1; font-size: 13px; padding: 40px; border: none; }").arg(Theme::hex(Theme::colors().textTertiary)));
            m_rightLayout->addWidget(placeholder);
            m_rightLayout->addStretch();
        }
        populateLeftList();
        updateSmartBar();
        if (rightSB) rightSB->blockSignals(false);
        return;
    }

    // Character filter from MATCH side panel list
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

    const CardBuildContext ctx = makeCardBuildContext();
    const auto& lineToClips = ctx.lineToClips;

    // Rows: the same filtered lines, in the same order, as the left list
    // (populateLeftList), so row i is left-list row i.
    m_cardBuild.script = m_script.get();
    for (size_t li = 0; li < m_script->lines.size(); ++li) {
        const auto& line = m_script->lines[li];
        // Apply character filter
        if (!filterChar.isEmpty() &&
            QString::fromStdString(line.character) != filterChar)
            continue;
        // Apply unmatched filter — show only lines without a matched clip
        if (filterUnmatched && lineToClips.count(line.lineNumber) > 0)
            continue;
        m_cardBuild.lineIndices.push_back(li);
    }
    const size_t rows = m_cardBuild.lineIndices.size();
    m_cardWidgets.assign(rows, nullptr);
    m_cardWaveforms.assign(rows, nullptr);
    m_cardClipIndices.assign(rows, -1);
    m_rightLayout->addStretch();

    // Build what the restored scroll position shows, plus a screen of
    // margin, right now; the rest streams in short slices so a long script
    // doesn't freeze the UI (5-7 s after Auto-Sync). Rows build top-down,
    // so later ones only extend the page below what is on screen.
    const QWidget* viewport = m_rightScrollArea ? m_rightScrollArea->viewport() : nullptr;
    const int viewportW = viewport ? viewport->width() : 0;
    const int viewportH = viewport ? viewport->height() : 0;
    const QMargins pageMargins = m_rightLayout->contentsMargins();
    const int cardW = std::max(0, viewportW - pageMargins.left() - pageMargins.right());
    const int wanted = savedScrollPos + 2 * std::max(viewportH, 600);
    const int spacing = std::max(0, m_rightLayout->spacing());
    int builtHeight = 0;
    while (m_cardBuild.next < rows && builtHeight < wanted) {
        const size_t row = m_cardBuild.next++;
        buildCardRow(row, ctx);
        if (const QWidget* card = m_cardWidgets[row]) {
            builtHeight += spacing + ((cardW > 0 && card->hasHeightForWidth())
                ? card->heightForWidth(cardW) : card->sizeHint().height());
        }
    }
    m_cardBuild.active = m_cardBuild.next < rows;
    m_cardBuild.width = cardW;   // the width the layout lays cards out at
    m_cardBuild.builtHeight = builtHeight;
    if (m_cardBuild.active) {
        reserveCardPageHeight();
        m_cardBuildTimer->start();
    } else {
        m_cardPool.shrink(m_cardPool.activeCount + 16);   // Phase 5.A headroom
    }

    // A resizable QScrollArea with height-for-width content and an
    // as-needed bar re-measures the whole page at up to 16 one-pixel-
    // narrower widths on every layout pass (its scrollbar anti-flicker
    // search), so every card's word-wrapped text was re-laid out ~16 times
    // per streamed slice and per card refresh. A page that did not fit two
    // screens shows the bar anyway: pin it on (same look), skipping that.
    if (m_rightScrollArea)
        m_rightScrollArea->setVerticalScrollBarPolicy(
            m_cardBuild.active ? Qt::ScrollBarAlwaysOn : Qt::ScrollBarAsNeeded);

    // Re-enable repaints after bulk creation
    if (m_rightScrollContent)
        m_rightScrollContent->setUpdatesEnabled(true);

    // Also update left list to stay in sync
    populateLeftList();
    updateSmartBar();
    updateWorkflowState();

    // Restore scroll position after rebuild. Two passes: one immediate
    // (queued for after the current event-loop iteration so layout settles)
    // and one slightly delayed to overwrite any focus-driven auto-scroll
    // that QScrollArea performs when widgets are re-parented in.
    auto restoreScroll = [this, savedScrollPos]() {
        if (!m_rightScrollArea) return;
        auto* sb = m_rightScrollArea->verticalScrollBar();
        if (!sb) return;
        sb->blockSignals(true);
        sb->setValue(std::min(savedScrollPos, sb->maximum()));
        sb->blockSignals(false);
    };
    if (rightSB) rightSB->blockSignals(false);
    if (m_rightScrollArea && m_rightScrollArea->verticalScrollBar() && savedScrollPos > 0) {
        QTimer::singleShot(0,  this, restoreScroll);
        QTimer::singleShot(30, this, restoreScroll);
    }
}

void AudioSync::buildCardRow(size_t row, const CardBuildContext& ctx)
{
    const ScriptLine& line = m_script->lines[m_cardBuild.lineIndices[row]];
    auto* card = m_cardPool.acquire();
    if (!card) {
        // Parented (hidden) up front, so its content is built and polished
        // once under its final ancestors instead of again on insertion.
        card = new QFrame(m_rightScrollContent);
        card->hide();
        m_cardPool.adopt(card);
    }
    MiniWaveformWidget* waveform = nullptr;
    int cardClipIdx = -1;
    fillCard(card, line, static_cast<int>(row) + 1, ctx, waveform, cardClipIdx);

    // Store mapping
    m_cardWidgets[row]     = card;
    m_cardWaveforms[row]   = waveform;
    m_cardClipIndices[row] = cardClipIdx;

    // Rows build top-down, so `row` cards precede this one in the layout
    // (the trailing stretch stays last).
    m_rightLayout->insertWidget(static_cast<int>(row), card);

    // Install event filter on card and focusable children for keyboard routing + click
    card->installEventFilter(this);
    for (auto* child : card->findChildren<QWidget*>()) {
        if (qobject_cast<QLineEdit*>(child) || qobject_cast<QComboBox*>(child))
            continue;
        child->installEventFilter(this);
    }

    // Show the card (pooled cards are hidden after releaseAll)
    card->setVisible(true);
}

void AudioSync::buildPendingCardRows(int budgetMs)
{
    if (!m_cardBuild.active || m_destroying.load(std::memory_order_acquire)) return;
    // The script the rows index into was replaced or stashed (session
    // switch): this build is stale.
    if (!m_script || m_script.get() != m_cardBuild.script || !m_rightLayout) {
        cancelCardBuild();
        return;
    }

    // Fresh per slice: clips may have been matched/reassigned since the last.
    const CardBuildContext ctx = makeCardBuildContext();
    const size_t rows = m_cardBuild.lineIndices.size();
    // No setUpdatesEnabled() toggling here: re-enabling walks and repaints
    // every existing card, which made the streamed build quadratic. New
    // rows land below what is on screen.
    QElapsedTimer slice;
    slice.start();
    const int spacing = std::max(0, m_rightLayout->spacing());
    do {
        const size_t row = m_cardBuild.next++;
        if (m_cardBuild.lineIndices[row] >= m_script->lines.size()) continue;
        buildCardRow(row, ctx);
        if (const QWidget* card = m_cardWidgets[row]) {
            m_cardBuild.builtHeight += spacing +
                ((m_cardBuild.width > 0 && card->hasHeightForWidth())
                    ? card->heightForWidth(m_cardBuild.width) : card->sizeHint().height());
        }
    } while (m_cardBuild.next < rows && (budgetMs < 0 || slice.elapsed() < budgetMs));

    if (m_cardBuild.next < rows) reserveCardPageHeight();
    if (m_cardBuild.next >= rows) {
        m_cardBuild.active = false;
        m_cardPool.shrink(m_cardPool.activeCount + 16);   // Phase 5.A headroom
        // Built: the page takes its real height (one final layout pass).
        if (m_cardBuild.reservedHeight > 0) {
            setTrailingCardStretch(0);
            if (m_rightScrollContent) m_rightScrollContent->setMinimumHeight(0);
            m_cardBuild.reservedHeight = 0;
        }
    }

    // A left-list click on a card that wasn't built yet scrolls there now.
    if (const int line = m_cardBuild.pendingScrollLine; line >= 0) {
        for (size_t i = 0; i < m_cardScriptLineNums.size() && i < m_cardWidgets.size(); ++i) {
            if (m_cardScriptLineNums[i] == line && m_cardWidgets[i]) {
                scrollToCard(line);
                break;
            }
        }
    }
}

void AudioSync::continueCardBuild()
{
    // Each slice is followed by Qt's own pass over the page (layout, paint),
    // which grows with the card count. Building for about as long as that
    // pass took keeps building >= half the time, and the UI never stalls
    // for more than ~2 x 40 ms.
    const qint64 sinceLast = m_cardBuild.sinceSlice.isValid()
        ? m_cardBuild.sinceSlice.elapsed() : 0;
    const int budgetMs = static_cast<int>(std::clamp<qint64>(sinceLast, 12, 40));
    // Left rows are cheap; they share the slice with the cards.
    QElapsedTimer slice;
    slice.start();
    if (m_leftBuild.active)
        buildPendingLeftRows(m_cardBuild.active ? budgetMs / 3 : budgetMs);
    if (m_cardBuild.active)
        buildPendingCardRows(std::max(1, budgetMs - static_cast<int>(slice.elapsed())));
    if (m_cardBuild.active || m_leftBuild.active) {
        m_cardBuild.sinceSlice.start();
        m_cardBuildTimer->start();
    }
}

void AudioSync::finishCardBuild()
{
    if (m_cardBuildTimer) m_cardBuildTimer->stop();
    buildPendingLeftRows(-1);
    buildPendingCardRows(-1);
}

void AudioSync::reserveCardPageHeight()
{
    // While cards stream in, a page shorter than its cards makes every
    // layout pass squeeze all of them and the scroll area then stretch them
    // back: each card (and its word-wrapped text) re-laid out twice per
    // slice, quadratic over the build. Reserve the projected final height
    // instead; the trailing stretch takes the slack until the last slice.
    if (!m_rightScrollContent || !m_rightLayout || m_cardBuild.next == 0) return;
    const size_t rows = m_cardBuild.lineIndices.size();
    const int perCard = m_cardBuild.builtHeight / static_cast<int>(m_cardBuild.next);
    const QMargins margins = m_rightLayout->contentsMargins();
    const int projected = margins.top() + margins.bottom() + m_cardBuild.builtHeight +
        perCard * static_cast<int>(rows - m_cardBuild.next);
    if (projected <= m_cardBuild.reservedHeight) return;
    m_cardBuild.reservedHeight = projected + projected / 8;   // headroom: few re-reserves
    // Cards expand vertically (their control column ends in a stretch), so
    // they would share the slack and all resize as it shrinks. Give it all
    // to the trailing stretch while building.
    setTrailingCardStretch(1);
    m_rightScrollContent->setMinimumHeight(m_cardBuild.reservedHeight);
}

void AudioSync::setTrailingCardStretch(int stretch)
{
    if (!m_rightLayout || m_rightLayout->count() == 0) return;
    const int last = m_rightLayout->count() - 1;
    if (m_rightLayout->itemAt(last)->spacerItem() && m_rightLayout->stretch(last) != stretch)
        m_rightLayout->setStretch(last, stretch);
}

void AudioSync::cancelCardBuild()
{
    if (m_cardBuildTimer) m_cardBuildTimer->stop();
    if (m_cardBuild.reservedHeight > 0) {
        setTrailingCardStretch(0);
        if (m_rightScrollContent) m_rightScrollContent->setMinimumHeight(0);
    }
    m_cardBuild = PendingCardBuild{};
    m_leftBuild = PendingLeftBuild{};
}

int AudioSync::builtCardCount() const noexcept
{
    return static_cast<int>(std::count_if(m_cardWidgets.begin(), m_cardWidgets.end(),
                                          [](const QPointer<QWidget>& w) { return !w.isNull(); }));
}

void AudioSync::setClipsForTesting(std::vector<SyncClip> clips)
{
    m_clips = std::move(clips);
    populateCards();
}

// Builds (or rebuilds in place) one script-line card.  Shared by
// populateCards() and rebuildCardsForLines(); `card` may still hold content
// from its previous use.
void AudioSync::fillCard(QFrame* card, const ScriptLine& line, int cardIdx,
                         const CardBuildContext& ctx,
                         MiniWaveformWidget*& waveformOut, int& clipIdxOut)
{
    const auto& lineToClips  = ctx.lineToClips;
    const auto& displayLines = ctx.displayLines;
    const auto& _tc = Theme::colors();
    const auto _radM = QString::number(Theme::metrics().radiusMd);
    const auto _rad  = QString::number(Theme::metrics().radiusSm);
    card->setProperty("cardIdx", cardIdx);

    // Find matched clip(s) for this line
    auto clipIt = lineToClips.find(line.lineNumber);
    bool hasClip = (clipIt != lineToClips.end() && !clipIt->second.empty());
    size_t primaryClipIdx = SIZE_MAX;
    int matchState = 0;
    float confidence = 0.0f;
    if (hasClip) {
        // Pick the clip with the highest match state (confirmed > tentative > unmatched)
        // so the card visual matches the aggregate counters which count a line as
        // confirmed if ANY clip for that line is confirmed.
        for (size_t ci : clipIt->second) {
            const auto& c = m_clips[ci];
            if (c.matchState > matchState ||
                (c.matchState == matchState && c.confidence > confidence)) {
                primaryClipIdx = ci;
                matchState = c.matchState;
                confidence = c.confidence;
            }
        }
        // A take the user clicked to view is the one the card shows.
        auto shown = m_displayedTakeId.find(line.lineNumber);
        if (shown != m_displayedTakeId.end()) {
            for (size_t ci : clipIt->second) {
                if (m_clips[ci].id == shown->second) { primaryClipIdx = ci; break; }
            }
        }
    }

    // ── Card frame ─────────────────────────────────────────────────
    QString borderColor, bgColor;
    switch (matchState) {
    case 2:  bgColor = Theme::hex(_tc.successBg); borderColor = Theme::hex(_tc.success); break;
    case 1:  bgColor = Theme::hex(_tc.warningBg); borderColor = Theme::hex(_tc.warning); break;
    // Unmatched: red, to match the left script-line list (errorBg/error).
    default: bgColor = Theme::hex(_tc.errorBg); borderColor = Theme::hex(_tc.error); break;
    }

    QString hoverBg, hoverBorder;
    switch (matchState) {
    case 2:  hoverBg = Theme::hex(_tc.successBg.lighter(120)); hoverBorder = Theme::hex(_tc.success.lighter(120)); break;
    case 1:  hoverBg = Theme::hex(_tc.warningBg.lighter(120)); hoverBorder = Theme::hex(_tc.warning.lighter(120)); break;
    default: hoverBg = Theme::hex(_tc.surface3); hoverBorder = Theme::hex(_tc.border); break;
    }

    // Reused card (pool, or an in-place single-card rebuild): clear the
    // old content.  Qt will NOT replace a non-empty layout on an existing
    // widget (setLayout() returns early with a warning), so the new
    // QHBoxLayout created below would be orphaned and the card would keep
    // showing stale content.  Only layout items are deleted: the
    // highlight overlay (not in the layout) survives.
    if (QLayout* oldLayout = card->layout()) {
        while (auto* item = oldLayout->takeAt(0)) {
            if (auto* w = item->widget()) {
                w->setParent(nullptr);
                w->disconnect();  // prevent queued signals from firing on freed widget
                delete w;
            }
            delete item;
        }
        delete oldLayout;  // removes layout from card
    }
    card->setObjectName(QString("scriptCard_%1").arg(line.lineNumber));
    card->setCursor(Qt::PointingHandCursor);
    card->setStyleSheet(
        QString("QFrame#scriptCard_%3 { background: %1; border: 1px solid %2; "
                "border-radius: %6px; margin-bottom: 2px; }"
                "QFrame#scriptCard_%3:hover { background: %4; border: 1px solid %5; "
                "border-radius: %6px; margin-bottom: 2px; }")
            .arg(bgColor, borderColor).arg(line.lineNumber)
            .arg(hoverBg, hoverBorder).arg(_radM));

    // ── Outer: left (script + waveform) | separator | right (controls) ──
    auto* cardOuterLayout = new QHBoxLayout(card);
    cardOuterLayout->setContentsMargins(14, 10, 10, 10);
    cardOuterLayout->setSpacing(0);

    auto* leftWidget = new QWidget;
    leftWidget->setStyleSheet("background: transparent; border: none;");
    auto* leftLayout = new QVBoxLayout(leftWidget);
    leftLayout->setContentsMargins(0, 0, 8, 0);
    leftLayout->setSpacing(6);
    cardOuterLayout->addWidget(leftWidget, 1);

    auto* sepLine = new QFrame;
    sepLine->setFrameShape(QFrame::VLine);
    sepLine->setStyleSheet(QString("QFrame { color: %1; }").arg(Theme::hex(_tc.border)));
    sepLine->setFixedWidth(1);
    cardOuterLayout->addWidget(sepLine);

    auto* rightWidget = new QWidget;
    rightWidget->setFixedWidth(130);
    rightWidget->setStyleSheet("background: transparent; border: none;");
    auto* rightLayout = new QVBoxLayout(rightWidget);
    rightLayout->setContentsMargins(8, 0, 0, 0);
    rightLayout->setSpacing(4);
    cardOuterLayout->addWidget(rightWidget);

    // ── Header (left panel): # | Character | Script Text ──────
    auto* headerRow = new QHBoxLayout;
    headerRow->setSpacing(8);

    auto* numLabel = new QLabel(QString("#%1").arg(cardIdx));
    numLabel->setFixedSize(36, 24);
    numLabel->setAlignment(Qt::AlignCenter);
    QString badgeColor = (matchState == 2) ? Theme::hex(_tc.successBtnBg)
                       : (matchState == 1) ? Theme::hex(_tc.warning) : Theme::hex(_tc.error);
    numLabel->setStyleSheet(
        QString("QLabel { color: %2; font-weight: bold; font-size: %4px; "
                "background: %1; border-radius: %3px; border: none; }").arg(badgeColor, Theme::hex(_tc.textBright), _rad,
                    QString::number(Theme::typography().sizeXs)));
    headerRow->addWidget(numLabel);

    if (m_script->isShotScript()) {
        // One announcer, so the column shows the shot instead.
        headerRow->addWidget(createShotTag(line, 90));
    } else {
        QColor charColor = characterColor(line.character);
        auto* charLabel = new QLabel(QString::fromStdString(line.character));
        charLabel->setFixedWidth(90);
        charLabel->setAlignment(Qt::AlignCenter);
        charLabel->setStyleSheet(
            QString("QLabel { color: %1; font-weight: bold; font-size: 13px; border: none; }")
                .arg(charColor.name()));
        headerRow->addWidget(charLabel);
    }

    auto* dialogueLabel = new QLabel(QString::fromStdString(line.dialogue));
    dialogueLabel->setWordWrap(true);
    enableScriptTextCopy(dialogueLabel, dialogueLabel->text());
    addGenerateVoiceAction(dialogueLabel, line.lineNumber,
                           QString::fromStdString(line.character),
                           dialogueLabel->text(),
                           QString::fromStdString(line.segment));
    dialogueLabel->setStyleSheet(
        QString("QLabel { color: %1; font-size: 13px; border: none; }").arg(Theme::hex(_tc.textPrimary)));
    dialogueLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    headerRow->addWidget(dialogueLabel, 1);
    leftLayout->addLayout(headerRow);

    if (!line.action.empty()) {
        auto* actionLabel = new QLabel(QString::fromStdString(line.action));
        actionLabel->setWordWrap(true);
        actionLabel->setStyleSheet(
            QString("QLabel { color: %1; font-size: %2px; font-style: italic; border: none; "
                    "padding-left: 142px; }")
                .arg(Theme::hex(_tc.textSecondary))
                .arg(Theme::typography().sizeSmall));
        leftLayout->addWidget(actionLabel);
    }

    // ── Status icon (right panel top) ──────────────────────────
    auto* statusIcon = new QLabel;
    statusIcon->setFixedHeight(20);
    statusIcon->setAlignment(Qt::AlignCenter);
    switch (matchState) {
    case 2:
        statusIcon->setText(QStringLiteral("\u2713"));
        statusIcon->setStyleSheet(QString("QLabel { color: %1; font-weight: bold; font-size: %2px; border: none; }").arg(Theme::hex(_tc.success)).arg(Theme::typography().sizeCaption));
        break;
    case 1:
        statusIcon->setText("?");
        statusIcon->setStyleSheet(QString("QLabel { color: %1; font-weight: bold; font-size: %2px; border: none; }").arg(Theme::hex(_tc.warning)).arg(Theme::typography().sizeCaption));
        break;
    default:
        statusIcon->setText(QStringLiteral("\u25CB"));
        statusIcon->setStyleSheet(QString("QLabel { color: %1; font-size: %2px; border: none; }").arg(Theme::hex(_tc.textDisabled)).arg(Theme::typography().sizeCaption));
        break;
    }
    rightLayout->addWidget(statusIcon);

    // ── Audio content ──────────────────────────────────────────────
    MiniWaveformWidget* waveform = nullptr;

    if (hasClip) {
        const auto& clip = m_clips[primaryClipIdx];

        // Confidence bar
        if (confidence > 0.001f) {
            auto* confBar = new QProgressBar;
            confBar->setFixedHeight(4);
            confBar->setRange(0, 100);
            confBar->setValue(static_cast<int>(confidence * 100.0f));
            confBar->setTextVisible(false);
            QString barColor = (confidence >= 0.8f) ? Theme::hex(_tc.success)
                             : (confidence >= 0.5f) ? Theme::hex(_tc.warning)
                                                     : Theme::hex(_tc.error);
            confBar->setStyleSheet(
                QString("QProgressBar { background: %2; border: none; border-radius: 2px; }"
                        "QProgressBar::chunk { background: %1; border-radius: 2px; }")
                    .arg(barColor, Theme::hex(_tc.inputBg)));
            confBar->setToolTip(QString("Match confidence: %1%")
                .arg(static_cast<int>(confidence * 100.0f)));
            leftLayout->addWidget(confBar);
        }

        // Waveform
        waveform = new MiniWaveformWidget;
        auto sampleIt = m_audioSamples.find(clip.sourceFile);
        if (sampleIt != m_audioSamples.end()) {
            waveform->setAudioShared(sampleIt->second.samples, sampleIt->second.sampleRate,
                               clip.start, clip.end);
        }

        // Connect waveform signals
        size_t clipIdx = primaryClipIdx;
        connect(waveform, &MiniWaveformWidget::trimChanged,
                this, [this, clipIdx, waveform](double inPt, double outPt) {
            if (clipIdx < m_clips.size()) {
                // Capture old state on first drag move
                if (!m_trimDebounceTimer->isActive()) {
                    m_preTrimStart = m_clips[clipIdx].start;
                    m_preTrimEnd   = m_clips[clipIdx].end;
                    m_preTrimMatchState = m_clips[clipIdx].matchState;
                    m_trimDebounceClipIdx = clipIdx;
                }
                m_clips[clipIdx].start = inPt;
                m_clips[clipIdx].end   = outPt;
                if (m_clips[clipIdx].matchState == 2)
                    m_clips[clipIdx].matchState = 1;
                if (waveform->isPlayheadVisible()) {
                    double ph = waveform->playhead();
                    if (ph < inPt) waveform->setPlayhead(inPt);
                    else if (ph >= outPt) waveform->setPlayhead(outPt - 0.001);
                }
                m_trimDebounceTimer->start();
                // Rebuild playback buffer so trim changes are heard immediately
                if (m_playingClipIdx == static_cast<int>(clipIdx))
                    playClip(clipIdx);
            }
        });
        connect(waveform, &MiniWaveformWidget::seekRequested,
                this, [this, clipIdx, waveform](double t) {
            if (clipIdx < m_clips.size())
                t = std::clamp(t, m_clips[clipIdx].start, m_clips[clipIdx].end - 0.001);
            waveform->setPlayhead(t);
            waveform->setPlayheadVisible(true);
            m_selectedClipIdx = static_cast<int>(clipIdx);
            if (m_playingClipIdx == static_cast<int>(clipIdx))
                seekPlayingClip(t);
            else
                scrubClipAt(clipIdx, t);
        });
        connect(waveform, &MiniWaveformWidget::playToggleRequested,
                this, [this, clipIdx]() {
            m_selectedClipIdx = static_cast<int>(clipIdx);
            togglePlayClip(clipIdx);
        });
        connect(waveform, &MiniWaveformWidget::shuttleSpeedChanged,
                this, [this, clipIdx](double speed) {
            if (clipIdx >= m_clips.size()) {
                spdlog::warn("AudioSync shuttle: clipIdx {} >= clips.size() {}", clipIdx, m_clips.size());
                return;
            }
            if (!m_audioEngine) {
                spdlog::warn("AudioSync shuttle: no audio engine");
                return;
            }
            spdlog::info("AudioSync shuttle: clip={} speed={:.2f} playing={} engineState={}",
                         clipIdx, speed, m_playingClipIdx,
                         static_cast<int>(m_audioEngine->transportState()));
            m_selectedClipIdx = static_cast<int>(clipIdx);
            if (speed == 0.0) {
                pausePlayback();
            } else {
                if (m_playingClipIdx != static_cast<int>(clipIdx)) {
                    playClip(clipIdx);
                    // playClip may fail silently (e.g. no audio data).
                    // If it didn't start, don't try to shuttle.
                    if (m_playingClipIdx != static_cast<int>(clipIdx)) {
                        spdlog::warn("AudioSync shuttle: playClip failed for clip {}", clipIdx);
                        return;
                    }
                }
                m_audioEngine->setPlaybackSpeed(speed);
                if (m_audioEngine->transportState() != TransportState::Playing)
                    m_audioEngine->play();
                if (m_playheadTimer) m_playheadTimer->start();
            }
        });
        connect(waveform, &MiniWaveformWidget::inPointSet,
                this, [this, clipIdx, waveform](double t) {
            if (clipIdx < m_clips.size()) {
                // trimChanged already fired from the same I-key press and
                // modified start/end/matchState.  Use saved pre-trim state.
                double oldStart;
                int oldMS;
                if (m_trimDebounceTimer->isActive() && m_trimDebounceClipIdx == clipIdx) {
                    m_trimDebounceTimer->stop();
                    oldStart = m_preTrimStart;
                    oldMS    = m_preTrimMatchState;
                } else {
                    oldStart = m_clips[clipIdx].start;
                    oldMS    = m_clips[clipIdx].matchState;
                    m_clips[clipIdx].start = t;
                    if (m_clips[clipIdx].matchState == 2)
                        m_clips[clipIdx].matchState = 1;
                }
                int newMS = m_clips[clipIdx].matchState;
                if (m_commandStack) {
                    m_commandStack->pushWithoutExecute(std::make_unique<LambdaCommand>(
                        "Set in point",
                        [this, clipIdx, t, newMS]() {
                            if (clipIdx < m_clips.size()) {
                                m_clips[clipIdx].start = t;
                                m_clips[clipIdx].matchState = newMS;
                                if (auto* wf = waveformForClip(static_cast<int>(clipIdx)))
                                    wf->setTrimRange(t, m_clips[clipIdx].end);
                                refreshLeftListStates();
                                updateCardMatchStyle(clipIdx);
                                updateSmartBar();
                            }
                        },
                        [this, clipIdx, oldStart, oldMS]() {
                            if (clipIdx < m_clips.size()) {
                                m_clips[clipIdx].start = oldStart;
                                m_clips[clipIdx].matchState = oldMS;
                                if (auto* wf = waveformForClip(static_cast<int>(clipIdx)))
                                    wf->setTrimRange(oldStart, m_clips[clipIdx].end);
                                refreshLeftListStates();
                                updateCardMatchStyle(clipIdx);
                                updateSmartBar();
                            }
                        }
                    ));
                }
                refreshLeftListStates();
                updateCardMatchStyle(clipIdx);
                updateSmartBar();

                // Restart playback from the new in-point so the user
                // always hears the result of their trim.
                waveform->setPlayhead(t);
                waveform->setPlayheadVisible(true);
                playClip(clipIdx);
            }
        });
        connect(waveform, &MiniWaveformWidget::outPointSet,
                this, [this, clipIdx, waveform](double t) {
            if (clipIdx < m_clips.size()) {
                // trimChanged already fired from the same O-key press.
                double oldEnd;
                int oldMS;
                if (m_trimDebounceTimer->isActive() && m_trimDebounceClipIdx == clipIdx) {
                    m_trimDebounceTimer->stop();
                    oldEnd = m_preTrimEnd;
                    oldMS  = m_preTrimMatchState;
                } else {
                    oldEnd = m_clips[clipIdx].end;
                    oldMS  = m_clips[clipIdx].matchState;
                    m_clips[clipIdx].end = t;
                    if (m_clips[clipIdx].matchState == 2)
                        m_clips[clipIdx].matchState = 1;
                }
                int newMS = m_clips[clipIdx].matchState;
                if (m_commandStack) {
                    m_commandStack->pushWithoutExecute(std::make_unique<LambdaCommand>(
                        "Set out point",
                        [this, clipIdx, t, newMS]() {
                            if (clipIdx < m_clips.size()) {
                                m_clips[clipIdx].end = t;
                                m_clips[clipIdx].matchState = newMS;
                                if (auto* wf = waveformForClip(static_cast<int>(clipIdx)))
                                    wf->setTrimRange(m_clips[clipIdx].start, t);
                                refreshLeftListStates();
                                updateCardMatchStyle(clipIdx);
                                updateSmartBar();
                            }
                        },
                        [this, clipIdx, oldEnd, oldMS]() {
                            if (clipIdx < m_clips.size()) {
                                m_clips[clipIdx].end = oldEnd;
                                m_clips[clipIdx].matchState = oldMS;
                                if (auto* wf = waveformForClip(static_cast<int>(clipIdx)))
                                    wf->setTrimRange(m_clips[clipIdx].start, oldEnd);
                                refreshLeftListStates();
                                updateCardMatchStyle(clipIdx);
                                updateSmartBar();
                            }
                        }
                    ));
                }
                refreshLeftListStates();
                updateCardMatchStyle(clipIdx);
                updateSmartBar();

                // Rebuild playback buffer so out-point change is heard immediately
                if (m_playingClipIdx == static_cast<int>(clipIdx))
                    playClip(clipIdx);
            }
        });
        connect(waveform, &MiniWaveformWidget::deleteRegionRequested,
                this, [this, clipIdx](double s, double e) {
            if (clipIdx < m_clips.size()) {
                int oldMS = m_clips[clipIdx].matchState;
                m_clips[clipIdx].deletedRegions.emplace_back(s, e);
                if (m_clips[clipIdx].matchState == 2)
                    m_clips[clipIdx].matchState = 1;
                int newMS = m_clips[clipIdx].matchState;
                if (m_commandStack) {
                    m_commandStack->pushWithoutExecute(std::make_unique<LambdaCommand>(
                        "Delete audio region",
                        [this, clipIdx, s, e, newMS]() {
                            if (clipIdx < m_clips.size()) {
                                m_clips[clipIdx].deletedRegions.emplace_back(s, e);
                                m_clips[clipIdx].matchState = newMS;
                                if (auto* wf = waveformForClip(static_cast<int>(clipIdx)))
                                    wf->setDeletedRegions(m_clips[clipIdx].deletedRegions);
                                refreshLeftListStates();
                                updateCardMatchStyle(clipIdx);
                                updateSmartBar();
                            }
                        },
                        [this, clipIdx, oldMS]() {
                            if (clipIdx < m_clips.size() && !m_clips[clipIdx].deletedRegions.empty()) {
                                m_clips[clipIdx].deletedRegions.pop_back();
                                m_clips[clipIdx].matchState = oldMS;
                                if (auto* wf = waveformForClip(static_cast<int>(clipIdx)))
                                    wf->setDeletedRegions(m_clips[clipIdx].deletedRegions);
                                refreshLeftListStates();
                                updateCardMatchStyle(clipIdx);
                                updateSmartBar();
                            }
                        }
                    ));
                }
            }
        });
        connect(waveform, &MiniWaveformWidget::deletedRegionsChanged,
                this, [this, clipIdx, waveform]() {
            if (clipIdx < m_clips.size()) {
                auto oldRegions = m_clips[clipIdx].deletedRegions;
                int oldMS = m_clips[clipIdx].matchState;
                m_clips[clipIdx].deletedRegions = waveform->deletedRegions();
                if (m_clips[clipIdx].matchState == 2)
                    m_clips[clipIdx].matchState = 1;
                int newMS = m_clips[clipIdx].matchState;
                if (m_playingClipIdx == static_cast<int>(clipIdx))
                    playClip(clipIdx);
                refreshLeftListStates();
                updateCardMatchStyle(clipIdx);
                updateSmartBar();
                if (m_commandStack) {
                    auto newRegions = m_clips[clipIdx].deletedRegions;
                    m_commandStack->pushWithoutExecute(std::make_unique<LambdaCommand>(
                        "Edit deleted regions",
                        [this, clipIdx, newRegions, newMS]() {
                            if (clipIdx < m_clips.size()) {
                                m_clips[clipIdx].deletedRegions = newRegions;
                                m_clips[clipIdx].matchState = newMS;
                                if (auto* wf = waveformForClip(static_cast<int>(clipIdx)))
                                    wf->setDeletedRegions(newRegions);
                                refreshLeftListStates();
                                updateCardMatchStyle(clipIdx);
                                updateSmartBar();
                            }
                        },
                        [this, clipIdx, oldRegions, oldMS]() {
                            if (clipIdx < m_clips.size()) {
                                m_clips[clipIdx].deletedRegions = oldRegions;
                                m_clips[clipIdx].matchState = oldMS;
                                if (auto* wf = waveformForClip(static_cast<int>(clipIdx)))
                                    wf->setDeletedRegions(oldRegions);
                                refreshLeftListStates();
                                updateCardMatchStyle(clipIdx);
                                updateSmartBar();
                            }
                        }
                    ));
                }
            }
        });
        if (!clip.deletedRegions.empty())
            waveform->setDeletedRegions(clip.deletedRegions);

        // Video sources: candidate-take chips + a preview of the shown
        // take beside its waveform. Audio sources keep the plain layout.
        if (VideoFrameGrabber::isVideoFile(clip.sourceFile)) {
            if (clipIt->second.size() > 1)
                leftLayout->addWidget(createTakeStrip(line.lineNumber, clipIt->second,
                                                      primaryClipIdx));
            auto* mediaRow = new QHBoxLayout;
            mediaRow->setSpacing(8);
            auto* preview = new VideoFrameView;
            preview->setFixedSize(192, 108);
            preview->setCursor(Qt::PointingHandCursor);
            preview->setHoverScrubEnabled(true);
            preview->setToolTip(QStringLiteral(
                "Hover and move left/right to scrub this take.\n"
                "Click to open the take player (big view + in/out points)."));
            QPointer<VideoFrameView> previewGuard(preview);
            frameGrabber()->requestStill(
                preview, clip.sourceFile, clip.start + 0.35 * std::max(0.0, clip.end - clip.start),
                preview->requestSize(), [previewGuard](const QImage& image) {
                    if (previewGuard) previewGuard->setPoster(image);
                });
            connect(preview, &VideoFrameView::hoverScrubbed, this,
                    [this, clipIdx, previewGuard](double fraction) {
                // Playback owns the preview while this take plays.
                if (!previewGuard || clipIdx >= m_clips.size() ||
                    m_playingClipIdx == static_cast<int>(clipIdx))
                    return;
                const auto& c = m_clips[clipIdx];
                frameGrabber()->requestLive(
                    previewGuard, c.sourceFile, c.start + fraction * std::max(0.0, c.end - c.start),
                    previewGuard->requestSize(), [previewGuard](const QImage& image) {
                        if (previewGuard && previewGuard->isHoverScrubbing())
                            previewGuard->setFrame(image);
                    }, VideoFrameGrabber::HoverScrub);
            });
            connect(preview, &VideoFrameView::hoverEnded, this, [this, clipIdx, previewGuard]() {
                if (previewGuard && m_playingClipIdx != static_cast<int>(clipIdx))
                    previewGuard->showPoster();
            });
            connect(preview, &VideoFrameView::clicked, this, [this, clipIdx]() {
                // Deferred: the dialog's result may rebuild this card.
                QTimer::singleShot(0, this, [this, clipIdx]() {
                    if (!m_destroying.load(std::memory_order_acquire)) openTakePlayer(clipIdx);
                });
            });
            m_takePreviewLabels[primaryClipIdx] = preview;
            mediaRow->addWidget(preview);
            mediaRow->addWidget(waveform, 1);
            leftLayout->addLayout(mediaRow);
        } else {
            leftLayout->addWidget(waveform);
        }

        // ── Right-panel controls ────────────────────────────────────
        // CONFIRM / CONFIRMED is a toggle: clicking when tentative
        // (matchState=1) sets matchState=2; clicking when confirmed
        // sets it back to 1.  We update the button in place rather
        // than rebuilding all cards (a populateCards() here would
        // tear down every widget and the scroll position jumps).
        // Both directions push a LambdaCommand so Ctrl+Z reverses
        // either step.
        {
            auto* confirmBtn = new QPushButton;
            confirmBtn->setFixedHeight(36);
            confirmBtn->setCursor(Qt::PointingHandCursor);

            auto applyButtonStyle = [confirmBtn, _tc, _radM](int ms) {
                const bool isConfirmed = (ms == 2);
                confirmBtn->setText(isConfirmed
                    ? QStringLiteral("\u2713  CONFIRMED")
                    : QStringLiteral("\u2713  CONFIRM"));
                confirmBtn->setToolTip(isConfirmed
                    ? "Click to unconfirm match"
                    : "Confirm match");
                confirmBtn->setStyleSheet(
                    QString("QPushButton { background: %1; color: %2; font-weight: bold; "
                    "font-size: 13px; border: 2px solid %3; border-radius: %4px; }"
                    "QPushButton:hover { background: %5; }").arg(
                        isConfirmed ? Theme::hex(_tc.success)
                                    : Theme::hex(_tc.successBtnBg),
                        Theme::hex(_tc.textBright),
                        Theme::hex(_tc.success), _radM,
                        Theme::hex(_tc.successBtnHover)));
            };
            // Size the button using the LONGER "CONFIRMED" label first
            // and lock that as the minimum width.  Otherwise the button
            // sized for "CONFIRM" (shorter) is too narrow to fit
            // "CONFIRMED" on the first paint, clipping the trailing "D"
            // until a manual relayout (e.g. clicking unconfirm then
            // confirm again).  Computing this from sizeHint after the
            // stylesheet is applied lets the actual font / padding /
            // border be measured rather than estimated.
            applyButtonStyle(2);
            confirmBtn->ensurePolished();
            confirmBtn->setMinimumWidth(confirmBtn->sizeHint().width());
            // The SHOWN clip's own state (for dialogue scripts that is the
            // highest-state clip, i.e. the line's aggregate as before).
            applyButtonStyle(m_clips[primaryClipIdx].matchState);

            // Captured by QPointer so undo lambdas can safely no-op the
            // visual update if the card has since been torn down by
            // another populateCards() pass \u2014 the underlying m_clips
            // state still gets reverted; only the in-place visual
            // refresh is skipped.
            QPointer<QPushButton> btnGuard(confirmBtn);
            connect(confirmBtn, &QPushButton::clicked, this,
                    [this, clipIdx, btnGuard, applyButtonStyle]() {
                if (clipIdx >= m_clips.size()) return;
                const int oldMS = m_clips[clipIdx].matchState;
                const int newMS = (oldMS == 2) ? 1 : 2;
                if (oldMS == newMS) return;
                // Shot scripts: one chosen take per line — confirming a
                // take un-picks the line's other confirmed take(s).
                std::vector<size_t> demoted;
                const bool shotScript = m_script && m_script->isShotScript();
                if (newMS == 2 && shotScript) {
                    for (size_t i = 0; i < m_clips.size(); ++i) {
                        if (i != clipIdx && m_clips[i].matchState == 2 &&
                            m_clips[i].scriptLineNumber == m_clips[clipIdx].scriptLineNumber)
                            demoted.push_back(i);
                    }
                }
                const int confirmLine = m_clips[clipIdx].scriptLineNumber;
                auto apply = [this, clipIdx, btnGuard, applyButtonStyle, demoted, shotScript,
                              confirmLine](int ms) {
                    if (m_destroying.load(std::memory_order_acquire)) return;
                    if (clipIdx >= m_clips.size()) return;
                    m_clips[clipIdx].matchState = ms;
                    for (size_t d : demoted)
                        if (d < m_clips.size()) m_clips[d].matchState = (ms == 2) ? 1 : 2;
                    if (shotScript) {
                        // Take chips show which take is picked: rebuild this
                        // line's card (deferred — this button is inside it).
                        scheduleCardRebuild({confirmLine});
                    }
                    if (btnGuard) applyButtonStyle(ms);
                    updateCardMatchStyle(clipIdx);
                    refreshLeftListStates();
                    updateWorkflowState();
                    emit voiceContextChanged();
                };
                if (m_commandStack) {
                    m_commandStack->execute(std::make_unique<LambdaCommand>(
                        newMS == 2 ? "Confirm audio match"
                                   : "Unconfirm audio match",
                        [apply, newMS]() { apply(newMS); },
                        [apply, oldMS]() { apply(oldMS); }));
                } else {
                    apply(newMS);
                }
            });
            rightLayout->addWidget(confirmBtn);
        }

        auto* smallBtnRow = new QHBoxLayout;
        smallBtnRow->setSpacing(3);
        auto makeSmallBtn = [&_tc, &_rad](const QString& text, const QString& tip, const QString& bg) {
            auto* btn = new QPushButton(text);
            btn->setFixedHeight(26);
            btn->setToolTip(tip);
            btn->setCursor(Qt::PointingHandCursor);
            btn->setStyleSheet(
                QString("QPushButton { background: %1; color: %2; border: 1px solid %3; "
                        "border-radius: %4px; font-size: %5px; padding: 2px 6px; }"
                        "QPushButton:hover { border-color: %6; }")
                    .arg(bg, Theme::hex(_tc.textSecondary), Theme::hex(_tc.controlBorder), _rad,
                         QString::number(Theme::typography().sizeXs), Theme::hex(_tc.accent)));
            return btn;
        };

        auto* playBtn = makeSmallBtn(QStringLiteral("\u25B6"), "Play clip", Theme::hex(_tc.successBg));
        auto* trimBtn = makeSmallBtn(QStringLiteral("\u26A1"), "Auto-trim", Theme::hex(_tc.surface3));
        connect(playBtn, &QPushButton::clicked, this, [this, clipIdx]() {
            m_selectedClipIdx = static_cast<int>(clipIdx);
            togglePlayClip(clipIdx);
        });
        connect(trimBtn, &QPushButton::clicked, this, [this, clipIdx]() {
            autoTrimClip(clipIdx);
        });
        smallBtnRow->addWidget(playBtn);
        smallBtnRow->addWidget(trimBtn);
        rightLayout->addLayout(smallBtnRow);

        auto* rejectBtn = new QPushButton(QStringLiteral("\u2715  Reject"));
        rejectBtn->setFixedHeight(26);
        rejectBtn->setToolTip("Reject match");
        rejectBtn->setCursor(Qt::PointingHandCursor);
        rejectBtn->setStyleSheet(
            QString("QPushButton { background: %1; color: %2; border: 1px solid %3; "
            "border-radius: %4px; font-size: %5px; padding: 2px 6px; }"
            "QPushButton:hover { border-color: %6; }").arg(
                Theme::hex(_tc.dangerBg), Theme::hex(_tc.textSecondary),
                Theme::hex(_tc.controlBorder), _rad,
                QString::number(Theme::typography().sizeXs), Theme::hex(_tc.error)));
        connect(rejectBtn, &QPushButton::clicked, this, [this, clipIdx]() {
            if (clipIdx >= m_clips.size()) return;
            struct Snap {
                int         matchState;
                int         scriptLineNumber;
                std::string scriptSegment;
                float       confidence;
            };
            Snap oldSnap{
                m_clips[clipIdx].matchState,
                m_clips[clipIdx].scriptLineNumber,
                m_clips[clipIdx].scriptSegment,
                m_clips[clipIdx].confidence};
            Snap newSnap{0, -1, std::string{}, 0.0f};
            const int rejectLine = oldSnap.scriptLineNumber;
            auto apply = [this, clipIdx, rejectLine](const Snap& s) {
                if (m_destroying.load(std::memory_order_acquire)) return;
                if (clipIdx >= m_clips.size()) return;
                m_clips[clipIdx].matchState       = s.matchState;
                m_clips[clipIdx].scriptLineNumber = s.scriptLineNumber;
                m_clips[clipIdx].scriptSegment    = s.scriptSegment;
                m_clips[clipIdx].confidence       = s.confidence;
                // Only that line's card changes. Deferred so the clicked
                // widget isn't deleted during its own signal.
                scheduleCardRebuild({rejectLine});
            };
            if (m_commandStack) {
                m_commandStack->execute(std::make_unique<LambdaCommand>(
                    "Reject audio match",
                    [apply, newSnap]() { apply(newSnap); },
                    [apply, oldSnap]() { apply(oldSnap); }));
            } else {
                apply(newSnap);
            }
        });
        rightLayout->addWidget(rejectBtn);

        rightLayout->addStretch();

        // Reassign dropdown — the item list (all script lines) is filled
        // lazily on first popup to keep card creation O(1) instead of O(N).
        auto* lineCombo = new LazyComboBox;
        lineCombo->setFixedHeight(24);
        lineCombo->setStyleSheet(
            QString("QComboBox { background: %1; color: %2; border: 1px solid %3; "
            "padding: 2px 6px; border-radius: %4px; font-size: %5px; }").arg(
                Theme::hex(_tc.inputBg), Theme::hex(_tc.textPrimary),
                Theme::hex(_tc.inputBorder), _rad,
                QString::number(Theme::typography().sizeXs)));
        lineCombo->addItem("Reassign...");
        lineCombo->setCurrentIndex(0);
        QPointer<LazyComboBox> comboGuard(lineCombo);
        lineCombo->firstPopup = [comboGuard, displayLines]() {
            if (!comboGuard) return;
            comboGuard->blockSignals(true);
            for (const auto& dl : *displayLines) {
                QString entry = QString("%1. %2: %3")
                    .arg(dl.displayNum)
                    .arg(QString::fromStdString(dl.character))
                    .arg(QString::fromStdString(dl.dialogue).left(20));
                comboGuard->addItem(entry);
            }
            comboGuard->blockSignals(false);
        };
        connect(lineCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
                this, [this, clipIdx, displayLines](int comboIdx) {
            if (clipIdx >= m_clips.size() || comboIdx <= 0) return;
            size_t lineIdx = static_cast<size_t>(comboIdx - 1);
            if (lineIdx >= displayLines->size()) return;
            const auto& dl = (*displayLines)[lineIdx];
            // Per-clip snapshot — a reassign touches the moved clip AND
            // any clip already sitting on the target line (which must be
            // bumped off, since a script line holds at most one match;
            // without this you'd end up with two clips — often two
            // different characters — on one line, which then exported as
            // out-of-order/duplicate entries).
            struct ClipSnap {
                size_t      idx;
                int         matchState;
                int         scriptLineNumber;
                std::string character;
                std::string scriptSegment;
                float       confidence;
            };
            auto capture = [this](size_t idx) -> ClipSnap {
                return { idx, m_clips[idx].matchState,
                         m_clips[idx].scriptLineNumber, m_clips[idx].character,
                         m_clips[idx].scriptSegment, m_clips[idx].confidence };
            };
            std::vector<ClipSnap> before, after;
            // The reassigned clip → tentative match on the chosen line.
            before.push_back(capture(clipIdx));
            after.push_back({ clipIdx, 1, dl.lineNumber, dl.character,
                              dl.character + ": " + dl.dialogue, 1.0f });
            // Bump any OTHER clip currently matched to that line back to
            // unmatched (keep its own character — it's still that speaker's
            // audio, just no longer assigned here).
            for (size_t i = 0; i < m_clips.size(); ++i) {
                if (i == clipIdx) continue;
                if (m_clips[i].scriptLineNumber == dl.lineNumber
                    && m_clips[i].matchState != 0) {
                    before.push_back(capture(i));
                    ClipSnap u = capture(i);
                    u.matchState = 0;
                    u.scriptLineNumber = -1;
                    u.scriptSegment.clear();
                    u.confidence = 0.0f;
                    after.push_back(std::move(u));
                }
            }
            // Cards to rebuild: every line a touched clip sits on before or after.
            std::vector<int> affectedLines;
            for (const auto* v : {&before, &after})
                for (const auto& snap : *v)
                    if (snap.scriptLineNumber > 0) affectedLines.push_back(snap.scriptLineNumber);
            auto applySnaps = [this, affectedLines](const std::vector<ClipSnap>& snaps) {
                if (m_destroying.load(std::memory_order_acquire)) return;
                for (const auto& s : snaps) {
                    if (s.idx >= m_clips.size()) continue;
                    m_clips[s.idx].matchState       = s.matchState;
                    m_clips[s.idx].scriptLineNumber = s.scriptLineNumber;
                    m_clips[s.idx].character        = s.character;
                    m_clips[s.idx].scriptSegment    = s.scriptSegment;
                    m_clips[s.idx].confidence       = s.confidence;
                }
                // Deferred so the combo isn't deleted during its own signal
                scheduleCardRebuild(affectedLines);
            };
            if (m_commandStack) {
                m_commandStack->execute(std::make_unique<LambdaCommand>(
                    "Reassign audio match",
                    [applySnaps, after]() { applySnaps(after); },
                    [applySnaps, before]() { applySnaps(before); }));
            } else {
                applySnaps(after);
            }
        });
        rightLayout->addWidget(lineCombo);

    } else {
        // ── Unmatched: left (text) | right (manual match) ──────────
        auto* noAudioLabel = new QLabel("No audio assigned");
        noAudioLabel->setAlignment(Qt::AlignCenter);
        noAudioLabel->setStyleSheet(
            QString("QLabel { color: %1; font-size: 13px; font-style: italic; border: none; "
            "padding: 20px; }").arg(Theme::hex(_tc.textTertiary)));
        leftLayout->addWidget(noAudioLabel, 1);

        int lineNum = line.lineNumber;

        // Audio file selector — lets the user pick which imported
        // audio file this line should match against.
        if (m_audioPaths.size() > 1) {
            auto* audioFileCombo = new QComboBox;
            audioFileCombo->setFixedHeight(24);
            audioFileCombo->setStyleSheet(
                QString("QComboBox { background: %1; color: %2; border: 1px solid %3; "
                "padding: 2px 6px; border-radius: %4px; font-size: %5px; }").arg(
                    Theme::hex(_tc.inputBg), Theme::hex(_tc.textPrimary),
                    Theme::hex(_tc.inputBorder), _rad,
                    QString::number(Theme::typography().sizeXs)));
            audioFileCombo->addItem("Audio file: auto");
            // Build sorted list of (filename, full path) pairs
            std::vector<std::pair<QString, QString>> sortedAudio;
            sortedAudio.reserve(m_audioPaths.size());
            for (const auto& ap : m_audioPaths) {
                QString fname = QString::fromStdString(
                    pathToUtf8(utf8ToPath(ap).filename()));
                sortedAudio.emplace_back(fname, QString::fromStdString(ap));
            }
            std::sort(sortedAudio.begin(), sortedAudio.end(),
                [](const auto& a, const auto& b) {
                    return a.first.compare(b.first, Qt::CaseInsensitive) < 0;
                });
            for (const auto& [fname, fullPath] : sortedAudio)
                audioFileCombo->addItem(fname, fullPath);
            // Pre-select if already assigned
            auto ait = m_lineAudioFile.find(lineNum);
            if (ait != m_lineAudioFile.end()) {
                for (int ci = 1; ci < audioFileCombo->count(); ++ci) {
                    if (audioFileCombo->itemData(ci).toString().toStdString() == ait->second) {
                        audioFileCombo->setCurrentIndex(ci);
                        break;
                    }
                }
            }
            connect(audioFileCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
                    this, [this, lineNum](int idx) {
                if (idx <= 0) {
                    m_lineAudioFile.erase(lineNum);
                } else {
                    auto* combo = qobject_cast<QComboBox*>(sender());
                    if (combo) {
                        m_lineAudioFile[lineNum] = combo->itemData(idx).toString().toStdString();
                    }
                }
            });
            rightLayout->addWidget(audioFileCombo);
        }

        auto* manualBtn = new QPushButton("MANUAL");
        manualBtn->setFixedHeight(40);
        manualBtn->setToolTip("Open manual match dialog");
        manualBtn->setCursor(Qt::PointingHandCursor);
        manualBtn->setStyleSheet(
            QString("QPushButton { background: %1; color: %2; font-weight: bold; "
            "font-size: 13px; border: 2px solid %3; border-radius: %4px; }"
            "QPushButton:hover { background: %3; }").arg(
                Theme::hex(_tc.accentDim), Theme::hex(_tc.textBright),
                Theme::hex(_tc.accent), _radM));
        connect(manualBtn, &QPushButton::clicked, this, [this, lineNum]() {
            openManualMatch(lineNum);
        });
        rightLayout->addWidget(manualBtn);

        rightLayout->addStretch();

        // Assign from orphan dropdown
        auto* assignCombo = new LazyComboBox;
        assignCombo->setFixedHeight(24);
        assignCombo->setStyleSheet(
            QString("QComboBox { background: %1; color: %2; border: 1px solid %3; "
            "padding: 2px 6px; border-radius: %4px; font-size: %5px; }").arg(
                Theme::hex(_tc.inputBg), Theme::hex(_tc.textPrimary),
                Theme::hex(_tc.inputBorder), _rad,
                QString::number(Theme::typography().sizeXs)));
        assignCombo->addItem("Assign clip...");
        // Unassigned clips, listed fresh on every open.
        QPointer<LazyComboBox> assignGuard(assignCombo);
        assignCombo->refillEveryPopup = true;
        assignCombo->firstPopup = [this, assignGuard]() {
            if (!assignGuard) return;
            assignGuard->blockSignals(true);
            while (assignGuard->count() > 1) assignGuard->removeItem(1);
            assignGuard->setCurrentIndex(0);
            for (size_t i = 0; i < m_clips.size(); ++i) {
                if (m_clips[i].scriptLineNumber < 0) {
                    const auto& oc = m_clips[i];
                    QString label = QString("Clip #%1 (%2) %3s")
                        .arg(i + 1)
                        .arg(oc.character.empty() ? "?" : QString::fromStdString(oc.character))
                        .arg(oc.end - oc.start, 0, 'f', 1);
                    assignGuard->addItem(label, QVariant(static_cast<int>(i)));
                }
            }
            assignGuard->blockSignals(false);
        };
        std::string lineChar = line.character;
        std::string lineDial = line.dialogue;
        connect(assignCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
                this, [this, lineNum, lineChar, lineDial](int idx) {
            if (idx <= 0) return;
            auto* combo = qobject_cast<QComboBox*>(sender());
            if (!combo) return;
            int clipIdxRaw = combo->itemData(idx).toInt();
            if (clipIdxRaw < 0) return;
            const size_t clipIdx = static_cast<size_t>(clipIdxRaw);
            if (clipIdx >= m_clips.size()) return;
            // Per-clip snapshot — assigning a clip to a line also bumps any
            // clip already matched to that line off it (one clip per line),
            // so two different speakers can't end up on the same line and
            // export out of order. Mirrors the Reassign… combo above.
            struct ClipSnap {
                size_t      idx;
                int         matchState;
                int         scriptLineNumber;
                std::string character;
                std::string scriptSegment;
                float       confidence;
            };
            auto capture = [this](size_t idx) -> ClipSnap {
                return { idx, m_clips[idx].matchState,
                         m_clips[idx].scriptLineNumber, m_clips[idx].character,
                         m_clips[idx].scriptSegment, m_clips[idx].confidence };
            };
            std::vector<ClipSnap> before, after;
            before.push_back(capture(clipIdx));
            after.push_back({ clipIdx, 1, lineNum, lineChar,
                              lineChar + ": " + lineDial, 1.0f });
            for (size_t i = 0; i < m_clips.size(); ++i) {
                if (i == clipIdx) continue;
                if (m_clips[i].scriptLineNumber == lineNum
                    && m_clips[i].matchState != 0) {
                    before.push_back(capture(i));
                    ClipSnap u = capture(i);
                    u.matchState = 0;
                    u.scriptLineNumber = -1;
                    u.scriptSegment.clear();
                    u.confidence = 0.0f;
                    after.push_back(std::move(u));
                }
            }
            // Cards to rebuild: every line a touched clip sits on before or after.
            std::vector<int> affectedLines;
            for (const auto* v : {&before, &after})
                for (const auto& snap : *v)
                    if (snap.scriptLineNumber > 0) affectedLines.push_back(snap.scriptLineNumber);
            auto applySnaps = [this, affectedLines](const std::vector<ClipSnap>& snaps) {
                if (m_destroying.load(std::memory_order_acquire)) return;
                for (const auto& s : snaps) {
                    if (s.idx >= m_clips.size()) continue;
                    m_clips[s.idx].matchState       = s.matchState;
                    m_clips[s.idx].scriptLineNumber = s.scriptLineNumber;
                    m_clips[s.idx].character        = s.character;
                    m_clips[s.idx].scriptSegment    = s.scriptSegment;
                    m_clips[s.idx].confidence       = s.confidence;
                }
                // Deferred so the combo isn't deleted during its own signal
                scheduleCardRebuild(affectedLines);
            };
            if (m_commandStack) {
                m_commandStack->execute(std::make_unique<LambdaCommand>(
                    "Assign audio clip to script line",
                    [applySnaps, after]() { applySnaps(after); },
                    [applySnaps, before]() { applySnaps(before); }));
            } else {
                applySnaps(after);
            }
        });
        rightLayout->addWidget(assignCombo);
    }

    waveformOut = waveform;
    clipIdxOut  = hasClip ? static_cast<int>(primaryClipIdx) : -1;
}

void AudioSync::rebuildCardsForLines(const std::vector<int>& lineNumbers)
{
    if (m_destroying.load(std::memory_order_acquire) || !m_script) return;
    // The UNMATCHED filter adds/removes whole cards when a line's state
    // changes; only a full rebuild handles that.
    if (m_charFilterList && m_charFilterList->currentItem() &&
        m_charFilterList->currentRow() > 0 &&
        m_charFilterList->currentItem()->text() == "UNMATCHED") {
        populateCards();
        return;
    }

    const CardBuildContext ctx = makeCardBuildContext();
    for (int lineNumber : lineNumbers) {
        const ScriptLine* line = nullptr;
        for (const auto& sl : m_script->lines)
            if (sl.lineNumber == lineNumber) { line = &sl; break; }
        if (!line) continue;

        const QString name = QString("scriptCard_%1").arg(lineNumber);
        for (size_t i = 0; i < m_cardWidgets.size(); ++i) {
            auto* card = qobject_cast<QFrame*>(m_cardWidgets[i].data());
            if (!card || card->objectName() != name) continue;

            if (i < m_cardClipIndices.size() && m_cardClipIndices[i] >= 0) {
                const auto old = static_cast<size_t>(m_cardClipIndices[i]);
                if (m_playingClipIdx == static_cast<int>(old)) stopPlayback();
                m_takePreviewLabels.erase(old);
            }
            // Same as populateCards(): move focus off a widget about to be
            // deleted, or QScrollArea auto-scrolls to wherever it lands.
            if (auto* fw = QApplication::focusWidget(); fw && card->isAncestorOf(fw) && m_rightScrollArea)
                m_rightScrollArea->setFocus(Qt::OtherFocusReason);
            const int cardIdx = card->property("cardIdx").toInt();
            MiniWaveformWidget* waveform = nullptr;
            int clipIdx = -1;
            fillCard(card, *line, cardIdx, ctx, waveform, clipIdx);
            if (i < m_cardWaveforms.size())   m_cardWaveforms[i]   = waveform;
            if (i < m_cardClipIndices.size()) m_cardClipIndices[i] = clipIdx;
            for (auto* child : card->findChildren<QWidget*>()) {
                if (qobject_cast<QLineEdit*>(child) || qobject_cast<QComboBox*>(child))
                    continue;
                child->installEventFilter(this);
            }
            break;
        }
    }
    // Always: a line whose card is filtered out still changes the counts.
    refreshLeftListStates();
    updateWorkflowState();
}

} // namespace rt
