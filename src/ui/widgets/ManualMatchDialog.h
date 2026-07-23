/*
 * ManualMatchDialog — full-screen waveform overlay for manually matching
 * a script line to an audio region.
 *
 * Shows the entire audio file, greys out confirmed regions, and lets
 * the user click+drag to select the correct region.
 */

#pragma once

#include <QDialog>
#include <QComboBox>
#include <QLabel>
#include <QPushButton>

#include <string>
#include <unordered_map>
#include <vector>
#include <utility>

class QTimer;

namespace rt {

class AudioEngine;
class AVSyncClock;
class FullWaveformWidget;

class ManualMatchDialog : public QDialog
{
    Q_OBJECT

public:
    /// Result returned when the dialog is accepted.
    struct Result {
        std::string audioFile;   ///< Which audio file was selected
        double      start{0.0};  ///< Selection start (seconds)
        double      end{0.0};    ///< Selection end (seconds)
    };

    /// Audio sample data passed in.
    struct AudioData {
        std::vector<float> samples;
        uint32_t sampleRate{44100};
    };

    explicit ManualMatchDialog(
        const std::string& character,
        const std::string& dialogue,
        int lineNumber,
        const std::vector<std::string>& audioFiles,
        const std::unordered_map<std::string, AudioData>& audioSamples,
        const std::vector<std::pair<double,double>>& confirmedRegions,
        const std::vector<std::pair<double,double>>& tentativeRegions,
        const std::string& preselectedFile = {},
        AudioEngine* engine = nullptr,
        QWidget* parent = nullptr);

    ~ManualMatchDialog() override;

    /// Get the result after accept.
    [[nodiscard]] Result result() const { return m_result; }

    /// Call after setting m_confirmedByFile/m_tentativeByFile to load the first file.
    void loadInitialFile();

private slots:
    void onAudioFileChanged(int index);
    void onSelectionChanged(double start, double end);
    void onPreviewClicked();
    void onConfirmClicked();

protected:
    void keyPressEvent(QKeyEvent* event) override;
    void showEvent(QShowEvent* event) override;

public:
    // Per-file confirmed/tentative regions: keyed by audio path
    std::unordered_map<std::string, std::vector<std::pair<double,double>>> m_confirmedByFile;
    std::unordered_map<std::string, std::vector<std::pair<double,double>>> m_tentativeByFile;

private:
    void updateSelectionLabel();
    void loadFileIntoWaveform(const std::string& path);

    // Input data
    std::string m_character;
    std::string m_dialogue;
    int         m_lineNumber;
    std::vector<std::string> m_audioFiles;
    std::unordered_map<std::string, AudioData> m_audioSamples;

    // All confirmed/tentative for current file
    std::vector<std::pair<double,double>> m_currentConfirmed;
    std::vector<std::pair<double,double>> m_currentTentative;

    // Result
    Result m_result;

    // Audio
    AudioEngine* m_audioEngine{nullptr};
    std::string  m_currentFile;

    // Playback state
    void startPreviewPlayback();
    void stopPreviewPlayback();
    void setupFullFileSource();
    void installFullFileSource();
    QTimer* m_playheadTimer{nullptr};
    std::vector<float> m_fullFileBuffer;  ///< Full file resampled to engine rate
    double m_fullFileRate{48000.0};       ///< Engine sample rate
    double m_playbackEndTime{0.0};
    bool m_isPlaying{false};
    AVSyncClock* m_savedSyncClock{nullptr}; ///< Timeline clock detached during modal preview

    // JKL shuttle state
    int m_jShuttleLevel{0};
    int m_lShuttleLevel{0};

    // UI
    FullWaveformWidget* m_waveform{nullptr};
    QComboBox*          m_fileCombo{nullptr};
    QLabel*             m_scriptLabel{nullptr};
    QLabel*             m_selectionLabel{nullptr};
    QPushButton*        m_previewBtn{nullptr};
    QPushButton*        m_confirmBtn{nullptr};
    QPushButton*        m_cancelBtn{nullptr};
};

} // namespace rt
