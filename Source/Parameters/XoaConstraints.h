#pragma once

#include <juce_core/juce_core.h>

#include <cmath>
#include <cstdlib>
#include <optional>
#include <vector>

#include "XoaParameterIDs.h"
#include "XoaParameterDefaults.h"

//==============================================================================
// XOA — parameter bounds: one table, two gates.
//
//  Gate 1  clampToBounds()         -> TreeParameterStore::setWriteInterceptor
//          Live writes. Cannot reject; in-range values are returned as the
//          SAME var object (byte-identical results for validated callers).
//  Gate 2  validateLoadedProperty() -> XmlPersistence::Options::propertyValidator
//          File-load merges only. Values arrive as STRING vars from XML —
//          parse-then-check; in-range returns the original string var
//          unchanged (preserves save->load->save byte stability); rejected
//          (nullopt) means the target keeps its current/default value.
//
// The table is a function-local static (immune to static-init order against
// the inline Identifiers) and is scanned linearly — ~30 entries, trivial.
//==============================================================================

namespace xoa::constraints
{

struct Bounds
{
    double min, max, defaultValue;
    bool isInt;
};

inline const std::vector<std::pair<juce::Identifier, Bounds>>& allBounds()
{
    namespace d = xoa::defaults;
    static const std::vector<std::pair<juce::Identifier, Bounds>> table = {
        // Config
        { ids::masterGain,     { d::masterGainMin, d::masterGainMax, d::masterGainDefault, false } },
        { ids::oscReceivePort, { d::oscPortMin, d::oscPortMax, d::oscReceivePortDefault, true } },
        { ids::oscSendPort,    { d::oscPortMin, d::oscPortMax, d::oscSendPortDefault, true } },

        // Config / scene rotation + playback (playbackLoop is bool and
        // playbackFilePath is string -> unbounded by convention)
        { ids::rotationYaw,          { d::rotationYawMin, d::rotationYawMax, d::rotationYawDefault, false } },
        { ids::rotationPitch,        { d::rotationPitchMin, d::rotationPitchMax, d::rotationPitchDefault, false } },
        { ids::rotationRoll,         { d::rotationRollMin, d::rotationRollMax, d::rotationRollDefault, false } },
        { ids::playbackContentOrder, { d::playbackContentOrderMin, d::playbackContentOrderMax, d::playbackContentOrderDefault, true } },
        { ids::playbackConvention,   { d::playbackConventionMin, d::playbackConventionMax, d::playbackConventionDefault, true } },
        { ids::distanceCompMode,     { d::distanceCompModeMin, d::distanceCompModeMax, d::distanceCompModeDefault, true } },
        { ids::listenerX,            { d::positionMin, d::positionMax, d::listenerXDefault, false } },
        { ids::listenerY,            { d::positionMin, d::positionMax, d::listenerYDefault, false } },
        { ids::listenerZ,            { d::positionMin, d::positionMax, d::listenerZDefault, false } },

        // Structural counts
        { ids::inputCount,   { 1.0, (double) xoa::kMaxInputs,   (double) xoa::kDefaultInputs,   true } },
        { ids::speakerCount, { 1.0, (double) xoa::kMaxSpeakers, (double) xoa::kDefaultSpeakers, true } },

        // Inputs
        { ids::inputGain,           { d::inputGainMin, d::inputGainMax, d::inputGainDefault, false } },
        { ids::inputPositionX,      { d::positionMin, d::positionMax, d::inputPositionXDefault, false } },
        { ids::inputPositionY,      { d::positionMin, d::positionMax, d::inputPositionYDefault, false } },
        { ids::inputPositionZ,      { d::positionMin, d::positionMax, d::inputPositionZDefault, false } },
        { ids::inputCoordinateMode, { d::coordinateModeMin, d::coordinateModeMax, d::coordinateModeDefault, true } },
        { ids::inputSpread,         { d::inputSpreadMin, d::inputSpreadMax, d::inputSpreadDefault, false } },
        { ids::inputMaxSpeed,       { d::inputMaxSpeedMin, d::inputMaxSpeedMax, d::inputMaxSpeedDefault, false } },
        { ids::inputTrackingSmooth, { d::inputTrackingSmoothMin, d::inputTrackingSmoothMax, d::inputTrackingSmoothDefault, false } },

        // Speakers
        { ids::speakerGain,           { d::speakerGainMin, d::speakerGainMax, d::speakerGainDefault, false } },
        { ids::speakerDelay,          { d::speakerDelayMin, d::speakerDelayMax, d::speakerDelayDefault, false } },
        { ids::speakerPositionX,      { d::positionMin, d::positionMax, 0.0, false } },
        { ids::speakerPositionY,      { d::positionMin, d::positionMax, 0.0, false } },
        { ids::speakerPositionZ,      { d::positionMin, d::positionMax, 0.0, false } },
        { ids::speakerCoordinateMode, { d::coordinateModeMin, d::coordinateModeMax, d::coordinateModeDefault, true } },

        // Speaker EQ bands (eqFrequency fallback default is the mid band —
        // only used for the non-finite live-write path; load rejection keeps
        // the per-band default already in the tree)
        { ids::eqShape,     { d::eqShapeMin, d::eqShapeMax, d::eqShapeDefault, true } },
        { ids::eqFrequency, { d::eqFrequencyMin, d::eqFrequencyMax, 1000.0, false } },
        { ids::eqGain,      { d::eqGainMin, d::eqGainMax, d::eqGainDefault, false } },
        { ids::eqQ,         { d::eqQMin, d::eqQMax, d::eqQDefault, false } },
        { ids::eqSlope,     { d::eqSlopeMin, d::eqSlopeMax, d::eqSlopeDefault, false } },

        // Decoder
        { ids::decoderType,               { d::decoderTypeMin, d::decoderTypeMax, d::decoderTypeDefault, true } },
        { ids::decoderWeighting,          { d::decoderWeightingMin, d::decoderWeightingMax, d::decoderWeightingDefault, true } },
        { ids::decoderCrossoverFrequency, { d::decoderCrossoverFrequencyMin, d::decoderCrossoverFrequencyMax, d::decoderCrossoverFrequencyDefault, false } },
        { ids::decoderNormalization,      { d::decoderNormalizationMin, d::decoderNormalizationMax, d::decoderNormalizationDefault, true } },
    };
    return table;
}

/** Bounds for a parameter, or nullptr for unbounded ones (strings, bools). */
inline const Bounds* findBounds (const juce::Identifier& id)
{
    for (const auto& entry : allBounds())
        if (entry.first == id)
            return &entry.second;
    return nullptr;
}

/** Gate 1 — live-write clamp (see header block). */
inline juce::var clampToBounds (const juce::Identifier& id, const juce::var& proposed)
{
    const auto* b = findBounds (id);
    if (b == nullptr)
        return proposed;

    const double value = static_cast<double> (proposed);
    if (! std::isfinite (value))
        return b->isInt ? juce::var (static_cast<int> (b->defaultValue))
                        : juce::var (b->defaultValue);

    if (value >= b->min && value <= b->max)
        return proposed;   // unchanged: byte-identical contract

    const double clamped = juce::jlimit (b->min, b->max, value);
    return b->isInt ? juce::var (static_cast<int> (std::lround (clamped)))
                    : juce::var (clamped);
}

/** Gate 2 — file-load merge validator (see header block). */
inline std::optional<juce::var> validateLoadedProperty (const juce::Identifier& id,
                                                        const juce::var& value)
{
    const auto* b = findBounds (id);
    if (b == nullptr)
        return value;   // unbounded parameters pass through verbatim

    // XML-loaded values are strings: parse strictly so garbage is rejected
    // rather than silently read as 0.
    const juce::String text = value.toString().trim();
    if (text.isEmpty())
        return std::nullopt;

    const char* raw = text.toRawUTF8();
    char* end = nullptr;
    const double parsed = std::strtod (raw, &end);
    if (end == raw || *end != '\0' || ! std::isfinite (parsed))
        return std::nullopt;   // unparseable / NaN / inf -> keep target value

    if (parsed >= b->min && parsed <= b->max)
        return value;   // original var unchanged -> byte-stable re-save

    const double clamped = juce::jlimit (b->min, b->max, parsed);
    return b->isInt ? juce::var (static_cast<int> (std::lround (clamped)))
                    : juce::var (clamped);
}

} // namespace xoa::constraints
