#pragma once

//==============================================================================
// Scripted deterministic scenarios for the XOA offline-render harness.
//
// Every value here is a pure function of (scenario, 50 Hz tick) - no RNG, no
// wall clock - so a scenario always renders the identical stream, which is what
// makes the per-machine SHA baselines meaningful. The runner steps the rotation
// and bus-parameter snapshots at each tick, exactly as the app's message thread
// would, and the RT algorithm re-smooths (crossfades rotation) internally.
//
// The scenarios exercise distinct chain paths:
//   static3     order-3 scene, identity rotation      - the null-decode anchor
//   rotate      order-3 scene, swept yaw + pitch wobble - rotation transitions
//   order-adapt order-10 scene, content order 1/3/10  - the FR-7 gather path
//   scene10     order-10 scene, fixed 30 deg yaw       - full 121-ch rotate/GEMM
//   shoebox-allrad order-10, AllRAD on an irregular room rig (WP7) - VBAP +
//                  imaginary nadir speaker
//   dual-band   order-10, 30 deg yaw, dual-band decode (WP7) - LR4 band split
//   comp        order-10, AllRAD room rig + per-speaker compensation (WP7) -
//                  distance delay/gain + one EQ band
//
// The WP7 additions carry their rig (layout / decoder options / comp) in the
// harness's rigFor(); this header owns only the per-tick scene params.
//==============================================================================

#include <cmath>
#include <string>
#include <vector>

#include "XoaConstants.h"
#include "DSP/AmbiSphericalHarmonics.h"

namespace scenario
{

enum class Id { Static3 = 0, Rotate, OrderAdapt, Scene10, ShoeboxAllRad, DualBand, Comp };

inline const char* name (Id id)
{
    switch (id)
    {
        case Id::Static3:       return "static3";
        case Id::Rotate:        return "rotate";
        case Id::OrderAdapt:    return "order-adapt";
        case Id::Scene10:       return "scene10";
        case Id::ShoeboxAllRad: return "shoebox-allrad";
        case Id::DualBand:      return "dual-band";
        case Id::Comp:          return "comp";
    }
    return "?";
}

inline bool fromName (const std::string& s, Id& out)
{
    if (s == "static3")        { out = Id::Static3;       return true; }
    if (s == "rotate")         { out = Id::Rotate;        return true; }
    if (s == "order-adapt")    { out = Id::OrderAdapt;    return true; }
    if (s == "scene10")        { out = Id::Scene10;       return true; }
    if (s == "shoebox-allrad") { out = Id::ShoeboxAllRad; return true; }
    if (s == "dual-band")      { out = Id::DualBand;      return true; }
    if (s == "comp")           { out = Id::Comp;          return true; }
    return false;
}

inline const std::vector<Id>& allScenarios()
{
    static const std::vector<Id> all { Id::Static3, Id::Rotate, Id::OrderAdapt, Id::Scene10,
                                       Id::ShoeboxAllRad, Id::DualBand, Id::Comp };
    return all;
}

//==============================================================================
/** SH order the input scene is rendered at (fills numChannels(order) channels). */
inline int sceneOrder (Id id)
{
    return (id == Id::Static3 || id == Id::Rotate) ? 3 : xoa::kAmbisonicOrder;
}

/** Content order the decoder gather treats the stream as (drives FR-7 order
    adaptation). order-adapt cycles 1 -> 3 -> 10 on one-second boundaries. */
inline int contentOrder (Id id, int tick)
{
    switch (id)
    {
        case Id::Static3:
        case Id::Rotate:     return 3;
        case Id::Scene10:
        case Id::ShoeboxAllRad:
        case Id::DualBand:
        case Id::Comp:       return xoa::kAmbisonicOrder;
        case Id::OrderAdapt:
        {
            static const int cycle[3] = { 1, 3, xoa::kAmbisonicOrder };
            return cycle[(tick / 50) % 3];   // tick/50 = seconds
        }
    }
    return xoa::kAmbisonicOrder;
}

/** Scene orientation (yaw/pitch/roll in degrees) at a given tick. */
inline void rotation (Id id, int tick, double& yawDeg, double& pitchDeg, double& rollDeg)
{
    const double t = (double) tick / 50.0;   // seconds
    switch (id)
    {
        case Id::Rotate:
            yawDeg   = 60.0 * t;
            pitchDeg = 15.0 * std::sin (2.0 * t);
            rollDeg  = 0.0;
            break;
        case Id::Scene10:
        case Id::DualBand:                 // fixed yaw exercises rotate + dual-band together
            yawDeg = 30.0; pitchDeg = 0.0; rollDeg = 0.0;
            break;
        case Id::Static3:
        case Id::OrderAdapt:
        case Id::ShoeboxAllRad:            // identity: isolate the AllRAD decode
        case Id::Comp:                     // identity: isolate the comp stage
        default:
            yawDeg = 0.0; pitchDeg = 0.0; rollDeg = 0.0;
            break;
    }
}

} // namespace scenario
