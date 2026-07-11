#pragma once

#include <juce_core/juce_core.h>

#include <cmath>
#include <vector>

#include "XoaConstants.h"
#include "Helpers/XoaCoordinates.h"
#include "DSP/AmbiAllRAD.h"
#include "DSP/AmbiConventions.h"
#include "DSP/AmbiOrderWeights.h"
#include "DSP/AmbiSphericalHarmonics.h"
#include "DSP/XoaLinearAlgebra.h"

//==============================================================================
// XOA - decoder matrix design (non-RT / control side). FR-12, FR-16, FR-17.
//
// Produces a decode matrix D (row-major [speaker][ACN channel]) that maps an
// SN3D/ACN bus vector a to speaker gains g = D*a. Geometry only -- per-speaker
// gain/delay/mute/solo and EQ are the WP7 output stage, never folded here.
//
// Two decoder families (AllRAD is WP7):
//   * SAD (sampling):     D0[s][c] = (1/L)*(2 l_c+1)*Y^SN3D_c(u_s)
//   * mode-matching:      D = pinv(Y^N3D)*diag(g_l)*diag(sqrt(2 l_c+1)),
//                         Y^N3D[c][s] = sqrt(2 l_c+1)*Y^SN3D_c(u_s)
// then per-order weighting (basic / max-rE, xoa::weights) and one global
// normalization scalar (amplitude or energy). Formulas are pinned identically
// in tools/reference/gen_decoder_reference.py (the golden generator).
//
// Design order auto-clamps to min(kAmbisonicOrder, floor(sqrt(L))-1): an
// underdetermined pinv produces minimum-norm decoders that measure fine but
// localise wrong, so clamping (with a surfaced warning) beats regularising.
// The condition number of Y^N3D is reported for both types (FR-17); a coplanar
// ring is rank-deficient in full 3D and reports a huge kappa + warning while
// still yielding a usable truncated-SVD matrix.
//==============================================================================

namespace xoa::decoder
{

struct SpeakerLayout
{
    int count = 0;
    coords::Cartesian positions[xoa::kMaxSpeakers];
};

enum class Type { sad = 0, modeMatch = 1, allRad = 2 };        // == decoderType; allRad ships WP7
enum class Weighting { basic = 0, maxRe = 1 };                 // == decoderWeighting
enum class NormalizationMode { amplitude = 0, energy = 1 };    // == decoderNormalization

struct DesignOptions
{
    Type type = Type::sad;
    Weighting weighting = Weighting::maxRe;
    NormalizationMode normalization = NormalizationMode::energy;
    int requestedOrder = xoa::kAmbisonicOrder;
    double rankToleranceRel = 1.0e-9;
    double tikhonovLambdaRel = 0.0;
    bool dualBand = false;          // FR-14: basic (LF) / max-rE (HF) split
    double crossoverHz = 400.0;     // carried through; the RT bus applies it
};

struct DecoderMatrix
{
    int numSpeakers = 0;
    int order = 0;
    std::vector<double> d;   // [numSpeakers * (order+1)^2] row-major

    double  at (int speaker, int channel) const noexcept
    {
        return d[(size_t) speaker * sh::numChannels (order) + channel];
    }
};

constexpr double kKappaWarnThreshold = 100.0;

struct DesignResult
{
    DecoderMatrix matrix;
    int designOrder = 0;
    bool orderClamped = false;
    double conditionNumber = 0.0, sigmaMax = 0.0, sigmaMin = 0.0;
    int effectiveRank = 0;
    bool conditionWarning = false;
    bool svdConverged = true;
    int numImaginarySpeakers = 0;   // AllRAD imaginary loudspeakers inserted (FR-13)
    bool allRadFellBack = false;    // AllRAD requested but the rig could not enclose -> SAD

    // Dual-band (FR-14): the matrix is the BASIC-weighted, basic-normalized
    // decode; the RT bus splits the bus at crossoverHz and multiplies the HF
    // band per channel by hfDiagonal[c] = maxRe[l_c] * (alpha_maxRe/alpha_basic)
    // so a single decode GEMM yields basic below / max-rE above. Empty when
    // single-band.
    bool dualBand = false;
    double crossoverHz = 400.0;
    std::vector<double> hfDiagonal;   // per-channel HF gain, size numChannels(order)

    juce::StringArray warnings;
};

/** Largest decode order a speaker count can support: min(10, floor(sqrt(L))-1). */
inline int maxDesignOrderForSpeakerCount (int numSpeakers) noexcept
{
    if (numSpeakers < 4)
        return 0;
    int root = (int) std::floor (std::sqrt ((double) numSpeakers));
    return juce::jmin (xoa::kAmbisonicOrder, root - 1);
}

namespace detail
{
    // Unit direction of a speaker; r == 0 maps to front (coords convention).
    inline coords::Cartesian unitDir (const coords::Cartesian& p) noexcept
    {
        const double r = std::sqrt (p.x * p.x + p.y * p.y + p.z * p.z);
        if (r <= 0.0)
            return { 1.0, 0.0, 0.0 };
        return { p.x / r, p.y / r, p.z / r };
    }

    inline void orderWeights (Weighting w, int order, double* g) noexcept
    {
        if (w == Weighting::basic) weights::basic (order, g);
        else                       weights::maxRe (order, g);
    }

    // Global normalization scalar for the weighted matrix D (L x K).
    inline double normalizationScalar (const std::vector<double>& d, int L, int order,
                                       NormalizationMode mode)
    {
        const int K = sh::numChannels (order);
        if (mode == NormalizationMode::amplitude)
        {
            double wsum = 0.0;
            for (int s = 0; s < L; ++s)
                wsum += d[(size_t) s * K + 0];              // W column
            return (std::abs (wsum) > 1e-9) ? 1.0 / wsum : 1.0;
        }
        double acc = 0.0;
        for (int s = 0; s < L; ++s)
            for (int c = 0; c < K; ++c)
            {
                const double v = d[(size_t) s * K + c];
                acc += v * v / (2.0 * sh::acnToOrder (c) + 1.0);
            }
        return (acc > 1e-12) ? 1.0 / std::sqrt (acc) : 1.0;
    }
}

/** Design a decoder for a layout. Never throws; problems surface in warnings. */
inline DesignResult design (const SpeakerLayout& layout, const DesignOptions& opts)
{
    DesignResult r;
    const int L = layout.count;

    // AllRAD decodes to a dense virtual t-design (well-conditioned at order 10
    // for any L >= 4), so it is NOT limited by the real speaker count - it
    // bypasses the sqrt(L) clamp the pinv/SAD paths need. If AllRAD later falls
    // back to SAD (non-enclosing rig), that SAD runs at this higher order and
    // its ill-conditioning is reported normally.
    const int maxOrder = (opts.type == Type::allRad)
                             ? xoa::kAmbisonicOrder
                             : maxDesignOrderForSpeakerCount (L);
    int order = juce::jlimit (0, maxOrder, opts.requestedOrder);
    if (order < opts.requestedOrder)
    {
        r.orderClamped = true;
        r.warnings.add ("Design order clamped to " + juce::String (order)
                        + " for " + juce::String (L) + " speakers (requested "
                        + juce::String (opts.requestedOrder) + ").");
    }
    r.designOrder = order;

    const int K = sh::numChannels (order);
    r.matrix.numSpeakers = L;
    r.matrix.order = order;
    r.matrix.d.assign ((size_t) L * K, 0.0);
    if (L <= 0)
        return r;

    // Y^SN3D[c][s] and, transposed, A = (Y^N3D)^T (L x K, tall) for the SVD.
    std::vector<double> ysn ((size_t) K * L);
    std::vector<double> aTall ((size_t) L * K);
    double shBuf[xoa::kNumSHChannels];
    bool originWarned = false;
    for (int s = 0; s < L; ++s)
    {
        const auto u = detail::unitDir (layout.positions[s]);
        if (! originWarned && layout.positions[s].x == 0.0 && layout.positions[s].y == 0.0
            && layout.positions[s].z == 0.0)
        {
            r.warnings.add ("Speaker " + juce::String (s + 1) + " at the origin; direction undefined (using front).");
            originWarned = true;
        }
        sh::evaluate (u, order, shBuf);
        for (int c = 0; c < K; ++c)
        {
            ysn[(size_t) c * L + s] = shBuf[c];
            aTall[(size_t) s * K + c] = conv::sn3dToN3d (sh::acnToOrder (c)) * shBuf[c];
        }
    }

    // Dual-band builds the BASIC-weighted matrix (the HF max-rE ratio goes into
    // hfDiagonal); single-band uses the requested weighting. Single-band is thus
    // byte-for-byte unchanged from before dual-band existed.
    const Weighting matrixWeighting = opts.dualBand ? Weighting::basic : opts.weighting;
    double g[xoa::kAmbisonicOrder + 1];
    detail::orderWeights (matrixWeighting, order, g);

    // Condition number of Y^N3D (== A) is reported for all decoder types.
    {
        const auto pv = linalg::pseudoInverse (aTall.data(), L, K,
                                               { opts.rankToleranceRel, 0.0 });
        r.conditionNumber = pv.conditionNumber;
        r.sigmaMax = pv.sigmaMax;
        r.sigmaMin = pv.sigmaMin;
        r.effectiveRank = pv.effectiveRank;
        r.svdConverged = pv.converged;
    }

    // AllRAD (FR-13): decode to the virtual t-design + VBAP to the rig. On a
    // rig that cannot enclose the listener, fall back to SAD with a warning
    // rather than silently zeroing part of the sphere.
    Type effectiveType = opts.type;
    if (opts.type == Type::allRad)
    {
        const auto ar = allrad::computeUnweighted (layout.positions, L, order);
        r.warnings.addArray (ar.warnings);
        if (ar.ok)
        {
            r.numImaginarySpeakers = ar.numImaginary;
            for (int s = 0; s < L; ++s)
                for (int c = 0; c < K; ++c)
                {
                    const int l = sh::acnToOrder (c);
                    r.matrix.d[(size_t) s * K + c] = ar.dPre[(size_t) s * K + c] * g[l];
                }
            effectiveType = Type::allRad;   // done; SAD/mode-match branches skipped
        }
        else
        {
            r.allRadFellBack = true;
            r.warnings.add ("AllRAD unavailable (the layout does not enclose the "
                            "listener); using SAD.");
            effectiveType = Type::sad;
        }
    }

    if (effectiveType == Type::sad)
    {
        for (int s = 0; s < L; ++s)
            for (int c = 0; c < K; ++c)
            {
                const int l = sh::acnToOrder (c);
                r.matrix.d[(size_t) s * K + c] =
                    (1.0 / L) * (2.0 * l + 1.0) * ysn[(size_t) c * L + s] * g[l];
            }
    }
    else if (effectiveType == Type::modeMatch)
    {
        // pinv(Y^N3D) = (pseudoInverse(A))^T, with A = (Y^N3D)^T. Ap is K x L.
        const auto pv = linalg::pseudoInverse (aTall.data(), L, K,
                                               { opts.rankToleranceRel, opts.tikhonovLambdaRel });
        for (int s = 0; s < L; ++s)
            for (int c = 0; c < K; ++c)
            {
                const int l = sh::acnToOrder (c);
                const double pinvY = pv.pinv[(size_t) c * L + s];   // pinv(Y^N3D)[s][c]
                r.matrix.d[(size_t) s * K + c] = pinvY * g[l] * conv::sn3dToN3d (l);
            }
    }

    if (opts.dualBand)
    {
        // r.matrix.d currently holds the unweighted (basic) decode D_pre. Level-
        // match the two bands under the chosen normalization: the matrix is
        // alpha_basic * D_pre; the HF diagonal is maxRe[l] * alpha_maxRe/alpha_basic
        // so D_pre * diag(hfDiagonal) == the single-band max-rE decode exactly.
        const double alphaBasic = detail::normalizationScalar (r.matrix.d, L, order, opts.normalization);

        double gMaxRe[xoa::kAmbisonicOrder + 1];
        weights::maxRe (order, gMaxRe);
        std::vector<double> mMaxRe = r.matrix.d;
        for (int s = 0; s < L; ++s)
            for (int c = 0; c < K; ++c)
                mMaxRe[(size_t) s * K + c] *= gMaxRe[sh::acnToOrder (c)];
        const double alphaMaxRe = detail::normalizationScalar (mMaxRe, L, order, opts.normalization);

        for (double& v : r.matrix.d)
            v *= alphaBasic;

        const double ratio = (std::abs (alphaBasic) > 1e-300) ? alphaMaxRe / alphaBasic : 1.0;
        r.dualBand = true;
        r.crossoverHz = opts.crossoverHz;
        r.hfDiagonal.assign ((size_t) K, 0.0);
        for (int c = 0; c < K; ++c)
            r.hfDiagonal[(size_t) c] = gMaxRe[sh::acnToOrder (c)] * ratio;
    }
    else
    {
        const double alpha = detail::normalizationScalar (r.matrix.d, L, order, opts.normalization);
        for (double& v : r.matrix.d)
            v *= alpha;
    }

    if (! std::isfinite (r.conditionNumber) || r.conditionNumber > kKappaWarnThreshold)
    {
        r.conditionWarning = true;
        r.warnings.add ("Layout is ill-conditioned for order " + juce::String (order)
                        + " (condition number "
                        + (std::isfinite (r.conditionNumber) ? juce::String (r.conditionNumber, 1) : juce::String ("infinite"))
                        + ", rank " + juce::String (r.effectiveRank) + "/" + juce::String (K) + ").");
    }
    if (! r.svdConverged)
        r.warnings.add ("SVD did not converge; decoder may be inaccurate.");

    return r;
}

//==============================================================================
// Regularity detection (FR-16): classify + suggest, never assume.
//==============================================================================

/** Suggested dual-band crossover (Hz), rig-radius-scaled and anchored at the
    2 m / 400 Hz default; UI hint only, clamped to the parameter range. */
inline double suggestedCrossoverHz (double meanRadiusMeters) noexcept
{
    return juce::jlimit (80.0, 2000.0, 400.0 * (2.0 / juce::jmax (0.1, meanRadiusMeters)));
}

enum class LayoutClass { ring = 0, dome, sphere, irregular };

struct LayoutClassification
{
    LayoutClass layoutClass = LayoutClass::irregular;
    double radiusSpreadRel = 0.0, meanRadius = 0.0;
    double minElevationDeg = 0.0, maxElevationDeg = 0.0;
    int suggestedDecoderType = 2;   // decoderType int: 0 SAD, 1 modeMatch, 2 allRAD
};

inline LayoutClassification classify (const SpeakerLayout& layout)
{
    LayoutClassification c;
    const int L = layout.count;
    if (L < 4)
    {
        c.layoutClass = LayoutClass::irregular;
        c.suggestedDecoderType = 2;
        return c;
    }

    double rMin = 1e300, rMax = 0.0, rSum = 0.0, elMin = 90.0, elMax = -90.0;
    for (int s = 0; s < L; ++s)
    {
        const auto& p = layout.positions[s];
        const double r = std::sqrt (p.x * p.x + p.y * p.y + p.z * p.z);
        rMin = juce::jmin (rMin, r);
        rMax = juce::jmax (rMax, r);
        rSum += r;
        const auto sph = coords::cartesianToSpherical (p);
        elMin = juce::jmin (elMin, sph.elevationDeg);
        elMax = juce::jmax (elMax, sph.elevationDeg);
    }
    c.meanRadius = rSum / L;
    c.radiusSpreadRel = (c.meanRadius > 0.0) ? (rMax - rMin) / c.meanRadius : 0.0;
    c.minElevationDeg = elMin;
    c.maxElevationDeg = elMax;

    if (c.radiusSpreadRel > 0.05)
        c.layoutClass = LayoutClass::irregular;                 // non-concentric
    else if ((elMax - elMin) <= 4.0)
        c.layoutClass = LayoutClass::ring;                      // coplanar (any elevation)
    else if (elMin >= -15.0)
        c.layoutClass = LayoutClass::dome;                      // hemisphere-ish
    else if (elMin <= -30.0 && elMax >= 30.0)
        c.layoutClass = LayoutClass::sphere;
    else
        c.layoutClass = LayoutClass::irregular;

    switch (c.layoutClass)
    {
        case LayoutClass::ring:   c.suggestedDecoderType = 0; break;   // SAD
        case LayoutClass::dome:   c.suggestedDecoderType = 0; break;   // SAD (mode-matching a dome is iffy)
        case LayoutClass::sphere: c.suggestedDecoderType = 1; break;   // mode-matching
        case LayoutClass::irregular: default: c.suggestedDecoderType = 2; break;   // AllRAD (WP7)
    }
    return c;
}

} // namespace xoa::decoder
