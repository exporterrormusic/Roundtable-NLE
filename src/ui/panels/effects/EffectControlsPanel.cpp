/*
 * EffectControlsPanel.cpp -- clip binding, transform, and keyframe ops.
 *
 * UI construction  --> EffectControlsPanelUI.cpp
 * Property tree    --> EffectControlsPanelTree.cpp
 */

#include "panels/effects/EffectControlsPanel.h"
#include "widgets/ScrubbySpinBox.h"
#include "Theme.h"

#include "timeline/Clip.h"
#include "timeline/AudioClip.h"
#include "timeline/ImageClip.h"
#include "timeline/VideoClip.h"
#include "timeline/SpineClip.h"
#include "timeline/PngPuppetClip.h"
#include "timeline/Track.h"
#include "timeline/Timeline.h"
#include "timeline/KeyframeTrack.h"
#include "timeline/GraphicLayer.h"
#include "timeline/GraphicClip.h"
#include "playback/PlaybackController.h"
#include "command/CommandStack.h"
#include "command/LambdaCommand.h"
#include "command/commands/EffectCommands.h"
#include "command/commands/KeyframeCmds.h"
#include "effects/Effect.h"

#include <QApplication>
#include <QDoubleSpinBox>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QImage>
#include <QMimeData>

#include <cmath>
#include <algorithm>
#include <mutex>
#include <string>
#include <unordered_map>

namespace rt {

namespace {
// Audio-volume spin is displayed in dB. Keyframe track stores linear gain.
// dB range used by the UI: [-60 .. +12]. Values at the min are treated as
// effectively muted (1e-3 gain) for a smooth ramp.
constexpr float kAudioVolumeMinDb = -60.0f;
constexpr float kAudioVolumeMaxDb = 12.0f;

inline float dbToGain(float db) noexcept {
    if (db <= kAudioVolumeMinDb) return 0.0f;
    return std::pow(10.0f, db / 20.0f);
}
inline float gainToDb(float gain) noexcept {
    if (gain <= 0.0f) return kAudioVolumeMinDb;
    float db = 20.0f * std::log10(gain);
    if (db < kAudioVolumeMinDb) db = kAudioVolumeMinDb;
    if (db > kAudioVolumeMaxDb) db = kAudioVolumeMaxDb;
    return db;
}

// Native pixel dimensions of a PNG puppet's resting face, cached per path to
// avoid repeated disk decodes.  PngPuppetClip doesn't store its source size
// (it's just the decoded PNG), so the Scale display probes it the same way the
// transform overlay does (TimelineWorkspaceOverlay.cpp).
bool puppetNativeDims(PngPuppetClip* clip, uint32_t& outW, uint32_t& outH)
{
    if (!clip) return false;
    std::string path = clip->facePath(PngPuppetClip::MouthClosedEyesOpen);
    if (path.empty()) return false;

    static std::mutex s_mtx;
    static std::unordered_map<std::string, std::pair<uint32_t, uint32_t>> s_cache;
    std::lock_guard<std::mutex> lock(s_mtx);
    auto it = s_cache.find(path);
    if (it == s_cache.end()) {
        QImage img(QString::fromStdString(path));
        std::pair<uint32_t, uint32_t> dims{0, 0};
        if (!img.isNull() && img.width() > 0 && img.height() > 0)
            dims = {static_cast<uint32_t>(img.width()),
                    static_cast<uint32_t>(img.height())};
        it = s_cache.emplace(path, dims).first;
    }
    outW = it->second.first;
    outH = it->second.second;
    return outW > 0 && outH > 0;
}
} // namespace

// ─── Per-layer Motion source indirection ─────────────────────────────────
// When a graphic layer is selected (Essential Graphics → text/shape inside
// a GraphicClip), the Motion section in Effect Controls binds to that
// LAYER's transform instead of the clip's, matching Premiere's per-layer
// Vector Motion behavior. Otherwise it binds to the clip's tracks. The
// six helpers below resolve to the right KeyframeTrack at each call site.

KeyframeTrack<float>* EffectControlsPanel::effPosX() noexcept
{
    if (m_graphicLayer) return &m_graphicLayer->transform().posX;
    return m_clip ? &m_clip->positionX() : nullptr;
}
KeyframeTrack<float>* EffectControlsPanel::effPosY() noexcept
{
    if (m_graphicLayer) return &m_graphicLayer->transform().posY;
    return m_clip ? &m_clip->positionY() : nullptr;
}
KeyframeTrack<float>* EffectControlsPanel::effScaleX() noexcept
{
    if (m_graphicLayer) return &m_graphicLayer->transform().scaleX;
    return m_clip ? &m_clip->scaleX() : nullptr;
}
KeyframeTrack<float>* EffectControlsPanel::effScaleY() noexcept
{
    if (m_graphicLayer) return &m_graphicLayer->transform().scaleY;
    return m_clip ? &m_clip->scaleY() : nullptr;
}
KeyframeTrack<float>* EffectControlsPanel::effRotation() noexcept
{
    if (m_graphicLayer) return &m_graphicLayer->transform().rotation;
    return m_clip ? &m_clip->rotation() : nullptr;
}
KeyframeTrack<float>* EffectControlsPanel::effShutterAngle() noexcept
{
    // Motion blur belongs to the rendered clip as a whole. A selected
    // Essential Graphics child still uses the parent clip's shutter.
    return m_clip ? &m_clip->shutterAngle() : nullptr;
}
KeyframeTrack<float>* EffectControlsPanel::effOpacity() noexcept
{
    if (m_graphicLayer) return &m_graphicLayer->transform().opacity;
    return m_clip ? &m_clip->opacity() : nullptr;
}
KeyframeTrack<float>* EffectControlsPanel::effAnchorX() noexcept
{
    if (m_graphicLayer) return &m_graphicLayer->transform().anchorX;
    return m_clip ? &m_clip->anchorX() : nullptr;
}
KeyframeTrack<float>* EffectControlsPanel::effAnchorY() noexcept
{
    if (m_graphicLayer) return &m_graphicLayer->transform().anchorY;
    return m_clip ? &m_clip->anchorY() : nullptr;
}

double EffectControlsPanel::posDisplayFactorX() const noexcept
{
    // Layer transform's posX is in PROJECT-resolution pixels (= sequence
    // pixels), so no conversion. Clip-level positionX is stored REF-1920;
    // multiply by seqW/1920 for the seq-px display value.
    if (m_graphicLayer) return 1.0;
    return (m_seqW > 0 ? static_cast<double>(m_seqW) : 1920.0) / 1920.0;
}
double EffectControlsPanel::posDisplayFactorY() const noexcept
{
    if (m_graphicLayer) return 1.0;
    return (m_seqH > 0 ? static_cast<double>(m_seqH) : 1080.0) / 1080.0;
}

void EffectControlsPanel::setSelectedGraphicLayer(GraphicLayer* layer)
{
    if (m_graphicLayer == layer) return;
    m_graphicLayer = layer;
    if (!m_clip) return;
    // CRITICAL: do NOT rebuild the property tree here. layerSelected can
    // fire synchronously from inside a paint cycle (Qt direct-connect
    // default), and destroying child widgets mid-paint leaves Qt's
    // sibling-iterator with a dangling pointer — the next
    // QWidget::isVisible() crashes the app. Instead, retarget the
    // existing rows' track pointers in place; all read/write call sites
    // already route through eff*() helpers, so the spinboxes will write
    // to the new source on next edit, and populateFromClip below pulls
    // the new values into the spinboxes.
    m_updating = true;
    if (m_posRow) {
        m_posRow->setTrack(effPosX());
        m_posRow->clearExtraTracks();
        m_posRow->addExtraTrack(effPosY());
    }
    if (m_scaleRow) {
        m_scaleRow->setTrack(effScaleX());
        m_scaleRow->clearExtraTracks();
        if (m_uniformScaleCheck && m_uniformScaleCheck->isChecked())
            m_scaleRow->addExtraTrack(effScaleY());
    }
    if (m_scaleWRow)   m_scaleWRow->setTrack(effScaleY());
    if (m_rotationRow) m_rotationRow->setTrack(effRotation());
    if (m_shutterAngleRow) m_shutterAngleRow->setTrack(effShutterAngle());
    if (m_anchorRow) {
        m_anchorRow->setTrack(effAnchorX());
        m_anchorRow->clearExtraTracks();
        m_anchorRow->addExtraTrack(effAnchorY());
    }
    if (m_opacityRow)  m_opacityRow->setTrack(effOpacity());
    populateFromClip();
    m_updating = false;
}

void EffectControlsPanel::setClip(Clip* clip, Track* track)
{
    const bool editable = !track || !track->isLocked();
    if (m_splitter) m_splitter->setEnabled(editable);
    if (m_clip == clip) {
        m_track = track;
        return;
    }
    m_transformEditSpin = nullptr;
    m_transformEditBefore.clear();
    // Effect/mask keyboard selection belongs to the previous clip.  Keeping
    // either stable ID selected across a clip switch would make the next
    // Delete target invisible state (or consume the key for no visible row).
    m_selectedEffectIndex = -1;
    m_hasSelectedMask = false;
    m_selectedMaskEffectId = 0;
    m_selectedMaskId = 0;
    // A fresh clip starts with no per-layer selection — the workspace
    // calls setSelectedGraphicLayer() afterwards if Essential Graphics
    // already has one chosen for this clip.
    m_graphicLayer = nullptr;
    m_clip = clip;
    m_track = track;
    m_updating = true;

    clearPropertyTree();

    if (clip) {
        m_clipNameLabel->setText(QString::fromStdString(clip->label()));

        const char* typeStr = "Video";
        QColor badgeColor;
        const auto& tc = Theme::colors();
        switch (clip->clipType()) {
        case ClipType::Spine:      typeStr = "Spine";      badgeColor = QColor(200, 170, 255); break;
        case ClipType::Video:      typeStr = "Video";      badgeColor = tc.accent; break;
        case ClipType::Audio:      typeStr = "Audio";      badgeColor = tc.success; break;
        case ClipType::Title:      typeStr = "Title";      badgeColor = QColor(230, 180, 80); break;
        case ClipType::Adjustment: typeStr = "Adjustment"; badgeColor = QColor(180, 180, 180); break;
        case ClipType::Image:      typeStr = "Image";      badgeColor = tc.accent; break;
        case ClipType::Graphic:    typeStr = "Graphic";    badgeColor = QColor(230, 130, 80); break;
        case ClipType::Sequence:   typeStr = "Sequence";   badgeColor = QColor(180, 130, 230); break;
        }
        m_clipTypeLabel->setText(typeStr);
        m_clipTypeLabel->setStyleSheet(QStringLiteral(
            "QLabel { color: %1; font-size: %3px; font-weight: bold; "
            "background: %2; border-radius: %4px; padding: 1px 6px; }")
            .arg(Theme::hex(tc.textPrimary),
                 Theme::hex(badgeColor.darker(200)))
            .arg(Theme::typography().sizeXxs)
            .arg(Theme::metrics().radiusSm));

        buildPropertyTree();
        populateFromClip();

        m_kfTimeline->setClip(clip);
        m_kfTimeline->setPropertyRows(m_propertyRows);
        syncMaskPathTimelineLanes();

        // Prime each PropertyRow's visual/nav state at the current playhead.
        // The click itself resolves the live Timeline time, so this cached
        // state cannot redirect a later click to an older frame.
        const int64_t relTick = clipRelativeTick();
        for (auto* row : m_propertyRows)
            row->updateForTime(relTick);

        // Show property tree, hide empty state
        m_splitter->show();
        m_emptyLabel->hide();
    } else {
        m_clipNameLabel->setText("No clip selected");
        m_clipTypeLabel->clear();
        m_clipTypeLabel->setStyleSheet(QStringLiteral(
            "QLabel { background: transparent; border: none; }"));
        m_kfTimeline->setClip(nullptr);
        m_kfTimeline->setMaskPathLanes({});

        // Hide property tree, show empty state
        m_splitter->hide();
        m_emptyLabel->show();
    }

    m_updating = false;
    emit clipChanged(clip);
}

void EffectControlsPanel::clearClip()
{
    setClip(nullptr);
}

void EffectControlsPanel::removeAllKeyframes()
{
    if (!m_clip) return;

    // Full snapshots so undo restores values, interpolation, tangents, and
    // complete Mask Path geometry exactly.
    struct TrackSnap {
        KeyframeTrack<float>*        trk;
        float                        oldDefault;
        std::vector<Keyframe<float>> oldKfs;
        float                        collapseVal;  // static value to keep
    };
    struct MaskSnap {
        quint64 effectId;
        uint64_t maskId;
        OpacityMask before;
        OpacityMask after;
    };
    std::vector<TrackSnap> snaps;
    std::vector<MaskSnap> maskSnaps;
    const int64_t t = clipRelativeTick();

    auto addTrack = [&](KeyframeTrack<float>* trk) {
        if (!trk || trk->keyframeCount() == 0) return;     // nothing to remove
        for (const auto& s : snaps) if (s.trk == trk) return;  // dedupe
        // Collapse to the value the track shows at the current playhead so the
        // current frame looks unchanged; only the motion is removed.
        snaps.push_back({trk, trk->defaultValue(), trk->keyframes(),
                         trk->evaluate(t)});
    };
    auto addMask = [&](OpacityMask& mask, quint64 effectId) {
        const bool hasKeys = !mask.pathKeys.empty()
            || mask.feather.keyframeCount() > 0
            || mask.maskOpacity.keyframeCount() > 0
            || mask.expansion.keyframeCount() > 0;
        if (!hasKeys) return;

        OpacityMask after = mask;
        if (!after.pathKeys.empty()) {
            after.base = after.geometryAt(t);
            after.pathKeys.clear();
            after.pathAnimated = false;
        }
        auto collapse = [t](KeyframeTrack<float>& track) {
            if (track.keyframeCount() == 0) return;
            const float value = track.evaluate(t);
            while (track.keyframeCount() > 0) track.removeKeyframe(0);
            track.setDefaultValue(value);
        };
        collapse(after.feather);
        collapse(after.maskOpacity);
        collapse(after.expansion);
        maskSnaps.push_back({effectId, mask.maskId, mask, std::move(after)});
    };

    // Clip-level transform tracks.
    addTrack(&m_clip->positionX());
    addTrack(&m_clip->positionY());
    addTrack(&m_clip->scaleX());
    addTrack(&m_clip->scaleY());
    addTrack(&m_clip->rotation());
    addTrack(&m_clip->shutterAngle());
    addTrack(&m_clip->opacity());
    addTrack(&m_clip->anchorX());
    addTrack(&m_clip->anchorY());
    if (auto* ac = dynamic_cast<AudioClip*>(m_clip)) {
        addTrack(&ac->pan());
        addTrack(&ac->volume());
    }
    // GraphicClip: every text/shape layer's own (per-layer) transform tracks —
    // these are invisible in Effect Controls unless that layer is selected.
    if (auto* gc = dynamic_cast<GraphicClip*>(m_clip)) {
        for (size_t i = 0; i < gc->layerCount(); ++i) {
            auto* layer = gc->layer(i);
            if (!layer) continue;
            auto& tf = layer->transform();
            addTrack(&tf.posX);    addTrack(&tf.posY);
            addTrack(&tf.scaleX);  addTrack(&tf.scaleY);
            addTrack(&tf.rotation);
            addTrack(&tf.opacity);
            addTrack(&tf.anchorX); addTrack(&tf.anchorY);
            if (layer->layerType() == GraphicLayerType::Text) {
                auto* text = static_cast<TextLayer*>(layer);
                addTrack(&text->tracking());
                addTrack(&text->leading());
                addTrack(&text->baselineShift());
            }
        }
    }

    for (auto& mask : m_clip->masks()) addMask(mask, 0);
    auto& effects = m_clip->effects();
    for (size_t i = 0; i < effects.effectCount(); ++i) {
        Effect& effect = effects.effect(i);
        for (size_t p = 0; p < effect.paramCount(); ++p)
            addTrack(&effect.param(p).track);
        for (auto& mask : effect.masks()) addMask(mask, effect.id());
    }

    if (snaps.empty() && maskSnaps.empty()) return;

    Clip* clip = m_clip;
    auto resolveMask = [clip](quint64 effectId, uint64_t maskId)
            -> OpacityMask* {
        std::vector<OpacityMask>* masks = nullptr;
        if (effectId == 0) masks = &clip->masks();
        else if (Effect* effect = clip->effects().effectById(effectId))
            masks = &effect->masks();
        if (!masks) return nullptr;
        const auto it = std::find_if(masks->begin(), masks->end(),
            [maskId](const OpacityMask& mask) {
                return mask.maskId == maskId;
            });
        return it == masks->end() ? nullptr : &*it;
    };

    auto* panel = this;
    auto doRemove = [snaps, maskSnaps, resolveMask, panel]() {
        for (const auto& s : snaps) {
            while (s.trk->keyframeCount() > 0) s.trk->removeKeyframe(0);
            s.trk->setDefaultValue(s.collapseVal);
        }
        for (const auto& s : maskSnaps)
            if (auto* mask = resolveMask(s.effectId, s.maskId)) *mask = s.after;
        panel->refresh();
        if (!maskSnaps.empty()) emit panel->maskChanged();
        emit panel->propertyChanged();
    };
    auto undoRemove = [snaps, maskSnaps, resolveMask, panel]() {
        for (const auto& s : snaps) {
            while (s.trk->keyframeCount() > 0) s.trk->removeKeyframe(0);
            s.trk->setDefaultValue(s.oldDefault);
            for (const auto& kf : s.oldKfs) s.trk->restoreKeyframe(kf);
        }
        for (const auto& s : maskSnaps)
            if (auto* mask = resolveMask(s.effectId, s.maskId)) *mask = s.before;
        panel->refresh();
        if (!maskSnaps.empty()) emit panel->maskChanged();
        emit panel->propertyChanged();
    };

    if (m_commandStack)
        m_commandStack->execute(std::make_unique<LambdaCommand>(
            "Remove All Keyframes", doRemove, undoRemove));
    else
        doRemove();
}

void EffectControlsPanel::refresh()
{
    if (!m_clip) return;
    m_updating = true;
    clearPropertyTree();
    buildPropertyTree();
    populateFromClip();
    m_kfTimeline->setPropertyRows(m_propertyRows);
    syncMaskPathTimelineLanes();
    m_updating = false;

    // Update property-row button states (diamond add/remove, prev/next)
    // and force the mini-timeline to repaint with current keyframe data.
    const int64_t relTick = clipRelativeTick();
    for (auto* row : m_propertyRows)
        row->updateForTime(relTick);
    updateMaskPathControls(relTick);
    m_kfTimeline->update();
}


void EffectControlsPanel::setPlayheadTick(int64_t tick)
{
    m_playheadTick = tick;
    m_kfTimeline->setPlayheadTick(tick);

    // Update keyframe nav buttons for current time
    const int64_t relTick = m_clip
        ? std::clamp<int64_t>(tick - m_clip->timelineIn(), 0,
                              std::max<int64_t>(0, m_clip->duration()))
        : tick;
    for (auto* row : m_propertyRows) {
        row->updateForTime(relTick);
    }
    updateMaskPathControls(relTick);

    // Update footer timecode (assume 24fps if no clip frame rate available)
    if (m_footerTimecodeLabel) {
        double fps = 24.0;
        auto tc = tickToTimecode(tick, fps);
        m_footerTimecodeLabel->setText(QString::fromStdString(tc.toString()));
    }

    // Update spin box values to reflect evaluated keyframes at current time.
    // Skip if the user is mid-edit in one of our spinboxes (focused) —
    // populateFromClip() calls setValue() which would clobber the typed
    // text. Without this guard, typing a new audio volume during playback
    // was silently reset on the next playback tick, so the change never
    // made it through to the audio mixer.
    if (m_clip && !m_updating && !isAnySpinBoxBeingEdited()) {
        m_updating = true;
        populateFromClip();
        m_updating = false;
    }
}

bool EffectControlsPanel::isAnySpinBoxBeingEdited() const
{
    QWidget* fw = QApplication::focusWidget();
    if (!fw) return false;
    // Spinbox's internal QLineEdit holds focus while editing — walk up
    // until we either find a QDoubleSpinBox child of THIS panel or run out.
    for (QWidget* w = fw; w; w = w->parentWidget()) {
        if (qobject_cast<QDoubleSpinBox*>(w) && isAncestorOf(w))
            return true;
    }
    return false;
}

// EDIT INVARIANT (see CommandStack.h "EDIT DISCIPLINE"): keyframe time is
// clip-relative. Always go through this when reading/writing a clip KeyframeTrack.
int64_t EffectControlsPanel::clipRelativeTick() const noexcept
{
    if (!m_clip) return 0;
    // Prefer the Timeline's live playhead so a value edit fired right after
    // a scrub (before setPlayheadTick has propagated through signals) still
    // sees the *current* playhead and writes a new keyframe at the right
    // time. Falls back to the cached m_playheadTick when no timeline is set.
    const int64_t playhead = m_timeline ? m_timeline->playheadPosition()
                                        : m_playheadTick;
    return std::clamp<int64_t>(playhead - m_clip->timelineIn(), 0,
                               std::max<int64_t>(0, m_clip->duration()));
}

void EffectControlsPanel::updateMaskPathControls(int64_t clipLocalTime)
{
    if (!m_clip) return;
    const int64_t duration = std::max<int64_t>(0, m_clip->duration());
    const int64_t t = std::clamp<int64_t>(clipLocalTime, 0, duration);
    for (const auto& controls : m_maskPathControls) {
        auto* masks = maskListFor(controls.effectId);
        if (!masks) continue;
        auto maskIt = std::find_if(masks->begin(), masks->end(),
            [&controls](const OpacityMask& mask) {
                return mask.maskId == controls.maskId;
            });
        if (maskIt == masks->end()) continue;
        const auto& mask = *maskIt;
        const bool animated = mask.pathAnimated;
        const bool atKey = animated && mask.hasPathKeyAt(t);
        const bool hasPrev = animated && mask.prevPathKeyTime(t) != t;
        const bool hasNext = animated && mask.nextPathKeyTime(t) != t;

        if (controls.stopwatch) {
            controls.stopwatch->blockSignals(true);
            controls.stopwatch->setChecked(animated);
            controls.stopwatch->blockSignals(false);
        }
        if (controls.diamond) {
            controls.diamond->setEnabled(animated);
            controls.diamond->setChecked(atKey);
        }
        if (controls.previous) controls.previous->setEnabled(hasPrev);
        if (controls.next) controls.next->setEnabled(hasNext);
    }
    if (m_kfTimeline) m_kfTimeline->update();
}

void EffectControlsPanel::syncMaskPathTimelineLanes()
{
    if (!m_kfTimeline) return;
    std::vector<KeyframeTimeline::MaskPathLane> lanes;
    lanes.reserve(m_maskPathControls.size());
    for (const auto& controls : m_maskPathControls) {
        if (controls.row)
            lanes.push_back(
                {controls.row, controls.effectId, controls.maskId});
    }
    m_kfTimeline->setMaskPathLanes(lanes);
}

void EffectControlsPanel::syncValuesFromClip()
{
    if (!m_clip || m_updating) return;
    m_updating = true;
    populateFromClip();
    // External edits (program-monitor transform drag, viewport scrub)
    // create / move keyframes on the bound tracks — the mini-timeline
    // needs to repaint so the diamonds reflect the new positions live.
    if (m_kfTimeline) m_kfTimeline->update();
    for (auto* row : m_propertyRows)
        if (row) row->updateForTime(clipRelativeTick());
    updateMaskPathControls(clipRelativeTick());
    m_updating = false;
}

double EffectControlsPanel::coverFitForCurrentClip() const noexcept
{
    // Default factor 1.0 → no conversion (existing fill-model display).
    if (!m_clip || m_seqW == 0 || m_seqH == 0) return 1.0;

    uint32_t srcW = 0, srcH = 0;
    if (auto* ic = dynamic_cast<ImageClip*>(m_clip)) {
        srcW = ic->sourceWidth();
        srcH = ic->sourceHeight();
    } else if (auto* vc = dynamic_cast<VideoClip*>(m_clip)) {
        // Video CHARACTERS are composited with the baked 0.85 / containFit
        // factor and don't have a meaningful native-pixel size; leave their
        // Scale display on the fill model.
        if (vc->isVideoCharacter()) return 1.0;
        srcW = vc->sourceWidth();
        srcH = vc->sourceHeight();
    } else if (auto* pc = dynamic_cast<PngPuppetClip*>(m_clip)) {
        // PNG puppets are composited like characters: CONTAIN-fit the native
        // PNG into the canvas, then the shared 0.85× compose-fit (see
        // CompositeServiceLayerBuild.cpp where layer.containFit = true).  So a
        // stored scale of 1.0 does NOT render the PNG 1:1 — the displayed
        // percentage must fold in contain-fit × 0.85 the same way image/video
        // clips fold in cover-fit, otherwise the number always reads 100%.
        uint32_t pw = 0, ph = 0;
        if (!puppetNativeDims(pc, pw, ph)) return 1.0;
        const double sx = static_cast<double>(m_seqW) / static_cast<double>(pw);
        const double sy = static_cast<double>(m_seqH) / static_cast<double>(ph);
        constexpr double kComposeFit = 0.85;
        return std::min(sx, sy) * kComposeFit;
    } else {
        // SpineClip, TitleClip, GraphicClip, etc. — keep existing fill-model
        // Scale numbers.  Their "native pixels" is either an arbitrary cache
        // raster (spine) or a canvas-relative authoring unit (title/graphic).
        return 1.0;
    }

    if (srcW == 0 || srcH == 0) return 1.0;

    // Same factor the compositor's cover-fit applies under the hood.
    const double sx = static_cast<double>(m_seqW) / static_cast<double>(srcW);
    const double sy = static_cast<double>(m_seqH) / static_cast<double>(srcH);
    return std::max(sx, sy);
}


EffectControlsPanel::TransformState EffectControlsPanel::captureTransformState() const
{
    if (!m_clip) return {};
    int64_t t = clipRelativeTick();
    auto evalOr = [](KeyframeTrack<float>* trk, int64_t time, float fallback) {
        return trk ? trk->evaluate(time) : fallback;
    };
    TransformState s{
        evalOr(const_cast<EffectControlsPanel*>(this)->effPosX(),     t, 0.0f),
        evalOr(const_cast<EffectControlsPanel*>(this)->effPosY(),     t, 0.0f),
        evalOr(const_cast<EffectControlsPanel*>(this)->effScaleX(),   t, 1.0f),
        evalOr(const_cast<EffectControlsPanel*>(this)->effScaleY(),   t, 1.0f),
        evalOr(const_cast<EffectControlsPanel*>(this)->effRotation(), t, 0.0f),
        evalOr(const_cast<EffectControlsPanel*>(this)->effOpacity(),  t, 1.0f),
        m_clip->speed()
    };
    if (auto* ac = dynamic_cast<AudioClip*>(m_clip)) {
        s.pan    = ac->pan().evaluate(t);
        s.volume = ac->volume().evaluate(t);
    }
    return s;
}

void EffectControlsPanel::restoreTransformState(const TransformState& s)
{
    if (!m_clip) return;
    int64_t t = clipRelativeTick();
    auto writeIf = [t](KeyframeTrack<float>* trk, float val) {
        if (trk) trk->writeValue(t, val);
    };
    writeIf(effPosX(),     s.posX);
    writeIf(effPosY(),     s.posY);
    writeIf(effScaleX(),   s.scaleX);
    writeIf(effScaleY(),   s.scaleY);
    writeIf(effRotation(), s.rotation);
    writeIf(effOpacity(),  s.opacity);
    m_clip->setSpeed(s.speed);
    if (auto* ac = dynamic_cast<AudioClip*>(m_clip)) {
        ac->pan().writeValue(t, s.pan);
        ac->volume().writeValue(t, s.volume);
    }
}


void EffectControlsPanel::applyTransformLive()
{
    // Called during scrub drag — update ONLY the property being scrubbed.
    // Non-animated tracks (no keyframes) update the default value.
    // Animated tracks (has keyframes) write at the current playhead time.
    if (!m_clip || m_updating) return;

    auto* spin = qobject_cast<ScrubbySpinBox*>(sender());
    if (!spin) return;

    // Helper: write to a track — animated tracks get a keyframe, static tracks update default
    auto writeTrack = [this](KeyframeTrack<float>& track, float val) {
        track.writeValue(clipRelativeTick(), val);
    };

    auto writeIfTrack = [&](KeyframeTrack<float>* trk, float val) {
        if (trk) writeTrack(*trk, val);
    };
    // Compound-pin helper: when EITHER track of a compound 2D row (Position
    // X/Y, Anchor X/Y) is being scrubbed and the row is animated, BOTH
    // components must get a keyframe at the playhead — otherwise X keeps
    // accumulating keyframes while Y is silently absent, and dragging the
    // shared diamond moves only X (the "Y position isn't being tracked"
    // bug).  When the row is static (neither track has keyframes yet),
    // writeIfTrack still falls through to updating the default value.
    auto writeCompound = [this](KeyframeTrack<float>* updated, float updatedVal,
                                KeyframeTrack<float>* sibling) {
        if (!updated) return;
        const bool animated = (updated->keyframeCount() > 0) ||
                              (sibling && sibling->keyframeCount() > 0);
        if (animated) {
            const int64_t t = clipRelativeTick();
            updated->addKeyframe(t, updatedVal);
            if (sibling)
                sibling->addKeyframe(t, sibling->evaluate(t));
        } else {
            updated->writeValue(clipRelativeTick(), updatedVal);
        }
    };

    if (spin == m_posXSpin) {
        // Spin shows display-space pixels; divide by display-factor on write.
        // (clip-level: stored REF-1920, factor = seqW/1920. layer-level:
        // stored project-px, factor = 1.0.)
        writeCompound(effPosX(),
                      static_cast<float>(spin->value() / posDisplayFactorX()),
                      effPosY());
    } else if (spin == m_posYSpin) {
        writeCompound(effPosY(),
                      static_cast<float>(spin->value() / posDisplayFactorY()),
                      effPosX());
    } else if (spin == m_scaleSpin || spin == m_scaleWSpin) {
        // Spin shows Premiere-style native percentage MAGNITUDE; convert back
        // to the engine's cover-fit-multiplier storage so the compositor
        // renders exactly the same on-screen size the displayed number
        // promises.  Cover-fit is 1.0 for graphic clips.
        const double sf = coverFitForCurrentClip();
        const double divisor = (sf > 0.0001) ? sf : 1.0;
        const float mag = static_cast<float>(spin->value() / 100.0 / divisor);
        const int64_t t = clipRelativeTick();
        auto* sx = effScaleX();
        auto* sy = effScaleY();
        // Flip H/V is stored as the SIGN of scaleX/scaleY.  The spinner only
        // carries magnitude, so re-apply each track's CURRENT sign — otherwise
        // editing scale silently un-flips the clip (or, combined with the
        // min=0 spinner clamp, collapses it to 0).
        auto signedFor = [&](KeyframeTrack<float>* trk) -> float {
            return (trk && trk->evaluate(t) < 0.0f) ? -mag : mag;
        };
        const bool uniform = m_uniformScaleCheck && m_uniformScaleCheck->isChecked();
        // Which track this spinner primarily drives.
        auto* primary = (spin == m_scaleSpin) ? sx : sy;
        if (uniform) {
            // Both axes share the magnitude but keep their own flip signs.
            if (sx || sy) {
                const bool animated = (sx && sx->keyframeCount() > 0)
                                   || (sy && sy->keyframeCount() > 0);
                const float sxv = signedFor(sx);
                const float syv = signedFor(sy);
                if (animated) {
                    if (sx) sx->addKeyframe(t, sxv);
                    if (sy) sy->addKeyframe(t, syv);
                } else {
                    if (sx) sx->writeValue(t, sxv);
                    if (sy) sy->writeValue(t, syv);
                }
            }
        } else {
            writeIfTrack(primary, signedFor(primary));
        }
    } else if (spin == m_rotationSpin) {
        writeIfTrack(effRotation(), static_cast<float>(spin->value()));
    } else if (spin == m_shutterAngleSpin) {
        writeIfTrack(effShutterAngle(), static_cast<float>(spin->value()));
    } else if (spin == m_opacitySpin) {
        writeIfTrack(effOpacity(), static_cast<float>(spin->value() / 100.0));
    } else if (spin == m_anchorXSpin) {
        writeCompound(effAnchorX(),
                      static_cast<float>(spin->value() / posDisplayFactorX()),
                      effAnchorY());
    } else if (spin == m_anchorYSpin) {
        writeCompound(effAnchorY(),
                      static_cast<float>(spin->value() / posDisplayFactorY()),
                      effAnchorX());
    } else if (spin == m_cropLeftSpin || spin == m_cropRightSpin ||
               spin == m_cropTopSpin  || spin == m_cropBottomSpin) {
        // Crop is stored on the clip (not a keyframe track). Push all four
        // values at once since VideoClip/SpineClip::setCrop is atomic.
        writeCropFromSpins();
    } else if (spin == m_speedSpin) {
        double newSpd = spin->value() / 100.0;
        if (newSpd <= 0.0) newSpd = 0.01;
        double oldSpd = m_clip->speed();
        m_clip->setSpeed(newSpd);
        // Adjust duration to keep source range consistent
        int64_t newDur = static_cast<int64_t>(std::llround(m_clip->duration() * oldSpd / newSpd));
        if (newDur < kMinClipDuration) newDur = kMinClipDuration;
        // Clamp to next clip's start so we don't overlap
        if (m_track) {
            size_t ci = m_track->findClipIndexById(m_clip->id());
            for (size_t i = ci + 1; i < m_track->clipCount(); ++i) {
                const Clip* next = m_track->clip(i);
                if (next && next->timelineIn() > m_clip->timelineIn()) {
                    int64_t maxDur = next->timelineIn() - m_clip->timelineIn();
                    if (newDur > maxDur && maxDur >= kMinClipDuration) newDur = maxDur;
                    break;
                }
            }
        }
        m_clip->setDuration(newDur);
    } else if (auto* ac = dynamic_cast<AudioClip*>(m_clip)) {
        if (spin == m_panSpin) {
            float newPan = static_cast<float>(spin->value() / 100.0);
            writeTrack(ac->pan(), newPan);
            emit audioLevelsChanged(ac->id(), ac->volume().evaluate(clipRelativeTick()), newPan);
        }
        else if (spin == m_audioVolumeSpin) {
            // Spin displays dB; keyframe track stores linear gain.
            float newGain = dbToGain(static_cast<float>(spin->value()));
            writeTrack(ac->volume(), newGain);
            emit audioLevelsChanged(ac->id(), newGain, ac->pan().evaluate(clipRelativeTick()));
        }
    }

    emit propertyChanged();
}


bool EffectControlsPanel::clipHasCrop() const noexcept
{
    return m_clip && m_clip->supportsCrop();
}

void EffectControlsPanel::writeCropFromSpins()
{
    if (!m_clip || !m_cropLeftSpin || !m_cropRightSpin ||
        !m_cropTopSpin || !m_cropBottomSpin)
        return;
    const float l = static_cast<float>(m_cropLeftSpin->value());
    const float r = static_cast<float>(m_cropRightSpin->value());
    const float t = static_cast<float>(m_cropTopSpin->value());
    const float b = static_cast<float>(m_cropBottomSpin->value());
    if (auto* vc = dynamic_cast<VideoClip*>(m_clip)) {
        vc->setCrop(l, r, t, b);
    } else if (auto* sc = dynamic_cast<SpineClip*>(m_clip)) {
        sc->setCrop(l, r, t, b);
    }
}


void EffectControlsPanel::commitTransform(double /*oldVal*/, double /*newVal*/)
{
    // Called at end of scrub — push per-property undo command.
    if (!m_clip || m_updating) return;

    auto* spin = qobject_cast<ScrubbySpinBox*>(sender());
    if (!spin) return;
    auto editBefore = std::move(m_transformEditBefore);
    m_transformEditBefore.clear();
    m_transformEditSpin = nullptr;

    // Crop has no keyframe track — it lives on the clip. Build an undo
    // command from the four spins' pre-scrub vs current values (only the
    // scrubbed axis differs; the others' old == new, so they're no-ops).
    if (spin == m_cropLeftSpin || spin == m_cropRightSpin ||
        spin == m_cropTopSpin  || spin == m_cropBottomSpin) {
        if (!clipHasCrop()) return;
        auto axisOld = [spin](ScrubbySpinBox* s) {
            return static_cast<float>(s == spin ? s->scrubStartValue() : s->value());
        };
        const float oL = axisOld(m_cropLeftSpin);
        const float oR = axisOld(m_cropRightSpin);
        const float oT = axisOld(m_cropTopSpin);
        const float oB = axisOld(m_cropBottomSpin);
        const float nL = static_cast<float>(m_cropLeftSpin->value());
        const float nR = static_cast<float>(m_cropRightSpin->value());
        const float nT = static_cast<float>(m_cropTopSpin->value());
        const float nB = static_cast<float>(m_cropBottomSpin->value());
        if (oL == nL && oR == nR && oT == nT && oB == nB) return;  // no change
        Clip* clip  = m_clip;
        auto* panel = this;
        auto setCrop = [clip](float l, float r, float t, float b) {
            if (auto* vc = dynamic_cast<VideoClip*>(clip)) vc->setCrop(l, r, t, b);
            else if (auto* sc = dynamic_cast<SpineClip*>(clip)) sc->setCrop(l, r, t, b);
        };
        if (m_commandStack) {
            m_commandStack->pushWithoutExecute(std::make_unique<LambdaCommand>(
                "Crop",
                [setCrop, nL, nR, nT, nB, panel]() {
                    setCrop(nL, nR, nT, nB);
                    panel->populateFromClip();
                    emit panel->propertyChanged();
                },
                [setCrop, oL, oR, oT, oB, panel]() {
                    setCrop(oL, oR, oT, oB);
                    panel->populateFromClip();
                    emit panel->propertyChanged();
                }));
        }
        return;
    }

    // Identify which track this spin operates on
    KeyframeTrack<float>* track = nullptr;
    double factor = 1.0;  // spin-display-value = track-value * factor
    bool uniformScale = false;

    if      (spin == m_posXSpin)     { track = effPosX();
                                       factor = posDisplayFactorX(); }
    else if (spin == m_posYSpin)     { track = effPosY();
                                       factor = posDisplayFactorY(); }
    else if (spin == m_scaleSpin)    { track = effScaleX();
                                       // displayed = stored × coverFit × 100,
                                       // so factor (= displayed / stored) is
                                       // 100 × coverFit.  Captured at commit
                                       // time so the undo command replays
                                       // with the same conversion used live.
                                       factor = 100.0 * coverFitForCurrentClip();
                                       uniformScale = m_uniformScaleCheck && m_uniformScaleCheck->isChecked(); }
    else if (spin == m_scaleWSpin)   { track = effScaleY();
                                       factor = 100.0 * coverFitForCurrentClip(); }
    else if (spin == m_rotationSpin) { track = effRotation(); }
    else if (spin == m_shutterAngleSpin) { track = effShutterAngle(); }
    else if (spin == m_opacitySpin)  { track = effOpacity(); factor = 100.0; }
    else if (spin == m_anchorXSpin)  { track = effAnchorX(); factor = posDisplayFactorX(); }
    else if (spin == m_anchorYSpin)  { track = effAnchorY(); factor = posDisplayFactorY(); }
    else if (spin == m_speedSpin) {
        // Speed is not a keyframe track
        double oldPct = spin->scrubStartValue();
        double oldSpd = oldPct / 100.0;
        if (oldSpd <= 0.0) oldSpd = 0.01;
        double newSpd = m_clip->speed();
        int64_t newDur = m_clip->duration();
        // Calculate old duration: oldDur = newDur * newSpd / oldSpd
        int64_t oldDur = static_cast<int64_t>(std::llround(newDur * newSpd / oldSpd));
        if (oldDur < kMinClipDuration) oldDur = kMinClipDuration;
        Clip* c = m_clip; auto* p = this;
        if (m_commandStack)
            m_commandStack->pushWithoutExecute(std::make_unique<LambdaCommand>(
                "Set Speed",
                [c, newSpd, newDur, p]() { c->setSpeed(newSpd); c->setDuration(newDur); p->populateFromClip(); emit p->propertyChanged(); },
                [c, oldSpd, oldDur, p]() { c->setSpeed(oldSpd); c->setDuration(oldDur); p->populateFromClip(); emit p->propertyChanged(); }));
        return;
    } else if (auto* ac = dynamic_cast<AudioClip*>(m_clip)) {
        if      (spin == m_panSpin)         { track = &ac->pan(); factor = 100.0; }
        else if (spin == m_audioVolumeSpin) { track = &ac->volume(); /* dB conversion handled below */ }
    }

    if (!track) return;  // crop / anchor / anti-flicker — no track yet

    // The live write has already happened. Record exact before/after states
    // for a captured gesture, including every compound sibling and handle.
    if (!editBefore.empty()) {
        std::vector<TransformTrackSnapshot> editAfter;
        editAfter.reserve(editBefore.size());
        for (const auto& state : editBefore) {
            if (!state.track) continue;
            editAfter.push_back(
                {state.track, state.track->defaultValue(), state.track->keyframes()});
        }

        auto sameKeyframe = [](const Keyframe<float>& a,
                               const Keyframe<float>& b) {
            return a.time == b.time && a.value == b.value
                && a.interp == b.interp
                && a.bezierOutX == b.bezierOutX
                && a.bezierOutY == b.bezierOutY
                && a.bezierInX == b.bezierInX
                && a.bezierInY == b.bezierInY
                && a.spatialInterp == b.spatialInterp
                && a.spatialOutX == b.spatialOutX
                && a.spatialOutY == b.spatialOutY
                && a.spatialInX == b.spatialInX
                && a.spatialInY == b.spatialInY;
        };
        auto sameStates = [&sameKeyframe](
                const std::vector<TransformTrackSnapshot>& a,
                const std::vector<TransformTrackSnapshot>& b) {
            if (a.size() != b.size()) return false;
            for (size_t i = 0; i < a.size(); ++i) {
                if (a[i].track != b[i].track
                    || a[i].defaultValue != b[i].defaultValue
                    || a[i].keyframes.size() != b[i].keyframes.size())
                    return false;
                for (size_t k = 0; k < a[i].keyframes.size(); ++k)
                    if (!sameKeyframe(a[i].keyframes[k], b[i].keyframes[k]))
                        return false;
            }
            return true;
        };
        if (sameStates(editBefore, editAfter)) return;

        auto before = std::make_shared<std::vector<TransformTrackSnapshot>>(
            std::move(editBefore));
        auto after = std::make_shared<std::vector<TransformTrackSnapshot>>(
            std::move(editAfter));
        auto* panel = this;
        auto applyStates = [panel](
                const std::vector<TransformTrackSnapshot>& states) {
            for (const auto& state : states) {
                if (!state.track) continue;
                while (state.track->keyframeCount() > 0)
                    state.track->removeKeyframe(state.track->keyframeCount() - 1);
                state.track->setDefaultValue(state.defaultValue);
                for (const auto& key : state.keyframes)
                    state.track->restoreKeyframe(key);
            }
            panel->populateFromClip();
            const int64_t t = panel->clipRelativeTick();
            for (auto* row : panel->m_propertyRows)
                if (row) row->updateForTime(t);
            if (panel->m_kfTimeline) panel->m_kfTimeline->update();
            emit panel->propertyChanged();
        };
        if (m_commandStack) {
            m_commandStack->pushWithoutExecute(std::make_unique<LambdaCommand>(
                "Transform",
                [after, applyStates]() { applyStates(*after); },
                [before, applyStates]() { applyStates(*before); }));
        }
        return;
    }

    float oldF;
    float newF;
    if (spin == m_audioVolumeSpin) {
        oldF = dbToGain(static_cast<float>(spin->scrubStartValue()));
        newF = dbToGain(static_cast<float>(spin->value()));
    } else {
        oldF = static_cast<float>(spin->scrubStartValue() / factor);
        newF = static_cast<float>(spin->value() / factor);
    }
    int64_t t = (track->keyframeCount() > 0) ? clipRelativeTick() : 0;

    auto* trk = track;
    auto* panel = this;

    // Detect if the scrub created a NEW keyframe (vs updating an existing one).
    // If so, undo must remove it rather than just restoring the old value.
    auto kfWasCreated = [](const KeyframeTrack<float>& tk, int64_t time, float oldVal) -> bool {
        if (tk.isStatic() || tk.keyframeCount() < 2) return false;
        if (!tk.hasKeyframeAt(time)) return false;
        // Evaluate without the KF at time — if result matches oldVal, the KF
        // was created by the scrub (the old value came from interpolation).
        KeyframeTrack<float> tmp(tk.defaultValue());
        for (const auto& kf : tk.keyframes()) {
            if (kf.time != time) tmp.restoreKeyframe(kf);
        }
        return std::abs(tmp.evaluate(time) - oldVal) < 0.01f;
    };

    bool createdKF = kfWasCreated(*trk, t, oldF);
    // Animated tracks (stopwatch ON) keyframe on every edit (Premiere-style),
    // so the redo path must re-add the keyframe rather than touch the default.
    bool wasAnimated = trk->keyframeCount() > 0;

    // Also handle uniform scale (mirror to scaleY)
    KeyframeTrack<float>* trkY = uniformScale ? effScaleY() : nullptr;
    int64_t tY = trkY ? ((trkY->keyframeCount() > 0) ? clipRelativeTick() : 0) : 0;
    bool createdKFY = trkY ? kfWasCreated(*trkY, tY, oldF) : false;
    bool wasAnimatedY = trkY && (trkY->keyframeCount() > 0);

    if (m_commandStack) {
        m_commandStack->pushWithoutExecute(std::make_unique<LambdaCommand>(
            "Transform",
            [trk, t, newF, wasAnimated, trkY, tY, wasAnimatedY, panel]() {
                if (wasAnimated) trk->addKeyframe(t, newF);
                else trk->setDefaultValue(newF);
                if (trkY) {
                    if (wasAnimatedY) trkY->addKeyframe(tY, newF);
                    else trkY->setDefaultValue(newF);
                }
                panel->populateFromClip();
                emit panel->propertyChanged();
            },
            [trk, t, oldF, createdKF, trkY, tY, createdKFY, panel]() {
                if (createdKF)
                    trk->removeKeyframeAtTime(t);
                else
                    trk->writeValue(t, oldF);
                if (trkY) {
                    if (createdKFY) trkY->removeKeyframeAtTime(tY);
                    else trkY->writeValue(tY, oldF);
                }
                panel->populateFromClip();
                emit panel->propertyChanged();
            }));
    }
}


void EffectControlsPanel::resetPropertyRow(PropertyRow* row)
{
    // Premiere Pro-style per-attribute reset: every value spin in the row is
    // returned to the engine-native factory default and the property's
    // keyframes are cleared, all as one undoable command.
    if (!row || !m_clip) return;

    const auto spins = row->findChildren<ScrubbySpinBox*>();
    if (spins.isEmpty()) return;

    // Full snapshot of one keyframe track so undo restores it exactly
    // (default value + every keyframe, including bezier handles).
    struct TrackSnap {
        KeyframeTrack<float>*        trk;
        float                        oldDefault;
        std::vector<Keyframe<float>> oldKfs;
        float                        factory;   // value to reset to
    };
    std::vector<TrackSnap> snaps;

    auto addTrack = [&](KeyframeTrack<float>* trk, float factory) {
        if (!trk) return;
        for (const auto& s : snaps) if (s.trk == trk) return;  // dedupe
        snaps.push_back({trk, trk->defaultValue(), trk->keyframes(), factory});
    };

    auto* audio = dynamic_cast<AudioClip*>(m_clip);
    bool  resetSpeed = false;

    for (auto* spin : spins) {
        if      (spin == m_posXSpin)                 addTrack(effPosX(),     0.0f);
        else if (spin == m_posYSpin)                 addTrack(effPosY(),     0.0f);
        else if (spin == m_scaleSpin)                addTrack(effScaleX(),   1.0f);
        else if (spin == m_scaleWSpin)               addTrack(effScaleY(),   1.0f);
        else if (spin == m_rotationSpin)             addTrack(effRotation(), 0.0f);
        else if (spin == m_shutterAngleSpin)         addTrack(effShutterAngle(), 0.0f);
        else if (spin == m_opacitySpin)              addTrack(effOpacity(),  1.0f);
        else if (spin == m_anchorXSpin)              addTrack(effAnchorX(),  0.0f);
        else if (spin == m_anchorYSpin)              addTrack(effAnchorY(),  0.0f);
        else if (spin == m_speedSpin)                resetSpeed = true;
        else if (audio && spin == m_panSpin)         addTrack(&audio->pan(),        0.0f);
        else if (audio && spin == m_audioVolumeSpin) addTrack(&audio->volume(),     1.0f);
    }

    // Scale and Scale Width share a lock — reset the companion too so the
    // displayed pair stays consistent.
    const bool uniform = m_uniformScaleCheck && m_uniformScaleCheck->isChecked();
    if (uniform) {
        bool touchedScale = false;
        for (const auto& s : snaps)
            if (s.trk == effScaleX() || s.trk == effScaleY())
                touchedScale = true;
        if (touchedScale) {
            addTrack(effScaleX(), 1.0f);
            addTrack(effScaleY(), 1.0f);
        }
    }

    if (snaps.empty() && !resetSpeed) {
        // Crop / anti-flicker rows are not keyframe-backed. Crop lives on the
        // clip, so zero this row's spins and write the result back (undoably).
        // Anti-flicker has no clip storage — just clear the spin display.
        const bool isCrop = clipHasCrop() &&
            (spins.contains(m_cropLeftSpin) || spins.contains(m_cropRightSpin) ||
             spins.contains(m_cropTopSpin)  || spins.contains(m_cropBottomSpin));

        // Capture pre-reset crop (all four axes) for undo before zeroing.
        float oL = 0, oR = 0, oT = 0, oB = 0;
        if (isCrop) {
            oL = static_cast<float>(m_cropLeftSpin->value());
            oR = static_cast<float>(m_cropRightSpin->value());
            oT = static_cast<float>(m_cropTopSpin->value());
            oB = static_cast<float>(m_cropBottomSpin->value());
        }

        for (auto* spin : spins) {
            spin->blockSignals(true);
            spin->setValue(0.0);
            spin->blockSignals(false);
        }

        if (isCrop) {
            const float nL = static_cast<float>(m_cropLeftSpin->value());
            const float nR = static_cast<float>(m_cropRightSpin->value());
            const float nT = static_cast<float>(m_cropTopSpin->value());
            const float nB = static_cast<float>(m_cropBottomSpin->value());
            Clip* clip  = m_clip;
            auto* panel = this;
            auto setCrop = [clip](float l, float r, float t, float b) {
                if (auto* vc = dynamic_cast<VideoClip*>(clip)) vc->setCrop(l, r, t, b);
                else if (auto* sc = dynamic_cast<SpineClip*>(clip)) sc->setCrop(l, r, t, b);
            };
            if (m_commandStack) {
                m_commandStack->execute(std::make_unique<LambdaCommand>(
                    QStringLiteral("Reset %1").arg(row->propertyName()).toStdString(),
                    [setCrop, nL, nR, nT, nB, panel]() {
                        setCrop(nL, nR, nT, nB);
                        panel->populateFromClip();
                        emit panel->propertyChanged();
                    },
                    [setCrop, oL, oR, oT, oB, panel]() {
                        setCrop(oL, oR, oT, oB);
                        panel->populateFromClip();
                        emit panel->propertyChanged();
                    }));
            } else {
                setCrop(nL, nR, nT, nB);
            }
        }
        emit propertyChanged();
        return;
    }

    const double oldSpeed = m_clip->speed();
    auto* panel = this;
    Clip* clip  = m_clip;
    // Capture the playhead at reset time so undo replays write at the same
    // tick the user originally clicked the reset button on, even if the
    // playhead has moved since.
    const int64_t resetTick = clipRelativeTick();

    auto apply = [clip, panel, resetTick](bool toFactory,
                                          const std::vector<TrackSnap>& src) {
        for (const auto& s : src) {
            if (toFactory) {
                // Premiere convention: if the track is animated, the reset
                // button writes (or replaces) a keyframe at the current
                // playhead with the factory value — preserving all other
                // keyframes. Only when the track has NO keyframes does
                // reset fall back to changing the default value.
                if (!s.oldKfs.empty()) {
                    s.trk->addKeyframe(resetTick, s.factory);
                } else {
                    s.trk->setDefaultValue(s.factory);
                }
            } else {
                // Undo: wipe and restore the captured snapshot exactly.
                while (s.trk->keyframeCount() > 0) s.trk->removeKeyframe(0);
                s.trk->setDefaultValue(s.oldDefault);
                for (const auto& kf : s.oldKfs) s.trk->restoreKeyframe(kf);
            }
        }
        panel->populateFromClip();
        emit panel->propertyChanged();
        if (auto* ac = dynamic_cast<AudioClip*>(clip))
            emit panel->audioLevelsChanged(
                ac->id(),
                ac->volume().evaluate(panel->clipRelativeTick()),
                ac->pan().evaluate(panel->clipRelativeTick()));
    };

    auto doReset = [apply, snaps, resetSpeed, clip]() {
        if (resetSpeed) clip->setSpeed(1.0);
        apply(true, snaps);
    };
    auto undoReset = [apply, snaps, resetSpeed, clip, oldSpeed]() {
        if (resetSpeed) clip->setSpeed(oldSpeed);
        apply(false, snaps);
    };

    if (m_commandStack) {
        m_commandStack->execute(std::make_unique<LambdaCommand>(
            QStringLiteral("Reset %1").arg(row->propertyName()).toStdString(),
            doReset, undoReset));
    } else {
        doReset();
    }
}

void EffectControlsPanel::applyTransform()
{
    // Keyboard entry path — delegates to the same per-property logic
    if (!m_clip || m_updating) return;
    applyTransformLive();
}

// ── Keyframe operations ─────────────────────────────────────────────────────



void EffectControlsPanel::beginTransformEdit()
{
    if (!m_clip || !m_commandStack || m_updating) return;
    auto* spin = qobject_cast<ScrubbySpinBox*>(sender());
    if (!spin || m_transformEditSpin == spin) return;

    m_transformEditSpin = spin;
    m_transformEditBefore.clear();
    auto snapshot = [this](KeyframeTrack<float>* track) {
        if (!track) return;
        const auto duplicate = std::find_if(
            m_transformEditBefore.begin(), m_transformEditBefore.end(),
            [track](const TransformTrackSnapshot& state) {
                return state.track == track;
            });
        if (duplicate != m_transformEditBefore.end()) return;
        m_transformEditBefore.push_back(
            {track, track->defaultValue(), track->keyframes()});
    };

    if (spin == m_posXSpin || spin == m_posYSpin) {
        snapshot(effPosX());
        snapshot(effPosY());
    } else if (spin == m_anchorXSpin || spin == m_anchorYSpin) {
        snapshot(effAnchorX());
        snapshot(effAnchorY());
    } else if (spin == m_scaleSpin || spin == m_scaleWSpin) {
        snapshot(spin == m_scaleSpin ? effScaleX() : effScaleY());
        if (m_uniformScaleCheck && m_uniformScaleCheck->isChecked()) {
            snapshot(effScaleX());
            snapshot(effScaleY());
        }
    } else if (spin == m_rotationSpin) {
        snapshot(effRotation());
    } else if (spin == m_shutterAngleSpin) {
        snapshot(effShutterAngle());
    } else if (spin == m_opacitySpin) {
        snapshot(effOpacity());
    } else if (auto* audio = dynamic_cast<AudioClip*>(m_clip)) {
        if (spin == m_panSpin) snapshot(&audio->pan());
        else if (spin == m_audioVolumeSpin) snapshot(&audio->volume());
    }
}

void EffectControlsPanel::onAddKeyframe(KeyframeTrack<float>* track, int64_t time)
{
    if (!track) return;
    std::vector<KeyframeTrack<float>*> tracks;
    auto include = [&tracks](KeyframeTrack<float>* candidate) {
        if (!candidate) return;
        if (std::find(tracks.begin(), tracks.end(), candidate) == tracks.end())
            tracks.push_back(candidate);
    };
    include(track);
    if (track == effPosX() || track == effPosY()) {
        include(effPosX());
        include(effPosY());
    } else if (track == effAnchorX() || track == effAnchorY()) {
        include(effAnchorX());
        include(effAnchorY());
    } else if (m_uniformScaleCheck && m_uniformScaleCheck->isChecked()
               && (track == effScaleX() || track == effScaleY())) {
        include(effScaleX());
        include(effScaleY());
    }
    const bool compound = m_commandStack && tracks.size() > 1;
    if (compound) m_commandStack->beginMacro("Add Keyframe");
    auto addOne = [this, time](KeyframeTrack<float>* trk) {
        float v = trk->evaluate(time);
        if (m_commandStack)
            m_commandStack->execute(
                std::make_unique<AddKeyframeCommand>(trk, time, v));
        else
            trk->addKeyframe(time, v);
    };
    for (auto* trk : tracks) addOne(trk);
    // Position, anchor, and uniform scale are exposed as one control, so their
    // component tracks must also be committed as one history action.
    if (compound) m_commandStack->endMacro();
    // Refresh button states so the diamond switches to "delete" mode
    for (auto* row : m_propertyRows)
        row->updateForTime(time);
    m_kfTimeline->update();
    emit propertyChanged();
}

void EffectControlsPanel::onDeleteKeyframe(KeyframeTrack<float>* track, int64_t time)
{
    if (!track) return;
    std::vector<KeyframeTrack<float>*> tracks;
    auto include = [&tracks](KeyframeTrack<float>* candidate) {
        if (!candidate) return;
        if (std::find(tracks.begin(), tracks.end(), candidate) == tracks.end())
            tracks.push_back(candidate);
    };
    include(track);
    if (track == effPosX() || track == effPosY()) {
        include(effPosX());
        include(effPosY());
    } else if (track == effAnchorX() || track == effAnchorY()) {
        include(effAnchorX());
        include(effAnchorY());
    } else if (m_uniformScaleCheck && m_uniformScaleCheck->isChecked()
               && (track == effScaleX() || track == effScaleY())) {
        include(effScaleX());
        include(effScaleY());
    }
    const size_t removable = static_cast<size_t>(std::count_if(
        tracks.begin(), tracks.end(), [time](const auto* trk) {
            return trk && trk->hasKeyframeAt(time);
        }));
    const bool compound = m_commandStack && removable > 1;
    if (compound) m_commandStack->beginMacro("Remove Keyframe");
    auto removeOne = [this, time](KeyframeTrack<float>* trk) {
        if (m_commandStack)
            m_commandStack->execute(
                std::make_unique<RemoveKeyframeCommand>(trk, time));
        else
            trk->removeKeyframeAtTime(time);
    };
    for (auto* trk : tracks)
        if (trk->hasKeyframeAt(time)) removeOne(trk);
    if (compound) m_commandStack->endMacro();
    // Refresh button states so the diamond switches to "add" mode
    for (auto* row : m_propertyRows)
        row->updateForTime(time);
    m_kfTimeline->update();
    emit propertyChanged();
}

void EffectControlsPanel::onGoToPrevKeyframe(KeyframeTrack<float>* track)
{
    if (!track || !m_clip) return;
    const int64_t relTick = clipRelativeTick();
    int64_t prevTime = -1;
    for (size_t i = 0; i < track->keyframeCount(); ++i) {
        int64_t t = track->keyframe(i).time;
        if (t < relTick) prevTime = t;
    }
    if (prevTime >= 0) {
        int64_t absTick = prevTime + m_clip->timelineIn();
        m_kfTimeline->setPlayheadTick(absTick);
        emit seekRequested(absTick);
    }
}

void EffectControlsPanel::onGoToNextKeyframe(KeyframeTrack<float>* track)
{
    if (!track || !m_clip) return;
    const int64_t relTick = clipRelativeTick();
    for (size_t i = 0; i < track->keyframeCount(); ++i) {
        int64_t t = track->keyframe(i).time;
        if (t > relTick) {
            int64_t absTick = t + m_clip->timelineIn();
            m_kfTimeline->setPlayheadTick(absTick);
            emit seekRequested(absTick);
            return;
        }
    }
}

// ── Effect deletion ─────────────────────────────────────────────────────

void EffectControlsPanel::deleteEffect(size_t index)
{
    if (!m_clip || (m_track && m_track->isLocked())
        || index >= m_clip->effects().effectCount()) return;
    if (m_commandStack) {
        m_commandStack->execute(
            std::make_unique<RemoveEffectCommand>(&m_clip->effects(), index));
    }
    m_selectedEffectIndex = -1;
    refresh();
    emit propertyChanged();
}

void EffectControlsPanel::deleteSelectedEffect()
{
    if (m_selectedEffectIndex >= 0)
        deleteEffect(static_cast<size_t>(m_selectedEffectIndex));
}

bool EffectControlsPanel::deleteSelectedMask()
{
    if (!m_hasSelectedMask) return false;
    // Delete is still consumed while locked so it cannot fall through and
    // delete a timeline clip or another focused object.
    if (m_track && m_track->isLocked()) return true;

    const quint64 effectId = m_selectedMaskEffectId;
    const uint64_t maskId = m_selectedMaskId;
    // Once Effect Controls has selected a mask, Delete belongs to that mask.
    // Consume even a stale id so it can never become a timeline clip delete.
    if (!deleteMask(effectId, maskId)) {
        m_hasSelectedMask = false;
        m_selectedMaskEffectId = 0;
        m_selectedMaskId = 0;
    }
    return true;
}

bool EffectControlsPanel::selectMaskById(quint64 effectId, uint64_t maskId)
{
    auto* masks = maskListFor(effectId);
    if (!masks) return false;
    const auto maskIt = std::find_if(
        masks->begin(), masks->end(),
        [maskId](const OpacityMask& mask) { return mask.maskId == maskId; });
    if (maskIt == masks->end()) return false;

    const int maskIndex = static_cast<int>(maskIt - masks->begin());
    m_hasSelectedMask = true;
    m_selectedMaskEffectId = effectId;
    m_selectedMaskId = maskId;
    m_selectedEffectIndex = -1;

    const auto& tc = Theme::colors();
    for (auto* effectHeader : m_effectHeaders) {
        effectHeader->setStyleSheet(QStringLiteral(
            "background: %1; border-bottom: 1px solid %2;")
            .arg(Theme::hex(tc.surface2), Theme::hex(tc.border)));
    }
    for (auto& section : m_sectionArrows) {
        const QVariant sectionMaskId = section.header->property("maskId");
        if (!sectionMaskId.isValid()) continue;
        const bool selected =
            sectionMaskId.toULongLong() == maskId
            && section.header->property("maskEffectId").toULongLong()
                == effectId;
        section.header->setStyleSheet(QStringLiteral(
            "background: %1; border-top: 1px solid %2; border-bottom: 1px solid %2;")
            .arg(Theme::hex(selected ? tc.accentDim : tc.surface2),
                 Theme::hex(tc.border)));
    }

    setFocus(Qt::MouseFocusReason);
    emit maskSelected(maskIndex, effectId);
    return true;
}

void EffectControlsPanel::keyPressEvent(QKeyEvent* event)
{
    // ── Ctrl+C / Ctrl+X / Ctrl+V: keyframe clipboard takes priority ────
    // When keyframes are selected in the mini-timeline, copy/paste/cut
    // operates on keyframes, not effects — matching Premiere Pro behavior.
    if (event->modifiers() & Qt::ControlModifier) {
        if (event->key() == Qt::Key_C && m_kfTimeline
            && m_kfTimeline->hasSelectedKeyframes()) {
            m_kfTimeline->copySelectedKeyframes();
            event->accept();
            return;
        }
        if (event->key() == Qt::Key_X && m_kfTimeline
            && m_kfTimeline->hasSelectedKeyframes()) {
            cutSelectedKeyframes();
            event->accept();
            return;
        }
        if (event->key() == Qt::Key_V && m_kfTimeline
            && m_kfTimeline->hasKfClipboardData()) {
            pasteKeyframes();
            event->accept();
            return;
        }
    }

    // CTRL+C: copy selected effect (only when no keyframes are selected)
    if ((event->key() == Qt::Key_C) && (event->modifiers() & Qt::ControlModifier)
        && m_selectedEffectIndex >= 0) {
        copySelectedEffect();
        event->accept();
        return;
    }
    // CTRL+V: paste copied effect (only when no kf clipboard data)
    if ((event->key() == Qt::Key_V) && (event->modifiers() & Qt::ControlModifier)
        && m_copiedEffect && m_clip) {
        pasteEffect();
        event->accept();
        return;
    }
    if ((event->key() == Qt::Key_Delete || event->key() == Qt::Key_Backspace)
        && m_hasSelectedMask) {
        deleteSelectedMask();
        event->accept();
        return;
    }
    if ((event->key() == Qt::Key_Delete || event->key() == Qt::Key_Backspace)
        && m_selectedEffectIndex >= 0) {
        deleteEffect(static_cast<size_t>(m_selectedEffectIndex));
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}

void EffectControlsPanel::copySelectedEffect()
{
    if (m_selectedEffectIndex < 0 || !m_clip) return;
    size_t idx = static_cast<size_t>(m_selectedEffectIndex);
    if (idx >= m_clip->effects().effectCount()) return;
    m_copiedEffect = m_clip->effects().effect(idx).cloneWithMasks();
    spdlog::info("EffectControlsPanel: copied effect '{}'",
                 m_copiedEffect->name());
}

void EffectControlsPanel::pasteEffect()
{
    if (!m_copiedEffect || !m_clip || !m_commandStack
        || (m_track && m_track->isLocked())) return;

    // Guard: audio effects (FillLeft/Right) only on AudioClip;
    // video/image effects only on non-AudioClip.
    const bool effectIsAudio = isAudioEffect(m_copiedEffect->effectType());
    const bool clipIsAudio   = (m_clip->isAudio());
    if (effectIsAudio != clipIsAudio) {
        spdlog::warn("EffectControlsPanel: refusing to paste {} effect '{}' onto {} clip '{}'",
                     effectIsAudio ? "audio" : "video",
                     m_copiedEffect->name(),
                     clipIsAudio ? "audio" : "video",
                     m_clip->label());
        return;
    }

    auto cloned = m_copiedEffect->cloneWithMasks();
    m_commandStack->execute(
        std::make_unique<AddEffectCommand>(
            &m_clip->effects(), std::move(cloned),
            m_clip->effects().effectCount()));
    refresh();
    emit propertyChanged();
    spdlog::info("EffectControlsPanel: pasted effect '{}'",
                 m_copiedEffect->name());
}

// ── Keyframe clipboard delegation ─────────────────────────────────────────

void EffectControlsPanel::copySelectedKeyframes()
{
    if (m_kfTimeline) m_kfTimeline->copySelectedKeyframes();
}

void EffectControlsPanel::cutSelectedKeyframes()
{
    if (m_kfTimeline && !(m_track && m_track->isLocked()))
        m_kfTimeline->cutSelectedKeyframes();
}

void EffectControlsPanel::pasteKeyframes()
{
    if (m_kfTimeline && !(m_track && m_track->isLocked()))
        m_kfTimeline->pasteKeyframes();
}

bool EffectControlsPanel::hasKfClipboardData() const noexcept
{
    return m_kfTimeline && m_kfTimeline->hasKfClipboardData();
}

bool EffectControlsPanel::hasSelectedKeyframes() const noexcept
{
    return m_kfTimeline && m_kfTimeline->hasSelectedKeyframes();
}

void EffectControlsPanel::clearKfClipboard() noexcept
{
    if (m_kfTimeline) m_kfTimeline->clearKfClipboard();
}

void EffectControlsPanel::installMaskSelectionFilters(QWidget* root)
{
    if (!root) return;
    root->installEventFilter(this);
    for (auto* child : root->findChildren<QWidget*>())
        child->installEventFilter(this);
}

bool EffectControlsPanel::eventFilter(QObject* watched, QEvent* event)
{
    auto* w = qobject_cast<QWidget*>(watched);
    auto maskRootFor = [this](QWidget* candidate) -> QWidget* {
        while (candidate && candidate != this) {
            if (candidate->property("maskIndex").isValid()
                && candidate->property("maskId").isValid()) {
                return candidate;
            }
            candidate = candidate->parentWidget();
        }
        return nullptr;
    };
    QWidget* maskRoot = w ? maskRootFor(w) : nullptr;

    // Tool buttons and non-text mask controls do not own a meaningful Delete
    // action. Route it straight to the selected mask instead of allowing it
    // to reach the workspace's clip/layer deletion shortcut. Text/spin
    // editors retain their normal character-deletion behavior.
    if (event->type() == QEvent::KeyPress && maskRoot
        && (qobject_cast<QToolButton*>(w)
            || qobject_cast<QCheckBox*>(w)
            || qobject_cast<QComboBox*>(w))) {
        auto* key = static_cast<QKeyEvent*>(event);
        if (key->key() == Qt::Key_Delete || key->key() == Qt::Key_Backspace) {
            const quint64 effectId =
                maskRoot->property("maskEffectId").toULongLong();
            const uint64_t maskId =
                maskRoot->property("maskId").toULongLong();
            deleteMask(effectId, maskId);
            key->accept();
            return true;
        }
    }

    if (event->type() == QEvent::MouseButtonPress && w) {
        const auto& tc = Theme::colors();

        // A filtered child resolves selection through its nearest tagged
        // mask row/header, then continues into its own normal click.
        if (maskRoot) {
            const quint64 clickedFxId =
                maskRoot->property("maskEffectId").toULongLong();
            const uint64_t clickedMaskId =
                maskRoot->property("maskId").toULongLong();
            selectMaskById(clickedFxId, clickedMaskId);
            return false; // preserve the child's click/toggle action
        }

        // Find which effect header was clicked
        for (size_t i = 0; i < m_effectHeaders.size(); ++i) {
            // Reset all headers to default style
            m_effectHeaders[i]->setStyleSheet(QStringLiteral(
                "background: %1; border-bottom: 1px solid %2;")
                .arg(Theme::hex(tc.surface2), Theme::hex(tc.border)));
        }
        for (size_t i = 0; i < m_effectHeaders.size(); ++i) {
            if (m_effectHeaders[i] == w) {
                m_selectedEffectIndex = static_cast<int>(i);
                m_hasSelectedMask = false;
                m_selectedMaskEffectId = 0;
                m_selectedMaskId = 0;
                for (auto& sec : m_sectionArrows) {
                    if (sec.header->property("maskId").isValid()) {
                        sec.header->setStyleSheet(QStringLiteral(
                            "background: %1; border-top: 1px solid %2; border-bottom: 1px solid %2;")
                            .arg(Theme::hex(tc.surface2),
                                 Theme::hex(tc.border)));
                    }
                }
                // Highlight selected header
                w->setStyleSheet(QStringLiteral(
                    "background: %1; border-bottom: 1px solid %2;")
                    .arg(Theme::hex(tc.accentDim), Theme::hex(tc.border)));
                setFocus(); // Ensure we get key events
                break;
            }
        }
    }
    return QWidget::eventFilter(watched, event);
}

} // namespace rt
