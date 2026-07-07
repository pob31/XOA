#pragma once

#include <juce_core/juce_core.h>

#include <cmath>

//==============================================================================
// XOA — coordinate conversions, Ambisonics conventions.
//
// Frame (AmbiX / ACN-SN3D standard):
//   +X = front, +Y = left, +Z = up (right-handed)
//   azimuth   = atan2(y, x), degrees, counter-clockwise positive
//               (0 = front, +90 = left), normalized to (-180, +180]
//   elevation = asin(z / r), degrees, in [-90, +90] (+90 = zenith)
//
// Adapted from WFS-DIY's CoordinateConverter.h SHAPE only — its azimuth is a
// stage convention (measured from +Y via atan2(x, y)) and must not be used
// here: it would put an Ambisonics front source at 90 degrees and flip the
// rotation sense.
//
// All math in double (schema rule: float only at read boundaries).
// r == 0 has undefined direction: conversions return azimuth = elevation = 0.
//==============================================================================

namespace xoa::coords
{

/** Matches the *CoordinateMode int parameters (display preference only —
    positions are stored as canonical cartesian meters). */
enum class Mode
{
    cartesian   = 0,
    cylindrical = 1,
    spherical   = 2
};

struct Cartesian   { double x {}, y {}, z {}; };                       // meters
struct Cylindrical { double radius {}, azimuthDeg {}, z {}; };         // radius in the XY plane
struct Spherical   { double radius {}, azimuthDeg {}, elevationDeg {}; };

/** Normalize an azimuth to (-180, +180]. */
inline double normalizeAzimuthDegrees (double degrees) noexcept
{
    double a = std::fmod (degrees, 360.0);
    if (a > 180.0)
        a -= 360.0;
    else if (a <= -180.0)
        a += 360.0;
    return a;
}

inline Spherical cartesianToSpherical (const Cartesian& c) noexcept
{
    const double r = std::sqrt (c.x * c.x + c.y * c.y + c.z * c.z);
    if (r <= 0.0)
        return { 0.0, 0.0, 0.0 };

    const double az = juce::radiansToDegrees (std::atan2 (c.y, c.x));
    const double el = juce::radiansToDegrees (std::asin (juce::jlimit (-1.0, 1.0, c.z / r)));
    return { r, normalizeAzimuthDegrees (az), el };
}

inline Cartesian sphericalToCartesian (const Spherical& s) noexcept
{
    const double az = juce::degreesToRadians (s.azimuthDeg);
    const double el = juce::degreesToRadians (juce::jlimit (-90.0, 90.0, s.elevationDeg));
    const double rCosEl = s.radius * std::cos (el);
    return { rCosEl * std::cos (az), rCosEl * std::sin (az), s.radius * std::sin (el) };
}

inline Cylindrical cartesianToCylindrical (const Cartesian& c) noexcept
{
    const double radius = std::sqrt (c.x * c.x + c.y * c.y);
    if (radius <= 0.0)
        return { 0.0, 0.0, c.z };

    const double az = juce::radiansToDegrees (std::atan2 (c.y, c.x));
    return { radius, normalizeAzimuthDegrees (az), c.z };
}

inline Cartesian cylindricalToCartesian (const Cylindrical& c) noexcept
{
    const double az = juce::degreesToRadians (c.azimuthDeg);
    return { c.radius * std::cos (az), c.radius * std::sin (az), c.z };
}

/** Human-readable position per display mode (WP10 consumer). */
inline juce::String formatForDisplay (const Cartesian& c, Mode mode)
{
    const auto num = [] (double v) { return juce::String (v, 2); };

    switch (mode)
    {
        case Mode::cylindrical:
        {
            const auto cyl = cartesianToCylindrical (c);
            return "r " + num (cyl.radius) + " m, az " + num (cyl.azimuthDeg)
                 + " deg, z " + num (cyl.z) + " m";
        }
        case Mode::spherical:
        {
            const auto sph = cartesianToSpherical (c);
            return "r " + num (sph.radius) + " m, az " + num (sph.azimuthDeg)
                 + " deg, el " + num (sph.elevationDeg) + " deg";
        }
        case Mode::cartesian:
        default:
            return "x " + num (c.x) + " m, y " + num (c.y) + " m, z " + num (c.z) + " m";
    }
}

} // namespace xoa::coords
