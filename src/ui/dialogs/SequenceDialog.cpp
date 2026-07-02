/*
 * SequenceDialog.cpp — Step-card sequence creation dialog.
 * Matches Projects→NEW tab: AR → Resolution → FPS with inline custom fields.
 */

#include "SequenceDialog.h"
#include "Theme.h"

#include <QButtonGroup>
#include <QDoubleSpinBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

#include <cmath>
#include <numeric>

namespace rt {

SequenceDialog::SequenceDialog(QWidget* parent)
    : QDialog(parent)
{
    const auto& c = Theme::colors();
    const auto& m = Theme::metrics();
    const auto& t = Theme::typography();

    setWindowTitle(tr("New Sequence"));
    setMinimumWidth(540);
    setModal(true);

    setStyleSheet(QStringLiteral("QDialog { background: %1; }")
        .arg(Theme::rgb(c.surface0)));

    auto cardStyle = [&]() -> QString {
        return QStringLiteral(
            "background: %1; border: 1px solid %2; padding: 10px 12px;")
            .arg(Theme::rgb(c.surface1))
            .arg(Theme::rgb(c.border));
    };
    auto stepHeaderStyle = [&]() -> QString {
        return QStringLiteral(
            "font-size: %2px; font-weight: 700; color: %1;"
            " letter-spacing: 0.6px; text-transform: uppercase;")
            .arg(Theme::rgb(c.textPrimary))
            .arg(t.sizeXs);
    };
    auto chipBtnStyle = [&]() -> QString {
        return QStringLiteral(
            "QPushButton { background: %1; border: 1px solid %2;"
            "  color: %3; font-size: %9px; font-weight: 600;"
            "  padding: 8px 8px; min-height: 36px; }"
            "QPushButton:hover { background: %4; border-color: %5; }"
            "QPushButton:checked { background: %6; border-color: %7;"
            "  color: %8; }")
            .arg(Theme::rgb(c.surface0))
            .arg(Theme::rgb(c.border))
            .arg(Theme::rgb(c.textPrimary))
            .arg(Theme::rgb(c.surface2))
            .arg(Theme::rgb(c.borderLight))
            .arg(Theme::rgb(c.accentDim))
            .arg(Theme::rgb(c.accent))
            .arg(Theme::rgb(c.accent))
            .arg(t.sizeXs);
    };
    auto inputStyle = [&]() -> QString {
        return QStringLiteral(
            "background: %1; border: 1px solid %2;"
            " color: %3; font-size: 13px; padding: 8px 10px;")
            .arg(Theme::rgb(c.surface0))
            .arg(Theme::rgb(c.border))
            .arg(Theme::rgb(c.textPrimary));
    };
    auto smallInputStyle = [&]() -> QString {
        return QStringLiteral(
            "background: %1; border: 1px solid %2;"
            " color: %3; font-size: 13px; padding: 6px 8px;"
            " min-width: 50px; max-width: 60px; text-align: center;")
            .arg(Theme::rgb(c.surface0))
            .arg(Theme::rgb(c.border))
            .arg(Theme::rgb(c.textPrimary));
    };
    auto dashedBtnStyle = [&]() -> QString {
        return QStringLiteral(
            "QPushButton { background: %1; border: 1px dashed %2;"
            "  color: %3; font-size: %9px; font-weight: 600;"
            "  padding: 8px 8px; min-height: 36px; }"
            "QPushButton:hover { background: %4; border-color: %5; }"
            "QPushButton:checked { background: %6; border-color: %7;"
            "  color: %8; }")
            .arg(Theme::rgb(c.surface0))
            .arg(Theme::rgb(c.border))
            .arg(Theme::rgb(c.textPrimary))
            .arg(Theme::rgb(c.surface2))
            .arg(Theme::rgb(c.borderLight))
            .arg(Theme::rgb(c.accentDim))
            .arg(Theme::rgb(c.accent))
            .arg(Theme::rgb(c.accent))
            .arg(t.sizeXs);
    };

    auto* rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(m.spacingXl, m.spacingXl,
                                   m.spacingXl, m.spacingXl);
    rootLayout->setSpacing(8);

    // ── Header ───────────────────────────────────────────────────────────
    {
        auto* hdr = new QLabel(tr("New Sequence"));
        hdr->setStyleSheet(QStringLiteral(
            "font-size: %3px; font-weight: %1; color: %2;")
            .arg(t.weightBold)
            .arg(Theme::rgb(c.textPrimary))
            .arg(t.sizeBody));
        rootLayout->addWidget(hdr);
    }

    // ── Step 1: Sequence Name ───────────────────────────────────────────
    {
        auto* card = new QWidget;
        card->setStyleSheet(cardStyle());
        auto* lay = new QVBoxLayout(card);
        lay->setContentsMargins(14, 12, 14, 12);
        lay->setSpacing(8);

        auto* sl = new QLabel(tr("SEQUENCE NAME"));
        sl->setStyleSheet(stepHeaderStyle());
        lay->addWidget(sl);

        m_nameEdit = new QLineEdit;
        m_nameEdit->setPlaceholderText(tr("Sequence 1"));
        m_nameEdit->setText(tr("Sequence 1"));
        m_nameEdit->setStyleSheet(inputStyle());
        lay->addWidget(m_nameEdit);

        rootLayout->addWidget(card);
    }

    // ── Step 2: Aspect Ratio ────────────────────────────────────────────
    {
        auto* card = new QWidget;
        card->setStyleSheet(cardStyle());
        auto* lay = new QVBoxLayout(card);
        lay->setContentsMargins(14, 12, 14, 12);
        lay->setSpacing(8);

        auto* sl = new QLabel(tr("ASPECT RATIO"));
        sl->setStyleSheet(stepHeaderStyle());
        lay->addWidget(sl);

        m_arGroup = new QButtonGroup(this);
        m_arGroup->setExclusive(true);

        auto* arGrid = new QWidget;
        auto* arLay = new QGridLayout(arGrid);
        arLay->setContentsMargins(0, 0, 0, 0);
        arLay->setSpacing(3);

        struct { const char* label; int id; bool def; } arBtns[] = {
            {"16:9", 0, true},
            {"9:16", 1, false},
            {"21:9", 2, false},
            {"Custom", 3, false},
        };
        for (int i = 0; i < 4; ++i) {
            auto* btn = new QPushButton(arBtns[i].label);
            btn->setCheckable(true);
            btn->setChecked(arBtns[i].def);
            btn->setCursor(Qt::PointingHandCursor);
            btn->setStyleSheet(i == 3 ? dashedBtnStyle() : chipBtnStyle());
            m_arGroup->addButton(btn, arBtns[i].id);
            arLay->addWidget(btn, 0, i);
        }

        connect(m_arGroup, &QButtonGroup::idClicked,
                this, &SequenceDialog::onArClicked);

        lay->addWidget(arGrid);

        // Custom AR row (hidden by default)
        m_customArRow = new QWidget;
        m_customArRow->setVisible(false);
        auto* caLay = new QHBoxLayout(m_customArRow);
        caLay->setContentsMargins(0, 0, 0, 0);
        caLay->setSpacing(6);

        auto* caWLbl = new QLabel(QStringLiteral("W"));
        caWLbl->setStyleSheet(QStringLiteral("font-size: 10px; color: %1; font-weight: 600;")
            .arg(Theme::rgb(c.textPrimary)));
        caLay->addWidget(caWLbl);

        m_customArW = new QSpinBox;
        m_customArW->setRange(1, 999);
        m_customArW->setValue(16);
        m_customArW->setStyleSheet(smallInputStyle());
        caLay->addWidget(m_customArW);

        auto* caSep = new QLabel(QStringLiteral(":"));
        caSep->setStyleSheet(QStringLiteral("color: %1; font-weight: 700;")
            .arg(Theme::rgb(c.textPrimary)));
        caLay->addWidget(caSep);

        auto* caHLbl = new QLabel(QStringLiteral("H"));
        caHLbl->setStyleSheet(QStringLiteral("font-size: 10px; color: %1; font-weight: 600;")
            .arg(Theme::rgb(c.textPrimary)));
        caLay->addWidget(caHLbl);

        m_customArH = new QSpinBox;
        m_customArH->setRange(1, 999);
        m_customArH->setValue(9);
        m_customArH->setStyleSheet(smallInputStyle());
        caLay->addWidget(m_customArH);

        caLay->addStretch();
        lay->addWidget(m_customArRow);

        connect(m_customArW, QOverload<int>::of(&QSpinBox::valueChanged),
                this, [this](int) {
            if (m_arGroup->checkedId() == 3) {
                rebuildResGrid();
                updatePreview();
            }
        });
        connect(m_customArH, QOverload<int>::of(&QSpinBox::valueChanged),
                this, [this](int) {
            if (m_arGroup->checkedId() == 3) {
                rebuildResGrid();
                updatePreview();
            }
        });

        rootLayout->addWidget(card);
    }

    // ── Step 3: Resolution ──────────────────────────────────────────────
    {
        auto* card = new QWidget;
        card->setStyleSheet(cardStyle());
        auto* lay = new QVBoxLayout(card);
        lay->setContentsMargins(14, 12, 14, 12);
        lay->setSpacing(8);

        auto* sl = new QLabel(tr("RESOLUTION"));
        sl->setStyleSheet(stepHeaderStyle());
        lay->addWidget(sl);

        m_resGroup = new QButtonGroup(this);
        m_resGroup->setExclusive(true);

        // Container widget for the dynamic resolution grid
        m_resGridWidget = new QWidget;
        auto* resOuterLay = new QVBoxLayout(m_resGridWidget);
        resOuterLay->setContentsMargins(0, 0, 0, 0);
        resOuterLay->setSpacing(0);
        lay->addWidget(m_resGridWidget);

        // Custom resolution row (hidden by default)
        m_customResRow = new QWidget;
        m_customResRow->setVisible(false);
        auto* crLay = new QHBoxLayout(m_customResRow);
        crLay->setContentsMargins(0, 0, 0, 0);
        crLay->setSpacing(6);

        auto* wLbl = new QLabel(QStringLiteral("W"));
        wLbl->setStyleSheet(QStringLiteral("font-size: 10px; color: %1; font-weight: 600;")
            .arg(Theme::rgb(c.textPrimary)));
        crLay->addWidget(wLbl);

        m_widthSpin = new QSpinBox;
        m_widthSpin->setRange(64, 7680);
        m_widthSpin->setValue(1920);
        m_widthSpin->setStyleSheet(smallInputStyle());
        crLay->addWidget(m_widthSpin);

        auto* sep = new QLabel(QStringLiteral("\u00D7"));
        sep->setStyleSheet(QStringLiteral("color: %1; font-weight: 700;")
            .arg(Theme::rgb(c.textPrimary)));
        crLay->addWidget(sep);

        auto* hLbl = new QLabel(QStringLiteral("H"));
        hLbl->setStyleSheet(QStringLiteral("font-size: 10px; color: %1; font-weight: 600;")
            .arg(Theme::rgb(c.textPrimary)));
        crLay->addWidget(hLbl);

        m_heightSpin = new QSpinBox;
        m_heightSpin->setRange(36, 4320);
        m_heightSpin->setValue(1080);
        m_heightSpin->setStyleSheet(smallInputStyle());
        crLay->addWidget(m_heightSpin);

        crLay->addStretch();
        lay->addWidget(m_customResRow);

        connect(m_widthSpin, QOverload<int>::of(&QSpinBox::valueChanged),
                this, [this](int) { updatePreview(); });
        connect(m_heightSpin, QOverload<int>::of(&QSpinBox::valueChanged),
                this, [this](int) { updatePreview(); });

        rootLayout->addWidget(card);
    }

    // ── Step 4: Frame Rate ──────────────────────────────────────────────
    {
        auto* card = new QWidget;
        card->setStyleSheet(cardStyle());
        auto* lay = new QVBoxLayout(card);
        lay->setContentsMargins(14, 12, 14, 12);
        lay->setSpacing(8);

        auto* sl = new QLabel(tr("FRAME RATE"));
        sl->setStyleSheet(stepHeaderStyle());
        lay->addWidget(sl);

        m_fpsGroup = new QButtonGroup(this);
        m_fpsGroup->setExclusive(true);

        auto* fpsGrid = new QWidget;
        auto* fpsLay = new QGridLayout(fpsGrid);
        fpsLay->setContentsMargins(0, 0, 0, 0);
        fpsLay->setSpacing(3);

        struct { const char* label; double fps; } fpsPresets[] = {
            {"24", 24.0},
            {"30", 30.0},
            {"60", 60.0},
            {"Custom", 0.0},
        };
        for (int i = 0; i < 4; ++i) {
            auto* btn = new QPushButton(fpsPresets[i].label);
            btn->setCheckable(true);
            btn->setCursor(Qt::PointingHandCursor);
            btn->setStyleSheet(i == 3 ? dashedBtnStyle() : chipBtnStyle());
            m_fpsGroup->addButton(btn, i);
            fpsLay->addWidget(btn, 0, i);
        }
        if (auto* btn = m_fpsGroup->button(1))
            btn->setChecked(true);

        connect(m_fpsGroup, &QButtonGroup::idClicked,
                this, &SequenceDialog::onFpsClicked);

        lay->addWidget(fpsGrid);

        // Custom FPS row (hidden by default)
        m_customFpsRow = new QWidget;
        m_customFpsRow->setVisible(false);
        auto* cfLay = new QHBoxLayout(m_customFpsRow);
        cfLay->setContentsMargins(0, 0, 0, 0);
        cfLay->setSpacing(6);

        auto* cfLbl = new QLabel(QStringLiteral("FPS"));
        cfLbl->setStyleSheet(QStringLiteral("font-size: 10px; color: %1; font-weight: 600;")
            .arg(Theme::rgb(c.textPrimary)));
        cfLay->addWidget(cfLbl);

        m_fpsSpin = new QDoubleSpinBox;
        m_fpsSpin->setRange(1.0, 240.0);
        m_fpsSpin->setValue(30.0);
        m_fpsSpin->setDecimals(2);
        m_fpsSpin->setStyleSheet(smallInputStyle());
        cfLay->addWidget(m_fpsSpin);

        cfLay->addStretch();
        lay->addWidget(m_customFpsRow);

        rootLayout->addWidget(card);
    }

    // ── Summary preview ─────────────────────────────────────────────────
    {
        m_previewLabel = new QLabel(
            QStringLiteral("1920\u00D71080  |  30 fps"));
        m_previewLabel->setAlignment(Qt::AlignCenter);
        m_previewLabel->setStyleSheet(QStringLiteral(
            "font-size: 13px; font-weight: 600; color: %1; padding: 6px;")
            .arg(Theme::rgb(c.accent)));
        rootLayout->addWidget(m_previewLabel);
    }

    // ── Action buttons ──────────────────────────────────────────────────
    {
        auto* btnRow = new QHBoxLayout;
        btnRow->addStretch();

        QString actionBtnStyle = QStringLiteral(
            "QPushButton { background: %1; border: 1px solid %2;"
            "  color: %3; font-size: %7px; font-weight: 700;"
            "  padding: 8px 22px; min-width: 80px; }"
            "QPushButton:hover { background: %4; border-color: %5; color: %6; }")
            .arg(Theme::rgb(c.surface1))
            .arg(Theme::rgb(c.border))
            .arg(Theme::rgb(c.textPrimary))
            .arg(Theme::rgb(c.surface2))
            .arg(Theme::rgb(c.borderLight))
            .arg(Theme::rgb(c.textBright))
            .arg(t.sizeXs);

        auto* okBtn = new QPushButton(tr("Create Sequence"));
        m_okBtn = okBtn;
        okBtn->setCursor(Qt::PointingHandCursor);
        okBtn->setStyleSheet(actionBtnStyle);
        connect(okBtn, &QPushButton::clicked, this, &QDialog::accept);
        btnRow->addWidget(okBtn);

        auto* cancelBtn = new QPushButton(tr("Cancel"));
        cancelBtn->setCursor(Qt::PointingHandCursor);
        cancelBtn->setStyleSheet(actionBtnStyle);
        connect(cancelBtn, &QPushButton::clicked, this, &QDialog::reject);
        btnRow->addWidget(cancelBtn);

        rootLayout->addLayout(btnRow);
    }

    // Build initial resolution grid for 16:9
    rebuildResGrid();
    onFpsClicked(1); // 30 fps
}

QString SequenceDialog::sequenceName() const
{
    QString name = m_nameEdit->text().trimmed();
    return name.isEmpty() ? QStringLiteral("Sequence 1") : name;
}

uint32_t SequenceDialog::width() const
{
    return static_cast<uint32_t>(m_widthSpin->value());
}

uint32_t SequenceDialog::height() const
{
    return static_cast<uint32_t>(m_heightSpin->value());
}

double SequenceDialog::frameRate() const
{
    return m_fpsSpin->value();
}

void SequenceDialog::setMediaProperties(uint32_t mediaWidth, uint32_t mediaHeight, double mediaFps)
{
    if (mediaWidth > 0 && mediaHeight > 0) {
        const int mw = static_cast<int>(mediaWidth);
        const int mh = static_cast<int>(mediaHeight);

        // Determine aspect ratio from dimensions and select the matching
        // AR button (or Custom, revealing the custom AR row).
        int g = std::gcd(mw, mh);
        if (g <= 0) g = 1;
        int arW = mw / g;
        int arH = mh / g;
        bool customAr = false;
        if (arW == 16 && arH == 9) {
            if (auto* btn = m_arGroup->button(0)) btn->setChecked(true);
        } else if (arW == 9 && arH == 16) {
            if (auto* btn = m_arGroup->button(1)) btn->setChecked(true);
        } else if (arW == 21 && arH == 9) {
            if (auto* btn = m_arGroup->button(2)) btn->setChecked(true);
        } else {
            customAr = true;
            if (auto* btn = m_arGroup->button(3)) btn->setChecked(true);
            m_customArW->setValue(arW);
            m_customArH->setValue(arH);
        }
        m_customArRow->setVisible(customAr);

        // Rebuild the resolution grid for the detected aspect ratio, then
        // select the preset matching the actual sequence resolution — or
        // fall back to Custom, revealing the row with the exact dimensions.
        rebuildResGrid();

        int presetIdx = -1;
        for (size_t i = 0; i < m_resPresets.size(); ++i) {
            if (m_resPresets[i].first == mw && m_resPresets[i].second == mh) {
                presetIdx = static_cast<int>(i);
                break;
            }
        }
        if (presetIdx >= 0) {
            if (auto* btn = m_resGroup->button(presetIdx)) btn->setChecked(true);
            m_customResRow->setVisible(false);
        } else {
            const int customId = static_cast<int>(m_resPresets.size());
            if (auto* btn = m_resGroup->button(customId)) btn->setChecked(true);
            m_customResRow->setVisible(true);
        }

        // rebuildResGrid() reset the spinboxes to its first preset — apply
        // the actual sequence dimensions last so width()/height() are correct.
        m_widthSpin->setValue(mw);
        m_heightSpin->setValue(mh);
    }
    if (mediaFps > 0.0)
        m_fpsSpin->setValue(mediaFps);

    // Match fps preset
    double fpsMatch[] = {24.0, 30.0, 60.0};
    bool fpsMatched = false;
    for (int i = 0; i < 3; ++i) {
        if (auto* btn = m_fpsGroup->button(i)) {
            if (std::abs(fpsMatch[i] - mediaFps) < 0.001) {
                btn->setChecked(true);
                fpsMatched = true;
                break;
            }
        }
    }
    if (!fpsMatched)
        if (auto* btn = m_fpsGroup->button(3))
            btn->setChecked(true);

    m_customFpsRow->setVisible(m_fpsGroup->checkedId() == 3);
    updatePreview();
}

void SequenceDialog::setSequenceName(const QString& name)
{
    m_nameEdit->setText(name);
}

void SequenceDialog::setAcceptButtonText(const QString& text)
{
    if (m_okBtn) m_okBtn->setText(text);
}

void SequenceDialog::onArClicked(int id)
{
    m_customArRow->setVisible(id == 3);
    rebuildResGrid();
    updatePreview();
}

void SequenceDialog::onResClicked(int id)
{
    // Apply the preset values to the spinboxes when a non-Custom button is clicked
    if (id >= 0 && static_cast<size_t>(id) < m_resPresets.size()) {
        m_widthSpin->setValue(m_resPresets[static_cast<size_t>(id)].first);
        m_heightSpin->setValue(m_resPresets[static_cast<size_t>(id)].second);
    }
    m_customResRow->setVisible(id == static_cast<int>(m_resPresets.size())); // Custom = last
    updatePreview();
}

void SequenceDialog::onFpsClicked(int id)
{
    double fpsVals[] = {24.0, 30.0, 60.0, 0.0};
    if (id >= 0 && id < 4 && id != 3)
        m_fpsSpin->setValue(fpsVals[id]);
    m_customFpsRow->setVisible(id == 3);
    updatePreview();
}

void SequenceDialog::rebuildResGrid()
{
    // Get current aspect ratio
    int arW = 16, arH = 9;
    int arId = m_arGroup->checkedId();
    if (arId == 1)      { arW = 9;  arH = 16; }
    else if (arId == 2) { arW = 21; arH = 9;  }
    else if (arId == 3) { arW = m_customArW->value(); arH = m_customArH->value(); }

    // Clear existing buttons from the group and grid widget
    auto* outerLay = m_resGridWidget->layout();
    if (!outerLay) return;
    QLayoutItem* item;
    while ((item = outerLay->takeAt(0)) != nullptr) {
        if (auto* w = item->widget()) {
            const auto btns = w->findChildren<QAbstractButton*>();
            for (auto* btn : btns)
                m_resGroup->removeButton(btn);
            w->deleteLater();
        }
        delete item;
    }

    // Build preset buttons based on aspect ratio
    auto* container = new QWidget(m_resGridWidget);
    auto* grid = new QGridLayout(container);
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setSpacing(3);

    // Resolution presets
    struct ResPreset { int w; int h; const char* tag; };
    QVector<ResPreset> presets;

    if (arW == 21 && arH == 9) {
        presets = {{3440, 1440, "UW"}, {3840, 1600, "UW"}, {5120, 2160, "5K"}};
    } else if (arW >= arH) {
        // Landscape: base on fixed widths
        int widths[] = {1280, 1920, 3840};
        const char* tags[] = {"HD", "FHD", "4K"};
        for (int i = 0; i < 3; ++i) {
            int bh = static_cast<int>(std::round(static_cast<double>(widths[i]) * arH / arW));
            presets.append({widths[i], bh, tags[i]});
        }
    } else {
        // Portrait: base on fixed heights
        int heights[] = {720, 1080, 2160};
        const char* tags[] = {"HD", "FHD", "4K"};
        for (int i = 0; i < 3; ++i) {
            int bw = static_cast<int>(std::round(static_cast<double>(heights[i]) * arW / arH));
            presets.append({bw, heights[i], tags[i]});
        }
    }

    const auto& c = Theme::colors();
    const auto& t = Theme::typography();
    auto chipStyle = [&]() {
        return QStringLiteral(
            "QPushButton { background: %1; border: 1px solid %2;"
            "  color: %3; font-size: %9px; font-weight: 600;"
            "  padding: 8px 8px; min-height: 36px; }"
            "QPushButton:hover { background: %4; border-color: %5; }"
            "QPushButton:checked { background: %6; border-color: %7; color: %8; }")
            .arg(Theme::rgb(c.surface0)).arg(Theme::rgb(c.border))
            .arg(Theme::rgb(c.textPrimary))
            .arg(Theme::rgb(c.surface2)).arg(Theme::rgb(c.borderLight))
            .arg(Theme::rgb(c.accentDim)).arg(Theme::rgb(c.accent))
            .arg(Theme::rgb(c.accent))
            .arg(t.sizeXs);
    };
    auto dashStyle = [&]() {
        return QStringLiteral(
            "QPushButton { background: %1; border: 1px dashed %2;"
            "  color: %3; font-size: %9px; font-weight: 600;"
            "  padding: 8px 8px; min-height: 36px; }"
            "QPushButton:hover { background: %4; border-color: %5; }"
            "QPushButton:checked { background: %6; border-color: %7; color: %8; }")
            .arg(Theme::rgb(c.surface0)).arg(Theme::rgb(c.border))
            .arg(Theme::rgb(c.textPrimary))
            .arg(Theme::rgb(c.surface2)).arg(Theme::rgb(c.borderLight))
            .arg(Theme::rgb(c.accentDim)).arg(Theme::rgb(c.accent))
            .arg(Theme::rgb(c.accent))
            .arg(t.sizeXs);
    };

    // Create preset buttons + disconnect previous connection
    disconnect(m_resGroup, nullptr, this, nullptr);

    for (int i = 0; i < presets.size(); ++i) {
        auto* btn = new QPushButton(
            QStringLiteral("%1\u00D7%2  %3").arg(presets[i].w).arg(presets[i].h).arg(presets[i].tag));
        btn->setCheckable(true);
        btn->setChecked(i == 0);
        btn->setCursor(Qt::PointingHandCursor);
        btn->setStyleSheet(chipStyle());
        m_resGroup->addButton(btn, i);
        grid->addWidget(btn, i / 2, i % 2);
    }

    // Custom button
    int customId = presets.size();
    auto* customBtn = new QPushButton(QStringLiteral("Custom"));
    customBtn->setCheckable(true);
    customBtn->setCursor(Qt::PointingHandCursor);
    customBtn->setStyleSheet(dashStyle());
    m_resGroup->addButton(customBtn, customId);
    grid->addWidget(customBtn, customId / 2, customId % 2);

    outerLay->addWidget(container);

    connect(m_resGroup, &QButtonGroup::idClicked,
            this, &SequenceDialog::onResClicked);

    // Store presets for onResClicked to apply on selection
    m_resPresets.clear();
    for (const auto& p : presets)
        m_resPresets.push_back({p.w, p.h});

    // Apply the first preset's values
    if (!presets.isEmpty()) {
        m_widthSpin->setValue(presets[0].w);
        m_heightSpin->setValue(presets[0].h);
    }

    m_customResRow->setVisible(false);
    updatePreview();
}

void SequenceDialog::updatePreview()
{
    m_previewLabel->setText(
        QStringLiteral("%1\u00D7%2  |  %3 fps")
            .arg(m_widthSpin->value())
            .arg(m_heightSpin->value())
            .arg(m_fpsSpin->value(), 0, 'f', 2));
}

} // namespace rt
