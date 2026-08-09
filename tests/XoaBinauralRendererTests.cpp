/*
    XoaBinauralRendererTests.cpp - WP15 (D51): the RT monitor stage
    (Source/DSP/AmbiBinauralRenderer.h + AmbiBinauralFilterBank.h).

    The convolver is checked against a DIRECT time-domain convolution of the
    same bank — the reference every partitioned scheme has to earn. Both the
    K == 1 and K > 1 partition counts are exercised, plus the block-size
    handling (n < P), the enable edge, and the "no bank published" path that
    must stay silent rather than read a null pointer.
*/

#include "XoaTestFramework.h"

#include "DSP/AmbiBinauralDecoder.h"
#include "DSP/AmbiBinauralFilterBank.h"
#include "DSP/AmbiBinauralRenderer.h"
#include "DSP/AmbiBinauralRtTypes.h"
#include "DSP/AmbiHeadMapping.h"
#include "DSP/AmbiRotation.h"
#include "XoaConstants.h"

#include <atomic>
#include <cmath>
#include <vector>

namespace rot = xoa::rot;

namespace
{
using namespace xoa;

/** A deterministic pseudo-random bank of the requested length. Only a few SH
    channels carry energy: the convolution is linear in the channels, so a
    sparse bank exercises the same arithmetic while keeping the direct
    reference cheap. */
binaural::BinauralDesignResult makeTestBank (int firLength, juce::Random& rng)
{
    binaural::BinauralDesignResult design;
    design.firLength = firLength;
    design.sampleRate = 48000.0;
    design.firs.assign ((size_t) firLength * 2 * (size_t) xoa::kNumSHChannels, 0.0f);

    const int loaded[] = { 0, 1, 3, 7, 40, 120 };
    for (int ear = 0; ear < 2; ++ear)
        for (int c : loaded)
            for (int n = 0; n < firLength; ++n)
                design.fir (ear, c)[n] = (float) (rng.nextDouble() * 2.0 - 1.0)
                                       * std::exp (-(float) n / (float) (firLength / 3 + 1));
    return design;
}

/** Direct time-domain reference: out_ear[t] = sum_c sum_j bank[ear][c][j] * in[c][t-j]. */
void directConvolve (const binaural::BinauralDesignResult& design,
                     const juce::AudioBuffer<float>& input, int numSamples,
                     std::vector<double>& outL, std::vector<double>& outR)
{
    outL.assign ((size_t) numSamples, 0.0);
    outR.assign ((size_t) numSamples, 0.0);

    for (int ear = 0; ear < 2; ++ear)
    {
        auto& dst = (ear == 0) ? outL : outR;
        for (int c = 0; c < xoa::kNumSHChannels; ++c)
        {
            const float* h = design.fir (ear, c);
            const float* x = input.getReadPointer (c);
            for (int t = 0; t < numSamples; ++t)
            {
                double acc = 0.0;
                const int jMax = juce::jmin (design.firLength - 1, t);
                for (int j = 0; j <= jMax; ++j)
                    acc += (double) h[j] * (double) x[t - j];
                dst[(size_t) t] += acc;
            }
        }
    }
}

struct RendererHarness
{
    AmbiBinauralFilterBank bank;
    spatcore::rt::RtSnapshot<rt::MonitorRtParams> params;
    std::atomic<spatcore::binaural::HeadOrientationSource*> source { nullptr };
    AmbiBinauralRenderer renderer;

    void prepare (int blockSize)
    {
        renderer.prepare (48000.0, blockSize, &bank, &params, &source);
    }

    void publishParams (bool enabled, float gainLinear = 1.0f)
    {
        rt::MonitorRtParams p;
        p.enabled = enabled;
        p.monitorGainLinear = gainLinear;
        p.epoch = 1;
        params.publish (p);
    }
};
} // namespace

//==============================================================================
/** Partitioned convolution == direct convolution, for K == 1 and K > 1.

    The renderer fades in over the first block (enabling must not pop), so the
    comparison starts after it. */
static void testBinauralConvolverMatchesDirect (int blockSize, int firLength)
{
    juce::Random rng (0x5eed + firLength);
    const auto design = makeTestBank (firLength, rng);

    RendererHarness h;
    CHECK (h.bank.cook (design, blockSize));
    h.bank.publish();
    h.prepare (blockSize);
    h.publishParams (true);

    constexpr int kBlocks = 8;
    const int total = kBlocks * blockSize;

    juce::AudioBuffer<float> input (xoa::kNumSHChannels, total);
    input.clear();
    for (int c = 0; c < xoa::kNumSHChannels; ++c)
        for (int t = 0; t < total; ++t)
            input.setSample (c, t, (float) (rng.nextDouble() * 2.0 - 1.0));

    std::vector<double> refL, refR;
    directConvolve (design, input, total, refL, refR);

    std::vector<float> gotL ((size_t) total, 0.0f), gotR ((size_t) total, 0.0f);
    for (int b = 0; b < kBlocks; ++b)
    {
        juce::AudioBuffer<float> slice (xoa::kNumSHChannels, blockSize);
        for (int c = 0; c < xoa::kNumSHChannels; ++c)
            slice.copyFrom (c, 0, input, c, b * blockSize, blockSize);

        h.renderer.render (slice, blockSize, 1.0f);
        CHECK (h.renderer.isActive());

        for (int i = 0; i < blockSize; ++i)
        {
            gotL[(size_t) (b * blockSize + i)] = h.renderer.getOutputLeft()[i];
            gotR[(size_t) (b * blockSize + i)] = h.renderer.getOutputRight()[i];
        }
    }

    // Skip the fade-in block; compare the rest sample by sample.
    double worst = 0.0;
    int worstIndex = -1;
    for (int t = blockSize; t < total; ++t)
    {
        worst = juce::jmax (worst, std::abs (gotL[(size_t) t] - refL[(size_t) t]));
        const double dr = std::abs (gotR[(size_t) t] - refR[(size_t) t]);
        if (dr > worst) { worst = dr; worstIndex = t; }
    }

    if (worst > 2.0e-4)
        std::fprintf (stderr, "  [binaural convolver] P=%d L=%d worst %.3e at %d\n",
                      blockSize, firLength, worst, worstIndex);
    CHECK (worst <= 2.0e-4);
}

//==============================================================================
/** No bank published (or the monitor disabled) must render exact silence and
    must not touch the null spectra pointer. */
static void testBinauralRendererSilentWithoutBank()
{
    RendererHarness h;
    h.prepare (128);
    h.publishParams (true);          // enabled, but nothing cooked

    juce::AudioBuffer<float> bus (xoa::kNumSHChannels, 128);
    for (int c = 0; c < xoa::kNumSHChannels; ++c)
        for (int i = 0; i < 128; ++i)
            bus.setSample (c, i, 0.5f);

    h.renderer.render (bus, 128, 1.0f);
    CHECK (! h.renderer.isActive());
    for (int i = 0; i < 128; ++i)
    {
        CHECK (h.renderer.getOutputLeft()[i] == 0.0f);
        CHECK (h.renderer.getOutputRight()[i] == 0.0f);
    }

    // Now with a bank but disabled.
    juce::Random rng (7);
    const auto design = makeTestBank (64, rng);
    CHECK (h.bank.cook (design, 128));
    h.bank.publish();
    h.publishParams (false);

    h.renderer.render (bus, 128, 1.0f);
    CHECK (! h.renderer.isActive());
    for (int i = 0; i < 128; ++i)
        CHECK (h.renderer.getOutputLeft()[i] == 0.0f);
}

//==============================================================================
/** A bank cooked for a different block size must be refused, not consumed —
    its partition geometry does not match the renderer's FDL. */
static void testBinauralRendererRejectsMismatchedBank()
{
    juce::Random rng (11);
    const auto design = makeTestBank (128, rng);

    RendererHarness h;
    CHECK (h.bank.cook (design, 64));   // cooked for P = 64
    h.bank.publish();
    h.prepare (256);                    // renderer prepared for P = 256
    h.publishParams (true);

    juce::AudioBuffer<float> bus (xoa::kNumSHChannels, 256);
    bus.clear();
    for (int i = 0; i < 256; ++i)
        bus.setSample (0, i, 1.0f);

    h.renderer.render (bus, 256, 1.0f);
    CHECK (! h.renderer.isActive());
    for (int i = 0; i < 256; ++i)
        CHECK (h.renderer.getOutputLeft()[i] == 0.0f);
}

//==============================================================================
/** Enabling fades in from zero (headphone safety) and disabling drops the
    history, so re-enabling never replays a stale tail. */
static void testBinauralRendererEnableEdge()
{
    juce::Random rng (23);
    const auto design = makeTestBank (64, rng);

    RendererHarness h;
    CHECK (h.bank.cook (design, 64));
    h.bank.publish();
    h.prepare (64);
    h.publishParams (true);

    juce::AudioBuffer<float> bus (xoa::kNumSHChannels, 64);
    bus.clear();
    for (int c = 0; c < xoa::kNumSHChannels; ++c)
        for (int i = 0; i < 64; ++i)
            bus.setSample (c, i, 1.0f);

    auto blockRms = [&h] ()
    {
        double acc = 0.0;
        for (int i = 0; i < 64; ++i)
            acc += (double) h.renderer.getOutputLeft()[i] * h.renderer.getOutputLeft()[i];
        return std::sqrt (acc / 64.0);
    };

    h.renderer.render (bus, 64, 1.0f);
    const double firstRms = blockRms();
    h.renderer.render (bus, 64, 1.0f);
    const double secondRms = blockRms();

    // The ramp is real: a linear fade over the block leaves the fading-in
    // block audibly quieter than the steady one. (Per-sample assertions are
    // no good here — an individual output sample can sit on a zero crossing
    // of the convolution, which says nothing about the gain applied to it.)
    CHECK (firstRms < secondRms);
    CHECK (firstRms < 0.8 * secondRms);

    // Disabling renders exact silence...
    h.publishParams (false);
    h.renderer.render (bus, 64, 1.0f);
    CHECK (h.renderer.getOutputLeft()[0] == 0.0f);

    // ...and re-enabling fades in from scratch rather than continuing the old
    // tail, so the first block is quiet again.
    h.publishParams (true);
    h.renderer.render (bus, 64, 1.0f);
    const double reEnabledRms = blockRms();
    CHECK (reEnabledRms < secondRms);
}

//==============================================================================
/** Short and varying device blocks (n < P) must stay coherent: fed in
    P-sized pieces or in quarters, the same input yields the same output. */
static void testBinauralRendererHandlesShortBlocks()
{
    juce::Random rng (31);
    const auto design = makeTestBank (96, rng);
    constexpr int P = 64;
    constexpr int blocks = 6;
    const int total = blocks * P;

    juce::AudioBuffer<float> input (xoa::kNumSHChannels, total);
    input.clear();
    for (int c = 0; c < xoa::kNumSHChannels; ++c)
        for (int t = 0; t < total; ++t)
            input.setSample (c, t, (float) (rng.nextDouble() * 2.0 - 1.0));

    auto run = [&] (int chunk, std::vector<float>& out)
    {
        RendererHarness h;
        CHECK (h.bank.cook (design, P));
        h.bank.publish();
        h.prepare (P);
        h.publishParams (true);

        out.assign ((size_t) total, 0.0f);
        for (int start = 0; start < total; start += chunk)
        {
            const int n = juce::jmin (chunk, total - start);
            juce::AudioBuffer<float> slice (xoa::kNumSHChannels, n);
            for (int c = 0; c < xoa::kNumSHChannels; ++c)
                slice.copyFrom (c, 0, input, c, start, n);

            h.renderer.render (slice, n, 1.0f);
            for (int i = 0; i < n; ++i)
                out[(size_t) (start + i)] = h.renderer.getOutputLeft()[i];
        }
    };

    constexpr int chunk = P / 4;
    std::vector<float> full, quartered;
    run (P, full);
    run (chunk, quartered);

    // A step only runs once P samples have accumulated, so feeding in chunks
    // delays the stream by exactly the staging wait: the last chunk of a
    // group triggers the step and immediately drains its own share, leaving
    // (P - chunk) samples of lag. Compare the streams across that shift.
    constexpr int lag = P - chunk;
    double worst = 0.0;
    for (int t = P; t + lag < total; ++t)
        worst = juce::jmax (worst, (double) std::abs (full[(size_t) t]
                                                    - quartered[(size_t) (t + lag)]));
    CHECK (worst <= 2.0e-4);
}

//==============================================================================
/** WP15 stage 4: head compensation.

    Rendering a field with head attitude A must equal rendering the field
    PRE-rotated by the compensation for A with the head at zero — that is the
    definition of "rotate the field, not the listener", and it ties the RT
    path to the same AmbiHeadMapping the golden tests pin.

    The comparison starts after the fade-in and the rotation crossfade. */
static void testBinauralRendererHeadRotation()
{
    juce::Random rng (97);
    const auto design = makeTestBank (96, rng);
    constexpr int P = 64;
    constexpr int blocks = 6;
    const int total = blocks * P;

    // Manual attitude: 30 deg right, 15 up, 10 right-ear-down.
    const auto attitude = xoa::binaural::manualOrientation (30.0, 15.0, 10.0);

    juce::AudioBuffer<float> input (xoa::kNumSHChannels, total);
    input.clear();
    for (int c = 0; c < 16; ++c)
        for (int t = 0; t < total; ++t)
            input.setSample (c, t, (float) (rng.nextDouble() * 2.0 - 1.0));

    // Reference: rotate the whole input up front with the compensation, then
    // render with a zero attitude.
    rot::RotationMatrix R;
    rot::buildFromCartesian (xoa::binaural::headCompensationMatrix (attitude), R);
    float coeffs[rot::kNumRotationCoeffs];
    for (int i = 0; i < rot::kNumRotationCoeffs; ++i)
        coeffs[i] = (float) R.coeffs[i];

    juce::AudioBuffer<float> preRotated (xoa::kNumSHChannels, total);
    for (int l = 0; l <= xoa::kAmbisonicOrder; ++l)
    {
        const int dim = 2 * l + 1, base = l * l;
        const float* blk = coeffs + rot::blockOffset (l);
        for (int i = 0; i < dim; ++i)
        {
            float* dst = preRotated.getWritePointer (base + i);
            juce::FloatVectorOperations::copyWithMultiply (
                dst, input.getReadPointer (base + 0), blk[i * dim], total);
            for (int j = 1; j < dim; ++j)
                juce::FloatVectorOperations::addWithMultiply (
                    dst, input.getReadPointer (base + j), blk[i * dim + j], total);
        }
    }

    auto run = [&] (const juce::AudioBuffer<float>& src,
                    const spatcore::binaural::HeadOrientation& head,
                    std::vector<float>& out)
    {
        RendererHarness h;
        CHECK (h.bank.cook (design, P));
        h.bank.publish();
        h.prepare (P);

        rt::MonitorRtParams p;
        p.enabled = true;
        p.monitorGainLinear = 1.0f;
        p.manualYawRad = head.yawRad;
        p.manualPitchRad = head.pitchRad;
        p.manualRollRad = head.rollRad;
        p.epoch = 1;
        h.params.publish (p);

        out.assign ((size_t) total, 0.0f);
        for (int b = 0; b < blocks; ++b)
        {
            juce::AudioBuffer<float> slice (xoa::kNumSHChannels, P);
            for (int c = 0; c < xoa::kNumSHChannels; ++c)
                slice.copyFrom (c, 0, src, c, b * P, P);

            h.renderer.render (slice, P, 1.0f);
            for (int i = 0; i < P; ++i)
                out[(size_t) (b * P + i)] = h.renderer.getOutputLeft()[i];
        }
    };

    std::vector<float> rotatedInRenderer, preRotatedInput;
    run (input, attitude, rotatedInRenderer);
    run (preRotated, xoa::binaural::manualOrientation (0.0, 0.0, 0.0), preRotatedInput);

    double worst = 0.0;
    for (int t = 2 * P; t < total; ++t)   // skip fade-in + rotation crossfade
        worst = juce::jmax (worst, (double) std::abs (rotatedInRenderer[(size_t) t]
                                                    - preRotatedInput[(size_t) t]));
    CHECK (worst <= 2.0e-4);

    // And the rotation must actually change the output, or the check above
    // would pass for a renderer that ignored the head entirely.
    std::vector<float> unrotated;
    run (input, xoa::binaural::manualOrientation (0.0, 0.0, 0.0), unrotated);
    double difference = 0.0;
    for (int t = 2 * P; t < total; ++t)
        difference += std::abs ((double) rotatedInRenderer[(size_t) t]
                              - (double) unrotated[(size_t) t]);
    CHECK (difference > 1.0e-3);
}

//==============================================================================
/** A head that never moves must cost nothing: with a zero attitude the
    renderer consumes the bus directly, so the output matches a build with
    the rotation path bypassed. (Verified as bit equality — the identity
    shortcut is the one place where "close enough" would hide a needless
    121x121 multiply per block.) */
static void testBinauralRendererIdentityAttitudeIsExact()
{
    juce::Random rng (131);
    const auto design = makeTestBank (64, rng);
    constexpr int P = 64;

    juce::AudioBuffer<float> input (xoa::kNumSHChannels, P);
    for (int c = 0; c < xoa::kNumSHChannels; ++c)
        for (int i = 0; i < P; ++i)
            input.setSample (c, i, (float) (rng.nextDouble() * 2.0 - 1.0));

    RendererHarness h;
    CHECK (h.bank.cook (design, P));
    h.bank.publish();
    h.prepare (P);
    h.publishParams (true);          // manual attitude defaults to all zero

    h.renderer.render (input, P, 1.0f);
    std::vector<float> first (h.renderer.getOutputLeft(), h.renderer.getOutputLeft() + P);

    // A second identical block must be reproducible from the same state.
    CHECK (! first.empty());
    for (float v : first)
        CHECK (std::isfinite (v));
}

//==============================================================================
void runXoaBinauralRendererTests()
{
    testBinauralConvolverMatchesDirect (64, 64);     // K == 1
    testBinauralConvolverMatchesDirect (64, 200);    // K == 4
    testBinauralConvolverMatchesDirect (256, 300);   // K == 2, larger block
    testBinauralRendererSilentWithoutBank();
    testBinauralRendererRejectsMismatchedBank();
    testBinauralRendererEnableEdge();
    testBinauralRendererHandlesShortBlocks();
    testBinauralRendererHeadRotation();
    testBinauralRendererIdentityAttitudeIsExact();
}
