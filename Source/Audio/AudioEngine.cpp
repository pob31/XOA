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
}

void AudioEngine::audioDeviceStopped()
{
    algorithm.releaseResources();
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

    sceneCounter += n;
}

} // namespace xoa
