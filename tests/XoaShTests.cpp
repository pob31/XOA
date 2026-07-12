/*
    XoaShTests.cpp - WP3 tests: spherical-harmonic evaluation, conventions,
    order weights, and FR-7 order adaptation. Mapped to the WP3 exit criteria:
      goldens ................. S3
      orthonormality (1e-10) .. S4
      pole/axis symmetry ...... S5
      FuMa round-trip ......... S11 (non-self-referential)
      max-rE vs published ..... S7

    Golden data in tests/data/*.json is produced by tools/reference/gen_*.py
    (mpmath, ~40-digit references). Loaded here via juce::JSON.
*/

#include "XoaTestFramework.h"

#include "DSP/AmbiConventions.h"
#include "DSP/AmbiOrderWeights.h"
#include "DSP/AmbiSphericalHarmonics.h"
#include "XoaConstants.h"

#include <array>
#include <cmath>
#include <vector>

namespace sh = xoa::sh;
namespace conv = xoa::conv;
namespace weights = xoa::weights;

//==============================================================================
static juce::var loadJson (const char* name)
{
    const auto file = juce::File (XOA_TESTS_DATA_DIR).getChildFile (name);
    const auto parsed = juce::JSON::parse (file.loadFileAsString());
    CHECK (parsed.isObject());
    return parsed;
}

// JUCE parses whole numbers as int64 and decimals as double; the cast covers both.
static double num (const juce::var& v) { return static_cast<double> (v); }

static bool approx (double a, double b, double tol) noexcept { return std::abs (a - b) <= tol; }

static double maxAbsDiff (const double* a, const double* b, int n) noexcept
{
    double m = 0.0;
    for (int i = 0; i < n; ++i)
        m = std::max (m, std::abs (a[i] - b[i]));
    return m;
}

//==============================================================================
// S1 - indexing invariants
//==============================================================================
static void testIndexing()
{
    CHECK (sh::numChannels (xoa::kAmbisonicOrder) == xoa::kNumSHChannels);

    // acn <-> (order, degree) bijective over all 121 channels
    for (int c = 0; c < xoa::kNumSHChannels; ++c)
    {
        const int l = sh::acnToOrder (c);
        const int m = sh::acnToDegree (c);
        CHECK (l >= 0 && l <= xoa::kAmbisonicOrder);
        CHECK (m >= -l && m <= l);
        CHECK (sh::acn (l, m) == c);
    }

    // legendreIndex bijective over the 66 packed entries
    int expected = 0;
    for (int l = 0; l <= xoa::kAmbisonicOrder; ++l)
        for (int m = 0; m <= l; ++m)
            CHECK (sh::legendreIndex (l, m) == expected++);
    CHECK (expected == sh::numLegendreEntries (xoa::kAmbisonicOrder));
    CHECK (sh::numLegendreEntries (xoa::kAmbisonicOrder) == sh::kMaxLegendreEntries);

    // order 0 evaluates to {1}
    double y0 = -1.0;
    sh::evaluate (0.0, 0.0, 0, &y0);
    CHECK (y0 == 1.0);
}

//==============================================================================
// S2 - closed-form anchors (AmbiX paper), a third reference besides goldens
//==============================================================================
static void testClosedForms()
{
    const double s3 = std::sqrt (3.0);
    struct Dir { double azDeg, elDeg; };
    const Dir dirs[] = { {0,0}, {90,0}, {180,0}, {-90,0}, {40,25}, {-120,-60} };

    for (const auto& d : dirs)
    {
        const double az = juce::degreesToRadians (d.azDeg);
        const double el = juce::degreesToRadians (d.elDeg);
        const double ca = std::cos (az), sa = std::sin (az);
        const double ce = std::cos (el), se = std::sin (el);

        double y[xoa::kNumSHChannels];
        sh::evaluate (d.azDeg, d.elDeg, 2, y);

        CHECK (approx (y[0], 1.0, 1e-14));                         // W
        CHECK (approx (y[1], ce * sa, 1e-14));                     // Y (1,-1)
        CHECK (approx (y[2], se, 1e-14));                          // Z (1, 0)
        CHECK (approx (y[3], ce * ca, 1e-14));                     // X (1,+1)
        CHECK (approx (y[4], (s3 / 2) * ce * ce * std::sin (2 * az), 1e-14));  // V
        CHECK (approx (y[5], (s3 / 2) * std::sin (2 * el) * sa, 1e-14));       // T
        CHECK (approx (y[6], (3 * se * se - 1) / 2, 1e-14));                   // R
        CHECK (approx (y[7], (s3 / 2) * std::sin (2 * el) * ca, 1e-14));       // S
        CHECK (approx (y[8], (s3 / 2) * ce * ce * std::cos (2 * az), 1e-14));  // U
    }
}

//==============================================================================
// S3 - golden SH values, all 121 channels
//==============================================================================
static void testGoldenSh()
{
    const auto doc = loadJson ("sh_reference.json");
    const int order = static_cast<int> (num (doc["order"]));
    CHECK (order == xoa::kAmbisonicOrder);
    const double tol = num (doc["provenance"]["testTolerance"]);   // 1e-13

    const auto dirs = doc["directions"];
    CHECK (dirs.size() > 0);
    for (int d = 0; d < dirs.size(); ++d)
    {
        const auto entry = dirs[d];
        double y[xoa::kNumSHChannels];
        sh::evaluate (num (entry["azimuthDeg"]), num (entry["elevationDeg"]), order, y);

        const auto ref = entry["sh"];
        CHECK (ref.size() == xoa::kNumSHChannels);
        for (int c = 0; c < xoa::kNumSHChannels; ++c)
            CHECK (approx (y[c], num (ref[c]), tol));
    }
}

//==============================================================================
// S4 - orthonormality over the committed quadrature grid (N3D basis).
// Sign-blind (a CS-phase flip preserves orthonormality) - signs are S2/S3/S5's job.
//==============================================================================
static void testOrthonormality()
{
    const auto doc = loadJson ("sh_quadrature.json");
    const double tol = num (doc["provenance"]["testTolerance"]);   // 1e-10
    const auto points = doc["points"];
    const int N = xoa::kNumSHChannels;

    double n3dGains[xoa::kNumSHChannels];
    conv::sn3dToN3dGains (xoa::kAmbisonicOrder, n3dGains);

    std::vector<double> E (static_cast<size_t> (N) * N, 0.0);
    for (int p = 0; p < points.size(); ++p)
    {
        const auto pt = points[p];
        const double w = num (pt["weight"]);
        double y[xoa::kNumSHChannels];
        sh::evaluate (num (pt["azimuthDeg"]), num (pt["elevationDeg"]), xoa::kAmbisonicOrder, y);
        for (int c = 0; c < N; ++c)
            y[c] *= n3dGains[c];   // SN3D -> N3D

        for (int a = 0; a < N; ++a)
        {
            const double wy = w * y[a];
            double* row = &E[static_cast<size_t> (a) * N];
            for (int b = 0; b < N; ++b)
                row[b] += wy * y[b];
        }
    }

    double worst = 0.0;
    for (int a = 0; a < N; ++a)
        for (int b = 0; b < N; ++b)
            worst = std::max (worst, std::abs (E[static_cast<size_t> (a) * N + b] - (a == b ? 1.0 : 0.0)));
    CHECK (worst < tol);
}

//==============================================================================
// S5 - properties
//==============================================================================
static void testProperties()
{
    const int order = xoa::kAmbisonicOrder;
    double y[xoa::kNumSHChannels], y2[xoa::kNumSHChannels];

    // (a) +Z: Y_{l,0} = 1, m != 0 channels vanish. -Z: Y_{l,0} = (-1)^l.
    sh::evaluate (0.0, 90.0, order, y);
    for (int c = 0; c < xoa::kNumSHChannels; ++c)
    {
        if (sh::acnToDegree (c) == 0) CHECK (approx (y[c], 1.0, 1e-14));
        else                          CHECK (std::abs (y[c]) < 1e-13);   // ~0 (pi/2 irrational)
    }
    sh::evaluate (0.0, -90.0, order, y);
    for (int c = 0; c < xoa::kNumSHChannels; ++c)
    {
        const int l = sh::acnToOrder (c), m = sh::acnToDegree (c);
        if (m == 0) CHECK (approx (y[c], (l % 2 == 0) ? 1.0 : -1.0, 1e-13));
        else        CHECK (std::abs (y[c]) < 1e-13);
    }

    // (b) m = 0 channels are azimuth-invariant
    sh::evaluate (30.0, 20.0, order, y);
    sh::evaluate (200.0, 20.0, order, y2);
    for (int c = 0; c < xoa::kNumSHChannels; ++c)
        if (sh::acnToDegree (c) == 0)
            CHECK (approx (y[c], y2[c], 1e-14));

    // (c) az + 180 scales each channel by (-1)^|m|
    sh::evaluate (33.0, -12.0, order, y);
    sh::evaluate (33.0 + 180.0, -12.0, order, y2);
    for (int c = 0; c < xoa::kNumSHChannels; ++c)
    {
        const int m = std::abs (sh::acnToDegree (c));
        CHECK (approx (y2[c], (m % 2 == 0 ? 1.0 : -1.0) * y[c], 1e-14));
    }

    // (d) antipode parity Y(-d) = (-1)^l Y(d)  (via cartesian)
    const xoa::coords::Cartesian dvec { 0.4, -0.7, 0.55 };
    sh::evaluate (dvec, order, y);
    sh::evaluate (xoa::coords::Cartesian { -dvec.x, -dvec.y, -dvec.z }, order, y2);
    for (int c = 0; c < xoa::kNumSHChannels; ++c)
    {
        const int l = sh::acnToOrder (c);
        CHECK (approx (y2[c], (l % 2 == 0 ? 1.0 : -1.0) * y[c], 1e-14));
    }

    // (e) left-right mirror az -> -az: m < 0 channels negate, m >= 0 unchanged
    // (pre-validates the WP4 mirror table)
    sh::evaluate (57.0, 15.0, order, y);
    sh::evaluate (-57.0, 15.0, order, y2);
    for (int c = 0; c < xoa::kNumSHChannels; ++c)
    {
        const int m = sh::acnToDegree (c);
        CHECK (approx (y2[c], (m < 0 ? -1.0 : 1.0) * y[c], 1e-14));
    }

    // (f) +X and +Y cardinal slots (kills az-sense + sin/cos-slot bugs)
    sh::evaluate (0.0, 0.0, order, y);           // +X front
    CHECK (approx (y[sh::acn (0, 0)], 1.0, 1e-14));
    CHECK (approx (y[sh::acn (1, -1)], 0.0, 1e-14));  // Y
    CHECK (approx (y[sh::acn (1, 0)], 0.0, 1e-14));   // Z
    CHECK (approx (y[sh::acn (1, 1)], 1.0, 1e-14));   // X
    for (int c = 0; c < xoa::kNumSHChannels; ++c)
        if (sh::acnToDegree (c) < 0)
            CHECK (std::abs (y[c]) < 1e-14);          // all m<0 (sin) vanish at az=0
    sh::evaluate (90.0, 0.0, order, y);          // +Y left
    CHECK (approx (y[sh::acn (1, -1)], 1.0, 1e-14));  // Y = 1
    CHECK (approx (y[sh::acn (1, 1)], 0.0, 1e-14));   // X = 0

    // (g) SN3D addition-theorem sentinel: sum_m Y_{l,m}^2 = 1 per order, at
    // near-pole directions where recurrence error would show first
    const double stress[][2] = { {12.3, 89.99}, {-45.6, -89.99}, {200.0, 60.0}, {5.0, -30.0} };
    for (const auto& s : stress)
    {
        sh::evaluate (s[0], s[1], order, y);
        for (int l = 0; l <= order; ++l)
        {
            double sum = 0.0;
            for (int m = -l; m <= l; ++m)
                sum += y[sh::acn (l, m)] * y[sh::acn (l, m)];
            CHECK (approx (sum, 1.0, 1e-12));
        }
    }
}

//==============================================================================
// S6 - API consistency
//==============================================================================
static void testApiConsistency()
{
    const int order = xoa::kAmbisonicOrder;
    double a[xoa::kNumSHChannels], b[xoa::kNumSHChannels];

    // degrees == radians
    sh::evaluate (37.0, -22.0, order, a);
    sh::evaluateRadians (juce::degreesToRadians (37.0), juce::degreesToRadians (-22.0), order, b);
    CHECK (maxAbsDiff (a, b, xoa::kNumSHChannels) < 1e-15);

    // azimuth wrap 370 == 10
    sh::evaluate (370.0, 5.0, order, a);
    sh::evaluate (10.0, 5.0, order, b);
    CHECK (maxAbsDiff (a, b, xoa::kNumSHChannels) < 1e-15);

    // Cartesian == Spherical for the same direction
    const xoa::coords::Spherical sph { 3.0, 48.0, -17.0 };   // radius ignored
    const auto cart = xoa::coords::sphericalToCartesian (sph);
    sh::evaluate (sph, order, a);
    sh::evaluate (cart, order, b);
    CHECK (maxAbsDiff (a, b, xoa::kNumSHChannels) < 1e-13);

    // r = 0 -> front
    sh::evaluate (xoa::coords::Cartesian { 0.0, 0.0, 0.0 }, order, a);
    sh::evaluate (0.0, 0.0, order, b);
    CHECK (maxAbsDiff (a, b, xoa::kNumSHChannels) < 1e-15);

    // Legendre 1-arg == 2-arg
    double p1[sh::kMaxLegendreEntries], p2[sh::kMaxLegendreEntries];
    const double x = std::sin (juce::degreesToRadians (33.0));
    sh::evaluateSchmidtLegendre (x, order, p1);
    sh::evaluateSchmidtLegendre (x, std::cos (juce::degreesToRadians (33.0)), order, p2);
    CHECK (maxAbsDiff (p1, p2, sh::numLegendreEntries (order)) < 1e-15);
}

//==============================================================================
// S7 - max-rE weights
//==============================================================================
static void testMaxRe()
{
    const auto doc = loadJson ("order_weights.json");
    const auto orders = doc["orders"];

    for (int i = 0; i < orders.size(); ++i)
    {
        const auto entry = orders[i];
        const int N = static_cast<int> (num (entry["order"]));
        CHECK (approx (weights::maxReCosine (N), num (entry["rE"]), 1e-14));

        double w[xoa::kAmbisonicOrder + 1];
        weights::maxRe (N, w);
        const auto ref = entry["maxRe"];
        CHECK (ref.size() == N + 1);
        CHECK (w[0] == 1.0);
        for (int l = 0; l <= N; ++l)
        {
            CHECK (approx (w[l], num (ref[l]), 1e-14));
            if (l > 0) CHECK (w[l] < w[l - 1]);   // strictly decreasing
        }
    }

    // exact anchors N = 1..3
    CHECK (approx (weights::maxReCosine (1), 1.0 / std::sqrt (3.0), 1e-15));
    CHECK (approx (weights::maxReCosine (2), std::sqrt (3.0 / 5.0), 1e-15));
    CHECK (approx (weights::maxReCosine (3),
                   std::sqrt ((3.0 + 2.0 * std::sqrt (6.0 / 5.0)) / 7.0), 1e-15));

    // seed-band sanity (independent of goldens)
    for (int N = 1; N <= xoa::kAmbisonicOrder; ++N)
    {
        const double seed = std::cos (juce::degreesToRadians (137.9 / (N + 1.51)));
        CHECK (std::abs (weights::maxReCosine (N) - seed) < 5e-3);
    }
}

//==============================================================================
// S8 - in-phase weights
//==============================================================================
static void testInPhase()
{
    const auto doc = loadJson ("order_weights.json");
    const auto orders = doc["orders"];
    for (int i = 0; i < orders.size(); ++i)
    {
        const auto entry = orders[i];
        const int N = static_cast<int> (num (entry["order"]));
        double w[xoa::kAmbisonicOrder + 1];
        weights::inPhase (N, w);
        const auto ref = entry["inPhase"];
        for (int l = 0; l <= N; ++l)
            CHECK (approx (w[l], num (ref[l]), 1e-15));
    }

    // rational anchors
    double w1[2], w2[3], w3[4];
    weights::inPhase (1, w1); CHECK (approx (w1[1], 1.0 / 3.0, 1e-15));
    weights::inPhase (2, w2); CHECK (approx (w2[1], 0.5, 1e-15) && approx (w2[2], 0.1, 1e-15));
    weights::inPhase (3, w3);
    CHECK (approx (w3[1], 0.6, 1e-15) && approx (w3[2], 0.2, 1e-15) && approx (w3[3], 1.0 / 35.0, 1e-15));
}

//==============================================================================
// S9 - basic weights + perChannel mapping
//==============================================================================
static void testBasicAndPerChannel()
{
    double b[xoa::kAmbisonicOrder + 1];
    weights::basic (xoa::kAmbisonicOrder, b);
    for (int l = 0; l <= xoa::kAmbisonicOrder; ++l)
        CHECK (b[l] == 1.0);

    // perOrder = order index itself, so perChannel[c] == acnToOrder(c)
    double perOrder[xoa::kAmbisonicOrder + 1];
    for (int l = 0; l <= xoa::kAmbisonicOrder; ++l)
        perOrder[l] = static_cast<double> (l);
    double perCh[xoa::kNumSHChannels];
    weights::perChannel (perOrder, xoa::kAmbisonicOrder, perCh);
    const int spots[] = { 0, 1, 3, 4, 9, 15, 120 };
    const int expect[] = { 0, 1, 1, 2, 3, 3, 10 };
    for (int i = 0; i < 7; ++i)
        CHECK (perCh[spots[i]] == static_cast<double> (expect[i]));
}

//==============================================================================
// S10 - N3D <-> SN3D
//==============================================================================
static void testConventions()
{
    const int spotL[] = { 0, 1, 2, 3, 10 };
    const double spotV[] = { 1.0, std::sqrt (3.0), std::sqrt (5.0), std::sqrt (7.0), std::sqrt (21.0) };
    for (int i = 0; i < 5; ++i)
        CHECK (approx (conv::sn3dToN3d (spotL[i]), spotV[i], 1e-15));

    // round trip sn3d -> n3d -> sn3d on a random-ish vector
    double toN3d[xoa::kNumSHChannels], toSn3d[xoa::kNumSHChannels];
    conv::sn3dToN3dGains (xoa::kAmbisonicOrder, toN3d);
    conv::n3dToSn3dGains (xoa::kAmbisonicOrder, toSn3d);
    double v[xoa::kNumSHChannels], tmp[xoa::kNumSHChannels], back[xoa::kNumSHChannels];
    for (int c = 0; c < xoa::kNumSHChannels; ++c)
        v[c] = 0.1 * c - 3.0;
    conv::applyGains (toN3d, xoa::kAmbisonicOrder, v, tmp);
    conv::applyGains (toSn3d, xoa::kAmbisonicOrder, tmp, back);
    CHECK (maxAbsDiff (v, back, xoa::kNumSHChannels) < 1e-13);

    // applyGains in-place (in == out)
    conv::applyGains (toN3d, xoa::kAmbisonicOrder, v, v);
    CHECK (approx (v[sh::acn (2, 0)], (0.1 * sh::acn (2, 0) - 3.0) * std::sqrt (5.0), 1e-13));
}

//==============================================================================
// S11 - FuMa -> AmbiX (non-self-referential: goldens derive gains from maxN)
//==============================================================================
static void testFuma()
{
    const auto doc = loadJson ("fuma_reference.json");
    const double tol = num (doc["provenance"]["testTolerance"]);   // 1e-13
    const auto dirs = doc["directions"];

    for (int d = 0; d < dirs.size(); ++d)
    {
        const auto entry = dirs[d];
        const auto fumaVar = entry["fuma"];
        const auto ambixVar = entry["ambix"];
        double fuma[conv::kNumFumaChannels], out[xoa::kNumSHChannels];
        for (int f = 0; f < conv::kNumFumaChannels; ++f)
            fuma[f] = num (fumaVar[f]);

        // full order-3 frame
        CHECK (conv::fumaToAmbix (fuma, 3, out));
        for (int c = 0; c < conv::kNumFumaChannels; ++c)
            CHECK (approx (out[c], num (ambixVar[c]), tol));

        // prefix truncation: order 1 uses only WXYZ (4 FuMa channels)
        double out1[4];
        CHECK (conv::fumaToAmbix (fuma, 1, out1));
        for (int c = 0; c < 4; ++c)
            CHECK (approx (out1[c], num (ambixVar[c]), tol));
    }

    // W spot: FuMa W = 1/sqrt2 -> SN3D W = 1
    double wOnly[1] = { 1.0 / std::sqrt (2.0) }, wOut[1];
    CHECK (conv::fumaToAmbix (wOnly, 0, wOut));
    CHECK (approx (wOut[0], 1.0, 1e-15));

    // rejection: order 4 returns false and zero-fills
    double big[25];
    for (double& x : big) x = 7.0;
    CHECK (! conv::fumaToAmbix (big, 4, big));
    for (int c = 0; c < sh::numChannels (4); ++c)
        CHECK (big[c] == 0.0);

    // channel-count -> order mapping
    CHECK (conv::fumaOrderForChannelCount (1) == 0);
    CHECK (conv::fumaOrderForChannelCount (4) == 1);
    CHECK (conv::fumaOrderForChannelCount (9) == 2);
    CHECK (conv::fumaOrderForChannelCount (16) == 3);
    CHECK (conv::fumaOrderForChannelCount (3) == -1);
    CHECK (conv::fumaOrderForChannelCount (25) == -1);
}

//==============================================================================
// S12 - FR-7 order adaptation, exact expected relationships
//==============================================================================
static void testOrderAdaptation()
{
    const int hi = xoa::kAmbisonicOrder, lo = 3;

    for (int t = 0; t < 10; ++t)
    {
        const double azDeg = -180.0 + 36.0 * t + 7.0;
        const double elDeg = -80.0 + 16.0 * t;

        double enc3[16], enc10[xoa::kNumSHChannels];
        sh::evaluate (azDeg, elDeg, lo, enc3);
        sh::evaluate (azDeg, elDeg, hi, enc10);

        // (a) upmix 3 -> 10: identical on channels 0..15, zero above; and a
        // generic direction has nonzero content above order 3 in a TRUE order-10
        // encoding (proves upmix invents no detail)
        double up[xoa::kNumSHChannels];
        weights::adaptOrder (enc3, lo, up, hi);
        for (int c = 0; c < 16; ++c)
            CHECK (approx (up[c], enc3[c], 1e-15));
        for (int c = 16; c < xoa::kNumSHChannels; ++c)
            CHECK (up[c] == 0.0);
        double aboveEnergy = 0.0;
        for (int c = 16; c < xoa::kNumSHChannels; ++c)
            aboveEnergy += std::abs (enc10[c]);
        CHECK (aboveEnergy > 1e-3);   // true enc10 has higher-order content

        // (b) downmix 10 -> 3 == maxRe(3)-weighted order-3 encoding (SH never
        // mixes across l, so truncated enc10 == enc3 exactly)
        double down[16];
        weights::adaptOrder (enc10, hi, down, lo);
        double reW[4], reCh[16];
        weights::maxRe (lo, reW);
        weights::perChannel (reW, lo, reCh);
        for (int c = 0; c < 16; ++c)
            CHECK (approx (down[c], reCh[c] * enc3[c], 1e-14));

        // (c) adaptOrder == zero-pad (x) orderAdaptGains  (WP6 folding contract)
        double gains[16];
        weights::orderAdaptGains (hi, lo, gains);
        for (int c = 0; c < 16; ++c)
            CHECK (approx (down[c], gains[c] * enc10[c], 1e-14));

        // (d) up then down == maxRe(3)-weighted original
        double upThenDown[16];
        weights::adaptOrder (up, hi, upThenDown, lo);
        for (int c = 0; c < 16; ++c)
            CHECK (approx (upThenDown[c], reCh[c] * enc3[c], 1e-14));
    }

    // in-place upmix aliasing (in == out)
    double frame[xoa::kNumSHChannels];
    sh::evaluate (12.0, 34.0, lo, frame);
    double ref[16];
    for (int c = 0; c < 16; ++c) ref[c] = frame[c];
    weights::adaptOrder (frame, lo, frame, hi);
    for (int c = 0; c < 16; ++c)
        CHECK (approx (frame[c], ref[c], 1e-15));
    for (int c = 16; c < xoa::kNumSHChannels; ++c)
        CHECK (frame[c] == 0.0);
}

//==============================================================================
// S13 - source-spread order taper (FR-5, WP8)
//==============================================================================
static double rENorm (const double* g, int N)   // ||rE|| from the standard formula
{
    double numr = 0.0, den = 0.0;
    for (int l = 0; l < N; ++l)   numr += 2.0 * (l + 1) * g[l] * g[l + 1];
    for (int l = 0; l <= N; ++l)  den  += (2.0 * l + 1.0) * g[l] * g[l];
    return den > 0.0 ? numr / den : 0.0;
}

static void testSpreadTaper()
{
    const int N = xoa::kAmbisonicOrder;
    double g[xoa::kAmbisonicOrder + 1];

    // sigma = 0 -> identity (point source): all weights 1.
    weights::spreadTaper (N, 0.0, g);
    for (int l = 0; l <= N; ++l)
        CHECK (approx (g[l], 1.0, 1e-12));

    // sigma = 180 -> omni: order 0 only, energy folded into g_0 = sqrt((N+1)^2).
    weights::spreadTaper (N, 180.0, g);
    CHECK (approx (g[0], static_cast<double> (N + 1), 1e-12));
    for (int l = 1; l <= N; ++l)
        CHECK (g[l] == 0.0);

    // Energy sum_l (2l+1) g_l^2 is spread-invariant == (N+1)^2, across a sweep,
    // g_0 stays positive/finite, and the taper never resurrects a zeroed order
    // (monotone cutoff -> the nonzero orders are a contiguous prefix). The rE
    // trajectory is UNIMODAL, not monotone: from basic (sigma 0) it rises to the
    // max-rE peak (the P_l taper passes through the rE-maximizing weights) and
    // then falls to 0 at omni. (The DEVPLAN's "monotonically decreasing" wording
    // is a plan oversight - a mild high-order taper sharpens rE before widening.)
    const double target = static_cast<double> ((N + 1) * (N + 1));
    const int steps = 36;
    std::vector<double> reSeq;
    for (int step = 0; step <= steps; ++step)
    {
        const double sigma = 180.0 * step / steps;
        weights::spreadTaper (N, sigma, g);

        double energy = 0.0;
        for (int l = 0; l <= N; ++l) energy += (2.0 * l + 1.0) * g[l] * g[l];
        CHECK (approx (energy, target, 1e-9));
        CHECK (g[0] > 0.0 && std::isfinite (g[0]));

        bool zeroed = false;
        for (int l = 0; l <= N; ++l)
        {
            if (g[l] == 0.0) zeroed = true;
            else CHECK (! zeroed);          // no order revives after a cut
        }
        reSeq.push_back (rENorm (g, N));
    }

    // Unimodal: strictly up to a peak, then strictly down; ends well below start.
    int peak = 0;
    for (int i = 1; i < (int) reSeq.size(); ++i) if (reSeq[(size_t) i] > reSeq[(size_t) peak]) peak = i;
    for (int i = 1; i <= peak; ++i)             CHECK (reSeq[(size_t) i]     >= reSeq[(size_t) i - 1] - 1e-9);
    for (int i = peak + 1; i < (int) reSeq.size(); ++i) CHECK (reSeq[(size_t) i] <= reSeq[(size_t) i - 1] + 1e-9);
    CHECK (reSeq.back() < 0.01);                                  // omni -> rE ~ 0
    CHECK (reSeq.back() < reSeq.front());                        // net widening

    // basic-weight anchor: at sigma = 0, ||rE|| = N/(N+1); the peak reaches the
    // order-N max-rE rE.
    CHECK (approx (reSeq.front(), static_cast<double> (N) / (N + 1), 1e-9));
    double reMax[xoa::kAmbisonicOrder + 1];
    weights::maxRe (N, reMax);
    CHECK (approx (reSeq[(size_t) peak], rENorm (reMax, N), 5e-3));

    // max-rE reproduction: at sigma/2 = acos(r_E(N)) the taper is the order-N
    // max-rE family (up to the shared energy scale).
    for (int order = 1; order <= N; ++order)
    {
        const double rE = weights::maxReCosine (order);
        const double sigma = 2.0 * juce::radiansToDegrees (std::acos (rE));
        weights::spreadTaper (order, sigma, g);

        double re[xoa::kAmbisonicOrder + 1];
        weights::maxRe (order, re);            // re[0] == 1
        for (int l = 0; l <= order; ++l)
            CHECK (approx (g[l] / g[0], re[l], 1e-11));   // ratios match max-rE
    }
}

//==============================================================================
void runXoaShTests()
{
    testIndexing();
    testClosedForms();
    testGoldenSh();
    testOrthonormality();
    testProperties();
    testApiConsistency();
    testMaxRe();
    testInPhase();
    testBasicAndPerChannel();
    testConventions();
    testFuma();
    testOrderAdaptation();
    testSpreadTaper();
}
