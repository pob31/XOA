/*
  ==============================================================================

    XOA — tenth-order Ambisonics spatial audio processor.
    AudioInterfaceWindow — the stage-2 Audio Interface window: device info bar,
    Device Settings / Input Patch / Output Patch tabs around the shared
    spatcore patch matrix. XOA-owned shell (deliberately not shared — handoff
    §7), copied in shape from WFS-DIY's AudioInterfaceWindow/AudioPatchTab and
    reduced: one XoaPatchTab class serves both directions. There is no
    processing/transport gate (D49): XOA is a processor whose program comes
    from external players it cannot sense, so the safety net is the explicit
    matrix interaction and the generator's 500 ms protective ramp; test tones
    still stop on every exit path (tab change, mode change, window close).

    This file is part of XOA, released under the GNU General Public License
    v3.0. See LICENSE for details.

  ==============================================================================
*/

#pragma once

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <memory>

#include "Audio/AudioEngine.h"
#include "Parameters/XoaValueTreeState.h"
#include "XoaPatchMatrixShim.h"
#include "../Tabs/TabPage.h"          // AppContext
#include "../Widgets/LongPressButton.h"
#include "../Widgets/XoaStandardSlider.h"

namespace xoa::ui
{

//==============================================================================
/** Read-only device facts across the window top: type, device, sample rate,
    buffer size and the truthful active channel counts (DeviceHost). */
class DeviceInfoBar : public juce::Component,
                      private juce::Timer
{
public:
    explicit DeviceInfoBar (xoa::AudioEngine& engineToUse);
    ~DeviceInfoBar() override;

    void paint (juce::Graphics& g) override;

private:
    void timerCallback() override { repaint(); }

    xoa::AudioEngine& engine;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DeviceInfoBar)
};

//==============================================================================
/** Device type / device / sample rate / buffer size, plus Control Panel and
    Reset Device. No channel selection on purpose: every mutation routes
    through the engine's DeviceHost, whose policy opens ALL channels with
    explicit masks (§2.2). SR/buffer changes go through one local helper that
    re-applies the mask policy afterwards (D39 stays open in spatcore). */
class DeviceSettingsPanel : public juce::Component,
                            private juce::ChangeListener
{
public:
    explicit DeviceSettingsPanel (xoa::AudioEngine& engineToUse);
    ~DeviceSettingsPanel() override;

    void resized() override;

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override { updateAllControls(); }

    void updateAllControls();          // rebuild every combo from the manager
    void applySampleRateOrBuffer();    // setAudioDeviceSetup + re-assert masks

    xoa::AudioEngine& engine;
    juce::AudioDeviceManager& deviceManager;

    juce::Label deviceTypeLabel, deviceLabel, sampleRateLabel, bufferSizeLabel, errorLabel;
    juce::ComboBox deviceTypeCombo, deviceCombo, sampleRateCombo, bufferSizeCombo;
    juce::TextButton controlPanelButton, resetDeviceButton;

    bool isUpdating = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DeviceSettingsPanel)
};

//==============================================================================
/** One patch tab: mode buttons + long-press Unpatch All + the shared matrix.
    The output tab adds Testing mode with inline test-signal controls (type /
    level / frequency with the WFS-DIY non-linear mappings / Hold); the input
    tab runs the 20 Hz header-repaint timer that animates the signal-presence
    tint. */
class XoaPatchTab : public juce::Component,
                    private juce::Timer
{
public:
    XoaPatchTab (AppContext& ctx, bool isInputTab);
    ~XoaPatchTab() override;

    void resized() override;
    void paint (juce::Graphics& g) override;

    /** Back to Scrolling; stops any test tone (tab switch / window close). */
    void resetMode();

    /** Stop the test tone without touching the settings. */
    void stopTestAudio();

    XoaPatchMatrix& getMatrix() { return matrix; }

private:
    void timerCallback() override;
    void setMode (XoaPatchMatrix::Mode mode);
    void applyTestSettings();
    void updateTestControlsVisibility();

    static float sliderValueToDb (float v);
    static float dbToSliderValue (float dB);
    static float sliderValueToFrequency (float v);
    static float frequencyToSliderValue (float hz);

    AppContext& context;
    const bool isInput;

    juce::TextButton scrollingButton, patchingButton, testingButton;
    LongPressButton unpatchAllButton;

    XoaPatchMatrix matrix;

    // Testing controls (output tab only; visible in Testing mode).
    juce::ComboBox signalTypeCombo;
    juce::TextButton holdButton;
    XoaStandardSlider levelSlider, frequencySlider;
    juce::Label levelValueLabel, frequencyValueLabel;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (XoaPatchTab)
};

//==============================================================================
/** Info bar + Device Settings / Input Patch / Output Patch tabs. */
class AudioInterfaceContent : public juce::Component
{
public:
    explicit AudioInterfaceContent (AppContext& ctx);
    ~AudioInterfaceContent() override = default;

    void resized() override;
    void paint (juce::Graphics& g) override;

    /** Stop tones + reset modes (window closing). */
    void windowClosing();

private:
    /** Tab switches must stop a running test tone (§7.2 B exit paths). */
    struct NotifyingTabbedComponent : juce::TabbedComponent
    {
        NotifyingTabbedComponent() : juce::TabbedComponent (juce::TabbedButtonBar::TabsAtTop) {}
        std::function<void (int)> onTabChanged;
        void currentTabChanged (int newIndex, const juce::String&) override
        {
            if (onTabChanged)
                onTabChanged (newIndex);
        }
    };

    AppContext& context;

    DeviceInfoBar infoBar;
    NotifyingTabbedComponent tabs;

    // Owned by the TabbedComponent.
    DeviceSettingsPanel* devicePanel = nullptr;
    XoaPatchTab* inputTab = nullptr;
    XoaPatchTab* outputTab = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AudioInterfaceContent)
};

//==============================================================================
class AudioInterfaceWindow : public juce::DocumentWindow
{
public:
    explicit AudioInterfaceWindow (AppContext& ctx);
    ~AudioInterfaceWindow() override = default;

    void closeButtonPressed() override;

private:
    AudioInterfaceContent* content = nullptr;   // owned by the DocumentWindow

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AudioInterfaceWindow)
};

} // namespace xoa::ui
