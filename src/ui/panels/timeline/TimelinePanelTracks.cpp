/*
 * TimelinePanelTracks.cpp — Track management for TimelinePanel.
 * Split from TimelinePanel.cpp for maintainability.
 *
 * Contains: rebuildTracks(), ensureDefaultTracks(), refreshTrackContents(),
 *           clearGapSelection(), notifyZoomChanged(), setAnimVideoCache().
 */

#include "panels/timeline/TimelinePanel.h"
#include "panels/timeline/TimelinePanelInternal.h"
#include "widgets/TimelineTrackWidget.h"
#include "widgets/TimelineRuler.h"

#include "timeline/Timeline.h"
#include "timeline/Track.h"
#include "timeline/Clip.h"
#include "command/CommandStack.h"
#include "command/LambdaCommand.h"
#include "command/commands/TrackCommands.h"

#include <QScrollArea>
#include <QScrollBar>
#include <QTimer>
#include <QPointer>
#include <QBoxLayout>

#include <spdlog/spdlog.h>

#include <algorithm>

namespace rt {

// ═════════════════════════════════════════════════════════════════════════════
//  setTimeline — Set the timeline data model
// ═════════════════════════════════════════════════════════════════════════════

void TimelinePanel::setTimeline(Timeline* timeline)
{
    ++m_waveformLoadGeneration;
    m_pendingWaveformPaths.clear();
    m_failedWaveformPaths.clear();

    // Invalidate in-flight background thumbnail decodes from the previous
    // timeline so their results don't land in the new project's caches.
    ++m_thumbnailLoadGeneration;
    m_pendingThumbnailPaths.clear();
    m_failedThumbnailPaths.clear();

    // Clear clip selection when switching to a new timeline so stale
    // clip IDs from the previous project don't persist in m_selection.
    m_selection.clear();
    m_timeline = timeline;
    if (timeline)
    {
        // Ensure at least 1 video track and 1 audio track exist (Premiere Pro default)
        ensureDefaultTracks();

        m_layoutEngine.setTotalDuration(timeline->duration());
        // Reset scroll so stale position from a previous timeline doesn't
        // persist if the deferred zoomToFit below bails out.
        m_layoutEngine.setScrollX(0);

        // Pass markers to ruler for rendering
        m_ruler->setMarkers(&timeline->markers());

        // Deferred initial zoom: set a sensible default view showing ~1 minute.
        // Uses zoomToFit capped at 60 seconds so very long timelines don't
        // start extremely zoomed out.  The user can still manually zoom or
        // use View > Zoom to Fit (which calls zoomToFit() uncapped).
        QTimer::singleShot(0, this, [this]() {
            if (m_destroying.load(std::memory_order_acquire)) return;
            if (m_timeline) {
                double w = m_ruler->width();
                if (w <= 0) w = width() - headerWidth();
                if (w <= 0) w = 800;
                m_layoutEngine.setViewportWidth(w);

                int64_t dur = m_timeline->duration();
                constexpr int64_t kMaxInitialView = 60; // seconds
                int64_t maxTicks = TimelineLayoutEngine::secondsToTicks(kMaxInitialView);
                if (dur <= 0 || dur > maxTicks) dur = maxTicks;
                m_layoutEngine.zoomToFit(0, dur, w);
                onScrollChanged();

                // Scroll to the saved playhead position so the timeline
                // viewport matches where the user left off (e.g. after
                // crash recovery / project re-open).  Without this, the
                // timeline always opens at scroll=0 regardless of the
                // playhead position stored in the project file.
                setPlayheadPosition(m_timeline->playheadPosition());
            }
        });
    }
    rebuildTracks();
}

// ═════════════════════════════════════════════════════════════════════════════
//  rebuildTracks — Reconcile track widgets against the current timeline.
//
//  Reuses existing TrackHeader / TimelineTrackWidget instances in place
//  (calling setTrack() to repoint them at the current Track*) instead of
//  destroying and recreating them on every edit.  Destroy+create caused
//  a one-frame blank flash because newly-allocated child widgets are
//  visible immediately but only receive their first paintEvent on the
//  next event-loop pass — during that gap the entire timeline showed as
//  empty background.  Only widgets for tracks beyond the previous count
//  are newly created (and have their signals connected); widgets past
//  the new count are hidden + deleteLater()'d.
// ═════════════════════════════════════════════════════════════════════════════

void TimelinePanel::rebuildTracks()
{
    // Bail out during destruction — the destructor sets m_destroying before
    // Qt tears down child widgets.  Rebuilding tracks during teardown would
    // create/destroy widgets in a partially-destroyed hierarchy, risking
    // heap corruption (0xC0000374) from use-after-free in paint/layout.
    if (m_destroying.load(std::memory_order_acquire)) return;

    // Make sure a divider track separates the video and audio sections
    // BEFORE we count tracks, so the divider is part of this pass.
    ensureSectionDivider();

    spdlog::info("[TimelinePanel] ENTER rebuildTracks: m_timeline={} trackCount={}",
                 (void*)m_timeline,
                 m_timeline ? m_timeline->trackCount() : 0);

    auto* headerLayout = m_trackHeaderArea->layout();
    auto* trackLayout  = m_trackContentArea->layout();

    // ── Tear-down path when there is no timeline ─────────────────────────
    if (!m_timeline) {
        for (auto h : m_trackHeaders) if (h) { h->hide(); h->setEnabled(false); h->disconnect(); h->deleteLater(); }
        for (auto w : m_trackWidgets) if (w) { w->hide(); w->setEnabled(false); w->disconnect(); w->deleteLater(); }
        m_trackHeaders.clear();
        m_trackWidgets.clear();
        while (headerLayout->count() > 0) headerLayout->takeAt(0);
        while (trackLayout->count() > 0)  trackLayout->takeAt(0);
        return;
    }

    // ── Auto-rename tracks with Premiere Pro-style numbering ──────────────
    // Video tracks: V1 = lowest (bottom of video section), V2 above, etc.
    // Audio tracks: A1 = highest (top of audio section), A2 below, etc.
    {
        // Collect indices for each type
        std::vector<size_t> videoIndices, audioIndices;
        for (size_t i = 0; i < m_timeline->trackCount(); ++i) {
            Track* t = m_timeline->track(i);
            if (t->isDivider()) continue; // Dividers have no name
            if (t->type() == TrackType::Video) videoIndices.push_back(i);
            else                                audioIndices.push_back(i);
        }

        // Video: topmost (lowest index) = highest number
        // e.g., 3 video tracks at indices 0,1,2 → V3, V2, V1
        // Preserve custom track names; only renumber empty names or names
        // that still match the auto-pattern V# (otherwise inserting a
        // divider or adding a track would wipe the user's rename).
        for (size_t vi = 0; vi < videoIndices.size(); ++vi) {
            Track* t = m_timeline->track(videoIndices[vi]);
            const std::string& cur = t->name();
            bool isAutoName = cur.empty()
                || (cur.size() >= 2 && cur[0] == 'V'
                    && std::all_of(cur.begin() + 1, cur.end(), ::isdigit));
            if (isAutoName) {
                int num = static_cast<int>(videoIndices.size() - vi);  // bottom-up
                t->setName("V" + std::to_string(num));
            }
        }

        // Audio: topmost = A1, next = A2, etc.
        // Preserve custom track names (e.g. per-character names from AudioSync export).
        for (size_t ai = 0; ai < audioIndices.size(); ++ai) {
            Track* t = m_timeline->track(audioIndices[ai]);
            const std::string& cur = t->name();
            // Only auto-rename if name is empty or already an auto-name (A1, A2, ...)
            bool isAutoName = cur.empty()
                || (cur.size() >= 2 && cur[0] == 'A'
                    && std::all_of(cur.begin() + 1, cur.end(), ::isdigit));
            if (isAutoName) {
                int num = static_cast<int>(ai + 1);
                t->setName("A" + std::to_string(num));
            }
        }
    }

    // Compact any null QPointer slots from prior deferred deletions so the
    // indexing in Phases 1–3 below is contiguous.
    auto compact = [](auto& vec) {
        vec.erase(std::remove_if(vec.begin(), vec.end(),
                                 [](const auto& p){ return !p; }),
                  vec.end());
    };
    compact(m_trackHeaders);
    compact(m_trackWidgets);

    const size_t newCount = m_timeline->trackCount();
    const size_t liveOld  = std::min(m_trackHeaders.size(), m_trackWidgets.size());

    // Determine the current "standard" track height so dividers can size
    // themselves as 1/4 of whatever the user's real tracks are right now.
    // Use the first non-divider track as reference; fall back to 80.
    float refTrackHeight = 80.0f;
    for (size_t ri = 0; ri < newCount; ++ri) {
        Track* t = m_timeline->track(ri);
        if (!t || t->isDivider()) continue;
        float rh = t->height();
        if (rh >= 1.0f) { refTrackHeight = rh; break; }
    }
    const float dividerHeight = std::max(8.0f, refTrackHeight * 0.25f);

    auto resolvedHeight = [&](Track* track) {
        float h = track->height();
        if (track->isDivider()) {
            h = dividerHeight;
            track->setHeight(h);
        } else if (h < 1.0f) {
            h = 80.0f;
            track->setHeight(h);
        }
        return h;
    };

    // ── Phase 1: reuse existing widgets at positions [0, min(liveOld,newCount)) ──
    // No destruction, no flicker — repoint each widget at its (possibly new)
    // Track* and update its row height.  Signals are already connected and
    // pick up the new index from the QPointer-stored m_trackIndex via setTrack().
    const size_t reuseN = std::min(liveOld, newCount);
    for (size_t i = 0; i < reuseN; ++i)
    {
        Track* track = m_timeline->track(i);
        float h = resolvedHeight(track);

        if (auto header = m_trackHeaders[i]) {
            header->setTrack(track, i);
            header->setHeight(h);
            header->setFixedWidth(m_headerScroll->width());
        }
        if (auto tw = m_trackWidgets[i]) {
            tw->setTrack(track, i);
            tw->setFixedHeight(static_cast<int>(h));
        }
    }

    // ── Phase 2: drop excess widgets at the tail (only when tracks shrink) ──
    // Take only the doomed widgets out of the layout, hide them, and
    // schedule deletion.  The rest of the layout (and its painted state)
    // stays exactly as it was.
    if (newCount < liveOld)
    {
        for (size_t i = newCount; i < liveOld; ++i)
        {
            auto header = m_trackHeaders[i];
            auto widget = m_trackWidgets[i];
            if (header) {
                headerLayout->removeWidget(header);
                header->hide();
                header->setEnabled(false);
                header->disconnect();
                header->deleteLater();
            }
            if (widget) {
                trackLayout->removeWidget(widget);
                widget->hide();
                widget->setEnabled(false);
                widget->disconnect();
                widget->deleteLater();
            }
        }
        m_trackHeaders.resize(newCount);
        m_trackWidgets.resize(newCount);
    }

    // On the very first build the layouts are empty — add the top stretch
    // so subsequently inserted widgets land in the centre-aligned region.
    if (liveOld == 0 && headerLayout->count() == 0) {
        static_cast<QVBoxLayout*>(headerLayout)->addStretch();
        static_cast<QVBoxLayout*>(trackLayout)->addStretch();
    }

    // Detect whether a bottom stretch already exists.  A QSpacerItem
    // returns nullptr from widget() — that's how we tell stretches apart
    // from widget items.  On the first build there's only the top stretch
    // (no bottom one yet); we add it after Phase 3 in that case.
    auto hasBottomStretch = [](QLayout* L) {
        if (L->count() == 0) return false;
        QLayoutItem* last = L->itemAt(L->count() - 1);
        return last && last->widget() == nullptr
                    && last->spacerItem() != nullptr
                    && L->count() > 1; // exclude the lone top stretch case
    };

    // ── Phase 3: create + insert new widgets for tracks beyond liveOld ───
    // These are inserted BEFORE the bottom stretch (if one exists), so
    // existing widgets above them never reflow.
    spdlog::info("rebuildTracks: reuse {} / create {} / drop {} (was {} → now {})",
                 reuseN,
                 newCount > liveOld ? newCount - liveOld : 0,
                 liveOld > newCount ? liveOld - newCount : 0,
                 liveOld, newCount);
    for (size_t i = liveOld; i < newCount; ++i)
    {
        Track* track = m_timeline->track(i);
        float h = resolvedHeight(track);

        // Header
        auto* header = new TrackHeader(m_trackHeaderArea);
        header->setTrack(track, i);
        header->setHeight(h);
        header->setFixedWidth(m_headerScroll->width());
        header->setMouseTracking(true);  // Enable hover cursor changes
        const int hInsertAt = hasBottomStretch(headerLayout)
                                  ? headerLayout->count() - 1
                                  : headerLayout->count();
        static_cast<QBoxLayout*>(headerLayout)->insertWidget(hInsertAt, header);
        m_trackHeaders.push_back(header);

        // Track content
        auto* tw = new TimelineTrackWidget(m_trackContentArea);
        tw->setLayoutEngine(&m_layoutEngine);
        tw->setTrack(track, i);
        tw->setFixedHeight(static_cast<int>(h));
        const int tInsertAt = hasBottomStretch(trackLayout)
                                  ? trackLayout->count() - 1
                                  : trackLayout->count();
        static_cast<QBoxLayout*>(trackLayout)->insertWidget(tInsertAt, tw);
        m_trackWidgets.push_back(tw);

        // Connect track height resize from header drag
        QObject::connect(header, &TrackHeader::heightChanged,
            this, [this](size_t trackIdx, float newHeight) {
            spdlog::debug("[SIGNAL] TrackHeader {} heightChanged for track {} newHeight={}", (void*)sender(), trackIdx, newHeight);
            if (!m_timeline || trackIdx >= m_timeline->trackCount()) return;
            Track* tk = m_timeline->track(trackIdx);
            tk->setHeight(newHeight);
            // Also resize the corresponding track widget
            if (trackIdx < m_trackWidgets.size())
                m_trackWidgets[trackIdx]->setFixedHeight(static_cast<int>(newHeight));
            // If a real (non-divider) track was resized, propagate the new
            // 1/4-height to every divider so they stay proportional.
            if (!tk->isDivider()) {
                float dh = std::max(8.0f, newHeight * 0.25f);
                for (size_t di = 0; di < m_timeline->trackCount(); ++di) {
                    Track* dt = m_timeline->track(di);
                    if (!dt || !dt->isDivider()) continue;
                    dt->setHeight(dh);
                    if (di < m_trackHeaders.size() && m_trackHeaders[di])
                        m_trackHeaders[di]->setHeight(dh);
                    if (di < m_trackWidgets.size() && m_trackWidgets[di])
                        m_trackWidgets[di]->setFixedHeight(static_cast<int>(dh));
                }
            }
            updateMinHeaderWidth();
        });

        // Connect track targeting toggle
        QObject::connect(header, &TrackHeader::targetToggled,
            this, [this](size_t trackIdx, bool targeted) {
            spdlog::debug("[SIGNAL] TrackHeader {} targetToggled for track {} targeted={}", (void*)sender(), trackIdx, targeted);
            if (!m_timeline || trackIdx >= m_timeline->trackCount()) return;
            m_timeline->track(trackIdx)->setTargeted(targeted);
            if (trackIdx < m_trackHeaders.size())
                m_trackHeaders[trackIdx]->update();
        });

        // Connect lock/mute/solo/syncLock toggles
        QObject::connect(header, &TrackHeader::lockToggled,
            this, [this](size_t trackIdx, bool locked) {
            spdlog::debug("[SIGNAL] TrackHeader {} lockToggled for track {} locked={}", (void*)sender(), trackIdx, locked);
            if (!m_timeline || trackIdx >= m_timeline->trackCount()) return;
            m_timeline->track(trackIdx)->setLocked(locked);
            if (trackIdx < m_trackHeaders.size())
                m_trackHeaders[trackIdx]->update();
            if (trackIdx < m_trackWidgets.size())
                m_trackWidgets[trackIdx]->update();
        });
        QObject::connect(header, &TrackHeader::muteToggled,
            this, [this](size_t trackIdx, bool muted) {
            spdlog::debug("[SIGNAL] TrackHeader {} muteToggled for track {} muted={}", (void*)sender(), trackIdx, muted);
            if (!m_timeline || trackIdx >= m_timeline->trackCount()) return;
            m_timeline->track(trackIdx)->setMuted(muted);
            if (trackIdx < m_trackHeaders.size())
                m_trackHeaders[trackIdx]->update();
            emit contentChanged();
        });
        QObject::connect(header, &TrackHeader::soloToggled,
            this, [this](size_t trackIdx, bool soloed) {
            spdlog::debug("[SIGNAL] TrackHeader {} soloToggled for track {} soloed={}", (void*)sender(), trackIdx, soloed);
            if (!m_timeline || trackIdx >= m_timeline->trackCount()) return;
            m_timeline->track(trackIdx)->setSoloed(soloed);
            if (trackIdx < m_trackHeaders.size())
                m_trackHeaders[trackIdx]->update();
            emit contentChanged();
        });
        QObject::connect(header, &TrackHeader::syncLockToggled,
            this, [this](size_t trackIdx, bool syncLocked) {
            spdlog::debug("[SIGNAL] TrackHeader {} syncLockToggled for track {} syncLocked={}", (void*)sender(), trackIdx, syncLocked);
            if (!m_timeline || trackIdx >= m_timeline->trackCount()) return;
            m_timeline->track(trackIdx)->setSyncLocked(syncLocked);
            if (trackIdx < m_trackHeaders.size())
                m_trackHeaders[trackIdx]->update();
            if (trackIdx < m_trackWidgets.size())
                m_trackWidgets[trackIdx]->update();
        });

        // Connect track collapse/expand toggle
        QObject::connect(header, &TrackHeader::collapseToggled,
            this, [this](size_t trackIdx, bool collapsed) {
            spdlog::debug("[SIGNAL] TrackHeader {} collapseToggled for track {} collapsed={}", (void*)sender(), trackIdx, collapsed);
            if (!m_timeline || trackIdx >= m_timeline->trackCount()) return;
            static constexpr float kCollapsedHeight = 20.0f;
            Track* t = m_timeline->track(trackIdx);
            t->setCollapsed(collapsed);
            float newH = collapsed ? kCollapsedHeight : 60.0f;
            t->setHeight(newH);
            if (trackIdx < m_trackHeaders.size())
                m_trackHeaders[trackIdx]->setHeight(newH);
            if (trackIdx < m_trackWidgets.size())
                m_trackWidgets[trackIdx]->setFixedHeight(static_cast<int>(newH));
            updateMinHeaderWidth();
        });

        // Connect track management signals from header context menu
        QObject::connect(header, &TrackHeader::addTrackRequested,
            this, [this](bool video, bool above, size_t nearIndex) {
            spdlog::debug("[SIGNAL] TrackHeader {} addTrackRequested video={} above={} nearIndex={}", (void*)sender(), video, above, nearIndex);
            if (above)
                emit addTrackAbove(nearIndex, video);
            else
                emit addTrackBelow(nearIndex, video);
        });
        QObject::connect(header, &TrackHeader::deleteTrackRequested,
                this, [this](size_t trackIdx) {
            spdlog::debug("[SIGNAL] TrackHeader {} deleteTrackRequested for track {} (deferred)", (void*)sender(), trackIdx);
            QPointer<TimelinePanel> safeThis(this);
            QTimer::singleShot(0, this, [safeThis, trackIdx]() {
                if (!safeThis) {
                    spdlog::error("[DEFENSIVE] TimelinePanel was deleted before deferred deleteTrack could run!");
                    return;
                }
                spdlog::debug("[DEFENSIVE] Emitting deleteTrack for track {} after deferral", trackIdx);
                emit safeThis->deleteTrack(trackIdx);
            });
        });

        // Insert a divider track relative to this header's track.
        // Defer so we don't delete the TrackHeader that's still running
        // its contextMenu handler on the stack (use-after-free crash).
        QObject::connect(header, &TrackHeader::addDividerRequested,
                this, [this](bool above, size_t nearIdx) {
            spdlog::debug("[SIGNAL] TrackHeader {} addDividerRequested above={} nearIdx={}", (void*)sender(), above, nearIdx);
            if (!m_timeline) return;
            size_t insertAt = above ? nearIdx : nearIdx + 1;
            if (insertAt > m_timeline->trackCount())
                insertAt = m_timeline->trackCount();
            QPointer<TimelinePanel> safeThis(this);
            QTimer::singleShot(0, this, [safeThis, insertAt]() {
                if (!safeThis || !safeThis->m_timeline) {
                    spdlog::error("[DEFENSIVE] TimelinePanel or m_timeline was deleted before deferred addDivider could run!");
                    return;
                }
                spdlog::debug("[DEFENSIVE] Adding divider track at {} after deferral", insertAt);
                safeThis->addDividerUndoable(insertAt);
            });
        });

        // Drag-reorder signal wiring
        QObject::connect(header, &TrackHeader::reorderDragStarted,
            this, [this](size_t srcIdx) {
            spdlog::debug("[SIGNAL] TrackHeader {} reorderDragStarted srcIdx={}", (void*)sender(), srcIdx);
            m_reorderSrcIndex = srcIdx;
            if (m_ghostOverlay) {
                m_ghostOverlay->reorderMode = true;
                m_ghostOverlay->raise();
                m_ghostOverlay->show();
            }
        });
        QObject::connect(header, &TrackHeader::reorderDragMoved,
            this, [this](size_t /*srcIdx*/, const QPoint& gp) {
            spdlog::debug("[SIGNAL] TrackHeader {} reorderDragMoved", (void*)sender());
            updateReorderOverlay(gp);
        });
        QObject::connect(header, &TrackHeader::reorderDragFinished,
            this, [this](size_t srcIdx, const QPoint& gp, bool commit) {
            spdlog::debug("[SIGNAL] TrackHeader {} reorderDragFinished srcIdx={} commit={}", (void*)sender(), srcIdx, commit);
            size_t dst = computeReorderInsertionIndex(gp);
            if (m_ghostOverlay) {
                m_ghostOverlay->reorderMode = false;
                m_ghostOverlay->hide();
            }
            m_reorderSrcIndex = SIZE_MAX;
            if (!commit || !m_timeline) return;
            if (dst > srcIdx) --dst; // compensate for removal shift
            if (dst == srcIdx) return;
            if (srcIdx >= m_timeline->trackCount()) return;
            if (dst >= m_timeline->trackCount())
                dst = m_timeline->trackCount() - 1;
            // The permanent V/A boundary divider is auto-managed and
            // cannot be reordered by the user. TrackHeader blocks the
            // press, but defend here too in case the move came via some
            // other path.
            if (Track* src = m_timeline->track(srcIdx);
                    src && src->isPermanentDivider())
                return;
            // Defer past the mouseReleaseEvent so we don't delete the
            // TrackHeader that is currently on the stack.
            QTimer::singleShot(0, this, [this, srcIdx, dst]() {
                if (m_destroying.load(std::memory_order_acquire)) return;
                if (!m_timeline) return;
                m_timeline->moveTrack(srcIdx, dst);
                rebuildTracks();
            });
        });
        QObject::connect(header, &TrackHeader::trackSizePresetRequested,
            this, [this](size_t trackIdx, float height) {
            spdlog::debug("[SIGNAL] TrackHeader {} trackSizePresetRequested trackIdx={} height={}", (void*)sender(), trackIdx, height);
            if (!m_timeline || trackIdx >= m_timeline->trackCount()) return;
            m_timeline->track(trackIdx)->setHeight(height);
            if (trackIdx < m_trackWidgets.size())
                m_trackWidgets[trackIdx]->setFixedHeight(static_cast<int>(height));
            if (trackIdx < m_trackHeaders.size())
                m_trackHeaders[trackIdx]->setHeight(height);
            updateMinHeaderWidth();
        });

        // Connect track rename (undoable via command stack)
        QObject::connect(header, &TrackHeader::trackRenamed,
            this, [this](size_t trackIdx, const QString& newName) {
            spdlog::debug("[SIGNAL] TrackHeader {} trackRenamed trackIdx={} newName={}", (void*)sender(), trackIdx, newName.toStdString());
            if (!m_timeline || trackIdx >= m_timeline->trackCount()) return;
            Track* track = m_timeline->track(trackIdx);
            auto cmd = std::make_unique<SetTrackPropertyCommand<std::string>>(
                track, newName.toStdString(),
                +[](const Track& t) -> std::string { return t.name(); },
                +[](Track& t, std::string v) { t.setName(v); },
                "Rename Track",
                static_cast<int>(CommandTypeId::SetTrackName));
            if (m_commandStack) {
                m_commandStack->execute(std::move(cmd));
            } else {
                cmd->execute();
            }
            if (trackIdx < m_trackHeaders.size())
                m_trackHeaders[trackIdx]->update();
            updateMinHeaderWidth();
        });

        // Connect clip selection
        QObject::connect(tw, &TimelineTrackWidget::clipClicked,
            this, [this](size_t trackIndex, size_t clipIndex, bool shiftHeld) {
            spdlog::debug("[SIGNAL] TimelineTrackWidget {} clipClicked trackIndex={} clipIndex={} shiftHeld={}", (void*)sender(), trackIndex, clipIndex, shiftHeld);
            // Update SelectionSet (this was previously just bounced to
            // clipSelected signal without updating m_selection)
            if (!m_timeline) return;
            Track* trk = m_timeline->track(trackIndex);
            if (!trk || clipIndex >= trk->clipCount()) return;
            const Clip* clip = trk->clip(clipIndex);
            if (!clip) return;

            ClipRef ref{trackIndex, clip->id()};
            if (!shiftHeld && !m_selection.isSelected(ref))
                m_selection.clear();
            m_selection.selectClip(ref, shiftHeld);

            // Linked A/V selection: if the clip has a groupId and
            // linked selection is enabled, also select all clips with
            // the same groupId across all tracks.
            if (m_linkedSelectionEnabled) {
                uint64_t gid = clip->groupId();
                if (gid != 0) {
                    for (size_t ti = 0; ti < m_timeline->trackCount(); ++ti) {
                        Track* t = m_timeline->track(ti);
                        for (size_t ci = 0; ci < t->clipCount(); ++ci) {
                            const Clip* c = t->clip(ci);
                            if (c->groupId() == gid && c->id() != clip->id()) {
                                m_selection.selectClip({ti, c->id()}, true);
                                break; // one match per track for linked A/V
                            }
                        }
                    }
                }
            }

            emit selectionChanged();
            emit clipSelected(trackIndex, clipIndex);
            update();
        });

        // Install event filter so TimelinePanel gets mouse events for
        // drag/move/trim/marquee even though TimelineTrackWidget is the
        // deepest child that Qt delivers events to.
        tw->installEventFilter(this);
    }

    // First-build only: now that the first widgets are in place, add the
    // bottom stretch so tracks are vertically centred (Premiere Pro style).
    // Subsequent rebuilds keep the existing stretch in place.
    if (liveOld == 0 && newCount > 0) {
        static_cast<QVBoxLayout*>(headerLayout)->addStretch();
        static_cast<QVBoxLayout*>(trackLayout)->addStretch();
    }

    spdlog::info("rebuildTracks: now have {} headers, {} widgets for {} tracks",
                 m_trackHeaders.size(), m_trackWidgets.size(), newCount);

    // Scroll so the video/audio boundary is vertically centred — only on
    // first build / when track set materially changed.  On a pure in-place
    // reuse pass we leave the user's current vertical scroll alone.
    if (liveOld != newCount) {
        QTimer::singleShot(0, this, [this]() {
            if (m_destroying.load(std::memory_order_acquire)) return;
            if (!m_verticalScroll || !m_timeline) return;
            // Find the pixel position of the first audio track (= bottom of video section)
            int videoBottom = 0;
            for (size_t i = 0; i < m_timeline->trackCount(); ++i) {
                if (m_timeline->track(i)->type() == TrackType::Audio) break;
                videoBottom += static_cast<int>(m_timeline->track(i)->height());
            }
            int viewH = m_verticalScroll->viewport()->height();
            int contentH = m_trackContentArea->sizeHint().height();
            if (contentH > viewH) {
                int target = videoBottom - viewH * 2 / 5;
                if (target < 0) target = 0;
                m_verticalScroll->verticalScrollBar()->setValue(target);
            }
        });
    }

    // Load waveform peaks for audio clips and pass to track widgets
    loadWaveforms();
    for (auto tw : m_trackWidgets)
        if (tw) tw->setWaveformCache(&m_waveformPeaks);

    // Load video thumbnails and pass to track widgets
    loadThumbnails();
    for (auto tw : m_trackWidgets)
        if (tw) tw->setThumbnailCache(&m_thumbnailCache);

    // Pass animation video cache to track widgets
    for (auto tw : m_trackWidgets)
        if (tw) tw->setAnimVideoCache(m_animVideoCache);

    // Push in/out point overlays to track widgets
    {
        int64_t inPt  = m_timeline->inPoint();
        int64_t outPt = m_timeline->outPoint();
        for (auto tw : m_trackWidgets)
            if (tw) tw->setInOutPoints(inPt, outPt);
    }

    // Re-apply visual selection.  m_selection persists across rebuilds but
    // any newly-created widget needs to learn its highlight state.
    emit selectionChanged();

    // Update minimum header width based on current track names.
    // Run twice: once now (before first paint) and once deferred (after Qt's
    // layout pass settles widget widths). This ensures newly created track
    // headers reposition their label/buttons based on actual name length and
    // height — matching the behavior existing tracks get on resize.
    updateMinHeaderWidth();
    QTimer::singleShot(0, this, [this]() {
        if (m_destroying.load(std::memory_order_acquire)) return;
        updateMinHeaderWidth();
        for (auto hdr : m_trackHeaders)
            if (hdr) hdr->update();
    });

    // NOTE: no setUpdatesEnabled(true)/repaint() pair here — we never
    // disabled updates, and reused widgets already hold valid painted
    // content.  Newly-added widgets will get their first paintEvent on
    // the next event-loop iteration like any normal child widget.
    updateGeometry();
}

// ═════════════════════════════════════════════════════════════════════════════
//  insertTrackWidgetIncremental
// ═════════════════════════════════════════════════════════════════════════════

void TimelinePanel::insertTrackWidgetIncremental(size_t trackIndex)
{
    if (!m_timeline || trackIndex >= m_timeline->trackCount()) return;

    Track* track = m_timeline->track(trackIndex);
    if (!track) return;

    auto* headerLayout = m_trackHeaderArea->layout();
    auto* trackLayout  = m_trackContentArea->layout();
    if (!headerLayout || !trackLayout) return;

    // Renumber all tracks so V/A numbering stays correct after insertion
    {
        std::vector<size_t> videoIndices, audioIndices;
        for (size_t i = 0; i < m_timeline->trackCount(); ++i) {
            Track* t = m_timeline->track(i);
            if (t->isDivider()) continue;
            if (t->type() == TrackType::Video) videoIndices.push_back(i);
            else                                audioIndices.push_back(i);
        }
        for (size_t vi = 0; vi < videoIndices.size(); ++vi) {
            Track* t = m_timeline->track(videoIndices[vi]);
            t->setName("V" + std::to_string(videoIndices.size() - vi));
        }
        for (size_t ai = 0; ai < audioIndices.size(); ++ai) {
            Track* t = m_timeline->track(audioIndices[ai]);
            const std::string& cur = t->name();
            bool isAutoName = cur.empty()
                || (cur.size() >= 2 && cur[0] == 'A'
                    && std::all_of(cur.begin() + 1, cur.end(), ::isdigit));
            if (isAutoName) {
                t->setName("A" + std::to_string(ai + 1));
            }
        }
    }

    // Refresh existing headers that were renamed
    for (size_t i = 0; i < m_trackHeaders.size(); ++i) {
        if (m_trackHeaders[i]) {
            m_trackHeaders[i]->setTrack(m_timeline->track(i), i);
            m_trackHeaders[i]->update();
        }
    }
    // Refresh existing track widgets with updated track index
    for (size_t i = 0; i < m_trackWidgets.size(); ++i) {
        if (m_trackWidgets[i]) {
            m_trackWidgets[i]->setTrack(m_timeline->track(i), i);
            m_trackWidgets[i]->update();
        }
    }

    float h = track->height();
    if (h < 1.0f) h = 80.0f;

    // Create new header
    auto* header = new TrackHeader(m_trackHeaderArea);
    header->setTrack(track, trackIndex);
    header->setHeight(h);
    header->setFixedWidth(m_headerScroll->width());
    header->setMouseTracking(true);
    // Insert into layout before the stretch (stretch is last item)
    int layoutPos = qMin(static_cast<int>(trackIndex),
                         headerLayout->count() - 1);
    static_cast<QBoxLayout*>(headerLayout)->insertWidget(layoutPos, header);
    m_trackHeaders.insert(m_trackHeaders.begin() + static_cast<ptrdiff_t>(trackIndex), header);

    // Create new track widget
    auto* tw = new TimelineTrackWidget(m_trackContentArea);
    tw->setLayoutEngine(&m_layoutEngine);
    tw->setTrack(track, trackIndex);
    tw->setFixedHeight(static_cast<int>(h));
    tw->setWaveformCache(&m_waveformPeaks);
    tw->setThumbnailCache(&m_thumbnailCache);
    tw->setAnimVideoCache(m_animVideoCache);
    if (m_timeline) {
        tw->setInOutPoints(m_timeline->inPoint(), m_timeline->outPoint());
    }
    layoutPos = qMin(static_cast<int>(trackIndex),
                     trackLayout->count() - 1);
    static_cast<QBoxLayout*>(trackLayout)->insertWidget(layoutPos, tw);
    m_trackWidgets.insert(m_trackWidgets.begin() + static_cast<ptrdiff_t>(trackIndex), tw);

    // Connect signals (same as rebuildTracks)
    QObject::connect(header, &TrackHeader::heightChanged,
        this, [this](size_t trackIdx, float newHeight) {
        if (!m_timeline || trackIdx >= m_timeline->trackCount()) return;
        Track* tk = m_timeline->track(trackIdx);
        tk->setHeight(newHeight);
        if (trackIdx < m_trackWidgets.size())
            m_trackWidgets[trackIdx]->setFixedHeight(static_cast<int>(newHeight));
        if (!tk->isDivider()) {
            float dh = std::max(8.0f, newHeight * 0.25f);
            for (size_t di = 0; di < m_timeline->trackCount(); ++di) {
                Track* dt = m_timeline->track(di);
                if (!dt || !dt->isDivider()) continue;
                dt->setHeight(dh);
                if (di < m_trackHeaders.size() && m_trackHeaders[di])
                    m_trackHeaders[di]->setHeight(dh);
                if (di < m_trackWidgets.size() && m_trackWidgets[di])
                    m_trackWidgets[di]->setFixedHeight(static_cast<int>(dh));
            }
        }
        updateMinHeaderWidth();
    });

    QObject::connect(header, &TrackHeader::targetToggled,
        this, [this](size_t trackIdx, bool targeted) {
        if (!m_timeline || trackIdx >= m_timeline->trackCount()) return;
        m_timeline->track(trackIdx)->setTargeted(targeted);
        if (trackIdx < m_trackHeaders.size())
            m_trackHeaders[trackIdx]->update();
    });

    auto connectToggle = [&](auto signal, auto setter) {
        QObject::connect(header, signal, this,
            [this, setter](size_t trackIdx, bool val) {
            if (!m_timeline || trackIdx >= m_timeline->trackCount()) return;
            setter(m_timeline->track(trackIdx), val);
            if (trackIdx < m_trackHeaders.size())
                m_trackHeaders[trackIdx]->update();
            if (trackIdx < m_trackWidgets.size())
                m_trackWidgets[trackIdx]->update();
            emit contentChanged();
        });
    };

    connectToggle(&TrackHeader::lockToggled, [](Track* t, bool v) { t->setLocked(v); });
    connectToggle(&TrackHeader::muteToggled, [](Track* t, bool v) { t->setMuted(v); });
    connectToggle(&TrackHeader::soloToggled, [](Track* t, bool v) { t->setSoloed(v); });
    connectToggle(&TrackHeader::syncLockToggled, [](Track* t, bool v) { t->setSyncLocked(v); });

    QObject::connect(header, &TrackHeader::collapseToggled,
        this, [this](size_t trackIdx, bool collapsed) {
        if (!m_timeline || trackIdx >= m_timeline->trackCount()) return;
        static constexpr float kCollapsedHeight = 20.0f;
        Track* t = m_timeline->track(trackIdx);
        t->setCollapsed(collapsed);
        float newH = collapsed ? kCollapsedHeight : 60.0f;
        t->setHeight(newH);
        if (trackIdx < m_trackHeaders.size())
            m_trackHeaders[trackIdx]->setHeight(newH);
        if (trackIdx < m_trackWidgets.size())
            m_trackWidgets[trackIdx]->setFixedHeight(static_cast<int>(newH));
        updateMinHeaderWidth();
    });

    QObject::connect(header, &TrackHeader::addTrackRequested,
        this, [this](bool video, bool above, size_t nearIndex) {
        if (above) emit addTrackAbove(nearIndex, video);
        else       emit addTrackBelow(nearIndex, video);
    });

    QObject::connect(header, &TrackHeader::deleteTrackRequested,
        this, [this](size_t trackIdx) {
        QPointer<TimelinePanel> safeThis(this);
        QTimer::singleShot(0, this, [safeThis, trackIdx]() {
            if (!safeThis) return;
            emit safeThis->deleteTrack(trackIdx);
        });
    });

    QObject::connect(header, &TrackHeader::addDividerRequested,
        this, [this](bool above, size_t nearIdx) {
        if (!m_timeline) return;
        size_t insertAt = above ? nearIdx : nearIdx + 1;
        if (insertAt > m_timeline->trackCount())
            insertAt = m_timeline->trackCount();
        QPointer<TimelinePanel> safeThis(this);
        QTimer::singleShot(0, this, [safeThis, insertAt]() {
            if (!safeThis || !safeThis->m_timeline) return;
            safeThis->addDividerUndoable(insertAt);
        });
    });

    QObject::connect(header, &TrackHeader::reorderDragStarted,
        this, [this](size_t srcIdx) {
        m_reorderSrcIndex = srcIdx;
        if (m_ghostOverlay) {
            m_ghostOverlay->reorderMode = true;
            m_ghostOverlay->raise();
            m_ghostOverlay->show();
        }
    });

    QObject::connect(header, &TrackHeader::reorderDragMoved,
        this, [this](size_t, const QPoint& gp) {
        if (m_ghostOverlay && m_ghostOverlay->isVisible())
            updateReorderOverlay(gp);
    });

    QObject::connect(header, &TrackHeader::reorderDragFinished,
        this, [this](size_t srcIdx, const QPoint& gp) {
        m_reorderSrcIndex = SIZE_MAX;
        if (m_ghostOverlay) { m_ghostOverlay->reorderMode = false; m_ghostOverlay->hide(); }
        size_t dst = computeReorderInsertionIndex(gp);
        if (dst != srcIdx && dst != SIZE_MAX) {
            if (m_timeline) {
                // Permanent V/A divider is auto-managed — don't allow
                // reordering it out of its slot.
                if (Track* src = m_timeline->track(srcIdx);
                        src && src->isPermanentDivider())
                    return;
                auto track = m_timeline->takeTrack(srcIdx);
                if (track) {
                    size_t insertAt = dst;
                    if (dst > srcIdx) --insertAt;
                    m_timeline->insertTrack(insertAt, std::move(track));
                    rebuildTracks();
                }
            }
        }
    });

    emit selectionChanged();
    updateMinHeaderWidth();
    updateGeometry();
}

// ═════════════════════════════════════════════════════════════════════════════
//  ensureDefaultTracks
// ═════════════════════════════════════════════════════════════════════════════

void TimelinePanel::ensureDefaultTracks()
{
    if (!m_timeline) return;

    // Count existing video and audio tracks
    bool hasVideo = false;
    bool hasAudio = false;
    for (size_t i = 0; i < m_timeline->trackCount(); ++i)
    {
        const Track* t = m_timeline->track(i);
        if (t->type() == TrackType::Video) hasVideo = true;
        if (t->type() == TrackType::Audio) hasAudio = true;
    }

    // Like Premiere Pro: video tracks go FIRST, then audio tracks.
    // Add V1 before any audio tracks, add A1 at the end.
    if (!hasVideo)
    {
        // Insert V1 at position 0 (top) if no video tracks exist yet
        size_t insertIdx = 0;
        auto vTrack = std::make_unique<Track>(TrackType::Video, "V1");
        m_timeline->insertTrack(insertIdx, std::move(vTrack));
    }

    if (!hasAudio)
        m_timeline->addAudioTrack("A1");
}

// ═════════════════════════════════════════════════════════════════════════════
//  refreshTrackContents — Lightweight content refresh, NO widget rebuild
// ═════════════════════════════════════════════════════════════════════════════

void TimelinePanel::ensureSectionDivider()
{
    if (!m_timeline) return;

    // Near-black background for the V/A separator — noticeably darker than
    // the empty track background so the split reads as a recessed groove,
    // matching the user-added divider look.
    constexpr uint32_t kSepColor = 0xFF0A0A0Cu;

    // Helper: normalise a permanent divider's visual properties. Old
    // project files may have persisted leftover names ("V3") or no color;
    // clear / set them every pass so the boundary always renders cleanly.
    auto normalizePermanent = [](Track* d) {
        if (!d) return;
        d->setName("");
        d->setColor(kSepColor);
        d->setHeight(10.0f);
        d->setTargeted(false);
        d->setSyncLocked(false);
        d->setPermanentDivider(true);
    };

    // Promote "shadow dividers" — Video tracks that look like dividers
    // (height < 15 px; real video tracks default to 80 px) but lost their
    // isDivider flag in an older project file. Mark them as plain dividers
    // (NOT permanent); the boundary-pick step below upgrades the one at
    // the V/A seam to permanent.
    const size_t n0 = m_timeline->trackCount();
    for (size_t i = 0; i < n0; ++i) {
        Track* t = m_timeline->track(i);
        if (!t || t->isDivider()) continue;
        if (t->type() == TrackType::Video && t->height() < 15.0f) {
            t->setDivider(true);
            t->setName("");
        }
    }

    // Locate the V/A boundary on real tracks (dividers ignored).
    size_t firstAudio = SIZE_MAX;
    bool hasVideo = false;
    for (size_t i = 0; i < m_timeline->trackCount(); ++i) {
        Track* t = m_timeline->track(i);
        if (!t || t->isDivider()) continue;
        if (t->type() == TrackType::Audio && firstAudio == SIZE_MAX) firstAudio = i;
        if (t->type() == TrackType::Video) hasVideo = true;
    }

    // Without both a video and an audio section, the V/A divider has no
    // meaning — drop the permanent one (user-added dividers stay).
    if (firstAudio == SIZE_MAX || !hasVideo) {
        for (size_t i = m_timeline->trackCount(); i-- > 0; ) {
            Track* t = m_timeline->track(i);
            if (t && t->isPermanentDivider())
                m_timeline->removeTrack(i);
        }
        return;
    }

    // PERMANENT-DIVIDER POLICY
    //
    // Identity-first, position-second. The persisted Track::isPermanentDivider
    // flag (v20+) tells us WHICH divider is the auto-managed boundary one;
    // we move IT into the slot at firstAudio-1 if it's drifted. We never
    // promote whatever divider happens to be sitting at the boundary slot —
    // that previously hijacked user dividers (e.g. "Add Divider Above A1")
    // by silently turning them into the new permanent and demoting the old.
    //
    // Adoption (promoting a non-permanent divider to permanent) only happens
    // for legacy files that pre-date v20 OR for fresh timelines that never
    // had a permanent divider — and even then we only adopt the one that's
    // ALREADY at firstAudio-1 by chance, never relocating user dividers.
    size_t existingPermIdx = SIZE_MAX;
    for (size_t i = 0; i < m_timeline->trackCount(); ++i) {
        Track* t = m_timeline->track(i);
        if (t && t->isPermanentDivider()) {
            if (existingPermIdx == SIZE_MAX) existingPermIdx = i;
            else t->setPermanentDivider(false); // demote duplicates
        }
    }

    Track* boundary = nullptr;
    if (existingPermIdx != SIZE_MAX) {
        // We know which divider IS permanent — move it back to the
        // boundary slot if the user (or a recent insert) bumped it out.
        if (existingPermIdx != firstAudio - 1) {
            m_timeline->moveTrack(existingPermIdx, firstAudio - 1);
        }
        boundary = m_timeline->track(firstAudio - 1);
    } else {
        // No persisted permanent. Adopt the boundary divider if there
        // happens to be one (legacy/v18-v19 migration); otherwise insert
        // a fresh permanent divider. Never relocates other dividers.
        if (firstAudio > 0) {
            Track* slot = m_timeline->track(firstAudio - 1);
            if (slot && slot->isDivider())
                boundary = slot;
        }
        if (!boundary)
            boundary = m_timeline->addDividerTrack(firstAudio, /*permanent*/true);
    }
    normalizePermanent(boundary);
}

void TimelinePanel::addDividerUndoable(size_t insertAt)
{
    if (!m_timeline) return;
    if (insertAt > m_timeline->trackCount())
        insertAt = m_timeline->trackCount();

    QPointer<TimelinePanel> safeThis(this);
    auto redo = [safeThis, insertAt]() {
        if (!safeThis || !safeThis->m_timeline) return;
        safeThis->m_timeline->addDividerTrack(insertAt, /*permanent*/false);
        safeThis->rebuildTracks();
    };
    auto undo = [safeThis, insertAt]() {
        if (!safeThis || !safeThis->m_timeline) return;
        if (insertAt >= safeThis->m_timeline->trackCount()) return;
        Track* t = safeThis->m_timeline->track(insertAt);
        // Defensive: only remove if it really is the user divider we added —
        // skip permanent dividers in case ensureSectionDivider has reshuffled.
        if (!t || !t->isDivider() || t->isPermanentDivider()) return;
        safeThis->m_timeline->removeTrack(insertAt);
        safeThis->rebuildTracks();
    };

    if (m_commandStack) {
        m_commandStack->execute(std::make_unique<LambdaCommand>(
            "Add Divider Track", std::move(redo), std::move(undo)));
    } else {
        redo();
    }
}

void TimelinePanel::refreshTrackContents()
{
    if (!m_timeline) return;

    // Incrementally update waveform/thumbnail caches for any new clip IDs.
    // With the path-based secondary caches this is essentially free for
    // splits (same source file → instant copy from m_waveformByPath /
    // m_thumbnailByPath).
    loadWaveforms();
    loadThumbnails();

    // Ensure all track widgets point to the latest caches
    // (pointers are stable but set them on any new widgets).
    for (auto tw : m_trackWidgets) {
        tw->setWaveformCache(&m_waveformPeaks);
        tw->setThumbnailCache(&m_thumbnailCache);
        tw->setAnimVideoCache(m_animVideoCache);
    }

    // Repaint all track widgets so the new clip geometry is visible.
    onScrollChanged();
}

// ═════════════════════════════════════════════════════════════════════════════
//  Simple accessors / setters
// ═════════════════════════════════════════════════════════════════════════════

void TimelinePanel::clearGapSelection()
{
    m_gapSelection.active = false;
    for (auto tw : m_trackWidgets)
        tw->setGapHighlight(-1, -1);
}

void TimelinePanel::clearTransitionSelection()
{
    m_selectedTransitionTrack = SIZE_MAX;
    m_selectedTransitionIndex = SIZE_MAX;
    for (auto tw : m_trackWidgets)
        tw->setSelectedTransition(SIZE_MAX);
}

void TimelinePanel::notifyZoomChanged()
{
    onScrollChanged();
}

void TimelinePanel::setAnimVideoCache(const AnimationVideoCache* cache)
{
    m_animVideoCache = cache;
    for (auto tw : m_trackWidgets)
        tw->setAnimVideoCache(cache);
}

} // namespace rt
