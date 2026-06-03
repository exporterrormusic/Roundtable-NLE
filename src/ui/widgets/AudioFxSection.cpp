/*
 * AudioFxSection.cpp — see AudioFxSection.h.
 */

#include "widgets/AudioFxSection.h"
#include "widgets/ScrubbySpinBox.h"

#include "timeline/AudioClip.h"
#include "command/CommandStack.h"
#include "command/LambdaCommand.h"

#include "audiofx/FxChain.h"
#include "audiofx/ParametricEQ.h"
#include "audiofx/Dynamics.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFont>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QPointer>
#include <QPushButton>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

namespace rt {

namespace {

/// A labelled ScrubbySpinBox row helper.
ScrubbySpinBox* makeSpin(double lo, double hi, double step, int decimals,
                         double value, const QString& suffix = {})
{
    auto* s = new ScrubbySpinBox;
    s->setRange(lo, hi);
    s->setSingleStep(step);
    s->setScrubStep(step);
    s->setDecimals(decimals);
    if (!suffix.isEmpty()) s->setSuffix(suffix);
    s->setValue(value);
    return s;
}

const char* kBandTypeNames[] = { "Peak", "Low Shelf", "High Shelf" };

audiofx::Biquad::Type bandTypeFromIndex(int i)
{
    switch (i) {
    case 1:  return audiofx::Biquad::Type::LowShelf;
    case 2:  return audiofx::Biquad::Type::HighShelf;
    default: return audiofx::Biquad::Type::Peaking;
    }
}

int bandTypeToIndex(audiofx::Biquad::Type t)
{
    switch (t) {
    case audiofx::Biquad::Type::LowShelf:  return 1;
    case audiofx::Biquad::Type::HighShelf: return 2;
    default:                               return 0;
    }
}

} // namespace

AudioFxSection::AudioFxSection(QWidget* parent)
    : QWidget(parent)
{
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(4);

    // Header row: title + "Add" menu button.
    auto* header = new QHBoxLayout;
    header->addWidget(new QLabel(tr("<b>Audio Effects</b>")));
    header->addStretch();
    auto* addBtn = new QPushButton(tr("Add ▾"));
    addBtn->setToolTip(tr("Add an audio effect to this clip"));
    header->addWidget(addBtn);
    outer->addLayout(header);

    auto* menu = new QMenu(addBtn);
    menu->addAction(tr("Parametric EQ"), this, [this]() {
        commitEdit(tr("Add Parametric EQ"), [](audiofx::FxChain& c) {
            c.add(audiofx::ProcessorKind::ParametricEQ);
        });
    });
    menu->addAction(tr("Dynamics (Gate / Comp / Limiter)"), this, [this]() {
        commitEdit(tr("Add Dynamics"), [](audiofx::FxChain& c) {
            auto* d = static_cast<audiofx::Dynamics*>(c.add(audiofx::ProcessorKind::Dynamics));
            d->loadVoicePreset();
        });
    });
    addBtn->setMenu(menu);

    m_chainLayout = new QVBoxLayout;
    m_chainLayout->setContentsMargins(0, 0, 0, 0);
    m_chainLayout->setSpacing(4);
    outer->addLayout(m_chainLayout);
}

AudioFxSection::~AudioFxSection() = default;

void AudioFxSection::setClip(AudioClip* clip, CommandStack* stack)
{
    m_clip = clip;
    m_commandStack = stack;
    rebuild();
}

void AudioFxSection::commitEdit(const QString& name,
                                const std::function<void(audiofx::FxChain&)>& mutate)
{
    if (!m_clip) return;

    auto before = std::make_shared<audiofx::FxChain>(m_clip->audioFx().clone());
    auto after  = std::make_shared<audiofx::FxChain>(m_clip->audioFx().clone());
    mutate(*after);

    AudioClip* ac = m_clip;
    QPointer<AudioFxSection> self(this);

    auto refresh = [self, ac]() {
        if (!self) return;
        if (self->m_clip == ac && !self->m_rebuildQueued) {
            // Defer: an edit may originate from a child widget's own callback,
            // and rebuild() deletes that widget. Rebuild after the event returns.
            self->m_rebuildQueued = true;
            QTimer::singleShot(0, self, [self]() {
                if (self) { self->m_rebuildQueued = false; self->rebuild(); }
            });
        }
        emit self->changed();
    };

    auto redo = [ac, after, refresh]() { ac->audioFx() = after->clone(); refresh(); };
    auto undo = [ac, before, refresh]() { ac->audioFx() = before->clone(); refresh(); };

    if (m_commandStack)
        m_commandStack->execute(std::make_unique<LambdaCommand>(name.toStdString(), redo, undo));
    else
        redo();
}

void AudioFxSection::rebuild()
{
    // Clear existing per-processor widgets.
    while (QLayoutItem* item = m_chainLayout->takeAt(0)) {
        if (QWidget* w = item->widget()) w->deleteLater();
        delete item;
    }

    if (!m_clip) { setVisible(false); return; }
    setVisible(true);

    const auto& chain = m_clip->audioFx();
    if (chain.empty()) {
        auto* hint = new QLabel(tr("No audio effects. Use “Add ▾”."));
        hint->setEnabled(false);
        m_chainLayout->addWidget(hint);
        return;
    }

    for (int i = 0; i < static_cast<int>(chain.size()); ++i) {
        const auto& p = chain.at(i);
        QWidget* w = nullptr;
        if (dynamic_cast<const audiofx::ParametricEQ*>(&p))   w = buildEqWidget(i);
        else if (dynamic_cast<const audiofx::Dynamics*>(&p))  w = buildDynamicsWidget(i);
        if (w) m_chainLayout->addWidget(w);
    }
}

// ── Per-processor group scaffolding ──────────────────────────────────────────

namespace {

/// Build a processor group box with an enable checkbox + remove button header.
/// Returns the group box and (via out-params) its body form to fill.
QGroupBox* makeProcessorGroup(const QString& title, bool enabled,
                              QWidget* parent, QFormLayout** outForm,
                              QCheckBox** outEnable, QToolButton** outRemove)
{
    auto* box = new QGroupBox(parent);
    auto* v = new QVBoxLayout(box);
    v->setContentsMargins(6, 6, 6, 6);
    v->setSpacing(4);

    auto* head = new QHBoxLayout;
    auto* en = new QCheckBox(title);
    en->setChecked(enabled);
    QFont f = en->font(); f.setBold(true); en->setFont(f);
    head->addWidget(en);
    head->addStretch();
    auto* rm = new QToolButton;
    rm->setText("✕");
    rm->setToolTip(QObject::tr("Remove this effect"));
    head->addWidget(rm);
    v->addLayout(head);

    auto* form = new QFormLayout;
    form->setContentsMargins(0, 0, 0, 0);
    form->setSpacing(3);
    v->addLayout(form);

    *outForm = form;
    *outEnable = en;
    *outRemove = rm;
    return box;
}

} // namespace

QWidget* AudioFxSection::buildEqWidget(int index)
{
    auto* eq = dynamic_cast<audiofx::ParametricEQ*>(&m_clip->audioFx().at(index));
    if (!eq) return nullptr;

    QFormLayout* form = nullptr; QCheckBox* enable = nullptr; QToolButton* remove = nullptr;
    auto* box = makeProcessorGroup(tr("Parametric EQ"), eq->isEnabled(),
                                   this, &form, &enable, &remove);

    connect(enable, &QCheckBox::toggled, this, [this, index](bool on) {
        commitEdit(tr("Toggle EQ"), [index, on](audiofx::FxChain& c) {
            c.at(index).setEnabled(on);
        });
    });
    connect(remove, &QToolButton::clicked, this, [this, index]() {
        commitEdit(tr("Remove EQ"), [index](audiofx::FxChain& c) { c.remove(index); });
    });

    // ── Low cut (HPF) ──
    {
        auto* row = new QHBoxLayout;
        auto* on = new QCheckBox(tr("On"));
        on->setChecked(eq->highPassOn());
        auto* freq = makeSpin(20, 2000, 1, 0, eq->highPassFreq(), tr(" Hz"));
        row->addWidget(on); row->addWidget(freq);
        auto* host = new QWidget; host->setLayout(row); row->setContentsMargins(0,0,0,0);
        form->addRow(tr("Low Cut:"), host);

        connect(on, &QCheckBox::toggled, this, [this, index, freq](bool b) {
            const float f = static_cast<float>(freq->value());
            commitEdit(tr("EQ Low Cut"), [index, b, f](audiofx::FxChain& c) {
                static_cast<audiofx::ParametricEQ&>(c.at(index)).setHighPass(b, f, 0.707f);
            });
        });
        connect(freq, &ScrubbySpinBox::valueCommitted, this, [this, index, on](double, double nv) {
            const bool b = on->isChecked();
            commitEdit(tr("EQ Low Cut Freq"), [index, b, nv](audiofx::FxChain& c) {
                static_cast<audiofx::ParametricEQ&>(c.at(index)).setHighPass(b, static_cast<float>(nv), 0.707f);
            });
        });
    }

    // ── Three bell/shelf bands ──
    for (int b = 0; b < 3; ++b) {
        const auto& band = eq->band(b);
        auto* row = new QHBoxLayout;
        row->setContentsMargins(0, 0, 0, 0);
        auto* on = new QCheckBox;
        on->setChecked(band.enabled);
        auto* type = new QComboBox;
        for (auto* n : kBandTypeNames) type->addItem(n);
        type->setCurrentIndex(bandTypeToIndex(band.type));
        auto* freq = makeSpin(20, 20000, 1, 0, band.freqHz, tr(" Hz"));
        auto* gain = makeSpin(-24, 24, 0.1, 1, band.gainDb, tr(" dB"));
        auto* q    = makeSpin(0.1, 10, 0.05, 2, band.q, tr(" Q"));
        row->addWidget(on); row->addWidget(type); row->addWidget(freq);
        row->addWidget(gain); row->addWidget(q);
        auto* host = new QWidget; host->setLayout(row);
        form->addRow(tr("Band %1:").arg(b + 1), host);

        // All controls for a band write the whole band struct atomically.
        auto applyBand = [this, index, b, on, type, freq, gain, q](const QString& label) {
            audiofx::ParametricEQ::Band nb;
            nb.enabled = on->isChecked();
            nb.type    = bandTypeFromIndex(type->currentIndex());
            nb.freqHz  = static_cast<float>(freq->value());
            nb.gainDb  = static_cast<float>(gain->value());
            nb.q       = static_cast<float>(q->value());
            commitEdit(label, [index, b, nb](audiofx::FxChain& c) {
                static_cast<audiofx::ParametricEQ&>(c.at(index)).setBand(b, nb);
            });
        };
        connect(on,   &QCheckBox::toggled, this, [applyBand](bool) { applyBand(tr("EQ Band")); });
        connect(type, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
                [applyBand](int) { applyBand(tr("EQ Band Type")); });
        connect(freq, &ScrubbySpinBox::valueCommitted, this, [applyBand](double, double) { applyBand(tr("EQ Band Freq")); });
        connect(gain, &ScrubbySpinBox::valueCommitted, this, [applyBand](double, double) { applyBand(tr("EQ Band Gain")); });
        connect(q,    &ScrubbySpinBox::valueCommitted, this, [applyBand](double, double) { applyBand(tr("EQ Band Q")); });
    }

    // ── High cut (LPF) ──
    {
        auto* row = new QHBoxLayout; row->setContentsMargins(0,0,0,0);
        auto* on = new QCheckBox(tr("On"));
        on->setChecked(eq->lowPassOn());
        auto* freq = makeSpin(1000, 22000, 1, 0, eq->lowPassFreq(), tr(" Hz"));
        row->addWidget(on); row->addWidget(freq);
        auto* host = new QWidget; host->setLayout(row);
        form->addRow(tr("High Cut:"), host);

        connect(on, &QCheckBox::toggled, this, [this, index, freq](bool b) {
            const float f = static_cast<float>(freq->value());
            commitEdit(tr("EQ High Cut"), [index, b, f](audiofx::FxChain& c) {
                static_cast<audiofx::ParametricEQ&>(c.at(index)).setLowPass(b, f, 0.707f);
            });
        });
        connect(freq, &ScrubbySpinBox::valueCommitted, this, [this, index, on](double, double nv) {
            const bool b = on->isChecked();
            commitEdit(tr("EQ High Cut Freq"), [index, b, nv](audiofx::FxChain& c) {
                static_cast<audiofx::ParametricEQ&>(c.at(index)).setLowPass(b, static_cast<float>(nv), 0.707f);
            });
        });
    }

    // ── Output gain ──
    {
        auto* gain = makeSpin(-24, 24, 0.1, 1, eq->outputGainDb(), tr(" dB"));
        form->addRow(tr("Output:"), gain);
        connect(gain, &ScrubbySpinBox::valueCommitted, this, [this, index](double, double nv) {
            commitEdit(tr("EQ Output Gain"), [index, nv](audiofx::FxChain& c) {
                static_cast<audiofx::ParametricEQ&>(c.at(index)).setOutputGainDb(static_cast<float>(nv));
            });
        });
    }

    return box;
}

QWidget* AudioFxSection::buildDynamicsWidget(int index)
{
    auto* dyn = dynamic_cast<audiofx::Dynamics*>(&m_clip->audioFx().at(index));
    if (!dyn) return nullptr;

    QFormLayout* form = nullptr; QCheckBox* enable = nullptr; QToolButton* remove = nullptr;
    auto* box = makeProcessorGroup(tr("Dynamics"), dyn->isEnabled(),
                                   this, &form, &enable, &remove);

    connect(enable, &QCheckBox::toggled, this, [this, index](bool on) {
        commitEdit(tr("Toggle Dynamics"), [index, on](audiofx::FxChain& c) {
            c.at(index).setEnabled(on);
        });
    });
    connect(remove, &QToolButton::clicked, this, [this, index]() {
        commitEdit(tr("Remove Dynamics"), [index](audiofx::FxChain& c) { c.remove(index); });
    });

    // ── Gate ──
    {
        const auto& g = dyn->gate();
        auto* on  = new QCheckBox(tr("Gate"));      on->setChecked(g.enabled);
        auto* thr = makeSpin(-80, 0, 0.5, 1, g.thresholdDb, tr(" dB"));
        auto* rat = makeSpin(1, 20, 0.1, 1, g.ratio, tr(":1"));
        auto* rng = makeSpin(-90, 0, 1, 0, g.rangeDb, tr(" dB"));
        auto* att = makeSpin(0.1, 200, 0.1, 1, g.attackMs, tr(" ms"));
        auto* rel = makeSpin(1, 1000, 1, 0, g.releaseMs, tr(" ms"));
        form->addRow(on, new QLabel(tr("(noise gate)")));
        form->addRow(tr("  Threshold:"), thr);
        form->addRow(tr("  Ratio:"), rat);
        form->addRow(tr("  Range:"), rng);
        form->addRow(tr("  Attack:"), att);
        form->addRow(tr("  Release:"), rel);

        auto apply = [this, index, on, thr, rat, rng, att, rel](const QString& label) {
            audiofx::Dynamics::GateSettings s;
            s.enabled = on->isChecked();
            s.thresholdDb = static_cast<float>(thr->value());
            s.ratio = static_cast<float>(rat->value());
            s.rangeDb = static_cast<float>(rng->value());
            s.attackMs = static_cast<float>(att->value());
            s.releaseMs = static_cast<float>(rel->value());
            commitEdit(label, [index, s](audiofx::FxChain& c) {
                static_cast<audiofx::Dynamics&>(c.at(index)).setGate(s);
            });
        };
        connect(on, &QCheckBox::toggled, this, [apply](bool) { apply(tr("Gate")); });
        for (auto* s : { thr, rat, rng, att, rel })
            connect(s, &ScrubbySpinBox::valueCommitted, this, [apply](double, double) { apply(tr("Gate Param")); });
    }

    // ── Compressor ──
    {
        const auto& cm = dyn->compressor();
        auto* on  = new QCheckBox(tr("Compressor")); on->setChecked(cm.enabled);
        auto* thr = makeSpin(-60, 0, 0.5, 1, cm.thresholdDb, tr(" dB"));
        auto* rat = makeSpin(1, 20, 0.1, 1, cm.ratio, tr(":1"));
        auto* kne = makeSpin(0, 24, 0.5, 1, cm.kneeDb, tr(" dB"));
        auto* mk  = makeSpin(0, 24, 0.1, 1, cm.makeupDb, tr(" dB"));
        auto* att = makeSpin(0.1, 200, 0.1, 1, cm.attackMs, tr(" ms"));
        auto* rel = makeSpin(1, 1000, 1, 0, cm.releaseMs, tr(" ms"));
        form->addRow(on, new QLabel(tr("(compressor)")));
        form->addRow(tr("  Threshold:"), thr);
        form->addRow(tr("  Ratio:"), rat);
        form->addRow(tr("  Knee:"), kne);
        form->addRow(tr("  Makeup:"), mk);
        form->addRow(tr("  Attack:"), att);
        form->addRow(tr("  Release:"), rel);

        auto apply = [this, index, on, thr, rat, kne, mk, att, rel](const QString& label) {
            audiofx::Dynamics::CompSettings s;
            s.enabled = on->isChecked();
            s.thresholdDb = static_cast<float>(thr->value());
            s.ratio = static_cast<float>(rat->value());
            s.kneeDb = static_cast<float>(kne->value());
            s.makeupDb = static_cast<float>(mk->value());
            s.attackMs = static_cast<float>(att->value());
            s.releaseMs = static_cast<float>(rel->value());
            commitEdit(label, [index, s](audiofx::FxChain& c) {
                static_cast<audiofx::Dynamics&>(c.at(index)).setCompressor(s);
            });
        };
        connect(on, &QCheckBox::toggled, this, [apply](bool) { apply(tr("Compressor")); });
        for (auto* s : { thr, rat, kne, mk, att, rel })
            connect(s, &ScrubbySpinBox::valueCommitted, this, [apply](double, double) { apply(tr("Compressor Param")); });
    }

    // ── Limiter ──
    {
        const auto& lm = dyn->limiter();
        auto* on  = new QCheckBox(tr("Limiter")); on->setChecked(lm.enabled);
        auto* ceil = makeSpin(-12, 0, 0.1, 1, lm.ceilingDb, tr(" dB"));
        auto* rel  = makeSpin(1, 500, 1, 0, lm.releaseMs, tr(" ms"));
        form->addRow(on, new QLabel(tr("(brick-wall)")));
        form->addRow(tr("  Ceiling:"), ceil);
        form->addRow(tr("  Release:"), rel);

        auto apply = [this, index, on, ceil, rel](const QString& label) {
            audiofx::Dynamics::LimiterSettings s;
            s.enabled = on->isChecked();
            s.ceilingDb = static_cast<float>(ceil->value());
            s.releaseMs = static_cast<float>(rel->value());
            commitEdit(label, [index, s](audiofx::FxChain& c) {
                static_cast<audiofx::Dynamics&>(c.at(index)).setLimiter(s);
            });
        };
        connect(on, &QCheckBox::toggled, this, [apply](bool) { apply(tr("Limiter")); });
        for (auto* s : { ceil, rel })
            connect(s, &ScrubbySpinBox::valueCommitted, this, [apply](double, double) { apply(tr("Limiter Param")); });
    }

    return box;
}

} // namespace rt
