#include "panels/comfy/ComfyPanel.h"

#include "PathUtils.h"
#include "decode/VideoDecoder.h"
#include "panels/audio/ShowSession.h"
#include "panels/audio/VideoFrameGrabber.h"

#include <QCheckBox>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileInfo>
#include <QLabel>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QProcess>
#include <QPushButton>
#include <QSlider>
#include <QSpinBox>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace rt {
namespace {

class MaskCanvas final : public QWidget
{
public:
    explicit MaskCanvas(QSize sourceSize) : mask(sourceSize, QImage::Format_ARGB32_Premultiplied)
    {
        mask.fill(Qt::white);
        setMinimumSize(240, 160);
        setMouseTracking(true);
        setCursor(Qt::CrossCursor);
        QPixmap tile(24, 24);
        tile.fill(QColor(95, 95, 95));
        QPainter p(&tile);
        p.fillRect(0, 0, 12, 12, QColor(130, 130, 130));
        p.fillRect(12, 12, 12, 12, QColor(130, 130, 130));
        checker = QBrush(tile);
    }

    QImage mask;
    QImage frame;
    int diameter{40}; // source pixels
    bool restore{false};
    std::function<void(bool)> onEdited;

    void undo()
    {
        if (strokes.empty()) return;
        dragging = false;
        strokes.pop_back();
        mask.fill(Qt::white);
        for (const auto& stroke : strokes) {
            drawSegment(stroke, stroke.points.front(), stroke.points.front());
            for (qsizetype i = 1; i < stroke.points.size(); ++i)
                drawSegment(stroke, stroke.points[i - 1], stroke.points[i]);
        }
        onEdited(!strokes.empty());
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.fillRect(rect(), Qt::black);
        if (frame.isNull()) {
            p.setPen(Qt::white);
            p.drawText(rect(), Qt::AlignCenter, QStringLiteral("Loading render frame…"));
            return;
        }
        const QRectF target = imageRect();
        p.fillRect(target, checker);
        QImage layer = frame.convertToFormat(QImage::Format_ARGB32_Premultiplied);
        QPainter composite(&layer);
        composite.setRenderHint(QPainter::SmoothPixmapTransform);
        composite.setCompositionMode(QPainter::CompositionMode_DestinationIn);
        composite.drawImage(layer.rect(), mask);
        composite.end();
        p.setRenderHint(QPainter::SmoothPixmapTransform);
        p.drawImage(target, layer);
        if (underMouse() && target.contains(pointer)) {
            p.setRenderHint(QPainter::Antialiasing);
            p.setClipRect(target);
            const double radius = diameter * target.width() / mask.width() / 2;
            p.setPen(QPen(Qt::black, 3));
            p.drawEllipse(pointer, radius, radius);
            p.setPen(QPen(Qt::white, 1));
            p.drawEllipse(pointer, radius, radius);
        }
    }

    void mousePressEvent(QMouseEvent* e) override
    {
        pointer = e->position();
        if (e->button() != Qt::LeftButton || frame.isNull() || !imageRect().contains(pointer)) return;
        dragging = true;
        const QPointF point = sourcePoint(pointer);
        strokes.push_back({{point}, diameter, restore});
        drawSegment(strokes.back(), point, point);
        onEdited(true);
        update();
    }

    void mouseMoveEvent(QMouseEvent* e) override
    {
        pointer = e->position();
        if (dragging) extendStroke(pointer);
        update();
    }

    void mouseReleaseEvent(QMouseEvent* e) override
    {
        if (dragging && e->button() == Qt::LeftButton) {
            extendStroke(e->position());
            dragging = false;
            update();
        }
    }

    void leaveEvent(QEvent*) override { update(); }

private:
    struct Stroke { QList<QPointF> points; int width; bool restore; };
    std::vector<Stroke> strokes;
    QBrush checker;
    QPointF pointer;
    bool dragging{false};

    QRectF imageRect() const
    {
        const QSizeF extent = QSizeF(mask.size()).scaled(size(), Qt::KeepAspectRatio);
        return QRectF((width() - extent.width()) / 2, (height() - extent.height()) / 2,
                      extent.width(), extent.height());
    }

    QPointF sourcePoint(QPointF point) const
    {
        const QRectF target = imageRect();
        return (point - target.topLeft()) * (mask.width() / target.width());
    }

    void extendStroke(QPointF position)
    {
        auto& stroke = strokes.back();
        const QPointF next = sourcePoint(position);
        if (stroke.points.back() == next) return;
        drawSegment(stroke, stroke.points.back(), next);
        stroke.points.push_back(next);
    }

    void drawSegment(const Stroke& stroke, QPointF start, QPointF end)
    {
        QPainter p(&mask);
        p.setRenderHint(QPainter::Antialiasing);
        if (!stroke.restore) p.setCompositionMode(QPainter::CompositionMode_DestinationOut);
        if (start == end) {
            p.setBrush(Qt::white);
            p.setPen(Qt::NoPen);
            p.drawEllipse(start, stroke.width / 2.0, stroke.width / 2.0);
        } else {
            p.setPen(QPen(Qt::white, stroke.width, Qt::SolidLine, Qt::RoundCap));
            p.drawLine(start, end);
        }
    }
};

} // namespace

void ComfyPanel::editRenderMask()
{
    const RenderVersion* selected = shownRender();
    if (!selected || framingMode()) return;
    stopShotPlayback();
    RenderVersion cleaned = *selected; // keep trim, tail pad and render settings
    const auto shotKey = m_shots[static_cast<size_t>(m_selected)].key;
    const QFileInfo source(QString::fromStdString(cleaned.file));
    const QString ffmpeg = findFfmpeg();
    VideoDecoder probe;
    if (ffmpeg.isEmpty() || !probe.open(utf8ToPath(cleaned.file), true)) {
        QMessageBox::warning(this, tr("Paint alpha mask"), tr("Could not open the render or find FFmpeg."));
        return;
    }
    const auto info = probe.info();
    probe.close();
    if (!info.hasAlpha || info.packedAlpha) {
        QMessageBox::information(this, tr("Paint alpha mask"), tr("Choose a render with a native alpha channel."));
        return;
    }

    QDialog dialog(this);
    dialog.setWindowTitle(tr("Paint alpha mask — %1").arg(source.fileName()));
    dialog.resize(900, 620);
    auto* layout = new QVBoxLayout(&dialog);
    auto* hint = new QLabel(tr("Paint over leftover background. The checkerboard shows transparency. "
                               "One mask applies to every frame."));
    hint->setWordWrap(true);
    layout->addWidget(hint);
    auto* canvas = new MaskCanvas(QSize(info.width, info.height));
    canvas->setObjectName(QStringLiteral("comfyMaskCanvas"));
    layout->addWidget(canvas, 1);
    auto* scrub = new QSlider(Qt::Horizontal);
    const double fps = info.fps > 0 ? info.fps : 24;
    const int lastFrame = std::max(0, static_cast<int>(std::ceil(info.duration * fps - 1e-6)) - 1);
    scrub->setRange(0, lastFrame);
    scrub->setValue(std::clamp(static_cast<int>(std::round(m_playhead * fps)), 0, lastFrame));
    scrub->setToolTip(tr("Scrub to check the same mask on another frame"));
    layout->addWidget(scrub);
    auto* controls = new QHBoxLayout;
    controls->addWidget(new QLabel(tr("Brush size")));
    auto* size = new QSpinBox;
    size->setRange(1, std::max(canvas->mask.width(), canvas->mask.height()));
    size->setValue(canvas->diameter);
    size->setSuffix(tr(" px"));
    controls->addWidget(size);
    auto* restore = new QCheckBox(tr("Restore"));
    restore->setToolTip(tr("Restore this render’s original transparency under the brush"));
    controls->addWidget(restore);
    auto* undo = new QPushButton(tr("Undo stroke"));
    undo->setAutoDefault(false);
    undo->setShortcut(QKeySequence::Undo);
    undo->setEnabled(false);
    controls->addWidget(undo);
    controls->addStretch();
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
    auto* save = buttons->button(QDialogButtonBox::Save);
    save->setText(tr("Save masked render"));
    save->setEnabled(false);
    controls->addWidget(buttons);
    layout->addLayout(controls);
    canvas->onEdited = [=](bool edited) { undo->setEnabled(edited); save->setEnabled(edited); };
    connect(size, &QSpinBox::valueChanged, canvas, [canvas](int v) { canvas->diameter = v; canvas->update(); });
    connect(restore, &QCheckBox::toggled, canvas, [canvas](bool checked) { canvas->restore = checked; });
    connect(undo, &QPushButton::clicked, canvas, &MaskCanvas::undo);
    const auto requestFrame = [this, canvas, file = cleaned.file, fps](int index) {
        m_grabber->requestLive(canvas, file, index / fps, QSize(1280, 960),
            [canvas](const QImage& img) { canvas->frame = img; canvas->update(); }, VideoFrameGrabber::Player);
    };
    connect(scrub, &QSlider::valueChanged, &dialog, requestFrame);
    requestFrame(scrub->value());

    QTemporaryDir scratch;
    QTemporaryFile output(source.dir().filePath(QStringLiteral(".mask-XXXXXX.mkv")));
    if (!scratch.isValid() || !output.open()) {
        QMessageBox::warning(this, dialog.windowTitle(), tr("Could not create temporary files for this render."));
        return;
    }
    output.close();
    QProcess encoder;
    const QString maskPath = scratch.filePath(QStringLiteral("mask.png"));
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        if (!canvas->mask.save(maskPath)) {
            QMessageBox::warning(&dialog, dialog.windowTitle(), tr("Could not write the mask image."));
            return;
        }
        save->setEnabled(false);
        canvas->setEnabled(false);
        scrub->setEnabled(false);
        undo->setEnabled(false);
        hint->setText(tr("Saving masked render…"));
        // The source is the primary input. Repeat the single PNG for its full duration.
        // Blend only alpha in 12-bit YUVA (the ComfyUI ProRes decoder's precision).
        QStringList args{"-nostdin", "-y", "-loglevel", "error", "-noautorotate"};
        // WebM stores transparency separately. The native VP8/9 decoders drop it.
        if (info.codecName == "vp9") args << "-c:v" << "libvpx-vp9";
        if (info.codecName == "vp8") args << "-c:v" << "libvpx";
        args << QStringList{
            "-i", source.absoluteFilePath(), "-i", maskPath, "-filter_complex",
            "[0:v:0]format=yuva444p12le[src];[1:v:0]format=yuva444p12le[mask];"
            "[src][mask]blend=c3_mode=multiply[v]", "-map", "[v]", "-map", "0:a?",
            "-fps_mode", "passthrough", "-c:v", "ffv1", "-level", "3", "-g", "1",
            "-c:a", "copy", output.fileName()};
        encoder.start(ffmpeg, args);
    });
    const auto failed = [&](const QString& error) {
        hint->setText(tr("Could not save the masked render. %1").arg(error.right(500)));
        canvas->setEnabled(true);
        scrub->setEnabled(true);
        undo->setEnabled(true);
        save->setEnabled(true);
    };
    connect(&encoder, &QProcess::errorOccurred, &dialog, [&](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) failed(encoder.errorString());
    });
    connect(&encoder, &QProcess::finished, &dialog, [&](int code, QProcess::ExitStatus status) {
        if (status == QProcess::NormalExit && code == 0) dialog.accept();
        else failed(QString::fromLocal8Bit(encoder.readAllStandardError()));
    });
    if (dialog.exec() != QDialog::Accepted) {
        encoder.disconnect(&dialog);
        if (encoder.state() != QProcess::NotRunning) { encoder.kill(); encoder.waitForFinished(); }
        return;
    }

    const QString base = source.dir().filePath(source.completeBaseName() + QStringLiteral("-masked"));
    QString finalPath = base + QStringLiteral(".mkv");
    for (int version = 2; QFileInfo::exists(finalPath); ++version)
        finalPath = base + QStringLiteral("-%1.mkv").arg(version);
    if (!output.rename(finalPath)) {
        QMessageBox::warning(this, dialog.windowTitle(), output.errorString());
        return;
    }
    output.setAutoRemove(false);
    cleaned.file = finalPath.toStdString();
    cleaned.renderedAt = QDateTime::currentSecsSinceEpoch();
    auto& settings = m_show->pipeline().shots[shotKey];
    settings.renders.push_back(std::move(cleaned));
    settings.chosenRender = static_cast<int>(settings.renders.size()) - 1;
    settings.review = ShotReview::Unreviewed;
    settings.reviewedRender.clear();
    rebuildRenderStrip();
    useRender(settings.chosenRender, true);
    emit renderRecorded();
}

} // namespace rt
