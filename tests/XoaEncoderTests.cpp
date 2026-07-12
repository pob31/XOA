/*
    XoaEncoderTests.cpp - WP8 C4: the RT encoder stage in AmbiBusAlgorithm.

    Drives the real bus (encode -> rotate(bypassed) -> decode) against a double
    reference decode. Covers:
      - null/steady-state: output == decode . (encode . stem)
      - neutrality: numSources==0 (or null seams) is bit-identical to no encoder
      - click-free ramp-in on activation, ramp-out on deactivation
      - NFC DC gain path: each order lane settles to (r_ref/r_src)^l (ties C1)
      - stems-fewer-than-sources guard (no crash / no NaN)
*/

#include "XoaTestFramework.h"

#include "XoaConstants.h"
#include "DSP/AmbiBusAlgorithm.h"
#include "DSP/AmbiEncoder.h"
#include "DSP/AmbiNFCFilter.h"
#include "DSP/AmbiSphericalHarmonics.h"
#include "DSP/DecoderMatrixBuilder.h"
#include "Helpers/XoaCoordinates.h"

#include "spatcore/rt/RtSnapshot.h"

#include <cmath>
#include <vector>

namespace
{
xoa::decoder::SpeakerLayout makeRing (int count, double radius)
{
    xoa::decoder::SpeakerLayout layout;
    layout.count = count;
    for (int s = 0; s < count; ++s)
        layout.positions[s] = xoa::coords::sphericalToCartesian (
            { radius, xoa::coords::normalizeAzimuthDegrees (360.0 * s / count), 0.0 });
    return layout;
}

void fillStem (juce::AudioBuffer<float>& b, int ch, int n)
{
    float* d = b.getWritePointer (ch);
    for (int i = 0; i < n; ++i)
        d[i] = 0.3f * std::sin (0.05f * (float) (i + 1)) + 0.1f;
}

// double reference: output[s] = sum_{c < K} D.at(s,c) * busVec[c].
double decodeRef (const xoa::decoder::DecoderMatrix& D, const double* busVec, int s)
{
    const int K = xoa::sh::numChannels (D.order);
    double acc = 0.0;
    for (int c = 0; c < K; ++c)
        acc += D.at (s, c) * busVec[c];
    return acc;
}

// Shared rig + silent-HOA harness for the encoder tests.
struct Harness
{
    static constexpr int numOut = 24;
    xoa::DecoderMatrixBuilder builder;
    spatcore::rt::RtSnapshot<xoa::rt::RotationRtState> rotSnap;   // unpublished -> unrotated
    spatcore::rt::RtSnapshot<xoa::rt::BusRtParams>     busSnap;
    spatcore::rt::RtSnapshot<xoa::rt::EncoderRtParams> encSnap;
    std::vector<float>  encMatrix;
    std::vector<double> nfcPages;

    Harness()
        : encMatrix ((size_t) xoa::kMaxInputs * xoa::kNumSHChannels, 0.0f),
          nfcPages  ((size_t) xoa::kMaxInputs * xoa::nfc::kCoeffsPerSource, 0.0)
    {
        builder.rebuild (makeRing (24, 2.0), xoa::decoder::DesignOptions {});
        builder.publish();
        // silent HOA: numFileChannels 0 -> gather clears busA; epoch 1 passes the guard.
        busSnap.publish (xoa::rt::makeBusParams (0, 3, 0, 0, 0.0, 1u));
    }

    const xoa::decoder::DecoderMatrix& D() const { return builder.masterMatrix(); }
};

//==============================================================================
void testEncoderNullDecode()
{
    Harness h;
    const int n = 128;

    // one source on the ring radius (distance gain 1), front, NFC off.
    xoa::enc::SourceParams sp; sp.x = 2.0;
    xoa::enc::composeRow (sp, 2.0, h.encMatrix.data());   // row for source 0
    h.encSnap.publish ({ 1, 0, 2.0f, 1u });

    xoa::AmbiBusAlgorithm algo;
    algo.prepare (xoa::kMaxInputs, Harness::numOut, 48000.0, n, &h.builder, &h.rotSnap, &h.busSnap,
                  true, h.encMatrix.data(), h.nfcPages.data(), &h.encSnap);

    juce::AudioBuffer<float> hoa (1, n); hoa.clear();
    juce::AudioBuffer<float> stems (4, n); stems.clear(); fillStem (stems, 0, n);
    juce::AudioBuffer<float> out (Harness::numOut, n);

    auto runBlock = [&]
    {
        out.clear();
        juce::AudioSourceChannelInfo info (&out, 0, n);
        algo.processBlock (info, hoa, 0, Harness::numOut, &stems, 4);
    };

    runBlock();   // block 1: coeff ramps 0 -> row
    runBlock();   // block 2: steady

    const float* stem = stems.getReadPointer (0);
    double worst = 0.0;
    for (int i = 0; i < n; ++i)
    {
        double busVec[xoa::kNumSHChannels];
        for (int c = 0; c < xoa::kNumSHChannels; ++c)
            busVec[c] = (double) h.encMatrix[(size_t) c] * (double) stem[i];
        for (int s = 0; s < Harness::numOut; ++s)
            worst = std::max (worst, std::abs ((double) out.getSample (s, i) - decodeRef (h.D(), busVec, s)));
    }
    CHECK (worst < 1.0e-4);
}

//==============================================================================
void testEncoderNeutrality()
{
    const int numIn = 16, numOut = 24, n = 128;

    xoa::DecoderMatrixBuilder builder;
    builder.rebuild (makeRing (24, 2.0), xoa::decoder::DesignOptions {});
    builder.publish();

    const auto bp = xoa::rt::makeBusParams (0, 3, 0, numIn, 0.0, 1u);
    spatcore::rt::RtSnapshot<xoa::rt::RotationRtState> rotSnap; rotSnap.publish (xoa::rt::makeRotationState (0,0,0,1u));
    spatcore::rt::RtSnapshot<xoa::rt::BusRtParams>     busSnap; busSnap.publish (bp);

    juce::AudioBuffer<float> hoa (numIn, n);
    for (int c = 0; c < numIn; ++c)
    {
        float* d = hoa.getWritePointer (c);
        for (int i = 0; i < n; ++i) d[i] = 0.05f * (float) (c + 1) * std::sin (0.09f * (float) (i + c));
    }

    // Reference: NO encoder seams, 4-arg processBlock.
    juce::AudioBuffer<float> refOut (numOut, n);
    {
        xoa::AmbiBusAlgorithm algo;
        algo.prepare (numIn, numOut, 48000.0, n, &builder, &rotSnap, &busSnap, true);
        refOut.clear();
        juce::AudioSourceChannelInfo info (&refOut, 0, n);
        algo.processBlock (info, hoa, numIn, numOut);
    }

    // Test: encoder seams present but numSources == 0, with a NONZERO stems buffer.
    std::vector<float> encMatrix ((size_t) xoa::kMaxInputs * xoa::kNumSHChannels, 0.5f);
    std::vector<double> nfcPages ((size_t) xoa::kMaxInputs * xoa::nfc::kCoeffsPerSource, 0.0);
    spatcore::rt::RtSnapshot<xoa::rt::EncoderRtParams> encSnap; encSnap.publish ({ 0, 0, 2.0f, 1u });

    juce::AudioBuffer<float> testOut (numOut, n);
    {
        xoa::AmbiBusAlgorithm algo;
        algo.prepare (numIn, numOut, 48000.0, n, &builder, &rotSnap, &busSnap, true,
                      encMatrix.data(), nfcPages.data(), &encSnap);
        juce::AudioBuffer<float> stems (4, n);
        for (int c = 0; c < 4; ++c) fillStem (stems, c, n);
        testOut.clear();
        juce::AudioSourceChannelInfo info (&testOut, 0, n);
        algo.processBlock (info, hoa, numIn, numOut, &stems, 4);
    }

    // numSources==0 must add nothing: bit-identical to the no-encoder path.
    for (int s = 0; s < numOut; ++s)
        for (int i = 0; i < n; ++i)
            CHECK (testOut.getSample (s, i) == refOut.getSample (s, i));
}

//==============================================================================
void testEncoderRampInOut()
{
    Harness h;
    const int n = 256;
    xoa::enc::SourceParams sp; sp.x = 2.0;
    xoa::enc::composeRow (sp, 2.0, h.encMatrix.data());
    h.encSnap.publish ({ 1, 0, 2.0f, 1u });

    xoa::AmbiBusAlgorithm algo;
    algo.prepare (xoa::kMaxInputs, Harness::numOut, 48000.0, n, &h.builder, &h.rotSnap, &h.busSnap,
                  true, h.encMatrix.data(), h.nfcPages.data(), &h.encSnap);

    juce::AudioBuffer<float> hoa (1, n); hoa.clear();
    juce::AudioBuffer<float> stems (4, n); stems.clear();
    for (int i = 0; i < n; ++i) stems.setSample (0, i, 1.0f);   // constant stem
    juce::AudioBuffer<float> out (Harness::numOut, n);
    auto runBlock = [&] { out.clear(); juce::AudioSourceChannelInfo info (&out, 0, n);
                          algo.processBlock (info, hoa, 0, Harness::numOut, &stems, 4); };

    // Block 1: ramp-in. First sample uses ramp gain 0 -> output ~ 0; grows.
    runBlock();
    double firstMag = 0.0, lastMag = 0.0;
    for (int s = 0; s < Harness::numOut; ++s)
    {
        firstMag = std::max (firstMag, std::abs ((double) out.getSample (s, 0)));
        lastMag  = std::max (lastMag,  std::abs ((double) out.getSample (s, n - 1)));
    }
    CHECK (firstMag < 1.0e-6);          // no instantaneous jump on activation
    CHECK (lastMag  > 1.0e-3);          // ramped up by end of block

    // Block 2: steady.
    runBlock();
    double steady[Harness::numOut];
    double steadyMax = 0.0;
    for (int s = 0; s < Harness::numOut; ++s)
    {
        steady[s] = out.getSample (s, n - 1);
        steadyMax = std::max (steadyMax, std::abs (steady[s]));
    }

    // Deactivate: numSources 0, keep the stem. The block ramps the contribution
    // OUT using the still-present audio (starts ~steady, ends near 0 - JUCE's
    // linear ramp reaches ~steady/n at the last sample, not exactly 0).
    h.encSnap.publish ({ 0, 0, 2.0f, 2u });
    runBlock();
    double outStart = 0.0, outEnd = 0.0;
    for (int s = 0; s < Harness::numOut; ++s)
    {
        outStart = std::max (outStart, std::abs ((double) out.getSample (s, 0) - steady[s]));
        outEnd   = std::max (outEnd,   std::abs ((double) out.getSample (s, n - 1)));
        for (int i = 0; i < n; ++i) CHECK (std::isfinite (out.getSample (s, i)));
    }
    CHECK (outStart < 1.0e-3);              // starts at the previous steady value
    CHECK (outEnd   < steadyMax * 0.02);    // faded ~to silence (residual ~ steady/n)

    // Next block: fully removed (appliedCoeff now 0 -> the stage skips it).
    runBlock();
    double residual = 0.0;
    for (int s = 0; s < Harness::numOut; ++s)
        for (int i = 0; i < n; ++i) residual = std::max (residual, std::abs ((double) out.getSample (s, i)));
    CHECK (residual < 1.0e-6);
}

//==============================================================================
void testEncoderNfcDcGain()
{
    Harness h;
    const int n = 256;

    // source at radius 1 (distance gain 2), NFC on. r_ref 2 -> per-order DC
    // gain (2/1)^l = 2^l (clamp inactive for orders <= 3).
    xoa::enc::SourceParams sp; sp.x = 1.0;
    xoa::enc::composeRow (sp, 2.0, h.encMatrix.data());
    xoa::nfc::designSourceSections (1.0, 2.0, 48000.0, h.nfcPages.data());
    h.encSnap.publish ({ 1, (juce::uint64) 1, 2.0f, 1u });   // nfcMask bit 0 set

    xoa::AmbiBusAlgorithm algo;
    algo.prepare (xoa::kMaxInputs, Harness::numOut, 48000.0, n, &h.builder, &h.rotSnap, &h.busSnap,
                  true, h.encMatrix.data(), h.nfcPages.data(), &h.encSnap);

    juce::AudioBuffer<float> hoa (1, n); hoa.clear();
    juce::AudioBuffer<float> stems (4, n); stems.clear();
    for (int i = 0; i < n; ++i) stems.setSample (0, i, 1.0f);   // DC input
    juce::AudioBuffer<float> out (Harness::numOut, n);

    for (int b = 0; b < 200; ++b)   // settle the NFC transient (slowest pole ~ order 1)
    {
        out.clear();
        juce::AudioSourceChannelInfo info (&out, 0, n);
        algo.processBlock (info, hoa, 0, Harness::numOut, &stems, 4);
    }

    // steady bus[c] = row[c] * (r_ref/r_src)^order(c) = row[c] * 2^order(c).
    double busVec[xoa::kNumSHChannels];
    for (int c = 0; c < xoa::kNumSHChannels; ++c)
        busVec[c] = (double) h.encMatrix[(size_t) c] * std::pow (2.0, (double) xoa::sh::acnToOrder (c));

    double worst = 0.0;
    for (int s = 0; s < Harness::numOut; ++s)
        worst = std::max (worst, std::abs ((double) out.getSample (s, n - 1) - decodeRef (h.D(), busVec, s)));
    CHECK (worst < 2.0e-3);
}

//==============================================================================
void testStemsFewerThanSources()
{
    Harness h;
    const int n = 64;
    for (int i = 0; i < 4; ++i)
    {
        xoa::enc::SourceParams sp; sp.x = 2.0; sp.y = 0.3 * i;
        xoa::enc::composeRow (sp, 2.0, h.encMatrix.data() + (size_t) i * xoa::kNumSHChannels);
    }
    h.encSnap.publish ({ 4, 0, 2.0f, 1u });   // 4 sources requested

    xoa::AmbiBusAlgorithm algo;
    algo.prepare (xoa::kMaxInputs, Harness::numOut, 48000.0, n, &h.builder, &h.rotSnap, &h.busSnap,
                  true, h.encMatrix.data(), h.nfcPages.data(), &h.encSnap);

    juce::AudioBuffer<float> hoa (1, n); hoa.clear();
    juce::AudioBuffer<float> stems (2, n); stems.clear();   // only 2 stem channels
    fillStem (stems, 0, n); fillStem (stems, 1, n);
    juce::AudioBuffer<float> out (Harness::numOut, n);
    out.clear();
    juce::AudioSourceChannelInfo info (&out, 0, n);
    algo.processBlock (info, hoa, 0, Harness::numOut, &stems, 2);   // numStems < numSources

    for (int s = 0; s < Harness::numOut; ++s)
        for (int i = 0; i < n; ++i)
            CHECK (std::isfinite (out.getSample (s, i)));   // no OOB read / NaN
}

} // namespace

//==============================================================================
void runXoaEncoderTests()
{
    testEncoderNullDecode();
    testEncoderNeutrality();
    testEncoderRampInOut();
    testEncoderNfcDcGain();
    testStemsFewerThanSources();
}
