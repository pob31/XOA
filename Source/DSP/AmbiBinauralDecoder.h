/*
  ==============================================================================

    XOA — tenth-order Ambisonics spatial audio processor.
    AmbiBinauralDecoder — designs the static SH→ear FIR bank that turns the
    121-channel SH bus into a binaural pair (WP15, D51).

    This is the control-side solve: it runs once per HRTF set (on a background
    worker, never on the audio thread) and produces 2 × 121 FIRs. The RT stage
    then only convolves — no per-direction HRIR selection, no crossfading, no
    per-source delay lines. Head motion becomes an SH rotation upstream, which
    is why the bank can be static.

    Input is spatcore's `HrirDatabase` (SofaLoader's bake): a uniform
    72 az × 19 el grid of TIME-ALIGNED HRIRs plus the per-ear onset delays
    that were stripped out of them. Reuse stops there — `CookedHrirSet`'s
    per-direction partition cook is the wrong shape for an SH bank.

    Modes:
      sampling — SH projection ("virtual speaker") decode. Simple, exact to
                 implement, and independently reproducible in Python, which
                 makes it the golden-anchored reference mode.
      magls    — magnitude-least-squares above a crossover (stage 7).

    This file is part of XOA, released under the GNU General Public License
    v3.0. See LICENSE for details.

  ==============================================================================
*/

#pragma once

#include <juce_core/juce_core.h>

#include "spatcore/binaural/HrirSet.h"

#include "XoaConstants.h"

#include <vector>

namespace xoa::binaural
{

/** Which solve produces the bank. Values are persisted (binauralDecoderMode)
    — keep them stable. */
enum class DecoderMode : int
{
    sampling = 0,   // SH projection over the HRIR grid
    magls    = 1,   // magnitude least-squares above the crossover (stage 7)
};

struct BinauralDesignOptions
{
    DecoderMode mode = DecoderMode::sampling;
    double magLsCrossoverHz = 2000.0;   // stage 7; ignored by `sampling`
    bool diffuseFieldEq = false;        // reserved, not implemented
};

/** The designed bank: `firs` holds [ear][channel][tap], ear-major, with
    `firLength` taps per filter. Empty (isValid() == false) on failure, with
    `warning` explaining why. */
struct BinauralDesignResult
{
    std::vector<float> firs;
    int firLength = 0;
    double sampleRate = 0.0;
    juce::String warning;

    bool isValid() const noexcept
    {
        return firLength > 0
            && firs.size() == (size_t) firLength * 2 * (size_t) xoa::kNumSHChannels;
    }

    const float* fir (int ear, int channel) const noexcept
    {
        return firs.data()
             + ((size_t) ear * (size_t) xoa::kNumSHChannels + (size_t) channel)
               * (size_t) firLength;
    }

    float* fir (int ear, int channel) noexcept
    {
        return firs.data()
             + ((size_t) ear * (size_t) xoa::kNumSHChannels + (size_t) channel)
               * (size_t) firLength;
    }
};

/** Constant lead built into every designed filter, in samples.

    The ITD is re-applied with a windowed-sinc fractional delay, which rings
    backward as well as forward; leading every filter by the kernel's
    half-width keeps that pre-ring from being clipped at tap 0 (an
    asymmetric clip would corrupt the interaural level difference). It is the
    same for both ears, so nothing interaural moves — it is simply 16 samples
    (0.33 ms at 48 kHz) of monitor latency, which callers can report but need
    not compensate. */
constexpr int kBinauralFilterLeadSamples = 16;

/** Design the SH→ear bank from a baked HRIR grid. Allocates and takes tens of
    milliseconds at order 10 — background thread only. */
BinauralDesignResult designShFilters (const spatcore::binaural::HrirDatabase& db,
                                      const BinauralDesignOptions& options = {});

//==============================================================================
// Grid helpers — shared with the tests and the reference generator so the
// conventions exist in exactly one place.
//==============================================================================

/** Grid azimuth/elevation of a (az, el) index pair, in the SPATCORE head
    frame: azimuth 0 = front, POSITIVE = the listener's RIGHT; elevation
    positive = up (HrirSet.h). */
inline void gridDirectionDeg (int azIndex, int elIndex, double& azDeg, double& elDeg) noexcept
{
    using Db = spatcore::binaural::HrirDatabase;
    azDeg = (double) azIndex * (double) Db::kAzStepDeg;
    elDeg = -90.0 + (double) elIndex * (double) Db::kElStepDeg;
}

/** Convert a spatcore head-frame azimuth to XOA's soundfield azimuth.

    THE silent-error spot of this file: spatcore's HRIR grid measures azimuth
    positive to the listener's RIGHT (SOFA vertical-polar), while XOA's SH
    basis measures it counter-clockwise from +X with +Y = LEFT
    (Helpers/XoaCoordinates.h). The two therefore run in opposite directions
    and the mapping is a negation. Elevation agrees (positive = up). */
inline double gridAzToXoaAzDeg (double gridAzDeg) noexcept { return -gridAzDeg; }

/** Solid angle of one grid cell, steradians.

    The grid is uniform in azimuth and elevation, so a naive cos(el)·Δel·Δaz
    Riemann weight would both misweight the rings and hand the poles zero.
    Instead each cell gets its EXACT area, the zone between its elevation
    bounds split across the azimuth steps: Δaz · (sin(elHi) − sin(elLo)). The
    pole rows are half-height caps whose 72 samples are all the same physical
    point, so the cap area is shared equally between them. Summed over the
    grid these weights give exactly 4π — a true partition of the sphere. */
inline double gridCellSolidAngle (int elIndex) noexcept
{
    using Db = spatcore::binaural::HrirDatabase;
    const double stepRad = juce::degreesToRadians ((double) Db::kElStepDeg);
    const double elRad = juce::degreesToRadians (-90.0 + (double) elIndex
                                                 * (double) Db::kElStepDeg);
    const double halfStep = 0.5 * stepRad;
    const double lo = juce::jmax (-juce::MathConstants<double>::halfPi, elRad - halfStep);
    const double hi = juce::jmin ( juce::MathConstants<double>::halfPi, elRad + halfStep);
    const double zoneArea = (std::sin (hi) - std::sin (lo))
                          * juce::MathConstants<double>::twoPi;
    return zoneArea / (double) Db::kNumAz;
}

} // namespace xoa::binaural
