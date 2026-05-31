/*
 * AppPreferencesDialog.cpp — Application preferences implementation.
 */

#include "dialogs/AppPreferencesDialog.h"
#include "Theme.h"
#include "GpuContext.h"
#include "media/AudioEngine.h"
#include "media/VideoDecoder.h"
#include "HardwareDiagnostics.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QVBoxLayout>

#include <thread>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include "Settings.h"

namespace rt {

AppPreferencesDialog::AppPreferencesDialog(QWidget* parent,
                                           const std::vector<AudioDeviceInfo>& audioDevices)
    : QDialog(parent)
{
    setWindowTitle(tr("Preferences"));
    setMinimumWidth(500);

    auto* mainLayout = new QVBoxLayout(this);

    // ── Appearance ──────────────────────────────────────────────────────
    auto* appearanceGroup = new QGroupBox(tr("Appearance"), this);
    auto* appearanceForm = new QFormLayout(appearanceGroup);

    m_scrollbarWidthSpin = new QSpinBox(this);
    m_scrollbarWidthSpin->setRange(10, 28);
    m_scrollbarWidthSpin->setSuffix(tr(" px"));
    appearanceForm->addRow(tr("Scrollbar Width:"), m_scrollbarWidthSpin);

    mainLayout->addWidget(appearanceGroup);

    // ── General ─────────────────────────────────────────────────────────
    auto* generalGroup = new QGroupBox(tr("General"), this);
    auto* generalForm = new QFormLayout(generalGroup);

    m_autosaveSpin = new QSpinBox(this);
    m_autosaveSpin->setRange(1, 60);
    m_autosaveSpin->setSuffix(tr(" minutes"));
    generalForm->addRow(tr("Autosave Interval:"), m_autosaveSpin);

    mainLayout->addWidget(generalGroup);

    // ── Directories ─────────────────────────────────────────────────────
    auto* dirGroup = new QGroupBox(tr("Directories"), this);
    auto* dirForm = new QFormLayout(dirGroup);

    auto* projRow = new QHBoxLayout;
    m_projectsDirEdit = new QLineEdit(this);
    m_projectsDirEdit->setReadOnly(true);
    projRow->addWidget(m_projectsDirEdit, 1);
    auto* projBrowse = new QPushButton(tr("Browse..."), this);
    connect(projBrowse, &QPushButton::clicked, this, [this]() {
        QString dir = QFileDialog::getExistingDirectory(this,
            tr("Select Projects Directory"), m_projectsDirEdit->text());
        if (!dir.isEmpty()) m_projectsDirEdit->setText(dir);
    });
    projRow->addWidget(projBrowse);
    dirForm->addRow(tr("Projects Folder:"), projRow);

    auto* cacheRow = new QHBoxLayout;
    m_cacheDirEdit = new QLineEdit(this);
    m_cacheDirEdit->setReadOnly(true);
    cacheRow->addWidget(m_cacheDirEdit, 1);
    auto* cacheBrowse = new QPushButton(tr("Browse..."), this);
    connect(cacheBrowse, &QPushButton::clicked, this, [this]() {
        QString dir = QFileDialog::getExistingDirectory(this,
            tr("Select Cache Directory"), m_cacheDirEdit->text());
        if (!dir.isEmpty()) m_cacheDirEdit->setText(dir);
    });
    cacheRow->addWidget(cacheBrowse);
    dirForm->addRow(tr("Cache Folder:"), cacheRow);

    mainLayout->addWidget(dirGroup);

    // ── Audio ───────────────────────────────────────────────────────────
    auto* audioGroup = new QGroupBox(tr("Audio"), this);
    auto* audioForm = new QFormLayout(audioGroup);

    m_audioDeviceCombo = new QComboBox(this);
    m_audioDeviceCombo->addItem(tr("System Default"), -1);
    for (const auto& dev : audioDevices) {
        QString label = QString::fromStdString(dev.name);
        if (dev.isDefault) label += tr(" (default)");
        m_audioDeviceCombo->addItem(label, dev.index);
    }
    audioForm->addRow(tr("Output Device:"), m_audioDeviceCombo);

    mainLayout->addWidget(audioGroup);

    // ── Hardware Acceleration ───────────────────────────────────────────
    auto* hwGroup = new QGroupBox(tr("Hardware Acceleration"), this);
    auto* hwForm = new QFormLayout(hwGroup);

    m_hardwareDecodeCombo = new QComboBox(this);
    m_hardwareDecodeCombo->addItem(tr("Auto (prefer NVDEC / GPU)"), 0);
    m_hardwareDecodeCombo->addItem(tr("Software only"), 1);
    hwForm->addRow(tr("Video Decode:"), m_hardwareDecodeCombo);

    // CUDA availability status
    bool cudaOk = GpuContext::get().cudaAvailable();
    m_cudaStatusLabel = new QLabel(this);
    if (cudaOk) {
        m_cudaStatusLabel->setText(tr("✓ NVIDIA CUDA / NVDEC detected"));
        m_cudaStatusLabel->setStyleSheet("color: #64b96a;");
    } else {
        m_cudaStatusLabel->setText(tr("✗ NVIDIA CUDA not available — software fallback active"));
        m_cudaStatusLabel->setStyleSheet("color: #d96060;");
    }
    hwForm->addRow(tr("GPU Status:"), m_cudaStatusLabel);

    mainLayout->addWidget(hwGroup);

    // ── Performance (Boost mode) ────────────────────────────────────────
    auto* perfGroup = new QGroupBox(tr("Performance"), this);
    auto* perfLayout = new QVBoxLayout(perfGroup);

    m_boostCheck = new QCheckBox(
        tr("Boost mode — trade system resources for higher performance"), this);
    perfLayout->addWidget(m_boostCheck);

    // Detected machine tier (transparency: shows what Boost will scale).
    QString tierStr = tr("Unknown");
    {
        const auto& gi = GpuContext::get().device().gpuInfo();
        auto gpu = HardwareDiagnostics::classifyGpu(gi.vendorId, gi.deviceId,
                                                    gi.name, gi.vramSize);
        uint64_t ramBytes = 0;
#ifdef _WIN32
        MEMORYSTATUSEX ms{};
        ms.dwLength = sizeof(ms);
        if (GlobalMemoryStatusEx(&ms)) ramBytes = static_cast<uint64_t>(ms.ullTotalPhys);
#endif
        auto tier = HardwareDiagnostics::classifyMachine(
            gpu, ramBytes, std::thread::hardware_concurrency());
        tierStr = QString::fromLatin1(HardwareDiagnostics::machineTierName(tier));
    }

    auto* perfNote = new QLabel(
        tr("Detected machine tier: <b>%1</b>.<br>"
           "Default scales caches to your hardware automatically. Boost raises "
           "the GPU/CPU cache working set further for smoother scrubbing on "
           "capable machines, at the cost of higher VRAM/RAM use — best when the "
           "editor is your primary app. Takes effect after restart.").arg(tierStr),
        this);
    perfNote->setWordWrap(true);
    perfNote->setStyleSheet("color: #999;");
    perfLayout->addWidget(perfNote);

    mainLayout->addWidget(perfGroup);

    // ── Button box ──────────────────────────────────────────────────────
    mainLayout->addStretch();

    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    mainLayout->addWidget(buttons);

    connect(buttons, &QDialogButtonBox::accepted, this, [this]() {
        saveSettings();
        accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    loadSettings();
}

int AppPreferencesDialog::autosaveMinutes() const { return m_autosaveSpin->value(); }
QString AppPreferencesDialog::projectsDirectory() const { return m_projectsDirEdit->text(); }
QString AppPreferencesDialog::cacheDirectory() const { return m_cacheDirEdit->text(); }
int AppPreferencesDialog::scrollbarWidth() const { return m_scrollbarWidthSpin->value(); }
int AppPreferencesDialog::audioDeviceIndex() const
{
    return m_audioDeviceCombo ? m_audioDeviceCombo->currentData().toInt() : -1;
}

int AppPreferencesDialog::hardwareDecodeMode() const
{
    return m_hardwareDecodeCombo ? m_hardwareDecodeCombo->currentData().toInt() : 0;
}

void AppPreferencesDialog::loadSettings()
{
    auto s = rt::appSettings();
    m_autosaveSpin->setValue(s.value("AutosaveInterval", 5).toInt());
    m_scrollbarWidthSpin->setValue(s.value("ScrollbarWidth", 16).toInt());
    m_projectsDirEdit->setText(s.value("ProjectsDirectory").toString());
    m_cacheDirEdit->setText(s.value("CacheDirectory").toString());

    int hwMode = s.value("HardwareDecodeMode", 0).toInt();
    if (m_hardwareDecodeCombo) {
        for (int i = 0; i < m_hardwareDecodeCombo->count(); ++i) {
            if (m_hardwareDecodeCombo->itemData(i).toInt() == hwMode) {
                m_hardwareDecodeCombo->setCurrentIndex(i);
                break;
            }
        }
    }

    if (m_boostCheck)
        m_boostCheck->setChecked(s.value("performance/boostEnabled", false).toBool());

    int savedDevice = s.value("AudioDeviceIndex", -1).toInt();
    if (m_audioDeviceCombo) {
        for (int i = 0; i < m_audioDeviceCombo->count(); ++i) {
            if (m_audioDeviceCombo->itemData(i).toInt() == savedDevice) {
                m_audioDeviceCombo->setCurrentIndex(i);
                break;
            }
        }
    }
}

void AppPreferencesDialog::saveSettings()
{
    auto s = rt::appSettings();
    s.setValue("AutosaveInterval", m_autosaveSpin->value());
    s.setValue("ScrollbarWidth", m_scrollbarWidthSpin->value());
    if (!m_projectsDirEdit->text().isEmpty())
        s.setValue("ProjectsDirectory", m_projectsDirEdit->text());
    if (!m_cacheDirEdit->text().isEmpty())
        s.setValue("CacheDirectory", m_cacheDirEdit->text());
    if (m_audioDeviceCombo)
        s.setValue("AudioDeviceIndex", m_audioDeviceCombo->currentData().toInt());
    if (m_hardwareDecodeCombo) {
        int mode = m_hardwareDecodeCombo->currentData().toInt();
        s.setValue("HardwareDecodeMode", mode);
        // Apply immediately — affects all new VideoDecoder instances
        setForceSoftwareDecode(mode == 1);
    }
    if (m_boostCheck)
        s.setValue("performance/boostEnabled", m_boostCheck->isChecked());
}

} // namespace rt
