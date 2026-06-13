/*
 * test_waveform.cpp — Unit tests for Step 19: GPU Waveform Renderer.
 *
 * Tests:
 *   WaveformRendererTest — CPU-side layout utilities
 *     - LayoutPeaks_basic, converts peaks to screen columns
 *     - LayoutPeaks_empty, no peaks → no columns
 *     - PeaksToVertices, produces 2 vertices per peak
 *     - GradientColor_low/mid/high, verifies 3-stop gradient
 *     - GradientColor_clamp, out-of-range values clamped
 *     - IdealSamplesPerPixel, correct calculation
 *     - LayoutPeaks_amplitude, amplitude from peak range
 *
 *   WaveformWidgetTest — Qt widget creation and API
 *     - Creation, widget exists with default size
 *     - SetWaveformData_null, handles nullptr
 *     - SetWaveformData_valid, binds data and updates range
 *     - VisibleRange, set and get visible range
 *     - ZoomIn, halves visible range
 *     - ZoomOut, doubles visible range
 *     - ZoomToFit, resets to full range
 *     - Playhead, set and get position
 *     - PlayheadVisibility, show/hide playhead
 *     - Segments, set and retrieve segments
 *     - ActiveSegment, set and retrieve active segment
 *     - CoordinateConversion, frame↔pixel roundtrip
 *     - BackgroundColor, set and get
 *     - WaveformColors, set gradient colors
 *     - PlayheadColor, set and get
 *     - TimeGridVisible, show/hide grid
 *     - ChannelCount, set and get
 *     - SamplesPerPixel, computed from range and width
 *     - PositionSignal, click emits positionChanged
 *
 *   VUMeterTest — Audio level meter widget
 *     - Creation, widget exists with default size
 *     - SetLevel, single channel level
 *     - SetLevels, multi-channel levels
 *     - Reset, resets all levels
 *     - ChannelCount, set and retrieve
 *     - Orientation, set and retrieve
 *     - PeakHold, enable/disable peak hold
 *     - ScaleVisible, show/hide dB scale
 *     - MinDb, set and get min dB
 *     - LinearToDb, conversion check
 *     - DbToLinear, inverse conversion check
 *     - DbToPosition, normalized mapping
 *     - GradientColors, set and get
 *     - ClipColor, set and get
 *     - PeakHoldColor, set and get
 *     - ClippingSignal, emitted when level >= 1.0
 */

#include "WaveformRenderer.h"
#include "widgets/WaveformWidget.h"
#include "widgets/VUMeter.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QSignalSpy>
#include <QTest>

#include <cmath>
#include <vector>

// ── QApplication fixture ─────────────────────────────────────────────────────

static int    s_argc = 1;
static char   s_arg0[] = "test_waveform";
static char*  s_argv[] = {s_arg0, nullptr};

class WaveformTestEnv : public ::testing::Environment
{
public:
    void SetUp() override
    {
        if (!QApplication::instance())
            m_app = new QApplication(s_argc, s_argv);
    }
    void TearDown() override { /* QApplication leaks intentionally in tests */ }
private:
    QApplication* m_app{nullptr};
};

static auto* const g_env =
    ::testing::AddGlobalTestEnvironment(new WaveformTestEnv);

// ═════════════════════════════════════════════════════════════════════════════
// WaveformRendererTest
// ═════════════════════════════════════════════════════════════════════════════

class WaveformRendererTest : public ::testing::Test {};

TEST_F(WaveformRendererTest, LayoutPeaks_basic)
{
    // 3 peaks with known min/max values
    rt::WaveformPeak peaks[] = {
        {-0.5f,  0.5f},
        {-0.8f,  0.8f},
        {-0.2f,  0.2f}
    };

    auto cols = rt::WaveformRenderer::layoutPeaks(
        peaks, 3,
        10.0f,   // startX
        5.0f,    // pixelsPerPeak
        100.0f,  // centerY
        50.0f    // halfHeight
    );

    ASSERT_EQ(cols.size(), 3u);

    // First column at x = 10
    EXPECT_FLOAT_EQ(cols[0].x, 10.0f);
    // Y = center ± peak * halfHeight: min → 100 + 0.5*50 = 125, max → 100 - 0.5*50 = 75
    EXPECT_FLOAT_EQ(cols[0].yMin, 125.0f);
    EXPECT_FLOAT_EQ(cols[0].yMax,  75.0f);

    // Second peak at x = 15
    EXPECT_FLOAT_EQ(cols[1].x, 15.0f);
    EXPECT_FLOAT_EQ(cols[1].yMin, 140.0f);
    EXPECT_FLOAT_EQ(cols[1].yMax,  60.0f);

    // Third peak at x = 20
    EXPECT_FLOAT_EQ(cols[2].x, 20.0f);
    EXPECT_FLOAT_EQ(cols[2].yMin, 110.0f);
    EXPECT_FLOAT_EQ(cols[2].yMax,  90.0f);
}

TEST_F(WaveformRendererTest, LayoutPeaks_empty)
{
    auto cols = rt::WaveformRenderer::layoutPeaks(nullptr, 0, 0, 1, 100, 50);
    EXPECT_TRUE(cols.empty());
}

TEST_F(WaveformRendererTest, PeaksToVertices)
{
    rt::WaveformPeak peaks[] = {
        {-0.5f, 0.5f},
        {-1.0f, 1.0f}
    };

    auto verts = rt::WaveformRenderer::peaksToVertices(
        peaks, 2, 0.0f, 1.0f, 100.0f, 50.0f, 0.0f);

    // 2 peaks × 2 vertices each = 4
    ASSERT_EQ(verts.size(), 4u);

    // First peak min vertex
    EXPECT_FLOAT_EQ(verts[0].x, 0.0f);
    EXPECT_FLOAT_EQ(verts[0].y, 125.0f);  // center + 0.5*50

    // First peak max vertex
    EXPECT_FLOAT_EQ(verts[1].x, 0.0f);
    EXPECT_FLOAT_EQ(verts[1].y, 75.0f);   // center - 0.5*50

    // Second peak vertices
    EXPECT_FLOAT_EQ(verts[2].y, 150.0f);  // center + 1.0*50
    EXPECT_FLOAT_EQ(verts[3].y, 50.0f);   // center - 1.0*50

    // Channel stored
    EXPECT_FLOAT_EQ(verts[0].channel, 0.0f);
}

TEST_F(WaveformRendererTest, GradientColor_low)
{
    auto c = rt::WaveformRenderer::gradientColor(0.0f);
    // At 0 amplitude → colorLow (green): (0, 0.8, 0, 1)
    EXPECT_FLOAT_EQ(c.r, 0.0f);
    EXPECT_FLOAT_EQ(c.g, 0.8f);
    EXPECT_FLOAT_EQ(c.b, 0.0f);
    EXPECT_FLOAT_EQ(c.a, 1.0f);
}

TEST_F(WaveformRendererTest, GradientColor_mid)
{
    auto c = rt::WaveformRenderer::gradientColor(0.5f);
    // At 0.5 → colorMid (yellow): (0.9, 0.9, 0, 1)
    EXPECT_FLOAT_EQ(c.r, 0.9f);
    EXPECT_FLOAT_EQ(c.g, 0.9f);
    EXPECT_FLOAT_EQ(c.b, 0.0f);
    EXPECT_FLOAT_EQ(c.a, 1.0f);
}

TEST_F(WaveformRendererTest, GradientColor_high)
{
    auto c = rt::WaveformRenderer::gradientColor(1.0f);
    // At 1.0 → colorHigh (red): (1.0, 0.2, 0, 1)
    EXPECT_FLOAT_EQ(c.r, 1.0f);
    EXPECT_FLOAT_EQ(c.g, 0.2f);
    EXPECT_FLOAT_EQ(c.b, 0.0f);
    EXPECT_FLOAT_EQ(c.a, 1.0f);
}

TEST_F(WaveformRendererTest, GradientColor_clamp)
{
    // Negative amplitude → clamped to 0 → colorLow
    auto c1 = rt::WaveformRenderer::gradientColor(-1.0f);
    EXPECT_FLOAT_EQ(c1.r, 0.0f);
    EXPECT_FLOAT_EQ(c1.g, 0.8f);

    // Over 1.0 → clamped to 1 → colorHigh
    auto c2 = rt::WaveformRenderer::gradientColor(5.0f);
    EXPECT_FLOAT_EQ(c2.r, 1.0f);
    EXPECT_FLOAT_EQ(c2.g, 0.2f);
}

TEST_F(WaveformRendererTest, GradientColor_interpolation)
{
    // 0.25 is halfway between low and mid
    auto c = rt::WaveformRenderer::gradientColor(0.25f);
    // Expect midpoint: r = (0 + 0.9)/2 = 0.45, g = (0.8 + 0.9)/2 = 0.85
    EXPECT_NEAR(c.r, 0.45f, 0.01f);
    EXPECT_NEAR(c.g, 0.85f, 0.01f);
}

TEST_F(WaveformRendererTest, IdealSamplesPerPixel)
{
    // 480000 frames visible in 1920 pixels → 250 samples/pixel
    auto spp = rt::WaveformRenderer::idealSamplesPerPixel(
        960000, 480000, 1920);
    EXPECT_EQ(spp, 250u);
}

TEST_F(WaveformRendererTest, IdealSamplesPerPixel_zero)
{
    // Zero width → should return at least 1
    auto spp = rt::WaveformRenderer::idealSamplesPerPixel(100000, 100000, 0);
    EXPECT_GE(spp, 1u);
}

TEST_F(WaveformRendererTest, LayoutPeaks_amplitude)
{
    // Amplitude should reflect the range (max - min)
    rt::WaveformPeak peaks[] = {
        {-0.1f, 0.1f},   // small range → low amplitude
        {-1.0f, 1.0f}    // full range → high amplitude
    };

    auto cols = rt::WaveformRenderer::layoutPeaks(
        peaks, 2, 0, 1, 100, 50);

    ASSERT_EQ(cols.size(), 2u);
    EXPECT_LT(cols[0].amplitude, cols[1].amplitude);
    EXPECT_NEAR(cols[1].amplitude, 1.0f, 0.01f);
}

// ═════════════════════════════════════════════════════════════════════════════
// WaveformWidgetTest
// ═════════════════════════════════════════════════════════════════════════════

class WaveformWidgetTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        m_widget = std::make_unique<rt::WaveformWidget>();
        m_widget->resize(400, 120);
        m_widget->show();
        QApplication::processEvents();
    }

    void TearDown() override
    {
        m_widget.reset();
    }

    std::unique_ptr<rt::WaveformWidget> m_widget;
};

TEST_F(WaveformWidgetTest, Creation)
{
    EXPECT_NE(m_widget, nullptr);
    EXPECT_GE(m_widget->width(), 100);
    EXPECT_GE(m_widget->height(), 40);
}

TEST_F(WaveformWidgetTest, SetWaveformData_null)
{
    m_widget->setWaveformData(nullptr);
    EXPECT_EQ(m_widget->waveformData(), nullptr);
}

TEST_F(WaveformWidgetTest, SetWaveformData_valid)
{
    rt::WaveformData data;
    data.totalFrames = 480000;
    data.sampleRate  = 48000;
    data.channels    = 2;

    // Add a mip level
    rt::WaveformMipLevel mip;
    mip.samplesPerPeak = 256;
    mip.channels = 2;
    mip.peaks.resize(200);
    data.mipLevels.push_back(std::move(mip));

    m_widget->setWaveformData(&data);
    EXPECT_EQ(m_widget->waveformData(), &data);
    // Should auto-set visible range to full
    EXPECT_EQ(m_widget->visibleStartFrame(), 0);
    EXPECT_EQ(m_widget->visibleEndFrame(), 480000);
}

TEST_F(WaveformWidgetTest, VisibleRange)
{
    m_widget->setVisibleRange(1000, 5000);
    EXPECT_EQ(m_widget->visibleStartFrame(), 1000);
    EXPECT_EQ(m_widget->visibleEndFrame(), 5000);
}

TEST_F(WaveformWidgetTest, VisibleRange_invalid)
{
    m_widget->setVisibleRange(1000, 5000);
    // Invalid: start >= end → should be ignored
    m_widget->setVisibleRange(5000, 1000);
    EXPECT_EQ(m_widget->visibleStartFrame(), 1000);
    EXPECT_EQ(m_widget->visibleEndFrame(), 5000);
}

TEST_F(WaveformWidgetTest, ZoomIn)
{
    m_widget->setVisibleRange(0, 10000);
    m_widget->zoomIn();
    int64_t visible = m_widget->visibleEndFrame() - m_widget->visibleStartFrame();
    // Should be approximately half
    EXPECT_LE(visible, 6000);
    EXPECT_GE(visible, 4000);
}

TEST_F(WaveformWidgetTest, ZoomOut)
{
    rt::WaveformData data;
    data.totalFrames = 100000;
    data.sampleRate  = 48000;
    data.channels    = 1;
    m_widget->setWaveformData(&data);

    m_widget->setVisibleRange(25000, 75000);
    m_widget->zoomOut();
    int64_t visible = m_widget->visibleEndFrame() - m_widget->visibleStartFrame();
    // Should be approximately double, capped at totalFrames
    EXPECT_GE(visible, 90000);
}

TEST_F(WaveformWidgetTest, ZoomToFit)
{
    rt::WaveformData data;
    data.totalFrames = 96000;
    data.sampleRate  = 48000;
    data.channels    = 1;
    m_widget->setWaveformData(&data);

    m_widget->setVisibleRange(10000, 20000);
    m_widget->zoomToFit();
    EXPECT_EQ(m_widget->visibleStartFrame(), 0);
    EXPECT_EQ(m_widget->visibleEndFrame(), 96000);
}

TEST_F(WaveformWidgetTest, Playhead)
{
    m_widget->setPlayheadFrame(12345);
    EXPECT_EQ(m_widget->playheadFrame(), 12345);
}

TEST_F(WaveformWidgetTest, PlayheadVisibility)
{
    EXPECT_TRUE(m_widget->isPlayheadVisible());
    m_widget->setPlayheadVisible(false);
    EXPECT_FALSE(m_widget->isPlayheadVisible());
}

TEST_F(WaveformWidgetTest, Segments)
{
    std::vector<rt::WaveformSegment> segs = {
        {0, 1000, "Intro", false},
        {1000, 5000, "Main", true},
        {5000, 8000, "End", false}
    };
    m_widget->setSegments(segs);
    EXPECT_EQ(m_widget->segments().size(), 3u);
    EXPECT_EQ(m_widget->segments()[1].label, "Main");
    EXPECT_TRUE(m_widget->segments()[1].active);
}

TEST_F(WaveformWidgetTest, ActiveSegment)
{
    EXPECT_EQ(m_widget->activeSegment(), -1);
    m_widget->setActiveSegment(2);
    EXPECT_EQ(m_widget->activeSegment(), 2);
}

TEST_F(WaveformWidgetTest, CoordinateConversion)
{
    m_widget->setVisibleRange(0, 48000);
    // With 400px width, 48000 frames → 120 samples/pixel

    // Frame 24000 should be at pixel 200 (midpoint)
    double px = m_widget->frameToPixelX(24000);
    EXPECT_NEAR(px, 200.0, 1.0);

    // Round-trip
    int64_t frame = m_widget->pixelXToFrame(px);
    EXPECT_NEAR(frame, 24000, 200); // Allow small rounding
}

TEST_F(WaveformWidgetTest, BackgroundColor)
{
    m_widget->setBackgroundColor(QColor(30, 30, 35));
    EXPECT_EQ(m_widget->backgroundColor(), QColor(30, 30, 35));
}

TEST_F(WaveformWidgetTest, PlayheadColor)
{
    m_widget->setPlayheadColor(QColor(0, 200, 255));
    EXPECT_EQ(m_widget->playheadColor(), QColor(0, 200, 255));
}

TEST_F(WaveformWidgetTest, TimeGridVisible)
{
    EXPECT_TRUE(m_widget->isTimeGridVisible());
    m_widget->setTimeGridVisible(false);
    EXPECT_FALSE(m_widget->isTimeGridVisible());
}

TEST_F(WaveformWidgetTest, ChannelCount)
{
    m_widget->setChannelCount(1);
    EXPECT_EQ(m_widget->channelCount(), 1);
    m_widget->setChannelCount(4);
    EXPECT_EQ(m_widget->channelCount(), 4);
}

TEST_F(WaveformWidgetTest, SamplesPerPixel)
{
    m_widget->setVisibleRange(0, 48000);
    // 48000 frames / 400 px = 120
    EXPECT_NEAR(m_widget->samplesPerPixel(), 120.0, 5.0);
}

TEST_F(WaveformWidgetTest, PositionSignal)
{
    qRegisterMetaType<int64_t>("int64_t");
    QSignalSpy spy(m_widget.get(), &rt::WaveformWidget::positionChanged);

    m_widget->setVisibleRange(0, 48000);

    // Simulate click at center
    QTest::mouseClick(m_widget.get(), Qt::LeftButton, Qt::NoModifier,
                      QPoint(200, 60));

    EXPECT_GE(spy.count(), 1);
    auto emittedFrame = spy.at(0).at(0).toLongLong();
    // Click at mid-X should give ~24000
    EXPECT_NEAR(emittedFrame, 24000, 2000);
}

TEST_F(WaveformWidgetTest, VisibleRangeSignal)
{
    qRegisterMetaType<int64_t>("int64_t");
    QSignalSpy spy(m_widget.get(), &rt::WaveformWidget::visibleRangeChanged);

    m_widget->setVisibleRange(1000, 5000);

    EXPECT_GE(spy.count(), 1);
}

TEST_F(WaveformWidgetTest, SizeHints)
{
    auto sh = m_widget->sizeHint();
    EXPECT_GE(sh.width(), 100);
    EXPECT_GE(sh.height(), 40);

    auto msh = m_widget->minimumSizeHint();
    EXPECT_GE(msh.width(), 50);
    EXPECT_GE(msh.height(), 20);
}

TEST_F(WaveformWidgetTest, PaintWithData)
{
    // Verify painting doesn't crash with actual data
    rt::WaveformData data;
    data.totalFrames = 48000;
    data.sampleRate  = 48000;
    data.channels    = 1;

    rt::WaveformMipLevel mip;
    mip.samplesPerPeak = 256;
    mip.channels = 1;
    // ~187 peaks for 48000 samples at 256 spp
    mip.peaks.resize(188);
    for (size_t i = 0; i < mip.peaks.size(); ++i) {
        float v = static_cast<float>(i) / mip.peaks.size();
        mip.peaks[i] = {-v, v};
    }
    data.mipLevels.push_back(std::move(mip));

    m_widget->setWaveformData(&data);
    m_widget->repaint();
    QApplication::processEvents();
    SUCCEED(); // If no crash, pass
}

// ═════════════════════════════════════════════════════════════════════════════
// VUMeterTest
// ═════════════════════════════════════════════════════════════════════════════

class VUMeterTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        m_meter = std::make_unique<rt::VUMeter>();
        m_meter->show();
        QApplication::processEvents();
    }

    void TearDown() override
    {
        m_meter.reset();
    }

    std::unique_ptr<rt::VUMeter> m_meter;
};

TEST_F(VUMeterTest, Creation)
{
    EXPECT_NE(m_meter, nullptr);
    EXPECT_EQ(m_meter->channelCount(), 2);  // Default stereo
}

TEST_F(VUMeterTest, SetLevel)
{
    m_meter->setLevel(0, 0.5f);
    EXPECT_FLOAT_EQ(m_meter->level(0), 0.5f);
    m_meter->setLevel(1, 0.8f);
    EXPECT_FLOAT_EQ(m_meter->level(1), 0.8f);
}

TEST_F(VUMeterTest, SetLevel_outOfRange)
{
    // Invalid channel → no crash
    m_meter->setLevel(-1, 0.5f);
    m_meter->setLevel(99, 0.5f);
    EXPECT_FLOAT_EQ(m_meter->level(-1), 0.0f);
    EXPECT_FLOAT_EQ(m_meter->level(99), 0.0f);
}

TEST_F(VUMeterTest, SetLevels)
{
    m_meter->setLevels({0.3f, 0.7f});
    EXPECT_FLOAT_EQ(m_meter->level(0), 0.3f);
    EXPECT_FLOAT_EQ(m_meter->level(1), 0.7f);
}

TEST_F(VUMeterTest, Reset)
{
    m_meter->setLevels({0.6f, 0.9f});
    m_meter->reset();
    EXPECT_FLOAT_EQ(m_meter->level(0), 0.0f);
    EXPECT_FLOAT_EQ(m_meter->level(1), 0.0f);
}

TEST_F(VUMeterTest, ChannelCount)
{
    m_meter->setChannelCount(4);
    EXPECT_EQ(m_meter->channelCount(), 4);
    // Can set levels for new channels
    m_meter->setLevel(3, 0.4f);
    EXPECT_FLOAT_EQ(m_meter->level(3), 0.4f);
}

TEST_F(VUMeterTest, Orientation)
{
    EXPECT_EQ(m_meter->orientation(), rt::VUMeter::Orientation::Vertical);
    m_meter->setOrientation(rt::VUMeter::Orientation::Horizontal);
    EXPECT_EQ(m_meter->orientation(), rt::VUMeter::Orientation::Horizontal);
}

TEST_F(VUMeterTest, PeakHold)
{
    EXPECT_TRUE(m_meter->isPeakHoldEnabled());
    m_meter->setPeakHoldEnabled(false);
    EXPECT_FALSE(m_meter->isPeakHoldEnabled());
}

TEST_F(VUMeterTest, PeakHoldDecayMs)
{
    m_meter->setPeakHoldDecayMs(2000);
    EXPECT_EQ(m_meter->peakHoldDecayMs(), 2000);
}

TEST_F(VUMeterTest, ScaleVisible)
{
    EXPECT_TRUE(m_meter->isScaleVisible());
    m_meter->setScaleVisible(false);
    EXPECT_FALSE(m_meter->isScaleVisible());
}

TEST_F(VUMeterTest, MinDb)
{
    m_meter->setMinDb(-80.0f);
    EXPECT_FLOAT_EQ(m_meter->minDb(), -80.0f);
}

TEST_F(VUMeterTest, LinearToDb)
{
    // 1.0 → 0 dB
    EXPECT_NEAR(rt::VUMeter::linearToDb(1.0f), 0.0f, 0.01f);
    // 0.5 → ~-6.02 dB
    EXPECT_NEAR(rt::VUMeter::linearToDb(0.5f), -6.02f, 0.1f);
    // 0.0 → very negative
    EXPECT_LT(rt::VUMeter::linearToDb(0.0f), -100.0f);
}

TEST_F(VUMeterTest, DbToLinear)
{
    EXPECT_NEAR(rt::VUMeter::dbToLinear(0.0f), 1.0f, 0.01f);
    EXPECT_NEAR(rt::VUMeter::dbToLinear(-6.0f), 0.501f, 0.01f);
    EXPECT_NEAR(rt::VUMeter::dbToLinear(-20.0f), 0.1f, 0.01f);
}

TEST_F(VUMeterTest, DbToPosition)
{
    // 0 dB → position 1.0 (top)
    EXPECT_NEAR(m_meter->dbToPosition(0.0f), 1.0f, 0.01f);
    // minDb → position 0.0 (bottom)
    EXPECT_NEAR(m_meter->dbToPosition(m_meter->minDb()), 0.0f, 0.01f);
    // -30 dB with default minDb=-60 → position 0.5
    EXPECT_NEAR(m_meter->dbToPosition(-30.0f), 0.5f, 0.01f);
}

TEST_F(VUMeterTest, BackgroundColor)
{
    m_meter->setBackgroundColor(QColor(40, 40, 45));
    EXPECT_EQ(m_meter->backgroundColor(), QColor(40, 40, 45));
}

TEST_F(VUMeterTest, ClipColor)
{
    m_meter->setClipColor(QColor(255, 100, 0));
    EXPECT_EQ(m_meter->clipColor(), QColor(255, 100, 0));
}

TEST_F(VUMeterTest, PeakHoldColor)
{
    m_meter->setPeakHoldColor(QColor(200, 200, 200));
    EXPECT_EQ(m_meter->peakHoldColor(), QColor(200, 200, 200));
}

TEST_F(VUMeterTest, ClippingSignal)
{
    QSignalSpy spy(m_meter.get(), &rt::VUMeter::clipping);
    m_meter->setLevel(0, 1.0f);
    EXPECT_GE(spy.count(), 1);
    EXPECT_EQ(spy.at(0).at(0).toInt(), 0);
}

TEST_F(VUMeterTest, SizeHints)
{
    auto sh = m_meter->sizeHint();
    EXPECT_GT(sh.width(), 0);
    EXPECT_GT(sh.height(), 0);

    auto msh = m_meter->minimumSizeHint();
    EXPECT_GT(msh.width(), 0);
    EXPECT_GT(msh.height(), 0);
}

TEST_F(VUMeterTest, PaintVertical)
{
    m_meter->resize(40, 120);
    m_meter->setLevels({0.5f, 0.7f});
    m_meter->repaint();
    QApplication::processEvents();
    SUCCEED();
}

TEST_F(VUMeterTest, PaintHorizontal)
{
    m_meter->setOrientation(rt::VUMeter::Orientation::Horizontal);
    m_meter->resize(120, 40);
    m_meter->setLevels({0.5f, 0.7f});
    m_meter->repaint();
    QApplication::processEvents();
    SUCCEED();
}
