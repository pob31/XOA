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

// Audio patch section (stage 2 of the io/patch handoff). Property names match
// WFS-DIY / spatcore::ui::patch::PatchMatrixIds verbatim so the shared matrix
// binds without id overrides and a WFS-DIY patch file reads familiarly.
inline const juce::Identifier audioPatch  { "AudioPatch" };
inline const juce::Identifier inputPatch  { "InputPatch" };
inline const juce::Identifier outputPatch { "OutputPatch" };

// Section-file / manifest root node types
inline const juce::Identifier configFileRoot     { "XOAConfig" };
inline const juce::Identifier inputsFileRoot     { "XOAInputs" };
inline const juce::Identifier speakersFileRoot   { "XOASpeakers" };
inline const juce::Identifier decoderFileRoot    { "XOADecoder" };
inline const juce::Identifier monitoringFileRoot { "XOAMonitoring" };
inline const juce::Identifier audioPatchFileRoot { "XOAAudioPatch" };
inline const juce::Identifier projectManifest    { "XOAProject" };

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

// Config / OSC transport (WP9). Transport parameters are read-only over OSC
// itself (a peer must not reconfigure the socket out from under itself) - they
// are set from the UI / project file only.
inline const juce::Identifier oscTcpEnabled      { "oscTcpEnabled" };     // bind the TCP receiver
inline const juce::Identifier oscTcpPort         { "oscTcpPort" };        // TCP receive port
inline const juce::Identifier oscAcceptAnyHost   { "oscAcceptAnyHost" };  // false -> IP allow-list
inline const juce::Identifier oscFeedbackEnabled { "oscFeedbackEnabled" }; // emit parameter feedback
inline const juce::Identifier oscMeterEnabled    { "oscMeterEnabled" };   // emit /xoa/monitor/*

// Config / Scene rotation (FR-9/FR-10 — the runtime SO(3) orientation;
// "rotation*"/"playback*" ride the everything-else-is-Config scope rule)
inline const juce::Identifier rotationYaw   { "rotationYaw" };
inline const juce::Identifier rotationPitch { "rotationPitch" };
inline const juce::Identifier rotationRoll  { "rotationRoll" };

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
// Stem format: 0 = mono point source (the encoder path), 1..10 = an AmbiX
// (ACN/SN3D) Ambisonics group of order N spanning (N+1)^2 stem channels,
// merged into the bus with order-adapt gains. Position/spread/NFC are inert
// for group formats. Clusters (linking stems) are a later feature; nothing
// may assume a group's hardware channels are contiguous.
inline const juce::Identifier inputFormat { "inputFormat" };

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

// AudioPatch / {Input,Output}Patch node properties. These are DIRECT tree
// properties (the shared matrix and the store write them with explicit
// ValueTree access), not (id, channelIndex)-addressable parameters — keep
// them out of the OSC/descriptor surfaces.
inline const juce::Identifier patchData              { "patchData" };
inline const juce::Identifier rows                   { "rows" };
inline const juce::Identifier cols                   { "cols" };
inline const juce::Identifier activeHardwareInputs   { "activeHardwareInputs" };
inline const juce::Identifier activeHardwareOutputs  { "activeHardwareOutputs" };

// Monitoring / binaural (WP15, D51-D54). "binaural*" routes to the Monitoring
// section. No OSC bindings by design (D54) — these are GUI/project-file only.
// The headphone output PAIR is not here: it lives in the AudioPatch binaural
// tree, like the rest of the patch data.
inline const juce::Identifier binauralEnabled     { "binauralEnabled" };
inline const juce::Identifier binauralGain        { "binauralGain" };        // dB
inline const juce::Identifier binauralSofaFile    { "binauralSofaFile" };    // filename only, "" = built-in
inline const juce::Identifier binauralHeadTracker { "binauralHeadTracker" }; // stable source id, "manual"
inline const juce::Identifier binauralCameraIndex { "binauralCameraIndex" };
// First hardware output of the headphone pair (0-based; the pair is this
// channel and the next). -1 = no pair reserved. Read by the shared patch
// matrix through its binauralOutputChannel id, which is why the name matches
// spatcore's verbatim: the matrix greys the pair out so an operator cannot
// patch a speaker over the monitor.
inline const juce::Identifier binauralOutputChannel { "binauralOutputChannel" };
// Manual head attitude, degrees, in the SPATCORE sense the user experiences:
// +yaw turn right, +pitch look up, +roll right ear down (AmbiHeadMapping.h).
inline const juce::Identifier binauralManualYaw   { "binauralManualYaw" };
inline const juce::Identifier binauralManualPitch { "binauralManualPitch" };
inline const juce::Identifier binauralManualRoll  { "binauralManualRoll" };

// Decoder
inline const juce::Identifier decoderType               { "decoderType" };
inline const juce::Identifier decoderWeighting          { "decoderWeighting" };
inline const juce::Identifier decoderDualBandEnabled    { "decoderDualBandEnabled" };
inline const juce::Identifier decoderCrossoverFrequency { "decoderCrossoverFrequency" };
inline const juce::Identifier decoderNormalization      { "decoderNormalization" };

} // namespace xoa::ids
