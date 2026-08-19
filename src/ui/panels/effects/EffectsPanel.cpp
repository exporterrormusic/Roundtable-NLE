/*
 * EffectsPanel.cpp — Effects browser and stack manager.
 * Step 22: Effects System
 */

#include "panels/effects/EffectsPanel.h"
#include "PathUtils.h"

#include "Theme.h"

#include "timeline/Clip.h"
#include "timeline/ClipMutation.h"
#include "effects/Effect.h"
#include "effects/EffectStack.h"
#include "audiofx/FxChain.h"
#include "command/CommandStack.h"
#include "command/commands/EffectCommands.h"
#include "timeline/Transition.h"

#include <QDrag>
#include <QHBoxLayout>
#include <QMimeData>
#include <QPalette>
#include <QStyle>
#include <QToolButton>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>
#include <QMenu>
#include <QInputDialog>
#include <QMessageBox>
#include <QCoreApplication>

#include <fstream>
#include <functional>
#include <sstream>

namespace {
// MIME type used for effect drags so the timeline can identify them
// even when the Effects panel is in a separate floating window.
constexpr const char* kEffectMimeType = "application/x-roundtable-effect";
constexpr const char* kTransitionMimeType = "application/x-roundtable-transition";
// Audio DSP processors (EQ / Dynamics) live in a separate per-clip FxChain,
// not the EffectType/EffectStack system, so they carry their own MIME payload
// (the audiofx::ProcessorKind int) and a distinct item role.
constexpr const char* kAudioFxMimeType = "application/x-roundtable-audiofx";
constexpr int kAudioFxRole = Qt::UserRole + 3;
// Glitch presets are curated multi-effect macros, not a single EffectType, so
// they carry their own MIME payload (the GlitchPreset int) and item role.
constexpr const char* kGlitchPresetMimeType = "application/x-roundtable-glitch-preset";
constexpr int kGlitchPresetRole = Qt::UserRole + 4;

/// QTreeWidget subclass that embeds the effect type in custom MIME data
/// during drag operations, ensuring cross-window drops work correctly.
class EffectBrowserTree : public QTreeWidget {
public:
    using QTreeWidget::QTreeWidget;
protected:
    void startDrag(Qt::DropActions /*supportedActions*/) override {
        auto* item = currentItem();
        if (!item) return;

        auto* drag = new QDrag(this);
        auto* mimeData = new QMimeData;

        // Transition (UserRole+1), audio-FX processor (UserRole+3),
        // glitch preset (UserRole+4), or effect (UserRole).
        QVariant transData   = item->data(0, Qt::UserRole + 1);
        QVariant audioFxData = item->data(0, kAudioFxRole);
        QVariant glitchData  = item->data(0, kGlitchPresetRole);
        QVariant effectData  = item->data(0, Qt::UserRole);

        if (glitchData.isValid()) {
            mimeData->setData(kGlitchPresetMimeType, QByteArray::number(glitchData.toInt()));
        } else if (transData.isValid()) {
            int transType = transData.toInt();
            mimeData->setData(kTransitionMimeType, QByteArray::number(transType));
        } else if (audioFxData.isValid()) {
            mimeData->setData(kAudioFxMimeType, QByteArray::number(audioFxData.toInt()));
        } else if (effectData.isValid()) {
            int effectType = effectData.toInt();
            mimeData->setData(kEffectMimeType, QByteArray::number(effectType));
        } else {
            return;
        }

        drag->setMimeData(mimeData);
        drag->exec(Qt::CopyAction);
    }
};
} // anon namespace

namespace rt {

EffectsPanel::EffectsPanel(QWidget* parent)
    : QWidget(parent)
{
    // Resolve presets directory relative to the application
    m_presetsDir = std::filesystem::path(
        QCoreApplication::applicationDirPath().toStdString())
        .parent_path().parent_path() / "assets" / "presets" / "effects";
    std::filesystem::create_directories(m_presetsDir);
    setupUI();
}

EffectsPanel::~EffectsPanel() = default;

void EffectsPanel::setupUI()
{
    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(0);

    // ── Search bar row ─────────────────────────────────────────────────
    auto* searchRow = new QWidget(this);
    searchRow->setFixedHeight(28);
    searchRow->setStyleSheet(QStringLiteral(
        "background: %1; border-bottom: 1px solid %2;")
        .arg(Theme::hex(Theme::colors().surface2),
             Theme::hex(Theme::colors().panelBorder)));
    auto* searchLayout = new QHBoxLayout(searchRow);
    searchLayout->setContentsMargins(6, 0, 6, 0);
    searchLayout->setSpacing(4);

    m_searchField = new QLineEdit(searchRow);
    m_searchField->setPlaceholderText(QStringLiteral("\U0001F50D Search"));
    m_searchField->setClearButtonEnabled(true);
    m_searchField->setFixedHeight(22);
    m_searchField->setStyleSheet(QStringLiteral(
        "QLineEdit { background: %1; border: 1px solid %2; border-radius: 3px; "
        "color: %3; font-size: 12px; padding: 0 4px; }"
        "QLineEdit:focus { border: 1px solid %4; }")
        .arg(Theme::hex(Theme::colors().inputBg))
        .arg(Theme::hex(Theme::colors().controlBorder))
        .arg(Theme::hex(Theme::colors().textPrimary))
        .arg(Theme::hex(Theme::colors().controlBorderFocus)));
    connect(m_searchField, &QLineEdit::textChanged, this, [this](const QString& text) {
        filterBrowser(text);
    });
    searchLayout->addWidget(m_searchField);
    mainLayout->addWidget(searchRow);

    // ── Filter toolbar (Premiere Pro style icons) ──────────────────────
    auto* filterRow = new QWidget(this);
    filterRow->setFixedHeight(24);
    filterRow->setStyleSheet(QStringLiteral("background: %1; border-bottom: 1px solid %2;")
        .arg(Theme::hex(Theme::colors().surface2), Theme::hex(Theme::colors().panelBorder)));
    auto* filterLayout = new QHBoxLayout(filterRow);
    filterLayout->setContentsMargins(4, 0, 4, 0);
    filterLayout->setSpacing(2);

    auto makeFilterBtn = [&](const QString& icon, const QString& tip) {
        auto* btn = new QToolButton(filterRow);
        btn->setText(icon);
        btn->setToolTip(tip);
        btn->setFixedSize(20, 20);
        btn->setCheckable(true);
        btn->setStyleSheet(QStringLiteral(
            "QToolButton { background: transparent; color: %1; border: none; font-size: 12px; padding: 0; }"
            "QToolButton:hover { color: %2; }"
            "QToolButton:checked { color: %3; }")
            .arg(Theme::hex(Theme::colors().textSecondary),
                 Theme::hex(Theme::colors().textPrimary),
                 Theme::hex(Theme::colors().accent)));
        filterLayout->addWidget(btn);
        return btn;
    };

    makeFilterBtn(QStringLiteral("\u2261"), tr("Show/Hide Effects"));           // ≡
    makeFilterBtn(QStringLiteral("\u2261\u00B2"), tr("Show 32-bit Effects"));   // ≡²
    makeFilterBtn(QStringLiteral("\u223F"), tr("Show Accelerated Effects"));    // ∿
    filterLayout->addStretch();
    mainLayout->addWidget(filterRow);

    // ── Effect browser tree ────────────────────────────────────────────
    // Use QGroupBox with overridden style to keep compatibility while
    // rendering with zero chrome.
    m_browserGroup = new QGroupBox(this);
    m_browserGroup->setTitle(QString());
    m_browserGroup->setStyleSheet(QStringLiteral(
        "QGroupBox { margin: 0; padding: 0; border: none; }"));
    auto* browserLayout = new QVBoxLayout(m_browserGroup);
    browserLayout->setContentsMargins(0, 0, 0, 0);
    browserLayout->setSpacing(0);

    m_browserTree = new EffectBrowserTree(m_browserGroup);
    m_browserTree->setHeaderHidden(true);
    m_browserTree->setRootIsDecorated(true);
    m_browserTree->setDragEnabled(true);
    m_browserTree->setDragDropMode(QAbstractItemView::DragOnly);
    m_browserTree->setIndentation(16);
    m_browserTree->setIconSize(QSize(16, 16));
    // Use QPalette for base colors so native branch indicators remain visible.
    {
        QPalette pal = m_browserTree->palette();
        pal.setColor(QPalette::Base,      Theme::colors().surface0);
        pal.setColor(QPalette::Text,      Theme::colors().textPrimary);
        pal.setColor(QPalette::Highlight, Theme::colors().accent);
        pal.setColor(QPalette::HighlightedText, Theme::colors().textPrimary);
        m_browserTree->setPalette(pal);
    }
    m_browserTree->setStyleSheet(QStringLiteral(
        "QTreeWidget { border: none; font-size: 12px; }"
        "QTreeWidget::item { padding: 2px 4px; height: 20px; }"
        "QTreeWidget::item:selected { background: %1; }"
        "QTreeWidget::item:hover { background: %2; }")
        .arg(Theme::hex(Theme::colors().accent))
        .arg(Theme::hex(Theme::colors().controlBgHover)));
    m_browserTree->setAnimated(true);
    m_browserTree->setObjectName("EffectBrowserTree");
    browserLayout->addWidget(m_browserTree);
    mainLayout->addWidget(m_browserGroup, 1);

    // Keep old m_browserList for test compatibility
    m_browserList = new QListWidget(this);
    m_browserList->setVisible(false);
    mainLayout->addWidget(m_browserList);

    populateBrowser();

    // Update status with total effect/transition count
    {
        int count = 0;
        std::function<void(QTreeWidgetItem*)> countLeaves = [&](QTreeWidgetItem* item) {
            if (item->childCount() == 0 && (item->data(0, Qt::UserRole).isValid()
                || item->data(0, Qt::UserRole + 1).isValid()))
                ++count;
            for (int i = 0; i < item->childCount(); ++i)
                countLeaves(item->child(i));
        };
        for (int i = 0; i < m_browserTree->topLevelItemCount(); ++i)
            countLeaves(m_browserTree->topLevelItem(i));
        if (m_statusLabel)
            m_statusLabel->setText(tr("%1 effects").arg(count));
    }

    // ── Bottom toolbar (New Bin + Delete — Premiere Pro style) ─────────
    auto* bottomBar = new QWidget(this);
    bottomBar->setFixedHeight(24);
    bottomBar->setStyleSheet(QStringLiteral("background: %1; border-top: 1px solid %2;")
        .arg(Theme::hex(Theme::colors().surface1), Theme::hex(Theme::colors().panelBorder)));
    auto* bottomLayout = new QHBoxLayout(bottomBar);
    bottomLayout->setContentsMargins(4, 0, 4, 0);
    bottomLayout->setSpacing(2);

    bottomLayout->addStretch();

    auto bottomBtnStyle = QStringLiteral(
        "QToolButton { background: transparent; color: %1; border: none; font-size: 13px; padding: 0; }"
        "QToolButton:hover { color: %2; }")
        .arg(Theme::hex(Theme::colors().textSecondary),
             Theme::hex(Theme::colors().textPrimary));

    auto* newBinBtn = new QToolButton(bottomBar);
    newBinBtn->setText(QStringLiteral("\U0001F4C1"));  // 📁
    newBinBtn->setToolTip(tr("New Custom Bin"));
    newBinBtn->setFixedSize(22, 22);
    newBinBtn->setStyleSheet(bottomBtnStyle);
    bottomLayout->addWidget(newBinBtn);

    auto* deleteBtn = new QToolButton(bottomBar);
    deleteBtn->setText(QStringLiteral("\U0001F5D1"));  // 🗑
    deleteBtn->setToolTip(tr("Delete Custom Item"));
    deleteBtn->setFixedSize(22, 22);
    deleteBtn->setStyleSheet(bottomBtnStyle);
    bottomLayout->addWidget(deleteBtn);

    // ── Connect New Custom Bin button ──────────────────────────────────
    connect(newBinBtn, &QToolButton::clicked, this, [this]() {
        bool ok = false;
        QString name = QInputDialog::getText(this, tr("New Custom Bin"),
                                             tr("Bin name:"), QLineEdit::Normal,
                                             QString(), &ok);
        if (!ok || name.trimmed().isEmpty()) return;

        auto* binItem = new QTreeWidgetItem(m_browserTree);
        binItem->setText(0, name.trimmed());
        binItem->setIcon(0, style()->standardIcon(QStyle::SP_DirIcon));
        binItem->setFlags(binItem->flags() & ~Qt::ItemIsDragEnabled);
        binItem->setData(0, Qt::UserRole + 2, true);  // mark as custom bin
        m_browserTree->scrollToItem(binItem);
        m_browserTree->setCurrentItem(binItem);
    });

    // ── Connect Delete Custom Item button ─────────────────────────────
    connect(deleteBtn, &QToolButton::clicked, this, [this]() {
        auto* current = m_browserTree->currentItem();
        if (!current) return;
        // Only allow deleting custom bins (tagged with UserRole+2)
        bool isCustom = current->data(0, Qt::UserRole + 2).toBool();
        if (!isCustom) {
            QMessageBox::information(this, tr("Delete"),
                                     tr("Only custom bins can be deleted."));
            return;
        }
        auto answer = QMessageBox::question(this, tr("Delete Custom Bin"),
            tr("Delete \"%1\" and all its contents?").arg(current->text(0)),
            QMessageBox::Yes | QMessageBox::No);
        if (answer == QMessageBox::Yes) {
            delete current;
        }
    });

    mainLayout->addWidget(bottomBar);

    // ── Add button (hidden — Premiere Pro uses drag-drop only) ─────────
    m_addButton = new QPushButton(tr("Add Effect"), this);
    m_addButton->setEnabled(false);
    m_addButton->setVisible(false);  // Hidden: drag effects onto timeline clips instead

    // ── Effect stack (hidden — shown in Effect Controls panel instead) ──
    m_stackGroup = new QGroupBox(tr("Effect Stack"), this);
    m_stackGroup->setVisible(false);  // Premiere Pro style: stack is in Effect Controls
    auto* stackLayout = new QVBoxLayout(m_stackGroup);
    m_stackList = new QListWidget(m_stackGroup);
    stackLayout->addWidget(m_stackList);

    auto* btnLayout = new QHBoxLayout();
    m_removeButton   = new QPushButton(tr("Remove"), m_stackGroup);
    m_moveUpButton   = new QPushButton(tr("Up"), m_stackGroup);
    m_moveDownButton = new QPushButton(tr("Down"), m_stackGroup);
    m_removeButton->setEnabled(false);
    m_moveUpButton->setEnabled(false);
    m_moveDownButton->setEnabled(false);
    btnLayout->addWidget(m_removeButton);
    btnLayout->addWidget(m_moveUpButton);
    btnLayout->addWidget(m_moveDownButton);
    stackLayout->addLayout(btnLayout);

    // Premiere Pro style: effect stack shown in Effect Controls, not here

    // ── Status (hidden — Premiere Pro style) ───────────────────────────
    m_statusLabel = new QLabel(this);
    m_statusLabel->setFixedHeight(22);
    m_statusLabel->setStyleSheet(QStringLiteral(
        "color: %1; font-size: 11px; padding-left: 8px; background: %2; "
        "border-top: 1px solid %3;")
        .arg(Theme::hex(Theme::colors().textSecondary),
             Theme::hex(Theme::colors().surface1),
             Theme::hex(Theme::colors().panelBorder)));
    mainLayout->addWidget(m_statusLabel);

    // ── Connections ─────────────────────────────────────────────────────
    connect(m_browserTree, &QTreeWidget::currentItemChanged, this,
            [this](QTreeWidgetItem* current, QTreeWidgetItem*) {
        bool valid = current && current->data(0, Qt::UserRole).isValid();
        m_addButton->setEnabled(valid && m_clip != nullptr);
    });
    connect(m_addButton,     &QPushButton::clicked, this, &EffectsPanel::onAddClicked);
    connect(m_removeButton,  &QPushButton::clicked, this, &EffectsPanel::onRemoveClicked);
    connect(m_moveUpButton,  &QPushButton::clicked, this, &EffectsPanel::onMoveUpClicked);
    connect(m_moveDownButton,&QPushButton::clicked, this, &EffectsPanel::onMoveDownClicked);
    connect(m_stackList, &QListWidget::currentRowChanged, this, &EffectsPanel::onStackSelectionChanged);

    // Context menu for browser tree (save/load/delete presets)
    m_browserTree->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_browserTree, &QTreeWidget::customContextMenuRequested,
            this, &EffectsPanel::onBrowserContextMenu);

    // Load saved presets from disk
    loadPresetsFromDisk();
}

void EffectsPanel::populateBrowser()
{
    m_browserTree->clear();
    m_browserList->clear();

    // Folder icon for categories (using the style's standard folder pixmap)
    QIcon folderIcon = style()->standardIcon(QStyle::SP_DirIcon);

    // Premiere Pro categories — matching the screenshot exactly
    struct Category {
        const char* name;
        std::vector<EffectType> types;
    };
    std::vector<Category> categories = {
        { "Presets",            {} },
        { "Color Presets",      {} },
        { "Audio Effects",     { EffectType::FillLeftWithRight,
                                 EffectType::FillRightWithLeft } },
        { "Audio Transitions", {} },
        { "Video Effects",     { EffectType::ColorCorrect, EffectType::Blur,
                                 EffectType::Sharpen, EffectType::Glow,
                                 EffectType::ChromaKey, EffectType::Transform2D,
                                 EffectType::Vignette, EffectType::LUT,
                                 EffectType::Letterbox, EffectType::ColorGrading,
                                 EffectType::Tint,
                                 EffectType::OtsLeft, EffectType::OtsRight,
                                 EffectType::OtsIntro,
                                 EffectType::FlipHorizontal,
                                 EffectType::FlipVertical } },
        { "Glitch",            { EffectType::Scanlines, EffectType::BlockGlitch,
                                 EffectType::ChromaticSplit, EffectType::TurbulentDisplace,
                                 EffectType::Posterize, EffectType::Grain,
                                 EffectType::SignalTear } },
        { "Glitch Presets",    {} },   // populated separately below (multi-effect macros)
        { "Beat / Music",      { EffectType::BeatZoom, EffectType::BeatFlash,
                                 EffectType::BeatShake, EffectType::BeatChroma,
                                 EffectType::BeatDrop } },
        { "Video Transitions", {} },   // populated separately below
        { "Legacy",            {} },
    };

    for (auto& cat : categories) {
        auto* catItem = new QTreeWidgetItem(m_browserTree);
        catItem->setText(0, QString::fromUtf8(cat.name));
        catItem->setIcon(0, folderIcon);
        catItem->setFlags(catItem->flags() & ~Qt::ItemIsDragEnabled);
        QFont f = catItem->font(0);
        f.setBold(false);   // Premiere Pro uses normal weight for categories
        catItem->setFont(0, f);

        for (auto type : cat.types) {
            auto* child = new QTreeWidgetItem(catItem);
            child->setText(0, QString::fromUtf8(effectTypeName(type)));
            child->setData(0, Qt::UserRole, static_cast<int>(type));
            child->setFlags(child->flags() | Qt::ItemIsDragEnabled);
        }

        // Append the audiofx DSP processors (EQ / Dynamics) to Audio Effects.
        // These are NOT EffectType values — they carry kAudioFxRole and drop
        // into the clip's FxChain rather than its EffectStack.
        if (std::string(cat.name) == "Audio Effects") {
            struct FxEntry { const char* name; audiofx::ProcessorKind kind; };
            for (const FxEntry& fx : { FxEntry{"Parametric EQ", audiofx::ProcessorKind::ParametricEQ},
                                       FxEntry{"Dynamics",      audiofx::ProcessorKind::Dynamics} }) {
                auto* child = new QTreeWidgetItem(catItem);
                child->setText(0, QString::fromUtf8(fx.name));
                child->setData(0, kAudioFxRole, static_cast<int>(fx.kind));
                child->setFlags(child->flags() | Qt::ItemIsDragEnabled);
            }
        }

        catItem->setExpanded(false);
    }

    // ── Populate "Glitch Presets" with the curated multi-effect macros ──
    {
        QTreeWidgetItem* presetTop = nullptr;
        for (int c = 0; c < m_browserTree->topLevelItemCount(); ++c) {
            if (m_browserTree->topLevelItem(c)->text(0) == "Glitch Presets") {
                presetTop = m_browserTree->topLevelItem(c);
                break;
            }
        }
        if (presetTop) {
            for (int i = 0; i < static_cast<int>(GlitchPreset::Count); ++i) {
                auto preset = static_cast<GlitchPreset>(i);
                auto* child = new QTreeWidgetItem(presetTop);
                child->setText(0, QString::fromUtf8(glitchPresetName(preset)));
                child->setData(0, kGlitchPresetRole, i);
                child->setFlags(child->flags() | Qt::ItemIsDragEnabled);
            }
        }
    }

    // ── Populate "Video Transitions" with organized sub-categories ──
    {
        // Find the Video Transitions top-level item
        QTreeWidgetItem* transTopItem = nullptr;
        for (int c = 0; c < m_browserTree->topLevelItemCount(); ++c) {
            if (m_browserTree->topLevelItem(c)->text(0) == "Video Transitions") {
                transTopItem = m_browserTree->topLevelItem(c);
                break;
            }
        }
        if (transTopItem) {
            struct TransEntry { const char* name; TransitionType type; };
            struct TransSubCat { const char* name; std::vector<TransEntry> entries; };

            std::vector<TransSubCat> subCats = {
                { "Dissolve", {
                    { "Cross Dissolve",      TransitionType::CrossDissolve },
                    { "Dip to Black",        TransitionType::DipToBlack },
                    { "Dip to White",        TransitionType::DipToWhite },
                    { "Film Dissolve",       TransitionType::FilmDissolve },
                    { "Additive Dissolve",   TransitionType::AdditiveDissolve },
                    { "Morph Cut",           TransitionType::MorphCut },
                }},
                { "Fade", {
                    { "Fade to Black",       TransitionType::FadeToBlack },
                    { "Fade from Black",     TransitionType::FadeFromBlack },
                    { "Fade to White",       TransitionType::FadeToWhite },
                    { "Fade from White",     TransitionType::FadeFromWhite },
                }},
                { "Wipe", {
                    { "Wipe Left",           TransitionType::WipeLeft },
                    { "Wipe Right",          TransitionType::WipeRight },
                    { "Wipe Up",             TransitionType::WipeUp },
                    { "Wipe Down",           TransitionType::WipeDown },
                    { "Barn Door",           TransitionType::BarnDoor },
                    { "Clock Wipe",          TransitionType::ClockWipe },
                    { "Radial Wipe",         TransitionType::RadialWipe },
                    { "Diagonal Wipe",       TransitionType::DiagonalWipe },
                    { "Checker Wipe",        TransitionType::CheckerWipe },
                    { "Venetian Blinds",     TransitionType::VenetianBlinds },
                    { "Gradient Wipe",       TransitionType::GradientWipe },
                }},
                { "Iris", {
                    { "Iris Round",          TransitionType::IrisRound },
                    { "Iris Diamond",        TransitionType::IrisDiamond },
                    { "Iris Cross",          TransitionType::IrisCross },
                    { "Inset",               TransitionType::Inset },
                }},
                { "Slide", {
                    { "Push Left",           TransitionType::PushLeft },
                    { "Push Right",          TransitionType::PushRight },
                    { "Push Up",             TransitionType::PushUp },
                    { "Push Down",           TransitionType::PushDown },
                    { "Slide Left",          TransitionType::SlideLeft },
                    { "Slide Right",         TransitionType::SlideRight },
                    { "Slide Up",            TransitionType::SlideUp },
                    { "Slide Down",          TransitionType::SlideDown },
                    { "Split",               TransitionType::Split },
                    { "Center Split",        TransitionType::CenterSplit },
                    { "Swap",                TransitionType::Swap },
                }},
                { "Zoom", {
                    { "Zoom",                TransitionType::Zoom },
                    { "Cross Zoom",          TransitionType::CrossZoom },
                    { "Whip Pan",            TransitionType::WhipPan },
                }},
                { "Stylized", {
                    { "Random Blocks",       TransitionType::RandomBlocks },
                }},
            };

            for (auto& sc : subCats) {
                auto* subItem = new QTreeWidgetItem(transTopItem);
                subItem->setText(0, QString::fromUtf8(sc.name));
                subItem->setIcon(0, folderIcon);
                subItem->setFlags(subItem->flags() & ~Qt::ItemIsDragEnabled);

                for (auto& entry : sc.entries) {
                    auto* child = new QTreeWidgetItem(subItem);
                    child->setText(0, QString::fromUtf8(entry.name));
                    child->setData(0, Qt::UserRole + 1, static_cast<int>(entry.type));
                    child->setFlags(child->flags() | Qt::ItemIsDragEnabled);
                }
            }
        }
    }

    // Also populate hidden legacy list for test compatibility
    for (int i = 0; i < static_cast<int>(EffectType::Count); ++i) {
        m_browserList->addItem(QString::fromUtf8(effectTypeName(static_cast<EffectType>(i))));
    }
}

void EffectsPanel::filterBrowser(const QString& text)
{
    for (int c = 0; c < m_browserTree->topLevelItemCount(); ++c) {
        auto* catItem = m_browserTree->topLevelItem(c);
        int visibleChildren = 0;
        for (int i = 0; i < catItem->childCount(); ++i) {
            auto* child = catItem->child(i);
            bool match = text.isEmpty() ||
                         child->text(0).contains(text, Qt::CaseInsensitive);
            child->setHidden(!match);
            if (match) ++visibleChildren;
        }
        catItem->setHidden(visibleChildren == 0);
        if (visibleChildren > 0) catItem->setExpanded(true);
    }
}

void EffectsPanel::setClip(Clip* clip, Track* track)
{
    m_clip = clip;
    m_track = clip ? track : nullptr;
    refresh();
}

bool EffectsPanel::canMutateClip() const noexcept
{
    return rt::canMutateClip(m_clip, m_track);
}

void EffectsPanel::refresh()
{
    refreshStack();
    if (canMutateClip()) {
        auto* item = m_browserTree->currentItem();
        bool valid = item && item->data(0, Qt::UserRole).isValid();
        m_addButton->setEnabled(valid);
    } else {
        m_addButton->setEnabled(false);
    }
}

void EffectsPanel::refreshStack()
{
    m_updating = true;
    m_stackList->clear();

    if (m_clip) {
        auto& stack = m_clip->effects();
        for (size_t i = 0; i < stack.effectCount(); ++i) {
            auto& fx = stack.effect(i);
            QString text = QString::fromUtf8(fx.name());
            if (!fx.isEnabled()) text += " (disabled)";
            m_stackList->addItem(text);
        }
    }

    m_updating = false;
    onStackSelectionChanged();
}

void EffectsPanel::onAddClicked()
{
    if (!canMutateClip()) return;
    auto* item = m_browserTree->currentItem();
    if (!item) return;
    auto& stack = m_clip->effects();

    // Glitch preset macro: drops a curated stack of building blocks at once.
    if (item->data(0, kGlitchPresetRole).isValid()) {
        auto preset = static_cast<GlitchPreset>(item->data(0, kGlitchPresetRole).toInt());
        if (m_commandStack) {
            if (auto cmd = makeAddGlitchPresetCommand(&stack, preset))
                m_commandStack->execute(std::move(cmd));
        } else {
            for (auto& fx : buildGlitchPreset(preset))
                stack.addEffect(std::move(fx));
        }
        refreshStack();
        emit effectAdded();
        return;
    }

    if (!item->data(0, Qt::UserRole).isValid()) return;
    auto type = static_cast<EffectType>(item->data(0, Qt::UserRole).toInt());

    if (m_commandStack) {
        m_commandStack->execute(
            std::make_unique<AddEffectCommand>(&stack, type));
    } else {
        stack.addEffect(createEffect(type));
    }

    refreshStack();
    emit effectAdded();
}

void EffectsPanel::onRemoveClicked()
{
    if (!canMutateClip()) return;
    int row = m_stackList->currentRow();
    if (row < 0) return;

    auto& stack = m_clip->effects();
    if (static_cast<size_t>(row) >= stack.effectCount()) return;

    if (m_commandStack) {
        m_commandStack->execute(
            std::make_unique<RemoveEffectCommand>(&stack, static_cast<size_t>(row)));
    } else {
        (void)stack.removeEffect(static_cast<size_t>(row));
    }

    refreshStack();
    emit effectRemoved();
}

void EffectsPanel::onMoveUpClicked()
{
    if (!canMutateClip()) return;
    int row = m_stackList->currentRow();
    if (row <= 0) return;

    auto& stack = m_clip->effects();
    size_t from = static_cast<size_t>(row);
    size_t to   = from - 1;

    if (m_commandStack) {
        m_commandStack->execute(
            std::make_unique<MoveEffectCommand>(&stack, from, to));
    } else {
        stack.moveEffect(from, to);
    }

    refreshStack();
    m_stackList->setCurrentRow(row - 1);
    emit effectMoved();
}

void EffectsPanel::onMoveDownClicked()
{
    if (!canMutateClip()) return;
    int row = m_stackList->currentRow();
    if (row < 0) return;

    auto& stack = m_clip->effects();
    size_t from = static_cast<size_t>(row);
    if (from + 1 >= stack.effectCount()) return;
    size_t to = from + 1;

    if (m_commandStack) {
        m_commandStack->execute(
            std::make_unique<MoveEffectCommand>(&stack, from, to));
    } else {
        stack.moveEffect(from, to);
    }

    refreshStack();
    m_stackList->setCurrentRow(row + 1);
    emit effectMoved();
}

void EffectsPanel::onStackSelectionChanged()
{
    if (m_updating) return;
    int row = m_stackList->currentRow();
    bool hasSelection = row >= 0 && canMutateClip();

    m_removeButton->setEnabled(hasSelection);
    m_moveUpButton->setEnabled(hasSelection && row > 0);
    m_moveDownButton->setEnabled(hasSelection && m_clip &&
        static_cast<size_t>(row) + 1 < m_clip->effects().effectCount());
}

// ═════════════════════════════════════════════════════════════════════════════
//  Effect Presets — save / load / browse
// ═════════════════════════════════════════════════════════════════════════════

void EffectsPanel::onBrowserContextMenu(const QPoint& pos)
{
    auto* item = m_browserTree->itemAt(pos);
    QMenu menu(this);

    // If right-clicking on the "Presets" folder header
    bool isPresetsFolder = item && !item->parent() &&
                           item->text(0) == "Presets";

    // If right-clicking on a preset item (child of Presets folder)
    bool isPresetItem = item && item->parent() &&
                        item->parent()->text(0) == "Presets" &&
                        item->data(0, Qt::UserRole + 1).toBool();

    if (isPresetsFolder) {
        QAction* refreshAction = menu.addAction("Refresh Presets");
        QAction* chosen = menu.exec(m_browserTree->mapToGlobal(pos));
        if (chosen == refreshAction) {
            loadPresetsFromDisk();
        }
        return;
    }

    if (isPresetItem) {
        QAction* applyAction  = menu.addAction("Apply to Clip");
        QAction* deleteAction = menu.addAction("Delete Preset");
        applyAction->setEnabled(canMutateClip());
        QAction* chosen = menu.exec(m_browserTree->mapToGlobal(pos));
        if (chosen == applyAction && canMutateClip()) {
            auto path = std::filesystem::path(
                item->data(0, Qt::UserRole + 2).toString().toStdString());
            auto effect = loadEffectPreset(path);
            if (effect) {
                auto& stack = m_clip->effects();
                if (m_commandStack) {
                    m_commandStack->execute(
                        std::make_unique<AddEffectCommand>(
                            &stack, std::move(effect), stack.effectCount()));
                } else {
                    stack.addEffect(std::move(effect));
                }
                refreshStack();
                emit effectAdded();
            }
        } else if (chosen == deleteAction) {
            auto path = std::filesystem::path(
                item->data(0, Qt::UserRole + 2).toString().toStdString());
            std::filesystem::remove(path);
            loadPresetsFromDisk();
        }
        return;
    }

    // Regular effect item — offer "Save as Preset" if a clip has this effect applied
    if (m_clip && m_clip->effects().effectCount() > 0) {
        QMenu* saveMenu = menu.addMenu("Save Effect as Preset...");
        auto& stack = m_clip->effects();
        for (size_t i = 0; i < stack.effectCount(); ++i) {
            auto& fx = stack.effect(i);
            saveMenu->addAction(QString::fromUtf8(fx.name()), this, [this, &fx]() {
                saveEffectPreset(fx);
            });
        }
        menu.exec(m_browserTree->mapToGlobal(pos));
    }
}

void EffectsPanel::loadPresetsFromDisk()
{
    // Find the "Presets" folder in the tree
    QTreeWidgetItem* presetsFolder = nullptr;
    for (int i = 0; i < m_browserTree->topLevelItemCount(); ++i) {
        if (m_browserTree->topLevelItem(i)->text(0) == "Presets") {
            presetsFolder = m_browserTree->topLevelItem(i);
            break;
        }
    }
    if (!presetsFolder) return;

    // Clear existing preset children
    while (presetsFolder->childCount() > 0)
        delete presetsFolder->takeChild(0);

    // Scan directory for .json files
    if (!std::filesystem::exists(m_presetsDir)) return;

    for (const auto& entry : std::filesystem::directory_iterator(m_presetsDir)) {
        if (entry.path().extension() != ".json") continue;

        // Read the preset name from the file
        std::ifstream ifs(entry.path());
        if (!ifs.is_open()) continue;

        std::string content((std::istreambuf_iterator<char>(ifs)),
                             std::istreambuf_iterator<char>());

        // Simple JSON parse for "name" field
        std::string presetName = pathToUtf8(entry.path().stem());
        auto namePos = content.find("\"name\"");
        if (namePos != std::string::npos) {
            auto colonPos = content.find(':', namePos);
            auto quoteStart = content.find('"', colonPos + 1);
            auto quoteEnd = content.find('"', quoteStart + 1);
            if (quoteStart != std::string::npos && quoteEnd != std::string::npos)
                presetName = content.substr(quoteStart + 1, quoteEnd - quoteStart - 1);
        }

        auto* child = new QTreeWidgetItem(presetsFolder);
        child->setText(0, QString::fromStdString(presetName));
        child->setData(0, Qt::UserRole + 1, true);  // Mark as preset item
        child->setData(0, Qt::UserRole + 2,
                       QString::fromStdString(pathToUtf8(entry.path())));
        child->setFlags(child->flags() | Qt::ItemIsDragEnabled);
    }
}

void EffectsPanel::saveEffectPreset(const Effect& fx)
{
    bool ok = false;
    QString presetName = QInputDialog::getText(
        this, "Save Effect Preset", "Preset name:",
        QLineEdit::Normal, QString::fromUtf8(fx.name()), &ok);
    if (!ok || presetName.isEmpty()) return;

    // Build JSON
    std::ostringstream o;
    o << std::fixed;
    o << "{\n";
    o << "  \"name\": \"" << presetName.toStdString() << "\",\n";
    o << "  \"effectType\": " << static_cast<int>(fx.effectType()) << ",\n";
    o << "  \"params\": [\n";
    for (size_t i = 0; i < fx.paramCount(); ++i) {
        const auto& p = fx.param(i);
        o << "    {\n";
        o << "      \"name\": \"" << p.name << "\",\n";
        o << "      \"keyframes\": [\n";
        for (size_t k = 0; k < p.track.keyframeCount(); ++k) {
            const auto& kf = p.track.keyframe(k);
            o << "        { \"time\": " << kf.time
              << ", \"value\": " << kf.value
              << ", \"interp\": " << static_cast<int>(kf.interp) << " }";
            if (k + 1 < p.track.keyframeCount()) o << ",";
            o << "\n";
        }
        o << "      ]\n";
        o << "    }";
        if (i + 1 < fx.paramCount()) o << ",";
        o << "\n";
    }
    o << "  ]\n";
    o << "}\n";

    // Sanitize filename
    std::string filename = presetName.toStdString();
    for (char& c : filename) {
        if (c == '/' || c == '\\' || c == ':' || c == '*' ||
            c == '?' || c == '"' || c == '<' || c == '>' || c == '|')
            c = '_';
    }
    auto path = m_presetsDir / (filename + ".json");

    std::ofstream ofs(path);
    if (ofs.is_open()) {
        ofs << o.str();
        ofs.close();
        loadPresetsFromDisk();
    }
}

std::unique_ptr<Effect> EffectsPanel::loadEffectPreset(const std::filesystem::path& path)
{
    std::ifstream ifs(path);
    if (!ifs.is_open()) return nullptr;

    std::string content((std::istreambuf_iterator<char>(ifs)),
                         std::istreambuf_iterator<char>());

    // Parse effectType
    auto typePos = content.find("\"effectType\"");
    if (typePos == std::string::npos) return nullptr;

    auto colonPos = content.find(':', typePos);
    int typeInt = std::stoi(content.substr(colonPos + 1));

    if (typeInt < 0 || typeInt >= static_cast<int>(EffectType::Count))
        return nullptr;

    auto effect = createEffect(static_cast<EffectType>(typeInt));
    if (!effect) return nullptr;

    // Parse params and apply keyframe values
    // Simple parser: find each param block
    size_t searchPos = 0;
    size_t paramIdx = 0;

    while (paramIdx < effect->paramCount()) {
        auto namePos = content.find("\"name\"", searchPos);
        if (namePos == std::string::npos) break;

        // Find keyframes array
        auto kfArrayStart = content.find("\"keyframes\"", namePos);
        if (kfArrayStart == std::string::npos) break;

        auto arrStart = content.find('[', kfArrayStart);
        auto arrEnd   = content.find(']', arrStart);
        if (arrStart == std::string::npos || arrEnd == std::string::npos) break;

        std::string kfBlock = content.substr(arrStart, arrEnd - arrStart + 1);

        // Clear existing keyframes and parse new ones
        auto& track = effect->param(paramIdx).track;
        while (track.keyframeCount() > 0)
            track.removeKeyframe(track.keyframeCount() - 1);

        // Parse individual keyframes: { "time": N, "value": F, "interp": I }
        size_t kfPos = 0;
        while (true) {
            auto timePos = kfBlock.find("\"time\"", kfPos);
            if (timePos == std::string::npos) break;

            auto tc = kfBlock.find(':', timePos);
            int64_t time = std::stoll(kfBlock.substr(tc + 1));

            auto valPos = kfBlock.find("\"value\"", timePos);
            auto vc = kfBlock.find(':', valPos);
            float value = std::stof(kfBlock.substr(vc + 1));

            auto interpPos = kfBlock.find("\"interp\"", valPos);
            auto ic = kfBlock.find(':', interpPos);
            int interp = std::stoi(kfBlock.substr(ic + 1));

            track.addKeyframe(time, value, static_cast<InterpMode>(interp));

            kfPos = interpPos + 10;
        }

        searchPos = arrEnd + 1;
        ++paramIdx;
    }

    return effect;
}

} // namespace rt
