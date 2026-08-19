/*
 * ShortcutManager — Keyboard shortcut management for the NLE.
 *
 * Step 13: Maps keyboard shortcuts to editing actions and tools.
 *
 * Provides:
 *   - Shortcut registration (key + modifiers → action name)
 *   - Action registration (name → callback)
 *   - Tool shortcuts (A/B/R/N/Y/U for edit tools)
 *   - NLE-standard defaults matching the original Python app
 *   - Customizable key bindings
 */

#pragma once

#include <QKeySequence>
#include <QObject>
#include <QString>

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace rt {

/// Identifies an action that can be triggered by a shortcut.
struct ShortcutAction
{
    QString                  id;          ///< Unique action identifier
    QString                  displayName; ///< Human-readable name
    QKeySequence             defaultKey;  ///< Default shortcut
    QKeySequence             currentKey;  ///< Current (possibly customized) shortcut
    std::function<void()>    callback;    ///< Action callback
    QString                  category;    ///< UI grouping (e.g. "Timeline", "Transport")
    bool                     enabled{true};
};

/// Manages keyboard shortcuts for the entire application.
class ShortcutManager : public QObject
{
    Q_OBJECT

public:
    explicit ShortcutManager(QObject* parent = nullptr);
    ~ShortcutManager() override;

    // ── Action registration ─────────────────────────────────────────────

    /// Register an action with a default shortcut.
    void registerAction(const QString& id,
                        const QString& displayName,
                        const QKeySequence& defaultKey,
                        std::function<void()> callback,
                        const QString& category = "General");

    /// Remove an action by ID.
    void removeAction(const QString& id);

    /// Set a custom shortcut for an action.
    void setShortcut(const QString& actionId, const QKeySequence& key);

    /// Reset an action to its default shortcut.
    void resetToDefault(const QString& actionId);

    /// Reset all actions to default shortcuts.
    void resetAllToDefaults();

    // ── Lookup ──────────────────────────────────────────────────────────

    /// Find an action by ID. Returns nullptr if not found.
    [[nodiscard]] const ShortcutAction* action(const QString& id) const;

    /// Find all actions in a category.
    [[nodiscard]] std::vector<const ShortcutAction*> actionsInCategory(
        const QString& category) const;

    /// Get all registered action IDs.
    [[nodiscard]] std::vector<QString> actionIds() const;

    /// Get all categories.
    [[nodiscard]] std::vector<QString> categories() const;

    // ── Key handling ────────────────────────────────────────────────────

    /// Attempt to handle a key event. Returns true if a matching action was found.
    bool handleKeyPress(int key, Qt::KeyboardModifiers modifiers);

    /// Test a key event against one registered action's current binding.
    [[nodiscard]] bool matches(const QString& actionId, int key,
                               Qt::KeyboardModifiers modifiers) const;

    /// Trigger an action by ID without synthesizing a key event.  Menus and
    /// toolbar buttons use this so they share the exact callback registered by
    /// the customizable keyboard-shortcut path without changing its binding.
    bool triggerAction(const QString& actionId);

    /// Enable/disable an action.
    void setActionEnabled(const QString& id, bool enabled);

    /// Update the callback for an already-registered action.
    /// Does NOT change the key binding or display name.
    void setActionCallback(const QString& id, std::function<void()> callback);

    // ── NLE defaults ────────────────────────────────────────────────────

    /// Register all standard NLE keyboard shortcuts.
    /// This sets up the default bindings matching typical NLE workflows.
    void registerNLEDefaults();

    // ── Persistence ─────────────────────────────────────────────────────

    /// Save all customized shortcuts to QSettings.
    void saveToSettings() const;

    /// Load customized shortcuts from QSettings (call after registerNLEDefaults).
    void loadFromSettings();

    /// Export current shortcuts to a JSON file. Returns true on success.
    bool exportToFile(const QString& filePath) const;

    /// Import shortcuts from a JSON file. Returns true on success.
    bool importFromFile(const QString& filePath);

    // ── Action ID constants ─────────────────────────────────────────────

    // Timeline editing
    static constexpr const char* kCut       = "edit.cut";
    static constexpr const char* kCopy      = "edit.copy";
    static constexpr const char* kPaste     = "edit.paste";
    static constexpr const char* kDelete    = "edit.delete";
    static constexpr const char* kRippleDel = "edit.ripple_delete";
    static constexpr const char* kDuplicate = "edit.duplicate";
    static constexpr const char* kSelectAll = "edit.select_all";
    static constexpr const char* kSplitAt   = "edit.split";
    static constexpr const char* kSplitAll   = "edit.split_all";
    static constexpr const char* kToggleSnapping = "timeline.toggle_snapping";

    // Tools
    static constexpr const char* kToolSelection = "tool.selection";
    static constexpr const char* kToolRazor     = "tool.razor";
    static constexpr const char* kToolRolling   = "tool.rolling";
    static constexpr const char* kToolRipple    = "tool.ripple";
    static constexpr const char* kToolSlip      = "tool.slip";
    static constexpr const char* kToolSlide     = "tool.slide";
    static constexpr const char* kToolText      = "tool.text";
    static constexpr const char* kToolPen       = "tool.pen_mask";
    static constexpr const char* kToolZoom      = "tool.zoom";

    // Additional editing
    static constexpr const char* kMarkSelection = "edit.mark_selection";  ///< FCP7: X
    static constexpr const char* kMatchFrame    = "edit.match_frame";     ///< G (select clip under playhead)
    static constexpr const char* kAddMarker     = "edit.add_marker";      ///< FCP7: M

    // Transport
    static constexpr const char* kPlayPause    = "transport.play_pause";
    static constexpr const char* kStop         = "transport.stop";
    static constexpr const char* kFrameForward = "transport.frame_forward";
    static constexpr const char* kFrameBack    = "transport.frame_back";
    static constexpr const char* kNextEdit     = "transport.next_edit";
    static constexpr const char* kPrevEdit     = "transport.prev_edit";
    static constexpr const char* kGoStart      = "transport.go_start";
    static constexpr const char* kGoEnd        = "transport.go_end";

    // Paste variants
    static constexpr const char* kPasteInsert     = "edit.paste_insert";
    static constexpr const char* kPasteAttributes = "edit.paste_attributes";

    // In/Out
    static constexpr const char* kSetIn      = "io.set_in";
    static constexpr const char* kSetOut     = "io.set_out";
    static constexpr const char* kClearIO    = "io.clear";
    static constexpr const char* kClearIOAlt = "io.clear_alt";
    static constexpr const char* kGoIn       = "io.go_in";
    static constexpr const char* kGoOut      = "io.go_out";

    static constexpr const char* kDeselectAll       = "edit.deselect_all";
    static constexpr const char* kDefaultTransition = "edit.default_transition";
    static constexpr const char* kNewBin            = "project.new_bin";
    static constexpr const char* kZoomIn            = "timeline.zoom_in";
    static constexpr const char* kZoomOut           = "timeline.zoom_out";
    static constexpr const char* kZoomToFit         = "timeline.zoom_to_fit";
    static constexpr const char* kZoomInAlt         = "timeline.zoom_in_alt";
    static constexpr const char* kZoomOutAlt        = "timeline.zoom_out_alt";
    static constexpr const char* kZoomToFitAlt      = "timeline.zoom_to_fit_alt";
    static constexpr const char* kSelectAtPlayhead  = "edit.select_at_playhead";
    static constexpr const char* kPanelMaximize     = "window.maximize_panel";
    static constexpr const char* kOpenExport        = "window.open_export";

    // Undo/Redo
    static constexpr const char* kUndo = "edit.undo";
    static constexpr const char* kRedo = "edit.redo";

signals:
    /// Emitted when the active tool changes.
    void toolChanged(int toolIndex);

    /// Emitted when a shortcut is triggered.
    void actionTriggered(const QString& actionId);

    /// Emitted when a binding changes so live QShortcut instances update.
    void shortcutChanged(const QString& actionId, const QKeySequence& key);

private:
    std::unordered_map<QString, ShortcutAction> m_actions;

    /// Build a key from key + modifiers for lookup.
    static QString keyToString(int key, Qt::KeyboardModifiers mods);
};

} // namespace rt
