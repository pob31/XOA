/*
  ==============================================================================

    XOA — tenth-order Ambisonics spatial audio processor.
    AmbiBinauralRenderer — the RT monitor stage: 121 SH channels in, a
    binaural pair out (WP15, D51-D53).

    Called from AmbiBusAlgorithm between the dual-band stage and the decode
    GEMM (D52), so the monitor hears the same weighted field the loudspeaker
    decode consumes. It only READS the bus and writes its own buffers, which
    is what makes the speaker path provably unaffected.

    Convolution — uniform-partitioned overlap-save with a shared input
    transform. The naive shape would be 2 x 121 separate convolutions; here
    each SH channel is transformed ONCE per block and both ears accumulate
    from the same frequency-delay line, so the transform count is 121 forward
    + 2 inverse rather than 242 of each:

        per block:  X_c = FFT([prev_c | curr_c])            (121 forward)
                    Y_ear = sum_c sum_k X_c[pos-k] · H[ear][c][k]
                    out_ear = last P samples of IFFT(Y_ear)   (2 inverse)

    Block-size handling: the transform step consumes exactly P samples, so a
    device block that is shorter than the prepared size (or varies) is staged
    through a small FIFO. At the steady n == P case the step runs inside the
    same call and adds NO latency; a short block simply waits until P samples
    have accumulated, which is the only correct thing to do.

    Nothing here allocates: prepare() sizes the FDL for the longest filter
    the designer can produce (kMaxBinauralFirLength), and a published bank
    that somehow exceeds that is ignored rather than trusted.

    This file is part of XOA, released under the GNU General Public License
    v3.0. See LICENSE for details.

  ==============================================================================
*/

#pragma once

#include <juce_dsp/juce_dsp.h>

#include <atomic>
#include <memory>
#include <vector>

#include "spatcore/binaural/HeadOrientationSource.h"
#include "spatcore/rt/RtSnapshot.h"

#include "DSP/AmbiBinauralDecoder.h"
#include "DSP/AmbiBinauralFilterBank.h"
#include "DSP/AmbiBinauralRtTypes.h"
#include "DSP/AmbiHeadMapping.h"
#include "DSP/AmbiRotation.h"
#include "XoaConstants.h"

namespace xoa
{

class AmbiBinauralRenderer
{
public:
    AmbiBinauralRenderer() = default;

    /** Message thread, before the device starts. Allocates everything the
        audio thread will ever touch. */
    void prepare (double sampleRateToUse, int blockSizeToUse,
                  const AmbiBinauralFilterBank* bankToUse,
                  const spatcore::rt::RtSnapshot<rt::MonitorRtParams>* paramsToUse,
                  const std::atomic<spatcore::binaural::HeadOrientationSource*>* sourceToUse)
    {
        sampleRate = sampleRateToUse;
        blockSize = juce::jmax (1, blockSizeToUse);
        bank = bankToUse;
        params = paramsToUse;
        activeSource = sourceToUse;

        int order = 1;
        while ((1 << order) < 2 * blockSize)
            ++order;
        fftSize = 1 << order;
        spectrumFloats = 2 * fftSize;
        fft = std::make_unique<juce::dsp::FFT> (order);

        maxPartitions = (binaural::kMaxBinauralFirLength + blockSize - 1) / blockSize;

        fdl.assign ((size_t) xoa::kNumSHChannels * (size_t) maxPartitions
                        * (size_t) spectrumFloats, 0.0f);
        prevBlocks.assign ((size_t) xoa::kNumSHChannels * (size_t) blockSize, 0.0f);
        inputStage.assign ((size_t) xoa::kNumSHChannels * (size_t) blockSize, 0.0f);
        rotatedBus.setSize (xoa::kNumSHChannels, blockSize, false, false, true);
        rotationFade.setSize (xoa::kNumSHChannels, blockSize, false, false, true);
        fftWork.assign ((size_t) spectrumFloats, 0.0f);
        accum.assign ((size_t) spectrumFloats, 0.0f);

        outFifo[0].assign ((size_t) 2 * blockSize, 0.0f);
        outFifo[1].assign ((size_t) 2 * blockSize, 0.0f);
        outBlock[0].assign ((size_t) blockSize, 0.0f);
        outBlock[1].assign ((size_t) blockSize, 0.0f);

        reset();
    }

    /** Drop all history — on enable, on a bank geometry change, on restart. */
    void reset() noexcept
    {
        std::fill (fdl.begin(), fdl.end(), 0.0f);
        std::fill (prevBlocks.begin(), prevBlocks.end(), 0.0f);
        std::fill (inputStage.begin(), inputStage.end(), 0.0f);
        for (int e = 0; e < 2; ++e)
        {
            std::fill (outFifo[e].begin(), outFifo[e].end(), 0.0f);
            std::fill (outBlock[e].begin(), outBlock[e].end(), 0.0f);
        }
        hasRotation = false;
        crossfadeRotation = false;
        lastYaw = lastPitch = lastRoll = 0.0f;
        fdlPos = 0;
        inputFill = 0;
        outWrite = outRead = outCount = 0;
        activeEpoch = 0;
        wasEnabled = false;
        rampGain = 0.0f;
    }

    /** True while the monitor is producing output this block — the engine
        uses it to decide whether the reserved hardware pair is fed. */
    bool isActive() const noexcept { return activeThisBlock; }

    /** AUDIO THREAD. "This block produced nothing" — called when the bus
        chain bails out before reaching the tap (no decoder, processing
        disabled). Without it `isActive()` would still report last block's
        answer and the engine would scatter a stale buffer. */
    void markIdle() noexcept
    {
        activeThisBlock = false;
        if (wasEnabled)
            reset();
    }

    const float* getOutputLeft()  const noexcept { return outBlock[0].data(); }
    const float* getOutputRight() const noexcept { return outBlock[1].data(); }

    /** Total monitor latency in samples: the designer's kernel lead plus,
        when the device block is shorter than the prepared size, the staging
        wait. Reported, not compensated (a monitor path has no one to align
        with). */
    int getLatencySamples() const noexcept { return binaural::kBinauralFilterLeadSamples; }

    //==========================================================================
    /** AUDIO THREAD. `shBus` is the rotated/dual-banded SH bus; `n` samples
        are read from its first kNumSHChannels channels. `masterGainLinear`
        is the bus master gain, which the monitor must follow (the tap is
        pre-master). Allocation-free. */
    void render (const juce::AudioBuffer<float>& shBus, int n, float masterGainLinear) noexcept
    {
        activeThisBlock = false;
        if (fft == nullptr || n <= 0 || n > blockSize)
            return;

        const auto p = params != nullptr ? params->acquire() : rt::MonitorRtParams {};
        const auto handle = bank != nullptr ? bank->acquire() : rt::BinauralFilterRtHandle {};

        const bool bankUsable = handle.epoch != 0 && handle.spectra != nullptr
                             && handle.blockSize == blockSize
                             && handle.fftSize == fftSize
                             && handle.numPartitions > 0
                             && handle.numPartitions <= maxPartitions;

        if (p.epoch == 0 || ! p.enabled || ! bankUsable)
        {
            // Not running: clear the output and drop history so re-enabling
            // never replays a stale tail.
            if (wasEnabled)
                reset();
            std::fill (outBlock[0].begin(), outBlock[0].begin() + n, 0.0f);
            std::fill (outBlock[1].begin(), outBlock[1].begin() + n, 0.0f);
            return;
        }

        // A bank whose geometry changed invalidates the frequency history.
        if (handle.epoch != activeEpoch)
        {
            if (activeEpoch != 0 && handle.numPartitions != activePartitions)
            {
                std::fill (fdl.begin(), fdl.end(), 0.0f);
                fdlPos = 0;
            }
            activeEpoch = handle.epoch;
            activePartitions = handle.numPartitions;
        }

        if (! wasEnabled)
        {
            wasEnabled = true;
            rampGain = 0.0f;   // fade in: enabling must never pop into headphones
        }

        // ---- head compensation: rotate the field, not the listener ---------
        const juce::AudioBuffer<float>& source = applyHeadRotation (shBus, n, p);

        // ---- stage input, running one transform step per full P samples ----
        int consumed = 0;
        while (consumed < n)
        {
            const int take = juce::jmin (blockSize - inputFill, n - consumed);
            for (int c = 0; c < xoa::kNumSHChannels; ++c)
                juce::FloatVectorOperations::copy (
                    inputStage.data() + (size_t) c * blockSize + inputFill,
                    source.getReadPointer (c) + consumed, take);

            inputFill += take;
            consumed += take;

            if (inputFill == blockSize)
            {
                runTransformStep (handle);
                inputFill = 0;
            }
        }

        // ---- drain n samples, ramping the gain ------------------------------
        const float targetGain = masterGainLinear * p.monitorGainLinear;
        const float step = 1.0f / (float) juce::jmax (1, blockSize);

        for (int i = 0; i < n; ++i)
        {
            float l = 0.0f, r = 0.0f;
            if (outCount > 0)
            {
                l = outFifo[0][(size_t) outRead];
                r = outFifo[1][(size_t) outRead];
                outRead = (outRead + 1) % (2 * blockSize);
                --outCount;
            }

            // Short fade-in on enable (~one block), then track the gain.
            if (rampGain < 1.0f)
                rampGain = juce::jmin (1.0f, rampGain + step);

            outBlock[0][(size_t) i] = l * targetGain * rampGain;
            outBlock[1][(size_t) i] = r * targetGain * rampGain;
        }

        activeThisBlock = true;
    }

private:
    /** AUDIO THREAD. Rotate the field by the inverse of the head attitude, so
        a fixed set of SH→ear filters keeps pointing where the listener's ears
        actually are (D51). Returns the buffer the convolution should consume:
        the ORIGINAL bus when the attitude is identity, so a monitor with no
        tracker and no manual offset costs nothing and stays bit-exact.

        The attitude is read fresh from the active source every block (D53),
        bypassing the damped control path entirely; the manual parameters in
        the snapshot are the fallback whenever no source has a valid pose. */
    const juce::AudioBuffer<float>& applyHeadRotation (const juce::AudioBuffer<float>& shBus,
                                                       int n,
                                                       const rt::MonitorRtParams& p) noexcept
    {
        spatcore::binaural::HeadOrientation attitude;
        attitude.yawRad = p.manualYawRad;
        attitude.pitchRad = p.manualPitchRad;
        attitude.rollRad = p.manualRollRad;
        attitude.valid = true;

        if (activeSource != nullptr)
        {
            if (auto* src = activeSource->load (std::memory_order_acquire))
            {
                const auto tracked = src->getOrientation();
                if (tracked.valid && isFiniteAttitude (tracked))
                    attitude = tracked;   // a stale or faceless tracker falls back to manual
            }
        }

        // Last line of defence. A non-finite attitude would build a
        // non-orthonormal SH rotation (Debug-asserted inside buildFromCartesian)
        // and then poison the convolution state with NaN, which no amount of
        // downstream ramping recovers from. Sources are supposed to filter
        // this out; the audio thread does not get to trust them.
        if (! isFiniteAttitude (attitude))
        {
            attitude.yawRad = attitude.pitchRad = attitude.rollRad = 0.0f;
        }

        const bool identity = attitude.yawRad == 0.0f
                           && attitude.pitchRad == 0.0f
                           && attitude.rollRad == 0.0f;

        if (identity && ! hasRotation)
            return shBus;   // nothing to do, and nothing to fade from

        // Rebuild only when the attitude actually moved. The Ivanic-Ruedenberg
        // recursion is tens of microseconds at order 10 — cheap enough to run
        // on the audio thread, but not per block for a head that is still.
        if (attitude.yawRad != lastYaw || attitude.pitchRad != lastPitch
            || attitude.rollRad != lastRoll)
        {
            lastYaw = attitude.yawRad;
            lastPitch = attitude.pitchRad;
            lastRoll = attitude.rollRad;

            rot::RotationMatrix built;
            rot::buildFromCartesian (binaural::headCompensationMatrix (attitude), built);

            std::copy (currentRotation, currentRotation + rot::kNumRotationCoeffs,
                       previousRotation);
            for (int i = 0; i < rot::kNumRotationCoeffs; ++i)
                currentRotation[i] = (float) built.coeffs[i];

            crossfadeRotation = hasRotation;   // no fade into the very first one
            hasRotation = true;
        }

        rotate (currentRotation, shBus, rotatedBus, n);

        if (crossfadeRotation)
        {
            // One-block linear crossfade old -> new, the same idiom the scene
            // rotation uses: a head-tracker step between blocks would otherwise
            // click.
            rotate (previousRotation, shBus, rotationFade, n);
            for (int c = 0; c < xoa::kNumSHChannels; ++c)
            {
                rotatedBus.applyGainRamp (c, 0, n, 0.0f, 1.0f);
                rotatedBus.addFromWithRamp (c, 0, rotationFade.getReadPointer (c), n, 1.0f, 0.0f);
            }
            crossfadeRotation = false;
        }

        return rotatedBus;
    }

    static bool isFiniteAttitude (const spatcore::binaural::HeadOrientation& o) noexcept
    {
        return std::isfinite (o.yawRad) && std::isfinite (o.pitchRad)
            && std::isfinite (o.rollRad);
    }

    /** out = R · in, block-diagonal per degree (the WP4 layout). */
    static void rotate (const float* coeffs, const juce::AudioBuffer<float>& in,
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

    /** One overlap-save step: transform the staged P samples of every SH
        channel, then accumulate both ears over all channels and partitions. */
    void runTransformStep (const rt::BinauralFilterRtHandle& handle) noexcept
    {
        const int parts = juce::jmin (handle.numPartitions, maxPartitions);

        // Forward transform per SH channel: [previous P | current P].
        for (int c = 0; c < xoa::kNumSHChannels; ++c)
        {
            float* prev = prevBlocks.data() + (size_t) c * blockSize;
            const float* curr = inputStage.data() + (size_t) c * blockSize;

            std::fill (fftWork.begin(), fftWork.end(), 0.0f);
            juce::FloatVectorOperations::copy (fftWork.data(), prev, blockSize);
            juce::FloatVectorOperations::copy (fftWork.data() + blockSize, curr, blockSize);
            fft->performRealOnlyForwardTransform (fftWork.data());

            float* x = fdl.data()
                     + ((size_t) c * (size_t) maxPartitions + (size_t) fdlPos)
                       * (size_t) spectrumFloats;
            juce::FloatVectorOperations::copy (x, fftWork.data(), spectrumFloats);

            juce::FloatVectorOperations::copy (prev, curr, blockSize);
        }

        for (int ear = 0; ear < 2; ++ear)
        {
            std::fill (accum.begin(), accum.end(), 0.0f);

            for (int c = 0; c < xoa::kNumSHChannels; ++c)
            {
                for (int k = 0; k < parts; ++k)
                {
                    int idx = fdlPos - k;
                    if (idx < 0)
                        idx += maxPartitions;

                    const float* x = fdl.data()
                                   + ((size_t) c * (size_t) maxPartitions + (size_t) idx)
                                     * (size_t) spectrumFloats;
                    const float* h = handle.spectrum (ear, c, k);

                    for (int b = 0; b < spectrumFloats; b += 2)
                    {
                        const float xr = x[b], xi = x[b + 1];
                        const float hr = h[b], hi = h[b + 1];
                        accum[(size_t) b]     += xr * hr - xi * hi;
                        accum[(size_t) b + 1] += xr * hi + xi * hr;
                    }
                }
            }

            fft->performRealOnlyInverseTransform (accum.data());

            // Overlap-save: the linear-convolution samples are the second
            // half. Both ears write the SAME FIFO positions, so the write
            // cursor only advances once, after the ear loop.
            const float* valid = accum.data() + blockSize;
            int write = outWrite;
            for (int i = 0; i < blockSize; ++i)
            {
                outFifo[ear][(size_t) write] = valid[i];
                write = (write + 1) % (2 * blockSize);
            }
        }

        outWrite = (outWrite + blockSize) % (2 * blockSize);
        outCount = juce::jmin (outCount + blockSize, 2 * blockSize);

        fdlPos = (fdlPos + 1 < maxPartitions) ? fdlPos + 1 : 0;
    }

    //==========================================================================
    double sampleRate = 0.0;
    int blockSize = 0;
    int fftSize = 0;
    int spectrumFloats = 0;
    int maxPartitions = 0;

    std::unique_ptr<juce::dsp::FFT> fft;

    const AmbiBinauralFilterBank* bank = nullptr;
    const spatcore::rt::RtSnapshot<rt::MonitorRtParams>* params = nullptr;
    const std::atomic<spatcore::binaural::HeadOrientationSource*>* activeSource = nullptr;

    std::vector<float> fdl;          // [channel][partition][2N]
    std::vector<float> prevBlocks;   // [channel][P] overlap-save history
    std::vector<float> inputStage;   // [channel][P] staging for a full step
    std::vector<float> fftWork, accum;
    std::vector<float> outFifo[2];   // 2P ring per ear
    std::vector<float> outBlock[2];  // this block's output, P long

    // Head compensation (D51/D53). The two coefficient sets are the current
    // and previous rotations, held for the one-block crossfade.
    juce::AudioBuffer<float> rotatedBus, rotationFade;
    float currentRotation[rot::kNumRotationCoeffs] = {};
    float previousRotation[rot::kNumRotationCoeffs] = {};
    bool hasRotation = false;
    bool crossfadeRotation = false;
    float lastYaw = 0.0f, lastPitch = 0.0f, lastRoll = 0.0f;

    int fdlPos = 0;
    int inputFill = 0;
    int outWrite = 0, outRead = 0, outCount = 0;
    juce::uint32 activeEpoch = 0;
    int activePartitions = 0;
    bool wasEnabled = false;
    bool activeThisBlock = false;
    float rampGain = 0.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AmbiBinauralRenderer)
};

} // namespace xoa
