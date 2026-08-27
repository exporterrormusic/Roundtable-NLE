#pragma once

#include "panels/audio/VoiceGenerationService.h"

#include <QWidget>
#include <QStringList>

class QComboBox;
class QDoubleSpinBox;
class QGroupBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QSpinBox;
class QTextEdit;
class QTreeWidget;
class QTimer;

namespace rt {

class AudioSync;
class MiniWaveformWidget;
/// Reusable TTS surface used by both the Audio page and Timeline dock.
class VoiceGenerationPanel final : public QWidget
{
    Q_OBJECT

public:
    explicit VoiceGenerationPanel(VoiceGenerationService* service,
                                  bool compact,
                                  QWidget* parent = nullptr);

    void setAudioSync(AudioSync* audioSync);
    void refreshFromAudioSync();
    [[nodiscard]] QStringList availableCharacters() const;
    [[nodiscard]] QPushButton* generateButton() const noexcept { return m_generate; }
    [[nodiscard]] QPushButton* listenButton() const noexcept { return m_listen; }
    [[nodiscard]] QPushButton* approveSyncButton() const noexcept { return m_approveSync; }
    [[nodiscard]] QPushButton* approveImportButton() const noexcept { return m_approveImport; }
    [[nodiscard]] QPushButton* unloadModelButton() const noexcept { return m_unloadModel; }
    [[nodiscard]] QGroupBox* manualReferenceGroup() const noexcept { return m_manualReference; }
    [[nodiscard]] QWidget* manualReferenceContent() const noexcept { return m_manualReferenceContent; }

signals:
    /// Emitted only after the user approves a draft. The Timeline workspace
    /// imports the finalized source-adjacent file into Project Bin.
    void approvedForProject(const QString& path);

private:
    void buildUi(bool compact);
    void refreshProviderState();
    void refreshReferencePlan();
    void refreshManualTrack();
    void chooseScriptLine();
    void generate();
    void listenToDraft();
    void approveDraft(bool syncToScript);
    void clearDraft(bool deleteFile);
    void addApprovedClipToList(const VoiceGenerationRequest& request,
                               const QString& path, double duration);
    void saveApprovedReference();
    void onFinished(const VoiceGenerationRequest& request,
                    const QString& path, double duration);
    void onFailed(const VoiceGenerationRequest& request, const QString& error);

    VoiceGenerationService* m_service{nullptr};
    AudioSync* m_audioSync{nullptr};
    bool m_compact{false};

    QComboBox* m_provider{nullptr};
    QComboBox* m_reference{nullptr};
    QComboBox* m_character{nullptr};
    QGroupBox* m_manualReference{nullptr};
    QWidget* m_manualReferenceContent{nullptr};
    QLabel* m_autoReferenceSummary{nullptr};
    MiniWaveformWidget* m_referenceWaveform{nullptr};
    QLineEdit* m_referenceText{nullptr};
    QDoubleSpinBox* m_referenceStart{nullptr};
    QDoubleSpinBox* m_referenceEnd{nullptr};
    QTextEdit* m_text{nullptr};
    QDoubleSpinBox* m_speed{nullptr};
    QDoubleSpinBox* m_duration{nullptr};
    QSpinBox* m_seed{nullptr};
    QPushButton* m_generate{nullptr};
    QPushButton* m_cancel{nullptr};
    QPushButton* m_listen{nullptr};
    QPushButton* m_approveSync{nullptr};
    QPushButton* m_approveImport{nullptr};
    QPushButton* m_discard{nullptr};
    QPushButton* m_unloadModel{nullptr};
    QPushButton* m_saveReference{nullptr};
    QLabel* m_status{nullptr};
    QTreeWidget* m_scriptLines{nullptr};
    QListWidget* m_recent{nullptr};
    QTimer* m_draftAuditionTimer{nullptr};

    VoiceGenerationRequest m_draftRequest;
    QString m_activeRequestId;
    QString m_draftPath;
    double m_draftDuration{0.0};

    int m_selectedScriptLine{-1};
    QString m_selectedScriptSegment;
};

} // namespace rt
