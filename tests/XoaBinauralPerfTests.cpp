/*
    XoaBinauralPerfTests.cpp - WP15 stage 8: what the monitor actually costs
    on the audio thread at order 10.

    Not a pass/fail gate on absolute speed (a build machine's timing is not a
    contract) but a printed measurement plus a deliberately loose ceiling: the
    monitor must stay a small fraction of real time, and a regression that
    made it, say, 20x slower would fail rather than quietly eat the buffer.
    The ceiling applies to optimized builds only — Debug measures about ten
    times this and would fail for reasons that never reach a user.

    Reported as a percentage of real time — the number that decides whether a
    rig can run this alongside a 121-channel decode.
*/

#include "XoaTestFramework.h"

#include "DSP/AmbiBinauralDecoder.h"
#include "DSP/AmbiBinauralFilterBank.h"
#include "DSP/AmbiBinauralRenderer.h"
#include "DSP/AmbiBinauralRtTypes.h"
#include "DSP/AmbiHeadMapping.h"
#include "XoaConstants.h"

#include <atomic>
#include <cmath>
#include <vector>

namespace
{
using namespace xoa;

binaural::BinauralDesignResult makePerfBank (int firLength)
{
    juce::Random rng (2024);
    binaural::BinauralDesignResult design;
    design.firLength = firLength;
    design.sampleRate = 48000.0;
    design.firs.assign ((size_t) firLength * 2 * (size_t) xoa::kNumSHChannels, 0.0f);
    // A REAL bank is dense — every SH channel carries a filter, which is what
    // makes the convolution cost what it costs. A sparse bank would flatter.
    for (int ear = 0; ear < 2; ++ear)
        for (int c = 0; c < xoa::kNumSHChannels; ++c)
            for (int n = 0; n < firLength; ++n)
                design.fir (ear, c)[n] = (float) (rng.nextDouble() * 0.02 - 0.01);
    return design;
}

/** Wall-clock cost of `blocks` monitor renders, as a percentage of the real
    time that audio represents. */
double measureMonitorLoad (int blockSize, int firLength, bool withHeadMotion)
{
    AmbiBinauralFilterBank bank;
    const auto design = makePerfBank (firLength);
    if (! bank.cook (design, blockSize))
        return -1.0;
    bank.publish();

    spatcore::rt::RtSnapshot<rt::MonitorRtParams> params;
    std::atomic<spatcore::binaural::HeadOrientationSource*> source { nullptr };
    AmbiBinauralRenderer renderer;
    renderer.prepare (48000.0, blockSize, &bank, &params, &source);

    juce::AudioBuffer<float> bus (xoa::kNumSHChannels, blockSize);
    juce::Random rng (11);
    for (int c = 0; c < xoa::kNumSHChannels; ++c)
        for (int i = 0; i < blockSize; ++i)
            bus.setSample (c, i, (float) (rng.nextDouble() * 2.0 - 1.0));

    const int blocks = juce::jmax (200, (int) (48000 * 2 / blockSize));   // ~2 s of audio

    auto publish = [&] (double yawDeg)
    {
        rt::MonitorRtParams p;
        p.enabled = true;
        p.monitorGainLinear = 1.0f;
        const auto head = binaural::manualOrientation (yawDeg, 0.0, 0.0);
        p.manualYawRad = head.yawRad;
        p.manualPitchRad = head.pitchRad;
        p.manualRollRad = head.rollRad;
        p.epoch = 1;
        params.publish (p);
    };
    publish (withHeadMotion ? 1.0 : 0.0);

    const double start = juce::Time::getMillisecondCounterHiRes();
    for (int b = 0; b < blocks; ++b)
    {
        if (withHeadMotion)
            publish (std::sin (0.05 * (double) b) * 30.0);   // rebuild every block
        renderer.render (bus, blockSize, 1.0f);
    }
    const double elapsedMs = juce::Time::getMillisecondCounterHiRes() - start;

    const double audioMs = 1000.0 * (double) blocks * (double) blockSize / 48000.0;
    return 100.0 * elapsedMs / audioMs;
}
} // namespace

//==============================================================================
void runXoaBinauralPerfTests()
{
    struct Case { int block; int firLength; const char* label; };
    const Case cases[] = {
        {  64, 256, "64 smp / 256 tap" },
        { 128, 256, "128 smp / 256 tap" },
        { 256, 384, "256 smp / 384 tap" },
    };

    for (const auto& c : cases)
    {
        const double still = measureMonitorLoad (c.block, c.firLength, false);
        const double moving = measureMonitorLoad (c.block, c.firLength, true);
        CHECK (still > 0.0 && moving > 0.0);

        std::printf ("  [timing] binaural monitor, order 10, %-18s "
                     "head still %5.2f %% RT, head moving %5.2f %% RT\n",
                     c.label, still, moving);

        // Loose ceiling: a monitor that needed more than a third of real time
        // would be unusable next to the decode it is monitoring. A regression
        // tripwire, not a performance target — and only meaningful in an
        // optimized build (a Debug run measures roughly 10x this and says
        // nothing about the shipped code).
       #if ! JUCE_DEBUG
        CHECK (still < 35.0);
        CHECK (moving < 35.0);
       #endif
    }
}
