#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_dsp/juce_dsp.h>

#include <array>
#include <atomic>
#include <cstring>

#include "spatcore/rt/RtSnapshot.h"

#include "XoaConstants.h"
#include "DSP/AmbiRotation.h"
#include "DSP/AmbiRtTypes.h"
#include "DSP/AmbiSphericalHarmonics.h"
#include "DSP/DecoderMatrixBuilder.h"

//==============================================================================
// XOA - the real-time Ambisonics bus (WP6, the M1 "first audible" chain).
//
// Implements the spatcore duck-typed algorithm surface (prepare / reprepare /
// processBlock / setProcessingEnabled / releaseResources / metering) so it can
// live beside the WFS algorithms in the same broker. Unlike the WFS
// algorithms it takes no raw matrix pointers: its three inputs arrive through
// the RtSnapshot / DecoderMatrixBuilder seams the app publishes to.
//
// Per-block chain (channel-major, juce::FloatVectorOperations, allocation-free):
//
//   input channels --gather--> 121-ch bus --rotate--> bus' --decode GEMM--> outs
//                                                                --master gain
//
//   gather   busParams.srcChannel[c]/gain[c] map each input channel onto a bus
//            channel (order adaptation x convention x FuMa, pre-cooked in C2).
//   rotate   the WP4 block-diagonal SO(3) matrix, applied in float. On a
//            rotation change the block is a one-block linear crossfade of the
//            old and new renders - mathematically identical to lerping the
//            matrix, but SIMD-friendly and deterministic (FR-9, PRD sec.7
//            <=2-buffer response).
//   decode   the [numSpeakers x 121] float matrix from DecoderMatrixBuilder,
//            hot-swapped under the RtSnapshot pointer seam (FR-17).
//   master   post-decode gain, per-block linear ramp (click-free).
//
// Single-threaded in-callback at M1 scale; the fan-out over output speakers is
// a natural fit for spatcore/rt/AudioParallelFor if a future rig needs it.
//
// epoch guards: a null decoder matrix or a never-published bus (epoch 0) yields
// silence; a never-published rotation (epoch 0) yields an unrotated decode.
// RtSnapshot value-initializes T{}, so a publish-before-enable violation is
// inaudible, never UB.
//==============================================================================

namespace xoa
{

class AmbiBusAlgorithm
{
public:
    AmbiBusAlgorithm() = default;

    //==========================================================================
    void prepare (int maxInputChannels, int maxOutputChannels,
                  double sampleRate, int blockSize,
                  const DecoderMatrixBuilder* decoderSource,
                  const spatcore::rt::RtSnapshot<rt::RotationRtState>* rotationSource,
                  const spatcore::rt::RtSnapshot<rt::BusRtParams>* busParamsSource,
                  bool processingEnabled)
    {
        juce::ignoreUnused (maxInputChannels, maxOutputChannels);

        decoder = decoderSource;
        rotation = rotationSource;
        busParams = busParamsSource;
        currentSampleRate = sampleRate;
        allocatedBlock = juce::jmax (1, blockSize);

        busA.setSize (xoa::kNumSHChannels, allocatedBlock, false, false, true);
        busB.setSize (xoa::kNumSHChannels, allocatedBlock, false, false, true);
        busT.setSize (xoa::kNumSHChannels, allocatedBlock, false, false, true);

        crossover.prepare ({ sampleRate, (juce::uint32) allocatedBlock, (juce::uint32) xoa::kNumSHChannels });
        crossover.setType (juce::dsp::LinkwitzRileyFilterType::lowpass);   // unused; the 2-out split ignores it
        dualBandActive = false;
        activeCrossoverHz = 0.0f;
        for (float& g : activeHfGain) g = 1.0f;

        rot::RotationMatrix identity;
        rot::identity (identity);
        for (int i = 0; i < rot::kNumRotationCoeffs; ++i)
            currentRotation[i] = (float) identity.coeffs[i];
        lastRotationEpoch = 0;
        lastMasterGain = 1.0f;

        for (auto& p : outputPeak)
            p.store (0.0f, std::memory_order_relaxed);

        enabled.store (processingEnabled, std::memory_order_relaxed);
    }

    /** Device sample-rate / block-size change (allocations legal here - no
        callback is in flight). Snapshot sources are retained. */
    void reprepare (double sampleRate, int blockSize, bool processingEnabled)
    {
        currentSampleRate = sampleRate;
        const int newBlock = juce::jmax (1, blockSize);
        if (newBlock != allocatedBlock)
        {
            allocatedBlock = newBlock;
            busA.setSize (xoa::kNumSHChannels, allocatedBlock, false, false, true);
            busB.setSize (xoa::kNumSHChannels, allocatedBlock, false, false, true);
            busT.setSize (xoa::kNumSHChannels, allocatedBlock, false, false, true);
        }
        crossover.prepare ({ sampleRate, (juce::uint32) allocatedBlock, (juce::uint32) xoa::kNumSHChannels });
        dualBandActive = false;
        activeCrossoverHz = 0.0f;
        for (float& g : activeHfGain) g = 1.0f;
        enabled.store (processingEnabled, std::memory_order_relaxed);
    }

    //==========================================================================
    void processBlock (const juce::AudioSourceChannelInfo& bufferToFill,
                       const juce::AudioBuffer<float>& inputBuffer,
                       int numInputChannels, int numOutputChannels) noexcept
    {
        auto* outBuf = bufferToFill.buffer;
        const int startSample = bufferToFill.startSample;
        const int totalOutChannels = outBuf->getNumChannels();
        const int n = juce::jmin (bufferToFill.numSamples, allocatedBlock);

        const auto decoderHandle = decoder != nullptr ? decoder->acquire() : DecoderRtHandle {};
        const auto busState = busParams != nullptr ? busParams->acquire() : rt::BusRtParams {};

        // Guard: nothing to render -> silence + zeroed meters.
        if (! enabled.load (std::memory_order_relaxed)
            || decoderHandle.matrix == nullptr
            || decoderHandle.numSpeakers <= 0
            || busState.epoch == 0)
        {
            bufferToFill.clearActiveBufferRegion();
            for (auto& p : outputPeak)
                p.store (0.0f, std::memory_order_relaxed);
            activeDecoderEpoch.store (decoderHandle.epoch, std::memory_order_relaxed);
            return;
        }

        // --- 1. Gather: input channels -> 121-ch bus (busA) --------------------
        const int usableIn = juce::jmin (numInputChannels, inputBuffer.getNumChannels(),
                                         busState.numInputChannels);
        for (int c = 0; c < xoa::kNumSHChannels; ++c)
        {
            float* dst = busA.getWritePointer (c);
            const int src = busState.srcChannel[c];
            if (src >= 0 && src < usableIn && busState.gain[c] != 0.0f)
                juce::FloatVectorOperations::copyWithMultiply (
                    dst, inputBuffer.getReadPointer (src, startSample), busState.gain[c], n);
            else
                juce::FloatVectorOperations::clear (dst, n);
        }

        // --- 2. Rotate: busA -> rotated (busB, or busA when bypassed) ----------
        const auto rotState = rotation != nullptr ? rotation->acquire() : rt::RotationRtState {};
        juce::AudioBuffer<float>* rotated;   // non-const: dual-band filters it in place

        if (rotState.epoch == 0)
        {
            rotated = &busA;   // never published -> unrotated decode
        }
        else if (rotState.epoch == lastRotationEpoch)
        {
            applyRotation (currentRotation, busA, busB, n);   // steady state
            rotated = &busB;
        }
        else if (lastRotationEpoch == 0)
        {
            // First application: adopt the published matrix, no crossfade.
            std::memcpy (currentRotation, rotState.coeffs, sizeof (currentRotation));
            lastRotationEpoch = rotState.epoch;
            applyRotation (currentRotation, busA, busB, n);
            rotated = &busB;
        }
        else
        {
            // Rotation changed: one-block linear crossfade old -> new.
            applyRotation (currentRotation, busA, busB, n);   // old
            applyRotation (rotState.coeffs, busA, busT, n);   // new
            for (int c = 0; c < xoa::kNumSHChannels; ++c)
            {
                busB.applyGainRamp (c, 0, n, 1.0f, 0.0f);
                busB.addFromWithRamp (c, 0, busT.getReadPointer (c), n, 0.0f, 1.0f);
            }
            std::memcpy (currentRotation, rotState.coeffs, sizeof (currentRotation));
            lastRotationEpoch = rotState.epoch;
            rotated = &busB;
        }

        // --- 2.5 Dual-band (FR-14): LR4-split the rotated bus and reweight the
        // HF band per channel so the single decode GEMM yields basic below the
        // crossover / max-rE above. Skipped entirely when dual-band is off, so
        // that path is bit-identical to before this stage existed. busT is free
        // here (the rotation crossfade already consumed it).
        {
            const bool toggle = (decoderHandle.dualBand != dualBandActive);
            if (decoderHandle.dualBand || toggle)
            {
                if (toggle && decoderHandle.dualBand)
                {
                    crossover.reset();                                    // OFF -> ON: cold start
                    crossover.setCutoffFrequency (decoderHandle.crossoverHz);
                    activeCrossoverHz = decoderHandle.crossoverHz;
                }
                else if (decoderHandle.dualBand && decoderHandle.crossoverHz != activeCrossoverHz)
                {
                    crossover.setCutoffFrequency (decoderHandle.crossoverHz);   // TPT is state-preserving
                    activeCrossoverHz = decoderHandle.crossoverHz;
                }

                if (toggle)   // stash the dry bus to crossfade against
                    for (int c = 0; c < xoa::kNumSHChannels; ++c)
                        juce::FloatVectorOperations::copy (busT.getWritePointer (c),
                                                           rotated->getReadPointer (c), n);

                // HF weights to apply this block. ON (steady or OFF->ON) uses the
                // new handle's weights; an ON->OFF transition uses the STILL-ACTIVE
                // weights (activeHfGain), so the wet endpoint matches the prior
                // block and the crossfade to dry is continuous - the OFF handle's
                // padded 1.0s would otherwise step the HF band and click.
                const float* hf = decoderHandle.dualBand ? decoderHandle.hfGain : activeHfGain;

                // Wet = LP + hf * HP, in place (also advances the filter on the
                // OFF-transition block so the wet start is continuous).
                for (int c = 0; c < xoa::kNumSHChannels; ++c)
                {
                    float* wp = rotated->getWritePointer (c);
                    const float h = hf[c];
                    for (int i = 0; i < n; ++i)
                    {
                        float lo, hi;
                        crossover.processSample (c, wp[i], lo, hi);
                        wp[i] = lo + h * hi;
                    }
                }

                if (toggle)
                {
                    // One-block crossfade dry (busT) <-> wet (rotated): ON fades
                    // dry->wet, OFF fades wet->dry.
                    const float wetStart = decoderHandle.dualBand ? 0.0f : 1.0f;
                    const float wetEnd   = decoderHandle.dualBand ? 1.0f : 0.0f;
                    for (int c = 0; c < xoa::kNumSHChannels; ++c)
                    {
                        rotated->applyGainRamp (c, 0, n, wetStart, wetEnd);
                        rotated->addFromWithRamp (c, 0, busT.getReadPointer (c), n,
                                                  1.0f - wetStart, 1.0f - wetEnd);
                    }
                    dualBandActive = decoderHandle.dualBand;
                }

                // Remember the live HF weights for a future ON->OFF transition.
                if (decoderHandle.dualBand)
                    std::memcpy (activeHfGain, decoderHandle.hfGain, sizeof (activeHfGain));
            }
        }

        // --- 3. Decode GEMM: [L x 121] . bus -> output channels ----------------
        const int L = juce::jmin (decoderHandle.numSpeakers, numOutputChannels, totalOutChannels);
        for (int s = 0; s < L; ++s)
        {
            const float* row = decoderHandle.matrix + (size_t) s * xoa::kNumSHChannels;
            float* out = outBuf->getWritePointer (s, startSample);
            juce::FloatVectorOperations::copyWithMultiply (out, rotated->getReadPointer (0), row[0], n);
            for (int c = 1; c < xoa::kNumSHChannels; ++c)
                juce::FloatVectorOperations::addWithMultiply (out, rotated->getReadPointer (c), row[c], n);
        }

        // Output channels with no speaker: silence.
        for (int ch = L; ch < totalOutChannels; ++ch)
            juce::FloatVectorOperations::clear (outBuf->getWritePointer (ch, startSample), n);

        // Defensive: if the device ever delivers a block larger than prepared,
        // n was clamped to the bus allocation (preventing an internal overrun);
        // clear the unwritten output tail so it can never emit stale samples.
        if (const int overflow = bufferToFill.numSamples - n; overflow > 0)
            for (int ch = 0; ch < totalOutChannels; ++ch)
                juce::FloatVectorOperations::clear (outBuf->getWritePointer (ch, startSample + n), overflow);

        // --- 4. Master gain (post-decode, click-free ramp) ---------------------
        for (int s = 0; s < L; ++s)
            outBuf->applyGainRamp (s, startSample, n, lastMasterGain, busState.masterGainLinear);
        lastMasterGain = busState.masterGainLinear;

        // --- 5. Output metering ------------------------------------------------
        for (int s = 0; s < L; ++s)
            outputPeak[(size_t) s].store (outBuf->getMagnitude (s, startSample, n),
                                          std::memory_order_relaxed);
        for (int s = L; s < (int) outputPeak.size(); ++s)
            outputPeak[(size_t) s].store (0.0f, std::memory_order_relaxed);

        activeDecoderEpoch.store (decoderHandle.epoch, std::memory_order_relaxed);
    }

    //==========================================================================
    void setProcessingEnabled (bool e) noexcept { enabled.store (e, std::memory_order_relaxed); }

    void releaseResources()
    {
        busA.setSize (0, 0);
        busB.setSize (0, 0);
        busT.setSize (0, 0);
        allocatedBlock = 0;
    }

    /** Linear block-peak of a rendered output channel (relaxed atomic). */
    float getOutputPeakLevel (int channel) const noexcept
    {
        if (channel < 0 || channel >= (int) outputPeak.size())
            return 0.0f;
        return outputPeak[(size_t) channel].load (std::memory_order_relaxed);
    }

    juce::uint32 getActiveDecoderEpoch() const noexcept
    {
        return activeDecoderEpoch.load (std::memory_order_relaxed);
    }

private:
    //==========================================================================
    // out = R . in, block-diagonal per degree, channel-major over numSamples.
    // R is a float copy of the WP4 rotation matrix (rot::blockOffset layout).
    static void applyRotation (const float* coeffs,
                               const juce::AudioBuffer<float>& in,
                               juce::AudioBuffer<float>& out, int numSamples) noexcept
    {
        for (int l = 0; l <= xoa::kAmbisonicOrder; ++l)
        {
            const int blockDim = 2 * l + 1;
            const int base = l * l;                       // acn(l, -l)
            const float* blk = coeffs + rot::blockOffset (l);

            for (int i = 0; i < blockDim; ++i)
            {
                float* dst = out.getWritePointer (base + i);
                const float* rowCoeffs = blk + i * blockDim;
                juce::FloatVectorOperations::copyWithMultiply (
                    dst, in.getReadPointer (base + 0), rowCoeffs[0], numSamples);
                for (int j = 1; j < blockDim; ++j)
                    juce::FloatVectorOperations::addWithMultiply (
                        dst, in.getReadPointer (base + j), rowCoeffs[j], numSamples);
            }
        }
    }

    const DecoderMatrixBuilder* decoder = nullptr;
    const spatcore::rt::RtSnapshot<rt::RotationRtState>* rotation = nullptr;
    const spatcore::rt::RtSnapshot<rt::BusRtParams>* busParams = nullptr;

    juce::AudioBuffer<float> busA, busB, busT;   // gather / rotated / transition
    int allocatedBlock = 0;
    double currentSampleRate = 0.0;

    // Dual-band LR4 crossover, one filter handling all 121 bus channels.
    juce::dsp::LinkwitzRileyFilter<float> crossover;
    bool dualBandActive = false;
    float activeCrossoverHz = 0.0f;
    float activeHfGain[xoa::kNumSHChannels];   // live HF weights, for the ON->OFF crossfade

    float currentRotation[rot::kNumRotationCoeffs];
    juce::uint32 lastRotationEpoch = 0;
    float lastMasterGain = 1.0f;

    std::array<std::atomic<float>, xoa::kMaxSpeakers> outputPeak {};
    std::atomic<juce::uint32> activeDecoderEpoch { 0 };
    std::atomic<bool> enabled { false };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AmbiBusAlgorithm)
};

} // namespace xoa
