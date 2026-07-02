/*
 * KeyboardShortcutsDialog — implementation.
 */

#include "dialogs/KeyboardShortcutsDialog.h"
#include "ShortcutManager.h"
#include "Settings.h"
#include "Theme.h"

#include <QBoxLayout>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QTreeWidget>
#include <QTreeWidgetItem>

namespace rt {

KeyboardShortcutsDialog::KeyboardShortcutsDialog(ShortcutManager& manager,
                                                   QWidget* parent)
    : QDialog(parent)
    , m_manager(manager)
{
    setWindowTitle("Keyboard Shortcuts");
    resize(600, 500);
    buildUI();
    populateTree();
}

void KeyboardShortcutsDialog::buildUI()
{
    auto* layout = new QVBoxLayout(this);

    // Search bar
    m_searchEdit = new QLineEdit(this);
    m_searchEdit->setPlaceholderText("Filter shortcuts...");
    m_searchEdit->setClearButtonEnabled(true);
    connect(m_searchEdit, &QLineEdit::textChanged,
            this, &KeyboardShortcutsDialog::filterTree);
    layout->addWidget(m_searchEdit);

    // Tree widget
    m_tree = new QTreeWidget(this);
    m_tree->setHeaderLabels({"Action", "Shortcut", "Default"});
    m_tree->header()->setStretchLastSection(false);
    m_tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_tree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_tree->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_tree->setRootIsDecorated(true);
    m_tree->setAlternatingRowColors(true);
    connect(m_tree, &QTreeWidget::itemDoubleClicked,
            this, &KeyboardShortcutsDialog::onItemDoubleClicked);
    layout->addWidget(m_tree);

    // Hint label
    auto* hint = new QLabel("Double-click a shortcut to change it. Press Escape to cancel.", this);
    hint->setStyleSheet("color: #888;");
    layout->addWidget(hint);

    // Buttons
    auto* btnLayout = new QHBoxLayout();
    m_resetBtn = new QPushButton("Reset Selected", this);
    m_resetAllBtn = new QPushButton("Reset All", this);
    connect(m_resetBtn, &QPushButton::clicked, this, &KeyboardShortcutsDialog::resetSelected);
    connect(m_resetAllBtn, &QPushButton::clicked, this, &KeyboardShortcutsDialog::resetAll);
    btnLayout->addWidget(m_resetBtn);
    btnLayout->addWidget(m_resetAllBtn);

    auto* importBtn = new QPushButton("Import...", this);
    importBtn->setToolTip("Import shortcut presets from a JSON file");
    connect(importBtn, &QPushButton::clicked, this, [this]() {
        auto settings = rt::appSettings();
        QString lastDir = settings.value("Import/lastDir", QString()).toString();
        if (lastDir.isEmpty())
            lastDir = QDir::homePath();
        QString path = QFileDialog::getOpenFileName(
            this, "Import Shortcuts", lastDir,
            "JSON Files (*.json);;All Files (*)");
        if (path.isEmpty()) return;
        QString dir = QFileInfo(path).absolutePath();
        settings.setValue("Import/lastDir", dir);
        settings.sync();
        if (m_manager.importFromFile(path)) {
            // PopulateTree destroys all tree items — clear the capture
            // pointer so finishCapture() doesn't dereference freed memory.
            m_capturingItem = nullptr;
            m_capturing = false;
            populateTree();
            m_manager.saveToSettings();
        } else {
            QMessageBox::warning(this, "Import Failed",
                "Could not read the selected shortcut preset file.");
        }
    });
    btnLayout->addWidget(importBtn);

    auto* exportBtn = new QPushButton("Export...", this);
    exportBtn->setToolTip("Export current shortcuts to a JSON file");
    connect(exportBtn, &QPushButton::clicked, this, [this]() {
        QString path = QFileDialog::getSaveFileName(
            this, "Export Shortcuts", "shortcuts.json",
            "JSON Files (*.json);;All Files (*)");
        if (path.isEmpty()) return;
        if (!m_manager.exportToFile(path)) {
            QMessageBox::warning(this, "Export Failed",
                "Could not write the shortcut preset file.");
        }
    });
    btnLayout->addWidget(exportBtn);

    btnLayout->addStretch();

    auto* closeBtn = new QPushButton("Close", this);
    connect(closeBtn, &QPushButton::clicked, this, &QDialog::accept);
    btnLayout->addWidget(closeBtn);
    layout->addLayout(btnLayout);
}

void KeyboardShortcutsDialog::populateTree()
{
    m_tree->clear();

    auto cats = m_manager.categories();
    for (const auto& cat : cats) {
        auto* catItem = new QTreeWidgetItem(m_tree);
        catItem->setText(0, cat);
        catItem->setFlags(catItem->flags() & ~Qt::ItemIsSelectable);
        QFont f = catItem->font(0);
        f.setBold(true);
        catItem->setFont(0, f);

        auto actions = m_manager.actionsInCategory(cat);
        for (const auto* action : actions) {
            auto* item = new QTreeWidgetItem(catItem);
            item->setText(0, action->displayName);
            item->setText(1, action->currentKey.toString(QKeySequence::NativeText));
            item->setText(2, action->defaultKey.toString(QKeySequence::NativeText));
            item->setData(0, Qt::UserRole, action->id);

            // Highlight customized shortcuts
            if (action->currentKey != action->defaultKey) {
                item->setForeground(1, Theme::colors().success); // green
            }
        }

        catItem->setExpanded(true);
    }
}

void KeyboardShortcutsDialog::filterTree(const QString& text)
{
    for (int i = 0; i < m_tree->topLevelItemCount(); ++i) {
        auto* catItem = m_tree->topLevelItem(i);
        bool anyVisible = false;

        for (int j = 0; j < catItem->childCount(); ++j) {
            auto* child = catItem->child(j);
            bool visible = text.isEmpty()
                || child->text(0).contains(text, Qt::CaseInsensitive)
                || child->text(1).contains(text, Qt::CaseInsensitive);
            child->setHidden(!visible);
            if (visible) anyVisible = true;
        }

        catItem->setHidden(!anyVisible);
    }
}

void KeyboardShortcutsDialog::onItemDoubleClicked(QTreeWidgetItem* item, int column)
{
    (void)column;
    // Only act on action items (not category headers)
    if (!item->data(0, Qt::UserRole).isValid()) return;

    m_capturing = true;
    m_capturingItem = item;
    item->setText(1, "Press a key...");
}

void KeyboardShortcutsDialog::keyPressEvent(QKeyEvent* event)
{
    if (!m_capturing) {
        QDialog::keyPressEvent(event);
        return;
    }

    int key = event->key();

    // Ignore pure modifier presses
    if (key == Qt::Key_Shift || key == Qt::Key_Control
        || key == Qt::Key_Alt || key == Qt::Key_Meta) {
        return;
    }

    if (key == Qt::Key_Escape) {
        cancelCapture();
        return;
    }

    QKeySequence seq(static_cast<int>(event->modifiers()) | key);
    finishCapture(seq);
}

void KeyboardShortcutsDialog::finishCapture(const QKeySequence& key)
{
    if (!m_capturingItem) return;

    QString actionId = m_capturingItem->data(0, Qt::UserRole).toString();
    m_manager.setShortcut(actionId, key);

    m_capturingItem->setText(1, key.toString(QKeySequence::NativeText));

    // Highlight if customized
    const auto* action = m_manager.action(actionId);
    if (action && action->currentKey != action->defaultKey) {
        m_capturingItem->setForeground(1, Theme::colors().success);
    } else {
        m_capturingItem->setForeground(1, QColor());
    }

    m_capturing = false;
    m_capturingItem = nullptr;
    m_manager.saveToSettings();
}

void KeyboardShortcutsDialog::cancelCapture()
{
    if (!m_capturingItem) return;

    QString actionId = m_capturingItem->data(0, Qt::UserRole).toString();
    const auto* action = m_manager.action(actionId);
    if (action) {
        m_capturingItem->setText(1, action->currentKey.toString(QKeySequence::NativeText));
    }

    m_capturing = false;
    m_capturingItem = nullptr;
}

void KeyboardShortcutsDialog::resetSelected()
{
    auto* item = m_tree->currentItem();
    if (!item || !item->data(0, Qt::UserRole).isValid()) return;

    QString actionId = item->data(0, Qt::UserRole).toString();
    m_manager.resetToDefault(actionId);

    const auto* action = m_manager.action(actionId);
    if (action) {
        item->setText(1, action->currentKey.toString(QKeySequence::NativeText));
        item->setForeground(1, QColor()); // reset color
    }
    m_manager.saveToSettings();
}

void KeyboardShortcutsDialog::resetAll()
{
    auto reply = QMessageBox::question(this, "Reset All Shortcuts",
        "Reset all keyboard shortcuts to their defaults?",
        QMessageBox::Yes | QMessageBox::No);
    if (reply != QMessageBox::Yes) return;

    // PopulateTree destroys all tree items — clear the capture
    // pointer so finishCapture() doesn't dereference freed memory.
    m_capturingItem = nullptr;
    m_capturing = false;

    m_manager.resetAllToDefaults();
    m_manager.saveToSettings();
    populateTree();
}

} // namespace rt
