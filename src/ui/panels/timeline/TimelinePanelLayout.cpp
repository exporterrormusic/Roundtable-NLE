/*
 * TimelinePanelLayout.cpp — Layout construction for TimelinePanel.
 * Split from TimelinePanel.cpp for maintainability.
 *
 * Contains: setupLayout(), constructor, destructor, resizeEvent().
 */

#include "panels/timeline/TimelinePanel.h"
#include "Theme.h"
#include "widgets/TimelineRuler.h"
#include "widgets/TimelineTrackWidget.h"
#include "widgets/NLEScrollBar.h"

#include "panels/timeline/PlayheadLineWidget.h"
#include "panels/timeline/TimelinePanelInternal.h"
#include "MediaTaskQueue.h"

#include "timeline/Timeline.h"
#include "timeline/Track.h"

#include <QSplitter>
#include <QScrollBar>
#include <QTimer>
#include <QInputDialog>
#include <QLineEdit>

#include <spdlog/spdlog.h>

namespace rt {

// ═════════════════════════════════════════════════════════════════════════════
//  Constructor / Destructor
// ═════════════════════════════════════════════════════════════════════════════

TimelinePanel::TimelinePanel(QWidget* parent)
    : QWidget(parent)
{
    m_waveformTaskOwner = MediaTaskQueue::instance().createOwner();
    m_thumbnailTaskOwner = MediaTaskQueue::instance().createOwner();
    setAcceptDrops(true);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setupLayout();
}

TimelinePanel::~TimelinePanel()
{
    m_destroying.store(true);
    auto& tasks = MediaTaskQueue::instance();
    tasks.cancelOwner(m_waveformTaskOwner);
    tasks.cancelOwner(m_thumbnailTaskOwner);
}

// ═════════════════════════════════════════════════════════════════════════════
//  setupLayout — Builds the full widget hierarchy
// ═════════════════════════════════════════════════════════════════════════════

void TimelinePanel::setupLayout()
{
    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(0);

    // Top row: header spacer + ruler
    auto* topRow = new QHBoxLayout();
    topRow->setContentsMargins(0, 0, 0, 0);
    topRow->setSpacing(0);

    // The spacer must cover both the header scroll area width AND the
    // splitter handle (3px) so the ruler left edge aligns with the track
    // content viewport left edge.  Without this, the ruler and playhead
    // overlay are offset by the handle width.
    constexpr int kHandleW = 3; // must match m_headerSplitter->setHandleWidth()
    m_headerSpacer = new QWidget(this);
    m_headerSpacer->setFixedSize(TrackHeader::kHeaderWidth + kHandleW, TimelineRuler::kRulerHeight);
    m_headerSpacer->setStyleSheet(QStringLiteral("background-color: %1;")
        .arg(Theme::hex(Theme::colors().surface1)));

    m_ruler = new TimelineRuler(this);
    m_ruler->setLayoutEngine(&m_layoutEngine);

    topRow->addWidget(m_headerSpacer);
    topRow->addWidget(m_ruler, 1);
    mainLayout->addLayout(topRow);

    // Middle row: track headers + track content via resizable QSplitter
    m_trackHeaderArea = new QWidget();
    auto* headerLayout = new QVBoxLayout(m_trackHeaderArea);
    headerLayout->setContentsMargins(0, 0, 0, 0);
    headerLayout->setSpacing(0);

    m_headerScroll = new QScrollArea(this);
    m_headerScroll->setWidgetResizable(true);
    m_headerScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_headerScroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_headerScroll->setFrameShape(QFrame::NoFrame);
    // Lock header to kHeaderWidth during initial layout; unlocked on first resize
    m_headerScroll->setFixedWidth(TrackHeader::kHeaderWidth);
    m_headerScroll->setWidget(m_trackHeaderArea);

    // Track content area (inside scroll area)
    m_verticalScroll = new QScrollArea(this);
    m_verticalScroll->setWidgetResizable(true);
    m_verticalScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_verticalScroll->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_verticalScroll->setFrameShape(QFrame::NoFrame);

    m_trackContentArea = new QWidget();
    auto* trackLayout = new QVBoxLayout(m_trackContentArea);
    trackLayout->setContentsMargins(0, 0, 0, 0);
    trackLayout->setSpacing(0);

    m_verticalScroll->setWidget(m_trackContentArea);

    // Install event filter on the vertical scroll area (and its viewport)
    // so wheel events are forwarded to TimelinePanel for zoom/scroll,
    // and mouse events on empty track-area space trigger marquee selection.
    m_verticalScroll->installEventFilter(this);
    m_verticalScroll->viewport()->installEventFilter(this);
    m_trackContentArea->installEventFilter(this);
    m_trackHeaderArea->installEventFilter(this);

    // Enable mouse-tracking on the viewport and track-content widget so
    // cursor feedback (arrow, cross-hair, etc.) works when hovering empty
    // space that is not covered by a TimelineTrackWidget.
    m_verticalScroll->viewport()->setMouseTracking(true);
    m_trackContentArea->setMouseTracking(true);
    setMouseTracking(true);

    // Synchronise vertical scroll positions between header and content areas.
    connect(m_verticalScroll->verticalScrollBar(), &QScrollBar::valueChanged,
            m_headerScroll->verticalScrollBar(), &QScrollBar::setValue);
    connect(m_headerScroll->verticalScrollBar(), &QScrollBar::valueChanged,
            m_verticalScroll->verticalScrollBar(), &QScrollBar::setValue);

    // Resizable splitter between header column and track content
    m_headerSplitter = new QSplitter(Qt::Horizontal, this);
    m_headerSplitter->setChildrenCollapsible(false);
    m_headerSplitter->addWidget(m_headerScroll);
    m_headerSplitter->addWidget(m_verticalScroll);
    m_headerSplitter->setStretchFactor(0, 0); // header side: don't stretch
    m_headerSplitter->setStretchFactor(1, 1); // content side: stretch

    // Style the splitter: dark background matching theme, thin black handle
    m_headerSplitter->setHandleWidth(3);
    m_headerSplitter->setStyleSheet(QStringLiteral(
        "QSplitter { background: #16161e; }"
        "QSplitter::handle { background: #000000; }"));

    // Keep header spacer and scroll spacer in sync with the splitter.
    // Spacer must cover header width + handle so ruler aligns with viewport.
    connect(m_headerSplitter, &QSplitter::splitterMoved, this, [this](int, int) {
        if (m_destroying.load(std::memory_order_acquire)) return;
        const int hw = m_headerScroll->width();
        const int handle = m_headerSplitter->handleWidth();
        m_headerSpacer->setFixedWidth(hw + handle);
        m_scrollSpacer->setFixedWidth(hw + handle);
        // Re-layout track headers to match new width
        m_trackHeaderArea->setFixedWidth(hw);
        for (auto hdr : m_trackHeaders)
            hdr->setFixedWidth(hw);
        // Reposition playhead overlay — the track-content viewport changed
        // width so the viewport-relative X coordinate must be recalculated.
        QTimer::singleShot(0, this, [this]() {
            if (m_destroying.load(std::memory_order_acquire)) return;
            onScrollChanged();
        });
    });

    mainLayout->addWidget(m_headerSplitter, 1);

    // Bottom row: NLE scrollbar
    auto* bottomRow = new QHBoxLayout();
    bottomRow->setContentsMargins(0, 0, 0, 0);
    bottomRow->setSpacing(0);

    m_scrollSpacer = new QWidget(this);
    m_scrollSpacer->setFixedSize(TrackHeader::kHeaderWidth + kHandleW, NLEScrollBar::kBarHeight);
    m_scrollSpacer->setStyleSheet(QStringLiteral("background-color: %1;")
        .arg(Theme::hex(Theme::colors().surface2)));

    m_scrollBar = new NLEScrollBar(this);
    m_scrollBar->setLayoutEngine(&m_layoutEngine);

    bottomRow->addWidget(m_scrollSpacer);
    bottomRow->addWidget(m_scrollBar, 1);
    mainLayout->addLayout(bottomRow);

    // Connections
    connect(m_ruler, &TimelineRuler::playheadScrubbed,
            this, &TimelinePanel::onRulerScrub);
    connect(m_ruler, &TimelineRuler::markerRequested,
            this, [this](int64_t tick) {
        if (!m_timeline) return;
        bool ok = false;
        QString name = QInputDialog::getText(
            this, "Add Marker",
            "Marker name:",
            QLineEdit::Normal, QString(), &ok);
        if (ok) {
            m_timeline->addMarker(tick, name.toStdString(), 0xFF44FF44);
            m_ruler->setMarkers(&m_timeline->markers());
            m_ruler->update();
            emit contentChanged();
        }
    });
    connect(m_scrollBar, &NLEScrollBar::scrollChanged,
            this, &TimelinePanel::onScrollChanged);

    // Keep the scrollbar range in sync when clips are added/moved/removed.
    connect(this, &TimelinePanel::contentChanged, this, [this]() {
        if (m_timeline)
            m_layoutEngine.setTotalDuration(m_timeline->duration());
        onScrollChanged();
    });

    // Sync selection highlights to track widgets whenever selection changes.
    // This handler is AUTHORITATIVE: it fully reconciles both the per-clip
    // highlight and the per-track transition highlight from m_selection and
    // m_selectedTransitionTrack/Index every time.  Any code path that wants
    // to change selection just mutates those members and emits
    // selectionChanged() — it does not need to poke widgets itself.  A clip
    // selection and a transition selection are mutually exclusive, so a
    // non-empty clip selection forcibly clears the transition selection.
    connect(this, &TimelinePanel::selectionChanged, this, [this]() {
        if (!m_timeline) return;
        const bool hasClipSelection = !m_selection.empty();
        if (hasClipSelection) {
            m_selectedTransitionTrack = SIZE_MAX;
            m_selectedTransitionIndex = SIZE_MAX;
        }
        for (size_t ti = 0; ti < m_trackWidgets.size(); ++ti) {
            std::vector<size_t> selectedIndices;
            Track* trk = m_timeline->track(ti);
            if (trk) {
                for (const auto& ref : m_selection.clips()) {
                    if (ref.trackIndex == ti) {
                        size_t idx = trk->findClipIndexById(ref.clipId);
                        if (idx < trk->clipCount())
                            selectedIndices.push_back(idx);
                    }
                }
            }
            m_trackWidgets[ti]->setSelectedClips(selectedIndices);
            // Always reconcile the transition highlight from the single
            // source of truth (m_selectedTransitionTrack/Index) so a
            // stale highlight can never linger on any track.
            m_trackWidgets[ti]->setSelectedTransition(
                (ti == m_selectedTransitionTrack)
                    ? m_selectedTransitionIndex
                    : SIZE_MAX);
        }
    });

    // Ghost track overlay (not in any layout — positioned absolutely via setGeometry).
    // A second instance handles the companion side of a video+audio drag, so we
    // can show individual clip outlines on BOTH the video destination row AND
    // the audio destination row simultaneously when dragging multi-clip groups.
    m_ghostOverlay = new GhostTrackOverlay(this);
    m_ghostOverlayAudio = new GhostTrackOverlay(this);

    // Playhead overlay — a transparent widget parented to the scroll viewport
    // that draws only the thin playhead line.  Repositioning this tiny widget
    // is vastly cheaper than repainting every TimelineTrackWidget (which each
    // redraw 100+ clip rectangles) on every tick.
    m_playheadOverlay = new PlayheadLineWidget(m_verticalScroll->viewport());
    m_playheadOverlay->raise();
}

// ═════════════════════════════════════════════════════════════════════════════
//  resizeEvent
// ═════════════════════════════════════════════════════════════════════════════

void TimelinePanel::resizeEvent(QResizeEvent* event)
{
    // Guard against resize → layout → paint → resize recursion during
    // initial dock arrangement.  If the QSplitter triggers another resize
    // while we're already handling one, skip it to avoid infinite loops.
    static thread_local bool s_inResize = false;
    if (s_inResize) {
        QWidget::resizeEvent(event);
        return;
    }
    s_inResize = true;

    QWidget::resizeEvent(event);
    if (!m_splitterInitialized && m_headerSplitter && m_headerSplitter->width() > 0) {
        m_splitterInitialized = true;
        // Header was locked at kHeaderWidth via setFixedWidth. Now unlock for
        // user-draggable resizing, and pin the splitter at the correct position.
        int headerW = m_headerScroll->width();
        int total = m_headerSplitter->width();
        int hw = m_headerSplitter->handleWidth();
        m_headerScroll->setMinimumWidth(TrackHeader::kHeaderWidth);
        m_headerScroll->setMaximumWidth(QWIDGETSIZE_MAX);
        m_headerSplitter->setSizes({headerW, std::max(1, total - headerW - hw)});
        // Apply dynamic minimum based on track names
        updateMinHeaderWidth();
    }
    m_layoutEngine.setViewportWidth(m_ruler->width());
    m_scrollBar->update();
    m_ruler->update();

    s_inResize = false;
}

} // namespace rt
