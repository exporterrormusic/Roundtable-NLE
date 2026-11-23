/*
 * EssentialSoundSection.cpp — see EssentialSoundSection.h.
 */

#include "widgets/EssentialSoundSection.h"
#include "widgets/ScrubbySpinBox.h"

#include "audio/ClipLoudness.h"
#include "audiofx/EssentialSound.h"
#include "command/CommandStack.h"
#include "command/LambdaCommand.h"
#include "edit/EditSession.h"
#include "timeline/AudioClip.h"
#include "timeline/Timeline.h"
#include "timeline/Track.h"
#include "Constants.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QFont>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <memory>

namespace rt {

namespace {

using EsSettings = audiofx::EssentialSoundSettings;
using audiofx::SoundType;

AudioClip* esFindAudioClip(Timeline* timeline, uint64_t clipId)
{
    if (!timeline || clipId == 0) return nullptr;
    for (size_t ti = 0; ti < timeline->trackCount(); ++ti) {
        Track* tr = timeline->track(ti);
        if (!tr) continue;
        const size_t idx = tr->findClipIndexById(clipId);
        if (idx < tr->clipCount()) return dynamic_cast<AudioClip*>(tr->clip(idx));
    }
    return nullptr;
}

ScrubbySpinBox* esAmountSpin(float value)
{
    auto* s = new ScrubbySpinBox;
    s->setRange(0.0, 10.0);
    s->setSingleStep(0.1);
    s->setScrubStep(0.1);
    s->setDecimals(1);
    s->setValue(value);
    return s;
}

QGroupBox* esGroup(const QString& title, QFormLayout** form)
{
    auto* box = new QGroupBox(title);
    auto* f = new QFormLayout(box);
    f->setContentsMargins(6, 6, 6, 6);
    f->setSpacing(3);
    *form = f;
    return box;
}

} // namespace

EssentialSoundSection::EssentialSoundSection(QWidget* parent)
    : QWidget(parent)
{
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(4);
    outer->addWidget(new QLabel(tr("<b>Essential Sound</b>")));
    m_body = new QVBoxLayout;
    m_body->setContentsMargins(0, 0, 0, 0);
    m_body->setSpacing(4);
    outer->addLayout(m_body);
    setVisible(false);
}

EssentialSoundSection::~EssentialSoundSection() = default;

void EssentialSoundSection::setClip(AudioClip* clip, Timeline* timeline, CommandStack* stack)
{
    m_timeline = timeline;
    m_commandStack = stack;
    m_clipId = clip ? clip->id() : 0;
    rebuild();
}

AudioClip* EssentialSoundSection::resolveClip() const
{
    return esFindAudioClip(m_timeline, m_clipId);
}

void EssentialSoundSection::scheduleRebuild()
{
    if (m_rebuildQueued) return;
    m_rebuildQueued = true;
    QPointer<EssentialSoundSection> self(this);
    QTimer::singleShot(0, this, [self]() {
        if (self) { self->m_rebuildQueued = false; self->rebuild(); }
    });
}

void EssentialSoundSection::commitSettings(const QString& name,
                                           const std::function<void(EsSettings&)>& mutate)
{
    AudioClip* clip = resolveClip();
    if (!clip) return;
    const EsSettings before = clip->essentialSound();
    EsSettings after = before;
    mutate(after);
    if (after == before) return;

    Timeline* timeline = m_timeline;
    const uint64_t clipId = m_clipId;
    QPointer<EssentialSoundSection> self(this);
    auto apply = [timeline, clipId, self](const EsSettings& s) {
        if (AudioClip* c = esFindAudioClip(timeline, clipId)) c->setEssentialSound(s);
        if (self) {
            self->scheduleRebuild();
            emit self->changed();
        }
    };
    auto redo = [apply, after]() { apply(after); };
    auto undo = [apply, before]() { apply(before); };
    if (m_commandStack)
        executeTimelineEdit(m_commandStack, nullptr,
                            std::make_unique<LambdaCommand>(name.toStdString(), redo, undo));
    else
        redo();
}

void EssentialSoundSection::rebuild()
{
    while (QLayoutItem* item = m_body->takeAt(0)) {
        if (QWidget* w = item->widget()) w->deleteLater();
        delete item;
    }
    AudioClip* clip = resolveClip();
    if (!clip) { setVisible(false); return; }
    setVisible(true);

    m_body->addWidget(buildTypeRow());
    const SoundType type = clip->essentialSound().type;
    if (type == SoundType::None) {
        auto* hint = new QLabel(tr("Pick what this clip is to get Premiere-style "
                                   "repair, clarity and space controls."));
        hint->setWordWrap(true);
        hint->setEnabled(false);
        m_body->addWidget(hint);
        return;
    }
    m_body->addWidget(buildLoudness());
    if (type == SoundType::Dialogue) {
        m_body->addWidget(buildRepair());
        m_body->addWidget(buildClarity());
    }
    if (type != SoundType::Music) m_body->addWidget(buildCreative());
    if (type == SoundType::Ambience) m_body->addWidget(buildAmbience());
    if (type == SoundType::Music || type == SoundType::Ambience) m_body->addWidget(buildDucking());
}

// ── Sections ────────────────────────────────────────────────────────────────

QWidget* EssentialSoundSection::buildTypeRow()
{
    auto* row = new QWidget;
    auto* h = new QHBoxLayout(row);
    h->setContentsMargins(0, 0, 0, 0);
    h->setSpacing(3);
    const SoundType current = resolveClip()->essentialSound().type;
    for (int t = 1; t < static_cast<int>(SoundType::Count); ++t) {
        const auto type = static_cast<SoundType>(t);
        auto* b = new QPushButton(QString::fromUtf8(audiofx::soundTypeName(type)));
        b->setCheckable(true);
        b->setChecked(type == current);
        b->setToolTip(tr("Tag this clip as %1").arg(b->text()));
        h->addWidget(b);
        connect(b, &QPushButton::clicked, this, [this, type]() {
            commitSettings(tr("Essential Sound: %1").arg(audiofx::soundTypeName(type)),
                           [type](EsSettings& s) {
                if (s.type == type) return;
                s = EsSettings{};   // a new type starts from its defaults
                s.type = type;
                s.loudnessTarget = audiofx::defaultLoudnessTarget(type);
            });
        });
    }
    if (current != SoundType::None) {
        auto* clear = new QPushButton(tr("Clear"));
        clear->setToolTip(tr("Clear Audio Type (removes the Essential Sound processing)"));
        h->addWidget(clear);
        connect(clear, &QPushButton::clicked, this, [this]() {
            commitSettings(tr("Clear Audio Type"), [](EsSettings& s) { s = EsSettings{}; });
        });
    }
    return row;
}

QWidget* EssentialSoundSection::buildLoudness()
{
    QFormLayout* f = nullptr;
    auto* box = esGroup(tr("Loudness"), &f);
    const EsSettings& s = resolveClip()->essentialSound();
    auto* target = new ScrubbySpinBox;
    target->setRange(-40.0, -5.0);
    target->setSingleStep(0.5);
    target->setScrubStep(0.5);
    target->setDecimals(1);
    target->setSuffix(tr(" LUFS"));
    target->setValue(s.loudnessTarget);
    f->addRow(tr("Target:"), target);
    connect(target, &ScrubbySpinBox::valueCommitted, this, [this](double, double v) {
        commitSettings(tr("Loudness Target"),
                       [v](EsSettings& st) { st.loudnessTarget = static_cast<float>(v); });
    });
    auto* match = new QPushButton(tr("Auto-Match"));
    match->setToolTip(tr("Measure this clip (LUFS) and set its volume to the target"));
    f->addRow(QString(), match);
    connect(match, &QPushButton::clicked, this, [this]() { autoMatchLoudness(); });
    return box;
}

QWidget* EssentialSoundSection::buildRepair()
{
    QFormLayout* f = nullptr;
    auto* box = esGroup(tr("Repair"), &f);
    const EsSettings& s = resolveClip()->essentialSound();

    auto addToggleAmount = [&](const QString& label, bool on, float amount,
                               bool EsSettings::*flag, float EsSettings::*value) {
        auto* cb = new QCheckBox(label);
        cb->setChecked(on);
        auto* spin = esAmountSpin(amount);
        spin->setEnabled(on);
        f->addRow(cb, spin);
        connect(cb, &QCheckBox::toggled, this, [this, label, flag](bool v) {
            commitSettings(label, [flag, v](EsSettings& st) { st.*flag = v; });
        });
        connect(spin, &ScrubbySpinBox::valueCommitted, this, [this, label, value](double, double v) {
            commitSettings(label, [value, v](EsSettings& st) { st.*value = static_cast<float>(v); });
        });
    };
    addToggleAmount(tr("Reduce Rumble"), s.reduceRumble, s.rumbleAmount,
                    &EsSettings::reduceRumble, &EsSettings::rumbleAmount);
    addToggleAmount(tr("DeHum"), s.deHum, s.deHumAmount, &EsSettings::deHum, &EsSettings::deHumAmount);
    if (s.deHum) {
        auto* hz = new QComboBox;
        hz->addItems({tr("50 Hz"), tr("60 Hz")});
        hz->setCurrentIndex(s.hum60Hz ? 1 : 0);
        f->addRow(tr("  Mains:"), hz);
        connect(hz, qOverload<int>(&QComboBox::activated), this, [this](int i) {
            commitSettings(tr("DeHum Frequency"), [i](EsSettings& st) { st.hum60Hz = i == 1; });
        });
    }
    addToggleAmount(tr("DeEss"), s.deEss, s.deEssAmount, &EsSettings::deEss, &EsSettings::deEssAmount);
    return box;
}

QWidget* EssentialSoundSection::buildClarity()
{
    QFormLayout* f = nullptr;
    auto* box = esGroup(tr("Clarity"), &f);
    const EsSettings& s = resolveClip()->essentialSound();

    auto* dyn = new QCheckBox(tr("Dynamics"));
    dyn->setChecked(s.dynamics);
    auto* dynAmt = esAmountSpin(s.dynamicsAmount);
    dynAmt->setEnabled(s.dynamics);
    f->addRow(dyn, dynAmt);
    connect(dyn, &QCheckBox::toggled, this, [this](bool v) {
        commitSettings(tr("Dynamics"), [v](EsSettings& st) { st.dynamics = v; });
    });
    connect(dynAmt, &ScrubbySpinBox::valueCommitted, this, [this](double, double v) {
        commitSettings(tr("Dynamics"), [v](EsSettings& st) { st.dynamicsAmount = static_cast<float>(v); });
    });

    auto* eq = new QCheckBox(tr("EQ"));
    eq->setChecked(s.clarityEq);
    auto* preset = new QComboBox;
    for (int p = 0; p < static_cast<int>(audiofx::ClarityEqPreset::Count); ++p)
        preset->addItem(QString::fromUtf8(
            audiofx::clarityEqPresetName(static_cast<audiofx::ClarityEqPreset>(p))));
    preset->setCurrentIndex(s.eqPreset);
    preset->setEnabled(s.clarityEq);
    f->addRow(eq, preset);
    auto* eqAmt = esAmountSpin(s.eqAmount);
    eqAmt->setEnabled(s.clarityEq);
    f->addRow(tr("  Amount:"), eqAmt);
    connect(eq, &QCheckBox::toggled, this, [this](bool v) {
        commitSettings(tr("Clarity EQ"), [v](EsSettings& st) { st.clarityEq = v; });
    });
    connect(preset, qOverload<int>(&QComboBox::activated), this, [this](int i) {
        commitSettings(tr("Clarity EQ Preset"), [i](EsSettings& st) { st.eqPreset = i; });
    });
    connect(eqAmt, &ScrubbySpinBox::valueCommitted, this, [this](double, double v) {
        commitSettings(tr("Clarity EQ"), [v](EsSettings& st) { st.eqAmount = static_cast<float>(v); });
    });

    auto* enhance = new QCheckBox(tr("Enhance Speech"));
    enhance->setChecked(s.enhanceSpeech);
    enhance->setToolTip(tr("Presence and air for intelligibility"));
    f->addRow(enhance);
    connect(enhance, &QCheckBox::toggled, this, [this](bool v) {
        commitSettings(tr("Enhance Speech"), [v](EsSettings& st) { st.enhanceSpeech = v; });
    });
    return box;
}

QWidget* EssentialSoundSection::buildCreative()
{
    QFormLayout* f = nullptr;
    auto* box = esGroup(tr("Creative"), &f);
    const EsSettings& s = resolveClip()->essentialSound();
    auto* rev = new QCheckBox(tr("Reverb"));
    rev->setChecked(s.reverb);
    auto* preset = new QComboBox;
    for (int p = 0; p < static_cast<int>(audiofx::SpaceReverbPreset::Count); ++p)
        preset->addItem(QString::fromUtf8(
            audiofx::spaceReverbPresetName(static_cast<audiofx::SpaceReverbPreset>(p))));
    preset->setCurrentIndex(s.reverbPreset);
    preset->setEnabled(s.reverb);
    f->addRow(rev, preset);
    auto* amt = esAmountSpin(s.reverbAmount);
    amt->setEnabled(s.reverb);
    f->addRow(tr("  Amount:"), amt);
    connect(rev, &QCheckBox::toggled, this, [this](bool v) {
        commitSettings(tr("Reverb"), [v](EsSettings& st) { st.reverb = v; });
    });
    connect(preset, qOverload<int>(&QComboBox::activated), this, [this](int i) {
        commitSettings(tr("Reverb Preset"), [i](EsSettings& st) { st.reverbPreset = i; });
    });
    connect(amt, &ScrubbySpinBox::valueCommitted, this, [this](double, double v) {
        commitSettings(tr("Reverb"), [v](EsSettings& st) { st.reverbAmount = static_cast<float>(v); });
    });
    return box;
}

QWidget* EssentialSoundSection::buildAmbience()
{
    QFormLayout* f = nullptr;
    auto* box = esGroup(tr("Stereo Width"), &f);
    const EsSettings& s = resolveClip()->essentialSound();
    auto* on = new QCheckBox(tr("Widen"));
    on->setChecked(s.stereoWidth);
    auto* amt = esAmountSpin(s.widthAmount);
    amt->setEnabled(s.stereoWidth);
    f->addRow(on, amt);
    connect(on, &QCheckBox::toggled, this, [this](bool v) {
        commitSettings(tr("Stereo Width"), [v](EsSettings& st) { st.stereoWidth = v; });
    });
    connect(amt, &ScrubbySpinBox::valueCommitted, this, [this](double, double v) {
        commitSettings(tr("Stereo Width"), [v](EsSettings& st) { st.widthAmount = static_cast<float>(v); });
    });
    return box;
}

QWidget* EssentialSoundSection::buildDucking()
{
    QFormLayout* f = nullptr;
    auto* box = esGroup(tr("Ducking"), &f);
    auto* note = new QLabel(tr("Lowers this clip wherever a Dialogue clip plays "
                               "(writes volume keyframes, replacing existing ones)."));
    note->setWordWrap(true);
    f->addRow(note);
    auto* reduce = new ScrubbySpinBox;
    reduce->setRange(-40.0, 0.0);
    reduce->setSingleStep(0.5);
    reduce->setScrubStep(0.5);
    reduce->setDecimals(1);
    reduce->setSuffix(tr(" dB"));
    reduce->setValue(-15.0);
    f->addRow(tr("Duck Amount:"), reduce);
    auto* fade = new ScrubbySpinBox;
    fade->setRange(50.0, 3000.0);
    fade->setSingleStep(50.0);
    fade->setScrubStep(10.0);
    fade->setDecimals(0);
    fade->setSuffix(tr(" ms"));
    fade->setValue(800.0);
    f->addRow(tr("Fades:"), fade);
    auto* gen = new QPushButton(tr("Generate Keyframes"));
    f->addRow(QString(), gen);
    connect(gen, &QPushButton::clicked, this, [this, reduce, fade]() {
        generateDucking(static_cast<float>(reduce->value()), static_cast<int>(fade->value()));
    });
    return box;
}

// ── Actions ─────────────────────────────────────────────────────────────────

namespace {

/// One undo step replacing a clip's volume curve.
void esCommitVolume(CommandStack* stack, Timeline* timeline, uint64_t clipId,
                  const KeyframeTrack<float>& before, const KeyframeTrack<float>& after,
                  const std::string& name, const std::function<void()>& notify)
{
    auto apply = [timeline, clipId, notify](const KeyframeTrack<float>& v) {
        if (AudioClip* c = esFindAudioClip(timeline, clipId)) c->volume() = v;
        if (notify) notify();
    };
    auto redo = [apply, after]() { apply(after); };
    auto undo = [apply, before]() { apply(before); };
    if (stack)
        executeTimelineEdit(stack, nullptr, std::make_unique<LambdaCommand>(name, redo, undo));
    else
        redo();
}

float esBaseGain(const AudioClip& c)
{
    const auto& v = c.volume();
    return v.keyframeCount() > 0 ? v.keyframe(0).value : v.defaultValue();
}

} // namespace

void EssentialSoundSection::autoMatchLoudness()
{
    AudioClip* clip = resolveClip();
    if (!clip) return;
    QApplication::setOverrideCursor(Qt::WaitCursor);
    const auto lufs = measureClipLoudness(*clip);
    QApplication::restoreOverrideCursor();
    if (!lufs) {
        QMessageBox::information(this, tr("Auto-Match"),
                                 tr("This clip could not be measured (missing media or silence)."));
        return;
    }
    const float newLin = autoMatchVolume(*lufs, clip->essentialSound().loudnessTarget);
    KeyframeTrack<float> before = clip->volume();
    KeyframeTrack<float> after = before;
    if (after.keyframeCount() == 0) {
        after.setDefaultValue(newLin);
    } else {
        // Keep the keyframe shape (e.g. ducking); move it as a whole.
        const float cur = esBaseGain(*clip);
        const float ratio = cur > 1e-6f ? newLin / cur : newLin;
        for (size_t k = 0; k < after.keyframeCount(); ++k) after.keyframe(k).value *= ratio;
    }
    QPointer<EssentialSoundSection> self(this);
    esCommitVolume(m_commandStack, m_timeline, m_clipId, before, after, "Auto-Match Loudness",
                 [self]() { if (self) emit self->changed(); });
}

void EssentialSoundSection::generateDucking(float duckDb, int fadeMs)
{
    AudioClip* clip = resolveClip();
    if (!clip || !m_timeline) return;

    // Dialogue clips anywhere in the sequence (other than this one).
    std::vector<std::pair<int64_t, int64_t>> dialogue;
    for (size_t ti = 0; ti < m_timeline->trackCount(); ++ti) {
        const Track* tr = m_timeline->track(ti);
        if (!tr || tr->type() != TrackType::Audio || tr->isMuted()) continue;
        for (size_t ci = 0; ci < tr->clipCount(); ++ci) {
            const auto* ac = dynamic_cast<const AudioClip*>(tr->clip(ci));
            if (!ac || ac == clip || !ac->isEnabled()) continue;
            if (ac->essentialSound().type == SoundType::Dialogue)
                dialogue.push_back({ac->timelineIn(), ac->timelineOut()});
        }
    }
    if (dialogue.empty()) {
        QMessageBox::information(this, tr("Ducking"),
                                 tr("No clips are tagged Dialogue. Tag the dialogue clips "
                                    "in Essential Sound first."));
        return;
    }

    const int64_t fade = static_cast<int64_t>(fadeMs) * kTicksPerSecond / 1000;
    const auto ranges = audiofx::computeDuckingRanges(dialogue, 2 * fade, fade);
    const float base = esBaseGain(*clip);
    const float ducked = base * std::pow(10.0f, duckDb / 20.0f);
    const int64_t clipIn = clip->timelineIn();
    const int64_t clipLen = clip->duration();

    KeyframeTrack<float> after(base);
    after.setDefaultValue(base);
    int added = 0;
    for (const auto& [a, b] : ranges) {
        // Range [a, b] already includes the fade ramps on both sides.
        const int64_t la = a - clipIn, lb = b - clipIn;
        if (lb <= 0 || la >= clipLen) continue;
        const int64_t downStart = la, downEnd = la + fade;
        const int64_t upStart = lb - fade, upEnd = lb;
        auto put = [&](int64_t t, float v) {
            if (t < 0 || t > clipLen) return;
            after.addKeyframe(t, v);
            ++added;
        };
        put(downStart, base);
        put(std::max<int64_t>(0, downEnd), ducked);
        put(std::min(clipLen, upStart), ducked);
        put(upEnd, base);
    }
    if (added == 0) {
        QMessageBox::information(this, tr("Ducking"),
                                 tr("No Dialogue clip overlaps this clip."));
        return;
    }
    KeyframeTrack<float> before = clip->volume();
    QPointer<EssentialSoundSection> self(this);
    esCommitVolume(m_commandStack, m_timeline, m_clipId, before, after, "Generate Ducking Keyframes",
                 [self]() { if (self) emit self->changed(); });
}

} // namespace rt
