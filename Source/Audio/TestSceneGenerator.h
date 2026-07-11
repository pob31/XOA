#pragma once

#include <juce_core/juce_core.h>

#include <cmath>
#include <cstdint>

#include "XoaConstants.h"
#include "DSP/AmbiSphericalHarmonics.h"

//==============================================================================
// XOA - synthetic Ambisonics test scene (PRD sec.9: "order-10 content is rare;
// provide an internal test-scene generator").
//
// This header is compiled BOTH by the app (a UI-triggerable RT input source)
// and by the offline-render harness (a deterministic input stream). Every
// value is a pure function of (source, absolute sample index, sample rate) or
// (source, tick) - no RNG objects, no wall-clock time - so the same arguments
// always render the identical stream, which is what makes the harness's
// per-machine SHA baselines meaningful.
//
// Block-partition independence: renderScene's output at absolute sample n
// depends only on n, never on how the timeline is chopped into blocks. That is
// the property the harness's --block sweeps and the app's variable device block
// size both rely on (tested in B10).
//
// Directions are quantized to the app's 50 Hz control tick: the encoding SH
// gains are recomputed only when the tick advances, matching the rate at which
// a real control surface would move a source.
//==============================================================================

namespace xoa::scene
{

constexpr int    kNumSources = 3;
constexpr double kTickRateHz = 50.0;

/** The 50 Hz control tick containing an absolute sample index. Pure function
    of (n, sampleRate); n*50 stays exact in double for any realistic length
    (one hour at 96 kHz is ~1.7e10, well under 2^53). */
inline int tickForSample (juce::int64 sampleIndex, double sampleRate) noexcept
{
    return (int) std::floor ((double) sampleIndex * kTickRateHz / sampleRate);
}

//==============================================================================
// Squirrel-hash noise - the FrDiffusionModel / WFS scenarios idiom, duplicated
// locally so the generator depends on no RNG state.
//==============================================================================
inline float hashNoiseBipolar (std::uint32_t n, std::uint32_t key) noexcept
{
    n *= 0xB5297A4Du;
    n += key;
    n ^= n >> 8;
    n += 0x68E31DA4u;
    n ^= n << 8;
    n *= 0x1B56C4E9u;
    n ^= n >> 8;
    return (float) (std::int32_t) n * (1.0f / 2147483648.0f);
}

inline std::uint32_t makeKey (std::uint32_t a, std::uint32_t b) noexcept
{
    return a * 0x9E3779B9u + (b + 1u) * 0x85EBCA6Bu + 1u;
}

//==============================================================================
/** Mono signal for one virtual source at an absolute sample index: a
    fixed-phase sine at a per-source frequency, a unit impulse at sample 0 (so
    a single render excites every SH channel - useful for decoder response
    checks), plus low-level deterministic hash noise. */
inline float sourceSignalSample (int source, juce::int64 n, double sampleRate) noexcept
{
    const double freq = 110.0 + 70.0 * (double) source;
    float s = 0.25f * (float) std::sin (juce::MathConstants<double>::twoPi
                                         * freq * (double) n / sampleRate);
    if (n == 0)
        s += 0.9f;
    s += 0.001f * hashNoiseBipolar ((std::uint32_t) n,
                                    makeKey ((std::uint32_t) source * 31u + 7u, 0u));
    return s;
}

/** Deterministic orbit for one virtual source at a given tick. Azimuth is left
    unwrapped (sh::evaluate normalizes internally); elevation is bounded well
    inside [-90, 90]. */
inline void sourceDirection (int source, int tick, double& azimuthDeg, double& elevationDeg) noexcept
{
    const double t = (double) tick / kTickRateHz;   // seconds
    switch (source)
    {
        case 0:  azimuthDeg =  40.0 * t;          elevationDeg = 0.0;                        break;
        case 1:  azimuthDeg = -25.0 * t + 90.0;   elevationDeg = 30.0 * std::sin (0.30 * t); break;
        default: azimuthDeg =  15.0 * t + 200.0;  elevationDeg = 20.0 + 15.0 * std::sin (0.70 * t); break;
    }
}

//==============================================================================
/** Render the moving-source scene, encoded to the given order (ACN/SN3D), into
    `channels` (numChannels(order) buffers of at least numSamples). Overwrites
    (does not accumulate into) the destination. Allocation-free, noexcept: safe
    on the audio thread.

    The SH encoding gains are held per 50 Hz tick and recomputed only when the
    tick advances within the block, so output depends solely on absolute sample
    position - not on block boundaries. */
inline void renderScene (int order, juce::int64 blockStartSample, int numSamples,
                         double sampleRate, float* const* channels) noexcept
{
    jassert (order >= 0 && order <= xoa::kAmbisonicOrder);
    const int nch = sh::numChannels (order);

    for (int c = 0; c < nch; ++c)
        std::fill (channels[c], channels[c] + numSamples, 0.0f);

    if (numSamples <= 0)
        return;

    double gains[kNumSources][xoa::kNumSHChannels];
    int heldTick = -1;

    for (int i = 0; i < numSamples; ++i)
    {
        const juce::int64 n = blockStartSample + (juce::int64) i;

        const int tick = tickForSample (n, sampleRate);
        if (tick != heldTick)
        {
            for (int s = 0; s < kNumSources; ++s)
            {
                double az, el;
                sourceDirection (s, tick, az, el);
                sh::evaluate (az, el, order, gains[s]);
            }
            heldTick = tick;
        }

        for (int s = 0; s < kNumSources; ++s)
        {
            const float sig = sourceSignalSample (s, n, sampleRate);
            for (int c = 0; c < nch; ++c)
                channels[c][i] += sig * (float) gains[s][c];
        }
    }
}

} // namespace xoa::scene
