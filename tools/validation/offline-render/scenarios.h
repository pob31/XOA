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
//   encode-static  8 mono stems on the 2 m ring (= r_ref -> distance gain 1),
//                  spread 0, NFC off, identity rotation (WP8) - the encoder null
//                  anchor: output == decode . sum(encode . stem)
//   encode-move    4 mono stems (azimuth orbit; radial 0.5<->4 m sweep with NFC
//                  on; spread 0->90 deg sweep; one static), 20 deg yaw (WP8) -
//                  distance gain + clamps + NFC + spread + rotation together
//
// The WP7/WP8 additions carry their rig (layout / decoder options / comp /
// encoder flag) in the harness's rigFor(); this header owns the per-tick scene
// params and, for the encoders, the per-source specs + stem synthesis.
//==============================================================================

#include <cmath>
#include <string>
#include <vector>

#include "XoaConstants.h"
#include "DSP/AmbiSphericalHarmonics.h"

namespace scenario
{

enum class Id { Static3 = 0, Rotate, OrderAdapt, Scene10, ShoeboxAllRad, DualBand, Comp,
                EncodeStatic, EncodeMove };

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
        case Id::EncodeStatic:  return "encode-static";
        case Id::EncodeMove:    return "encode-move";
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
    if (s == "encode-static")  { out = Id::EncodeStatic;  return true; }
    if (s == "encode-move")    { out = Id::EncodeMove;    return true; }
    return false;
}

inline const std::vector<Id>& allScenarios()
{
    static const std::vector<Id> all { Id::Static3, Id::Rotate, Id::OrderAdapt, Id::Scene10,
                                       Id::ShoeboxAllRad, Id::DualBand, Id::Comp,
                                       Id::EncodeStatic, Id::EncodeMove };
    return all;
}

//==============================================================================
// Encoder scenarios (WP8): a scenario is an encoder scenario iff it has stems.
inline bool isEncoder (Id id) { return id == Id::EncodeStatic || id == Id::EncodeMove; }

inline int stemCount (Id id)
{
    if (id == Id::EncodeStatic) return 8;
    if (id == Id::EncodeMove)   return 4;
    return 0;
}

// Per-source encoder parameters at a 50 Hz tick (pure function). Canonical
// cartesian meters; the ring reference radius is 2 m so a source at radius 2
// has unity distance gain.
struct SourceSpec { double x = 1.0, y = 0.0, z = 0.0, gainDb = 0.0, spreadDeg = 0.0; bool nfc = false; };

inline SourceSpec sourceSpec (Id id, int src, int tick)
{
    constexpr double pi = 3.14159265358979323846;
    const double t = (double) tick / 50.0;   // seconds
    SourceSpec s;

    if (id == Id::EncodeStatic)
    {
        const double az = pi / 180.0 * (45.0 * src);   // 8 sources, 45 deg apart
        s.x = 2.0 * std::cos (az);
        s.y = 2.0 * std::sin (az);
        return s;
    }
    if (id == Id::EncodeMove)
    {
        switch (src)
        {
            case 0:   // azimuth orbit at radius 2
            {
                const double az = pi / 180.0 * (60.0 * t);
                s.x = 2.0 * std::cos (az);
                s.y = 2.0 * std::sin (az);
                break;
            }
            case 1:   // radial sweep 0.5 <-> 4 m on +Y, NFC on (distance + clamps)
                s.x = 0.0;
                s.y = 2.25 + 1.75 * std::sin (2.0 * pi * 0.25 * t);
                s.nfc = true;
                break;
            case 2:   // fixed at -X, spread sweep 0 -> 90 deg
                s.x = -2.0;
                s.spreadDeg = 45.0 + 45.0 * std::sin (2.0 * pi * 0.2 * t);
                break;
            default:  // static reference at -Y
                s.y = -2.0;
                break;
        }
        return s;
    }
    return s;
}

// One stem's audio sample at an absolute sample index (partition-independent,
// so the SHA baseline is block-size stable). Distinct tone per source.
inline float stemSample (Id id, int src, long long sampleIndex, double sr)
{
    (void) id;
    constexpr double pi = 3.14159265358979323846;
    const double freq = 150.0 + 50.0 * src;
    return 0.3f * (float) std::sin (2.0 * pi * freq * (double) sampleIndex / sr);
}

//==============================================================================
/** SH order the input scene is rendered at (fills numChannels(order) channels).
    Encoder scenarios have a silent HOA input (output is the encoded stems), so
    the smallest buffer suffices. */
inline int sceneOrder (Id id)
{
    if (isEncoder (id))                       return 0;
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
        case Id::EncodeStatic:
        case Id::EncodeMove: return 0;      // silent HOA; the stems carry the signal
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
        case Id::EncodeMove:               // fixed yaw: encoded sources rotate with the field
            yawDeg = 20.0; pitchDeg = 0.0; rollDeg = 0.0;
            break;
        case Id::Static3:
        case Id::OrderAdapt:
        case Id::ShoeboxAllRad:            // identity: isolate the AllRAD decode
        case Id::Comp:                     // identity: isolate the comp stage
        case Id::EncodeStatic:             // identity: the encoder null anchor
        default:
            yawDeg = 0.0; pitchDeg = 0.0; rollDeg = 0.0;
            break;
    }
}

} // namespace scenario
