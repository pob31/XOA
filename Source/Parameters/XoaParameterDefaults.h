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
