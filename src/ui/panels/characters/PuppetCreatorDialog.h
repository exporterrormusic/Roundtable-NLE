/*
 * PuppetCreatorDialog — create or edit a Veadotube-style PNG puppet character.
 *
 * The user enters a character name and, for each expression/costume variant,
 * picks the 4 face PNGs (mouth×eyes states).  On accept it copies the chosen
 * images into assets/png_characters/<name>/<variant>/ and writes puppet.json
 * via PuppetLibrary.
 */

#pragma once

#include <array>

#include <QDialog>
#include <QMap>
#include <QString>

#include "panels/characters/PuppetLibrary.h"

class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;

namespace rt {

class PuppetCreatorDialog : public QDialog
{
    Q_OBJECT

public:
    /// `editFolder` empty = create new; otherwise load that puppet for editing.
    explicit PuppetCreatorDialog(QWidget* parent = nullptr,
                                 const QString& editFolder = QString());

    /// Folder name of the puppet that was created/edited (valid after accept()).
    [[nodiscard]] QString savedFolderName() const { return m_savedFolder; }

private slots:
    void onChooseFace(int faceIndex);
    void onVariantChanged(int index);
    void onAddVariant();
    void onAccept();

private:
    void buildUI();
    void loadExisting(const QString& folder);
    void stashCurrentVariant();          ///< save the visible pickers into m_sources
    void showVariant(const QString& name);
    void refreshFacePreview(int faceIndex);

    QLineEdit*   m_nameEdit{nullptr};
    QComboBox*   m_variantCombo{nullptr};
    QPushButton* m_addVariantBtn{nullptr};
    std::array<QLabel*, puppetlib::kFaceCount>      m_facePreview{};
    std::array<QPushButton*, puppetlib::kFaceCount> m_faceButton{};

    // Per-variant chosen source paths (face order).  May point at external
    // files (to import) or at already-in-place project-relative paths (edit).
    QMap<QString, std::array<QString, puppetlib::kFaceCount>> m_sources;
    QString m_currentVariant;
    QString m_editFolder;       ///< non-empty when editing
    QString m_savedFolder;
};

} // namespace rt
