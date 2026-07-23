/*
 * KeyframeEditor.cpp â€” graph editor for keyframe animation curves.
 *
 * Step 21: Keyframe Editor / Graph Editor
 */

#include "panels/effects/KeyframeEditor.h"
#include "Theme.h"

#include "Constants.h"
#include "timeline/Clip.h"
#include "timeline/Keyframe.h"
#include "timeline/KeyframeTrack.h"
#include "command/CommandStack.h"
#include "command/commands/KeyframeCmds.h"

#include <QPainter>
#include <QPainterPath>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QKeyEvent>
#include <QContextMenuEvent>
#include <QMenu>
#include <QAction>
#include <QApplication>

#include <algorithm>
#include <cmath>
#include <limits>

namespace rt {

// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
//  Constants
// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•

static constexpr int    kGridLineAlpha       = 40;
static constexpr int    kGridTextAlpha       = 140;
static constexpr int    kCurvePenWidth       = 2;
static constexpr double kKeyframeRadius      = 5.0;
static constexpr double kTangentRadius       = 4.0;
static constexpr double kTangentLineLen      = 50.0; // pixels
static constexpr double kZoomFactor          = 1.15;
static constexpr int    kBezierSubdivisions  = 64;  // segments per curve
static constexpr double kMinViewSpan         = 100.0; // minimum view span (ticks or value units)
static constexpr int    kMarginLeft          = 50;
static constexpr int    kMarginRight         = 10;
static constexpr int    kMarginTop           = 10;
static constexpr int    kMarginBottom        = 25;

// Curve colors for the 6 base clip properties + extras
static const QColor kCurveColors[] = {
    QColor(255, 100, 100),  // opacity    - red
    QColor(100, 200, 255),  // positionX  - blue
    QColor(100, 255, 100),  // positionY  - green
    QColor(255, 200, 100),  // scaleX     - orange
    QColor(255, 255, 100),  // scaleY     - yellow
    QColor(200, 100, 255),  // rotation   - purple
    QColor(255, 150, 200),  // extra 1
    QColor(150, 255, 200),  // extra 2
};
static constexpr int kNumCurveColors = sizeof(kCurveColors) / sizeof(kCurveColors[0]);

// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
//  Construction
// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•

KeyframeEditor::KeyframeEditor(QWidget* parent)
    : QWidget(parent)
{
    setupUI();
    setMinimumSize(200, 120);
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
}

KeyframeEditor::~KeyframeEditor() = default;

void KeyframeEditor::setupUI()
{
    // Build context menu
    m_contextMenu       = new QMenu(this);
    m_actAddKeyframe    = m_contextMenu->addAction(QStringLiteral("Add Keyframe"));
    m_actDeleteKeyframes = m_contextMenu->addAction(QStringLiteral("Delete Selected"));
    m_contextMenu->addSeparator();
    // Premiere-style "Temporal Interpolation" submenu
    QMenu* interpSub = m_contextMenu->addMenu(QStringLiteral("Temporal Interpolation"));
    m_actLinear           = interpSub->addAction(QStringLiteral("Linear"));
    m_actBezier           = interpSub->addAction(QStringLiteral("Bezier"));
    m_actAutoBezier       = interpSub->addAction(QStringLiteral("Auto Bezier"));
    m_actContinuousBezier = interpSub->addAction(QStringLiteral("Continuous Bezier"));
    m_actHold             = interpSub->addAction(QStringLiteral("Hold"));
    interpSub->addSeparator();
    m_actEaseIn           = interpSub->addAction(QStringLiteral("Ease In"));
    m_actEaseOut          = interpSub->addAction(QStringLiteral("Ease Out"));
    m_contextMenu->addSeparator();
    m_actCopy           = m_contextMenu->addAction(QStringLiteral("Copy"));
    m_actPaste          = m_contextMenu->addAction(QStringLiteral("Paste"));
    m_contextMenu->addSeparator();
    m_actFitAll         = m_contextMenu->addAction(QStringLiteral("Fit All"));
    m_actFitSelection   = m_contextMenu->addAction(QStringLiteral("Fit Selection"));
    m_actSelectAll      = m_contextMenu->addAction(QStringLiteral("Select All"));
    m_contextMenu->addSeparator();
    m_actShowVelocity   = m_contextMenu->addAction(QStringLiteral("Show Velocity Graph"));
    m_actShowVelocity->setCheckable(true);
    m_actShowVelocity->setChecked(m_showVelocityGraph);
    connect(m_actShowVelocity, &QAction::toggled, this, &KeyframeEditor::setShowVelocityGraph);

    // Connect context-menu actions
    connect(m_actAddKeyframe, &QAction::triggered, this, [this]() {
        // Add keyframe at the first visible curve at context-menu position
        for (int i = 0; i < static_cast<int>(m_curves.size()); ++i) {
            if (m_curves[i].visible) {
                auto time = static_cast<int64_t>(m_contextMenuGraphPos.x());
                addKeyframe(i, time, static_cast<float>(m_contextMenuGraphPos.y()));
                break;
            }
        }
    });
    connect(m_actDeleteKeyframes, &QAction::triggered, this, &KeyframeEditor::deleteSelectedKeyframes);
    connect(m_actLinear,           &QAction::triggered, this, [this]() { setInterpolation(0); });
    connect(m_actBezier,           &QAction::triggered, this, [this]() { setInterpolation(1); });
    connect(m_actHold,             &QAction::triggered, this, [this]() { setInterpolation(2); });
    connect(m_actAutoBezier,       &QAction::triggered, this, [this]() { setInterpolation(3); });
    connect(m_actContinuousBezier, &QAction::triggered, this, [this]() { setInterpolation(4); });
    connect(m_actEaseIn,           &QAction::triggered, this, [this]() { setInterpolation(5); });
    connect(m_actEaseOut,          &QAction::triggered, this, [this]() { setInterpolation(6); });
    connect(m_actCopy,   &QAction::triggered, this, &KeyframeEditor::copySelectedKeyframes);
    connect(m_actPaste,  &QAction::triggered, this, [this]() {
        pasteKeyframes(static_cast<int64_t>(m_contextMenuGraphPos.x()));
    });
    connect(m_actFitAll,      &QAction::triggered, this, &KeyframeEditor::fitViewToAll);
    connect(m_actFitSelection,&QAction::triggered, this, &KeyframeEditor::fitViewToSelection);
    connect(m_actSelectAll,   &QAction::triggered, this, &KeyframeEditor::selectAll);
}

// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
//  Clip binding
// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•

void KeyframeEditor::setClip(Clip* clip)
{
    m_clip = clip;
    m_selection.clear();
    rebuildCurves();
    recomputeVelocityRange();
    emit selectionChanged();
    update();
}

void KeyframeEditor::setShowVelocityGraph(bool on) noexcept
{
    if (m_showVelocityGraph == on) return;
    m_showVelocityGraph = on;
    if (on) recomputeVelocityRange();
    if (m_actShowVelocity && m_actShowVelocity->isChecked() != on)
        m_actShowVelocity->setChecked(on);
    update();
}

void KeyframeEditor::recomputeVelocityRange()
{
    // Sweep the visible curves and find the largest |slope| we'd plot, so
    // the velocity pane uses an auto-scaled symmetric range (matches
    // Premiere's behavior when expanding the speed graph).
    double maxAbs = 0.0;
    for (const auto& ce : m_curves) {
        if (!ce.visible || !ce.track) continue;
        const auto& tk = *ce.track;
        if (tk.keyframeCount() < 2) continue;
        // Sample slope at a few hundred points across the keyframe span.
        const int64_t t0 = tk.keyframe(0).time;
        const int64_t t1 = tk.keyframe(tk.keyframeCount() - 1).time;
        if (t1 <= t0) continue;
        constexpr int kSamples = 200;
        const int64_t dt = std::max<int64_t>(1, (t1 - t0) / kSamples);
        const double  invDtSec = 1.0 / (static_cast<double>(dt) / static_cast<double>(kTicksPerSecond)); // 48000 ticks/sec assumed
        for (int s = 0; s < kSamples; ++s) {
            int64_t ta = t0 + dt * s;
            int64_t tb = ta + dt;
            double va = tk.evaluate(ta);
            double vb = tk.evaluate(tb);
            double slope = (vb - va) * invDtSec;
            if (std::abs(slope) > maxAbs) maxAbs = std::abs(slope);
        }
    }
    if (maxAbs < 1.0) maxAbs = 100.0;          // sane default when flat
    maxAbs *= 1.15;                            // 15% headroom
    m_viewVelocityMin = -maxAbs;
    m_viewVelocityMax =  maxAbs;
}

void KeyframeEditor::rebuildCurves()
{
    m_curves.clear();
    if (!m_clip) return;

    // Add the base Motion properties.
    auto addCurve = [&](const std::string& name, KeyframeTrack<float>* track, int colorIdx) {
        m_curves.push_back({name, kCurveColors[colorIdx % kNumCurveColors], track, true});
    };

    addCurve("Opacity",    &m_clip->opacity(),   0);
    addCurve("Position X", &m_clip->positionX(), 1);
    addCurve("Position Y", &m_clip->positionY(), 2);
    addCurve("Scale X",    &m_clip->scaleX(),    3);
    addCurve("Scale Y",    &m_clip->scaleY(),    4);
    addCurve("Rotation",   &m_clip->rotation(),  5);
    addCurve("Shutter Angle", &m_clip->shutterAngle(), 6);
}

// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
//  Curve accessors
// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•

int KeyframeEditor::curveCount() const noexcept
{
    return static_cast<int>(m_curves.size());
}

const CurveEntry& KeyframeEditor::curve(int i) const
{
    return m_curves[static_cast<size_t>(i)];
}

void KeyframeEditor::setCurveVisible(int index, bool visible)
{
    if (index >= 0 && index < static_cast<int>(m_curves.size())) {
        m_curves[static_cast<size_t>(index)].visible = visible;
        update();
    }
}

// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
//  Selection
// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•

void KeyframeEditor::clearSelection()
{
    m_selection.clear();
    emit selectionChanged();
    update();
}

void KeyframeEditor::selectKey(int curveIndex, int keyIndex, bool addToSelection)
{
    if (!addToSelection) m_selection.clear();
    m_selection.insert({curveIndex, keyIndex});
    emit selectionChanged();
    update();
}

void KeyframeEditor::selectAll()
{
    m_selection.clear();
    for (int ci = 0; ci < static_cast<int>(m_curves.size()); ++ci) {
        if (!m_curves[ci].visible || !m_curves[ci].track) continue;
        auto count = static_cast<int>(m_curves[ci].track->keyframeCount());
        for (int ki = 0; ki < count; ++ki)
            m_selection.insert({ci, ki});
    }
    emit selectionChanged();
    update();
}

void KeyframeEditor::boxSelect(const QRectF& graphRect, bool addToSelection)
{
    if (!addToSelection) m_selection.clear();

    for (int ci = 0; ci < static_cast<int>(m_curves.size()); ++ci) {
        if (!m_curves[ci].visible || !m_curves[ci].track) continue;
        auto& track = *m_curves[ci].track;
        for (int ki = 0; ki < static_cast<int>(track.keyframeCount()); ++ki) {
            const auto& kf = track.keyframe(ki);
            if (graphRect.contains(static_cast<double>(kf.time), static_cast<double>(kf.value)))
                m_selection.insert({ci, ki});
        }
    }
    emit selectionChanged();
    update();
}

// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
//  Keyframe operations
// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•

void KeyframeEditor::addKeyframe(int curveIndex, int64_t time, float value)
{
    if (curveIndex < 0 || curveIndex >= static_cast<int>(m_curves.size())) return;
    auto* track = m_curves[curveIndex].track;
    if (!track) return;

    if (m_commandStack) {
        m_commandStack->execute(
            std::make_unique<AddKeyframeCommand>(track, time, value, InterpMode::Linear));
    } else {
        track->addKeyframe(time, value, InterpMode::Linear);
    }
    emit keyframeChanged();
    update();
}

void KeyframeEditor::deleteSelectedKeyframes()
{
    if (m_selection.empty()) return;

    // Delete in reverse key-index order to preserve indices
    std::vector<SelectedKey> sorted(m_selection.begin(), m_selection.end());
    std::sort(sorted.begin(), sorted.end(), [](const SelectedKey& a, const SelectedKey& b) {
        if (a.curveIndex != b.curveIndex) return a.curveIndex > b.curveIndex;
        return a.keyIndex > b.keyIndex;
    });

    const bool batch = m_commandStack && sorted.size() > 1;
    if (batch) m_commandStack->beginMacro("Delete Keyframes");
    for (auto& sk : sorted) {
        if (sk.curveIndex < 0 || sk.curveIndex >= static_cast<int>(m_curves.size())) continue;
        auto* track = m_curves[sk.curveIndex].track;
        if (!track || sk.keyIndex < 0 || sk.keyIndex >= static_cast<int>(track->keyframeCount()))
            continue;
        auto t = track->keyframe(sk.keyIndex).time;
        if (m_commandStack)
            m_commandStack->execute(std::make_unique<RemoveKeyframeCommand>(track, t));
        else
            track->removeKeyframeAtTime(t);
    }
    if (batch) m_commandStack->endMacro();

    m_selection.clear();
    emit keyframeChanged();
    emit selectionChanged();
    update();
}

void KeyframeEditor::setInterpolation(int interpMode)
{
    if (m_selection.empty()) return;

    auto mode = static_cast<InterpMode>(std::clamp(interpMode, 0, 6));

    const bool batch = m_commandStack && m_selection.size() > 1;
    if (batch) m_commandStack->beginMacro("Set Keyframe Interpolation");
    for (auto& sk : m_selection) {
        if (sk.curveIndex < 0 || sk.curveIndex >= static_cast<int>(m_curves.size())) continue;
        auto* track = m_curves[sk.curveIndex].track;
        if (!track || sk.keyIndex < 0 || sk.keyIndex >= static_cast<int>(track->keyframeCount()))
            continue;
        const auto t = track->keyframe(sk.keyIndex).time;
        if (m_commandStack)
            m_commandStack->execute(std::make_unique<SetKeyframeInterpCommand>(track, t, mode));
        else
            track->keyframe(sk.keyIndex).interp = mode;
    }
    if (batch) m_commandStack->endMacro();
    emit keyframeChanged();
    update();
}

// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
//  Clipboard
// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•

void KeyframeEditor::copySelectedKeyframes()
{
    m_clipboard.clear();
    if (m_selection.empty()) return;

    // Find earliest time for relative offsets
    int64_t earliest = std::numeric_limits<int64_t>::max();
    for (auto& sk : m_selection) {
        if (sk.curveIndex < 0 || sk.curveIndex >= static_cast<int>(m_curves.size())) continue;
        auto* track = m_curves[sk.curveIndex].track;
        if (!track || sk.keyIndex < 0 || sk.keyIndex >= static_cast<int>(track->keyframeCount()))
            continue;
        earliest = std::min(earliest, track->keyframe(sk.keyIndex).time);
    }

    for (auto& sk : m_selection) {
        if (sk.curveIndex < 0 || sk.curveIndex >= static_cast<int>(m_curves.size())) continue;
        auto* track = m_curves[sk.curveIndex].track;
        if (!track || sk.keyIndex < 0 || sk.keyIndex >= static_cast<int>(track->keyframeCount()))
            continue;
        const auto& kf = track->keyframe(sk.keyIndex);
        Keyframe<float> copied = kf;
        copied.time -= earliest;
        m_clipboard.push_back({sk.curveIndex, copied});
    }
}

void KeyframeEditor::pasteKeyframes(int64_t time)
{
    if (m_clipboard.empty()) return;

    m_selection.clear();
    const bool batch = m_commandStack && m_clipboard.size() > 1;
    if (batch) m_commandStack->beginMacro("Paste Keyframes");
    for (auto& ce : m_clipboard) {
        if (ce.curveIndex < 0 || ce.curveIndex >= static_cast<int>(m_curves.size())) continue;
        auto* track = m_curves[ce.curveIndex].track;
        if (!track) continue;

        Keyframe<float> pasted = ce.keyframe;
        pasted.time += time;

        if (m_commandStack)
            m_commandStack->execute(
                std::make_unique<AddKeyframeCommand>(track, pasted));
        else
            track->restoreKeyframe(pasted);

        // Select the pasted keyframe
        for (int ki = 0; ki < static_cast<int>(track->keyframeCount()); ++ki) {
            if (track->keyframe(ki).time == pasted.time) {
                m_selection.insert({ce.curveIndex, ki});
                break;
            }
        }
    }
    if (batch) m_commandStack->endMacro();
    emit keyframeChanged();
    emit selectionChanged();
    update();
}

// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
//  View management
// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•

void KeyframeEditor::setViewRange(double timeMin, double timeMax,
                                   double valueMin, double valueMax)
{
    m_viewTimeMin  = timeMin;
    m_viewTimeMax  = timeMax;
    m_viewValueMin = valueMin;
    m_viewValueMax = valueMax;
    emit viewChanged();
    update();
}

void KeyframeEditor::fitViewToAll()
{
    if (m_curves.empty()) return;

    double tMin =  std::numeric_limits<double>::max();
    double tMax = -std::numeric_limits<double>::max();
    double vMin =  std::numeric_limits<double>::max();
    double vMax = -std::numeric_limits<double>::max();
    bool any = false;

    for (auto& ce : m_curves) {
        if (!ce.visible || !ce.track) continue;
        for (size_t i = 0; i < ce.track->keyframeCount(); ++i) {
            const auto& kf = ce.track->keyframe(i);
            tMin = std::min(tMin, static_cast<double>(kf.time));
            tMax = std::max(tMax, static_cast<double>(kf.time));
            vMin = std::min(vMin, static_cast<double>(kf.value));
            vMax = std::max(vMax, static_cast<double>(kf.value));
            any = true;
        }
    }

    if (!any) return;

    // Add padding (10% of range, minimum 1 unit)
    double tPad = std::max((tMax - tMin) * 0.1, static_cast<double>(kTicksPerSecond));
    double vPad = std::max((vMax - vMin) * 0.1, 0.1);
    setViewRange(tMin - tPad, tMax + tPad, vMin - vPad, vMax + vPad);
}

void KeyframeEditor::fitViewToSelection()
{
    if (m_selection.empty()) {
        fitViewToAll();
        return;
    }

    double tMin =  std::numeric_limits<double>::max();
    double tMax = -std::numeric_limits<double>::max();
    double vMin =  std::numeric_limits<double>::max();
    double vMax = -std::numeric_limits<double>::max();

    for (auto& sk : m_selection) {
        if (sk.curveIndex < 0 || sk.curveIndex >= static_cast<int>(m_curves.size())) continue;
        auto* track = m_curves[sk.curveIndex].track;
        if (!track || sk.keyIndex < 0 || sk.keyIndex >= static_cast<int>(track->keyframeCount()))
            continue;
        const auto& kf = track->keyframe(sk.keyIndex);
        tMin = std::min(tMin, static_cast<double>(kf.time));
        tMax = std::max(tMax, static_cast<double>(kf.time));
        vMin = std::min(vMin, static_cast<double>(kf.value));
        vMax = std::max(vMax, static_cast<double>(kf.value));
    }

    double tPad = std::max((tMax - tMin) * 0.1, static_cast<double>(kTicksPerSecond));
    double vPad = std::max((vMax - vMin) * 0.1, 0.1);
    setViewRange(tMin - tPad, tMax + tPad, vMin - vPad, vMax + vPad);
}

// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
//  Coordinate transforms
// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•

// When the velocity graph is shown, the value pane occupies the top half of
// the plot area; the velocity pane occupies the bottom half (with a 4px gap).
static constexpr double kVelocityGap = 4.0;

static double valuePaneHeight(double totalPlotH, bool showVelocity) noexcept
{
    if (!showVelocity) return totalPlotH;
    return std::max(20.0, (totalPlotH - kVelocityGap) * 0.5);
}

QPointF KeyframeEditor::graphToPixel(double time, double value) const
{
    double plotW = width()  - kMarginLeft - kMarginRight;
    double totalH = height() - kMarginTop  - kMarginBottom;
    double plotH = valuePaneHeight(totalH, m_showVelocityGraph);
    if (plotW <= 0) plotW = 1;
    if (plotH <= 0) plotH = 1;

    double timeRange  = m_viewTimeMax  - m_viewTimeMin;
    double valueRange = m_viewValueMax - m_viewValueMin;
    if (timeRange  == 0.0) timeRange  = 1.0;
    if (valueRange == 0.0) valueRange = 1.0;

    double x = kMarginLeft + (time  - m_viewTimeMin)  / timeRange  * plotW;
    double y = kMarginTop  + (1.0 - (value - m_viewValueMin) / valueRange) * plotH;
    return {x, y};
}

QPointF KeyframeEditor::pixelToGraph(double px, double py) const
{
    double plotW = width()  - kMarginLeft - kMarginRight;
    double totalH = height() - kMarginTop  - kMarginBottom;
    double plotH = valuePaneHeight(totalH, m_showVelocityGraph);
    if (plotW <= 0) plotW = 1;
    if (plotH <= 0) plotH = 1;

    double timeRange  = m_viewTimeMax  - m_viewTimeMin;
    double valueRange = m_viewValueMax - m_viewValueMin;

    double time  = m_viewTimeMin  + (px - kMarginLeft) / plotW * timeRange;
    double value = m_viewValueMin + (1.0 - (py - kMarginTop) / plotH) * valueRange;
    return {time, value};
}

QPointF KeyframeEditor::graphToPixelVelocity(double time, double velocity) const
{
    double plotW  = width()  - kMarginLeft - kMarginRight;
    double totalH = height() - kMarginTop  - kMarginBottom;
    if (plotW <= 0) plotW = 1;
    double valH = valuePaneHeight(totalH, true);
    double velTop = kMarginTop + valH + kVelocityGap;
    double velH = std::max(10.0, totalH - valH - kVelocityGap);

    double timeRange = m_viewTimeMax - m_viewTimeMin;
    if (timeRange == 0.0) timeRange = 1.0;
    double velRange = m_viewVelocityMax - m_viewVelocityMin;
    if (velRange == 0.0) velRange = 1.0;

    double x = kMarginLeft + (time     - m_viewTimeMin)    / timeRange * plotW;
    double y = velTop + (1.0 - (velocity - m_viewVelocityMin) / velRange) * velH;
    return {x, y};
}

QPointF KeyframeEditor::pixelToGraphVelocity(double px, double py) const
{
    double plotW  = width()  - kMarginLeft - kMarginRight;
    double totalH = height() - kMarginTop  - kMarginBottom;
    if (plotW <= 0) plotW = 1;
    double valH = valuePaneHeight(totalH, true);
    double velTop = kMarginTop + valH + kVelocityGap;
    double velH = std::max(10.0, totalH - valH - kVelocityGap);

    double timeRange = m_viewTimeMax - m_viewTimeMin;
    double velRange  = m_viewVelocityMax - m_viewVelocityMin;

    double time     = m_viewTimeMin    + (px - kMarginLeft) / plotW * timeRange;
    double velocity = m_viewVelocityMin + (1.0 - (py - velTop) / velH) * velRange;
    return {time, velocity};
}

// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
//  Hit testing
// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•

KeyframeEditor::HitResult KeyframeEditor::hitTest(const QPointF& pos) const
{
    HitResult best;
    double bestDist = kHitRadius;

    // Velocity-pane influence handles take priority when the cursor sits
    // inside the velocity pane region.
    if (m_showVelocityGraph) {
        const double valH  = (height() - kMarginTop - kMarginBottom) * 0.5 - 2.0;
        const double velTop = kMarginTop + valH + kVelocityGap;
        if (pos.y() >= velTop) {
            for (int ci = 0; ci < static_cast<int>(m_curves.size()); ++ci) {
                if (!m_curves[ci].visible || !m_curves[ci].track) continue;
                const auto& tk = *m_curves[ci].track;
                for (int ki = 0; ki < static_cast<int>(tk.keyframeCount()); ++ki) {
                    const auto& kf = tk.keyframe(static_cast<size_t>(ki));
                    // Out-handle
                    if (ki + 1 < static_cast<int>(tk.keyframeCount())) {
                        const auto& kfNext = tk.keyframe(static_cast<size_t>(ki + 1));
                        double dtSec = (kfNext.time - kf.time) / static_cast<double>(kTicksPerSecond);
                        double dv    = kfNext.value - kf.value;
                        if (dtSec > 0.0) {
                            double bx = std::max<double>(kf.bezierOutX, 0.001);
                            double slope = 3.0 * kf.bezierOutY * (dv / dtSec) / bx;
                            double tHandle = kf.time + bx * (kfNext.time - kf.time);
                            QPointF hPx = graphToPixelVelocity(tHandle, slope);
                            double d = QLineF(pos, hPx).length();
                            if (d < kTangentHitRadius && d < bestDist) {
                                bestDist = d;
                                best = {ci, ki, false, false, false, true};
                            }
                        }
                    }
                    // In-handle
                    if (ki > 0) {
                        const auto& kfPrev = tk.keyframe(static_cast<size_t>(ki - 1));
                        double dtSec = (kf.time - kfPrev.time) / static_cast<double>(kTicksPerSecond);
                        double dv    = kf.value - kfPrev.value;
                        if (dtSec > 0.0) {
                            double bx = std::min<double>(std::max<double>(kf.bezierInX, 0.001), 0.999);
                            double slope = 3.0 * (1.0 - kf.bezierInY) * (dv / dtSec) / (1.0 - bx);
                            double tHandle = kfPrev.time + bx * (kf.time - kfPrev.time);
                            QPointF hPx = graphToPixelVelocity(tHandle, slope);
                            double d = QLineF(pos, hPx).length();
                            if (d < kTangentHitRadius && d < bestDist) {
                                bestDist = d;
                                best = {ci, ki, false, false, true, false};
                            }
                        }
                    }
                }
            }
            if (best.curveIndex >= 0) return best;
        }
    }

    for (int ci = 0; ci < static_cast<int>(m_curves.size()); ++ci) {
        if (!m_curves[ci].visible || !m_curves[ci].track) continue;
        auto& track = *m_curves[ci].track;

        for (int ki = 0; ki < static_cast<int>(track.keyframeCount()); ++ki) {
            const auto& kf = track.keyframe(ki);
            QPointF kfPx = graphToPixel(kf.time, kf.value);
            double dist = QLineF(pos, kfPx).length();
            if (dist < bestDist) {
                bestDist = dist;
                best = {ci, ki, false, false};
            }

            // Check tangent handles (selected + has manually-edited handles)
            SelectedKey sk{ci, ki};
            const bool hasManualHandles =
                (kf.interp == InterpMode::Bezier || kf.interp == InterpMode::ContinuousBezier);
            if (m_selection.count(sk) && hasManualHandles) {
                // Out tangent
                if (ki + 1 < static_cast<int>(track.keyframeCount())) {
                    const auto& kfNext = track.keyframe(ki + 1);
                    double dt = static_cast<double>(kfNext.time - kf.time);
                    double dv = static_cast<double>(kfNext.value - kf.value);
                    QPointF tanPx = graphToPixel(
                        kf.time + kf.bezierOutX * dt,
                        kf.value + kf.bezierOutY * dv);
                    double td = QLineF(pos, tanPx).length();
                    if (td < kTangentHitRadius && td < bestDist) {
                        bestDist = td;
                        best = {ci, ki, false, true};
                    }
                }
            }

            // In tangent of THIS keyframe (controls the segment ending at it).
            // bezierIn{X,Y} are normalized to the incoming segment, so the
            // handle's absolute position lives between kfPrev and kf.
            if (ki > 0 && m_selection.count(sk) && hasManualHandles) {
                const auto& kfPrev = track.keyframe(ki - 1);
                double dt = static_cast<double>(kf.time - kfPrev.time);
                double dv = static_cast<double>(kf.value - kfPrev.value);
                QPointF tanPx = graphToPixel(
                    kfPrev.time + kf.bezierInX * dt,
                    kfPrev.value + kf.bezierInY * dv);
                double td = QLineF(pos, tanPx).length();
                if (td < kTangentHitRadius && td < bestDist) {
                    bestDist = td;
                    best = {ci, ki, true, false};
                }
            }
        }
    }
    return best;
}

// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•

} // namespace rt
