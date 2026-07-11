/*
    XoaSceneTests.cpp - WP6 synthetic test-scene generator (B10).

    TestSceneGenerator.h is compiled by both the app and the offline-render
    harness, and its per-machine SHA baselines depend on it being a pure,
    deterministic, block-partition-independent function. These tests pin all
    three properties:
      - determinism: identical arguments -> identical output;
      - block-partition independence: rendering [0, N) in one call equals
        rendering it in arbitrary sub-blocks (the property the harness --block
        sweeps rely on);
      - encoding correctness: the order-0 (W) channel is the analytic sum of
        source signals, and every order matches an independent sum-of-(sig * Y)
        recomputation from the WP3 primitives (validates the accumulation and
        tick-holding logic, not sh::evaluate itself, which has its own goldens).
*/

#include "XoaTestFramework.h"

#include "XoaConstants.h"
#include "Audio/TestSceneGenerator.h"
#include "DSP/AmbiSphericalHarmonics.h"

#include <cmath>
#include <vector>

namespace
{

// A small owner for numChannels(order) contiguous channel buffers plus the
// float* const* view renderScene wants.
struct SceneBuffer
{
    SceneBuffer (int order, int numSamples)
        : channels ((size_t) xoa::sh::numChannels (order),
                    std::vector<float> ((size_t) numSamples, 0.0f))
    {
        for (auto& ch : channels)
            ptrs.push_back (ch.data());
    }

    float* const* view() { return ptrs.data(); }
    int numChannels() const { return (int) channels.size(); }

    std::vector<std::vector<float>> channels;
    std::vector<float*> ptrs;
};

//==============================================================================
void testDeterminism()
{
    const int order = 5, n = 512;
    const double sr = 48000.0;

    SceneBuffer a (order, n), b (order, n);
    xoa::scene::renderScene (order, 1000, n, sr, a.view());
    xoa::scene::renderScene (order, 1000, n, sr, b.view());

    int mismatches = 0;
    for (int c = 0; c < a.numChannels(); ++c)
        for (int i = 0; i < n; ++i)
            if (a.channels[(size_t) c][(size_t) i] != b.channels[(size_t) c][(size_t) i])
                ++mismatches;
    CHECK (mismatches == 0);
}

//==============================================================================
// Rendering a contiguous span in one call vs in ragged sub-blocks must be
// bit-identical - including sub-blocks that straddle 50 Hz tick boundaries.
void testBlockPartitionIndependence()
{
    const int order = 7;
    const double sr = 44100.0;
    const juce::int64 start = 0;
    const int total = 2400;   // ~2.7 ticks at 44.1 kHz -> spans tick boundaries

    SceneBuffer whole (order, total);
    xoa::scene::renderScene (order, start, total, sr, whole.view());

    // Ragged partition whose cut points do not align to tick boundaries.
    const int cuts[] = { 100, 441, 300, 882, 677 };   // sums to 2400
    SceneBuffer pieced (order, total);
    juce::int64 offset = 0;
    for (int cut : cuts)
    {
        std::vector<float*> sub;
        for (auto& ch : pieced.channels)
            sub.push_back (ch.data() + offset);
        xoa::scene::renderScene (order, start + offset, cut, sr, sub.data());
        offset += cut;
    }
    CHECK (offset == total);

    int mismatches = 0;
    for (int c = 0; c < whole.numChannels(); ++c)
        for (int i = 0; i < total; ++i)
            if (whole.channels[(size_t) c][(size_t) i] != pieced.channels[(size_t) c][(size_t) i])
                ++mismatches;
    CHECK (mismatches == 0);
}

//==============================================================================
// Order 0: the only channel is W (SN3D Y_0 == 1), so the bus equals the raw
// sum of source signals - a fully analytic anchor that does not route through
// the multi-degree SH machinery.
void testOrderZeroIsSignalSum()
{
    const int order = 0, n = 256;
    const double sr = 48000.0;
    const juce::int64 start = 777;

    SceneBuffer buf (order, n);
    CHECK (buf.numChannels() == 1);
    xoa::scene::renderScene (order, start, n, sr, buf.view());

    int mismatches = 0;
    for (int i = 0; i < n; ++i)
    {
        const juce::int64 nn = start + i;
        float expected = 0.0f;
        for (int s = 0; s < xoa::scene::kNumSources; ++s)
            expected += xoa::scene::sourceSignalSample (s, nn, sr);
        if (buf.channels[0][(size_t) i] != expected)
            ++mismatches;
    }
    CHECK (mismatches == 0);
}

//==============================================================================
// Order 3: the bus must equal the independent sum over sources of
// (source signal) * (SH direction vector), with directions held per tick -
// exactly matching renderScene's own tick quantization.
void testEncodingMatchesSumOfProjections()
{
    const int order = 3, n = 300;
    const double sr = 48000.0;
    const juce::int64 start = 0;
    const int nch = xoa::sh::numChannels (order);

    SceneBuffer buf (order, n);
    xoa::scene::renderScene (order, start, n, sr, buf.view());

    double worst = 0.0;
    double y[xoa::scene::kNumSources][xoa::kNumSHChannels];
    int heldTick = -1;

    for (int i = 0; i < n; ++i)
    {
        const juce::int64 nn = start + i;
        const int tick = xoa::scene::tickForSample (nn, sr);
        if (tick != heldTick)
        {
            for (int s = 0; s < xoa::scene::kNumSources; ++s)
            {
                double az, el;
                xoa::scene::sourceDirection (s, tick, az, el);
                xoa::sh::evaluate (az, el, order, y[s]);
            }
            heldTick = tick;
        }

        for (int c = 0; c < nch; ++c)
        {
            double expected = 0.0;
            for (int s = 0; s < xoa::scene::kNumSources; ++s)
                expected += (double) xoa::scene::sourceSignalSample (s, nn, sr) * y[s][c];
            worst = juce::jmax (worst, std::abs ((double) buf.channels[(size_t) c][(size_t) i] - expected));
        }
    }
    // Both sides accumulate the same float products in the same order, so the
    // only slack is the double-vs-float reduction; keep it tight.
    CHECK (worst < 1.0e-5);
}

//==============================================================================
// The impulse at absolute sample 0 lands only in the block that contains it.
void testImpulseAtSampleZero()
{
    const int order = 2, n = 64;
    const double sr = 48000.0;

    SceneBuffer atZero (order, n);
    xoa::scene::renderScene (order, 0, n, sr, atZero.view());

    SceneBuffer later (order, n);
    xoa::scene::renderScene (order, n, n, sr, later.view());

    // W channel at the impulse sample carries the summed 0.9 impulses plus the
    // sample-0 sine/noise; it must exceed the quiet later block's first sample.
    CHECK (std::abs (atZero.channels[0][0]) > std::abs (later.channels[0][0]));
    CHECK (std::abs (atZero.channels[0][0]) > 1.0f);   // ~3 * 0.9 from the impulses
}

} // namespace

//==============================================================================
void runXoaSceneTests()
{
    testDeterminism();
    testBlockPartitionIndependence();
    testOrderZeroIsSignalSum();
    testEncodingMatchesSumOfProjections();
    testImpulseAtSampleZero();
}
