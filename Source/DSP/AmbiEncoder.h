#pragma once

#include <algorithm>
#include <cmath>

#include "XoaConstants.h"
#include "Helpers/XoaCoordinates.h"
#include "DSP/AmbiNFCFilter.h"
#include "DSP/AmbiOrderWeights.h"
#include "DSP/AmbiSphericalHarmonics.h"

//==============================================================================
// XOA - mono-source encoder composer (WP8, FR-5/FR-6). Pure, store-free math:
// one source's parameters -> its [121] encode-coefficient row. Lives in a
// header (like the rt:: composers) so the offline-render harness compiles it
// directly without a ValueTree. AmbiCalculationEngine wraps this with the
// message-thread plumbing; the NFC filtering itself is AmbiNFCFilter.h.
//
// The row is SH(direction) x linear-gain x distance-gain x spread-taper,
// broadcast per order. All math in double; the row is cast to float once (the
// RT-boundary rule). NFC is applied on the RT side per order-lane (see C4), not
// folded into this scalar row.
//==============================================================================

namespace xoa::enc
{

// Distance handling shares the NFC numerical-safety constants (decision 5).
constexpr double kMinRadius  = xoa::nfc::kMinSourceRadius;   // 0.25 m
constexpr double kMaxBoostDb = xoa::nfc::kMaxBoostDb;        // +20 dB gain ceiling

/** One source's control parameters (canonical cartesian meters, dB, degrees). */
struct SourceParams
{
    double x = 1.0, y = 0.0, z = 0.0;
    double gainDb = 0.0;
    double spreadDeg = 0.0;
    bool   mute = false;
};

/** Distance gain law v1 (decision 5): unity at the rig radius, 1/r nearer
    (capped at the boost ceiling), attenuating farther. */
inline double distanceGain (double rSrc, double rRef) noexcept
{
    const double r = std::max (rSrc, kMinRadius);
    const double g = (rRef > 0.0) ? rRef / r : 1.0;
    const double ceiling = std::pow (10.0, kMaxBoostDb / 20.0);
    return std::min (g, ceiling);
}

/** Compose one source's encode row into row121 (kNumSHChannels floats).
    @param sp    the source parameters.
    @param rRef  rig mean radius (m) for the distance-gain reference. */
inline void composeRow (const SourceParams& sp, double rRef, float* row121) noexcept
{
    if (sp.mute)
    {
        for (int c = 0; c < xoa::kNumSHChannels; ++c) row121[c] = 0.0f;
        return;
    }

    const coords::Spherical sph = coords::cartesianToSpherical ({ sp.x, sp.y, sp.z });

    double shGains[xoa::kNumSHChannels];
    sh::evaluate (sph.azimuthDeg, sph.elevationDeg, xoa::kAmbisonicOrder, shGains);

    double perOrder[xoa::kAmbisonicOrder + 1];
    weights::spreadTaper (xoa::kAmbisonicOrder, sp.spreadDeg, perOrder);

    const double lin  = std::pow (10.0, sp.gainDb / 20.0);
    const double dist = distanceGain (sph.radius, rRef);
    const double g    = lin * dist;

    for (int c = 0; c < xoa::kNumSHChannels; ++c)
        row121[c] = static_cast<float> (shGains[c] * g * perOrder[sh::acnToOrder (c)]);
}

/** Source radius (m) from cartesian position - the NFC design input. */
inline double sourceRadius (const SourceParams& sp) noexcept
{
    return std::sqrt (sp.x * sp.x + sp.y * sp.y + sp.z * sp.z);
}

} // namespace xoa::enc
