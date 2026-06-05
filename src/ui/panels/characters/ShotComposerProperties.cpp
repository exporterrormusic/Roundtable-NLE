/*
 * ShotComposerProperties.cpp - Properties panel UI construction.
 * Split from ShotComposerUI.cpp.
 */

#include "panels/characters/ShotComposer.h"
#include "panels/characters/ShotComposerInternal.h"

#include "Theme.h"

#ifdef ROUNDTABLE_HAS_SPINE
#include "spine/ModelManager.h"
#include "widgets/SpinePreviewWidget.h"
#endif

#include <QApplication>
#include <QComboBox>
#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPushButton>
#include <QSet>
#include <QScrollArea>
#include <QShortcut>
#include <QSlider>
#include <QLineEdit>
#include <QSplitter>
#include <QStyledItemDelegate>
#include <QTabWidget>
#include <QToolButton>
#include <QToolTip>
#include <QVBoxLayout>

#include <spdlog/spdlog.h>


namespace rt {

// A QMenu that stays open when a checkable item is toggled, so the user can
// tick several shows in one go. Non-checkable items (e.g. "New Show…") close
// the menu normally.
namespace {
class CheckableKeepOpenMenu : public QMenu {
public:
    using QMenu::QMenu;
protected:
    void mouseReleaseEvent(QMouseEvent* e) override {
        QAction* a = activeAction();
        if (a && a->isCheckable() && a->isEnabled()) {
            a->trigger();   // toggles checked state + emits triggered
            return;          // keep the menu open
        }
        QMenu::mouseReleaseEvent(e);
    }
};
} // anon

// â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

QWidget* ShotComposer::createPropertiesPanel()
{
    const auto& m = Theme::metrics();
    const auto& c = Theme::colors();
    // Right panel: vertical splitter â€” top = Shot Name + Properties, bottom = Layers
    auto* rightSplitter = new QSplitter(Qt::Vertical);
    rightSplitter->setChildrenCollapsible(false);

    // â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
    // TOP SECTION: Shot Name + Layer Properties + Camera
    // â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
    auto* topSection = new QWidget;
    auto* topLayout = new QVBoxLayout(topSection);
    topLayout->setContentsMargins(m.spacingMd, m.spacingMd, m.spacingMd, m.spacingMd);
    topLayout->setSpacing(m.spacingSm);

    // â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
    // Shot Name â€” compact row at the very top
    // â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
    auto* shotNameGroup = new QGroupBox(QStringLiteral("SHOT"));
    // QGroupBox styling inherited from panel stylesheet
    auto* shotNameLayout = new QVBoxLayout(shotNameGroup);
    shotNameLayout->setContentsMargins(m.spacingMd, m.spacingLg, m.spacingMd, m.spacingSm);
    shotNameLayout->setSpacing(m.spacingSm);

    // Shot name + save row
    auto* nameRow = new QHBoxLayout;
    nameRow->setContentsMargins(0, 0, 0, 0);
    nameRow->setSpacing(m.spacingXs);

    m_shotNameEdit = new QLineEdit;
    m_shotNameEdit->setPlaceholderText("Shot name...");
    // Input styling inherited from panel stylesheet
    m_shotNameEdit->setEnabled(false);
    nameRow->addWidget(m_shotNameEdit, 1);

    auto* saveShotBtnProps = new QPushButton(QStringLiteral("SAVE SHOT"));
    saveShotBtnProps->setToolTip("Save shot");
    saveShotBtnProps->setFixedHeight(36);
    saveShotBtnProps->setObjectName("SaveBtn");
    nameRow->addWidget(saveShotBtnProps);
    shotNameLayout->addLayout(nameRow);

    // ── Shows row — assign this shot to one or more shows (comma-separated) ──
    auto* showsRow = new QHBoxLayout;
    showsRow->setContentsMargins(0, 0, 0, 0);
    showsRow->setSpacing(m.spacingXs);

    auto* showsLabel = new QLabel(QStringLiteral("Shows:"));
    showsLabel->setStyleSheet(QStringLiteral("color: %1;")
        .arg(Theme::hex(c.textSecondary)));
    showsRow->addWidget(showsLabel);

    m_showsEdit = new QLineEdit;
    m_showsEdit->setPlaceholderText(QStringLiteral("e.g. Roundtable Talk, Kingdom Connection"));
    m_showsEdit->setToolTip(QStringLiteral(
        "The show this shot belongs to (its namespace). Used by the SHOWS filter."));
    m_showsEdit->setEnabled(false);
    m_showsEdit->setReadOnly(true);   // show is changed via the dropdown / move
    showsRow->addWidget(m_showsEdit, 1);

    // Dropdown button: pick the single show this shot belongs to (its
    // namespace). Choosing a show MOVES the shot into it. "No Show" / "New
    // Show…" are also available.
    auto* showsDropBtn = new QToolButton;
    showsDropBtn->setText(QStringLiteral("Move ▾"));
    showsDropBtn->setToolTip(QStringLiteral("Move this shot to a show"));
    showsDropBtn->setFixedHeight(36);
    showsDropBtn->setPopupMode(QToolButton::InstantPopup);
    showsRow->addWidget(showsDropBtn);

    auto* showsMenu = new QMenu(showsDropBtn);
    showsDropBtn->setMenu(showsMenu);

    // Move the current shot into `show` (empty = No Show) via m_showsEdit +
    // onShotShowsChanged() (which relocates the file and updates the registry).
    auto moveToShow = [this](const QString& show) {
        if (!m_showsEdit) return;
        m_showsEdit->setText(show);
        onShotShowsChanged();
    };

    connect(showsMenu, &QMenu::aboutToShow, this, [this, showsMenu, moveToShow]() {
        if (m_destroying.load(std::memory_order_acquire)) return;
        showsMenu->clear();
        if (!m_showsEdit || !m_showsEdit->isEnabled()) {
            auto* a = showsMenu->addAction(tr("(open or create a shot first)"));
            a->setEnabled(false);
            return;
        }
        const QString cur = QString::fromStdString(m_currentShot.show());
        QAction* noShow = showsMenu->addAction(tr("No Show"));
        noShow->setCheckable(true);
        noShow->setChecked(cur.isEmpty());
        connect(noShow, &QAction::triggered, this, [moveToShow]() { moveToShow(QString()); });
        showsMenu->addSeparator();
        for (const QString& s : knownShows()) {
            QAction* a = showsMenu->addAction(s);
            a->setCheckable(true);
            a->setChecked(s.compare(cur, Qt::CaseInsensitive) == 0);
            connect(a, &QAction::triggered, this, [moveToShow, s]() { moveToShow(s); });
        }
        showsMenu->addSeparator();
        QAction* newAct = showsMenu->addAction(tr("New Show…"));
        connect(newAct, &QAction::triggered, this, [this, moveToShow]() {
            bool ok = false;
            QString name = QInputDialog::getText(this, tr("New Show"),
                tr("Show name:"), QLineEdit::Normal, QString(), &ok);
            if (!ok || name.trimmed().isEmpty()) return;
            name = name.trimmed();
            m_presetManager.addShow(name.toStdString());
            moveToShow(name);
        });
    });

    shotNameLayout->addLayout(showsRow);

    connect(m_showsEdit, &QLineEdit::editingFinished,
            this, [this]() {
        if (m_destroying.load(std::memory_order_acquire)) return;
        if (m_updating) return;
        onShotShowsChanged();
    });

    m_defaultShotCheck = new QCheckBox("Set as character's default shot");
    m_defaultShotCheck->setEnabled(false);
    m_defaultShotCheck->setVisible(false);

    // Default character row â€” compact toggle
    auto* defaultLayout = new QHBoxLayout;
    defaultLayout->setContentsMargins(0, 0, 0, 0);
    defaultLayout->setSpacing(3);

    m_defaultCharCombo = new QComboBox;
    m_defaultCharCombo->setToolTip("Character to set this shot as default for");
    m_defaultCharCombo->setEnabled(false);
    m_defaultCharCombo->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    // ComboBox styling inherited from panel stylesheet
    defaultLayout->addWidget(m_defaultCharCombo);

    m_setDefaultBtn = new QPushButton(QStringLiteral("SET DEFAULT"));
    m_setDefaultBtn->setToolTip("Set as this character's default shot");
    m_setDefaultBtn->setFixedHeight(36);
    m_setDefaultBtn->setObjectName("StarBtn");
    m_setDefaultBtn->setEnabled(false);
    defaultLayout->addWidget(m_setDefaultBtn);

    shotNameLayout->addLayout(defaultLayout);

    connect(m_setDefaultBtn, &QPushButton::clicked, this, [this]() {
        if (m_currentShot.name().empty()) return;
        int idx = m_defaultCharCombo->currentIndex();
        if (idx < 0) return;

        QString charName = m_defaultCharCombo->currentText();

        // When a specific show is selected in the SHOWS filter, set a
        // per-show default for that show; otherwise set the global default.
        const QString show = activeShowFilter();
        const bool perShow = !show.isEmpty()
            && show != QStringLiteral("__UNASSIGNED__");

        if (perShow) {
            m_presetManager.setShowDefaultShot(show.toStdString(),
                                               charName.toStdString(),
                                               m_currentShot.name());
            spdlog::info("ShotComposer: Set '{}' as default shot for '{}' in show '{}'",
                         m_currentShot.name(), charName.toStdString(), show.toStdString());
        } else {
            // Record this shot as the global default for the selected character
            m_characterDefaults[charName.toStdString()] = m_currentShot.name();
            saveDefaults();
            spdlog::info("ShotComposer: Set '{}' as default shot for '{}'",
                         m_currentShot.name(), charName.toStdString());
        }
        refreshShotList();
        QToolTip::showText(m_setDefaultBtn->mapToGlobal(QPoint(0, -30)),
                           perShow
                             ? QString("Set as default for %1 in \"%2\"").arg(charName, show)
                             : QString("Set as default for %1").arg(charName),
                           m_setDefaultBtn, {}, 2500);
    });

    connect(saveShotBtnProps, &QPushButton::clicked, this, [this]() {
        saveCurrentShot();
    });

    topLayout->addWidget(shotNameGroup);

    connect(m_shotNameEdit, &QLineEdit::textChanged,
            this, &ShotComposer::onShotNameChanged);

    // â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
    // Layers section (added to splitter bottom later)
    // â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
    auto* layersGroup = new QGroupBox(QStringLiteral("LAYERS"));
    // Group box styling inherited from panel stylesheet
    auto* layersVbox = new QVBoxLayout(layersGroup);
    layersVbox->setContentsMargins(m.spacingMd, m.spacingLg, m.spacingMd, m.spacingMd);
    layersVbox->setSpacing(m.spacingSm);

    m_layerList = new QListWidget;
    m_layerList->setObjectName("LayerList");
    m_layerList->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_layerList->setDragDropMode(QAbstractItemView::InternalMove);
    m_layerList->setDefaultDropAction(Qt::MoveAction);
    m_layerList->setMinimumHeight(100);
    m_layerList->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_layerList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_layerList->setUniformItemSizes(false);
    {
        const auto& cols = Theme::colors();
        m_layerList->setStyleSheet(QStringLiteral(
            "QListWidget#LayerList::item:selected {"
            "  background: %1; border-radius: 3px; }"
            "QListWidget#LayerList::item:hover:!selected {"
            "  background: %2; border-radius: 3px; }")
            .arg(Theme::hex(cols.accent))
            .arg(Theme::rgba(cols.text, 15)));
    }
    m_layerList->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_layerList, &QListWidget::customContextMenuRequested,
            this, [this](const QPoint& pos) {
        auto* item = m_layerList->itemAt(pos);
        if (!item) return;

        QMenu menu(this);
        QAction* actCopyLayer     = menu.addAction("Copy Layer (Ctrl+C)");
        QAction* actPasteLayer    = menu.addAction("Paste Layer (Ctrl+V)");
        menu.addSeparator();
        QAction* actGroup   = menu.addAction("Group Layers (Ctrl+G)");
        QAction* actUngroup = menu.addAction("Ungroup (Ctrl+Shift+G)");
        menu.addSeparator();
        QAction* actCopyTransform = menu.addAction("Copy Transform (Ctrl+Shift+C)");
        QAction* actPasteTransform= menu.addAction("Paste Transform (Ctrl+Shift+V)");
        menu.addSeparator();
        QAction* actDelete        = menu.addAction(QStringLiteral("\xF0\x9F\x97\x91 Delete (Del)"));

        int selCount = m_layerList->selectionModel()->selectedRows().size();
        actGroup->setEnabled(selCount >= 1);
        actPasteLayer->setEnabled(!m_layerClipboard.empty());
        actPasteTransform->setEnabled(m_transformClipboard.has_value());

        QAction* chosen = menu.exec(m_layerList->viewport()->mapToGlobal(pos));
        if (!chosen) return;

        if (chosen == actCopyLayer)          copySelectedLayer();
        else if (chosen == actPasteLayer)    pasteLayer();
        else if (chosen == actGroup)         groupSelectedLayers();
        else if (chosen == actUngroup)       ungroupSelectedGroup();
        else if (chosen == actCopyTransform) copyTransform();
        else if (chosen == actPasteTransform)pasteTransform();
        else if (chosen == actDelete) {
            auto sel = m_layerList->selectionModel()->selectedRows();
            if (sel.isEmpty()) return;
            pushUndoState();
            std::vector<int> rows;
            for (const auto& idx : sel) rows.push_back(idx.row());
            std::sort(rows.rbegin(), rows.rend());
            for (int row : rows) {
                if (row < 0 || row >= m_currentShot.layerCount()) continue;
                const auto& ref = m_currentShot.layerOrder()[static_cast<size_t>(row)];
                if (ref.type == LayerType::Character) removeCharacter(ref.index);
                else                                  removeBackground(ref.index);
            }
        }
    });
    layersVbox->addWidget(m_layerList, 1);

    // Layer action buttons â€” compact Photoshop-style toolbar
    auto* layerBtns = new QHBoxLayout;
    layerBtns->setSpacing(m.spacingXxs);
    layerBtns->setContentsMargins(0, m.spacingXxs, 0, 0);

    auto makeLayerBtn = [](const QString& text, const QString& tip) {
        auto* btn = new QPushButton(text);
        btn->setToolTip(tip);
        btn->setFixedSize(40, 36);
        btn->setFocusPolicy(Qt::NoFocus);
        btn->setObjectName("LayerToolBtn");
        return btn;
    };

    m_addCharBtn = makeLayerBtn(QStringLiteral("\xF0\x9F\x91\xA4"), "Add selected character");
    layerBtns->addWidget(m_addCharBtn);

    m_addBgBtn = makeLayerBtn(QStringLiteral("\xF0\x9F\x96\xBC"), "Add selected background");
    layerBtns->addWidget(m_addBgBtn);

    m_addGroupBtn = makeLayerBtn(QStringLiteral("\xF0\x9F\x93\x81"), "Create layer group (Ctrl+G)");
    layerBtns->addWidget(m_addGroupBtn);

    layerBtns->addStretch();

    m_layerUpBtn = makeLayerBtn(QStringLiteral("\u25B2"), "Move layer up");
    layerBtns->addWidget(m_layerUpBtn);

    m_layerDownBtn = makeLayerBtn(QStringLiteral("\u25BC"), "Move layer down");
    layerBtns->addWidget(m_layerDownBtn);

    m_removeLayerBtn = makeLayerBtn(QStringLiteral("\xF0\x9F\x97\x91"), "Remove layer (Del)");
    m_removeLayerBtn->setObjectName("LayerToolBtnDanger");
    layerBtns->addWidget(m_removeLayerBtn);

    layersVbox->addLayout(layerBtns);

    // Use a stacked widget: page 0 = empty, page 1 = char tabs, page 2 = bg tabs
    m_propsStack = new QStackedWidget;
    m_propsStack->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

    // â”€â”€ Page 0: Empty placeholder â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    m_emptyPropsLabel = new QLabel("Select a layer to edit properties");
    m_emptyPropsLabel->setAlignment(Qt::AlignCenter);
    m_emptyPropsLabel->setObjectName("EmptyLabel");
    m_propsStack->addWidget(m_emptyPropsLabel);  // index 0

    // â”€â”€ Page 1: Character properties (side-by-side columns) â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    m_charPropsGroup = new QGroupBox;
    m_charPropsGroup->setFlat(true);
    auto* charColumnsLayout = new QHBoxLayout(m_charPropsGroup);
    charColumnsLayout->setContentsMargins(0, 0, 0, 0);
    charColumnsLayout->setSpacing(m.spacingSm);

    // Keep m_layerPropsTabs as nullptr â€” no longer used as a tab widget
    m_layerPropsTabs = nullptr;

    // â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
    // LEFT COLUMN: Transform + Crop
    // â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
    auto* transformColumn = new QWidget;
    transformColumn->setObjectName("TransformTabBg");
    auto* transformGrid = new QGridLayout(transformColumn);
    transformGrid->setContentsMargins(m.spacingLg, m.spacingXl, m.spacingLg, m.spacingLg);
    transformGrid->setHorizontalSpacing(10);
    transformGrid->setVerticalSpacing(10);

    // TRANSFORM header label (matching CHARACTER header on right)
    auto* transformHeaderLabel = new QLabel("TRANSFORM");
    transformHeaderLabel->setObjectName("PropLabel");
    transformHeaderLabel->setStyleSheet(QStringLiteral(
        "font-weight: bold; font-size: 18px; padding-bottom: 10px;"));
    int tRow = 0;
    transformGrid->addWidget(transformHeaderLabel, tRow, 0, 1, 5);
    ++tRow;

    auto makeShotSep = [transformColumn]() {
        auto* line = new QFrame(transformColumn);
        line->setFrameShape(QFrame::HLine);
        line->setFrameShadow(QFrame::Plain);
        // Separator styling inherited from panel stylesheet (frameShape="4")
        return line;
    };

    auto makeShotAxisLabel = [transformColumn](const QString& text) {
        auto* lbl = new QLabel(text, transformColumn);
        lbl->setFixedWidth(24);
        lbl->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        lbl->setObjectName("AxisLabel");
        lbl->setStyleSheet(QStringLiteral("font-size: 13px; font-weight: 600;"));
        return lbl;
    };

    // â”€â”€ Position â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    m_posXSpin = new ScrubbySpinBox;
    m_posXSpin->setRange(-200.0, 200.0);
    m_posXSpin->setScrubStep(0.1);
    m_posXSpin->setDecimals(1);
    m_posXSpin->setSuffix("%");

    m_posYSpin = new ScrubbySpinBox;
    m_posYSpin->setRange(-200.0, 200.0);
    m_posYSpin->setScrubStep(0.1);
    m_posYSpin->setDecimals(1);
    m_posYSpin->setSuffix("%");

    auto* posLabel = new QLabel("Position", transformColumn);
    posLabel->setObjectName("PropLabel");
    transformGrid->addWidget(posLabel,                    tRow, 0);
    transformGrid->addWidget(makeShotAxisLabel("X"),      tRow, 1);
    transformGrid->addWidget(m_posXSpin,                  tRow, 2);
    transformGrid->addWidget(makeShotAxisLabel("Y"),      tRow, 3);
    transformGrid->addWidget(m_posYSpin,                  tRow, 4);
    ++tRow;
    transformGrid->addWidget(makeShotSep(), tRow, 0, 1, 5);
    ++tRow;

    // â”€â”€ Scale â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    m_scaleSpin = new ScrubbySpinBox;
    m_scaleSpin->setRange(10.0, 1000.0);
    m_scaleSpin->setValue(100.0);
    m_scaleSpin->setScrubStep(0.5);
    m_scaleSpin->setDecimals(1);
    m_scaleSpin->setSuffix("%");

    auto* scaleLabel = new QLabel("Scale", transformColumn);
    scaleLabel->setObjectName("PropLabel");
    transformGrid->addWidget(scaleLabel,  tRow, 0);
    transformGrid->addWidget(m_scaleSpin, tRow, 1, 1, 4);
    ++tRow;
    transformGrid->addWidget(makeShotSep(), tRow, 0, 1, 5);
    ++tRow;

    // â”€â”€ Rotation â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    m_rotationSpin = new ScrubbySpinBox;
    m_rotationSpin->setRange(-180.0, 180.0);
    m_rotationSpin->setValue(0.0);
    m_rotationSpin->setScrubStep(0.5);
    m_rotationSpin->setDecimals(1);
    m_rotationSpin->setSuffix(QStringLiteral("\xC2\xB0"));

    auto* rotLabel = new QLabel("Rotation", transformColumn);
    rotLabel->setObjectName("PropLabel");
    transformGrid->addWidget(rotLabel,        tRow, 0);
    transformGrid->addWidget(m_rotationSpin,  tRow, 1, 1, 4);
    ++tRow;
    transformGrid->addWidget(makeShotSep(), tRow, 0, 1, 5);
    ++tRow;

    // â”€â”€ Opacity â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    m_opacitySpin = new ScrubbySpinBox;
    m_opacitySpin->setRange(0.0, 100.0);
    m_opacitySpin->setValue(100.0);
    m_opacitySpin->setScrubStep(0.5);
    m_opacitySpin->setDecimals(1);
    m_opacitySpin->setSuffix("%");

    auto* opacLabel = new QLabel("Opacity", transformColumn);
    opacLabel->setObjectName("PropLabel");
    transformGrid->addWidget(opacLabel,     tRow, 0);
    transformGrid->addWidget(m_opacitySpin, tRow, 1, 1, 4);
    ++tRow;
    transformGrid->addWidget(makeShotSep(), tRow, 0, 1, 5);
    ++tRow;

    // â”€â”€ Blur â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    m_blurSpin = new ScrubbySpinBox;
    m_blurSpin->setRange(0.0, 100.0);
    m_blurSpin->setValue(0.0);
    m_blurSpin->setScrubStep(1.0);
    m_blurSpin->setDecimals(1);
    m_blurSpin->setSuffix(" px");

    auto* blurLabel = new QLabel("Blur", transformColumn);
    blurLabel->setObjectName("PropLabel");
    transformGrid->addWidget(blurLabel,    tRow, 0);
    transformGrid->addWidget(m_blurSpin,   tRow, 1, 1, 4);
    ++tRow;
    transformGrid->addWidget(makeShotSep(), tRow, 0, 1, 5);
    ++tRow;

    // â”€â”€ Options â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    m_flipXCheck = new QCheckBox("Flip Horizontal");
    // Checkbox styling inherited from panel stylesheet
    transformGrid->addWidget(m_flipXCheck, tRow, 0, 1, 5);
    ++tRow;

    m_flipYCheck = new QCheckBox("Flip Vertical");
    // Checkbox styling inherited from panel stylesheet
    transformGrid->addWidget(m_flipYCheck, tRow, 0, 1, 5);
    ++tRow;

    m_visibleCheck = new QCheckBox("Visible");
    m_visibleCheck->setChecked(true);
    // Checkbox styling inherited from panel stylesheet
    transformGrid->addWidget(m_visibleCheck, tRow, 0, 1, 5);
    ++tRow;

    transformGrid->setColumnStretch(0, 0);
    transformGrid->setColumnStretch(1, 0);
    transformGrid->setColumnStretch(2, 1);
    transformGrid->setColumnStretch(3, 0);
    transformGrid->setColumnStretch(4, 1);

    // â”€â”€ Crop section (below transform with spacing) â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    // Add spacing between transform controls and crop
    transformGrid->setRowMinimumHeight(tRow, m.spacingLg);
    ++tRow;

    m_cropGroup = new QGroupBox(QStringLiteral("Crop"));
    m_cropGroup->setCheckable(true);
    m_cropGroup->setChecked(false);

    auto* cropGrid = new QGridLayout(m_cropGroup);
    cropGrid->setContentsMargins(m.spacingLg, 16, m.spacingLg, m.spacingMd);
    cropGrid->setHorizontalSpacing(8);
    cropGrid->setVerticalSpacing(6);

    // Row 0: Left / Right
    auto* leftLabel = new QLabel("Left", m_cropGroup);
    m_cropLeftSpin = new ScrubbySpinBox;
    m_cropLeftSpin->setRange(0.0, 100.0);
    m_cropLeftSpin->setScrubStep(0.5);
    m_cropLeftSpin->setDecimals(1);
    m_cropLeftSpin->setSuffix("%");
    cropGrid->addWidget(leftLabel,       0, 0);
    cropGrid->addWidget(m_cropLeftSpin,  0, 1);

    auto* rightLabel = new QLabel("Right", m_cropGroup);
    m_cropRightSpin = new ScrubbySpinBox;
    m_cropRightSpin->setRange(0.0, 100.0);
    m_cropRightSpin->setScrubStep(0.5);
    m_cropRightSpin->setDecimals(1);
    m_cropRightSpin->setSuffix("%");
    cropGrid->addWidget(rightLabel,       0, 2);
    cropGrid->addWidget(m_cropRightSpin,  0, 3);

    // Row 1: Top / Bottom
    auto* topLabel = new QLabel("Top", m_cropGroup);
    m_cropTopSpin = new ScrubbySpinBox;
    m_cropTopSpin->setRange(0.0, 100.0);
    m_cropTopSpin->setScrubStep(0.5);
    m_cropTopSpin->setDecimals(1);
    m_cropTopSpin->setSuffix("%");
    cropGrid->addWidget(topLabel,       1, 0);
    cropGrid->addWidget(m_cropTopSpin,  1, 1);

    auto* botLabel = new QLabel("Bottom", m_cropGroup);
    m_cropBottomSpin = new ScrubbySpinBox;
    m_cropBottomSpin->setRange(0.0, 100.0);
    m_cropBottomSpin->setScrubStep(0.5);
    m_cropBottomSpin->setDecimals(1);
    m_cropBottomSpin->setSuffix("%");
    cropGrid->addWidget(botLabel,          1, 2);
    cropGrid->addWidget(m_cropBottomSpin,  1, 3);

    cropGrid->setColumnStretch(0, 0);
    cropGrid->setColumnStretch(1, 1);
    cropGrid->setColumnStretch(2, 0);
    cropGrid->setColumnStretch(3, 1);

    // Row 2: Reset button
    auto* btnResetCrop = new QPushButton("Reset Crop");
    btnResetCrop->setFixedHeight(36);
    cropGrid->addWidget(btnResetCrop, 2, 0, 1, 4);

    transformGrid->addWidget(m_cropGroup, tRow, 0, 1, 5);
    ++tRow;

    transformGrid->setRowStretch(tRow, 1);  // push everything up

    charColumnsLayout->addWidget(transformColumn, 1);

    // â”€â”€ Vertical divider between columns â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    auto* columnDivider = new QFrame;
    columnDivider->setFrameShape(QFrame::VLine);
    columnDivider->setFrameShadow(QFrame::Plain);
    columnDivider->setFixedWidth(1);
    columnDivider->setStyleSheet(QStringLiteral(
        "QFrame { color: %1; background: %1; border: none; }")
        .arg(Theme::hex(c.borderLight)));
    charColumnsLayout->addWidget(columnDivider);

    // â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
    // RIGHT COLUMN: Character settings (Live2D / Spine)
    // â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
    auto* charSettingsColumn = new QWidget;
    auto* charSettingsLayout = new QVBoxLayout(charSettingsColumn);
    charSettingsLayout->setContentsMargins(m.spacingLg, m.spacingXl, m.spacingLg, m.spacingLg);
    charSettingsLayout->setSpacing(m.spacingMd);

    auto* charHeaderLabel = new QLabel("CHARACTER");
    charHeaderLabel->setObjectName("PropLabel");
    charHeaderLabel->setStyleSheet(QStringLiteral(
        "font-weight: bold; font-size: 18px; padding-bottom: 10px;"));
    charSettingsLayout->addWidget(charHeaderLabel);

    auto* charForm = new QFormLayout;
    charForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    charForm->setVerticalSpacing(8);
    charForm->setLabelAlignment(Qt::AlignRight);

    m_outfitCombo = new QComboBox;
    charForm->addRow("Outfit:", m_outfitCombo);

    m_stanceCombo = new QComboBox;
    m_stanceCombo->addItems({"Default", "Aim", "Cover"});
    charForm->addRow("Stance:", m_stanceCombo);

    m_animCombo = new QComboBox;
    m_animCombo->addItem("idle");
    charForm->addRow("Anim:", m_animCombo);

    m_talkingCheck = new QCheckBox("Talking Loop");
    charForm->addRow("", m_talkingCheck);

    charSettingsLayout->addLayout(charForm);
    charSettingsLayout->addStretch();

    charColumnsLayout->addWidget(charSettingsColumn, 1);

    m_charPropsGroup->setVisible(false);
    m_propsStack->addWidget(m_charPropsGroup);   // index 1

    // â”€â”€ Connect character property changes â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    auto charChanged = [this]() { onCharacterPropertyChanged(); };
    connect(m_posXSpin,      QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, charChanged);
    connect(m_posYSpin,      QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, charChanged);
    connect(m_scaleSpin,     QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, charChanged);
    connect(m_rotationSpin,  QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, charChanged);
    connect(m_opacitySpin,   QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, charChanged);
    connect(m_blurSpin,      QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, charChanged);
    connect(m_outfitCombo,   QOverload<int>::of(&QComboBox::currentIndexChanged), this, charChanged);
    connect(m_stanceCombo,   QOverload<int>::of(&QComboBox::currentIndexChanged), this, charChanged);
    connect(m_animCombo,     QOverload<int>::of(&QComboBox::currentIndexChanged), this, charChanged);
    connect(m_talkingCheck,  &QCheckBox::toggled, this, charChanged);
    connect(m_flipXCheck,    &QCheckBox::toggled, this, charChanged);
    connect(m_flipYCheck,    &QCheckBox::toggled, this, charChanged);
    connect(m_visibleCheck,  &QCheckBox::toggled, this, charChanged);
    connect(m_cropLeftSpin,  QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, charChanged);
    connect(m_cropRightSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, charChanged);
    connect(m_cropTopSpin,   QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, charChanged);
    connect(m_cropBottomSpin,QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, charChanged);
    connect(btnResetCrop, &QPushButton::clicked, this, [this]() {
        m_updating = true;
        m_cropLeftSpin->setValue(0);
        m_cropRightSpin->setValue(0);
        m_cropTopSpin->setValue(0);
        m_cropBottomSpin->setValue(0);
        m_updating = false;
        onCharacterPropertyChanged();
    });

    // â”€â”€ Page 2: Background properties (tabbed) â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    m_bgPropsGroup = new QGroupBox;
    m_bgPropsGroup->setFlat(true);
    auto* bgTabsLayout = new QVBoxLayout(m_bgPropsGroup);
    bgTabsLayout->setContentsMargins(0, 0, 0, 0);

    auto* bgTabs = new QTabWidget;
    // Tab styling inherited from panel stylesheet

    // BG Transform tab
    auto* bgTransformTab = new QWidget;
    bgTransformTab->setObjectName("TransformTabBg");
    auto* bgGrid = new QGridLayout(bgTransformTab);
    bgGrid->setContentsMargins(m.spacingLg, m.spacingLg, m.spacingLg, m.spacingMd);
    bgGrid->setHorizontalSpacing(8);
    bgGrid->setVerticalSpacing(6);

    auto makeBgSep = [bgTransformTab]() {
        auto* line = new QFrame(bgTransformTab);
        line->setFrameShape(QFrame::HLine);
        line->setFrameShadow(QFrame::Plain);
        // Separator styling inherited from panel stylesheet (frameShape="4")
        return line;
    };

    auto makeBgAxisLabel = [bgTransformTab](const QString& text) {
        auto* lbl = new QLabel(text, bgTransformTab);
        lbl->setFixedWidth(20);
        lbl->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        lbl->setObjectName("AxisLabel");
        return lbl;
    };

    int bgRow = 0;

    // â”€â”€ Position â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    m_bgPosXSpin = new ScrubbySpinBox;
    m_bgPosXSpin->setRange(-200.0, 200.0);
    m_bgPosXSpin->setScrubStep(1.0);
    m_bgPosXSpin->setDecimals(1);
    m_bgPosXSpin->setSuffix("%");

    m_bgPosYSpin = new ScrubbySpinBox;
    m_bgPosYSpin->setRange(-200.0, 200.0);
    m_bgPosYSpin->setScrubStep(1.0);
    m_bgPosYSpin->setDecimals(1);
    m_bgPosYSpin->setSuffix("%");

    auto* bgPosLabel = new QLabel("Position", bgTransformTab);
    bgPosLabel->setObjectName("PropLabel");
    bgGrid->addWidget(bgPosLabel,                 bgRow, 0);
    bgGrid->addWidget(makeBgAxisLabel("X"),       bgRow, 1);
    bgGrid->addWidget(m_bgPosXSpin,               bgRow, 2);
    bgGrid->addWidget(makeBgAxisLabel("Y"),       bgRow, 3);
    bgGrid->addWidget(m_bgPosYSpin,               bgRow, 4);
    ++bgRow;
    bgGrid->addWidget(makeBgSep(), bgRow, 0, 1, 5);
    ++bgRow;

    // â”€â”€ Scale â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    m_bgScaleSpin = new ScrubbySpinBox;
    m_bgScaleSpin->setRange(10.0, 1000.0);
    m_bgScaleSpin->setScrubStep(1.0);
    m_bgScaleSpin->setDecimals(1);
    m_bgScaleSpin->setSuffix("%");

    auto* bgScaleLabel = new QLabel("Scale", bgTransformTab);
    bgScaleLabel->setObjectName("PropLabel");
    bgGrid->addWidget(bgScaleLabel,   bgRow, 0);
    bgGrid->addWidget(m_bgScaleSpin,  bgRow, 1, 1, 4);
    ++bgRow;
    bgGrid->addWidget(makeBgSep(), bgRow, 0, 1, 5);
    ++bgRow;

    // â”€â”€ Opacity â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    m_bgOpacitySpin = new ScrubbySpinBox;
    m_bgOpacitySpin->setRange(0.0, 100.0);
    m_bgOpacitySpin->setValue(100.0);
    m_bgOpacitySpin->setScrubStep(1.0);
    m_bgOpacitySpin->setDecimals(0);
    m_bgOpacitySpin->setSuffix("%");

    auto* bgOpacLabel = new QLabel("Opacity", bgTransformTab);
    bgOpacLabel->setObjectName("PropLabel");
    bgGrid->addWidget(bgOpacLabel,      bgRow, 0);
    bgGrid->addWidget(m_bgOpacitySpin,  bgRow, 1, 1, 4);
    ++bgRow;
    bgGrid->addWidget(makeBgSep(), bgRow, 0, 1, 5);
    ++bgRow;

    // â”€â”€ Blur â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    m_bgBlurSpin = new ScrubbySpinBox;
    m_bgBlurSpin->setRange(0.0, 100.0);
    m_bgBlurSpin->setValue(0.0);
    m_bgBlurSpin->setScrubStep(1.0);
    m_bgBlurSpin->setDecimals(1);
    m_bgBlurSpin->setSuffix(" px");

    auto* bgBlurLabel = new QLabel("Blur", bgTransformTab);
    bgBlurLabel->setObjectName("PropLabel");
    bgGrid->addWidget(bgBlurLabel,     bgRow, 0);
    bgGrid->addWidget(m_bgBlurSpin,    bgRow, 1, 1, 4);

    ++bgRow;
    bgGrid->setRowStretch(bgRow, 1);  // push everything up

    bgGrid->setColumnStretch(0, 0);
    bgGrid->setColumnStretch(1, 0);
    bgGrid->setColumnStretch(2, 1);
    bgGrid->setColumnStretch(3, 0);
    bgGrid->setColumnStretch(4, 1);

    bgTabs->addTab(bgTransformTab, "Transform");

    // BG Video Timing tab
    auto* videoTimingTab = new QWidget;
    auto* videoTimingTabLayout = new QVBoxLayout(videoTimingTab);
    videoTimingTabLayout->setContentsMargins(m.spacingXs, m.spacingSm, m.spacingXs, m.spacingXs);

    m_videoTimingGroup = new QGroupBox(QStringLiteral("\xF0\x9F\x8E\xAC VIDEO IN/OUT"));
    // QGroupBox styling inherited from panel stylesheet
    auto* timingLayout = new QFormLayout(m_videoTimingGroup);
    timingLayout->setVerticalSpacing(4);

    m_videoInSpin = new ScrubbySpinBox;
    m_videoInSpin->setRange(0.0, 36000.0);
    m_videoInSpin->setScrubStep(0.1);
    m_videoInSpin->setDecimals(2);
    m_videoInSpin->setSuffix(" s");
    timingLayout->addRow("In:", m_videoInSpin);

    m_videoOutSpin = new ScrubbySpinBox;
    m_videoOutSpin->setRange(0.0, 36000.0);
    m_videoOutSpin->setScrubStep(0.1);
    m_videoOutSpin->setDecimals(2);
    m_videoOutSpin->setSuffix(" s");
    m_videoOutSpin->setSpecialValueText("End");
    timingLayout->addRow("Out:", m_videoOutSpin);

    auto* btnResetTiming = new QPushButton("Reset Timing");
    // Button styling inherited from panel stylesheet
    timingLayout->addRow(btnResetTiming);

    videoTimingTabLayout->addWidget(m_videoTimingGroup);
    videoTimingTabLayout->addStretch();
    bgTabs->addTab(videoTimingTab, "Video I/O");

    bgTabsLayout->addWidget(bgTabs);
    m_bgPropsGroup->setVisible(false);
    m_propsStack->addWidget(m_bgPropsGroup);     // index 2

    // â”€â”€ Connect BG / video property changes â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    connect(m_videoInSpin,  QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this]() { onVideoTimingChanged(); });
    connect(m_videoOutSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this]() { onVideoTimingChanged(); });
    connect(btnResetTiming, &QPushButton::clicked, this, [this]() {
        m_updating = true;
        m_videoInSpin->setValue(0);
        m_videoOutSpin->setValue(0);
        m_updating = false;
        onVideoTimingChanged();
    });

    auto bgChanged = [this]() { onBackgroundPropertyChanged(); };
    connect(m_bgPosXSpin,    QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, bgChanged);
    connect(m_bgPosYSpin,    QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, bgChanged);
    connect(m_bgScaleSpin,   QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, bgChanged);
    connect(m_bgOpacitySpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, bgChanged);
    connect(m_bgBlurSpin,    QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, bgChanged);

    topLayout->addWidget(m_propsStack, 1);

    // â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
    // Camera spins â€” hidden, kept for preset load/save compatibility
    // â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
    m_cameraZoomSpin = new ScrubbySpinBox;
    m_cameraZoomSpin->setRange(10.0, 1000.0);
    m_cameraZoomSpin->setValue(100.0);
    m_cameraZoomSpin->setVisible(false);

    m_cameraPanXSpin = new ScrubbySpinBox;
    m_cameraPanXSpin->setRange(-100.0, 100.0);
    m_cameraPanXSpin->setValue(0.0);
    m_cameraPanXSpin->setVisible(false);

    m_cameraPanYSpin = new ScrubbySpinBox;
    m_cameraPanYSpin->setRange(-100.0, 100.0);
    m_cameraPanYSpin->setValue(0.0);
    m_cameraPanYSpin->setVisible(false);

    // Add top section (Shot Name + Properties) to the splitter
    rightSplitter->addWidget(topSection);

    // â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
    // BOTTOM SECTION: Layers
    // â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
    rightSplitter->addWidget(layersGroup);

    // Give properties section ~60% and layers ~40% initial split
    rightSplitter->setStretchFactor(0, 3);  // Properties
    rightSplitter->setStretchFactor(1, 2);  // Layers

    connect(m_cameraZoomSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, &ShotComposer::onCameraPropertyChanged);
    connect(m_cameraPanXSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, &ShotComposer::onCameraPropertyChanged);
    connect(m_cameraPanYSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, &ShotComposer::onCameraPropertyChanged);

    // â”€â”€ Layer list connections â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    connect(m_layerList, &QListWidget::currentRowChanged,
            this, &ShotComposer::onLayerListSelectionChanged);

    // Keep SpinePreviewWidget's multi-select set in sync with the list's
    // actual selection (not just current-row changes).  This fires whenever
    // the user Ctrl/Shift-clicks in the layer list.
    connect(m_layerList->selectionModel(), &QItemSelectionModel::selectionChanged,
            this, [this]() {
#ifdef ROUNDTABLE_HAS_SPINE
        if (!m_spinePreview) return;
        QSet<int> sel;
        const auto rows = m_layerList->selectionModel()->selectedRows();
        for (const auto& idx : rows)
            sel.insert(idx.row());
        if (sel.isEmpty()) {
            int row = m_layerList->currentRow();
            if (row >= 0) sel.insert(row);
        }
        m_spinePreview->setSelectedLayers(sel);
#endif
    });

    connect(m_layerList->model(), &QAbstractItemModel::rowsMoved,
            this, [this](const QModelIndex&, int srcStart, int /*srcEnd*/,
                         const QModelIndex&, int dstRow) {
        int from = srcStart;
        int to   = (dstRow > from) ? dstRow - 1 : dstRow;
        if (from == to) return;

        pushUndoState();
        if (from < to) {
            for (int i = from; i < to; ++i)
                m_currentShot.swapLayers(i, i + 1);
        } else {
            for (int i = from; i > to; --i)
                m_currentShot.swapLayers(i, i - 1);
        }
        m_selectedLayer = to;
        refreshLayerList();
        populateLayerProperties();
        updatePreview();
        emit shotChanged();
    });

    connect(m_addCharBtn, &QPushButton::clicked, this, [this]() {
        auto items = m_characterLibrary->selectedItems();
        if (!items.isEmpty()) {
            auto* item = items.first();
            if (item->data(Qt::UserRole).toString() == QStringLiteral("video")) {
                addCharacter(item->text().toStdString(),
                             item->data(Qt::UserRole + 1).toString().toStdString(),
                             item->data(Qt::UserRole + 2).toString().toStdString());
            } else {
                addCharacter(item->data(Qt::UserRole).toString().toStdString());
            }
        }
    });

    connect(m_addBgBtn, &QPushButton::clicked, this, [this]() {
        auto items = m_backgroundLibrary->selectedItems();
        if (!items.isEmpty()) {
            auto* item = items.first();
            // Use relative path for subfolder backgrounds (UserRole+2) or display text for root
            QString bgPath = item->data(Qt::UserRole + 2).toString();
            if (bgPath.isEmpty())
                bgPath = item->text();
            addBackground(bgPath.toStdString());
        }
    });

    // Remove selected layers (supports multi-selection)
    connect(m_removeLayerBtn, &QPushButton::clicked, this, [this]() {
        auto sel = m_layerList->selectionModel()->selectedRows();
        if (sel.isEmpty()) return;
        pushUndoState();
        // Remove in reverse order to preserve indices
        std::vector<int> rows;
        for (const auto& idx : sel) rows.push_back(idx.row());
        std::sort(rows.rbegin(), rows.rend());
        for (int row : rows) {
            if (row < 0 || row >= m_currentShot.layerCount()) continue;
            const auto& ref = m_currentShot.layerOrder()[static_cast<size_t>(row)];
            if (ref.type == LayerType::Character)
                removeCharacter(ref.index);
            else
                removeBackground(ref.index);
        }
    });

    // Delete key shortcut â€” supports multi-selection
    auto* deleteShortcut = new QShortcut(QKeySequence(Qt::Key_Delete), this);
    deleteShortcut->setContext(Qt::WidgetWithChildrenShortcut);
    connect(deleteShortcut, &QShortcut::activated, this, [this]() {
        auto* focused = qApp->focusWidget();
        if (qobject_cast<QLineEdit*>(focused) || qobject_cast<QComboBox*>(focused))
            return;
        auto sel = m_layerList->selectionModel()->selectedRows();
        if (sel.isEmpty()) return;
        pushUndoState();
        std::vector<int> rows;
        for (const auto& idx : sel) rows.push_back(idx.row());
        std::sort(rows.rbegin(), rows.rend());
        for (int row : rows) {
            if (row < 0 || row >= m_currentShot.layerCount()) continue;
            const auto& ref = m_currentShot.layerOrder()[static_cast<size_t>(row)];
            if (ref.type == LayerType::Character)
                removeCharacter(ref.index);
            else
                removeBackground(ref.index);
        }
    });

    // â”€â”€ Keyboard shortcuts â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    // NOTE: Undo/Redo shortcuts are handled by MainWindow::onUndo/onRedo
    // which delegates to ShotComposer::undo()/redo() when on the COMPOSE page.
    // Adding local shortcuts here causes Qt ambiguous-shortcut conflicts with
    // the menu-bar actions, preventing Ctrl+Z/Ctrl+Y from reaching either handler.

    // Ctrl+S is handled via eventFilter instead of a QShortcut to avoid
    // ambiguous-shortcut conflicts with MainWindow's File > Save action
    // (which also uses QKeySequence::Save / WindowShortcut context).
    // Install on all major focusable children so Ctrl+S works regardless
    // of which child widget has keyboard focus.
#ifdef ROUNDTABLE_HAS_SPINE
    if (m_spinePreview) {
        m_spinePreview->installEventFilter(this);
        m_spinePreview->setAcceptDrops(true);
    }
#endif
    if (m_layerList) {
        m_layerList->installEventFilter(this);
        m_layerList->viewport()->setAcceptDrops(true);
        m_layerList->viewport()->installEventFilter(this);

        // Drop indicator line overlay (thin colored bar)
        m_dropIndicatorLine = new QWidget(m_layerList->viewport());
        m_dropIndicatorLine->setFixedHeight(3);
        m_dropIndicatorLine->setStyleSheet(
            QStringLiteral("background: %1; border-radius: 1px;")
            .arg(Theme::hex(Theme::colors().accent)));
        m_dropIndicatorLine->hide();
        m_dropIndicatorLine->setAttribute(Qt::WA_TransparentForMouseEvents);
    }
    if (m_shotList)    m_shotList->installEventFilter(this);
    if (m_charFilterList) m_charFilterList->installEventFilter(this);
    if (m_shotNameEdit) m_shotNameEdit->installEventFilter(this);

    // Install eventFilter on ALL children of topSection so Ctrl+S works
    // no matter which property widget has keyboard focus.
    {
        const auto children = topSection->findChildren<QWidget*>(QString(), Qt::FindChildrenRecursively);
        for (auto* w : children) {
            if (w->focusPolicy() != Qt::NoFocus)
                w->installEventFilter(this);
        }
    }

    auto* duplicateShortcut = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_D), this);
    duplicateShortcut->setContext(Qt::WidgetWithChildrenShortcut);
    connect(duplicateShortcut, &QShortcut::activated, this, &ShotComposer::duplicateCurrentShot);

    // Copy / Paste layers (Ctrl+C / Ctrl+V)
    auto* copyShortcut = new QShortcut(QKeySequence::Copy, this);
    copyShortcut->setContext(Qt::WidgetWithChildrenShortcut);
    connect(copyShortcut, &QShortcut::activated, this, &ShotComposer::copySelectedLayer);

    auto* pasteShortcut = new QShortcut(QKeySequence::Paste, this);
    pasteShortcut->setContext(Qt::WidgetWithChildrenShortcut);
    connect(pasteShortcut, &QShortcut::activated, this, &ShotComposer::pasteLayer);

    // Copy / Paste transform (Ctrl+Shift+C / Ctrl+Shift+V)
    auto* copyTransformShortcut = new QShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_C), this);
    copyTransformShortcut->setContext(Qt::WidgetWithChildrenShortcut);
    connect(copyTransformShortcut, &QShortcut::activated, this, &ShotComposer::copyTransform);

    auto* pasteTransformShortcut = new QShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_V), this);
    pasteTransformShortcut->setContext(Qt::WidgetWithChildrenShortcut);
    connect(pasteTransformShortcut, &QShortcut::activated, this, &ShotComposer::pasteTransform);

    auto* newShotShortcut = new QShortcut(QKeySequence::New, this);
    newShotShortcut->setContext(Qt::WidgetWithChildrenShortcut);
    connect(newShotShortcut, &QShortcut::activated, this, qOverload<>(&ShotComposer::newShot));

    // Move layers with Ctrl+Up/Down
    auto* moveUpShortcut = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_Up), this);
    moveUpShortcut->setContext(Qt::WidgetWithChildrenShortcut);
    connect(moveUpShortcut, &QShortcut::activated, this, &ShotComposer::moveSelectedLayerUp);

    auto* moveDownShortcut = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_Down), this);
    moveDownShortcut->setContext(Qt::WidgetWithChildrenShortcut);
    connect(moveDownShortcut, &QShortcut::activated, this, &ShotComposer::moveSelectedLayerDown);

    // Home key â€” reset viewport zoom/pan
    auto* homeShortcut = new QShortcut(QKeySequence(Qt::Key_Home), this);
    homeShortcut->setContext(Qt::WidgetWithChildrenShortcut);
    connect(homeShortcut, &QShortcut::activated, this, [this]() {
#ifdef ROUNDTABLE_HAS_SPINE
        if (m_spinePreview) {
            m_spinePreview->resetViewport();
            m_updating = true;
            m_cameraZoomSpin->setValue(100.0);
            m_cameraPanXSpin->setValue(0.0);
            m_cameraPanYSpin->setValue(0.0);
            m_updating = false;
            m_currentShot.setCameraZoom(1.0f);
            m_currentShot.setCameraX(0.0f);
            m_currentShot.setCameraY(0.0f);
            emit shotChanged();
        }
#endif
    });

    connect(m_layerUpBtn,   &QPushButton::clicked, this, &ShotComposer::moveSelectedLayerUp);
    connect(m_layerDownBtn, &QPushButton::clicked, this, &ShotComposer::moveSelectedLayerDown);
    connect(m_addGroupBtn,  &QPushButton::clicked, this, &ShotComposer::groupSelectedLayers);

    // ── Keyboard shortcuts ──────────────────────────────────────────────
    auto* grpShortcut = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_G), this);
    connect(grpShortcut, &QShortcut::activated, this, &ShotComposer::groupSelectedLayers);

    auto* ungrpShortcut = new QShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_G), this);
    connect(ungrpShortcut, &QShortcut::activated, this, &ShotComposer::ungroupSelectedGroup);

    return rightSplitter;
}

// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
// Refresh helpers
// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•


} // namespace rt
