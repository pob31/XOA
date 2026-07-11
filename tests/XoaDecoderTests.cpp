/*
    XoaDecoderTests.cpp - WP5 tests: linear algebra (Jacobi SVD/pinv), decoder
    design (SAD + mode-matching), regularity classification, rV/rE analysis,
    the non-RT matrix builder, and WFS import completion.

    Golden data tests/data/decoder_reference.json is produced by
    tools/reference/gen_decoder_reference.py (mpmath svd_r, ~40 digits).
*/

#include "XoaTestFramework.h"

#include "DSP/AmbiConventions.h"
#include "DSP/AmbiDecoderDesigner.h"
#include "DSP/AmbiRvReAnalysis.h"
#include "DSP/AmbiSphericalHarmonics.h"
#include "DSP/DecoderMatrixBuilder.h"
#include "DSP/TDesignTables.h"
#include "DSP/XoaLinearAlgebra.h"
#include "Helpers/XoaCoordinates.h"
#include "Parameters/XoaParameterIDs.h"
#include "Parameters/XoaValueTreeState.h"
#include "XoaConstants.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace linalg = xoa::linalg;
namespace dsh = xoa::sh;
namespace dconv = xoa::conv;
namespace dec = xoa::decoder;
namespace ana = xoa::analysis;
namespace dwt = xoa::weights;

//==============================================================================
static juce::var loadDecoderJson()
{
    const auto file = juce::File (XOA_TESTS_DATA_DIR).getChildFile ("decoder_reference.json");
    const auto parsed = juce::JSON::parse (file.loadFileAsString());
    CHECK (parsed.isObject());
    return parsed;
}

static double dnum (const juce::var& v) { return static_cast<double> (v); }
static bool dapprox (double a, double b, double tol) noexcept { return std::abs (a - b) <= tol; }

// Reconstruct A' = U diag(sigma) V^T and return max|A' - A|.
static double reconstructionError (const double* a, const linalg::SvdResult& s)
{
    double worst = 0.0;
    for (int i = 0; i < s.m; ++i)
        for (int j = 0; j < s.n; ++j)
        {
            double acc = 0.0;
            for (int k = 0; k < s.n; ++k)
                acc += s.u[(size_t) i * s.n + k] * s.sigma[(size_t) k] * s.v[(size_t) j * s.n + k];
            worst = std::max (worst, std::abs (acc - a[(size_t) i * s.n + j]));
        }
    return worst;
}

// max |(M^T M) - I| for an m x n column-orthonormal M (only columns with sigma>tol).
static double orthonormalityError (const std::vector<double>& mtx, int m, int n,
                                   const std::vector<double>& sigma, double sigTol)
{
    double worst = 0.0;
    for (int p = 0; p < n; ++p)
        for (int q = 0; q < n; ++q)
        {
            if (sigma[(size_t) p] <= sigTol || sigma[(size_t) q] <= sigTol)
                continue;
            double acc = 0.0;
            for (int i = 0; i < m; ++i)
                acc += mtx[(size_t) i * n + p] * mtx[(size_t) i * n + q];
            worst = std::max (worst, std::abs (acc - (p == q ? 1.0 : 0.0)));
        }
    return worst;
}

// Read a fixture's speaker positions from the golden JSON.
static std::vector<xoa::coords::Cartesian> fixturePositions (const juce::var& fixture)
{
    const auto pos = fixture["positions"];
    std::vector<xoa::coords::Cartesian> out;
    for (int s = 0; s < pos.size(); ++s)
        out.push_back ({ dnum (pos[s][0]), dnum (pos[s][1]), dnum (pos[s][2]) });
    return out;
}

// Build A = (Y^N3D)^T, an L x K matrix (tall, L >= K) row-major, so its
// singular values equal Y^N3D's. A[s][c] = sqrt(2 l_c + 1) * Y^SN3D_c(u_s).
static std::vector<double> buildYn3dTransposed (const std::vector<xoa::coords::Cartesian>& pos,
                                                int order, int& outL, int& outK)
{
    const int L = (int) pos.size();
    const int K = dsh::numChannels (order);
    outL = L; outK = K;
    std::vector<double> a ((size_t) L * K);
    double sh[xoa::kNumSHChannels];
    for (int s = 0; s < L; ++s)
    {
        dsh::evaluate (pos[(size_t) s], order, sh);
        for (int c = 0; c < K; ++c)
            a[(size_t) s * K + c] = dconv::sn3dToN3d (dsh::acnToOrder (c)) * sh[c];
    }
    return a;
}

static juce::var findFixture (const juce::var& doc, const juce::String& name)
{
    const auto fixtures = doc["fixtures"];
    for (int i = 0; i < fixtures.size(); ++i)
        if (fixtures[i]["name"].toString() == name)
            return fixtures[i];
    CHECK (false);
    return {};
}

static dec::SpeakerLayout layoutFromFixture (const juce::var& fixture)
{
    dec::SpeakerLayout layout;
    const auto pos = fixturePositions (fixture);
    layout.count = (int) pos.size();
    for (int s = 0; s < layout.count; ++s)
        layout.positions[s] = pos[(size_t) s];
    return layout;
}

// max |D - golden| over the whole matrix
static double matrixError (const dec::DecoderMatrix& d, const juce::var& golden)
{
    double worst = 0.0;
    const int K = dsh::numChannels (d.order);
    CHECK (golden.size() == d.numSpeakers);
    for (int s = 0; s < d.numSpeakers; ++s)
    {
        const auto row = golden[s];
        CHECK (row.size() == K);
        for (int c = 0; c < K; ++c)
            worst = std::max (worst, std::abs (d.at (s, c) - dnum (row[c])));
    }
    return worst;
}

//==============================================================================
// D1 - SVD closed forms
//==============================================================================
static void testSvdClosedForms()
{
    // identity 3x3
    {
        const double a[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };
        const auto s = linalg::jacobiSvd (a, 3, 3);
        CHECK (s.converged);
        for (int i = 0; i < 3; ++i) CHECK (dapprox (s.sigma[(size_t) i], 1.0, 1e-14));
        CHECK (reconstructionError (a, s) < 1e-14);
    }

    // diagonal with known descending singular values (2, then 3 -> sorted 3,2)
    {
        const double a[4] = { 2, 0, 0, 3 };
        const auto s = linalg::jacobiSvd (a, 2, 2);
        CHECK (dapprox (s.sigma[0], 3.0, 1e-14));
        CHECK (dapprox (s.sigma[1], 2.0, 1e-14));
        CHECK (reconstructionError (a, s) < 1e-14);
    }

    // rank-1 (outer product) 3x2: sigma = {sqrt(sum), 0}
    {
        const double col[3] = { 1, 2, 2 }, row[2] = { 3, 4 };
        double a[6];
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 2; ++j)
                a[i * 2 + j] = col[i] * row[j];
        const auto s = linalg::jacobiSvd (a, 3, 2);
        const double expected = std::sqrt ((1.0 + 4 + 4) * (9.0 + 16));   // ||col||*||row||
        CHECK (dapprox (s.sigma[0], expected, 1e-13));
        CHECK (std::abs (s.sigma[1]) < 1e-13);
        CHECK (reconstructionError (a, s) < 1e-13);
    }

    // symmetric 2x2 [[2,1],[1,2]]: singular values 3 and 1
    {
        const double a[4] = { 2, 1, 1, 2 };
        const auto s = linalg::jacobiSvd (a, 2, 2);
        CHECK (dapprox (s.sigma[0], 3.0, 1e-14));
        CHECK (dapprox (s.sigma[1], 1.0, 1e-14));
        CHECK (orthonormalityError (s.u, 2, 2, s.sigma, 1e-12) < 1e-14);
        CHECK (orthonormalityError (s.v, 2, 2, s.sigma, 1e-12) < 1e-14);
    }

    // pseudo-inverse of a full-rank tall matrix == left inverse: pinv(A) A = I
    {
        const double a[6] = { 1, 0, 0, 1, 1, 1 };   // 3x2, rank 2
        const auto p = linalg::pseudoInverse (a, 3, 2);
        CHECK (p.effectiveRank == 2);
        CHECK (std::isfinite (p.conditionNumber));
        // (pinv * A) should be 2x2 identity
        for (int i = 0; i < 2; ++i)
            for (int j = 0; j < 2; ++j)
            {
                double acc = 0.0;
                for (int k = 0; k < 3; ++k)
                    acc += p.pinv[(size_t) i * 3 + k] * a[(size_t) k * 2 + j];
                CHECK (dapprox (acc, (i == j ? 1.0 : 0.0), 1e-13));
            }
    }

    // rank-deficient: two identical columns -> effectiveRank 1
    {
        const double a[6] = { 1, 1, 2, 2, 3, 3 };   // 3x2, both cols equal-ish (col0==col1)
        const auto p = linalg::pseudoInverse (a, 3, 2);
        CHECK (p.effectiveRank == 1);
        CHECK (! std::isfinite (p.conditionNumber));   // sigmaMin == 0
    }
}

//==============================================================================
// D2 - Jacobi SVD on the fixture Y^N3D matrices vs mpmath goldens
//==============================================================================
static void testSvdOnFixtures()
{
    const auto doc = loadDecoderJson();
    const auto fixtures = doc["fixtures"];
    for (int fi = 0; fi < fixtures.size(); ++fi)
    {
        const auto fx = fixtures[fi];
        const int order = (int) dnum (fx["designOrder"]);
        const auto pos = fixturePositions (fx);
        int L, K;
        const auto a = buildYn3dTransposed (pos, order, L, K);
        const auto s = linalg::jacobiSvd (a.data(), L, K);
        CHECK (s.converged);

        const auto goldSigma = fx["singularValues"];
        CHECK (goldSigma.size() == K);
        const double sMax = dnum (goldSigma[0]);
        for (int j = 0; j < K; ++j)
            CHECK (dapprox (s.sigma[(size_t) j], dnum (goldSigma[j]), 1e-12 * sMax + 1e-13));

        // orthonormality of retained columns + reconstruction
        CHECK (orthonormalityError (s.u, L, K, s.sigma, 1e-9 * sMax) < 1e-12);
        CHECK (orthonormalityError (s.v, K, K, s.sigma, 1e-9 * sMax) < 1e-12);
        CHECK (reconstructionError (a.data(), s) < 1e-11 * sMax);

        // effective rank via the pinv path
        const auto p = linalg::pseudoInverse (a.data(), L, K);
        CHECK (p.effectiveRank == (int) dnum (fx["effectiveRank"]));
    }

    // ring is severely rank-deficient: rank 7, nine tiny singular values
    const auto ring = findFixture (doc, "ring24");
    int L, K;
    const auto a = buildYn3dTransposed (fixturePositions (ring), 3, L, K);
    const auto s = linalg::jacobiSvd (a.data(), L, K);
    int tiny = 0;
    for (int j = 0; j < K; ++j)
        if (s.sigma[(size_t) j] < 1e-9 * s.sigma[0]) ++tiny;
    CHECK (tiny == 9);
}

//==============================================================================
// D3 - Moore-Penrose properties on the rank-deficient ring
//==============================================================================
static void testMoorePenrose()
{
    const auto doc = loadDecoderJson();
    const auto ring = findFixture (doc, "ring24");
    int L, K;
    const auto a = buildYn3dTransposed (fixturePositions (ring), 3, L, K);   // L x K
    const auto p = linalg::pseudoInverse (a.data(), L, K);                   // K x L
    const double sMax = p.sigmaMax;

    // A A+ A == A  (K columns, L rows; A is L x K, A+ is K x L)
    double worstAPA = 0.0;
    for (int i = 0; i < L; ++i)
        for (int j = 0; j < K; ++j)
        {
            double ap = 0.0;                       // (A A+)_{i,k}
            for (int k = 0; k < L; ++k)
            {
                double aap = 0.0;
                for (int t = 0; t < K; ++t)
                    aap += a[(size_t) i * K + t] * p.pinv[(size_t) t * L + k];
                ap += aap * a[(size_t) k * K + j];
            }
            worstAPA = std::max (worstAPA, std::abs (ap - a[(size_t) i * K + j]));
        }
    CHECK (worstAPA < 1e-10 * sMax);

    // (A A+) symmetric
    std::vector<double> aap ((size_t) L * L, 0.0);
    for (int i = 0; i < L; ++i)
        for (int k = 0; k < L; ++k)
        {
            double acc = 0.0;
            for (int t = 0; t < K; ++t)
                acc += a[(size_t) i * K + t] * p.pinv[(size_t) t * L + k];
            aap[(size_t) i * L + k] = acc;
        }
    double worstSym = 0.0;
    for (int i = 0; i < L; ++i)
        for (int k = 0; k < L; ++k)
            worstSym = std::max (worstSym, std::abs (aap[(size_t) i * L + k] - aap[(size_t) k * L + i]));
    CHECK (worstSym < 1e-12);
}

//==============================================================================
// D4 - golden decoder matrices, all fixtures x types x weightings x norms
//==============================================================================
static void testGoldenMatrices()
{
    const auto doc = loadDecoderJson();
    const auto fixtures = doc["fixtures"];
    const dec::Type types[] = { dec::Type::sad, dec::Type::modeMatch };
    const char* typeName[] = { "sad", "modeMatch" };
    const dec::Weighting wts[] = { dec::Weighting::basic, dec::Weighting::maxRe };
    const char* wtName[] = { "basic", "maxRe" };
    const dec::NormalizationMode norms[] = { dec::NormalizationMode::amplitude, dec::NormalizationMode::energy };
    const char* normName[] = { "amplitude", "energy" };

    for (int fi = 0; fi < fixtures.size(); ++fi)
    {
        const auto fx = fixtures[fi];
        const auto layout = layoutFromFixture (fx);
        const auto matrices = fx["matrices"];

        for (int ti = 0; ti < 2; ++ti)
            for (int wi = 0; wi < 2; ++wi)
                for (int ni = 0; ni < 2; ++ni)
                {
                    dec::DesignOptions o;
                    o.type = types[ti]; o.weighting = wts[wi]; o.normalization = norms[ni];
                    const auto res = dec::design (layout, o);
                    const juce::Identifier key (juce::String (typeName[ti]) + "_" + wtName[wi] + "_" + normName[ni]);
                    CHECK (matrixError (res.matrix, matrices[key]) < 1e-12);
                }
    }

    // ring Tikhonov config
    const auto ring = findFixture (doc, "ring24");
    const auto tik = ring["tikhonov"];
    dec::DesignOptions o;
    o.type = dec::Type::modeMatch; o.weighting = dec::Weighting::basic;
    o.normalization = dec::NormalizationMode::energy;
    o.tikhonovLambdaRel = dnum (tik["lambdaRel"]);
    const auto res = dec::design (layoutFromFixture (ring), o);
    CHECK (matrixError (res.matrix, tik["matrix"]) < 1e-12);
}

//==============================================================================
// D5 - SAD == mode-matching on the icosahedron (spherical 5-design), N=2
//==============================================================================
static void testSadEqualsModeMatch()
{
    const auto doc = loadDecoderJson();
    const auto layout = layoutFromFixture (findFixture (doc, "icosahedron12"));

    for (auto w : { dec::Weighting::basic, dec::Weighting::maxRe })
        for (auto n : { dec::NormalizationMode::amplitude, dec::NormalizationMode::energy })
        {
            dec::DesignOptions os; os.type = dec::Type::sad; os.weighting = w; os.normalization = n;
            dec::DesignOptions om; om.type = dec::Type::modeMatch; om.weighting = w; om.normalization = n;
            const auto rs = dec::design (layout, os);
            const auto rm = dec::design (layout, om);
            double worst = 0.0;
            for (size_t i = 0; i < rs.matrix.d.size(); ++i)
                worst = std::max (worst, std::abs (rs.matrix.d[i] - rm.matrix.d[i]));
            CHECK (worst < 1e-12);
        }

    // kappa(icosa) == 1
    dec::DesignOptions o; o.type = dec::Type::modeMatch;
    const auto r = dec::design (layout, o);
    CHECK (dapprox (r.conditionNumber, 1.0, 1e-10));
    CHECK (! r.conditionWarning);
    CHECK (r.effectiveRank == 9);

    // Independent rE-magnitude physics anchor (NOT golden-derived): on a
    // spherical design a max-rE decode has ||rE|| == maxReCosine(order) at every
    // direction. Order 2 -> sqrt(3/5). Pins the perceptual energy-spread metric
    // that max-rE exists to set, independent of the shared rV/rE golden path.
    {
        dec::DesignOptions om; om.type = dec::Type::sad; om.weighting = dec::Weighting::maxRe;
        om.normalization = dec::NormalizationMode::energy;
        const auto rm = dec::design (layout, om);
        const double expected = dwt::maxReCosine (2);   // == sqrt(3/5)
        CHECK (dapprox (expected, std::sqrt (3.0 / 5.0), 1e-12));
        for (const auto d : { std::pair<double,double> { 0, 0 }, { 47, 20 }, { -130, -35 } })
        {
            const auto smp = ana::analyzeDirection (rm.matrix, layout, d.first, d.second);
            CHECK (dapprox (smp.reMagnitude, expected, 1e-9));
        }
    }
}

//==============================================================================
// D5b - mode-matching reconstruction property, GOLDEN-FREE and covering l=3.
// On a full-rank layout, re-encoding the decoder output reproduces the input:
// E * D = alpha * I_K, where E[c][s] = Y^SN3D_c(u_s) and D is the mode-matching
// (basic) decoder. The sqrt(2l+1) folding factors cancel exactly. This pins the
// mode-matching pinv formula + indexing at every l (dome24 is full rank 16 at
// order 3), independent of both the mpmath goldens and the SAD path.
//==============================================================================
static void testModeMatchReconstruction()
{
    const auto doc = loadDecoderJson();
    const auto dome = findFixture (doc, "dome24");    // full rank 16 at order 3
    const auto pos = fixturePositions (dome);
    const int L = (int) pos.size(), order = 3, K = dsh::numChannels (order);

    dec::SpeakerLayout layout;
    layout.count = L;
    for (int s = 0; s < L; ++s) layout.positions[s] = pos[(size_t) s];

    dec::DesignOptions o; o.type = dec::Type::modeMatch; o.weighting = dec::Weighting::basic;
    o.normalization = dec::NormalizationMode::amplitude;
    const auto r = dec::design (layout, o);
    CHECK (r.effectiveRank == K);   // must be full rank for exact reconstruction

    // E[c][s] = Y^SN3D_c(u_s)
    std::vector<double> E ((size_t) K * L);
    double sh[xoa::kNumSHChannels];
    for (int s = 0; s < L; ++s)
    {
        const double rr = std::sqrt (pos[(size_t) s].x * pos[(size_t) s].x
                                     + pos[(size_t) s].y * pos[(size_t) s].y
                                     + pos[(size_t) s].z * pos[(size_t) s].z);
        dsh::evaluate (xoa::coords::Cartesian { pos[(size_t) s].x / rr, pos[(size_t) s].y / rr,
                                                pos[(size_t) s].z / rr }, order, sh);
        for (int c = 0; c < K; ++c) E[(size_t) c * L + s] = sh[c];
    }

    // M = E * D  (K x K); expect alpha * I
    const double alpha = [&]
    {
        double acc = 0.0;
        for (int s = 0; s < L; ++s) acc += E[(size_t) 0 * L + s] * r.matrix.at (s, 0);
        return acc;
    }();
    CHECK (std::abs (alpha) > 1e-6);
    for (int i = 0; i < K; ++i)
        for (int j = 0; j < K; ++j)
        {
            double m = 0.0;
            for (int s = 0; s < L; ++s) m += E[(size_t) i * L + s] * r.matrix.at (s, j);
            CHECK (std::abs (m - (i == j ? alpha : 0.0)) < 1e-10);
        }
}

//==============================================================================
// D6 - condition number, warnings, order clamp
//==============================================================================
static void testConditionAndClamp()
{
    const auto doc = loadDecoderJson();

    // ring: severely rank-deficient -> warning, huge kappa, rank 7
    {
        const auto r = dec::design (layoutFromFixture (findFixture (doc, "ring24")),
                                    { dec::Type::modeMatch });
        CHECK (r.conditionWarning);
        CHECK (r.effectiveRank == 7);
        CHECK (! std::isfinite (r.conditionNumber) || r.conditionNumber > 1e12);
    }
    // dome: full rank 16 but kappa ~107 (> threshold) -> warning fires (honest hemisphere)
    {
        const auto r = dec::design (layoutFromFixture (findFixture (doc, "dome24")),
                                    { dec::Type::modeMatch });
        CHECK (r.effectiveRank == 16);
        CHECK (std::isfinite (r.conditionNumber));
        CHECK (r.conditionNumber > dec::kKappaWarnThreshold);   // ~107
        CHECK (r.conditionWarning);
    }
    // icosa: kappa 1 -> no warning
    {
        const auto r = dec::design (layoutFromFixture (findFixture (doc, "icosahedron12")),
                                    { dec::Type::modeMatch });
        CHECK (! r.conditionWarning);
    }
    // order clamp: requesting order 10 on 24 speakers -> design order 3
    {
        dec::DesignOptions o; o.requestedOrder = 10;
        const auto r = dec::design (layoutFromFixture (findFixture (doc, "ring24")), o);
        CHECK (r.designOrder == 3);
        CHECK (r.orderClamped);
        CHECK (! r.warnings.isEmpty());
    }
    CHECK (dec::maxDesignOrderForSpeakerCount (24) == 3);
    CHECK (dec::maxDesignOrderForSpeakerCount (12) == 2);
    CHECK (dec::maxDesignOrderForSpeakerCount (121) == 10);   // (11^2) -> min(10, 10)
    CHECK (dec::maxDesignOrderForSpeakerCount (3) == 0);

    // AllRAD (WP7) now designs at the FULL bus order (bypassing the sqrt(L)
    // clamp) because it decodes to a dense virtual t-design, not the real rig
    // directly. The icosahedron encloses the listener, so no fallback.
    {
        dec::DesignOptions o; o.type = dec::Type::allRad;
        const auto rAll = dec::design (layoutFromFixture (findFixture (doc, "icosahedron12")), o);
        CHECK (rAll.designOrder == xoa::kAmbisonicOrder);   // NOT clamped to 2
        CHECK (! rAll.allRadFellBack);
        CHECK (rAll.matrix.order == xoa::kAmbisonicOrder);
        CHECK (rAll.matrix.numSpeakers == 12);

        // It is a genuinely different decoder from mode-matching (which clamps
        // to order 2 on 12 speakers).
        dec::DesignOptions m; m.type = dec::Type::modeMatch;
        const auto rMm = dec::design (layoutFromFixture (findFixture (doc, "icosahedron12")), m);
        CHECK (rMm.matrix.order != rAll.matrix.order);
    }
}

//==============================================================================
// D7 - normalization anchors (exact, any layout).
// NOTE: these confirm the global scalar is APPLIED and self-consistent (they
// catch a skipped/partial normalization or the near-zero fallback branch); they
// are tautological w.r.t. the physics coefficients (the scalar is derived from
// the matrix, so the post-scale sum is 1 regardless of whether (2l+1)/g_l/pinv
// are correct). Decoder physics is pinned by D4 (mpmath goldens), D5 (SAD ==
// mode-match), D5b (reconstruction E*D==alpha*I), D3 (Moore-Penrose), D9 (rV).
//==============================================================================
static void testNormalizationAnchors()
{
    const auto doc = loadDecoderJson();
    const auto layout = layoutFromFixture (findFixture (doc, "dome24"));
    const int order = 3, K = dsh::numChannels (order);

    // SAD + amplitude: sum_s D[s][0] == 1
    {
        dec::DesignOptions o; o.type = dec::Type::sad; o.weighting = dec::Weighting::basic;
        o.normalization = dec::NormalizationMode::amplitude;
        const auto r = dec::design (layout, o);
        double wsum = 0.0;
        for (int s = 0; s < r.matrix.numSpeakers; ++s) wsum += r.matrix.at (s, 0);
        CHECK (dapprox (wsum, 1.0, 1e-12));
    }
    // SAD + energy: sum_s sum_c D^2/(2l+1) == 1
    {
        dec::DesignOptions o; o.type = dec::Type::sad; o.weighting = dec::Weighting::maxRe;
        o.normalization = dec::NormalizationMode::energy;
        const auto r = dec::design (layout, o);
        double acc = 0.0;
        for (int s = 0; s < r.matrix.numSpeakers; ++s)
            for (int c = 0; c < K; ++c)
            {
                const double v = r.matrix.at (s, c);
                acc += v * v / (2.0 * dsh::acnToOrder (c) + 1.0);
            }
        CHECK (dapprox (acc, 1.0, 1e-12));
    }
}

//==============================================================================
// D8 - regularity classification
//==============================================================================
static void testClassify()
{
    const auto doc = loadDecoderJson();
    CHECK (dec::classify (layoutFromFixture (findFixture (doc, "ring24"))).layoutClass == dec::LayoutClass::ring);
    CHECK (dec::classify (layoutFromFixture (findFixture (doc, "dome24"))).layoutClass == dec::LayoutClass::dome);
    CHECK (dec::classify (layoutFromFixture (findFixture (doc, "icosahedron12"))).layoutClass == dec::LayoutClass::sphere);

    // suggestions
    CHECK (dec::classify (layoutFromFixture (findFixture (doc, "ring24"))).suggestedDecoderType == 0);
    CHECK (dec::classify (layoutFromFixture (findFixture (doc, "icosahedron12"))).suggestedDecoderType == 1);

    // hardcoded 8-corner shoebox -> irregular/allRAD
    {
        dec::SpeakerLayout box; box.count = 8;
        int i = 0;
        for (double x : { -3.0, 3.0 }) for (double y : { -2.0, 2.0 }) for (double z : { 0.0, 2.5 })
            box.positions[i++] = { x, y, z };
        const auto c = dec::classify (box);
        CHECK (c.layoutClass == dec::LayoutClass::irregular);
        CHECK (c.suggestedDecoderType == 2);
    }
    // elevated coplanar ring -> still ring
    {
        dec::SpeakerLayout r; r.count = 12;
        for (int k = 0; k < 12; ++k)
        {
            const double az = juce::degreesToRadians (k * 30.0);
            const double el = juce::degreesToRadians (25.0);
            r.positions[k] = { 2.0 * std::cos (el) * std::cos (az), 2.0 * std::cos (el) * std::sin (az), 2.0 * std::sin (el) };
        }
        CHECK (dec::classify (r).layoutClass == dec::LayoutClass::ring);
    }
    // non-concentric -> irregular
    {
        dec::SpeakerLayout r; r.count = 8;
        for (int k = 0; k < 8; ++k)
        {
            const double az = juce::degreesToRadians (k * 45.0);
            const double rad = (k % 2 == 0) ? 2.0 : 4.0;   // alternating radius
            r.positions[k] = { rad * std::cos (az), rad * std::sin (az), 0.0 };
        }
        CHECK (dec::classify (r).layoutClass == dec::LayoutClass::irregular);
    }
}

//==============================================================================
// D9 - ring analytic rV/rE properties
//==============================================================================
static void testRingAnalytics()
{
    const auto doc = loadDecoderJson();
    const auto ring = findFixture (doc, "ring24");
    const auto layout = layoutFromFixture (ring);

    // SAD basic: rV direction error 0 for horizontal sources (regular rig)
    {
        dec::DesignOptions o; o.type = dec::Type::sad; o.weighting = dec::Weighting::basic;
        o.normalization = dec::NormalizationMode::amplitude;
        const auto r = dec::design (layout, o);
        const auto gold = ring["ringRvBasicSad"];
        const double az[] = { 0.0, 7.5, 33.1 };
        for (int i = 0; i < 3; ++i)
        {
            const auto smp = ana::analyzeDirection (r.matrix, layout, az[i], 0.0);
            CHECK (smp.rvDirErrorDeg < 1e-5);                                   // localises correctly
            CHECK (dapprox (smp.rvMagnitude, dnum (gold[i]["rvMagnitude"]), 1e-9));  // pinned (~1.25)
        }
    }
    // mode-matching basic: velocity matched -> ||rV|| = 1, dir 0
    {
        dec::DesignOptions o; o.type = dec::Type::modeMatch; o.weighting = dec::Weighting::basic;
        o.normalization = dec::NormalizationMode::amplitude;
        const auto r = dec::design (layout, o);
        for (double az : { 0.0, 7.5, 33.1 })
        {
            const auto smp = ana::analyzeDirection (r.matrix, layout, az, 0.0);
            CHECK (dapprox (smp.rvMagnitude, 1.0, 1e-9));
            CHECK (smp.rvDirErrorDeg < 1e-5);
            CHECK (smp.rvMagnitude >= ana::kRvMagnitudeMin && smp.rvMagnitude <= ana::kRvMagnitudeMax);
        }
    }
    // maxRe: rE direction error within acceptance for horizontal sources
    {
        dec::DesignOptions o; o.type = dec::Type::sad; o.weighting = dec::Weighting::maxRe;
        o.normalization = dec::NormalizationMode::energy;
        const auto r = dec::design (layout, o);
        for (double az : { 0.0, 20.0, 137.0 })
        {
            const auto smp = ana::analyzeDirection (r.matrix, layout, az, 0.0);
            CHECK (smp.reDirErrorDeg < ana::kReDirectionErrorMaxDeg);
        }
        // elevated source: a coplanar ring cannot image elevation -> large error
        const auto elevated = ana::analyzeDirection (r.matrix, layout, 40.0, 30.0);
        CHECK (elevated.reDirErrorDeg > 20.0);
    }
}

//==============================================================================
// D10 - rV/rE golden spot values (SAD maxRe energy) across fixtures
//==============================================================================
static void testRvReGoldens()
{
    const auto doc = loadDecoderJson();
    const auto fixtures = doc["fixtures"];
    for (int fi = 0; fi < fixtures.size(); ++fi)
    {
        const auto fx = fixtures[fi];
        const auto layout = layoutFromFixture (fx);
        dec::DesignOptions o; o.type = dec::Type::sad; o.weighting = dec::Weighting::maxRe;
        o.normalization = dec::NormalizationMode::energy;
        const auto r = dec::design (layout, o);

        const auto samples = fx["rvre"]["samples"];
        for (int si = 0; si < samples.size(); ++si)
        {
            const auto gs = samples[si];
            const auto smp = ana::analyzeDirection (r.matrix, layout,
                                                    dnum (gs["azimuthDeg"]), dnum (gs["elevationDeg"]));
            CHECK (dapprox (smp.rvMagnitude, dnum (gs["rvMagnitude"]), 1e-9));
            CHECK (dapprox (smp.reMagnitude, dnum (gs["reMagnitude"]), 1e-9));
            CHECK (dapprox (smp.energy, dnum (gs["energy"]), 1e-9));
            CHECK (dapprox (smp.rvDirErrorDeg, dnum (gs["rvDirErrorDeg"]), 1e-5));
            CHECK (dapprox (smp.reDirErrorDeg, dnum (gs["reDirErrorDeg"]), 1e-5));
        }
    }
}

//==============================================================================
// D11 - export round-trips
//==============================================================================
static void testExports()
{
    const auto doc = loadDecoderJson();
    const auto layout = layoutFromFixture (findFixture (doc, "dome24"));
    dec::DesignOptions o; o.type = dec::Type::sad; o.weighting = dec::Weighting::maxRe;
    const auto r = dec::design (layout, o);

    // matrix JSON round-trip is faithful to ~1 ULP (%.17g writes exact digits;
    // juce::JSON's number parser is not correctly-rounded to the last bit, so a
    // few values differ by an ULP -- fine for FR-18 interchange).
    const auto json = ana::decoderMatrixToJsonString (r.matrix);
    dec::DecoderMatrix loaded;
    CHECK (ana::decoderMatrixFromJson (json, loaded));
    CHECK (loaded.numSpeakers == r.matrix.numSpeakers && loaded.order == r.matrix.order);
    for (size_t i = 0; i < r.matrix.d.size(); ++i)
        CHECK (std::abs (loaded.d[i] - r.matrix.d[i]) <= 1e-13);

    // analysis CSV: header + 72*37 rows
    const auto grid = ana::analyzeGrid (r.matrix, layout);
    CHECK ((int) grid.size() == 72 * 37);
    const auto csv = ana::toCsv (grid);
    const auto lines = juce::StringArray::fromLines (csv);
    // header + N rows + trailing empty from the final newline
    CHECK (lines[0].startsWith ("azimuthDeg,elevationDeg"));
    int dataRows = 0;
    for (int i = 1; i < lines.size(); ++i)
        if (lines[i].isNotEmpty()) ++dataRows;
    CHECK (dataRows == 72 * 37);

    // matrix CSV row/col count
    const auto mcsv = ana::decoderMatrixToCsv (r.matrix);
    const auto mlines = juce::StringArray::fromLines (mcsv);
    int mrows = 0;
    for (const auto& ln : mlines) if (ln.isNotEmpty()) ++mrows;
    CHECK (mrows == r.matrix.numSpeakers);
    CHECK (juce::StringArray::fromTokens (mlines[0], ",", "").size() == dsh::numChannels (r.matrix.order));
}

//==============================================================================
// D12 - builder rebuild -> publish -> acquire, double-buffer hot-swap
//==============================================================================
static void testBuilder()
{
    const auto doc = loadDecoderJson();
    const auto ring = layoutFromFixture (findFixture (doc, "ring24"));   // 24 spk, order 3
    const auto icosa = layoutFromFixture (findFixture (doc, "icosahedron12"));   // 12 spk, order 2

    xoa::DecoderMatrixBuilder builder;
    dec::DesignOptions o; o.type = dec::Type::sad; o.weighting = dec::Weighting::maxRe;

    const auto res1 = builder.rebuild (ring, o);
    builder.publish();
    const auto h1 = builder.acquire();
    CHECK (h1.epoch == 1);
    CHECK (h1.numSpeakers == 24 && h1.designOrder == 3);

    // float RT copy == (float) double master, zero-padded above the design order
    const int K1 = dsh::numChannels (3);
    for (int s = 0; s < 24; ++s)
    {
        for (int c = 0; c < K1; ++c)
            CHECK (h1.matrix[(size_t) s * xoa::kNumSHChannels + c] == (float) res1.matrix.at (s, c));
        for (int c = K1; c < xoa::kNumSHChannels; ++c)
            CHECK (h1.matrix[(size_t) s * xoa::kNumSHChannels + c] == 0.0f);   // zero-pad
    }

    // capture the epoch-1 buffer pointer + a sentinel value
    const float* oldBuf = h1.matrix;
    const float oldVal = h1.matrix[0];

    // second layout -> epoch 2, new dims; the OLD buffer must be untouched
    builder.rebuild (icosa, o);
    builder.publish();
    const auto h2 = builder.acquire();
    CHECK (h2.epoch == 2);
    CHECK (h2.numSpeakers == 12 && h2.designOrder == 2);
    CHECK (h2.matrix != oldBuf);                // flipped to the other buffer
    CHECK (oldBuf[0] == oldVal);                // epoch-1 data intact (double-buffer)

    // an unpublished rebuild does not change what acquire() sees
    builder.rebuild (ring, o);
    CHECK (builder.acquire().epoch == 2);
}

//==============================================================================
// D14 - store readers
//==============================================================================
static void testStoreReaders()
{
    xoa::XoaValueTreeState state;   // fresh project = 24-speaker ring

    const auto layout = xoa::DecoderMatrixBuilder::layoutFromStore (state);
    CHECK (layout.count == xoa::kDefaultSpeakers);   // 24
    CHECK (dapprox (layout.positions[0].x, 2.0, 1e-9));   // spk0 at (2,0,0) front
    CHECK (std::abs (layout.positions[0].y) < 1e-9);
    CHECK (dapprox (layout.positions[6].y, 2.0, 1e-9));   // spk6 at (0,2,0) left
    CHECK (std::abs (layout.positions[6].x) < 1e-9);

    const auto opts = xoa::DecoderMatrixBuilder::optionsFromStore (state);
    CHECK (opts.type == dec::Type::sad);                    // decoderType default 0
    CHECK (opts.weighting == dec::Weighting::maxRe);        // decoderWeighting default 1
    CHECK (opts.normalization == dec::NormalizationMode::energy);   // decoderNormalization default 1
}

//==============================================================================
//==============================================================================
// WP7 T1 — the committed t-design table, re-verified with the production SH
// evaluator (independent of the generator's 40-digit mpmath check). The
// production evaluator caps at order 10, so strength >= 21 is established in
// two steps that only need order-10 evaluations:
//   1. Discrete orthogonality (Gram): sum_i Y_c1(x_i) Y_c2(x_i) ==
//      N * delta_{c1,c2} / (2 l_c + 1) under SN3D. Products of two
//      degree-<=10 SHs span degrees <= 20, so this proves exact quadrature
//      through degree 20 — and it is precisely the property that makes the
//      AllRAD virtual sampling decode exact.
//   2. Antipodal symmetry (checked structurally): Y_l(-x) = (-1)^l Y_l(x),
//      so every ODD degree — including 21 — sums to zero by construction.
//==============================================================================
static void testTDesignTable()
{
    using xoa::tdesign::kCount;
    using xoa::tdesign::kPoints;

    CHECK (xoa::tdesign::kStrength >= 2 * xoa::kAmbisonicOrder + 1);
    CHECK (kCount >= 4);

    // Unit norms + antipodal pairing (structural half of the proof).
    double worstNorm = 0.0;
    int unpaired = 0;
    for (int i = 0; i < kCount; ++i)
    {
        const double* p = kPoints[i];
        worstNorm = std::max (worstNorm,
                              std::abs (std::sqrt (p[0]*p[0] + p[1]*p[1] + p[2]*p[2]) - 1.0));

        bool found = false;
        for (int j = 0; j < kCount && ! found; ++j)
            found = std::abs (kPoints[j][0] + p[0]) < 1.0e-12
                 && std::abs (kPoints[j][1] + p[1]) < 1.0e-12
                 && std::abs (kPoints[j][2] + p[2]) < 1.0e-12;
        if (! found)
            ++unpaired;
    }
    CHECK (worstNorm < 1.0e-13);
    CHECK (unpaired == 0);

    // Gram check at order 10 (proves quadrature exactness through degree 20).
    constexpr int order = xoa::kAmbisonicOrder;
    constexpr int K = xoa::kNumSHChannels;
    std::vector<double> y ((size_t) kCount * K);
    double shBuf[xoa::kNumSHChannels];
    for (int i = 0; i < kCount; ++i)
    {
        const double* p = kPoints[i];
        dsh::evaluate (xoa::coords::Cartesian { p[0], p[1], p[2] }, order, shBuf);
        std::copy (shBuf, shBuf + K, y.begin() + (size_t) i * K);
    }

    double worst = 0.0;
    for (int c1 = 0; c1 < K; ++c1)
        for (int c2 = c1; c2 < K; ++c2)
        {
            double acc = 0.0;
            for (int i = 0; i < kCount; ++i)
                acc += y[(size_t) i * K + c1] * y[(size_t) i * K + c2];
            const double expected = (c1 == c2)
                ? (double) kCount / (2.0 * dsh::acnToOrder (c1) + 1.0)
                : 0.0;
            worst = std::max (worst, std::abs (acc - expected));
        }
    CHECK (worst < 1.0e-9 * kCount);
}

//==============================================================================
// WP7 C3 — AllRAD behavioral anchors (golden-free). Raw-matrix goldens vs a
// scipy reference land in C3b; here we verify the decoder localizes correctly
// on enclosing rigs, reports imaginary speakers, and falls back honestly.
//==============================================================================
static void testAllRad()
{
    const auto doc = loadDecoderJson();

    // Icosahedron encloses the listener: AllRAD designs at the full bus order,
    // no fallback, and localizes (rE points at the source) across the sphere.
    {
        const auto layout = layoutFromFixture (findFixture (doc, "icosahedron12"));
        dec::DesignOptions o;
        o.type = dec::Type::allRad;   // default maxRe / energy
        const auto r = dec::design (layout, o);
        CHECK (r.matrix.numSpeakers == 12);
        CHECK (r.designOrder == xoa::kAmbisonicOrder);
        CHECK (! r.allRadFellBack);

        double worstReErr = 0.0, worstReMag = 0.0;
        int n = 0;
        for (int elDeg = -60; elDeg <= 60; elDeg += 30)
            for (int azDeg = -180; azDeg < 180; azDeg += 30)
            {
                const auto s = ana::analyzeDirection (r.matrix, layout, azDeg, elDeg);
                if (! s.reValid)
                    continue;
                worstReErr = std::max (worstReErr, s.reDirErrorDeg);
                worstReMag = std::max (worstReMag, s.reMagnitude);
                ++n;
            }
        CHECK (n > 0);
        CHECK (worstReErr < 22.0);    // 12-speaker rig is coarse; VBAP still localizes
        CHECK (worstReMag <= 1.0 + 1e-9);   // energy vector never blows up
    }

    // Dome (missing floor) inserts an imaginary nadir; still no fallback.
    {
        const auto layout = layoutFromFixture (findFixture (doc, "dome24"));
        dec::DesignOptions o; o.type = dec::Type::allRad;
        const auto r = dec::design (layout, o);
        CHECK (! r.allRadFellBack);
        CHECK (r.numImaginarySpeakers >= 1);
        bool insertedNote = false;
        for (const auto& w : r.warnings)
            if (w.containsIgnoreCase ("imaginary")) insertedNote = true;
        CHECK (insertedNote);
        // Above-rim sources localize.
        const auto up = ana::analyzeDirection (r.matrix, layout, 40.0, 30.0);
        CHECK (up.reValid);
        CHECK (up.reDirErrorDeg < 22.0);
    }

    // A frontal wedge cannot enclose the listener -> AllRAD declines and the
    // designer falls back to SAD (a valid, non-empty matrix + a flag).
    {
        dec::SpeakerLayout wedge;
        int idx = 0;
        for (double azDeg : { -60.0, 0.0, 60.0 })
            for (double elDeg : { -50.0, 50.0 })
            {
                const double az = juce::degreesToRadians (azDeg), el = juce::degreesToRadians (elDeg);
                wedge.positions[idx++] = { 2.0 * std::cos (el) * std::cos (az),
                                           2.0 * std::cos (el) * std::sin (az),
                                           2.0 * std::sin (el) };
            }
        wedge.positions[idx++] = { 2.0, 0.0, 0.0 };
        wedge.count = idx;

        dec::DesignOptions o; o.type = dec::Type::allRad;
        const auto r = dec::design (wedge, o);
        CHECK (r.allRadFellBack);
        CHECK (r.matrix.numSpeakers == wedge.count);
        double energy = 0.0;
        for (double v : r.matrix.d) energy += v * v;
        CHECK (energy > 0.0);   // real SAD matrix, not zeros
    }
}

//==============================================================================
// WP7 C5 — dual-band factorization. For every decoder family, the dual-band
// matrix times its HF diagonal must reproduce the single-band max-rE decode
// exactly: that identity is what lets one decode GEMM serve both bands.
//==============================================================================
static void testDualBandFactorization()
{
    const auto doc = loadDecoderJson();
    for (const char* name : { "ring24", "dome24", "icosahedron12" })
    {
        const auto layout = layoutFromFixture (findFixture (doc, name));
        for (int typeI = 0; typeI <= 2; ++typeI)   // sad, modeMatch, allRad
        {
            dec::DesignOptions single;
            single.type = static_cast<dec::Type> (typeI);
            single.weighting = dec::Weighting::maxRe;
            single.normalization = dec::NormalizationMode::energy;
            const auto rs = dec::design (layout, single);

            dec::DesignOptions dual = single;
            dual.dualBand = true;
            dual.crossoverHz = 400.0;
            const auto rd = dec::design (layout, dual);

            CHECK (rd.dualBand);
            CHECK (rd.crossoverHz == 400.0);
            CHECK ((int) rd.hfDiagonal.size() == dsh::numChannels (rd.matrix.order));
            CHECK (rs.matrix.order == rd.matrix.order);
            CHECK (rs.matrix.numSpeakers == rd.matrix.numSpeakers);

            const int K = dsh::numChannels (rd.matrix.order);
            double worst = 0.0;
            for (int s = 0; s < rd.matrix.numSpeakers; ++s)
                for (int c = 0; c < K; ++c)
                    worst = std::max (worst, std::abs (rs.matrix.at (s, c)
                                          - rd.matrix.at (s, c) * rd.hfDiagonal[(size_t) c]));
            CHECK (worst < 1e-12);

            // HF/LF differ (max-rE below top order is < 1): the diagonal is not
            // all ones, so dual-band is doing real work.
            if (rd.matrix.order >= 1)
            {
                double spread = 0.0;
                for (int c = 0; c < K; ++c)
                    spread = std::max (spread, std::abs (rd.hfDiagonal[(size_t) c] - rd.hfDiagonal[0]));
                CHECK (spread > 1e-6);
            }
        }
    }
    // suggestion anchors + clamps
    CHECK (std::abs (dec::suggestedCrossoverHz (2.0) - 400.0) < 1e-9);
    CHECK (dec::suggestedCrossoverHz (100.0) == 80.0);
    CHECK (dec::suggestedCrossoverHz (0.01) == 2000.0);
}

void runXoaDecoderTests()
{
    testSvdClosedForms();
    testSvdOnFixtures();
    testMoorePenrose();
    testGoldenMatrices();
    testSadEqualsModeMatch();
    testModeMatchReconstruction();
    testConditionAndClamp();
    testNormalizationAnchors();
    testClassify();
    testRingAnalytics();
    testRvReGoldens();
    testExports();
    testBuilder();
    testStoreReaders();
    testTDesignTable();
    testAllRad();
    testDualBandFactorization();
}
