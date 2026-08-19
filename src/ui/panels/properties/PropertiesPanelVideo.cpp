/*
 * PropertiesPanelVideo.cpp — Video clip property application and UI setup.
 * Split from PropertiesPanelApply.cpp + PropertiesPanelUI.cpp.
 *
 * Contains: applyVideoVolume(), setupVideoSection().
 */

#include "panels/properties/PropertiesPanel.h"
#include "widgets/ScrubbySpinBox.h"
#include "Theme.h"

#include "timeline/Clip.h"
#include "timeline/VideoClip.h"
#include "command/CommandStack.h"
#include "command/LambdaCommand.h"

#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>

namespace rt {

// ═════════════════════════════════════════════════════════════════════════════
//  Video property apply
// ═════════════════════════════════════════════════════════════════════════════

void PropertiesPanel::applyVideoVolume()
{
    if (m_updating || !canMutateBoundClip() || m_clip->clipType() != ClipType::Video) return;
    auto* vc = static_cast<VideoClip*>(m_clip);
    float newVal = static_cast<float>(m_volumeSpin->value());
    if (newVal == vc->volume()) return;
    float oldVal = vc->volume();
    if (m_commandStack) {
        m_commandStack->execute(std::make_unique<LambdaCommand>(
            "Change video volume",
            [vc, newVal, this]() { vc->setVolume(newVal); populateFromClip(); emit propertyChanged(); },
            [vc, oldVal, this]() { vc->setVolume(oldVal); populateFromClip(); emit propertyChanged(); }));
    } else {
        vc->setVolume(newVal);
        emit propertyChanged();
    }
}

// ═════════════════════════════════════════════════════════════════════════════
//  Video section UI
// ═════════════════════════════════════════════════════════════════════════════

void PropertiesPanel::setupVideoSection(QWidget* container)
{
    const auto& m = Theme::metrics();
    m_videoSection = new QGroupBox("Video", container);
    auto* form = new QFormLayout(m_videoSection);
    form->setContentsMargins(m.spacingXs, 18, m.spacingXs, m.spacingXs);
    form->setSpacing(m.spacingSm);

    m_mediaPathLabel = new QLabel(m_videoSection);
    m_mediaPathLabel->setWordWrap(true);
    m_mediaPathLabel->setStyleSheet(QStringLiteral("color: %1;")
        .arg(Theme::hex(Theme::colors().textSecondary)));
    form->addRow("Source:", m_mediaPathLabel);

    m_volumeSpin = createScrubby(0.0, 2.0, 0.01, 3);
    m_volumeSpin->setToolTip(tr("Video clip audio volume (0 = mute, 1.0 = full)"));
    connect(m_volumeSpin, &ScrubbySpinBox::valueCommitted,
            this, [this](double, double) { applyVideoVolume(); });
    connect(m_volumeSpin, &QDoubleSpinBox::editingFinished,
            this, &PropertiesPanel::applyVideoVolume);
    form->addRow("Volume:", m_volumeSpin);

    container->layout()->addWidget(m_videoSection);
}

} // namespace rt
