/*
    XoaTestSignalTests.cpp - WP7 C8: the output test-signal generator (FR-21).

    Covers the deterministic-seed guarantee, replace-semantics, the active gate,
    and the SpeakerId stepping (sequencing + energy exclusivity + gap).
*/

#include "XoaTestFramework.h"

#include "Audio/TestSignalGenerator.h"

#include <cstring>

namespace
{

using SignalType = xoa::TestSignalGenerator::SignalType;

//==============================================================================
// A fixed seed makes pink noise reproducible: two freshly-prepared generators
// driven identically must produce bit-identical output.
void testTestSignalDeterminism()
{
    constexpr int n = 512;

    xoa::TestSignalGenerator a, b;
    for (auto* g : { &a, &b })
    {
        g->prepare (48000.0, n);
        g->setSignalType (SignalType::PinkNoise);
        g->setLevel (0.0f);
        g->setOutputChannel (0);
    }

    juce::AudioBuffer<float> ba (1, n), bb (1, n);
    ba.clear(); bb.clear();
    a.renderNextBlock (ba, 0, n);
    b.renderNextBlock (bb, 0, n);

    CHECK (std::memcmp (ba.getReadPointer (0), bb.getReadPointer (0), sizeof (float) * (size_t) n) == 0);
    CHECK (ba.getMagnitude (0, 0, n) > 0.0f);   // and it is a real (non-silent) signal
}

//==============================================================================
// The signal REPLACES its target channel and leaves the others untouched.
void testTestSignalReplaceSemantics()
{
    constexpr int n = 512;

    xoa::TestSignalGenerator gen;
    gen.prepare (48000.0, n);
    gen.setSignalType (SignalType::Tone);
    gen.setFrequency (1000.0f);
    gen.setLevel (0.0f);
    gen.setOutputChannel (1);

    juce::AudioBuffer<float> buf (3, n);
    for (int c = 0; c < 3; ++c)
        for (int i = 0; i < n; ++i)
            buf.setSample (c, i, 1.0f);   // stand-in "decoded" content

    gen.renderNextBlock (buf, 0, n);

    // Untouched neighbours.
    bool ch0 = true, ch2 = true;
    for (int i = 0; i < n; ++i)
    {
        if (buf.getSample (0, i) != 1.0f) ch0 = false;
        if (buf.getSample (2, i) != 1.0f) ch2 = false;
    }
    CHECK (ch0);
    CHECK (ch2);

    // Target replaced (sin(0)=0 at the head; a live tone by mid-block).
    CHECK (buf.getSample (1, 0) == 0.0f);
    CHECK (buf.getMagnitude (1, n / 4, n / 2) > 0.0f);
}

//==============================================================================
void testTestSignalActiveGate()
{
    xoa::TestSignalGenerator gen;
    gen.prepare (48000.0, 256);

    gen.setSignalType (SignalType::Off);
    CHECK (! gen.isActive());

    gen.setSignalType (SignalType::Tone);
    gen.setOutputChannel (-1);
    CHECK (! gen.isActive());               // a real signal but no target
    gen.setOutputChannel (0);
    CHECK (gen.isActive());

    gen.setSignalType (SignalType::SpeakerId);
    CHECK (gen.isActive());                 // steps every output; no target needed
}

//==============================================================================
// SpeakerId steps a burst across each output in turn: at any instant exactly
// one channel carries energy, matching getCurrentSpeakerIndex(); the gap is
// silent on every channel.
void testSpeakerIdSequencing()
{
    constexpr double sr = 48000.0;
    constexpr int    numOut = 4;

    xoa::TestSignalGenerator gen;
    gen.prepare (sr, 8192);
    gen.setLevel (0.0f);
    gen.setSignalType (SignalType::SpeakerId);

    auto advance = [&] (double seconds)
    {
        juce::AudioBuffer<float> scratch (numOut, (int) (seconds * sr));
        scratch.clear();
        gen.renderNextBlock (scratch, 0, scratch.getNumSamples());
    };

    // Probe a short window and assert exactly `expected` carries energy.
    auto probeExclusive = [&] (int expected)
    {
        juce::AudioBuffer<float> probe (numOut, 2400);   // 50 ms, inside one burst
        probe.clear();
        gen.renderNextBlock (probe, 0, 2400);

        CHECK (gen.getCurrentSpeakerIndex() == expected);
        for (int c = 0; c < numOut; ++c)
        {
            const bool hot = probe.getMagnitude (c, 0, 2400) > 0.0f;
            CHECK (hot == (c == expected));
        }
    };

    advance (0.30);  probeExclusive (0);   // ~0.30 s: speaker 0 burst
    advance (0.95);  probeExclusive (1);   // ~1.30 s: speaker 1 burst
    advance (0.95);  probeExclusive (2);   // ~2.30 s: speaker 2 burst

    // Into speaker 2's gap (withinSlot [0.75, 1.0)): silent everywhere, index cleared.
    advance (0.45);                        // ~2.80 s
    juce::AudioBuffer<float> gap (numOut, 1200);
    gap.clear();
    gen.renderNextBlock (gap, 0, 1200);
    CHECK (gen.getCurrentSpeakerIndex() == -1);
    for (int c = 0; c < numOut; ++c)
        CHECK (gap.getMagnitude (c, 0, 1200) == 0.0f);
}

} // namespace

//==============================================================================
void runXoaTestSignalTests()
{
    testTestSignalDeterminism();
    testTestSignalReplaceSemantics();
    testTestSignalActiveGate();
    testSpeakerIdSequencing();
}
