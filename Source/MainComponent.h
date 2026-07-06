/*
  ==============================================================================

    XOA — tenth-order Ambisonics spatial audio processor.
    Main component: audio device management + application shell.

    Bootstrap stage: opens the audio device, shows the device selector and a
    status line, and proves the spatcore wiring (the Ambisonics engine plugs
    in here next — see Documentation/XOA-PLAN.md).

    This file is part of XOA, released under the GNU General Public License
    v3.0. See LICENSE for details.

  ==============================================================================
*/

#pragma once

#include <juce_audio_utils/juce_audio_utils.h>

#include "XoaConstants.h"

//==============================================================================
class MainComponent : public juce::AudioAppComponent,
                      private juce::Timer
{
public:
    MainComponent();
    ~MainComponent() override;

    //==============================================================================
    // juce::AudioAppComponent
    void prepareToPlay (int samplesPerBlockExpected, double sampleRate) override;
    void getNextAudioBlock (const juce::AudioSourceChannelInfo& bufferToFill) override;
    void releaseResources() override;

    //==============================================================================
    // juce::Component
    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;

    juce::Label titleLabel;
    juce::Label statusLabel;
    std::unique_ptr<juce::AudioDeviceSelectorComponent> deviceSelector;

    // Written on the audio thread, read on the message thread (timer).
    std::atomic<float> inputPeakLinear { 0.0f };
    std::atomic<double> currentSampleRate { 0.0 };
    std::atomic<int>    currentBlockSize  { 0 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};
