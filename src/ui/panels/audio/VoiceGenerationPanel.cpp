#include "panels/audio/VoiceGenerationPanel.h"

#include "panels/audio/AudioSync.h"
#include "panels/audio/VoiceGenerationService.h"
#include "panels/audio/VoiceProviders.h"
#include "panels/audio/VoiceReferenceLibrary.h"
#include "Theme.h"
#include "widgets/MiniWaveformWidget.h"

#include <QAbstractItemView>
#include <QComboBox>
#include <QDesktopServices>
#include <QDir>
#include <QDoubleSpinBox>
#include <QDrag>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QMimeData>
#include <QPushButton>
#include <QSet>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTextEdit>
#include <QTimer>
#include <QUrl>
#include <QUuid>
#include <QVBoxLayout>

#include <algorithm>

namespace rt {

namespace {

// AudioSync emits change signals on every clip confirm; they are coalesced
// into one rebuild per burst.
constexpr int kRefreshCoalesceMs = 60;

class GeneratedAudioList final : public QListWidget
{
public:
    using QListWidget::QListWidget;

protected:
    void startDrag(Qt::DropActions) override
    {
        const auto selected = selectedItems();
        if (selected.isEmpty()) return;
        QList<QUrl> urls;
        for (auto* item : selected) {
            const QString path = item->data(Qt::UserRole).toString();
            if (!path.isEmpty()) urls.push_back(QUrl::fromLocalFile(path));
        }
        if (urls.isEmpty()) return;
        auto* mime = new QMimeData;
        mime->setUrls(urls);
        auto* drag = new QDrag(this);
        drag->setMimeData(mime);
        drag->exec(Qt::CopyAction);
    }
};

double referenceTargetSeconds(const QString& provider)
{
    const auto* info = findVoiceProvider(provider);
    return info ? info->referenceSeconds : 20.0;
}

struct AutomaticReferencePlan
{
    QList<VoiceReferenceSegment> segments;
    double duration{0.0};
    int trackCount{0};
    bool fromSavedLibrary{false};
};

/// Highest-confidence approved clips for `character` until the engine's
/// reference target is reached; otherwise the newest saved reference.
AutomaticReferencePlan planAutomaticReference(
    QVector<VoiceReferenceCandidate> candidates,
    const QString& character, const QString& provider)
{
    AutomaticReferencePlan plan;
    if (character.trimmed().isEmpty()) return plan;

    std::stable_sort(candidates.begin(), candidates.end(),
        [](const auto& left, const auto& right) {
            if (left.confidence != right.confidence)
                return left.confidence > right.confidence;
            return (left.end - left.start) > (right.end - right.start);
        });
    const double target = referenceTargetSeconds(provider);
    QSet<QString> tracks;
    for (const auto& candidate : candidates) {
        if (candidate.character.compare(character, Qt::CaseInsensitive) != 0)
            continue;
        const double clipDuration = candidate.end - candidate.start;
        if (clipDuration < 0.35 || candidate.transcript.trimmed().isEmpty()) continue;
        plan.segments.push_back({candidate.sourceFile, candidate.transcript,
                                 candidate.start, candidate.end});
        plan.duration += clipDuration;
        tracks.insert(candidate.sourceFile);
        if (plan.duration >= target) break;
    }
    if (plan.segments.isEmpty()) {
        if (const auto saved = VoiceReferenceLibrary::newestFor(character)) {
            plan.segments.push_back({saved->path, saved->transcript, 0.0, 0.0});
            plan.duration = saved->duration;
            plan.fromSavedLibrary = true;
            tracks.insert(saved->path);
        }
    }
    plan.trackCount = static_cast<int>(tracks.size());
    return plan;
}

} // namespace

VoiceGenerationPanel::VoiceGenerationPanel(VoiceGenerationService* service,
                                           QWidget* parent)
    : QWidget(parent), m_service(service)
{
    buildUi();
    m_refreshTimer = new QTimer(this);
    m_refreshTimer->setSingleShot(true);
    m_refreshTimer->setInterval(kRefreshCoalesceMs);
    connect(m_refreshTimer, &QTimer::timeout,
            this, &VoiceGenerationPanel::refreshFromAudioSync);

    if (m_service) {
        connect(m_service, &VoiceGenerationService::statusChanged,
                m_status, &QLabel::setText);
        connect(m_service, &VoiceGenerationService::busyChanged,
                this, [this](bool busy) {
            refreshGenerateAvailability();
            m_cancel->setEnabled(busy);
        });
        connect(m_service, &VoiceGenerationService::modelResidentChanged,
                m_unloadModel, &QPushButton::setEnabled);
        connect(m_service, &VoiceGenerationService::generationFinished,
                this, &VoiceGenerationPanel::onFinished);
        connect(m_service, &VoiceGenerationService::generationFailed,
                this, &VoiceGenerationPanel::onFailed);
    }
    m_unloadModel->setEnabled(m_service && m_service->isModelResident());
    refreshProviderState();
}

void VoiceGenerationPanel::buildUi()
{
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(10, 10, 10, 10);
    root->setSpacing(12);

    const QString sectionStyle = QStringLiteral(
        "QGroupBox { margin-top: 12px; padding-top: 8px; }"
        "QGroupBox::title { subcontrol-origin: margin; left: 10px; padding: 0 5px; }");

    auto* title = new QLabel(tr("Generate Voice Clip"), this);
    QFont titleFont = title->font();
    titleFont.setPointSize(titleFont.pointSize() + 2);
    titleFont.setBold(true);
    title->setFont(titleFont);
    root->addWidget(title);

    auto* form = new QFormLayout;
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    m_provider = new QComboBox(this);
    for (const auto& provider : voiceProviders())
        m_provider->addItem(provider.menuLabel, provider.key);
    form->addRow(tr("Engine"), m_provider);

    auto* engineState = new QWidget(this);
    auto* engineStateLayout = new QHBoxLayout(engineState);
    engineStateLayout->setContentsMargins(0, 0, 0, 0);
    engineStateLayout->setSpacing(8);
    m_engineStatus = new QLabel(engineState);
    m_engineStatus->setWordWrap(true);
    m_locateBreeze = new QPushButton(tr("Locate Existing Breeze..."), engineState);
    m_locateBreeze->setToolTip(tr(
        "Select the SPEECH-TEXT-SPEECH folder that already contains Breeze-TTS-2."));
    engineStateLayout->addWidget(m_engineStatus, 1);
    engineStateLayout->addWidget(m_locateBreeze);
    form->addRow(QString(), engineState);

    m_character = new QComboBox(this);
    m_character->setEditable(true);
    form->addRow(tr("Character"), m_character);

    auto* automaticReference = new QWidget(this);
    auto* automaticLayout = new QHBoxLayout(automaticReference);
    automaticLayout->setContentsMargins(0, 0, 0, 0);
    automaticLayout->setSpacing(6);
    m_autoReferenceSummary = new QLabel(
        tr("Approved clips will be selected automatically."), automaticReference);
    m_autoReferenceSummary->setWordWrap(true);
    m_saveReference = new QPushButton(tr("Save Approved..."), automaticReference);
    m_saveReference->setToolTip(tr(
        "Combine every confirmed clip for this character into a reusable reference "
        "(lossless FLAC) available in every project."));
    automaticLayout->addWidget(m_autoReferenceSummary, 1);
    automaticLayout->addWidget(m_saveReference);
    form->addRow(tr("Reference"), automaticReference);
    root->addLayout(form);

    m_manualReference = new QGroupBox(
        tr("Use a different voice reference (advanced)"), this);
    m_manualReference->setCheckable(true);
    m_manualReference->setChecked(false);
    m_manualReference->setToolTip(
        tr("Use this only when the approved automatic reference has a problem."));
    m_manualReference->setStyleSheet(sectionStyle);
    auto* manualLayout = new QVBoxLayout(m_manualReference);
    manualLayout->setContentsMargins(12, 20, 12, 12);
    manualLayout->setSpacing(8);
    m_manualReferenceContent = new QWidget(m_manualReference);
    auto* manualContentLayout = new QVBoxLayout(m_manualReferenceContent);
    manualContentLayout->setContentsMargins(0, 0, 0, 0);
    manualContentLayout->setSpacing(8);
    auto* manualForm = new QFormLayout;
    m_reference = new QComboBox(m_manualReference);
    m_reference->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    manualForm->addRow(tr("Imported track"), m_reference);
    m_referenceText = new QLineEdit(m_manualReference);
    m_referenceText->setPlaceholderText(
        tr("Auto-filled from transcription; correct it here if needed"));
    manualForm->addRow(tr("Transcript"), m_referenceText);
    auto* range = new QWidget(m_manualReference);
    auto* rangeLayout = new QHBoxLayout(range);
    rangeLayout->setContentsMargins(0, 0, 0, 0);
    m_referenceStart = new QDoubleSpinBox(range);
    m_referenceEnd = new QDoubleSpinBox(range);
    for (auto* spin : {m_referenceStart, m_referenceEnd}) {
        spin->setRange(0.0, 24.0 * 60.0 * 60.0);
        spin->setDecimals(2);
        spin->setSuffix(tr(" sec"));
    }
    rangeLayout->addWidget(new QLabel(tr("In"), range));
    rangeLayout->addWidget(m_referenceStart);
    rangeLayout->addWidget(new QLabel(tr("Out"), range));
    rangeLayout->addWidget(m_referenceEnd);
    manualForm->addRow(tr("Section"), range);
    manualContentLayout->addLayout(manualForm);
    m_referenceWaveform = new MiniWaveformWidget(m_manualReference);
    m_referenceWaveform->setMinimumHeight(58);
    manualContentLayout->addWidget(m_referenceWaveform);
    manualLayout->addWidget(m_manualReferenceContent);
    m_manualReferenceContent->setVisible(false);
    m_manualReference->setFlat(true);
    m_manualReference->setMaximumHeight(
        m_manualReference->fontMetrics().height() + 14);
    root->addWidget(m_manualReference);

    auto* promptTitle = new QLabel(tr("Text to generate"), this);
    QFont promptFont = promptTitle->font();
    promptFont.setBold(true);
    promptTitle->setFont(promptFont);
    root->addWidget(promptTitle);

    // Shows which script line the text came from; approval attaches the
    // clip to exactly that line instead of guessing by text similarity.
    m_scriptLink = new QWidget(this);
    auto* scriptLinkLayout = new QHBoxLayout(m_scriptLink);
    scriptLinkLayout->setContentsMargins(0, 0, 0, 0);
    scriptLinkLayout->setSpacing(6);
    m_scriptLinkLabel = new QLabel(m_scriptLink);
    m_scriptLinkLabel->setWordWrap(true);
    auto* unlink = new QPushButton(tr("Unlink"), m_scriptLink);
    unlink->setToolTip(tr("Stop tying this clip to the script line; approval will "
                          "match it by text instead."));
    scriptLinkLayout->addWidget(m_scriptLinkLabel, 1);
    scriptLinkLayout->addWidget(unlink);
    m_scriptLink->setVisible(false);
    root->addWidget(m_scriptLink);
    connect(unlink, &QPushButton::clicked, this, &VoiceGenerationPanel::clearScriptLine);

    m_text = new QTextEdit(this);
    m_text->setPlaceholderText(tr(
        "Type what the character should say, or right-click a script line "
        "and choose \"Generate voice for this line\"."));
    m_text->setAcceptRichText(false);
    m_text->setMinimumHeight(88);
    m_text->setMaximumHeight(110);
    root->addWidget(m_text);

    auto* options = new QHBoxLayout;
    m_speed = new QDoubleSpinBox(this);
    m_speed->setRange(0.5, 2.0);
    m_speed->setSingleStep(0.05);
    m_speed->setValue(1.0);
    m_speed->setSuffix(QStringLiteral("x"));
    m_duration = new QDoubleSpinBox(this);
    m_duration->setRange(0.0, 120.0);
    m_duration->setSpecialValueText(tr("Auto"));
    m_duration->setSuffix(tr(" sec"));
    m_seed = new QSpinBox(this);
    m_seed->setRange(0, 999999999);
    m_seed->setValue(42);
    m_seed->setToolTip(tr("Same seed + same inputs gives the same take. Change it "
                          "for a different delivery."));
    options->addWidget(new QLabel(tr("Speed"), this));
    options->addWidget(m_speed);
    options->addWidget(new QLabel(tr("Duration"), this));
    options->addWidget(m_duration);
    options->addWidget(new QLabel(tr("Seed"), this));
    options->addWidget(m_seed);
    root->addLayout(options);

    auto* actions = new QHBoxLayout;
    m_generate = new QPushButton(tr("Generate Draft"), this);
    m_generate->setDefault(true);
    m_cancel = new QPushButton(tr("Cancel"), this);
    m_cancel->setEnabled(false);
    actions->addWidget(m_generate, 1);
    actions->addWidget(m_cancel);
    root->addLayout(actions);

    auto* approval = new QGroupBox(tr("Audition && Approve"), this);
    approval->setStyleSheet(sectionStyle);
    auto* approvalLayout = new QVBoxLayout(approval);
    approvalLayout->setContentsMargins(12, 20, 12, 12);
    approvalLayout->setSpacing(8);
    auto* auditionActions = new QHBoxLayout;
    m_listen = new QPushButton(tr("▶ Listen"), approval);
    m_discard = new QPushButton(tr("Discard"), approval);
    m_listen->setEnabled(false);
    m_discard->setEnabled(false);
    auditionActions->addWidget(m_listen, 1);
    auditionActions->addWidget(m_discard);
    approvalLayout->addLayout(auditionActions);
    auto* approveActions = new QHBoxLayout;
    m_approveSync = new QPushButton(tr("Approve && Sync to Script"), approval);
    m_approveImport = new QPushButton(tr("Approve && Import Only"), approval);
    m_approveSync->setToolTip(tr(
        "Save beside the source audio, import into Project Bin, and match it to a script line for this character."));
    m_approveImport->setToolTip(tr(
        "Save beside the source audio and import into Project Bin without changing script matches."));
    m_approveSync->setEnabled(false);
    m_approveImport->setEnabled(false);
    approveActions->addWidget(m_approveSync, 1);
    approveActions->addWidget(m_approveImport, 1);
    approvalLayout->addLayout(approveActions);
    root->addWidget(approval);

    m_status = new QLabel(tr("Select a voice reference or use an automatic voice."), this);
    m_status->setWordWrap(true);
    root->addWidget(m_status);

    auto* recentGroup = new QGroupBox(tr("Approved Generated Clips"), this);
    recentGroup->setStyleSheet(sectionStyle);
    auto* recentLayout = new QVBoxLayout(recentGroup);
    recentLayout->setContentsMargins(12, 20, 12, 12);
    recentLayout->setSpacing(8);
    m_recent = new GeneratedAudioList(recentGroup);
    m_recent->setDragEnabled(true);
    m_recent->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_recent->setMinimumHeight(90);
    m_recent->setMaximumHeight(140);
    recentLayout->addWidget(m_recent);
    root->addWidget(recentGroup);

    m_unloadModel = new QPushButton(tr("Unload Model / Free VRAM"), this);
    m_unloadModel->setToolTip(tr(
        "Stop the local voice worker and release the model's GPU memory."));
    m_unloadModel->setEnabled(false);
    root->addWidget(m_unloadModel);

    connect(m_generate, &QPushButton::clicked, this, &VoiceGenerationPanel::generate);
    connect(m_text, &QTextEdit::textChanged,
            this, &VoiceGenerationPanel::refreshGenerateAvailability);
    connect(m_locateBreeze, &QPushButton::clicked, this, [this]() {
        QString initial = VoiceGenerationService::breezeInstallationRoot();
        if (initial.isEmpty()) initial = QDir::homePath();
        const QString folder = QFileDialog::getExistingDirectory(
            this, tr("Locate Breeze-TTS-2 Installation"), initial);
        if (folder.isEmpty()) return;
        QString error;
        if (!VoiceGenerationService::configureBreezeInstallation(folder, &error)) {
            QMessageBox::warning(this, tr("Breeze-TTS-2 Not Found"), error);
            return;
        }
        refreshProviderState();
    });
    connect(m_unloadModel, &QPushButton::clicked, this, [this]() {
        if (m_draftAuditionTimer) m_draftAuditionTimer->stop();
        if (m_audioSync) m_audioSync->stopVoiceDraftAudition();
        if (m_service) m_service->unloadModel();
    });
    connect(m_listen, &QPushButton::clicked, this, &VoiceGenerationPanel::listenToDraft);
    connect(m_approveSync, &QPushButton::clicked,
            this, [this]() { approveDraft(true); });
    connect(m_approveImport, &QPushButton::clicked,
            this, [this]() { approveDraft(false); });
    connect(m_discard, &QPushButton::clicked, this, [this]() {
        clearDraft(true);
        m_status->setText(tr("Draft discarded."));
    });
    connect(m_saveReference, &QPushButton::clicked,
            this, &VoiceGenerationPanel::saveApprovedReference);
    connect(m_cancel, &QPushButton::clicked, this, [this]() {
        if (m_service) m_service->cancel();
    });
    connect(m_provider, &QComboBox::currentIndexChanged,
            this, [this]() {
                refreshProviderState();
                refreshManualTrack();
                refreshReferencePlan();
            });
    connect(m_character, &QComboBox::currentTextChanged, this, [this](const QString& text) {
        // A line belongs to one character; picking someone else unlinks it.
        if (m_selectedScriptLine >= 0
            && text.trimmed().compare(m_selectedScriptCharacter, Qt::CaseInsensitive) != 0)
            clearScriptLine();
        refreshReferencePlan();
    });
    connect(m_reference, &QComboBox::currentIndexChanged,
            this, &VoiceGenerationPanel::refreshManualTrack);
    connect(m_manualReference, &QGroupBox::toggled, this, [this](bool checked) {
        m_manualReferenceContent->setVisible(checked);
        m_manualReference->setFlat(!checked);
        m_manualReference->setMaximumHeight(checked
            ? QWIDGETSIZE_MAX
            : m_manualReference->fontMetrics().height() + 14);
        m_manualReference->updateGeometry();
        refreshReferencePlan();
    });
    connect(m_referenceStart, &QDoubleSpinBox::valueChanged, this, [this](double start) {
        if (start >= m_referenceEnd->value()) {
            QSignalBlocker blocker(m_referenceEnd);
            m_referenceEnd->setValue(start + 0.1);
        }
        m_referenceWaveform->setTrimRange(start, m_referenceEnd->value());
        refreshManualTranscript();
    });
    connect(m_referenceEnd, &QDoubleSpinBox::valueChanged, this, [this](double end) {
        if (end <= m_referenceStart->value()) {
            QSignalBlocker blocker(m_referenceStart);
            m_referenceStart->setValue(std::max(0.0, end - 0.1));
        }
        m_referenceWaveform->setTrimRange(m_referenceStart->value(), end);
        refreshManualTranscript();
    });
    connect(m_referenceWaveform, &MiniWaveformWidget::trimChanging,
            this, [this](double start, double end) {
        const QSignalBlocker startBlocker(m_referenceStart);
        const QSignalBlocker endBlocker(m_referenceEnd);
        m_referenceStart->setValue(start);
        m_referenceEnd->setValue(end);
        refreshManualTranscript();
    });
    connect(m_referenceWaveform, &MiniWaveformWidget::trimChanged,
            this, [this](double start, double end) {
        const QSignalBlocker startBlocker(m_referenceStart);
        const QSignalBlocker endBlocker(m_referenceEnd);
        m_referenceStart->setValue(start);
        m_referenceEnd->setValue(end);
    });
    connect(m_recent, &QListWidget::itemDoubleClicked, this, [](QListWidgetItem* item) {
        if (!item) return;
        QDesktopServices::openUrl(QUrl::fromLocalFile(
            QFileInfo(item->data(Qt::UserRole).toString()).absolutePath()));
    });
}

void VoiceGenerationPanel::setAudioSync(AudioSync* audioSync)
{
    if (m_audioSync == audioSync) return;
    if (m_audioSync) disconnect(m_audioSync, nullptr, this, nullptr);
    m_audioSync = audioSync;
    if (m_audioSync) {
        connect(m_audioSync, &AudioSync::voiceContextChanged,
                this, &VoiceGenerationPanel::scheduleRefresh);
        connect(m_audioSync, &AudioSync::scriptLoaded,
                this, &VoiceGenerationPanel::scheduleRefresh);
        connect(m_audioSync, &AudioSync::audioImported,
                this, &VoiceGenerationPanel::scheduleRefresh);
        connect(m_audioSync, &AudioSync::syncCompleted,
                this, &VoiceGenerationPanel::scheduleRefresh);
        connect(m_audioSync, &AudioSync::voiceLineRequested,
                this, &VoiceGenerationPanel::setScriptLine);
    }
    refreshFromAudioSync();
}

void VoiceGenerationPanel::scheduleRefresh()
{
    m_refreshPending = true;
    // A hidden panel (inactive rail page, closed dock) catches up when shown.
    if (isVisible()) m_refreshTimer->start();
}

void VoiceGenerationPanel::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    if (m_refreshPending) refreshFromAudioSync();
}

QStringList VoiceGenerationPanel::availableCharacters()
{
    if (m_refreshPending) refreshFromAudioSync();
    QStringList result;
    for (int index = 0; index < m_character->count(); ++index)
        result.append(m_character->itemText(index));
    return result;
}

QString VoiceGenerationPanel::currentText() const
{
    return m_text->toPlainText();
}

QString VoiceGenerationPanel::currentCharacter() const
{
    return m_character->currentText();
}

QString VoiceGenerationPanel::currentProvider() const
{
    return m_provider->currentData().toString();
}

QVector<VoiceReferenceCandidate> VoiceGenerationPanel::approvedCandidates() const
{
    return m_audioSync ? m_audioSync->voiceReferenceCandidates()
                       : QVector<VoiceReferenceCandidate>{};
}

void VoiceGenerationPanel::refreshFromAudioSync()
{
    m_refreshPending = false;
    m_refreshTimer->stop();

    const QString currentCharacterText = m_character->currentText();
    const QString currentReference = m_reference->currentData().toMap()
                                         .value(QStringLiteral("path")).toString();
    {
        // Rebuild silently; the dependent refreshes run once below instead
        // of once per inserted item.
        const QSignalBlocker characterBlocker(m_character);
        const QSignalBlocker referenceBlocker(m_reference);
        m_character->clear();
        m_reference->clear();

        if (m_audioSync) {
            m_character->addItems(m_audioSync->scriptCharacters());
            for (const auto& track : m_audioSync->voiceImportedAudioTracks()) {
                const QString label = tr("%1  ·  %2s  ·  %3 approved")
                    .arg(track.displayName).arg(track.duration, 0, 'f', 1)
                    .arg(track.approvedClipCount);
                m_reference->addItem(label, QVariantMap{
                    {QStringLiteral("path"), track.sourceFile},
                    {QStringLiteral("duration"), track.duration},
                    {QStringLiteral("library"), false}
                });
            }
        }

        const auto libraryReferences = VoiceReferenceLibrary::list();
        if (!libraryReferences.isEmpty() && m_reference->count() > 0)
            m_reference->insertSeparator(m_reference->count());
        for (const auto& reference : libraryReferences) {
            m_reference->addItem(tr("Saved: %1 — %2")
                .arg(reference.character, QFileInfo(reference.path).fileName()), QVariantMap{
                    {QStringLiteral("path"), reference.path},
                    {QStringLiteral("text"), reference.transcript},
                    {QStringLiteral("character"), reference.character},
                    {QStringLiteral("duration"), reference.duration},
                    {QStringLiteral("library"), true}
                });
        }

        const int previousCharacter = m_character->findText(
            currentCharacterText, Qt::MatchFixedString);
        if (previousCharacter >= 0)
            m_character->setCurrentIndex(previousCharacter);
        else if (!currentCharacterText.isEmpty())
            m_character->setCurrentText(currentCharacterText);
        else if (m_character->count() > 0)
            m_character->setCurrentIndex(0);
        for (int index = 0; index < m_reference->count(); ++index) {
            if (m_reference->itemData(index).toMap().value(QStringLiteral("path")).toString()
                    == currentReference) {
                m_reference->setCurrentIndex(index);
                break;
            }
        }
    }
    refreshManualTrack();
    refreshReferencePlan();
}

void VoiceGenerationPanel::setScriptLine(int lineNumber, const QString& character,
                                         const QString& dialogue, const QString& segment)
{
    {
        const QSignalBlocker blocker(m_character);
        m_character->setCurrentText(character);
    }
    m_text->setPlainText(dialogue);
    m_selectedScriptLine = lineNumber;
    m_selectedScriptCharacter = character.trimmed();
    m_selectedScriptSegment = segment;
    refreshScriptLink();
    refreshReferencePlan();
    m_text->setFocus();
}

void VoiceGenerationPanel::clearScriptLine()
{
    m_selectedScriptLine = -1;
    m_selectedScriptCharacter.clear();
    m_selectedScriptSegment.clear();
    refreshScriptLink();
}

void VoiceGenerationPanel::refreshScriptLink()
{
    m_scriptLink->setVisible(m_selectedScriptLine >= 0);
    if (m_selectedScriptLine >= 0) {
        m_scriptLinkLabel->setText(tr("Voicing script line %1 (%2).")
            .arg(m_selectedScriptLine).arg(m_selectedScriptCharacter));
    }
}

void VoiceGenerationPanel::refreshProviderState()
{
    const QString provider = currentProvider();
    const auto* info = findVoiceProvider(provider);
    const bool breeze = provider == QStringLiteral("breeze");
    // Installation checks stat several files (and Breeze may probe drives
    // once), so they run on engine changes, not on every keystroke.
    m_providerInstalled = VoiceGenerationService::providerInstalled(provider);

    const QString engineName = info ? info->displayName : tr("This engine");
    const bool speed = info && info->supportsSpeed;
    const bool duration = info && info->supportsDuration;
    m_speed->setEnabled(speed);
    m_speed->setToolTip(speed ? tr("Speaking pace.")
                              : tr("%1 does not support speed control.").arg(engineName));
    m_duration->setEnabled(duration);
    m_duration->setToolTip(duration
        ? tr("Fit the line to an exact length. Auto lets the model decide.")
        : tr("%1 does not support a target duration.").arg(engineName));

    const auto& colors = Theme::colors();
    if (m_providerInstalled) {
        m_engineStatus->setText(breeze
            ? tr("Ready — using Breeze from %1").arg(QDir::toNativeSeparators(
                  VoiceGenerationService::breezeInstallationRoot()))
            : tr("Engine ready."));
        m_engineStatus->setStyleSheet(
            QStringLiteral("color: %1;").arg(Theme::hex(colors.success)));
    } else {
        m_engineStatus->setText(VoiceGenerationService::providerInstallHint(provider));
        m_engineStatus->setStyleSheet(
            QStringLiteral("color: %1;").arg(Theme::hex(colors.warning)));
    }
    m_locateBreeze->setVisible(breeze && !m_providerInstalled);
    refreshGenerateAvailability();
    if (!m_providerInstalled) {
        m_status->setText(tr(
            "Your text is ready, but the selected voice engine must be connected first."));
    } else if ((!m_service || !m_service->isBusy()) && info) {
        m_status->setText(info->readyHint);
    }
}

void VoiceGenerationPanel::refreshGenerateAvailability()
{
    const bool hasText = !m_text->toPlainText().trimmed().isEmpty();
    const bool busy = m_service && m_service->isBusy();
    m_generate->setEnabled(m_providerInstalled && hasText && !busy);
    if (!m_providerInstalled) {
        m_generate->setToolTip(VoiceGenerationService::providerInstallHint(currentProvider()));
    } else if (!hasText) {
        m_generate->setToolTip(tr("Type the words you want the character to say."));
    } else {
        m_generate->setToolTip(tr("Generate an audition draft using the selected voice."));
    }
}

void VoiceGenerationPanel::refreshReferencePlan()
{
    const QString character = m_character->currentText().trimmed();
    const auto candidates = approvedCandidates();
    const bool hasApproved = std::any_of(
        candidates.cbegin(), candidates.cend(),
        [&character](const auto& candidate) {
            return candidate.character.compare(character, Qt::CaseInsensitive) == 0;
        });
    m_saveReference->setEnabled(hasApproved);

    if (m_manualReference->isChecked()) {
        m_autoReferenceSummary->setText(tr("Manual override is active."));
        return;
    }
    const auto plan = planAutomaticReference(candidates, character, currentProvider());
    if (plan.segments.isEmpty()) {
        m_autoReferenceSummary->setText(tr(
            "No approved clips are available for %1. Confirm matched clips, "
            "or enable Manual reference override.").arg(
                character.isEmpty() ? tr("this character") : character));
        return;
    }
    m_autoReferenceSummary->setText(plan.fromSavedLibrary
        ? tr("Using a saved %1 reference (%2s).")
              .arg(character).arg(plan.duration, 0, 'f', 1)
        : tr("Auto-selected %1 approved clip(s), %2s from %3 imported track(s).")
              .arg(plan.segments.size()).arg(plan.duration, 0, 'f', 1)
              .arg(plan.trackCount));
}

void VoiceGenerationPanel::refreshManualTrack()
{
    if (m_reference->currentIndex() < 0) {
        m_referenceWaveform->hide();
        m_referenceText->clear();
        return;
    }
    const auto details = m_reference->currentData().toMap();
    const QString path = details.value(QStringLiteral("path")).toString();
    const double duration = details.value(QStringLiteral("duration")).toDouble();
    const bool library = details.value(QStringLiteral("library")).toBool();
    m_referenceText->setText(details.value(QStringLiteral("text")).toString());
    m_referenceStart->setMaximum(std::max(0.1, duration));
    m_referenceEnd->setMaximum(std::max(0.1, duration));
    {
        const QSignalBlocker startBlocker(m_referenceStart);
        const QSignalBlocker endBlocker(m_referenceEnd);
        m_referenceStart->setValue(0.0);
        m_referenceEnd->setValue(duration > 0.0
            ? std::min(duration, referenceTargetSeconds(currentProvider()))
            : 0.0);
    }
    const auto* samples = (!library && m_audioSync)
        ? m_audioSync->voiceAudioSamples(path) : nullptr;
    if (samples && !samples->samples.empty() && samples->sampleRate > 0) {
        const double fullDuration = static_cast<double>(samples->samples.size())
                                  / samples->sampleRate;
        m_referenceWaveform->setAudioShared(
            &samples->samples, samples->sampleRate, 0.0, fullDuration);
        m_referenceWaveform->setTrimHandlesVisible(true);
        m_referenceWaveform->setTrimRange(
            m_referenceStart->value(), m_referenceEnd->value());
        m_referenceWaveform->show();
        m_referenceStart->setEnabled(true);
        m_referenceEnd->setEnabled(true);
    } else {
        m_referenceWaveform->hide();
        m_referenceStart->setEnabled(!library);
        m_referenceEnd->setEnabled(!library);
    }
    refreshManualTranscript();
}

void VoiceGenerationPanel::refreshManualTranscript()
{
    if (!m_audioSync || m_reference->currentIndex() < 0) return;
    const auto details = m_reference->currentData().toMap();
    if (details.value(QStringLiteral("library")).toBool()) return;
    const QString transcript = m_audioSync->voiceTranscriptForRange(
        details.value(QStringLiteral("path")).toString(),
        m_referenceStart->value(), m_referenceEnd->value());
    m_referenceText->setText(transcript);
}

void VoiceGenerationPanel::generate()
{
    if (!m_service) return;
    clearDraft(true);
    VoiceGenerationRequest request;
    request.requestId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    request.provider = currentProvider();
    request.text = m_text->toPlainText().trimmed();
    request.character = m_character->currentText().trimmed();
    if (request.character.isEmpty()) request.character = tr("Unassigned");
    if (m_manualReference->isChecked()) {
        const auto reference = m_reference->currentData().toMap();
        VoiceReferenceSegment segment;
        segment.audioFile = reference.value(QStringLiteral("path")).toString();
        segment.transcript = m_referenceText->text().trimmed();
        const bool library = reference.value(QStringLiteral("library")).toBool();
        segment.start = library ? 0.0 : m_referenceStart->value();
        segment.end = library ? 0.0 : m_referenceEnd->value();
        if (!segment.audioFile.isEmpty()) request.references.push_back(segment);
    } else {
        request.references = planAutomaticReference(
            approvedCandidates(), request.character, request.provider).segments;
    }
    request.speed = m_speed->value();
    request.targetDuration = m_duration->isEnabled() ? m_duration->value() : 0.0;
    request.seed = m_seed->value();
    request.scriptLineNumber = m_selectedScriptLine;
    request.scriptSegment = m_selectedScriptSegment;
    m_activeRequestId = request.requestId;
    m_service->enqueue(request);
}

void VoiceGenerationPanel::listenToDraft()
{
    if (m_draftAuditionTimer && m_draftAuditionTimer->isActive()) {
        m_draftAuditionTimer->stop();
        if (m_audioSync) m_audioSync->stopVoiceDraftAudition();
        m_listen->setText(tr("▶ Listen"));
        m_status->setText(tr("Audition stopped."));
        return;
    }
    if (m_draftPath.isEmpty() || !QFileInfo::exists(m_draftPath)) {
        m_status->setText(tr("Generate a draft before listening."));
        return;
    }
    bool playingInApp = false;
    if (m_audioSync)
        playingInApp = m_audioSync->auditionVoiceDraft(m_draftPath);
    if (!playingInApp) {
        QDesktopServices::openUrl(QUrl::fromLocalFile(m_draftPath));
        m_status->setText(tr("Opened the draft in your audio player for review."));
        return;
    }

    m_listen->setText(tr("■ Stop"));
    m_status->setText(tr("Playing audition draft. Approve it only after listening."));
    if (!m_draftAuditionTimer) {
        m_draftAuditionTimer = new QTimer(this);
        m_draftAuditionTimer->setSingleShot(true);
        connect(m_draftAuditionTimer, &QTimer::timeout, this, [this]() {
            if (m_audioSync) m_audioSync->stopVoiceDraftAudition();
            m_listen->setText(tr("▶ Listen"));
        });
    }
    m_draftAuditionTimer->start(std::max(250, static_cast<int>(
        (m_draftDuration + 0.2) * 1000.0)));
}

void VoiceGenerationPanel::approveDraft(bool syncToScript)
{
    if (!m_service || m_draftPath.isEmpty()) return;
    if (m_draftAuditionTimer) m_draftAuditionTimer->stop();
    if (m_audioSync) m_audioSync->stopVoiceDraftAudition();
    m_listen->setText(tr("▶ Listen"));

    QString error;
    const QString approvedPath = m_service->approveDraft(
        m_draftRequest, m_draftPath, &error);
    if (approvedPath.isEmpty()) {
        m_status->setText(error);
        QMessageBox::warning(this, tr("Approve Voice Draft"), error);
        return;
    }

    bool matched = false;
    if (syncToScript && m_audioSync) {
        matched = m_audioSync->syncGeneratedAudio(
            approvedPath, m_draftRequest.character, m_draftRequest.text,
            m_draftRequest.scriptLineNumber, m_draftRequest.scriptSegment,
            m_draftDuration);
    }
    addApprovedClipToList(m_draftRequest, approvedPath, m_draftDuration);
    emit approvedForProject(approvedPath);

    if (syncToScript) {
        m_status->setText(matched
            ? tr("Approved, saved beside the source audio, imported, and synced to the script.")
            : tr("Approved and imported, but no matching line for this character was found. The clip remains unmatched."));
    } else {
        m_status->setText(tr(
            "Approved, saved beside the source audio, and imported into the project."));
    }
    clearDraft(false);
}

void VoiceGenerationPanel::clearDraft(bool deleteFile)
{
    if (m_draftAuditionTimer) m_draftAuditionTimer->stop();
    if (m_audioSync) m_audioSync->stopVoiceDraftAudition();
    if (deleteFile && !m_draftPath.isEmpty()) QFile::remove(m_draftPath);
    m_draftPath.clear();
    m_draftDuration = 0.0;
    m_draftRequest = {};
    m_listen->setText(tr("▶ Listen"));
    m_listen->setEnabled(false);
    m_discard->setEnabled(false);
    m_approveSync->setEnabled(false);
    m_approveImport->setEnabled(false);
}

void VoiceGenerationPanel::addApprovedClipToList(
    const VoiceGenerationRequest& request, const QString& path, double duration)
{
    auto* item = new QListWidgetItem(
        tr("%1  ·  %2s  ·  %3")
            .arg(request.character)
            .arg(duration, 0, 'f', 1)
            .arg(QFileInfo(path).fileName()));
    item->setData(Qt::UserRole, path);
    item->setToolTip(path + tr("\nDrag this approved clip to the timeline or use it from Project Bin."));
    item->setFlags(item->flags() | Qt::ItemIsDragEnabled);
    m_recent->insertItem(0, item);
}

void VoiceGenerationPanel::saveApprovedReference()
{
    if (!m_audioSync) return;
    const QString character = m_character->currentText().trimmed();
    QString path;
    QString error;
    if (!m_audioSync->saveApprovedVoiceReference(character, &path, &error)) {
        QMessageBox::warning(this, tr("Save Approved Reference"), error);
        return;
    }
    refreshFromAudioSync();
    QMessageBox::information(
        this, tr("Approved Reference Saved"),
        tr("Saved the approved %1 clips to the reusable voice-reference library:\n%2")
            .arg(character, path));
}

void VoiceGenerationPanel::onFinished(const VoiceGenerationRequest& request,
                                      const QString& path, double duration)
{
    if (request.requestId != m_activeRequestId) return;
    m_draftRequest = request;
    m_draftPath = path;
    m_draftDuration = duration;
    m_activeRequestId.clear();
    m_listen->setEnabled(true);
    m_discard->setEnabled(true);
    m_approveSync->setEnabled(true);
    m_approveImport->setEnabled(true);
    m_status->setText(tr(
        "Draft ready (%1s). Listen, then approve it for script sync or project-only import.")
        .arg(duration, 0, 'f', 1));
}

void VoiceGenerationPanel::onFailed(const VoiceGenerationRequest& request, const QString& error)
{
    if (!request.requestId.isEmpty() && request.requestId != m_activeRequestId) return;
    m_activeRequestId.clear();
    m_status->setText(error);
}

} // namespace rt
