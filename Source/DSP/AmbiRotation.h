#pragma once

#include <juce_core/juce_core.h>

#include <cmath>
#include <cstring>
#include <type_traits>

#include "XoaConstants.h"
#include "Helpers/XoaCoordinates.h"
#include "DSP/AmbiSphericalHarmonics.h"

//==============================================================================
// XOA — SO(3) soundfield rotation: block-diagonal per-order rotation matrices
// for the real SH basis (ACN/SN3D), built by the Ivanic-Ruedenberg recursion.
//
// Citation (implemented from the CORRECTED tables only): Ivanic, J. &
// Ruedenberg, K., "Rotation Matrices for Real Spherical Harmonics. Direct
// Determination by Recursion", J. Phys. Chem. 100(15), 6342-6347 (1996), AS
// CORRECTED BY THE ERRATUM J. Phys. Chem. A 102(45), 9099-9100 (1998). The
// uncorrected 1996 V/W tables are the classic implementation trap.
//
// Transformation convention (load-bearing): per degree l, R^l is the
// (2l+1)x(2l+1) orthogonal matrix with
//
//     Y_{l,m}(R·d) = sum_{m'} R^l_{m,m'} · Y_{l,m'}(d)      (ACTIVE rotation)
//
// Consequence: a source encoded at direction d has coefficients a_c = Y_c(d);
// the field rotated by R carries that source at R·d, i.e. coefficients
// Y_c(R·d) = R^l · a. Hence  apply(build(R), encode(d)) == encode(R·d)  —
// the decisive property test is literally this definition.
//
// SN3D == N3D blocks: within a degree l the N3D basis is sqrt(2l+1) times the
// SN3D basis — one scalar for all 2l+1 channels — and conjugating a block by
// a scalar multiple of the identity is the identity map. The same R^l rotates
// SN3D and N3D coefficient vectors (and is orthogonal).
//
// ACN l=1 note: channels (1,-1),(1,0),(1,+1) = (Y,Z,X) evaluate to the
// direction components (y,z,x); the l=1 seed below is the direction-cosine
// matrix conjugated by that permutation.
//
// Reflections: buildFromCartesian requires a PROPER rotation (det = +1,
// debug-asserted). Mirrors go through xoa::mirror — mirror∘rotation is not a
// rotation and is not representable here.
//
// RT notes: build is a control-side operation (~30k flops, all 11 blocks).
// apply costs sum_l (2l+1)^2 = 1771 MACs/sample at order 10 (PRD §5); this
// double-precision apply is the design/reference path — the RT float path is
// WP6's, which also owns the click-free policy (lerp old→new coefficients
// over ≤ one block; a lerped matrix is momentarily not exactly SO(3),
// inaudible over one block, single apply).
//==============================================================================

namespace xoa::rot
{

//==============================================================================
// Storage / indexing (constexpr, exact)
//==============================================================================

/** Entries in the l-th block: (2l+1)^2. */
constexpr int blockSize (int l) noexcept { return (2 * l + 1) * (2 * l + 1); }

/** Flat offset of block l: sum_{k<l} (2k+1)^2 = l(2l-1)(2l+1)/3 (exact).
    Anchors, l = 0..11: 0, 1, 10, 35, 84, 165, 286, 455, 680, 969, 1330, 1771. */
constexpr int blockOffset (int l) noexcept { return l * (2 * l - 1) * (2 * l + 1) / 3; }

/** Total coefficients for blocks 0..order. */
constexpr int numCoeffs (int order) noexcept { return blockOffset (order) + blockSize (order); }

constexpr int kNumRotationCoeffs = numCoeffs (xoa::kAmbisonicOrder);
static_assert (kNumRotationCoeffs == 1771,
               "order-10 block-diagonal rotation carries 1771 coefficients");

/** Block-diagonal rotation matrix for all degrees 0..kAmbisonicOrder.
    Trivially copyable POD (RtSnapshot-ready; 1771 * 8 = 14168 bytes).
    Addressing note: acn(l,m) = l*l + (m+l), so the channel run of degree l
    starts at l*l and the in-block row/column index is (m+l). */
struct RotationMatrix
{
    double coeffs[kNumRotationCoeffs];

    double*       block (int l) noexcept       { return coeffs + blockOffset (l); }
    const double* block (int l) const noexcept { return coeffs + blockOffset (l); }

    /** R^l_{m,m'}: row m (output degree index), column m' (input). */
    double entry (int l, int m, int mp) const noexcept
    {
        jassert (l >= 0 && l <= xoa::kAmbisonicOrder && std::abs (m) <= l && std::abs (mp) <= l);
        return coeffs[blockOffset (l) + (m + l) * (2 * l + 1) + (mp + l)];
    }

    double& entry (int l, int m, int mp) noexcept
    {
        jassert (l >= 0 && l <= xoa::kAmbisonicOrder && std::abs (m) <= l && std::abs (mp) <= l);
        return coeffs[blockOffset (l) + (m + l) * (2 * l + 1) + (mp + l)];
    }
};

static_assert (std::is_trivially_copyable_v<RotationMatrix>,
               "RotationMatrix is an RtSnapshot payload");

//==============================================================================
// Small linear-algebra types
//==============================================================================

/** 3x3 rotation acting on column vectors of (x,y,z): (R·d)_i = sum_j m[i][j] d_j.
    Row-major, rows/columns in (x,y,z) order. */
struct Mat3
{
    double m[3][3];
};

/** Rotate a cartesian vector. */
inline coords::Cartesian transform (const Mat3& r, const coords::Cartesian& v) noexcept
{
    return { r.m[0][0] * v.x + r.m[0][1] * v.y + r.m[0][2] * v.z,
             r.m[1][0] * v.x + r.m[1][1] * v.y + r.m[1][2] * v.z,
             r.m[2][0] * v.x + r.m[2][1] * v.y + r.m[2][2] * v.z };
}

/** Component order w,x,y,z; right-handed; ACTIVE rotation v' = q v q*.
    q and -q are the same rotation. Unit norm is not required at the API —
    the build normalizes. */
struct Quaternion
{
    double w, x, y, z;
};

/** Quaternion -> 3x3 active rotation. Normalizes first; a zero/non-finite
    quaternion jasserts and returns identity.
    Handedness check: q = (cos g/2, 0, 0, sin g/2) gives m[0][1] = -2wz
    = -sin g, matching Rz(g) below. */
inline Mat3 quaternionToMatrix (const Quaternion& q) noexcept
{
    const double n2 = q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z;
    if (! std::isfinite (n2) || n2 <= 0.0)
    {
        jassertfalse;
        return { { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } } };
    }

    const double s = 1.0 / std::sqrt (n2);
    const double w = q.w * s, x = q.x * s, y = q.y * s, z = q.z * s;
    return { { { 1 - 2 * (y * y + z * z), 2 * (x * y - w * z),     2 * (x * z + w * y) },
               { 2 * (x * y + w * z),     1 - 2 * (x * x + z * z), 2 * (y * z - w * x) },
               { 2 * (x * z - w * y),     2 * (y * z + w * x),     1 - 2 * (x * x + y * y) } } };
}

/** Yaw/pitch/roll (degrees) -> 3x3 rotation.

    PINNED CONVENTION — intrinsic Z-Y'-X'': yaw g about +Z first, then pitch b
    about the new +Y, then roll a about the twice-rotated +X, each by the
    right-hand rule in the +X-front/+Y-left/+Z-up frame. As matrices on column
    vectors (roll applied first reading right-to-left):

        R = Rz(g) · Ry(b) · Rx(a)

        Rz = | cg -sg 0 |   Ry = |  cb 0 sb |   Rx = | 1  0   0 |
             | sg  cg 0 |        |  0  1 0  |        | 0 ca -sa |
             | 0   0  1 |        | -sb 0 cb |        | 0 sa  ca |

        R = | cg*cb   cg*sb*sa - sg*ca   cg*sb*ca + sg*sa |
            | sg*cb   sg*sb*sa + cg*ca   sg*sb*ca - cg*sa |
            | -sb     cb*sa              cb*ca            |

    Perceptual semantics that follow (the R6 test contract — stated bluntly):
      yaw+   turns the scene toward the LEFT (+Y): encode(az,el) -> encode(az+g, el).
      pitch+ tips the front of the scene DOWN (RH about +Y takes +X toward -Z):
             encode(0,0) -> encode(0,-b). Ecosystem tools often expose the
             opposite sign; any user-facing flip is a WP9/WP10 boundary
             mapping, never applied here.
      roll+  raises the LEFT side of the scene (RH about +X takes +Y toward +Z):
             encode(+90,0) -> encode(+90,+a); the front (+X) is invariant.
    Head-tracking note: compensating a HEAD rotation applies the inverse
    (transpose) — WP9's concern. */
inline Mat3 yawPitchRollToMatrix (double yawDeg, double pitchDeg, double rollDeg) noexcept
{
    const double g = juce::degreesToRadians (yawDeg);
    const double b = juce::degreesToRadians (pitchDeg);
    const double a = juce::degreesToRadians (rollDeg);
    const double cg = std::cos (g), sg = std::sin (g);
    const double cb = std::cos (b), sb = std::sin (b);
    const double ca = std::cos (a), sa = std::sin (a);
    return { { { cg * cb, cg * sb * sa - sg * ca, cg * sb * ca + sg * sa },
               { sg * cb, sg * sb * sa + cg * ca, sg * sb * ca - cg * sa },
               { -sb,     cb * sa,                cb * ca } } };
}

//==============================================================================
// Build
//==============================================================================

/** Every block the unit matrix. */
inline void identity (RotationMatrix& out) noexcept
{
    for (int l = 0; l <= xoa::kAmbisonicOrder; ++l)
    {
        const int n = 2 * l + 1;
        double* b = out.block (l);
        for (int i = 0; i < n; ++i)
            for (int j = 0; j < n; ++j)
                b[i * n + j] = (i == j) ? 1.0 : 0.0;
    }
}

namespace detail
{
    /** P_i(a,b) helper of the I-R recursion: built from the l=1 block r and
        the previous block R^{l-1} (size np = 2(l-1)+1, index offset lm1 = l-1).
        i in {-1,0,+1}; a in [-(l-1), l-1] guaranteed by the U/V/W case splits
        and zero-coefficient guards. */
    inline double irP (const double* r1, const double* prev, int lm1,
                       int i, int a, int b, int l) noexcept
    {
        const int np = 2 * lm1 + 1;
        const auto r = [r1] (int ri, int rj) noexcept { return r1[(ri + 1) * 3 + (rj + 1)]; };
        const auto p = [prev, lm1, np] (int pa, int pb) noexcept
        {
            return prev[(pa + lm1) * np + (pb + lm1)];
        };

        if (b == l)
            return r (i, +1) * p (a, lm1) - r (i, -1) * p (a, -lm1);
        if (b == -l)
            return r (i, +1) * p (a, -lm1) + r (i, -1) * p (a, lm1);
        return r (i, 0) * p (a, b);
    }
} // namespace detail

/** Build all blocks 0..kAmbisonicOrder from a 3x3 proper rotation
    (column-vector convention; det must be +1 — debug-asserted; reflections go
    through xoa::mirror).

    l=1 seed, derived from our own definitions (the l=1 SH triple evaluates to
    the direction components (y,z,x)):
        Y_{1,-1}(R·d) = y' = Ryy·Y_{1,-1} + Ryz·Y_{1,0} + Ryx·Y_{1,+1}
        Y_{1, 0}(R·d) = z' = Rzy·Y_{1,-1} + Rzz·Y_{1,0} + Rzx·Y_{1,+1}
        Y_{1,+1}(R·d) = x' = Rxy·Y_{1,-1} + Rxz·Y_{1,0} + Rxx·Y_{1,+1}
    i.e. R1_{m,m'} = R_{sigma(m),sigma(m')} with sigma(-1)=y, sigma(0)=z,
    sigma(+1)=x.

    Recursion for l >= 2 (Ivanic-Ruedenberg 1996, erratum-corrected 1998):
        R^l_{m,m'} = u·U + v·V + w·W
    with D = (l+m')(l-m') for |m'| < l, D = (2l)(2l-1) for |m'| = l, and
        u =        sqrt( (l+m)(l-m) / D )
        v = (1/2)· sqrt( (1+d_{m,0})(l+|m|-1)(l+|m|) / D ) · (1 - 2 d_{m,0})
        w = -(1/2)·sqrt( (l-|m|-1)(l-|m|) / D ) · (1 - d_{m,0})
    Integer numerators are computed BEFORE the sqrt (|m| = l makes the
    w-numerator exactly 0, never negative under sqrt), and U/W are evaluated
    ONLY when their coefficient is nonzero — the skipped helper would index
    outside R^{l-1}:
        u = 0  iff |m| = l;   w = 0  iff |m| >= l-1.

        U = P_0(m, m')                                        (all m)
        V: m=0 : P_1(1,m') + P_{-1}(-1,m')
           m>0 : P_1(m-1,m')·sqrt(1+d_{m,1})   - P_{-1}(-m+1,m')·(1-d_{m,1})
           m<0 : P_1(m+1,m')·(1-d_{m,-1})      + P_{-1}(-m-1,m')·sqrt(1+d_{m,-1})
        W: m=0 : (w = 0 — never evaluated)
           m>0 : P_1(m+1,m') + P_{-1}(-m-1,m')
           m<0 : P_1(m-1,m') - P_{-1}(-m+1,m')
    The sqrt(1+d) factors in V are exactly the 1998-erratum corrections; even
    post-erratum secondary sources disagree on the m<0 case — the version
    above produces orthogonal blocks and is enforced by the R2/R3/R5 tests
    against an I-R-free quadrature reference.

    Hand-verified anchors (in OUR basis; kept as recurrence tripwires):
      yaw Rz(a):  R2_{2,2} = cos 2a, R2_{-2,2} = sin 2a; at every l the
                  per-|mu| 2x2 pattern R^l_{-mu,-mu}=R^l_{mu,mu}=cos(mu a),
                  R^l_{-mu,+mu}=sin(mu a), R^l_{+mu,-mu}=-sin(mu a), rest 0.
      pitch Ry(b): R2_{0,0} = cos^2 b - sin^2 b / 2, R2_{0,1} = -sqrt3·sinb·cosb,
                   R2_{0,2} = (sqrt3/2)·sin^2 b.
      roll Rx(a): R2_{-1,-1} = cos 2a — exercises the erratum-critical
                  sqrt(1+d_{m,-1}) = sqrt2 branch (the uncorrected table
                  breaks this and orthogonality). */
inline void buildFromCartesian (const Mat3& r, RotationMatrix& out) noexcept
{
#if JUCE_DEBUG
    const double det = r.m[0][0] * (r.m[1][1] * r.m[2][2] - r.m[1][2] * r.m[2][1])
                     - r.m[0][1] * (r.m[1][0] * r.m[2][2] - r.m[1][2] * r.m[2][0])
                     + r.m[0][2] * (r.m[1][0] * r.m[2][1] - r.m[1][1] * r.m[2][0]);
    jassert (std::abs (det - 1.0) < 1.0e-6);   // proper rotations only
#endif

    // l = 0: W is rotation-invariant.
    out.coeffs[0] = 1.0;

    // l = 1 seed: permutation-conjugated direction cosines, sigma = (y,z,x).
    {
        double* b1 = out.block (1);
        constexpr int sigma[3] = { 1, 2, 0 };   // SH index (m+1) -> cartesian axis
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                b1[i * 3 + j] = r.m[sigma[i]][sigma[j]];
    }

    // l >= 2: the recursion, reading only block(1) and block(l-1).
    const double* r1 = out.block (1);
    for (int l = 2; l <= xoa::kAmbisonicOrder; ++l)
    {
        const int lm1 = l - 1;
        const double* prev = out.block (lm1);
        double* blk = out.block (l);
        const int n = 2 * l + 1;

        for (int m = -l; m <= l; ++m)
        {
            const int am = std::abs (m);
            for (int mp = -l; mp <= l; ++mp)
            {
                const int denom = (std::abs (mp) < l) ? (l + mp) * (l - mp)
                                                      : (2 * l) * (2 * l - 1);

                double value = 0.0;

                // u * U
                const int uNum = (l + m) * (l - m);
                if (uNum > 0)
                {
                    const double u = std::sqrt (static_cast<double> (uNum) / denom);
                    value += u * detail::irP (r1, prev, lm1, 0, m, mp, l);
                }

                // v * V
                {
                    const int vNum = (m == 0 ? 2 : 1) * (l + am - 1) * (l + am);
                    const double v = 0.5 * std::sqrt (static_cast<double> (vNum) / denom)
                                     * (m == 0 ? -1.0 : 1.0);
                    double bigV;
                    if (m == 0)
                        bigV = detail::irP (r1, prev, lm1, +1, 1, mp, l)
                             + detail::irP (r1, prev, lm1, -1, -1, mp, l);
                    else if (m > 0)
                        bigV = detail::irP (r1, prev, lm1, +1, m - 1, mp, l)
                                   * (m == 1 ? std::sqrt (2.0) : 1.0)
                             - detail::irP (r1, prev, lm1, -1, -m + 1, mp, l)
                                   * (m == 1 ? 0.0 : 1.0);
                    else
                        bigV = detail::irP (r1, prev, lm1, +1, m + 1, mp, l)
                                   * (m == -1 ? 0.0 : 1.0)
                             + detail::irP (r1, prev, lm1, -1, -m - 1, mp, l)
                                   * (m == -1 ? std::sqrt (2.0) : 1.0);
                    value += v * bigV;
                }

                // w * W  (w = 0 for |m| >= l-1 and for m = 0)
                const int wNum = (l - am - 1) * (l - am);
                if (m != 0 && wNum > 0)
                {
                    const double w = -0.5 * std::sqrt (static_cast<double> (wNum) / denom);
                    const double bigW = (m > 0)
                        ? detail::irP (r1, prev, lm1, +1, m + 1, mp, l)
                            + detail::irP (r1, prev, lm1, -1, -m - 1, mp, l)
                        : detail::irP (r1, prev, lm1, +1, m - 1, mp, l)
                            - detail::irP (r1, prev, lm1, -1, -m + 1, mp, l);
                    value += w * bigW;
                }

                blk[(m + l) * n + (mp + l)] = value;
            }
        }
    }
}

/** Build from a quaternion (normalized internally; q == -q; zero/non-finite
    falls back to identity with a jassert). */
inline void buildFromQuaternion (const Quaternion& q, RotationMatrix& out) noexcept
{
    buildFromCartesian (quaternionToMatrix (q), out);
}

/** Build from yaw/pitch/roll degrees per the pinned Z-Y'-X'' convention. */
inline void buildFromYawPitchRoll (double yawDeg, double pitchDeg, double rollDeg,
                                   RotationMatrix& out) noexcept
{
    buildFromCartesian (yawPitchRollToMatrix (yawDeg, pitchDeg, rollDeg), out);
}

//==============================================================================
// Apply
//==============================================================================

/** out = R · in over numChannels(order) channels (block-diagonal multiply,
    ~1771 MACs at order 10). Exact in == out is allowed (per-block scratch);
    PARTIAL overlap is forbidden. Release-safe: order outside
    [0, kAmbisonicOrder] no-ops (jassert). */
inline void apply (const RotationMatrix& rot, int order, const double* in, double* out) noexcept
{
    jassert (order >= 0 && order <= xoa::kAmbisonicOrder);
    jassert (in != nullptr && out != nullptr);
    if (order < 0 || order > xoa::kAmbisonicOrder)
        return;

    double scratch[2 * xoa::kAmbisonicOrder + 1];

    for (int l = 0; l <= order; ++l)
    {
        const int n = 2 * l + 1;
        const int base = l * l;   // acn(l, -l)
        const double* blk = rot.block (l);

        const double* src = in + base;
        if (in == out)
        {
            std::memcpy (scratch, src, static_cast<size_t> (n) * sizeof (double));
            src = scratch;
        }

        for (int i = 0; i < n; ++i)
        {
            double acc = 0.0;
            const double* row = blk + i * n;
            for (int j = 0; j < n; ++j)
                acc += row[j] * src[j];
            out[base + i] = acc;
        }
    }
}

} // namespace xoa::rot
