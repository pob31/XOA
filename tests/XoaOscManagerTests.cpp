/*
    XoaOscManagerTests.cpp - WP9 C4: the OSC manager inbound path.

    Exercises the manager without opening a socket: a message is serialised to
    bytes and fed through the test seam handleRawPacket(), which runs the same
    dispatch the ingest queue would on the message thread. Replies (/xoa/get,
    /xoa/ping) are captured through the injectable send hook.
*/

#include "XoaTestFramework.h"

#include "Audio/AudioEngine.h"
#include "DSP/AmbiRotation.h"
#include "Network/OSCManager.h"
#include "Parameters/XoaParameterIDs.h"
#include "Parameters/XoaValueTreeState.h"

#include "spatcore/control/osc/OSCSerializer.h"

#include <juce_osc/juce_osc.h>

#include <cmath>
#include <limits>
#include <vector>

namespace
{

namespace sc = spatcore::control::osc;
using juce::OSCMessage;

struct CapturedReply
{
    juce::String  ip;
    int           port = 0;
    OSCMessage    msg { juce::OSCAddressPattern ("/none") };
};

// store + engine + manager with reply capture and a serialise-and-inject helper.
struct ManagerFixture
{
    xoa::XoaValueTreeState store;
    xoa::AudioEngine       engine { store };
    xoa::OSCManager        mgr { store, engine };
    std::vector<CapturedReply> replies;

    ManagerFixture()
    {
        mgr.setSendFn ([this] (const juce::String& ip, int port, const OSCMessage& m)
        {
            replies.push_back ({ ip, port, m });
        });
    }

    void inject (const OSCMessage& m, const juce::String& ip = "127.0.0.1", int port = 5005)
    {
        const auto bytes = sc::OSCSerializer::serializeMessage (m);
        mgr.handleRawPacket (bytes.getData(), bytes.getSize(), ip, port);
    }
};

//==============================================================================
void testManagerParamWrites()
{
    ManagerFixture f;

    // Non-channelized.
    f.inject (OSCMessage ("/xoa/config/masterGain", -6.0f));
    CHECK (std::abs ((double) f.store.getFloatParameter (xoa::ids::masterGain) + 6.0) < 1.0e-4);

    f.store.setNumSpeakers (4);

    // Channelized write form: (i)channel (f)value, 1-based channel.
    f.inject (OSCMessage ("/xoa/speaker/gain", (juce::int32) 2, -3.0f));
    CHECK (std::abs ((double) f.store.getFloatParameter (xoa::ids::speakerGain, 1) + 3.0) < 1.0e-4);

    // Channelized indexed form.
    f.inject (OSCMessage ("/xoa/speaker/3/gain", -9.0f));
    CHECK (std::abs ((double) f.store.getFloatParameter (xoa::ids::speakerGain, 2) + 9.0) < 1.0e-4);

    // Rotation + listener (non-channelized floats).
    f.inject (OSCMessage ("/xoa/rotation/yaw", 45.0f));
    CHECK (std::abs ((double) f.store.getFloatParameter (xoa::ids::rotationYaw) - 45.0) < 1.0e-4);

    f.inject (OSCMessage ("/xoa/listener/y", 1.25f));
    CHECK (std::abs ((double) f.store.getFloatParameter (xoa::ids::listenerY) - 1.25) < 1.0e-4);

    // No replies for plain parameter writes.
    CHECK (f.replies.empty());
}

//==============================================================================
void testManagerEqWrite()
{
    ManagerFixture f;
    f.store.setNumSpeakers (2);

    f.inject (OSCMessage ("/xoa/speaker/1/eq/2/frequency", 1200.0f));
    CHECK (std::abs ((double) f.store.getEqBandParameter (0, 1, xoa::ids::eqFrequency) - 1200.0) < 1.0e-2);

    f.inject (OSCMessage ("/xoa/speaker/1/eq/2/gain", 4.0f));
    CHECK (std::abs ((double) f.store.getEqBandParameter (0, 1, xoa::ids::eqGain) - 4.0) < 1.0e-3);
}

//==============================================================================
void testManagerStructuralCount()
{
    ManagerFixture f;
    const int before = f.store.getNumSpeakers();
    f.inject (OSCMessage ("/xoa/config/speakerCount", (juce::int32) (before + 4)));
    CHECK (f.store.getNumSpeakers() == before + 4);
}

//==============================================================================
void testManagerRejections()
{
    ManagerFixture f;
    const double m0 = (double) f.store.getFloatParameter (xoa::ids::masterGain);

    f.inject (OSCMessage ("/xoa/config/masterGain", std::numeric_limits<float>::quiet_NaN()));
    CHECK ((double) f.store.getFloatParameter (xoa::ids::masterGain) == m0);   // unchanged

    f.inject (OSCMessage ("/xoa/config/bogus", 1.0f));
    CHECK ((double) f.store.getFloatParameter (xoa::ids::masterGain) == m0);

    // Transport params are read-only over OSC -> ignored.
    const int rx0 = f.store.getIntParameter (xoa::ids::oscReceivePort);
    f.inject (OSCMessage ("/xoa/config/oscReceivePort", (juce::int32) 9100));
    CHECK (f.store.getIntParameter (xoa::ids::oscReceivePort) == rx0);

    CHECK (f.replies.empty());
}

//==============================================================================
void testManagerBypassesUndo()
{
    ManagerFixture f;
    auto* undo = f.store.getUndoManagerForDomain (xoa::XoaValueTreeState::configDomain);

    CHECK (! undo->canUndo());
    f.inject (OSCMessage ("/xoa/config/masterGain", -3.0f));   // OSC write
    CHECK (! undo->canUndo());                                 // ... not undoable

    f.store.setParameter (xoa::ids::masterGain, -6.0);         // UI write
    CHECK (undo->canUndo());                                   // ... records normally
}

//==============================================================================
void testManagerGet()
{
    ManagerFixture f;
    f.store.setNumSpeakers (4);
    f.store.setParameterWithoutUndo (xoa::ids::speakerGain, -6.0, 1);   // speaker 2 (0-based 1)

    // Write-form query: /xoa/get "/xoa/speaker/gain" 2  -> reply in indexed form.
    f.inject (OSCMessage ("/xoa/get", juce::String ("/xoa/speaker/gain"), (juce::int32) 2));
    CHECK (f.replies.size() == 1);
    if (! f.replies.empty())
    {
        const auto& r = f.replies.back();
        CHECK (r.ip == "127.0.0.1");
        CHECK (r.port == 5005);
        CHECK (r.msg.getAddressPattern().toString() == "/xoa/speaker/2/gain");
        CHECK (r.msg.size() == 1);
        CHECK (r.msg[0].isFloat32());
        CHECK (std::abs (r.msg[0].getFloat32() + 6.0f) < 1.0e-4f);
    }

    // Non-channelized query.
    f.replies.clear();
    f.store.setParameterWithoutUndo (xoa::ids::masterGain, -2.0);
    f.inject (OSCMessage ("/xoa/get", juce::String ("/xoa/config/masterGain")));
    CHECK (f.replies.size() == 1);
    if (! f.replies.empty())
    {
        CHECK (f.replies.back().msg.getAddressPattern().toString() == "/xoa/config/masterGain");
        CHECK (std::abs (f.replies.back().msg[0].getFloat32() + 2.0f) < 1.0e-4f);
    }

    // Unknown address -> no reply.
    f.replies.clear();
    f.inject (OSCMessage ("/xoa/get", juce::String ("/xoa/config/bogus")));
    CHECK (f.replies.empty());
}

//==============================================================================
void testManagerPing()
{
    ManagerFixture f;

    f.inject (OSCMessage ("/xoa/ping", (juce::int32) 42));
    CHECK (f.replies.size() == 1);
    if (! f.replies.empty())
    {
        const auto& r = f.replies.back();
        CHECK (r.msg.getAddressPattern().toString() == "/xoa/pong");
        CHECK (r.msg.size() == 1);
        CHECK (r.msg[0].isInt32());
        CHECK (r.msg[0].getInt32() == 42);
    }

    f.replies.clear();
    f.inject (OSCMessage ("/xoa/ping"));
    CHECK (f.replies.size() == 1);
    if (! f.replies.empty())
    {
        CHECK (f.replies.back().msg.getAddressPattern().toString() == "/xoa/pong");
        CHECK (f.replies.back().msg.size() == 0);
    }
}

//==============================================================================
void testManagerIpFilter()
{
    ManagerFixture f;
    f.store.setParameterWithoutUndo (xoa::ids::oscAcceptAnyHost, false);
    f.store.setParameterWithoutUndo (xoa::ids::oscSendAddress, juce::String ("10.0.0.5"));

    const double m0 = (double) f.store.getFloatParameter (xoa::ids::masterGain);

    // Foreign host -> dropped.
    f.inject (OSCMessage ("/xoa/config/masterGain", -6.0f), "192.168.1.9");
    CHECK ((double) f.store.getFloatParameter (xoa::ids::masterGain) == m0);

    // The configured send address is allowed.
    f.inject (OSCMessage ("/xoa/config/masterGain", -6.0f), "10.0.0.5");
    CHECK (std::abs ((double) f.store.getFloatParameter (xoa::ids::masterGain) + 6.0) < 1.0e-4);

    // Loopback is always allowed.
    f.store.setParameterWithoutUndo (xoa::ids::masterGain, 0.0);
    f.inject (OSCMessage ("/xoa/config/masterGain", -3.0f), "127.0.0.1");
    CHECK (std::abs ((double) f.store.getFloatParameter (xoa::ids::masterGain) + 3.0) < 1.0e-4);
}

//==============================================================================
// C5 - outbound feedback + meters.
//==============================================================================
void testManagerFeedback()
{
    ManagerFixture f;

    // A non-OSC-origin change is echoed to the target (default 127.0.0.1:9001)
    // in indexed form.
    f.store.setParameterWithoutUndo (xoa::ids::masterGain, -4.0);
    f.mgr.flushOutbound();
    CHECK (f.replies.size() == 1);
    if (! f.replies.empty())
    {
        const auto& r = f.replies.back();
        CHECK (r.ip == "127.0.0.1");
        CHECK (r.port == 9001);
        CHECK (r.msg.getAddressPattern().toString() == "/xoa/config/masterGain");
        CHECK (r.msg[0].isFloat32());
        CHECK (std::abs (r.msg[0].getFloat32() + 4.0f) < 1.0e-4f);
    }

    // Channelized feedback uses the indexed address.
    f.replies.clear();
    f.store.setNumSpeakers (4);
    f.store.setParameterWithoutUndo (xoa::ids::speakerGain, -6.0, 2);   // speaker 3 (0-based 2)
    f.mgr.flushOutbound();
    bool sawSpeaker3 = false;
    for (const auto& r : f.replies)
        if (r.msg.getAddressPattern().toString() == "/xoa/speaker/3/gain")
        {
            sawSpeaker3 = true;
            CHECK (std::abs (r.msg[0].getFloat32() + 6.0f) < 1.0e-4f);
        }
    CHECK (sawSpeaker3);
}

void testManagerFeedbackSuppressesOscEcho()
{
    ManagerFixture f;
    f.inject (OSCMessage ("/xoa/config/masterGain", -5.0f));   // OSC origin
    f.mgr.flushOutbound();
    CHECK (f.replies.empty());   // a peer's own write is not echoed back
    CHECK (std::abs ((double) f.store.getFloatParameter (xoa::ids::masterGain) + 5.0) < 1.0e-4);
}

void testManagerFeedbackCoalesces()
{
    ManagerFixture f;
    f.store.setParameterWithoutUndo (xoa::ids::masterGain, -1.0);
    f.store.setParameterWithoutUndo (xoa::ids::masterGain, -2.0);
    f.store.setParameterWithoutUndo (xoa::ids::masterGain, -3.0);
    f.mgr.flushOutbound();
    CHECK (f.replies.size() == 1);   // rate limiter coalesces per address
    if (! f.replies.empty())
        CHECK (std::abs (f.replies.back().msg[0].getFloat32() + 3.0f) < 1.0e-4f);
}

void testManagerFeedbackDisabled()
{
    ManagerFixture f;
    f.store.setParameterWithoutUndo (xoa::ids::oscFeedbackEnabled, false);
    f.store.setParameterWithoutUndo (xoa::ids::masterGain, -6.0);
    f.mgr.flushOutbound();
    CHECK (f.replies.empty());
}

void testManagerMeters()
{
    ManagerFixture f;
    f.store.setNumSpeakers (3);

    // Disabled by default -> no frame.
    f.mgr.sendMeterFrame();
    CHECK (f.replies.empty());

    // Enabled -> one peak per output + cpu + latency.
    f.store.setParameterWithoutUndo (xoa::ids::oscMeterEnabled, true);
    f.mgr.sendMeterFrame();
    CHECK (f.replies.size() == 5);

    int peaks = 0, cpu = 0, lat = 0;
    for (const auto& r : f.replies)
    {
        const auto a = r.msg.getAddressPattern().toString();
        if (a == "/xoa/monitor/output/peak")
        {
            ++peaks;
            CHECK (r.msg.size() == 2);
            CHECK (r.msg[0].isInt32());
            CHECK (r.msg[1].isFloat32());
        }
        else if (a == "/xoa/monitor/cpu")       { ++cpu; CHECK (r.msg.size() == 1); }
        else if (a == "/xoa/monitor/latencyMs") { ++lat; CHECK (r.msg.size() == 1); }
    }
    CHECK (peaks == 3);
    CHECK (cpu == 1);
    CHECK (lat == 1);
}

//==============================================================================
// C6 - head-tracking.
//==============================================================================
void testManagerQuaternion()
{
    ManagerFixture f;

    // A head orientation (pure +Y rotation, non-gimbal). The field must be
    // rotated by the INVERSE, so the store's rotation triple rebuilds the
    // conjugate quaternion's matrix.
    const float qw = 0.8f, qx = 0.0f, qy = 0.6f, qz = 0.0f;
    f.inject (OSCMessage ("/xoa/tracking/quaternion", qw, qx, qy, qz));

    const double yaw   = (double) f.store.getParameter (xoa::ids::rotationYaw);
    const double pitch = (double) f.store.getParameter (xoa::ids::rotationPitch);
    const double roll  = (double) f.store.getParameter (xoa::ids::rotationRoll);

    const auto got = xoa::rot::yawPitchRollToMatrix (yaw, pitch, roll);
    const xoa::rot::Quaternion inv { (double) qw, -(double) qx, -(double) qy, -(double) qz };
    const auto expected = xoa::rot::quaternionToMatrix (inv);

    double d = 0.0;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            d = juce::jmax (d, std::abs (got.m[i][j] - expected.m[i][j]));
    CHECK (d < 1.0e-9);
}

void testManagerTrackingPosition()
{
    ManagerFixture f;

    // Default input-1 position is (1, 0, 0); a tracked update routes through
    // the 1-Euro seam and moves the stored position toward the target.
    CHECK (std::abs ((double) f.store.getFloatParameter (xoa::ids::inputPositionY, 0)) < 1.0e-9);

    f.inject (OSCMessage ("/xoa/tracking/position", (juce::int32) 1, 1.5f, 2.0f, 0.5f));

    CHECK ((double) f.store.getFloatParameter (xoa::ids::inputPositionY, 0) > 0.5);

    // A bad input index is ignored (no crash, no spurious write).
    f.inject (OSCMessage ("/xoa/tracking/position", (juce::int32) 0, 1.0f, 1.0f, 1.0f));
}

} // namespace

//==============================================================================
void runXoaOscManagerTests()
{
    testManagerParamWrites();
    testManagerEqWrite();
    testManagerStructuralCount();
    testManagerRejections();
    testManagerBypassesUndo();
    testManagerGet();
    testManagerPing();
    testManagerIpFilter();
    testManagerFeedback();
    testManagerFeedbackSuppressesOscEcho();
    testManagerFeedbackCoalesces();
    testManagerFeedbackDisabled();
    testManagerMeters();
    testManagerQuaternion();
    testManagerTrackingPosition();
}
