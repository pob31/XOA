#pragma once

#include <juce_core/juce_core.h>

#include <cmath>
#include <vector>

#include "Helpers/XoaCoordinates.h"
#include "DSP/ConvexHull.h"

//==============================================================================
// XOA - vector-base amplitude panning over a triangulated loudspeaker hull
// (WP7, the geometric half of AllRAD). Non-RT / control side.
//
// triangulate() normalizes speaker positions to unit directions, inserts
// IMAGINARY loudspeakers at coverage gaps (Zotter-Frank: e.g. a missing floor
// gets a nadir speaker so the hull closes; their gains are discarded by the
// AllRAD accumulation later), builds the convex hull, and precomputes one
// closed-form 3x3 inverse per triangle. computeGains() then pans a unit
// direction over the triangulation: g = (L^-1)^T u for the triangle whose
// gains are most positive, L2-normalized (Pulkki 1997).
//
// A triangulation is usable for VBAP ONLY IF the hull ENCLOSES THE ORIGIN -
// otherwise there are directions in no triangle and computeGains would return
// silence for them. The elevation-gap pole insertion is a heuristic; the
// authoritative test is enclosure (originEnclosed): every consistently-wound
// hull face shares the sign of the triple product a.(bxc), i.e. every face
// plane keeps the origin on its interior side. Mixed signs => the origin pokes
// through some face => a coverage gap (a frontal/wedge rig, an off-origin
// planar wall).
//
// Degeneracy and gaps are first-class: if the first hull does not enclose the
// origin (a coplanar ring through the origin, or a partial-coverage rig),
// triangulate() forces BOTH poles in and retries once; only if that STILL does
// not enclose the origin does it report ok = false (the decoder designer then
// falls back to SAD with a warning - honest for a rig AllRAD cannot cover).
//==============================================================================

namespace xoa::vbap
{

/** Coverage-gap threshold: if no speaker lies below -45 deg elevation, an
    imaginary nadir speaker is inserted (likewise +45 deg / zenith). */
constexpr double kImaginaryElevationGapDeg = 45.0;

/** Triangles with |det| below this are skipped (a face nearly coplanar with
    the origin cannot be inverted meaningfully). */
constexpr double kTriangleDetEpsilon = 1.0e-9;

/** Gains this far below zero still count as "inside" (rounding slack). */
constexpr double kGainNegativeTolerance = 1.0e-9;

struct Triangle
{
    int i0 = 0, i1 = 0, i2 = 0;
    double inv[9] = {};   // row-major inverse of L = [u_i0; u_i1; u_i2]
};

struct Triangulation
{
    std::vector<coords::Cartesian> dirs;   // unit dirs: real speakers first, imaginaries appended
    int numReal = 0;
    int numImaginary = 0;
    std::vector<Triangle> triangles;
    bool ok = false;
    juce::StringArray warnings;
};

namespace detail
{
    inline coords::Cartesian unitDir (const coords::Cartesian& p) noexcept
    {
        const double r = std::sqrt (p.x * p.x + p.y * p.y + p.z * p.z);
        if (r <= 0.0)
            return { 1.0, 0.0, 0.0 };   // r == 0 has no direction; front, coords convention
        return { p.x / r, p.y / r, p.z / r };
    }

    /** Triple product a.(b x c) == 6x the signed volume of the tetrahedron
        (origin, a, b, c). Its sign tells which side of face (a,b,c)'s plane the
        origin is on. */
    inline double faceOriginVolume (const coords::Cartesian& a, const coords::Cartesian& b,
                                    const coords::Cartesian& c) noexcept
    {
        return a.x * (b.y * c.z - b.z * c.y)
             - a.y * (b.x * c.z - b.z * c.x)
             + a.z * (b.x * c.y - b.y * c.x);
    }

    /** True iff the convex hull described by these faces STRICTLY encloses the
        origin. convhull_3d winds all faces consistently, so an enclosing hull
        has the origin strictly on the interior side of every face plane ->
        every face's triple product a.(bxc) shares one sign and none is a
        sliver. A coverage gap makes some face plane cross to the far side of
        the origin (both signs appear); a boundary-touching gap - e.g. a
        frontal wedge whose back faces contain the z-axis through the origin -
        produces sliver faces (|v| ~ 0). Rejecting sliver faces catches the
        latter, which a pure sign vote would miss. */
    inline bool originEnclosed (const std::vector<coords::Cartesian>& dirs,
                                const std::vector<int>& faces) noexcept
    {
        int pos = 0, neg = 0, sliver = 0;
        for (size_t f = 0; f + 2 < faces.size(); f += 3)
        {
            const double v = faceOriginVolume (dirs[(size_t) faces[f]],
                                               dirs[(size_t) faces[f + 1]],
                                               dirs[(size_t) faces[f + 2]]);
            if (v > kTriangleDetEpsilon)       ++pos;
            else if (v < -kTriangleDetEpsilon) ++neg;
            else                               ++sliver;   // origin on/near this face plane
        }
        return sliver == 0 && ((pos >= 4 && neg == 0) || (neg >= 4 && pos == 0));
    }

    /** Build the triangle list (with per-face inverses) from hull faces.
        Returns the number of faces skipped by the determinant guard. */
    inline int buildTriangles (const std::vector<coords::Cartesian>& dirs,
                               const std::vector<int>& faces,
                               std::vector<Triangle>& out)
    {
        int skipped = 0;
        out.clear();
        out.reserve (faces.size() / 3);

        for (size_t f = 0; f + 2 < faces.size(); f += 3)
        {
            const auto& a = dirs[(size_t) faces[f]];
            const auto& b = dirs[(size_t) faces[f + 1]];
            const auto& c = dirs[(size_t) faces[f + 2]];

            // det of L = [a; b; c] (rows) == triple product a . (b x c)
            const double det = faceOriginVolume (a, b, c);
            if (std::abs (det) < kTriangleDetEpsilon)
            {
                ++skipped;
                continue;
            }

            Triangle t;
            t.i0 = faces[f];
            t.i1 = faces[f + 1];
            t.i2 = faces[f + 2];

            // inv = adjugate(L) / det, row-major.
            const double d = 1.0 / det;
            t.inv[0] = (b.y * c.z - b.z * c.y) * d;
            t.inv[1] = (a.z * c.y - a.y * c.z) * d;
            t.inv[2] = (a.y * b.z - a.z * b.y) * d;
            t.inv[3] = (b.z * c.x - b.x * c.z) * d;
            t.inv[4] = (a.x * c.z - a.z * c.x) * d;
            t.inv[5] = (a.z * b.x - a.x * b.z) * d;
            t.inv[6] = (b.x * c.y - b.y * c.x) * d;
            t.inv[7] = (a.y * c.x - a.x * c.y) * d;
            t.inv[8] = (a.x * b.y - a.y * b.x) * d;
            out.push_back (t);
        }
        return skipped;
    }
}

/** Triangulate a speaker layout for VBAP. Positions need not be normalized;
    at least 3 real speakers are required. */
inline Triangulation triangulate (const coords::Cartesian* positions, int count)
{
    Triangulation t;
    if (positions == nullptr || count < 3)
    {
        t.warnings.add ("VBAP triangulation needs at least 3 speakers.");
        return t;
    }

    t.numReal = count;
    t.dirs.reserve ((size_t) count + 2);
    double minElDeg = 90.0, maxElDeg = -90.0;
    for (int i = 0; i < count; ++i)
    {
        const auto u = detail::unitDir (positions[i]);
        t.dirs.push_back (u);
        const double el = juce::radiansToDegrees (std::asin (juce::jlimit (-1.0, 1.0, u.z)));
        minElDeg = juce::jmin (minElDeg, el);
        maxElDeg = juce::jmax (maxElDeg, el);
    }

    // Coverage-gap imaginaries (Zotter-Frank): close the hull below/above.
    bool haveNadir = false, haveZenith = false;
    if (minElDeg > -kImaginaryElevationGapDeg)
    {
        t.dirs.push_back ({ 0.0, 0.0, -1.0 });
        haveNadir = true;
        t.warnings.add ("Inserted imaginary speaker at nadir (no coverage below -"
                        + juce::String (kImaginaryElevationGapDeg, 0) + " deg).");
    }
    if (maxElDeg < kImaginaryElevationGapDeg)
    {
        t.dirs.push_back ({ 0.0, 0.0, 1.0 });
        haveZenith = true;
        t.warnings.add ("Inserted imaginary speaker at zenith (no coverage above +"
                        + juce::String (kImaginaryElevationGapDeg, 0) + " deg).");
    }
    t.numImaginary = (haveNadir ? 1 : 0) + (haveZenith ? 1 : 0);

    // Build the hull and keep it only if it ENCLOSES THE ORIGIN (usable for
    // VBAP over the whole sphere). Enclosure, not "some triangles built", is
    // the success test - a frontal/wedge/off-origin-plane rig builds a fine
    // hull that simply does not wrap the origin.
    auto attempt = [&t]() -> bool
    {
        const auto faces = hull::triangleFaces (t.dirs.data(), (int) t.dirs.size());
        if (faces.size() < 12)                          // a closed 3-D hull has >= 4 faces
            return false;
        if (! detail::originEnclosed (t.dirs, faces))   // coverage gap / degenerate
            return false;
        detail::buildTriangles (t.dirs, faces, t.triangles);
        return t.triangles.size() >= 4;
    };

    if (! attempt())
    {
        // No enclosing hull yet (a coplanar ring through the origin, or a
        // partial-coverage rig): force both poles in and retry once.
        if (! haveNadir)
        {
            t.dirs.push_back ({ 0.0, 0.0, -1.0 });
            t.warnings.add ("Degenerate/partial hull; forced imaginary speaker at nadir.");
        }
        if (! haveZenith)
        {
            t.dirs.push_back ({ 0.0, 0.0, 1.0 });
            t.warnings.add ("Degenerate/partial hull; forced imaginary speaker at zenith.");
        }
        t.numImaginary = (int) t.dirs.size() - t.numReal;

        if (! attempt())
        {
            t.triangles.clear();
            t.warnings.add ("VBAP triangulation failed: the layout does not enclose "
                            "the listener (AllRAD needs coverage around the sphere).");
            return t;   // ok stays false -> designer falls back to SAD
        }
    }

    t.ok = true;
    return t;
}

/** VBAP gains for the unit direction u over t.dirs (imaginaries included).
    gains must hold t.dirs.size() doubles; it is zero-filled and the winning
    triangle's three entries are set, L2-normalized (||g||_2 == 1). Returns
    the winning triangle index, or -1 (gains all zero) when the triangulation
    is not ok or no triangle contains u. */
inline int computeGains (const Triangulation& t, const coords::Cartesian& u, double* gains)
{
    const size_t n = t.dirs.size();
    for (size_t i = 0; i < n; ++i)
        gains[i] = 0.0;
    if (! t.ok)
        return -1;

    // g = (L^-1)^T u, i.e. g_i = sum_j inv[j][i] * u_j. The containing
    // triangle is the one whose smallest gain is largest (all >= 0 inside).
    int best = -1;
    double bestMin = -1.0e300;
    double bestG[3] = { 0.0, 0.0, 0.0 };
    for (size_t f = 0; f < t.triangles.size(); ++f)
    {
        const auto& tri = t.triangles[f];
        const double g0 = tri.inv[0] * u.x + tri.inv[3] * u.y + tri.inv[6] * u.z;
        const double g1 = tri.inv[1] * u.x + tri.inv[4] * u.y + tri.inv[7] * u.z;
        const double g2 = tri.inv[2] * u.x + tri.inv[5] * u.y + tri.inv[8] * u.z;
        const double gmin = juce::jmin (g0, g1, g2);
        if (gmin > bestMin)
        {
            bestMin = gmin;
            best = (int) f;
            bestG[0] = g0; bestG[1] = g1; bestG[2] = g2;
        }
    }

    if (best < 0 || bestMin < -kGainNegativeTolerance)
        return -1;

    double g0 = juce::jmax (0.0, bestG[0]);
    double g1 = juce::jmax (0.0, bestG[1]);
    double g2 = juce::jmax (0.0, bestG[2]);
    const double norm = std::sqrt (g0 * g0 + g1 * g1 + g2 * g2);
    if (norm <= 0.0)
        return -1;
    g0 /= norm; g1 /= norm; g2 /= norm;

    const auto& tri = t.triangles[(size_t) best];
    gains[(size_t) tri.i0] = g0;
    gains[(size_t) tri.i1] = g1;
    gains[(size_t) tri.i2] = g2;
    return best;
}

} // namespace xoa::vbap
