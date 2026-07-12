#pragma once

#include <algorithm>
#include <cmath>

#include "XoaConstants.h"
#include "DSP/AmbiNfcTables.h"

//==============================================================================
// XOA - near-field compensation (NFC) filters (WP8, FR-6).
//
// Per Ambisonic order l, a source at radius r_src reproduced on a rig of mean
// radius r_ref is shaped by
//
//   H_l(s) = prod_i (s - q_i c/r_src) / (s - q_i c/r_ref)
//
// where q_i are the reverse-Bessel roots in AmbiNfcTables.h. Zeros move with the
// source; POLES DEPEND ONLY ON r_ref, so stability is fixed by the rig and never
// by the (moving) source - a fast-moving source only slides the zeros. DC gain
// is (r_ref/r_src)^l (proximity bass-boost), HF gain -> 1. The full derivation
// and the sign convention live in tools/reference/gen_bessel_roots.py.
//
// Two halves, matching the app's control/RT split:
//   * designSourceSections()  - control side (message thread), double math:
//     radii + sample rate -> 150 float section coefficients for one source,
//     bilinear-transformed (no prewarp) with the numerical-safety clamps.
//   * SourceNfcBank           - RT side: runs the per-order cascades on a mono
//     stem to produce the 11 order-lanes (lane 0 = dry). Filter state persists
//     across coefficient updates; reset() only on an enable rising-edge.
//
// The coefficient page is packed (150 floats/source): kSectionOffset[l] locates
// order l's sections, kSectionCount[l] gives how many. Design math is double;
// coefficients and audio are float (the RT-boundary rule).
//==============================================================================

namespace xoa::nfc
{

constexpr int kMaxOrder         = tables::kMaxOrder;          // 10
constexpr int kNumLanes         = kMaxOrder + 1;              // lane 0 = dry
constexpr int kSectionsPerSource = tables::kTotalSections;    // 30
constexpr int kCoeffsPerSection = 5;                          // b0 b1 b2 a1 a2
constexpr int kCoeffsPerSource  = kSectionsPerSource * kCoeffsPerSection;   // 150

// Numerical-safety clamps (PRD sec.9: NFC is spicy near Nyquist / small radii).
constexpr double kMinSourceRadius = 0.25;   // m - geometry floor on r_src
constexpr double kMaxBoostDb      = 20.0;   // per-order DC-boost ceiling (r_ref/r_src)^l

//==============================================================================
/** Direct-Form-I section with externally supplied coefficients. b2 == a2 == 0
    realizes a 1st-order section. Same difference equation as spatcore's
    OutputEQBiquadFilter (y = b0 x + b1 x1 + b2 x2 - a1 y1 - a2 y2), but the
    coefficients are injected (arbitrary bilinear-transformed poles/zeros)
    rather than computed from a cookbook shape. */
struct NfcSection
{
    float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f, a1 = 0.0f, a2 = 0.0f;
    float x1 = 0.0f, x2 = 0.0f, y1 = 0.0f, y2 = 0.0f;

    void reset() noexcept { x1 = x2 = y1 = y2 = 0.0f; }

    void setCoeffs (const float* c) noexcept   // 5 floats; does NOT touch state
    {
        b0 = c[0]; b1 = c[1]; b2 = c[2]; a1 = c[3]; a2 = c[4];
    }

    float process (float in) noexcept
    {
        const float out = b0 * in + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
        x2 = x1; x1 = in;
        y2 = y1; y1 = out;
        return out;
    }
};

//==============================================================================
/** Clamp r_src for order l so the DC boost (r_ref/r_src)^l stays within
    kMaxBoostDb, and never below the geometry floor. Higher orders clamp
    earlier (a fixed total-dB ceiling means a tighter radius floor as l grows). */
inline double clampedSourceRadius (double rSrc, double rRef, int order) noexcept
{
    const double boostFloor = rRef * std::pow (10.0, -kMaxBoostDb / (20.0 * order));
    return std::max ({ rSrc, kMinSourceRadius, boostFloor });
}

//==============================================================================
/** Design one source's 150 section coefficients. Control side, double math.

    @param rSrc         source radius (m); clamped per order for safety.
    @param rRef         rig mean radius (m); sets the (stable) poles.
    @param sampleRate   active device rate (FR-4: filters designed at runtime).
    @param out          kCoeffsPerSource (150) floats, packed by kSectionOffset.

    Bilinear transform s -> K(z-1)/(z+1), K = 2 Fs, NO prewarp: DC maps exactly
    (so the DC-boost law is preserved bit-for-bit) and the response tends to
    unity at Nyquist. There is no single critical frequency to preserve here -
    the reference-curve test quantifies the midband warp against Daniel's
    analog curves. */
inline void designSourceSections (double rSrc, double rRef, double sampleRate,
                                  float* out) noexcept
{
    const double c = xoa::kSpeedOfSound;
    const double K = 2.0 * sampleRate;

    for (int l = 1; l <= kMaxOrder; ++l)
    {
        const double rEff = clampedSourceRadius (rSrc, rRef, l);
        const int base  = tables::kSectionOffset[l];
        const int count = tables::kSectionCount[l];

        for (int j = 0; j < count; ++j)
        {
            const tables::Root q = tables::kRoots[base + j];
            float* coeff = out + (size_t) (base + j) * kCoeffsPerSection;

            // zero z = q c/rEff, pole p = q c/rRef.
            const double zr = q.re * c / rEff, zi = q.im * c / rEff;
            const double pr = q.re * c / rRef, pi = q.im * c / rRef;

            if (q.im == 0.0)
            {
                // 1st-order (s - zr)/(s - pr), both real (zr, pr < 0).
                const double d = K - pr;
                coeff[0] = (float) ((K - zr) / d);
                coeff[1] = (float) (-(K + zr) / d);
                coeff[2] = 0.0f;
                coeff[3] = (float) (-(K + pr) / d);
                coeff[4] = 0.0f;
            }
            else
            {
                // 2nd-order from conjugate pairs: s^2 - 2*Re*s + |.|^2.
                const double az = zr,          mz = zr * zr + zi * zi;
                const double ap = pr,          mp = pr * pr + pi * pi;
                const double nb0 = K * K - 2.0 * az * K + mz;
                const double nb1 = 2.0 * (mz - K * K);
                const double nb2 = K * K + 2.0 * az * K + mz;
                const double da0 = K * K - 2.0 * ap * K + mp;
                const double da1 = 2.0 * (mp - K * K);
                const double da2 = K * K + 2.0 * ap * K + mp;
                coeff[0] = (float) (nb0 / da0);
                coeff[1] = (float) (nb1 / da0);
                coeff[2] = (float) (nb2 / da0);
                coeff[3] = (float) (da1 / da0);
                coeff[4] = (float) (da2 / da0);
            }
        }
    }
}

//==============================================================================
/** RT-side per-source filter bank. Holds the section state for all 30 sections
    of one source. processLanes() runs each order's cascade on the dry stem to
    produce the order-lanes: lane 0 = dry, lane l = stem through order l's
    ceil(l/2)-section cascade. Coefficients are (re)applied each block from the
    control-side page; state persists (reset() only on NFC enable). */
class SourceNfcBank
{
public:
    void reset() noexcept
    {
        for (auto& s : sections) s.reset();
    }

    /** @param dry     mono stem, n samples.
        @param lanes   kNumLanes channel pointers; lanes[0..kMaxOrder] each n
                       samples. Filled in place.
        @param n       sample count.
        @param coeffs  this source's kCoeffsPerSource page. */
    void processLanes (const float* dry, float* const* lanes, int n,
                       const float* coeffs) noexcept
    {
        for (int i = 0; i < n; ++i)
            lanes[0][i] = dry[i];

        for (int l = 1; l <= kMaxOrder; ++l)
        {
            const int base  = tables::kSectionOffset[l];
            const int count = tables::kSectionCount[l];

            for (int j = 0; j < count; ++j)
                sections[(size_t) (base + j)].setCoeffs (coeffs + (size_t) (base + j) * kCoeffsPerSection);

            float* lane = lanes[l];
            for (int i = 0; i < n; ++i)
            {
                float v = dry[i];
                for (int j = 0; j < count; ++j)
                    v = sections[(size_t) (base + j)].process (v);
                lane[i] = v;
            }
        }
    }

private:
    NfcSection sections[kSectionsPerSource];
};

} // namespace xoa::nfc
