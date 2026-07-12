#pragma once

#include <juce_core/juce_core.h>
#include <juce_osc/juce_osc.h>

#include <functional>
#include <memory>

#include "spatcore/control/osc/OscTransportTypes.h"

//==============================================================================
// XOA - OSC manager (WP9 C4). Owns the inbound transports and turns received
// /xoa messages into store writes, /xoa/get replies, and /xoa/ping handshakes.
// The address<->id mapping lives entirely in OSCMessageRouter (C3); this class
// owns transport lifecycle, the ingest queue, IP filtering, and origin tagging.
//
// Threading: receiver threads only push raw bytes into the spatcore
// OSCIngestQueue; the queue drains on the message thread and calls onDatagram,
// so every store write happens on the message thread (RT rules). start()/stop()
// and the config listeners run on the message thread.
//
// Head-tracking (/xoa/tracking/*) and outbound feedback/meters are layered on
// in C6/C5; this chunk is inbound parameters + get + ping.
//==============================================================================

namespace spatcore::control::osc { class OSCReceiverWithSenderIP; class OSCTCPReceiver; class OSCIngestQueue; }

namespace xoa
{

class XoaValueTreeState;
class AudioEngine;

class OSCManager
{
public:
    OSCManager (XoaValueTreeState& storeToUse, AudioEngine& engineToUse);
    ~OSCManager();

    /** Create the ingest queue + receivers, install the default UDP reply
        sender, register the config listeners, and bind per the store's osc*
        config. Message thread. Idempotent. */
    void start();

    /** Tear down the receivers/queue and unregister listeners. Message thread. */
    void stop();

    bool isReceiving() const noexcept;
    int  getUdpPort()  const noexcept;

    /** Outbound reply/feedback hook. start() installs a default that serialises
        and sends a UDP datagram to (ip, port); tests override to capture. */
    using SendFn = std::function<void (const juce::String& ip, int port, const juce::OSCMessage&)>;
    void setSendFn (SendFn fn) { sendFn = std::move (fn); }

    /** Test seam: inject a raw OSC packet as if it arrived on the socket. Runs
        the full dispatch synchronously (bypassing the ingest-queue timer), so a
        test can serialise a message, feed it here, and observe the store. */
    void handleRawPacket (const void* data, size_t size, const juce::String& senderIP,
                          int senderPort,
                          spatcore::control::osc::ConnectionMode transport
                              = spatcore::control::osc::ConnectionMode::UDP);

private:
    void reconnect();                 // (re)bind receivers to the current config
    void registerConfigListeners();
    void unregisterConfigListeners();

    void onDatagram (const juce::MemoryBlock& data, const juce::String& ip, int port,
                     spatcore::control::osc::ConnectionMode transport);
    void handleMessage (const juce::OSCMessage& msg, const juce::String& ip, int port,
                        spatcore::control::osc::ConnectionMode transport);
    void applyParameter (const juce::OSCMessage& msg);
    void handleGet  (const juce::OSCMessage& msg, const juce::String& ip, int port);
    void handlePing (const juce::OSCMessage& msg, const juce::String& ip, int port);

    bool isAllowedHost (const juce::String& ip) const;
    void sendReply (const juce::String& ip, int port, const juce::OSCMessage& msg);
    void sendDatagram (const juce::String& ip, int port, const juce::OSCMessage& msg);

    XoaValueTreeState& store;
    AudioEngine&       engine;

    std::unique_ptr<spatcore::control::osc::OSCReceiverWithSenderIP> udpReceiver;
    std::unique_ptr<spatcore::control::osc::OSCTCPReceiver>          tcpReceiver;
    std::unique_ptr<spatcore::control::osc::OSCIngestQueue>          ingestQueue;

    juce::DatagramSocket txSocket { false };   // send-only (never bound to receive)
    SendFn sendFn;
    bool   started = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (OSCManager)
};

} // namespace xoa
