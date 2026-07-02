/*
 * InterpretFootageDialog.cpp — Implementation.
 */

#include "dialogs/InterpretFootageDialog.h"

#include "Theme.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

namespace rt {

InterpretFootageDialog::InterpretFootageDialog(const FootageInterpretation& current,
                                                double nativeFps,
                                                bool isVFR,
                                                QWidget* parent)
    : QDialog(parent)
    , m_nativeFps(nativeFps)
{
    setWindowTitle(tr("Interpret Footage"));
    setMinimumWidth(380);

    auto* mainLayout = new QVBoxLayout(this);

    // ── Frame Rate ──────────────────────────────────────────────────────
    auto* fpsGroup = new QGroupBox(tr("Frame Rate"), this);
    auto* fpsForm = new QFormLayout(fpsGroup);

    auto* nativeLabel = new QLabel(tr("Native: %1 fps").arg(nativeFps, 0, 'f', 3), this);
    fpsForm->addRow(nativeLabel);

    // Phase 4.3a: warn on variable-frame-rate sources.  The native rate
    // above is the AVERAGE; real frame timing is uneven.  Playback/export
    // are time-anchored so audio stays in sync, but assuming a constant
    // rate here makes frame timing uniform.
    if (isVFR) {
        auto* vfrLabel = new QLabel(
            tr("⚠ Variable frame rate detected — the native rate is the "
               "average. Assume a constant rate below to conform timing."),
            this);
        vfrLabel->setWordWrap(true);
        vfrLabel->setStyleSheet(QStringLiteral("color: %1;")
            .arg(Theme::hex(Theme::colors().warning)));
        fpsForm->addRow(vfrLabel);
    }

    m_fpsSpin = new QDoubleSpinBox(this);
    m_fpsSpin->setRange(0.0, 240.0);
    m_fpsSpin->setDecimals(3);
    m_fpsSpin->setSpecialValueText(tr("Use file rate"));
    m_fpsSpin->setValue(current.overrideFps);
    fpsForm->addRow(tr("Assume this frame rate:"), m_fpsSpin);

    mainLayout->addWidget(fpsGroup);

    // ── Alpha Channel ───────────────────────────────────────────────────
    auto* alphaGroup = new QGroupBox(tr("Alpha Channel"), this);
    auto* alphaForm = new QFormLayout(alphaGroup);

    m_alphaCombo = new QComboBox(this);
    m_alphaCombo->addItem(tr("Auto-detect"),     static_cast<int>(AlphaInterpretation::Auto));
    m_alphaCombo->addItem(tr("Ignore"),          static_cast<int>(AlphaInterpretation::Ignore));
    m_alphaCombo->addItem(tr("Straight (Unmatted)"), static_cast<int>(AlphaInterpretation::Straight));
    m_alphaCombo->addItem(tr("Premultiplied (Matted)"), static_cast<int>(AlphaInterpretation::Premultiplied));
    for (int i = 0; i < m_alphaCombo->count(); ++i) {
        if (m_alphaCombo->itemData(i).toInt() == static_cast<int>(current.alpha)) {
            m_alphaCombo->setCurrentIndex(i);
            break;
        }
    }
    alphaForm->addRow(tr("Interpretation:"), m_alphaCombo);

    mainLayout->addWidget(alphaGroup);

    // ── Pixel Aspect Ratio ──────────────────────────────────────────────
    auto* parGroup = new QGroupBox(tr("Pixel Aspect Ratio"), this);
    auto* parForm = new QFormLayout(parGroup);

    m_parCombo = new QComboBox(this);
    m_parCombo->addItem(tr("Square Pixels (1.0)"),       QVariant(1.0));
    m_parCombo->addItem(tr("D1/DV NTSC (0.9091)"),      QVariant(0.9091));
    m_parCombo->addItem(tr("D1/DV PAL (1.0940)"),       QVariant(1.0940));
    m_parCombo->addItem(tr("D1/DV NTSC WS (1.2121)"),   QVariant(1.2121));
    m_parCombo->addItem(tr("HDV 1080/DVCPRO HD (1.333)"), QVariant(1.333));
    m_parCombo->addItem(tr("Anamorphic 2:1 (2.0)"),     QVariant(2.0));

    // Select the closest match
    double bestDist = 1e9;
    int bestIdx = 0;
    for (int i = 0; i < m_parCombo->count(); ++i) {
        double d = std::abs(m_parCombo->itemData(i).toDouble() - current.pixelAspectRatio);
        if (d < bestDist) { bestDist = d; bestIdx = i; }
    }
    m_parCombo->setCurrentIndex(bestIdx);
    parForm->addRow(tr("Ratio:"), m_parCombo);

    mainLayout->addWidget(parGroup);

    // ── Buttons ─────────────────────────────────────────────────────────
    mainLayout->addStretch();
    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    mainLayout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
}

FootageInterpretation InterpretFootageDialog::result() const
{
    FootageInterpretation interp;
    interp.overrideFps = m_fpsSpin->value();
    interp.alpha = static_cast<AlphaInterpretation>(
        m_alphaCombo->currentData().toInt());
    interp.pixelAspectRatio = m_parCombo->currentData().toDouble();
    return interp;
}

} // namespace rt
