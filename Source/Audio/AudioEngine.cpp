#include "Audio/AudioEngine.h"
#include "Audio/TestSceneGenerator.h"

#include "Parameters/XoaParameterIDs.h"

namespace xoa
{

// Trailing-edge debounce for decoder rebuilds: long enough to coalesce a drag,
// far longer than any audio block so the <=1-publish-per-block contract holds.
static constexpr int kDecoderDebounceMs = 150;

//==============================================================================
AudioEngine::AudioEngine (XoaValueTreeState& s)
    : store (s)
{
    registerListeners();

    // Publish-before-enable: a valid snapshot on every seam before the device
    // (and thus the algorithm) can read one.
    publishRotation();
    publishBusParams();
    publishSpeakerComp();
    rebuildDecoderNow();
}

AudioEngine::~AudioEngine()
{
    closeAudioDevice();
    stopTimer();
    rebuildWorker.stop();      // join the worker BEFORE cancelling async updates,
    cancelPendingUpdate();     // so no triggerAsyncUpdate can fire into a dead object
    unregisterListeners();
}

//==============================================================================
void AudioEngine::registerListeners()
{
    if (listenersRegistered)
        return;

    store.addParameterListener (ids::rotationYaw,   [this] (const juce::var&) { publishRotation(); });
    store.addParameterListener (ids::rotationPitch, [this] (const juce::var&) { publishRotation(); });
    store.addParameterListener (ids::rotationRoll,  [this] (const juce::var&) { publishRotation(); });

    store.addParameterListener (ids::masterGain,           [this] (const juce::var&) { publishBusParams(); });
    store.addParameterListener (ids::playbackContentOrder, [this] (const juce::var&) { publishBusParams(); });
    store.addParameterListener (ids::playbackConvention,   [this] (const juce::var&) { publishBusParams(); });

    // Distance-comp mode lives in Config, so it needs its own listener (the
    // Speakers subtree listener below only sees per-speaker edits). It changes
    // only the comp POD -> republish, no decoder rebuild.
    store.addParameterListener (ids::distanceCompMode, [this] (const juce::var&) { publishSpeakerComp(); });

    // The Speakers and Decoder subtrees drive decoder rebuilds; one listener on
    // each catches position edits, count changes (child add/remove) and merges.
    // Hold the subtree handles as members: a ValueTree registers listeners by
    // instance address, so listening on a temporary would unregister at once.
    speakersSection = store.getSpeakersSection();
    decoderSection = store.getDecoderSection();
    speakersSection.addListener (this);
    decoderSection.addListener (this);

    listenersRegistered = true;
}

void AudioEngine::unregisterListeners()
{
    if (! listenersRegistered)
        return;

    store.removeParameterListeners (ids::rotationYaw);
    store.removeParameterListeners (ids::rotationPitch);
    store.removeParameterListeners (ids::rotationRoll);
    store.removeParameterListeners (ids::masterGain);
    store.removeParameterListeners (ids::playbackContentOrder);
    store.removeParameterListeners (ids::playbackConvention);
    store.removeParameterListeners (ids::distanceCompMode);

    speakersSection.removeListener (this);
    decoderSection.removeListener (this);

    listenersRegistered = false;
}

//==============================================================================
void AudioEngine::openAudioDevice()
{
    const juce::String saved = store.getStringParameter (ids::audioDeviceState);
    std::unique_ptr<juce::XmlElement> savedXml =
        saved.isNotEmpty() ? juce::parseXML (saved) : nullptr;

    deviceManager.initialise (0, xoa::kMaxSpeakers, savedXml.get(), true);
    deviceManager.addChangeListener (this);
    deviceManager.addAudioCallback (this);
    callbackRegistered = true;
}

void AudioEngine::closeAudioDevice()
{
    if (callbackRegistered)
    {
        deviceManager.removeAudioCallback (this);
        deviceManager.removeChangeListener (this);
        callbackRegistered = false;

        // Persist the final device state.
        if (auto xml = deviceManager.createStateXml())
            store.setParameterWithoutUndo (ids::audioDeviceState, xml->toString());
    }
}

//==============================================================================
void AudioEngine::setInputSource (InputSource source)
{
    inputSource.store (source, std::memory_order_relaxed);
    publishBusParams();   // the gather differs between file and scene
}

FilePlayer::OpenResult AudioEngine::openFile (const juce::File& file)
{
    auto r = filePlayer.open (file);
    if (r.ok)
    {
        fileNumChannels.store (r.numChannels, std::memory_order_relaxed);
        fileDetectedOrder.store (r.detectedOrder, std::memory_order_relaxed);
        store.setParameter (ids::playbackFilePath, file.getFullPathName());
        setInputSource (InputSource::file);   // recomposes + publishes the gather
    }
    return r;
}

//==============================================================================
void AudioEngine::publishRotation()
{
    const double yaw   = (double) store.getFloatParameter (ids::rotationYaw);
    const double pitch = (double) store.getFloatParameter (ids::rotationPitch);
    const double roll  = (double) store.getFloatParameter (ids::rotationRoll);
    rotationSnapshot.publish (rt::makeRotationState (yaw, pitch, roll, ++rotationEpoch));
}

void AudioEngine::publishBusParams()
{
    const double masterDb = (double) store.getFloatParameter (ids::masterGain);

    if (inputSource.load (std::memory_order_relaxed) == InputSource::testScene)
    {
        // The synthetic scene is order-10 SN3D; the file content-order/convention
        // overrides do not apply to it.
        busParamsSnapshot.publish (rt::makeBusParams (0, xoa::kAmbisonicOrder, 0,
                                                      xoa::kNumSHChannels, masterDb, ++busEpoch));
    }
    else
    {
        const int overrideOrder = store.getIntParameter (ids::playbackContentOrder);
        const int convention    = store.getIntParameter (ids::playbackConvention);
        busParamsSnapshot.publish (rt::makeBusParams (
            overrideOrder, fileDetectedOrder.load (std::memory_order_relaxed), convention,
            fileNumChannels.load (std::memory_order_relaxed), masterDb, ++busEpoch));
    }
}

void AudioEngine::publishSpeakerComp()
{
    speakerCompSnapshot.publish (composeSpeakerCompParams (store, ++speakerCompEpoch));
}

void AudioEngine::updateSpeakerEq()
{
    // Benign-staleness (D3): push coefficients onto the RT-owned biquads from
    // the message thread. A no-op before the device is prepared (0 channels).
    speakerComp.setEqParameters (composeSpeakerEqParams (store));
}

void AudioEngine::onSpeakerStructureChanged()
{
    // A speaker was added/removed/reordered: the decoder must redesign AND the
    // comp delay/gain + EQ maps shift with the new channel set.
    markDecoderDirty();
    publishSpeakerComp();
    updateSpeakerEq();
}

// D17 property split - route each per-speaker / decoder edit to the minimum
// work it actually requires.
void AudioEngine::valueTreePropertyChanged (juce::ValueTree&, const juce::Identifier& property)
{
    // Trim / mute / solo: comp gain only (no decoder rebuild).
    if (property == ids::speakerGain || property == ids::speakerDelay
        || property == ids::speakerMute || property == ids::speakerSolo)
    {
        publishSpeakerComp();
        return;
    }

    // EQ: RT biquad coefficients only.
    if (property == ids::speakerEqEnabled || property == ids::eqShape
        || property == ids::eqFrequency || property == ids::eqGain
        || property == ids::eqQ || property == ids::eqSlope)
    {
        updateSpeakerEq();
        return;
    }

    // Positions drive both the decode geometry and the distance comp.
    if (property == ids::speakerPositionX || property == ids::speakerPositionY
        || property == ids::speakerPositionZ)
    {
        markDecoderDirty();
        publishSpeakerComp();
        return;
    }

    // Decoder props, coordinate-mode, names, ids: decoder rebuild (safe default).
    markDecoderDirty();
}

void AudioEngine::markDecoderDirty()
{
    startTimer (kDecoderDebounceMs);   // (re)arm the trailing-edge one-shot
}

void AudioEngine::timerCallback()
{
    stopTimer();
    requestAsyncRebuild();   // debounce elapsed -> design off the message thread
}

void AudioEngine::requestAsyncRebuild()
{
    ++rebuildGeneration;
    rebuildInFlight.store (true, std::memory_order_release);
    rebuildWorker.submit ({ DecoderMatrixBuilder::layoutFromStore (store),
                            DecoderMatrixBuilder::optionsFromStore (store),
                            rebuildGeneration });
}

void AudioEngine::handleAsyncUpdate()
{
    juce::uint64 gen = 0;
    decoder::DesignResult result;
    if (! rebuildWorker.takeCompleted (gen, result))
        return;

    // Discard results superseded by a newer request or a synchronous flush.
    if (gen != rebuildGeneration)
        return;

    decoderBuilder.adoptResult (std::move (result));
    decoderBuilder.publish();
    rebuildInFlight.store (false, std::memory_order_release);
    if (onDecoderRebuilt)
        onDecoderRebuilt (decoderBuilder.lastDesignResult());
}

void AudioEngine::finishPendingAsyncRebuild()
{
    rebuildWorker.waitUntilIdle();
    handleAsyncUpdate();
}

void AudioEngine::flushDecoderRebuild()
{
    stopTimer();
    ++rebuildGeneration;   // invalidate any in-flight async result
    rebuildDecoderNow();
    rebuildInFlight.store (false, std::memory_order_release);
}

void AudioEngine::rebuildDecoderNow()
{
    const auto result = decoderBuilder.rebuild (store);
    decoderBuilder.publish();
    if (onDecoderRebuilt)
        onDecoderRebuilt (result);
}

//==============================================================================
void AudioEngine::changeListenerCallback (juce::ChangeBroadcaster*)
{
    if (auto xml = deviceManager.createStateXml())
        store.setParameterWithoutUndo (ids::audioDeviceState, xml->toString());
}

//==============================================================================
void AudioEngine::audioDeviceAboutToStart (juce::AudioIODevice* device)
{
    const double sr = device->getCurrentSampleRate();
    const int block = device->getCurrentBufferSizeSamples();
    const int outLatency = device->getOutputLatencyInSamples();

    deviceSampleRate.store (sr, std::memory_order_relaxed);
    deviceBlockSize.store (block, std::memory_order_relaxed);
    // Reported latency excludes the file read-ahead (documented): device output
    // latency plus one processing block.
    measuredLatencyMs.store (sr > 0.0 ? (double) (outLatency + block) / sr * 1000.0 : 0.0,
                             std::memory_order_relaxed);

    inputScratch.setSize (xoa::kMaxFileChannels, block, false, false, true);
    filePlayer.prepareToPlay (sr, block);

    const int numOut = device->getActiveOutputChannels().countNumberOfSetBits();
    algorithm.prepare (xoa::kNumSHChannels, numOut, sr, block,
                       &decoderBuilder, &rotationSnapshot, &busParamsSnapshot, true);

    // Per-speaker comp runs after the decode. Allocate its per-output state for
    // the actual device outs, then seed the RT biquads from the current EQ (the
    // ms-based comp POD is already published and needs no rebuild for the SR).
    speakerComp.prepare (sr, block, numOut, &speakerCompSnapshot);
    updateSpeakerEq();

    testSignal.prepare (sr, block);
}

void AudioEngine::audioDeviceStopped()
{
    algorithm.releaseResources();
    speakerComp.releaseResources();
    filePlayer.releaseResources();
    deviceSampleRate.store (0.0, std::memory_order_relaxed);
    deviceBlockSize.store (0, std::memory_order_relaxed);
}

void AudioEngine::audioDeviceIOCallbackWithContext (const float* const* inputChannelData,
                                                    int numInputChannels,
                                                    float* const* outputChannelData,
                                                    int numOutputChannels,
                                                    int numSamples,
                                                    const juce::AudioIODeviceCallbackContext& context)
{
    juce::ignoreUnused (inputChannelData, numInputChannels, context);

    juce::ScopedNoDenormals noDenormals;

    juce::AudioBuffer<float> outBuf (outputChannelData, numOutputChannels, numSamples);

    // The input source writes into inputScratch, allocated to the block size
    // reported at audioDeviceAboutToStart. JUCE's contract only varies the
    // callback block downward (or restarts the device, which re-sizes the
    // scratch), but AmbiBusAlgorithm defends against an over-size block, so the
    // upstream scratch write must too: clamp the render to the scratch length
    // and silence any tail we could not fill (defense-in-depth, no allocation).
    const int n = juce::jmin (numSamples, inputScratch.getNumSamples());
    if (n < numSamples)
        outBuf.clear();

    juce::AudioSourceChannelInfo info (&outBuf, 0, n);

    if (inputSource.load (std::memory_order_relaxed) == InputSource::testScene)
    {
        float* ptrs[xoa::kNumSHChannels];
        for (int c = 0; c < xoa::kNumSHChannels; ++c)
            ptrs[c] = inputScratch.getWritePointer (c);
        scene::renderScene (xoa::kAmbisonicOrder, sceneCounter, n,
                            deviceSampleRate.load (std::memory_order_relaxed), ptrs);
        algorithm.processBlock (info, inputScratch, xoa::kNumSHChannels, numOutputChannels);
    }
    else
    {
        filePlayer.renderNextBlock (inputScratch, n);
        algorithm.processBlock (info, inputScratch,
                                fileNumChannels.load (std::memory_order_relaxed), numOutputChannels);
    }

    // Per-speaker compensation (delay/EQ/gain) on the decoded output, in place.
    speakerComp.processBlock (outBuf, numOutputChannels, n);

    // Output test signal (FR-21), injected post-comp with replace-semantics on
    // its target channel(s) so it lands exactly where the meters read it.
    if (testSignal.isActive())
        testSignal.renderNextBlock (outBuf, 0, n);

    // Post-comp / post-test-signal block-peak meters (what leaves the device).
    for (int s = 0; s < numOutputChannels && s < xoa::kMaxSpeakers; ++s)
        outputPeak[(size_t) s].store (outBuf.getMagnitude (s, 0, n), std::memory_order_relaxed);
    for (int s = juce::jmax (0, numOutputChannels); s < xoa::kMaxSpeakers; ++s)
        outputPeak[(size_t) s].store (0.0f, std::memory_order_relaxed);

    sceneCounter += n;
}

} // namespace xoa
