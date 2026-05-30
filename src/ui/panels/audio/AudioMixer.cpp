/*
 * AudioMixer.cpp -- rebuild, sync, meters, fader/pan/mute/solo, events.
 *
 * Commands/setupUI --> AudioMixerUI.cpp
 */

#include "AudioMixer.h"

#include "Theme.h"

#include <spdlog/spdlog.h>

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

namespace rt {

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

void AudioMixer::rebuildStrips()
{
    spdlog::info("[AudioMixer] ENTER rebuildStrips: m_timeline={} (ptr)", (void*)m_timeline);
    // Remove old strips
    while (m_stripLayout->count() > 0) {
        auto* item = m_stripLayout->takeAt(0);
        if (item->widget()) {
            item->widget()->deleteLater();
        }
        delete item;
    }
    m_strips.clear();

    if (!m_timeline) {
        // Add just the master strip
        m_stripLayout->addWidget(createMasterStrip());
        m_stripLayout->addStretch();
        return;
    }

    // Create a strip for each audio track
    for (size_t i = 0; i < m_timeline->trackCount(); ++i) {
        Track* track = m_timeline->track(i);
        spdlog::info("[AudioMixer] rebuildStrips: track idx={} ptr={} type={} (expect Audio={})", i, (void*)track, track ? (int)track->type() : -1, (int)TrackType::Audio);
        if (!track || track->type() != TrackType::Audio) continue;

        ChannelStrip strip;
        strip.track      = track;
        strip.trackIndex = i;
        strip.isMaster   = false;

        spdlog::info("[AudioMixer] Creating ChannelStrip for track idx={} ptr={}", i, (void*)track);
        QWidget* w = createChannelStrip(strip);
        m_strips.push_back(strip);
        m_stripLayout->addWidget(w);
    }

    // Separator line
    auto* sep = new QWidget();
    sep->setObjectName("MixerSeparator");
    sep->setFixedWidth(2);
    sep->setMinimumHeight(80);
    sep->setStyleSheet(
        QString("QWidget { background: %1; }")
            .arg(Theme::hex(Theme::colors().panelBorder)));
    m_stripLayout->addWidget(sep);

    // Master strip
    m_stripLayout->addWidget(createMasterStrip());

    m_stripLayout->addStretch();

    // Update status bar
    if (m_statusLabel) {
        int count = static_cast<int>(m_strips.size());
        m_statusLabel->setText(count == 1 ? tr("1 track")
                                          : tr("%1 tracks").arg(count));
    }

    // Toggle empty state
    bool empty = m_strips.empty();
    if (m_emptyLabel)  m_emptyLabel->setVisible(empty);
    if (m_scrollArea)  m_scrollArea->setVisible(!empty);
}

// ═════════════════════════════════════════════════════════════════════════════
// Sync helpers
// ═════════════════════════════════════════════════════════════════════════════

void AudioMixer::syncStripToTrack(ChannelStrip& strip)
{
    spdlog::info("[AudioMixer] ENTER syncStripToTrack: strip.track={} strip.trackIndex={}", (void*)strip.track, strip.trackIndex);
    if (!strip.track) return;

    // Block signals to prevent feedback loops
    if (strip.fader) {
        strip.fader->blockSignals(true);
        strip.fader->setValue(ChannelStrip::volumeToFader(strip.track->volume()));
        strip.fader->blockSignals(false);
    }

    if (strip.faderLabel) {
        auto dbStr = ChannelStrip::volumeToDbString(strip.track->volume());
        spdlog::info("[AudioMixer] Setting faderLabel text: '{}' for track ptr={} index={}", dbStr.toStdString(), (void*)strip.track, strip.trackIndex);
        strip.faderLabel->setText(dbStr);
    }

    if (strip.panDial) {
        strip.panDial->blockSignals(true);
        strip.panDial->setValue(ChannelStrip::panToDial(strip.track->pan()));
        strip.panDial->blockSignals(false);
    }

    if (strip.panLabel) {
        auto panStr = ChannelStrip::panToString(strip.track->pan());
        spdlog::info("[AudioMixer] Setting panLabel text: '{}' for track ptr={} index={}", panStr.toStdString(), (void*)strip.track, strip.trackIndex);
        strip.panLabel->setText(panStr);
    }

    if (strip.muteButton) {
        strip.muteButton->blockSignals(true);
        strip.muteButton->setChecked(strip.track->isMuted());
        strip.muteButton->blockSignals(false);
    }

    if (strip.soloButton) {
        strip.soloButton->blockSignals(true);
        strip.soloButton->setChecked(strip.track->isSoloed());
        strip.soloButton->blockSignals(false);
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// Context menu & reset helpers
// ═════════════════════════════════════════════════════════════════════════════

void AudioMixer::showStripContextMenu(const QPoint& pos, ChannelStrip& strip)
{
    QMenu menu(this);
    menu.setStyleSheet(
        QStringLiteral(
        "QMenu { background: %1; border: 1px solid %2; border-radius: 6px; "
        "color: %3; font-size: 13px; padding: 4px 0; }"
        "QMenu::item { padding: 7px 24px; }"
        "QMenu::item:selected { background: %4; color: %5; }"
        "QMenu::separator { height: 1px; background: %2; margin: 4px 10px; }")
        .arg(Theme::hex(Theme::colors().surface2))
        .arg(Theme::hex(Theme::colors().borderLight))
        .arg(Theme::hex(Theme::colors().textPrimary))
        .arg(Theme::hex(Theme::colors().accent))
        .arg(Theme::hex(Theme::colors().textBright)));

    auto* resetVolAct = menu.addAction(QStringLiteral("Reset Volume to 0 dB"));
    auto* resetPanAct = menu.addAction(QStringLiteral("Reset Pan to Center"));
    menu.addSeparator();
    auto* muteAct = menu.addAction(strip.track && strip.track->isMuted()
                                    ? QStringLiteral("Unmute")
                                    : QStringLiteral("Mute"));
    auto* soloAct = menu.addAction(strip.track && strip.track->isSoloed()
                                    ? QStringLiteral("Unsolo")
                                    : QStringLiteral("Solo"));

    auto* chosen = menu.exec(strip.container->mapToGlobal(pos));
    if (!chosen) return;

    if (chosen == resetVolAct) {
        resetFader(strip);
    } else if (chosen == resetPanAct) {
        resetPan(strip);
    } else if (chosen == muteAct) {
        if (strip.muteButton) {
            strip.muteButton->click();
        }
    } else if (chosen == soloAct) {
        if (strip.soloButton) {
            strip.soloButton->click();
        }
    }
}

void AudioMixer::resetFader(ChannelStrip& strip)
{
    if (!strip.fader) return;
    strip.fader->setValue(kFaderDefault); // triggers onFaderChanged via signal
}

void AudioMixer::resetPan(ChannelStrip& strip)
{
    if (!strip.panDial) return;
    strip.panDial->setValue(0); // triggers onPanChanged via signal
}

// ═════════════════════════════════════════════════════════════════════════════
// Metering
// ═════════════════════════════════════════════════════════════════════════════

void AudioMixer::updateMeters()
{
    // Lifetime: m_timeline is nulled via onTimelineDestroyed() before the
    // timeline is freed, and strip.track pointers are cleared with it, so the
    // dereferences below can no longer touch freed memory. (This is why the
    // former try/catch(...) + /EHa SEH band-aid was removed — see #20.)
    if (!m_audioEngine) return;

    // If audio is not actively playing, decay meters to silence
    auto state = m_audioEngine->transportState();
    if (state != TransportState::Playing && state != TransportState::Scrubbing) {
        constexpr float decay = 0.85f;
        bool allSilent = true;
        if (m_masterStrip.vuMeter) {
            float curL = m_masterStrip.vuMeter->level(0);
            float curR = m_masterStrip.vuMeter->level(1);
            m_masterStrip.vuMeter->setLevel(0, curL * decay < 0.001f ? 0.0f : curL * decay);
            m_masterStrip.vuMeter->setLevel(1, curR * decay < 0.001f ? 0.0f : curR * decay);
            if (curL >= 0.001f || curR >= 0.001f) allSilent = false;
        }
        for (auto& strip : m_strips) {
            if (!strip.vuMeter) continue;
            float curL = strip.vuMeter->level(0);
            float curR = strip.vuMeter->level(1);
            strip.vuMeter->setLevel(0, curL * decay < 0.001f ? 0.0f : curL * decay);
            strip.vuMeter->setLevel(1, curR * decay < 0.001f ? 0.0f : curR * decay);
            if (curL >= 0.001f || curR >= 0.001f) allSilent = false;
        }
        // Stop polling once fully decayed — restartMeterTimer() resumes on play
        if (allSilent && m_meterTimer)
            m_meterTimer->stop();
        return;
    }

    // Master meter from audio engine
    AudioMeter m = m_audioEngine->meter();
    if (m_masterStrip.vuMeter) {
        m_masterStrip.vuMeter->setLevel(0, m.peakL);
        m_masterStrip.vuMeter->setLevel(1, m.peakR);
    }

    // Per-track meters: the audio engine currently only does master metering.
    // Show proportional levels based on track volume/pan for visual feedback.
    // Guard against dangling track pointers (use-after-free when tracks are
    // removed from timeline without rebuildStrips() being called).
    for (auto& strip : m_strips) {
        if (!strip.track || !strip.vuMeter) continue;
        // Validate track pointer is still in the timeline before accessing
        if (m_timeline) {
            bool found = false;
            for (size_t ti = 0; ti < m_timeline->trackCount(); ++ti) {
                if (m_timeline->track(ti) == strip.track) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                strip.track = nullptr;
                continue;
            }
        }
        const float vol = strip.track->volume();
        const float pan = strip.track->pan();
        // Approximate per-track levels from master meter scaled by volume
        const float leftGain  = vol * std::max(0.0f, 1.0f - pan);
        const float rightGain = vol * std::max(0.0f, 1.0f + pan);
        // Scale by master peak as approximation
        const float avgPeak = (m.peakL + m.peakR) * 0.5f;
        strip.vuMeter->setLevel(0, avgPeak * leftGain);
        strip.vuMeter->setLevel(1, avgPeak * rightGain);
    }
}

void AudioMixer::ensureMeterTimerRunning()
{
    if (m_meterTimer && !m_meterTimer->isActive())
        m_meterTimer->start(kMeterRefreshMs);
}

// ═════════════════════════════════════════════════════════════════════════════
// Accessors
// ═════════════════════════════════════════════════════════════════════════════

ChannelStrip* AudioMixer::stripForTrack(size_t trackIndex)
{
    for (auto& s : m_strips) {
        if (s.trackIndex == trackIndex) return &s;
    }
    return nullptr;
}

// ═════════════════════════════════════════════════════════════════════════════
// Slots
// ═════════════════════════════════════════════════════════════════════════════

void AudioMixer::onFaderChanged(int value)
{
    auto* slider = qobject_cast<QSlider*>(sender());
    if (!slider) return;

    const auto idx = slider->property("trackIndex").toUInt();
    const float newVol = ChannelStrip::faderToVolume(value);

    // Master fader
    if (idx == 9999) {
        const float oldVol = m_audioEngine ? m_audioEngine->masterVolume() : 1.0f;
        if (m_audioEngine) m_audioEngine->setMasterVolume(newVol);
        if (m_masterStrip.faderLabel)
            m_masterStrip.faderLabel->setText(ChannelStrip::volumeToDbString(newVol));

        if (m_commandStack) {
            m_commandStack->execute(
                std::make_unique<SetMasterVolumeCommand>(m_audioEngine, oldVol, newVol)
            );
        }
        return;
    }

    // Track fader
    ChannelStrip* strip = stripForTrack(idx);
    if (!strip || !strip->track) return;

    const float oldVol = strip->track->volume();

    if (m_commandStack) {
        m_commandStack->execute(
            std::make_unique<SetTrackVolumeCommand>(strip->track, oldVol, newVol)
        );
    } else {
        strip->track->setVolume(newVol);
    }

    if (strip->faderLabel)
        strip->faderLabel->setText(ChannelStrip::volumeToDbString(newVol));

    emit volumeChanged(idx, newVol);
}

void AudioMixer::onPanChanged(int value)
{
    auto* dial = qobject_cast<QDial*>(sender());
    if (!dial) return;

    const auto idx = dial->property("trackIndex").toUInt();
    const float newPan = ChannelStrip::dialToPan(value);

    ChannelStrip* strip = stripForTrack(idx);
    if (!strip || !strip->track) return;

    const float oldPan = strip->track->pan();

    if (m_commandStack) {
        m_commandStack->execute(
            std::make_unique<SetTrackPanCommand>(strip->track, oldPan, newPan)
        );
    } else {
        strip->track->setPan(newPan);
    }

    if (strip->panLabel)
        strip->panLabel->setText(ChannelStrip::panToString(newPan));

    emit panChanged(idx, newPan);
}

void AudioMixer::onMuteToggled()
{
    auto* btn = qobject_cast<QPushButton*>(sender());
    if (!btn) return;

    const auto idx = btn->property("trackIndex").toUInt();

    // Master mute — toggle via command stack for undo support
    if (idx == 9999) {
        const bool muted = btn->isChecked();
        if (m_audioEngine) {
            const float curVol = m_audioEngine->masterVolume();
            const float newVol = muted ? 0.0f : 1.0f;
            if (m_commandStack) {
                m_commandStack->execute(
                    std::make_unique<SetMasterVolumeCommand>(m_audioEngine, curVol, newVol)
                );
            } else {
                m_audioEngine->setMasterVolume(newVol);
            }
            if (m_masterStrip.fader) {
                m_masterStrip.fader->blockSignals(true);
                m_masterStrip.fader->setValue(ChannelStrip::volumeToFader(newVol));
                m_masterStrip.fader->blockSignals(false);
            }
            if (m_masterStrip.faderLabel)
                m_masterStrip.faderLabel->setText(ChannelStrip::volumeToDbString(newVol));
        }
        return;
    }

    ChannelStrip* strip = stripForTrack(idx);
    if (!strip || !strip->track) return;

    const bool newMuted = !strip->track->isMuted();

    if (m_commandStack) {
        m_commandStack->execute(
            std::make_unique<SetTrackMuteCommand>(strip->track, newMuted)
        );
    } else {
        strip->track->setMuted(newMuted);
    }

    btn->setChecked(newMuted);

    emit muteChanged(idx, newMuted);
}

void AudioMixer::onSoloToggled()
{
    auto* btn = qobject_cast<QPushButton*>(sender());
    if (!btn) return;

    const auto idx = btn->property("trackIndex").toUInt();

    ChannelStrip* strip = stripForTrack(idx);
    if (!strip || !strip->track) return;

    const bool newSoloed = !strip->track->isSoloed();

    if (m_commandStack) {
        m_commandStack->execute(
            std::make_unique<SetTrackSoloCommand>(strip->track, newSoloed)
        );
    } else {
        strip->track->setSoloed(newSoloed);
    }

    btn->setChecked(newSoloed);

    emit soloChanged(idx, newSoloed);
}

// ═════════════════════════════════════════════════════════════════════════════
// Event filter — double-click on fader/dial to reset
// ═════════════════════════════════════════════════════════════════════════════

bool AudioMixer::eventFilter(QObject* obj, QEvent* event)
{
    if (event->type() == QEvent::MouseButtonDblClick) {
        // Double-click on a fader → reset to 0 dB
        if (auto* slider = qobject_cast<QSlider*>(obj)) {
            const auto idx = slider->property("trackIndex").toUInt();
            if (idx == 9999) {
                resetFader(m_masterStrip);
            } else {
                if (auto* s = stripForTrack(idx))
                    resetFader(*s);
            }
            return true;
        }
        // Double-click on a pan dial → reset to center
        if (auto* dial = qobject_cast<QDial*>(obj)) {
            const auto idx = dial->property("trackIndex").toUInt();
            if (auto* s = stripForTrack(idx))
                resetPan(*s);
            return true;
        }
    }
    return QWidget::eventFilter(obj, event);
}

} // namespace rt
