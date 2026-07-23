/*
 * Focused tests for MaskTracker's mask-coordinate to decoded-pixel mapping.
 */

#include <gtest/gtest.h>

#include "panels/effects/MaskTracker.h"

namespace {

using rt::MaskCoordinateSpace;
using rt::MaskTrackParams;
using rt::MaskTracker::detail::buildCoordinateMapping;

TEST(MaskTrackerCoordinateMapping, SourceLocalUsesNativeDecodedPixels)
{
    MaskTrackParams params;
    params.outW = 3840;
    params.outH = 2160;
    params.posX = 431.0f;
    params.posY = -197.0f;
    params.scaleX = 0.0f; // A hidden/collapsed layer is still trackable.
    params.scaleY = -2.5f;
    params.rotation = 37.0f;
    params.anchorX = 120.0f;
    params.anchorY = -80.0f;

    const auto mapping = buildCoordinateMapping(
        params, MaskCoordinateSpace::SourceLocal,
        1920, 1080, /*sourceRotationDegrees=*/90);

    ASSERT_TRUE(mapping.isValid());

    float sourceX = 0.0f;
    float sourceY = 0.0f;
    mapping.toSourcePixels(0.25f, 0.75f, sourceX, sourceY);
    EXPECT_FLOAT_EQ(sourceX, 480.0f);
    EXPECT_FLOAT_EQ(sourceY, 810.0f);

    float maskU = 0.0f;
    float maskV = 0.0f;
    mapping.sourceDeltaToMask(192.0f, -108.0f, maskU, maskV);
    EXPECT_NEAR(maskU, 0.1f, 1.0e-6f);
    EXPECT_NEAR(maskV, -0.1f, 1.0e-6f);
    EXPECT_FLOAT_EQ(mapping.pixelsPerMaskU(), 1920.0f);
    EXPECT_FLOAT_EQ(mapping.pixelsPerMaskV(), 1080.0f);
}

TEST(MaskTrackerCoordinateMapping, LegacySequenceFrameHonorsSourceRotation)
{
    MaskTrackParams params;
    // A native 640x360 frame displayed 90 degrees clockwise is 360x640.
    // Matching output dimensions isolate the orientation mapping from fit.
    params.outW = 360;
    params.outH = 640;

    const auto mapping = buildCoordinateMapping(
        params, MaskCoordinateSpace::LegacySequenceFrame,
        640, 360, /*sourceRotationDegrees=*/90);

    ASSERT_TRUE(mapping.isValid());

    float sourceX = 0.0f;
    float sourceY = 0.0f;
    mapping.toSourcePixels(0.25f, 0.75f, sourceX, sourceY);
    // 90-degree display rotation maps display UV (u,v) to native (v,1-u).
    EXPECT_NEAR(sourceX, 480.0f, 1.0e-4f);
    EXPECT_NEAR(sourceY, 270.0f, 1.0e-4f);

    float maskU = 0.0f;
    float maskV = 0.0f;
    mapping.sourceDeltaToMask(64.0f, -36.0f, maskU, maskV);
    EXPECT_NEAR(maskU, 0.1f, 1.0e-6f);
    EXPECT_NEAR(maskV, 0.1f, 1.0e-6f);
    EXPECT_NEAR(mapping.pixelsPerMaskU(), 360.0f, 1.0e-4f);
    EXPECT_NEAR(mapping.pixelsPerMaskV(), 640.0f, 1.0e-4f);
}

} // namespace
