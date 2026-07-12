#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include <array>
#include <cstdint>
#include <vector>

#include "spatcore/dsp/DelayTargetSmoother.h"
#include "spatcore/dsp/OutputEQBiquadFilter.h"
#include "spatcore/rt/RtSnapshot.h"

#include "XoaConstants.h"
#include "SpeakerCompParams.h"

//==============================================================================
// XOA - per-speaker compensation RT stage (WP7, FR-15). Runs on the audio
// thread, AFTER the bus decode and BEFORE the engine's output meters. Per
// output channel the chain is:
//
//     fractional delay  ->  6-band parametric EQ  ->  gain ramp
//
//   Delay      one fractional delay line per output, driven by a
//              DelayTargetSmoother (small edits glide, large edits snap under a
//              mute-move-unmute envelope). Target arrives in MILLISECONDS via
//              the RtSnapshot and is converted to samples here, so a device
//              restart at a new rate needs no message-side recompose.
//   EQ         6x OutputEQBiquadFilter in series (Audio EQ Cookbook), ported
//              from WFS-DIY's OutputEQProcessor. Coefficients are pushed from
//              the message thread via setEqParameters() under benign-staleness
//              (the biquad short-circuits no-change), the production-proven WFS
//              pattern: at worst a torn coefficient read causes a one-tick
//              transient, never a fault.
//   Gain       one juce::SmoothedValue per output ramps trim x distance-atten x
//              mute/solo (already folded control-side into a single linear gain)
//              across the block - click-free mute/solo.
//
// NEUTRALITY: at the default config (distanceCompMode 0, no trim, EQ disabled,
// all audible) every stage is an exact identity - delay target 0 reads the
// just-written sample, the smoother/gain sit settled at 1.0, and the biquads
// are shape 0 pass-throughs - so the stage is bit-transparent and the offline
// baselines are undisturbed. The XoaCompTests neutrality test pins this.
//==============================================================================

namespace xoa
{

class SpeakerCompProcessor
{
public:
    SpeakerCompProcessor() = default;

    //==========================================================================
    /** Allocate per-output state for `numOutputs` device outputs. `snapshotSource`
        is the message-thread-published comp POD (may be null in isolated tests,
        in which case the stage is a settled pass-through). Called from a
        non-audio thread (prepareToPlay / device restart). */
    void prepare (double newSampleRate, int /*maxBlockSize*/, int numOutputs,
                  spatcore::rt::RtSnapshot<SpeakerCompRtParams>* snapshotSource)
    {
        sampleRate = newSampleRate > 0.0 ? newSampleRate : 48000.0;
        numChannels = juce::jlimit (0, xoa::kMaxSpeakers, numOutputs);
        snapshot = snapshotSource;
        samplePos = 0;

        // Delay-line capacity: the full comp delay range (kMaxCompDelayMs) plus a
        // 2-sample interpolation margin. Allocated per ACTUAL device out (not the
        // 256 clamp), keeping the memory bound tractable on small rigs.
        const int maxDelaySamples = (int) std::ceil (kMaxCompDelayMs * 0.001 * sampleRate);
        delayCapacity = maxDelaySamples + 2;

        // ~10 ms smoothing window for the delay smoother; teleport threshold at
        // its default (3x window) snaps edits larger than ~30 ms.
        windowSamples = juce::jmax (2, (int) std::lround (0.010 * sampleRate));

        delayLines.assign ((size_t) numChannels, DelayLine {});
        smoothers.assign ((size_t) numChannels, spatcore::dsp::DelayTargetSmoother {});
        eqBanks.assign ((size_t) numChannels, EqBank {});
        eqEnabled.assign ((size_t) numChannels, (juce::uint8) 0);
        gainSmoothers.assign ((size_t) numChannels, juce::SmoothedValue<float> {});

        for (int c = 0; c < numChannels; ++c)
        {
            delayLines[(size_t) c].prepare (delayCapacity);
            smoothers[(size_t) c].prepare (windowSamples);
            for (auto& f : eqBanks[(size_t) c])
                f.prepare (sampleRate);
            auto& g = gainSmoothers[(size_t) c];
            g.reset (sampleRate, 0.020);            // 20 ms gain ramp
            g.setCurrentAndTargetValue (1.0f);       // start settled -> neutral
        }
    }

    void releaseResources()
    {
        delayLines.clear();
        smoothers.clear();
        eqBanks.clear();
        eqEnabled.clear();
        gainSmoothers.clear();
        numChannels = 0;
        snapshot = nullptr;
    }

    void reset()
    {
        for (auto& dl : delayLines) dl.reset();
        for (auto& sm : smoothers)  sm.reset();
        for (auto& bank : eqBanks)
            for (auto& f : bank) f.reset();
        samplePos = 0;
    }

    //==========================================================================
    /** Message thread (benign-staleness, D3): push the 6-band EQ config onto the
        RT-owned biquads. The biquad setParameters short-circuits no-change, so
        calling this unconditionally per timer tick is cheap. */
    void setEqParameters (const SpeakerEqParams& eq)
    {
        const int n = juce::jmin (eq.numSpeakers, numChannels);
        for (int c = 0; c < n; ++c)
        {
            eqEnabled[(size_t) c] = eq.enabled[c] ? (juce::uint8) 1 : (juce::uint8) 0;
            auto& bank = eqBanks[(size_t) c];
            for (int b = 0; b < xoa::kNumEqBands; ++b)
            {
                const auto& bp = eq.bands[c][b];
                // Disabled channel -> every band forced to OFF (shape 0).
                const int shape = eq.enabled[c] ? bp.shape : 0;
                bank[(size_t) b].setParameters (shape, bp.freq, bp.gainDb, bp.q, bp.slope);
            }
        }
    }

    //==========================================================================
    /** Audio thread. Applies delay -> EQ -> gain in place to the first
        `numActive` channels of `buffer` (clamped to the prepared channel count
        and the published speaker count). Allocation-free. */
    void processBlock (juce::AudioBuffer<float>& buffer, int numActive, int numSamples) noexcept
    {
        if (numChannels == 0 || numSamples <= 0)
            return;

        SpeakerCompRtParams params;
        if (snapshot != nullptr)
            params = snapshot->acquire();

        const int n = juce::jmin (juce::jmin (numActive, numChannels),
                                  juce::jmin (params.numSpeakers, buffer.getNumChannels()));

        for (int c = 0; c < n; ++c)
        {
            float* data = buffer.getWritePointer (c) + 0;
            auto&  dl   = delayLines[(size_t) c];
            auto&  sm   = smoothers[(size_t) c];

            // Delay target (samples), observed once at the block head.
            const float targetSamples = params.delayMs[c] * 0.001f * (float) sampleRate;
            sm.observe (targetSamples, samplePos);

            for (int i = 0; i < numSamples; ++i)
            {
                const float in = data[i];
                dl.write (in);
                const auto s = sm.smoothedAt (samplePos + i);
                data[i] = dl.readFrac (s.delay) * s.gain;
                dl.advance();
            }

            // 6-band EQ in series (each biquad no-ops when shape 0).
            if (eqEnabled[(size_t) c] != 0)
                for (auto& f : eqBanks[(size_t) c])
                    f.processBlock (data, numSamples);

            // Gain ramp: trim x distance atten x mute/solo.
            auto& g = gainSmoothers[(size_t) c];
            g.setTargetValue (params.gainLinear[c]);
            if (g.isSmoothing())
            {
                for (int i = 0; i < numSamples; ++i)
                    data[i] *= g.getNextValue();
            }
            else
            {
                const float gv = g.getNextValue();   // == target, settled
                if (gv != 1.0f)
                    juce::FloatVectorOperations::multiply (data, gv, numSamples);
            }
        }

        samplePos += numSamples;
    }

private:
    //==========================================================================
    /** Circular fractional (linear-interpolated) delay line. write() the newest
        sample, readFrac(d) reads d samples behind it, then advance(). At d == 0
        readFrac returns the just-written sample exactly (bit-transparent). */
    struct DelayLine
    {
        void prepare (int capacity)
        {
            size = juce::jmax (2, capacity);
            buffer.assign ((size_t) size, 0.0f);
            writePos = 0;
        }

        void reset()
        {
            std::fill (buffer.begin(), buffer.end(), 0.0f);
            writePos = 0;
        }

        void write (float x) noexcept { buffer[(size_t) writePos] = x; }

        float readFrac (float delaySamples) const noexcept
        {
            float rp = (float) writePos - delaySamples;
            if (rp < 0.0f) rp += (float) size;
            if (rp < 0.0f) rp = 0.0f;                       // guard pathological delay

            int i0 = (int) rp;
            if (i0 >= size) i0 -= size;
            float frac = rp - (float) (int) rp;

            // Integer delay (incl. the delay-0 neutral case): return the tap
            // verbatim. Skipping the `+ buffer[i1]*0.0f` term keeps -0.0f, NaN,
            // and denormals bit-exact - the neutrality/baseline guarantee, and
            // an interpolation identity (frac 0 -> the i0 tap either way).
            if (frac == 0.0f)
                return buffer[(size_t) i0];

            int i1 = i0 + 1;
            if (i1 >= size) i1 -= size;

            return buffer[(size_t) i0] * (1.0f - frac) + buffer[(size_t) i1] * frac;
        }

        void advance() noexcept { if (++writePos >= size) writePos = 0; }

        std::vector<float> buffer;
        int size = 0;
        int writePos = 0;
    };

    using EqBank = std::array<spatcore::dsp::OutputEQBiquadFilter, xoa::kNumEqBands>;

    double sampleRate = 48000.0;
    int    numChannels = 0;
    int    delayCapacity = 0;
    int    windowSamples = 480;
    std::int64_t samplePos = 0;

    spatcore::rt::RtSnapshot<SpeakerCompRtParams>* snapshot = nullptr;

    std::vector<DelayLine> delayLines;
    std::vector<spatcore::dsp::DelayTargetSmoother> smoothers;
    std::vector<EqBank> eqBanks;
    std::vector<juce::uint8> eqEnabled;   // uint8 avoids vector<bool> proxy
    std::vector<juce::SmoothedValue<float>> gainSmoothers;
};

} // namespace xoa
