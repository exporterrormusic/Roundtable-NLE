/*
 * PropertiesPanelAudio.cpp — Audio clip property application and UI setup.
 * Split from PropertiesPanelApply.cpp + PropertiesPanelUI.cpp.
 *
 * Contains: applyAudioVolume(), applyAudioPan(), applyAudioFadeIn(),
 *           applyAudioFadeOut(), setupAudioSection().
 */

#include "panels/properties/PropertiesPanel.h"
#include "widgets/ScrubbySpinBox.h"
#include "widgets/AudioFxSection.h"
#include "Theme.h"

#include "timeline/Clip.h"
#include "timeline/AudioClip.h"
#include "command/CommandStack.h"
#include "command/LambdaCommand.h"

#include <QComboBox>
#include <QFormLayout>
#include <QGroupBox>

namespace rt {

// ═════════════════════════════════════════════════════════════════════════════
//  Audio property apply
// ═════════════════════════════════════════════════════════════════════════════

void PropertiesPanel::applyAudioVolume()
{
    if (m_updating || !canMutateBoundClip() || !m_clip->isAudio()) return;
    auto* ac = static_cast<AudioClip*>(m_clip);
    float newVal = static_cast<float>(m_audioVolumeSpin->value());
    float oldVal = ac->volume().evaluate(0);
    if (m_commandStack) {
        m_commandStack->execute(std::make_unique<LambdaCommand>(
            "Change audio volume",
            [ac, newVal, this]() { ac->volume().addKeyframe(0, newVal); populateFromClip(); emit propertyChanged(); },
            [ac, oldVal, this]() { ac->volume().addKeyframe(0, oldVal); populateFromClip(); emit propertyChanged(); }));
    } else {
        ac->volume().addKeyframe(0, newVal);
        emit propertyChanged();
    }
}

void PropertiesPanel::applyAudioPan()
{
    if (m_updating || !canMutateBoundClip() || !m_clip->isAudio()) return;
    auto* ac = static_cast<AudioClip*>(m_clip);
    float newVal = static_cast<float>(m_panSpin->value());
    float oldVal = ac->pan().evaluate(0);
    if (m_commandStack) {
        m_commandStack->execute(std::make_unique<LambdaCommand>(
            "Change audio pan",
            [ac, newVal, this]() { ac->pan().addKeyframe(0, newVal); populateFromClip(); emit propertyChanged(); },
            [ac, oldVal, this]() { ac->pan().addKeyframe(0, oldVal); populateFromClip(); emit propertyChanged(); }));
    } else {
        ac->pan().addKeyframe(0, newVal);
        emit propertyChanged();
    }
}

void PropertiesPanel::applyAudioFadeIn()
{
    if (m_updating || !canMutateBoundClip() || !m_clip->isAudio()) return;
    auto* ac = static_cast<AudioClip*>(m_clip);
    int64_t newVal = static_cast<int64_t>(m_fadeInSpin->value());
    int64_t oldVal = ac->fadeInDuration();
    if (newVal == oldVal) return;
    if (m_commandStack) {
        m_commandStack->execute(std::make_unique<LambdaCommand>(
            "Change fade in",
            [ac, newVal, this]() { ac->setFadeInDuration(newVal); populateFromClip(); emit propertyChanged(); },
            [ac, oldVal, this]() { ac->setFadeInDuration(oldVal); populateFromClip(); emit propertyChanged(); }));
    } else {
        ac->setFadeInDuration(newVal);
        emit propertyChanged();
    }
}

void PropertiesPanel::applyAudioFadeOut()
{
    if (m_updating || !canMutateBoundClip() || !m_clip->isAudio()) return;
    auto* ac = static_cast<AudioClip*>(m_clip);
    int64_t newVal = static_cast<int64_t>(m_fadeOutSpin->value());
    int64_t oldVal = ac->fadeOutDuration();
    if (newVal == oldVal) return;
    if (m_commandStack) {
        m_commandStack->execute(std::make_unique<LambdaCommand>(
            "Change fade out",
            [ac, newVal, this]() { ac->setFadeOutDuration(newVal); populateFromClip(); emit propertyChanged(); },
            [ac, oldVal, this]() { ac->setFadeOutDuration(oldVal); populateFromClip(); emit propertyChanged(); }));
    } else {
        ac->setFadeOutDuration(newVal);
        emit propertyChanged();
    }
}

void PropertiesPanel::applyAudioStream()
{
    if (m_updating || !canMutateBoundClip() || !m_clip->isAudio() || !m_audioStreamCombo) return;
    auto* ac = static_cast<AudioClip*>(m_clip);
    const QVariant data = m_audioStreamCombo->currentData();
    if (!data.isValid()) return;
    const int newVal = data.toInt();
    const int oldVal = ac->audioStreamIndex();
    if (newVal == oldVal) return;
    const quint64 clipId = ac->id();
    // propertyChanged rebuilds the audio sources (new cache key -> new stream);
    // audioStreamChanged tells the timeline to re-decode this clip's waveform.
    if (m_commandStack) {
        m_commandStack->execute(std::make_unique<LambdaCommand>(
            "Change audio stream",
            [ac, newVal, clipId, this]() { ac->setAudioStreamIndex(newVal); populateFromClip(); emit propertyChanged(); emit audioStreamChanged(clipId); },
            [ac, oldVal, clipId, this]() { ac->setAudioStreamIndex(oldVal); populateFromClip(); emit propertyChanged(); emit audioStreamChanged(clipId); }));
    } else {
        ac->setAudioStreamIndex(newVal);
        emit propertyChanged();
        emit audioStreamChanged(clipId);
    }
}

// ═════════════════════════════════════════════════════════════════════════════
//  Audio section UI
// ═════════════════════════════════════════════════════════════════════════════

void PropertiesPanel::setupAudioSection(QWidget* container)
{
    const auto& m = Theme::metrics();
    m_audioSection = new QGroupBox("Audio", container);
    auto* form = new QFormLayout(m_audioSection);
    form->setContentsMargins(m.spacingXs, 18, m.spacingXs, m.spacingXs);
    form->setSpacing(m.spacingSm);

    // Audio stream picker — for multicam / camera scratch+lav / OBS multi-track
    // sources. Populated per-clip in populateFromAudio(); disabled when the
    // file has a single audio stream (nothing to choose).
    m_audioStreamCombo = new QComboBox(m_audioSection);
    m_audioStreamCombo->setToolTip(tr("Which audio stream of the source file to use"));
    m_audioStreamCombo->setEditable(false);
    m_audioStreamCombo->installEventFilter(this);
    connect(m_audioStreamCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int) { if (m_updating) return; applyAudioStream(); });
    form->addRow("Audio Stream:", m_audioStreamCombo);

    m_audioVolumeSpin = createScrubby(0.0, 2.0, 0.01, 3);
    m_audioVolumeSpin->setToolTip(tr("Audio clip volume (0 = mute, 1.0 = full)"));
    connect(m_audioVolumeSpin, &ScrubbySpinBox::valueCommitted,
            this, [this](double, double) { applyAudioVolume(); });
    connect(m_audioVolumeSpin, &QDoubleSpinBox::editingFinished,
            this, &PropertiesPanel::applyAudioVolume);
    form->addRow("Volume:", m_audioVolumeSpin);

    m_panSpin = createScrubby(-1.0, 1.0, 0.01, 3);
    m_panSpin->setToolTip(tr("Stereo pan (-1.0 = left, 0 = center, 1.0 = right)"));
    connect(m_panSpin, &ScrubbySpinBox::valueCommitted,
            this, [this](double, double) { applyAudioPan(); });
    connect(m_panSpin, &QDoubleSpinBox::editingFinished,
            this, &PropertiesPanel::applyAudioPan);
    form->addRow("Pan:", m_panSpin);

    m_fadeInSpin = createScrubby(0.0, 100000.0, 100.0, 0, " ticks");
    m_fadeInSpin->setToolTip(tr("Fade-in duration in ticks"));
    m_fadeInSpin->setIntegerMode();
    connect(m_fadeInSpin, &ScrubbySpinBox::valueCommitted,
            this, [this](double, double) { applyAudioFadeIn(); });
    connect(m_fadeInSpin, &QDoubleSpinBox::editingFinished,
            this, &PropertiesPanel::applyAudioFadeIn);
    form->addRow("Fade In:", m_fadeInSpin);

    m_fadeOutSpin = createScrubby(0.0, 100000.0, 100.0, 0, " ticks");
    m_fadeOutSpin->setToolTip(tr("Fade-out duration in ticks"));
    m_fadeOutSpin->setIntegerMode();
    connect(m_fadeOutSpin, &ScrubbySpinBox::valueCommitted,
            this, [this](double, double) { applyAudioFadeOut(); });
    connect(m_fadeOutSpin, &QDoubleSpinBox::editingFinished,
            this, &PropertiesPanel::applyAudioFadeOut);
    form->addRow("Fade Out:", m_fadeOutSpin);

    container->layout()->addWidget(m_audioSection);

    // Audio effect chain editor (EQ / Dynamics) — sits just below the basic
    // audio controls. Hidden until an audio clip is selected.
    m_audioFxSection = new AudioFxSection(container);
    m_audioFxSection->setVisible(false);
    connect(m_audioFxSection, &AudioFxSection::changed,
            this, &PropertiesPanel::propertyChanged);
    container->layout()->addWidget(m_audioFxSection);
}

} // namespace rt
