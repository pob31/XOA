/*
    XoaPatchTests.cpp - stage 2: per-input stem formats (D44/D45), the
    AudioPatch schema and its reconcile (D43), the routing POD the audio
    thread uses, and the HOA group merge in the bus algorithm.
*/

#include "XoaTestFramework.h"

#include "XoaConstants.h"
#include "Audio/PatchRouting.h"
#include "DSP/AmbiBusAlgorithm.h"
#include "DSP/AmbiNFCFilter.h"
#include "DSP/AmbiOrderWeights.h"
#include "DSP/AmbiSphericalHarmonics.h"
#include "DSP/DecoderMatrixBuilder.h"
#include "Helpers/XoaCoordinates.h"
#include "Parameters/XoaParameterIDs.h"
#include "Parameters/XoaValueTreeState.h"

#include "spatcore/rt/RtSnapshot.h"

#include <cmath>
#include <vector>

namespace
{

using xoa::XoaValueTreeState;
namespace ids = xoa::ids;

// double reference: output[s] = sum_{c < K} D.at(s,c) * busVec[c].
double decodeRef (const xoa::decoder::DecoderMatrix& D, const double* busVec, int s)
{
    const int K = xoa::sh::numChannels (D.order);
    double acc = 0.0;
    for (int c = 0; c < K; ++c)
        acc += D.at (s, c) * busVec[c];
    return acc;
}

//==============================================================================
// Spans: mono is one channel, an HOA group is (order+1)^2, and the offsets are
// the running sum — the flattened addressing every other stage indexes by.
void testStemSpans()
{
    CHECK (XoaValueTreeState::channelCountForFormat (0) == 1);
    CHECK (XoaValueTreeState::channelCountForFormat (1) == 4);
    CHECK (XoaValueTreeState::channelCountForFormat (2) == 9);
    CHECK (XoaValueTreeState::channelCountForFormat (10) == 121);

    XoaValueTreeState store;
    store.setNumInputs (4);
    CHECK (store.getTotalStemChannels() == 4);          // all mono
    CHECK (store.getStemChannelOffset (2) == 2);

    store.setParameter (ids::inputFormat, 1, 0);        // input 0 -> FOA
    CHECK (store.getInputChannelCount (0) == 4);
    CHECK (store.getStemChannelOffset (0) == 0);
    CHECK (store.getStemChannelOffset (1) == 4);        // input 1 starts after the group
    CHECK (store.getTotalStemChannels() == 7);          // 4 + 1 + 1 + 1

    store.setParameter (ids::inputFormat, 0, 0);        // back to mono
    CHECK (store.getTotalStemChannels() == 4);
}

//==============================================================================
// The kMaxStemChannels ceiling holds by stepping formats down, last HOA input
// first. Two order-10 groups (121 each) cannot both fit in 128.
void testStemSpanCeiling()
{
    XoaValueTreeState store;
    store.setNumInputs (2);

    store.setParameter (ids::inputFormat, 10, 0);
    CHECK (store.getTotalStemChannels() <= xoa::kMaxStemChannels);
    CHECK (store.getInputFormat (0) == 10);             // 121 + 1 fits

    store.setParameter (ids::inputFormat, 10, 1);       // 121 + 121 does not
    CHECK (store.getTotalStemChannels() <= xoa::kMaxStemChannels);
    CHECK (store.getInputFormat (0) == 10);             // the earlier input keeps its order
    CHECK (store.getInputFormat (1) < 10);              // the later one was stepped down
}

//==============================================================================
// A fresh project patches identity, and the routing POD reproduces it.
void testDefaultPatchIsIdentity()
{
    XoaValueTreeState store;
    store.setNumInputs (4);
    store.setNumSpeakers (6);

    const auto patch = xoa::rt::composePatchRouting (store, 1);

    CHECK (patch.numInputs == 4);
    CHECK (patch.numStemChannels == 4);
    for (int k = 0; k < 4; ++k)
    {
        CHECK (patch.hwForStemChannel[k] == k);
        CHECK (patch.stemOffset[k] == k);
        CHECK (patch.stemSpan[k] == 1);
    }
    for (int s = 0; s < 6; ++s)
        CHECK (patch.hwForSpeaker[s] == s);
}

//==============================================================================
// An explicit patchData string round-trips into the routing maps, including a
// NON-CONTIGUOUS assignment (the case the whole hardware-indexing design
// exists for) and an unpatched row.
void testPatchDataRoundTrip()
{
    XoaValueTreeState store;
    store.setNumSpeakers (3);

    auto tree = store.getOutputPatchTree();
    CHECK (tree.isValid());

    // speaker 0 -> hw 5, speaker 1 -> hw 2, speaker 2 -> unpatched
    juce::StringArray rows;
    rows.add ("0,0,0,0,0,1");
    rows.add ("0,0,1,0,0,0");
    rows.add ("0,0,0,0,0,0");
    tree.setProperty (ids::patchData, rows.joinIntoString (";"), nullptr);

    const auto patch = xoa::rt::composePatchRouting (store, 2);
    CHECK (patch.hwForSpeaker[0] == 5);
    CHECK (patch.hwForSpeaker[1] == 2);
    CHECK (patch.hwForSpeaker[2] == -1);
    CHECK (patch.epoch == 2);
}

//==============================================================================
// A format change re-spans the input patch rows: every other input keeps its
// hardware channel, and the row count follows the new total.
void testReconcilePreservesOtherInputs()
{
    XoaValueTreeState store;
    store.setNumInputs (4);

    auto tree = store.getInputPatchTree();
    CHECK (tree.isValid());
    CHECK ((int) tree.getProperty (ids::rows) == 4);

    // Park inputs 1..3 on distinctive hardware channels, input 0 on hw 0.
    juce::StringArray rows;
    rows.add ("1,0,0,0,0,0,0,0,0,0");   // input 0 -> hw 0
    rows.add ("0,0,0,0,0,0,0,1,0,0");   // input 1 -> hw 7
    rows.add ("0,0,0,0,0,0,0,0,1,0");   // input 2 -> hw 8
    rows.add ("0,0,0,0,0,0,0,0,0,1");   // input 3 -> hw 9
    tree.setProperty (ids::patchData, rows.joinIntoString (";"), nullptr);
    store.reconcileAudioPatch();        // sync the span cache to this layout

    store.setParameter (ids::inputFormat, 1, 0);   // input 0 -> FOA (4 rows)

    CHECK (store.getTotalStemChannels() == 7);
    CHECK ((int) store.getInputPatchTree().getProperty (ids::rows) == 7);

    const auto patch = xoa::rt::composePatchRouting (store, 3);
    CHECK (patch.numStemChannels == 7);
    CHECK (patch.stemSpan[0] == 4);
    CHECK (patch.stemOffset[1] == 4);

    // Input 0's first component keeps its channel; the inputs after it keep
    // theirs, now addressed through their shifted flattened rows.
    CHECK (patch.hwForStemChannel[0] == 0);
    CHECK (patch.hwForStemChannel[4] == 7);
    CHECK (patch.hwForStemChannel[5] == 8);
    CHECK (patch.hwForStemChannel[6] == 9);
}

//==============================================================================
// Growing the speaker count extends the output patch with the identity
// continuation, so a project that never opens the patch window keeps behaving
// exactly as it did before patching existed.
void testSpeakerGrowthKeepsIdentity()
{
    XoaValueTreeState store;
    store.setNumSpeakers (4);
    store.setNumSpeakers (8);

    const auto patch = xoa::rt::composePatchRouting (store, 4);
    for (int s = 0; s < 8; ++s)
        CHECK (patch.hwForSpeaker[s] == s);
}

//==============================================================================
// The HOA merge, against a double reference decode (the XoaEncoderTests
// idiom): an order-1 group's channel c lands on BUS channel c at its
// order-adapt x gain factor, and contributes nothing above its own order.
void testHoaGroupMergeIntoBus()
{
    constexpr int numOut = 24;
    constexpr int n = 64;

    xoa::decoder::SpeakerLayout layout;
    layout.count = numOut;
    for (int s = 0; s < numOut; ++s)
        layout.positions[s] = xoa::coords::sphericalToCartesian (
            { 2.0, xoa::coords::normalizeAzimuthDegrees (360.0 * s / numOut), 0.0 });

    xoa::DecoderMatrixBuilder builder;
    builder.rebuild (layout, xoa::decoder::DesignOptions {});
    builder.publish();

    spatcore::rt::RtSnapshot<xoa::rt::RotationRtState> rotSnap;   // unpublished -> unrotated
    spatcore::rt::RtSnapshot<xoa::rt::BusRtParams> busSnap;
    busSnap.publish (xoa::rt::makeBusParams (0, 3, 0, 0, 0.0, 1u));   // silent HOA gather

    // Input 0 is an order-1 group. Its liveMatrix row carries the order-adapt
    // gains times a 0.5 linear input gain — exactly what composeHoaRow builds.
    std::vector<float> encMatrix ((size_t) xoa::kMaxInputs * xoa::kNumSHChannels, 0.0f);
    std::vector<double> nfcPages ((size_t) xoa::kMaxInputs * xoa::nfc::kCoeffsPerSource, 0.0);
    double adapt[xoa::kNumSHChannels];
    xoa::weights::orderAdaptGains (1, xoa::kAmbisonicOrder, adapt);
    constexpr float groupGain = 0.5f;
    for (int c = 0; c < xoa::kNumSHChannels; ++c)
        encMatrix[(size_t) c] = (float) adapt[c] * groupGain;

    spatcore::rt::RtSnapshot<xoa::rt::EncoderRtParams> encSnap;
    xoa::rt::EncoderRtParams enc = xoa::rt::makeMonoEncoderParams (1, 0, 2.0f, 1u);
    enc.stemOrder[0]  = 1;   // FOA: stem rows 0..3
    enc.stemOffset[0] = 0;
    encSnap.publish (enc);

    xoa::AmbiBusAlgorithm algo;
    algo.prepare (xoa::kMaxInputs, numOut, 48000.0, n, &builder, &rotSnap, &busSnap,
                  true, encMatrix.data(), nfcPages.data(), &encSnap);

    // Four distinct component signals so a mis-mapped channel is visible.
    juce::AudioBuffer<float> stems (4, n);
    for (int c = 0; c < 4; ++c)
    {
        float* d = stems.getWritePointer (c);
        for (int i = 0; i < n; ++i)
            d[i] = 0.2f * std::sin (0.03f * (float) (i + 1) * (float) (c + 1));
    }

    juce::AudioBuffer<float> hoa (1, n); hoa.clear();
    juce::AudioBuffer<float> out (numOut, n);

    auto runBlock = [&]
    {
        out.clear();
        juce::AudioSourceChannelInfo info (&out, 0, n);
        algo.processBlock (info, hoa, 0, numOut, &stems, 4);
    };
    runBlock();   // block 1: coefficients ramp in
    runBlock();   // block 2: steady

    // Reference: bus channel c = stem row c * adapt[c] * gain for c < 4, and
    // zero above the group's order. Then decode through the same matrix.
    const auto& D = builder.masterMatrix();
    double worst = 0.0;
    for (int i = 0; i < n; ++i)
    {
        double busVec[xoa::kNumSHChannels] = {};
        for (int c = 0; c < 4; ++c)
            busVec[c] = (double) stems.getReadPointer (c)[i] * adapt[c] * (double) groupGain;

        for (int s = 0; s < numOut; ++s)
            worst = juce::jmax (worst, std::abs (decodeRef (D, busVec, s)
                                                 - (double) out.getSample (s, i)));
    }
    CHECK (worst < 1.0e-5);

    // And the group really is silent above its own order: zeroing the four
    // component rows must silence the output entirely.
    stems.clear();
    runBlock();
    for (int s = 0; s < numOut; ++s)
        CHECK (out.getMagnitude (s, 0, n) < 1.0e-6f);
}

} // namespace

//==============================================================================
void runXoaPatchTests()
{
    testStemSpans();
    testStemSpanCeiling();
    testDefaultPatchIsIdentity();
    testPatchDataRoundTrip();
    testReconcilePreservesOtherInputs();
    testSpeakerGrowthKeepsIdentity();
    testHoaGroupMergeIntoBus();
}
