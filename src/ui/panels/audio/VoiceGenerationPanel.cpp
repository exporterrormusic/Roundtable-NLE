#include "panels/audio/VoiceGenerationPanel.h"

#include "panels/audio/AudioSync.h"
#include "panels/audio/VoiceGenerationService.h"
#include "widgets/MiniWaveformWidget.h"

#include <QAbstractItemView>
#include <QComboBox>
#include <QDesktopServices>
#include <QDir>
#include <QDoubleSpinBox>
#include <QDrag>
#include <QFile>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QMimeData>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPushButton>
#include <QSet>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QSplitter>
#include <QStandardPaths>
#include <QTextEdit>
#include <QTimer>
#include <QTreeWidget>
#include <QUrl>
#include <QUuid>
#include <QVBoxLayout>

#include <algorithm>
#include <optional>

namespace rt {

namespace {

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

struct SavedReference
{
    QString path;
    QString character;
    QString transcript;
    double duration{0.0};
};

QVector<SavedReference> savedReferences()
{
    QVector<SavedReference> result;
    const QDir directory(QDir(QStandardPaths::writableLocation(
        QStandardPaths::AppLocalDataLocation)).filePath(QStringLiteral("Voice References")));
    const auto metadataFiles = directory.entryInfoList(
        {QStringLiteral("*.json")}, QDir::Files, QDir::Time);
    for (const auto& info : metadataFiles) {
        QFile file(info.absoluteFilePath());
        if (!file.open(QIODevice::ReadOnly)) continue;
        const auto document = QJsonDocument::fromJson(file.readAll());
        if (!document.isObject()) continue;
        const auto metadata = document.object();
        const QString mp3 = directory.filePath(info.completeBaseName()
                                               + QStringLiteral(".mp3"));
        if (!QFileInfo::exists(mp3)) continue;
        result.push_back({
            mp3,
            metadata.value(QStringLiteral("character")).toString(),
            metadata.value(QStringLiteral("transcript")).toString(),
            metadata.value(QStringLiteral("duration")).toDouble()
        });
    }
    return result;
}

std::optional<SavedReference> savedReferenceForCharacter(const QString& character)
{
    for (const auto& reference : savedReferences()) {
        if (reference.character.compare(character, Qt::CaseInsensitive) == 0)
            return reference;
    }
    return std::nullopt;
}

double desiredReferenceDuration(const QString& provider)
{
    return provider == QStringLiteral("fish-s2") ? 20.0 : 8.0;
}

QList<VoiceReferenceSegment> automaticReferenceSegments(
    const AudioSync* audioSync, const QString& character, const QString& provider,
    double* durationOut = nullptr, int* trackCountOut = nullptr)
{
    QList<VoiceReferenceSegment> result;
    double duration = 0.0;
    QSet<QString> tracks;
    if (audioSync && !character.trimmed().isEmpty()) {
        auto candidates = audioSync->voiceReferenceCandidates();
        std::stable_sort(candidates.begin(), candidates.end(),
            [](const auto& left, const auto& right) {
                if (left.confidence != right.confidence)
                    return left.confidence > right.confidence;
                return (left.end - left.start) > (right.end - right.start);
            });
        const double target = desiredReferenceDuration(provider);
        for (const auto& candidate : candidates) {
            if (candidate.character.compare(character, Qt::CaseInsensitive) != 0)
                continue;
            const double clipDuration = candidate.end - candidate.start;
            if (clipDuration < 0.35 || candidate.transcript.trimmed().isEmpty()) continue;
            result.push_back({candidate.sourceFile, candidate.transcript,
                              candidate.start, candidate.end});
            duration += clipDuration;
            tracks.insert(candidate.sourceFile);
            if (duration >= target) break;
        }
    }
    if (result.isEmpty()) {
        if (const auto saved = savedReferenceForCharacter(character)) {
            result.push_back({saved->path, saved->transcript, 0.0, 0.0});
            duration = saved->duration;
            tracks.insert(saved->path);
        }
    }
    if (durationOut) *durationOut = duration;
    if (trackCountOut) *trackCountOut = tracks.size();
    return result;
}

} // namespace

VoiceGenerationPanel::VoiceGenerationPanel(VoiceGenerationService* service,
                                           bool compact,
                                           QWidget* parent)
    : QWidget(parent), m_service(service), m_compact(compact)
{
    buildUi(compact);
    if (m_service) {
        connect(m_service, &VoiceGenerationService::statusChanged,
                m_status, &QLabel::setText);
        connect(m_service, &VoiceGenerationService::busyChanged,
                this, [this](bool busy) {
            m_generate->setEnabled(!busy);
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

void VoiceGenerationPanel::buildUi(bool compact)
{
    auto* controls = new QWidget(this);
    auto* controlsLayout = new QVBoxLayout(controls);
    controlsLayout->setContentsMargins(10, 10, 10, 10);
    controlsLayout->setSpacing(12);

    const QString sectionStyle = QStringLiteral(
        "QGroupBox { margin-top: 12px; padding-top: 8px; }"
        "QGroupBox::title { subcontrol-origin: margin; left: 10px; padding: 0 5px; }");

    auto* title = new QLabel(compact ? tr("Generate Voice Clip") : tr("Voice Generation"), controls);
    QFont titleFont = title->font();
    titleFont.setPointSize(titleFont.pointSize() + 2);
    titleFont.setBold(true);
    title->setFont(titleFont);
    controlsLayout->addWidget(title);

    auto* form = new QFormLayout;
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    m_provider = new QComboBox(controls);
#ifdef ROUNDTABLE_HAS_OMNIVOICE
    m_provider->addItem(tr("OmniVoice (Apache-2.0)"), QStringLiteral("omnivoice"));
#endif
#ifdef ROUNDTABLE_HAS_FISH_S2
    m_provider->addItem(tr("Fish S2 Pro (personal / non-commercial)"), QStringLiteral("fish-s2"));
#endif
    form->addRow(tr("Engine"), m_provider);

    m_character = new QComboBox(controls);
    m_character->setEditable(true);
    form->addRow(tr("Character"), m_character);

    auto* automaticReference = new QWidget(controls);
    auto* automaticLayout = new QHBoxLayout(automaticReference);
    automaticLayout->setContentsMargins(0, 0, 0, 0);
    automaticLayout->setSpacing(6);
    m_autoReferenceSummary = new QLabel(
        tr("Approved clips will be selected automatically."), automaticReference);
    m_autoReferenceSummary->setWordWrap(true);
    m_saveReference = new QPushButton(tr("Save Approved..."), automaticReference);
    m_saveReference->setToolTip(
        tr("Combine every confirmed clip for this character into a reusable MP3 reference."));
    automaticLayout->addWidget(m_autoReferenceSummary, 1);
    automaticLayout->addWidget(m_saveReference);
    form->addRow(tr("Reference"), automaticReference);
    controlsLayout->addLayout(form);

    m_manualReference = new QGroupBox(tr("Manual reference override"), controls);
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
        tr("Exact words spoken in the selected range"));
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
    controlsLayout->addWidget(m_manualReference);

    m_text = new QTextEdit(controls);
    m_text->setPlaceholderText(tr("Type what the character should say..."));
    m_text->setAcceptRichText(false);
    m_text->setMinimumHeight(compact ? 88 : 125);
    controlsLayout->addWidget(m_text);

    auto* options = new QHBoxLayout;
    m_speed = new QDoubleSpinBox(controls);
    m_speed->setRange(0.5, 2.0);
    m_speed->setSingleStep(0.05);
    m_speed->setValue(1.0);
    m_speed->setSuffix(QStringLiteral("x"));
    m_duration = new QDoubleSpinBox(controls);
    m_duration->setRange(0.0, 120.0);
    m_duration->setSpecialValueText(tr("Auto"));
    m_duration->setSuffix(tr(" sec"));
    m_seed = new QSpinBox(controls);
    m_seed->setRange(0, 999999999);
    m_seed->setValue(42);
    options->addWidget(new QLabel(tr("Speed"), controls));
    options->addWidget(m_speed);
    options->addWidget(new QLabel(tr("Duration"), controls));
    options->addWidget(m_duration);
    options->addWidget(new QLabel(tr("Seed"), controls));
    options->addWidget(m_seed);
    controlsLayout->addLayout(options);

    auto* actions = new QHBoxLayout;
    m_generate = new QPushButton(tr("Generate Draft"), controls);
    m_generate->setDefault(true);
    m_cancel = new QPushButton(tr("Cancel"), controls);
    m_cancel->setEnabled(false);
    actions->addWidget(m_generate, 1);
    actions->addWidget(m_cancel);
    controlsLayout->addLayout(actions);

    auto* approval = new QGroupBox(tr("Audition && Approve"), controls);
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
    controlsLayout->addWidget(approval);

    m_status = new QLabel(tr("Select a voice reference or use an automatic voice."), controls);
    m_status->setWordWrap(true);
    controlsLayout->addWidget(m_status);

    auto* recentGroup = new QGroupBox(tr("Approved Generated Clips"), controls);
    recentGroup->setStyleSheet(sectionStyle);
    auto* recentLayout = new QVBoxLayout(recentGroup);
    recentLayout->setContentsMargins(12, 20, 12, 12);
    recentLayout->setSpacing(8);
    m_recent = new GeneratedAudioList(recentGroup);
    m_recent->setDragEnabled(true);
    m_recent->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_recent->setMinimumHeight(compact ? 90 : 120);
    recentLayout->addWidget(m_recent);
    controlsLayout->addWidget(recentGroup, 1);

    m_unloadModel = new QPushButton(tr("Unload Model / Free VRAM"), controls);
    m_unloadModel->setToolTip(tr(
        "Stop the local voice worker and release the model's GPU memory."));
    m_unloadModel->setEnabled(false);
    controlsLayout->addWidget(m_unloadModel);

    connect(m_generate, &QPushButton::clicked, this, &VoiceGenerationPanel::generate);
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
    connect(m_character, &QComboBox::currentTextChanged,
            this, &VoiceGenerationPanel::refreshReferencePlan);
    connect(m_reference, &QComboBox::currentIndexChanged,
            this, &VoiceGenerationPanel::refreshManualTrack);
    connect(m_manualReference, &QGroupBox::toggled, this, [this](bool checked) {
        if (m_manualReferenceContent)
            m_manualReferenceContent->setVisible(checked);
        refreshReferencePlan();
    });
    connect(m_referenceStart, &QDoubleSpinBox::valueChanged, this, [this](double start) {
        if (start >= m_referenceEnd->value()) {
            QSignalBlocker blocker(m_referenceEnd);
            m_referenceEnd->setValue(start + 0.1);
        }
        if (m_referenceWaveform)
            m_referenceWaveform->setTrimRange(start, m_referenceEnd->value());
    });
    connect(m_referenceEnd, &QDoubleSpinBox::valueChanged, this, [this](double end) {
        if (end <= m_referenceStart->value()) {
            QSignalBlocker blocker(m_referenceStart);
            m_referenceStart->setValue(std::max(0.0, end - 0.1));
        }
        if (m_referenceWaveform)
            m_referenceWaveform->setTrimRange(m_referenceStart->value(), end);
    });
    connect(m_referenceWaveform, &MiniWaveformWidget::trimChanging,
            this, [this](double start, double end) {
        const QSignalBlocker startBlocker(m_referenceStart);
        const QSignalBlocker endBlocker(m_referenceEnd);
        m_referenceStart->setValue(start);
        m_referenceEnd->setValue(end);
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

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    if (compact) {
        root->addWidget(controls);
        return;
    }

    auto* scriptPanel = new QWidget(this);
    auto* scriptLayout = new QVBoxLayout(scriptPanel);
    scriptLayout->setContentsMargins(8, 10, 10, 10);
    auto* scriptTitle = new QLabel(tr("Script — double-click a line to generate it"), scriptPanel);
    scriptLayout->addWidget(scriptTitle);
    m_scriptLines = new QTreeWidget(scriptPanel);
    m_scriptLines->setColumnCount(3);
    m_scriptLines->setHeaderLabels({tr("#"), tr("Character"), tr("Dialogue")});
    m_scriptLines->setRootIsDecorated(false);
    m_scriptLines->setAlternatingRowColors(true);
    m_scriptLines->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_scriptLines->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_scriptLines->header()->setSectionResizeMode(2, QHeaderView::Stretch);
    scriptLayout->addWidget(m_scriptLines, 1);
    connect(m_scriptLines, &QTreeWidget::itemDoubleClicked,
            this, [this](QTreeWidgetItem*, int) { chooseScriptLine(); });
    connect(m_scriptLines, &QTreeWidget::itemSelectionChanged,
            this, &VoiceGenerationPanel::chooseScriptLine);

    auto* splitter = new QSplitter(Qt::Horizontal, this);
    splitter->addWidget(controls);
    splitter->addWidget(scriptPanel);
    splitter->setStretchFactor(0, 2);
    splitter->setStretchFactor(1, 3);
    splitter->setSizes({520, 760});
    root->addWidget(splitter);
}

void VoiceGenerationPanel::setAudioSync(AudioSync* audioSync)
{
    if (m_audioSync == audioSync) return;
    if (m_audioSync) disconnect(m_audioSync, nullptr, this, nullptr);
    m_audioSync = audioSync;
    if (m_audioSync) {
        connect(m_audioSync, &AudioSync::voiceContextChanged,
                this, &VoiceGenerationPanel::refreshFromAudioSync);
        connect(m_audioSync, &AudioSync::scriptLoaded,
                this, [this](int) { refreshFromAudioSync(); });
        connect(m_audioSync, &AudioSync::audioImported,
                this, [this](const QString&) { refreshFromAudioSync(); });
        connect(m_audioSync, &AudioSync::syncCompleted,
                this, [this](int, int) { refreshFromAudioSync(); });
    }
    refreshFromAudioSync();
}

QStringList VoiceGenerationPanel::availableCharacters() const
{
    QStringList result;
    if (!m_character) return result;
    for (int index = 0; index < m_character->count(); ++index)
        result.append(m_character->itemText(index));
    return result;
}

void VoiceGenerationPanel::refreshFromAudioSync()
{
    const QString currentCharacter = m_character->currentText();
    const QString currentReference = m_reference->currentData().toMap()
                                         .value(QStringLiteral("path")).toString();
    m_character->clear();
    m_reference->clear();

    if (m_scriptLines) m_scriptLines->clear();
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

    const auto libraryReferences = savedReferences();
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
        currentCharacter, Qt::MatchFixedString);
    if (previousCharacter >= 0)
        m_character->setCurrentIndex(previousCharacter);
    else if (m_character->count() > 0)
        m_character->setCurrentIndex(0);
    for (int index = 0; index < m_reference->count(); ++index) {
        if (m_reference->itemData(index).toMap().value(QStringLiteral("path")).toString()
                == currentReference) {
            m_reference->setCurrentIndex(index);
            break;
        }
    }

    if (m_scriptLines && m_audioSync) {
        for (const auto& line : m_audioSync->voiceScriptLines()) {
            auto* item = new QTreeWidgetItem(m_scriptLines,
                {QString::number(line.lineNumber), line.character, line.dialogue});
            item->setData(0, Qt::UserRole, line.lineNumber);
            item->setData(0, Qt::UserRole + 1, line.segment);
        }
    }
    refreshManualTrack();
    refreshReferencePlan();
}

void VoiceGenerationPanel::refreshProviderState()
{
    const QString provider = m_provider->currentData().toString();
    const bool installed = VoiceGenerationService::providerInstalled(provider);
    const bool omni = provider == QStringLiteral("omnivoice");
    m_duration->setEnabled(omni);
    m_speed->setEnabled(omni);
    m_generate->setEnabled(installed && (!m_service || !m_service->isBusy()));
    if (!installed) m_status->setText(VoiceGenerationService::providerInstallHint(provider));
    else if (!m_service || !m_service->isBusy())
        m_status->setText(omni
            ? tr("OmniVoice ready. For the strongest clone, use a clean 3–10 second "
                 "reference and its exact transcript. Duration targeting is available.")
            : tr("Fish S2 Pro ready. For the strongest clone, use a clean 10–30 second "
                 "reference and its exact transcript. Close other GPU-heavy apps before loading."));
}

void VoiceGenerationPanel::refreshReferencePlan()
{
    const QString character = m_character->currentText().trimmed();
    double duration = 0.0;
    int trackCount = 0;
    const auto references = automaticReferenceSegments(
        m_audioSync, character, m_provider->currentData().toString(),
        &duration, &trackCount);
    const auto approvedCandidates = m_audioSync
        ? m_audioSync->voiceReferenceCandidates()
        : QVector<VoiceReferenceCandidate>{};
    const bool hasCurrentApproved = std::any_of(
        approvedCandidates.cbegin(), approvedCandidates.cend(),
        [&character](const auto& candidate) {
            return candidate.character.compare(character, Qt::CaseInsensitive) == 0;
        });
    m_saveReference->setEnabled(hasCurrentApproved);

    if (m_manualReference->isChecked()) {
        m_autoReferenceSummary->setText(tr("Manual override is active."));
        return;
    }
    if (references.isEmpty()) {
        m_autoReferenceSummary->setText(tr(
            "No approved clips are available for %1. Confirm matched clips, "
            "or enable Manual reference override.").arg(
                character.isEmpty() ? tr("this character") : character));
        return;
    }
    const bool usingSaved = !hasCurrentApproved;
    m_autoReferenceSummary->setText(usingSaved
        ? tr("Using a saved %1 reference (%2s).")
              .arg(character).arg(duration, 0, 'f', 1)
        : tr("Auto-selected %1 approved clip(s), %2s from %3 imported track(s).")
              .arg(references.size()).arg(duration, 0, 'f', 1).arg(trackCount));
}

void VoiceGenerationPanel::refreshManualTrack()
{
    if (!m_reference || m_reference->currentIndex() < 0) {
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
            ? std::min(duration, desiredReferenceDuration(m_provider->currentData().toString()))
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
}

void VoiceGenerationPanel::chooseScriptLine()
{
    if (!m_scriptLines) return;
    auto* item = m_scriptLines->currentItem();
    if (!item) return;
    m_selectedScriptLine = item->data(0, Qt::UserRole).toInt();
    m_selectedScriptSegment = item->data(0, Qt::UserRole + 1).toString();
    m_character->setCurrentText(item->text(1));
    m_text->setPlainText(item->text(2));
    refreshReferencePlan();
}

void VoiceGenerationPanel::generate()
{
    if (!m_service) return;
    clearDraft(true);
    VoiceGenerationRequest request;
    request.requestId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    request.provider = m_provider->currentData().toString();
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
        request.references = automaticReferenceSegments(
            m_audioSync, request.character, request.provider);
    }
    if (!request.references.isEmpty()) {
        const auto& first = request.references.front();
        request.referenceAudio = first.audioFile;
        request.referenceText = first.transcript;
        request.referenceStart = first.start;
        request.referenceEnd = first.end;
    }
    request.speed = m_speed->value();
    request.targetDuration = m_duration->value();
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
            if (m_listen) m_listen->setText(tr("▶ Listen"));
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
            .arg(QFileInfo(path).fileName()), m_recent);
    item->setData(Qt::UserRole, path);
    item->setToolTip(path + tr("\nDrag this approved clip to the timeline or use it from Project Bin."));
    item->setFlags(item->flags() | Qt::ItemIsDragEnabled);
    m_recent->insertItem(0, m_recent->takeItem(m_recent->row(item)));
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
