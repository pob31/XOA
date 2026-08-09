#include "Audio/AudioEngine.h"
#include "Audio/TestSceneGenerator.h"

#include "Parameters/XoaParameterDefaults.h"
#include "Parameters/XoaParameterIDs.h"

#include <cmath>

namespace xoa
{

// Trailing-edge debounce for decoder rebuilds: long enough to coalesce a drag,
// far longer than any audio block so the <=1-publish-per-block contract holds.
static constexpr int kDecoderDebounceMs = 150;

//==============================================================================
AudioEngine::AudioEngine (XoaValueTreeState& s)
    : store (s), calcEngine (s)
{
    // The shared generator seeds from the wall clock by default; pin XOA's
    // fixed seed so renders stay reproducible (applies at each prepare()).
    testSignal.setDeterministicSeed (kTestSignalSeed);

    registerListeners();

    // Publish-before-enable: a valid snapshot on every seam before the device
    // (and thus the algorithm) can read one.
    publishRotation();
    publishBusParams();
    publishSpeakerComp();
    publishPatchRouting();
    rebuildDecoderNow();
    updateReferenceRadius();   // seed the encoder r_ref from the speaker layout
}

AudioEngine::~AudioEngine()
{
    closeAudioDevice();
    // Tracker hardware goes down only after the audio callback has stopped:
    // the RT stage holds a raw pointer to the active orientation source, so
    // the source must outlive every block that could still read it.
    monitoringEngine.stopMonitoring();
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

    store.addParameterListener (ids::masterGain, [this] (const juce::var&) { publishBusParams(); });

    // Binaural monitoring (WP15): tracker selection starts/stops hardware, and
    // a camera-index edit only takes effect on the next start, so it restarts
    // whatever is running.
    store.addParameterListener (ids::binauralHeadTracker,
                                [this] (const juce::var&) { monitoringEngine.applyTrackerSelection(); });
    store.addParameterListener (ids::binauralCameraIndex,
                                [this] (const juce::var&) { monitoringEngine.reapplyCameraIndex(); });

    // Distance-comp mode lives in Config, so it needs its own listener (the
    // Speakers subtree listener below only sees per-speaker edits). It changes
    // only the comp POD -> republish, no decoder rebuild.
    store.addParameterListener (ids::distanceCompMode, [this] (const juce::var&) { publishSpeakerComp(); });

    // Listener position (D18/FR-25) re-references the distance comp -> republish
    // the comp POD only. It does NOT move the rig geometry, so the decoder and
    // the encoder r_ref (updateReferenceRadius) are deliberately untouched.
    store.addParameterListener (ids::listenerX, [this] (const juce::var&) { publishSpeakerComp(); });
    store.addParameterListener (ids::listenerY, [this] (const juce::var&) { publishSpeakerComp(); });
    store.addParameterListener (ids::listenerZ, [this] (const juce::var&) { publishSpeakerComp(); });

    // The Speakers and Decoder subtrees drive decoder rebuilds; one listener on
    // each catches position edits, count changes (child add/remove) and merges.
    // Hold the subtree handles as members: a ValueTree registers listeners by
    // instance address, so listening on a temporary would unregister at once.
    speakersSection = store.getSpeakersSection();
    decoderSection = store.getDecoderSection();
    speakersSection.addListener (this);
    decoderSection.addListener (this);

    // Patch routing follows the AudioPatch trees. Format/count changes reach
    // this listener too: the store's reconcile rewrites patchData whenever the
    // spans move, so one listener covers every route.
    audioPatchSection = store.getAudioPatchSection();
    audioPatchSection.addListener (this);

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
    store.removeParameterListeners (ids::distanceCompMode);
    store.removeParameterListeners (ids::listenerX);
    store.removeParameterListeners (ids::listenerY);
    store.removeParameterListeners (ids::listenerZ);
    store.removeParameterListeners (ids::binauralHeadTracker);
    store.removeParameterListeners (ids::binauralCameraIndex);

    speakersSection.removeListener (this);
    decoderSection.removeListener (this);
    audioPatchSection.removeListener (this);

    listenersRegistered = false;
}

//==============================================================================
void AudioEngine::openAudioDevice()
{
    const juce::String saved = store.getStringParameter (ids::audioDeviceState);
    std::unique_ptr<juce::XmlElement> savedXml =
        saved.isNotEmpty() ? juce::parseXML (saved) : nullptr;

    // Restore through DeviceHost so every mutation writes explicit channel
    // masks with JUCE's useDefault*Channels flags cleared — without that,
    // setAudioDeviceSetup silently substitutes range(0, count) and the state
    // XML can never round-trip a channel selection (handoff §2.2). The policy
    // opens EVERY channel the device has, up to kMaxHardwareChannels.
    lastDeviceError = deviceHost.restoreFromXml (savedXml.get(), true);
    deviceManager.addChangeListener (this);
    deviceManager.addAudioCallback (&ioCallback);   // once — the manager re-arms it across device changes
    callbackRegistered = true;

    // Seed the patch trees' active-channel counts from the device we just
    // opened. The ChangeListener only fires on LATER changes, and a count of
    // 0 disables the matrices' overflow gating entirely — so without this the
    // patch window would offer channels the device never opened.
    store.updateHardwareChannelCount (deviceHost.getNumActiveInputs(),
                                      deviceHost.getNumActiveOutputs());

    // Bring the encoder engine to the device's sample rate / rig radius and keep
    // its live matrices fresh at 50 Hz while the device is open.
    syncCalcEngineToDevice();
    calcEngine.startTicking();
}

void AudioEngine::closeAudioDevice()
{
    if (callbackRegistered)
    {
        calcEngine.stopTicking();
        // Blocks until the audio thread has left the callback — must stay
        // ahead of destroying anything getNextAudioBlock touches.
        deviceManager.removeAudioCallback (&ioCallback);
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
    publishBusParams();   // the gather differs between none and scene
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
        // The synthetic scene is order-10 SN3D.
        busParamsSnapshot.publish (rt::makeBusParams (0, xoa::kAmbisonicOrder, 0,
                                                      xoa::kNumSHChannels, masterDb, ++busEpoch));
    }
    else
    {
        // No HOA source (D48): zero input channels makes every gather slot
        // srcChannel -1, so the gather stage clears busA each block and the
        // encoder stage (stems / HOA groups from device inputs) accumulates
        // on top. Master gain still rides this snapshot, so this publisher
        // must keep running even with no source.
        busParamsSnapshot.publish (rt::makeBusParams (0, 0, 0, 0, masterDb, ++busEpoch));
    }
}

void AudioEngine::publishSpeakerComp()
{
    speakerCompSnapshot.publish (composeSpeakerCompParams (store, ++speakerCompEpoch));
}

void AudioEngine::publishPatchRouting()
{
    patchSnapshot.publish (rt::composePatchRouting (store, ++patchEpoch));
}

int AudioEngine::getHardwareOutputForSpeaker (int speakerIndex) const
{
    if (speakerIndex < 0 || speakerIndex >= store.getNumSpeakers())
        return -1;

    const auto tree = store.getOutputPatchTree();
    if (! tree.isValid())
        return speakerIndex;   // no patch section: identity

    const auto rows = juce::StringArray::fromTokens (
        tree.getProperty (ids::patchData).toString(), ";", "");
    return speakerIndex < rows.size() ? rt::patchRowColumn (rows[speakerIndex]) : -1;
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
    updateReferenceRadius();   // the encoder r_ref follows the rig
}

// D17 property split - route each per-speaker / decoder edit to the minimum
// work it actually requires.
void AudioEngine::valueTreePropertyChanged (juce::ValueTree&, const juce::Identifier& property)
{
    // Patch-tree properties (this listener also covers audioPatchSection).
    // Routing follows patchData/rows; cols and activeHardware* are
    // display-side only. None of them touch the decoder.
    if (property == ids::patchData || property == ids::rows)
    {
        publishPatchRouting();
        return;
    }
    if (property == ids::cols || property == ids::activeHardwareInputs
        || property == ids::activeHardwareOutputs)
        return;

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
        updateReferenceRadius();   // the encoder r_ref follows the rig
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
// Mean speaker radius -> encoder r_ref (NFC poles + distance-gain reference).
// Origin-placed speakers (r == 0) are skipped; an empty/degenerate rig falls
// back to the default radius. Message thread.
void AudioEngine::updateReferenceRadius()
{
    const int n = store.getNumSpeakers();
    double sum = 0.0;
    int cnt = 0;
    for (int s = 0; s < n; ++s)
    {
        const double x = store.getFloatParameter (ids::speakerPositionX, s);
        const double y = store.getFloatParameter (ids::speakerPositionY, s);
        const double z = store.getFloatParameter (ids::speakerPositionZ, s);
        const double r = std::sqrt (x * x + y * y + z * z);
        if (r > 1.0e-6) { sum += r; ++cnt; }
    }
    calcEngine.setReferenceRadius (cnt > 0 ? sum / cnt : defaults::kDefaultRigRadius);
}

// Push the running device's sample rate to the encoder engine and recompute its
// live matrices now (message thread: called from openAudioDevice / device state
// changes, never the audio callback).
void AudioEngine::syncCalcEngineToDevice()
{
    if (auto* dev = deviceManager.getCurrentAudioDevice())
        if (const double sr = dev->getCurrentSampleRate(); sr > 0.0)
            calcEngine.setSampleRate (sr);
    updateReferenceRadius();
    calcEngine.tick();
}

//==============================================================================
void AudioEngine::changeListenerCallback (juce::ChangeBroadcaster*)
{
    // Re-assert the enable-all mask policy after any device change — e.g. one
    // made through the stock selector in SystemConfigTab, which knows nothing
    // of DeviceHost. No-ops when the setup already matches, so the change
    // broadcast this can itself trigger terminates immediately.
    if (auto err = deviceHost.enableAllChannels(); err.isNotEmpty())
        lastDeviceError = err;

    // Truthful active counts to the patch trees (overflow gating, §7.1) —
    // from DeviceHost, never the device's channel-name lists.
    store.updateHardwareChannelCount (deviceHost.getNumActiveInputs(),
                                      deviceHost.getNumActiveOutputs());

    if (auto xml = deviceManager.createStateXml())
        store.setParameterWithoutUndo (ids::audioDeviceState, xml->toString());

    // A device (re)start may have changed the sample rate; keep the encoder
    // engine's NFC design in step (message thread).
    syncCalcEngineToDevice();
}

//==============================================================================
void AudioEngine::prepareToPlay (int samplesPerBlockExpected, double sampleRate)
{
    const double sr = sampleRate;
    const int block = samplesPerBlockExpected;

    // The AudioSource signature carries only rate and block size; everything
    // else comes off the device / DeviceHost — and NEVER off the io buffer,
    // whose width is the hardware span, not the speaker count (§5.2).
    int outLatency = 0;
    if (auto* device = deviceManager.getCurrentAudioDevice())
        outLatency = device->getOutputLatencyInSamples();

    deviceSampleRate.store (sr, std::memory_order_relaxed);
    deviceBlockSize.store (block, std::memory_order_relaxed);
    // Reported latency excludes the file read-ahead (documented): device output
    // latency plus one processing block.
    measuredLatencyMs.store (sr > 0.0 ? (double) (outLatency + block) / sr * 1000.0 : 0.0,
                             std::memory_order_relaxed);

    // The device masks stay contiguous from bit 0 (DeviceHost's enable-all
    // policy writes range(0, N) — the patch layer routes INTERNALLY and never
    // deselects device channels), so io-buffer row h == hardware channel h.
    // Assert the guarantee this whole stage rests on.
    jassert (ioCallback.getChannelMap().isIdentityMapping());

    const int numIn    = juce::jmin (deviceHost.getNumActiveInputs(), xoa::kMaxHardwareChannels);
    const int numOutHw = juce::jmin (deviceHost.getNumActiveOutputs(), xoa::kMaxHardwareChannels);
    numActiveInputs.store (numIn, std::memory_order_relaxed);
    numActiveOutputs.store (numOutHw, std::memory_order_relaxed);

    // The decode/comp stage runs in the SPEAKER domain and is scattered to
    // hardware outputs by the output patch — its channel count is the store's
    // speaker count, not the device's output count (D42/D46).
    const int numSpk = juce::jmin (store.getNumSpeakers(), xoa::kMaxSpeakers);
    numSpeakersPrepared.store (numSpk, std::memory_order_relaxed);

    inputScratch.setSize (xoa::kMaxFileChannels, block, false, false, true);
    stemScratch.setSize (xoa::kMaxStemChannels, block, false, false, true);
    speakerScratch.setSize (xoa::kMaxSpeakers, block, false, false, true);
    speakerScratch.clear();
    hwWritten.assign ((size_t) xoa::kMaxHardwareChannels, 0);

    algorithm.prepare (xoa::kNumSHChannels, numSpk, sr, block,
                       &decoderBuilder, &rotationSnapshot, &busParamsSnapshot, true,
                       calcEngine.encodeMatrix(), calcEngine.nfcCoeffs(), &calcEngine.encoderSource());

    // Per-speaker comp runs after the decode, still per speaker. Seed the RT
    // biquads from the current EQ (the ms-based comp POD is already published
    // and needs no rebuild for the SR).
    speakerComp.prepare (sr, block, numSpk, &speakerCompSnapshot);
    updateSpeakerEq();

    testSignal.prepare (sr, block);
}

void AudioEngine::releaseResources()
{
    algorithm.releaseResources();
    speakerComp.releaseResources();
    deviceSampleRate.store (0.0, std::memory_order_relaxed);
    deviceBlockSize.store (0, std::memory_order_relaxed);
    numActiveInputs.store (0, std::memory_order_relaxed);
    numActiveOutputs.store (0, std::memory_order_relaxed);
    numSpeakersPrepared.store (0, std::memory_order_relaxed);
    for (auto& p : hwInputPeak)
        p.store (0.0f, std::memory_order_relaxed);
}

void AudioEngine::getNextAudioBlock (const juce::AudioSourceChannelInfo& info)
{
    juce::ScopedNoDenormals noDenormals;

    // The io buffer is HARDWARE-indexed: row h is hardware channel h, spanning
    // max(highest in, highest out)+1 channels — NOT the speaker count (§5.2).
    // DeviceIoCallback always fills from sample 0.
    juce::AudioBuffer<float>& ioBuf = *info.buffer;
    jassert (info.startSample == 0);

    const auto patch = patchSnapshot.acquire();

    const int numIn    = juce::jmin (numActiveInputs.load (std::memory_order_relaxed),
                                     ioBuf.getNumChannels());
    const int numOutHw = juce::jmin (numActiveOutputs.load (std::memory_order_relaxed),
                                     ioBuf.getNumChannels());
    const int numSpk   = juce::jmin (numSpeakersPrepared.load (std::memory_order_relaxed),
                                     xoa::kMaxSpeakers);

    // The input source writes into inputScratch, allocated to the block size
    // reported at prepareToPlay. DeviceIoCallback already clamps an over-size
    // device block, but AmbiBusAlgorithm defends against one anyway, so the
    // upstream scratch write keeps the same defense: clamp the render to the
    // scratch length (defense-in-depth, no allocation).
    const int n = juce::jmin (info.numSamples, inputScratch.getNumSamples());

    // Hardware-input meters for the patch matrix tinting — read straight off
    // the device rows, BEFORE anything writes.
    for (int h = 0; h < xoa::kMaxHardwareChannels; ++h)
        hwInputPeak[(size_t) h].store (h < numIn ? ioBuf.getMagnitude (h, 0, n) : 0.0f,
                                       std::memory_order_relaxed);

    // Stem gather (WP8 + D43): flattened stem channel k reads the hardware
    // input the input patch routes it from (identity when unpatched at
    // startup), or the internal test feed. Filled whenever a source could
    // exist, so the encoder's one-block ramp-out on deactivation still has
    // audio to fade. The RT stage gates on the published numSources.
    //
    // ORDER IS LOAD-BEARING (§5.1): a buffer row that is an active output
    // aliases the device's own output storage, with the matching input copied
    // into that same row before this call. Every input must be read BEFORE
    // the output scatter below writes any hardware row.
    const bool testStems = stemFeed.load (std::memory_order_relaxed) == StemFeed::test;
    const juce::AudioBuffer<float>* stemsPtr = nullptr;
    int numStems = 0;
    if (testStems || numIn > 0)
    {
        numStems = juce::jmin (patch.numStemChannels, stemScratch.getNumChannels());
        if (testStems)
        {
            const double sr = deviceSampleRate.load (std::memory_order_relaxed);
            const double f0 = 2.0 * juce::MathConstants<double>::pi / (sr > 0.0 ? sr : 48000.0);
            for (int k = 0; k < numStems; ++k)
            {
                float* d = stemScratch.getWritePointer (k);
                const double w = f0 * (220.0 + 40.0 * k);   // distinct tone per stem channel
                for (int j = 0; j < n; ++j)
                    d[j] = 0.2f * (float) std::sin (w * (double) (sceneCounter + j));
            }
        }
        else
        {
            for (int k = 0; k < numStems; ++k)
            {
                float* d = stemScratch.getWritePointer (k);
                const int hw = patch.hwForStemChannel[k];
                if (hw >= 0 && hw < numIn)
                    juce::FloatVectorOperations::copy (d, ioBuf.getReadPointer (hw), n);
                else
                    juce::FloatVectorOperations::clear (d, n);
            }
        }
        stemsPtr = &stemScratch;
    }

    // Per-input stem meters (observation-only): max across the input's span.
    for (int i = 0; i < xoa::kMaxInputs; ++i)
    {
        float peak = 0.0f;
        if (i < patch.numInputs && stemsPtr != nullptr)
            for (int r = 0; r < patch.stemSpan[i]; ++r)
            {
                const int k = patch.stemOffset[i] + r;
                if (k < numStems)
                    peak = juce::jmax (peak, stemScratch.getMagnitude (k, 0, n));
            }
        inputPeak[(size_t) i].store (peak, std::memory_order_relaxed);
    }

    // Decode + comp run in the SPEAKER domain on speakerScratch; the output
    // patch scatters the result onto hardware rows afterwards (D46). The io
    // buffer's input rows are therefore never decode targets, which is what
    // makes the §5.1 aliasing invariant structural rather than accidental.
    juce::AudioBuffer<float> spkBuf (speakerScratch.getArrayOfWritePointers(), numSpk, info.numSamples);
    if (n < info.numSamples)
        spkBuf.clear();   // silence the whole block rather than emit a partial tail

    juce::AudioSourceChannelInfo outInfo (&spkBuf, 0, n);

    if (inputSource.load (std::memory_order_relaxed) == InputSource::testScene)
    {
        float* ptrs[xoa::kNumSHChannels];
        for (int c = 0; c < xoa::kNumSHChannels; ++c)
            ptrs[c] = inputScratch.getWritePointer (c);
        scene::renderScene (xoa::kAmbisonicOrder, sceneCounter, n,
                            deviceSampleRate.load (std::memory_order_relaxed), ptrs);
        algorithm.processBlock (outInfo, inputScratch, xoa::kNumSHChannels, numSpk,
                                stemsPtr, numStems);
    }
    else
    {
        // No HOA source (D48): the published zero-channel BusRtParams makes
        // the gather clear busA; the encoder stage supplies all content.
        algorithm.processBlock (outInfo, inputScratch, 0, numSpk,
                                stemsPtr, numStems);
    }

    // Per-speaker compensation (delay/EQ/gain) on the decoded output, in place.
    speakerComp.processBlock (spkBuf, numSpk, n);

    // Output scatter: speaker s -> hardware row hwForSpeaker[s]; active
    // hardware outputs no speaker feeds are cleared (they would otherwise
    // replay the input that was pre-copied into their row).
    std::fill (hwWritten.begin(), hwWritten.end(), (char) 0);
    for (int s = 0; s < numSpk; ++s)
    {
        const int hw = patch.hwForSpeaker[s];
        if (hw >= 0 && hw < numOutHw)
        {
            juce::FloatVectorOperations::copy (ioBuf.getWritePointer (hw),
                                               spkBuf.getReadPointer (s), n);
            hwWritten[(size_t) hw] = 1;
        }
    }
    for (int h = 0; h < numOutHw; ++h)
        if (! hwWritten[(size_t) h])
            juce::FloatVectorOperations::clear (ioBuf.getWritePointer (h), n);

    // Output test signal (FR-21), injected post-scatter in the HARDWARE
    // domain (D46) with replace-semantics, so the patch matrix's testing mode
    // reaches every open device output — patched or not.
    if (testSignal.isActive())
    {
        juce::AudioBuffer<float> hwOut (ioBuf.getArrayOfWritePointers(), numOutHw, n);
        testSignal.renderNextBlock (hwOut, 0, n);
    }

    // Per-speaker block-peak meters: what actually leaves the device on the
    // speaker's patched output (post-scatter, post-test-signal); 0 when the
    // speaker is unpatched.
    for (int s = 0; s < xoa::kMaxSpeakers; ++s)
    {
        float peak = 0.0f;
        if (s < numSpk)
            if (const int hw = patch.hwForSpeaker[s]; hw >= 0 && hw < numOutHw)
                peak = ioBuf.getMagnitude (hw, 0, n);
        outputPeak[(size_t) s].store (peak, std::memory_order_relaxed);
    }

    sceneCounter += n;
}

} // namespace xoa
