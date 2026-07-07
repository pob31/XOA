#pragma once

#include <juce_core/juce_core.h>

#include <cmath>

#include "XoaConstants.h"
#include "Helpers/XoaCoordinates.h"

//==============================================================================
// XOA — real spherical-harmonic evaluation.
//
// Convention (AmbiX, fixed for the whole project):
//   * ACN channel ordering:   acn = l*(l+1) + m
//   * SN3D normalization:     Schmidt semi-normalized, NO Condon-Shortley phase
//   * Frame (from XoaCoordinates): +X front, +Y left, +Z up;
//                                  azimuth CCW from +X, elevation from horizon.
//
// The real SH evaluated here are
//   Y_{l,m}(az, el) = P̄_l^{|m|}(sin el) * ( cos(|m| az)  for m >= 0
//                                            sin(|m| az)  for m <  0 )
// where P̄ is the Schmidt semi-normalized associated Legendre function. SN3D
// *is* Schmidt semi-normalization, so there is no post-scaling step. Direct
// consequences (all asserted in the tests): Y_{0,0} == 1, |Y_{l,m}| <= 1, and
// the SN3D addition theorem sum_m Y_{l,m}(d)^2 == 1 for every order l and
// direction d.
//
// Naming: Ambisonics "order" == the SH degree. Code uses l = order, m = degree
// with the Ambisonics meaning (l in [0, order], m in [-l, l]).
//
// Legendre recurrence references: the Schmidt quasi-normalized form of Winch,
// Ivers, Turner & Stening, "Geomagnetism and Schmidt quasi-normalization",
// Geophys. J. Int. 160 (2005), §3; identical in shape to the fully-normalized
// recurrences of Holmes & Featherstone, J. Geodesy 76 (2002), eqs (11)-(13)
// (differing only by the missing sqrt(2l+1)); Zotter & Frank, "Ambisonics"
// (Springer 2019), App. A.2 gives the N3D twin.
//==============================================================================

namespace xoa::sh
{

//==============================================================================
// Indexing (all constexpr, exact by construction, order-generic).
//==============================================================================

/** Number of SH channels for a given order: (order+1)^2. */
constexpr int numChannels (int order) noexcept
{
    return (order + 1) * (order + 1);
}

/** ACN index for (order l, degree m). */
constexpr int acn (int l, int m) noexcept
{
    // jassert is not constexpr-usable in C++17; bounds are covered by tests.
    return l * (l + 1) + m;
}

/** The order l that owns a given ACN index. Integer-exact loop (no sqrt, so it
    stays usable in constexpr contexts and never rounds). */
constexpr int acnToOrder (int acnIndex) noexcept
{
    int l = 0;
    while ((l + 1) * (l + 1) <= acnIndex)
        ++l;
    return l;
}

/** The degree m of a given ACN index. */
constexpr int acnToDegree (int acnIndex) noexcept
{
    const int l = acnToOrder (acnIndex);
    return acnIndex - l * (l + 1);
}

/** Packed index for the Schmidt Legendre table (0 <= m <= l), triangular. */
constexpr int legendreIndex (int l, int m) noexcept
{
    return l * (l + 1) / 2 + m;
}

/** Number of packed Legendre entries for a given order. */
constexpr int numLegendreEntries (int order) noexcept
{
    return (order + 1) * (order + 2) / 2;
}

/** Upper bound for the Legendre scratch buffer at the project order. */
constexpr int kMaxLegendreEntries = numLegendreEntries (xoa::kAmbisonicOrder);   // 66

//==============================================================================
// Schmidt semi-normalized associated Legendre functions (the testability seam).
//==============================================================================

/** Evaluate P̄_l^m(x) for all 0 <= m <= l <= order into outPacked (indexed by
    legendreIndex). Schmidt semi-normalized, NO Condon-Shortley phase.

    @param x  sin(elevation) = cos(colatitude), the recurrence argument.
    @param u  cos(elevation) = sin(colatitude) >= 0, multiplies the sectoral
              seeds. Passed explicitly so elevation callers hand over cos(el)
              directly instead of sqrt(1 - x*x), which loses precision near the
              poles.

    Recurrence (see header references):
      seed      P̄_0^0 = 1
      sectoral  P̄_m^m = c_m * u * P̄_{m-1}^{m-1},  c_1 = 1, c_m = sqrt((2m-1)/(2m)) for m>=2
                        (the Schmidt sqrt(2 - δ_m0) factor cancels the unnormalized
                         1/sqrt(2) exactly once, at m = 1 — do not "fix" c_1)
      upward    P̄_l^m = a_lm * x * P̄_{l-1}^m - b_lm * P̄_{l-2}^m
                        a_lm = (2l-1) / sqrt(l^2 - m^2)
                        b_lm = sqrt(((l-1)^2 - m^2) / (l^2 - m^2))
      With P̄_{m-1}^m := 0 the upward step subsumes the first off-sectoral term
      (a_{m+1,m} = sqrt(2m+1), b_{m+1,m} = 0), so one loop l = m+1..order suffices.
    Spot checks: P̄_1^0 = x, P̄_2^0 = (3x^2 - 1)/2, P̄_1^1 = u, P̄_2^2 = (sqrt3/2) u^2,
                 P̄_3^1 = sqrt(3/8) (5x^2 - 1) u.
    The coefficients are identical for m = 0 and m > 0. */
inline void evaluateSchmidtLegendre (double x, double u, int order, double* outPacked) noexcept
{
    jassert (order >= 0 && order <= xoa::kAmbisonicOrder);
    jassert (outPacked != nullptr);
    // Release-safe guard: order > kAmbisonicOrder would index past a caller's
    // project-sized buffer (and past the scratch buffer in evaluateRadians).
    // A contract violation no-ops rather than risking out-of-bounds writes.
    if (order < 0 || order > xoa::kAmbisonicOrder)
        return;

    outPacked[legendreIndex (0, 0)] = 1.0;
    if (order == 0)
        return;

    // Sectoral diagonal P̄_m^m.
    double diagonal = 1.0;   // P̄_0^0
    for (int m = 1; m <= order; ++m)
    {
        const double c = (m == 1) ? 1.0 : std::sqrt ((2.0 * m - 1.0) / (2.0 * m));
        diagonal = c * u * diagonal;
        outPacked[legendreIndex (m, m)] = diagonal;
    }

    // Upward recurrence in l at fixed m.
    for (int m = 0; m < order; ++m)
    {
        double pMinus2 = 0.0;                              // P̄_{m-1}^m := 0
        double pMinus1 = outPacked[legendreIndex (m, m)];  // P̄_m^m
        for (int l = m + 1; l <= order; ++l)
        {
            const double l2m2 = static_cast<double> (l * l - m * m);
            const double a = (2.0 * l - 1.0) / std::sqrt (l2m2);
            const double b = std::sqrt ((static_cast<double> ((l - 1) * (l - 1) - m * m)) / l2m2);
            const double p = a * x * pMinus1 - b * pMinus2;
            outPacked[legendreIndex (l, m)] = p;
            pMinus2 = pMinus1;
            pMinus1 = p;
        }
    }
}

/** Convenience overload deriving u = cos(el) from x = sin(el). Prefer the
    two-argument form when cos(el) is already in hand. */
inline void evaluateSchmidtLegendre (double x, int order, double* outPacked) noexcept
{
    evaluateSchmidtLegendre (x, std::sqrt (std::max (0.0, 1.0 - x * x)), order, outPacked);
}

//==============================================================================
// Real SH evaluation.
//==============================================================================

/** Evaluate the real SH basis at (azimuth, elevation) in RADIANS into out,
    which must hold numChannels(order) doubles. Allocation-free (fixed scratch),
    noexcept.

    Preconditions (jassert): order in [0, kAmbisonicOrder]; az in [-2pi, 2pi];
    el in [-pi/2, pi/2] (u = cos(el) >= 0 is assumed). The degree-input
    `evaluate` overload enforces these by normalizing first. */
inline void evaluateRadians (double azimuthRad, double elevationRad, int order, double* out) noexcept
{
    jassert (order >= 0 && order <= xoa::kAmbisonicOrder);
    jassert (out != nullptr);
    jassert (azimuthRad >= -juce::MathConstants<double>::twoPi
             && azimuthRad <= juce::MathConstants<double>::twoPi);
    jassert (elevationRad >= -juce::MathConstants<double>::halfPi
             && elevationRad <= juce::MathConstants<double>::halfPi);
    if (order < 0 || order > xoa::kAmbisonicOrder)
        return;   // release-safe: never write past the caller's buffer

    const double x = std::sin (elevationRad);
    const double u = std::cos (elevationRad);

    double p[kMaxLegendreEntries];
    evaluateSchmidtLegendre (x, u, order, p);

    // m = 0 column (cos(0) = 1).
    for (int l = 0; l <= order; ++l)
        out[acn (l, 0)] = p[legendreIndex (l, 0)];

    // m > 0: cos into the +m slot, sin into the -m slot. Azimuth terms taken
    // directly (no Chebyshev recurrence). The degrees-input evaluate() path
    // normalizes az to (-pi, pi], so |m*az| <= 10pi there; even at the raw
    // radian precondition bound (|az| <= 2pi -> |m*az| <= 20pi) libm sin/cos
    // argument reduction stays exact to < 1 ulp, so no fmod is needed.
    for (int m = 1; m <= order; ++m)
    {
        const double cosM = std::cos (m * azimuthRad);
        const double sinM = std::sin (m * azimuthRad);
        for (int l = m; l <= order; ++l)
        {
            const double leg = p[legendreIndex (l, m)];
            out[acn (l, m)]  = leg * cosM;
            out[acn (l, -m)] = leg * sinM;
        }
    }
}

/** Evaluate the real SH basis at (azimuth, elevation) in DEGREES. Normalizes
    azimuth to (-180, 180] and clamps elevation to [-90, 90] before converting,
    so the radian preconditions always hold. */
inline void evaluate (double azimuthDeg, double elevationDeg, int order, double* out) noexcept
{
    const double az = juce::degreesToRadians (coords::normalizeAzimuthDegrees (azimuthDeg));
    const double el = juce::degreesToRadians (juce::jlimit (-90.0, 90.0, elevationDeg));
    evaluateRadians (az, el, order, out);
}

/** Evaluate for a spherical direction (radius ignored). */
inline void evaluate (const coords::Spherical& s, int order, double* out) noexcept
{
    evaluate (s.azimuthDeg, s.elevationDeg, order, out);
}

/** Evaluate for a cartesian direction; the vector is normalized to a
    direction. r == 0 has no direction and maps to front (+X), matching the
    XoaCoordinates convention. */
inline void evaluate (const coords::Cartesian& c, int order, double* out) noexcept
{
    evaluate (coords::cartesianToSpherical (c), order, out);
}

} // namespace xoa::sh
