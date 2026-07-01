/*
 * CaptionsPanel â€” captions/subtitle editor panel.
 *
 * Displays caption clips from the active sequence's caption tracks,
 * allows editing text/speaker/style, and provides a Transcribe button.
 */

#pragma once

#include "timeline/TimelineObserver.h"

#include <QWidget>
#include <QListWidget>
#include <QPushButton>
#include <QTextEdit>
#include <QLineEdit>
#include <QLabel>
#include <QComboBox>
#include <QSpinBox>
#include <QProgressBar>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace rt {

class Timeline;
class CaptionClip;
class Track;
class CommandStack;
class Transcriber;
struct TranscriptionResult;

/// One clip on the timeline to transcribe: its media file, where it sits on
/// the timeline, which slice of the source it shows, and a speaker label.
/// The source window lets us transcribe each unique file ONCE and map only
/// the cues each clip actually covers — so split clips of one recording don't
/// each re-emit the whole transcript (the "dozens of duplicate lines" bug).
struct CaptionTranscribeSource {
 std::string path;
 int64_t     timelineInTicks{0};
 int64_t     sourceInTicks{0};
 int64_t     durationTicks{0};
 std::string speaker;         ///< track name (empty if it's a default A#/V# name)
};

/// A finished caption cue prepared off-thread, ready to become a CaptionClip
/// on the UI thread (timeline-absolute ticks + speaker).
struct PreparedCaptionCue {
 int64_t     inTick{0};
 int64_t     outTick{0};
 int64_t     clipEndTick{0};  ///< end of the source audio clip — captions never extend past this
 std::string text;
 std::string speaker;
};

class CaptionsPanel : public QWidget, public TimelineObserver
{
 Q_OBJECT

public:
 explicit CaptionsPanel(QWidget* parent = nullptr);
 ~CaptionsPanel() override;

 void setTimeline(Timeline* timeline);

 /// TimelineObserver: the timeline (and every caption clip it owns) is being
 /// freed — drop all cached pointers before returning.
 void onTimelineDestroyed(Timeline* tl) override;
 void setCommandStack(CommandStack* stack) noexcept { m_commandStack = stack; }

 /// Rebuild the caption list from the active timeline.
 void refresh();

 /// Select and scroll to a specific caption clip by track/clip index.
 void selectCaption(size_t trackIndex, size_t clipIndex);

signals:
 /// Emitted when the user clicks "Transcribe" (selected clip or sequence).
 void transcribeRequested();

 /// Emitted when the user clicks "Clear All Captions".
 void clearAllRequested();

 /// Emitted when a caption is selected (for playhead jump).
 void captionSelected(int64_t timelineIn);

 /// Emitted when caption content changes.
 void captionEdited();

private slots:
 void onListSelectionChanged();
 void onTextChanged();
 void onSpeakerChanged();
 void onPositionChanged(int index);
 void onFontChanged(const QString& family);
 void onFontSizeChanged(int size);
 void onAddCaption();
 void onDeleteCaption();
 void onClearAll();
 void onFillGaps();
 void onTranscribe();
 void onAddCaptionsToTimeline();

private:
 void buildUI();
 void applyTheme();
 void updateButtonStates();

 /// Resolve a clip id to the live CaptionClip on the timeline (nullptr if it
 /// was deleted). Undo/redo lambdas capture ids, never Clip pointers.
 CaptionClip* findCaptionClipById(uint64_t clipId) const;

 /// Collect ALL audio sources on the timeline to transcribe (one per
 /// audio clip / video-clip-with-audio), each with its own offset + speaker.
 std::vector<CaptionTranscribeSource> findTranscriptionSources() const;

 /// Add prepared caption cues (from all sources, merged) to the caption
 /// track as one undoable batch.
 void applyPreparedCues(const std::vector<PreparedCaptionCue>& cues);

 /// Info about a caption clip shown in the list.
 struct CaptionEntry {
 size_t trackIndex{0};
 size_t clipIndex{0};
 CaptionClip* clip{nullptr};
 };

 Timeline* m_timeline{nullptr};
 CommandStack* m_commandStack{nullptr};

 // UI widgets
 QListWidget* m_captionList{nullptr};
 QTextEdit* m_textEdit{nullptr};
 QLineEdit* m_speakerEdit{nullptr};
 QComboBox* m_positionCombo{nullptr};
 QComboBox* m_fontCombo{nullptr};
 QSpinBox* m_fontSizeSpin{nullptr};
 QPushButton* m_transcribeBtn{nullptr};
 QPushButton* m_addTrackBtn{nullptr};
 QPushButton* m_clearAllBtn{nullptr};
 QPushButton* m_addBtn{nullptr};
 QPushButton* m_deleteBtn{nullptr};
 QPushButton* m_fillGapsBtn{nullptr};
 QProgressBar* m_progressBar{nullptr};
 QLabel* m_timeLabel{nullptr};
 QLabel* m_countLabel{nullptr};
 QWidget* m_editorSection{nullptr};

 // Data
 std::vector<CaptionEntry> m_entries;
 bool m_updatingUI{false};

 // Transcription (whisper) — owned via shared_ptr so a detached worker
 // thread can keep it alive past panel destruction.
 std::shared_ptr<Transcriber> m_transcriber;
 bool m_transcribing{false};
};

} // namespace rt
