#include "OSCManager.h"

#include <cmath>
#include <cstring>

#include "spatcore/control/osc/OSCIngestQueue.h"
#include "spatcore/control/osc/OSCParser.h"
#include "spatcore/control/osc/OSCRateLimiter.h"
#include "spatcore/control/osc/OSCReceiverWithSenderIP.h"
#include "spatcore/control/osc/OSCSerializer.h"
#include "spatcore/control/osc/OSCTCPReceiver.h"

#include "Audio/AudioEngine.h"
#include "Network/OSCMessageRouter.h"
#include "Parameters/XoaParameterIDs.h"
#include "Parameters/XoaValueTreeState.h"

namespace xoa
{

namespace sc = spatcore::control::osc;

//==============================================================================
OSCManager::OSCManager (XoaValueTreeState& storeToUse, AudioEngine& engineToUse)
    : store (storeToUse), engine (engineToUse)
{
    // Outbound feedback path (works with or without start(): no sockets). The
    // rate limiter coalesces per address at 50 Hz; the store's post-write
    // observer feeds every non-OSC-origin change here.
    rateLimiter = std::make_unique<sc::OSCRateLimiter> (sc::MAX_RATE_HZ);
    rateLimiter->setSendCallback ([this] (int, const juce::OSCMessage& m) { sendToTarget (m); });

    store.setPostWriteObserver ([this] (const juce::ValueTree& n, const juce::Identifier& id,
                                        const juce::var& v, int ch)
    {
        onStoreWrite (n, id, v, ch);
    });
}

OSCManager::~OSCManager()
{
    store.setPostWriteObserver (nullptr);   // stop feedback before teardown
    stop();
}

//==============================================================================
void OSCManager::start()
{
    if (started)
        return;

    // Inbound coalescing: high-rate write-form streams collapse newest-wins per
    // (address, channel); indexed forms (a digit after the prefix) bypass to the
    // FIFO. Config/decoder are low-rate and go straight to the FIFO.
    sc::OSCIngestQueue::Classifier classifier;
    classifier.rules = {
        { "/xoa/input/",              true  },
        { "/xoa/speaker/",            true  },
        { "/xoa/tracking/position",   true  },
        { "/xoa/tracking/quaternion", false },
        { "/xoa/rotation/",           false },
        { "/xoa/listener/",           false },
    };
    classifier.digitAfterPrefixBypasses = true;

    ingestQueue = std::make_unique<sc::OSCIngestQueue> (classifier);
    ingestQueue->setDispatch ([this] (const juce::MemoryBlock& d, const juce::String& ip,
                                      int port, sc::ConnectionMode t)
    {
        onDatagram (d, ip, port, t);
    });

    udpReceiver = std::make_unique<sc::OSCReceiverWithSenderIP>();
    udpReceiver->setRawDataCallback ([this] (juce::MemoryBlock d, juce::String ip, int port)
    {
        if (ingestQueue != nullptr)
            ingestQueue->push (std::move (d), std::move (ip), port, sc::ConnectionMode::UDP);
    });

    tcpReceiver = std::make_unique<sc::OSCTCPReceiver>();
    tcpReceiver->setRawDataCallback ([this] (juce::MemoryBlock d, juce::String ip, int port)
    {
        if (ingestQueue != nullptr)
            ingestQueue->push (std::move (d), std::move (ip), port, sc::ConnectionMode::TCP);
    });

    txSocket.bindToPort (0);   // ephemeral local port for send-only replies

    if (! sendFn)
        sendFn = [this] (const juce::String& ip, int port, const juce::OSCMessage& m)
        {
            sendDatagram (ip, port, m);
        };

    registerConfigListeners();
    reconnect();
    startTimerHz (10);   // /xoa/monitor/* meter frames (gated by oscMeterEnabled)
    started = true;
}

void OSCManager::stop()
{
    if (! started)
        return;

    stopTimer();
    unregisterConfigListeners();
    if (udpReceiver != nullptr) udpReceiver->disconnect();
    if (tcpReceiver != nullptr) tcpReceiver->disconnect();

    udpReceiver.reset();
    tcpReceiver.reset();
    ingestQueue.reset();
    txSocket.shutdown();
    started = false;
}

bool OSCManager::isReceiving() const noexcept
{
    return udpReceiver != nullptr && udpReceiver->isConnected();
}

int OSCManager::getUdpPort() const noexcept
{
    return udpReceiver != nullptr ? udpReceiver->getPortNumber() : 0;
}

//==============================================================================
void OSCManager::reconnect()
{
    if (udpReceiver != nullptr) udpReceiver->disconnect();
    if (tcpReceiver != nullptr) tcpReceiver->disconnect();

    if (! static_cast<bool> (store.getParameter (ids::oscEnabled)))
        return;

    if (udpReceiver != nullptr)
        udpReceiver->connect (store.getIntParameter (ids::oscReceivePort));

    if (tcpReceiver != nullptr && static_cast<bool> (store.getParameter (ids::oscTcpEnabled)))
        tcpReceiver->connect (store.getIntParameter (ids::oscTcpPort));
}

void OSCManager::registerConfigListeners()
{
    const auto reconnectOnChange = [this] (const juce::var&) { reconnect(); };
    store.addParameterListener (ids::oscEnabled,     reconnectOnChange);
    store.addParameterListener (ids::oscReceivePort, reconnectOnChange);
    store.addParameterListener (ids::oscTcpEnabled,  reconnectOnChange);
    store.addParameterListener (ids::oscTcpPort,     reconnectOnChange);
}

void OSCManager::unregisterConfigListeners()
{
    store.removeParameterListeners (ids::oscEnabled);
    store.removeParameterListeners (ids::oscReceivePort);
    store.removeParameterListeners (ids::oscTcpEnabled);
    store.removeParameterListeners (ids::oscTcpPort);
}

//==============================================================================
void OSCManager::handleRawPacket (const void* data, size_t size, const juce::String& senderIP,
                                  int senderPort, sc::ConnectionMode transport)
{
    onDatagram (juce::MemoryBlock (data, size), senderIP, senderPort, transport);
}

void OSCManager::onDatagram (const juce::MemoryBlock& data, const juce::String& ip, int port,
                             sc::ConnectionMode transport)
{
    const int size = (int) data.getSize();
    if (size <= 0)
        return;

    if (! isAllowedHost (ip))
        return;

    const char* bytes = static_cast<const char*> (data.getData());

    try
    {
        int pos = 0;
        if (size >= 8 && std::memcmp (bytes, "#bundle", 7) == 0)
        {
            const juce::OSCBundle bundle = sc::OSCParser::parseBundle (bytes, size, pos);
            for (const auto& element : bundle)
                if (element.isMessage())
                    handleMessage (element.getMessage(), ip, port, transport);
        }
        else
        {
            const juce::OSCMessage msg = sc::OSCParser::parseMessage (bytes, size, pos);
            handleMessage (msg, ip, port, transport);
        }
    }
    catch (const juce::OSCFormatError&)
    {
        // Malformed packet: drop it. A misbehaving client cannot desync us.
    }
}

void OSCManager::handleMessage (const juce::OSCMessage& msg, const juce::String& ip, int port,
                                sc::ConnectionMode)
{
    const juce::String address = msg.getAddressPattern().toString();

    if (address == "/xoa/get")  { handleGet  (msg, ip, port); return; }
    if (address == "/xoa/ping") { handlePing (msg, ip, port); return; }

    // /xoa/tracking/* is handled in C6; unknown addresses fall through to the
    // parameter parser and are rejected there.
    applyParameter (msg);
}

void OSCManager::applyParameter (const juce::OSCMessage& msg)
{
    const auto p = osc::parseParameterMessage (msg);
    if (! p.valid)
        return;   // unknown/malformed/NaN -> dropped

    const sc::OriginTagScope originScope (sc::OriginTag::OSC);
    if (p.bandIndex >= 0)
        store.setEqBandParameterWithoutUndo (p.channelIndex, p.bandIndex, p.id, p.value);
    else
        store.setParameterWithoutUndo (p.id, p.value, p.channelIndex);
}

//==============================================================================
void OSCManager::handleGet (const juce::OSCMessage& msg, const juce::String& ip, int port)
{
    if (msg.size() < 1 || ! msg[0].isString())
        return;

    const auto r = osc::resolveAddress (msg[0].getString());
    if (! r.valid)
        return;

    const osc::Binding* b = osc::findBindingById (r.id);
    const bool channelized = (b != nullptr && b->channelized);

    int ch   = r.channelIndex;
    int band = r.bandIndex;

    if (channelized && ! r.channelInPath)
    {
        // Write-form query: the channel is the second argument.
        double c = 0.0;
        if (msg.size() < 2 || ! osc::argToNumber (msg[1], c))
            return;
        ch = (int) std::lround (c) - 1;
        if (ch < 0)
            return;
    }

    juce::var value;
    if (band >= 0)
        value = store.getEqBandParameter (ch, band, r.id);
    else if (channelized)
        value = store.getParameter (r.id, ch);
    else
        value = store.getParameter (r.id, -1);

    if (value.isVoid())
        return;   // unset / out-of-range channel -> no reply

    sendReply (ip, port, osc::makeFeedbackMessage (r.id, ch, band, value));
}

void OSCManager::handlePing (const juce::OSCMessage& msg, const juce::String& ip, int port)
{
    juce::OSCMessage pong ("/xoa/pong");
    if (msg.size() >= 1 && msg[0].isInt32())
        pong.addInt32 (msg[0].getInt32());
    sendReply (ip, port, pong);
}

//==============================================================================
bool OSCManager::isAllowedHost (const juce::String& ip) const
{
    if (static_cast<bool> (store.getParameter (ids::oscAcceptAnyHost)))
        return true;
    if (ip.isEmpty() || ip == "127.0.0.1" || ip == "::1")
        return true;   // loopback always allowed
    return ip == store.getStringParameter (ids::oscSendAddress);
}

void OSCManager::sendReply (const juce::String& ip, int port, const juce::OSCMessage& msg)
{
    if (sendFn)
        sendFn (ip, port, msg);
}

void OSCManager::sendDatagram (const juce::String& ip, int port, const juce::OSCMessage& msg)
{
    const juce::MemoryBlock bytes = sc::OSCSerializer::serializeMessage (msg);
    txSocket.write (ip, port, bytes.getData(), (int) bytes.getSize());
}

//==============================================================================
// Outbound feedback + meters.
//==============================================================================
void OSCManager::onStoreWrite (const juce::ValueTree& node, const juce::Identifier& id,
                               const juce::var& value, int channelIndex)
{
    if (! static_cast<bool> (store.getParameter (ids::oscFeedbackEnabled)))
        return;
    if (sc::getCurrentOriginTag() == sc::OriginTag::OSC)
        return;   // don't echo a peer's own write straight back to it

    const osc::Binding* b = osc::findBindingById (id);
    if (b == nullptr)
        return;   // not OSC-exposed (transport params, ids, showName, ...)

    // handlePostWrite resolves channelIndex to the speaker index even for EQ
    // band properties; recover the band index from the changed node.
    const int band = (node.getType() == ids::band)
                         ? static_cast<int> (node.getProperty (ids::idProp, 0)) - 1
                         : -1;

    if (rateLimiter != nullptr)
        rateLimiter->queueMessage (0, osc::makeFeedbackMessage (id, channelIndex, band, value));
}

void OSCManager::sendToTarget (const juce::OSCMessage& msg)
{
    const juce::String ip = store.getStringParameter (ids::oscSendAddress);
    const int port = store.getIntParameter (ids::oscSendPort);
    if (sendFn && ip.isNotEmpty() && port > 0)
        sendFn (ip, port, msg);
}

void OSCManager::timerCallback()
{
    sendMeterFrame();
}

void OSCManager::sendMeterFrame()
{
    if (! static_cast<bool> (store.getParameter (ids::oscMeterEnabled)))
        return;

    const int numOut = juce::jmin (store.getNumSpeakers(), xoa::kMaxSpeakers);
    for (int c = 0; c < numOut; ++c)
        sendToTarget (juce::OSCMessage ("/xoa/monitor/output/peak",
                                        (juce::int32) (c + 1), engine.getOutputPeakLevel (c)));

    sendToTarget (juce::OSCMessage ("/xoa/monitor/cpu",       (float) engine.getCpuLoad()));
    sendToTarget (juce::OSCMessage ("/xoa/monitor/latencyMs", (float) engine.getMeasuredLatencyMs()));
}

void OSCManager::flushOutbound()
{
    if (rateLimiter != nullptr)
        rateLimiter->flushAll();
}

} // namespace xoa
