/*
    XoaVbapTests.cpp - WP7 C2: convex-hull seam + VBAP properties.

    Everything here is golden-free geometry: the properties (direction
    reconstruction, partition behavior at speaker/edge directions, unit-power
    normalization, full sphere coverage, imaginary-speaker insertion and the
    coplanar-ring degeneracy retry) pin the math without depending on any
    particular hull triangulation, so they hold regardless of how the
    quickhull splits symmetric faces.
*/

#include "XoaTestFramework.h"

#include "XoaConstants.h"
#include "DSP/AmbiVBAP.h"
#include "DSP/ConvexHull.h"
#include "DSP/TDesignTables.h"
#include "Helpers/XoaCoordinates.h"

#include <cmath>
#include <vector>

namespace
{

namespace coords = xoa::coords;
namespace vbap = xoa::vbap;

//==============================================================================
// Layouts (code-generated; radii deliberately non-unit to test normalization).
//==============================================================================

// Octahedron: +/-X, +/-Y, +/-Z. Elevations span +/-90 -> no imaginaries.
std::vector<coords::Cartesian> makeOctahedron (double radius)
{
    return {
        {  radius, 0.0, 0.0 }, { -radius, 0.0, 0.0 },
        { 0.0,  radius, 0.0 }, { 0.0, -radius, 0.0 },
        { 0.0, 0.0,  radius }, { 0.0, 0.0, -radius },
    };
}

// Horizontal ring (coplanar; degenerate for a 3-D hull).
std::vector<coords::Cartesian> makeRing (int count, double radius)
{
    std::vector<coords::Cartesian> out;
    for (int s = 0; s < count; ++s)
    {
        const double az = juce::degreesToRadians (360.0 * s / count);
        out.push_back ({ radius * std::cos (az), radius * std::sin (az), 0.0 });
    }
    return out;
}

// Dome: upper-hemisphere rings + a top speaker (no coverage below 0 deg).
std::vector<coords::Cartesian> makeDome (double radius)
{
    std::vector<coords::Cartesian> out;
    const struct { double elDeg; int count; double azOffsetDeg; } rings[] = {
        { 0.0, 12, 0.0 }, { 30.0, 7, 12.857 }, { 55.0, 4, 45.0 }, { 80.0, 1, 0.0 },
    };
    for (const auto& ring : rings)
        for (int s = 0; s < ring.count; ++s)
        {
            const double az = juce::degreesToRadians (ring.azOffsetDeg + 360.0 * s / ring.count);
            const double el = juce::degreesToRadians (ring.elDeg);
            out.push_back ({ radius * std::cos (el) * std::cos (az),
                             radius * std::cos (el) * std::sin (az),
                             radius * std::sin (el) });
        }
    return out;
}

// Coplanar ring tilted about the x-axis so elevations span +/-tiltDeg (>45 ->
// no coverage-gap pole insertion, so the degenerate-hull RETRY path is what
// closes it).
std::vector<coords::Cartesian> makeTiltedRing (int count, double radius, double tiltDeg)
{
    const double t = juce::degreesToRadians (tiltDeg);
    std::vector<coords::Cartesian> out;
    for (int s = 0; s < count; ++s)
    {
        const double az = juce::degreesToRadians (360.0 * s / count);
        const double x = std::cos (az);
        const double y = std::sin (az) * std::cos (t);
        const double z = std::sin (az) * std::sin (t);
        out.push_back ({ radius * x, radius * y, radius * z });
    }
    return out;
}

// Frontal wedge: all speakers in the +x hemisphere, elevations +/-50 (so no
// pole insertion), does NOT enclose the listener -> AllRAD must decline.
std::vector<coords::Cartesian> makeFrontalWedge()
{
    std::vector<coords::Cartesian> out;
    for (double azDeg : { -60.0, 0.0, 60.0 })
        for (double elDeg : { -50.0, 50.0 })
        {
            const double az = juce::degreesToRadians (azDeg), el = juce::degreesToRadians (elDeg);
            out.push_back ({ 2.0 * std::cos (el) * std::cos (az),
                             2.0 * std::cos (el) * std::sin (az),
                             2.0 * std::sin (el) });
        }
    out.push_back ({ 2.0, 0.0, 0.0 });   // dead-centre front
    return out;
}

// Planar wall offset from the origin (x = 2): a spherical cap after
// normalization, does not enclose the listener.
std::vector<coords::Cartesian> makeOffsetWall()
{
    std::vector<coords::Cartesian> out;
    for (int iy = -2; iy <= 2; ++iy)
        for (int iz = -2; iz <= 2; ++iz)
            out.push_back ({ 2.0, 1.5 * iy, 1.25 * iz });
    return out;
}

double norm3 (double x, double y, double z) { return std::sqrt (x * x + y * y + z * z); }

//==============================================================================
void testHullSeam()
{
    // A tetrahedron has exactly 4 faces; degenerate inputs return empty.
    const std::vector<coords::Cartesian> tetra = {
        { 1, 1, 1 }, { 1, -1, -1 }, { -1, 1, -1 }, { -1, -1, 1 },
    };
    const auto faces = xoa::hull::triangleFaces (tetra.data(), 4);
    CHECK (faces.size() == 12);
    for (int idx : faces)
        CHECK (idx >= 0 && idx < 4);

    CHECK (xoa::hull::triangleFaces (tetra.data(), 3).empty());   // < 4 points
    CHECK (xoa::hull::triangleFaces (nullptr, 8).empty());

    // Coplanar square: convhull_3d does NOT fail - it returns flat sliver
    // faces. The VBAP determinant guard is the real degeneracy filter: every
    // returned face must be rejected (usable triangles == 0).
    const std::vector<coords::Cartesian> square = {
        { 1, 1, 0 }, { 1, -1, 0 }, { -1, 1, 0 }, { -1, -1, 0 },
    };
    const auto degenerate = xoa::hull::triangleFaces (square.data(), 4);
    if (! degenerate.empty())
    {
        std::vector<vbap::Triangle> usable;
        const int skipped = vbap::detail::buildTriangles (square, degenerate, usable);
        CHECK (usable.empty());
        CHECK (skipped == (int) (degenerate.size() / 3));
    }
}

//==============================================================================
void testTriangulationSolids()
{
    // Octahedron: full elevation coverage -> no imaginaries, 8 triangles.
    const auto oct = makeOctahedron (2.5);
    const auto t = vbap::triangulate (oct.data(), (int) oct.size());
    CHECK (t.ok);
    CHECK (t.numReal == 6);
    CHECK (t.numImaginary == 0);
    CHECK (t.triangles.size() == 8);
    for (const auto& d : t.dirs)
        CHECK (std::abs (norm3 (d.x, d.y, d.z) - 1.0) < 1.0e-12);   // normalized

    // Dome: nadir imaginary only (top speaker at 80 deg >= +45).
    const auto dome = makeDome (3.0);
    const auto td = vbap::triangulate (dome.data(), (int) dome.size());
    CHECK (td.ok);
    CHECK (td.numReal == 24);
    CHECK (td.numImaginary == 1);
    CHECK (td.dirs.back().z == -1.0);   // the nadir
    CHECK (td.warnings.joinIntoString (" ").containsIgnoreCase ("nadir"));
}

//==============================================================================
// Flat horizontal ring: elevations are all 0, so BOTH coverage-gap poles are
// inserted BEFORE the first hull attempt (the gap-insertion path, not the
// degenerate retry - the tilted-ring test below exercises the retry).
void testFlatRingGapInsertion()
{
    const auto ring = makeRing (24, 2.0);
    const auto t = vbap::triangulate (ring.data(), (int) ring.size());
    CHECK (t.ok);
    CHECK (t.numReal == 24);
    CHECK (t.numImaginary == 2);
    CHECK (t.triangles.size() >= 4);
    // Gap-insertion path: "Inserted ..." warnings, not "forced".
    const auto w = t.warnings.joinIntoString (" ");
    CHECK (w.containsIgnoreCase ("Inserted imaginary speaker at nadir"));
    CHECK (w.containsIgnoreCase ("Inserted imaginary speaker at zenith"));
    CHECK (! w.containsIgnoreCase ("forced"));

    // A horizontal source panned on the ring puts (near) nothing on the poles.
    double gains[26];
    const int tri = vbap::computeGains (t, { std::cos (0.3), std::sin (0.3), 0.0 }, gains);
    CHECK (tri >= 0);
    CHECK (std::abs (gains[24]) < 1.0e-9);
    CHECK (std::abs (gains[25]) < 1.0e-9);

    // Too few speakers: graceful failure.
    const auto two = makeRing (2, 2.0);
    const auto t2 = vbap::triangulate (two.data(), 2);
    CHECK (! t2.ok);
}

//==============================================================================
// Tilted coplanar ring (elevations +/-50 -> no gap poles): the bare hull is
// coplanar-through-origin and fails; the degenerate RETRY forces both poles
// and closes it. This is the path testFlatRingGapInsertion cannot reach.
void testTiltedRingRetryPath()
{
    const auto ring = makeTiltedRing (24, 2.0, 50.0);
    const auto t = vbap::triangulate (ring.data(), (int) ring.size());
    CHECK (t.ok);
    CHECK (t.numReal == 24);
    CHECK (t.numImaginary == 2);
    // Retry path: the "forced" warnings, NOT the gap-insertion "Inserted" ones.
    const auto w = t.warnings.joinIntoString (" ");
    CHECK (w.containsIgnoreCase ("forced imaginary speaker at nadir"));
    CHECK (w.containsIgnoreCase ("forced imaginary speaker at zenith"));

    // The closed hull covers the whole sphere.
    std::vector<double> gains (t.dirs.size(), 0.0);
    int found = 0;
    for (int i = 0; i < xoa::tdesign::kCount; ++i)
        if (vbap::computeGains (t, { xoa::tdesign::kPoints[i][0], xoa::tdesign::kPoints[i][1],
                                     xoa::tdesign::kPoints[i][2] }, gains.data()) >= 0)
            ++found;
    CHECK (found == xoa::tdesign::kCount);
}

//==============================================================================
// A rig that cannot enclose the listener (frontal wedge, off-origin wall) must
// report ok=false even after the forced-pole retry, so the designer falls back
// to SAD rather than silently zeroing most of the sphere.
void testNonEnclosingLayoutsDecline()
{
    const auto wedge = makeFrontalWedge();
    const auto tw = vbap::triangulate (wedge.data(), (int) wedge.size());
    CHECK (! tw.ok);
    CHECK (tw.triangles.empty());
    CHECK (tw.warnings.joinIntoString (" ").containsIgnoreCase ("does not enclose"));

    const auto wall = makeOffsetWall();
    const auto tl = vbap::triangulate (wall.data(), (int) wall.size());
    CHECK (! tl.ok);

    // computeGains on a not-ok triangulation zero-fills and returns -1.
    double gains[64];
    for (int i = 0; i < 64; ++i) gains[i] = 1.0;
    CHECK (vbap::computeGains (tw, { 1.0, 0.0, 0.0 }, gains) == -1);
    CHECK (gains[0] == 0.0);
}

//==============================================================================
// At a speaker direction the pan is exactly that speaker.
void testGainsAtSpeakerDirs()
{
    const auto oct = makeOctahedron (1.0);
    const auto t = vbap::triangulate (oct.data(), (int) oct.size());

    double gains[6];
    for (int s = 0; s < 6; ++s)
    {
        const int tri = vbap::computeGains (t, t.dirs[(size_t) s], gains);
        CHECK (tri >= 0);
        for (int i = 0; i < 6; ++i)
        {
            if (i == s) CHECK (std::abs (gains[i] - 1.0) < 1.0e-12);
            else        CHECK (std::abs (gains[i]) < 1.0e-12);
        }
    }
}

//==============================================================================
// On an edge between two adjacent speakers: exactly two nonzero, equal gains
// (the octahedron edge midpoint is symmetric between its endpoints).
void testGainsOnEdge()
{
    const auto oct = makeOctahedron (1.0);
    const auto t = vbap::triangulate (oct.data(), (int) oct.size());

    const double inv = 1.0 / std::sqrt (2.0);
    double gains[6];
    const int tri = vbap::computeGains (t, { inv, inv, 0.0 }, gains);   // between +X and +Y
    CHECK (tri >= 0);
    CHECK (std::abs (gains[0] - inv) < 1.0e-12);   // +X
    CHECK (std::abs (gains[2] - inv) < 1.0e-12);   // +Y
    CHECK (std::abs (gains[1]) < 1.0e-12);
    CHECK (std::abs (gains[3]) < 1.0e-12);
    CHECK (std::abs (gains[4]) < 1.0e-12);
    CHECK (std::abs (gains[5]) < 1.0e-12);
}

//==============================================================================
// Reconstruction: the weighted sum of speaker dirs is parallel to the source
// direction (the defining VBAP property), and ||g||_2 == 1.
void testReconstructionAndCoverage()
{
    const auto oct = makeOctahedron (1.0);
    const auto tOct = vbap::triangulate (oct.data(), (int) oct.size());
    const auto dome = makeDome (3.0);
    const auto tDome = vbap::triangulate (dome.data(), (int) dome.size());

    std::vector<double> gains;
    for (const auto* t : { &tOct, &tDome })
    {
        gains.assign (t->dirs.size(), 0.0);
        int found = 0;
        double worstParallel = 0.0, worstNorm = 0.0;

        for (int i = 0; i < xoa::tdesign::kCount; ++i)
        {
            const coords::Cartesian u { xoa::tdesign::kPoints[i][0],
                                        xoa::tdesign::kPoints[i][1],
                                        xoa::tdesign::kPoints[i][2] };
            const int tri = vbap::computeGains (*t, u, gains.data());
            if (tri < 0)
                continue;
            ++found;

            double sx = 0.0, sy = 0.0, sz = 0.0, g2 = 0.0;
            for (size_t s = 0; s < t->dirs.size(); ++s)
            {
                sx += gains[s] * t->dirs[s].x;
                sy += gains[s] * t->dirs[s].y;
                sz += gains[s] * t->dirs[s].z;
                g2 += gains[s] * gains[s];
            }
            worstNorm = juce::jmax (worstNorm, std::abs (std::sqrt (g2) - 1.0));

            // Parallel test: |(s/|s|) x u| ~ 0.
            const double sn = norm3 (sx, sy, sz);
            const double cx = (sy * u.z - sz * u.y) / sn;
            const double cy = (sz * u.x - sx * u.z) / sn;
            const double cz = (sx * u.y - sy * u.x) / sn;
            worstParallel = juce::jmax (worstParallel, norm3 (cx, cy, cz));
        }

        // Full sphere coverage: every t-design direction lands in a triangle.
        CHECK (found == xoa::tdesign::kCount);
        CHECK (worstNorm < 1.0e-12);
        CHECK (worstParallel < 1.0e-9);
    }
}

//==============================================================================
// The returned triangle index really identifies the containing triangle: its
// three vertices are exactly the speakers with nonzero gain.
void testReturnedTriangleIndex()
{
    const auto dome = makeDome (3.0);
    const auto t = vbap::triangulate (dome.data(), (int) dome.size());
    std::vector<double> gains (t.dirs.size(), 0.0);

    int checked = 0;
    for (int i = 0; i < xoa::tdesign::kCount; i += 7)   // sample the sphere
    {
        const coords::Cartesian u { xoa::tdesign::kPoints[i][0], xoa::tdesign::kPoints[i][1],
                                    xoa::tdesign::kPoints[i][2] };
        const int tri = vbap::computeGains (t, u, gains.data());
        if (tri < 0)
            continue;
        ++checked;
        CHECK (tri < (int) t.triangles.size());
        const auto& v = t.triangles[(size_t) tri];

        // Every nonzero-gain index is a vertex of the returned triangle.
        for (size_t s = 0; s < gains.size(); ++s)
            if (std::abs (gains[s]) > 1.0e-12)
                CHECK ((int) s == v.i0 || (int) s == v.i1 || (int) s == v.i2);
    }
    CHECK (checked > 0);
}

//==============================================================================
// A below-horizon source on a dome pans mostly onto the imaginary nadir.
void testDomeImaginaryAbsorbsFloor()
{
    const auto dome = makeDome (3.0);
    const auto t = vbap::triangulate (dome.data(), (int) dome.size());
    CHECK (t.numImaginary == 1);
    const size_t nadirIndex = t.dirs.size() - 1;

    std::vector<double> gains (t.dirs.size(), 0.0);
    const int tri = vbap::computeGains (t, { 0.0, 0.0, -1.0 }, gains.data());   // straight down
    CHECK (tri >= 0);
    CHECK (std::abs (gains[nadirIndex] - 1.0) < 1.0e-9);   // all on the imaginary

    // Real-speaker energy after discarding the imaginary row would be ~0 here.
    double realEnergy = 0.0;
    for (size_t s = 0; s < (size_t) t.numReal; ++s)
        realEnergy += gains[s] * gains[s];
    CHECK (realEnergy < 1.0e-12);
}

} // namespace

//==============================================================================
void runXoaVbapTests()
{
    testHullSeam();
    testTriangulationSolids();
    testFlatRingGapInsertion();
    testTiltedRingRetryPath();
    testNonEnclosingLayoutsDecline();
    testGainsAtSpeakerDirs();
    testGainsOnEdge();
    testReconstructionAndCoverage();
    testReturnedTriangleIndex();
    testDomeImaginaryAbsorbsFloor();
}
