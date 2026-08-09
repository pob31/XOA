/*
  ==============================================================================

    XOA — tenth-order Ambisonics spatial audio processor.
    AmbiHeadMapping — the ONE place where spatcore's head-attitude convention
    is translated into XOA's soundfield convention (WP15, D51).

    Two independent conventions meet here, and neither may be bent:

    spatcore::binaural::HeadOrientation (BinauralTypes.h) — head frame
      +x out of the RIGHT ear, +y facing, +z up; angles in RADIANS, offsets
      from a "facing the stage" baseline:
        +yaw   = turn right
        +pitch = look up
        +roll  = right ear down
      The webcam plugin's signs were validated empirically against a human,
      so these are ground truth, not a preference.

    xoa::rot (AmbiRotation.h) — soundfield frame
      +X front, +Y left, +Z up; angles in DEGREES, intrinsic Z-Y'-X'':
        yaw+   turns the scene toward the LEFT (+Y)
        pitch+ tips the front of the scene DOWN
        roll+  raises the LEFT side of the scene
      That header states the policy this file exists to honour: "any
      user-facing flip is a WP9/WP10 boundary mapping, NEVER applied here".

    Derivation of the head attitude in XOA axes (same intrinsic Z-Y'-X''
    ordering spatcore composes with, so the two agree term by term):
      turn right  → front (+X) toward the right (−Y) → Rz(−yaw)
      look up     → front (+X) toward up (+Z); XOA's pitch+ tips it DOWN
                    → Ry(−pitch)
      right ear down → the LEFT side rises, which is XOA's roll+ → Rx(+roll)
    i.e. R_head = yawPitchRollToMatrix(−yaw°, −pitch°, +roll°).

    Compensation is the INVERSE of the head attitude (AmbiRotation.h:175):
    rotating the field by R_headᵀ holds the world still while the head moves,
    which is what a fixed set of SH→ear filters needs. Since R_head is
    orthonormal the inverse is the transpose — computed as a matrix, never as
    three negated angles (rotations do not commute; the composed result and
    the per-angle negation differ as soon as two axes are involved, which
    XoaHeadMappingTests asserts).

  ==============================================================================
*/

#pragma once

#include <juce_core/juce_core.h>

#include "spatcore/binaural/BinauralTypes.h"

#include "AmbiRotation.h"

namespace xoa::binaural
{

/** Head attitude (spatcore convention, radians) as a rotation matrix in XOA's
    soundfield frame. Not what the renderer applies — see
    headCompensationMatrix — but the honest attitude, useful for tests and
    debug readouts. */
inline rot::Mat3 headAttitudeMatrix (const spatcore::binaural::HeadOrientation& o) noexcept
{
    return rot::yawPitchRollToMatrix (-juce::radiansToDegrees ((double) o.yawRad),
                                      -juce::radiansToDegrees ((double) o.pitchRad),
                                       juce::radiansToDegrees ((double) o.rollRad));
}

/** The soundfield rotation that compensates a head attitude: R_headᵀ.
    Feed straight to rot::buildFromCartesian. */
inline rot::Mat3 headCompensationMatrix (const spatcore::binaural::HeadOrientation& o) noexcept
{
    const rot::Mat3 head = headAttitudeMatrix (o);
    rot::Mat3 out {};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            out.m[i][j] = head.m[j][i];
    return out;
}

/** The compensation expressed in XOA's yaw/pitch/roll degrees — the form the
    debug readout and any UI display want. Equivalent to decomposing
    headCompensationMatrix(). */
inline void headCompensationYawPitchRoll (const spatcore::binaural::HeadOrientation& o,
                                          double& yawDeg, double& pitchDeg,
                                          double& rollDeg) noexcept
{
    rot::matrixToYawPitchRoll (headCompensationMatrix (o), yawDeg, pitchDeg, rollDeg);
}

/** Manual orientation (XOA store parameters, degrees in the SPATCORE sense —
    the UI speaks "turn right / look up / right ear down" because that is what
    a user tilting their head expects) packed into a HeadOrientation, so the
    manual and tracked paths converge on one representation before any
    mapping happens. */
inline spatcore::binaural::HeadOrientation manualOrientation (double yawDeg, double pitchDeg,
                                                              double rollDeg) noexcept
{
    spatcore::binaural::HeadOrientation o;
    o.yawRad   = (float) juce::degreesToRadians (yawDeg);
    o.pitchRad = (float) juce::degreesToRadians (pitchDeg);
    o.rollRad  = (float) juce::degreesToRadians (rollDeg);
    o.valid    = true;
    return o;
}

} // namespace xoa::binaural
