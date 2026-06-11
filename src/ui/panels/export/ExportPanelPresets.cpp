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


namespace rt {

void ExportPanel::populatePresets()
{
    m_presetCombo->addItem(tr("Custom"), static_cast<int>(ExportPreset::Custom));
    m_presetCombo->addItem(tr("YouTube 1080p 30fps"), static_cast<int>(ExportPreset::YouTube1080p30));
    m_presetCombo->addItem(tr("YouTube 1080p 60fps"), static_cast<int>(ExportPreset::YouTube1080p60));
    m_presetCombo->addItem(tr("YouTube 4K 30fps"), static_cast<int>(ExportPreset::YouTube4K30));
    m_presetCombo->addItem(tr("YouTube 4K 60fps"), static_cast<int>(ExportPreset::YouTube4K60));
    m_presetCombo->addItem(tr("Broadcast 1080i"), static_cast<int>(ExportPreset::Broadcast1080i));
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
    obj[QStringLiteral("audio")]     = m_audioCheck->isChecked();

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
    m_codecCombo->addItem(QStringLiteral("H.264"), static_cast<int>(EncoderCodec::H264));
    m_codecCombo->addItem(QStringLiteral("H.265 (HEVC)"), static_cast<int>(EncoderCodec::H265));
    m_codecCombo->addItem(QStringLiteral("AV1"), static_cast<int>(EncoderCodec::AV1));
    m_codecCombo->addItem(QStringLiteral("ProRes"), static_cast<int>(EncoderCodec::ProRes));
    m_codecCombo->addItem(QStringLiteral("DNxHR"), static_cast<int>(EncoderCodec::DNxHR));
    m_codecCombo->addItem(QStringLiteral("Image Sequence"), static_cast<int>(EncoderCodec::ImageSequence));
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
    if (m_deletePresetBtn) m_deletePresetBtn->setEnabled(isUserPreset);

    if (isUserPreset) {
        // Load user preset from JSON
        QString name = m_presetCombo->itemText(index);
        QFile f(customPresetsDir() + QStringLiteral("/") + name + QStringLiteral(".json"));
        if (f.open(QIODevice::ReadOnly)) {
            QJsonObject obj = QJsonDocument::fromJson(f.readAll()).object();
            m_widthSpin->setValue(obj[QStringLiteral("width")].toInt(1920));
            m_heightSpin->setValue(obj[QStringLiteral("height")].toInt(1080));
            m_crfSlider->setValue(obj[QStringLiteral("crf")].toInt(50));
            m_audioCheck->setChecked(obj[QStringLiteral("audio")].toBool(true));
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

    // Set FPS combo
    int fpsVal = static_cast<int>(cfg.encoderConfig.fpsNum / std::max(cfg.encoderConfig.fpsDen, 1));
    for (int i = 0; i < m_fpsCombo->count(); ++i) {
        if (m_fpsCombo->itemData(i).toInt() == fpsVal) {
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

void ExportPanel::onCodecChanged(int /*index*/)
{
    auto codec = static_cast<EncoderCodec>(m_codecCombo->currentData().toInt());

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
