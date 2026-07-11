#pragma once

#include <juce_core/juce_core.h>

#include <vector>

#include "XoaConstants.h"
#include "Helpers/XoaCoordinates.h"
#include "DSP/AmbiSphericalHarmonics.h"
#include "DSP/AmbiVBAP.h"
#include "DSP/TDesignTables.h"

//==============================================================================
// XOA - All-Round Ambisonic Decoding (Zotter-Frank, FR-13). Non-RT / control.
//
// AllRAD = sample the bus onto a dense virtual t-design (a well-conditioned
// SAD at order 10 regardless of the real speaker count), then VBAP each
// virtual source onto the real rig:
//
//   D_allrad[s][c] = sum_v  G_vbap[s][v] * (1/V)(2 l_c + 1) Y_c(x_v)
//
// over the V virtual directions x_v. Imaginary loudspeakers (inserted by the
// triangulation to close coverage gaps) receive gains during the VBAP but are
// DISCARDED from the accumulation, so their energy is simply lost - the
// Zotter-Frank imaginary-speaker treatment.
//
// The returned decode is UNWEIGHTED (no per-order max-rE/basic weighting, no
// global normalization); the caller (AmbiDecoderDesigner::design) applies the
// same right-diagonal weighting and normalization tail it uses for SAD and
// mode-matching, which is exactly what lets dual-band factor uniformly across
// all three decoder families.
//
// If the rig cannot enclose the listener (AmbiVBAP declines), ok is false and
// the designer falls back to SAD.
//==============================================================================

namespace xoa::allrad
{

struct Result
{
    bool ok = false;
    std::vector<double> dPre;   // [numSpeakers * numChannels(order)] row-major, UNWEIGHTED
    int numImaginary = 0;
    juce::StringArray warnings;
};

// Takes raw speaker positions (not decoder::SpeakerLayout) to avoid a circular
// include with AmbiDecoderDesigner.h, which is the sole caller.
inline Result computeUnweighted (const coords::Cartesian* positions, int count, int order)
{
    Result r;
    const int L = count;
    const int K = sh::numChannels (order);
    r.dPre.assign ((size_t) L * K, 0.0);
    if (L < 3)
    {
        r.warnings.add ("AllRAD needs at least 3 speakers.");
        return r;
    }

    auto tri = vbap::triangulate (positions, L);
    r.warnings.addArray (tri.warnings);
    if (! tri.ok)
        return r;   // caller falls back to SAD
    r.numImaginary = tri.numImaginary;

    const int V = tdesign::kCount;
    std::vector<double> gains (tri.dirs.size(), 0.0);
    double shBuf[xoa::kNumSHChannels];

    for (int v = 0; v < V; ++v)
    {
        const coords::Cartesian x { tdesign::kPoints[v][0], tdesign::kPoints[v][1],
                                    tdesign::kPoints[v][2] };
        if (vbap::computeGains (tri, x, gains.data()) < 0)
            continue;   // an enclosing hull covers every direction; guard anyway

        sh::evaluate (x, order, shBuf);
        for (int s = 0; s < L; ++s)   // real speakers only; imaginaries discarded
        {
            const double g = gains[(size_t) s];
            if (g == 0.0)
                continue;
            double* row = &r.dPre[(size_t) s * K];
            for (int c = 0; c < K; ++c)
                row[c] += g * (2.0 * sh::acnToOrder (c) + 1.0) * shBuf[c];
        }
    }

    const double invV = 1.0 / (double) V;
    for (double& d : r.dPre)
        d *= invV;

    r.ok = true;
    return r;
}

} // namespace xoa::allrad
