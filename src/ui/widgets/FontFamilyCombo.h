/*
 * FontFamilyCombo — Premiere Pro-style font family picker.
 *
 * Modeled on Premiere's dvaui::sharedui::UI_FontPopup (inspected in the
 * installed dvaui.dll): a type-to-filter field over a font menu whose rows
 * show a favorite star, the family name, and a sample drawn in the font
 * itself; favorites and a most-recently-used list sit above the full list.
 *
 * Live preview: hovering a row (or moving to it with the arrow keys) emits
 * familyPreviewRequested() after a short dwell, so the selected text in the
 * Program Monitor shows the font before anything is committed. Choosing a
 * row emits the normal QComboBox::activated/textActivated (one undo step in
 * the caller); closing the menu any other way (Esc, click away) emits
 * familyPreviewEnded() so the caller reverts. Premiere does the same with
 * an "interactive" change message that never posts history.
 *
 * Still a QComboBox: editable, with a case-insensitive "contains" completer,
 * so the existing panel code and tests keep their contract.
 */

#pragma once

#include <QComboBox>
#include <QString>
#include <QStringList>
#include <QTimer>

namespace rt {

class FontFamilyCombo : public QComboBox
{
    Q_OBJECT

public:
    explicit FontFamilyCombo(QWidget* parent = nullptr);

    /// The installed family whose name matches `text` (case-insensitive),
    /// or an empty string. Section headers never match.
    [[nodiscard]] QString installedFamily(const QString& text) const;

    /// Show `family` without emitting signals. An uninstalled family is
    /// still displayed (Premiere shows a missing font's name).
    void setCurrentFamily(const QString& family);

    void showPopup() override;
    void hidePopup() override;

    // ── Favorites + recently used (app-wide, persisted) ─────────────────
    [[nodiscard]] static QStringList favoriteFamilies();
    [[nodiscard]] static bool isFavorite(const QString& family);
    static void setFavorite(const QString& family, bool favorite);
    [[nodiscard]] static QStringList recentFamilies();
    static void noteFamilyUsed(const QString& family);
    static constexpr int kMaxRecentFamilies = 5;

    /// Item role holding the row kind (see RowKind).
    static constexpr int kRowKindRole = Qt::UserRole + 40;
    enum RowKind { AllRow = 0, RecentRow = 1, FavoriteRow = 2, HeaderRow = 3 };

signals:
    /// Hover / keyboard highlight dwelled on `family`: preview it.
    void familyPreviewRequested(const QString& family);
    /// The menu closed without a choice: revert any preview.
    void familyPreviewEnded();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void rebuildTopSections();
    void requestPreview(const QString& family);
    void scheduleEndPreview();
    [[nodiscard]] int allSectionStart() const;
    [[nodiscard]] bool isFamilyRow(int row) const;

    QStringList m_families;      // installed families, sorted (the All list)
    QTimer      m_previewTimer;
    QString     m_pendingPreview;
    bool        m_previewing{false};
};

} // namespace rt
