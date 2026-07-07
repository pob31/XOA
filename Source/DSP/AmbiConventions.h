#pragma once

#include <juce_core/juce_core.h>

#include <array>
#include <cmath>

#include "XoaConstants.h"
#include "DSP/AmbiSphericalHarmonics.h"

//==============================================================================
// XOA — Ambisonics convention conversions at the ingest boundary (FR-2).
//
// Internally everything is ACN/SN3D (AmbiX). This header converts foreign
// conventions in:
//   * N3D <-> SN3D   per-order diagonal rescaling (SN3D->N3D gain = sqrt(2l+1))
//   * FuMa -> ACN/SN3D   for orders <= 3 only (legacy import; FR-2, PRD §9).
//                        FuMa beyond order 3 is non-standard and rejected.
//
// Export back to FuMa or N3D is a non-goal (FR-2 is import-only).
//==============================================================================

namespace xoa::conv
{

/** Normalization tag. Ints match the WP2 `decoderNormalization` parameter
    (0 = amplitude/SN3D-ish, 1 = energy/N3D-ish) only loosely — this enum is
    about the SH basis normalization, cross-check at the WP5 call site. */
enum class Normalization { sn3d = 0, n3d = 1 };

//==============================================================================
// N3D <-> SN3D
//==============================================================================

/** Per-order gain to convert an SN3D coefficient to N3D: sqrt(2l+1). */
inline double sn3dToN3d (int l) noexcept { return std::sqrt (2.0 * l + 1.0); }

/** Per-order gain to convert an N3D coefficient to SN3D: 1/sqrt(2l+1). */
inline double n3dToSn3d (int l) noexcept { return 1.0 / std::sqrt (2.0 * l + 1.0); }

/** Fill perChannel (numChannels(order) doubles) with the SN3D->N3D gain of
    each ACN channel's order. */
inline void sn3dToN3dGains (int order, double* perChannel) noexcept
{
    for (int c = 0; c < sh::numChannels (order); ++c)
        perChannel[c] = sn3dToN3d (sh::acnToOrder (c));
}

/** Fill perChannel with the N3D->SN3D gain of each ACN channel's order. */
inline void n3dToSn3dGains (int order, double* perChannel) noexcept
{
    for (int c = 0; c < sh::numChannels (order); ++c)
        perChannel[c] = n3dToSn3d (sh::acnToOrder (c));
}

/** out[c] = in[c] * gains[c] for all numChannels(order) channels. in == out is
    allowed (in-place). */
inline void applyGains (const double* gains, int order, const double* in, double* out) noexcept
{
    for (int c = 0; c < sh::numChannels (order); ++c)
        out[c] = in[c] * gains[c];
}

//==============================================================================
// FuMa -> ACN/SN3D (orders <= 3)
//==============================================================================

constexpr int kMaxFumaOrder    = 3;
constexpr int kNumFumaChannels = 16;   // (3+1)^2

/** One FuMa channel's destination: its ACN slot and the gain that takes the
    FuMa (maxN) coefficient to SN3D. */
struct FumaChannelMap
{
    int    acn;
    double fumaToSn3dGain;
};

/** The FuMa->AmbiX table, in FuMa channel order (WXYZ RSTUV KLMNOPQ).

    Direction rule (this is what arbitrates the reciprocal-table trap): FuMa is
    maxN — every FuMa basis function peaks at magnitude 1 (except W = pressure
    scaled by 1/sqrt2) — so the FuMa->SN3D gain equals the peak magnitude of the
    corresponding SN3D harmonic over the sphere, max_sphere |Y^SN3D_{l,m}| (and
    an extra sqrt2 for W). Circulated tables that list the reciprocals
    (2/sqrt3, sqrt(45/32), ...) are the SN3D->FuMa direction — not this one.

    Traps: FuMa first order is X, Y, Z = (1,+1),(1,-1),(1,0) with m=0 (Z) LAST,
    unlike l=2,3 where m=0 comes first.

    Cite-checked against two independent published sources (recorded here as the
    WP3 hard gate): M. Kronlachner, "Ambisonics plug-in suite" / master's thesis
    (2014), ambix_converter FuMa tables; and J. Daniel, PhD thesis (2000), §3.3
    FuMa/maxN normalization tables (cross-referenced with D. Malham's FMH
    definitions). The generator tools/reference/gen_fuma_reference.py
    additionally derives every gain numerically from the maxN rule, so a
    transcription slip cannot survive the S11 test. */
inline const std::array<FumaChannelMap, kNumFumaChannels>& fumaToAmbixTable() noexcept
{
    static const std::array<FumaChannelMap, kNumFumaChannels> table = {{
        // FuMa W  (0, 0) -> ACN 0,  gain sqrt2
        { 0,  std::sqrt (2.0) },
        // FuMa X  (1,+1) -> ACN 3,  gain 1
        { 3,  1.0 },
        // FuMa Y  (1,-1) -> ACN 1,  gain 1
        { 1,  1.0 },
        // FuMa Z  (1, 0) -> ACN 2,  gain 1
        { 2,  1.0 },
        // FuMa R  (2, 0) -> ACN 6,  gain 1
        { 6,  1.0 },
        // FuMa S  (2,+1) -> ACN 7,  gain sqrt3/2
        { 7,  std::sqrt (3.0) / 2.0 },
        // FuMa T  (2,-1) -> ACN 5,  gain sqrt3/2
        { 5,  std::sqrt (3.0) / 2.0 },
        // FuMa U  (2,+2) -> ACN 8,  gain sqrt3/2
        { 8,  std::sqrt (3.0) / 2.0 },
        // FuMa V  (2,-2) -> ACN 4,  gain sqrt3/2
        { 4,  std::sqrt (3.0) / 2.0 },
        // FuMa K  (3, 0) -> ACN 12, gain 1
        { 12, 1.0 },
        // FuMa L  (3,+1) -> ACN 13, gain sqrt(32/45)
        { 13, std::sqrt (32.0 / 45.0) },
        // FuMa M  (3,-1) -> ACN 11, gain sqrt(32/45)
        { 11, std::sqrt (32.0 / 45.0) },
        // FuMa N  (3,+2) -> ACN 14, gain sqrt5/3
        { 14, std::sqrt (5.0) / 3.0 },
        // FuMa O  (3,-2) -> ACN 10, gain sqrt5/3
        { 10, std::sqrt (5.0) / 3.0 },
        // FuMa P  (3,+3) -> ACN 15, gain sqrt(5/8)
        { 15, std::sqrt (5.0 / 8.0) },
        // FuMa Q  (3,-3) -> ACN 9,  gain sqrt(5/8)
        { 9,  std::sqrt (5.0 / 8.0) },
    }};
    return table;
}

/** Is this FuMa order importable (0..3)? */
constexpr bool isFumaOrderSupported (int order) noexcept
{
    return order >= 0 && order <= kMaxFumaOrder;
}

/** Map a FuMa channel count to its (full-3D) order, or -1 if it is not a valid
    full-sphere FuMa set. Rejects 2-D / mixed-order FuMa layouts. */
constexpr int fumaOrderForChannelCount (int numFumaChannels) noexcept
{
    switch (numFumaChannels)
    {
        case 1:  return 0;
        case 4:  return 1;
        case 9:  return 2;
        case 16: return 3;
        default: return -1;
    }
}

/** Convert a FuMa coefficient frame (WXYZ RSTUV KLMNOPQ, maxN, W scaled by
    1/sqrt2) to ACN/SN3D. `out` must hold numChannels(fumaOrder) doubles; bus
    zero-padding up to order 10 is xoa::weights::adaptOrder's job (kept separate
    for composability).

    Returns false and zero-fills `out` when fumaOrder > 3 (PRD §9 explicit
    rejection). The math stays noexcept — the user-facing "FuMa beyond order 3
    is unsupported" message belongs at the WP6 file-import call site. */
inline bool fumaToAmbix (const double* fuma, int fumaOrder, double* out) noexcept
{
    if (! isFumaOrderSupported (fumaOrder))
    {
        for (int c = 0; c < sh::numChannels (juce::jmax (0, fumaOrder)); ++c)
            out[c] = 0.0;
        return false;
    }

    const auto& table = fumaToAmbixTable();
    const int n = sh::numChannels (fumaOrder);
    for (int f = 0; f < n; ++f)
        out[table[static_cast<size_t> (f)].acn] = fuma[f] * table[static_cast<size_t> (f)].fumaToSn3dGain;
    return true;
}

} // namespace xoa::conv
