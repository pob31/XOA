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
#include "DSP/AmbiBusAlgorithm.h"
#include "DSP/AmbiConventions.h"
#include "DSP/AmbiDecoderDesigner.h"
#include "DSP/AmbiOrderWeights.h"
#include "DSP/AmbiRotation.h"
#include "DSP/AmbiRtTypes.h"
#include "DSP/AmbiSphericalHarmonics.h"
#include "DSP/DecoderMatrixBuilder.h"
#include "Helpers/XoaCoordinates.h"

#include <algorithm>
#include <cmath>

namespace
{

namespace dec = xoa::decoder;
namespace coords = xoa::coords;

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

//==============================================================================
// B2-B9 - the AmbiBusAlgorithm RT chain, checked against a double reference.
//==============================================================================

// A regular ring on the XY plane, matching XoaValueTreeState's default speakers.
dec::SpeakerLayout makeRing (int count, double radius)
{
    dec::SpeakerLayout layout;
    layout.count = count;
    for (int s = 0; s < count; ++s)
    {
        const double az = 360.0 * (double) s / (double) count;
        layout.positions[s] = coords::sphericalToCartesian (
            { radius, coords::normalizeAzimuthDegrees (az), 0.0 });
    }
    return layout;
}

// Deterministic per-channel input signal (no dependence on the scene generator).
void fillInput (juce::AudioBuffer<float>& in, int numCh, int n)
{
    for (int c = 0; c < numCh; ++c)
    {
        float* d = in.getWritePointer (c);
        for (int i = 0; i < n; ++i)
            d[i] = 0.1f * (float) (c + 1) * std::sin (0.07f * (float) (i + 1) + 0.30f * (float) c)
                 + 0.02f * (float) (((c * 7 + i) % 5) - 2);
    }
}

void runOneBlock (xoa::AmbiBusAlgorithm& algo, const juce::AudioBuffer<float>& input,
                  int numIn, int numOut, juce::AudioBuffer<float>& output, int n)
{
    output.setSize (numOut, n, false, false, true);
    output.clear();
    juce::AudioSourceChannelInfo info (&output, 0, n);
    algo.processBlock (info, input, numIn, numOut);
}

// Worst |output - reference| where reference = gather -> (optional) rotate ->
// decode -> master gain, computed in double from the double master matrix.
double worstDecodeError (const juce::AudioBuffer<float>& output,
                         const juce::AudioBuffer<float>& input, int numIn,
                         const xoa::rot::RotationMatrix* R,   // null -> identity (bypass)
                         const dec::DecoderMatrix& D,
                         const xoa::rt::BusRtParams& bp,
                         double gainMul, int n)
{
    using namespace xoa;
    double worst = 0.0;
    const int K = sh::numChannels (D.order);
    for (int i = 0; i < n; ++i)
    {
        double busVec[kNumSHChannels] = { 0.0 };
        for (int c = 0; c < kNumSHChannels; ++c)
        {
            const int src = bp.srcChannel[c];
            busVec[c] = (src >= 0 && src < numIn)
                          ? (double) input.getSample (src, i) * (double) bp.gain[c] : 0.0;
        }
        double rotVec[kNumSHChannels];
        if (R != nullptr) rot::apply (*R, kAmbisonicOrder, busVec, rotVec);
        else              std::copy (busVec, busVec + kNumSHChannels, rotVec);

        for (int s = 0; s < D.numSpeakers; ++s)
        {
            double acc = 0.0;
            for (int c = 0; c < K; ++c)
                acc += D.at (s, c) * rotVec[c];
            acc *= gainMul;
            worst = std::max (worst, std::abs ((double) output.getSample (s, i) - acc));
        }
    }
    return worst;
}

//==============================================================================
// B2 - the M1 null test: static scene, identity rotation, SAD decode to the
// 24-ring == the double reference matrix . gathered bus, to float tolerance.
void testNullDecode()
{
    const int numIn = 16, numOut = 24, n = 64;

    xoa::DecoderMatrixBuilder builder;
    builder.rebuild (makeRing (24, 2.0), dec::DesignOptions {});   // SAD/maxRe, clamps to order 3
    builder.publish();
    const auto& D = builder.masterMatrix();
    CHECK (D.numSpeakers == 24);
    CHECK (D.order == 3);

    const auto bp = xoa::rt::makeBusParams (0, 3, 0, numIn, 0.0, 1u);
    const auto rs = xoa::rt::makeRotationState (0.0, 0.0, 0.0, 1u);
    spatcore::rt::RtSnapshot<xoa::rt::RotationRtState> rotSnap; rotSnap.publish (rs);
    spatcore::rt::RtSnapshot<xoa::rt::BusRtParams>      busSnap; busSnap.publish (bp);

    xoa::AmbiBusAlgorithm algo;
    algo.prepare (numIn, numOut, 48000.0, n, &builder, &rotSnap, &busSnap, true);

    juce::AudioBuffer<float> input (numIn, n);
    fillInput (input, numIn, n);
    juce::AudioBuffer<float> output;
    runOneBlock (algo, input, numIn, numOut, output, n);

    CHECK (worstDecodeError (output, input, numIn, nullptr, D, bp, 1.0, n) < 1.0e-4);
}

//==============================================================================
// B3 - steady rotation: a first-published orientation snaps in (no crossfade),
// so the block equals decode(R . bus).
void testSteadyRotation()
{
    const int numIn = 16, numOut = 24, n = 64;

    xoa::DecoderMatrixBuilder builder;
    builder.rebuild (makeRing (24, 2.0), dec::DesignOptions {});
    builder.publish();
    const auto& D = builder.masterMatrix();

    const auto bp = xoa::rt::makeBusParams (0, 3, 0, numIn, 0.0, 1u);
    spatcore::rt::RtSnapshot<xoa::rt::RotationRtState> rotSnap;
    rotSnap.publish (xoa::rt::makeRotationState (30.0, 20.0, 10.0, 1u));
    spatcore::rt::RtSnapshot<xoa::rt::BusRtParams> busSnap; busSnap.publish (bp);

    xoa::AmbiBusAlgorithm algo;
    algo.prepare (numIn, numOut, 48000.0, n, &builder, &rotSnap, &busSnap, true);

    juce::AudioBuffer<float> input (numIn, n);
    fillInput (input, numIn, n);
    juce::AudioBuffer<float> output;
    runOneBlock (algo, input, numIn, numOut, output, n);

    xoa::rot::RotationMatrix R; xoa::rot::buildFromYawPitchRoll (30.0, 20.0, 10.0, R);
    CHECK (worstDecodeError (output, input, numIn, &R, D, bp, 1.0, n) < 1.0e-4);
}

//==============================================================================
// B4 - transition block: after identity is established, publishing a new
// orientation makes the block a one-block linear crossfade of the old and new
// decodes (the documented ramp contract).
void testTransitionCrossfade()
{
    const int numIn = 16, numOut = 24, n = 128;

    xoa::DecoderMatrixBuilder builder;
    builder.rebuild (makeRing (24, 2.0), dec::DesignOptions {});
    builder.publish();
    const auto& D = builder.masterMatrix();

    const auto bp = xoa::rt::makeBusParams (0, 3, 0, numIn, 0.0, 1u);
    spatcore::rt::RtSnapshot<xoa::rt::RotationRtState> rotSnap;
    rotSnap.publish (xoa::rt::makeRotationState (0.0, 0.0, 0.0, 1u));   // identity, epoch 1
    spatcore::rt::RtSnapshot<xoa::rt::BusRtParams> busSnap; busSnap.publish (bp);

    xoa::AmbiBusAlgorithm algo;
    algo.prepare (numIn, numOut, 48000.0, n, &builder, &rotSnap, &busSnap, true);

    juce::AudioBuffer<float> input (numIn, n);
    fillInput (input, numIn, n);
    juce::AudioBuffer<float> output;

    runOneBlock (algo, input, numIn, numOut, output, n);          // block 1: establishes identity
    rotSnap.publish (xoa::rt::makeRotationState (40.0, -15.0, 25.0, 2u));  // new orientation
    runOneBlock (algo, input, numIn, numOut, output, n);          // block 2: crossfade

    xoa::rot::RotationMatrix Rnew; xoa::rot::buildFromYawPitchRoll (40.0, -15.0, 25.0, Rnew);
    const int K = xoa::sh::numChannels (D.order);

    double worst = 0.0;
    for (int i = 0; i < n; ++i)
    {
        double busVec[xoa::kNumSHChannels] = { 0.0 };
        for (int c = 0; c < xoa::kNumSHChannels; ++c)
        {
            const int src = bp.srcChannel[c];
            busVec[c] = (src >= 0 && src < numIn)
                          ? (double) input.getSample (src, i) * (double) bp.gain[c] : 0.0;
        }
        double rotNew[xoa::kNumSHChannels];
        xoa::rot::apply (Rnew, xoa::kAmbisonicOrder, busVec, rotNew);   // old = identity = busVec

        const double gNew = (double) i / (double) n;   // JUCE ramp: sample i -> i/n
        const double gOld = 1.0 - gNew;
        for (int s = 0; s < D.numSpeakers; ++s)
        {
            double oldS = 0.0, newS = 0.0;
            for (int c = 0; c < K; ++c)
            {
                oldS += D.at (s, c) * busVec[c];
                newS += D.at (s, c) * rotNew[c];
            }
            const double expected = oldS * gOld + newS * gNew;
            worst = std::max (worst, std::abs ((double) output.getSample (s, i) - expected));
        }
    }
    // Slightly looser than the steady tests: the algorithm ramps the bus then
    // decodes, the reference decodes then ramps (equal by linearity, not bit).
    CHECK (worst < 1.0e-3);

    // The very next block is pure steady-state at the new orientation.
    runOneBlock (algo, input, numIn, numOut, output, n);
    CHECK (worstDecodeError (output, input, numIn, &Rnew, D, bp, 1.0, n) < 1.0e-4);
}

//==============================================================================
// B5 - runtime order adaptation: order-1 content leaves bus channels >= 4
// silent (zero-pad up); a runtime republish to order 3 changes the decode.
void testOrderAdaptRuntime()
{
    const int numOut = 24, n = 64;

    xoa::DecoderMatrixBuilder builder;
    builder.rebuild (makeRing (24, 2.0), dec::DesignOptions {});
    builder.publish();
    const auto& D = builder.masterMatrix();

    // Order-1 content: only 4 input channels.
    const auto bp1 = xoa::rt::makeBusParams (0, 1, 0, 4, 0.0, 1u);
    CHECK (bp1.srcChannel[3] == 3);
    CHECK (bp1.srcChannel[4] == -1);   // zero-padded above order 1

    spatcore::rt::RtSnapshot<xoa::rt::RotationRtState> rotSnap;
    rotSnap.publish (xoa::rt::makeRotationState (0.0, 0.0, 0.0, 1u));
    spatcore::rt::RtSnapshot<xoa::rt::BusRtParams> busSnap; busSnap.publish (bp1);

    xoa::AmbiBusAlgorithm algo;
    algo.prepare (16, numOut, 48000.0, n, &builder, &rotSnap, &busSnap, true);

    juce::AudioBuffer<float> input (16, n);
    fillInput (input, 16, n);   // 16 channels available, but only 4 declared usable
    juce::AudioBuffer<float> output;

    runOneBlock (algo, input, 4, numOut, output, n);
    CHECK (worstDecodeError (output, input, 4, nullptr, D, bp1, 1.0, n) < 1.0e-4);

    // Republish as order-3 content, 16 channels: the decode must change.
    const auto bp3 = xoa::rt::makeBusParams (0, 3, 0, 16, 0.0, 2u);
    busSnap.publish (bp3);
    runOneBlock (algo, input, 16, numOut, output, n);
    CHECK (worstDecodeError (output, input, 16, nullptr, D, bp3, 1.0, n) < 1.0e-4);
}

//==============================================================================
// B6 - master gain: a steady -6.0206 dB (0.5 linear) scales the decode by 0.5.
void testMasterGain()
{
    const int numIn = 16, numOut = 24, n = 64;

    xoa::DecoderMatrixBuilder builder;
    builder.rebuild (makeRing (24, 2.0), dec::DesignOptions {});
    builder.publish();
    const auto& D = builder.masterMatrix();

    const auto bp = xoa::rt::makeBusParams (0, 3, 0, numIn, -6.0205999133, 1u);
    CHECK (std::abs (bp.masterGainLinear - 0.5f) < 1.0e-6f);

    spatcore::rt::RtSnapshot<xoa::rt::RotationRtState> rotSnap;
    rotSnap.publish (xoa::rt::makeRotationState (0.0, 0.0, 0.0, 1u));
    spatcore::rt::RtSnapshot<xoa::rt::BusRtParams> busSnap; busSnap.publish (bp);

    xoa::AmbiBusAlgorithm algo;
    algo.prepare (numIn, numOut, 48000.0, n, &builder, &rotSnap, &busSnap, true);

    juce::AudioBuffer<float> input (numIn, n);
    fillInput (input, numIn, n);
    juce::AudioBuffer<float> output;

    runOneBlock (algo, input, numIn, numOut, output, n);   // block 1: gain ramps 1.0 -> 0.5
    runOneBlock (algo, input, numIn, numOut, output, n);   // block 2: steady 0.5
    CHECK (worstDecodeError (output, input, numIn, nullptr, D, bp, 0.5, n) < 1.0e-4);
}

//==============================================================================
// B7 - safety guards: no crash, correct silence/bypass under missing publishes.
void testSafetyGuards()
{
    const int numIn = 16, numOut = 24, n = 64;
    juce::AudioBuffer<float> input (numIn, n);
    fillInput (input, numIn, n);
    juce::AudioBuffer<float> output;

    const auto bp = xoa::rt::makeBusParams (0, 3, 0, numIn, 0.0, 1u);
    const auto rs = xoa::rt::makeRotationState (0.0, 0.0, 0.0, 1u);

    // (a) decoder never published -> null matrix -> silence.
    {
        xoa::DecoderMatrixBuilder builder;   // no rebuild/publish
        spatcore::rt::RtSnapshot<xoa::rt::RotationRtState> rotSnap; rotSnap.publish (rs);
        spatcore::rt::RtSnapshot<xoa::rt::BusRtParams>      busSnap; busSnap.publish (bp);
        xoa::AmbiBusAlgorithm algo;
        algo.prepare (numIn, numOut, 48000.0, n, &builder, &rotSnap, &busSnap, true);
        runOneBlock (algo, input, numIn, numOut, output, n);
        CHECK (output.getMagnitude (0, 0, n) == 0.0f);
        CHECK (algo.getOutputPeakLevel (0) == 0.0f);
    }

    // (b) bus params never published (epoch 0) -> silence even with a decoder.
    {
        xoa::DecoderMatrixBuilder builder;
        builder.rebuild (makeRing (24, 2.0), dec::DesignOptions {}); builder.publish();
        spatcore::rt::RtSnapshot<xoa::rt::RotationRtState> rotSnap; rotSnap.publish (rs);
        spatcore::rt::RtSnapshot<xoa::rt::BusRtParams>      busSnap;   // never published
        xoa::AmbiBusAlgorithm algo;
        algo.prepare (numIn, numOut, 48000.0, n, &builder, &rotSnap, &busSnap, true);
        runOneBlock (algo, input, numIn, numOut, output, n);
        CHECK (output.getMagnitude (0, 0, n) == 0.0f);
    }

    // (c) rotation never published (epoch 0) -> unrotated decode, not silence.
    {
        xoa::DecoderMatrixBuilder builder;
        builder.rebuild (makeRing (24, 2.0), dec::DesignOptions {}); builder.publish();
        const auto& D = builder.masterMatrix();
        spatcore::rt::RtSnapshot<xoa::rt::RotationRtState> rotSnap;   // never published
        spatcore::rt::RtSnapshot<xoa::rt::BusRtParams>      busSnap; busSnap.publish (bp);
        xoa::AmbiBusAlgorithm algo;
        algo.prepare (numIn, numOut, 48000.0, n, &builder, &rotSnap, &busSnap, true);
        runOneBlock (algo, input, numIn, numOut, output, n);
        CHECK (output.getMagnitude (0, 0, n) > 0.0f);
        CHECK (worstDecodeError (output, input, numIn, nullptr, D, bp, 1.0, n) < 1.0e-4);
    }

    // (d) disabled -> silence.
    {
        xoa::DecoderMatrixBuilder builder;
        builder.rebuild (makeRing (24, 2.0), dec::DesignOptions {}); builder.publish();
        spatcore::rt::RtSnapshot<xoa::rt::RotationRtState> rotSnap; rotSnap.publish (rs);
        spatcore::rt::RtSnapshot<xoa::rt::BusRtParams>      busSnap; busSnap.publish (bp);
        xoa::AmbiBusAlgorithm algo;
        algo.prepare (numIn, numOut, 48000.0, n, &builder, &rotSnap, &busSnap, false);
        runOneBlock (algo, input, numIn, numOut, output, n);
        CHECK (output.getMagnitude (0, 0, n) == 0.0f);
    }
}

//==============================================================================
// B8 - metering: rendered channels report a nonzero peak, channels beyond the
// speaker count report zero.
void testMeters()
{
    const int numIn = 16, numOut = 24, n = 64;

    xoa::DecoderMatrixBuilder builder;
    builder.rebuild (makeRing (24, 2.0), dec::DesignOptions {}); builder.publish();

    const auto bp = xoa::rt::makeBusParams (0, 3, 0, numIn, 0.0, 1u);
    spatcore::rt::RtSnapshot<xoa::rt::RotationRtState> rotSnap;
    rotSnap.publish (xoa::rt::makeRotationState (0.0, 0.0, 0.0, 1u));
    spatcore::rt::RtSnapshot<xoa::rt::BusRtParams> busSnap; busSnap.publish (bp);

    xoa::AmbiBusAlgorithm algo;
    algo.prepare (numIn, numOut, 48000.0, n, &builder, &rotSnap, &busSnap, true);

    juce::AudioBuffer<float> input (numIn, n);
    fillInput (input, numIn, n);
    juce::AudioBuffer<float> output;
    runOneBlock (algo, input, numIn, numOut, output, n);

    bool anyNonzero = false;
    for (int s = 0; s < 24; ++s)
        if (algo.getOutputPeakLevel (s) > 0.0f)
            anyNonzero = true;
    CHECK (anyNonzero);
    CHECK (algo.getOutputPeakLevel (24) == 0.0f);    // beyond the 24 rendered rows
    CHECK (algo.getOutputPeakLevel (200) == 0.0f);
    // The reported peak matches the buffer magnitude for a rendered channel.
    CHECK (std::abs (algo.getOutputPeakLevel (0) - output.getMagnitude (0, 0, n)) < 1.0e-6f);
}

//==============================================================================
// B9 - mid-stream decoder hot-swap: republishing a different weighting changes
// the decode on the next block.
void testDecoderSwap()
{
    const int numIn = 16, numOut = 24, n = 64;
    const auto layout = makeRing (24, 2.0);

    xoa::DecoderMatrixBuilder builder;
    dec::DesignOptions maxReOpts; maxReOpts.weighting = dec::Weighting::maxRe;
    builder.rebuild (layout, maxReOpts);
    builder.publish();
    const dec::DecoderMatrix Dmaxre = builder.masterMatrix();   // copy before the swap

    const auto bp = xoa::rt::makeBusParams (0, 3, 0, numIn, 0.0, 1u);
    spatcore::rt::RtSnapshot<xoa::rt::RotationRtState> rotSnap;
    rotSnap.publish (xoa::rt::makeRotationState (0.0, 0.0, 0.0, 1u));
    spatcore::rt::RtSnapshot<xoa::rt::BusRtParams> busSnap; busSnap.publish (bp);

    xoa::AmbiBusAlgorithm algo;
    algo.prepare (numIn, numOut, 48000.0, n, &builder, &rotSnap, &busSnap, true);

    juce::AudioBuffer<float> input (numIn, n);
    fillInput (input, numIn, n);
    juce::AudioBuffer<float> output;

    runOneBlock (algo, input, numIn, numOut, output, n);
    CHECK (worstDecodeError (output, input, numIn, nullptr, Dmaxre, bp, 1.0, n) < 1.0e-4);

    // Swap to basic weighting; the matrices genuinely differ.
    dec::DesignOptions basicOpts; basicOpts.weighting = dec::Weighting::basic;
    builder.rebuild (layout, basicOpts);
    builder.publish();
    const dec::DecoderMatrix& Dbasic = builder.masterMatrix();

    double maxDelta = 0.0;
    for (int s = 0; s < Dbasic.numSpeakers; ++s)
        for (int c = 0; c < xoa::sh::numChannels (Dbasic.order); ++c)
            maxDelta = std::max (maxDelta, std::abs (Dbasic.at (s, c) - Dmaxre.at (s, c)));
    CHECK (maxDelta > 1.0e-6);   // the swap is meaningful

    runOneBlock (algo, input, numIn, numOut, output, n);
    CHECK (worstDecodeError (output, input, numIn, nullptr, Dbasic, bp, 1.0, n) < 1.0e-4);
}

//==============================================================================
// B11 - non-trivial gather through the RT chain. The SN3D scenarios above all
// yield an identity gather (srcChannel[c]==c, gain[c]==1), so they cannot tell a
// correct gather from one that ignores srcChannel[] or drops the gain multiply.
// FuMa is a genuine channel permutation with a non-unit W gain, and N3D has
// non-unit per-degree gains, so a broken gather here diverges from the double
// reference (which honours bp.srcChannel/bp.gain).
void testNonTrivialGather()
{
    const int numOut = 24, n = 64;

    xoa::DecoderMatrixBuilder builder;
    builder.rebuild (makeRing (24, 2.0), dec::DesignOptions {});
    builder.publish();
    const auto& D = builder.masterMatrix();

    spatcore::rt::RtSnapshot<xoa::rt::RotationRtState> rotSnap;
    rotSnap.publish (xoa::rt::makeRotationState (0.0, 0.0, 0.0, 1u));
    spatcore::rt::RtSnapshot<xoa::rt::BusRtParams> busSnap;

    xoa::AmbiBusAlgorithm algo;

    // FuMa order-1 content (4 channels, WXYZ): the gather is a real permutation
    // (srcChannel[0]=0, [1]=2, [2]=3, [3]=1) with gain[0]=sqrt2. Distinct
    // per-channel input makes the permutation observable.
    {
        const auto bp = xoa::rt::makeBusParams (0, 1, 2 /*FuMa*/, 4, 0.0, 1u);
        CHECK (bp.srcChannel[1] != 1);                   // genuine remap, not identity
        CHECK (bp.gain[0] != 1.0f);                      // W carries the sqrt2 scale
        busSnap.publish (bp);

        algo.prepare (4, numOut, 48000.0, n, &builder, &rotSnap, &busSnap, true);
        juce::AudioBuffer<float> input (4, n);
        fillInput (input, 4, n);
        juce::AudioBuffer<float> output;
        runOneBlock (algo, input, 4, numOut, output, n);
        CHECK (worstDecodeError (output, input, 4, nullptr, D, bp, 1.0, n) < 1.0e-4);
    }

    // N3D order-2 content (9 channels): identity remap but non-unit per-degree
    // gains (1/sqrt3 at l=1, 1/sqrt5 at l=2), so a dropped gain multiply shows.
    {
        const auto bp = xoa::rt::makeBusParams (0, 2, 1 /*N3D*/, 9, 0.0, 2u);
        CHECK (bp.gain[1] != 1.0f);
        busSnap.publish (bp);

        algo.prepare (9, numOut, 48000.0, n, &builder, &rotSnap, &busSnap, true);
        juce::AudioBuffer<float> input (9, n);
        fillInput (input, 9, n);
        juce::AudioBuffer<float> output;
        runOneBlock (algo, input, 9, numOut, output, n);
        CHECK (worstDecodeError (output, input, 9, nullptr, D, bp, 1.0, n) < 1.0e-4);
    }
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

    testNullDecode();
    testSteadyRotation();
    testTransitionCrossfade();
    testOrderAdaptRuntime();
    testMasterGain();
    testSafetyGuards();
    testMeters();
    testDecoderSwap();
    testNonTrivialGather();
}
