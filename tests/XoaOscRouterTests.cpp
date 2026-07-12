/*
    XoaOscRouterTests.cpp - WP9 C3: the pure OSC message router.

    The router (Source/Network/OSCMessageRouter.h) is sockets-free and
    store-free, so it is exhaustively testable in isolation: every binding
    round-trips through both address forms, coercion is tolerant, and malformed
    traffic is rejected with valid == false.
*/

#include "XoaTestFramework.h"

#include "Network/OSCMessageRouter.h"
#include "Parameters/XoaParameterIDs.h"

#include <juce_osc/juce_osc.h>

#include <cmath>
#include <limits>

namespace
{

namespace osc = xoa::osc;
using juce::OSCMessage;

// A valid value argument matching a binding's wire type (channel prefix added
// separately by the callers that build the write form).
OSCMessage writeForm (const osc::Binding& b, int channel1Based)
{
    const juce::String addr = juce::String ("/xoa/") + osc::familyToken (b.family) + "/" + b.leaf;
    switch (b.type)
    {
        case osc::WireType::f32:     return OSCMessage (addr, (juce::int32) channel1Based, 1.5f);
        case osc::WireType::i32:     return OSCMessage (addr, (juce::int32) channel1Based, (juce::int32) 1);
        case osc::WireType::boolean: return OSCMessage (addr, (juce::int32) channel1Based, (juce::int32) 1);
        case osc::WireType::str:     return OSCMessage (addr, (juce::int32) channel1Based, juce::String ("x"));
    }
    return OSCMessage (addr);
}

OSCMessage plainForm (const osc::Binding& b)
{
    const juce::String addr = juce::String ("/xoa/") + osc::familyToken (b.family) + "/" + b.leaf;
    switch (b.type)
    {
        case osc::WireType::f32:     return OSCMessage (addr, 1.5f);
        case osc::WireType::i32:     return OSCMessage (addr, (juce::int32) 1);
        case osc::WireType::boolean: return OSCMessage (addr, (juce::int32) 1);
        case osc::WireType::str:     return OSCMessage (addr, juce::String ("x"));
    }
    return OSCMessage (addr);
}

// Build a message at a literal address carrying a value of `type`.
OSCMessage valueAt (const juce::String& address, osc::WireType type)
{
    switch (type)
    {
        case osc::WireType::f32:     return OSCMessage (address, 1.5f);
        case osc::WireType::i32:     return OSCMessage (address, (juce::int32) 1);
        case osc::WireType::boolean: return OSCMessage (address, (juce::int32) 1);
        case osc::WireType::str:     return OSCMessage (address, juce::String ("x"));
    }
    return OSCMessage (address);
}

//==============================================================================
// Every binding is reachable and resolves to its own id, in whatever form(s)
// the map doc permits it. This is the completeness gate: a new row that forgot
// to wire an address fails here.
void testRouterEveryBindingRoundTrips()
{
    for (const auto& b : osc::bindings())
    {
        if (b.family == osc::Family::speakerEq)
        {
            // EQ is indexed-only: /xoa/speaker/<n>/eq/<b>/<leaf>
            const juce::String addr = juce::String ("/xoa/speaker/2/eq/3/") + b.leaf;
            const auto p = osc::parseParameterMessage (valueAt (addr, b.type));
            CHECK (p.valid);
            CHECK (p.id == b.id);
            CHECK (p.channelIndex == 1);
            CHECK (p.bandIndex == 2);
        }
        else if (b.channelized)
        {
            const auto pw = osc::parseParameterMessage (writeForm (b, 4));
            CHECK (pw.valid);
            CHECK (pw.id == b.id);
            CHECK (pw.channelIndex == 3);
            CHECK (pw.bandIndex == -1);

            const juce::String idxAddr = juce::String ("/xoa/") + osc::familyToken (b.family) + "/4/" + b.leaf;
            const auto pi = osc::parseParameterMessage (valueAt (idxAddr, b.type));
            CHECK (pi.valid);
            CHECK (pi.id == b.id);
            CHECK (pi.channelIndex == 3);
        }
        else
        {
            const auto p = osc::parseParameterMessage (plainForm (b));
            CHECK (p.valid);
            CHECK (p.id == b.id);
            CHECK (p.channelIndex == -1);
        }
    }
}

//==============================================================================
// indexedAddress() output must parse back to the same id / channel / band -
// the property that makes feedback and /xoa/get replies self-consistent.
void testRouterIndexedAddressRoundTrips()
{
    for (const auto& b : osc::bindings())
    {
        const int ch = (b.channelized || b.family == osc::Family::speakerEq) ? 2 : -1;
        const int bd = (b.family == osc::Family::speakerEq) ? 4 : -1;

        const juce::String addr = osc::indexedAddress (b.id, ch, bd);
        CHECK (addr.isNotEmpty());

        const auto p = osc::parseParameterMessage (valueAt (addr, b.type));
        CHECK (p.valid);
        CHECK (p.id == b.id);
        if (b.channelized || b.family == osc::Family::speakerEq)
            CHECK (p.channelIndex == 2);
        else
            CHECK (p.channelIndex == -1);
        if (b.family == osc::Family::speakerEq)
            CHECK (p.bandIndex == 4);
    }
}

//==============================================================================
void testRouterCoercion()
{
    // int32 where a float leaf is expected -> tolerant, stored as double.
    {
        OSCMessage m ("/xoa/config/masterGain", (juce::int32) -3);
        const auto p = osc::parseParameterMessage (m);
        CHECK (p.valid);
        CHECK (std::abs ((double) p.value + 3.0) < 1.0e-9);
    }
    // float where an int leaf is expected -> rounds to int.
    {
        OSCMessage m ("/xoa/decoder/type", 2.0f);
        const auto p = osc::parseParameterMessage (m);
        CHECK (p.valid);
        CHECK ((int) p.value == 2);
    }
    // boolean leaf from int 1.
    {
        OSCMessage m ("/xoa/speaker/1/mute", (juce::int32) 1);
        const auto p = osc::parseParameterMessage (m);
        CHECK (p.valid);
        CHECK ((bool) p.value == true);
        CHECK (p.channelIndex == 0);
    }
    // string leaf, indexed form.
    {
        OSCMessage m ("/xoa/input/2/name", juce::String ("Lead"));
        const auto p = osc::parseParameterMessage (m);
        CHECK (p.valid);
        CHECK (p.value.toString() == "Lead");
        CHECK (p.channelIndex == 1);
    }
    // string leaf, write form (channel then string).
    {
        OSCMessage m ("/xoa/speaker/name", (juce::int32) 3, juce::String ("Sub"));
        const auto p = osc::parseParameterMessage (m);
        CHECK (p.valid);
        CHECK (p.id == xoa::ids::speakerName);
        CHECK (p.channelIndex == 2);
        CHECK (p.value.toString() == "Sub");
    }
}

//==============================================================================
void testRouterRejections()
{
    auto rejects = [] (const OSCMessage& m) { return ! osc::parseParameterMessage (m).valid; };

    // Non-finite float anywhere in the message.
    CHECK (rejects (OSCMessage ("/xoa/config/masterGain", std::numeric_limits<float>::quiet_NaN())));
    CHECK (rejects (OSCMessage ("/xoa/config/masterGain", std::numeric_limits<float>::infinity())));

    // Unknown leaf / family / non-xoa root.
    CHECK (rejects (OSCMessage ("/xoa/config/bogus", 1.0f)));
    CHECK (rejects (OSCMessage ("/xoa/bogus/thing", 1.0f)));
    CHECK (rejects (OSCMessage ("/other/thing", 1.0f)));

    // Transport params are read-only over OSC: not in the table -> rejected.
    CHECK (rejects (OSCMessage ("/xoa/config/oscReceivePort", (juce::int32) 9100)));

    // Channelized write missing its value (channel only).
    CHECK (rejects (OSCMessage ("/xoa/input/gain", (juce::int32) 3)));

    // Wrong value type: string where a number is expected.
    CHECK (rejects (OSCMessage ("/xoa/config/masterGain", juce::String ("loud"))));

    // Bad channel index (0 is out of the 1-based range).
    CHECK (rejects (OSCMessage ("/xoa/input/0/gain", -6.0f)));

    // EQ in write form is not allowed (indexed only).
    CHECK (rejects (OSCMessage ("/xoa/speaker/eq/frequency", (juce::int32) 1, 1000.0f)));
}

//==============================================================================
void testRouterFeedbackBuilder()
{
    // Float parameter feedback: indexed address + float arg.
    {
        const auto m = osc::makeFeedbackMessage (xoa::ids::inputGain, 2, -1, juce::var (-6.0));
        CHECK (m.getAddressPattern().toString() == "/xoa/input/3/gain");
        CHECK (m.size() == 1);
        CHECK (m[0].isFloat32());
        CHECK (std::abs (m[0].getFloat32() + 6.0f) < 1.0e-5f);
    }
    // Non-channelized parameter ignores the channel argument.
    {
        const auto addr = osc::indexedAddress (xoa::ids::masterGain, -1);
        CHECK (addr == "/xoa/config/masterGain");
    }
    // EQ band feedback address.
    {
        const auto addr = osc::indexedAddress (xoa::ids::eqFrequency, 1, 2);
        CHECK (addr == "/xoa/speaker/2/eq/3/frequency");
    }
    // Boolean feedback emits int 0/1.
    {
        const auto m = osc::makeFeedbackMessage (xoa::ids::speakerMute, 0, -1, juce::var (true));
        CHECK (m.getAddressPattern().toString() == "/xoa/speaker/1/mute");
        CHECK (m[0].isInt32());
        CHECK (m[0].getInt32() == 1);
    }
}

} // namespace

//==============================================================================
void runXoaOscRouterTests()
{
    testRouterEveryBindingRoundTrips();
    testRouterIndexedAddressRoundTrips();
    testRouterCoercion();
    testRouterRejections();
    testRouterFeedbackBuilder();
}
