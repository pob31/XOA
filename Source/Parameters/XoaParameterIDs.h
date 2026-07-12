#pragma once

#include <juce_core/juce_core.h>

//==============================================================================
// XOA — parameter and node-type identifiers.
//
// One inline const juce::Identifier per ValueTree node type and per parameter
// (C++17 inline variables: one instance program-wide). Scope routing is by
// name prefix (see XoaValueTreeState::getParameterScope): "input*" = per-input,
// "speaker*"/"eq*" = per-speaker, "decoder*" = decoder section, everything
// else = Config. Keep new parameter names inside that convention.
//
// Rule: no other namespace-scope global may construct from these ids at
// static-init time (the constraints table is a function-local static for
// exactly this reason).
//==============================================================================

namespace xoa::ids
{

// ValueTree node types
inline const juce::Identifier root       { "XOA" };
inline const juce::Identifier config     { "Config" };
inline const juce::Identifier inputs     { "Inputs" };
inline const juce::Identifier input      { "Input" };
inline const juce::Identifier speakers   { "Speakers" };
inline const juce::Identifier speaker    { "Speaker" };
inline const juce::Identifier decoder    { "Decoder" };
inline const juce::Identifier monitoring { "Monitoring" };
inline const juce::Identifier channel    { "Channel" };
inline const juce::Identifier position   { "Position" };
inline const juce::Identifier encoder    { "Encoder" };
inline const juce::Identifier eq         { "EQ" };
inline const juce::Identifier band       { "Band" };

// Section-file / manifest root node types
inline const juce::Identifier configFileRoot   { "XOAConfig" };
inline const juce::Identifier inputsFileRoot   { "XOAInputs" };
inline const juce::Identifier speakersFileRoot { "XOASpeakers" };
inline const juce::Identifier decoderFileRoot  { "XOADecoder" };
inline const juce::Identifier projectManifest  { "XOAProject" };

// Bookkeeping properties
inline const juce::Identifier idProp        { "id" };
inline const juce::Identifier schemaVersion { "schemaVersion" };
inline const juce::Identifier inputCount    { "inputCount" };
inline const juce::Identifier speakerCount  { "speakerCount" };

// Config
inline const juce::Identifier showName         { "showName" };
inline const juce::Identifier masterGain       { "masterGain" };
inline const juce::Identifier oscEnabled       { "oscEnabled" };
inline const juce::Identifier oscReceivePort   { "oscReceivePort" };
inline const juce::Identifier oscSendPort      { "oscSendPort" };
inline const juce::Identifier oscSendAddress   { "oscSendAddress" };
inline const juce::Identifier audioDeviceState { "audioDeviceState" };

// Config / Scene rotation (FR-9/FR-10 — the runtime SO(3) orientation;
// "rotation*"/"playback*" ride the everything-else-is-Config scope rule)
inline const juce::Identifier rotationYaw   { "rotationYaw" };
inline const juce::Identifier rotationPitch { "rotationPitch" };
inline const juce::Identifier rotationRoll  { "rotationRoll" };

// Config / Playback (FR-8). Play state and transport position are
// deliberately runtime-only: persisting them would pollute undo/dirty.
inline const juce::Identifier playbackFilePath     { "playbackFilePath" };
inline const juce::Identifier playbackLoop         { "playbackLoop" };
inline const juce::Identifier playbackContentOrder { "playbackContentOrder" };  // 0 = auto-detect
inline const juce::Identifier playbackConvention   { "playbackConvention" };    // 0 SN3D, 1 N3D, 2 FuMa

// Config / per-speaker distance compensation (FR-15). "distance*" routes to
// Config; the per-speaker delay/gain come from speaker positions, not schema.
inline const juce::Identifier distanceCompMode { "distanceCompMode" };  // 0 off, 1 delay, 2 delay+gain

// Config / listener position (D18/FR-25). Sweet-spot shift: the distance-comp
// delays/gains are referenced to this point instead of the rig origin.
// "listener*" routes to Config. Default (0,0,0) = origin (comp bit-identical).
inline const juce::Identifier listenerX { "listenerX" };
inline const juce::Identifier listenerY { "listenerY" };
inline const juce::Identifier listenerZ { "listenerZ" };

// Config / mono encoders (FR-5/FR-6, WP8). Master gate for the encoder stage;
// off by default so the RT bus is bit-identical to M2 (no stems mixed in).
inline const juce::Identifier monoInputsEnabled { "monoInputsEnabled" };

// Input / Channel
inline const juce::Identifier inputName { "inputName" };
inline const juce::Identifier inputGain { "inputGain" };
inline const juce::Identifier inputMute { "inputMute" };

// Input / Position (canonical cartesian meters; mode is display-only)
inline const juce::Identifier inputPositionX      { "inputPositionX" };
inline const juce::Identifier inputPositionY      { "inputPositionY" };
inline const juce::Identifier inputPositionZ      { "inputPositionZ" };
inline const juce::Identifier inputCoordinateMode { "inputCoordinateMode" };
// Position conditioning (WP8): speed-limited moves + 1-Euro tracking smoothing.
inline const juce::Identifier inputMaxSpeed       { "inputMaxSpeed" };       // m/s, 0 = off
inline const juce::Identifier inputTrackingSmooth { "inputTrackingSmooth" }; // %, 1-Euro smoothing

// Input / Encoder
inline const juce::Identifier inputSpread     { "inputSpread" };
inline const juce::Identifier inputNfcEnabled { "inputNfcEnabled" };

// Speaker / Channel
inline const juce::Identifier speakerName  { "speakerName" };
inline const juce::Identifier speakerGain  { "speakerGain" };
inline const juce::Identifier speakerDelay { "speakerDelay" };
inline const juce::Identifier speakerMute  { "speakerMute" };
inline const juce::Identifier speakerSolo  { "speakerSolo" };   // transient: stripped on save

// Speaker / Position
inline const juce::Identifier speakerPositionX      { "speakerPositionX" };
inline const juce::Identifier speakerPositionY      { "speakerPositionY" };
inline const juce::Identifier speakerPositionZ      { "speakerPositionZ" };
inline const juce::Identifier speakerCoordinateMode { "speakerCoordinateMode" };

// Speaker / EQ ("eq*" band names match WFS-DIY verbatim so the FR-16 EQ
// import is a later name-for-name copy)
inline const juce::Identifier speakerEqEnabled { "speakerEqEnabled" };
inline const juce::Identifier eqShape          { "eqShape" };
inline const juce::Identifier eqFrequency      { "eqFrequency" };
inline const juce::Identifier eqGain           { "eqGain" };
inline const juce::Identifier eqQ              { "eqQ" };
inline const juce::Identifier eqSlope          { "eqSlope" };

// Decoder
inline const juce::Identifier decoderType               { "decoderType" };
inline const juce::Identifier decoderWeighting          { "decoderWeighting" };
inline const juce::Identifier decoderDualBandEnabled    { "decoderDualBandEnabled" };
inline const juce::Identifier decoderCrossoverFrequency { "decoderCrossoverFrequency" };
inline const juce::Identifier decoderNormalization      { "decoderNormalization" };

} // namespace xoa::ids
