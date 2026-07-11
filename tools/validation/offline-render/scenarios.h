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
// The four scenarios exercise distinct chain paths:
//   static3     order-3 scene, identity rotation      - the null-decode anchor
//   rotate      order-3 scene, swept yaw + pitch wobble - rotation transitions
//   order-adapt order-10 scene, content order 1/3/10  - the FR-7 gather path
//   scene10     order-10 scene, fixed 30 deg yaw       - full 121-ch rotate/GEMM
//==============================================================================

#include <cmath>
#include <string>
#include <vector>

#include "XoaConstants.h"
#include "DSP/AmbiSphericalHarmonics.h"

namespace scenario
{

enum class Id { Static3 = 0, Rotate, OrderAdapt, Scene10 };

inline const char* name (Id id)
{
    switch (id)
    {
        case Id::Static3:    return "static3";
        case Id::Rotate:     return "rotate";
        case Id::OrderAdapt: return "order-adapt";
        case Id::Scene10:    return "scene10";
    }
    return "?";
}

inline bool fromName (const std::string& s, Id& out)
{
    if (s == "static3")     { out = Id::Static3;    return true; }
    if (s == "rotate")      { out = Id::Rotate;     return true; }
    if (s == "order-adapt") { out = Id::OrderAdapt; return true; }
    if (s == "scene10")     { out = Id::Scene10;    return true; }
    return false;
}

inline const std::vector<Id>& allScenarios()
{
    static const std::vector<Id> all { Id::Static3, Id::Rotate, Id::OrderAdapt, Id::Scene10 };
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
        case Id::Scene10:    return xoa::kAmbisonicOrder;
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
            yawDeg = 30.0; pitchDeg = 0.0; rollDeg = 0.0;
            break;
        case Id::Static3:
        case Id::OrderAdapt:
        default:
            yawDeg = 0.0; pitchDeg = 0.0; rollDeg = 0.0;
            break;
    }
}

} // namespace scenario
