#pragma once

//==============================================================================
// XOA — project-wide Ambisonics constants.
//
// Conventions (fixed for the whole project):
//   * Order:          10th ("XOA" = 10OA)
//   * Channel order:  ACN (Ambisonic Channel Number)
//   * Normalization:  SN3D (i.e. the AmbiX convention)
// Decoder-side weighting (max-rE, in-phase, dual-band) is a decoder property,
// not part of the encoding convention.
//==============================================================================

namespace xoa
{

constexpr int kAmbisonicOrder = 10;

/** Number of spherical-harmonic channels: (N+1)^2 = 121 for N = 10. */
constexpr int kNumSHChannels = (kAmbisonicOrder + 1) * (kAmbisonicOrder + 1);

static_assert (kNumSHChannels == 121, "10th-order Ambisonics carries 121 SH channels");

} // namespace xoa
