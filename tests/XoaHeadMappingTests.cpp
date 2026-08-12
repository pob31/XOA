/*
    XoaHeadMappingTests.cpp - WP15 (D51): the spatcore-to-XOA head-attitude
    convention boundary (Source/DSP/AmbiHeadMapping.h).

    These are the tests that keep the webcam plugin's empirically-validated
    signs meaningful after the frame change. Each perceptual case is anchored
    on the WP3 encoder, not on a shared formula: encode a source, rotate the
    field by the compensation, and assert the source lands where a listener
    who moved their head that way would hear it.

    Reference conventions:
      spatcore HeadOrientation — +yaw turn right, +pitch look up,
                                 +roll right ear down (radians)
      xoa::rot                 — +yaw scene toward +Y (left), +pitch front
                                 down, +roll left side up (degrees)
*/

#include "XoaTestFramework.h"

#include "DSP/AmbiHeadMapping.h"
#include "DSP/AmbiRotation.h"
#include "DSP/AmbiSphericalHarmonics.h"
#include "XoaConstants.h"

#include <cmath>

namespace rot = xoa::rot;
namespace sh = xoa::sh;
namespace hm = xoa::binaural;

//==============================================================================
static double hmMaxDiff (const double* a, const double* b, int n) noexcept
{
    double m = 0.0;
    for (int i = 0; i < n; ++i)
        m = std::max (m, std::abs (a[i] - b[i]));
    return m;
}

static spatcore::binaural::HeadOrientation headDeg (double yawDeg, double pitchDeg,
                                                    double rollDeg)
{
    return hm::manualOrientation (yawDeg, pitchDeg, rollDeg);
}

/** Encode at (az, el), rotate by the compensation for `head`, and compare
    against a direct encode at the direction the listener should perceive. */
static void checkPerceived (const spatcore::binaural::HeadOrientation& head,
                            double srcAzDeg, double srcElDeg,
                            double expectAzDeg, double expectElDeg)
{
    rot::RotationMatrix R;
    rot::buildFromCartesian (hm::headCompensationMatrix (head), R);

    double enc[xoa::kNumSHChannels], got[xoa::kNumSHChannels], want[xoa::kNumSHChannels];
    sh::evaluate (srcAzDeg, srcElDeg, xoa::kAmbisonicOrder, enc);
    rot::apply (R, xoa::kAmbisonicOrder, enc, got);
    sh::evaluate (expectAzDeg, expectElDeg, xoa::kAmbisonicOrder, want);

    // HeadOrientation carries FLOAT radians (it is an RtSnapshot POD), so the
    // degrees->float radians->degrees round trip caps agreement around 1e-8 in
    // the SH coefficients. The convention itself is exact; this tolerance is
    // the float storage, not slop in the mapping.
    const double diff = hmMaxDiff (got, want, xoa::kNumSHChannels);
    if (diff >= 1e-6)
    {
        double cy, cp, cr;
        rot::matrixToYawPitchRoll (hm::headCompensationMatrix (head), cy, cp, cr);
        std::fprintf (stderr,
                      "  head(ypr rad %.3f %.3f %.3f) -> compensation(ypr deg %.2f %.2f %.2f);"
                      " src (%.1f,%.1f) expected (%.1f,%.1f), maxDiff %.3e\n",
                      head.yawRad, head.pitchRad, head.rollRad, cy, cp, cr,
                      srcAzDeg, srcElDeg, expectAzDeg, expectElDeg, diff);
    }
    CHECK (diff < 1e-6);
}

//==============================================================================
// The three perceptual anchors, one per axis.
static void testHeadMappingPerAxis()
{
    // Turn the head 30 deg RIGHT: a frontal source moves to the listener's
    // LEFT, i.e. toward +Y = increasing azimuth.
    checkPerceived (headDeg (30, 0, 0), 0.0, 0.0, 30.0, 0.0);

    // Look UP 20 deg: a frontal source drops BELOW the listener's horizon.
    checkPerceived (headDeg (0, 20, 0), 0.0, 0.0, 0.0, -20.0);

    // Tilt RIGHT EAR DOWN 25 deg: the left ear rises, so a source at the
    // listener's left drops relative to the head; the front axis is invariant.
    checkPerceived (headDeg (0, 0, 25), 90.0, 0.0, 90.0, -25.0);
    checkPerceived (headDeg (0, 0, 25), 0.0, 0.0, 0.0, 0.0);

    // Zero attitude is identity — the monitor with no tracker must not rotate.
    checkPerceived (headDeg (0, 0, 0), 33.0, 17.0, 33.0, 17.0);
}

//==============================================================================
// Compensation is the inverse of the attitude, and the mapping is a pure
// change of basis (orthonormality is preserved, det stays +1).
static void testHeadMappingInverseAndOrthonormality()
{
    const auto head = headDeg (37.0, -22.0, 51.0);
    const auto A = hm::headAttitudeMatrix (head);
    const auto C = hm::headCompensationMatrix (head);

    // C == A^T, and A·C == identity.
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            CHECK (std::abs (C.m[i][j] - A.m[j][i]) < 1e-15);

    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
        {
            double dot = 0.0;
            for (int k = 0; k < 3; ++k)
                dot += A.m[i][k] * C.m[k][j];
            CHECK (std::abs (dot - (i == j ? 1.0 : 0.0)) < 1e-12);
        }

    const double det =
        C.m[0][0] * (C.m[1][1] * C.m[2][2] - C.m[1][2] * C.m[2][1])
      - C.m[0][1] * (C.m[1][0] * C.m[2][2] - C.m[1][2] * C.m[2][0])
      + C.m[0][2] * (C.m[1][0] * C.m[2][1] - C.m[1][1] * C.m[2][0]);
    CHECK (std::abs (det - 1.0) < 1e-12);   // proper rotation: buildFromCartesian asserts this
}

//==============================================================================
// The trap the spatcore zero-calibration test also guards: composing
// rotations is NOT negating each angle. If someone ever "simplifies"
// headCompensationMatrix into three sign flips, this fails.
static void testHeadMappingCompositionIsNotPerAngleNegation()
{
    const auto head = headDeg (40.0, 30.0, 25.0);
    const auto C = hm::headCompensationMatrix (head);

    // The naive shortcut: negate each angle, rebuild.
    const auto naive = hm::headAttitudeMatrix (headDeg (-40.0, -30.0, -25.0));

    double maxDiff = 0.0;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            maxDiff = std::max (maxDiff, std::abs (C.m[i][j] - naive.m[i][j]));
    CHECK (maxDiff > 1e-3);

    // Single-axis attitudes are the special case where they DO agree — worth
    // pinning, since it explains why a yaw-only bring-up can hide the bug.
    const auto yawOnly = hm::headCompensationMatrix (headDeg (40.0, 0.0, 0.0));
    const auto yawNaive = hm::headAttitudeMatrix (headDeg (-40.0, 0.0, 0.0));
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            CHECK (std::abs (yawOnly.m[i][j] - yawNaive.m[i][j]) < 1e-12);
}

//==============================================================================
// The ypr readout form agrees with the matrix form.
static void testHeadMappingYprRoundTrip()
{
    const auto head = headDeg (18.0, -9.0, 44.0);

    double yawDeg, pitchDeg, rollDeg;
    hm::headCompensationYawPitchRoll (head, yawDeg, pitchDeg, rollDeg);

    rot::RotationMatrix viaAngles, viaMatrix;
    rot::buildFromYawPitchRoll (yawDeg, pitchDeg, rollDeg, viaAngles);
    rot::buildFromCartesian (hm::headCompensationMatrix (head), viaMatrix);

    CHECK (hmMaxDiff (viaAngles.coeffs, viaMatrix.coeffs, rot::kNumRotationCoeffs) < 1e-9);
}

//==============================================================================
void runXoaHeadMappingTests()
{
    testHeadMappingPerAxis();
    testHeadMappingInverseAndOrthonormality();
    testHeadMappingCompositionIsNotPerAngleNegation();
    testHeadMappingYprRoundTrip();
}
