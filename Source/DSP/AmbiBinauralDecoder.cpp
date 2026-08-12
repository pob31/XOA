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

#include <juce_dsp/juce_dsp.h>

#include <cmath>
#include <vector>

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

/** The SH projection itself:

        h_ear[c][n] = (2l+1)/(4pi) * sum_g w_g * Y_c(d_g) * hrir_ear(d_g)[n]

    With `restoreItd` the per-ear onset from `relDelaySec` is put back before
    projecting (true phase); without it the time-ALIGNED HRIRs are projected
    as they come, which is what the high band wants. Both carry the same
    constant kFilterLead, so the two results stay time-aligned with each other
    and can be blended bin by bin.

    Returns [ear][channel][tap] in double — the caller cooks to float. */
std::vector<double> projectGrid (const Db& db, int usableTaps, int firLength, bool restoreItd)
{
    std::vector<double> accum ((size_t) firLength * 2 * (size_t) xoa::kNumSHChannels, 0.0);
    std::vector<double> shaped ((size_t) firLength, 0.0);

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
                const double delaySamples = restoreItd
                    ? (double) db.relDelaySec[(size_t) db.delayIndex (azIndex, elIndex, ear)]
                          * db.sampleRate
                    : 0.0;

                std::fill (shaped.begin(), shaped.end(), 0.0);
                addDelayed (hrir, usableTaps, delaySamples + (double) kFilterLead,
                            shaped.data(), firLength);

                for (int c = 0; c < xoa::kNumSHChannels; ++c)
                {
                    const double scale = w * channelGain[c] * shBasis[c];
                    if (scale == 0.0)
                        continue;

                    double* dst = accum.data()
                                + ((size_t) ear * (size_t) xoa::kNumSHChannels + (size_t) c)
                                  * (size_t) firLength;
                    for (int n = 0; n < firLength; ++n)
                        dst[n] += scale * shaped[(size_t) n];
                }
            }
        }
    }

    // The cell weights partition the sphere; a drift here means the grid
    // constants moved under us.
    jassert (std::abs (weightSum - 4.0 * juce::MathConstants<double>::pi) < 1.0e-9);
    juce::ignoreUnused (weightSum);

    return accum;
}

/** Complementary per-bin blend of two banks: `low` below the crossover,
    `high` above, with a one-octave raised-cosine transition.

    The two gains are REAL and sum to exactly 1 at every bin, so the split
    itself adds no magnitude ripple — the only artifact is the phase handover
    between the ITD-carrying and time-aligned versions, confined to the
    transition band. This is the frequency-domain sibling of the loudspeaker
    path's LR4 dual-band decode. */
void blendAtCrossover (const std::vector<double>& low, const std::vector<double>& high,
                       int firLength, int sourceTaps, int leadSamples,
                       double crossoverHz, double sampleRate,
                       std::vector<float>& out)
{
    int order = 1;
    while ((1 << order) < 2 * firLength)
        ++order;
    const int fftSize = 1 << order;
    const int numBins = fftSize / 2 + 1;

    // One octave centred on the crossover.
    const double fLow = crossoverHz / juce::MathConstants<double>::sqrt2;
    const double fHigh = crossoverHz * juce::MathConstants<double>::sqrt2;

    std::vector<float> lowGain ((size_t) numBins, 0.0f);
    for (int b = 0; b < numBins; ++b)
    {
        const double f = (double) b * sampleRate / (double) fftSize;
        double g;
        if (f <= fLow)
            g = 1.0;
        else if (f >= fHigh)
            g = 0.0;
        else
        {
            const double t = std::log (f / fLow) / std::log (fHigh / fLow);
            g = 0.5 * (1.0 + std::cos (juce::MathConstants<double>::pi * t));
        }
        lowGain[(size_t) b] = (float) g;
    }

    juce::dsp::FFT fft (order);
    std::vector<float> lowWork ((size_t) 2 * fftSize, 0.0f);
    std::vector<float> highWork ((size_t) 2 * fftSize, 0.0f);

    for (int ear = 0; ear < 2; ++ear)
    {
        for (int c = 0; c < xoa::kNumSHChannels; ++c)
        {
            const size_t base = ((size_t) ear * (size_t) xoa::kNumSHChannels + (size_t) c)
                              * (size_t) firLength;

            // Both inputs are placed at `leadSamples`, giving the crossover's
            // BACKWARD smear somewhere to go. Without the lead it would wrap
            // around the transform and be cut off by the truncation below —
            // which showed up as a 2% DC deficit, i.e. the blend quietly
            // eating broadband gain.
            std::fill (lowWork.begin(), lowWork.end(), 0.0f);
            std::fill (highWork.begin(), highWork.end(), 0.0f);
            for (int n = 0; n < sourceTaps; ++n)
            {
                lowWork[(size_t) (n + leadSamples)]  = (float) low[base + (size_t) n];
                highWork[(size_t) (n + leadSamples)] = (float) high[base + (size_t) n];
            }

            fft.performRealOnlyForwardTransform (lowWork.data());
            fft.performRealOnlyForwardTransform (highWork.data());

            // Blend the whole interleaved buffer: JUCE mirrors the spectrum,
            // so applying a symmetric real gain bin by bin keeps the result
            // conjugate-symmetric and the inverse transform real.
            for (int b = 0; b < fftSize; ++b)
            {
                const int bin = (b <= fftSize / 2) ? b : fftSize - b;
                const float g = lowGain[(size_t) juce::jmin (bin, numBins - 1)];
                lowWork[(size_t) (2 * b)]     = g * lowWork[(size_t) (2 * b)]
                                              + (1.0f - g) * highWork[(size_t) (2 * b)];
                lowWork[(size_t) (2 * b + 1)] = g * lowWork[(size_t) (2 * b + 1)]
                                              + (1.0f - g) * highWork[(size_t) (2 * b + 1)];
            }

            fft.performRealOnlyInverseTransform (lowWork.data());

            // Back to firLength taps. firLength already reserves room for the
            // crossover's own impulse response, so the tail here is small; a
            // short raised-cosine fade keeps the cut from ringing.
            constexpr int kFadeTaps = 8;
            for (int n = 0; n < firLength; ++n)
            {
                float w = 1.0f;
                if (n >= firLength - kFadeTaps)
                {
                    const float t = (float) (n - (firLength - kFadeTaps)) / (float) kFadeTaps;
                    w = 0.5f * (1.0f + std::cos (juce::MathConstants<float>::pi * t));
                }
                out[base + (size_t) n] = w * lowWork[(size_t) n];
            }
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

    // The crossover blend costs taps of its own: a transition one octave wide
    // around fc has an impulse response roughly fs / (fHigh - fLow) long, and
    // truncating it would ring and eat the DC gain. Reserve twice that.
    int crossoverHeadroom = 0;
    if (options.mode == DecoderMode::alignedHf)
    {
        const double transitionHz = options.crossoverHz
                                  * (juce::MathConstants<double>::sqrt2
                                     - 1.0 / juce::MathConstants<double>::sqrt2);
        if (transitionHz > 1.0)
            crossoverHeadroom = 2 * (int) std::ceil (db.sampleRate / transitionHz);
    }

    const int firLength = juce::jmin (usableTaps + maxDelaySamples + 2 * kSincHalf + 1
                                          + crossoverHeadroom,
                                      kMaxBinauralFirLength);

    result.firLength = firLength;
    result.firs.assign ((size_t) firLength * 2 * (size_t) xoa::kNumSHChannels, 0.0f);

    // The ITD-carrying projection is what `sampling` produces on its own and
    // what `alignedHf` uses below the crossover.
    const auto withItd = projectGrid (db, usableTaps, firLength, true);

    if (options.mode == DecoderMode::sampling)
    {
        for (size_t i = 0; i < withItd.size(); ++i)
            result.firs[i] = (float) withItd[i];
        return result;
    }

    // alignedHf: a second projection of the SAME grid with the ITD left OUT
    // (spatcore hands the HRIRs over time-aligned, so this is the cheap one),
    // then a per-bin complementary blend — true phase low, aligned high.
    const auto aligned = projectGrid (db, usableTaps, firLength, false);

    // The projections carry their content in the first (firLength -
    // crossoverHeadroom) taps; shifting by half the headroom leaves the
    // crossover room to smear in both directions inside firLength.
    const int lead = crossoverHeadroom / 2;
    const int sourceTaps = juce::jmax (0, firLength - lead);
    blendAtCrossover (withItd, aligned, firLength, sourceTaps, lead,
                      options.crossoverHz, db.sampleRate, result.firs);
    return result;
}

} // namespace xoa::binaural
