#pragma once

#include <juce_core/juce_core.h>

#include <cmath>
#include <vector>

#include "XoaConstants.h"
#include "Helpers/XoaCoordinates.h"
#include "DSP/AmbiDecoderDesigner.h"
#include "DSP/AmbiSphericalHarmonics.h"

//==============================================================================
// XOA - rV/rE decoder analysis and matrix/analysis export (FR-18).
//
// For a source direction d the decoder produces speaker gains g_s = sum_c D[s][c] a_c
// with a = Y^SN3D(d). Two localisation vectors:
//   velocity  rV = sum_s g_s u_s / sum_s g_s          (||rV|| ~ 1 for velocity/basic
//                                                       decoding below the crossover)
//   energy    rE = sum_s g_s^2 u_s / sum_s g_s^2       (||rE|| < 1; its direction is
//                                                       the perceived high-frequency image)
// Direction error = angle(vector, d). A decoder can thus be *seen* before it is heard.
//
// The analysis grid here is a plain az/el display grid (exactness is not needed for
// visualisation); the committed quadrature grid is for the WP3 orthonormality test.
//==============================================================================

namespace xoa::analysis
{

// Acceptance tolerances (DEVPLAN WP5 -> consumed by WP13). "Coverage region" is
// fixture/rig-defined (a ring's equator, a dome above its rim); the constants are
// the thresholds applied there.
constexpr double kReDirectionErrorMaxDeg = 5.0;    // rE dir error, above crossover, in coverage
constexpr double kRvMagnitudeMin = 0.95;           // ||rV|| below crossover, regular rigs
constexpr double kRvMagnitudeMax = 1.05;

struct DirectionSample
{
    double azimuthDeg = 0.0, elevationDeg = 0.0;
    double rvMagnitude = 0.0, rvDirErrorDeg = 0.0;
    double reMagnitude = 0.0, reDirErrorDeg = 0.0;
    double energy = 0.0;
    bool rvValid = false, reValid = false;   // false when the denominator underflowed
};

namespace detail
{
    inline coords::Cartesian unit (const coords::Cartesian& p) noexcept
    {
        const double r = std::sqrt (p.x * p.x + p.y * p.y + p.z * p.z);
        return (r > 0.0) ? coords::Cartesian { p.x / r, p.y / r, p.z / r }
                         : coords::Cartesian { 1.0, 0.0, 0.0 };
    }

    inline double angleDeg (const coords::Cartesian& v, double mag, const coords::Cartesian& d) noexcept
    {
        if (mag <= 0.0)
            return 0.0;
        double cosang = (v.x * d.x + v.y * d.y + v.z * d.z) / mag;
        cosang = juce::jlimit (-1.0, 1.0, cosang);
        return juce::radiansToDegrees (std::acos (cosang));
    }
}

inline DirectionSample analyzeDirection (const decoder::DecoderMatrix& mtx,
                                         const decoder::SpeakerLayout& layout,
                                         double azimuthDeg, double elevationDeg)
{
    DirectionSample out;
    out.azimuthDeg = azimuthDeg;
    out.elevationDeg = elevationDeg;

    const int order = mtx.order;
    const int K = sh::numChannels (order);
    double a[xoa::kNumSHChannels];
    sh::evaluate (azimuthDeg, elevationDeg, order, a);

    const coords::Cartesian d = detail::unit (
        coords::sphericalToCartesian ({ 1.0, azimuthDeg, elevationDeg }));

    double sumG = 0.0, sumG2 = 0.0;
    coords::Cartesian rvNum { 0, 0, 0 }, reNum { 0, 0, 0 };
    for (int s = 0; s < mtx.numSpeakers; ++s)
    {
        double gs = 0.0;
        for (int c = 0; c < K; ++c)
            gs += mtx.d[(size_t) s * K + c] * a[c];
        const auto u = detail::unit (layout.positions[s]);
        sumG += gs;
        sumG2 += gs * gs;
        rvNum.x += gs * u.x; rvNum.y += gs * u.y; rvNum.z += gs * u.z;
        reNum.x += gs * gs * u.x; reNum.y += gs * gs * u.y; reNum.z += gs * gs * u.z;
    }
    out.energy = sumG2;

    if (std::abs (sumG) > 1e-12)
    {
        const coords::Cartesian rv { rvNum.x / sumG, rvNum.y / sumG, rvNum.z / sumG };
        out.rvMagnitude = std::sqrt (rv.x * rv.x + rv.y * rv.y + rv.z * rv.z);
        out.rvDirErrorDeg = detail::angleDeg (rv, out.rvMagnitude, d);
        out.rvValid = true;
    }
    if (sumG2 > 1e-12)
    {
        const coords::Cartesian re { reNum.x / sumG2, reNum.y / sumG2, reNum.z / sumG2 };
        out.reMagnitude = std::sqrt (re.x * re.x + re.y * re.y + re.z * re.z);
        out.reDirErrorDeg = detail::angleDeg (re, out.reMagnitude, d);
        out.reValid = true;
    }
    return out;
}

struct GridOptions { int azimuthSteps = 72; int elevationSteps = 37; };   // 5-degree display grid

inline std::vector<DirectionSample> analyzeGrid (const decoder::DecoderMatrix& mtx,
                                                 const decoder::SpeakerLayout& layout,
                                                 GridOptions g = {})
{
    std::vector<DirectionSample> out;
    out.reserve ((size_t) g.azimuthSteps * g.elevationSteps);
    for (int ei = 0; ei < g.elevationSteps; ++ei)
    {
        const double el = -90.0 + 180.0 * ei / (g.elevationSteps - 1);
        for (int ai = 0; ai < g.azimuthSteps; ++ai)
        {
            const double az = -180.0 + 360.0 * ai / g.azimuthSteps;
            out.push_back (analyzeDirection (mtx, layout, az, el));
        }
    }
    return out;
}

//==============================================================================
// Exports (FR-18). %.17g gives bit-exact double round-trips.
//==============================================================================
namespace detail
{
    inline juce::String g17 (double v)
    {
        char buf[32];
        std::snprintf (buf, sizeof (buf), "%.17g", v);
        return juce::String (buf);
    }
}

inline juce::String toCsv (const std::vector<DirectionSample>& samples)
{
    juce::String s;
    s << "azimuthDeg,elevationDeg,rvMagnitude,rvDirErrorDeg,reMagnitude,reDirErrorDeg,energy\n";
    for (const auto& d : samples)
    {
        s << detail::g17 (d.azimuthDeg) << ',' << detail::g17 (d.elevationDeg) << ','
          << (d.rvValid ? detail::g17 (d.rvMagnitude) : juce::String()) << ','
          << (d.rvValid ? detail::g17 (d.rvDirErrorDeg) : juce::String()) << ','
          << (d.reValid ? detail::g17 (d.reMagnitude) : juce::String()) << ','
          << (d.reValid ? detail::g17 (d.reDirErrorDeg) : juce::String()) << ','
          << detail::g17 (d.energy) << '\n';
    }
    return s;
}

inline juce::String decoderMatrixToCsv (const decoder::DecoderMatrix& m)
{
    const int K = sh::numChannels (m.order);
    juce::String s;
    for (int spk = 0; spk < m.numSpeakers; ++spk)
    {
        for (int c = 0; c < K; ++c)
        {
            if (c > 0) s << ',';
            s << detail::g17 (m.at (spk, c));
        }
        s << '\n';
    }
    return s;
}

inline juce::String decoderMatrixToJsonString (const decoder::DecoderMatrix& m)
{
    const int K = sh::numChannels (m.order);
    juce::String s;
    s << "{\n  \"numSpeakers\": " << m.numSpeakers << ",\n  \"order\": " << m.order
      << ",\n  \"convention\": \"SN3D/ACN in, rows=speakers\",\n  \"matrix\": [\n";
    for (int spk = 0; spk < m.numSpeakers; ++spk)
    {
        s << "    [";
        for (int c = 0; c < K; ++c) { if (c > 0) s << ", "; s << detail::g17 (m.at (spk, c)); }
        s << (spk + 1 < m.numSpeakers ? "],\n" : "]\n");
    }
    s << "  ]\n}\n";
    return s;
}

inline bool decoderMatrixFromJson (const juce::String& json, decoder::DecoderMatrix& out)
{
    const auto v = juce::JSON::parse (json);
    if (! v.isObject())
        return false;
    out.numSpeakers = (int) v["numSpeakers"];
    out.order = (int) v["order"];
    const int K = sh::numChannels (out.order);
    const auto rows = v["matrix"];
    if (! rows.isArray() || rows.size() != out.numSpeakers)
        return false;
    out.d.assign ((size_t) out.numSpeakers * K, 0.0);
    for (int s = 0; s < out.numSpeakers; ++s)
    {
        const auto row = rows[s];
        if (! row.isArray() || row.size() != K)
            return false;
        for (int c = 0; c < K; ++c)
            out.d[(size_t) s * K + c] = (double) row[c];
    }
    return true;
}

} // namespace xoa::analysis
