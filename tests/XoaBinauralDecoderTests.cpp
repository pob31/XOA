/*
    XoaBinauralDecoderTests.cpp - WP15 (D51): the SH->binaural decoder solve
    (Source/DSP/AmbiBinauralDecoder).

    Two independent kinds of check:

      1. Golden — rebuild the synthetic HRIR fixture from
         tools/reference/gen_binaural_reference.py as a spatcore HrirDatabase,
         run designShFilters(), and compare the whole 2 x 121 bank against the
         mpmath reference. The generator implements the projection from the
         spec rather than transcribing the C++, so agreement means two
         derivations of the same law meet.

      2. Property — re-encode a plane wave, run it through the bank, and check
         the reconstruction against the fixture's own HRIR for that direction.
         This is the one that would catch a golden that is confidently wrong
         in both languages (e.g. an azimuth flip applied consistently in the
         generator AND the decoder): it anchors on the ENCODER instead.

    On what the reconstruction can and cannot promise: restoring the ITD makes
    the HRIR field direction-dependently DELAYED, and a delay is not
    band-limited in the SH domain. An order-10 projection therefore reproduces
    the arrival TIME very well (measured: within ~1 sample) while smearing the
    peak AMPLITUDE — 5% on an ipsilateral ear, but 20%+ on a delayed
    contralateral one. That is inherent to the sampling decode, not a defect
    here, and it is precisely the error MagLS attacks (stage 7). The
    reconstruction checks below therefore assert timing and interaural
    ordering tightly, amplitude only as gross sanity, and leave exact values
    to the golden. The one exact amplitude anchor that IS available is the W
    channel (below), which the truncation cannot touch.
*/

#include "XoaTestFramework.h"

#include "DSP/AmbiBinauralDecoder.h"
#include "DSP/AmbiSphericalHarmonics.h"
#include "XoaConstants.h"

#include "spatcore/binaural/HrirSet.h"

#include <cmath>
#include <memory>
#include <vector>

namespace bin = xoa::binaural;
namespace shx = xoa::sh;

namespace
{
using Db = spatcore::binaural::HrirDatabase;

// Fixture constants — must match gen_binaural_reference.py.
constexpr double kFixtureSampleRate = 48000.0;
constexpr int    kFixtureHrirLength = 8;
constexpr double kMaxItdSec = 0.0007;

const double kTapShape[kFixtureHrirLength] =
    { 1.0, 0.5, -0.25, 0.125, -0.0625, 0.03125, -0.015625, 0.0078125 };

double fixtureEarGain (double gridAzDeg, double gridElDeg, int ear)
{
    const double az = juce::degreesToRadians (gridAzDeg);
    const double el = juce::degreesToRadians (gridElDeg);
    const double earSign = (ear == 0) ? -1.0 : 1.0;      // ear 0 = LEFT
    const double lateral = std::sin (az) * std::cos (el) * earSign;
    return 0.6 + 0.4 * lateral;
}

/** Signed ITD: positive = source to the RIGHT, so the LEFT ear is later. */
double fixtureItdSec (double gridAzDeg, double gridElDeg)
{
    return kMaxItdSec * std::sin (juce::degreesToRadians (gridAzDeg))
                      * std::cos (juce::degreesToRadians (gridElDeg));
}

/** Build the synthetic fixture as a real HrirDatabase (loader contract: the
    earlier ear holds 0 in relDelaySec, both values >= 0). */
std::shared_ptr<Db> buildFixtureDatabase()
{
    auto db = std::make_shared<Db>();
    db->sampleRate = kFixtureSampleRate;
    db->hrirLength = kFixtureHrirLength;
    db->hrirs.assign ((size_t) Db::kNumAz * Db::kNumEl * 2 * kFixtureHrirLength, 0.0f);
    db->relDelaySec.assign ((size_t) Db::kNumAz * Db::kNumEl * 2, 0.0f);

    for (int azIndex = 0; azIndex < Db::kNumAz; ++azIndex)
    {
        for (int elIndex = 0; elIndex < Db::kNumEl; ++elIndex)
        {
            double azDeg, elDeg;
            bin::gridDirectionDeg (azIndex, elIndex, azDeg, elDeg);

            const double itd = fixtureItdSec (azDeg, elDeg);
            db->relDelaySec[(size_t) db->delayIndex (azIndex, elIndex, 0)] =
                (float) (itd >= 0.0 ? itd : 0.0);
            db->relDelaySec[(size_t) db->delayIndex (azIndex, elIndex, 1)] =
                (float) (itd >= 0.0 ? 0.0 : -itd);

            for (int ear = 0; ear < 2; ++ear)
            {
                const double g = fixtureEarGain (azDeg, elDeg, ear);
                float* dst = db->hrirs.data() + db->hrirIndex (azIndex, elIndex, ear);
                for (int n = 0; n < kFixtureHrirLength; ++n)
                    dst[n] = (float) (g * kTapShape[n]);
            }
        }
    }
    return db;
}

juce::var loadBinauralGolden()
{
    const auto file = juce::File (XOA_TESTS_DATA_DIR).getChildFile ("binaural_reference.json");
    const auto parsed = juce::JSON::parse (file.loadFileAsString());
    CHECK (parsed.isObject());
    return parsed;
}
} // namespace

//==============================================================================
// 1. The golden bank.
static void testBinauralDecoderGolden()
{
    const auto golden = loadBinauralGolden();
    if (! golden.isObject())
        return;

    const auto db = buildFixtureDatabase();
    const auto designed = bin::designShFilters (*db);
    CHECK (designed.isValid());
    if (! designed.isValid())
        return;

    const int goldenLength = (int) golden["firLength"];
    const double tol = (double) golden["tolerance"];
    CHECK (designed.firLength == goldenLength);
    CHECK ((int) golden["numChannels"] == xoa::kNumSHChannels);
    if (designed.firLength != goldenLength)
        return;

    const auto* ears = golden["firs"].getArray();
    CHECK (ears != nullptr && ears->size() == 2);
    if (ears == nullptr || ears->size() != 2)
        return;

    double worst = 0.0;
    int worstEar = -1, worstChannel = -1, worstTap = -1;

    for (int ear = 0; ear < 2; ++ear)
    {
        const auto* channels = (*ears)[ear].getArray();
        CHECK (channels != nullptr && channels->size() == xoa::kNumSHChannels);
        if (channels == nullptr)
            return;

        for (int c = 0; c < xoa::kNumSHChannels; ++c)
        {
            const auto* taps = (*channels)[c].getArray();
            CHECK (taps != nullptr && taps->size() == goldenLength);
            if (taps == nullptr)
                return;

            const float* got = designed.fir (ear, c);
            for (int n = 0; n < goldenLength; ++n)
            {
                const double diff = std::abs ((double) got[n] - (double) (*taps)[n]);
                if (diff > worst)
                {
                    worst = diff;
                    worstEar = ear; worstChannel = c; worstTap = n;
                }
            }
        }
    }

    if (worst > tol)
        std::fprintf (stderr,
                      "  binaural bank worst deviation %.3e at ear %d channel %d tap %d\n",
                      worst, worstEar, worstChannel, worstTap);
    CHECK (worst <= tol);
}

//==============================================================================
// 2. Encoder-anchored reconstruction: encode a plane wave from direction d,
// sum it through the bank, and compare against the fixture's HRIR for d (with
// its ITD restored). Truncation at order 10 leaves a real residual, so this
// checks the shape and the ITD, not bit equality.
static void testBinauralDecoderReconstructsHrir()
{
    const auto db = buildFixtureDatabase();
    const auto designed = bin::designShFilters (*db);
    if (! designed.isValid())
        return;

    struct Case { int azIndex, elIndex; };
    // Front, hard right, hard left, and an elevated oblique — the directions
    // where an azimuth-handedness or ear-order slip is unmissable.
    const Case cases[] = { { 0, 9 }, { 18, 9 }, { 54, 9 }, { 9, 12 } };

    for (const auto& testCase : cases)
    {
        double azDeg, elDeg;
        bin::gridDirectionDeg (testCase.azIndex, testCase.elIndex, azDeg, elDeg);

        double basis[xoa::kNumSHChannels];
        shx::evaluate (bin::gridAzToXoaAzDeg (azDeg), elDeg, xoa::kAmbisonicOrder, basis);

        for (int ear = 0; ear < 2; ++ear)
        {
            std::vector<double> recon ((size_t) designed.firLength, 0.0);
            for (int c = 0; c < xoa::kNumSHChannels; ++c)
            {
                const float* h = designed.fir (ear, c);
                for (int n = 0; n < designed.firLength; ++n)
                    recon[(size_t) n] += basis[c] * (double) h[n];
            }

            // Energy and peak position of the reconstruction vs the fixture.
            double reconEnergy = 0.0;
            int reconPeak = 0;
            double reconPeakAbs = 0.0;
            for (int n = 0; n < designed.firLength; ++n)
            {
                reconEnergy += recon[(size_t) n] * recon[(size_t) n];
                if (std::abs (recon[(size_t) n]) > reconPeakAbs)
                {
                    reconPeakAbs = std::abs (recon[(size_t) n]);
                    reconPeak = n;
                }
            }

            const double gain = fixtureEarGain (azDeg, elDeg, ear);
            // Every filter carries the constant kernel lead (both ears alike),
            // so the expected arrival is the ITD plus that offset.
            const double delaySamples =
                (double) db->relDelaySec[(size_t) db->delayIndex (testCase.azIndex,
                                                                  testCase.elIndex, ear)]
                * kFixtureSampleRate + (double) bin::kBinauralFilterLeadSamples;

            // Arrival time — the property the sampling decode DOES get right,
            // and the one that carries the ITD.
            CHECK (std::abs ((double) reconPeak - delaySamples) <= 1.5);

            // Amplitude: gross sanity only (see the header note). A missing
            // 4pi, a dropped quadrature weight or a lost (2l+1) would move
            // this by orders of magnitude, which is what it is here to catch.
            CHECK (reconPeakAbs > 0.15 * gain);
            CHECK (reconPeakAbs < 3.0 * gain);
            CHECK (reconEnergy > 0.0);
        }
    }
}

//==============================================================================
// 2b. The exact normalization anchor. For SN3D the W basis function is
// identically 1, so the projection collapses to
//     h_ear[0][n] = (1/4pi) * sum_g w_g * hrir_ear(d_g)[n]
// i.e. the solid-angle-weighted MEAN of the delayed HRIRs. No SH truncation
// enters, so this is exact to float precision and pins the (2l+1)/(4pi)
// factor and the quadrature weights on their own — independently of the
// golden and of the encoder.
static void testBinauralDecoderWChannelIsSphericalMean()
{
    const auto db = buildFixtureDatabase();
    const auto designed = bin::designShFilters (*db);
    if (! designed.isValid())
        return;

    // Reference mean, accumulated the same way the decoder does but with no
    // SH basis involved at all.
    for (int ear = 0; ear < 2; ++ear)
    {
        std::vector<double> mean ((size_t) designed.firLength, 0.0);
        double weightSum = 0.0;

        for (int azIndex = 0; azIndex < Db::kNumAz; ++azIndex)
        {
            for (int elIndex = 0; elIndex < Db::kNumEl; ++elIndex)
            {
                double azDeg, elDeg;
                bin::gridDirectionDeg (azIndex, elIndex, azDeg, elDeg);
                const double w = bin::gridCellSolidAngle (elIndex);
                weightSum += w;

                const double gain = fixtureEarGain (azDeg, elDeg, ear);
                // Only the DC sum is compared below, and a fractional-delay
                // kernel has unity DC gain by construction, so the reference
                // can ignore where the delay puts the energy and simply
                // accumulate it.
                for (int n = 0; n < kFixtureHrirLength; ++n)
                    mean[(size_t) n] += gain * kTapShape[n] * w
                                      / (4.0 * juce::MathConstants<double>::pi);
            }
        }

        CHECK (std::abs (weightSum - 4.0 * juce::MathConstants<double>::pi) < 1e-9);

        // Total (DC) gain is unaffected by any delay, fractional or not: a
        // fractional-delay kernel has unity DC gain by construction, so the
        // SUM of the W filter must equal the sum of the reference mean
        // exactly, whatever the smear did to individual taps.
        double designedSum = 0.0, meanSum = 0.0;
        const float* w0 = designed.fir (ear, 0);
        for (int n = 0; n < designed.firLength; ++n)
        {
            designedSum += (double) w0[n];
            meanSum += mean[(size_t) n];
        }
        CHECK (std::abs (designedSum - meanSum) < 1e-5);

        // And the mean HRIR of this fixture is the tap shape times the
        // average gain over the sphere, which is 0.6 (the lateral term
        // integrates to zero) — a closed form the decoder never sees.
        double tapSum = 0.0;
        for (double t : kTapShape)
            tapSum += t;
        CHECK (std::abs (designedSum - 0.6 * tapSum) < 1e-5);
    }
}

//==============================================================================
// 3. Interaural sanity across the horizon: a source on the right must arrive
// at the right ear FIRST and louder, and vice versa. This is the check that
// fails loudly if the ear indices or the azimuth flip are ever swapped.
static void testBinauralDecoderInterauralPolarity()
{
    const auto db = buildFixtureDatabase();
    const auto designed = bin::designShFilters (*db);
    if (! designed.isValid())
        return;

    auto reconstruct = [&] (double gridAzDeg, double gridElDeg, int ear,
                            double& peakAbs, int& peakIndex)
    {
        double basis[xoa::kNumSHChannels];
        shx::evaluate (bin::gridAzToXoaAzDeg (gridAzDeg), gridElDeg,
                       xoa::kAmbisonicOrder, basis);

        peakAbs = 0.0;
        peakIndex = 0;
        for (int n = 0; n < designed.firLength; ++n)
        {
            double v = 0.0;
            for (int c = 0; c < xoa::kNumSHChannels; ++c)
                v += basis[c] * (double) designed.fir (ear, c)[n];
            if (std::abs (v) > peakAbs)
            {
                peakAbs = std::abs (v);
                peakIndex = n;
            }
        }
    };

    // Hard right (grid azimuth +90 = the listener's right).
    {
        double leftPeak, rightPeak;
        int leftIndex, rightIndex;
        reconstruct (90.0, 0.0, 0, leftPeak, leftIndex);
        reconstruct (90.0, 0.0, 1, rightPeak, rightIndex);
        CHECK (rightPeak > leftPeak);        // ipsilateral ear is louder
        CHECK (rightIndex < leftIndex);      // and arrives first
    }

    // Hard left (grid azimuth 270 == -90).
    {
        double leftPeak, rightPeak;
        int leftIndex, rightIndex;
        reconstruct (270.0, 0.0, 0, leftPeak, leftIndex);
        reconstruct (270.0, 0.0, 1, rightPeak, rightIndex);
        CHECK (leftPeak > rightPeak);
        CHECK (leftIndex < rightIndex);
    }

    // Dead ahead: symmetric in both level and time.
    {
        double leftPeak, rightPeak;
        int leftIndex, rightIndex;
        reconstruct (0.0, 0.0, 0, leftPeak, leftIndex);
        reconstruct (0.0, 0.0, 1, rightPeak, rightIndex);
        CHECK (std::abs (leftPeak - rightPeak) < 0.02 * juce::jmax (leftPeak, rightPeak));
        CHECK (leftIndex == rightIndex);
    }
}

//==============================================================================
// 4. Degenerate input must fail cleanly rather than assert or allocate wildly.
static void testBinauralDecoderRejectsEmptySet()
{
    spatcore::binaural::HrirDatabase empty;
    empty.hrirLength = 0;
    const auto designed = bin::designShFilters (empty);
    CHECK (! designed.isValid());
    CHECK (designed.warning.isNotEmpty());
}

//==============================================================================
void runXoaBinauralDecoderTests()
{
    testBinauralDecoderGolden();
    testBinauralDecoderReconstructsHrir();
    testBinauralDecoderWChannelIsSphericalMean();
    testBinauralDecoderInterauralPolarity();
    testBinauralDecoderRejectsEmptySet();
}
