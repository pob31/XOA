#pragma once

#include <juce_audio_devices/juce_audio_devices.h>

#include <atomic>
#include <functional>

#include "spatcore/rt/RtSnapshot.h"

#include "XoaConstants.h"
#include "Audio/FilePlayer.h"
#include "DSP/AmbiBusAlgorithm.h"
#include "DSP/AmbiDecoderDesigner.h"
#include "DSP/AmbiRtTypes.h"
#include "DSP/DecoderMatrixBuilder.h"
#include "Parameters/XoaValueTreeState.h"

//==============================================================================
// XOA - the audio engine (WP6). Owns the device layer (spatcore deliberately
// does not) and hosts the RT bus chain, wiring the store to it through three
// message-thread controllers:
//
//   RotationPublisher     rotation params -> RtSnapshot<RotationRtState>
//   BusParamsPublisher    master gain / playback params + input-source ->
//                         RtSnapshot<BusRtParams>
//   DecoderRebuildControl Speakers/Decoder subtree changes -> a 150 ms
//                         debounce -> DecoderMatrixBuilder rebuild + publish
//
// Construction runs the message-thread setup and publishes an initial snapshot
// for each seam (publish-before-enable); openAudioDevice() then starts the
// device (kept separate so headless tests exercise the controllers without
// opening hardware). The audio callback runs under ScopedNoDenormals and the
// three snapshots are acquired once per block by AmbiBusAlgorithm.
//==============================================================================

namespace xoa
{

class AudioEngine : private juce::AudioIODeviceCallback,
                    private juce::ChangeListener,
                    private juce::ValueTree::Listener,
                    private juce::Timer
{
public:
    enum class InputSource { file, testScene };

    explicit AudioEngine (XoaValueTreeState& store);
    ~AudioEngine() override;

    //==========================================================================
    // Device lifecycle (message thread).
    //==========================================================================
    void openAudioDevice();     // restore persisted state, start the callback
    void closeAudioDevice();    // stop the callback, persist state

    juce::AudioDeviceManager& getDeviceManager() noexcept { return deviceManager; }
    FilePlayer&               getFilePlayer()    noexcept { return filePlayer; }
    DecoderMatrixBuilder&     getDecoderBuilder() noexcept { return decoderBuilder; }

    //==========================================================================
    // Input source + file (message thread).
    //==========================================================================
    void setInputSource (InputSource source);
    InputSource getInputSource() const noexcept { return inputSource.load (std::memory_order_relaxed); }

    /** Open a file, point the input at it, and persist the path. On success
        the bus gather is recomposed for the file's channel count/order. */
    FilePlayer::OpenResult openFile (const juce::File& file);

    /** Force the pending decoder rebuild now (startup + tests + explicit UI). */
    void flushDecoderRebuild();

    /** True while a Speakers/Decoder change has armed the debounce but the
        rebuild has not yet run (test seam for the listener -> timer path). */
    bool isDecoderRebuildPending() const noexcept { return isTimerRunning(); }

    //==========================================================================
    // Metering / status (any thread; atomics).
    //==========================================================================
    float  getOutputPeakLevel (int channel) const noexcept { return algorithm.getOutputPeakLevel (channel); }
    double getMeasuredLatencyMs() const noexcept { return measuredLatencyMs.load (std::memory_order_relaxed); }
    double getCpuLoad() const { return deviceManager.getCpuUsage(); }
    double getSampleRate() const noexcept { return deviceSampleRate.load (std::memory_order_relaxed); }
    int    getBlockSize()  const noexcept { return deviceBlockSize.load (std::memory_order_relaxed); }

    /** Invoked (message thread) after each decoder rebuild, for UI status. */
    std::function<void (const decoder::DesignResult&)> onDecoderRebuilt;

    //==========================================================================
    // Test seams (const access to the published snapshots).
    //==========================================================================
    const spatcore::rt::RtSnapshot<rt::RotationRtState>& rotationSource() const noexcept { return rotationSnapshot; }
    const spatcore::rt::RtSnapshot<rt::BusRtParams>&     busParamsSource() const noexcept { return busParamsSnapshot; }

private:
    //==========================================================================
    // juce::AudioIODeviceCallback
    void audioDeviceIOCallbackWithContext (const float* const* inputChannelData, int numInputChannels,
                                           float* const* outputChannelData, int numOutputChannels,
                                           int numSamples,
                                           const juce::AudioIODeviceCallbackContext& context) override;
    void audioDeviceAboutToStart (juce::AudioIODevice* device) override;
    void audioDeviceStopped() override;

    // juce::ChangeListener (device state persistence)
    void changeListenerCallback (juce::ChangeBroadcaster*) override;

    // juce::ValueTree::Listener (Speakers/Decoder subtree -> decoder rebuild)
    void valueTreePropertyChanged (juce::ValueTree&, const juce::Identifier&) override { markDecoderDirty(); }
    void valueTreeChildAdded (juce::ValueTree&, juce::ValueTree&) override { markDecoderDirty(); }
    void valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree&, int) override { markDecoderDirty(); }
    void valueTreeChildOrderChanged (juce::ValueTree&, int, int) override { markDecoderDirty(); }
    void valueTreeParentChanged (juce::ValueTree&) override {}

    // juce::Timer (decoder rebuild debounce)
    void timerCallback() override;

    //==========================================================================
    void publishRotation();
    void publishBusParams();
    void markDecoderDirty();
    void rebuildDecoderNow();
    void registerListeners();
    void unregisterListeners();

    XoaValueTreeState& store;

    // Persistent handles to the two subtrees we listen on. A juce::ValueTree
    // registers listeners against the *instance's* address, so listening on a
    // temporary (getSpeakersSection()) would unregister the moment it dies -
    // these members keep the registration alive for the engine's lifetime.
    juce::ValueTree speakersSection;
    juce::ValueTree decoderSection;

    juce::AudioDeviceManager deviceManager;
    FilePlayer               filePlayer;
    DecoderMatrixBuilder     decoderBuilder;

    spatcore::rt::RtSnapshot<rt::RotationRtState> rotationSnapshot;
    spatcore::rt::RtSnapshot<rt::BusRtParams>     busParamsSnapshot;
    AmbiBusAlgorithm         algorithm;

    juce::AudioBuffer<float> inputScratch;   // [kMaxFileChannels x block]

    std::atomic<InputSource> inputSource { InputSource::file };
    std::atomic<int> fileNumChannels { 0 };
    std::atomic<int> fileDetectedOrder { 0 };

    // Epochs advance on the message thread only (one writer per snapshot).
    juce::uint32 rotationEpoch = 0;
    juce::uint32 busEpoch = 0;

    std::atomic<double> measuredLatencyMs { 0.0 };
    std::atomic<double> deviceSampleRate { 0.0 };
    std::atomic<int>    deviceBlockSize { 0 };

    juce::int64 sceneCounter = 0;   // audio-thread-owned scene sample position
    bool listenersRegistered = false;
    bool callbackRegistered = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AudioEngine)
};

} // namespace xoa
