/*
 * PuppetCreatorDialog.cpp — create/edit a PNG puppet character.
 */

#include "panels/characters/PuppetCreatorDialog.h"
#include "Settings.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPixmap>
#include <QPushButton>
#include <QVBoxLayout>

#include <spdlog/spdlog.h>

namespace rt {

PuppetCreatorDialog::PuppetCreatorDialog(QWidget* parent, const QString& editFolder)
    : QDialog(parent)
    , m_editFolder(editFolder)
{
    setWindowTitle(editFolder.isEmpty() ? tr("Create PNG Puppet")
                                        : tr("Edit PNG Puppet"));
    setModal(true);
    buildUI();

    if (!editFolder.isEmpty()) {
        loadExisting(editFolder);
    } else {
        m_currentVariant = QStringLiteral("default");
        m_sources.insert(m_currentVariant, {});
        m_variantCombo->addItem(m_currentVariant);
        showVariant(m_currentVariant);
    }
    resize(460, 520);
}

void PuppetCreatorDialog::buildUI()
{
    auto* root = new QVBoxLayout(this);

    auto* form = new QFormLayout();
    m_nameEdit = new QLineEdit(this);
    m_nameEdit->setPlaceholderText(tr("Character name"));
    form->addRow(tr("Name:"), m_nameEdit);

    auto* variantRow = new QHBoxLayout();
    m_variantCombo = new QComboBox(this);
    variantRow->addWidget(m_variantCombo, 1);
    m_addVariantBtn = new QPushButton(tr("Add variant…"), this);
    variantRow->addWidget(m_addVariantBtn);
    form->addRow(tr("Variant:"), variantRow);
    root->addLayout(form);

    auto* hint = new QLabel(
        tr("Pick the 4 PNGs for this variant. They should share the same size."),
        this);
    hint->setWordWrap(true);
    root->addWidget(hint);

    // 4 face rows: preview thumbnail + label + choose button.
    for (int i = 0; i < puppetlib::kFaceCount; ++i) {
        auto* roww = new QHBoxLayout();

        auto* preview = new QLabel(this);
        preview->setFixedSize(64, 64);
        preview->setFrameShape(QFrame::Box);
        preview->setAlignment(Qt::AlignCenter);
        preview->setText(tr("—"));
        m_facePreview[static_cast<size_t>(i)] = preview;
        roww->addWidget(preview);

        auto* lbl = new QLabel(puppetlib::faceLabels()[static_cast<size_t>(i)], this);
        lbl->setWordWrap(true);
        roww->addWidget(lbl, 1);

        auto* btn = new QPushButton(tr("Choose…"), this);
        connect(btn, &QPushButton::clicked, this, [this, i]() { onChooseFace(i); });
        m_faceButton[static_cast<size_t>(i)] = btn;
        roww->addWidget(btn);

        root->addLayout(roww);
    }

    root->addStretch(1);

    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Save | QDialogButtonBox::Cancel, this);
    root->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, this, &PuppetCreatorDialog::onAccept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    connect(m_variantCombo, qOverload<int>(&QComboBox::currentIndexChanged),
            this, &PuppetCreatorDialog::onVariantChanged);
    connect(m_addVariantBtn, &QPushButton::clicked,
            this, &PuppetCreatorDialog::onAddVariant);
}

void PuppetCreatorDialog::loadExisting(const QString& folder)
{
    PuppetManifest m;
    if (!puppetlib::load(folder, m)) {
        spdlog::warn("PuppetCreatorDialog: failed to load '{}'", folder.toStdString());
        m_currentVariant = QStringLiteral("default");
        m_sources.insert(m_currentVariant, {});
        m_variantCombo->addItem(m_currentVariant);
        showVariant(m_currentVariant);
        return;
    }
    m_nameEdit->setText(m.displayName);
    for (const QString& vName : m.variantOrder) {
        m_sources.insert(vName, m.variants.value(vName).faces);
        m_variantCombo->addItem(vName);
    }
    m_currentVariant = m.variantOrder.isEmpty() ? QStringLiteral("default")
                                                : m.variantOrder.first();
    showVariant(m_currentVariant);
}

void PuppetCreatorDialog::stashCurrentVariant()
{
    if (m_currentVariant.isEmpty()) return;
    auto it = m_sources.find(m_currentVariant);
    if (it == m_sources.end())
        it = m_sources.insert(m_currentVariant, {});
    // The pickers write directly into m_sources via onChooseFace, so nothing
    // extra to copy here — kept as a hook for clarity / future fields.
}

void PuppetCreatorDialog::showVariant(const QString& name)
{
    m_currentVariant = name;
    if (!m_sources.contains(name))
        m_sources.insert(name, {});
    const int idx = m_variantCombo->findText(name);
    if (idx >= 0 && m_variantCombo->currentIndex() != idx) {
        QSignalBlocker block(m_variantCombo);
        m_variantCombo->setCurrentIndex(idx);
    }
    for (int i = 0; i < puppetlib::kFaceCount; ++i)
        refreshFacePreview(i);
}

void PuppetCreatorDialog::refreshFacePreview(int faceIndex)
{
    auto* preview = m_facePreview[static_cast<size_t>(faceIndex)];
    if (!preview) return;
    const QString path = m_sources.value(m_currentVariant)
                             .at(static_cast<size_t>(faceIndex));
    if (path.isEmpty()) {
        preview->setPixmap(QPixmap());
        preview->setText(tr("—"));
        return;
    }
    QPixmap pm(path);
    if (pm.isNull()) {
        preview->setPixmap(QPixmap());
        preview->setText(tr("?"));
        return;
    }
    preview->setPixmap(pm.scaled(preview->size(), Qt::KeepAspectRatio,
                                 Qt::SmoothTransformation));
    preview->setText(QString());
}

void PuppetCreatorDialog::onChooseFace(int faceIndex)
{
    // Reopen at the last-used folder (Premiere-style), like the Library import.
    auto settings = rt::appSettings();
    QString lastDir = settings.value(QStringLiteral("Puppet/lastImportDir"),
                                     QString()).toString();
    if (lastDir.isEmpty() || !QDir(lastDir).exists())
        lastDir = QDir::homePath();

    const QString file = QFileDialog::getOpenFileName(
        this, tr("Choose PNG"), lastDir,
        tr("PNG images (*.png);;All images (*.png *.webp *.tga *.tiff)"));
    if (file.isEmpty()) return;

    settings.setValue(QStringLiteral("Puppet/lastImportDir"),
                      QFileInfo(file).absolutePath());
    settings.sync();

    auto it = m_sources.find(m_currentVariant);
    if (it == m_sources.end())
        it = m_sources.insert(m_currentVariant, {});
    (*it)[static_cast<size_t>(faceIndex)] = file;
    refreshFacePreview(faceIndex);
}

void PuppetCreatorDialog::onVariantChanged(int index)
{
    if (index < 0) return;
    stashCurrentVariant();
    showVariant(m_variantCombo->itemText(index));
}

void PuppetCreatorDialog::onAddVariant()
{
    bool ok = false;
    const QString name = QInputDialog::getText(
        this, tr("Add Variant"), tr("Variant name (e.g. angry, costume2):"),
        QLineEdit::Normal, QString(), &ok).trimmed();
    if (!ok || name.isEmpty()) return;
    if (m_sources.contains(name)) {
        showVariant(name);
        return;
    }
    stashCurrentVariant();
    m_sources.insert(name, {});
    m_variantCombo->addItem(name);
    showVariant(name);
}

void PuppetCreatorDialog::onAccept()
{
    stashCurrentVariant();

    const QString displayName = m_nameEdit->text().trimmed();
    if (displayName.isEmpty()) {
        QMessageBox::warning(this, tr("Missing name"),
                             tr("Please enter a character name."));
        return;
    }

    // Require at least the resting face for the first variant so the puppet
    // always has something to display.
    const QString firstVariant = m_variantCombo->count() > 0
        ? m_variantCombo->itemText(0) : QStringLiteral("default");
    if (m_sources.value(firstVariant).at(0).isEmpty()) {  // index 0 = resting face
        QMessageBox::warning(this, tr("Missing image"),
            tr("At least the resting image (mouth closed / eyes open) is required."));
        return;
    }

    const QString folder = m_editFolder.isEmpty()
        ? puppetlib::sanitizeFolderName(displayName)
        : m_editFolder;

    // Prevent clobbering a different existing puppet when creating new.
    if (m_editFolder.isEmpty() &&
        QFile::exists(puppetlib::manifestPath(folder))) {
        const auto reply = QMessageBox::question(this, tr("Puppet exists"),
            tr("A puppet named \"%1\" already exists. Overwrite it?").arg(folder),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (reply != QMessageBox::Yes) return;
    }

    const QString destPrefix =
        puppetlib::puppetsRootDir() + QStringLiteral("/") + folder + QStringLiteral("/");

    PuppetManifest manifest;
    manifest.folderName  = folder;
    manifest.displayName = displayName;

    // Build variant order from the combo so it matches the UI.
    for (int vi = 0; vi < m_variantCombo->count(); ++vi) {
        const QString vName = m_variantCombo->itemText(vi);
        const auto& src = m_sources.value(vName);

        PuppetVariant variant;
        for (int i = 0; i < puppetlib::kFaceCount; ++i) {
            const QString chosen = src.at(static_cast<size_t>(i));
            if (chosen.isEmpty()) {
                // Fall back to the resting face so a partially-filled variant
                // still renders (idle face stands in for missing states).
                variant.faces[static_cast<size_t>(i)].clear();
                continue;
            }
            // Already in place (edit mode, image unchanged) → keep as-is.
            if (chosen.startsWith(destPrefix) || chosen.startsWith(QStringLiteral("assets/"))) {
                if (QFileInfo(chosen).exists()) {
                    variant.faces[static_cast<size_t>(i)] = chosen;
                    continue;
                }
            }
            const QString rel =
                puppetlib::importFaceImage(folder, vName, i, chosen);
            if (rel.isEmpty()) {
                QMessageBox::warning(this, tr("Import failed"),
                    tr("Could not copy image for %1 / %2.")
                        .arg(vName, puppetlib::faceLabels()[static_cast<size_t>(i)]));
                return;
            }
            variant.faces[static_cast<size_t>(i)] = rel;
        }
        manifest.variantOrder << vName;
        manifest.variants.insert(vName, variant);
    }

    if (!puppetlib::save(manifest)) {
        QMessageBox::warning(this, tr("Save failed"),
                             tr("Could not write the puppet manifest."));
        return;
    }

    m_savedFolder = folder;
    accept();
}

} // namespace rt
