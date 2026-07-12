#pragma once

#include <juce_core/juce_core.h>

#include <cmath>

#include "XoaConstants.h"
#include "DSP/AmbiSphericalHarmonics.h"

//==============================================================================
// XOA — per-order weighting and order adaptation (FR-3, FR-7 math half).
//
// A "weight family" is a vector g_0..g_N (one gain per order l), applied to the
// SH bus by broadcasting each g_l across that order's 2l+1 channels. Families:
//   * basic    g_l = 1                       (velocity / matching decode)
//   * max-rE   g_l = P_l(r_E), r_E = largest root of P_{N+1}   (energy decode)
//   * in-phase g_l = N!(N+1)! / ((N+l+1)!(N-l)!)
//
// Weights are raw (g_0 = 1, W preserved). Energy / loudness normalization is a
// decoder concern (WP5/WP7), deliberately NOT baked in here.
//
// FR-7 order adaptation: expand a per-order vector to per-channel, upmix by
// zero-padding, downmix by truncation + target-order max-rE re-weighting.
//
// References: F. Zotter & M. Frank, "Ambisonics" (Springer 2019), ch. 4
// (max-rE, in-phase, the r_E ~ cos(137.9°/(N+1.51)) approximation).
//==============================================================================

namespace xoa::weights
{

/** Legendre polynomial P_l(x) via the standard three-term recurrence
    P_0 = 1, P_1 = x, l·P_l = (2l-1)·x·P_{l-1} - (l-1)·P_{l-2}.
    Public: WP5 rV/rE analysis and WP8 spread reuse it. */
inline double legendrePolynomial (int l, double x) noexcept
{
    jassert (l >= 0);
    if (l == 0) return 1.0;
    if (l == 1) return x;

    double pMinus2 = 1.0;   // P_0
    double pMinus1 = x;     // P_1
    double p = x;
    for (int k = 2; k <= l; ++k)
    {
        p = ((2.0 * k - 1.0) * x * pMinus1 - (k - 1.0) * pMinus2) / k;
        pMinus2 = pMinus1;
        pMinus1 = p;
    }
    return p;
}

/** The max-rE characteristic value r_E for a design order: the largest root of
    the Legendre polynomial P_{order+1}. Exposed for tests and the WP8 spread
    tapering.

    Seed from the Zotter-Frank closed form cos(137.9°/(N+1.51)), then Newton on
    P_{N+1}. The largest root sits strictly inside (0, 1) (r_E ~ 0.9782 at
    N = 10) on the last monotone-convex branch of P_{N+1}, so Newton converges
    cleanly for all N <= 10; the goldens verify every order regardless. */
inline double maxReCosine (int order) noexcept
{
    jassert (order >= 0);
    if (order == 0)
        return 1.0;   // Order 0 is omnidirectional: r_E is undefined. Return 1.0 so any
                      // P_l(r_E) broadcast degenerates to unit (basic) weights.

    const int np1 = order + 1;
    double x = std::cos (juce::degreesToRadians (137.9 / (order + 1.51)));

    for (int iter = 0; iter < 50; ++iter)
    {
        const double pN   = legendrePolynomial (order, x);
        const double pNp1 = legendrePolynomial (np1, x);
        // P'_{N+1}(x) = (N+1)·(P_N(x) - x·P_{N+1}(x)) / (1 - x^2)
        const double denom = 1.0 - x * x;
        if (denom <= 0.0)
            break;
        const double deriv = np1 * (pN - x * pNp1) / denom;
        if (deriv == 0.0)
            break;
        const double dx = pNp1 / deriv;
        x = juce::jlimit (1.0e-9, 1.0 - 1.0e-12, x - dx);
        if (std::abs (dx) <= 1.0e-15)
            return x;
    }
    // Should always converge for N <= 10; the tests would catch a regression.
    return x;
}

//==============================================================================
// Weight families — each writes order+1 doubles, g_0 = 1.
//==============================================================================

/** Basic (velocity) weights: all ones. */
inline void basic (int order, double* perOrder) noexcept
{
    for (int l = 0; l <= order; ++l)
        perOrder[l] = 1.0;
}

/** max-rE (energy) weights: g_l = P_l(r_E). */
inline void maxRe (int order, double* perOrder) noexcept
{
    const double rE = maxReCosine (order);
    for (int l = 0; l <= order; ++l)
        perOrder[l] = legendrePolynomial (l, rE);
}

/** in-phase weights via the exact ratio recurrence
    g_0 = 1, g_l = g_{l-1}·(N-l+1)/(N+l+1) — equal to N!(N+1)!/((N+l+1)!(N-l)!)
    but without factorials/overflow.
    Anchors: N=1 {1,1/3}; N=2 {1,1/2,1/10}; N=3 {1,3/5,1/5,1/35}. */
inline void inPhase (int order, double* perOrder) noexcept
{
    perOrder[0] = 1.0;
    for (int l = 1; l <= order; ++l)
        perOrder[l] = perOrder[l - 1] * (static_cast<double> (order - l + 1)
                                         / static_cast<double> (order + l + 1));
}

//==============================================================================
/** Source-spread (width) taper (FR-5): an energy-normalized order taper that
    blurs a point source into a wider virtual source by de-emphasizing high
    orders. Writes order+1 doubles.

      g_l(sigma) = P_l(cos(sigma/2))   with a monotone cutoff: at the first order
                   whose P_l is <= 0, that order and all higher are zeroed (no
                   Legendre sign-oscillation resurrects a higher order),
      then scaled so sum_l (2l+1) g_l^2 equals its point-source value
      (N+1)^2 = sum_l (2l+1) - i.e. the rE-energy is spread-invariant.

    sigma = 0    -> P_l(1) = 1 for all l -> identity (point source).
    sigma = 180  -> P_1(0) = 0 cuts at order 1 -> order-0 only (omni).
    At sigma/2 = acos(r_E(N)) the raw taper is exactly the order-N max-rE
    family (P_l(r_E)), so spread and max-rE share one machinery.

    Reference: Kronlachner (2014, IEM); Zotter & Frank, Ambisonics ch.4. */
inline void spreadTaper (int order, double spreadDeg, double* perOrder) noexcept
{
    jassert (order >= 0);
    const double x = std::cos (juce::degreesToRadians (spreadDeg) * 0.5);

    bool cutoff = false;
    for (int l = 0; l <= order; ++l)
    {
        if (cutoff)
        {
            perOrder[l] = 0.0;
            continue;
        }
        const double p = legendrePolynomial (l, x);
        if (l > 0 && p <= 0.0)   // P_0(x) == 1 > 0 always, so order 0 survives
        {
            perOrder[l] = 0.0;
            cutoff = true;
        }
        else
        {
            perOrder[l] = p;
        }
    }

    // Energy-normalize to the point-source rE-energy sum_l (2l+1) = (N+1)^2.
    double energy = 0.0, target = 0.0;
    for (int l = 0; l <= order; ++l)
    {
        energy += (2.0 * l + 1.0) * perOrder[l] * perOrder[l];
        target += (2.0 * l + 1.0);
    }
    const double scale = energy > 0.0 ? std::sqrt (target / energy) : 1.0;
    for (int l = 0; l <= order; ++l)
        perOrder[l] *= scale;
}

//==============================================================================
// Expansion and FR-7 order adaptation.
//==============================================================================

/** Broadcast a per-order vector to per-channel: perChannelOut[c] =
    perOrder[acnToOrder(c)] for all numChannels(order) channels. */
inline void perChannel (const double* perOrder, int order, double* perChannelOut) noexcept
{
    for (int c = 0; c < sh::numChannels (order); ++c)
        perChannelOut[c] = perOrder[sh::acnToOrder (c)];
}

/** FR-7 per-channel adaptation gains, numChannels(outOrder) doubles:
      gains[c] = 0                            if acnToOrder(c) > inOrder   (upmix zero-pad)
               = maxRe_{outOrder}[order(c)]    if inOrder > outOrder        (downmix re-weight)
               = 1                            otherwise.
    This is the WP6-facing deliverable (folds into the RT gain stage). */
inline void orderAdaptGains (int inOrder, int outOrder, double* perChannelOut) noexcept
{
    const bool downmix = inOrder > outOrder;
    double reWeights[xoa::kAmbisonicOrder + 1];
    if (downmix)
        maxRe (outOrder, reWeights);

    for (int c = 0; c < sh::numChannels (outOrder); ++c)
    {
        const int l = sh::acnToOrder (c);
        if (l > inOrder)
            perChannelOut[c] = 0.0;
        else if (downmix)
            perChannelOut[c] = reWeights[l];
        else
            perChannelOut[c] = 1.0;
    }
}

/** Reference (offline) order adaptation: out[c] = gains[c]·(c < numChannels(inOrder) ? in[c] : 0)
    for all numChannels(outOrder) channels. in == out is allowed only when the
    pointers are exactly equal (no partial overlap). The RT path composes
    orderAdaptGains into its matrix instead of calling this. */
inline void adaptOrder (const double* in, int inOrder, double* out, int outOrder) noexcept
{
    double gains[xoa::kNumSHChannels];
    orderAdaptGains (inOrder, outOrder, gains);

    const int inChannels = sh::numChannels (inOrder);
    // Each out[c] reads only in[c] (identity index mapping), so with in == out
    // (the only aliasing the contract allows) the loop order is irrelevant.
    for (int c = 0; c < sh::numChannels (outOrder); ++c)
        out[c] = gains[c] * (c < inChannels ? in[c] : 0.0);
}

} // namespace xoa::weights
