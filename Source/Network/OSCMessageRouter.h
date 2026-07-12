#pragma once

#include <juce_core/juce_core.h>
#include <juce_osc/juce_osc.h>

#include <cmath>
#include <vector>

#include "Parameters/XoaParameterIDs.h"

//==============================================================================
// XOA - OSC message router (WP9 C3). Pure, static, no sockets and no store: it
// maps the frozen /xoa address scheme (Documentation/XOA-OSC-MAP.md) onto the
// XoaValueTreeState parameter ids and back, and nothing else. OSCManager (C4)
// owns the transports and the store; it parses raw bytes to juce::OSCMessage
// via spatcore's OSCParser, hands each message here, and applies the result.
//
// The `bindings()` table is the SINGLE SOURCE OF TRUTH for the address<->id
// mapping - both inbound parsing and outbound feedback/read-back read it, so a
// new parameter is exposed over OSC by adding exactly one row.
//
// Two inbound forms are accepted (see the map doc):
//   write form:    /xoa/<family>/<leaf>            (i)ch (T)v   [channelized]
//                  /xoa/<family>/<leaf>            (T)v          [non-chan]
//   indexed form:  /xoa/<family>/<n>/<leaf>        (T)v
//                  /xoa/speaker/<n>/eq/<b>/<leaf>  (T)v          [EQ: indexed only]
// Feedback / read-back is always emitted in the indexed form.
//==============================================================================

namespace xoa::osc
{

enum class Family { unknown, input, speaker, speakerEq, decoder, config, rotation, listener };

// The leaf's wire type: drives tolerant coercion (int32<->float32) and the
// store-var type produced, matching what the schema stores for that id.
enum class WireType { f32, i32, boolean, str };

struct Binding
{
    Family           family;
    const char*      leaf;         // trailing address segment, e.g. "gain"
    juce::Identifier id;           // store parameter id
    WireType         type;
    bool             channelized;  // family carries a per-channel index
};

//==============================================================================
inline const std::vector<Binding>& bindings()
{
    namespace i = xoa::ids;
    static const std::vector<Binding> table = {
        // input (channelized)
        { Family::input, "gain",           i::inputGain,           WireType::f32,     true },
        { Family::input, "mute",           i::inputMute,           WireType::boolean, true },
        { Family::input, "name",           i::inputName,           WireType::str,     true },
        { Family::input, "positionX",      i::inputPositionX,      WireType::f32,     true },
        { Family::input, "positionY",      i::inputPositionY,      WireType::f32,     true },
        { Family::input, "positionZ",      i::inputPositionZ,      WireType::f32,     true },
        { Family::input, "coordinateMode", i::inputCoordinateMode, WireType::i32,     true },
        { Family::input, "maxSpeed",       i::inputMaxSpeed,       WireType::f32,     true },
        { Family::input, "trackingSmooth", i::inputTrackingSmooth, WireType::f32,     true },
        { Family::input, "spread",         i::inputSpread,         WireType::f32,     true },
        { Family::input, "nfcEnabled",     i::inputNfcEnabled,     WireType::boolean, true },
        // speaker (channelized)
        { Family::speaker, "gain",           i::speakerGain,           WireType::f32,     true },
        { Family::speaker, "delay",          i::speakerDelay,          WireType::f32,     true },
        { Family::speaker, "mute",           i::speakerMute,           WireType::boolean, true },
        { Family::speaker, "solo",           i::speakerSolo,           WireType::boolean, true },
        { Family::speaker, "name",           i::speakerName,           WireType::str,     true },
        { Family::speaker, "positionX",      i::speakerPositionX,      WireType::f32,     true },
        { Family::speaker, "positionY",      i::speakerPositionY,      WireType::f32,     true },
        { Family::speaker, "positionZ",      i::speakerPositionZ,      WireType::f32,     true },
        { Family::speaker, "coordinateMode", i::speakerCoordinateMode, WireType::i32,     true },
        { Family::speaker, "eqEnabled",      i::speakerEqEnabled,      WireType::boolean, true },
        // speaker EQ band (double-indexed: /xoa/speaker/<n>/eq/<b>/<leaf>)
        { Family::speakerEq, "shape",     i::eqShape,     WireType::i32, true },
        { Family::speakerEq, "frequency", i::eqFrequency, WireType::f32, true },
        { Family::speakerEq, "gain",      i::eqGain,      WireType::f32, true },
        { Family::speakerEq, "q",         i::eqQ,         WireType::f32, true },
        { Family::speakerEq, "slope",     i::eqSlope,     WireType::f32, true },
        // decoder
        { Family::decoder, "type",               i::decoderType,               WireType::i32,     false },
        { Family::decoder, "weighting",          i::decoderWeighting,          WireType::i32,     false },
        { Family::decoder, "dualBandEnabled",    i::decoderDualBandEnabled,    WireType::boolean, false },
        { Family::decoder, "crossoverFrequency", i::decoderCrossoverFrequency, WireType::f32,     false },
        { Family::decoder, "normalization",      i::decoderNormalization,      WireType::i32,     false },
        // config
        { Family::config, "masterGain",           i::masterGain,           WireType::f32,     false },
        { Family::config, "distanceCompMode",     i::distanceCompMode,     WireType::i32,     false },
        { Family::config, "monoInputsEnabled",    i::monoInputsEnabled,    WireType::boolean, false },
        { Family::config, "playbackLoop",         i::playbackLoop,         WireType::boolean, false },
        { Family::config, "playbackContentOrder", i::playbackContentOrder, WireType::i32,     false },
        { Family::config, "playbackConvention",   i::playbackConvention,   WireType::i32,     false },
        { Family::config, "inputCount",           i::inputCount,           WireType::i32,     false },
        { Family::config, "speakerCount",         i::speakerCount,         WireType::i32,     false },
        // rotation
        { Family::rotation, "yaw",   i::rotationYaw,   WireType::f32, false },
        { Family::rotation, "pitch", i::rotationPitch, WireType::f32, false },
        { Family::rotation, "roll",  i::rotationRoll,  WireType::f32, false },
        // listener (D18)
        { Family::listener, "x", i::listenerX, WireType::f32, false },
        { Family::listener, "y", i::listenerY, WireType::f32, false },
        { Family::listener, "z", i::listenerZ, WireType::f32, false },
    };
    return table;
}

inline const char* familyToken (Family f) noexcept
{
    switch (f)
    {
        case Family::input:     return "input";
        case Family::speaker:   return "speaker";
        case Family::speakerEq: return "speaker";   // EQ lives under /speaker/<n>/eq/<b>/
        case Family::decoder:   return "decoder";
        case Family::config:    return "config";
        case Family::rotation:  return "rotation";
        case Family::listener:  return "listener";
        case Family::unknown:   break;
    }
    return "";
}

inline const Binding* findBinding (Family fam, const juce::String& leaf) noexcept
{
    for (const auto& b : bindings())
        if (b.family == fam && leaf == b.leaf)
            return &b;
    return nullptr;
}

inline const Binding* findBindingById (const juce::Identifier& id) noexcept
{
    for (const auto& b : bindings())
        if (b.id == id)
            return &b;
    return nullptr;
}

inline bool isAllDigits (const juce::String& s) noexcept
{
    if (s.isEmpty())
        return false;
    for (int c = 0; c < s.length(); ++c)
        if (! juce::CharacterFunctions::isDigit (s[c]))
            return false;
    return true;
}

//==============================================================================
// Address resolution: maps an address pattern to a binding plus any
// path-derived channel/band. It does NOT touch OSC arguments (so it also
// serves /xoa/get, where the address is a string argument). channelInPath is
// true only for the indexed form; the write form leaves channelIndex == -1 and
// the caller reads the channel from the message's first int argument.
struct Resolved
{
    Family           family = Family::unknown;
    juce::Identifier id;
    WireType         type = WireType::f32;
    int              channelIndex = -1;   // 0-based (-1 = none / not in path)
    int              bandIndex = -1;      // 0-based (-1 = not an EQ band)
    bool             channelInPath = false;
    bool             valid = false;
    juce::String     reason;
};

inline Resolved resolveAddress (const juce::String& address)
{
    Resolved r;
    juce::StringArray segs;
    segs.addTokens (address, "/", "");
    segs.removeEmptyStrings();

    if (segs.size() < 2 || segs[0] != "xoa")
    {
        r.reason = "not an /xoa address";
        return r;
    }

    auto finish = [&] (Family family, const juce::String& leaf) -> Resolved
    {
        if (const Binding* b = findBinding (family, leaf))
        {
            r.family = family;
            r.id = b->id;
            r.type = b->type;
            r.valid = true;
        }
        else
        {
            r.reason = "unknown parameter '" + leaf + "'";
        }
        return r;
    };

    const juce::String fam = segs[1];

    if (fam == "input" || fam == "speaker")
    {
        const Family family = (fam == "input") ? Family::input : Family::speaker;

        if (segs.size() == 3)                                        // write form
            return finish (family, segs[2]);

        if (segs.size() == 4 && isAllDigits (segs[2]))               // indexed form
        {
            r.channelIndex = segs[2].getIntValue() - 1;
            r.channelInPath = true;
            if (r.channelIndex < 0) { r.reason = "channel must be >= 1"; return r; }
            return finish (family, segs[3]);
        }

        if (family == Family::speaker && segs.size() == 6            // EQ indexed form
            && isAllDigits (segs[2]) && segs[3] == "eq" && isAllDigits (segs[4]))
        {
            r.channelIndex = segs[2].getIntValue() - 1;
            r.bandIndex    = segs[4].getIntValue() - 1;
            r.channelInPath = true;
            if (r.channelIndex < 0 || r.bandIndex < 0) { r.reason = "index must be >= 1"; return r; }
            return finish (Family::speakerEq, segs[5]);
        }

        r.reason = "malformed " + fam + " address";
        return r;
    }

    if (fam == "decoder" || fam == "config" || fam == "rotation" || fam == "listener")
    {
        const Family family = fam == "decoder"  ? Family::decoder
                            : fam == "config"   ? Family::config
                            : fam == "rotation" ? Family::rotation
                                                : Family::listener;
        if (segs.size() == 3)
            return finish (family, segs[2]);

        r.reason = "malformed " + fam + " address";
        return r;
    }

    r.reason = "unknown family '" + fam + "'";
    return r;
}

//==============================================================================
// Argument helpers.

inline bool hasOnlyFiniteFloats (const juce::OSCMessage& msg, juce::String& reason)
{
    for (int i = 0; i < msg.size(); ++i)
        if (msg[i].isFloat32() && ! std::isfinite (msg[i].getFloat32()))
        {
            reason = "non-finite float argument";
            return false;
        }
    return true;
}

// int32 or float32 -> double (tolerant). False if the arg is neither.
inline bool argToNumber (const juce::OSCArgument& a, double& out) noexcept
{
    if (a.isFloat32()) { out = (double) a.getFloat32(); return true; }
    if (a.isInt32())   { out = (double) a.getInt32();   return true; }
    return false;
}

// Coerce one argument to the store-typed var for `type`. False on type mismatch.
inline bool coerceValue (const juce::OSCArgument& a, WireType type,
                         juce::var& out, juce::String& reason)
{
    if (type == WireType::str)
    {
        if (! a.isString()) { reason = "expected a string argument"; return false; }
        out = a.getString();
        return true;
    }

    double v = 0.0;
    if (! argToNumber (a, v)) { reason = "expected a numeric argument"; return false; }

    switch (type)
    {
        case WireType::f32:     out = v;                          break;   // schema stores double
        case WireType::i32:     out = (int) std::lround (v);      break;
        case WireType::boolean: out = (v != 0.0);                 break;
        case WireType::str:                                       break;   // handled above
    }
    return true;
}

//==============================================================================
// Inbound parameter parse (both forms). channelIndex/bandIndex are 0-based;
// channelIndex == -1 means non-channelized. On failure valid == false and
// `reason` explains why (unknown address, missing/typed-wrong value, NaN).
struct ParsedParam
{
    juce::Identifier id;
    int          channelIndex = -1;
    int          bandIndex = -1;
    juce::var    value;
    bool         valid = false;
    juce::String reason;
};

inline ParsedParam parseParameterMessage (const juce::OSCMessage& msg)
{
    ParsedParam p;
    juce::String reason;

    if (! hasOnlyFiniteFloats (msg, reason)) { p.reason = reason; return p; }

    const Resolved r = resolveAddress (msg.getAddressPattern().toString());
    if (! r.valid) { p.reason = r.reason; return p; }

    p.id = r.id;
    p.bandIndex = r.bandIndex;

    const Binding* b = findBindingById (r.id);
    const bool channelized = (b != nullptr && b->channelized);

    int valueArg = 0;
    if (channelized && ! r.channelInPath)
    {
        // Write form: the first argument is the 1-based channel.
        double ch = 0.0;
        if (msg.size() < 1 || ! argToNumber (msg[0], ch))
        { p.reason = "channelized write needs a channel argument"; return p; }
        p.channelIndex = (int) std::lround (ch) - 1;
        if (p.channelIndex < 0) { p.reason = "channel must be >= 1"; return p; }
        valueArg = 1;
    }
    else
    {
        p.channelIndex = r.channelIndex;   // from the path, or -1 for non-channelized
    }

    if (msg.size() <= valueArg) { p.reason = "missing value argument"; return p; }
    if (! coerceValue (msg[valueArg], r.type, p.value, reason)) { p.reason = reason; return p; }

    p.valid = true;
    return p;
}

//==============================================================================
// Outbound / read-back builders (indexed form).

inline juce::String indexedAddress (const juce::Identifier& id, int channelIndex, int bandIndex = -1)
{
    const Binding* b = findBindingById (id);
    if (b == nullptr)
        return {};

    juce::String a = "/xoa/";
    a << familyToken (b->family);

    if (b->family == Family::speakerEq)
        a << "/" << (channelIndex + 1) << "/eq/" << (bandIndex + 1) << "/" << b->leaf;
    else if (b->channelized)
        a << "/" << (channelIndex + 1) << "/" << b->leaf;
    else
        a << "/" << b->leaf;

    return a;
}

inline juce::OSCMessage makeValueMessage (const juce::String& address, WireType type,
                                          const juce::var& value)
{
    switch (type)
    {
        case WireType::f32:     return juce::OSCMessage (address, (float) (double) value);
        case WireType::i32:     return juce::OSCMessage (address, (juce::int32) (int) value);
        case WireType::boolean: return juce::OSCMessage (address, (juce::int32) ((bool) value ? 1 : 0));
        case WireType::str:     return juce::OSCMessage (address, value.toString());
    }
    return juce::OSCMessage (address);
}

// Build the read-back message for a parameter from its live store var value.
inline juce::OSCMessage makeFeedbackMessage (const juce::Identifier& id, int channelIndex,
                                             int bandIndex, const juce::var& value)
{
    const Binding* b = findBindingById (id);
    const juce::String addr = indexedAddress (id, channelIndex, bandIndex);
    return makeValueMessage (addr, b != nullptr ? b->type : WireType::f32, value);
}

} // namespace xoa::osc
