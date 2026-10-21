/*
 * MonitorUiKit — shared widget builders for ProgramMonitor / SourceMonitor.
 *
 * Both monitors follow Premiere's layout. Under the picture, top to bottom:
 *
 *   info row    timecode · zoom (Fit) · [middle] · resolution (Full) · wrench · duration
 *   mini ruler  MiniTimeline
 *   button row  centred groups of TransportButtons
 *
 *   Program: Add Marker, Mark In, Mark Out | Go to In, Step Back, Play,
 *            Step Forward, Go to Out | Lift, Extract | Export Frame
 *   Source:  Add Marker, Mark In, Mark Out | Go to In, Step Back, Play,
 *            Step Forward, Go to Out | Export Frame, Mute
 *
 * The wrench opens the monitor's settings menu (Loop, Safe Margins). The
 * kit builds the widgets; each monitor keeps its own signal connections and
 * behavior.
 */

#pragma once

#include <QString>

class QAction;
class QComboBox;
class QHBoxLayout;
class QLabel;
class QLineEdit;
class QMenu;
class QWidget;

namespace rt {

class ShortcutManager;
class TransportButton;

namespace monitorui {

// ── Shared style sheets (already UiScale-scaled) ────────────────────────
[[nodiscard]] QString comboStyle();

// ── Individual controls ─────────────────────────────────────────────────
/// Thin background-colored spacer between viewport and controls.
[[nodiscard]] QWidget* makeViewSpacer(QWidget* parent, int heightPx);

/// Info row container (timecode, combos, wrench, duration) + its layout.
[[nodiscard]] QWidget* makeInfoRow(QWidget* parent, QHBoxLayout** outLayout);

/// Button row container + its layout, with the left stretch already added:
/// the caller adds its groups (addButtonGroupGap() between them), then
/// finishButtonRow().
[[nodiscard]] QWidget* makeButtonRow(QWidget* parent, QHBoxLayout** outLayout);
void addButtonGroupGap(QHBoxLayout* layout);
void finishButtonRow(QHBoxLayout* layout);

/// One button-row button. `name` is Premiere's button name; the tooltip is
/// "name (key)", the key coming from `actionId`'s current binding (see
/// applyShortcutTooltips) or `fixedKey` for keys that are not rebindable.
[[nodiscard]] TransportButton* makeRowButton(QWidget* parent, int icon,
                                             const QString& name,
                                             const QString& actionId = {},
                                             const QString& fixedKey = {});

/// Re-reads every row button's key from `shortcuts` (and again whenever a
/// binding changes) so the tooltips show the user's own keys.
void applyShortcutTooltips(QWidget* root, const ShortcutManager* shortcuts);

/// The wrench and its settings menu (Loop, Safe Margins; both checkable).
struct SettingsMenuKit {
    TransportButton* button{nullptr};
    QMenu*           menu{nullptr};
    QAction*         loop{nullptr};
    QAction*         safeMargins{nullptr};
};
[[nodiscard]] SettingsMenuKit makeSettingsMenu(QWidget* parent);

/// Shuttle speed readout ("2x"), hidden until shuttling.
[[nodiscard]] QLabel* makeShuttleSpeedLabel(QWidget* parent);

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

/// Hidden zoom-percentage label (updated from viewZoomChanged).
[[nodiscard]] QLabel* makeZoomLabel(QWidget* parent);

/// Right-aligned duration timecode label.
[[nodiscard]] QLabel* makeDurationLabel(QWidget* parent);

// ── The playback group shared by both button rows ───────────────────────
/// Go to In, Step Back 1 Frame, Play-Stop Toggle, Step Forward 1 Frame,
/// Go to Out, appended to `layout`.
struct PlaybackGroupKit {
    TransportButton* goToIn{nullptr};
    TransportButton* stepBack{nullptr};
    TransportButton* playPause{nullptr};
    TransportButton* stepForward{nullptr};
    TransportButton* goToOut{nullptr};
};
[[nodiscard]] PlaybackGroupKit addPlaybackGroup(QWidget* parent, QHBoxLayout* layout);

/// Add Marker, Mark In, Mark Out, appended to `layout`.
struct MarkGroupKit {
    TransportButton* addMarker{nullptr};
    TransportButton* markIn{nullptr};
    TransportButton* markOut{nullptr};
};
[[nodiscard]] MarkGroupKit addMarkGroup(QWidget* parent, QHBoxLayout* layout);

} // namespace monitorui
} // namespace rt
