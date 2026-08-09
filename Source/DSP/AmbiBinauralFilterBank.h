/*
  ==============================================================================

    XOA — tenth-order Ambisonics spatial audio processor.
    AmbiBinauralFilterBank — FFT-bakes a designed SH→ear bank into partition
    spectra and publishes it to the audio thread (WP15, D51).

    The same double-buffer + RtSnapshot hot-swap DecoderMatrixBuilder uses:
    cook() fills the INACTIVE buffer, publish() flips and stamps a new epoch,
    so the pointer the RT side last acquired stays valid until the next
    publish (staleness <= 1 block). Nothing is ever freed on the audio thread.

    Cooking is uniform-partitioned overlap-save: partition size P = the device
    block size, transform length N = 2P, K = ceil(firLength / P) partitions
    per filter. Re-cook is cheap and happens on a block-size change; a
    sample-rate change needs a full redesign instead (the HRIRs themselves are
    resampled at load).

    This file is part of XOA, released under the GNU General Public License
    v3.0. See LICENSE for details.

  ==============================================================================
*/

#pragma once

#include <juce_dsp/juce_dsp.h>

#include <vector>

#include "spatcore/rt/RtSnapshot.h"

#include "DSP/AmbiBinauralDecoder.h"
#include "DSP/AmbiBinauralRtTypes.h"
#include "XoaConstants.h"

namespace xoa
{

class AmbiBinauralFilterBank
{
public:
    AmbiBinauralFilterBank() = default;

    /** Message thread / worker: FFT-bake `design` for `blockSize` into the
        inactive buffer. Returns false (and leaves the published bank alone)
        if the design or block size is unusable. Allocates. */
    bool cook (const binaural::BinauralDesignResult& design, int blockSize)
    {
        if (! design.isValid() || blockSize < 4)
            return false;

        int order = 1;
        while ((1 << order) < 2 * blockSize)
            ++order;

        const int fft = 1 << order;                 // N (>= 2P)
        const int partitions = (design.firLength + blockSize - 1) / blockSize;
        const int floatsPerSpectrum = 2 * fft;

        const int writeIndex = 1 - activeIndex;
        auto& buf = buffers[(size_t) writeIndex];
        buf.assign ((size_t) 2 * xoa::kNumSHChannels * partitions * floatsPerSpectrum, 0.0f);

        juce::dsp::FFT transform (order);
        std::vector<float> work ((size_t) floatsPerSpectrum, 0.0f);

        for (int ear = 0; ear < 2; ++ear)
        {
            for (int c = 0; c < xoa::kNumSHChannels; ++c)
            {
                const float* fir = design.fir (ear, c);
                for (int k = 0; k < partitions; ++k)
                {
                    std::fill (work.begin(), work.end(), 0.0f);
                    const int start = k * blockSize;
                    const int count = juce::jmin (blockSize, design.firLength - start);
                    for (int i = 0; i < count; ++i)
                        work[(size_t) i] = fir[start + i];

                    transform.performRealOnlyForwardTransform (work.data());

                    float* dst = buf.data()
                               + ((size_t) ((ear * xoa::kNumSHChannels + c) * partitions + k))
                                 * (size_t) floatsPerSpectrum;
                    std::copy (work.begin(), work.end(), dst);
                }
            }
        }

        pending.spectra = buf.data();
        pending.fftSize = fft;
        pending.numPartitions = partitions;
        pending.blockSize = blockSize;
        pending.firLength = design.firLength;
        return true;
    }

    /** Flip to the freshly cooked buffer and publish. Message thread. */
    void publish()
    {
        activeIndex = 1 - activeIndex;
        ++epoch;

        auto handle = pending;
        handle.spectra = buffers[(size_t) activeIndex].data();
        handle.epoch = epoch;
        snapshot.publish (handle);
        published = handle;
    }

    /** Publish an empty bank — the RT stage then renders silence rather than
        reading a bank that no longer matches the device. Message thread. */
    void publishEmpty()
    {
        ++epoch;
        rt::BinauralFilterRtHandle handle;   // epoch 0 fields, spectra == nullptr
        snapshot.publish (handle);
        published = handle;
    }

    /** RT thread: allocation-free POD copy. */
    rt::BinauralFilterRtHandle acquire() const { return snapshot.acquire(); }

    /** Message thread: what was last published (for UI / tests). */
    const rt::BinauralFilterRtHandle& lastPublished() const noexcept { return published; }

private:
    std::vector<float> buffers[2];
    int activeIndex = 0;
    juce::uint32 epoch = 0;

    rt::BinauralFilterRtHandle pending;    // shape of the cooked-but-unpublished buffer
    rt::BinauralFilterRtHandle published;
    spatcore::rt::RtSnapshot<rt::BinauralFilterRtHandle> snapshot;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AmbiBinauralFilterBank)
};

} // namespace xoa
