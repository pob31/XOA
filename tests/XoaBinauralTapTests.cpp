/*
    XoaBinauralTapTests.cpp - WP15 (D52): the monitor tap inside
    AmbiBusAlgorithm, and the routing POD that carries the headphone pair.

    The load-bearing claim of D52 is that binaural monitoring cannot affect
    what the loudspeakers hear. That is asserted here the only way worth
    asserting it: run the SAME input through the bus algorithm twice, once
    with a live monitor attached and once without, and require the speaker
    output to be BIT-identical — not close, identical.
*/

#include "XoaTestFramework.h"

#include "XoaConstants.h"
#include "Audio/PatchRouting.h"
#include "DSP/AmbiBinauralDecoder.h"
#include "DSP/AmbiBinauralFilterBank.h"
#include "DSP/AmbiBinauralRenderer.h"
#include "DSP/AmbiBusAlgorithm.h"
#include "DSP/AmbiRtTypes.h"
#include "DSP/DecoderMatrixBuilder.h"
#include "Parameters/XoaParameterIDs.h"
#include "Parameters/XoaValueTreeState.h"

#include "spatcore/rt/RtSnapshot.h"

#include <atomic>
#include <cmath>
#include <vector>

namespace
{
using xoa::XoaValueTreeState;
namespace ids = xoa::ids;

constexpr int kBlock = 64;
constexpr int kNumSpeakers = 8;

/** A non-trivial monitor bank so the tap really does work while the speaker
    comparison runs — a silent monitor would prove nothing. */
xoa::binaural::BinauralDesignResult makeTapBank()
{
    juce::Random rng (4242);
    xoa::binaural::BinauralDesignResult design;
    design.firLength = 96;
    design.sampleRate = 48000.0;
    design.firs.assign ((size_t) design.firLength * 2 * (size_t) xoa::kNumSHChannels, 0.0f);
    for (int ear = 0; ear < 2; ++ear)
        for (int c : { 0, 1, 2, 3, 15 })
            for (int n = 0; n < design.firLength; ++n)
                design.fir (ear, c)[n] = (float) (rng.nextDouble() * 0.5 - 0.25);
    return design;
}

/** Render `blocks` blocks of the same deterministic HOA input through a
    freshly built algorithm, optionally with a monitor attached, and return
    the concatenated speaker output. */
std::vector<float> renderSpeakers (bool withMonitor, std::vector<float>* monitorLeft = nullptr)
{
    XoaValueTreeState store;
    store.setNumSpeakers (kNumSpeakers);

    xoa::DecoderMatrixBuilder builder;
    builder.rebuild (store);
    builder.publish();

    spatcore::rt::RtSnapshot<xoa::rt::RotationRtState> rotSnap;
    spatcore::rt::RtSnapshot<xoa::rt::BusRtParams> busSnap;
    // order 10 content on all 121 channels, 0 dB master.
    busSnap.publish (xoa::rt::makeBusParams (xoa::kAmbisonicOrder, xoa::kAmbisonicOrder,
                                             0, xoa::kNumSHChannels, 0.0, 1u));

    xoa::AmbiBinauralFilterBank bank;
    spatcore::rt::RtSnapshot<xoa::rt::MonitorRtParams> monitorSnap;
    std::atomic<spatcore::binaural::HeadOrientationSource*> source { nullptr };
    xoa::AmbiBinauralRenderer renderer;

    if (withMonitor)
    {
        CHECK (bank.cook (makeTapBank(), kBlock));
        bank.publish();
        renderer.prepare (48000.0, kBlock, &bank, &monitorSnap, &source);

        xoa::rt::MonitorRtParams p;
        p.enabled = true;
        p.monitorGainLinear = 1.0f;
        p.epoch = 1;
        monitorSnap.publish (p);
    }

    xoa::AmbiBusAlgorithm algo;
    algo.prepare (xoa::kNumSHChannels, kNumSpeakers, 48000.0, kBlock,
                  &builder, &rotSnap, &busSnap, true,
                  nullptr, nullptr, nullptr,
                  withMonitor ? &renderer : nullptr);

    // Deterministic HOA content on the low-order channels.
    juce::AudioBuffer<float> hoa (xoa::kNumSHChannels, kBlock);
    juce::AudioBuffer<float> out (kNumSpeakers, kBlock);

    constexpr int kBlocks = 4;
    std::vector<float> speakers;
    speakers.reserve ((size_t) kBlocks * kNumSpeakers * kBlock);

    for (int b = 0; b < kBlocks; ++b)
    {
        hoa.clear();
        for (int c = 0; c < 16; ++c)
        {
            float* d = hoa.getWritePointer (c);
            for (int i = 0; i < kBlock; ++i)
                d[i] = 0.25f * std::sin (0.017f * (float) (b * kBlock + i + 1)
                                         * (float) (c + 1));
        }

        out.clear();
        juce::AudioSourceChannelInfo info (&out, 0, kBlock);
        algo.processBlock (info, hoa, xoa::kNumSHChannels, kNumSpeakers, nullptr, 0);

        for (int s = 0; s < kNumSpeakers; ++s)
            for (int i = 0; i < kBlock; ++i)
                speakers.push_back (out.getReadPointer (s)[i]);

        if (withMonitor && monitorLeft != nullptr)
            for (int i = 0; i < kBlock; ++i)
                monitorLeft->push_back (renderer.getOutputLeft()[i]);
    }

    return speakers;
}
} // namespace

//==============================================================================
/** D52: an active monitor leaves the loudspeaker render BIT-identical. */
static void testMonitorTapLeavesSpeakersBitIdentical()
{
    std::vector<float> monitorOut;
    const auto without = renderSpeakers (false);
    const auto with    = renderSpeakers (true, &monitorOut);

    CHECK (without.size() == with.size());
    if (without.size() != with.size())
        return;

    // Both paths must actually carry signal, or "identical" would just be
    // two silences agreeing with each other.
    double monitorEnergy = 0.0, speakerEnergy = 0.0;
    for (float v : monitorOut)
        monitorEnergy += (double) v * v;
    for (float v : without)
        speakerEnergy += (double) v * v;
    CHECK (monitorEnergy > 0.0);
    CHECK (speakerEnergy > 0.0);

    size_t differing = 0;
    for (size_t i = 0; i < with.size(); ++i)
        if (with[i] != without[i])   // deliberate exact comparison
            ++differing;

    if (differing != 0)
        std::fprintf (stderr, "  [binaural tap] %zu of %zu speaker samples differ\n",
                      differing, with.size());
    CHECK (differing == 0);
}

//==============================================================================
/** The routing POD derives the headphone pair from the Monitoring section,
    and refuses a first channel that would put the pair's second half past
    the addressing ceiling. */
static void testBinauralPairRouting()
{
    XoaValueTreeState store;

    // Default: no pair reserved.
    auto patch = xoa::rt::composePatchRouting (store, 1u);
    CHECK (patch.binauralHwLeft == -1);
    CHECK (patch.binauralHwRight == -1);

    store.setParameter (ids::binauralOutputChannel, 10);
    patch = xoa::rt::composePatchRouting (store, 2u);
    CHECK (patch.binauralHwLeft == 10);
    CHECK (patch.binauralHwRight == 11);

    // Explicitly disabled again.
    store.setParameter (ids::binauralOutputChannel, -1);
    patch = xoa::rt::composePatchRouting (store, 3u);
    CHECK (patch.binauralHwLeft == -1);

    // The clamp keeps the pair inside the hardware ceiling: the bounds table
    // stops the first channel one short of the last addressable output.
    store.setParameter (ids::binauralOutputChannel, xoa::kMaxHardwareChannels + 50);
    patch = xoa::rt::composePatchRouting (store, 4u);
    CHECK (patch.binauralHwLeft >= 0);
    CHECK (patch.binauralHwRight < xoa::kMaxHardwareChannels);
}

//==============================================================================
/** A bus algorithm that bails out early (no decoder published) must leave
    the monitor reporting "idle", or the engine would scatter a stale block
    onto the headphone pair. */
static void testMonitorIdleWhenBusBailsOut()
{
    XoaValueTreeState store;
    store.setNumSpeakers (kNumSpeakers);

    xoa::DecoderMatrixBuilder builder;   // deliberately never rebuilt/published

    spatcore::rt::RtSnapshot<xoa::rt::RotationRtState> rotSnap;
    spatcore::rt::RtSnapshot<xoa::rt::BusRtParams> busSnap;
    // order 10 content on all 121 channels, 0 dB master.
    busSnap.publish (xoa::rt::makeBusParams (xoa::kAmbisonicOrder, xoa::kAmbisonicOrder,
                                             0, xoa::kNumSHChannels, 0.0, 1u));

    xoa::AmbiBinauralFilterBank bank;
    CHECK (bank.cook (makeTapBank(), kBlock));
    bank.publish();

    spatcore::rt::RtSnapshot<xoa::rt::MonitorRtParams> monitorSnap;
    std::atomic<spatcore::binaural::HeadOrientationSource*> source { nullptr };
    xoa::AmbiBinauralRenderer renderer;
    renderer.prepare (48000.0, kBlock, &bank, &monitorSnap, &source);

    xoa::rt::MonitorRtParams p;
    p.enabled = true;
    p.monitorGainLinear = 1.0f;
    p.epoch = 1;
    monitorSnap.publish (p);

    xoa::AmbiBusAlgorithm algo;
    algo.prepare (xoa::kNumSHChannels, kNumSpeakers, 48000.0, kBlock,
                  &builder, &rotSnap, &busSnap, true, nullptr, nullptr, nullptr, &renderer);

    juce::AudioBuffer<float> hoa (xoa::kNumSHChannels, kBlock);
    hoa.clear();
    for (int i = 0; i < kBlock; ++i)
        hoa.setSample (0, i, 0.5f);

    juce::AudioBuffer<float> out (kNumSpeakers, kBlock);
    juce::AudioSourceChannelInfo info (&out, 0, kBlock);
    algo.processBlock (info, hoa, xoa::kNumSHChannels, kNumSpeakers, nullptr, 0);

    CHECK (! renderer.isActive());
}

//==============================================================================
void runXoaBinauralTapTests()
{
    testMonitorTapLeavesSpeakersBitIdentical();
    testBinauralPairRouting();
    testMonitorIdleWhenBusBailsOut();
}
