/*
  ==============================================================================

    XOA — tenth-order Ambisonics spatial audio processor.
    Minimal shell UI (WP6): transport, rotation dials, decoder pick, meters,
    status line. Deliberately throwaway plain-JUCE - the WFS-DIY kit port and
    the real tabs land in WP10.

    This file is part of XOA, released under the GNU General Public License
    v3.0. See LICENSE for details.

  ==============================================================================
*/

#pragma once

#include <juce_audio_utils/juce_audio_utils.h>

#include <memory>

#include "XoaConstants.h"
#include "Audio/AudioEngine.h"
#include "Audio/TestSignalGenerator.h"
#include "Network/OSCManager.h"
#include "Parameters/XoaFileManager.h"
#include "Parameters/XoaValueTreeState.h"
#include "InputListComponent.h"
#include "SpeakerListComponent.h"

//==============================================================================
class MainComponent : public juce::Component,
                      private juce::Timer
{
public:
    /** commandLine (from Main.cpp) may name an XOA project to load on startup
        and/or carry "--osc" to force-enable the OSC receiver - the headless
        entry point the control-replay harness drives. */
    explicit MainComponent (const juce::String& commandLine = {});
    ~MainComponent() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;

    void openFileDialog();
    void applyLoadedProject (const juce::File& folderOrManifest);   // load + refresh UI
    void applyStartupCommandLine (const juce::String& commandLine);
    void updateSuggestionLabel();
    void refreshStatusLine();

    // Two-way bind a rotary to a Config float param (UI writes the store; store
    // changes - e.g. a future OSC write - move the dial).
    void bindRotary (juce::Slider&, const juce::Identifier& id, double lo, double hi,
                     const juce::String& suffix);
    // Bind a combo (item index == the int param value) to a store param.
    void bindCombo (juce::ComboBox&, const juce::Identifier& id);
    // Bind a toggle to a bool store param (two-way).
    void bindToggle (juce::ToggleButton&, const juce::Identifier& id);

    // Store -> engine -> device (declared in construction order). The OSC
    // manager comes last so it tears down first (before engine/store).
    xoa::XoaValueTreeState store;
    xoa::XoaFileManager    fileManager { store };
    xoa::AudioEngine       engine { store };
    xoa::OSCManager        oscManager { store, engine };

    // Transport.
    juce::TextButton   openButton  { "Open file…" };
    juce::TextButton   playButton  { "Play" };
    juce::TextButton   stopButton  { "Stop" };
    juce::ToggleButton loopButton  { "Loop" };
    juce::ToggleButton sceneButton { "Test scene (order 10)" };
    juce::ToggleButton oscButton   { "OSC receive" };   // WP9: bound to oscEnabled
    juce::Slider       positionSlider { juce::Slider::LinearHorizontal, juce::Slider::NoTextBox };
    juce::Label        fileLabel;

    // Rotation.
    juce::Slider yawSlider, pitchSlider, rollSlider;
    juce::Label  yawLabel { {}, "Yaw" }, pitchLabel { {}, "Pitch" }, rollLabel { {}, "Roll" };

    // Master + content interpretation.
    juce::Slider   masterSlider;
    juce::Label    masterLabel { {}, "Master" };
    juce::ComboBox contentOrderCombo, conventionCombo;

    // Decoder.
    juce::ComboBox decoderTypeCombo, weightingCombo, normalizationCombo;
    juce::Label    suggestionLabel;
    juce::TextButton importWfsButton   { "Import WFS layout…" };
    juce::TextButton loadProjectButton { "Load project…" };

    // Dual-band decode (FR-14).
    juce::ToggleButton dualBandButton { "Dual-band" };
    juce::Slider       crossoverSlider;
    juce::Label        crossoverLabel { {}, "Crossover" };

    // Mono encoders (FR-5/FR-6): master gate, stem source, per-input table.
    juce::ToggleButton monoInputsButton { "Mono encoders" };
    juce::ComboBox     stemFeedCombo;
    juce::Label        stemFeedLabel { {}, "Stems" };
    juce::Viewport     inputViewport;
    std::unique_ptr<InputListComponent> inputList;

    // Per-speaker compensation (FR-15): distance mode + the trim/mute/solo table.
    juce::ComboBox   distanceModeCombo;
    juce::Label      distanceModeLabel { {}, "Distance comp" };
    juce::Viewport   speakerViewport;
    std::unique_ptr<SpeakerListComponent> speakerList;

    // Output test signal (FR-21) — engine-owned, not persisted.
    juce::ComboBox testSignalCombo;
    juce::Slider   testLevelSlider, testFreqSlider, testChannelSlider;
    juce::Label    testSignalLabel { {}, "Test signal" };
    juce::Label    testInfoLabel;

    // Status + device.
    juce::Label statusLabel;
    std::unique_ptr<juce::AudioDeviceSelectorComponent> deviceSelector;
    juce::Rectangle<int> meterArea;   // output peak strip, drawn in paint()

    // Decoder rebuild status (message-thread callback -> read in refreshStatusLine).
    juce::String decoderStatus;

    std::unique_ptr<juce::FileChooser> fileChooser;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};
