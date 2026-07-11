/*
    XoaBusTests.cpp - WP6 RT bus suite.

    B1 (C2): the AmbiRtTypes.h snapshot composers, checked against the WP3/WP4
    double-precision primitives they compose (an independent recomputation,
    per the design-double / RT-float rule).

    B2-B9 (C3) extend this file with the AmbiBusAlgorithm chain tests,
    including the M1 null test (processBlock output == matrix * scene).
*/

#include "XoaTestFramework.h"

#include "XoaConstants.h"
#include "DSP/AmbiConventions.h"
#include "DSP/AmbiOrderWeights.h"
#include "DSP/AmbiRotation.h"
#include "DSP/AmbiRtTypes.h"
#include "DSP/AmbiSphericalHarmonics.h"

#include <cmath>

namespace
{

//==============================================================================
// B1 - makeRotationState vs the double build it wraps
//==============================================================================
void testRotationStateComposer()
{
    using namespace xoa;

    // Identity orientation -> float cast of the identity rotation matrix
    const auto ident = rt::makeRotationState (0.0, 0.0, 0.0, 1u);
    CHECK (ident.epoch == 1u);

    rot::RotationMatrix reference;
    rot::identity (reference);
    int mismatches = 0;
    for (int i = 0; i < rot::kNumRotationCoeffs; ++i)
        if (ident.coeffs[i] != static_cast<float> (reference.coeffs[i]))
            ++mismatches;
    CHECK (mismatches == 0);

    // Non-trivial orientation: the composer is exactly the double build cast
    // to float, coefficient for coefficient
    const auto state = rt::makeRotationState (30.0, 20.0, 10.0, 7u);
    CHECK (state.epoch == 7u);

    rot::buildFromYawPitchRoll (30.0, 20.0, 10.0, reference);
    mismatches = 0;
    for (int i = 0; i < rot::kNumRotationCoeffs; ++i)
        if (state.coeffs[i] != static_cast<float> (reference.coeffs[i]))
            ++mismatches;
    CHECK (mismatches == 0);
}

//==============================================================================
// B1 - gather table: SN3D identity prefix + order-adapt gains
//==============================================================================
void testBusParamsSn3d()
{
    using namespace xoa;

    const int order = 3;
    const int channels = sh::numChannels (order);   // 16

    const auto p = rt::makeBusParams (0, order, 0, channels, 0.0, 3u);
    CHECK (p.epoch == 3u);
    CHECK (p.contentOrder == order);
    CHECK (p.numInputChannels == channels);
    CHECK (p.masterGainLinear == 1.0f);

    double adapt[kNumSHChannels];
    weights::orderAdaptGains (order, kAmbisonicOrder, adapt);

    int mismatches = 0;
    for (int c = 0; c < kNumSHChannels; ++c)
    {
        if (c < channels)
        {
            if (p.srcChannel[c] != c)                                   ++mismatches;
            if (p.gain[c] != static_cast<float> (adapt[c]))             ++mismatches;
        }
        else
        {
            if (p.srcChannel[c] != -1)                                  ++mismatches;
            if (p.gain[c] != 0.0f)                                      ++mismatches;
        }
    }
    CHECK (mismatches == 0);

    // Full-order content (auto-detected order 10): identity gather, unit gains
    const auto full = rt::makeBusParams (0, 10, 0, kNumSHChannels, 0.0, 4u);
    mismatches = 0;
    for (int c = 0; c < kNumSHChannels; ++c)
        if (full.srcChannel[c] != c || full.gain[c] != 1.0f)
            ++mismatches;
    CHECK (mismatches == 0);
}

//==============================================================================
// B1 - override wins over detection; short files warn and stay silent above
//==============================================================================
void testBusParamsOverrideAndShortFile()
{
    using namespace xoa;

    // Override order 1 beats detected order 3
    const auto p1 = rt::makeBusParams (1, 3, 0, 16, 0.0, 1u);
    CHECK (p1.contentOrder == 1);
    CHECK (p1.srcChannel[3] == 3);
    CHECK (p1.srcChannel[4] == -1);   // beyond numChannels(1)

    // Order-3 content but only 9 source channels: usable prefix only + warning
    juce::String warning;
    const auto p2 = rt::makeBusParams (3, 0, 0, 9, 0.0, 2u, &warning);
    CHECK (warning.isNotEmpty());
    CHECK (p2.srcChannel[8] == 8);
    CHECK (p2.srcChannel[9] == -1);
    CHECK (p2.numInputChannels == 9);

    // No source channels: all silent, valid params
    const auto p3 = rt::makeBusParams (0, 0, 0, 0, 0.0, 5u);
    CHECK (p3.numInputChannels == 0);
    CHECK (p3.srcChannel[0] == -1);
}

//==============================================================================
// B1 - N3D scaling folds into the gather gains
//==============================================================================
void testBusParamsN3d()
{
    using namespace xoa;

    const int order = 2;
    const int channels = sh::numChannels (order);   // 9

    const auto p = rt::makeBusParams (0, order, 1, channels, 0.0, 1u);

    double adapt[kNumSHChannels];
    weights::orderAdaptGains (order, kAmbisonicOrder, adapt);

    int mismatches = 0;
    for (int c = 0; c < channels; ++c)
    {
        const auto expected = static_cast<float> (adapt[c] * conv::n3dToSn3d (sh::acnToOrder (c)));
        if (p.srcChannel[c] != c || p.gain[c] != expected)
            ++mismatches;
    }
    CHECK (mismatches == 0);
    CHECK (p.gain[0] == static_cast<float> (adapt[0]));   // l=0: n3dToSn3d == 1
    CHECK (p.gain[1] < p.gain[0]);                        // l=1: 1/sqrt(3) < 1
}

//==============================================================================
// B1 - FuMa is a remap + gain; beyond order 3 it falls back loudly
//==============================================================================
void testBusParamsFuma()
{
    using namespace xoa;

    // First-order FuMa (WXYZ): W->ACN0 (sqrt2), X->ACN3, Y->ACN1, Z->ACN2
    const auto p = rt::makeBusParams (0, 1, 2, 4, 0.0, 1u);
    CHECK (p.srcChannel[0] == 0);
    CHECK (p.srcChannel[1] == 2);
    CHECK (p.srcChannel[2] == 3);
    CHECK (p.srcChannel[3] == 1);

    double adapt[kNumSHChannels];
    weights::orderAdaptGains (1, kAmbisonicOrder, adapt);
    CHECK (p.gain[0] == static_cast<float> (adapt[0] * std::sqrt (2.0)));
    CHECK (p.gain[1] == static_cast<float> (adapt[1]));
    CHECK (p.srcChannel[4] == -1);

    // FuMa at order 5 is undefined: warning + SN3D fallback (identity gather)
    juce::String warning;
    const auto fb = rt::makeBusParams (5, 0, 2, 36, 0.0, 2u, &warning);
    CHECK (warning.containsIgnoreCase ("FuMa"));
    CHECK (fb.srcChannel[0] == 0);
    CHECK (fb.srcChannel[35] == 35);
    CHECK (fb.contentOrder == 5);
}

//==============================================================================
// B1 - master gain is cooked to linear at compose time
//==============================================================================
void testBusParamsMasterGain()
{
    using namespace xoa;

    CHECK (rt::makeBusParams (0, 10, 0, 121, 0.0, 1u).masterGainLinear == 1.0f);

    const auto p = rt::makeBusParams (0, 10, 0, 121, -20.0, 1u);
    CHECK (std::abs (p.masterGainLinear - 0.1f) < 1.0e-7f);

    const auto q = rt::makeBusParams (0, 10, 0, 121, 6.0, 1u);
    CHECK (std::abs (q.masterGainLinear - 1.9952624f) < 1.0e-5f);
}

} // namespace

//==============================================================================
void runXoaBusTests()
{
    testRotationStateComposer();
    testBusParamsSn3d();
    testBusParamsOverrideAndShortFile();
    testBusParamsN3d();
    testBusParamsFuma();
    testBusParamsMasterGain();
}
