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
class VideoFrameGrabber;
class VideoFrameView;

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
        std::vector<float> samples;   ///< owned copy (tests), unused when `shared` is set
        uint32_t sampleRate{44100};
        /// The caller's samples, not copied (AudioSync passes its loaded
        /// files: copying every one cost ~700 MB per hour of audio per open).
        /// Must outlive the dialog.
        const std::vector<float>* shared{nullptr};
        [[nodiscard]] const std::vector<float>& pcm() const noexcept
        {
            return shared ? *shared : samples;
        }
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

    /// Current playhead position (seconds).
    [[nodiscard]] double playhead() const;

    /// Get the result after accept.
    [[nodiscard]] Result result() const { return m_result; }

    /// Call after setting m_confirmedByFile/m_tentativeByFile to load the first file.
    void loadInitialFile();

    /// Enable the video pane: video files then show their picture above the
    /// waveform, following the playhead and the in/out point being dragged.
    /// Call before loadInitialFile().
    void setVideoFrameGrabber(VideoFrameGrabber* grabber);

    /// Edit an existing take instead of matching a new one: retitles the
    /// dialog, preselects [start, end] and zooms to it.  Call after
    /// loadInitialFile().
    void setEditRange(const QString& title, const QString& confirmText,
                      double start, double end);

private slots:
    void onAudioFileChanged(int index);
    void onSelectionChanged(double start, double end);
    void onPreviewClicked();
    void onConfirmClicked();

protected:
    void keyPressEvent(QKeyEvent* event) override;
    void keyReleaseEvent(QKeyEvent* event) override;
    void changeEvent(QEvent* event) override;
    void showEvent(QShowEvent* event) override;

public:
    // Per-file confirmed/tentative regions: keyed by audio path
    std::unordered_map<std::string, std::vector<std::pair<double,double>>> m_confirmedByFile;
    std::unordered_map<std::string, std::vector<std::pair<double,double>>> m_tentativeByFile;

private:
    void updateSelectionLabel();
    void loadFileIntoWaveform(const std::string& path);
    /// Show the current file's video frame at timeSec (video files only).
    void showVideoAt(double timeSec);

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
    const std::vector<float>* m_fullFile{nullptr};   ///< m_fullFileBuffer, or the samples when no resample
    [[nodiscard]] bool hasFullFile() const noexcept { return m_fullFile && !m_fullFile->empty(); }
    double m_fullFileRate{48000.0};       ///< Engine sample rate
    double m_playbackEndTime{0.0};
    bool m_isPlaying{false};
    AVSyncClock* m_savedSyncClock{nullptr}; ///< Timeline clock detached during modal preview

    // JKL shuttle state
    int m_jShuttleLevel{0};
    int m_lShuttleLevel{0};

    // Premiere K-hold: K+tap J/L steps one frame; K+hold J/L plays slow motion.
    bool    m_kHeld{false};
    int     m_slowDir{0};              ///< J/L held with K: -1 / +1, 0 = none
    QTimer* m_slowHoldTimer{nullptr};  ///< tap vs hold decision
    bool    m_slowPlaying{false};
    void stepFrame(int direction);
    void startSlowPlayback(int direction);
    void stopSlowMotion();
    [[nodiscard]] double frameDuration() const;

    // Video pane (video files only)
    VideoFrameGrabber*  m_frameGrabber{nullptr};
    VideoFrameView*     m_videoView{nullptr};
    bool                m_currentIsVideo{false};
    double              m_lastSelStart{-1.0};
    double              m_lastSelEnd{-1.0};

    // UI
    QLabel*             m_titleLabel{nullptr};
    QLabel*             m_fileLabel{nullptr};
    FullWaveformWidget* m_waveform{nullptr};
    QComboBox*          m_fileCombo{nullptr};
    QLabel*             m_scriptLabel{nullptr};
    QLabel*             m_selectionLabel{nullptr};
    QPushButton*        m_previewBtn{nullptr};
    QPushButton*        m_confirmBtn{nullptr};
    QPushButton*        m_cancelBtn{nullptr};
};

} // namespace rt
