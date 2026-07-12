/*
    XoaLayoutGeneratorTests.cpp - WP10 C7.

    Unit tests for the pure speaker-layout generators (ring / line / dome / grid):
    counts, radii on the sphere/ring, z ranges, spacing, centring, and the dome's
    elevation monotonicity.
*/

#include <juce_core/juce_core.h>

#include <cmath>

#include "XoaTestFramework.h"
#include "GUI/Layout/SpeakerLayoutGenerators.h"

namespace
{
double radiusXY (const xoa::ui::layout::SpeakerPos& p) { return std::sqrt (p.x * p.x + p.y * p.y); }
double radius3D (const xoa::ui::layout::SpeakerPos& p) { return std::sqrt (p.x * p.x + p.y * p.y + p.z * p.z); }
bool near (double a, double b, double tol = 1e-9) { return std::abs (a - b) <= tol; }
}

void runXoaLayoutGeneratorTests()
{
    namespace L = xoa::ui::layout;

    // Ring: count, on-circle radius, height, first point at azimuth 0.
    {
        const auto r = L::ring (8, 2.0, 1.0, 0.0);
        CHECK (r.size() == 8);
        for (const auto& p : r)
        {
            CHECK (near (radiusXY (p), 2.0, 1e-9));
            CHECK (near (p.z, 1.0));
        }
        CHECK (near (r[0].x, 2.0, 1e-9));
        CHECK (near (r[0].y, 0.0, 1e-9));
    }

    // Line: endpoints and midpoint.
    {
        const auto l = L::line (5, { 0.0, 0.0, 0.0 }, { 4.0, 0.0, 0.0 });
        CHECK (l.size() == 5);
        CHECK (near (l.front().x, 0.0) && near (l.back().x, 4.0));
        CHECK (near (l[2].x, 2.0));
        // Single point degenerates to the start.
        const auto one = L::line (1, { 1.0, 2.0, 3.0 }, { 9.0, 9.0, 9.0 });
        CHECK (one.size() == 1 && near (one[0].x, 1.0) && near (one[0].z, 3.0));
    }

    // Dome: expected count, on-sphere radius, elevation monotonicity, zenith cap.
    {
        const double R = 2.0;
        const auto d = L::dome (3, 8, R, 60.0, true);
        // ring0=8, ring1=round(8*cos30)=7, ring2=round(8*cos60)=4, +zenith = 20
        CHECK (d.size() == 20);
        double lastZ = -1e9;
        for (const auto& p : d)
        {
            CHECK (near (radius3D (p), R, 1e-9));   // all on the sphere of radius R
            CHECK (p.z >= lastZ - 1e-9);            // returned bottom-to-top
            lastZ = p.z;
        }
        // Zenith is the last point.
        CHECK (near (d.back().x, 0.0) && near (d.back().y, 0.0) && near (d.back().z, R, 1e-9));
        // A single-ring dome sits at elevation 0 (the base ring).
        const auto flat = L::dome (1, 6, R, 60.0, false);
        CHECK (flat.size() == 6);
        for (const auto& p : flat)
            CHECK (near (p.z, 0.0, 1e-9));
    }

    // Ceiling grid: count, height, centring.
    {
        const auto g = L::ceilingGrid (3, 2, 1.0, 3.0);
        CHECK (g.size() == 6);
        double sx = 0.0, sy = 0.0;
        for (const auto& p : g)
        {
            CHECK (near (p.z, 3.0));
            sx += p.x; sy += p.y;
        }
        CHECK (near (sx, 0.0, 1e-9) && near (sy, 0.0, 1e-9));   // centred on the origin
    }
}
