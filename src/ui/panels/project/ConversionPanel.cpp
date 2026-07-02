/*
 * ConversionPanel.cpp — Spine animation → video cache conversion UI.
 *
 * Shows a table of all downloaded characters with their conversion
 * progress and allows manual triggering of pre-renders.
 */

#include "panels/project/ConversionPanel.h"
#include "PathUtils.h"
#include "QtHelpers.h"
#include "Theme.h"

#ifdef ROUNDTABLE_HAS_SPINE
#include "spine/AnimationVideoCache.h"
#include "spine/ModelManager.h"
#include "spine/SpineEngine.h"
#include "spine/SpineAnimation.h"
#endif

#include <QColorDialog>
#include <QDir>
#include <QDirIterator>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QMessageBox>
#include <QProcess>
#include <QVBoxLayout>

#include <spdlog/spdlog.h>

namespace rt {

// ═════════════════════════════════════════════════════════════════════════════
// Construction
// ═════════════════════════════════════════════════════════════════════════════

ConversionPanel::ConversionPanel(QWidget* parent)
    : QWidget(parent)
{
    setupUI();

    // Auto-refresh every 2 seconds while conversions are running
    m_refreshTimer = new QTimer(this);
    m_refreshTimer->setInterval(2000);
    connect(m_refreshTimer, &QTimer::timeout, this, &ConversionPanel::onTimerTick);
}

ConversionPanel::~ConversionPanel()
{
    m_destroying.store(true, std::memory_order_release);

    // Stop the refresh timer — prevents it firing during destruction
    if (m_refreshTimer) {
        m_refreshTimer->stop();
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// Configuration
// ═════════════════════════════════════════════════════════════════════════════

void ConversionPanel::setAnimVideoCache(AnimationVideoCache* cache)
{
    m_animVideoCache = cache;
    // Don't refreshTable() here — loading every skeleton to count
    // animations is too expensive at startup.  The table populates
    // on-demand when the user switches to the Convert tab.
}

void ConversionPanel::setModelManager(ModelManager* mgr)
{
    m_modelManager = mgr;
    // Deferred — see setAnimVideoCache comment.
}

// ═════════════════════════════════════════════════════════════════════════════
// UI Setup
// ═════════════════════════════════════════════════════════════════════════════

void ConversionPanel::setupUI()
{
    const auto& c = Theme::colors();
    const auto& m = Theme::metrics();
    const auto& t = Theme::typography();

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(m.spacingXl * 2, m.spacingXl, m.spacingXl * 2, m.spacingXl);
    layout->setSpacing(m.spacingXl);

    // ── Title ───────────────────────────────────────────────────────────
    m_titleLabel = new QLabel(QStringLiteral(
        "\xF0\x9F\x94\x84  Animation Conversion"));
    m_titleLabel->setStyleSheet(QStringLiteral(
        "font-size: 28px; font-weight: bold; color: %1; padding: 6px 0;")
        .arg(Theme::rgb(c.accent)));
    layout->addWidget(m_titleLabel);

    // ── Description ─────────────────────────────────────────────────────
    auto* desc = new QLabel(
        "Pre-render Spine animations to video for faster timeline "
        "playback. Characters with cached videos skip real-time Spine rendering.");
    desc->setWordWrap(true);
    desc->setStyleSheet(QStringLiteral(
        "font-size: %1px; color: %2; padding: 4px 0 10px 0;")
        .arg(t.sizeCaption)
        .arg(Theme::rgb(c.textSecondary)));
    layout->addWidget(desc);

    // ── Overall progress ────────────────────────────────────────────────
    auto* progressFrame = new QFrame;
    progressFrame->setStyleSheet(QStringLiteral(
        "QFrame { background: %1; border: 1px solid %2; border-radius: %3px; padding: 12px; }")
        .arg(Theme::rgb(c.surface1), Theme::rgb(c.border))
        .arg(m.radiusLg));
    auto* progressLayout = new QVBoxLayout(progressFrame);
    progressLayout->setContentsMargins(20, 16, 20, 16);
    progressLayout->setSpacing(12);

    m_overallLabel = new QLabel("Overall Progress: 0 / 0 animations cached");
    m_overallLabel->setStyleSheet(QStringLiteral(
        "font-size: %1px; font-weight: 600; color: %2; border: none; background: transparent;")
        .arg(t.sizeBody)
        .arg(Theme::rgb(c.textPrimary)));
    progressLayout->addWidget(m_overallLabel);

    m_overallProgress = new QProgressBar;
    m_overallProgress->setRange(0, 100);
    m_overallProgress->setValue(0);
    m_overallProgress->setMinimumHeight(28);
    m_overallProgress->setStyleSheet(QStringLiteral(
        "QProgressBar { border: 1px solid %1; border-radius: %6px;"
        "  background: %2; text-align: center; font-size: %7px;"
        "  font-weight: 600; color: %3; }"
        "QProgressBar::chunk { background: qlineargradient("
        "  x1:0, y1:0, x2:1, y2:0,"
        "  stop:0 %4, stop:1 %5);"
        "  border-radius: %8px; }")
        .arg(Theme::rgb(c.border), Theme::rgb(c.surface0),
             Theme::rgb(c.textPrimary),
             Theme::rgb(c.accent),
             Theme::rgb(c.accentDim))
        .arg(m.radiusLg)
        .arg(t.sizeSmall)
        .arg(m.radiusMd));
    progressLayout->addWidget(m_overallProgress);

    m_statusLabel = new QLabel("Ready");
    m_statusLabel->setStyleSheet(QStringLiteral(
        "font-size: %1px; color: %2; border: none; background: transparent;")
        .arg(t.sizeSmall)
        .arg(Theme::rgb(c.textTertiary)));
    progressLayout->addWidget(m_statusLabel);

    layout->addWidget(progressFrame);

    // ── Encoder format selector ─────────────────────────────────────────
    auto* encoderRow = new QHBoxLayout;
    encoderRow->setSpacing(m.spacingMd);

    auto* encoderLabel = new QLabel("Encoder Format:");
    encoderLabel->setStyleSheet(QStringLiteral(
        "font-size: %1px; font-weight: 600; color: %2;")
        .arg(t.sizeCaption)
        .arg(Theme::rgb(c.textPrimary)));
    encoderRow->addWidget(encoderLabel);

    m_encoderCombo = new QComboBox;
    m_encoderCombo->addItem("Green Screen  (.mp4)",   static_cast<int>(SpineCacheFormat::GreenScreen));
    m_encoderCombo->addItem("Blue Screen  (.mp4)",    static_cast<int>(SpineCacheFormat::BlueScreen));
    m_encoderCombo->addItem("Custom Color  (.mp4)",   static_cast<int>(SpineCacheFormat::CustomColor));
    m_encoderCombo->addItem("ProRes 4444  (.mov)",    static_cast<int>(SpineCacheFormat::ProRes4444));
    m_encoderCombo->setMinimumWidth(320);
    m_encoderCombo->setMinimumHeight(36);
    m_encoderCombo->setStyleSheet(QStringLiteral(
        "QComboBox { font-size: %8px; padding: 6px 12px;"
        "  background: %1; color: %2; border: 1px solid %3;"
        "  border-radius: %4px; }"
        "QComboBox::drop-down { border: none; }"
        "QComboBox QAbstractItemView { background: %5; color: %6;"
        "  selection-background-color: %7; }")
        .arg(Theme::rgb(c.surface1), Theme::rgb(c.textPrimary),
             Theme::rgb(c.border), QString::number(m.radiusMd),
             Theme::rgb(c.surface0), Theme::rgb(c.textPrimary),
             Theme::rgb(c.accentDim))
        .arg(t.sizeSmall));
    m_encoderCombo->setToolTip(
        "Green Screen: Character rendered on solid #00FF00 background. Key it out in your editing software.\n"
        "Blue Screen: Character rendered on solid #0000FF background.\n"
        "Custom Color: Choose any solid background colour for chroma keying.\n"
        "ProRes 4444: Native alpha channel, large files, perfect quality, CPU encode.");
    connect(m_encoderCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int index) {
        if (m_destroying.load(std::memory_order_acquire)) return;
#ifdef ROUNDTABLE_HAS_SPINE
        if (m_animVideoCache) {
            auto fmt = static_cast<SpineCacheFormat>(m_encoderCombo->itemData(index).toInt());
            m_animVideoCache->setEncoderFormat(fmt);
            // Show/hide colour swatch for CustomColor
            if (m_colorSwatchBtn) {
                bool isCustom = (fmt == SpineCacheFormat::CustomColor);
                m_colorSwatchBtn->setVisible(isCustom);
            }
        }
#endif
    });
    encoderRow->addWidget(m_encoderCombo);

    // ── Chroma key colour swatch / picker ─────────────────────────────
    // Shows the current background colour. Click to open a colour picker.
    // Only visible when "Custom Color" is selected in the encoder combo.
    m_colorSwatchBtn = new QPushButton;
    m_colorSwatchBtn->setFixedSize(36, 36);
    m_colorSwatchBtn->setCursor(Qt::PointingHandCursor);
    m_colorSwatchBtn->setToolTip("Click to change chroma key background colour");
    // Bright green (#00FF00) default
    m_colorSwatchBtn->setStyleSheet(QStringLiteral(
        "QPushButton { background: #00FF00; border: 2px solid %1; border-radius: 6px; }"
        "QPushButton:hover { border-color: %2; }")
        .arg(Theme::rgb(c.border), Theme::rgb(c.accent)));
    // Capture theme colours by value for use in the lambda
    const auto swatchBorder = Theme::rgb(c.border);
    const auto swatchAccent = Theme::rgb(c.accent);
    connect(m_colorSwatchBtn, &QPushButton::clicked, this, [this, swatchBorder, swatchAccent]() {
        QColor current(m_colorSwatchBtn->property("chromaColor").isValid()
            ? m_colorSwatchBtn->property("chromaColor").value<QColor>()
            : QColor(0, 255, 0));
        QColor chosen = QColorDialog::getColor(current, this,
            "Choose Chroma Key Background Colour");
        if (chosen.isValid()) {
            m_colorSwatchBtn->setProperty("chromaColor", chosen);
            m_colorSwatchBtn->setStyleSheet(QStringLiteral(
                "QPushButton { background: %1; border: 2px solid %2; border-radius: 6px; }"
                "QPushButton:hover { border-color: %3; }")
                .arg(chosen.name(), swatchBorder, swatchAccent));
#ifdef ROUNDTABLE_HAS_SPINE
            if (m_animVideoCache) {
                m_animVideoCache->setChromaKeyColor(
                    static_cast<uint8_t>(chosen.red()),
                    static_cast<uint8_t>(chosen.green()),
                    static_cast<uint8_t>(chosen.blue()));
            }
#endif
        }
    });
    // Initially hidden — shown only when CustomColor is selected
    m_colorSwatchBtn->setVisible(false);
    encoderRow->addWidget(m_colorSwatchBtn);

    encoderRow->addStretch();

    // ── Hide-converted toggle ───────────────────────────────────────────
    m_hideConvertedCheck = new QCheckBox("Hide fully converted");
    m_hideConvertedCheck->setStyleSheet(QStringLiteral(
        "QCheckBox { font-size: %1px; color: %2; spacing: %3px; }")
        .arg(t.sizeSmall)
        .arg(Theme::rgb(c.textSecondary))
        .arg(m.spacingMd));
    m_hideConvertedCheck->setToolTip(
        "Hide characters where all animations are already cached");
    connect(m_hideConvertedCheck, &QCheckBox::toggled,
            this, &ConversionPanel::refreshTable);
    encoderRow->addWidget(m_hideConvertedCheck);

    layout->addLayout(encoderRow);

    // ── Character table ─────────────────────────────────────────────────
    m_table = new QTreeWidget;
    m_table->setObjectName("ConversionTable");
    m_table->setHeaderLabels({"Character", "Outfit", "Codec", "Cached", "Total", "Status"});
    m_table->setRootIsDecorated(false);
    m_table->setAlternatingRowColors(true);
    m_table->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_table->setSortingEnabled(true);
    m_table->sortByColumn(0, Qt::AscendingOrder);

    // Column widths — use interactive resize so columns fill available space
    m_table->header()->setStretchLastSection(true);
    m_table->header()->setSectionResizeMode(QHeaderView::Interactive);
    m_table->setColumnWidth(0, 260);
    m_table->setColumnWidth(1, 170);
    m_table->setColumnWidth(2, 130);
    m_table->setColumnWidth(3, 100);
    m_table->setColumnWidth(4, 100);

    m_table->setStyleSheet(QStringLiteral(
        "QTreeWidget { background: %1; border: 1px solid %2; border-radius: %3px;"
        "  font-size: %10px; outline: none; }"
        "QTreeWidget::item { padding: 8px 12px; min-height: 36px; }"
        "QTreeWidget::item:selected { background: %4; }"
        "QTreeWidget::item:hover { background: %5; }"
        "QHeaderView::section { background: %6; color: %7;"
        "  font-size: %11px; font-weight: 600; padding: 10px 14px;"
        "  border: none; border-bottom: 2px solid %8;"
        "  border-right: 1px solid %9; }")
        .arg(Theme::rgb(c.surface0), Theme::rgb(c.border),
             QString::number(m.radiusMd),
             Theme::rgb(c.accentDim), Theme::rgb(c.surface2),
             Theme::rgb(c.surface1), Theme::rgb(c.textPrimary),
             Theme::rgb(c.accent), Theme::rgb(c.border))
        .arg(t.sizeCaption)
        .arg(t.sizeSmall));

    // Enable context menu on the table
    m_table->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_table, &QWidget::customContextMenuRequested,
            this, &ConversionPanel::onTableContextMenu);

    layout->addWidget(m_table, 1);

    // ── Action buttons ──────────────────────────────────────────────────
    auto* btnRow = new QHBoxLayout;
    btnRow->setSpacing(m.spacingMd);

    m_convertAllBtn = new QPushButton(QStringLiteral(
        "\xF0\x9F\x9A\x80  Convert All"));
    m_convertAllBtn->setObjectName("PrimaryBtn");
    m_convertAllBtn->setMinimumHeight(52);
    m_convertAllBtn->setMinimumWidth(220);
    m_convertAllBtn->setCursor(Qt::PointingHandCursor);
    m_convertAllBtn->setStyleSheet(QStringLiteral(
        "QPushButton { font-size: %7px; font-weight: 700; padding: 12px 28px;"
        "  background: %1; color: white; border-radius: %2px; border: none; }"
        "QPushButton:hover { background: %3; }"
        "QPushButton:pressed { background: %4; }"
        "QPushButton:disabled { background: %5; color: %6; }")
        .arg(Theme::rgb(c.accent))
        .arg(m.radiusLg)
        .arg(Theme::rgb(c.accentHover))
        .arg(Theme::rgb(c.accentDim))
        .arg(Theme::rgb(c.surface2))
        .arg(Theme::rgb(c.textTertiary))
        .arg(t.sizeBody));
    m_convertAllBtn->setToolTip(
        "Queue all downloaded characters for video cache conversion");
    connect(m_convertAllBtn, &QPushButton::clicked,
            this, &ConversionPanel::onConvertAllClicked);
    btnRow->addWidget(m_convertAllBtn);

    auto* convertSelectedBtn = new QPushButton(QStringLiteral(
        "\xE2\x96\xB6  Convert Selected"));
    convertSelectedBtn->setObjectName("GhostBtn");
    convertSelectedBtn->setMinimumHeight(52);
    convertSelectedBtn->setMinimumWidth(220);
    convertSelectedBtn->setCursor(Qt::PointingHandCursor);
    convertSelectedBtn->setStyleSheet(QStringLiteral(
        "QPushButton { font-size: %6px; font-weight: 600; padding: 12px 28px;"
        "  background: transparent; color: %1; border: 2px solid %2;"
        "  border-radius: %3px; }"
        "QPushButton:hover { background: %4; }"
        "QPushButton:pressed { background: %5; }")
        .arg(Theme::rgb(c.accent))
        .arg(Theme::rgb(c.accent))
        .arg(m.radiusLg)
        .arg(Theme::rgb(c.surface2))
        .arg(Theme::rgb(c.accentDim))
        .arg(t.sizeBody));
    convertSelectedBtn->setToolTip(
        "Convert only the selected characters in the table");
    connect(convertSelectedBtn, &QPushButton::clicked, this, [this]() {
        if (m_destroying.load(std::memory_order_acquire)) return;
        auto selected = m_table->selectedItems();
        if (selected.isEmpty()) return;
        // Snapshot (charName, outfit) pairs BEFORE calling onConvertCharacter,
        // because onConvertCharacter -> refreshTable() -> m_table->clear()
        // deletes every QTreeWidgetItem in `selected`, so iterating
        // `selected` after the first call dereferences freed memory.
        QVector<QPair<QString, QString>> jobs;
        jobs.reserve(selected.size());
        for (auto* item : selected) {
            QString charName = item->data(0, Qt::UserRole).toString();
            QString outfit   = item->data(0, Qt::UserRole + 1).toString();
            if (!charName.isEmpty())
                jobs.append({charName, outfit});
        }
        for (const auto& job : jobs)
            onConvertCharacter(job.first, job.second);
    });
    btnRow->addWidget(convertSelectedBtn);

    // ── Cancel button ──────────────────────────────────────
    // Cancels any ongoing conversion. Hidden by default, shown
    // when rendering is active.
    m_cancelBtn = new QPushButton(QStringLiteral(
        "\xE2\x9C\x97  Cancel"));
    m_cancelBtn->setObjectName("DangerBtn");
    m_cancelBtn->setMinimumHeight(52);
    m_cancelBtn->setMinimumWidth(160);
    m_cancelBtn->setCursor(Qt::PointingHandCursor);
    m_cancelBtn->setVisible(false);
    m_cancelBtn->setStyleSheet(QStringLiteral(
        "QPushButton { font-size: %2px; font-weight: 600; padding: 12px 28px;"
        "  background: transparent; color: %3; border: 2px solid %3;"
        "  border-radius: %1px; }"
        "QPushButton:hover { background: %4; }"
        "QPushButton:pressed { background: %5; }")
        .arg(m.radiusLg)
        .arg(t.sizeBody)
        .arg(Theme::hex(c.error))
        .arg(Theme::rgba(c.error, 46))
        .arg(Theme::rgba(c.error, 82)));
    m_cancelBtn->setToolTip("Cancel all ongoing and queued conversions");
    connect(m_cancelBtn, &QPushButton::clicked,
            this, &ConversionPanel::onCancelClicked);
    btnRow->addWidget(m_cancelBtn);

    // ── Delete Selected button ─────────────────────────────
    // Removes converted .mp4/.mov cache files for the selected
    // character/outfit pairs.  Does NOT touch assets/characters/<name>/
    // — Spine and Live2D source files remain.
    auto* deleteSelectedBtn = new QPushButton(QStringLiteral(
        "\xF0\x9F\x97\x91  Delete Converted"));
    deleteSelectedBtn->setObjectName("DangerBtn");
    deleteSelectedBtn->setMinimumHeight(52);
    deleteSelectedBtn->setMinimumWidth(220);
    deleteSelectedBtn->setCursor(Qt::PointingHandCursor);
    deleteSelectedBtn->setStyleSheet(QStringLiteral(
        "QPushButton { font-size: %2px; font-weight: 600; padding: 12px 28px;"
        "  background: transparent; color: %3; border: 2px solid %3;"
        "  border-radius: %1px; }"
        "QPushButton:hover { background: %4; }"
        "QPushButton:pressed { background: %5; }")
        .arg(m.radiusLg)
        .arg(t.sizeBody)
        .arg(Theme::hex(c.error))
        .arg(Theme::rgba(c.error, 46))
        .arg(Theme::rgba(c.error, 82)));
    deleteSelectedBtn->setToolTip(
        "Delete the converted video cache for the selected character outfits.\n"
        "Source Spine / Live2D assets are NOT touched.");
    connect(deleteSelectedBtn, &QPushButton::clicked, this, [this]() {
        if (m_destroying.load(std::memory_order_acquire)) return;
#ifdef ROUNDTABLE_HAS_SPINE
        if (!m_animVideoCache) return;
        auto selected = m_table->selectedItems();
        if (selected.isEmpty()) return;

        // Snapshot pairs (refreshTable will clear the QTreeWidget items).
        QVector<QPair<QString, QString>> jobs;
        jobs.reserve(selected.size());
        for (auto* item : selected) {
            QString charName = item->data(0, Qt::UserRole).toString();
            QString outfit   = item->data(0, Qt::UserRole + 1).toString();
            if (!charName.isEmpty())
                jobs.append({charName, outfit});
        }
        if (jobs.isEmpty()) return;

        QString summary;
        for (int i = 0; i < jobs.size() && i < 8; ++i) {
            summary += QString("  \u2022 %1 / %2\n")
                           .arg(jobs[i].first, jobs[i].second);
        }
        if (jobs.size() > 8)
            summary += QString("  \u2026 and %1 more\n").arg(jobs.size() - 8);

        auto reply = QMessageBox::question(
            this,
            tr("Delete Converted Animations"),
            tr("Delete the cached converted video files for:\n\n%1\n"
               "This removes the .mp4 / .mov cache only \u2014 the original "
               "Spine and Live2D assets are NOT deleted.\n\nProceed?")
                .arg(summary),
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No);
        if (reply != QMessageBox::Yes) return;

        for (const auto& job : jobs) {
            m_animVideoCache->removeAllForCharacterOutfit(
                job.first.toStdString(), job.second.toStdString());
        }
        refreshTable();
#endif
    });
    btnRow->addWidget(deleteSelectedBtn);

    btnRow->addStretch();

    m_refreshBtn = new QPushButton(QStringLiteral(
        "\xF0\x9F\x94\x84  Refresh"));
    m_refreshBtn->setObjectName("GhostBtn");
    m_refreshBtn->setMinimumHeight(52);
    m_refreshBtn->setCursor(Qt::PointingHandCursor);
    m_refreshBtn->setStyleSheet(QStringLiteral(
        "QPushButton { font-size: %1px; font-weight: 600; padding: 12px 24px; }")
        .arg(t.sizeCaption));
    m_refreshBtn->setToolTip("Refresh conversion status");
    connect(m_refreshBtn, &QPushButton::clicked,
            this, &ConversionPanel::onRefreshClicked);
    btnRow->addWidget(m_refreshBtn);

    layout->addLayout(btnRow);
}

// ═════════════════════════════════════════════════════════════════════════════
// Table population
// ═════════════════════════════════════════════════════════════════════════════

void ConversionPanel::refreshTable()
{
    m_table->clear();

#ifdef ROUNDTABLE_HAS_SPINE
    spdlog::info("ConversionPanel::refreshTable — mgr={}, scanned={}, cache={}",
                 static_cast<const void*>(m_modelManager),
                 m_modelManager ? m_modelManager->isScanned() : false,
                 static_cast<const void*>(m_animVideoCache));
    if (!m_modelManager) {
        m_statusLabel->setText("Model manager not available (Spine support may be disabled).");
        m_convertAllBtn->setEnabled(false);
        return;
    }

    if (!m_modelManager->isScanned()) {
        m_statusLabel->setText("Scanning characters... Please wait and refresh.");
        m_convertAllBtn->setEnabled(false);
        return;
    }

    if (m_modelManager->entries().empty()) {
        m_statusLabel->setText("No characters found. Download characters first.");
        m_convertAllBtn->setEnabled(false);
        return;
    }

    m_convertAllBtn->setEnabled(true);

    const auto& c = Theme::colors();

    size_t totalCached = 0;
    size_t totalAnims  = 0;

    for (const auto& entry : m_modelManager->entries()) {
        // Only show downloaded characters (those that have outfits)
        if (entry.outfits.empty()) continue;

        for (const auto& outfit : entry.outfits) {
            // Count total animations by loading skeleton
            int animCount = 0;
            {
                SpineEngine engine;
                auto paths = SpineEngine::resolvePaths(
                    m_modelManager->assetsDir(), entry.name,
                    outfit.name, CharacterStance::Default);
                if (paths.valid && engine.loadSkeleton(paths.skelPath, paths.atlasPath)) {
                    auto anims = engine.animation().listAnimations();
                    // Check if this skeleton has a talk animation
                    bool hasTalk = !engine.animation().detectTalkAnimation().empty();
                    for (const auto& a : anims) {
                        // Match the queueAllAnimations filter
                        if (a.duration <= 0.0f) continue;
                        if (a.name == "talk_start") continue;
                        ++animCount;
                    }
                    // Each animation produces two cache entries when talk
                    // is available: normal + _talk variant
                    if (hasTalk) animCount *= 2;
                }
            }

            // Count cached animations for this specific outfit
            size_t cachedCount = 0;
            if (m_animVideoCache) {
                cachedCount = m_animVideoCache->countForCharacterOutfit(
                    entry.name, outfit.name);
            }

            totalAnims  += static_cast<size_t>(animCount);
            totalCached += cachedCount;

            // Hide fully converted entries when toggle is checked
            if (m_hideConvertedCheck && m_hideConvertedCheck->isChecked()
                && animCount > 0
                && static_cast<int>(cachedCount) >= animCount)
                continue;

            // Create table item
            auto* item = new QTreeWidgetItem;
            item->setText(0, QString::fromStdString(entry.displayName));
            item->setText(1, QString::fromStdString(outfit.name));

            // Codec column — determined from first cached file's extension
            if (m_animVideoCache && cachedCount > 0) {
                auto codec = m_animVideoCache->codecForCharacterOutfit(
                    entry.name, outfit.name);
                item->setText(2, QString::fromStdString(codec));
            } else {
                item->setText(2, QStringLiteral("\xE2\x80\x94")); // em dash
            }
            item->setTextAlignment(2, Qt::AlignCenter);

            item->setText(3, QString::number(cachedCount));
            item->setText(4, QString::number(animCount));

            // Store character/outfit in data roles for Convert Selected
            item->setData(0, Qt::UserRole, QString::fromStdString(entry.name));
            item->setData(0, Qt::UserRole + 1, QString::fromStdString(outfit.name));

            // Center-align numeric columns
            item->setTextAlignment(3, Qt::AlignCenter);
            item->setTextAlignment(4, Qt::AlignCenter);
            item->setTextAlignment(5, Qt::AlignCenter);

            // Status text + coloring
            if (animCount == 0) {
                item->setText(5, "N/A");
                item->setForeground(5, c.textTertiary);
            } else if (static_cast<int>(cachedCount) >= animCount) {
                item->setText(5, QStringLiteral("\xE2\x9C\x85  Complete"));
                item->setForeground(5, QColor(100, 220, 100));
                // Green tint for entire row
                for (int col = 0; col < 6; ++col)
                    item->setBackground(col, QColor(40, 120, 60, 35));
            } else if (cachedCount > 0) {
                int pct = static_cast<int>(cachedCount * 100 / animCount);
                item->setText(5, QStringLiteral("\xE2\x8F\xB3  %1%").arg(pct));
                item->setForeground(5, QColor(220, 180, 60));
                // Yellow tint
                for (int col = 0; col < 6; ++col)
                    item->setBackground(col, QColor(120, 100, 30, 25));
            } else {
                // Check if this specific character/outfit has pending jobs
                bool queued = false;
                if (m_animVideoCache)
                    queued = m_animVideoCache->hasPendingForCharacterOutfit(
                        entry.name, outfit.name);
                if (queued) {
                    item->setText(5, QStringLiteral("\xF0\x9F\x94\x84  Queued"));
                    item->setForeground(5, QColor(100, 160, 255));
                } else {
                    item->setText(5, QStringLiteral("\xE2\x9D\x8C  None"));
                    item->setForeground(5, QColor(220, 80, 80));
                }
            }

            m_table->addTopLevelItem(item);
        }
    }

    spdlog::info("ConversionPanel::refreshTable — entries={}, cached={}, total={}",
                 m_modelManager->entries().size(), totalCached, totalAnims);

    // Update progress bar — show batch progress only while rendering
    if (m_animVideoCache && m_animVideoCache->isRendering() && m_batchTotal > 0) {
        size_t pending = m_animVideoCache->pendingCount();
        size_t done    = m_batchTotal - std::min(pending, m_batchTotal);
        m_overallLabel->setText(QString("Converting: %1 / %2 jobs done")
                                   .arg(done).arg(m_batchTotal));
        m_overallProgress->setMaximum(static_cast<int>(m_batchTotal));
        m_overallProgress->setValue(static_cast<int>(done));
    } else {
        m_overallLabel->setText(QString("%1 / %2 animations cached")
                                   .arg(totalCached).arg(totalAnims));
        m_overallProgress->setMaximum(100);
        m_overallProgress->setValue(0);
    }

    // Status message
    bool rendering = m_animVideoCache && m_animVideoCache->isRendering();
    m_cancelBtn->setVisible(rendering);
    m_convertAllBtn->setEnabled(!rendering);

    if (rendering) {
        size_t pending = m_animVideoCache->pendingCount();
        std::string currentJob = m_animVideoCache->currentJobDescription();
        QString statusMsg;
        if (!currentJob.empty()) {
            statusMsg = QStringLiteral(
                "\xF0\x9F\x94\x84  Rendering: %1  (%2 remaining in queue)")
                .arg(QString::fromStdString(currentJob))
                .arg(pending);
        } else {
            statusMsg = QStringLiteral(
                "\xF0\x9F\x94\x84  Converting... %1 animations queued")
                .arg(pending);
        }
        m_statusLabel->setText(statusMsg);
        if (!m_refreshTimer->isActive())
            m_refreshTimer->start();
    } else if (totalCached == totalAnims && totalAnims > 0) {
        m_statusLabel->setText(QStringLiteral(
            "\xE2\x9C\x85  All animations converted!"));
        m_refreshTimer->stop();
    } else {
        m_statusLabel->setText(QStringLiteral(
            "Ready \xE2\x80\x94 click Convert All to start pre-rendering"));
        m_refreshTimer->stop();
    }

#else
    m_statusLabel->setText("Spine support not available in this build.");
    m_cancelBtn->setVisible(false);
#endif
}

void ConversionPanel::updateOverallProgress()
{
    // Quick update without full table rebuild
#ifdef ROUNDTABLE_HAS_SPINE
    if (!m_animVideoCache) return;

    size_t totalCached = 0;
    size_t totalAnims  = 0;

    for (int i = 0; i < m_table->topLevelItemCount(); ++i) {
        auto* item = m_table->topLevelItem(i);
        QString charName = item->data(0, Qt::UserRole).toString();
        QString outfitName = item->data(0, Qt::UserRole + 1).toString();
        size_t cached = m_animVideoCache->countForCharacterOutfit(
            charName.toStdString(), outfitName.toStdString());
        int total = item->text(4).toInt();

        item->setText(3, QString::number(cached));
        totalCached += cached;
        totalAnims  += static_cast<size_t>(total);
    }

    // Update progress bar — batch progress while rendering
    if (m_animVideoCache->isRendering() && m_batchTotal > 0) {
        size_t pending = m_animVideoCache->pendingCount();
        size_t done    = m_batchTotal - std::min(pending, m_batchTotal);
        m_overallLabel->setText(QString("Converting: %1 / %2 jobs done")
                                   .arg(done).arg(m_batchTotal));
        m_overallProgress->setMaximum(static_cast<int>(m_batchTotal));
        m_overallProgress->setValue(static_cast<int>(done));
    } else {
        m_overallLabel->setText(QString("%1 / %2 animations cached")
                                   .arg(totalCached).arg(totalAnims));
        m_overallProgress->setMaximum(100);
        m_overallProgress->setValue(0);
    }

    bool rendering = m_animVideoCache->isRendering();
    m_cancelBtn->setVisible(rendering);
    m_convertAllBtn->setEnabled(!rendering);

    if (rendering) {
        size_t pending = m_animVideoCache->pendingCount();
        std::string currentJob = m_animVideoCache->currentJobDescription();
        QString statusMsg;
        if (!currentJob.empty()) {
            statusMsg = QStringLiteral(
                "\xF0\x9F\x94\x84  Rendering: %1  (%2 remaining)")
                .arg(QString::fromStdString(currentJob))
                .arg(pending);
        } else {
            statusMsg = QStringLiteral(
                "\xF0\x9F\x94\x84  Processing queue... %1 remaining")
                .arg(pending);
        }
        m_statusLabel->setText(statusMsg);
    } else {
        m_batchTotal = 0; // Reset batch tracking
        if (totalCached == totalAnims && totalAnims > 0)
            m_statusLabel->setText(QStringLiteral("\xE2\x9C\x85  All animations converted!"));
        else
            m_statusLabel->setText("Ready");
        m_refreshTimer->stop();
        // Do a full refresh to update status column colors
        refreshTable();
        emit conversionsFinished();
    }
#endif
}

// ═════════════════════════════════════════════════════════════════════════════
// Actions
// ═════════════════════════════════════════════════════════════════════════════

void ConversionPanel::onConvertAllClicked()
{
#ifdef ROUNDTABLE_HAS_SPINE
    if (!m_animVideoCache || !m_modelManager) return;

    spdlog::info("ConversionPanel: converting all downloaded characters");

    for (const auto& entry : m_modelManager->entries()) {
        if (entry.outfits.empty()) continue;
        for (const auto& outfit : entry.outfits) {
            m_animVideoCache->queueAllAnimations(entry.name, outfit.name);
        }
    }

    // Capture batch total for progress bar
    m_batchTotal = m_animVideoCache->pendingCount();

    // Start periodic refresh
    if (!m_refreshTimer->isActive())
        m_refreshTimer->start();

    refreshTable();
#endif
}

void ConversionPanel::onRefreshClicked()
{
    refreshTable();
}

void ConversionPanel::onConvertCharacter(const QString& charName, const QString& outfit)
{
#ifdef ROUNDTABLE_HAS_SPINE
    if (!m_animVideoCache) return;

    spdlog::info("ConversionPanel: converting '{}' / '{}'",
                 charName.toStdString(), outfit.toStdString());

    m_animVideoCache->queueAllAnimations(charName.toStdString(),
                                          outfit.toStdString());

    // Capture batch total for progress bar
    m_batchTotal = m_animVideoCache->pendingCount();

    // Start periodic refresh
    if (!m_refreshTimer->isActive())
        m_refreshTimer->start();

    refreshTable();
#endif
}

void ConversionPanel::onTimerTick()
{
    updateOverallProgress();
}

void ConversionPanel::onCancelClicked()
{
#ifdef ROUNDTABLE_HAS_SPINE
    if (!m_animVideoCache) return;

    spdlog::info("ConversionPanel: cancel requested by user");
    m_animVideoCache->cancelAll();
    m_batchTotal = 0;

    m_cancelBtn->setVisible(false);
    m_convertAllBtn->setEnabled(true);
    m_refreshBtn->setEnabled(true);

    m_statusLabel->setText(QStringLiteral("\xE2\x9C\x97  Conversion cancelled"));
    m_overallProgress->setValue(0);

    m_refreshTimer->stop();
    refreshTable();
#endif
}

void ConversionPanel::onTableContextMenu(const QPoint& pos)
{
    auto* item = m_table->itemAt(pos);
    if (!item) return;

    QString charName = item->data(0, Qt::UserRole).toString();
    QString outfit   = item->data(0, Qt::UserRole + 1).toString();
    if (charName.isEmpty() || outfit.isEmpty()) return;

    QMenu menu(this);

    // "Reveal converted files in Explorer" action
    menu.addAction(QStringLiteral("\xF0\x9F\x93\x82  Reveal Converted in Explorer"), this,
        [this, charName, outfit]() {
            if (m_destroying.load(std::memory_order_acquire)) return;
#ifdef ROUNDTABLE_HAS_SPINE
            if (!m_animVideoCache) return;

            // Cache layout: <cacheRoot>/<formatDir>/<charName>/<outfit>/*.{mp4,mov,webm}
            // A given character+outfit can have files under any of the four
            // format subdirs (H264_Green/H264_Blue/H264_Custom/ProRes); prefer
            // the active encoder format, then fall back to whichever exists.
            const QString cacheRoot = QString::fromStdString(
                pathToUtf8(m_animVideoCache->cacheDirectory()));

            auto outfitDirFor = [&](SpineCacheFormat fmt) {
                return cacheRoot + "/"
                    + QString::fromStdString(AnimationVideoCache::formatDirName(fmt))
                    + "/" + charName + "/" + outfit;
            };

            const SpineCacheFormat preferred = m_animVideoCache->encoderFormat();
            QStringList candidates;
            candidates << outfitDirFor(preferred);
            for (auto fmt : {SpineCacheFormat::GreenScreen,
                             SpineCacheFormat::BlueScreen,
                             SpineCacheFormat::CustomColor,
                             SpineCacheFormat::ProRes4444}) {
                if (fmt != preferred)
                    candidates << outfitDirFor(fmt);
            }

            for (const QString& dirPath : candidates) {
                QDir d(dirPath);
                if (!d.exists()) continue;
                QDirIterator it(dirPath,
                    QStringList() << "*.mp4" << "*.mov" << "*.webm",
                    QDir::Files, QDirIterator::Subdirectories);
                QString selectPath = it.hasNext()
                    ? QDir::toNativeSeparators(it.next())
                    : QDir::toNativeSeparators(dirPath);
                QProcess::startDetached("explorer.exe",
                    {"/select,", selectPath});
                return;
            }

            // Nothing converted yet for this char/outfit — open the cache
            // root so the user can see the converted folder, not the Spine
            // source folder (which would be misleading).
            if (QDir(cacheRoot).exists()) {
                QProcess::startDetached("explorer.exe",
                    {QDir::toNativeSeparators(cacheRoot)});
            }
#endif
        });

    menu.exec(m_table->viewport()->mapToGlobal(pos));
}

} // namespace rt
