/*
 * MonitorUiKit — shared widget builders for ProgramMonitor / SourceMonitor.
 *
 * The two monitors share Premiere-style chrome: the control bar, timecode
 * label/edit pair, fit-mode and playback-resolution combos, safe-area
 * button, export-frame button, zoom/duration labels, and the transport
 * bar.  These used to be duplicated verbatim in ProgramMonitorUI.cpp and
 * SourceMonitorUI.cpp (~160 duplicated blocks).  The kit builds the
 * widgets; each monitor keeps its own signal connections and behavior.
 */

#pragma once

#include <QColor>
#include <QIcon>
#include <QString>

class QComboBox;
class QFrame;
class QHBoxLayout;
class QLabel;
class QLineEdit;
class QPushButton;
class QWidget;

namespace rt {

class TransportButton;

namespace monitorui {

// ── Shared style sheets (already UiScale-scaled) ────────────────────────
[[nodiscard]] QString comboStyle();
[[nodiscard]] QString checkedButtonStyle();
[[nodiscard]] QString exportButtonStyle();
[[nodiscard]] QString loopButtonStyle();

// ── Individual controls ─────────────────────────────────────────────────
/// Thin background-colored spacer between viewport and controls.
[[nodiscard]] QWidget* makeViewSpacer(QWidget* parent, int heightPx);

/// "#ControlBar" container + its HBox layout (Premiere-style info bar).
[[nodiscard]] QWidget* makeControlBar(QWidget* parent, QHBoxLayout** outLayout);

/// Accent-blue monospace timecode display (click-to-edit pattern: the caller
/// installs its event filter and pairs it with makeTimecodeEdit()).
[[nodiscard]] QLabel* makeTimecodeLabel(QWidget* parent);

/// Hidden HH:MM:SS:FF line edit shown when the timecode label is clicked.
[[nodiscard]] QLineEdit* makeTimecodeEdit(QWidget* parent);

/// Fit / Fill / 25–200% zoom presets combo (8 items, indices match both
/// monitors' onFitModeChanged handlers).
[[nodiscard]] QComboBox* makeFitModeCombo(QWidget* parent);

/// Playback resolution combo: Full, 1/2, 1/4, 1/8 (+ Auto when withAuto).
[[nodiscard]] QComboBox* makePlaybackResCombo(QWidget* parent, bool withAuto);

/// Premiere-style nested-rectangles safe-margins icon.
[[nodiscard]] QIcon makeSafeAreaIcon(QColor fg);

/// Checkable safe-area toggle button (caller connects toggled()).
[[nodiscard]] QPushButton* makeSafeAreaButton(QWidget* parent);

/// Line-icon export-frame button (caller connects clicked()).
[[nodiscard]] QPushButton* makeExportFrameButton(QWidget* parent);

/// Hidden zoom-percentage label (updated from viewZoomChanged).
[[nodiscard]] QLabel* makeZoomLabel(QWidget* parent);

/// Right-aligned duration timecode label.
[[nodiscard]] QLabel* makeDurationLabel(QWidget* parent);

/// Slim vertical divider used inside the transport bar.
[[nodiscard]] QFrame* makeVDivider(QWidget* parent);

// ── Transport bar ────────────────────────────────────────────────────────
/// Shared playback controls appended to an existing monitor control row.
/// Export Frame remains a single distinct action in that same row.
struct TransportBarKit {
    QWidget*         bar{nullptr};
    QHBoxLayout*     layout{nullptr};
    TransportButton* goStart{nullptr};
    TransportButton* stepBack{nullptr};
    TransportButton* playPause{nullptr};
    TransportButton* stop{nullptr};
    TransportButton* stepForward{nullptr};
    TransportButton* goEnd{nullptr};
    QPushButton*     loop{nullptr};
    QLabel*          shuttleSpeed{nullptr};
};
[[nodiscard]] TransportBarKit makeTransportBar(QWidget* parent,
                                               QHBoxLayout* layout);

} // namespace monitorui
} // namespace rt
