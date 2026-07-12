#pragma once

#include <juce_audio_devices/juce_audio_devices.h>

#include <array>
#include <atomic>
#include <functional>

#include "spatcore/rt/RtSnapshot.h"

#include "XoaConstants.h"
#include "Audio/DecoderRebuildWorker.h"
#include "Audio/FilePlayer.h"
#include "Audio/SpeakerCompParams.h"
#include "Audio/SpeakerCompProcessor.h"
#include "Audio/TestSignalGenerator.h"
#include "DSP/AmbiBusAlgorithm.h"
#include "DSP/AmbiCalculationEngine.h"
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
                    private juce::Timer,
                    private juce::AsyncUpdater
{
public:
    enum class InputSource { file, testScene };

    /** Where the mono-encoder stems come from: device input channels
        (identity-mapped hw ch i -> input i) or a deterministic internal test
        feed (audible without hardware). Runtime-only, not persisted. */
    enum class StemFeed { device, test };

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
    TestSignalGenerator&      getTestSignalGenerator() noexcept { return testSignal; }

    //==========================================================================
    // Input source + file (message thread).
    //==========================================================================
    void setInputSource (InputSource source);
    InputSource getInputSource() const noexcept { return inputSource.load (std::memory_order_relaxed); }

    /** Select the mono-encoder stem source (message thread). */
    void setStemFeed (StemFeed feed) { stemFeed.store (feed, std::memory_order_relaxed); }
    StemFeed getStemFeed() const noexcept { return stemFeed.load (std::memory_order_relaxed); }

    /** The control-side encoder engine (UI/tests drive tick() and parameters
        through the store; this exposes it for the offline harness and tests). */
    AmbiCalculationEngine& getCalculationEngine() noexcept { return calcEngine; }

    /** Open a file, point the input at it, and persist the path. On success
        the bus gather is recomposed for the file's channel count/order. */
    FilePlayer::OpenResult openFile (const juce::File& file);

    /** Force the pending decoder rebuild now, synchronously (startup +
        explicit UI + tests). Invalidates any in-flight async rebuild. */
    void flushDecoderRebuild();

    /** Kick off an asynchronous rebuild on the worker thread (the debounce
        timer path; also a test seam). Returns immediately; the result is
        published on the message thread when design() finishes. */
    void requestAsyncRebuild();

    /** True while a Speakers/Decoder change has armed the debounce but the
        rebuild has not yet run (test seam for the listener -> timer path). */
    bool isDecoderRebuildPending() const noexcept { return isTimerRunning(); }

    /** True while a background decoder design is running (UI "rebuilding..."). */
    bool isDecoderRebuildInFlight() const noexcept { return rebuildInFlight.load (std::memory_order_acquire); }

    /** Block until the background worker is idle, then adopt+publish its result
        now (headless test seam - no message loop needed). */
    void finishPendingAsyncRebuild();

    //==========================================================================
    // Metering / status (any thread; atomics).
    //==========================================================================
    /** POST-compensation block-peak of an output channel (engine-owned meter,
        measured after the per-speaker delay/EQ/gain stage - what actually
        leaves the device). The bus-algorithm meters stay pre-comp. */
    float  getOutputPeakLevel (int channel) const noexcept
    {
        if (channel < 0 || channel >= xoa::kMaxSpeakers)
            return 0.0f;
        return outputPeak[(size_t) channel].load (std::memory_order_relaxed);
    }
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
    const spatcore::rt::RtSnapshot<SpeakerCompRtParams>& speakerCompSource() const noexcept { return speakerCompSnapshot; }

    /** Recompose + publish the per-speaker comp POD now (test seam; also the
        listener path for gain/delay/mute/solo/distance-mode edits). */
    void publishSpeakerComp();

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

    // juce::ValueTree::Listener (Speakers/Decoder subtree). The property route
    // splits work (D17): trim/mute/solo -> cheap comp republish only; EQ ->
    // biquad refresh only; positions -> decoder rebuild AND comp; everything
    // else (decoder props, names) -> decoder rebuild. A count change touches
    // both, so child add/remove/reorder republish comp as well as redesigning.
    void valueTreePropertyChanged (juce::ValueTree&, const juce::Identifier& property) override;
    void valueTreeChildAdded (juce::ValueTree&, juce::ValueTree&) override { onSpeakerStructureChanged(); }
    void valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree&, int) override { onSpeakerStructureChanged(); }
    void valueTreeChildOrderChanged (juce::ValueTree&, int, int) override { onSpeakerStructureChanged(); }
    void valueTreeParentChanged (juce::ValueTree&) override {}

    // juce::Timer (decoder rebuild debounce)
    void timerCallback() override;

    // juce::AsyncUpdater (worker thread -> message thread hop)
    void handleAsyncUpdate() override;

    //==========================================================================
    void publishRotation();
    void publishBusParams();
    void updateSpeakerEq();              // push the 6-band EQ onto the RT biquads
    void onSpeakerStructureChanged();    // count change -> rebuild + comp + EQ
    void markDecoderDirty();
    void rebuildDecoderNow();
    void registerListeners();
    void unregisterListeners();
    void updateReferenceRadius();        // mean speaker radius -> calc engine (r_ref)
    void syncCalcEngineToDevice();       // push device sample rate -> calc engine

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
    AmbiCalculationEngine    calcEngine;   // control-side encoder (owns the live matrices)

    // Background decoder design. rebuildGeneration is a message-thread-only
    // counter; the worker stamps each job with it and handleAsyncUpdate
    // publishes a completed job only if its stamp still matches (a later submit
    // or a synchronous flush bumps the counter and thereby discards stale work).
    DecoderRebuildWorker rebuildWorker { [this] { triggerAsyncUpdate(); } };
    juce::uint64 rebuildGeneration = 0;
    std::atomic<bool> rebuildInFlight { false };

    spatcore::rt::RtSnapshot<rt::RotationRtState> rotationSnapshot;
    spatcore::rt::RtSnapshot<rt::BusRtParams>     busParamsSnapshot;
    spatcore::rt::RtSnapshot<SpeakerCompRtParams> speakerCompSnapshot;
    AmbiBusAlgorithm         algorithm;
    SpeakerCompProcessor     speakerComp;
    TestSignalGenerator      testSignal;

    // Post-comp output meters (what leaves the device), updated per block on the
    // audio thread. Distinct from AmbiBusAlgorithm's pre-comp meters.
    std::array<std::atomic<float>, xoa::kMaxSpeakers> outputPeak {};

    juce::AudioBuffer<float> inputScratch;   // [kMaxFileChannels x block]
    juce::AudioBuffer<float> stemScratch;    // [kMaxInputs x block] mono-encoder stems

    std::atomic<InputSource> inputSource { InputSource::file };
    std::atomic<StemFeed>    stemFeed { StemFeed::device };
    std::atomic<int> fileNumChannels { 0 };
    std::atomic<int> fileDetectedOrder { 0 };

    // Epochs advance on the message thread only (one writer per snapshot).
    juce::uint32 rotationEpoch = 0;
    juce::uint32 busEpoch = 0;
    juce::uint32 speakerCompEpoch = 0;

    std::atomic<double> measuredLatencyMs { 0.0 };
    std::atomic<double> deviceSampleRate { 0.0 };
    std::atomic<int>    deviceBlockSize { 0 };

    juce::int64 sceneCounter = 0;   // audio-thread-owned scene sample position
    bool listenersRegistered = false;
    bool callbackRegistered = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AudioEngine)
};

} // namespace xoa
