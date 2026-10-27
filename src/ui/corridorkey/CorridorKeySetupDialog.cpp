/*
 * CorridorKeySetupDialog.cpp — see CorridorKeySetupDialog.h.
 */

#include "corridorkey/CorridorKeySetupDialog.h"

#include "corridorkey/CorridorKeyService.h"
#include "corridorkey/CorridorKeySetup.h"

#include "PathUtils.h"
#include "Theme.h"

#include <QDesktopServices>
#include <QHBoxLayout>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QUrl>
#include <QVBoxLayout>

namespace rt {

namespace ck = corridorkey;

CorridorKeySetupDialog::CorridorKeySetupDialog(CorridorKeyService* service, QWidget* parent)
    : QDialog(parent)
    , m_service(service)
{
    setWindowTitle(tr("CorridorKey"));
    setMinimumWidth(560);
    const auto& tc = Theme::colors();

    auto* layout = new QVBoxLayout(this);
    layout->setSpacing(10);

    auto* title = new QLabel(tr("Set up the CorridorKey AI keyer"), this);
    title->setStyleSheet(QStringLiteral("font-size: %1px; font-weight: bold;").arg(Theme::typography().sizeH2));
    layout->addWidget(title);

    auto* intro = new QLabel(tr(
        "CorridorKey, by Corridor Digital, is a neural green-screen keyer. It keeps hair, motion blur, "
        "thin cables and soft shadows that Ultra Key cannot, but it is slow, so it runs in the "
        "background on shots you have finished: in Ultra Key, click <b>Render with CorridorKey</b>. "
        "Ultra Key stays your live key while you edit."), this);
    intro->setWordWrap(true);
    layout->addWidget(intro);

    const QString folder = QString::fromStdString(pathToUtf8(service->layout().runtimeDir));
    auto* downloads = new QLabel(tr(
        "<b>This downloads about 8 GB, once:</b>"
        "<ul style='margin-left:-20px'>"
        "<li>The uv installer and a private copy of Python 3.12 — about 50 MB</li>"
        "<li>PyTorch for NVIDIA GPUs and other libraries — about 3.2 GB</li>"
        "<li>The CorridorKey model and the three models it builds on (NVIDIA C-RADIO, "
        "Microsoft MoGe, Google DeepMind RVM) — about 4.9 GB</li>"
        "</ul>"
        "Everything goes into <i>%1</i> and takes about 9.5 GB there; nothing is installed "
        "anywhere else, and uninstalling the app removes it. Plan for 20–60 minutes on a "
        "typical connection. You can keep editing while it downloads.").arg(folder.toHtmlEscaped()), this);
    downloads->setWordWrap(true);
    downloads->setTextFormat(Qt::RichText);
    layout->addWidget(downloads);

    QString gpuName;
    m_gpuProblem = CorridorKeySetup::checkGpu(&gpuName);
    m_gpuLabel = new QLabel(this);
    m_gpuLabel->setWordWrap(true);
    if (m_gpuProblem.isEmpty()) {
        m_gpuLabel->setText(tr("Needs an NVIDIA RTX 30-series or newer graphics card. This computer: %1 ✓")
                                .arg(gpuName.toHtmlEscaped()));
    } else {
        m_gpuLabel->setText(m_gpuProblem);
        m_gpuLabel->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::hex(tc.warning)));
    }
    layout->addWidget(m_gpuLabel);

    const QString licence = QString::fromStdString(pathToUtf8(service->layout().codeDir / "LICENSE"));
    auto* credits = new QLabel(tr(
        "CorridorKey © Corridor Digital, model by Zachary Harris — free to use, including for "
        "commercial work, under the <a href='%1'>CorridorKey Licence</a>. The foundation models keep "
        "their own licences (NVIDIA Open Model License, MIT, CC BY 4.0).")
        .arg(QUrl::fromLocalFile(licence).toString()), this);
    credits->setWordWrap(true);
    credits->setTextFormat(Qt::RichText);
    credits->setOpenExternalLinks(true);
    credits->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::hex(tc.textSecondary)));
    layout->addWidget(credits);

    m_stageLabel = new QLabel(this);
    m_stageLabel->setWordWrap(true);
    layout->addWidget(m_stageLabel);
    m_progress = new QProgressBar(this);
    m_progress->setRange(0, 1000);
    m_progress->setTextVisible(false);
    layout->addWidget(m_progress);

    auto* buttons = new QHBoxLayout;
    buttons->addStretch();
    m_cancelButton = new QPushButton(tr("Cancel Download"), this);
    m_closeButton = new QPushButton(tr("Not Now"), this);
    m_installButton = new QPushButton(tr("Download and Install"), this);
    m_installButton->setDefault(true);
    buttons->addWidget(m_cancelButton);
    buttons->addWidget(m_closeButton);
    buttons->addWidget(m_installButton);
    layout->addLayout(buttons);

    connect(m_installButton, &QPushButton::clicked, this, [this]() { m_service->startSetup(); });
    connect(m_cancelButton, &QPushButton::clicked, this, [this]() { m_service->cancelSetup(); });
    connect(m_closeButton, &QPushButton::clicked, this, &QDialog::close);
    connect(m_service, &CorridorKeyService::setupChanged, this, &CorridorKeySetupDialog::sync);
    sync();
}

void CorridorKeySetupDialog::sync()
{
    const bool running = m_service->isSetupRunning();
    const bool installed = m_service->isInstalled();
    const QString stage = m_service->setupStage();

    m_progress->setVisible(running || installed);
    m_progress->setValue(installed ? 1000 : int(m_service->setupProgress() * 1000.0));
    m_stageLabel->setVisible(!stage.isEmpty() || installed);
    if (installed) {
        m_stageLabel->setText(tr("<b>CorridorKey is ready.</b> Select a clip with Ultra Key and click "
                                 "Render with CorridorKey."));
    } else if (running) {
        m_stageLabel->setText(stage);
    } else {
        m_stageLabel->setText(stage);   // the last failure, if any
    }

    m_installButton->setVisible(!running && !installed);
    m_installButton->setEnabled(m_gpuProblem.isEmpty());
    m_installButton->setText(stage.isEmpty() ? tr("Download and Install") : tr("Try Again"));
    m_cancelButton->setVisible(running);
    m_closeButton->setText(running ? tr("Hide (keeps downloading)") : installed ? tr("Close") : tr("Not Now"));
}

} // namespace rt
