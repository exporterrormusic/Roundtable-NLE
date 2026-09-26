/*
 * TimelinePanelAudioGain.cpp — Premiere Pro "Audio Gain" dialog.
 *
 *   ( ) Set Gain to:            every clip gets exactly this gain
 *   ( ) Adjust Gain by:         every clip's gain moves by this much
 *   ( ) Normalize Max Peak to:  ONE shared offset so the loudest peak across
 *                               all selected clips lands on the target
 *   ( ) Normalize All Peaks to: each clip on its own, so every clip's peak
 *                               lands on the target
 *   Peak Amplitude:             loudest peak of the selection at current gain
 *
 * Applies to every selected audio clip (or just the right-clicked one when it
 * is not part of the selection) as ONE undo step. "Gain" here is the clip's
 * volume track: a static volume changes its value, an animated one keeps its
 * keyframe shape and is scaled as a whole. Peaks are measured over the part
 * of the source each clip actually plays, not the whole file.
 */

#include "panels/timeline/TimelinePanel.h"
#include "widgets/ScrubbySpinBox.h"

#include "Constants.h"
#include "audio/AudioFile.h"
#include "command/CommandStack.h"
#include "command/LambdaCommand.h"
#include "timeline/AudioClip.h"
#include "timeline/Timeline.h"
#include "timeline/Track.h"

#include <QApplication>
#include <QButtonGroup>
#include <QDialog>
#include <QDialogButtonBox>
#include <QGridLayout>
#include <QLabel>
#include <QRadioButton>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

namespace rt {

namespace {

constexpr double kMinGainDb = -96.0;
constexpr double kMaxGainDb = 24.0;

// Premiere reopens the dialog on the option used last.
int s_lastAudioGainMode = TimelinePanel::AudioGainAdjust;

float currentGainLinear(const AudioClip& ac)
{
    const auto& vol = ac.volume();
    return vol.keyframeCount() > 0 ? vol.keyframe(0).value : vol.defaultValue();
}

double linearToDb(double lin)
{
    return lin > 1e-6 ? 20.0 * std::log10(lin) : kMinGainDb;
}

// Loudest |sample| (linear, before gain) in the source range the clip plays.
// Returns a negative value when the media can't be read.
float sourcePeakOfClip(const AudioClip& ac)
{
    AudioFile af;
    if (!af.open(ac.mediaPath(), ac.audioStreamIndex()))
        return -1.0f;
    const auto& info = af.info();
    if (info.sampleRate == 0 || info.channels == 0)
        return -1.0f;

    const double sr  = static_cast<double>(info.sampleRate);
    const double spd = std::max(std::abs(ac.speed()), 0.01);
    const int64_t start = std::max<int64_t>(
        0, static_cast<int64_t>(static_cast<double>(ac.sourceIn()) * sr / kTicksPerSecond));
    const int64_t count = static_cast<int64_t>(
        std::ceil(static_cast<double>(ac.duration()) * spd * sr / kTicksPerSecond));

    constexpr int64_t kChunkFrames = 1 << 18;
    std::vector<float> buf;
    float peak = 0.0f;
    for (int64_t pos = start, end = start + count; pos < end;) {
        const int64_t got = af.readRegion(pos, std::min(kChunkFrames, end - pos), buf);
        if (got <= 0) break;
        const size_t n = std::min(buf.size(), static_cast<size_t>(got) * info.channels);
        for (size_t i = 0; i < n; ++i)
            peak = std::max(peak, std::fabs(buf[i]));
        pos += got;
    }
    return peak;
}

AudioClip* findAudioClip(Timeline* timeline, uint64_t clipId)
{
    if (!timeline) return nullptr;
    for (size_t ti = 0; ti < timeline->trackCount(); ++ti) {
        Track* tr = timeline->track(ti);
        if (!tr) continue;
        const size_t idx = tr->findClipIndexById(clipId);
        if (idx < tr->clipCount())
            return dynamic_cast<AudioClip*>(tr->clip(idx));
    }
    return nullptr;
}

} // namespace

void TimelinePanel::showAudioGainDialog(Clip* clicked, int initialMode)
{
    auto* clickedAudio = dynamic_cast<AudioClip*>(clicked);
    if (!m_timeline || !clickedAudio) return;

    struct Target {
        AudioClip* clip;
        double     gainDb;
        float      sourcePeak; // linear, <0 = unreadable
    };
    std::vector<Target> targets;
    auto addTarget = [&](AudioClip* ac) {
        for (const auto& t : targets)
            if (t.clip == ac) return;
        targets.push_back({ac, linearToDb(currentGainLinear(*ac)), -1.0f});
    };

    // Clicked clip first so it drives the "Set Gain to" field.
    addTarget(clickedAudio);
    bool clickedIsSelected = false;
    for (const auto& ref : m_selection.clips()) {
        if (ref.clipId == clickedAudio->id()) clickedIsSelected = true;
    }
    if (clickedIsSelected) {
        for (const auto& ref : m_selection.clips()) {
            Track* tr = m_timeline->track(ref.trackIndex);
            if (!tr || tr->isLocked()) continue;
            const size_t idx = tr->findClipIndexById(ref.clipId);
            if (idx >= tr->clipCount()) continue;
            if (auto* ac = dynamic_cast<AudioClip*>(tr->clip(idx)))
                addTarget(ac);
        }
    }

    // Measure peaks (the only slow part — reads the used source ranges).
    QApplication::setOverrideCursor(Qt::WaitCursor);
    double groupPeakDb = kMinGainDb;
    bool anyPeak = false;
    for (auto& t : targets) {
        t.sourcePeak = sourcePeakOfClip(*t.clip);
        if (t.sourcePeak > 0.0f) {
            groupPeakDb = std::max(groupPeakDb, linearToDb(t.sourcePeak) + t.gainDb);
            anyPeak = true;
        }
    }
    QApplication::restoreOverrideCursor();

    // ── Dialog ──────────────────────────────────────────────────────────
    QDialog dlg(this);
    dlg.setWindowTitle(tr("Audio Gain"));
    dlg.setMinimumWidth(320);

    auto* layout = new QVBoxLayout(&dlg);
    auto* grid = new QGridLayout;
    grid->setHorizontalSpacing(12);
    grid->setVerticalSpacing(8);

    const QString labels[4] = {
        tr("Set Gain to:"), tr("Adjust Gain by:"),
        tr("Normalize Max Peak to:"), tr("Normalize All Peaks to:"),
    };
    const double lo[4] = {kMinGainDb, kMinGainDb - kMaxGainDb, kMinGainDb, kMinGainDb};
    const double hi[4] = {kMaxGainDb, kMaxGainDb - kMinGainDb, 0.0, 0.0};
    const double primaryDb = targets.front().gainDb;
    const double initial[4] = {primaryDb, 0.0, 0.0, 0.0};

    auto* group = new QButtonGroup(&dlg);
    QRadioButton*   radios[4];
    ScrubbySpinBox* spins[4];
    for (int i = 0; i < 4; ++i) {
        radios[i] = new QRadioButton(labels[i]);
        group->addButton(radios[i], i);
        spins[i] = new ScrubbySpinBox;
        spins[i]->setDecimals(1);
        spins[i]->setRange(lo[i], hi[i]);
        spins[i]->setSingleStep(0.1);
        spins[i]->setScrubStep(0.1);
        spins[i]->setSuffix(QStringLiteral(" dB"));
        spins[i]->setValue(initial[i]);
        spins[i]->setMinimumWidth(90);
        grid->addWidget(radios[i], i, 0);
        grid->addWidget(spins[i], i, 1);
    }
    // Normalizing needs readable peaks.
    for (int i : {int(AudioGainNormalizeMax), int(AudioGainNormalizeAll)}) {
        radios[i]->setEnabled(anyPeak);
        spins[i]->setEnabled(anyPeak);
    }
    int mode = std::clamp(initialMode >= 0 ? initialMode : s_lastAudioGainMode, 0, 3);
    if (!radios[mode]->isEnabled()) mode = AudioGainAdjust;
    radios[mode]->setChecked(true);
    layout->addLayout(grid);

    // Editing a field selects its option; Set/Adjust mirror each other
    // relative to the right-clicked clip, like Premiere.
    auto syncing = std::make_shared<bool>(false);
    for (int i = 0; i < 4; ++i) {
        QObject::connect(spins[i], &QDoubleSpinBox::valueChanged, &dlg,
                         [=](double v) {
            if (*syncing) return;
            radios[i]->setChecked(true);
            *syncing = true;
            if (i == AudioGainSet)
                spins[AudioGainAdjust]->setValue(v - primaryDb);
            else if (i == AudioGainAdjust)
                spins[AudioGainSet]->setValue(primaryDb + v);
            *syncing = false;
        });
    }

    auto* peakLabel = new QLabel(anyPeak
        ? tr("Peak Amplitude: %1 dB").arg(groupPeakDb, 0, 'f', 1)
        : tr("Peak Amplitude: unavailable (media offline)"));
    peakLabel->setContentsMargins(0, 8, 0, 0);
    layout->addWidget(peakLabel);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);

    if (dlg.exec() != QDialog::Accepted) return;

    mode = group->checkedId();
    if (mode < 0) return;
    s_lastAudioGainMode = mode;
    const double value = spins[mode]->value();

    // ── Apply as one undoable step ──────────────────────────────────────
    struct Change {
        uint64_t              clipId;
        KeyframeTrack<float>  before;
        KeyframeTrack<float>  after;
    };
    auto changes = std::make_shared<std::vector<Change>>();
    for (const auto& t : targets) {
        double newDb = t.gainDb;
        switch (mode) {
        case AudioGainSet:          newDb = value; break;
        case AudioGainAdjust:       newDb = t.gainDb + value; break;
        case AudioGainNormalizeMax: newDb = t.gainDb + (value - groupPeakDb); break;
        case AudioGainNormalizeAll:
            if (t.sourcePeak <= 0.0f) continue; // unreadable: leave as is
            newDb = value - linearToDb(t.sourcePeak);
            break;
        default: break;
        }
        newDb = std::clamp(newDb, kMinGainDb, kMaxGainDb);
        if (std::abs(newDb - t.gainDb) < 1e-4) continue;

        const float curLin = currentGainLinear(*t.clip);
        const float newLin = static_cast<float>(std::pow(10.0, newDb / 20.0));
        Change ch{t.clip->id(), t.clip->volume(), t.clip->volume()};
        if (ch.after.keyframeCount() == 0) {
            ch.after.setDefaultValue(newLin);
        } else {
            // Keep the keyframe shape; move it as a whole.
            const float ratio = curLin > 1e-6f ? newLin / curLin : newLin;
            for (size_t k = 0; k < ch.after.keyframeCount(); ++k)
                ch.after.keyframe(k).value *= ratio;
        }
        changes->push_back(std::move(ch));
    }
    if (changes->empty()) return;

    // Clip IDs, resolved at execute time (clips may be re-created by undo).
    auto apply = [this, changes](bool redo) {
        for (const auto& ch : *changes) {
            if (auto* ac = findAudioClip(m_timeline, ch.clipId))
                ac->volume() = redo ? ch.after : ch.before;
        }
        onScrollChanged();
        emit contentChanged();
    };
    if (m_commandStack) {
        m_commandStack->execute(std::make_unique<LambdaCommand>(
            "Audio Gain", [apply] { apply(true); }, [apply] { apply(false); }));
    } else {
        apply(true);
    }
}

} // namespace rt
