/*
 * CaptionsPanel.cpp â€” captions/subtitle editor panel.
 */

#include "panels/captions/CaptionsPanel.h"
#include "Theme.h"
#include "Constants.h"

#include "timeline/Timeline.h"
#include "timeline/Track.h"
#include "timeline/CaptionClip.h"
#include "timeline/VideoClip.h"
#include "timeline/AudioClip.h"
#include "command/CommandStack.h"
#include "command/LambdaCommand.h"
#include "ai/Transcriber.h"
#include "QtHelpers.h"
#include "viewport/TransformOverlayWidget.h"

#include <spdlog/spdlog.h>

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFrame>
#include <QSplitter>
#include <QHeaderView>
#include <QScrollBar>
#include <QMessageBox>
#include <QMetaObject>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <thread>
#include <unordered_map>

namespace rt {

// â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
// Helpers
// â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
// Compact MM:SS.mmm timecode (minutes:seconds.milliseconds). Hours are only
// shown when the value is actually >= 1 hour, so short projects aren't padded
// with an excessive "HH:" / ":FF" frame field.
static QString ticksToTimecode(int64_t ticks)
{
 if (ticks < 0) ticks = 0;
 int64_t totalMs = ticks * 1000 / static_cast<int64_t>(kTicksPerSecond);
 int ms = static_cast<int>(totalMs % 1000);
 int64_t totalSec = totalMs / 1000;
 int seconds = static_cast<int>(totalSec % 60);
 int minutes = static_cast<int>((totalSec / 60) % 60);
 int hours = static_cast<int>(totalSec / 3600);
 if (hours > 0) {
 return QString("%1:%2:%3.%4")
 .arg(hours)
 .arg(minutes, 2, 10, QLatin1Char('0'))
 .arg(seconds, 2, 10, QLatin1Char('0'))
 .arg(ms, 3, 10, QLatin1Char('0'));
 }
 return QString("%1:%2.%3")
 .arg(minutes, 2, 10, QLatin1Char('0'))
 .arg(seconds, 2, 10, QLatin1Char('0'))
 .arg(ms, 3, 10, QLatin1Char('0'));
}

// Build the list-row label: "[TC] SPEAKER: text" (speaker uppercased) or
// "[TC] text" when there is no speaker.
static QString buildCaptionLabel(int64_t timelineIn, const QString& speaker,
                                 QString text)
{
 if (text.length() > 60) text = text.left(57) + "...";
 const QString tc = ticksToTimecode(timelineIn);
 if (!speaker.trimmed().isEmpty())
 return QString("[%1] %2: %3").arg(tc, speaker.toUpper(), text);
 return QString("[%1] %2").arg(tc, text);
}

// â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
// Construction
// â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
CaptionsPanel::CaptionsPanel(QWidget* parent)
 : QWidget(parent)
{
 buildUI();
 applyTheme();
}

void CaptionsPanel::buildUI()
{
 // The action-bar buttons + editor rows need a baseline width; without a
 // minimum the panel starts narrower than its content and the layout looks
 // broken until the user widens it.
 setMinimumWidth(280);

 auto* mainLayout = new QVBoxLayout(this);
 mainLayout->setContentsMargins(4, 4, 4, 4);
 mainLayout->setSpacing(4);

 // â”€â”€ Top action bar (two rows so it stays compact when narrow) â”€â”€â”€â”€â”€
 // Row 1: primary actions
 auto* actionRow1 = new QHBoxLayout;
 actionRow1->setSpacing(6);

 m_addTrackBtn = new QPushButton("\xE2\x9E\x95 Add Captions to Timeline", this);
 m_addTrackBtn->setObjectName("addTrackBtn");
 m_addTrackBtn->setFixedHeight(30);
 m_addTrackBtn->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
 m_addTrackBtn->setToolTip("Create the Captions track at the top of the timeline");
 actionRow1->addWidget(m_addTrackBtn);

 m_transcribeBtn = new QPushButton("\xF0\x9F\x8E\x99 Transcribe", this);
 m_transcribeBtn->setObjectName("transcribeBtn");
 m_transcribeBtn->setFixedHeight(30);
 m_transcribeBtn->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
 m_transcribeBtn->setToolTip("Transcribe all spoken audio on the timeline into caption clips");
 actionRow1->addWidget(m_transcribeBtn);

 mainLayout->addLayout(actionRow1);

 // Row 2: secondary actions
 auto* actionRow2 = new QHBoxLayout;
 actionRow2->setSpacing(6);

 m_clearAllBtn = new QPushButton("\xF0\x9F\x97\x91 Clear All", this);
 m_clearAllBtn->setObjectName("clearAllBtn");
 m_clearAllBtn->setFixedHeight(30);
 m_clearAllBtn->setToolTip("Remove all caption clips from the timeline");
 actionRow2->addWidget(m_clearAllBtn);

 m_fillGapsBtn = new QPushButton("\xE2\x96\xB6 Fill Gaps", this);
 m_fillGapsBtn->setFixedHeight(30);
 m_fillGapsBtn->setToolTip("Extend selected caption to fill gap before the next caption");
 actionRow2->addWidget(m_fillGapsBtn);

 actionRow2->addStretch();

 m_addBtn = new QPushButton("+", this);
 m_addBtn->setFixedSize(30, 30);
 m_addBtn->setToolTip("Add empty caption at playhead");
 actionRow2->addWidget(m_addBtn);

 m_deleteBtn = new QPushButton("\xE2\x9C\x95", this);
 m_deleteBtn->setFixedSize(30, 30);
 m_deleteBtn->setToolTip("Delete selected caption");
 actionRow2->addWidget(m_deleteBtn);

 mainLayout->addLayout(actionRow2);

 // â”€â”€ Transcription progress bar (hidden unless transcribing) â”€â”€â”€â”€â”€â”€â”€â”€
 m_progressBar = new QProgressBar(this);
 m_progressBar->setObjectName("captionProgress");
 m_progressBar->setFixedHeight(16);
 m_progressBar->setTextVisible(true);
 m_progressBar->setVisible(false);
 mainLayout->addWidget(m_progressBar);

 // â”€â”€ Caption count + info bar â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
 m_countLabel = new QLabel("No captions", this);
 m_countLabel->setObjectName("captionCountLabel");
 mainLayout->addWidget(m_countLabel);

 // â”€â”€ Filter box â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
 m_filterEdit = new QLineEdit(this);
 m_filterEdit->setPlaceholderText("Filter captions (text or speaker)...");
 m_filterEdit->setClearButtonEnabled(true);
 mainLayout->addWidget(m_filterEdit);
 connect(m_filterEdit, &QLineEdit::textChanged,
 this, [this](const QString&) { applyCaptionFilter(); });

 // Replace row â€” replaces the filter text inside every caption's text.
 auto* replaceRow = new QHBoxLayout();
 m_replaceEdit = new QLineEdit(this);
 m_replaceEdit->setPlaceholderText("Replace with...");
 m_replaceEdit->setClearButtonEnabled(true);
 replaceRow->addWidget(m_replaceEdit, 1);
 m_replaceAllBtn = new QPushButton("Replace All", this);
 m_replaceAllBtn->setToolTip("Replace every occurrence of the filter text in all caption texts (one undo step)");
 connect(m_replaceAllBtn, &QPushButton::clicked, this, &CaptionsPanel::onReplaceAll);
 replaceRow->addWidget(m_replaceAllBtn);
 mainLayout->addLayout(replaceRow);

 // â”€â”€ Caption list â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
 m_captionList = new QListWidget(this);
 m_captionList->setSelectionMode(QAbstractItemView::SingleSelection);
 m_captionList->setMinimumHeight(100);
 mainLayout->addWidget(m_captionList, 1);

 // â”€â”€ Editor section â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
 m_editorSection = new QWidget(this);
 auto* editorFrame = new QFrame(m_editorSection);
 editorFrame->setObjectName("captionEditorFrame");
 auto* editorOuter = new QVBoxLayout(m_editorSection);
 editorOuter->setContentsMargins(0, 0, 0, 0);
 editorOuter->addWidget(editorFrame);

 auto* editorLayout = new QVBoxLayout(editorFrame);
 editorLayout->setContentsMargins(6, 6, 6, 6);
 editorLayout->setSpacing(4);

 // Time label
 m_timeLabel = new QLabel("--:--:--:-- \xE2\x86\x92 --:--:--:--", editorFrame);
 m_timeLabel->setObjectName("captionTimeLabel");
 editorLayout->addWidget(m_timeLabel);

 // Speaker
 auto* speakerRow = new QHBoxLayout;
 speakerRow->addWidget(new QLabel("Speaker:", editorFrame));
 m_speakerEdit = new QLineEdit(editorFrame);
 m_speakerEdit->setPlaceholderText("Speaker name...");
 speakerRow->addWidget(m_speakerEdit);
 editorLayout->addLayout(speakerRow);

 // Position row
 auto* posRow = new QHBoxLayout;
 posRow->addWidget(new QLabel("Position:", editorFrame));
 m_positionCombo = new QComboBox(editorFrame);
 m_positionCombo->addItems({"Bottom", "Top", "Middle"});
 m_positionCombo->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
 posRow->addWidget(m_positionCombo);
 editorLayout->addLayout(posRow);

 // Font row
 auto* fontRow = new QHBoxLayout;
 fontRow->addWidget(new QLabel("Font:", editorFrame));
 m_fontCombo = new QComboBox(editorFrame);
 m_fontCombo->addItems({"Arial", "Helvetica", "Roboto", "Open Sans", "Noto Sans"});
 m_fontCombo->setEditable(true);
 m_fontCombo->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
 fontRow->addWidget(m_fontCombo);

 m_fontSizeSpin = new QSpinBox(editorFrame);
 m_fontSizeSpin->setRange(8, 200);
 m_fontSizeSpin->setValue(32);
 m_fontSizeSpin->setSuffix("px");
 m_fontSizeSpin->setFixedWidth(70);
 fontRow->addWidget(m_fontSizeSpin);
 editorLayout->addLayout(fontRow);

 // Apply the complete current track style to every caption at once.
 m_applyAllBtn = new QPushButton("Apply Style to All Captions", editorFrame);
 m_applyAllBtn->setToolTip(
     "Apply this caption's complete typography, colors, outline and position "
     "to ALL captions (one undo step)");
 connect(m_applyAllBtn, &QPushButton::clicked, this, &CaptionsPanel::onApplyStyleToAll);
 editorLayout->addWidget(m_applyAllBtn);

 // Text edit
 m_textEdit = new QTextEdit(editorFrame);
 m_textEdit->setPlaceholderText("Caption text...");
 m_textEdit->setMaximumHeight(80);
 editorLayout->addWidget(m_textEdit);

 mainLayout->addWidget(m_editorSection);

 // â”€â”€ Connections â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
 connect(m_captionList, &QListWidget::currentRowChanged,
 this, &CaptionsPanel::onListSelectionChanged);
 connect(m_textEdit, &QTextEdit::textChanged,
 this, &CaptionsPanel::onTextChanged);
 connect(m_speakerEdit, &QLineEdit::textChanged,
 this, &CaptionsPanel::onSpeakerChanged);
 connect(m_positionCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
 this, &CaptionsPanel::onPositionChanged);
 connect(m_fontCombo, &QComboBox::currentTextChanged,
 this, &CaptionsPanel::onFontChanged);
 connect(m_fontSizeSpin, QOverload<int>::of(&QSpinBox::valueChanged),
 this, &CaptionsPanel::onFontSizeChanged);
 connect(m_addTrackBtn, &QPushButton::clicked,
 this, &CaptionsPanel::onAddCaptionsToTimeline);
 connect(m_transcribeBtn, &QPushButton::clicked,
 this, &CaptionsPanel::onTranscribe);
 connect(m_clearAllBtn, &QPushButton::clicked,
 this, &CaptionsPanel::onClearAll);
 connect(m_addBtn, &QPushButton::clicked,
 this, &CaptionsPanel::onAddCaption);
 connect(m_deleteBtn, &QPushButton::clicked,
 this, &CaptionsPanel::onDeleteCaption);
 connect(m_fillGapsBtn, &QPushButton::clicked,
 this, &CaptionsPanel::onFillGaps);

 updateButtonStates();
}

void CaptionsPanel::applyTheme()
{
 const auto& tc = Theme::colors();
 setStyleSheet(QString(
 "QFrame#captionEditorFrame { border: 1px solid %1; border-radius: 4px; }"
 "QLabel#captionTimeLabel { color: %2; font-family: 'Consolas', monospace; font-size: %15px; }"
 "QLabel#captionCountLabel { color: %3; font-size: 10px; padding: 1px 2px; }"
 "QListWidget { background: %4; color: %5; border: 1px solid %1; }"
 "QTextEdit { background: %4; color: %5; border: 1px solid %1; }"
 "QLineEdit { background: %4; color: %5; border: 1px solid %1; padding: 2px 4px; }"
 "QLabel { color: %5; }"
 "QPushButton { background: %6; color: %5; border: 1px solid %1; border-radius: 3px; padding: 4px 10px; }"
 "QPushButton:hover { background: %7; }"
 "QPushButton:disabled { color: %8; background: %9; }"
 "QPushButton#transcribeBtn { background: %10; font-weight: bold; }"
 "QPushButton#transcribeBtn:hover { background: %11; }"
 "QPushButton#clearAllBtn { background: %12; color: %13; }"
 "QPushButton#clearAllBtn:hover { background: %14; }"
 )
 .arg(Theme::hex(tc.border)) // 1
 .arg(Theme::hex(tc.accent)) // 2
 .arg(Theme::hex(tc.textSecondary)) // 3
 .arg(Theme::hex(tc.surface1)) // 4
 .arg(Theme::hex(tc.textPrimary)) // 5
 .arg(Theme::hex(tc.surface2)) // 6
 .arg(Theme::hex(tc.surface3)) // 7
 .arg(Theme::hex(tc.textDisabled)) // 8
 .arg(Theme::hex(tc.surface1)) // 9
 .arg(Theme::hex(tc.primaryBtnBg)) // 10
 .arg(Theme::hex(tc.primaryBtnHover))// 11
 .arg(Theme::hex(tc.dangerBg)) // 12
 .arg(Theme::hex(tc.dangerText)) // 13
 .arg(Theme::hex(tc.dangerBgHover)) // 14
 .arg(Theme::typography().sizeXxs)); // 15
}

// â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
// Timeline binding
// â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
CaptionsPanel::~CaptionsPanel()
{
 if (m_timeline)
 m_timeline->removeObserver(this);
}

void CaptionsPanel::setTimeline(Timeline* timeline)
{
 if (m_timeline)
 m_timeline->removeObserver(this);
 m_timeline = timeline;
 if (m_timeline)
 m_timeline->addObserver(this);
 refresh();
}

void CaptionsPanel::onTimelineDestroyed(Timeline* tl)
{
 if (tl != m_timeline) return;
 // The timeline owns every caption clip — drop all cached pointers together.
 // Do NOT call removeObserver(): the observer list dies with the timeline.
 m_timeline = nullptr;
 refresh();   // clears m_entries and the list widget (null-timeline path)
}

CaptionClip* CaptionsPanel::findCaptionClipById(uint64_t clipId) const
{
 if (!m_timeline) return nullptr;
 for (size_t i = 0; i < m_timeline->trackCount(); ++i) {
 Track* trk = m_timeline->track(i);
 if (!trk || !trk->isCaptionTrack()) continue;
 const size_t idx = trk->findClipIndexById(clipId);
 if (idx == trk->clipCount()) continue;
 Clip* c = trk->clip(idx);
 if (c && c->isCaption())
 return static_cast<CaptionClip*>(c);
 }
 return nullptr;
}

CaptionClip* CaptionsPanel::entryClip(int row) const
{
 if (row < 0 || row >= static_cast<int>(m_entries.size())) return nullptr;
 return findCaptionClipById(m_entries[row].clipId);
}

// â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
// Refresh â€” rebuild caption list from timeline
// â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
void CaptionsPanel::refresh()
{
 m_updatingUI = true;
 m_entries.clear();
 m_captionList->clear();

 double totalSec = 0.0;
 if (m_timeline) {
 // Live pointers only within this scope (for sort/labels); entries cache ids.
 std::vector<std::pair<CaptionEntry, CaptionClip*>> collected;
 for (size_t ti = 0; ti < m_timeline->trackCount(); ++ti) {
 Track* track = m_timeline->track(ti);
 if (!track->isCaptionTrack()) continue;

 for (size_t ci = 0; ci < track->clipCount(); ++ci) {
 Clip* clip = track->clip(ci);
 if (!clip->isCaption()) continue;

 auto* cc = static_cast<CaptionClip*>(clip);
 CaptionEntry entry;
 entry.trackIndex = ti;
 entry.clipIndex = ci;
 entry.clipId = cc->id();
 collected.emplace_back(entry, cc);
 }
 }

 // Sort by timeline position
 std::sort(collected.begin(), collected.end(),
 [](const auto& a, const auto& b) {
 return a.second->timelineIn() < b.second->timelineIn();
 });

 // Populate list
 for (const auto& [entry, cc] : collected) {
 m_entries.push_back(entry);
 m_captionList->addItem(buildCaptionLabel(
 cc->timelineIn(),
 QString::fromStdString(cc->speaker()),
 QString::fromStdString(cc->text())));
 totalSec += static_cast<double>(cc->duration()) / kTicksPerSecond;
 }
 }

 m_updatingUI = false;

 // Update count label
 if (m_entries.empty()) {
 m_countLabel->setText("No captions");
 } else {
 m_countLabel->setText(QString("%1 caption%2 \xC2\xB7 %3 total")
 .arg(m_entries.size())
 .arg(m_entries.size() == 1 ? "" : "s")
 .arg(QString::number(totalSec, 'f', 1) + "s"));
 }

 // Clear editor if no entries
 if (m_entries.empty()) {
 m_textEdit->clear();
 m_speakerEdit->clear();
 m_timeLabel->setText("--:--:--:-- \xE2\x86\x92 --:--:--:--");
 }

 applyCaptionFilter();
 updateButtonStates();
}

void CaptionsPanel::applyCaptionFilter()
{
 if (!m_filterEdit || !m_captionList) return;
 const QString needle = m_filterEdit->text().trimmed();
 for (int i = 0; i < m_captionList->count(); ++i) {
 auto* item = m_captionList->item(i);
 item->setHidden(!needle.isEmpty() &&
 !item->text().contains(needle, Qt::CaseInsensitive));
 }
}

// â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
// Select a specific caption
// â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
void CaptionsPanel::selectCaption(size_t trackIndex, size_t clipIndex)
{
 for (int i = 0; i < static_cast<int>(m_entries.size()); ++i) {
 if (m_entries[i].trackIndex == trackIndex &&
 m_entries[i].clipIndex == clipIndex) {
 m_captionList->setCurrentRow(i);
 return;
 }
 }
}

// â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
// Slots
// â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
void CaptionsPanel::onListSelectionChanged()
{
 if (m_updatingUI) return;

 int row = m_captionList->currentRow();
 if (row < 0 || row >= static_cast<int>(m_entries.size())) return;

 CaptionClip* cc = entryClip(row);
 if (!cc) return;
 m_updatingUI = true;

 m_textEdit->setPlainText(QString::fromStdString(cc->text()));
 m_speakerEdit->setText(QString::fromStdString(cc->speaker()));
 m_positionCombo->setCurrentIndex(static_cast<int>(cc->position()));
 m_fontCombo->setCurrentText(QString::fromStdString(cc->fontFamily()));
 m_fontSizeSpin->setValue(cc->fontSize());

 QString tcIn = ticksToTimecode(cc->timelineIn());
 QString tcOut = ticksToTimecode(cc->timelineOut());
 m_timeLabel->setText(QString("%1 \u2192 %2").arg(tcIn, tcOut));

 m_updatingUI = false;

 emit captionSelected(cc->timelineIn());
}

void CaptionsPanel::onTextChanged()
{
 if (m_updatingUI) return;

 int row = m_captionList->currentRow();
 if (row < 0 || row >= static_cast<int>(m_entries.size())) return;

 CaptionClip* cc = entryClip(row);
 if (!cc) return;
 std::string newText = m_textEdit->toPlainText().toStdString();
 std::string oldText = cc->text();
 if (newText == oldText) return;
 const auto oldStyles = cc->styleRuns();

 // Apply directly (don't call refresh â€” it destroys cursor position)
 cc->replaceTextPreservingStyles(newText);
 const auto newStyles = cc->styleRuns();

 // Capture the clip ID, never the pointer: the clip can be deleted (and its
 // memory freed) while this command is still in the stack.
 CaptionsPanel* self = this;
 const uint64_t undoClipId = cc->id();
 if (m_commandStack) {
 m_commandStack->pushWithoutExecute(std::make_unique<LambdaCommand>(
 "Edit Caption Text",
 [undoClipId, newText, newStyles, self]() { if (auto* c = self->findCaptionClipById(undoClipId)) { c->setText(newText); c->setStyleRuns(newStyles); self->refresh(); } },
 [undoClipId, oldText, oldStyles, self]() { if (auto* c = self->findCaptionClipById(undoClipId)) { c->setText(oldText); c->setStyleRuns(oldStyles); self->refresh(); } }));
 }

 // Update list item text inline.
 // Use the captured row index instead of currentItem() to avoid a
 // dangling-pointer crash: QTextEdit::textChanged is emitted during
 // key-press processing (inside QTextDocumentPrivate::finishEdit),
 // and Qt delivers signals synchronously.  If the QListWidget item
 // was destroyed between the initial row lookup and this update
 // (e.g. via re-entrant refresh from a nested signal), currentItem()
 // would return a freed pointer.  item(row) is NULL-safe when the
 // row is out of range.
 m_updatingUI = true;
 if (row >= 0 && row < m_captionList->count()) {
     if (auto* item = m_captionList->item(row))
         item->setText(buildCaptionLabel(cc->timelineIn(),
             QString::fromStdString(cc->speaker()),
             m_textEdit->toPlainText()));
 }
 m_updatingUI = false;

 emit captionEdited();
}

void CaptionsPanel::onSpeakerChanged()
{
 if (m_updatingUI) return;

 int row = m_captionList->currentRow();
 if (row < 0 || row >= static_cast<int>(m_entries.size())) return;

 CaptionClip* cc = entryClip(row);
 if (!cc) return;
 std::string newSpeaker = m_speakerEdit->text().toStdString();
 std::string oldSpeaker = cc->speaker();
 if (newSpeaker == oldSpeaker) return;

 // Apply directly
 cc->setSpeaker(newSpeaker);

 CaptionsPanel* self = this;
 const uint64_t undoClipId = cc->id();
 if (m_commandStack) {
 m_commandStack->pushWithoutExecute(std::make_unique<LambdaCommand>(
 "Edit Caption Speaker",
 [undoClipId, newSpeaker, self]() { if (auto* c = self->findCaptionClipById(undoClipId)) { c->setSpeaker(newSpeaker); self->refresh(); } },
 [undoClipId, oldSpeaker, self]() { if (auto* c = self->findCaptionClipById(undoClipId)) { c->setSpeaker(oldSpeaker); self->refresh(); } }));
 }

 // Update list item text inline.
 // Use captured row index (not currentItem()) to avoid dangling pointer
 // — see onTextChanged() for rationale.
 m_updatingUI = true;
 if (row >= 0 && row < m_captionList->count()) {
     if (auto* item = m_captionList->item(row))
         item->setText(buildCaptionLabel(cc->timelineIn(),
             m_speakerEdit->text(),
             QString::fromStdString(cc->text())));
 }
 m_updatingUI = false;

 emit captionEdited();
}

void CaptionsPanel::onPositionChanged(int index)
{
 if (m_updatingUI) return;
 int row = m_captionList->currentRow();
 if (row < 0 || row >= static_cast<int>(m_entries.size())) return;
 if (index < 0) return;

 CaptionClip* cc = entryClip(row);
 if (!cc) return;
 auto newPos = static_cast<CaptionPosition>(index);
 auto oldPos = cc->position();
 if (newPos == oldPos) return;

 cc->setPosition(newPos);

 CaptionsPanel* self = this;
 const uint64_t undoClipId = cc->id();
 if (m_commandStack) {
 m_commandStack->pushWithoutExecute(std::make_unique<LambdaCommand>(
 "Edit Caption Position",
 [undoClipId, newPos, self]() { if (auto* c = self->findCaptionClipById(undoClipId)) { c->setPosition(newPos); emit self->captionEdited(); } },
 [undoClipId, oldPos, self]() { if (auto* c = self->findCaptionClipById(undoClipId)) { c->setPosition(oldPos); emit self->captionEdited(); } }));
 }

 emit captionEdited();
}

void CaptionsPanel::onFontChanged(const QString& family)
{
 if (m_updatingUI) return;
 if (m_monitorTextEditing) {
 emit inlineFontFamilyRequested(family);
 return;
 }
 int row = m_captionList->currentRow();
 if (row < 0 || row >= static_cast<int>(m_entries.size())) return;

 CaptionClip* cc = entryClip(row);
 if (!cc) return;
 std::string newFont = family.toStdString();
 std::string oldFont = cc->fontFamily();
 if (newFont == oldFont) return;

 cc->setFontFamily(newFont);

 CaptionsPanel* self = this;
 const uint64_t undoClipId = cc->id();
 if (m_commandStack) {
 m_commandStack->pushWithoutExecute(std::make_unique<LambdaCommand>(
 "Edit Caption Font",
 [undoClipId, newFont, self]() { if (auto* c = self->findCaptionClipById(undoClipId)) { c->setFontFamily(newFont); emit self->captionEdited(); } },
 [undoClipId, oldFont, self]() { if (auto* c = self->findCaptionClipById(undoClipId)) { c->setFontFamily(oldFont); emit self->captionEdited(); } }));
 }

 emit captionEdited();
}

void CaptionsPanel::onFontSizeChanged(int size)
{
 if (m_updatingUI) return;
 if (m_monitorTextEditing) {
 emit inlineFontSizeRequested(static_cast<float>(size));
 return;
 }
 int row = m_captionList->currentRow();
 if (row < 0 || row >= static_cast<int>(m_entries.size())) return;

 CaptionClip* cc = entryClip(row);
 if (!cc) return;
 float newSize = static_cast<float>(size);
 float oldSize = cc->fontSize();
 if (newSize == oldSize) return;

 cc->setFontSize(newSize);

 CaptionsPanel* self = this;
 const uint64_t undoClipId = cc->id();
 if (m_commandStack) {
 m_commandStack->pushWithoutExecute(std::make_unique<LambdaCommand>(
 "Edit Caption Font Size",
 [undoClipId, newSize, self]() { if (auto* c = self->findCaptionClipById(undoClipId)) { c->setFontSize(newSize); emit self->captionEdited(); } },
 [undoClipId, oldSize, self]() { if (auto* c = self->findCaptionClipById(undoClipId)) { c->setFontSize(oldSize); emit self->captionEdited(); } }));
 }

 emit captionEdited();
}

void CaptionsPanel::setInlineTextSelectionFormat(
 const QString& family, float pointSize, int, bool, bool, bool,
 float, float, float, uint32_t mixedFlags)
{
 if (!m_monitorTextEditing) return;
 m_updatingUI = true;
 if (m_fontCombo) {
     if (mixedFlags & InlineMixedFamily) m_fontCombo->setCurrentIndex(-1);
     else m_fontCombo->setCurrentText(family);
 }
 if (m_fontSizeSpin) {
 m_fontSizeSpin->setValue(static_cast<int>(std::lround(pointSize)));
     m_fontSizeSpin->setToolTip(mixedFlags & InlineMixedSize
         ? tr("Mixed font sizes in selection") : QString());
 }
 m_updatingUI = false;
}

void CaptionsPanel::onAddCaption()
{
 if (!m_timeline) return;

 // Find or create a caption track
 Track* captionTrack = nullptr;
 for (size_t i = 0; i < m_timeline->trackCount(); ++i) {
 if (m_timeline->track(i)->isCaptionTrack()) {
 captionTrack = m_timeline->track(i);
 break;
 }
 }
 if (!captionTrack) {
 captionTrack = m_timeline->addCaptionTrack();
 }

 // Create a 3-second caption at the playhead
 int64_t playhead = m_timeline->playheadPosition();
 auto clip = std::make_unique<CaptionClip>();
 clip->setTimelineIn(playhead);
 clip->setDuration(kTicksPerSecond * 3); // 3 seconds
 clip->setText("New caption");
 uint64_t clipId = clip->id();

 // Capture for lambda â€” we need shared ownership of the clip for undo
 auto clipShared = std::make_shared<std::unique_ptr<CaptionClip>>(std::move(clip));
 CaptionsPanel* self = this;

 // Lambdas resolve the timeline through the panel (nulled by
 // onTimelineDestroyed) instead of capturing a raw Timeline*.
 auto doIt = [clipShared, self]() {
 Timeline* tl = self->m_timeline;
 if (!tl) return;
 // Find caption track
 Track* trk = nullptr;
 for (size_t i = 0; i < tl->trackCount(); ++i) {
 if (tl->track(i)->isCaptionTrack()) {
 trk = tl->track(i);
 break;
 }
 }
 if (!trk) trk = tl->addCaptionTrack();
 if (*clipShared) {
 trk->addClip(std::move(*clipShared));
 }
 self->refresh();
 emit self->captionEdited();
 };

 auto undoIt = [clipId, clipShared, self]() {
 Timeline* tl = self->m_timeline;
 if (!tl) return;
 for (size_t i = 0; i < tl->trackCount(); ++i) {
 auto* trk = tl->track(i);
 if (!trk->isCaptionTrack()) continue;
 auto removed = trk->removeClipById(clipId);
 if (removed) {
 *clipShared = std::unique_ptr<CaptionClip>(
 static_cast<CaptionClip*>(removed.release()));
 self->refresh();
 emit self->captionEdited();
 return;
 }
 }
 };

 if (m_commandStack) {
 spdlog::info("CaptionsPanel: executing Add Caption command");
 m_commandStack->execute(std::make_unique<LambdaCommand>(
 "Add Caption", doIt, undoIt));
 } else {
 spdlog::warn("CaptionsPanel: no CommandStack, executing directly");
 doIt();
 }

 refresh();
 emit captionEdited();
}

void CaptionsPanel::onDeleteCaption()
{
 if (!m_timeline) return;

 int row = m_captionList->currentRow();
 if (row < 0 || row >= static_cast<int>(m_entries.size())) return;

 const auto& entry = m_entries[row];
 uint64_t clipId = entry.clipId;
 Timeline* tl = m_timeline;
 CaptionsPanel* self = this;

 // Find the caption track
 Track* track = tl->track(entry.trackIndex);
 if (!track) return;

 // Remove the clip now (execute)
 auto removed = track->removeClipById(clipId);
 if (!removed) return;

 auto clipShared = std::make_shared<std::unique_ptr<Clip>>(std::move(removed));

 if (m_commandStack) {
 m_commandStack->pushWithoutExecute(std::make_unique<LambdaCommand>(
 "Delete Caption",
 [clipId, clipShared, self]() {
 // Redo: remove again
 Timeline* tl = self->m_timeline;
 if (!tl) return;
 for (size_t i = 0; i < tl->trackCount(); ++i) {
 auto* trk = tl->track(i);
 if (!trk->isCaptionTrack()) continue;
 auto re = trk->removeClipById(clipId);
 if (re) {
 *clipShared = std::move(re);
 self->refresh();
 emit self->captionEdited();
 return;
 }
 }
 },
 [clipShared, self]() {
 // Undo: re-add the clip
 Timeline* tl = self->m_timeline;
 if (!tl) return;
 Track* trk = nullptr;
 for (size_t i = 0; i < tl->trackCount(); ++i) {
 if (tl->track(i)->isCaptionTrack()) {
 trk = tl->track(i);
 break;
 }
 }
 if (!trk || !*clipShared) return;
 trk->addClip(std::move(*clipShared));
 self->refresh();
 emit self->captionEdited();
 }));
 }

 refresh();
 emit captionEdited();
}

void CaptionsPanel::onApplyStyleToAll()
{
 if (!m_timeline || m_entries.empty()) return;

 CaptionClip* source = entryClip(m_captionList->currentRow());
 if (!source) source = findCaptionClipById(m_entries.front().clipId);
 if (!source) return;
 CaptionStyle style = source->captionStyle();
 // Keep the visible quick controls authoritative even if their edit signal
 // has not yet completed, and give the shared style a persistent track name.
 style.fontFamily = m_fontCombo->currentText().toStdString();
 style.fontSize = static_cast<float>(m_fontSizeSpin->value());
 style.position = static_cast<CaptionPosition>(
     std::max(0, m_positionCombo->currentIndex()));
 if (style.name.empty()) style.name = "Caption Track Style";

 // Snapshot per-clip old styles so undo restores each caption exactly.
 struct OldStyle { uint64_t id; CaptionStyle style; };
 auto old = std::make_shared<std::vector<OldStyle>>();
 for (const auto& e : m_entries)
 if (auto* c = findCaptionClipById(e.clipId))
 old->push_back({e.clipId, c->captionStyle()});
 if (old->empty()) return;

 CaptionsPanel* self = this;
 auto doIt = [self, style, old]() {
 for (const auto& o : *old)
 if (auto* c = self->findCaptionClipById(o.id))
 c->setCaptionStyle(style);
 self->refresh();
 emit self->captionEdited();
 };
 auto undoIt = [self, old]() {
 for (const auto& o : *old)
 if (auto* c = self->findCaptionClipById(o.id))
 c->setCaptionStyle(o.style);
 self->refresh();
 emit self->captionEdited();
 };

 if (m_commandStack) {
 m_commandStack->execute(std::make_unique<LambdaCommand>(
 "Apply Caption Style to All", doIt, undoIt));
 } else {
 doIt();
 }
}

void CaptionsPanel::onReplaceAll()
{
 if (!m_timeline || m_entries.empty()) return;
 const QString needle = m_filterEdit ? m_filterEdit->text() : QString();
 if (needle.trimmed().isEmpty()) {
 QMessageBox::information(this, "Replace All",
 "Type the text to find into the filter box first.");
 return;
 }
 const QString replacement = m_replaceEdit ? m_replaceEdit->text() : QString();

 // Precompute per-clip old/new text; only clips whose text actually changes
 // participate (filter also matches speaker â€” don't rewrite those).
 struct Change {
 uint64_t id;
 std::string oldText;
 std::string newText;
 std::vector<TextStyleRun> oldStyles;
 std::vector<TextStyleRun> newStyles;
 };
 auto changes = std::make_shared<std::vector<Change>>();
 for (const auto& e : m_entries) {
 auto* c = findCaptionClipById(e.clipId);
 if (!c) continue;
 QString txt = QString::fromStdString(c->text());
 QString replaced = txt;
 replaced.replace(needle, replacement, Qt::CaseInsensitive);
 if (replaced != txt)
 {
 const std::string newText = replaced.toStdString();
 changes->push_back({e.clipId, c->text(), newText, c->styleRuns(),
 remapTextStyleRuns(c->text(), newText, c->styleRuns())});
 }
 }
 if (changes->empty()) {
 QMessageBox::information(this, "Replace All",
 QString("No caption text contains \"%1\".").arg(needle));
 return;
 }

 CaptionsPanel* self = this;
 auto doIt = [self, changes]() {
 for (const auto& ch : *changes)
 if (auto* c = self->findCaptionClipById(ch.id)) {
 c->setText(ch.newText);
 c->setStyleRuns(ch.newStyles);
 }
 self->refresh();
 emit self->captionEdited();
 };
 auto undoIt = [self, changes]() {
 for (const auto& ch : *changes)
 if (auto* c = self->findCaptionClipById(ch.id)) {
 c->setText(ch.oldText);
 c->setStyleRuns(ch.oldStyles);
 }
 self->refresh();
 emit self->captionEdited();
 };

 if (m_commandStack) {
 m_commandStack->execute(std::make_unique<LambdaCommand>(
 "Replace in Captions", doIt, undoIt));
 } else {
 doIt();
 }
}

void CaptionsPanel::onClearAll()
{
 if (!m_timeline || m_entries.empty()) return;

 if (QMessageBox::question(this, "Clear All Captions",
 QString("Remove all %1 caption(s) from the timeline?").arg(m_entries.size()),
 QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
 return;

 // Snapshot the current caption clips (clones) so undo can restore them.
 auto saved = std::make_shared<std::vector<std::unique_ptr<Clip>>>();
 for (const auto& e : m_entries)
 if (auto* c = findCaptionClipById(e.clipId)) saved->push_back(c->clone());
 if (saved->empty()) { emit clearAllRequested(); return; }

 CaptionsPanel* self = this;

 auto doIt = [self]() {
 Timeline* tl = self->m_timeline;
 if (!tl) return;
 Track* trk = tl->captionTrack();
 if (trk) {
 while (trk->clipCount() > 0)
 trk->removeClip(0);
 }
 self->refresh();
 emit self->captionEdited();
 };
 auto undoIt = [saved, self]() {
 Timeline* tl = self->m_timeline;
 if (!tl) return;
 Track* trk = tl->addCaptionTrack();
 for (auto& c : *saved)
 if (c) trk->addClip(c->clone());
 self->refresh();
 emit self->captionEdited();
 };

 if (m_commandStack) {
 m_commandStack->execute(std::make_unique<LambdaCommand>(
 "Clear All Captions", doIt, undoIt));
 } else {
 doIt();
 }

 emit clearAllRequested(); // external hook (kept for compatibility)
}

void CaptionsPanel::onFillGaps()
{
 if (!m_timeline) return;

 int row = m_captionList->currentRow();
 if (row < 0 || row >= static_cast<int>(m_entries.size())) return;

 const auto& entry = m_entries[row];
 CaptionClip* cc = entryClip(row);
 if (!cc) return;
 Track* track = m_timeline->track(entry.trackIndex);
 if (!track) return;

 // Find this clip's index in the track and check for a next clip
 size_t idx = track->findClipIndexById(cc->id());
 if (idx >= track->clipCount()) return;
 if (idx + 1 >= track->clipCount()) return; // no next clip, nothing to fill

 const Clip* nextClip = track->clip(idx + 1);
 int64_t gapEnd = nextClip->timelineIn();
 int64_t currentEnd = cc->timelineOut();
 if (gapEnd <= currentEnd) return; // no gap

 int64_t oldDuration = cc->duration();
 int64_t newDuration = gapEnd - cc->timelineIn();

 // Apply directly
 cc->setDuration(newDuration);

 CaptionsPanel* self = this;
 uint64_t clipId = cc->id();
 if (m_commandStack) {
 m_commandStack->pushWithoutExecute(std::make_unique<LambdaCommand>(
 "Fill Caption Gap",
 [clipId, newDuration, self]() { if (auto* c = self->findCaptionClipById(clipId)) { c->setDuration(newDuration); self->refresh(); emit self->captionEdited(); } },
 [clipId, oldDuration, self]() { if (auto* c = self->findCaptionClipById(clipId)) { c->setDuration(oldDuration); self->refresh(); emit self->captionEdited(); } }));
 }

 refresh();

 // Re-select the same caption
 for (int i = 0; i < static_cast<int>(m_entries.size()); ++i) {
 if (m_entries[i].clipId == clipId) {
 m_captionList->setCurrentRow(i);
 break;
 }
 }

 emit captionEdited();
}

// ─────────────────────────────────────────────────────────────────────────────
// Transcription — auto-generate caption clips from the timeline audio
// ─────────────────────────────────────────────────────────────────────────────
// True if `name` is a default auto track label (V1, A2, …) rather than a
// meaningful character/speaker name worth tagging captions with.
static bool isAutoTrackName(const std::string& name)
{
 if (name.empty()) return true;
 if (name.size() < 2) return true;
 if (name[0] != 'V' && name[0] != 'A') return false;
 for (size_t i = 1; i < name.size(); ++i)
 if (!std::isdigit(static_cast<unsigned char>(name[i]))) return false;
 return true;
}

// True if a track looks like a music / sound-effects track that should NOT be
// transcribed (matched on the track name — whisper otherwise hallucinates
// repeated lines over music).
static bool isMusicTrackName(const std::string& name)
{
 std::string n;
 n.reserve(name.size());
 for (char c : name) n += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
 static const char* kMusicKeywords[] = {
 "music", "bgm", "score", "soundtrack", "sfx", "fx",
 "ambient", "ambience", "foley", "noise"
 };
 for (const char* kw : kMusicKeywords)
 if (n.find(kw) != std::string::npos) return true;
 return false;
}

// Trim leading/trailing whitespace from a transcript line.
static std::string trimmedText(std::string s)
{
 while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
 while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
 return s;
}

// True for whisper's non-speech annotations — e.g. "[Music]", "(applause)",
// "♪ ... ♪". These should never become caption entries.
static bool isNonSpeechAnnotation(const std::string& t)
{
 if (t.empty()) return true;
 // Musical-note glyphs (UTF-8 for ♪ U+266A / ♫ U+266B).
 if (t.find("\xE2\x99\xAA") != std::string::npos ||
 t.find("\xE2\x99\xAB") != std::string::npos)
 return true;
 // Entirely wrapped in [] or () ⇒ a sound annotation, not dialogue.
 const char f = t.front(), b = t.back();
 if ((f == '[' && b == ']') || (f == '(' && b == ')')) return true;
 return false;
}

std::vector<CaptionTranscribeSource> CaptionsPanel::findTranscriptionSources() const
{
 std::vector<CaptionTranscribeSource> sources;
 if (!m_timeline) return sources;

 // Transcribe every audio clip and every video clip that carries audio,
 // on every track (so all characters get captions). Speaker = track name
 // when the track has a real (non-default) name. Music/SFX tracks are
 // skipped so whisper doesn't hallucinate captions over them.
 for (size_t ti = 0; ti < m_timeline->trackCount(); ++ti) {
 Track* track = m_timeline->track(ti);
 if (!track || track->isCaptionTrack() || track->isDivider()) continue;
 if (isMusicTrackName(track->name())) {
 spdlog::info("[Captions] skipping music track '{}'", track->name());
 continue; // don't transcribe music
 }

 std::string speaker;
 if (!isAutoTrackName(track->name())) speaker = track->name();

 int collected = 0;
 for (size_t ci = 0; ci < track->clipCount(); ++ci) {
 Clip* clip = track->clip(ci);
 if (!clip) continue;
 std::string path;
 if (clip->isAudio()) {
 path = static_cast<AudioClip*>(clip)->mediaPath();
 } else if (clip->clipType() == ClipType::Video) {
 auto* vc = static_cast<VideoClip*>(clip);
 if (vc->hasAudio()) path = vc->mediaPath();
 }
 if (path.empty()) {
 spdlog::warn("[Captions] track '{}' clip {} has no audio path "
 "(type={}) — skipped", track->name(), ci,
 static_cast<int>(clip->clipType()));
 continue;
 }
 ++collected;

 CaptionTranscribeSource src;
 src.path = path;
 src.timelineInTicks = clip->timelineIn();
 src.sourceInTicks   = clip->sourceIn();
 src.durationTicks   = clip->duration();
 src.speaker = speaker;
 sources.push_back(std::move(src));
 }
 spdlog::info("[Captions] track '{}' (type={}): {} audio clip(s) queued for transcription",
 track->name(), static_cast<int>(track->type()), collected);
 }
 spdlog::info("[Captions] {} total audio source(s) to transcribe", sources.size());
 return sources;
}

void CaptionsPanel::onAddCaptionsToTimeline()
{
 if (!m_timeline) return;

 // Already present — just make sure the panel/preview are in sync.
 if (m_timeline->captionTrack()) {
 refresh();
 emit captionEdited();
 return;
 }

 Timeline* tl = m_timeline;
 CaptionsPanel* self = this;
 auto doIt = [tl, self]() {
 tl->addCaptionTrack();
 self->refresh();
 emit self->captionEdited();
 };
 auto undoIt = [tl, self]() {
 for (size_t i = 0; i < tl->trackCount(); ++i) {
 if (tl->track(i)->isCaptionTrack()) { tl->removeTrack(i); break; }
 }
 self->refresh();
 emit self->captionEdited();
 };

 if (m_commandStack) {
 m_commandStack->execute(std::make_unique<LambdaCommand>(
 "Add Caption Track", doIt, undoIt));
 } else {
 doIt();
 }
}

void CaptionsPanel::onTranscribe()
{
 emit transcribeRequested(); // keep external hook (e.g. status wiring)

 if (m_transcribing) return;
 if (!m_timeline) return;

 std::vector<CaptionTranscribeSource> sources = findTranscriptionSources();
 if (sources.empty()) {
 QMessageBox::information(this, "Transcribe",
 "No audio found on the timeline to transcribe.\n"
 "Add an audio clip (or a video clip with audio) first.");
 return;
 }

 if (!m_transcriber) {
 m_transcriber = std::make_shared<Transcriber>();
 m_transcriber->setModelsDirectory(
 (rt::userDataDir() + "/models").toStdString());
 }

 m_transcribing = true;
 m_transcribeBtn->setEnabled(false);
 m_transcribeBtn->setText("Transcribing\xE2\x80\xA6");
 m_progressBar->setRange(0, 0);            // busy until the model is ready
 m_progressBar->setFormat("Loading model\xE2\x80\xA6");
 m_progressBar->setVisible(true);

 // Progress reporter — posts UI updates back to the panel safely.
 auto report = [this](int pct, const QString& status) {
 QMetaObject::invokeMethod(this, [this, pct, status]() {
 if (!m_progressBar) return;
 if (pct < 0) {                       // indeterminate / busy
 m_progressBar->setRange(0, 0);
 } else {
 m_progressBar->setRange(0, 100);
 m_progressBar->setValue(pct);
 }
 m_progressBar->setFormat(status);
 }, Qt::QueuedConnection);
 };

 // Capture a shared_ptr copy so the worker keeps the Transcriber alive even
 // if the panel is destroyed mid-run. Results are posted back via
 // invokeMethod(this, ...) which Qt drops if `this` was destroyed.
 std::shared_ptr<Transcriber> tr = m_transcriber;
 std::thread([this, tr, sources, report]() {
 if (!tr->isModelLoaded()) {
 report(-1, "Loading model\xE2\x80\xA6");
 tr->loadModel(WhisperModelSize::Base);
 }

 // Group clips by media path, and transcribe each UNIQUE file ONCE
 // (cache by path). Transcribing per-clip would re-run the whole file for
 // every split clip and stack identical cues.
 std::unordered_map<std::string, std::vector<const CaptionTranscribeSource*>> byPath;
 for (const auto& src : sources) byPath[src.path].push_back(&src);

 std::unordered_map<std::string, TranscriptionResult> cache;
 const int total = static_cast<int>(byPath.size());
 int idx = 0;
 for (auto& kv : byPath) {
 const int fileIdx = idx;
 auto fileProg = [report, fileIdx, total](float pct, const std::string&) {
 int overall = total > 0
 ? static_cast<int>((fileIdx + pct / 100.0f) / total * 100.0f)
 : 0;
 report(overall, QString("Transcribing\xE2\x80\xA6 %1%").arg(overall));
 };
 spdlog::info("[Captions] transcribing source '{}' ({} clip(s))",
 kv.first, kv.second.size());
 cache[kv.first] = tr->transcribe(kv.first, "", fileProg);
 ++idx;
 }

 // Map each file's cues onto the timeline for every clip that uses it.
 // Clips are slices of the source ([sourceIn, sourceIn+duration]); when a
 // file is SPLIT across multiple clips we keep each whisper segment that
 // OVERLAPS a clip's window (not just one whose start lands inside it — the
 // clip's manual boundaries never align with whisper's re-segmentation, so
 // a start-within test silently drops a character's lines), and clamp the
 // segment to that window. A file used by a single clip contributes all its
 // cues.
 std::vector<PreparedCaptionCue> cues;
 for (auto& kv : byPath) {
 const TranscriptionResult& result = cache[kv.first];
 const bool windowFilter = kv.second.size() > 1;
 for (const CaptionTranscribeSource* s : kv.second) {
 const int64_t srcStart = s->sourceInTicks;
 const int64_t srcEnd   = s->sourceInTicks + s->durationTicks;
 const int64_t clipEnd  = s->timelineInTicks + s->durationTicks;
 for (const auto& seg : result.segments) {
 std::string text = trimmedText(seg.text);
 if (text.empty() || isNonSpeechAnnotation(text)) continue; // skip [Music] etc.

 int64_t segStartSrc = static_cast<int64_t>(seg.start * kTicksPerSecond);
 int64_t segEndSrc   = static_cast<int64_t>(seg.end   * kTicksPerSecond);
 if (segEndSrc <= segStartSrc) segEndSrc = segStartSrc + kTicksPerSecond / 2;

 if (windowFilter && s->durationTicks > 0) {
 // Keep only segments that overlap this clip's source window,
 // then clamp them to it.
 if (segEndSrc <= srcStart || segStartSrc >= srcEnd) continue;
 if (segStartSrc < srcStart) segStartSrc = srcStart;
 if (segEndSrc   > srcEnd)   segEndSrc   = srcEnd;
 }

 PreparedCaptionCue cue;
 cue.inTick  = s->timelineInTicks + (segStartSrc - s->sourceInTicks);
 cue.outTick = s->timelineInTicks + (segEndSrc   - s->sourceInTicks);
 if (cue.inTick < 0) cue.inTick = 0;
 if (cue.outTick <= cue.inTick) cue.outTick = cue.inTick + kTicksPerSecond / 2;
 cue.clipEndTick = clipEnd;
 cue.text = text;
 cue.speaker = !seg.character.empty() ? seg.character : s->speaker;
 cues.push_back(std::move(cue));
 }
 }
 }

 // Sort by time and drop near-duplicate consecutive cues (whisper can
 // repeat a phrase many times over silence/non-speech).
 std::sort(cues.begin(), cues.end(),
 [](const PreparedCaptionCue& a, const PreparedCaptionCue& b) {
 return a.inTick < b.inTick;
 });
 std::vector<PreparedCaptionCue> deduped;
 deduped.reserve(cues.size());
 for (auto& c : cues) {
 if (!deduped.empty()) {
 auto& prev = deduped.back();
 // Same text + same speaker starting within 300 ms ⇒ duplicate
 // (e.g. a segment that overlapped two adjacent clips). Keep the
 // longer of the two so a boundary sliver doesn't replace the full
 // line.
 if (prev.text == c.text && prev.speaker == c.speaker &&
 std::llabs(c.inTick - prev.inTick) < (kTicksPerSecond * 3 / 10)) {
 if ((c.outTick - c.inTick) > (prev.outTick - prev.inTick))
 prev = c;
 continue;
 }
 }
 deduped.push_back(std::move(c));
 }

 // Close only SMALL gaps (< 1s): extend a caption to the next cue's start
 // so adjacent lines look continuous. Larger gaps are left alone — a caption
 // should not linger across real silence or a missed line.
 const int64_t kMaxBridge = kTicksPerSecond; // 1 second
 for (size_t i = 0; i + 1 < deduped.size(); ++i) {
 const int64_t gap = deduped[i + 1].inTick - deduped[i].outTick;
 if (gap > 0 && gap < kMaxBridge)
 deduped[i].outTick = deduped[i + 1].inTick;
 }

 QMetaObject::invokeMethod(this, [this, deduped]() {
 m_transcribing = false;
 m_transcribeBtn->setEnabled(true);
 m_transcribeBtn->setText("\xF0\x9F\x8E\x99 Transcribe");
 if (m_progressBar) m_progressBar->setVisible(false);
 if (deduped.empty()) {
 QMessageBox::warning(this, "Transcribe",
 "Transcription produced no results.");
 return;
 }
 applyPreparedCues(deduped);
 }, Qt::QueuedConnection);
 }).detach();
}

void CaptionsPanel::applyPreparedCues(const std::vector<PreparedCaptionCue>& cues)
{
 if (!m_timeline || cues.empty()) return;

 // Build the caption clips up-front (UI thread, so clip-id assignment is
 // safe), sorted by timeline position.
 auto clips = std::make_shared<std::vector<std::unique_ptr<CaptionClip>>>();
 for (const auto& cue : cues) {
 auto cc = std::make_unique<CaptionClip>();
 cc->setTimelineIn(cue.inTick);
 cc->setDuration(cue.outTick - cue.inTick);
 cc->setText(cue.text);
 if (!cue.speaker.empty()) cc->setSpeaker(cue.speaker);
 clips->push_back(std::move(cc));
 }
 if (clips->empty()) return;

 Timeline* tl = m_timeline;
 CaptionsPanel* self = this;
 // doIt clones the prototypes and records the ids actually added so undo
 // (and a subsequent redo) can find them — clone() assigns fresh ids.
 auto addedIds = std::make_shared<std::vector<uint64_t>>();

 auto doIt = [tl, clips, addedIds, self]() {
 addedIds->clear();
 Track* trk = tl->addCaptionTrack(); // creates or returns existing
 for (auto& c : *clips) {
 if (!c) continue;
 auto clone = c->clone();
 addedIds->push_back(clone->id());
 trk->addClip(std::move(clone));
 }
 self->refresh();
 emit self->captionEdited();
 };
 auto undoIt = [tl, addedIds, self]() {
 Track* trk = tl->captionTrack();
 if (!trk) return;
 for (uint64_t id : *addedIds) trk->removeClipById(id);
 addedIds->clear();
 self->refresh();
 emit self->captionEdited();
 };

 if (m_commandStack) {
 m_commandStack->execute(std::make_unique<LambdaCommand>(
 "Transcribe to Captions", doIt, undoIt));
 } else {
 doIt();
 }
}

void CaptionsPanel::updateButtonStates()
{
 bool hasCaptions = !m_entries.empty();
 bool hasSelection = m_captionList->currentRow() >= 0;

 m_deleteBtn->setEnabled(hasSelection);
 m_fillGapsBtn->setEnabled(hasSelection);
 m_clearAllBtn->setEnabled(hasCaptions);
 m_editorSection->setVisible(hasCaptions);
}

} // namespace rt
