/*
    XoaCalcEngineTests.cpp - WP8 C3: control-side mono-encoder calculation engine.

    Headless (no device). Drives AmbiCalculationEngine::tick() directly and
    checks: row composition matches the pure composer, mute zeros a row, edits
    recompose the touched row, the gate/count/mask drive the published
    EncoderRtParams, NFC pages match designSourceSections, the speed limiter is
    in the position path, and submitTrackedPosition smooths / rejects jumps.
*/

#include "XoaTestFramework.h"

#include "XoaConstants.h"
#include "DSP/AmbiCalculationEngine.h"
#include "DSP/AmbiEncoder.h"
#include "DSP/AmbiNFCFilter.h"
#include "Parameters/XoaParameterIDs.h"
#include "Parameters/XoaValueTreeState.h"

#include <cmath>
#include <vector>

namespace
{
template <typename T>
double maxAbsDiff (const T* a, const T* b, int n)
{
    double m = 0.0;
    for (int i = 0; i < n; ++i) m = std::max (m, (double) std::abs (a[i] - b[i]));
    return m;
}

//==============================================================================
void testRowMatchesComposer()
{
    xoa::XoaValueTreeState store;
    xoa::AmbiCalculationEngine calc (store);

    store.setParameter (xoa::ids::inputGain, -6.0, 2);
    store.setParameter (xoa::ids::inputPositionX, 3.0, 2);
    store.setParameter (xoa::ids::inputPositionY, 1.0, 2);
    store.setParameter (xoa::ids::inputPositionZ, -0.5, 2);
    store.setParameter (xoa::ids::inputSpread, 30.0, 2);
    calc.tick();

    xoa::enc::SourceParams sp;
    sp.x = 3.0; sp.y = 1.0; sp.z = -0.5; sp.gainDb = -6.0; sp.spreadDeg = 30.0;
    float expected[xoa::kNumSHChannels];
    xoa::enc::composeRow (sp, calc.getReferenceRadius(), expected);

    const float* row = calc.encodeMatrix() + 2 * xoa::kNumSHChannels;
    CHECK (maxAbsDiff (row, expected, xoa::kNumSHChannels) < 1.0e-6);

    // A different, untouched row (0) stays at its own default composition.
    xoa::enc::SourceParams sp0;   // default (1,0,0), 0 dB, 0 spread
    float expected0[xoa::kNumSHChannels];
    xoa::enc::composeRow (sp0, calc.getReferenceRadius(), expected0);
    CHECK (maxAbsDiff (calc.encodeMatrix(), expected0, xoa::kNumSHChannels) < 1.0e-6);
}

//==============================================================================
void testMuteZerosRow()
{
    xoa::XoaValueTreeState store;
    xoa::AmbiCalculationEngine calc (store);

    store.setParameter (xoa::ids::inputMute, true, 3);
    calc.tick();

    const float* row = calc.encodeMatrix() + 3 * xoa::kNumSHChannels;
    for (int c = 0; c < xoa::kNumSHChannels; ++c)
        CHECK (row[c] == 0.0f);
}

//==============================================================================
void testEditRecomposesOnlyTouchedRow()
{
    xoa::XoaValueTreeState store;
    xoa::AmbiCalculationEngine calc (store);
    calc.tick();

    // snapshot row 0 and row 5 before editing row 5
    std::vector<float> row0Before (calc.encodeMatrix(), calc.encodeMatrix() + xoa::kNumSHChannels);

    store.setParameter (xoa::ids::inputGain, 6.0, 5);
    calc.tick();

    // row 0 unchanged, row 5 changed
    CHECK (maxAbsDiff (calc.encodeMatrix(), row0Before.data(), xoa::kNumSHChannels) == 0.0);

    xoa::enc::SourceParams sp5; sp5.gainDb = 6.0;   // default position, spread 0
    float expected5[xoa::kNumSHChannels];
    xoa::enc::composeRow (sp5, calc.getReferenceRadius(), expected5);
    CHECK (maxAbsDiff (calc.encodeMatrix() + 5 * xoa::kNumSHChannels, expected5, xoa::kNumSHChannels) < 1.0e-6);
}

//==============================================================================
void testGateAndCountDriveSnapshot()
{
    xoa::XoaValueTreeState store;
    xoa::AmbiCalculationEngine calc (store);

    // default: mono inputs disabled -> numSources 0 (stage off).
    const auto p0 = calc.encoderSource().acquire();
    CHECK (p0.numSources == 0);

    // enable -> numSources == input count, epoch advances.
    store.setParameter (xoa::ids::monoInputsEnabled, true);
    const auto p1 = calc.encoderSource().acquire();
    CHECK (p1.numSources == store.getNumInputs());
    CHECK (p1.epoch > p0.epoch);

    // grow the input count -> numSources tracks it after a tick.
    store.setNumInputs (store.getNumInputs() + 3);
    calc.tick();
    const auto p2 = calc.encoderSource().acquire();
    CHECK (p2.numSources == store.getNumInputs());

    // disable -> back to 0.
    store.setParameter (xoa::ids::monoInputsEnabled, false);
    const auto p3 = calc.encoderSource().acquire();
    CHECK (p3.numSources == 0);
}

//==============================================================================
void testNfcMaskAndPages()
{
    xoa::XoaValueTreeState store;
    xoa::AmbiCalculationEngine calc (store);

    store.setParameter (xoa::ids::inputNfcEnabled, true, 1);
    store.setParameter (xoa::ids::inputNfcEnabled, true, 4);
    calc.tick();

    const auto p = calc.encoderSource().acquire();
    CHECK (p.nfcEnabled (1));
    CHECK (p.nfcEnabled (4));
    CHECK (! p.nfcEnabled (0));
    CHECK (! p.nfcEnabled (2));

    // NFC pages are always designed (the mask only gates RT use). Input 0 is at
    // the default (1,0,0) -> radius 1 m; its page must match designSourceSections.
    double expected[xoa::nfc::kCoeffsPerSource];
    xoa::nfc::designSourceSections (1.0, calc.getReferenceRadius(), calc.getSampleRate(), expected);
    CHECK (maxAbsDiff (calc.nfcCoeffs(), expected, xoa::nfc::kCoeffsPerSource) < 1.0e-6);

    // Moving a source rewrites its page for the new radius.
    store.setParameter (xoa::ids::inputPositionX, 0.5, 2);
    calc.tick();
    double expected2[xoa::nfc::kCoeffsPerSource];
    xoa::nfc::designSourceSections (0.5, calc.getReferenceRadius(), calc.getSampleRate(), expected2);
    CHECK (maxAbsDiff (calc.nfcCoeffs() + 2 * xoa::nfc::kCoeffsPerSource, expected2,
                       xoa::nfc::kCoeffsPerSource) < 1.0e-6);
}

//==============================================================================
void testReferenceRadiusAndSampleRate()
{
    xoa::XoaValueTreeState store;
    xoa::AmbiCalculationEngine calc (store);

    calc.setSampleRate (96000.0);
    calc.setReferenceRadius (3.0);
    calc.tick();

    CHECK (calc.getSampleRate() == 96000.0);
    CHECK (std::abs (calc.getReferenceRadius() - 3.0) < 1e-12);

    // page for input 0 (radius 1) must now use the new SR + r_ref.
    double expected[xoa::nfc::kCoeffsPerSource];
    xoa::nfc::designSourceSections (1.0, 3.0, 96000.0, expected);
    CHECK (maxAbsDiff (calc.nfcCoeffs(), expected, xoa::nfc::kCoeffsPerSource) < 1.0e-6);

    // r_ref also changed the distance gain, so the row reflects it.
    xoa::enc::SourceParams sp0;   // (1,0,0)
    float expectedRow[xoa::kNumSHChannels];
    xoa::enc::composeRow (sp0, 3.0, expectedRow);
    CHECK (maxAbsDiff (calc.encodeMatrix(), expectedRow, xoa::kNumSHChannels) < 1.0e-6);
}

//==============================================================================
void testSpeedLimiterInPath()
{
    xoa::XoaValueTreeState store;
    xoa::AmbiCalculationEngine calc (store);
    calc.tick();   // settle input 0 at its default (1,0,0)

    store.setParameter (xoa::ids::inputMaxSpeed, 2.0, 0);   // 2 m/s cap
    store.setParameter (xoa::ids::inputPositionX, 5.0, 0);  // jump the TARGET to 5 m

    xoa::enc::SourceParams spFinal; spFinal.x = 5.0;
    float finalRow[xoa::kNumSHChannels];
    xoa::enc::composeRow (spFinal, calc.getReferenceRadius(), finalRow);

    calc.tick();   // one 20 ms frame: can move at most 2*0.02 = 0.04 m, not 4 m
    CHECK (maxAbsDiff (calc.encodeMatrix(), finalRow, xoa::kNumSHChannels) > 1.0e-3);

    // ~4 m at 2 m/s = 2 s = 100 ticks; give margin and confirm arrival.
    for (int t = 0; t < 300; ++t) calc.tick();
    CHECK (maxAbsDiff (calc.encodeMatrix(), finalRow, xoa::kNumSHChannels) < 1.0e-4);
}

//==============================================================================
void testTrackedPositionSmoothAndReject()
{
    xoa::XoaValueTreeState store;
    xoa::AmbiCalculationEngine calc (store);

    // first sample initializes the 1-Euro filter to the raw value -> the store
    // moves toward the fed position.
    calc.submitTrackedPosition (0, 1, 2.0f, 0.0f, 0.0f, 1.0f);
    CHECK (store.getFloatParameter (xoa::ids::inputPositionX, 0) > 1.5);

    // establish a baseline near the origin, then a single huge spike: the jump
    // detector rejects it (held), so the store does NOT leap to 100 m.
    calc.submitTrackedPosition (0, 1, 0.0f, 0.0f, 0.0f, 1.0f);
    calc.submitTrackedPosition (0, 1, 100.0f, 0.0f, 0.0f, 1.0f);
    CHECK (store.getFloatParameter (xoa::ids::inputPositionX, 0) < 50.0);
}

} // namespace

//==============================================================================
void runXoaCalcEngineTests()
{
    testRowMatchesComposer();
    testMuteZerosRow();
    testEditRecomposesOnlyTouchedRow();
    testGateAndCountDriveSnapshot();
    testNfcMaskAndPages();
    testReferenceRadiusAndSampleRate();
    testSpeedLimiterInPath();
    testTrackedPositionSmoothAndReject();
}
