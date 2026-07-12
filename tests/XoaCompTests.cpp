/*
    XoaCompTests.cpp - WP7 C7: per-speaker compensation.

    C7a covers the pure message-side composers (distance delay/gain laws,
    mute/solo folding, EQ readout). The RT SpeakerCompProcessor tests (C7b) are
    appended to this file.
*/

#include "XoaTestFramework.h"

#include "XoaConstants.h"
#include "Audio/SpeakerCompParams.h"
#include "Parameters/XoaParameterIDs.h"
#include "Parameters/XoaValueTreeState.h"

#include <cmath>

namespace
{

// A 4-speaker layout on the +x axis at radii 1, 2, 3, 4 m (the farthest is #3).
// XoaValueTreeState is non-copyable, so populate a caller-owned store.
void populateRadii (xoa::XoaValueTreeState& store)
{
    store.setNumSpeakers (4);
    for (int s = 0; s < 4; ++s)
    {
        store.setParameter (xoa::ids::speakerPositionX, (double) (s + 1), s);
        store.setParameter (xoa::ids::speakerPositionY, 0.0, s);
        store.setParameter (xoa::ids::speakerPositionZ, 0.0, s);
    }
}

//==============================================================================
void testCompComposerDistanceModes()
{
    xoa::XoaValueTreeState store;
    populateRadii (store);

    // Mode 0 (off): trim only - no delay, unity gain.
    store.setParameter (xoa::ids::distanceCompMode, 0);
    const auto p0 = xoa::composeSpeakerCompParams (store, 7u);
    CHECK (p0.numSpeakers == 4);
    CHECK (p0.epoch == 7u);
    for (int s = 0; s < 4; ++s)
    {
        CHECK (p0.delayMs[s] == 0.0f);
        CHECK (std::abs (p0.gainLinear[s] - 1.0f) < 1.0e-6f);
    }

    // Mode 1 (delay): align to the farthest (r=4); no distance gain.
    store.setParameter (xoa::ids::distanceCompMode, 1);
    const auto p1 = xoa::composeSpeakerCompParams (store, 8u);
    for (int s = 0; s < 4; ++s)
    {
        const double expected = (4.0 - (s + 1)) / xoa::kSpeedOfSound * 1000.0;
        CHECK (std::abs (p1.delayMs[s] - expected) < 1.0e-3);
        CHECK (std::abs (p1.gainLinear[s] - 1.0f) < 1.0e-6f);
    }
    CHECK (p1.delayMs[3] == 0.0f);   // the farthest speaker gets no added delay

    // Mode 2 (delay + gain): attenuate-only, referenced to the farthest.
    store.setParameter (xoa::ids::distanceCompMode, 2);
    const auto p2 = xoa::composeSpeakerCompParams (store, 9u);
    for (int s = 0; s < 4; ++s)
    {
        const double db = juce::jlimit (-24.0, 0.0, 20.0 * std::log10 ((double) (s + 1) / 4.0));
        CHECK (std::abs (p2.gainLinear[s] - (float) std::pow (10.0, db / 20.0)) < 1.0e-5f);
    }
    CHECK (std::abs (p2.gainLinear[3] - 1.0f) < 1.0e-6f);   // farthest -> 0 dB
    CHECK (p2.gainLinear[0] < p2.gainLinear[1]);            // nearer -> more attenuation
}

//==============================================================================
void testCompComposerTrimMuteSolo()
{
    xoa::XoaValueTreeState store;
    populateRadii (store);
    store.setParameter (xoa::ids::distanceCompMode, 0);

    // Trim: speaker 1 at -6 dB.
    store.setParameter (xoa::ids::speakerGain, -6.0, 1);
    auto p = xoa::composeSpeakerCompParams (store, 1u);
    CHECK (std::abs (p.gainLinear[1] - (float) std::pow (10.0, -6.0 / 20.0)) < 1.0e-5f);

    // Mute speaker 0 -> silenced; others audible.
    store.setParameter (xoa::ids::speakerMute, true, 0);
    p = xoa::composeSpeakerCompParams (store, 2u);
    CHECK (p.gainLinear[0] == 0.0f);
    CHECK (p.gainLinear[2] > 0.0f);

    // Solo speaker 2 -> only #2 audible (others silenced, #0 stays muted).
    store.setParameter (xoa::ids::speakerSolo, true, 2);
    p = xoa::composeSpeakerCompParams (store, 3u);
    CHECK (p.gainLinear[2] > 0.0f);
    CHECK (p.gainLinear[1] == 0.0f);
    CHECK (p.gainLinear[3] == 0.0f);
    CHECK (p.gainLinear[0] == 0.0f);

    // Mute wins over solo.
    store.setParameter (xoa::ids::speakerMute, true, 2);
    p = xoa::composeSpeakerCompParams (store, 4u);
    CHECK (p.gainLinear[2] == 0.0f);
}

//==============================================================================
void testEqComposer()
{
    xoa::XoaValueTreeState store;
    store.setNumSpeakers (2);
    store.setParameter (xoa::ids::speakerEqEnabled, true, 0);
    store.setEqBandParameter (0, 1, xoa::ids::eqShape, 3);          // peak
    store.setEqBandParameter (0, 1, xoa::ids::eqFrequency, 1200.0);
    store.setEqBandParameter (0, 1, xoa::ids::eqGain, 6.0);
    store.setEqBandParameter (0, 1, xoa::ids::eqQ, 2.0);

    const auto p = xoa::composeSpeakerEqParams (store);
    CHECK (p.numSpeakers == 2);
    CHECK (p.enabled[0]);
    CHECK (! p.enabled[1]);
    CHECK (p.bands[0][1].shape == 3);
    CHECK (std::abs (p.bands[0][1].freq - 1200.0f) < 1.0e-3f);
    CHECK (std::abs (p.bands[0][1].gainDb - 6.0f) < 1.0e-5f);
    CHECK (std::abs (p.bands[0][1].q - 2.0f) < 1.0e-5f);
    CHECK (p.bands[0][0].shape == 0);   // untouched band -> OFF
}

} // namespace

//==============================================================================
void runXoaCompTests()
{
    testCompComposerDistanceModes();
    testCompComposerTrimMuteSolo();
    testEqComposer();
}
