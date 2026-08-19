/*
 * ShortcutManager.cpp — Keyboard shortcut management implementation.
 * Step 13: Timeline Editing Tools
 */

#include "ShortcutManager.h"

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QSettings>

#include "Settings.h"

#include <algorithm>
#include <set>

namespace rt {

ShortcutManager::ShortcutManager(QObject* parent)
    : QObject(parent)
{
}

ShortcutManager::~ShortcutManager() = default;

// ── Action registration ─────────────────────────────────────────────────────

void ShortcutManager::registerAction(const QString& id,
                                      const QString& displayName,
                                      const QKeySequence& defaultKey,
                                      std::function<void()> callback,
                                      const QString& category)
{
    ShortcutAction action;
    action.id          = id;
    action.displayName = displayName;
    action.defaultKey  = defaultKey;
    action.currentKey  = defaultKey;
    action.callback    = std::move(callback);
    action.category    = category;

    m_actions[id] = std::move(action);
}

void ShortcutManager::removeAction(const QString& id)
{
    m_actions.erase(id);
}

void ShortcutManager::setShortcut(const QString& actionId, const QKeySequence& key)
{
    auto it = m_actions.find(actionId);
    if (it != m_actions.end()) {
        it->second.currentKey = key;
        emit shortcutChanged(actionId, key);
    }
}

void ShortcutManager::resetToDefault(const QString& actionId)
{
    auto it = m_actions.find(actionId);
    if (it != m_actions.end()) {
        it->second.currentKey = it->second.defaultKey;
        emit shortcutChanged(actionId, it->second.currentKey);
    }
}

void ShortcutManager::resetAllToDefaults()
{
    for (auto& [id, action] : m_actions) {
        action.currentKey = action.defaultKey;
        emit shortcutChanged(id, action.currentKey);
    }
}

// ── Lookup ──────────────────────────────────────────────────────────────────

const ShortcutAction* ShortcutManager::action(const QString& id) const
{
    auto it = m_actions.find(id);
    return it != m_actions.end() ? &it->second : nullptr;
}

std::vector<const ShortcutAction*> ShortcutManager::actionsInCategory(
    const QString& category) const
{
    std::vector<const ShortcutAction*> result;
    for (const auto& [id, action] : m_actions)
    {
        if (action.category == category)
            result.push_back(&action);
    }
    return result;
}

std::vector<QString> ShortcutManager::actionIds() const
{
    std::vector<QString> result;
    result.reserve(m_actions.size());
    for (const auto& [id, _] : m_actions)
        result.push_back(id);
    return result;
}

std::vector<QString> ShortcutManager::categories() const
{
    std::set<QString> cats;
    for (const auto& [_, action] : m_actions)
        cats.insert(action.category);
    return {cats.begin(), cats.end()};
}

// ── Key handling ────────────────────────────────────────────────────────────

bool ShortcutManager::handleKeyPress(int key, Qt::KeyboardModifiers modifiers)
{
    QKeySequence pressed(static_cast<int>(key) | static_cast<int>(modifiers));

    for (const auto& [id, action] : m_actions)
    {
        if (!action.enabled || action.currentKey.isEmpty())
            continue;

        if (action.currentKey == pressed && action.callback)
        {
            action.callback();
            emit actionTriggered(id);
            return true;
        }
    }
    return false;
}

bool ShortcutManager::matches(const QString& actionId, int key,
                              Qt::KeyboardModifiers modifiers) const
{
    const auto it = m_actions.find(actionId);
    if (it == m_actions.end() || !it->second.enabled
        || it->second.currentKey.isEmpty())
        return false;
    return it->second.currentKey
        == QKeySequence(static_cast<int>(key) | static_cast<int>(modifiers));
}

bool ShortcutManager::triggerAction(const QString& actionId)
{
    auto it = m_actions.find(actionId);
    if (it == m_actions.end() || !it->second.enabled || !it->second.callback)
        return false;

    it->second.callback();
    emit actionTriggered(actionId);
    return true;
}

void ShortcutManager::setActionEnabled(const QString& id, bool enabled)
{
    auto it = m_actions.find(id);
    if (it != m_actions.end())
        it->second.enabled = enabled;
}

void ShortcutManager::setActionCallback(const QString& id, std::function<void()> callback)
{
    auto it = m_actions.find(id);
    if (it != m_actions.end())
        it->second.callback = std::move(callback);
}

// ── NLE defaults ────────────────────────────────────────────────────────────

void ShortcutManager::registerNLEDefaults()
{
    // Note: Callbacks are registered as empty — the panel wires them up later.
    auto empty = []() {};

    // Timeline editing
    registerAction(kCut, "Cut", QKeySequence(Qt::CTRL | Qt::Key_X), empty, "Edit");
    registerAction(kCopy, "Copy", QKeySequence(Qt::CTRL | Qt::Key_C), empty, "Edit");
    registerAction(kPaste, "Paste", QKeySequence(Qt::CTRL | Qt::Key_V), empty, "Edit");
    registerAction(kDelete, "Delete", QKeySequence(Qt::Key_Delete), empty, "Edit");
    registerAction(kRippleDel, "Ripple Delete", QKeySequence(Qt::SHIFT | Qt::Key_Delete), empty, "Edit");
    registerAction(kDuplicate, "Duplicate", QKeySequence(Qt::CTRL | Qt::Key_D), empty, "Edit");
    registerAction(kSelectAll, "Select All", QKeySequence(Qt::CTRL | Qt::Key_A), empty, "Edit");
    registerAction(kSplitAt, "Split Selected Clips", QKeySequence(Qt::Key_F), empty, "Edit");
    registerAction(kSplitAll, "Split All Tracks", QKeySequence(Qt::SHIFT | Qt::Key_F), empty, "Edit");
    registerAction(kToggleSnapping, "Toggle Snapping", QKeySequence(Qt::Key_N), empty, "Timeline");

    // Tools (FCP7 bindings)
    registerAction(kToolSelection, "Selection Tool", QKeySequence(Qt::Key_A), empty, "Tools");
    registerAction(kToolRazor, "Razor Tool", QKeySequence(Qt::Key_B), empty, "Tools");
    registerAction(kToolRolling, "Rolling Edit Tool", QKeySequence(Qt::Key_R), empty, "Tools");
    registerAction(kToolRipple, "Ripple Edit Tool", QKeySequence(), empty, "Tools");
    registerAction(kToolSlip, "Slip Tool", QKeySequence(Qt::Key_S), empty, "Tools");
    registerAction(kToolSlide, "Slide Tool", QKeySequence(Qt::Key_U), empty, "Tools");
    registerAction(kToolText, "Text Tool", QKeySequence(Qt::Key_T), empty, "Tools");
    registerAction(kToolPen, "Pen Mask Tool", QKeySequence(Qt::Key_P), empty, "Tools");
    registerAction(kToolZoom, "Zoom Tool", QKeySequence(Qt::Key_Z), empty, "Tools");

    // Additional editing (FCP7)
    registerAction(kMarkSelection, "Mark Selection", QKeySequence(Qt::Key_X), empty, "Edit");
    registerAction(kMatchFrame, "Match Frame", QKeySequence(Qt::Key_G), empty, "Edit");
    registerAction(kAddMarker, "Add Marker", QKeySequence(Qt::Key_M), empty, "Edit");

    // Transport
    registerAction(kPlayPause, "Play/Pause", QKeySequence(Qt::Key_Space), empty, "Transport");
    registerAction(kFrameForward, "Step Forward", QKeySequence(Qt::Key_Right), empty, "Transport");
    registerAction(kFrameBack, "Step Backward", QKeySequence(Qt::Key_Left), empty, "Transport");
    registerAction(kNextEdit, "Next Edit Point", QKeySequence(Qt::Key_Down), empty, "Transport");
    registerAction(kPrevEdit, "Previous Edit Point", QKeySequence(Qt::Key_Up), empty, "Transport");
    registerAction(kGoStart, "Go to Start", QKeySequence(Qt::Key_Home), empty, "Transport");
    registerAction(kGoEnd, "Go to End", QKeySequence(Qt::Key_End), empty, "Transport");

    // In/Out — NOTE: I/O/X keys are handled directly by TimelineWorkspace::keyPressEvent
    // so they fire regardless of which panel has focus.  Empty callbacks here would
    // silently consume the events in TimelinePanel::keyPressEvent, preventing the
    // workspace handler from ever seeing them.  Register with no key binding instead.
    registerAction(kPasteInsert, "Paste Insert", QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_C), empty, "Edit");
    registerAction(kPasteAttributes, "Paste Attributes", QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_V), empty, "Edit");

    registerAction(kSetIn, "Set In Point", QKeySequence(), empty, "In/Out");
    registerAction(kSetOut, "Set Out Point", QKeySequence(), empty, "In/Out");
    registerAction(kClearIO, "Clear In/Out", QKeySequence(Qt::ALT | Qt::Key_X), empty, "In/Out");
    registerAction(kClearIOAlt, "Clear In/Out (Alternate)", QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_X), empty, "In/Out");
    registerAction(kGoIn, "Go to In Point", QKeySequence(Qt::SHIFT | Qt::Key_I), empty, "In/Out");
    registerAction(kGoOut, "Go to Out Point", QKeySequence(Qt::SHIFT | Qt::Key_O), empty, "In/Out");

    registerAction(kDeselectAll, "Deselect All", QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_A), empty, "Edit");
    registerAction(kDefaultTransition, "Apply Default Transition", QKeySequence(Qt::CTRL | Qt::Key_T), empty, "Edit");
    registerAction(kNewBin, "New Bin", QKeySequence(Qt::CTRL | Qt::Key_B), empty, "Project");
    registerAction(kZoomIn, "Zoom In", QKeySequence(Qt::CTRL | Qt::Key_Equal), empty, "Timeline");
    registerAction(kZoomOut, "Zoom Out", QKeySequence(Qt::CTRL | Qt::Key_Minus), empty, "Timeline");
    registerAction(kZoomToFit, "Zoom to Fit", QKeySequence(Qt::SHIFT | Qt::Key_Z), empty, "Timeline");
    registerAction(kZoomInAlt, "Zoom In (Alternate)", QKeySequence(Qt::Key_Equal), empty, "Timeline");
    registerAction(kZoomOutAlt, "Zoom Out (Alternate)", QKeySequence(Qt::Key_Minus), empty, "Timeline");
    registerAction(kZoomToFitAlt, "Zoom to Fit (Alternate)", QKeySequence(Qt::Key_Backslash), empty, "Timeline");
    registerAction(kSelectAtPlayhead, "Select Clip at Playhead", QKeySequence(Qt::Key_D), empty, "Edit");
    registerAction(kPanelMaximize, "Maximize Panel", QKeySequence(Qt::Key_QuoteLeft), empty, "Window");
    registerAction(kOpenExport, "Open Export", QKeySequence(Qt::CTRL | Qt::Key_E), empty, "Window");

    // Undo/Redo
    registerAction(kUndo, "Undo", QKeySequence(Qt::CTRL | Qt::Key_Z), empty, "Edit");
    registerAction(kRedo, "Redo", QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Z), empty, "Edit");
}

QString ShortcutManager::keyToString(int key, Qt::KeyboardModifiers mods)
{
    return QKeySequence(key | static_cast<int>(mods)).toString();
}

// ── Persistence ─────────────────────────────────────────────────────────────

void ShortcutManager::saveToSettings() const
{
    auto s = rt::appSettings();
    s.beginGroup("Shortcuts");
    s.remove("");  // clear group first
    for (auto& [id, act] : m_actions) {
        if (act.currentKey != act.defaultKey)
            s.setValue(id, act.currentKey.toString());
    }
    s.endGroup();
}

void ShortcutManager::loadFromSettings()
{
    auto s = rt::appSettings();
    s.beginGroup("Shortcuts");
    for (const auto& key : s.childKeys()) {
        auto it = m_actions.find(key);
        if (it != m_actions.end()) {
            it->second.currentKey = QKeySequence(s.value(key).toString());
            emit shortcutChanged(key, it->second.currentKey);
        }
    }
    s.endGroup();
}

bool ShortcutManager::exportToFile(const QString& filePath) const
{
    QJsonObject root;
    for (auto& [id, act] : m_actions)
        root.insert(id, act.currentKey.toString());

    QFile file(filePath);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    return true;
}

bool ShortcutManager::importFromFile(const QString& filePath)
{
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly))
        return false;

    auto doc = QJsonDocument::fromJson(file.readAll());
    if (!doc.isObject())
        return false;

    QJsonObject root = doc.object();
    for (auto it = root.begin(); it != root.end(); ++it) {
        auto aid = m_actions.find(it.key());
        if (aid != m_actions.end()) {
            aid->second.currentKey = QKeySequence(it.value().toString());
            emit shortcutChanged(it.key(), aid->second.currentKey);
        }
    }
    return true;
}

} // namespace rt
