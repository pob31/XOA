#pragma once

#include <juce_core/juce_core.h>

#include <cmath>

#include "XoaConstants.h"
#include "DSP/AmbiSphericalHarmonics.h"

//==============================================================================
// XOA — soundfield mirror planes: pure per-channel sign flips (FR-11 half).
//
// Derivations (real SH, ACN/SN3D; az CCW from +X, el from horizon):
//   leftRight  (x,y,z) -> (x,-y,z):  az -> -az.
//              cos(m·(-az)) = cos(m·az); sin(m·(-az)) = -sin(m·az)
//              => m < 0 (sin) channels negate, m >= 0 unchanged.
//   frontBack  (x,y,z) -> (-x,y,z):  az -> 180deg - az.
//              cos(m(pi-az)) = (-1)^m cos(m·az); sin(m(pi-az)) = (-1)^{m+1} sin(m·az)
//              => (-1)^{|m|} on m >= 0, (-1)^{|m|+1} on m < 0.
//   upDown     (x,y,z) -> (x,y,-z):  el -> -el.
//              Associated-Legendre parity Pbar_l^m(-x) = (-1)^{l+m} Pbar_l^m(x)
//              => (-1)^{l+|m|} on every channel. (SN3D normalization is
//              positive, so parity is unaffected.)
//
// Cross-checks (tested in R8): leftRight ∘ frontBack sign = (-1)^{|m|} on all
// channels = the az+180deg relation = a 180deg yaw ROTATION; the triple
// upDown ∘ leftRight ∘ frontBack = (-1)^l = the antipode parity. A mirror
// composed with a rotation is NOT a rotation (det = -1) — mirrors stay a
// separate processing stage; the chain order is WP6's decision. (A WP6
// optimization may fold these signs into the rotation matrix columns.)
//==============================================================================

namespace xoa::mirror
{

enum class Plane
{
    leftRight,   // y -> -y
    frontBack,   // x -> -x
    upDown       // z -> -z
};

/** Sign for one channel (order l, degree m). */
inline double channelSign (Plane plane, int l, int m) noexcept
{
    const int am = std::abs (m);
    switch (plane)
    {
        case Plane::leftRight: return m < 0 ? -1.0 : 1.0;
        case Plane::frontBack: return ((am + (m < 0 ? 1 : 0)) % 2 == 0) ? 1.0 : -1.0;
        case Plane::upDown:    return ((l + am) % 2 == 0) ? 1.0 : -1.0;
    }
    return 1.0;
}

/** Fill numChannels(order) entries with the plane's per-channel signs.
    Release-safe: order outside [0, kAmbisonicOrder] no-ops (jassert). */
inline void signs (Plane plane, int order, double* perChannel) noexcept
{
    jassert (order >= 0 && order <= xoa::kAmbisonicOrder);
    jassert (perChannel != nullptr);
    if (order < 0 || order > xoa::kAmbisonicOrder)
        return;

    for (int c = 0; c < sh::numChannels (order); ++c)
        perChannel[c] = channelSign (plane, sh::acnToOrder (c), sh::acnToDegree (c));
}

/** out[c] = sign[c] * in[c]; pure diagonal, in == out allowed. */
inline void apply (Plane plane, int order, const double* in, double* out) noexcept
{
    jassert (order >= 0 && order <= xoa::kAmbisonicOrder);
    jassert (in != nullptr && out != nullptr);
    if (order < 0 || order > xoa::kAmbisonicOrder)
        return;

    for (int c = 0; c < sh::numChannels (order); ++c)
        out[c] = channelSign (plane, sh::acnToOrder (c), sh::acnToDegree (c)) * in[c];
}

} // namespace xoa::mirror
