// ExportPanelPresets.cpp
// Export presets + codec/container/accel population.
// Extracted from ExportPanel.cpp for file size (behavior-preserving).

#include "ExportPanel.h"
#include "ExportMiniTimeline.h"

#include "Theme.h"

#include "command/CommandStack.h"
#include "command/LambdaCommand.h"
#include "Encoder.h"
#include "Muxer.h"
#include "RenderQueue.h"

#include "widgets/CollapsibleSection.h"
#include "widgets/ToggleSwitch.h"

#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QApplication>
#include <QMenu>
#include <QMessageBox>
#include <QSettings>
#include <QShowEvent>
#include <QSplitter>

#include "audio/AudioEngine.h"
#include "cache/FrameCache.h"
#include "playback/PlaybackController.h"
#include "project/Project.h"
#include "timeline/Timeline.h"
#include "timeline/Track.h"
#include "timeline/Clip.h"

#include "MainWindow.h"
#include "App.h"
#include "HardwareDiagnostics.h"

#include <spdlog/spdlog.h>

#include <QApplication>
#include <QMetaObject>
#include <cmath>


namespace rt {

void ExportPanel::populatePresets()
{
    m_presetCombo->addItem(tr("Custom"), static_cast<int>(ExportPreset::Custom));
    m_presetCombo->addItem(tr("YouTube 1080p 30fps"), static_cast<int>(ExportPreset::YouTube1080p30));
    m_presetCombo->addItem(tr("YouTube 1080p 60fps"), static_cast<int>(ExportPreset::YouTube1080p60));
    m_presetCombo->addItem(tr("YouTube 4K 30fps"), static_cast<int>(ExportPreset::YouTube4K30));
    m_presetCombo->addItem(tr("YouTube 4K 60fps"), static_cast<int>(ExportPreset::YouTube4K60));
    m_presetCombo->addItem(tr("Archive ProRes HQ"), static_cast<int>(ExportPreset::ArchiveProRes));
    m_presetCombo->addItem(tr("Web Optimized"), static_cast<int>(ExportPreset::WebOptimized));
    loadCustomPresets();
}

QString ExportPanel::customPresetsDir() const
{
    return QCoreApplication::applicationDirPath() + QStringLiteral("/assets/presets/export");
}

void ExportPanel::loadCustomPresets()
{
    QDir dir(customPresetsDir());
    if (!dir.exists()) return;
    const auto files = dir.entryInfoList({QStringLiteral("*.json")}, QDir::Files, QDir::Name);
    for (const auto& fi : files) {
        // User presets get data value = -1 (not a built-in ExportPreset enum)
        m_presetCombo->addItem(fi.baseName(), -1);
    }
}

void ExportPanel::onSavePreset()
{
    bool ok = false;
    QString name = QInputDialog::getText(this, tr("Save Export Preset"),
                                         tr("Preset name:"), QLineEdit::Normal,
                                         QString(), &ok);
    if (!ok || name.trimmed().isEmpty()) return;
    name = name.trimmed();

    QDir dir(customPresetsDir());
    if (!dir.exists()) dir.mkpath(QStringLiteral("."));

    QJsonObject obj;
    obj[QStringLiteral("width")]     = m_widthSpin->value();
    obj[QStringLiteral("height")]    = m_heightSpin->value();
    obj[QStringLiteral("fps")]       = m_fpsCombo->currentData().toInt();
    obj[QStringLiteral("codec")]     = m_codecCombo->currentData().toInt();
    obj[QStringLiteral("accel")]     = m_accelCombo->currentData().toInt();
    obj[QStringLiteral("crf")]       = m_crfSlider->value();
    obj[QStringLiteral("container")] = m_containerCombo->currentData().toInt();
    obj[QStringLiteral("video")]     = videoEnabled();
    obj[QStringLiteral("audio")]     = audioEnabled();
    obj[QStringLiteral("audioFormat")] = m_audioFormatCombo->currentData().toInt();
    if (m_profileCombo && m_profileCombo->currentData().isValid())
        obj[QStringLiteral("profile")] = m_profileCombo->currentData().toInt();

    QString path = dir.filePath(name + QStringLiteral(".json"));
    QFile f(path);
    if (f.open(QIODevice::WriteOnly)) {
        f.write(QJsonDocument(obj).toJson(QJsonDocument::Compact));
        f.close();
    }

    // Refresh combo: check if already there
    bool found = false;
    for (int i = 0; i < m_presetCombo->count(); ++i) {
        if (m_presetCombo->itemText(i) == name) { found = true; break; }
    }
    if (!found)
        m_presetCombo->addItem(name, -1);

    // Select the saved preset
    for (int i = 0; i < m_presetCombo->count(); ++i) {
        if (m_presetCombo->itemText(i) == name) {
            m_presetCombo->setCurrentIndex(i);
            break;
        }
    }
}

void ExportPanel::onDeletePreset()
{
    int idx = m_presetCombo->currentIndex();
    if (idx < 0) return;
    int data = m_presetCombo->currentData().toInt();
    if (data != -1) return; // can only delete user presets

    QString name = m_presetCombo->currentText();
    auto answer = QMessageBox::question(this, tr("Delete Preset"),
                                        tr("Delete preset \"%1\"?").arg(name));
    if (answer != QMessageBox::Yes) return;

    QDir dir(customPresetsDir());
    dir.remove(name + QStringLiteral(".json"));
    m_presetCombo->removeItem(idx);
}

void ExportPanel::populateCodecs()
{
    // Video formats (data = EncoderCodec int).
    m_codecCombo->addItem(QStringLiteral("H.264"), static_cast<int>(EncoderCodec::H264));
    m_codecCombo->addItem(QStringLiteral("H.265 (HEVC)"), static_cast<int>(EncoderCodec::H265));
    m_codecCombo->addItem(QStringLiteral("AV1"), static_cast<int>(EncoderCodec::AV1));
    m_codecCombo->addItem(QStringLiteral("ProRes"), static_cast<int>(EncoderCodec::ProRes));
    m_codecCombo->addItem(QStringLiteral("DNxHR"), static_cast<int>(EncoderCodec::DNxHR));
    m_codecCombo->addItem(QStringLiteral("Image Sequence"), static_cast<int>(EncoderCodec::ImageSequence));

    // Audio-only formats (data = kAudioFormatBase + AudioCodec). Picking one
    // hides the VIDEO section and exports just the mixed-down audio, Premiere-
    // style.  See onCodecChanged.
    m_codecCombo->insertSeparator(m_codecCombo->count());
    m_codecCombo->addItem(QStringLiteral("MP3 (audio only)"),
                          kAudioFormatBase + static_cast<int>(AudioCodec::MP3));
    m_codecCombo->addItem(QStringLiteral("AAC / M4A (audio only)"),
                          kAudioFormatBase + static_cast<int>(AudioCodec::AAC));
    m_codecCombo->addItem(QStringLiteral("Waveform / WAV (audio only)"),
                          kAudioFormatBase + static_cast<int>(AudioCodec::PCM_S16LE));
    m_codecCombo->addItem(QStringLiteral("FLAC (audio only)"),
                          kAudioFormatBase + static_cast<int>(AudioCodec::FLAC));
}

void ExportPanel::populateAudioFormats()
{
    // Each item carries its AudioCodec int as data. AAC default (most compatible).
    m_audioFormatCombo->addItem(tr("AAC (M4A)"),   static_cast<int>(AudioCodec::AAC));
    m_audioFormatCombo->addItem(tr("MP3"),         static_cast<int>(AudioCodec::MP3));
    m_audioFormatCombo->addItem(tr("WAV (PCM)"),   static_cast<int>(AudioCodec::PCM_S16LE));
    m_audioFormatCombo->addItem(tr("FLAC"),        static_cast<int>(AudioCodec::FLAC));
}

bool ExportPanel::videoEnabled() const
{
    return m_videoSection && m_videoSection->toggle() &&
           m_videoSection->toggle()->isChecked();
}

bool ExportPanel::audioEnabled() const
{
    return m_audioSection && m_audioSection->toggle() &&
           m_audioSection->toggle()->isChecked();
}

void ExportPanel::updateAudioSectionState()
{
    const bool audioOnly = !videoEnabled();
    // For a video export the muxed audio is AAC; only audio-only exports get to
    // pick the file format, so the combo is editable only then.
    if (m_audioFormatCombo) m_audioFormatCombo->setEnabled(audioOnly);

    const int ac = m_audioFormatCombo ? m_audioFormatCombo->currentData().toInt() : -1;
    const bool lossy = (ac == static_cast<int>(AudioCodec::AAC) ||
                        ac == static_cast<int>(AudioCodec::MP3));
    const bool showBitrate = audioOnly && lossy;
    if (m_audioBitrateLabel) m_audioBitrateLabel->setVisible(showBitrate);
    if (m_audioBitrateCombo) m_audioBitrateCombo->setVisible(showBitrate);
}

void ExportPanel::onVideoEnabledChanged(bool on)
{
    // Can't export nothing: if video is turned off while audio is also off,
    // turn audio on so there's still a stream to export.
    if (!on && !audioEnabled() && m_audioSection && m_audioSection->toggle())
        m_audioSection->toggle()->setChecked(true);

    // Audio-only exports auto-expand the AUDIO section so the format is visible.
    if (!on && m_audioSection && !m_audioSection->isExpanded())
        m_audioSection->setExpanded(true);

    updateAudioSectionState();

    // Track the output extension to the active format.
    if (!on && m_audioFormatCombo) {
        applyOutputExtension(QString::fromLatin1(audioCodecExtension(
            static_cast<AudioCodec>(m_audioFormatCombo->currentData().toInt()))));
    } else if (on && m_containerCombo) {
        applyOutputExtension(QString::fromLatin1(containerFormatExtension(
            static_cast<ContainerFormat>(m_containerCombo->currentData().toInt()))));
    }

    updateFileEstimate();
}

void ExportPanel::onAudioEnabledChanged(bool on)
{
    // Mirror of onVideoEnabledChanged: don't allow both off.
    if (!on && !videoEnabled() && m_videoSection && m_videoSection->toggle())
        m_videoSection->toggle()->setChecked(true);

    updateFileEstimate();
}

void ExportPanel::onAudioFormatChanged(int /*index*/)
{
    updateAudioSectionState();

    // When exporting audio only, keep the filename extension in sync.
    if (!videoEnabled() && m_audioFormatCombo) {
        applyOutputExtension(QString::fromLatin1(audioCodecExtension(
            static_cast<AudioCodec>(m_audioFormatCombo->currentData().toInt()))));
    }

    updateFileEstimate();
}

void ExportPanel::applyOutputExtension(const QString& dotExt)
{
    if (!m_outputPath) return;
    const QString cur = m_outputPath->text().trimmed();
    if (cur.isEmpty()) return;

    QFileInfo fi(cur);
    const QString base = fi.completeBaseName();   // strips the trailing suffix only
    if (base.isEmpty()) return;
    const QString dir = fi.path();

    QString next = base + dotExt;
    if (!dir.isEmpty() && dir != QStringLiteral("."))
        next = dir + QStringLiteral("/") + next;

    if (next != cur) {
        m_outputPath->setText(next);
        syncPartsFromOutputPath();   // keep File Name field's extension in step
    }
}

void ExportPanel::populateContainers()
{
    m_containerCombo->addItem(QStringLiteral("MP4"), static_cast<int>(ContainerFormat::MP4));
    m_containerCombo->addItem(QStringLiteral("MOV"), static_cast<int>(ContainerFormat::MOV));
    m_containerCombo->addItem(QStringLiteral("MKV"), static_cast<int>(ContainerFormat::MKV));
    m_containerCombo->addItem(QStringLiteral("WebM"), static_cast<int>(ContainerFormat::WebM));
    m_containerCombo->addItem(QStringLiteral("AVI"), static_cast<int>(ContainerFormat::AVI));
}

void ExportPanel::populateAccel()
{
    m_accelCombo->addItem(QStringLiteral("Auto"), static_cast<int>(HardwareAccel::NVENC));
    m_accelCombo->addItem(QStringLiteral("CPU Only"), static_cast<int>(HardwareAccel::None));
    m_accelCombo->addItem(QStringLiteral("NVENC"), static_cast<int>(HardwareAccel::NVENC));
    m_accelCombo->addItem(QStringLiteral("Quick Sync"), static_cast<int>(HardwareAccel::QSV));
    m_accelCombo->addItem(QStringLiteral("AMF"), static_cast<int>(HardwareAccel::AMF));
}

void ExportPanel::onPresetChanged(int index)
{
    int data = m_presetCombo->itemData(index).toInt();
    bool isUserPreset = (data == -1);
    if (m_deletePresetAction) m_deletePresetAction->setEnabled(isUserPreset);

    if (isUserPreset) {
        // Load user preset from JSON
        QString name = m_presetCombo->itemText(index);
        QFile f(customPresetsDir() + QStringLiteral("/") + name + QStringLiteral(".json"));
        if (f.open(QIODevice::ReadOnly)) {
            QJsonObject obj = QJsonDocument::fromJson(f.readAll()).object();
            m_widthSpin->setValue(obj[QStringLiteral("width")].toInt(1920));
            m_heightSpin->setValue(obj[QStringLiteral("height")].toInt(1080));
            m_crfSlider->setValue(obj[QStringLiteral("crf")].toInt(50));
            // Restore the VIDEO / AUDIO enable switches (older presets only
            // stored "audio", so video defaults to on).
            if (m_videoSection && m_videoSection->toggle())
                m_videoSection->toggle()->setChecked(obj[QStringLiteral("video")].toBool(true));
            if (m_audioSection && m_audioSection->toggle())
                m_audioSection->toggle()->setChecked(obj[QStringLiteral("audio")].toBool(true));
            if (m_audioFormatCombo && obj.contains(QStringLiteral("audioFormat"))) {
                int af = obj[QStringLiteral("audioFormat")].toInt();
                for (int i = 0; i < m_audioFormatCombo->count(); ++i) {
                    if (m_audioFormatCombo->itemData(i).toInt() == af) {
                        m_audioFormatCombo->setCurrentIndex(i);
                        break;
                    }
                }
            }
            int fps = obj[QStringLiteral("fps")].toInt(30);
            for (int i = 0; i < m_fpsCombo->count(); ++i) {
                if (m_fpsCombo->itemData(i).toInt() == fps) { m_fpsCombo->setCurrentIndex(i); break; }
            }
            int codec = obj[QStringLiteral("codec")].toInt();
            for (int i = 0; i < m_codecCombo->count(); ++i) {
                if (m_codecCombo->itemData(i).toInt() == codec) { m_codecCombo->setCurrentIndex(i); break; }
            }
            int accel = obj[QStringLiteral("accel")].toInt();
            for (int i = 0; i < m_accelCombo->count(); ++i) {
                if (m_accelCombo->itemData(i).toInt() == accel) { m_accelCombo->setCurrentIndex(i); break; }
            }
            int container = obj[QStringLiteral("container")].toInt();
            for (int i = 0; i < m_containerCombo->count(); ++i) {
                if (m_containerCombo->itemData(i).toInt() == container) { m_containerCombo->setCurrentIndex(i); break; }
            }
            // Restore the codec profile AFTER the codec (setting the codec above
            // re-populated the profile combo via onCodecChanged).
            if (m_profileCombo && obj.contains(QStringLiteral("profile"))) {
                int profile = obj[QStringLiteral("profile")].toInt();
                for (int i = 0; i < m_profileCombo->count(); ++i) {
                    if (m_profileCombo->itemData(i).toInt() == profile) { m_profileCombo->setCurrentIndex(i); break; }
                }
            }
        }
    } else {
        auto preset = static_cast<ExportPreset>(data);
        if (preset != ExportPreset::Custom)
            updateUIFromPreset(preset);
    }
}

void ExportPanel::updateUIFromPreset(ExportPreset preset)
{
    ExportJobConfig cfg;
    cfg.applyPreset(preset);

    m_widthSpin->setValue(cfg.outputWidth);
    m_heightSpin->setValue(cfg.outputHeight);
    // Map CRF back to quality slider (0-100)
    {
        int crf = cfg.encoderConfig.crf;
        int q = ((35 - crf) * 100) / 21;
        q = std::clamp(q, 0, 100);
        m_crfSlider->setValue(q);
    }

    // Set codec combo
    for (int i = 0; i < m_codecCombo->count(); ++i) {
        if (m_codecCombo->itemData(i).toInt() == static_cast<int>(cfg.encoderConfig.codec)) {
            m_codecCombo->setCurrentIndex(i);
            break;
        }
    }

    // Set FPS combo — match on the exact rational rate so 24000/1001 selects
    // "23.976" and 24/1 selects "24" (item data is a double).
    const double cfgFps = static_cast<double>(cfg.encoderConfig.fpsNum) /
                          std::max(cfg.encoderConfig.fpsDen, 1);
    for (int i = 0; i < m_fpsCombo->count(); ++i) {
        if (std::abs(m_fpsCombo->itemData(i).toDouble() - cfgFps) < 0.01) {
            m_fpsCombo->setCurrentIndex(i);
            break;
        }
    }

    // Set container combo
    for (int i = 0; i < m_containerCombo->count(); ++i) {
        if (m_containerCombo->itemData(i).toInt() == cfg.containerFormat) {
            m_containerCombo->setCurrentIndex(i);
            break;
        }
    }
}

void ExportPanel::updateAlphaAvailability()
{
    if (!m_alphaCheck) return;
    auto codec = static_cast<EncoderCodec>(m_codecCombo->currentData().toInt());
    bool supports = false;
    if (codec == EncoderCodec::ProRes && m_profileCombo &&
        m_profileCombo->currentData().isValid()) {
        auto p = static_cast<ProResProfile>(m_profileCombo->currentData().toInt());
        supports = (p == ProResProfile::_4444 || p == ProResProfile::_4444XQ);
    } else if (codec == EncoderCodec::ImageSequence) {
        supports = true;   // PNG (the default image format) carries alpha
    }
    m_alphaCheck->setEnabled(supports);
    if (!supports) m_alphaCheck->setChecked(false);
}

bool ExportPanel::exportAlphaRequested() const
{
    // The checkbox is enabled only for alpha-capable targets, so isEnabled()
    // doubles as the capability gate.
    return m_alphaCheck && m_alphaCheck->isEnabled() && m_alphaCheck->isChecked();
}

void ExportPanel::onCodecChanged(int /*index*/)
{
    const int data = m_codecCombo->currentData().toInt();

    // ── Audio-only format chosen in the Format dropdown (MP3/AAC/WAV/FLAC) ──
    // Premiere-style: hide the VIDEO section, force the export to audio-only,
    // and point the AUDIO section's format at the chosen codec.
    if (isAudioFormatData(data)) {
        const auto ac = static_cast<AudioCodec>(data - kAudioFormatBase);
        if (m_audioFormatCombo) {
            for (int i = 0; i < m_audioFormatCombo->count(); ++i) {
                if (m_audioFormatCombo->itemData(i).toInt() == static_cast<int>(ac)) {
                    m_audioFormatCombo->setCurrentIndex(i);
                    break;
                }
            }
        }
        if (m_videoSection) m_videoSection->setVisible(false);
        if (m_videoSection && m_videoSection->toggle())
            m_videoSection->toggle()->setChecked(false);   // → audio-only
        if (m_audioSection) m_audioSection->setExpanded(true);
        updateAudioSectionState();
        applyOutputExtension(QString::fromLatin1(audioCodecExtension(ac)));
        updateFileEstimate();
        return;
    }

    // Video format: make sure the VIDEO section is shown + enabled (it may have
    // been hidden by a previous audio-format selection).
    if (m_videoSection) m_videoSection->setVisible(true);
    if (m_videoSection && m_videoSection->toggle() && !m_videoSection->toggle()->isChecked())
        m_videoSection->toggle()->setChecked(true);

    auto codec = static_cast<EncoderCodec>(data);

    // Codec profile combo: populate per-codec and show only for ProRes / DNxHR.
    if (m_profileCombo && m_profileLabel) {
        const bool isProRes = (codec == EncoderCodec::ProRes);
        const bool isDNxHR  = (codec == EncoderCodec::DNxHR);
        if (isProRes || isDNxHR) {
            m_profileCombo->blockSignals(true);
            m_profileCombo->clear();
            if (isProRes) {
                m_profileCombo->addItem(tr("Proxy"),         static_cast<int>(ProResProfile::Proxy));
                m_profileCombo->addItem(tr("LT"),            static_cast<int>(ProResProfile::LT));
                m_profileCombo->addItem(tr("422 Standard"),  static_cast<int>(ProResProfile::Standard));
                m_profileCombo->addItem(tr("422 HQ"),        static_cast<int>(ProResProfile::HQ));
                m_profileCombo->addItem(tr("4444"),          static_cast<int>(ProResProfile::_4444));
                m_profileCombo->addItem(tr("4444 XQ"),       static_cast<int>(ProResProfile::_4444XQ));
                m_profileCombo->setCurrentIndex(3);          // 422 HQ default
            } else {
                m_profileCombo->addItem(tr("LB (8-bit)"),    static_cast<int>(DNxHRProfile::LB));
                m_profileCombo->addItem(tr("SQ (8-bit)"),    static_cast<int>(DNxHRProfile::SQ));
                m_profileCombo->addItem(tr("HQ (8-bit)"),    static_cast<int>(DNxHRProfile::HQ));
                m_profileCombo->addItem(tr("HQX (10-bit)"),  static_cast<int>(DNxHRProfile::HQX));
                m_profileCombo->addItem(tr("444 (10-bit)"),  static_cast<int>(DNxHRProfile::_444));
                m_profileCombo->setCurrentIndex(2);          // HQ default
            }
            m_profileCombo->blockSignals(false);
        }
        m_profileLabel->setVisible(isProRes || isDNxHR);
        m_profileCombo->setVisible(isProRes || isDNxHR);
    }

    // ProRes, DNxHR and Image Sequence don't use CRF â€” disable the quality slider
    bool usesCrf = (codec != EncoderCodec::ProRes &&
                    codec != EncoderCodec::DNxHR &&
                    codec != EncoderCodec::ImageSequence);
    m_crfSlider->setEnabled(usesCrf);
    m_crfLabel->setEnabled(usesCrf);
    if (!usesCrf) {
        m_crfLabel->setText((codec == EncoderCodec::ProRes || codec == EncoderCodec::DNxHR)
            ? tr("N/A") : tr("N/A"));
    } else {
        onCrfChanged(m_crfSlider->value()); // Refresh label text
    }

    // Auto-switch container to match codec
    if (codec == EncoderCodec::ProRes || codec == EncoderCodec::DNxHR) {
        // ProRes/DNxHR â†’ MOV
        for (int i = 0; i < m_containerCombo->count(); ++i) {
            if (m_containerCombo->itemData(i).toInt() == static_cast<int>(ContainerFormat::MOV)) {
                m_containerCombo->setCurrentIndex(i);
                break;
            }
        }
    } else if (codec == EncoderCodec::AV1) {
        // AV1 â†’ WebM (modern) or MKV
        for (int i = 0; i < m_containerCombo->count(); ++i) {
            if (m_containerCombo->itemData(i).toInt() == static_cast<int>(ContainerFormat::WebM)) {
                m_containerCombo->setCurrentIndex(i);
                break;
            }
        }
    }

    // Hardware accel only works with H.264/H.265
    bool hwAvailable = (codec == EncoderCodec::H264 || codec == EncoderCodec::H265);
    m_accelCombo->setEnabled(hwAvailable);
    if (!hwAvailable) {
        // Set to CPU only
        for (int i = 0; i < m_accelCombo->count(); ++i) {
            if (m_accelCombo->itemData(i).toInt() == static_cast<int>(HardwareAccel::None)) {
                m_accelCombo->setCurrentIndex(i);
                break;
            }
        }
    }

    updateAlphaAvailability();

    // Keep the output extension in step with the chosen container (video mode).
    if (videoEnabled() && m_containerCombo)
        applyOutputExtension(QString::fromLatin1(containerFormatExtension(
            static_cast<ContainerFormat>(m_containerCombo->currentData().toInt()))));

    updateFileEstimate();
}

void ExportPanel::onCrfChanged(int value)
{
    // Map 0-100 slider to friendly label
    QString label;
    if      (value >= 88) label = tr("Best");
    else if (value >= 63) label = tr("High");
    else if (value >= 38) label = tr("Medium");
    else if (value >= 13) label = tr("Low");
    else                  label = tr("Lowest");
    m_crfLabel->setText(label);
}
} // namespace rt
