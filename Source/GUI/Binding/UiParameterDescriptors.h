/*
  ==============================================================================

    XOA — tenth-order Ambisonics spatial audio processor.
    UiParameterDescriptors — the id-keyed UI metadata layer (WP10 C4): label key,
    unit, step, widget kind, log skew, per-channel flag, undo domain, and combo
    enum-label keys, for every v1 schema parameter. Ranges/defaults are NOT here —
    the binding layer merges xoa::constraints::findBounds(id), so this table can
    never diverge from the schema bounds (retires the shell's hardcoded ranges).

    Console-safe: depends only on juce_core + the parameter ids + the constraints
    table, so the completeness test links it without any GUI/engine dependency.

    This file is part of XOA, released under the GNU General Public License
    v3.0. See LICENSE for details.

  ==============================================================================
*/

#pragma once

#include <juce_core/juce_core.h>

#include <vector>

#include "Parameters/XoaParameterIDs.h"

namespace xoa::ui
{

/** Undo domain a parameter belongs to. Mirrors XoaValueTreeState::UndoDomain but
    stays store-independent so this header is console-safe; the binding layer maps
    it to the store enum. */
enum class Domain { config, inputs, speakers, decoder };

/** How the parameter is presented / edited. `system` = reachable through a
    dedicated surface (device selector, file dialog) rather than a bound widget;
    such descriptors exist for coverage but are exempt from the "reachable via a
    registered widget" completeness rule. */
enum class Kind { slider, dial, toggle, combo, text, system };

struct UiDescriptor
{
    juce::Identifier          id;
    const char*               labelKey;    // LOC key, e.g. "param.masterGain"
    const char*               unit;        // LOC key ("units.db") or "" for none
    double                    step;         // editor step; 0 = free / n/a
    Kind                      kind;
    bool                      logSkew;      // slider uses a logarithmic skew
    bool                      perChannel;   // addressed with a channel index
    Domain                    domain;
    std::vector<const char*>  enumKeys;     // combo item LOC keys; size == span
};

//==============================================================================
inline const std::vector<UiDescriptor>& allDescriptors()
{
    namespace i = xoa::ids;
    static const std::vector<UiDescriptor> table = {
        // ---- Config (global) ------------------------------------------------
        { i::showName,         "param.showName",         "",             0.0,  Kind::text,   false, false, Domain::config,   {} },
        { i::masterGain,       "param.masterGain",       "units.db",     0.1,  Kind::slider, false, false, Domain::config,   {} },
        { i::oscEnabled,       "param.oscEnabled",       "",             0.0,  Kind::toggle, false, false, Domain::config,   {} },
        { i::oscReceivePort,   "param.oscReceivePort",   "",             1.0,  Kind::text,   false, false, Domain::config,   {} },
        { i::oscSendPort,      "param.oscSendPort",      "",             1.0,  Kind::text,   false, false, Domain::config,   {} },
        { i::oscSendAddress,   "param.oscSendAddress",   "",             0.0,  Kind::text,   false, false, Domain::config,   {} },
        { i::oscTcpEnabled,    "param.oscTcpEnabled",    "",             0.0,  Kind::toggle, false, false, Domain::config,   {} },
        { i::oscTcpPort,       "param.oscTcpPort",       "",             1.0,  Kind::text,   false, false, Domain::config,   {} },
        { i::oscAcceptAnyHost, "param.oscAcceptAnyHost", "",             0.0,  Kind::toggle, false, false, Domain::config,   {} },
        { i::oscFeedbackEnabled,"param.oscFeedbackEnabled","",           0.0,  Kind::toggle, false, false, Domain::config,   {} },
        { i::oscMeterEnabled,  "param.oscMeterEnabled",  "",             0.0,  Kind::toggle, false, false, Domain::config,   {} },
        { i::audioDeviceState, "param.audioDeviceState", "",             0.0,  Kind::system, false, false, Domain::config,   {} },
        { i::rotationYaw,      "param.rotationYaw",      "units.degrees",1.0,  Kind::dial,   false, false, Domain::config,   {} },
        { i::rotationPitch,    "param.rotationPitch",    "units.degrees",1.0,  Kind::dial,   false, false, Domain::config,   {} },
        { i::rotationRoll,     "param.rotationRoll",     "units.degrees",1.0,  Kind::dial,   false, false, Domain::config,   {} },
        { i::playbackFilePath, "param.playbackFilePath", "",             0.0,  Kind::system, false, false, Domain::config,   {} },
        { i::playbackLoop,     "param.playbackLoop",     "",             0.0,  Kind::toggle, false, false, Domain::config,   {} },
        { i::playbackContentOrder, "param.playbackContentOrder", "",     1.0,  Kind::combo,  false, false, Domain::config,
            { "enum.contentOrder.auto", "1", "2", "3", "4", "5", "6", "7", "8", "9", "10" } },
        { i::playbackConvention, "param.playbackConvention", "",         1.0,  Kind::combo,  false, false, Domain::config,
            { "enum.convention.sn3d", "enum.convention.n3d", "enum.convention.fuma" } },
        { i::distanceCompMode, "param.distanceCompMode", "",             1.0,  Kind::combo,  false, false, Domain::config,
            { "enum.distanceComp.off", "enum.distanceComp.delay", "enum.distanceComp.delayGain" } },
        { i::listenerX,        "param.listenerX",        "units.meters", 0.01, Kind::slider, false, false, Domain::config,   {} },
        { i::listenerY,        "param.listenerY",        "units.meters", 0.01, Kind::slider, false, false, Domain::config,   {} },
        { i::listenerZ,        "param.listenerZ",        "units.meters", 0.01, Kind::slider, false, false, Domain::config,   {} },
        { i::monoInputsEnabled,"param.monoInputsEnabled","",             0.0,  Kind::toggle, false, false, Domain::config,   {} },
        { i::inputCount,       "param.inputCount",       "",             1.0,  Kind::text,   false, false, Domain::inputs,   {} },
        { i::speakerCount,     "param.speakerCount",     "",             1.0,  Kind::text,   false, false, Domain::speakers, {} },

        // ---- Inputs (per channel) ------------------------------------------
        { i::inputName,        "param.inputName",        "",             0.0,  Kind::text,   false, true,  Domain::inputs,   {} },
        { i::inputGain,        "param.inputGain",        "units.db",     0.1,  Kind::slider, false, true,  Domain::inputs,   {} },
        { i::inputMute,        "param.inputMute",        "",             0.0,  Kind::toggle, false, true,  Domain::inputs,   {} },
        { i::inputPositionX,   "param.inputPositionX",   "units.meters", 0.01, Kind::slider, false, true,  Domain::inputs,   {} },
        { i::inputPositionY,   "param.inputPositionY",   "units.meters", 0.01, Kind::slider, false, true,  Domain::inputs,   {} },
        { i::inputPositionZ,   "param.inputPositionZ",   "units.meters", 0.01, Kind::slider, false, true,  Domain::inputs,   {} },
        { i::inputCoordinateMode, "param.inputCoordinateMode", "",       1.0,  Kind::combo,  false, true,  Domain::inputs,
            { "enum.coordMode.cartesian", "enum.coordMode.cylindrical", "enum.coordMode.spherical" } },
        { i::inputMaxSpeed,    "param.inputMaxSpeed",    "units.mps",    0.1,  Kind::slider, false, true,  Domain::inputs,   {} },
        { i::inputTrackingSmooth, "param.inputTrackingSmooth", "units.percent", 1.0, Kind::slider, false, true, Domain::inputs, {} },
        { i::inputSpread,      "param.inputSpread",      "units.degrees",1.0,  Kind::dial,   false, true,  Domain::inputs,   {} },
        { i::inputNfcEnabled,  "param.inputNfcEnabled",  "",             0.0,  Kind::toggle, false, true,  Domain::inputs,   {} },

        // ---- Speakers (per channel) ----------------------------------------
        { i::speakerName,      "param.speakerName",      "",             0.0,  Kind::text,   false, true,  Domain::speakers, {} },
        { i::speakerGain,      "param.speakerGain",      "units.db",     0.1,  Kind::slider, false, true,  Domain::speakers, {} },
        { i::speakerDelay,     "param.speakerDelay",     "units.ms",     0.1,  Kind::slider, false, true,  Domain::speakers, {} },
        { i::speakerMute,      "param.speakerMute",      "",             0.0,  Kind::toggle, false, true,  Domain::speakers, {} },
        { i::speakerSolo,      "param.speakerSolo",      "",             0.0,  Kind::toggle, false, true,  Domain::speakers, {} },
        { i::speakerPositionX, "param.speakerPositionX", "units.meters", 0.01, Kind::slider, false, true,  Domain::speakers, {} },
        { i::speakerPositionY, "param.speakerPositionY", "units.meters", 0.01, Kind::slider, false, true,  Domain::speakers, {} },
        { i::speakerPositionZ, "param.speakerPositionZ", "units.meters", 0.01, Kind::slider, false, true,  Domain::speakers, {} },
        { i::speakerCoordinateMode, "param.speakerCoordinateMode", "",   1.0,  Kind::combo,  false, true,  Domain::speakers,
            { "enum.coordMode.cartesian", "enum.coordMode.cylindrical", "enum.coordMode.spherical" } },
        { i::speakerEqEnabled, "param.speakerEqEnabled", "",             0.0,  Kind::toggle, false, true,  Domain::speakers, {} },

        // ---- Speaker EQ bands (two-index; bound via bindEqBand) -------------
        { i::eqShape,          "param.eqShape",          "",             1.0,  Kind::combo,  false, true,  Domain::speakers,
            { "enum.eqShape.off", "enum.eqShape.lowCut", "enum.eqShape.lowShelf", "enum.eqShape.peak",
              "enum.eqShape.bandPass", "enum.eqShape.highShelf", "enum.eqShape.highCut", "enum.eqShape.allPass" } },
        { i::eqFrequency,      "param.eqFrequency",      "units.hz",     1.0,  Kind::slider, true,  true,  Domain::speakers, {} },
        { i::eqGain,           "param.eqGain",           "units.db",     0.1,  Kind::slider, false, true,  Domain::speakers, {} },
        { i::eqQ,              "param.eqQ",              "",             0.01, Kind::slider, false, true,  Domain::speakers, {} },
        { i::eqSlope,          "param.eqSlope",          "",             0.01, Kind::slider, false, true,  Domain::speakers, {} },

        // ---- Decoder --------------------------------------------------------
        { i::decoderType,      "param.decoderType",      "",             1.0,  Kind::combo,  false, false, Domain::decoder,
            { "enum.decoderType.sad", "enum.decoderType.modeMatch", "enum.decoderType.allRad" } },
        { i::decoderWeighting, "param.decoderWeighting", "",             1.0,  Kind::combo,  false, false, Domain::decoder,
            { "enum.decoderWeighting.basic", "enum.decoderWeighting.maxRe" } },
        { i::decoderDualBandEnabled, "param.decoderDualBandEnabled", "", 0.0,  Kind::toggle, false, false, Domain::decoder,  {} },
        { i::decoderCrossoverFrequency, "param.decoderCrossoverFrequency", "units.hz", 1.0, Kind::slider, true, false, Domain::decoder, {} },
        { i::decoderNormalization, "param.decoderNormalization", "",     1.0,  Kind::combo,  false, false, Domain::decoder,
            { "enum.decoderNormalization.amplitude", "enum.decoderNormalization.energy" } },
    };
    return table;
}

/** Descriptor for a parameter id, or nullptr if none. */
inline const UiDescriptor* findDescriptor (const juce::Identifier& id)
{
    for (const auto& d : allDescriptors())
        if (d.id == id)
            return &d;
    return nullptr;
}

} // namespace xoa::ui
