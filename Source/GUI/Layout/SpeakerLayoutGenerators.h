/*
  ==============================================================================

    XOA — tenth-order Ambisonics spatial audio processor.
    SpeakerLayoutGenerators — pure geometry for the speaker-layout editor (WP10
    C7). Ring / line are the planar generators (WFS-DIY shape); dome (stacked
    rings on a sphere, count per ring proportional to cos(elevation)) and the
    ceiling grid are XOA's new 3-D generators. XOA frame: +X front, +Y left, +Z up
    (azimuth = atan2(y, x)).

    Console-safe (no JUCE GUI); unit-tested by XoaLayoutGeneratorTests.

    This file is part of XOA, released under the GNU General Public License
    v3.0. See LICENSE for details.

  ==============================================================================
*/

#pragma once

#include <cmath>
#include <vector>

namespace xoa::ui::layout
{

struct SpeakerPos { double x {}, y {}, z {}; };

constexpr double kPi = 3.14159265358979323846;

inline double deg2rad (double d) noexcept { return d * kPi / 180.0; }

/** N speakers evenly spaced around a horizontal ring at height z. */
inline std::vector<SpeakerPos> ring (int n, double radius, double z = 0.0, double startAzDeg = 0.0)
{
    std::vector<SpeakerPos> out;
    if (n <= 0) return out;
    out.reserve ((size_t) n);
    for (int i = 0; i < n; ++i)
    {
        const double az = deg2rad (startAzDeg) + 2.0 * kPi * (double) i / (double) n;
        out.push_back ({ radius * std::cos (az), radius * std::sin (az), z });
    }
    return out;
}

/** N speakers evenly spaced on the segment from `start` to `end` (inclusive). */
inline std::vector<SpeakerPos> line (int n, SpeakerPos start, SpeakerPos end)
{
    std::vector<SpeakerPos> out;
    if (n <= 0) return out;
    out.reserve ((size_t) n);
    const double denom = (n == 1) ? 1.0 : (double) (n - 1);
    for (int i = 0; i < n; ++i)
    {
        const double t = (n == 1) ? 0.0 : (double) i / denom;
        out.push_back ({ start.x + (end.x - start.x) * t,
                         start.y + (end.y - start.y) * t,
                         start.z + (end.z - start.z) * t });
    }
    return out;
}

/** Dome: `numRings` horizontal rings on a sphere of `radius`, from elevation 0
    (the base ring) up to `apexElevationDeg`, the per-ring speaker count scaled by
    cos(elevation) from `baseCount` (min 1 per ring). Optionally caps the dome with
    a single zenith speaker. Rings are returned bottom-to-top. */
inline std::vector<SpeakerPos> dome (int numRings, int baseCount, double radius,
                                     double apexElevationDeg = 75.0, bool includeZenith = true)
{
    std::vector<SpeakerPos> out;
    if (numRings <= 0 || baseCount <= 0)
        return out;

    for (int r = 0; r < numRings; ++r)
    {
        const double elDeg = (numRings == 1) ? 0.0
                           : apexElevationDeg * (double) r / (double) (numRings - 1);
        const double el = deg2rad (elDeg);
        int count = (int) std::lround ((double) baseCount * std::cos (el));
        count = count < 1 ? 1 : count;

        const double rCosEl = radius * std::cos (el);
        const double zEl    = radius * std::sin (el);
        for (int i = 0; i < count; ++i)
        {
            const double az = 2.0 * kPi * (double) i / (double) count;
            out.push_back ({ rCosEl * std::cos (az), rCosEl * std::sin (az), zEl });
        }
    }

    if (includeZenith)
        out.push_back ({ 0.0, 0.0, radius });

    return out;
}

/** Ceiling (or floor) grid: nx x ny speakers in the XY plane at height z, centred
    on the origin, `spacing` metres apart. Row-major (y outer, x inner). */
inline std::vector<SpeakerPos> ceilingGrid (int nx, int ny, double spacing, double z)
{
    std::vector<SpeakerPos> out;
    if (nx <= 0 || ny <= 0) return out;
    out.reserve ((size_t) (nx * ny));
    const double x0 = -0.5 * (double) (nx - 1) * spacing;
    const double y0 = -0.5 * (double) (ny - 1) * spacing;
    for (int j = 0; j < ny; ++j)
        for (int i = 0; i < nx; ++i)
            out.push_back ({ x0 + (double) i * spacing, y0 + (double) j * spacing, z });
    return out;
}

} // namespace xoa::ui::layout
