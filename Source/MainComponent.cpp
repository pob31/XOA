/*
  ==============================================================================

    XOA — tenth-order Ambisonics spatial audio processor.

    This file is part of XOA, released under the GNU General Public License
    v3.0. See LICENSE for details.

  ==============================================================================
*/

#include "MainComponent.h"

#include "spatcore/dsp/NumericGuards.h"

//==============================================================================
MainComponent::MainComponent()
{
    titleLabel.setText ("XOA — 10th-order Ambisonics", juce::dontSendNotification);
    titleLabel.setFont (juce::FontOptions (24.0f, juce::Font::bold));
    titleLabel.setJustificationType (juce::Justification::centred);
    addAndMakeVisible (titleLabel);

    statusLabel.setJustificationType (juce::Justification::centred);
    addAndMakeVisible (statusLabel);

    deviceSelector = std::make_unique<juce::AudioDeviceSelectorComponent> (
        deviceManager,
        0, 64,     // input channels: min, max
        0, 64,     // output channels: min, max
        false,     // MIDI in
        false,     // MIDI out
        false,     // stereo-pair view
        false);    // advanced options hidden
    addAndMakeVisible (*deviceSelector);

    setSize (820, 620);

    // 2 in / 2 out to start with; the Ambisonics engine will renegotiate
    // channel counts when it lands.
    setAudioChannels (2, 2);

    startTimerHz (10);
}

MainComponent::~MainComponent()
{
    shutdownAudio();
}

//==============================================================================
void MainComponent::prepareToPlay (int samplesPerBlockExpected, double sampleRate)
{
    currentSampleRate.store (sampleRate);
    currentBlockSize.store (samplesPerBlockExpected);
}

void MainComponent::getNextAudioBlock (const juce::AudioSourceChannelInfo& bufferToFill)
{
    // Track the input peak for the status line, then output silence — the
    // encode → transform → decode pipeline replaces this.
    float peak = 0.0f;

    if (auto* buffer = bufferToFill.buffer)
        for (int ch = 0; ch < buffer->getNumChannels(); ++ch)
            peak = juce::jmax (peak,
                               buffer->getMagnitude (ch, bufferToFill.startSample,
                                                     bufferToFill.numSamples));

    // spatcore's defensive clamp — NaN/Inf-tolerant (and proof of wiring).
    inputPeakLinear.store (spatcore::dsp::WFSHelpers::safeClamp (0.0f, 10.0f, peak));

    bufferToFill.clearActiveBufferRegion();
}

void MainComponent::releaseResources()
{
    currentSampleRate.store (0.0);
    currentBlockSize.store (0);
}

//==============================================================================
void MainComponent::paint (juce::Graphics& g)
{
    g.fillAll (getLookAndFeel().findColour (juce::ResizableWindow::backgroundColourId));
}

void MainComponent::resized()
{
    auto area = getLocalBounds().reduced (12);
    titleLabel .setBounds (area.removeFromTop (40));
    statusLabel.setBounds (area.removeFromTop (28));
    area.removeFromTop (8);
    deviceSelector->setBounds (area);
}

//==============================================================================
void MainComponent::timerCallback()
{
    const auto sr    = currentSampleRate.load();
    const auto block = currentBlockSize.load();
    const auto peak  = inputPeakLinear.load();

    juce::String status;
    status << "order " << xoa::kAmbisonicOrder
           << "  ·  " << xoa::kNumSHChannels << " SH channels (ACN/SN3D)";

    if (sr > 0.0)
        status << "  ·  " << juce::String (sr / 1000.0, 1) << " kHz / "
               << block << " smp"
               << "  ·  in peak " << juce::String (juce::Decibels::gainToDecibels (peak, -90.0f), 1) << " dB";
    else
        status << "  ·  audio device stopped";

    statusLabel.setText (status, juce::dontSendNotification);
}
