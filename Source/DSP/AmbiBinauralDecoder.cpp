/*
  ==============================================================================

    XOA — tenth-order Ambisonics spatial audio processor.
    AmbiBinauralDecoder implementation (WP15, D51).

    Sampling mode, in one line:

        h_ear[c][n] = (2 l_c + 1)/(4π) · Σ_g w_g · Y_c(d_g) · hrir_ear(d_g)[n]

    i.e. project the measured HRIR field onto the real SN3D/ACN basis by
    quadrature over the bake grid. The (2l+1)/(4π) factor inverts SN3D's
    orthogonality relation ∫ Y_c Y_c' dΩ = 4π/(2l+1) · δ, so re-encoding a
    plane wave and summing through the bank reproduces that direction's HRIR.
    This is the same law as XOA's SAD speaker decoder with the virtual
    speakers taken to the whole grid (w_g = 4π/L there), which is why it is
    also called the virtual-speaker decode.

    Before projecting, the ITD is put BACK. SofaLoader hands over time-aligned
    HRIRs with the per-ear onset in `relDelaySec` — alignment is what makes
    render-time interpolation between neighbouring directions safe, but a
    binaural decoder that discarded it would have no interaural time
    difference at all. The delays are fractional, so they are re-applied with
    a windowed-sinc fractional delay (offline: cost is irrelevant, quality is
    not).

  ==============================================================================
*/

#include "DSP/AmbiBinauralDecoder.h"

#include "DSP/AmbiSphericalHarmonics.h"

#include <cmath>

namespace xoa::binaural
{

namespace
{

using Db = spatcore::binaural::HrirDatabase;

/** Half-width of the fractional-delay kernel. 16 each side is well past the
    point where the windowed sinc stops mattering for a 256-tap HRIR. */
constexpr int kSincHalf = 16;

/** Constant lead applied to EVERY filter, both ears alike.

    A windowed-sinc fractional delay is centred on its target, so it rings
    kSincHalf samples BACKWARD as well as forward. Without a lead, any delay
    smaller than kSincHalf would have that pre-ring clipped at tap 0 — which
    silently loses energy and, worse, does so asymmetrically (the near ear is
    truncated more than the far one, corrupting the very interaural level
    difference the bank exists to carry). Offsetting every filter by the
    kernel's half-width makes the clip impossible.

    The cost is a fixed latency common to both ears: 16 samples, 0.33 ms at
    48 kHz. Being common, it shifts nothing interaural and is inaudible on a
    monitor path. */
constexpr int kFilterLead = kBinauralFilterLeadSamples;
static_assert (kFilterLead == kSincHalf, "the lead must cover the kernel's backward reach");

/** Windowed-sinc fractional delay: add `src` (length n) into `dst` delayed by
    `delaySamples` (>= 0, fractional). `dst` must have room for
    n + ceil(delay) + kSincHalf taps. */
void addDelayed (const float* src, int n, double delaySamples,
                 double* dst, int dstLength)
{
    const int intDelay = (int) std::floor (delaySamples);
    const double frac = delaySamples - (double) intDelay;

    // Integer delay: a plain shift, no filtering (and no sinc ringing).
    if (frac < 1.0e-9)
    {
        for (int i = 0; i < n; ++i)
        {
            const int j = i + intDelay;
            if (j >= 0 && j < dstLength)
                dst[j] += (double) src[i];
        }
        return;
    }

    // Blackman-windowed sinc centred on the fractional offset.
    double kernel[2 * kSincHalf + 1];
    double sum = 0.0;
    for (int k = -kSincHalf; k <= kSincHalf; ++k)
    {
        const double x = (double) k - frac;
        double s;
        if (std::abs (x) < 1.0e-12)
            s = 1.0;
        else
            s = std::sin (juce::MathConstants<double>::pi * x)
                / (juce::MathConstants<double>::pi * x);

        // Blackman window over the kernel span, centred on the same offset.
        const double t = ((double) (k + kSincHalf) - frac)
                       / (double) (2 * kSincHalf);
        const double w = 0.42 - 0.5 * std::cos (juce::MathConstants<double>::twoPi * t)
                       + 0.08 * std::cos (2.0 * juce::MathConstants<double>::twoPi * t);
        kernel[k + kSincHalf] = s * w;
        sum += s * w;
    }
    // Unity DC gain: the window would otherwise scale the whole response.
    if (std::abs (sum) > 1.0e-12)
        for (auto& k : kernel)
            k /= sum;

    for (int i = 0; i < n; ++i)
    {
        const double v = (double) src[i];
        if (v == 0.0)
            continue;
        const int base = i + intDelay;
        for (int k = -kSincHalf; k <= kSincHalf; ++k)
        {
            const int j = base + k;
            if (j >= 0 && j < dstLength)
                dst[j] += v * kernel[k + kSincHalf];
        }
    }
}

} // namespace

//==============================================================================
BinauralDesignResult designShFilters (const Db& db, const BinauralDesignOptions& options)
{
    BinauralDesignResult result;
    result.sampleRate = db.sampleRate;

    if (db.hrirLength <= 0 || db.sampleRate <= 0.0
        || db.hrirs.size() < (size_t) db.hrirIndex (Db::kNumAz - 1, Db::kNumEl - 1, 1)
                             + (size_t) db.hrirLength)
    {
        result.warning = "HRIR set is empty or malformed";
        return result;
    }

    if (options.mode == DecoderMode::magls)
    {
        // Stage 7. Falling back keeps a wrong-mode project audible rather than
        // silent, and the warning surfaces in the Monitoring tab.
        result.warning = "MagLS is not implemented yet — using the sampling decode";
    }

    // Filter length: the aligned HRIR, plus the largest ITD we have to put
    // back, plus the fractional-delay kernel's reach.
    double maxDelaySec = 0.0;
    for (float d : db.relDelaySec)
        maxDelaySec = juce::jmax (maxDelaySec, (double) d);

    int usableTaps = db.hrirLength;
    if (usableTaps > kMaxBinauralHrirTaps)
    {
        usableTaps = kMaxBinauralHrirTaps;
        result.warning = "HRIR set is " + juce::String (db.hrirLength)
                       + " taps — using the first " + juce::String (kMaxBinauralHrirTaps)
                       + " (a set this long is usually a room response, which a"
                         " head-tracked monitor decode cannot use)";
    }

    const int maxDelaySamples = (int) std::ceil (maxDelaySec * db.sampleRate);
    const int firLength = juce::jmin (usableTaps + maxDelaySamples + 2 * kSincHalf + 1,
                                      kMaxBinauralFirLength);

    result.firLength = firLength;
    result.firs.assign ((size_t) firLength * 2 * (size_t) xoa::kNumSHChannels, 0.0f);

    // Accumulate in double: 1368 grid points per channel, and the pole rows
    // add many near-identical contributions.
    std::vector<double> accum ((size_t) firLength * 2 * (size_t) xoa::kNumSHChannels, 0.0);
    std::vector<double> delayed ((size_t) firLength, 0.0);

    double shBasis[xoa::kNumSHChannels];
    double channelGain[xoa::kNumSHChannels];
    for (int c = 0; c < xoa::kNumSHChannels; ++c)
    {
        const int l = (int) std::sqrt ((double) c);   // ACN: l = floor(sqrt(acn))
        channelGain[c] = (2.0 * l + 1.0) / (4.0 * juce::MathConstants<double>::pi);
    }

    double weightSum = 0.0;

    for (int azIndex = 0; azIndex < Db::kNumAz; ++azIndex)
    {
        for (int elIndex = 0; elIndex < Db::kNumEl; ++elIndex)
        {
            double gridAzDeg, gridElDeg;
            gridDirectionDeg (azIndex, elIndex, gridAzDeg, gridElDeg);

            // Head frame -> soundfield frame (the azimuth handedness flip).
            sh::evaluate (gridAzToXoaAzDeg (gridAzDeg), gridElDeg,
                          xoa::kAmbisonicOrder, shBasis);

            const double w = gridCellSolidAngle (elIndex);
            weightSum += w;

            for (int ear = 0; ear < 2; ++ear)
            {
                const float* hrir = db.hrirs.data() + db.hrirIndex (azIndex, elIndex, ear);
                const double delaySamples =
                    (double) db.relDelaySec[(size_t) db.delayIndex (azIndex, elIndex, ear)]
                    * db.sampleRate;

                std::fill (delayed.begin(), delayed.end(), 0.0);
                addDelayed (hrir, usableTaps, delaySamples + (double) kFilterLead,
                            delayed.data(), firLength);

                for (int c = 0; c < xoa::kNumSHChannels; ++c)
                {
                    const double scale = w * channelGain[c] * shBasis[c];
                    if (scale == 0.0)
                        continue;

                    double* dst = accum.data()
                                + ((size_t) ear * (size_t) xoa::kNumSHChannels + (size_t) c)
                                  * (size_t) firLength;
                    for (int n = 0; n < firLength; ++n)
                        dst[n] += scale * delayed[(size_t) n];
                }
            }
        }
    }

    // The cell weights partition the sphere; a drift here means the grid
    // constants moved under us.
    jassert (std::abs (weightSum - 4.0 * juce::MathConstants<double>::pi) < 1.0e-9);

    for (size_t i = 0; i < accum.size(); ++i)
        result.firs[i] = (float) accum[i];

    return result;
}

} // namespace xoa::binaural
