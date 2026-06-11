/*
 * AppPreferencesDialog — Application-wide preferences.
 *
 * Settings are persisted via QSettings ("ROUNDTABLE" / "NLE").
 */

#pragma once

#include "audio/AudioEngine.h"

#include <QDialog>
#include <QLabel>

#include <vector>
#include <string>

class QComboBox;
class QLineEdit;
class QSpinBox;

namespace rt {

class AppPreferencesDialog : public QDialog
{
    Q_OBJECT

public:
    /// Construct with an optional list of audio devices for the Audio section.
    explicit AppPreferencesDialog(QWidget* parent = nullptr,
                                  const std::vector<AudioDeviceInfo>& audioDevices = {});

    int autosaveMinutes() const;
    QString projectsDirectory() const;
    QString cacheDirectory() const;

    int  scrollbarWidth() const;

    /// Returns the selected audio output device index (-1 = system default).
    int audioDeviceIndex() const;

    /// Returns the selected hardware decode mode (0 = auto/prefer HW, 1 = software only).
    int hardwareDecodeMode() const;

private:
    void loadSettings();
    void saveSettings();

    QSpinBox*  m_autosaveSpin{nullptr};
    QSpinBox*  m_scrollbarWidthSpin{nullptr};
    QLineEdit* m_projectsDirEdit{nullptr};
    QLineEdit* m_cacheDirEdit{nullptr};
    QComboBox* m_audioDeviceCombo{nullptr};
    QComboBox* m_hardwareDecodeCombo{nullptr};
    QLabel*    m_cudaStatusLabel{nullptr};
};

} // namespace rt
