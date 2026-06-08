/*
 * SourceMonitorDrag.cpp -- eventFilter, source drag-out, drag/drop loading.
 * Extracted from SourceMonitor.cpp (behavior-preserving).
 */

#include "panels/monitors/SourceMonitor.h"
#include "panels/monitors/WaveformDisplayWidget.h"
#include "PathUtils.h"

#include "Theme.h"
#include "media/FrameCache.h"

#include "viewport/Viewport.h"
#include "widgets/MiniTimeline.h"
#include "widgets/TransportButton.h"
#include "media/PlaybackController.h"
#include "media/MediaPool.h"
#include "media/MediaSourceService.h"
#include "media/AudioFile.h"
#include "media/AudioEngine.h"
#include "media/AudioPlaybackService.h"
#include "media/AVSyncClock.h"
#include "timeline/Timeline.h"
#include "timeline/Track.h"
#include "timeline/AudioClip.h"
#include "timeline/SequenceClip.h"

#ifdef ROUNDTABLE_HAS_SPINE
#include "CompositeService.h"
#include "timeline/SpineClip.h"
#endif

#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QStackedLayout>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QDrag>
#include <QMimeData>
#include <QMouseEvent>
#include <QComboBox>
#include <QSettings>
#include <QPainter>
#include <QPointer>
#include <QResizeEvent>
#include <QTimer>
#include <QTreeWidget>
#include <QApplication>

#include <algorithm>
#include <cmath>
#include <thread>

namespace rt {

bool SourceMonitor::eventFilter(QObject* watched, QEvent* event)
{
    // Grab keyboard focus when user clicks anywhere in the Source Monitor
    // (viewport, mini-timeline, waveform) so JKL/Space route here.
    if (event->type() == QEvent::MouseButtonPress ||
        event->type() == QEvent::MouseButtonDblClick) {
        // Timecode label click → show editable timecode entry
        if (watched == m_timecodeLabel && event->type() == QEvent::MouseButtonPress) {
            m_timecodeLabel->hide();
            m_timecodeEdit->setText(m_timecodeLabel->text());
            m_timecodeEdit->show();
            m_timecodeEdit->setFocus();
            m_timecodeEdit->selectAll();
            return true;
        }
        setFocus();
    }

    // Forward drag events from the viewport child to our own handlers
    if (watched == m_viewport) {
        if (event->type() == QEvent::DragEnter) {
            dragEnterEvent(static_cast<QDragEnterEvent*>(event));
            return event->isAccepted();
        }
        if (event->type() == QEvent::DragMove) {
            dragMoveEvent(static_cast<QDragMoveEvent*>(event));
            return event->isAccepted();
        }
        if (event->type() == QEvent::Drop) {
            dropEvent(static_cast<QDropEvent*>(event));
            return event->isAccepted();
        }

        // Drag-out: click and drag from the viewport to the timeline
        if (event->type() == QEvent::MouseButtonPress) {
            auto* me = static_cast<QMouseEvent*>(event);
            if (me->button() == Qt::LeftButton && m_hasClip)
                m_dragStartPos = me->pos();
        }
        if (event->type() == QEvent::MouseMove) {
            auto* me = static_cast<QMouseEvent*>(event);
            if ((me->buttons() & Qt::LeftButton) && m_hasClip
                && !m_dragStartPos.isNull()
                && (me->pos() - m_dragStartPos).manhattanLength()
                       >= QApplication::startDragDistance()) {
                m_dragStartPos = QPoint();
                startSourceDrag(SourceDragMode::Both);
                return true;
            }
        }
    }

    // Drag-out from the dedicated video / audio drag buttons. A disabled
    // button (source lacks that stream) must not start a drag.
    if ((watched == m_btnDragVideo || watched == m_btnDragAudio) && m_hasClip
        && static_cast<QWidget*>(watched)->isEnabled()) {
        if (event->type() == QEvent::MouseButtonPress) {
            auto* me = static_cast<QMouseEvent*>(event);
            if (me->button() == Qt::LeftButton)
                m_dragBtnStartPos = me->pos();
        } else if (event->type() == QEvent::MouseMove) {
            auto* me = static_cast<QMouseEvent*>(event);
            if ((me->buttons() & Qt::LeftButton)
                && !m_dragBtnStartPos.isNull()
                && (me->pos() - m_dragBtnStartPos).manhattanLength()
                       >= QApplication::startDragDistance()) {
                m_dragBtnStartPos = QPoint();
                startSourceDrag(watched == m_btnDragVideo
                                    ? SourceDragMode::VideoOnly
                                    : SourceDragMode::AudioOnly);
                return true;
            }
        }
    }
    return QWidget::eventFilter(watched, event);
}

void SourceMonitor::startSourceDrag(SourceDragMode mode)
{
    if (!m_hasClip) return;

    auto region = sourceRegion();
    auto* mimeData = new QMimeData;

    if (m_isSequence) {
        // Sequence drag-out: encode sequence index + in/out
        mimeData->setData("application/x-roundtable-sequence",
                          QByteArray::number(qulonglong(m_sequenceIndex)));
        mimeData->setData("application/x-roundtable-sequence-duration",
                          QByteArray::number(qlonglong(m_clipDuration)));
        mimeData->setData("application/x-roundtable-source-in",
                          QByteArray::number(qlonglong(region.sourceIn)));
        mimeData->setData("application/x-roundtable-source-out",
                          QByteArray::number(qlonglong(region.sourceOut)));
        mimeData->setData("application/x-roundtable-sequence-has-audio",
                          QByteArray(m_seqHasAudio ? "1" : "0"));
    } else {
        auto filePath = m_mediaSources
            ? m_mediaSources->sourceInfo(m_mediaHandle).value().path
            : (m_pool ? m_pool->getPath(m_mediaHandle) : std::filesystem::path());
        if (filePath.empty()) { delete mimeData; return; }

        mimeData->setData("application/x-roundtable-media",
                          QByteArray::number(qulonglong(m_mediaHandle)));
        mimeData->setUrls({QUrl::fromLocalFile(
            QString::fromStdString(pathToUtf8(filePath)))});

        // Attach source in/out so the timeline can trim the clip
        mimeData->setData("application/x-roundtable-source-in",
                          QByteArray::number(qlonglong(region.sourceIn)));
        mimeData->setData("application/x-roundtable-source-out",
                          QByteArray::number(qlonglong(region.sourceOut)));
    }

    // Premiere-style video-only / audio-only drag flag. Absent = both.
    const char* modeStr = (mode == SourceDragMode::VideoOnly) ? "video"
                        : (mode == SourceDragMode::AudioOnly) ? "audio"
                        : nullptr;
    if (modeStr)
        mimeData->setData("application/x-roundtable-drag-mode",
                          QByteArray(modeStr));

    // Create a clean drag pixmap (Premiere-style pill)
    QString label = m_clipLabel ? m_clipLabel->text()
                                : QStringLiteral("Source");
    if (mode == SourceDragMode::VideoOnly)      label += QStringLiteral(" (V)");
    else if (mode == SourceDragMode::AudioOnly) label += QStringLiteral(" (A)");
    QFontMetrics fm(font());
    int textW = fm.horizontalAdvance(label);
    int pillW = qBound(120, textW + 24, 300);
    int pillH = 28;
    QPixmap pix(pillW, pillH);
    pix.fill(Qt::transparent);
    {
        const auto& tc = Theme::colors();
        QPainter p(&pix);
        p.setRenderHint(QPainter::Antialiasing);
        p.setBrush(QColor(tc.surface2));
        p.setPen(Qt::NoPen);
        p.drawRoundedRect(0, 0, pillW, pillH, 4, 4);
        // accent stripe – purple for sequences, blue for media
        p.setBrush(m_isSequence ? QColor("#7B68EE")
                                : QColor(tc.accent));
        p.drawRoundedRect(0, 0, 4, pillH, 2, 2);
        // text
        p.setPen(QColor(tc.text));
        p.drawText(QRect(10, 0, pillW - 14, pillH),
                   Qt::AlignVCenter | Qt::AlignLeft,
                   fm.elidedText(label, Qt::ElideRight, pillW - 18));
    }

    auto* drag = new QDrag(this);
    drag->setMimeData(mimeData);
    drag->setPixmap(pix);
    drag->setHotSpot(QPoint(pillW / 2, pillH / 2));
    drag->exec(Qt::CopyAction);
}

// Accept any drag that carries either our custom MIME, a tree-widget
// source, or a file URL. The URL clause is what makes NikkeBKG (whose
// BackgroundGridWidget is a QListWidget and only sets URLs +
// application/x-roundtable-asset) actually deliver a drop event; before
// this, dragEnterEvent silently rejected it and the user saw nothing.
// Also covers spine character drags (spine:URI), which the dropEvent
// dispatches to the spine-load path below.
static bool sourceMonitorAcceptsDrag(QDropEvent* e)
{
    const auto* md = e->mimeData();
    return md->hasFormat("application/x-roundtable-media") ||
           md->hasFormat("application/x-roundtable-sequence") ||
           md->hasFormat("application/x-roundtable-asset")   ||
           md->hasUrls()                                      ||
           qobject_cast<QTreeWidget*>(e->source());
}

void SourceMonitor::dragEnterEvent(QDragEnterEvent* event)
{
    if (sourceMonitorAcceptsDrag(event))
        event->acceptProposedAction();
}

void SourceMonitor::dragMoveEvent(QDragMoveEvent* event)
{
    if (sourceMonitorAcceptsDrag(event))
        event->acceptProposedAction();
}

void SourceMonitor::dropEvent(QDropEvent* event)
{
    // Opens a file path in the pool and loads it (Color Mattes are
    // generated PNGs that may not yet have a pool handle, so a drag only
    // carries their file URL). Returns true on success.
    auto loadFromPath = [this](const QString& path) -> bool {
        if (path.isEmpty() || !m_pool) return false;
        // Drop sources sometimes carry a relative path (NikkeBKG stored
        // "assets/NikkeBKG/foo.png"). Both MediaSourceService and
        // MediaPool canonicalise inputs and silently fail when the
        // resolved file doesn't exist relative to the process CWD. Make
        // the path absolute up-front so the drop works regardless of
        // where the app was launched from.
        std::filesystem::path p(path.toStdWString());
        if (p.is_relative()) {
            std::error_code ec;
            auto abs = std::filesystem::absolute(p, ec);
            if (!ec) p = std::move(abs);
        }
        // Prefer MediaSourceService — its handle is what updateFrameDisplay's
        // primary requestFrame() path keys off. A handle opened only via the
        // raw MediaPool isn't recognised by m_mediaSources, so the first
        // frame request after drop comes back empty and the viewport stays
        // blank until the user presses play (the bug the user described
        // for Characters / NikkeBKG / "sometimes Backgrounds").
        uint64_t h = 0;
        if (m_mediaSources) {
            auto res = m_mediaSources->openSource({p, RenderRequestType::Still, false});
            h = res.handle;
        }
        if (h == 0)
            h = m_pool->open(p);
        if (h == 0) return false;
        loadClip(h, m_pool);
        // Stills can miss the cache on the very first synchronous request.
        // A deferred second paint guarantees the frame lands once decoded.
        QTimer::singleShot(0, this, [this]() {
            if (!m_destroying.load(std::memory_order_acquire))
                updateFrameDisplay();
        });
        return true;
    };
    auto firstLocalFile = [](const QDropEvent* e) -> QString {
        for (const QUrl& u : e->mimeData()->urls())
            if (u.isLocalFile()) return u.toLocalFile();
        return {};
    };

    // Try custom mime type first (from ThumbnailGrid / BinTreeWidget)
    if (event->mimeData()->hasFormat("application/x-roundtable-media")) {
        bool ok = false;
        uint64_t handle = event->mimeData()->data("application/x-roundtable-media")
                              .toULongLong(&ok);

        if (ok && handle != 0) {
            if (m_pool) {
                loadClip(handle, m_pool);
                // Stills can miss the cache on the very first synchronous
                // request — same reason loadFromPath schedules a deferred
                // repaint below. Without this, dropping an image from the
                // bin shows a blank viewport until the user drops again.
                QTimer::singleShot(0, this, [this]() {
                    if (!m_destroying.load(std::memory_order_acquire))
                        updateFrameDisplay();
                });
            }
            else
                emit dropReceived(handle);
            event->acceptProposedAction();
            return;
        }

        // No valid pool handle (Library item / Color Matte). Try the file
        // URL the drag carries. If that doesn't load (e.g. the multi-select
        // drag packed a comma handle list and no usable URL, or a
        // Videos/Audio item whose URL round-trip differs), DON'T dead-end —
        // fall through to the QTreeWidget-source and bare-URL fallbacks
        // below, which read the dragged item's path directly.
        if (loadFromPath(firstLocalFile(event))) {
            event->acceptProposedAction();
            return;
        }
        // fall through (no return)
    }

    // Sequence via custom MIME (drag from another source)
    if (event->mimeData()->hasFormat("application/x-roundtable-sequence")) {
        bool ok = false;
        size_t seqIdx = event->mimeData()->data("application/x-roundtable-sequence")
                            .toULongLong(&ok);
        if (ok) {
            emit sequenceDropReceived(seqIdx);
            event->acceptProposedAction();
        }
        return;
    }

    // Fallback: QTreeWidget default drag provides model data but no custom mime.
    // The source QTreeWidget item has media handle in UserRole+1.
    if (auto* tree = qobject_cast<QTreeWidget*>(event->source())) {
        auto* item = tree->currentItem();
        if (!item) return;
        // Skip bins
        if (item->data(0, Qt::UserRole + 2).toBool()) return;

        // Handle sequences from the tree
        if (item->data(0, Qt::UserRole + 3).toBool()) {
            size_t seqIdx = item->data(0, Qt::UserRole + 4).toULongLong();
            emit sequenceDropReceived(seqIdx);
            event->acceptProposedAction();
            return;
        }

        uint64_t handle = item->data(0, Qt::UserRole + 1).toULongLong();
        if (handle == 0) {
            const QString role = item->data(0, Qt::UserRole).toString();
            // Spine character animation drag (CharactersPanel). The payload
            // isn't a real file path — route to TimelineWorkspace which
            // owns the CompositeService needed for live spine rendering.
            if (role.startsWith(QStringLiteral("spine:"))) {
                emit spineDropReceived(role);
                event->acceptProposedAction();
                return;
            }
            // Color Matte / generated asset with no pool handle yet —
            // open it from its stored path.
            if (loadFromPath(role))
                event->acceptProposedAction();
            return;
        }

        if (m_pool) {
            loadClip(handle, m_pool);
            // See note above in the custom-mime branch — stills miss the
            // first synchronous frame request; a deferred repaint guarantees
            // they appear on the initial drop.
            QTimer::singleShot(0, this, [this]() {
                if (!m_destroying.load(std::memory_order_acquire))
                    updateFrameDisplay();
            });
        }
        else
            emit dropReceived(handle);

        event->acceptProposedAction();
        return;
    }

    // Last resort: a plain file-URL drop (Color Matte PNG, external file).
    if (event->mimeData()->hasUrls() && loadFromPath(firstLocalFile(event)))
        event->acceptProposedAction();
}
} // namespace rt
