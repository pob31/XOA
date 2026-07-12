#pragma once

#include <juce_core/juce_core.h>

#include <cmath>
#include <type_traits>

#include "XoaConstants.h"
#include "DSP/AmbiConventions.h"
#include "DSP/AmbiOrderWeights.h"
#include "DSP/AmbiRotation.h"
#include "DSP/AmbiSphericalHarmonics.h"

//==============================================================================
// XOA - trivially-copyable RT snapshots + their control-side composers (WP6).
//
// Two snapshot channels feed AmbiBusAlgorithm through spatcore::rt::RtSnapshot
// (the third, the decode matrix, is DecoderMatrixBuilder's DecoderRtHandle):
//
//   RotationRtState  float copy of the WP4 block-diagonal SO(3) matrix.
//   BusRtParams      the "gather table" - which input channel feeds each bus
//                    channel and at what gain (order adaptation x N3D/SN3D
//                    scaling x FuMa remap, composed in double, cooked to
//                    float) - plus the pre-cooked master gain.
//
// epoch == 0 is the "never published" sentinel on both (RtSnapshot
// zero-initializes T{}): the RT side bypasses rotation / renders silence
// rather than consuming uninitialized state. Publish-before-enable remains
// the caller's job; the sentinel makes ordering violations inaudible, not UB.
//
// Single-writer rule: all composition and publishing happens on the message
// thread (WP9's OSC ingest must marshal through the store there too).
// Mirror (FR-11, deferred past WP6) folds into makeRotationState's output by
// scaling matrix columns with xoa::mirror::signs - this is the one compose
// point, nothing downstream changes.
//==============================================================================

namespace xoa::rt
{

//==============================================================================
struct RotationRtState
{
    /** Block-diagonal rotation coefficients, degree l at rot::blockOffset(l),
        same layout as rot::RotationMatrix but float. */
    float coeffs[rot::kNumRotationCoeffs];

    /** 0 = never published -> RT bypasses rotation (zeroed coeffs are NOT
        identity, so the guard is load-bearing). */
    juce::uint32 epoch = 0;
};

static_assert (std::is_trivially_copyable_v<RotationRtState>,
               "RotationRtState must be a POD for RtSnapshot");

//==============================================================================
/** Values match ids::playbackConvention. */
enum class ContentConvention { sn3d = 0, n3d = 1, fuma = 2 };

struct BusRtParams
{
    /** Input channel feeding bus channel c, -1 = bus channel is silent.
        FuMa content makes this a genuine remap; SN3D/N3D content is the
        identity prefix. */
    int srcChannel[xoa::kNumSHChannels];

    /** Per-bus-channel gain: orderAdaptGains(M, 10) x convention scaling
        (x sqrt2 etc. for FuMa), composed in double, cast to float. */
    float gain[xoa::kNumSHChannels];

    /** Read guard for srcChannel entries (the RT side never reads an input
        channel >= this, whatever the table says). */
    int numInputChannels = 0;

    /** Effective content order M (informational: status line). */
    int contentOrder = 0;

    /** ids::masterGain cooked dB -> linear at publish time. */
    float masterGainLinear = 1.0f;

    /** 0 = never published -> RT renders silence. */
    juce::uint32 epoch = 0;
};

static_assert (std::is_trivially_copyable_v<BusRtParams>,
               "BusRtParams must be a POD for RtSnapshot");

//==============================================================================
/** Side-band POD for the WP8 mono-encoder stage. The encode gains and the NFC
    coefficients ride the live flat-matrix seam (app-owned float arrays handed
    as const float* at prepare, rewritten in place at 50 Hz); this snapshot
    carries only the scalars the RT side needs without tearing: how many source
    rows are live, which sources have NFC on, and the rig radius (informational).

    numSources == 0 is the "stage off" state (monoInputsEnabled off, or no
    stems) - the RT encoder is skipped entirely, so the bus is bit-identical to
    the M1/M2 chain. */
struct EncoderRtParams
{
    int numSources = 0;                    // active encoder inputs (0 = stage off)
    juce::uint64 nfcMask = 0;              // bit i set = input i has NFC enabled
    float referenceRadius = 2.0f;         // rig mean radius (m); design is control-side
    juce::uint32 epoch = 0;               // 0 = never published

    bool nfcEnabled (int i) const noexcept
    {
        return i >= 0 && i < 64 && (nfcMask & (juce::uint64) 1 << i) != 0;
    }
};

static_assert (std::is_trivially_copyable_v<EncoderRtParams>,
               "EncoderRtParams must be a POD for RtSnapshot");
static_assert (xoa::kMaxInputs <= 64, "EncoderRtParams::nfcMask is a 64-bit mask");

//==============================================================================
// Composers (message thread; also compiled by the harness and tests).
//==============================================================================

/** Build the rotation snapshot from the three Config parameters (WP4 pinned
    intrinsic Z-Y'-X'' convention). */
inline RotationRtState makeRotationState (double yawDeg, double pitchDeg, double rollDeg,
                                          juce::uint32 epoch) noexcept
{
    rot::RotationMatrix m;
    rot::buildFromYawPitchRoll (yawDeg, pitchDeg, rollDeg, m);

    RotationRtState s;
    for (int i = 0; i < rot::kNumRotationCoeffs; ++i)
        s.coeffs[i] = static_cast<float> (m.coeffs[i]);
    s.epoch = epoch;
    return s;
}

/** Build the gather table for content of the given order and convention.

    @param overrideOrder    ids::playbackContentOrder; <= 0 means "auto" ->
                            use detectedOrder.
    @param detectedOrder    FilePlayer's channel-count heuristic (or the true
                            order for synthetic sources).
    @param convention       ids::playbackConvention (ContentConvention).
    @param numFileChannels  channels the input source actually delivers.
    @param masterGainDb     ids::masterGain.
    @param warning          optional out: FuMa>3 fallback (PRD sec.9 rejection
                            rule) and missing-channel notes land here.
*/
inline BusRtParams makeBusParams (int overrideOrder, int detectedOrder, int convention,
                                  int numFileChannels, double masterGainDb,
                                  juce::uint32 epoch, juce::String* warning = nullptr)
{
    BusRtParams p;

    const int order = juce::jlimit (0, xoa::kAmbisonicOrder,
                                    overrideOrder > 0 ? overrideOrder : detectedOrder);
    auto conv = static_cast<ContentConvention> (
        juce::jlimit (0, 2, convention));

    // FuMa beyond order 3 is non-standard: refuse the interpretation loudly
    // and fall back to reading the channels as SN3D (PRD sec.9).
    if (conv == ContentConvention::fuma && ! conv::isFumaOrderSupported (order))
    {
        if (warning != nullptr)
            *warning << "FuMa is only defined up to order 3 (got order "
                     << juce::String (order) << "); reading channels as SN3D. ";
        conv = ContentConvention::sn3d;
    }

    const int contentChannels = sh::numChannels (order);
    const int usableChannels  = juce::jmin (contentChannels, numFileChannels,
                                            xoa::kMaxFileChannels);

    if (warning != nullptr && numFileChannels > 0 && usableChannels < contentChannels)
        *warning << "Order " << juce::String (order) << " content expects "
                 << juce::String (contentChannels) << " channels but the source has "
                 << juce::String (numFileChannels) << "; missing channels are silent. ";

    // Order adaptation up/down to the fixed 121-channel bus (FR-7), composed
    // in double with the convention scaling, cast to float once at the end.
    double adapt[xoa::kNumSHChannels];
    weights::orderAdaptGains (order, xoa::kAmbisonicOrder, adapt);

    for (int c = 0; c < xoa::kNumSHChannels; ++c)
    {
        p.srcChannel[c] = -1;
        p.gain[c] = 0.0f;
    }

    if (conv == ContentConvention::fuma)
    {
        const auto& table = conv::fumaToAmbixTable();
        for (int f = 0; f < usableChannels; ++f)
        {
            const auto& entry = table[static_cast<size_t> (f)];
            p.srcChannel[entry.acn] = f;
            p.gain[entry.acn] = static_cast<float> (adapt[entry.acn] * entry.fumaToSn3dGain);
        }
    }
    else
    {
        const bool isN3d = (conv == ContentConvention::n3d);
        for (int c = 0; c < usableChannels; ++c)
        {
            p.srcChannel[c] = c;
            const double scale = isN3d ? conv::n3dToSn3d (sh::acnToOrder (c)) : 1.0;
            p.gain[c] = static_cast<float> (adapt[c] * scale);
        }
    }

    p.numInputChannels = juce::jlimit (0, xoa::kMaxFileChannels, numFileChannels);
    p.contentOrder = order;
    p.masterGainLinear = static_cast<float> (std::pow (10.0, masterGainDb / 20.0));
    p.epoch = epoch;
    return p;
}

} // namespace xoa::rt
