#pragma once

#include <juce_core/juce_core.h>

#include <vector>

#include "spatcore/rt/RtSnapshot.h"

#include "XoaConstants.h"
#include "DSP/AmbiDecoderDesigner.h"
#include "DSP/AmbiSphericalHarmonics.h"
#include "Parameters/XoaParameterIDs.h"
#include "Parameters/XoaValueTreeState.h"

//==============================================================================
// XOA - non-RT decoder build + atomic hot-swap plumbing (FR-17).
//
// The message thread rebuilds the decoder matrix (double master) on layout /
// decoder-parameter change, converts it to a float RT copy zero-padded to the
// fixed [L x 121] bus width (so the RT GEMM shape never branches on design
// order, PRD sec.5), double-buffers it, and publishes a POD handle through
// spatcore::rt::RtSnapshot. The RT thread acquires the handle once per block.
//
// WP5 ships the synchronous rebuild()+publish() pair (headless-testable). The
// store-listener + juce::Timer debounce that calls this on parameter change is
// WP6; the contract it must honour is at most one publish() per audio-block
// interval, so the previously published pointer stays valid until the next
// publish (RT staleness <= 1 block).
//==============================================================================

namespace xoa
{

struct DecoderRtHandle
{
    const float* matrix = nullptr;   // [numSpeakers * kNumSHChannels] row-major, zero-padded
    int numSpeakers = 0;
    int designOrder = 0;
    juce::uint32 epoch = 0;

    // Dual-band (FR-14): when set, the RT bus splits at crossoverHz and scales
    // the HF band per channel by hfGain[c] before the single decode GEMM.
    bool dualBand = false;
    float crossoverHz = 400.0f;
    float hfGain[xoa::kNumSHChannels] = {};   // valid 0..kNumSHChannels-1; 1.0 beyond designOrder
};

static_assert (std::is_trivially_copyable_v<DecoderRtHandle>,
               "DecoderRtHandle must be a POD for RtSnapshot");

class DecoderMatrixBuilder
{
public:
    DecoderMatrixBuilder() = default;

    //==========================================================================
    // Store readers (also free-standing for tests).
    //==========================================================================

    static decoder::SpeakerLayout layoutFromStore (const XoaValueTreeState& state)
    {
        decoder::SpeakerLayout layout;
        layout.count = juce::jmin (state.getNumSpeakers(), xoa::kMaxSpeakers);
        for (int s = 0; s < layout.count; ++s)
            layout.positions[s] = {
                state.getFloatParameter (ids::speakerPositionX, s),
                state.getFloatParameter (ids::speakerPositionY, s),
                state.getFloatParameter (ids::speakerPositionZ, s)
            };
        return layout;
    }

    static decoder::DesignOptions optionsFromStore (const XoaValueTreeState& state)
    {
        decoder::DesignOptions o;
        o.type = static_cast<decoder::Type> (state.getIntParameter (ids::decoderType));
        o.weighting = static_cast<decoder::Weighting> (state.getIntParameter (ids::decoderWeighting));
        o.normalization = static_cast<decoder::NormalizationMode> (state.getIntParameter (ids::decoderNormalization));
        o.requestedOrder = xoa::kAmbisonicOrder;
        o.dualBand = static_cast<bool> (state.getParameter (ids::decoderDualBandEnabled));
        o.crossoverHz = state.getFloatParameter (ids::decoderCrossoverFrequency);
        return o;
    }

    //==========================================================================
    // Build + publish (message thread).
    //==========================================================================

    /** Adopt an already-computed design (e.g. from a background worker) into
        the inactive buffer. The design() call is the slow part; splitting it
        out lets the async rebuild run design() off the message thread and only
        marshal this cheap buffer-fill back. Message thread only. */
    void adoptResult (decoder::DesignResult result)
    {
        lastResult = std::move (result);
        master = lastResult.matrix;

        // Fill the inactive float buffer, zero-padded to the full bus width.
        const int writeIndex = 1 - activeIndex;
        auto& buf = rtBuffers[(size_t) writeIndex];
        const int L = master.numSpeakers;
        const int K = sh::numChannels (master.order);
        buf.assign ((size_t) L * xoa::kNumSHChannels, 0.0f);
        for (int s = 0; s < L; ++s)
            for (int c = 0; c < K; ++c)
                buf[(size_t) s * xoa::kNumSHChannels + c] = (float) master.at (s, c);

        pendingNumSpeakers = L;
        pendingOrder = master.order;

        // Dual-band HF diagonal, cooked to float and zero-padded to the bus
        // width (1.0 beyond the design order so unused bus channels pass HF
        // through unchanged; they carry no content anyway).
        pendingDualBand = lastResult.dualBand;
        pendingCrossoverHz = (float) lastResult.crossoverHz;
        for (int c = 0; c < xoa::kNumSHChannels; ++c)
            pendingHfGain[c] = (lastResult.dualBand && c < (int) lastResult.hfDiagonal.size())
                                   ? (float) lastResult.hfDiagonal[(size_t) c]
                                   : 1.0f;
    }

    decoder::DesignResult rebuild (const decoder::SpeakerLayout& layout,
                                   const decoder::DesignOptions& opts)
    {
        adoptResult (decoder::design (layout, opts));
        return lastResult;
    }

    decoder::DesignResult rebuild (const XoaValueTreeState& state)
    {
        return rebuild (layoutFromStore (state), optionsFromStore (state));
    }

    /** Flip to the freshly built buffer and publish the handle. */
    void publish()
    {
        activeIndex = 1 - activeIndex;
        ++epoch;
        DecoderRtHandle h;
        h.matrix = rtBuffers[(size_t) activeIndex].data();
        h.numSpeakers = pendingNumSpeakers;
        h.designOrder = pendingOrder;
        h.epoch = epoch;
        h.dualBand = pendingDualBand;
        h.crossoverHz = pendingCrossoverHz;
        for (int c = 0; c < xoa::kNumSHChannels; ++c)
            h.hfGain[c] = pendingHfGain[c];
        snapshot.publish (h);
    }

    /** RT thread: current handle (allocation-free POD copy). */
    DecoderRtHandle acquire() const { return snapshot.acquire(); }

    const decoder::DecoderMatrix& masterMatrix() const { return master; }
    const decoder::DesignResult&  lastDesignResult() const { return lastResult; }

private:
    decoder::DecoderMatrix master;
    decoder::DesignResult  lastResult;
    std::vector<float> rtBuffers[2];
    int activeIndex = 0;
    int pendingNumSpeakers = 0, pendingOrder = 0;
    bool pendingDualBand = false;
    float pendingCrossoverHz = 400.0f;
    float pendingHfGain[xoa::kNumSHChannels] = {};
    juce::uint32 epoch = 0;
    spatcore::rt::RtSnapshot<DecoderRtHandle> snapshot;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DecoderMatrixBuilder)
};

} // namespace xoa
