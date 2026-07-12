/*
    XoaNfcTests.cpp - WP8 C1: near-field compensation filters (FR-6).

    Validates, in order of what breaks worst:
      1. reverse-Bessel root residuals |theta_l(q)| ~ 0 (the table is correct)
      2. realized digital magnitude vs the double-precision digital goldens
         (the C++ bilinear coefficient math is correct)
      3. realized digital magnitude vs the physical Daniel analog curves, by band
         (the filter has the right shape), where the clamp is inactive
      4. exact DC gain (r_ref/r_src)^l (bilinear maps DC exactly)
      5. stability at the worst case (order 10, 44.1/96 kHz, clamped small radius)
      6. clamp: DC boost never exceeds the ceiling; continuity across the boundary
      7. SourceNfcBank end-to-end: lane 0 is the dry stem; each lane settles to
         its order's DC gain (ties the RT bank to designSourceSections)

    Goldens: tests/data/nfc_reference.json from tools/reference/gen_nfc_reference.py.
*/

#include "XoaTestFramework.h"

#include "XoaConstants.h"
#include "DSP/AmbiNFCFilter.h"
#include "DSP/AmbiNfcTables.h"

#include <juce_core/juce_core.h>

#include <cmath>
#include <complex>
#include <vector>

namespace
{
using cd = std::complex<double>;

juce::var loadNfcJson()
{
    const auto file = juce::File (XOA_TESTS_DATA_DIR).getChildFile ("nfc_reference.json");
    const auto parsed = juce::JSON::parse (file.loadFileAsString());
    CHECK (parsed.isObject());
    return parsed;
}

double num (const juce::var& v) { return static_cast<double> (v); }

// theta_l at a complex point via theta_l = (2l-1) theta_{l-1} + y^2 theta_{l-2}.
cd thetaEval (int l, cd y)
{
    cd tm2 (1.0, 0.0);          // theta_0
    cd tm1 = y + 1.0;           // theta_1
    if (l == 0) return tm2;
    if (l == 1) return tm1;
    cd t = tm1;
    for (int k = 2; k <= l; ++k)
    {
        t = (2.0 * k - 1.0) * tm1 + y * y * tm2;
        tm2 = tm1;
        tm1 = t;
    }
    return t;
}

// |H(e^{jw})| in dB for order l's sections within a designed page.
double orderDigitalMagDb (const float* page, int l, double freqHz, double sr)
{
    const double w = 2.0 * juce::MathConstants<double>::pi * freqHz / sr;
    const cd e1 = std::exp (cd (0.0, -w));
    const cd e2 = std::exp (cd (0.0, -2.0 * w));
    const int base  = xoa::nfc::tables::kSectionOffset[l];
    const int count = xoa::nfc::tables::kSectionCount[l];

    cd h (1.0, 0.0);
    for (int j = 0; j < count; ++j)
    {
        const float* c = page + (size_t) (base + j) * xoa::nfc::kCoeffsPerSection;
        const cd numr = (double) c[0] + (double) c[1] * e1 + (double) c[2] * e2;
        const cd denr = 1.0 + (double) c[3] * e1 + (double) c[4] * e2;
        h *= numr / denr;
    }
    return 20.0 * std::log10 (std::abs (h));
}

// DC gain (z = 1) in dB for order l's sections.
double orderDcGainDb (const float* page, int l)
{
    const int base  = xoa::nfc::tables::kSectionOffset[l];
    const int count = xoa::nfc::tables::kSectionCount[l];
    double g = 1.0;
    for (int j = 0; j < count; ++j)
    {
        const float* c = page + (size_t) (base + j) * xoa::nfc::kCoeffsPerSection;
        const double bsum = (double) c[0] + (double) c[1] + (double) c[2];
        const double asum = 1.0 + (double) c[3] + (double) c[4];
        g *= bsum / asum;
    }
    return 20.0 * std::log10 (std::abs (g));
}

//==============================================================================
void testBesselRootResiduals()
{
    for (int l = 1; l <= xoa::nfc::kMaxOrder; ++l)
    {
        const int base  = xoa::nfc::tables::kSectionOffset[l];
        const int count = xoa::nfc::tables::kSectionCount[l];
        for (int j = 0; j < count; ++j)
        {
            const auto r = xoa::nfc::tables::kRoots[base + j];
            const cd q (r.re, r.im);
            CHECK (std::abs (thetaEval (l, q)) < 1.0e-9);
            CHECK (r.re < 0.0);                     // every root strictly LHP
            if (r.im != 0.0)                        // conjugate must also be a root
                CHECK (std::abs (thetaEval (l, std::conj (q))) < 1.0e-9);
        }
    }
    // section bookkeeping matches ceil(l/2) and the packed total
    int total = 0;
    for (int l = 1; l <= xoa::nfc::kMaxOrder; ++l)
    {
        CHECK (xoa::nfc::tables::kSectionCount[l] == (l + 1) / 2);
        total += xoa::nfc::tables::kSectionCount[l];
    }
    CHECK (total == xoa::nfc::kSectionsPerSource);
    CHECK (xoa::nfc::kSectionsPerSource == 30);
    CHECK (xoa::nfc::kCoeffsPerSource == 150);
}

//==============================================================================
void testDigitalAndAnalogCurves()
{
    const auto doc = loadNfcJson();
    const auto* cases = doc["cases"].getArray();
    CHECK (cases != nullptr && cases->size() > 0);

    const auto& tol = doc["tolerances"];
    const double tolDigital = 0.05;                         // float-coeff safe; bugs are dB-scale
    const double tolBelow8  = num (tol["analogBelowFsOver8Db"]);
    const double tolBelow4  = num (tol["analogBelowFsOver4Db"]);
    const double tolAbove4  = num (tol["analogAboveFsOver4Db"]);

    std::vector<float> page ((size_t) xoa::nfc::kCoeffsPerSource);

    int digitalPts = 0, analogPts = 0;
    for (const auto& cv : *cases)
    {
        const double sr   = num (cv["sampleRate"]);
        const double rRef = num (cv["rRef"]);
        const double rSrc = num (cv["rSrc"]);
        const int    l    = (int) num (cv["order"]);
        const bool   clamped = (bool) cv["clampedByOrder"];

        xoa::nfc::designSourceSections (rSrc, rRef, sr, page.data());

        const auto* freqs  = cv["freqs"].getArray();
        const auto* magDig = cv["magDigitalDb"].getArray();
        const auto* magAna = cv["magAnalogDb"].getArray();
        CHECK (freqs != nullptr && magDig != nullptr && magAna != nullptr);

        for (int i = 0; i < freqs->size(); ++i)
        {
            const double f = num ((*freqs)[i]);
            const double got = orderDigitalMagDb (page.data(), l, f, sr);

            // (2) digital vs digital - pins the bilinear coefficient math.
            CHECK (std::abs (got - num ((*magDig)[i])) < tolDigital);
            ++digitalPts;

            // (3) digital vs Daniel analog - only where the clamp is inactive.
            if (! clamped)
            {
                const double band = (f < sr / 8.0) ? tolBelow8
                                  : (f < sr / 4.0) ? tolBelow4 : tolAbove4;
                CHECK (std::abs (got - num ((*magAna)[i])) < band);
                ++analogPts;
            }
        }

        // (4) exact DC gain.
        CHECK (std::abs (orderDcGainDb (page.data(), l) - num (cv["dcGainDb"])) < 0.02);
    }
    CHECK (digitalPts > 1000);
    CHECK (analogPts  > 100);
}

//==============================================================================
// A 2nd-order section is stable iff |a2| < 1 and |a1| < 1 + a2; a 1st-order
// (a2 == 0) iff |a1| < 1.
bool sectionStable (const float* c)
{
    const double a1 = c[3], a2 = c[4];
    if (c[2] == 0.0f && a2 == 0.0)          // 1st-order
        return std::abs (a1) < 1.0;
    return std::abs (a2) < 1.0 && std::abs (a1) < 1.0 + a2;
}

void testWorstCaseStability()
{
    std::vector<float> page ((size_t) xoa::nfc::kCoeffsPerSource);
    const double rSrcSmall = 0.05;          // below kMinSourceRadius -> clamped

    for (double sr : { 44100.0, 96000.0 })
        for (double rRef : { 0.5, 2.0, 10.0 })
        {
            xoa::nfc::designSourceSections (rSrcSmall, rRef, sr, page.data());

            for (int l = 1; l <= xoa::nfc::kMaxOrder; ++l)
            {
                const int base  = xoa::nfc::tables::kSectionOffset[l];
                const int count = xoa::nfc::tables::kSectionCount[l];
                for (int j = 0; j < count; ++j)
                    CHECK (sectionStable (page.data() + (size_t) (base + j) * xoa::nfc::kCoeffsPerSection));
            }

            // 1 s impulse through the full bank must decay, no NaN/Inf.
            xoa::nfc::SourceNfcBank bank;
            const int n = (int) sr;
            std::vector<float> dry ((size_t) n, 0.0f);
            dry[0] = 1.0f;
            std::vector<std::vector<float>> laneStore ((size_t) xoa::nfc::kNumLanes,
                                                       std::vector<float> ((size_t) n, 0.0f));
            float* lanePtrs[xoa::nfc::kNumLanes];
            for (int k = 0; k < xoa::nfc::kNumLanes; ++k) lanePtrs[k] = laneStore[(size_t) k].data();

            bank.processLanes (dry.data(), lanePtrs, n, page.data());

            for (int l = 1; l <= xoa::nfc::kMaxOrder; ++l)
            {
                const float* lane = lanePtrs[l];
                double tail = 0.0, peak = 0.0;
                for (int i = 0; i < n; ++i)
                {
                    CHECK (std::isfinite (lane[i]));
                    peak = std::max (peak, (double) std::abs (lane[i]));
                    if (i > n * 9 / 10) tail = std::max (tail, (double) std::abs (lane[i]));
                }
                CHECK (tail < peak * 1.0e-3 + 1.0e-6);   // decayed
            }
        }
}

//==============================================================================
void testClampCeilingAndContinuity()
{
    std::vector<float> page ((size_t) xoa::nfc::kCoeffsPerSource);
    const double rRef = 2.0, sr = 48000.0;

    // DC boost per order never exceeds the ceiling.
    xoa::nfc::designSourceSections (0.05, rRef, sr, page.data());   // very near -> clamped
    for (int l = 1; l <= xoa::nfc::kMaxOrder; ++l)
        CHECK (orderDcGainDb (page.data(), l) < xoa::nfc::kMaxBoostDb + 0.05);

    // Continuity: sweep r_src across each order's clamp boundary; DC gain must
    // not jump, and below the boundary it stays pinned at the ceiling.
    for (int l = 1; l <= xoa::nfc::kMaxOrder; ++l)
    {
        const double boundary = rRef * std::pow (10.0, -xoa::nfc::kMaxBoostDb / (20.0 * l));
        double prev = -1.0;
        for (double frac : { 1.5, 1.05, 1.0, 0.95, 0.5 })
        {
            const double rSrc = std::max (boundary * frac, 1.0e-4);
            xoa::nfc::designSourceSections (rSrc, rRef, sr, page.data());
            const double dc = orderDcGainDb (page.data(), l);
            if (rSrc >= boundary)
                CHECK (dc < xoa::nfc::kMaxBoostDb + 0.05);
            else
                CHECK (std::abs (dc - xoa::nfc::kMaxBoostDb) < 0.05);   // pinned at ceiling
            if (prev >= 0.0)
                CHECK (dc - prev > -0.05);        // monotone non-decreasing as source nears
            prev = dc;
        }
    }
}

//==============================================================================
void testBankLanesAndDcSettle()
{
    std::vector<float> page ((size_t) xoa::nfc::kCoeffsPerSource);
    const double rRef = 2.0, rSrc = 1.0, sr = 48000.0;
    xoa::nfc::designSourceSections (rSrc, rRef, sr, page.data());

    xoa::nfc::SourceNfcBank bank;
    const int n = 4096;
    std::vector<float> dry ((size_t) n, 1.0f);           // DC input
    std::vector<std::vector<float>> laneStore ((size_t) xoa::nfc::kNumLanes,
                                               std::vector<float> ((size_t) n, 0.0f));
    float* lanePtrs[xoa::nfc::kNumLanes];
    for (int k = 0; k < xoa::nfc::kNumLanes; ++k) lanePtrs[k] = laneStore[(size_t) k].data();

    bank.processLanes (dry.data(), lanePtrs, n, page.data());

    // lane 0 is the dry stem, bit-identical.
    for (int i = 0; i < n; ++i) CHECK (lanePtrs[0][i] == 1.0f);

    // each lane l settles to its order-l DC gain (r_ref/r_src)^l.
    for (int l = 1; l <= xoa::nfc::kMaxOrder; ++l)
    {
        const double expected = std::pow (rRef / rSrc, (double) l);   // clamp inactive here
        CHECK (std::abs ((double) lanePtrs[l][n - 1] - expected) < expected * 1.0e-3 + 1.0e-4);
    }

    // reset clears state: a fresh impulse produces b0 (first sample) exactly.
    bank.reset();
    std::vector<float> imp ((size_t) 8, 0.0f); imp[0] = 1.0f;
    std::vector<std::vector<float>> l2 ((size_t) xoa::nfc::kNumLanes, std::vector<float> (8, 0.0f));
    float* p2[xoa::nfc::kNumLanes];
    for (int k = 0; k < xoa::nfc::kNumLanes; ++k) p2[k] = l2[(size_t) k].data();
    bank.processLanes (imp.data(), p2, 8, page.data());
    CHECK (p2[0][0] == 1.0f);
    CHECK (std::isfinite (p2[xoa::nfc::kMaxOrder][0]));
}

} // namespace

//==============================================================================
void runXoaNfcTests()
{
    testBesselRootResiduals();
    testDigitalAndAnalogCurves();
    testWorstCaseStability();
    testClampCeilingAndContinuity();
    testBankLanesAndDcSettle();
}
