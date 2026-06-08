/*
 * ProjectBinSequence.cpp — Sequence creation for ProjectBin.
 * Extracted from ProjectBin.cpp (modularization phase).
 *
 * Contains: createNewSequence, createSequenceFromMedia, createColorMatte
 */

#include "QtHelpers.h"
#include "panels/project/ProjectBin.h"
#include "panels/project/ProjectBinInternal.h"
#include "Theme.h"
#include "widgets/MediaDragTreeWidget.h"
#include "widgets/ThumbnailGrid.h"
#include "media/ThumbnailGenerator.h"
#include "project/Project.h"
#include "project/Settings.h"
#include "timeline/Timeline.h"
#include "timeline/Track.h"
#include "timeline/VideoClip.h"
#include "timeline/AudioClip.h"
#include "timeline/ImageClip.h"
#include "media/MediaPool.h"
#include "dialogs/SequenceDialog.h"
#include "command/CommandStack.h"
#include "command/LambdaCommand.h"

#include <QColorDialog>
#include <QFileInfo>
#include <QInputDialog>
#include <QLineEdit>
#include <QMessageBox>
#include <QImage>
#include <QRegularExpression>

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
    return img.save(QString::fromStdString(path.string()), "PNG");
}
} // namespace

bool ProjectBin::isAdjustmentLayer(const std::filesystem::path& path)
{
    return projectBinIsAdjustmentPath(path);
}

bool ProjectBin::isColorMatte(const std::filesystem::path& path)
{
    for (const auto& part : path) {
        std::string s = part.string();
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char c){ return std::tolower(c); });
        if (s == "mattes") return true;
    }
    return false;
}

void ProjectBin::editColorMatte(const std::filesystem::path& mattePath)
{
    // Seed the picker with the matte's current colour and preserve its
    // dimensions so the edit is a pure recolour.
    QImage current(QString::fromStdString(mattePath.string()));
    QColor seed = current.isNull() ? QColor(Qt::white)
                                   : current.pixelColor(0, 0);
    int w = current.isNull() ? 1920 : current.width();
    int h = current.isNull() ? 1080 : current.height();

    QColor color = QColorDialog::getColor(
        seed, this, tr("Color Matte"), QColorDialog::ShowAlphaChannel);
    if (!color.isValid())
        return;

    if (!writeMattePng(mattePath, color, w, h)) {
        spdlog::error("Failed to rewrite color matte: {}", mattePath.string());
        QMessageBox::warning(this, tr("Error"),
                             tr("Failed to update color matte image."));
        return;
    }
    spdlog::info("ProjectBin: recoloured matte '{}'", mattePath.string());

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
    QString stem = QFileInfo(QString::fromStdString(filePath.string())).completeBaseName();
    if (stem.isEmpty()) stem = QStringLiteral("Sequence");
    std::string seqName = stem.toStdString();

    // Compute clip duration in ticks
    int64_t clipDuration = secondsToTicks(mediaDurationSec);
    if (clipDuration <= 0)
        clipDuration = secondsToTicks(5.0); // default 5 seconds

    // Determine media type from extension
    std::string ext = filePath.extension().string();
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
    std::string fileStr = filePath.string();

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
        spdlog::error("Failed to save color matte: {}", mattePath.string());
        QMessageBox::warning(this, tr("Error"),
                             tr("Failed to save color matte image."));
        return;
    }

    // 6. Import the generated matte into the bin
    addFiles({mattePath});
    spdlog::info("ProjectBin: created color matte '{}' at {}",
                 name.toStdString(), mattePath.string());
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
