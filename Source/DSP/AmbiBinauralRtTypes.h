/*
  ==============================================================================

    XOA — tenth-order Ambisonics spatial audio processor.
    AmbiBinauralRtTypes — the RT snapshot PODs for binaural monitoring
    (WP15, D51-D53).

    Same contract as AmbiRtTypes.h: trivially copyable, composed and published
    on the message thread by ONE writer, acquired once per block by the audio
    thread, and `epoch == 0` is the never-published sentinel (RtSnapshot
    zero-initializes T{}) — the RT side must treat it as "monitor off" rather
    than consume uninitialized state.

    Note what is NOT here: the head attitude. Per D53 the RT stage reads the
    active orientation source fresh every block, bypassing this snapshot
    entirely; only the MANUAL fallback attitude travels here, because that one
    genuinely belongs to the slow parameter path.

    This file is part of XOA, released under the GNU General Public License
    v3.0. See LICENSE for details.

  ==============================================================================
*/

#pragma once

#include <juce_core/juce_core.h>

#include <type_traits>

#include "XoaConstants.h"

namespace xoa::rt
{

/** Monitor control state: the enable gate, the pre-cooked gain, and the
    manual head attitude used whenever no tracker has a valid pose. Angles are
    radians in the SPATCORE convention (+yaw right, +pitch up, +roll right ear
    down) — the same representation a tracker delivers, so the RT stage has
    exactly one form to handle (see DSP/AmbiHeadMapping.h). */
struct MonitorRtParams
{
    bool  enabled = false;
    float monitorGainLinear = 1.0f;

    float manualYawRad = 0.0f;
    float manualPitchRad = 0.0f;
    float manualRollRad = 0.0f;

    /** 0 = never published -> the monitor stays silent. */
    juce::uint32 epoch = 0;
};

static_assert (std::is_trivially_copyable_v<MonitorRtParams>,
               "MonitorRtParams must be a POD for RtSnapshot");

/** Handle to the cooked SH→ear filter bank.

    `spectra` points into a buffer owned by AmbiBinauralFilterBank and stays
    valid until the NEXT publish (RT staleness <= 1 block, the same rule
    DecoderRtHandle lives by). Layout, ear-major:

        [ear][channel][partition][2 * fftSize floats]

    in juce::dsp::FFT::performRealOnlyForwardTransform order. */
struct BinauralFilterRtHandle
{
    const float* spectra = nullptr;
    int fftSize = 0;          // N = 2 * blockSize
    int numPartitions = 0;    // K = ceil(firLength / blockSize)
    int blockSize = 0;        // P — the partition size the bank was cooked for
    int firLength = 0;
    juce::uint32 epoch = 0;   // 0 = no bank -> silence

    /** Floats per partition spectrum. */
    int spectrumFloats() const noexcept { return 2 * fftSize; }

    const float* spectrum (int ear, int channel, int partition) const noexcept
    {
        return spectra
             + ((size_t) ((ear * xoa::kNumSHChannels + channel) * numPartitions + partition))
               * (size_t) spectrumFloats();
    }
};

static_assert (std::is_trivially_copyable_v<BinauralFilterRtHandle>,
               "BinauralFilterRtHandle must be a POD for RtSnapshot");

} // namespace xoa::rt
