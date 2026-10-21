/*
 * MiniWaveformWidget.cpp — compact waveform display with trim handles,
 * shift+drag crop regions, and drag-past-edge auto-expand.
 */

#include "widgets/MiniWaveformWidget.h"
#include "Theme.h"

#include <QPainter>
#include <QMouseEvent>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

namespace rt {

MiniWaveformWidget::MiniWaveformWidget(QWidget* parent)
    : QWidget(parent)
{
    setMinimumHeight(30);
    setMaximumHeight(60);
    setFixedHeight(50);
    setCursor(Qt::ArrowCursor);
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
}

void MiniWaveformWidget::setAudio(const std::vector<float>& allSamples,
                                   uint32_t sampleRate,
                                   double clipStart, double clipEnd)
{
    m_samples    = SampleBuffer(allSamples);
    m_sampleRate = sampleRate;
    m_clipStart  = clipStart;
    m_clipEnd    = clipEnd;

    // Set view window with padding around the clip
    double dur = fileDuration();
    m_viewStart = std::max(0.0, clipStart - kContextPadding);
    m_viewEnd   = std::min(dur, clipEnd + kContextPadding);

    // Default trim = clip boundaries
    m_trimIn  = clipStart;
    m_trimOut = clipEnd;

    m_playhead = clipStart;
    m_playheadVisible = true;

    m_silencesComputed = false;
    // detectSilences deferred to first paint
    update();
}

void MiniWaveformWidget::setAudioShared(const SampleBuffer& samples,
                                         uint32_t sampleRate,
                                         double clipStart, double clipEnd)
{
    m_samples    = samples;   // shares the caller's samples
    m_sampleRate = sampleRate;
    m_clipStart  = clipStart;
    m_clipEnd    = clipEnd;

    double dur = fileDuration();
    m_viewStart = std::max(0.0, clipStart - kContextPadding);
    m_viewEnd   = std::min(dur, clipEnd + kContextPadding);

    m_trimIn  = clipStart;
    m_trimOut = clipEnd;

    m_playhead = clipStart;
    m_playheadVisible = true;

    m_silencesComputed = false;
    // detectSilences deferred to first paint
    update();
}

void MiniWaveformWidget::setTrimRange(double inPoint, double outPoint)
{
    m_trimIn  = inPoint;
    m_trimOut = outPoint;
    // Auto-expand view to contain trim points
    expandViewIfNeeded(inPoint);
    expandViewIfNeeded(outPoint);
    update();
}

void MiniWaveformWidget::setPlayhead(double timeSec)
{
    m_playhead = timeSec;
    update();  // Always repaint — the widget is small so the cost is trivial
}

void MiniWaveformWidget::setDeletedRegions(const std::vector<std::pair<double,double>>& regions)
{
    m_deletedRegions = regions;
    mergeDeletedRegions();
    update();
}

std::vector<std::pair<double,double>> MiniWaveformWidget::effectiveRanges() const
{
    // Compute playable sub-ranges: clip [trimIn..trimOut] minus deleted regions
    std::vector<std::pair<double,double>> ranges;
    if (m_deletedRegions.empty()) {
        ranges.emplace_back(m_trimIn, m_trimOut);
        return ranges;
    }

    // Sorted copy of deleted regions clipped to trim range
    auto deleted = m_deletedRegions;
    std::sort(deleted.begin(), deleted.end());

    double cur = m_trimIn;
    for (const auto& [ds, de] : deleted) {
        double s = std::max(ds, m_trimIn);
        double e = std::min(de, m_trimOut);
        if (s >= e) continue;
        if (cur < s - 0.001)
            ranges.emplace_back(cur, s);
        cur = e;
    }
    if (cur < m_trimOut - 0.001)
        ranges.emplace_back(cur, m_trimOut);
    return ranges;
}

// ── Helpers ──────────────────────────────────────────────────────────────

double MiniWaveformWidget::fileDuration() const
{
    return (m_sampleRate > 0) ? static_cast<double>(sam().size()) / m_sampleRate : 0.0;
}

void MiniWaveformWidget::expandViewIfNeeded(double timeSec)
{
    double dur = fileDuration();
    if (timeSec < m_viewStart + kEdgeExpand)
        m_viewStart = std::max(0.0, timeSec - kContextPadding);
    if (timeSec > m_viewEnd - kEdgeExpand)
        m_viewEnd = std::min(dur, timeSec + kContextPadding);
}

void MiniWaveformWidget::mergeDeletedRegions()
{
    if (m_deletedRegions.size() <= 1) return;
    std::sort(m_deletedRegions.begin(), m_deletedRegions.end());
    std::vector<std::pair<double,double>> merged;
    merged.push_back(m_deletedRegions[0]);
    for (size_t i = 1; i < m_deletedRegions.size(); ++i) {
        auto& last = merged.back();
        if (m_deletedRegions[i].first <= last.second + 0.05) {
            last.second = std::max(last.second, m_deletedRegions[i].second);
        } else {
            merged.push_back(m_deletedRegions[i]);
        }
    }
    m_deletedRegions = std::move(merged);
}

// ── Coordinate conversion ────────────────────────────────────────────────

double MiniWaveformWidget::pixelToTime(int x) const
{
    double viewDuration = m_viewEnd - m_viewStart;
    if (viewDuration <= 0.0 || width() <= 0) return m_viewStart;
    return m_viewStart + static_cast<double>(x) / width() * viewDuration;
}

int MiniWaveformWidget::timeToPixel(double t) const
{
    double viewDuration = m_viewEnd - m_viewStart;
    if (viewDuration <= 0.0) return 0;
    return static_cast<int>((t - m_viewStart) / viewDuration * width());
}

// ── Silence detection ────────────────────────────────────────────────────

void MiniWaveformWidget::detectSilences()
{
    m_silences.clear();
    m_silencesComputed = true;

    const auto& s = sam();
    if (s.empty() || m_sampleRate == 0) return;

    int windowSize = static_cast<int>(0.05 * m_sampleRate);
    if (windowSize <= 0) return;

    int startSample = static_cast<int>(m_clipStart * m_sampleRate);
    int endSample   = static_cast<int>(m_clipEnd * m_sampleRate);
    startSample = std::clamp(startSample, 0, static_cast<int>(s.size()));
    endSample   = std::clamp(endSample,   0, static_cast<int>(s.size()));

    bool inSilence = false;
    double silStart = 0.0;

    for (int pos = startSample; pos < endSample; pos += windowSize) {
        int blockEnd = std::min(pos + windowSize, endSample);
        float sumSq = 0.0f;
        for (int j = pos; j < blockEnd; ++j)
            sumSq += s[static_cast<size_t>(j)] * s[static_cast<size_t>(j)];
        float rms = std::sqrt(sumSq / static_cast<float>(blockEnd - pos));

        double t = static_cast<double>(pos) / m_sampleRate;
        if (rms < kSilenceThreshold) {
            if (!inSilence) { silStart = t; inSilence = true; }
        } else {
            if (inSilence) {
                if (t - silStart > 0.1) m_silences.push_back({silStart, t});
                inSilence = false;
            }
        }
    }
    if (inSilence) {
        double silEnd = static_cast<double>(endSample) / m_sampleRate;
        if (silEnd - silStart > 0.1) m_silences.push_back({silStart, silEnd});
    }
}

// ── Paint ────────────────────────────────────────────────────────────────

void MiniWaveformWidget::paintEvent(QPaintEvent* event)
{
    static thread_local int s_paintDepth = 0;
    if (++s_paintDepth > 5) {
        --s_paintDepth;
        QWidget::paintEvent(event);
        return;
    }

    // Lazy silence detection on first paint
    if (!m_silencesComputed)
        const_cast<MiniWaveformWidget*>(this)->detectSilences();

    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, false);

    int w = width();
    int h = height();

    const auto& tc = Theme::colors();

    // Background
    p.fillRect(rect(), tc.waveformBg);

    const auto& samples = sam();
    if (samples.empty() || m_sampleRate == 0 || w <= 0) { --s_paintDepth; return; }

    double viewDuration = m_viewEnd - m_viewStart;
    if (viewDuration <= 0.0) { --s_paintDepth; return; }

    int viewStartSample = static_cast<int>(m_viewStart * m_sampleRate);
    int viewEndSample   = static_cast<int>(m_viewEnd   * m_sampleRate);
    viewStartSample = std::clamp(viewStartSample, 0, static_cast<int>(samples.size()));
    viewEndSample   = std::clamp(viewEndSample,   0, static_cast<int>(samples.size()));
    int totalViewSamples = viewEndSample - viewStartSample;
    if (totalViewSamples <= 0) return;

    double samplesPerPixel = static_cast<double>(totalViewSamples) / w;
    int trimInPx  = timeToPixel(m_trimIn);
    int trimOutPx = timeToPixel(m_trimOut);

    float maxAmp = 0.001f;
    {
        const auto [lo, hi] = samples.minMax(static_cast<size_t>(viewStartSample),
                                             static_cast<size_t>(viewEndSample));
        maxAmp = std::max({maxAmp, std::abs(lo), std::abs(hi)});
    }

    int centerY = h / 2;

    // Draw waveform columns
    for (int px = 0; px < w; ++px) {
        int blockStart = viewStartSample + static_cast<int>(px * samplesPerPixel);
        int blockEnd   = viewStartSample + static_cast<int>((px + 1) * samplesPerPixel);
        blockStart = std::clamp(blockStart, 0, static_cast<int>(samples.size()));
        blockEnd   = std::clamp(blockEnd,   blockStart, static_cast<int>(samples.size()));
        if (blockStart >= blockEnd) continue;

        const auto [lo, hi] = samples.minMax(static_cast<size_t>(blockStart),
                                             static_cast<size_t>(blockEnd));
        const float minVal = std::min(0.0f, lo);
        const float maxVal = std::max(0.0f, hi);

        float normMin = minVal / maxAmp;
        float normMax = maxVal / maxAmp;
        int y1 = centerY - static_cast<int>(normMax * (centerY - 2));
        int y2 = centerY - static_cast<int>(normMin * (centerY - 2));

        bool inTrim = (px >= trimInPx && px <= trimOutPx);
        QColor col = inTrim ? tc.accent : tc.waveformFg;
        p.setPen(col);
        p.drawLine(px, y1, px, y2);
    }

    // Dim regions outside trim
    if (m_trimHandlesVisible) {
        if (trimInPx > 0)
            p.fillRect(0, 0, trimInPx, h, QColor(0, 0, 0, 100));
        if (trimOutPx < w)
            p.fillRect(trimOutPx, 0, w - trimOutPx, h, QColor(0, 0, 0, 100));
    }

    // ── Draw permanent deleted regions (gray striped + orange handles) ──
    for (size_t idx = 0; idx < m_deletedRegions.size(); ++idx) {
        auto [ds, de] = m_deletedRegions[idx];
        if (de <= m_viewStart || ds >= m_viewEnd) continue;
        int x1 = std::max(0, timeToPixel(ds));
        int x2 = std::min(w, timeToPixel(de));
        if (x2 <= x1) continue;

        // Gray overlay
        QColor delOverlay = tc.textDisabled; delOverlay.setAlpha(150);
        p.fillRect(x1, 0, x2 - x1, h, delOverlay);
        // Diagonal stripes
        p.setPen(QPen(tc.surface0, 1));
        for (int sx = x1; sx < x2; sx += 8)
            p.drawLine(sx, 0, sx + h, h);
        // Orange edge handles
        int hw = kHandleWidth;
        QColor handleCol = tc.warning; handleCol.setAlpha(220);
        p.fillRect(x1 - hw / 2, 0, hw, h, handleCol);
        p.fillRect(x2 - hw / 2, 0, hw, h, handleCol);
    }

    // ── In-progress delete selection (red striped) ──
    if (m_deleteSelecting) {
        double s = std::min(m_deleteStart, m_deleteEnd);
        double e = std::max(m_deleteStart, m_deleteEnd);
        int x1 = std::max(0, timeToPixel(s));
        int x2 = std::min(w, timeToPixel(e));
        if (x2 > x1) {
            QColor delSelFill = tc.error; delSelFill.setAlpha(100);
            p.fillRect(x1, 0, x2 - x1, h, delSelFill);
            QColor delSelStripe = tc.error; delSelStripe.setAlpha(180);
            p.setPen(QPen(delSelStripe, 1));
            for (int sx = x1; sx < x2; sx += 6)
                p.drawLine(sx, 0, sx + 3, h);
            p.setPen(QPen(tc.error, 2));
            p.drawRect(x1, 0, x2 - x1, h - 1);
        }
    }

    // Silence indicators
    for (const auto& sil : m_silences) {
        int x1 = timeToPixel(sil.start);
        int x2 = timeToPixel(sil.end);
        if (x2 > x1 && x2 > 0 && x1 < w)
            p.fillRect(std::max(0, x1), h - 3, std::min(w, x2) - std::max(0, x1), 3,
                       QColor(tc.error.red(), tc.error.green(), tc.error.blue(), 150));
    }

    // Trim handles
    if (m_trimHandlesVisible) {
        QColor trimInCol = tc.success; trimInCol.setAlpha(200);
        p.fillRect(std::max(0, trimInPx - kHandleWidth / 2), 0,
                   kHandleWidth, h, trimInCol);
        QColor trimOutCol = tc.error; trimOutCol.setAlpha(200);
        p.fillRect(std::min(w - kHandleWidth, trimOutPx - kHandleWidth / 2), 0,
                   kHandleWidth, h, trimOutCol);
    }

    // Playhead
    if (m_playheadVisible) {
        int phPx = timeToPixel(m_playhead);
        if (phPx >= 0 && phPx < w) {
            p.setPen(QPen(tc.playhead, 2));
            p.drawLine(phPx, 0, phPx, h);
            QPolygon tri;
            tri << QPoint(phPx - 4, 0) << QPoint(phPx + 4, 0) << QPoint(phPx, 6);
            p.setBrush(tc.playhead);
            p.setPen(Qt::NoPen);
            p.drawPolygon(tri);
        }
    }

    --s_paintDepth;
}

// ── Mouse interaction ────────────────────────────────────────────────────

void MiniWaveformWidget::mousePressEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(event);
        return;
    }

    int x = event->pos().x();
    double clickTime = pixelToTime(x);
    bool isShift = event->modifiers() & Qt::ShiftModifier;

    // ── Shift+click starts delete selection ──
    if (isShift) {
        m_deleteSelecting = true;
        m_deleteStart = clickTime;
        m_deleteEnd   = clickTime;
        m_dragMode = None;
        update();
        event->accept();
        return;
    }

    // ── Check deleted region edge handles ──
    for (size_t i = 0; i < m_deletedRegions.size(); ++i) {
        int sx = timeToPixel(m_deletedRegions[i].first);
        int ex = timeToPixel(m_deletedRegions[i].second);
        if (std::abs(x - sx) < kHandleHitZone) {
            m_dragMode = DragDeleteEdge;
            m_dragDeleteRegionIdx = static_cast<int>(i);
            m_dragDeleteIsStart = true;
            setCursor(Qt::SizeHorCursor);
            event->accept();
            return;
        }
        if (std::abs(x - ex) < kHandleHitZone) {
            m_dragMode = DragDeleteEdge;
            m_dragDeleteRegionIdx = static_cast<int>(i);
            m_dragDeleteIsStart = false;
            setCursor(Qt::SizeHorCursor);
            event->accept();
            return;
        }
    }

    // ── Check trim handles ──
    int inPx  = timeToPixel(m_trimIn);
    int outPx = timeToPixel(m_trimOut);

    if (std::abs(x - inPx) < kHandleHitZone) {
        m_dragMode = DragIn;
        setCursor(Qt::SizeHorCursor);
    } else if (std::abs(x - outPx) < kHandleHitZone) {
        m_dragMode = DragOut;
        setCursor(Qt::SizeHorCursor);
    } else {
        m_dragMode = DragPlayhead;
        m_playhead = clickTime;
        m_playheadVisible = true;
        // Reset shuttle levels so next L press starts at 1×
        if (m_jShuttleLevel != 0 || m_lShuttleLevel != 0) {
            m_jShuttleLevel = 0;
            m_lShuttleLevel = 0;
            emit shuttleSpeedChanged(0.0);
        }
        setFocus(Qt::MouseFocusReason);
        update();
        emit seekRequested(clickTime);
    }
    // Don't accept — let the QListWidget parent see the press so it can
    // update its currentRow (which drives spacebar→play routing).
}

void MiniWaveformWidget::mouseMoveEvent(QMouseEvent* event)
{
    int x = event->pos().x();
    double t = pixelToTime(x);

    // ── Delete selection in-progress ──
    if (m_deleteSelecting) {
        m_deleteEnd = t;
        update();
        return;
    }

    // ── Trim handle drag with auto-expand ──
    if (m_dragMode == DragIn) {
        t = std::max(0.0, std::min(t, m_trimOut - 0.05));
        m_trimIn = t;
        expandViewIfNeeded(t);
        update();
        emit trimChanging(m_trimIn, m_trimOut);
    } else if (m_dragMode == DragOut) {
        t = std::max(m_trimIn + 0.05, std::min(t, fileDuration()));
        m_trimOut = t;
        expandViewIfNeeded(t);
        update();
        emit trimChanging(m_trimIn, m_trimOut);
    } else if (m_dragMode == DragPlayhead) {
        t = std::clamp(t, m_viewStart, m_viewEnd);
        m_playhead = t;
        m_playheadVisible = true;
        update();
        emit seekRequested(t);
    } else if (m_dragMode == DragDeleteEdge) {
        // Resize deleted region edge
        if (m_dragDeleteRegionIdx >= 0 &&
            m_dragDeleteRegionIdx < static_cast<int>(m_deletedRegions.size()))
        {
            auto& region = m_deletedRegions[static_cast<size_t>(m_dragDeleteRegionIdx)];
            if (m_dragDeleteIsStart) {
                double newStart = std::min(t, region.second - 0.05);
                region.first = std::max(0.0, newStart);
            } else {
                double newEnd = std::max(t, region.first + 0.05);
                region.second = std::min(fileDuration(), newEnd);
            }
            update();
        }
    } else {
        // Cursor hint near handles
        int inPx  = timeToPixel(m_trimIn);
        int outPx = timeToPixel(m_trimOut);
        bool nearHandle = (std::abs(x - inPx) < kHandleHitZone ||
                           std::abs(x - outPx) < kHandleHitZone);
        // Also check deleted region handles
        for (const auto& [ds, de] : m_deletedRegions) {
            if (std::abs(x - timeToPixel(ds)) < kHandleHitZone ||
                std::abs(x - timeToPixel(de)) < kHandleHitZone) {
                nearHandle = true;
                break;
            }
        }
        setCursor(nearHandle ? Qt::SizeHorCursor : Qt::ArrowCursor);
    }
}

void MiniWaveformWidget::mouseReleaseEvent(QMouseEvent* event)
{
    // ── Finalize delete selection ──
    if (m_deleteSelecting) {
        m_deleteSelecting = false;
        double s = std::min(m_deleteStart, m_deleteEnd);
        double e = std::max(m_deleteStart, m_deleteEnd);
        if (e - s > 0.05) { // Minimum 50ms
            m_deletedRegions.emplace_back(s, e);
            mergeDeletedRegions();
            emit deleteRegionRequested(s, e);
            emit deletedRegionsChanged();
        }
        update();
        return;
    }

    if (m_dragMode == DragIn || m_dragMode == DragOut) {
        m_dragMode = None;
        setCursor(Qt::ArrowCursor);
        emit trimChanged(m_trimIn, m_trimOut);
    } else if (m_dragMode == DragDeleteEdge) {
        m_dragMode = None;
        m_dragDeleteRegionIdx = -1;
        setCursor(Qt::ArrowCursor);
        mergeDeletedRegions();
        emit deletedRegionsChanged();
    } else if (m_dragMode == DragPlayhead) {
        m_dragMode = None;
        setCursor(Qt::ArrowCursor);
    } else {
        m_dragMode = None;
    }
    QWidget::mouseReleaseEvent(event);
}

void MiniWaveformWidget::mouseDoubleClickEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton) {
        QWidget::mouseDoubleClickEvent(event);
        return;
    }

    int x = event->pos().x();
    double clickTime = pixelToTime(x);

    // Double-click on a deleted region to restore it
    for (size_t i = 0; i < m_deletedRegions.size(); ++i) {
        if (clickTime >= m_deletedRegions[i].first && clickTime <= m_deletedRegions[i].second) {
            m_deletedRegions.erase(m_deletedRegions.begin() + static_cast<ptrdiff_t>(i));
            emit deletedRegionsChanged();
            update();
            event->accept();
            return;
        }
    }

    // Fallback: treat as playhead click so rapid clicks aren't silently eaten
    m_dragMode = DragPlayhead;
    m_playhead = clickTime;
    m_playheadVisible = true;
    update();
    emit seekRequested(clickTime);
}

// ── Keyboard shortcuts (Premiere Pro style) ──────────────────────────────

void MiniWaveformWidget::keyPressEvent(QKeyEvent* event)
{
    switch (event->key()) {
    case Qt::Key_Space:
        emit playToggleRequested();
        break;

    case Qt::Key_Escape:
        // Cancel pending delete selection
        if (m_deleteSelecting) {
            m_deleteSelecting = false;
            update();
        }
        break;

    case Qt::Key_Delete:
    case Qt::Key_Backspace:
        // If a delete selection is in progress, commit it
        if (m_deleteSelecting) {
            m_deleteSelecting = false;
            double s = std::min(m_deleteStart, m_deleteEnd);
            double e = std::max(m_deleteStart, m_deleteEnd);
            if (e - s > 0.05) {
                m_deletedRegions.emplace_back(s, e);
                mergeDeletedRegions();
                emit deleteRegionRequested(s, e);
                emit deletedRegionsChanged();
            }
            update();
        }
        break;

    case Qt::Key_I:
        if (m_playheadVisible && m_playhead >= m_viewStart && m_playhead < m_trimOut - 0.01) {
            m_trimIn = m_playhead;
            expandViewIfNeeded(m_trimIn);
            emit trimChanged(m_trimIn, m_trimOut);
            emit inPointSet(m_playhead);
            update();
        }
        break;

    case Qt::Key_O:
        if (m_playheadVisible && m_playhead > m_trimIn + 0.01 && m_playhead <= fileDuration()) {
            m_trimOut = m_playhead;
            expandViewIfNeeded(m_trimOut);
            emit trimChanged(m_trimIn, m_trimOut);
            emit outPointSet(m_playhead);
            update();
        }
        break;

    case Qt::Key_BracketLeft:
        {
            double newIn = std::max(0.0, m_trimIn - 0.05);
            if (newIn < m_trimOut - 0.1) {
                m_trimIn = newIn;
                expandViewIfNeeded(m_trimIn);
                emit trimChanged(m_trimIn, m_trimOut);
                update();
            }
        }
        break;

    case Qt::Key_BracketRight:
        {
            double newOut = std::min(fileDuration(), m_trimOut + 0.05);
            if (newOut > m_trimIn + 0.1) {
                m_trimOut = newOut;
                expandViewIfNeeded(m_trimOut);
                emit trimChanged(m_trimIn, m_trimOut);
                update();
            }
        }
        break;

    case Qt::Key_Comma:
        m_playhead = std::max(m_viewStart, m_playhead - 0.033);
        m_playheadVisible = true;
        emit seekRequested(m_playhead);
        update();
        break;

    case Qt::Key_Period:
        m_playhead = std::min(m_viewEnd, m_playhead + 0.033);
        m_playheadVisible = true;
        emit seekRequested(m_playhead);
        update();
        break;

    case Qt::Key_J: {
        // Shuttle reverse — mirrors PlaybackController::shuttleReverse()
        if (m_lShuttleLevel > 0) {
            // Was going forward — reset and start reverse
            m_lShuttleLevel = 0;
            m_jShuttleLevel = 0;
        }
        m_jShuttleLevel = std::min(m_jShuttleLevel + 1, 3);
        m_lShuttleLevel = 0;
        double speed = -std::pow(2.0, m_jShuttleLevel - 1); // -1, -2, -4
        emit shuttleSpeedChanged(speed);
        break;
    }

    case Qt::Key_K:
        // Shuttle pause
        m_jShuttleLevel = 0;
        m_lShuttleLevel = 0;
        emit shuttleSpeedChanged(0.0);
        break;

    case Qt::Key_L: {
        // Shuttle forward — mirrors PlaybackController::shuttleForward()
        if (m_jShuttleLevel > 0) {
            // Was going reverse — reset and start forward
            m_jShuttleLevel = 0;
            m_lShuttleLevel = 0;
        }
        m_lShuttleLevel = std::min(m_lShuttleLevel + 1, 3);
        m_jShuttleLevel = 0;
        double speed = std::pow(2.0, m_lShuttleLevel - 1); // 1, 2, 4
        emit shuttleSpeedChanged(speed);
        break;
    }

    case Qt::Key_Home:
        m_playhead = m_trimIn;
        m_playheadVisible = true;
        emit seekRequested(m_playhead);
        update();
        break;

    case Qt::Key_End:
        m_playhead = m_trimOut;
        m_playheadVisible = true;
        emit seekRequested(m_playhead);
        update();
        break;

    default:
        QWidget::keyPressEvent(event);
        break;
    }
}

// ── Shift+Wheel zoom ────────────────────────────────────────────────────

void MiniWaveformWidget::wheelEvent(QWheelEvent* event)
{
    const bool ctrlHeld  = event->modifiers() & Qt::ControlModifier;
    const bool shiftHeld = event->modifiers() & Qt::ShiftModifier;

    if (ctrlHeld) {
        // Ctrl + wheel: zoom centered on cursor position
        event->accept();

        const double zoomFactor = (event->angleDelta().y() > 0) ? 0.8 : 1.25;
        const double mouseTime = pixelToTime(static_cast<int>(event->position().x()));

        double viewDur = m_viewEnd - m_viewStart;
        double newDur  = viewDur * zoomFactor;

        double dur = fileDuration();
        newDur = std::clamp(newDur, 0.05, std::max(dur, 0.1));

        double ratio = (viewDur > 0.0)
                           ? (mouseTime - m_viewStart) / viewDur
                           : 0.5;
        double newStart = mouseTime - ratio * newDur;
        double newEnd   = newStart + newDur;

        if (newStart < 0.0) { newEnd -= newStart; newStart = 0.0; }
        if (newEnd > dur)    { newStart -= (newEnd - dur); newEnd = dur; }
        newStart = std::max(0.0, newStart);

        m_viewStart = newStart;
        m_viewEnd   = newEnd;
        update();
    } else {
        // Plain (or Shift) wheel: horizontal scroll (pan left/right).
        // Wheel up scrolls the view left, matching the convention for
        // timeline tools.  Plain wheel intentionally takes over from the
        // outer card list scroll while the cursor is over the waveform.
        (void)shiftHeld;
        event->accept();

        // 15% of the view per notch, by the fraction of a notch turned.
        double viewDur = m_viewEnd - m_viewStart;
        double dur = fileDuration();
        const double panAmount = -viewDur * 0.15 * event->angleDelta().y() / 120.0;

        m_viewStart += panAmount;
        m_viewEnd   += panAmount;

        if (m_viewStart < 0.0) { m_viewEnd -= m_viewStart; m_viewStart = 0.0; }
        if (m_viewEnd > dur) { m_viewStart -= (m_viewEnd - dur); m_viewEnd = dur; }
        m_viewStart = std::max(0.0, m_viewStart);
        update();
    }
}

} // namespace rt
