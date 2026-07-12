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

//==============================================================================
// Channel-count ceilings and fresh-project defaults.
//
// kMaxSpeakers is the v1 validation clamp (PRD FR-20: 256 outputs v1,
// architecture clean to 512) — no arrays are sized by it; raising it later is
// a one-line change plus merge-backfill of older project files.
//==============================================================================

constexpr int kMaxInputs       = 64;
constexpr int kMaxSpeakers     = 256;
constexpr int kDefaultInputs   = 8;
constexpr int kDefaultSpeakers = 24;   // the M1 24-ring validation fixture

/** Per-speaker EQ band count (matches spatcore's per-output EQ chain). */
constexpr int kNumEqBands = 6;

/** FR-8 file-playback ceiling: multichannel WAV/CAF/FLAC up to 128 channels
    (order 10 needs 121; the headroom matches the PRD's stated cap). */
constexpr int kMaxFileChannels = 128;

/** Speed of sound (m/s), the single project-wide value. Used by the WP7
    per-speaker distance compensation (delay alignment) and the WP8 near-field
    compensation filters (the s·r/c normalization). */
constexpr double kSpeedOfSound = 343.0;

} // namespace xoa
