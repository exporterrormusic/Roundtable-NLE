/*
 * ProjectBinSequence.cpp — Sequence creation for ProjectBin.
 * Extracted from ProjectBin.cpp (modularization phase).
 *
 * Contains: createNewSequence, createSequenceFromMedia, createColorMatte,
 * createBarsAndTone
 */

#include "QtHelpers.h"
#include "PathUtils.h"
#include "panels/project/ProjectBin.h"
#include "panels/project/ProjectBinInternal.h"
#include "Theme.h"
#include "widgets/MediaDragTreeWidget.h"
#include "widgets/ThumbnailGrid.h"
#include "decode/ThumbnailGenerator.h"
#include "project/Project.h"
#include "project/Settings.h"
#include "timeline/Timeline.h"
#include "timeline/Track.h"
#include "timeline/VideoClip.h"
#include "timeline/AudioClip.h"
#include "timeline/ImageClip.h"
#include "playback/MediaPool.h"
#include "dialogs/SequenceDialog.h"
#include "command/CommandStack.h"
#include "command/LambdaCommand.h"

#include <QColorDialog>
#include <QCoreApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFileInfo>
#include <QFormLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QImage>
#include <QProcess>
#include <QProgressDialog>
#include <QRegularExpression>
#include <QSpinBox>

#ifdef _WIN32
#include <windows.h>
#endif

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <filesystem>

namespace rt {

namespace {
// Write a solid-colour matte PNG of the given size. Returns false on
// failure. Shared by createColorMatte() and editColorMatte().
bool writeMattePng(const std::filesystem::path& path,
                   const QColor& color, int w, int h)
{
    if (w <= 0 || h <= 0) { w = 1920; h = 1080; }
    QImage img(w, h, QImage::Format_ARGB32_Premultiplied);
    img.fill(color);
    return img.save(QString::fromStdString(pathToUtf8(path)), "PNG");
}
} // namespace

bool ProjectBin::isAdjustmentLayer(const std::filesystem::path& path)
{
    return projectBinIsAdjustmentPath(path);
}

bool ProjectBin::isColorMatte(const std::filesystem::path& path)
{
    for (const auto& part : path) {
        std::string s = pathToUtf8(part);
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char c){ return std::tolower(c); });
        if (s == "mattes") return true;
    }
    return false;
}

bool ProjectBin::isBarsAndTone(const std::filesystem::path& path)
{
    for (const auto& part : path) {
        std::string s = pathToUtf8(part);
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char c){ return std::tolower(c); });
        if (s == "bars and tone") return true;
    }
    return false;
}

void ProjectBin::editColorMatte(const std::filesystem::path& mattePath)
{
    // Seed the picker with the matte's current colour and preserve its
    // dimensions so the edit is a pure recolour.
    QImage current(QString::fromStdString(pathToUtf8(mattePath)));
    QColor seed = current.isNull() ? QColor(Qt::white)
                                   : current.pixelColor(0, 0);
    int w = current.isNull() ? 1920 : current.width();
    int h = current.isNull() ? 1080 : current.height();

    QColor color = QColorDialog::getColor(
        seed, this, tr("Color Matte"), QColorDialog::ShowAlphaChannel);
    if (!color.isValid())
        return;

    if (!writeMattePng(mattePath, color, w, h)) {
        spdlog::error("Failed to rewrite color matte: {}", pathToUtf8(mattePath));
        QMessageBox::warning(this, tr("Error"),
                             tr("Failed to update color matte image."));
        return;
    }
    spdlog::info("ProjectBin: recoloured matte '{}'", pathToUtf8(mattePath));

    // Re-decode everywhere it's used (timeline/program monitor) — handled
    // by the MainWindow listener.
    emit mediaContentChanged(mattePath);

    // Refresh the bin's own thumbnail for this file.
    if (m_generator) m_generator->clearCache();
    if (m_grid) {
        for (auto& gi : m_grid->mutableItems())
            if (!gi.isFolder && gi.filePath == mattePath)
                gi.thumbnail.reset();
        m_grid->loadVisibleThumbnails();
    }
    syncListView();
}

// =============================================================================
//  Sequence creation
// =============================================================================

void ProjectBin::createNewSequence()
{
    // Show dialog FIRST — no project is created until user confirms
    SequenceDialog dlg(this);
    dlg.setWindowTitle(tr("New Sequence"));

    if (m_project) {
        dlg.setMediaProperties(
            m_project->settings().resolution().width,
            m_project->settings().resolution().height,
            m_project->settings().frameRate());
        dlg.setSequenceName(QString::fromStdString(m_project->nextSequenceName()));
    } else {
        dlg.setMediaProperties(1920, 1080, 30.0);
        dlg.setSequenceName(QStringLiteral("Sequence 1"));
    }

    if (dlg.exec() != QDialog::Accepted)
        return;

    QString seqName = dlg.sequenceName();
    uint32_t w = dlg.width();
    uint32_t h = dlg.height();
    double fps = dlg.frameRate();
    std::string name = seqName.toStdString();

    // Auto-create project if none exists, with the chosen settings
    if (!m_project) {
        auto* newProj = new Project();
        newProj->setName("Untitled");
        newProj->defaultSettings().setResolution(w, h);
        newProj->defaultSettings().setFrameRate(fps);
        // Name the default sequence what the user chose + give it its own settings
        if (newProj->sequenceCount() > 0 && newProj->sequence(0)) {
            newProj->sequence(0)->setName(name);
            newProj->sequence(0)->settings().setResolution(w, h);
            newProj->sequence(0)->settings().setFrameRate(fps);
        }
        emit projectCreated(newProj);
        if (!m_project) { delete newProj; return; }
        // Bin already reflects the project — just signal the sequence
        emit sequencesChanged();
        emit sequenceOpened(0);
    } else {
        // Existing project: add a new sequence with ITS OWN settings. Do NOT
        // touch other sequences — they are independent (Premiere-style). The
        // project default template is updated so the next New Sequence dialog
        // remembers this choice.
        m_project->defaultSettings().setResolution(w, h);
        m_project->defaultSettings().setFrameRate(fps);
        auto applyNewSeqSettings = [w, h, fps](Timeline* seq) {
            if (seq) {
                seq->settings().setResolution(w, h);
                seq->settings().setFrameRate(fps);
            }
        };
        if (m_commandStack) {
            size_t newIdx = m_project->sequenceCount();
            m_commandStack->execute(std::make_unique<LambdaCommand>(
                "Add Sequence '" + name + "'",
                [this, name, newIdx, applyNewSeqSettings]() {
                    if (m_destroying.load(std::memory_order_acquire)) return;
                    applyNewSeqSettings(m_project->addSequence(name));
                    syncListView();
                    emit sequencesChanged();
                    emit sequenceOpened(newIdx);
                },
                [this, newIdx]() {
                    if (m_destroying.load(std::memory_order_acquire)) return;
                    m_project->removeSequence(newIdx);
                    syncListView();
                    emit sequencesChanged();
                }));
        } else {
            applyNewSeqSettings(m_project->addSequence(name));
            syncListView();
            emit sequencesChanged();
            emit sequenceOpened(m_project->sequenceCount() - 1);
        }
    }
}

// -----------------------------------------------------------------------------
//  Create sequence from media (drag-to-create-sequence button)
// -----------------------------------------------------------------------------

void ProjectBin::createSequenceFromMedia(const std::filesystem::path& filePath)
{
    if (!m_project) return;

    // Determine media properties from the MediaPool
    uint32_t mediaW = 0, mediaH = 0;
    int mediaRotation = 0;
    double mediaFps = 30.0;
    double mediaDurationSec = 0.0;
    bool mediaHasAudio = false;

    if (m_pool) {
        uint64_t handle = m_pool->open(filePath);
        if (handle != 0) {
            const auto* info = m_pool->getInfo(handle);
            if (info) {
                mediaW = info->width;
                mediaH = info->height;
                mediaRotation = info->rotation;
                if (info->fps > 0.0) mediaFps = info->fps;
                mediaDurationSec = info->duration;
                mediaHasAudio = info->hasAudio;
            }
        }
    }

    // Default to 1920x1080 30fps if no media info
    if (mediaW == 0 || mediaH == 0) {
        mediaW = 1920;
        mediaH = 1080;
    }

    // The NEW sequence matches the media (Premiere-style). Other sequences are
    // untouched — settings are per-sequence. The new timeline's own settings
    // are assigned on builtTimeline below; the project default template is
    // updated so the next New Sequence dialog remembers this size.
    m_project->defaultSettings().setResolution(mediaW, mediaH);
    m_project->defaultSettings().setFrameRate(mediaFps);

    // Create a sequence named after the media file
    QString stem = QFileInfo(QString::fromStdString(pathToUtf8(filePath))).completeBaseName();
    if (stem.isEmpty()) stem = QStringLiteral("Sequence");
    std::string seqName = stem.toStdString();

    // Compute clip duration in ticks
    int64_t clipDuration = secondsToTicks(mediaDurationSec);
    if (clipDuration <= 0)
        clipDuration = secondsToTicks(5.0); // default 5 seconds

    // Determine media type from extension
    std::string ext = pathToUtf8(filePath.extension());
    for (auto& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    const bool isVideo = (ext == ".mp4" || ext == ".mov" || ext == ".mkv" ||
                          ext == ".webm" || ext == ".avi" || ext == ".m4v");
    const bool isAudio = (ext == ".wav" || ext == ".mp3" || ext == ".flac" ||
                          ext == ".ogg" || ext == ".m4a" || ext == ".aac" || ext == ".opus");
    const bool isImage = (ext == ".png" || ext == ".jpg" || ext == ".jpeg" ||
                          ext == ".bmp" || ext == ".gif" || ext == ".tga" ||
                          ext == ".tiff" || ext == ".webp");

    // Pre-build the timeline so undo/redo swaps it cleanly via
    // insertSequence/extractSequence (no dangling pointer window).
    auto builtTimeline = std::make_unique<Timeline>();
    builtTimeline->setName(seqName);
    // This sequence's own resolution/fps match the dragged clip.
    builtTimeline->settings().setResolution(mediaW, mediaH);
    builtTimeline->settings().setFrameRate(mediaFps);
    std::string fileStr = pathToUtf8(filePath);

    if (isVideo) {
        // Replace default V1+A1 with our populated tracks
        while (builtTimeline->trackCount() > 0)
            builtTimeline->removeTrack(0);

        Track* vTrack = builtTimeline->addVideoTrack("Video 1");
        auto vClip = std::make_unique<VideoClip>(fileStr);
        vClip->setTimelineIn(0);
        vClip->setDuration(clipDuration);
        vClip->setSourceIn(0);
        vClip->setSourceResolution(mediaW, mediaH);
        vClip->setSourceRotation(mediaRotation);
        vClip->setSourceFps(mediaFps);
        vClip->setSourceDuration(clipDuration);
        vClip->setLabel(QFileInfo(QString::fromStdString(fileStr))
                            .fileName().toStdString());
        vTrack->addClip(std::move(vClip));

        if (mediaHasAudio) {
            Track* aTrack = builtTimeline->addAudioTrack("Audio 1");
            auto aClip = std::make_unique<AudioClip>(fileStr);
            aClip->setTimelineIn(0);
            aClip->setDuration(clipDuration);
            aClip->setSourceIn(0);
            aClip->setSourceDuration(clipDuration);
            aClip->setLabel(QFileInfo(QString::fromStdString(fileStr))
                                .fileName().toStdString());
            aTrack->addClip(std::move(aClip));
        }
    } else if (isImage) {
        while (builtTimeline->trackCount() > 0)
            builtTimeline->removeTrack(0);

        Track* vTrack = builtTimeline->addVideoTrack("Video 1");
        auto iClip = std::make_unique<ImageClip>(fileStr);
        iClip->setTimelineIn(0);
        iClip->setDuration(clipDuration);
        iClip->setSourceIn(0);
        iClip->setSourceResolution(mediaW, mediaH);
        iClip->setLabel(QFileInfo(QString::fromStdString(fileStr))
                            .fileName().toStdString());
        vTrack->addClip(std::move(iClip));
    } else if (isAudio) {
        while (builtTimeline->trackCount() > 0)
            builtTimeline->removeTrack(0);

        Track* aTrack = builtTimeline->addAudioTrack("Audio 1");
        auto aClip = std::make_unique<AudioClip>(fileStr);
        aClip->setTimelineIn(0);
        aClip->setDuration(clipDuration);
        aClip->setSourceIn(0);
        aClip->setSourceDuration(clipDuration);
        aClip->setLabel(QFileInfo(QString::fromStdString(fileStr))
                            .fileName().toStdString());
        aTrack->addClip(std::move(aClip));
    }

    // Wrap in shared_ptr so we can move the unique_ptr through std::function captures
    auto sharedTimeline = std::make_shared<std::unique_ptr<Timeline>>(
        std::move(builtTimeline));

    size_t newIdx = m_project->sequenceCount();

    auto addSeqCmd = std::make_unique<LambdaCommand>(
        "Add Sequence '" + seqName + "'",
        [this, newIdx, sharedTimeline]() {
            if (m_destroying.load(std::memory_order_acquire)) return;
            m_project->insertSequence(newIdx, std::move(*sharedTimeline));
            *sharedTimeline = nullptr;
            syncListView();
            emit sequencesChanged();
            emit sequenceOpened(newIdx);
        },
        [this, newIdx, sharedTimeline]() {
            if (m_destroying.load(std::memory_order_acquire)) return;
            *sharedTimeline = m_project->extractSequence(newIdx);
            syncListView();
            emit sequencesChanged();
        });

    if (m_commandStack) {
        m_commandStack->execute(std::move(addSeqCmd));
    } else {
        m_project->insertSequence(newIdx, std::move(*sharedTimeline));
        *sharedTimeline = nullptr;
        syncListView();
        emit sequencesChanged();
        emit sequenceOpened(newIdx);
    }
}

// -----------------------------------------------------------------------------
//  Color Matte (Premiere Pro-style)
// -----------------------------------------------------------------------------

void ProjectBin::createColorMatte()
{
    // 1. Pick a color
    QColor color = QColorDialog::getColor(Qt::white, this,
                                          tr("Choose Color Matte Color"),
                                          QColorDialog::ShowAlphaChannel);
    if (!color.isValid())
        return;

    // 2. Ask for a name
    bool ok = false;
    QString name = QInputDialog::getText(this, tr("New Color Matte"),
                                         tr("Matte name:"),
                                         QLineEdit::Normal,
                                         QStringLiteral("Color Matte"), &ok);
    name = name.trimmed();
    if (!ok || name.isEmpty())
        return;

    // 3. Determine output directory
    std::filesystem::path matteDir;
    if (m_project && !m_project->filePath().empty()) {
        // Place alongside the project file
        matteDir = m_project->filePath().parent_path() / "Mattes";
    } else {
        // Fallback to user data directory
        matteDir = std::filesystem::path(userDataDir().toStdString()) / "Mattes";
    }
    std::filesystem::create_directories(matteDir);

    // 4. Generate a unique filename
    QString safeName = name;
    safeName.replace(QRegularExpression(R"([<>:"/\\|?*])"), QStringLiteral("_"));
    std::filesystem::path mattePath = matteDir / (safeName.toStdString() + ".png");
    {
        int counter = 1;
        while (std::filesystem::exists(mattePath)) {
            mattePath = matteDir / (safeName.toStdString() + "_" + std::to_string(counter++) + ".png");
        }
    }

    // 5. Create the solid-color PNG (1920x1080 like Premiere's default)
    if (!writeMattePng(mattePath, color, 1920, 1080)) {
        spdlog::error("Failed to save color matte: {}", pathToUtf8(mattePath));
        QMessageBox::warning(this, tr("Error"),
                             tr("Failed to save color matte image."));
        return;
    }

    // 6. Import the generated matte into the bin
    addFiles({mattePath});
    spdlog::info("ProjectBin: created color matte '{}' at {}",
                 name.toStdString(), pathToUtf8(mattePath));
}

// -----------------------------------------------------------------------------
//  Bars and Tone (Premiere Pro-style)
// -----------------------------------------------------------------------------

void ProjectBin::createBarsAndTone()
{
    const Settings defaults = m_project ? m_project->settings() : Settings{};
    const Resolution defaultResolution = defaults.resolution();

    auto nameTaken = [this](const QString& candidate) {
        if (!m_grid) return false;
        for (const auto& item : m_grid->items()) {
            if (item.isFolder) continue;
            const QString existing = item.displayName.isEmpty()
                ? QString::fromStdString(pathToUtf8(item.filePath.stem()))
                : item.displayName;
            if (existing.compare(candidate, Qt::CaseInsensitive) == 0)
                return true;
        }
        return false;
    };
    QString defaultName = QStringLiteral("Bars and Tone");
    for (int suffix = 2; nameTaken(defaultName); ++suffix)
        defaultName = QStringLiteral("Bars and Tone %1").arg(suffix);

    QDialog dialog(this);
    dialog.setWindowTitle(tr("New Bars and Tone"));
    dialog.setModal(true);
    auto* form = new QFormLayout(&dialog);

    auto* nameEdit = new QLineEdit(defaultName, &dialog);
    auto* widthSpin = new QSpinBox(&dialog);
    widthSpin->setRange(16, 16384);
    widthSpin->setValue(static_cast<int>(defaultResolution.width));
    auto* heightSpin = new QSpinBox(&dialog);
    heightSpin->setRange(16, 16384);
    heightSpin->setValue(static_cast<int>(defaultResolution.height));
    auto* fpsSpin = new QDoubleSpinBox(&dialog);
    fpsSpin->setRange(1.0, 240.0);
    fpsSpin->setDecimals(3);
    fpsSpin->setValue(defaults.frameRate());
    fpsSpin->setSuffix(tr(" fps"));
    auto* durationSpin = new QDoubleSpinBox(&dialog);
    durationSpin->setRange(0.1, 3600.0);
    durationSpin->setDecimals(1);
    durationSpin->setValue(10.0);
    durationSpin->setSuffix(tr(" sec"));
    auto* sampleRateSpin = new QSpinBox(&dialog);
    sampleRateSpin->setRange(8000, 192000);
    sampleRateSpin->setSingleStep(1000);
    sampleRateSpin->setValue(static_cast<int>(defaults.sampleRate()));
    sampleRateSpin->setSuffix(tr(" Hz"));
    auto* toneLabel = new QLabel(tr("1,000 Hz stereo at -12 dBFS"), &dialog);

    form->addRow(tr("Name:"), nameEdit);
    form->addRow(tr("Width:"), widthSpin);
    form->addRow(tr("Height:"), heightSpin);
    form->addRow(tr("Frame rate:"), fpsSpin);
    form->addRow(tr("Duration:"), durationSpin);
    form->addRow(tr("Audio sample rate:"), sampleRateSpin);
    form->addRow(tr("Reference tone:"), toneLabel);
    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    form->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    nameEdit->selectAll();
    nameEdit->setFocus();

    if (dialog.exec() != QDialog::Accepted) return;
    QString name = nameEdit->text().trimmed();
    if (name.isEmpty()) return;

    QString ffmpegPath;
    const QString appDir = QCoreApplication::applicationDirPath();
    const QStringList candidates = {
        appDir + QStringLiteral("/ffmpeg.exe"),
        appDir + QStringLiteral("/../../../third_party/ffmpeg/bin/ffmpeg.exe"),
        QStringLiteral("third_party/ffmpeg/bin/ffmpeg.exe"),
        QStringLiteral("tools/ffmpeg/ffmpeg.exe")
    };
    for (const QString& candidate : candidates) {
        QFileInfo info(candidate);
        if (info.exists() && info.isFile()) {
            ffmpegPath = info.absoluteFilePath();
            break;
        }
    }
    if (ffmpegPath.isEmpty()) {
        QMessageBox::warning(this, tr("Bars and Tone"),
            tr("FFmpeg could not be found, so Bars and Tone could not be generated."));
        return;
    }

    std::filesystem::path outputDir;
    if (m_project && !m_project->filePath().empty())
        outputDir = m_project->filePath().parent_path() / "Bars and Tone";
    else
        outputDir = utf8ToPath(userDataDir().toUtf8().toStdString())
            / "Bars and Tone";
    std::error_code directoryError;
    std::filesystem::create_directories(outputDir, directoryError);
    if (directoryError) {
        QMessageBox::warning(this, tr("Bars and Tone"),
            tr("The generated-media folder could not be created."));
        return;
    }

    QString safeName = name;
    safeName.replace(QRegularExpression(R"([<>:"/\\|?*])"),
                     QStringLiteral("_"));
    const std::string safeNameUtf8 = safeName.toUtf8().toStdString();
    std::filesystem::path outputPath =
        outputDir / utf8ToPath(safeNameUtf8 + ".mkv");
    for (int suffix = 2; std::filesystem::exists(outputPath); ++suffix) {
        outputPath = outputDir / utf8ToPath(
            safeNameUtf8 + " " + std::to_string(suffix) + ".mkv");
    }

    const int width = widthSpin->value();
    const int height = heightSpin->value();
    const double fps = fpsSpin->value();
    const double duration = durationSpin->value();
    const int sampleRate = sampleRateSpin->value();
    const QString videoSource = QStringLiteral(
        "smptehdbars=size=%1x%2:rate=%3")
        .arg(width).arg(height).arg(fps, 0, 'f', 3);
    // aevalsrc gives an exact full-scale-relative amplitude. 0.2511886432 is
    // 10^(-12/20), producing the standard -12 dBFS reference level.
    const QString toneSource = QStringLiteral(
        "aevalsrc=exprs=0.2511886432*sin(2*PI*1000*t)|"
        "0.2511886432*sin(2*PI*1000*t):s=%1:d=%2:c=stereo")
        .arg(sampleRate).arg(duration, 0, 'f', 3);
    const QString output = QString::fromStdString(pathToUtf8(outputPath));
    const QStringList args = {
        QStringLiteral("-hide_banner"), QStringLiteral("-loglevel"),
        QStringLiteral("error"), QStringLiteral("-y"),
        QStringLiteral("-f"), QStringLiteral("lavfi"),
        QStringLiteral("-i"), videoSource,
        QStringLiteral("-f"), QStringLiteral("lavfi"),
        QStringLiteral("-i"), toneSource,
        QStringLiteral("-map"), QStringLiteral("0:v:0"),
        QStringLiteral("-map"), QStringLiteral("1:a:0"),
        QStringLiteral("-t"), QString::number(duration, 'f', 3),
        QStringLiteral("-c:v"), QStringLiteral("ffv1"),
        QStringLiteral("-level"), QStringLiteral("3"),
        QStringLiteral("-g"), QStringLiteral("1"),
        QStringLiteral("-pix_fmt"), QStringLiteral("yuv444p10le"),
        QStringLiteral("-color_primaries"), QStringLiteral("bt709"),
        QStringLiteral("-color_trc"), QStringLiteral("bt709"),
        QStringLiteral("-colorspace"), QStringLiteral("bt709"),
        QStringLiteral("-c:a"), QStringLiteral("pcm_s24le"),
        QStringLiteral("-ar"), QString::number(sampleRate),
        QStringLiteral("-ac"), QStringLiteral("2"), output
    };

    QProcess process;
    process.setProcessChannelMode(QProcess::SeparateChannels);
#ifdef _WIN32
    process.setCreateProcessArgumentsModifier(
        [](QProcess::CreateProcessArguments* processArgs) {
            processArgs->flags |= CREATE_NO_WINDOW;
        });
#endif
    QProgressDialog progress(tr("Generating Bars and Tone..."), tr("Cancel"),
                             0, 0, this);
    progress.setWindowTitle(tr("Bars and Tone"));
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(0);
    progress.setAutoClose(false);

    connect(&progress, &QProgressDialog::canceled, &process, &QProcess::kill);
    process.start(ffmpegPath, args);
    if (!process.waitForStarted(5000)) {
        QMessageBox::warning(this, tr("Bars and Tone"),
            tr("FFmpeg failed to start: %1").arg(process.errorString()));
        return;
    }
    progress.show();
    while (process.state() != QProcess::NotRunning) {
        process.waitForFinished(50);
        QCoreApplication::processEvents();
    }
    progress.close();

    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0
        || !std::filesystem::exists(outputPath)) {
        const QString error = QString::fromUtf8(process.readAllStandardError());
        std::error_code removeError;
        std::filesystem::remove(outputPath, removeError);
        spdlog::error("ProjectBin: Bars and Tone generation failed: {}",
                      error.toStdString());
        QMessageBox::warning(this, tr("Bars and Tone"),
            error.isEmpty() ? tr("Bars and Tone generation was canceled or failed.")
                            : tr("Bars and Tone generation failed:\n%1").arg(error));
        return;
    }

    addFiles({outputPath});
    if (m_grid) {
        for (auto& item : m_grid->mutableItems()) {
            if (!item.isFolder && item.filePath == outputPath) {
                item.displayName = name;
                break;
            }
        }
        syncListView();
        if (!m_listView) syncIconView();
    }
    spdlog::info(
        "ProjectBin: created Bars and Tone '{}' ({}x{}, {:.3f} fps, {:.1f}s, {} Hz) at {}",
        name.toStdString(), width, height, fps, duration, sampleRate,
        pathToUtf8(outputPath));
}

// -----------------------------------------------------------------------------
//  Adjustment Layer (Premiere Pro-style)
// -----------------------------------------------------------------------------

void ProjectBin::createAdjustmentLayer()
{
    // Premiere prompts for a name; default to "Adjustment Layer" with a
    // numeric suffix when one already exists.
    auto makeUniqueName = [this](const QString& base) {
        QString candidate = base;
        auto exists = [this](const QString& name) {
            for (const auto& it : m_grid->items()) {
                if (it.isFolder) continue;
                if (!projectBinIsAdjustmentPath(it.filePath)) continue;
                if (it.displayName == name) return true;
            }
            return false;
        };
        int n = 1;
        while (exists(candidate))
            candidate = QStringLiteral("%1 %2").arg(base).arg(++n);
        return candidate;
    };

    QString defaultName = makeUniqueName(QStringLiteral("Adjustment Layer"));

    bool ok = false;
    QString name = QInputDialog::getText(this, tr("New Adjustment Layer"),
                                         tr("Name:"),
                                         QLineEdit::Normal,
                                         defaultName, &ok);
    name = name.trimmed();
    if (!ok || name.isEmpty())
        return;

    // Build a sentinel path that's an invalid Windows filename, so the grid's
    // path-dedup stays correct and there's no risk of collision with imported
    // media. The path also serializes cleanly through Project::BinItem.
    QString safeName = name;
    safeName.replace(QRegularExpression(R"([<>:"/\\|?*])"), QStringLiteral("_"));
    std::filesystem::path sentinel = std::filesystem::path(kAdjustmentSentinelDir) /
        (safeName.toStdString() + kAdjustmentSentinelExt);

    // Add via addRestoredItem so we control displayName + labelColor directly.
    auto doAdd = [this, sentinel, name]() {
        if (m_destroying.load(std::memory_order_acquire)) return;
        if (m_grid->hasItem(sentinel)) return;
        m_grid->addRestoredItem(sentinel, MediaType::Unknown, /*handle*/ 0,
                                /*itemId*/ 0, name, /*labelColor*/ 0xFFFFAA44);
        syncListView();
        if (!m_listView) syncIconView();
    };
    auto doRemove = [this, sentinel]() {
        if (m_destroying.load(std::memory_order_acquire)) return;
        m_grid->removeItem(sentinel);
        syncListView();
        if (!m_listView) syncIconView();
    };

    if (m_commandStack) {
        m_commandStack->execute(std::make_unique<LambdaCommand>(
            "Add Adjustment Layer '" + name.toStdString() + "'",
            doAdd, doRemove));
    } else {
        doAdd();
    }
    spdlog::info("ProjectBin: created adjustment layer '{}'", name.toStdString());
}

void ProjectBin::scaleClipsToResolution(Timeline* seq,
                                        const Resolution& from,
                                        const Resolution& to)
{
    // Intentionally a no-op.
    //
    // Clip positions are stored as pixel offsets from a fixed 1920×1080
    // reference and scaled to the output resolution at composite time
    // (CompositeServiceLayerBuild.cpp / OverlayMath.cpp), and clip scale
    // is applied on top of a resolution-independent cover/contain fit
    // (Compositor::buildViewportTransform).  Both are therefore already
    // resolution-independent: changing the sequence resolution preserves
    // the exact visual layout WITHOUT modifying any position/scale value.
    //
    // Rescaling them by the resolution ratio (as this previously did)
    // double-applies the scaling — zooming in when going up in resolution
    // and out when going down.  Leaving the values untouched is what keeps
    // every clip at the same on-screen position and size.
    (void)seq; (void)from; (void)to;
}

} // namespace rt
