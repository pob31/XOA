#pragma once

#include <juce_core/juce_core.h>

#include "../XoaConstants.h"

//==============================================================================
// XOA — parameter defaults and ranges.
//
// The single data table both validation gates and all schema builders read.
// Every numeric is double: juce::var has no float constructor, and a float
// routed through var stores its promotion garbage (0.1f ->
// 0.10000000149011612) straight into project files. float exists only at
// read boundaries (getFloatParameter).
//
// Naming convention (WFS-DIY shape): xDefault / xMin / xMax triples.
//==============================================================================

namespace xoa::defaults
{

/** Stamped on the live root, every section-file root, and the manifest.
    Files with a NEWER version are refused; older/equal versions merge
    (backfill covers additive schema change). */
constexpr int kSchemaVersion = 1;

/** Fresh-project speaker ring radius (meters). */
constexpr double kDefaultRigRadius = 2.0;

// Config ----------------------------------------------------------------
inline const juce::String showNameDefault { "Untitled Show" };
constexpr double masterGainDefault = 0.0,  masterGainMin = -60.0, masterGainMax = 12.0;   // dB
constexpr bool   oscEnabledDefault = false;
constexpr double oscReceivePortDefault = 9000.0, oscPortMin = 1.0, oscPortMax = 65535.0;
constexpr double oscSendPortDefault    = 9001.0;
inline const juce::String oscSendAddressDefault { "127.0.0.1" };
// WP9 OSC transport additions. TCP off by default (UDP is the common path);
// feedback on so a peer sees the effect of others' edits; meters off (opt-in).
constexpr bool   oscTcpEnabledDefault = false;
constexpr double oscTcpPortDefault = 9002.0;
constexpr bool   oscAcceptAnyHostDefault = true;    // trackers are rarely send-targets
constexpr bool   oscFeedbackEnabledDefault = true;
constexpr bool   oscMeterEnabledDefault = false;

// Config / Scene rotation (degrees, WP4 yaw-pitch-roll convention: intrinsic
// Z-Y'-X''). Pitch is limited to +/-90 so every orientation has exactly one
// representation (and WP9's quaternion decomposition lands in range).
constexpr double rotationYawDefault   = 0.0, rotationYawMin   = -180.0, rotationYawMax   = 180.0;
constexpr double rotationPitchDefault = 0.0, rotationPitchMin =  -90.0, rotationPitchMax =  90.0;
constexpr double rotationRollDefault  = 0.0, rotationRollMin  = -180.0, rotationRollMax  = 180.0;

// Config / Playback. Content order 0 means "auto-detect from channel count";
// the max is the bus order (order-generic rule FR-3).
inline const juce::String playbackFilePathDefault {};
constexpr bool   playbackLoopDefault = false;
constexpr double playbackContentOrderDefault = 0.0, playbackContentOrderMin = 0.0,
                 playbackContentOrderMax = (double) xoa::kAmbisonicOrder;
constexpr double playbackConventionDefault = 0.0,
                 playbackConventionMin = 0.0, playbackConventionMax = 2.0;   // 0 SN3D, 1 N3D, 2 FuMa

// Config / distance compensation (FR-15). Default off preserves M1 behaviour
// and every existing baseline.
constexpr double distanceCompModeDefault = 0.0,
                 distanceCompModeMin = 0.0, distanceCompModeMax = 2.0;   // 0 off, 1 delay, 2 delay+gain

// Config / listener position (D18/FR-25), meters. Default origin = rig center;
// at the origin the compensation re-reference is bit-identical to the pre-D18
// law (see SpeakerCompParams.h). Bounds share the position range [-100, 100].
constexpr double listenerXDefault = 0.0, listenerYDefault = 0.0, listenerZDefault = 0.0;

// Config / mono encoders (WP8). Default off keeps the RT bus bit-identical to M2.
constexpr bool   monoInputsEnabledDefault = false;

// Inputs ----------------------------------------------------------------
constexpr double inputGainDefault = 0.0, inputGainMin = -60.0, inputGainMax = 12.0;       // dB
constexpr bool   inputMuteDefault = false;
// Default position (1, 0, 0), not the origin: r = 0 has undefined direction
// and is the NFC worst case.
constexpr double inputPositionXDefault = 1.0, inputPositionYDefault = 0.0,
                 inputPositionZDefault = 0.0;
constexpr double positionMin = -100.0, positionMax = 100.0;                               // meters
constexpr double coordinateModeDefault = 0.0, coordinateModeMin = 0.0, coordinateModeMax = 2.0;
constexpr double inputSpreadDefault = 0.0, inputSpreadMin = 0.0, inputSpreadMax = 180.0;  // degrees
constexpr bool   inputNfcEnabledDefault = false;
// Position conditioning (WP8). maxSpeed 0 = off; the limiter's own clamp is
// [0.01, 20] m/s. trackingSmooth is the 1-Euro percentage (0 = raw, 100 = max).
constexpr double inputMaxSpeedDefault = 0.0, inputMaxSpeedMin = 0.0, inputMaxSpeedMax = 20.0;   // m/s
constexpr double inputTrackingSmoothDefault = 50.0, inputTrackingSmoothMin = 0.0, inputTrackingSmoothMax = 100.0;  // %

// Speakers --------------------------------------------------------------
constexpr double speakerGainDefault  = 0.0, speakerGainMin = -60.0, speakerGainMax = 12.0; // dB trim
constexpr double speakerDelayDefault = 0.0, speakerDelayMin = 0.0,  speakerDelayMax = 500.0; // ms
constexpr bool   speakerMuteDefault = false;
constexpr bool   speakerSoloDefault = false;
constexpr bool   speakerEqEnabledDefault = false;

// Speaker EQ bands. eqShape values are pinned to
// spatcore::dsp::OutputEQBiquadFilter: 0 OFF, 1 LowCut, 2 LowShelf, 3 Peak,
// 4 BandPass, 5 HighShelf, 6 HighCut, 7 AllPass.
constexpr double eqShapeDefault = 0.0, eqShapeMin = 0.0, eqShapeMax = 7.0;
constexpr double eqFrequencyMin = 20.0, eqFrequencyMax = 20000.0;                         // Hz
constexpr double kEqBandDefaultHz[xoa::kNumEqBands] = { 63.0, 160.0, 400.0, 1000.0, 2500.0, 6300.0 };
constexpr double eqGainDefault  = 0.0,   eqGainMin  = -24.0, eqGainMax  = 24.0;           // dB
constexpr double eqQDefault     = 0.707, eqQMin     = 0.1,   eqQMax     = 10.0;
// eqSlope is the RBJ shelf slope S consumed by spatcore::dsp::OutputEQBiquadFilter
// (the (1/slope - 1) term), NOT a dB/octave figure. Range matches WFS-DIY's stored
// slope (S in [0.1, 1.0], no shelf peaking) and sits inside spatcore's [0.1, 20]
// clamp, so WFS EQ import lands in range name-for-name.
constexpr double eqSlopeDefault = 0.7,   eqSlopeMin = 0.1,   eqSlopeMax = 1.0;

// Decoder ---------------------------------------------------------------
constexpr double decoderTypeDefault = 0.0, decoderTypeMin = 0.0, decoderTypeMax = 2.0;    // 0 SAD, 1 mode-match, 2 AllRAD
constexpr double decoderWeightingDefault = 1.0, decoderWeightingMin = 0.0, decoderWeightingMax = 1.0; // 0 basic, 1 max-rE
constexpr bool   decoderDualBandEnabledDefault = false;
constexpr double decoderCrossoverFrequencyDefault = 400.0,
                 decoderCrossoverFrequencyMin = 80.0, decoderCrossoverFrequencyMax = 2000.0; // Hz
constexpr double decoderNormalizationDefault = 1.0,
                 decoderNormalizationMin = 0.0, decoderNormalizationMax = 1.0;            // 0 amplitude, 1 energy

// Helpers ---------------------------------------------------------------
inline juce::String getDefaultInputName (int index)   { return "Input "   + juce::String (index + 1); }
inline juce::String getDefaultSpeakerName (int index) { return "Speaker " + juce::String (index + 1); }

} // namespace xoa::defaults
