/*
 * AudioMixerUI.cpp -- commands, ChannelStrip helpers, constructor, setupUI.
 *
 * Split from AudioMixer.cpp for maintainability.
 */

#include "AudioMixer.h"

#include "Theme.h"

#include "command/Command.h"
#include "command/CommandStack.h"
#include "media/AudioEngine.h"
#include "timeline/Timeline.h"
#include "timeline/Track.h"

#include <QApplication>
#include <QHBoxLayout>
#include <QMenu>
#include <QMouseEvent>
#include <QScreen>
#include <QScrollArea>
#include <QTimer>

#include <algorithm>
#include <cmath>



// ═════════════════════════════════════════════════════════════════════════════
// Command IDs for merge support
// ═════════════════════════════════════════════════════════════════════════════

static constexpr int kCmdIdSetVolume = 2500;
static constexpr int kCmdIdSetPan    = 2501;

// ═════════════════════════════════════════════════════════════════════════════
// Mixer command implementations
// ═════════════════════════════════════════════════════════════════════════════

namespace rt {

void SetTrackVolumeCommand::execute() { m_track->setVolume(m_newVolume); }
void SetTrackVolumeCommand::undo()    { m_track->setVolume(m_oldVolume); }

std::string SetTrackVolumeCommand::description() const { return "Set Track Volume"; }
int SetTrackVolumeCommand::typeId() const { return kCmdIdSetVolume; }

bool SetTrackVolumeCommand::mergeWith(const Command& next)
{
    if (next.typeId() != kCmdIdSetVolume) return false;
    auto& other = static_cast<const SetTrackVolumeCommand&>(next);
    if (other.m_track != m_track) return false;
    m_newVolume = other.m_newVolume;
    return true;
}

void SetTrackPanCommand::execute() { m_track->setPan(m_newPan); }
void SetTrackPanCommand::undo()    { m_track->setPan(m_oldPan); }

std::string SetTrackPanCommand::description() const { return "Set Track Pan"; }
int SetTrackPanCommand::typeId() const { return kCmdIdSetPan; }

bool SetTrackPanCommand::mergeWith(const Command& next)
{
    if (next.typeId() != kCmdIdSetPan) return false;
    auto& other = static_cast<const SetTrackPanCommand&>(next);
    if (other.m_track != m_track) return false;
    m_newPan = other.m_newPan;
    return true;
}

void SetTrackMuteCommand::execute() { m_track->setMuted(m_muted); }
void SetTrackMuteCommand::undo()    { m_track->setMuted(!m_muted); }

std::string SetTrackMuteCommand::description() const
{
    return m_muted ? "Mute Track" : "Unmute Track";
}

void SetTrackSoloCommand::execute() { m_track->setSoloed(m_soloed); }
void SetTrackSoloCommand::undo()    { m_track->setSoloed(!m_soloed); }

std::string SetTrackSoloCommand::description() const
{
    return m_soloed ? "Solo Track" : "Unsolo Track";
}

/// Undoable command: set master volume (internal, not in header).
class SetMasterVolumeCommand : public Command
{
public:
    SetMasterVolumeCommand(AudioEngine* engine, float oldVol, float newVol)
        : m_engine(engine), m_oldVolume(oldVol), m_newVolume(newVol) {}

    void execute() override { if (m_engine) m_engine->setMasterVolume(m_newVolume); }
    void undo()    override { if (m_engine) m_engine->setMasterVolume(m_oldVolume); }

    [[nodiscard]] std::string description() const override
    {
        return "Set Master Volume";
    }

    [[nodiscard]] int typeId() const override { return kCmdIdSetVolume + 100; }

    [[nodiscard]] bool mergeWith(const Command& next) override
    {
        if (next.typeId() != typeId()) return false;
        auto& other = static_cast<const SetMasterVolumeCommand&>(next);
        m_newVolume = other.m_newVolume;
        return true;
    }

private:
    AudioEngine* m_engine;
    float        m_oldVolume;
    float        m_newVolume;
};

// ═════════════════════════════════════════════════════════════════════════════
// ChannelStrip static helpers
// ═════════════════════════════════════════════════════════════════════════════

float ChannelStrip::faderToVolume(int faderValue) noexcept
{
    // Logarithmic mapping: 0 → -inf (0.0), kFaderMax → +6 dB (~2.0)
    // Unity (0 dB) at ~80% of slider range
    if (faderValue <= 0) return 0.0f;

    constexpr float maxDb  =  6.0f;   // Top of fader
    constexpr float minDb  = -60.0f;  // Bottom of fader (practical silence)
    constexpr float range  = maxDb - minDb;
    constexpr int   maxVal = 1000;

    const float normalized = static_cast<float>(faderValue) / static_cast<float>(maxVal);
    const float db = minDb + normalized * range;
    return std::pow(10.0f, db / 20.0f);
}

int ChannelStrip::volumeToFader(float volume) noexcept
{
    if (volume <= 0.0f) return 0;

    constexpr float maxDb  =  6.0f;
    constexpr float minDb  = -60.0f;
    constexpr float range  = maxDb - minDb;
    constexpr int   maxVal = 1000;

    const float db = 20.0f * std::log10(std::max(volume, 1e-10f));
    const float normalized = (db - minDb) / range;
    return std::clamp(static_cast<int>(normalized * maxVal), 0, maxVal);
}

QString ChannelStrip::volumeToDbString(float volume)
{
    if (volume <= 0.0f) return QStringLiteral("-inf dB");
    const float db = 20.0f * std::log10(std::max(volume, 1e-10f));
    return QString::number(static_cast<double>(db), 'f', 1) + QStringLiteral(" dB");
}

float ChannelStrip::dialToPan(int dialValue) noexcept
{
    return std::clamp(static_cast<float>(dialValue) / 100.0f, -1.0f, 1.0f);
}

int ChannelStrip::panToDial(float pan) noexcept
{
    return std::clamp(static_cast<int>(std::round(pan * 100.0f)), -100, 100);
}

QString ChannelStrip::panToString(float pan)
{
    if (std::abs(pan) < 0.01f) return QStringLiteral("C");
    if (pan < 0.0f)
        return QStringLiteral("L") + QString::number(static_cast<int>(std::abs(pan) * 100));
    return QStringLiteral("R") + QString::number(static_cast<int>(pan * 100));
}

// ═════════════════════════════════════════════════════════════════════════════
// AudioMixer
// ═════════════════════════════════════════════════════════════════════════════

AudioMixer::AudioMixer(QWidget* parent)
    : QWidget(parent)
{
    setupUI();
}

AudioMixer::~AudioMixer()
{
    // Unregister so a later Timeline destruction can't call back into a
    // destroyed AudioMixer.
    if (m_timeline) m_timeline->removeObserver(this);
}

QSize AudioMixer::sizeHint() const { return {520, 440}; }

// ═════════════════════════════════════════════════════════════════════════════
// Configuration
// ═════════════════════════════════════════════════════════════════════════════

void AudioMixer::setTimeline(Timeline* timeline)
{
    if (m_timeline != timeline) {
        // Observe the timeline so onTimelineDestroyed() can null m_timeline
        // before it is freed (no more dangling deref in the meter timer).
        if (m_timeline) m_timeline->removeObserver(this);
        m_timeline = timeline;
        if (m_timeline) m_timeline->addObserver(this);
    }
    rebuildStrips();
}

void AudioMixer::onTimelineDestroyed(Timeline* tl)
{
    if (tl != m_timeline) return;
    // The timeline is going away: drop every reference into it and halt the
    // meter timer so updateMeters() cannot touch freed memory. Strips hold
    // raw Track* into this timeline, so clear them too.
    m_timeline = nullptr;
    if (m_meterTimer) m_meterTimer->stop();
    for (auto& strip : m_strips) strip.track = nullptr;
}

void AudioMixer::setAudioEngine(AudioEngine* engine)
{
    m_audioEngine = engine;

    // If we have an engine, start the meter polling timer
    if (engine && !m_meterTimer) {
        m_meterTimer = new QTimer(this);
        connect(m_meterTimer, &QTimer::timeout, this, &AudioMixer::updateMeters);
        m_meterTimer->start(kMeterRefreshMs);
    } else if (!engine && m_meterTimer) {
        m_meterTimer->stop();
        delete m_meterTimer;
        m_meterTimer = nullptr;
    }
}

void AudioMixer::setCommandStack(CommandStack* stack)
{
    m_commandStack = stack;
}

// ═════════════════════════════════════════════════════════════════════════════
// UI setup
// ═════════════════════════════════════════════════════════════════════════════

void AudioMixer::setupUI()
{
    const auto& tc = Theme::colors();
    const auto& m  = Theme::metrics();
    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(0);

    // ── Toolbar ──────────────────────────────────────────────────────────
    auto* toolbar = new QWidget(this);
    toolbar->setFixedHeight(28);
    toolbar->setStyleSheet(QStringLiteral(
        "QWidget { background: %1; border-bottom: 1px solid %2; }")
        .arg(Theme::hex(tc.surface2), Theme::hex(tc.panelBorder)));
    auto* toolbarLayout = new QHBoxLayout(toolbar);
    toolbarLayout->setContentsMargins(m.spacingXs, m.spacingXxs, m.spacingXs, m.spacingXxs);
    toolbarLayout->setSpacing(m.spacingXs);

    auto* titleLabel = new QLabel(tr("Audio Mixer"), toolbar);
    titleLabel->setStyleSheet(
        QString("QLabel { color: %1; font-weight: bold; font-size: 12px; "
                "background: transparent; border: none; }")
            .arg(Theme::hex(tc.textPrimary)));
    toolbarLayout->addWidget(titleLabel);
    toolbarLayout->addStretch();
    mainLayout->addWidget(toolbar);

    // ── Scroll area for channel strips ───────────────────────────────────
    m_scrollArea = new QScrollArea(this);
    m_scrollArea->setObjectName("MixerScroll");
    m_scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_scrollArea->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_scrollArea->setWidgetResizable(true);
    m_scrollArea->setFrameShape(QFrame::NoFrame);
    m_scrollArea->setStyleSheet(
        QString("QScrollArea { background: %1; border: none; }")
            .arg(Theme::hex(tc.surface0)));

    m_stripContainer = new QWidget();
    m_stripContainer->setObjectName("StripContainer");
    m_stripContainer->setStyleSheet(
        QString("QWidget#StripContainer { background: %1; }")
            .arg(Theme::hex(tc.surface0)));
    m_stripLayout = new QHBoxLayout(m_stripContainer);
    m_stripLayout->setContentsMargins(m.spacingMd, m.spacingMd, m.spacingMd, m.spacingMd);
    m_stripLayout->setSpacing(m.spacingSm);
    m_stripLayout->addStretch();

    m_scrollArea->setWidget(m_stripContainer);
    mainLayout->addWidget(m_scrollArea);

    // ── Empty state ──────────────────────────────────────────────────────
    m_emptyLabel = new QLabel(tr("No audio tracks.\nAdd audio tracks to the timeline to mix."), this);
    m_emptyLabel->setAlignment(Qt::AlignCenter);
    m_emptyLabel->setObjectName("EmptyStateLabel");
    m_emptyLabel->setStyleSheet(QStringLiteral(
        "color: %1; font-size: 12px; background: transparent;")
        .arg(Theme::hex(tc.textDisabled)));
    m_emptyLabel->setVisible(true);
    m_scrollArea->setVisible(false);
    mainLayout->addWidget(m_emptyLabel, 1);

    // ── Status bar ───────────────────────────────────────────────────────
    auto* statusBar = new QWidget(this);
    statusBar->setFixedHeight(22);
    statusBar->setStyleSheet(QStringLiteral(
        "QWidget { background: %1; border-top: 1px solid %2; }")
        .arg(Theme::hex(tc.surface2), Theme::hex(tc.panelBorder)));
    auto* statusLayout = new QHBoxLayout(statusBar);
    statusLayout->setContentsMargins(m.spacingMd, 0, m.spacingMd, 0);
    statusLayout->setSpacing(0);

    m_statusLabel = new QLabel(tr("0 tracks"), statusBar);
    m_statusLabel->setStyleSheet(
        QString("QLabel { color: %1; font-size: 12px; background: transparent; border: none; }")
            .arg(Theme::hex(tc.textSecondary)));
    statusLayout->addWidget(m_statusLabel);
    statusLayout->addStretch();
    mainLayout->addWidget(statusBar);

    setMinimumHeight(360);
}

// ═════════════════════════════════════════════════════════════════════════════
// Strip construction
// ═════════════════════════════════════════════════════════════════════════════

QWidget* AudioMixer::createChannelStrip(ChannelStrip& strip)
{
    const auto& m = Theme::metrics();
    auto* container = new QWidget();
    container->setObjectName("ChannelStrip");
    container->setFixedWidth(120);
    strip.container = container;

    auto* layout = new QVBoxLayout(container);
    layout->setContentsMargins(m.spacingMd, 10, m.spacingMd, 10);
    layout->setSpacing(5);

    // Track name label
    strip.nameLabel = new QLabel(
        strip.track ? QString::fromStdString(strip.track->name()) : QStringLiteral("?"),
        container
    );
    strip.nameLabel->setObjectName("TrackName");
    strip.nameLabel->setAlignment(Qt::AlignCenter);
    layout->addWidget(strip.nameLabel);

    // Thin divider below track name
    auto* nameDivider = new QWidget(container);
    nameDivider->setObjectName("StripDivider");
    nameDivider->setFixedHeight(1);
    layout->addWidget(nameDivider);

    layout->addSpacing(2);

    // VU Meter
    strip.vuMeter = new VUMeter(container);
    strip.vuMeter->setChannelCount(2);
    strip.vuMeter->setOrientation(VUMeter::Orientation::Vertical);
    strip.vuMeter->setScaleVisible(false);
    strip.vuMeter->setMinimumHeight(140);
    strip.vuMeter->setFixedWidth(44);
    layout->addWidget(strip.vuMeter, 1, Qt::AlignHCenter);

    // Volume fader (vertical slider)
    strip.fader = new QSlider(Qt::Vertical, container);
    strip.fader->setRange(kFaderMin, kFaderMax);
    strip.fader->setValue(ChannelStrip::volumeToFader(
        strip.track ? strip.track->volume() : 1.0f
    ));
    strip.fader->setFixedHeight(130);
    strip.fader->setToolTip(QStringLiteral("Volume — double-click to reset to 0 dB"));
    strip.fader->setProperty("trackIndex", static_cast<uint>(strip.trackIndex));
    connect(strip.fader, &QSlider::valueChanged, this, &AudioMixer::onFaderChanged);
    // Double-click to reset fader to unity (0 dB)
    strip.fader->installEventFilter(this);
    layout->addWidget(strip.fader, 0, Qt::AlignHCenter);

    // Fader dB label
    strip.faderLabel = new QLabel(
        ChannelStrip::volumeToDbString(strip.track ? strip.track->volume() : 1.0f),
        container
    );
    strip.faderLabel->setObjectName("DbLabel");
    strip.faderLabel->setAlignment(Qt::AlignCenter);
    layout->addWidget(strip.faderLabel);

    layout->addSpacing(2);

    // Pan knob (QDial)
    strip.panDial = new QDial(container);
    strip.panDial->setRange(kPanMin, kPanMax);
    strip.panDial->setValue(ChannelStrip::panToDial(
        strip.track ? strip.track->pan() : 0.0f
    ));
    strip.panDial->setFixedSize(50, 50);
    strip.panDial->setWrapping(false);
    strip.panDial->setNotchesVisible(true);
    strip.panDial->setToolTip(QStringLiteral("Pan — double-click to center"));
    strip.panDial->setProperty("trackIndex", static_cast<uint>(strip.trackIndex));
    connect(strip.panDial, &QDial::valueChanged, this, &AudioMixer::onPanChanged);
    // Double-click to reset pan to center
    strip.panDial->installEventFilter(this);
    layout->addWidget(strip.panDial, 0, Qt::AlignHCenter);

    // Pan label
    strip.panLabel = new QLabel(
        ChannelStrip::panToString(strip.track ? strip.track->pan() : 0.0f),
        container
    );
    strip.panLabel->setObjectName("PanLabel");
    strip.panLabel->setAlignment(Qt::AlignCenter);
    layout->addWidget(strip.panLabel);

    layout->addSpacing(m.spacingXs);

    // Mute / Solo buttons row
    auto* btnRow = new QHBoxLayout();
    btnRow->setSpacing(5);

    strip.muteButton = new QPushButton(QStringLiteral("M"), container);
    strip.muteButton->setObjectName("MuteBtn");
    strip.muteButton->setFixedSize(42, 28);
    strip.muteButton->setCheckable(true);
    strip.muteButton->setToolTip(QStringLiteral("Mute"));
    strip.muteButton->setProperty("trackIndex", static_cast<uint>(strip.trackIndex));
    if (strip.track) strip.muteButton->setChecked(strip.track->isMuted());
    connect(strip.muteButton, &QPushButton::clicked, this, &AudioMixer::onMuteToggled);
    btnRow->addWidget(strip.muteButton);

    strip.soloButton = new QPushButton(QStringLiteral("S"), container);
    strip.soloButton->setObjectName("SoloBtn");
    strip.soloButton->setFixedSize(42, 28);
    strip.soloButton->setCheckable(true);
    strip.soloButton->setToolTip(QStringLiteral("Solo"));
    strip.soloButton->setProperty("trackIndex", static_cast<uint>(strip.trackIndex));
    if (strip.track) strip.soloButton->setChecked(strip.track->isSoloed());
    connect(strip.soloButton, &QPushButton::clicked, this, &AudioMixer::onSoloToggled);
    btnRow->addWidget(strip.soloButton);

    layout->addLayout(btnRow);

    // Context menu on right-click
    container->setContextMenuPolicy(Qt::CustomContextMenu);
    const size_t stripTrackIdx = strip.trackIndex;
    connect(container, &QWidget::customContextMenuRequested, this,
            [this, stripTrackIdx](const QPoint& pos) {
                if (auto* s = stripForTrack(stripTrackIdx))
                    showStripContextMenu(pos, *s);
            });

    return container;
}

QWidget* AudioMixer::createMasterStrip()
{
    const auto& m = Theme::metrics();
    m_masterStrip = ChannelStrip{};
    m_masterStrip.isMaster = true;

    auto* container = new QWidget();
    container->setObjectName("MasterStrip");
    container->setFixedWidth(140);
    m_masterStrip.container = container;

    auto* layout = new QVBoxLayout(container);
    layout->setContentsMargins(10, 10, 10, 10);
    layout->setSpacing(5);

    // Master label
    m_masterStrip.nameLabel = new QLabel(QStringLiteral("MASTER"), container);
    m_masterStrip.nameLabel->setObjectName("MasterName");
    m_masterStrip.nameLabel->setAlignment(Qt::AlignCenter);
    layout->addWidget(m_masterStrip.nameLabel);

    // Thin divider below master name
    auto* masterDivider = new QWidget(container);
    masterDivider->setObjectName("StripDivider");
    masterDivider->setFixedHeight(1);
    layout->addWidget(masterDivider);

    layout->addSpacing(2);

    // VU Meter
    m_masterStrip.vuMeter = new VUMeter(container);
    m_masterStrip.vuMeter->setChannelCount(2);
    m_masterStrip.vuMeter->setOrientation(VUMeter::Orientation::Vertical);
    m_masterStrip.vuMeter->setScaleVisible(true);
    m_masterStrip.vuMeter->setMinimumHeight(140);
    m_masterStrip.vuMeter->setFixedWidth(56);
    layout->addWidget(m_masterStrip.vuMeter, 1, Qt::AlignHCenter);

    // Master fader
    m_masterStrip.fader = new QSlider(Qt::Vertical, container);
    m_masterStrip.fader->setRange(kFaderMin, kFaderMax);
    const float masterVol = m_audioEngine ? m_audioEngine->masterVolume() : 1.0f;
    m_masterStrip.fader->setValue(ChannelStrip::volumeToFader(masterVol));
    m_masterStrip.fader->setFixedHeight(130);
    m_masterStrip.fader->setToolTip(QStringLiteral("Master Volume — double-click to reset to 0 dB"));
    m_masterStrip.fader->setProperty("trackIndex", static_cast<uint>(9999)); // sentinel
    connect(m_masterStrip.fader, &QSlider::valueChanged, this, &AudioMixer::onFaderChanged);
    m_masterStrip.fader->installEventFilter(this);
    layout->addWidget(m_masterStrip.fader, 0, Qt::AlignHCenter);

    // Master dB label
    m_masterStrip.faderLabel = new QLabel(
        ChannelStrip::volumeToDbString(masterVol), container
    );
    m_masterStrip.faderLabel->setObjectName("DbLabel");
    m_masterStrip.faderLabel->setAlignment(Qt::AlignCenter);
    layout->addWidget(m_masterStrip.faderLabel);

    layout->addSpacing(m.spacingXs);

    // Mute button (master mute)
    m_masterStrip.muteButton = new QPushButton(QStringLiteral("M"), container);
    m_masterStrip.muteButton->setObjectName("MuteBtn");
    m_masterStrip.muteButton->setFixedSize(42, 28);
    m_masterStrip.muteButton->setCheckable(true);
    m_masterStrip.muteButton->setToolTip(QStringLiteral("Master Mute"));
    m_masterStrip.muteButton->setProperty("trackIndex", static_cast<uint>(9999));
    connect(m_masterStrip.muteButton, &QPushButton::clicked, this, &AudioMixer::onMuteToggled);
    layout->addWidget(m_masterStrip.muteButton, 0, Qt::AlignHCenter);

    return container;
}

// ═════════════════════════════════════════════════════════════════════════════
// Rebuild
// ═════════════════════════════════════════════════════════════════════════════


} // namespace rt
