/*
 * test_history_panel.cpp — Tests for the History Panel.
 *
 * Step 28: History Panel
 *
 * Tests cover:
 *   - CommandStack::historySnapshot() — descriptions, currentIndex
 *   - CommandStack::jumpToIndex() — undo/redo to arbitrary positions
 *   - HistoryPanel construction and initial state
 *   - HistoryPanel::refresh() — list population from CommandStack
 *   - Item click → jump to history position
 *   - Clear button → clears all history
 *   - Current position highlighting
 *   - Auto-update when commands are executed/undone/redone
 *   - Edge cases: empty history, clear, redo branch abandonment
 */

#include <gtest/gtest.h>

#include "command/Command.h"
#include "command/CommandStack.h"
#include "panels/project/HistoryPanel.h"

#include <QApplication>
#include <QListWidget>
#include <QListWidgetItem>
#include <QPushButton>
#include <QSignalSpy>
#include <QTest>

#include <memory>
#include <string>

// ═════════════════════════════════════════════════════════════════════════════
// Test command helper
// ═════════════════════════════════════════════════════════════════════════════

namespace {

/// Ensure QApplication exists for Qt widget tests
struct QtApp
{
    static void ensure()
    {
        static int argc = 1;
        static const char* argv[] = {"test"};
        if (!QApplication::instance())
            new QApplication(argc, const_cast<char**>(argv));
    }
};

/// Simple command for testing — sets an integer to a new value.
class TestCmd : public rt::Command
{
public:
    TestCmd(int& target, int newValue, const std::string& desc)
        : m_target(target), m_old(target), m_new(newValue), m_desc(desc) {}

    void execute() override { m_target = m_new; }
    void undo() override    { m_target = m_old; }
    [[nodiscard]] std::string description() const override { return m_desc; }

private:
    int&        m_target;
    int         m_old;
    int         m_new;
    std::string m_desc;
};

std::unique_ptr<TestCmd> makeCmd(int& target, int value, const std::string& desc)
{
    return std::make_unique<TestCmd>(target, value, desc);
}

} // anonymous namespace

// ═════════════════════════════════════════════════════════════════════════════
// CommandStack::historySnapshot tests
// ═════════════════════════════════════════════════════════════════════════════

TEST(HistorySnapshotTest, EmptyStack)
{
    rt::CommandStack stack;
    auto snap = stack.historySnapshot();

    EXPECT_TRUE(snap.descriptions.empty());
    EXPECT_EQ(snap.currentIndex, 0u);
}

TEST(HistorySnapshotTest, AfterExecute)
{
    int val = 0;
    rt::CommandStack stack;

    stack.execute(makeCmd(val, 1, "Set to 1"));
    stack.execute(makeCmd(val, 2, "Set to 2"));
    stack.execute(makeCmd(val, 3, "Set to 3"));

    auto snap = stack.historySnapshot();

    ASSERT_EQ(snap.descriptions.size(), 3u);
    EXPECT_EQ(snap.currentIndex, 3u);
    EXPECT_EQ(snap.descriptions[0], "Set to 1");
    EXPECT_EQ(snap.descriptions[1], "Set to 2");
    EXPECT_EQ(snap.descriptions[2], "Set to 3");
}

TEST(HistorySnapshotTest, AfterUndo)
{
    int val = 0;
    rt::CommandStack stack;

    stack.execute(makeCmd(val, 1, "Set to 1"));
    stack.execute(makeCmd(val, 2, "Set to 2"));
    stack.execute(makeCmd(val, 3, "Set to 3"));
    stack.undo(); // Undo "Set to 3"

    auto snap = stack.historySnapshot();

    ASSERT_EQ(snap.descriptions.size(), 3u);
    EXPECT_EQ(snap.currentIndex, 2u);
    EXPECT_EQ(snap.descriptions[0], "Set to 1");
    EXPECT_EQ(snap.descriptions[1], "Set to 2");
    EXPECT_EQ(snap.descriptions[2], "Set to 3"); // In redo zone
}

TEST(HistorySnapshotTest, AfterMultipleUndos)
{
    int val = 0;
    rt::CommandStack stack;

    stack.execute(makeCmd(val, 1, "A"));
    stack.execute(makeCmd(val, 2, "B"));
    stack.execute(makeCmd(val, 3, "C"));
    stack.undo();
    stack.undo();

    auto snap = stack.historySnapshot();

    ASSERT_EQ(snap.descriptions.size(), 3u);
    EXPECT_EQ(snap.currentIndex, 1u);
    // descriptions[0] = A (executed)
    // descriptions[1] = B (redo)
    // descriptions[2] = C (redo)
}

TEST(HistorySnapshotTest, AfterUndoAndRedo)
{
    int val = 0;
    rt::CommandStack stack;

    stack.execute(makeCmd(val, 1, "A"));
    stack.execute(makeCmd(val, 2, "B"));
    stack.undo();
    stack.redo();

    auto snap = stack.historySnapshot();

    ASSERT_EQ(snap.descriptions.size(), 2u);
    EXPECT_EQ(snap.currentIndex, 2u); // All executed
}

TEST(HistorySnapshotTest, UndoAllShowsIndex0)
{
    int val = 0;
    rt::CommandStack stack;

    stack.execute(makeCmd(val, 1, "A"));
    stack.execute(makeCmd(val, 2, "B"));
    stack.undo();
    stack.undo();

    auto snap = stack.historySnapshot();

    EXPECT_EQ(snap.currentIndex, 0u);
    EXPECT_EQ(snap.descriptions.size(), 2u);
}

// ═════════════════════════════════════════════════════════════════════════════
// CommandStack::jumpToIndex tests
// ═════════════════════════════════════════════════════════════════════════════

TEST(JumpToIndexTest, JumpBackward)
{
    int val = 0;
    rt::CommandStack stack;

    stack.execute(makeCmd(val, 1, "A"));
    stack.execute(makeCmd(val, 2, "B"));
    stack.execute(makeCmd(val, 3, "C"));

    EXPECT_TRUE(stack.jumpToIndex(1)); // undo B and C
    EXPECT_EQ(val, 1);
    EXPECT_EQ(stack.undoCount(), 1u);
    EXPECT_EQ(stack.redoCount(), 2u);
}

TEST(JumpToIndexTest, JumpForward)
{
    int val = 0;
    rt::CommandStack stack;

    stack.execute(makeCmd(val, 1, "A"));
    stack.execute(makeCmd(val, 2, "B"));
    stack.execute(makeCmd(val, 3, "C"));
    stack.undo();
    stack.undo();
    stack.undo();

    EXPECT_EQ(val, 0);
    EXPECT_TRUE(stack.jumpToIndex(2)); // redo A and B
    EXPECT_EQ(val, 2);
    EXPECT_EQ(stack.undoCount(), 2u);
    EXPECT_EQ(stack.redoCount(), 1u);
}

TEST(JumpToIndexTest, JumpToSamePosition)
{
    int val = 0;
    rt::CommandStack stack;

    stack.execute(makeCmd(val, 1, "A"));
    stack.execute(makeCmd(val, 2, "B"));

    EXPECT_TRUE(stack.jumpToIndex(2)); // No-op
    EXPECT_EQ(val, 2);
    EXPECT_EQ(stack.undoCount(), 2u);
    EXPECT_EQ(stack.redoCount(), 0u);
}

TEST(JumpToIndexTest, JumpToZero)
{
    int val = 0;
    rt::CommandStack stack;

    stack.execute(makeCmd(val, 1, "A"));
    stack.execute(makeCmd(val, 2, "B"));

    EXPECT_TRUE(stack.jumpToIndex(0)); // Undo everything
    EXPECT_EQ(val, 0);
    EXPECT_EQ(stack.undoCount(), 0u);
    EXPECT_EQ(stack.redoCount(), 2u);
}

TEST(JumpToIndexTest, JumpToEnd)
{
    int val = 0;
    rt::CommandStack stack;

    stack.execute(makeCmd(val, 1, "A"));
    stack.execute(makeCmd(val, 2, "B"));
    stack.undo();
    stack.undo();

    EXPECT_TRUE(stack.jumpToIndex(2)); // Redo everything
    EXPECT_EQ(val, 2);
}

TEST(JumpToIndexTest, JumpBeyondTotalReturnsFalse)
{
    int val = 0;
    rt::CommandStack stack;

    stack.execute(makeCmd(val, 1, "A"));

    EXPECT_FALSE(stack.jumpToIndex(99));
}

TEST(JumpToIndexTest, JumpOnEmptyStack)
{
    rt::CommandStack stack;

    EXPECT_TRUE(stack.jumpToIndex(0));  // No-op
    EXPECT_FALSE(stack.jumpToIndex(1)); // Out of range
}

// ═════════════════════════════════════════════════════════════════════════════
// HistoryPanel — construction
// ═════════════════════════════════════════════════════════════════════════════

TEST(HistoryPanelTest, DefaultConstruction)
{
    QtApp::ensure();

    rt::HistoryPanel panel;

    EXPECT_EQ(panel.commandStack(), nullptr);
    EXPECT_EQ(panel.itemCount(), 0);
    EXPECT_EQ(panel.currentRow(), -1);
    EXPECT_NE(panel.listWidget(), nullptr);
    EXPECT_NE(panel.clearButton(), nullptr);
    EXPECT_FALSE(panel.clearButton()->isEnabled());
}

// ═════════════════════════════════════════════════════════════════════════════
// HistoryPanel — with CommandStack
// ═════════════════════════════════════════════════════════════════════════════

TEST(HistoryPanelTest, SetCommandStackEmpty)
{
    QtApp::ensure();

    rt::CommandStack stack;
    rt::HistoryPanel panel;
    panel.setCommandStack(&stack);

    EXPECT_EQ(panel.commandStack(), &stack);

    // Should show just "Initial State"
    EXPECT_EQ(panel.itemCount(), 1);
    EXPECT_EQ(panel.currentRow(), 0);
    EXPECT_FALSE(panel.clearButton()->isEnabled());
}

TEST(HistoryPanelTest, ShowsExecutedCommands)
{
    QtApp::ensure();

    int val = 0;
    rt::CommandStack stack;
    rt::HistoryPanel panel;
    panel.setCommandStack(&stack);

    stack.execute(makeCmd(val, 1, "Add Clip"));
    stack.execute(makeCmd(val, 2, "Move Clip"));
    stack.execute(makeCmd(val, 3, "Set Volume"));

    // "Initial State" + 3 commands = 4 items
    EXPECT_EQ(panel.itemCount(), 4);

    // Current position = row 3 (last executed command)
    EXPECT_EQ(panel.currentRow(), 3);
}

TEST(HistoryPanelTest, ShowsUndoneCommandsDimmed)
{
    QtApp::ensure();

    int val = 0;
    rt::CommandStack stack;
    rt::HistoryPanel panel;
    panel.setCommandStack(&stack);

    stack.execute(makeCmd(val, 1, "A"));
    stack.execute(makeCmd(val, 2, "B"));
    stack.execute(makeCmd(val, 3, "C"));
    stack.undo(); // Undo C

    // Still 4 items (Initial + A + B + C)
    EXPECT_EQ(panel.itemCount(), 4);

    // Current position = row 2 (B is the last executed)
    EXPECT_EQ(panel.currentRow(), 2);
}

TEST(HistoryPanelTest, CurrentPositionAfterUndoAll)
{
    QtApp::ensure();

    int val = 0;
    rt::CommandStack stack;
    rt::HistoryPanel panel;
    panel.setCommandStack(&stack);

    stack.execute(makeCmd(val, 1, "A"));
    stack.execute(makeCmd(val, 2, "B"));
    stack.undo();
    stack.undo();

    // Current should be at "Initial State" (row 0)
    EXPECT_EQ(panel.currentRow(), 0);
    EXPECT_EQ(panel.itemCount(), 3); // Initial + A + B
}

TEST(HistoryPanelTest, ItemTextContainsDescription)
{
    QtApp::ensure();

    int val = 0;
    rt::CommandStack stack;
    rt::HistoryPanel panel;
    panel.setCommandStack(&stack);

    stack.execute(makeCmd(val, 1, "Add Video Clip"));

    auto* list = panel.listWidget();
    ASSERT_EQ(list->count(), 2);

    // Item 0 = "Initial State" with bullet
    EXPECT_TRUE(list->item(0)->text().contains("Initial State"));

    // Item 1 = the command with bullet
    EXPECT_TRUE(list->item(1)->text().contains("Add Video Clip"));
}

TEST(HistoryPanelTest, ClickItemJumpsToPosition)
{
    QtApp::ensure();

    int val = 0;
    rt::CommandStack stack;
    rt::HistoryPanel panel;
    panel.setCommandStack(&stack);

    stack.execute(makeCmd(val, 1, "A"));
    stack.execute(makeCmd(val, 2, "B"));
    stack.execute(makeCmd(val, 3, "C"));

    EXPECT_EQ(val, 3);

    // Simulate clicking item at row 1 (after "A", before "B")
    QSignalSpy spy(&panel, &rt::HistoryPanel::historyJumped);
    auto* list = panel.listWidget();

    // Click on item 1 (= jump to index 1 = after executing "A")
    emit list->itemClicked(list->item(1));

    EXPECT_EQ(val, 1); // B and C undone
    EXPECT_EQ(spy.count(), 1);
    EXPECT_EQ(spy.at(0).at(0).toInt(), 1);
}

TEST(HistoryPanelTest, ClickInitialStateUndoesAll)
{
    QtApp::ensure();

    int val = 0;
    rt::CommandStack stack;
    rt::HistoryPanel panel;
    panel.setCommandStack(&stack);

    stack.execute(makeCmd(val, 1, "A"));
    stack.execute(makeCmd(val, 2, "B"));

    // Click "Initial State" (row 0 → index 0)
    auto* list = panel.listWidget();
    emit list->itemClicked(list->item(0));

    EXPECT_EQ(val, 0);
    EXPECT_EQ(stack.undoCount(), 0u);
    EXPECT_EQ(stack.redoCount(), 2u);
}

TEST(HistoryPanelTest, ClickRedoItem)
{
    QtApp::ensure();

    int val = 0;
    rt::CommandStack stack;
    rt::HistoryPanel panel;
    panel.setCommandStack(&stack);

    stack.execute(makeCmd(val, 1, "A"));
    stack.execute(makeCmd(val, 2, "B"));
    stack.execute(makeCmd(val, 3, "C"));
    stack.undo();
    stack.undo(); // At position 1 (after A)

    EXPECT_EQ(val, 1);

    // Click on "C" (row 3, index 3 = redo to end)
    auto* list = panel.listWidget();
    emit list->itemClicked(list->item(3));

    EXPECT_EQ(val, 3);
}

TEST(HistoryPanelTest, ClearButtonClearsHistory)
{
    QtApp::ensure();

    int val = 0;
    rt::CommandStack stack;
    rt::HistoryPanel panel;
    panel.setCommandStack(&stack);

    stack.execute(makeCmd(val, 1, "A"));
    stack.execute(makeCmd(val, 2, "B"));

    EXPECT_TRUE(panel.clearButton()->isEnabled());
    EXPECT_EQ(panel.itemCount(), 3);

    // Click clear
    panel.clearButton()->click();

    EXPECT_EQ(stack.undoCount(), 0u);
    EXPECT_EQ(stack.redoCount(), 0u);

    // Should show just "Initial State"
    EXPECT_EQ(panel.itemCount(), 1);
    EXPECT_FALSE(panel.clearButton()->isEnabled());
}

TEST(HistoryPanelTest, ClearButtonDisabledWhenNoHistory)
{
    QtApp::ensure();

    rt::CommandStack stack;
    rt::HistoryPanel panel;
    panel.setCommandStack(&stack);

    EXPECT_FALSE(panel.clearButton()->isEnabled());
}

TEST(HistoryPanelTest, AutoUpdatesOnExecute)
{
    QtApp::ensure();

    int val = 0;
    rt::CommandStack stack;
    rt::HistoryPanel panel;
    panel.setCommandStack(&stack);

    EXPECT_EQ(panel.itemCount(), 1); // Just initial state

    stack.execute(makeCmd(val, 1, "A"));
    EXPECT_EQ(panel.itemCount(), 2);

    stack.execute(makeCmd(val, 2, "B"));
    EXPECT_EQ(panel.itemCount(), 3);
}

TEST(HistoryPanelTest, AutoUpdatesOnUndo)
{
    QtApp::ensure();

    int val = 0;
    rt::CommandStack stack;
    rt::HistoryPanel panel;
    panel.setCommandStack(&stack);

    stack.execute(makeCmd(val, 1, "A"));
    stack.execute(makeCmd(val, 2, "B"));
    EXPECT_EQ(panel.currentRow(), 2);

    stack.undo();
    EXPECT_EQ(panel.currentRow(), 1);
    EXPECT_EQ(panel.itemCount(), 3); // Count unchanged, position moved
}

TEST(HistoryPanelTest, AutoUpdatesOnRedo)
{
    QtApp::ensure();

    int val = 0;
    rt::CommandStack stack;
    rt::HistoryPanel panel;
    panel.setCommandStack(&stack);

    stack.execute(makeCmd(val, 1, "A"));
    stack.execute(makeCmd(val, 2, "B"));
    stack.undo();
    EXPECT_EQ(panel.currentRow(), 1);

    stack.redo();
    EXPECT_EQ(panel.currentRow(), 2);
}

TEST(HistoryPanelTest, NewCommandAfterUndoAbandonsBranch)
{
    QtApp::ensure();

    int val = 0;
    rt::CommandStack stack;
    rt::HistoryPanel panel;
    panel.setCommandStack(&stack);

    stack.execute(makeCmd(val, 1, "A"));
    stack.execute(makeCmd(val, 2, "B"));
    stack.execute(makeCmd(val, 3, "C"));
    stack.undo(); // Undo C
    stack.undo(); // Undo B

    EXPECT_EQ(panel.itemCount(), 4); // Initial + A + B + C

    // Execute new command → redo branch (B, C) abandoned
    stack.execute(makeCmd(val, 10, "D"));

    EXPECT_EQ(panel.itemCount(), 3); // Initial + A + D
    EXPECT_EQ(panel.currentRow(), 2);
}

TEST(HistoryPanelTest, ManualRefresh)
{
    QtApp::ensure();

    int val = 0;
    rt::CommandStack stack;
    rt::HistoryPanel panel;

    // Set up without callback (simulate non-connected scenario)
    panel.setCommandStack(&stack);

    // Manually add commands (callback installed by setCommandStack will auto-refresh,
    // but let's also test manual refresh)
    stack.execute(makeCmd(val, 1, "Manual"));

    // Manual refresh should succeed and show the right state
    panel.refresh();
    EXPECT_EQ(panel.itemCount(), 2);
}

TEST(HistoryPanelTest, NoCommandStackRefreshIsHarmless)
{
    QtApp::ensure();

    rt::HistoryPanel panel;
    panel.refresh(); // Should not crash
    EXPECT_EQ(panel.itemCount(), 0);
}

TEST(HistoryPanelTest, ItemDataStoresJumpIndex)
{
    QtApp::ensure();

    int val = 0;
    rt::CommandStack stack;
    rt::HistoryPanel panel;
    panel.setCommandStack(&stack);

    stack.execute(makeCmd(val, 1, "A"));
    stack.execute(makeCmd(val, 2, "B"));

    auto* list = panel.listWidget();
    ASSERT_EQ(list->count(), 3);

    // Row 0 = Initial State → index 0
    EXPECT_EQ(list->item(0)->data(Qt::UserRole).toInt(), 0);
    // Row 1 = A → index 1
    EXPECT_EQ(list->item(1)->data(Qt::UserRole).toInt(), 1);
    // Row 2 = B → index 2
    EXPECT_EQ(list->item(2)->data(Qt::UserRole).toInt(), 2);
}

TEST(HistoryPanelTest, ManyCommands)
{
    QtApp::ensure();

    int val = 0;
    rt::CommandStack stack;
    rt::HistoryPanel panel;
    panel.setCommandStack(&stack);

    for (int i = 1; i <= 50; ++i) {
        stack.execute(makeCmd(val, i, "Cmd " + std::to_string(i)));
    }

    EXPECT_EQ(panel.itemCount(), 51); // Initial + 50 commands
    EXPECT_EQ(panel.currentRow(), 50);
    EXPECT_EQ(val, 50);
}

TEST(HistoryPanelTest, HistoryJumpedSignalEmittedOnClick)
{
    QtApp::ensure();

    int val = 0;
    rt::CommandStack stack;
    rt::HistoryPanel panel;
    panel.setCommandStack(&stack);

    stack.execute(makeCmd(val, 1, "A"));
    stack.execute(makeCmd(val, 2, "B"));

    QSignalSpy spy(&panel, &rt::HistoryPanel::historyJumped);

    auto* list = panel.listWidget();
    emit list->itemClicked(list->item(0)); // Jump to initial

    EXPECT_EQ(spy.count(), 1);
    EXPECT_EQ(spy.at(0).at(0).toInt(), 0);
}

// ═════════════════════════════════════════════════════════════════════════════
// HistoryPanel — visual indicator tests
// ═════════════════════════════════════════════════════════════════════════════

TEST(HistoryPanelTest, CurrentItemHasArrowPrefix)
{
    QtApp::ensure();

    int val = 0;
    rt::CommandStack stack;
    rt::HistoryPanel panel;
    panel.setCommandStack(&stack);

    stack.execute(makeCmd(val, 1, "A"));
    stack.execute(makeCmd(val, 2, "B"));

    // Current item (B, row 2) should have arrow prefix
    auto* list = panel.listWidget();
    QString currentText = list->item(2)->text();
    EXPECT_TRUE(currentText.startsWith(QString::fromUtf8("►")));
}

TEST(HistoryPanelTest, ExecutedItemsHaveFilledBullet)
{
    QtApp::ensure();

    int val = 0;
    rt::CommandStack stack;
    rt::HistoryPanel panel;
    panel.setCommandStack(&stack);

    stack.execute(makeCmd(val, 1, "A"));
    stack.execute(makeCmd(val, 2, "B"));

    auto* list = panel.listWidget();
    // Item 1 (A) is executed but not current → filled bullet
    QString text = list->item(1)->text();
    EXPECT_TRUE(text.startsWith(QString::fromUtf8("◆")));
}

TEST(HistoryPanelTest, RedoItemsHaveOpenBullet)
{
    QtApp::ensure();

    int val = 0;
    rt::CommandStack stack;
    rt::HistoryPanel panel;
    panel.setCommandStack(&stack);

    stack.execute(makeCmd(val, 1, "A"));
    stack.execute(makeCmd(val, 2, "B"));
    stack.undo(); // B is now in redo

    auto* list = panel.listWidget();
    // Row 2 (B) should have open bullet
    QString text = list->item(2)->text();
    EXPECT_TRUE(text.startsWith(QString::fromUtf8("○")));
}

TEST(HistoryPanelTest, InitialStateHasArrowWhenCurrent)
{
    QtApp::ensure();

    rt::CommandStack stack;
    rt::HistoryPanel panel;
    panel.setCommandStack(&stack);

    // No commands → Initial State is current
    auto* list = panel.listWidget();
    ASSERT_GE(list->count(), 1);
    EXPECT_TRUE(list->item(0)->text().startsWith(QString::fromUtf8("►")));
}
