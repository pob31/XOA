/*
    XoaCompTests.cpp - WP7 C7: per-speaker compensation.

    C7a covers the pure message-side composers (distance delay/gain laws,
    mute/solo folding, EQ readout). The RT SpeakerCompProcessor tests (C7b) are
    appended to this file.
*/

#include "XoaTestFramework.h"

#include "XoaConstants.h"
#include "Audio/SpeakerCompParams.h"
#include "Audio/SpeakerCompProcessor.h"
#include "Parameters/XoaParameterIDs.h"
#include "Parameters/XoaValueTreeState.h"

#include "spatcore/rt/RtSnapshot.h"

#include <cmath>
#include <cstring>
#include <vector>

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

// Pre-D18 origin-referenced compose (radius = ||speaker||), replicated verbatim
// so the listener=origin path can be proven byte-for-byte identical to it.
xoa::SpeakerCompRtParams composeOriginReferenced (const xoa::XoaValueTreeState& store,
                                                  juce::uint32 epoch)
{
    xoa::SpeakerCompRtParams p;
    const int L = juce::jmin (store.getNumSpeakers(), xoa::kMaxSpeakers);
    p.numSpeakers = L;
    p.epoch = epoch;
    const int mode = store.getIntParameter (xoa::ids::distanceCompMode);

    std::vector<double> radius ((size_t) L, 0.0);
    double rMax = 0.0;
    bool anySolo = false;
    for (int s = 0; s < L; ++s)
    {
        const double x = store.getFloatParameter (xoa::ids::speakerPositionX, s);
        const double y = store.getFloatParameter (xoa::ids::speakerPositionY, s);
        const double z = store.getFloatParameter (xoa::ids::speakerPositionZ, s);
        radius[(size_t) s] = std::sqrt (x * x + y * y + z * z);
        rMax = juce::jmax (rMax, radius[(size_t) s]);
        if (static_cast<bool> (store.getParameter (xoa::ids::speakerSolo, s)))
            anySolo = true;
    }
    for (int s = 0; s < L; ++s)
    {
        double delayMs = store.getFloatParameter (xoa::ids::speakerDelay, s);
        if (mode >= 1)
            delayMs += (rMax - radius[(size_t) s]) / xoa::kSpeedOfSound * 1000.0;
        p.delayMs[s] = (float) juce::jlimit (0.0, xoa::kMaxCompDelayMs, delayMs);

        double gainDb = store.getFloatParameter (xoa::ids::speakerGain, s);
        if (mode >= 2 && rMax > 0.0)
            gainDb += juce::jlimit (-24.0, 0.0,
                                    20.0 * std::log10 (juce::jmax (radius[(size_t) s], 0.1) / rMax));
        const bool muted  = static_cast<bool> (store.getParameter (xoa::ids::speakerMute, s));
        const bool soloed = static_cast<bool> (store.getParameter (xoa::ids::speakerSolo, s));
        const bool audible = (! anySolo || soloed) && ! muted;
        p.gainLinear[s] = audible ? (float) std::pow (10.0, gainDb / 20.0) : 0.0f;
    }
    return p;
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
// D18/FR-25 listener re-reference.
//==============================================================================

// At the default listener (0,0,0) the composed POD must be byte-for-byte the
// pre-D18 origin-referenced law - the guarantee that every existing baseline
// (offline `comp`, project round-trips) is undisturbed. Exercise the full POD
// (delay-mode 2 + a trim + a delay trim) so the memcmp is meaningful.
void testCompListenerOriginBitIdentity()
{
    xoa::XoaValueTreeState store;
    populateRadii (store);
    store.setParameter (xoa::ids::distanceCompMode, 2);
    store.setParameter (xoa::ids::speakerGain, -3.0, 1);
    store.setParameter (xoa::ids::speakerDelay, 12.0, 2);

    CHECK (store.getFloatParameter (xoa::ids::listenerX) == 0.0f);
    CHECK (store.getFloatParameter (xoa::ids::listenerY) == 0.0f);
    CHECK (store.getFloatParameter (xoa::ids::listenerZ) == 0.0f);

    const auto pNew = xoa::composeSpeakerCompParams (store, 42u);
    const auto pRef = composeOriginReferenced (store, 42u);
    CHECK (std::memcmp (&pNew, &pRef, sizeof (xoa::SpeakerCompRtParams)) == 0);
}

// An off-centre listener re-references distances to ||speaker - listener||.
// Speakers sit on +x at radii 1..4; the listener at (0,1,0) makes distances
// sqrt((s+1)^2 + 1), so speaker 3 stays farthest and the delays/gains follow
// the listener-referenced law - and visibly differ from the origin law.
void testCompListenerOffCenter()
{
    xoa::XoaValueTreeState store;
    populateRadii (store);
    store.setParameter (xoa::ids::distanceCompMode, 2);
    store.setParameter (xoa::ids::listenerX, 0.0);
    store.setParameter (xoa::ids::listenerY, 1.0);
    store.setParameter (xoa::ids::listenerZ, 0.0);

    const auto p = xoa::composeSpeakerCompParams (store, 5u);

    double dist[4], rMax = 0.0;
    for (int s = 0; s < 4; ++s)
    {
        dist[s] = std::sqrt ((double) ((s + 1) * (s + 1)) + 1.0);
        rMax = juce::jmax (rMax, dist[s]);
    }
    for (int s = 0; s < 4; ++s)
    {
        const double expDelay = (rMax - dist[s]) / xoa::kSpeedOfSound * 1000.0;
        CHECK (std::abs (p.delayMs[s] - expDelay) < 1.0e-3);
        const double db = juce::jlimit (-24.0, 0.0, 20.0 * std::log10 (dist[s] / rMax));
        CHECK (std::abs (p.gainLinear[s] - (float) std::pow (10.0, db / 20.0)) < 1.0e-5f);
    }
    CHECK (p.delayMs[3] == 0.0f);   // farthest from the listener -> no added delay

    // The re-reference actually moved things vs the origin-referenced law.
    const auto pOrigin = composeOriginReferenced (store, 5u);
    bool differs = false;
    for (int s = 0; s < 4; ++s)
        if (std::abs (p.delayMs[s] - pOrigin.delayMs[s]) > 1.0e-3f)
            differs = true;
    CHECK (differs);
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

//==============================================================================
// SpeakerCompProcessor (RT stage) tests.
//==============================================================================

// A default (all-neutral) comp POD for `n` speakers: no delay, unity gain.
xoa::SpeakerCompRtParams neutralParams (int n)
{
    xoa::SpeakerCompRtParams p;
    p.numSpeakers = n;
    p.epoch = 1u;
    for (int s = 0; s < n; ++s)
    {
        p.delayMs[s] = 0.0f;
        p.gainLinear[s] = 1.0f;
    }
    return p;
}

//==============================================================================
// The default config is a bit-exact identity, so the offline baselines stay
// undisturbed when comp is off. Feed random audio through a neutral processor
// and demand byte-for-byte equality.
void testCompNeutralPassthrough()
{
    constexpr double sr = 48000.0;
    constexpr int    numCh = 3;
    constexpr int    n = 512;

    spatcore::rt::RtSnapshot<xoa::SpeakerCompRtParams> snap;
    snap.publish (neutralParams (numCh));

    xoa::SpeakerCompProcessor comp;
    comp.prepare (sr, n, numCh, &snap);

    // Enabled EQ but every band OFF (shape 0) must still be transparent.
    xoa::SpeakerEqParams eq;
    eq.numSpeakers = numCh;
    for (int c = 0; c < numCh; ++c) eq.enabled[c] = true;   // bands default shape 0
    comp.setEqParameters (eq);

    juce::Random rng (1234);
    juce::AudioBuffer<float> buffer (numCh, n);
    juce::AudioBuffer<float> reference (numCh, n);
    for (int c = 0; c < numCh; ++c)
        for (int i = 0; i < n; ++i)
        {
            const float v = rng.nextFloat() * 2.0f - 1.0f;
            buffer.setSample (c, i, v);
            reference.setSample (c, i, v);
        }

    comp.processBlock (buffer, numCh, n);

    bool bitIdentical = true;
    for (int c = 0; c < numCh; ++c)
        if (std::memcmp (buffer.getReadPointer (c), reference.getReadPointer (c),
                         sizeof (float) * (size_t) n) != 0)
            bitIdentical = false;
    CHECK (bitIdentical);
}

//==============================================================================
// A pure delay: publish an integer-sample delay, feed a single impulse, and
// verify it re-emerges at that offset with its energy intact.
void testCompIntegerDelay()
{
    constexpr double sr = 48000.0;
    constexpr int    n = 128;
    const int        delaySamples = 10;

    xoa::SpeakerCompRtParams p = neutralParams (1);
    p.delayMs[0] = (float) (delaySamples * 1000.0 / sr);   // -> ~10 samples

    spatcore::rt::RtSnapshot<xoa::SpeakerCompRtParams> snap;
    snap.publish (p);

    xoa::SpeakerCompProcessor comp;
    comp.prepare (sr, n, 1, &snap);

    juce::AudioBuffer<float> buffer (1, n);
    buffer.clear();
    buffer.setSample (0, 0, 1.0f);   // unit impulse at t=0

    comp.processBlock (buffer, 1, n);

    // Peak lands at the delay offset; energy is preserved (unit gain).
    int   peakIdx = 0;
    float peakVal = 0.0f, energy = 0.0f;
    for (int i = 0; i < n; ++i)
    {
        const float v = buffer.getSample (0, i);
        energy += v;
        if (std::abs (v) > std::abs (peakVal)) { peakVal = v; peakIdx = i; }
    }
    CHECK (peakIdx == delaySamples);
    CHECK (peakVal > 0.999f);
    CHECK (std::abs (energy - 1.0f) < 1.0e-3f);
}

//==============================================================================
// Mute folds to gainLinear == 0; the RT gain must ramp there click-free (no
// step), and settle at silence.
void testCompMuteRamp()
{
    constexpr double sr = 48000.0;
    constexpr int    n = 2048;   // > 20 ms ramp at 48 k (960 samples)

    xoa::SpeakerCompRtParams p = neutralParams (1);
    p.gainLinear[0] = 0.0f;      // muted

    spatcore::rt::RtSnapshot<xoa::SpeakerCompRtParams> snap;
    snap.publish (p);

    xoa::SpeakerCompProcessor comp;
    comp.prepare (sr, n, 1, &snap);

    juce::AudioBuffer<float> buffer (1, n);
    for (int i = 0; i < n; ++i) buffer.setSample (0, i, 1.0f);   // DC

    comp.processBlock (buffer, 1, n);

    CHECK (buffer.getSample (0, 0) > 0.9f);            // starts near unity (no step)
    CHECK (buffer.getSample (0, n - 1) < 1.0e-4f);      // settles to silence

    bool monotoneDown = true;
    for (int i = 1; i < n; ++i)
        if (buffer.getSample (0, i) > buffer.getSample (0, i - 1) + 1.0e-6f)
            monotoneDown = false;
    CHECK (monotoneDown);
}

//==============================================================================
// A +6 dB peak EQ at its centre frequency lifts a sine by ~6 dB (0.5 dB tol).
void testCompEqPeakGain()
{
    constexpr double sr    = 48000.0;
    constexpr int    n     = 8192;
    constexpr double freq  = 1000.0;
    constexpr double gainDb = 6.0;

    spatcore::rt::RtSnapshot<xoa::SpeakerCompRtParams> snap;
    snap.publish (neutralParams (1));

    xoa::SpeakerCompProcessor comp;
    comp.prepare (sr, n, 1, &snap);

    xoa::SpeakerEqParams eq;
    eq.numSpeakers = 1;
    eq.enabled[0] = true;
    eq.bands[0][0].shape  = 3;              // peak
    eq.bands[0][0].freq   = (float) freq;
    eq.bands[0][0].gainDb = (float) gainDb;
    eq.bands[0][0].q      = 2.0f;
    comp.setEqParameters (eq);

    juce::AudioBuffer<float> buffer (1, n);
    const double w = 2.0 * juce::MathConstants<double>::pi * freq / sr;
    for (int i = 0; i < n; ++i)
        buffer.setSample (0, i, 0.5f * (float) std::sin (w * i));

    // Reference input amplitude, measured the same way (cancels sampling under-read).
    float inMax = 0.0f;
    for (int i = n / 2; i < n; ++i) inMax = juce::jmax (inMax, std::abs (buffer.getSample (0, i)));

    comp.processBlock (buffer, 1, n);

    float outMax = 0.0f;
    for (int i = n / 2; i < n; ++i) outMax = juce::jmax (outMax, std::abs (buffer.getSample (0, i)));

    const double ratioDb = 20.0 * std::log10 (outMax / inMax);
    CHECK (std::abs (ratioDb - gainDb) < 0.5);
}

//==============================================================================
// A device restart at a new sample rate must re-derive the delay in samples
// from the ms-based POD - no message-side recompose. Same ms target, two rates,
// both land the impulse at the ms-correct offset.
void testCompSampleRateIndependence()
{
    auto delayPeakAt = [] (double sr, int n)
    {
        xoa::SpeakerCompRtParams p = neutralParams (1);
        p.delayMs[0] = 1.0f;   // 1 ms

        spatcore::rt::RtSnapshot<xoa::SpeakerCompRtParams> snap;
        snap.publish (p);

        xoa::SpeakerCompProcessor comp;
        comp.prepare (sr, n, 1, &snap);

        juce::AudioBuffer<float> buffer (1, n);
        buffer.clear();
        buffer.setSample (0, 0, 1.0f);
        comp.processBlock (buffer, 1, n);

        int peakIdx = 0; float peakVal = 0.0f;
        for (int i = 0; i < n; ++i)
            if (std::abs (buffer.getSample (0, i)) > std::abs (peakVal))
            { peakVal = buffer.getSample (0, i); peakIdx = i; }
        return peakIdx;
    };

    CHECK (delayPeakAt (48000.0, 256) == 48);    // 1 ms @ 48 k
    CHECK (delayPeakAt (96000.0, 256) == 96);    // 1 ms @ 96 k
}

//==============================================================================
// Signed-zero neutrality: a -0.0f input must survive the neutral (delay-0)
// path bit-exact. Guards the readFrac integer-tap short-circuit - a naive
// buffer[i0]*1 + buffer[i1]*0 would flip -0.0f to +0.0f and perturb baselines.
void testCompSignedZeroNeutral()
{
    constexpr double sr = 48000.0;
    constexpr int    n = 8;

    spatcore::rt::RtSnapshot<xoa::SpeakerCompRtParams> snap;
    snap.publish (neutralParams (1));

    xoa::SpeakerCompProcessor comp;
    comp.prepare (sr, n, 1, &snap);

    juce::AudioBuffer<float> buffer (1, n);
    const float negZero = -0.0f;
    for (int i = 0; i < n; ++i) buffer.setSample (0, i, (i % 2 == 0) ? negZero : 0.25f);

    comp.processBlock (buffer, 1, n);

    // The even taps must remain the negative-zero bit pattern (not +0.0f).
    bool preserved = true;
    for (int i = 0; i < n; i += 2)
        if (! std::signbit (buffer.getSample (0, i)))
            preserved = false;
    CHECK (preserved);
}

//==============================================================================
// The delay smoother GLIDES to a small target change instead of stepping. Feed
// a continuous unit-slope ramp so the observed delay is recoverable as
// (n - output[n]); publish delay D1, then a small change to D2 a block later,
// and verify the recovered delay leaves D1 continuously and converges on D2 -
// NOT an instant jump (which is what a smoother stripped to `return target`
// would produce).
void testCompDelayGlide()
{
    constexpr double sr = 48000.0;
    constexpr int    block1 = 256;
    constexpr int    block2 = 1024;
    const double     d1 = 20.0, d2 = 30.0;   // samples

    spatcore::rt::RtSnapshot<xoa::SpeakerCompRtParams> snap;
    xoa::SpeakerCompRtParams p = neutralParams (1);
    p.delayMs[0] = (float) (d1 * 1000.0 / sr);
    snap.publish (p);

    xoa::SpeakerCompProcessor comp;
    comp.prepare (sr, block1 + block2, 1, &snap);

    juce::int64 pos = 0;
    auto processRamp = [&] (int len, std::vector<float>& out)
    {
        juce::AudioBuffer<float> buffer (1, len);
        for (int i = 0; i < len; ++i) buffer.setSample (0, i, (float) (pos + i));
        comp.processBlock (buffer, 1, len);
        for (int i = 0; i < len; ++i) out.push_back (buffer.getSample (0, i));
        pos += len;
    };

    std::vector<float> b1, b2;
    processRamp (block1, b1);              // settle at D1

    p.delayMs[0] = (float) (d2 * 1000.0 / sr);   // small change -> glide
    snap.publish (p);
    processRamp (block2, b2);

    // Recovered delay at the start of block 2 stays at D1 (continuity), NOT D2.
    const double recoveredStart = (double) block1 - (double) b2[0];
    CHECK (std::abs (recoveredStart - d1) < 0.5);

    // ... and converges to D2 by the end of the (long) block.
    const double recoveredEnd = (double) (block1 + block2 - 1) - (double) b2.back();
    CHECK (recoveredEnd > 29.0);

    // The glide is monotone (no overshoot / step).
    bool monotone = true;
    for (int i = 1; i < block2; ++i)
    {
        const double prev = (double) (block1 + i - 1) - (double) b2[(size_t) (i - 1)];
        const double cur  = (double) (block1 + i)     - (double) b2[(size_t) i];
        if (cur < prev - 1.0e-2) monotone = false;
    }
    CHECK (monotone);
}

//==============================================================================
// A large delay jump (> teleport threshold) triggers the smoother's
// mute-move-unmute envelope: on a DC signal the output dips to ~0 at the
// envelope midpoint and recovers to unity. Exercises the teleport branch.
void testCompDelayTeleport()
{
    constexpr double sr = 48000.0;
    constexpr int    blk = 256;
    const double     d1 = 5.0, d2 = 1600.0;   // 1595-sample jump > 30 ms threshold

    spatcore::rt::RtSnapshot<xoa::SpeakerCompRtParams> snap;
    xoa::SpeakerCompRtParams p = neutralParams (1);
    p.delayMs[0] = (float) (d1 * 1000.0 / sr);
    snap.publish (p);

    xoa::SpeakerCompProcessor comp;
    comp.prepare (sr, blk, 1, &snap);

    auto processDc = [&] (int blocks, std::vector<float>& out)
    {
        for (int b = 0; b < blocks; ++b)
        {
            juce::AudioBuffer<float> buffer (1, blk);
            for (int i = 0; i < blk; ++i) buffer.setSample (0, i, 1.0f);
            comp.processBlock (buffer, 1, blk);
            for (int i = 0; i < blk; ++i) out.push_back (buffer.getSample (0, i));
        }
    };

    std::vector<float> prefill, jump;
    processDc (8, prefill);                 // saturate the delay line with DC at D1

    p.delayMs[0] = (float) (d2 * 1000.0 / sr);
    snap.publish (p);
    processDc (2, jump);                    // covers the 480-sample envelope

    float minV = 1.0f, lastV = jump.back();
    for (float v : jump) minV = juce::jmin (minV, v);

    CHECK (minV < 0.05f);       // the mute dip
    CHECK (lastV > 0.99f);      // recovered to unity after the snap
}

} // namespace

//==============================================================================
void runXoaCompTests()
{
    testCompComposerDistanceModes();
    testCompComposerTrimMuteSolo();
    testCompListenerOriginBitIdentity();
    testCompListenerOffCenter();
    testEqComposer();
    testCompNeutralPassthrough();
    testCompSignedZeroNeutral();
    testCompIntegerDelay();
    testCompMuteRamp();
    testCompEqPeakGain();
    testCompSampleRateIndependence();
    testCompDelayGlide();
    testCompDelayTeleport();
}
