/*
 * test_monitor_overlay_drags.cpp — characterization of the Program Monitor
 * overlay's drags, driven with real mouse events.
 *
 * The overlay applies a drag live through its *Changed signals and reports
 * the whole drag once through a *Finished signal; the workspace records that
 * as one undo step between editDragStarted and editDragEnded. Each test pins
 * the exact signal sequence a drag produces (what, in which order, with which
 * values), so a refactor of the overlay's drag code can be checked against it.
 *
 * The rig: a 640x360 viewport showing a 1920x1080 sequence, so one widget
 * pixel is 3 reference pixels. The clip box is a 1920x1080 source at 50%:
 * corners (160,90) (480,90) (480,270) (160,270), centre (320,180).
 */

#include <gtest/gtest.h>

#include "command/CommandStack.h"
#include "edit/EditSession.h"
#include "timeline/KeyframeTrack.h"
#include "timeline/OpacityMask.h"
#include "timeline/Timeline.h"
#include "viewport/TransformOverlayWidget.h"
#include "viewport/VulkanViewport.h"

#include <QApplication>
#include <QMouseEvent>
#include <QString>

#include <cmath>
#include <memory>
#include <string>
#include <vector>

using namespace rt;

namespace {

void ensureApp()
{
    static int argc = 1;
    static const char* argv[] = {"test_monitor_overlay_drags"};
    if (!QApplication::instance())
        new QApplication(argc, const_cast<char**>(argv));
}

std::string num(double v)
{
    QString s = QString::asprintf("%.2f", v);
    if (s == QStringLiteral("-0.00")) s = QStringLiteral("0.00");
    return s.toStdString();
}

class DrivenOverlay final : public TransformOverlayWidget
{
public:
    using TransformOverlayWidget::TransformOverlayWidget;

    void press(QPointF pos, Qt::KeyboardModifiers mods = Qt::NoModifier,
               Qt::MouseButton button = Qt::LeftButton)
    {
        QMouseEvent event(QEvent::MouseButtonPress, pos, pos, pos, button,
                          button, mods);
        mousePressEvent(&event);
    }
    void move(QPointF pos, Qt::KeyboardModifiers mods = Qt::NoModifier,
              Qt::MouseButtons buttons = Qt::LeftButton)
    {
        QMouseEvent event(QEvent::MouseMove, pos, pos, pos, Qt::NoButton,
                          buttons, mods);
        mouseMoveEvent(&event);
    }
    void release(QPointF pos, Qt::KeyboardModifiers mods = Qt::NoModifier,
                 Qt::MouseButton button = Qt::LeftButton)
    {
        QMouseEvent event(QEvent::MouseButtonRelease, pos, pos, pos, button,
                          Qt::NoButton, mods);
        mouseReleaseEvent(&event);
    }
    /// Windows dropped the mouse grab mid-drag (no release arrives).
    void loseCapture()
    {
        QEvent ungrab(QEvent::UngrabMouse);
        eventFilter(this, &ungrab);
    }
};

struct OverlayRig
{
    VulkanViewport viewport;
    std::unique_ptr<DrivenOverlay> overlay;
    std::vector<std::string> log;
    TransformOverlayInfo box;

    OverlayRig()
    {
        viewport.resize(640, 360);
        viewport.show();
        QApplication::processEvents();
        overlay = std::make_unique<DrivenOverlay>(&viewport);
        overlay->setGeometry(viewport.rect());
        overlay->show();
        QApplication::processEvents();
        overlay->setSequenceResolution(1920, 1080);

        box.visible = true;
        box.srcW = 1920;
        box.srcH = 1080;
        box.scaleX = 0.5f;
        box.scaleY = 0.5f;
        overlay->setTransformOverlay(box);
        record();
    }

    void record()
    {
        auto* o = overlay.get();
        using W = TransformOverlayWidget;
        QObject::connect(o, &W::editDragStarted, [this] { log.push_back("started"); });
        QObject::connect(o, &W::editDragEnded, [this] { log.push_back("ended"); });
        QObject::connect(o, &W::transformPositionChanged, [this](float x, float y) {
            log.push_back("pos " + num(x) + " " + num(y));
        });
        QObject::connect(o, &W::transformScaleChanged, [this](float x, float y) {
            log.push_back("scale " + num(x) + " " + num(y));
        });
        QObject::connect(o, &W::transformRotationChanged, [this](float r) {
            log.push_back("rot " + num(r));
        });
        QObject::connect(o, &W::transformAnchorChanged, [this](float x, float y) {
            log.push_back("anchor " + num(x) + " " + num(y));
        });
        QObject::connect(o, &W::transformAnchorDragFinished,
                         [this](float ox, float oy, float nx, float ny) {
            log.push_back("anchor done " + num(ox) + " " + num(oy) + " -> " +
                          num(nx) + " " + num(ny));
        });
        QObject::connect(o, &W::transformDragFinished,
                         [this](float opx, float opy, float osx, float osy, float orot,
                                float npx, float npy, float nsx, float nsy, float nrot) {
            log.push_back("transform done " + num(opx) + " " + num(opy) + " " +
                          num(osx) + " " + num(osy) + " " + num(orot) + " -> " +
                          num(npx) + " " + num(npy) + " " + num(nsx) + " " +
                          num(nsy) + " " + num(nrot));
        });
        QObject::connect(o, &W::cropChanged, [this](float l, float r, float t, float b) {
            log.push_back("crop " + num(l) + " " + num(r) + " " + num(t) + " " + num(b));
        });
        QObject::connect(o, &W::cropDragFinished,
                         [this](float ol, float orr, float ot, float ob,
                                float nl, float nr, float nt, float nb) {
            log.push_back("crop done " + num(ol) + " " + num(orr) + " " + num(ot) +
                          " " + num(ob) + " -> " + num(nl) + " " + num(nr) + " " +
                          num(nt) + " " + num(nb));
        });
        QObject::connect(o, &W::maskLiveUpdate, [this] { log.push_back("mask live"); });
        QObject::connect(o, &W::maskDragFinished,
                         [this](int index, OpacityMask, OpacityMask) {
            log.push_back("mask done " + std::to_string(index));
        });
        QObject::connect(o, &W::maskCreated, [this](int index, OpacityMask) {
            log.push_back("mask created " + std::to_string(index));
        });
        QObject::connect(o, &W::motionPathLiveUpdate,
                         [this] { log.push_back("motion live"); });
        QObject::connect(o, &W::emptyAreaClicked,
                         [this](float x, float y, Qt::KeyboardModifiers) {
            log.push_back("empty " + num(x) + " " + num(y));
        });
    }

    using Log = std::vector<std::string>;
};

// The overlay's frame-to-widget scale: 1920 reference px over 640 widget px.
constexpr double kRefPerPx = 3.0;
const QPointF kCentre(320.0, 180.0);

double angleDeg(QPointF from, QPointF to)
{
    return std::atan2(to.y() - from.y(), to.x() - from.x()) * 180.0 /
           3.14159265358979;
}

} // namespace

TEST(MonitorOverlayDrags, BodyDragMovesLiveAndFinishesOnce)
{
    ensureApp();
    OverlayRig rig;
    rig.overlay->press({320, 180});
    rig.overlay->move({352, 180});
    rig.overlay->move({384, 196});
    rig.overlay->release({384, 196});

    EXPECT_EQ(rig.log, (OverlayRig::Log{
        "started",
        "pos " + num(32 * kRefPerPx) + " 0.00",
        "pos " + num(64 * kRefPerPx) + " " + num(16 * kRefPerPx),
        "transform done 0.00 0.00 0.50 0.50 0.00 -> " + num(64 * kRefPerPx) + " " +
            num(16 * kRefPerPx) + " 0.50 0.50 0.00",
        "ended"}));
    EXPECT_FLOAT_EQ(rig.overlay->transformOverlay().posX, 192.0f);
    EXPECT_FLOAT_EQ(rig.overlay->transformOverlay().posY, 48.0f);
}

TEST(MonitorOverlayDrags, ClickOnTheBodyStillReportsOneUnchangedDrag)
{
    // The workspace drops a drag that changed nothing; the overlay still
    // brackets and reports it.
    ensureApp();
    OverlayRig rig;
    rig.overlay->press({300, 170});
    rig.overlay->release({300, 170});

    EXPECT_EQ(rig.log, (OverlayRig::Log{
        "started",
        "transform done 0.00 0.00 0.50 0.50 0.00 -> 0.00 0.00 0.50 0.50 0.00",
        "ended"}));
}

TEST(MonitorOverlayDrags, CornerDragScalesUniformlyAboutTheCentre)
{
    ensureApp();
    OverlayRig rig;
    const QPointF start(156, 86), end(116, 66);   // just outside the top-left corner
    rig.overlay->press(start);
    rig.overlay->move(end);
    rig.overlay->release(end);

    const double ratio = std::hypot(end.x() - kCentre.x(), end.y() - kCentre.y()) /
                         std::hypot(start.x() - kCentre.x(), start.y() - kCentre.y());
    const std::string s = num(0.5 * ratio);
    EXPECT_EQ(rig.log, (OverlayRig::Log{
        "started",
        "scale " + s + " " + s,
        "transform done 0.00 0.00 0.50 0.50 0.00 -> 0.00 0.00 " + s + " " + s + " 0.00",
        "ended"}));
}

TEST(MonitorOverlayDrags, ShiftCornerDragScalesEachAxisFreely)
{
    ensureApp();
    OverlayRig rig;
    const QPointF start(156, 86), end(116, 76);
    rig.overlay->press(start, Qt::ShiftModifier);
    rig.overlay->move(end, Qt::ShiftModifier);
    rig.overlay->release(end, Qt::ShiftModifier);

    const std::string sx = num(0.5 * (kCentre.x() - end.x()) / (kCentre.x() - start.x()));
    const std::string sy = num(0.5 * (kCentre.y() - end.y()) / (kCentre.y() - start.y()));
    EXPECT_EQ(rig.log, (OverlayRig::Log{
        "started",
        "scale " + sx + " " + sy,
        "transform done 0.00 0.00 0.50 0.50 0.00 -> 0.00 0.00 " + sx + " " + sy + " 0.00",
        "ended"}));
}

TEST(MonitorOverlayDrags, DragOutsideACornerRotates)
{
    ensureApp();
    OverlayRig rig;
    const QPointF start(140, 70);   // 28 px out from the top-left corner
    const double a = (angleDeg(kCentre, start) + 10.0) * 3.14159265358979 / 180.0;
    const double r = std::hypot(start.x() - kCentre.x(), start.y() - kCentre.y());
    const QPointF end(kCentre.x() + r * std::cos(a), kCentre.y() + r * std::sin(a));
    rig.overlay->press(start);
    rig.overlay->move(end);
    rig.overlay->release(end);

    EXPECT_EQ(rig.log, (OverlayRig::Log{
        "started",
        "rot 10.00",
        "transform done 0.00 0.00 0.50 0.50 0.00 -> 0.00 0.00 0.50 0.50 10.00",
        "ended"}));
}

TEST(MonitorOverlayDrags, CtrlDragOnTheAnchorMovesOnlyTheAnchor)
{
    // A graphic layer (content-rect box in canvas pixels): the anchor is
    // grabbable without a presented frame. Its crosshair is at the centre.
    ensureApp();
    OverlayRig rig;
    rig.box.scaleX = rig.box.scaleY = 1.0f;
    rig.box.useContentRect = true;
    rig.box.contentL = 480.0f;
    rig.box.contentT = 270.0f;
    rig.box.contentR = 1440.0f;
    rig.box.contentB = 810.0f;
    rig.overlay->setTransformOverlay(rig.box);
    rig.overlay->press(kCentre, Qt::ControlModifier);
    rig.overlay->move({352, 196}, Qt::ControlModifier);
    rig.overlay->release({352, 196}, Qt::ControlModifier);

    EXPECT_EQ(rig.log, (OverlayRig::Log{
        "started",
        "anchor " + num(32 * kRefPerPx) + " " + num(16 * kRefPerPx),
        "anchor done 0.00 0.00 -> " + num(32 * kRefPerPx) + " " + num(16 * kRefPerPx),
        "ended"}));
    EXPECT_FLOAT_EQ(rig.overlay->transformOverlay().posX, 0.0f);
}

TEST(MonitorOverlayDrags, CtrlDragOnACropHandleCrops)
{
    ensureApp();
    OverlayRig rig;
    rig.box.cropEnabled = true;
    rig.overlay->setTransformOverlay(rig.box);
    // The left handle sits mid-way down the left edge; 32 px is 10% of the
    // 320 px box.
    rig.overlay->press({160, 180}, Qt::ControlModifier);
    rig.overlay->move({192, 180}, Qt::ControlModifier);
    rig.overlay->release({192, 180}, Qt::ControlModifier);

    EXPECT_EQ(rig.log, (OverlayRig::Log{
        "started",
        "crop 10.00 0.00 0.00 0.00",
        "crop done 0.00 0.00 0.00 0.00 -> 10.00 0.00 0.00 0.00",
        "ended"}));
}

TEST(MonitorOverlayDrags, CropStopsShortOfTheOppositeEdge)
{
    ensureApp();
    OverlayRig rig;
    rig.box.cropEnabled = true;
    rig.box.cropR = 40.0f;
    rig.overlay->setTransformOverlay(rig.box);
    rig.overlay->press({160, 180}, Qt::ControlModifier);
    rig.overlay->move({460, 180}, Qt::ControlModifier);
    rig.overlay->release({460, 180}, Qt::ControlModifier);

    // L + R stays at most 95%.
    EXPECT_EQ(rig.log, (OverlayRig::Log{
        "started",
        "crop 55.00 40.00 0.00 0.00",
        "crop done 0.00 40.00 0.00 0.00 -> 55.00 40.00 0.00 0.00",
        "ended"}));
}

namespace {

struct MaskRig : OverlayRig
{
    std::vector<OpacityMask> masks;

    MaskRig()
    {
        // Masks are edited in the clip's frame; give the mask owner the
        // whole frame so mask space maps 1:1 onto the 640x360 widget.
        TransformOverlayInfo frame;
        frame.visible = true;
        frame.srcW = 1920;
        frame.srcH = 1080;
        box = frame;
        overlay->setTransformOverlay(frame);
        overlay->setMaskOwnerOverlay(frame, true);

        OpacityMask mask;
        mask.shape = MaskShape::FreeDrawBezier;
        mask.base.vertices = {
            {0.3f, 0.3f, 0.0f, 0.0f, 0.0f, 0.0f},
            {0.7f, 0.3f, 0.0f, 0.0f, 0.0f, 0.0f},
            {0.5f, 0.7f, 0.0f, 0.0f, 0.0f, 0.0f},
        };
        masks.push_back(mask);
        overlay->setMasks(&masks);
        overlay->setActiveMaskIndex(0);
        overlay->setEditTool(0);
    }
};

} // namespace

TEST(MonitorOverlayDrags, MaskVertexDragMovesOnlyThatVertex)
{
    ensureApp();
    MaskRig rig;
    // Vertex 0 at (0.3, 0.3) of the frame: widget (192, 108).
    rig.overlay->press({192, 108});
    rig.overlay->move({224, 126});
    rig.overlay->release({224, 126});

    EXPECT_EQ(rig.log, (OverlayRig::Log{"started", "mask live", "mask done 0", "ended"}));
    const auto& v = rig.masks[0].base.vertices;
    EXPECT_NEAR(v[0].x, 0.35f, 0.001f);
    EXPECT_NEAR(v[0].y, 0.35f, 0.001f);
    EXPECT_NEAR(v[1].x, 0.7f, 0.001f);
    EXPECT_NEAR(v[2].y, 0.7f, 0.001f);
}

TEST(MonitorOverlayDrags, MaskDragReportsTheMaskAsItWasAtThePress)
{
    ensureApp();
    MaskRig rig;
    OpacityMask before, after;
    QObject::connect(rig.overlay.get(), &TransformOverlayWidget::maskDragFinished,
                     [&](int, OpacityMask o, OpacityMask n) { before = o; after = n; });
    rig.overlay->press({320, 180});   // inside: moves the whole mask
    rig.overlay->move({336, 180});
    rig.overlay->move({352, 180});
    rig.overlay->release({352, 180});

    EXPECT_EQ(rig.log, (OverlayRig::Log{
        "started", "mask live", "mask live", "mask done 0", "ended"}));
    ASSERT_EQ(before.base.vertices.size(), 3u);
    EXPECT_NEAR(before.base.vertices[0].x, 0.3f, 0.001f);
    EXPECT_NEAR(after.base.vertices[0].x, 0.35f, 0.001f);
    EXPECT_NEAR(rig.masks[0].base.vertices[0].x, 0.35f, 0.001f);
}

TEST(MonitorOverlayDrags, ShapeMaskToolDrawsOneMaskWithoutADragBracket)
{
    // Drawing a new mask is reported by maskCreated, which the workspace
    // records on its own (then one live refresh).
    ensureApp();
    MaskRig rig;
    rig.masks.clear();
    rig.overlay->setActiveMaskIndex(-1);
    rig.overlay->setEditTool(11);
    rig.overlay->press({100, 100});
    rig.overlay->move({200, 160});
    rig.overlay->release({200, 160});

    EXPECT_EQ(rig.log, (OverlayRig::Log{"mask created 0", "mask live"}));
}

namespace {

struct MotionRig : OverlayRig
{
    KeyframeTrack<float> x{0.0f};
    KeyframeTrack<float> y{0.0f};
    Timeline timeline;
    CommandStack stack;

    MotionRig()
    {
        box.visible = false;
        overlay->setTransformOverlay(box);
        // Two Bezier waypoints; the first one's out handle is 192 ref px
        // (64 widget px) right of it, at widget (384, 180).
        x.addKeyframe(0, 0.0f);
        x.addKeyframe(48000, 300.0f);
        y.addKeyframe(0, 0.0f);
        y.addKeyframe(48000, 0.0f);
        for (auto* track : {&x, &y})
            for (size_t i = 0; i < track->keyframeCount(); ++i)
                track->keyframe(i).spatialInterp = InterpMode::Bezier;
        x.keyframe(0).spatialOutX = 192.0f;
        overlay->setMotionPathTracks(&x, &y, &stack);
    }
};

} // namespace

TEST(MonitorOverlayDrags, MotionHandleDragRecordsItsOwnUndoStep)
{
    ensureApp();
    MotionRig rig;
    rig.overlay->press({384, 180});
    rig.overlay->move({384, 212});
    rig.overlay->release({384, 212});

    EXPECT_EQ(rig.log, (OverlayRig::Log{"started", "motion live", "ended"}));
    EXPECT_FLOAT_EQ(rig.x.keyframe(0).spatialOutX, 192.0f);
    EXPECT_FLOAT_EQ(rig.y.keyframe(0).spatialOutY, 96.0f);

    // No edit session: the overlay records the step itself.
    ASSERT_EQ(rig.stack.undoCount(), 1u);
    rig.stack.undo();
    EXPECT_FLOAT_EQ(rig.x.keyframe(0).spatialOutX, 192.0f);
    EXPECT_FLOAT_EQ(rig.y.keyframe(0).spatialOutY, 0.0f);
    rig.stack.redo();
    EXPECT_FLOAT_EQ(rig.y.keyframe(0).spatialOutY, 96.0f);
}

TEST(MonitorOverlayDrags, MotionHandleDragLeavesTheStepToAnOpenGesture)
{
    ensureApp();
    MotionRig rig;
    EditSession edits{rig.stack};
    edits.setSequenceResolver([&rig](uint64_t id) -> Timeline* {
        return id == rig.timeline.id() ? &rig.timeline : nullptr;
    });
    EditGesture gesture;
    QObject::connect(rig.overlay.get(), &TransformOverlayWidget::editDragStarted,
                     [&] { gesture = EditGesture(edits, "Transform", rig.timeline); });

    rig.overlay->press({384, 180});
    rig.overlay->move({384, 212});
    rig.overlay->release({384, 212});

    EXPECT_TRUE(edits.isOpen());
    EXPECT_EQ(rig.stack.undoCount(), 0u);
    gesture.cancel();
}

TEST(MonitorOverlayDrags, MiddleButtonPansWithoutAnEdit)
{
    ensureApp();
    OverlayRig rig;
    const float panX = rig.viewport.viewPanX();
    rig.overlay->press({320, 180}, Qt::NoModifier, Qt::MiddleButton);
    rig.overlay->move({340, 190}, Qt::NoModifier, Qt::MiddleButton);
    rig.overlay->release({340, 190}, Qt::NoModifier, Qt::MiddleButton);

    EXPECT_TRUE(rig.log.empty());
    EXPECT_FLOAT_EQ(rig.viewport.viewPanX(), panX + 20.0f);
}

TEST(MonitorOverlayDrags, ClickOffTheBoxBeforeAFrameIsShownPassesThrough)
{
    // emptyAreaClicked needs the presented frame's size; until then the
    // click is left to the viewport underneath.
    ensureApp();
    OverlayRig rig;
    rig.overlay->press({30, 30});
    rig.overlay->release({30, 30});
    EXPECT_TRUE(rig.log.empty());
}

TEST(MonitorOverlayDrags, LostCaptureFinishesTheDragAsARelease)
{
    // Windows can drop the grab mid-drag; the drag still ends as one
    // recorded edit rather than being left open.
    ensureApp();
    OverlayRig rig;
    rig.overlay->press({320, 180});
    rig.overlay->move({352, 180});
    rig.overlay->loseCapture();

    EXPECT_EQ(rig.log, (OverlayRig::Log{
        "started",
        "pos " + num(32 * kRefPerPx) + " 0.00",
        "transform done 0.00 0.00 0.50 0.50 0.00 -> " + num(32 * kRefPerPx) +
            " 0.00 0.50 0.50 0.00",
        "ended"}));

    // The next press starts a fresh drag from the new position.
    rig.log.clear();
    rig.overlay->press({352, 180});
    rig.overlay->release({352, 180});
    EXPECT_EQ(rig.log, (OverlayRig::Log{
        "started",
        "transform done " + num(32 * kRefPerPx) + " 0.00 0.50 0.50 0.00 -> " +
            num(32 * kRefPerPx) + " 0.00 0.50 0.50 0.00",
        "ended"}));
}

TEST(MonitorOverlayDrags, ReleaseWithoutAPressDoesNothing)
{
    ensureApp();
    OverlayRig rig;
    rig.overlay->move({350, 190});
    rig.overlay->release({350, 190});
    EXPECT_TRUE(rig.log.empty());
}
