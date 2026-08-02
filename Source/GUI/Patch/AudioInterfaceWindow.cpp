#include "AudioInterfaceWindow.h"

#include <cmath>

#include "../ColorScheme.h"
#include "../XoaLookAndFeel.h"
#include "Localization/LocalizationManager.h"

namespace xoa::ui
{

namespace
{
    int px (int v) { return juce::roundToInt ((float) v * XoaLookAndFeel::uiScale); }
}

//==============================================================================
// DeviceInfoBar
//==============================================================================

DeviceInfoBar::DeviceInfoBar (xoa::AudioEngine& engineToUse)
    : engine (engineToUse)
{
    startTimer (500);
}

DeviceInfoBar::~DeviceInfoBar()
{
    stopTimer();
}

void DeviceInfoBar::paint (juce::Graphics& g)
{
    const auto& scheme = ColorScheme::get();
    g.fillAll (scheme.backgroundAlt);

    juce::String text;
    auto& manager = engine.getDeviceManager();
    if (auto* device = manager.getCurrentAudioDevice())
    {
        auto& host = engine.getDeviceHost();
        text << manager.getCurrentAudioDeviceType() << "  ·  " << device->getName()
             << "  ·  " << juce::String (device->getCurrentSampleRate() / 1000.0, 1) << " kHz"
             << "  ·  " << device->getCurrentBufferSizeSamples() << " smp"
             << "  ·  " << LOC ("audioPatch.info.channels")
             << " " << host.getNumActiveInputs() << " / " << host.getNumActiveOutputs();
    }
    else
    {
        text = LOC ("audioPatch.info.noDevice");
        if (engine.getLastDeviceError().isNotEmpty())
            text << "  —  " << engine.getLastDeviceError();
    }

    g.setColour (scheme.textSecondary);
    g.setFont (juce::Font (juce::FontOptions (juce::jmax (11.0f, 13.0f * XoaLookAndFeel::uiScale))));
    g.drawText (text, getLocalBounds().reduced (px (10), 0), juce::Justification::centredLeft);
}

//==============================================================================
// DeviceSettingsPanel
//==============================================================================

DeviceSettingsPanel::DeviceSettingsPanel (xoa::AudioEngine& engineToUse)
    : engine (engineToUse), deviceManager (engineToUse.getDeviceManager())
{
    auto addLabelled = [this] (juce::Label& label, const char* key, juce::ComboBox& combo)
    {
        label.setText (LOC (key), juce::dontSendNotification);
        label.setJustificationType (juce::Justification::centredRight);
        addAndMakeVisible (label);
        addAndMakeVisible (combo);
    };
    addLabelled (deviceTypeLabel, "audioPatch.device.type",       deviceTypeCombo);
    addLabelled (deviceLabel,     "audioPatch.device.device",     deviceCombo);
    addLabelled (sampleRateLabel, "audioPatch.device.sampleRate", sampleRateCombo);
    addLabelled (bufferSizeLabel, "audioPatch.device.bufferSize", bufferSizeCombo);

    controlPanelButton.setButtonText (LOC ("audioPatch.device.controlPanel"));
    resetDeviceButton.setButtonText (LOC ("audioPatch.device.reset"));
    addAndMakeVisible (controlPanelButton);
    addAndMakeVisible (resetDeviceButton);

    errorLabel.setJustificationType (juce::Justification::centredLeft);
    errorLabel.setColour (juce::Label::textColourId, ColorScheme::accents::mute);
    addAndMakeVisible (errorLabel);

    // Every mutation routes through DeviceHost (§2.2): explicit masks, all
    // channels, useDefault* flags cleared.
    deviceTypeCombo.onChange = [this]
    {
        if (isUpdating) return;
        const auto typeName = deviceTypeCombo.getText();
        juce::String deviceName;
        for (auto* type : deviceManager.getAvailableDeviceTypes())
            if (type->getTypeName() == typeName)
            {
                type->scanForDevices();
                const auto names = type->getDeviceNames();
                const int def = juce::jlimit (0, juce::jmax (0, names.size() - 1),
                                              type->getDefaultDeviceIndex (false));
                if (! names.isEmpty())
                    deviceName = names[def];
            }
        errorLabel.setText (engine.getDeviceHost().openNamedDevice (typeName, deviceName),
                            juce::dontSendNotification);
    };

    deviceCombo.onChange = [this]
    {
        if (isUpdating) return;
        errorLabel.setText (engine.getDeviceHost().setDeviceAllChannels (deviceCombo.getText()),
                            juce::dontSendNotification);
    };

    sampleRateCombo.onChange = [this] { if (! isUpdating) applySampleRateOrBuffer(); };
    bufferSizeCombo.onChange = [this] { if (! isUpdating) applySampleRateOrBuffer(); };

    controlPanelButton.onClick = [this]
    {
        if (auto* device = deviceManager.getCurrentAudioDevice())
            if (device->hasControlPanel())
                device->showControlPanel();
    };

    resetDeviceButton.onClick = [this]
    {
        // Full reopen at the device's own defaults, masks re-asserted.
        if (auto* device = deviceManager.getCurrentAudioDevice())
            errorLabel.setText (engine.getDeviceHost().setDeviceAllChannels (device->getName()),
                                juce::dontSendNotification);
    };

    deviceManager.addChangeListener (this);
    updateAllControls();
}

DeviceSettingsPanel::~DeviceSettingsPanel()
{
    deviceManager.removeChangeListener (this);
}

void DeviceSettingsPanel::applySampleRateOrBuffer()
{
    // Mask-safe only because DeviceHost cleared the useDefault* flags in the
    // stored setup; re-assert the enable-all policy afterwards regardless
    // (D39: a spatcore setter enforcing this stays open).
    auto setup = deviceManager.getAudioDeviceSetup();
    setup.sampleRate = sampleRateCombo.getText().getDoubleValue();
    setup.bufferSize = bufferSizeCombo.getText().getIntValue();

    juce::String error = deviceManager.setAudioDeviceSetup (setup, true);
    if (error.isEmpty())
        error = engine.getDeviceHost().enableAllChannels();
    errorLabel.setText (error, juce::dontSendNotification);
}

void DeviceSettingsPanel::updateAllControls()
{
    const juce::ScopedValueSetter<bool> guard (isUpdating, true);

    deviceTypeCombo.clear (juce::dontSendNotification);
    int id = 1;
    for (auto* type : deviceManager.getAvailableDeviceTypes())
        deviceTypeCombo.addItem (type->getTypeName(), id++);
    deviceTypeCombo.setText (deviceManager.getCurrentAudioDeviceType(), juce::dontSendNotification);

    deviceCombo.clear (juce::dontSendNotification);
    for (auto* type : deviceManager.getAvailableDeviceTypes())
        if (type->getTypeName() == deviceManager.getCurrentAudioDeviceType())
        {
            int deviceId = 1;
            for (const auto& name : type->getDeviceNames())
                deviceCombo.addItem (name, deviceId++);
        }

    sampleRateCombo.clear (juce::dontSendNotification);
    bufferSizeCombo.clear (juce::dontSendNotification);

    if (auto* device = deviceManager.getCurrentAudioDevice())
    {
        deviceCombo.setText (device->getName(), juce::dontSendNotification);

        int srId = 1;
        for (const double sr : device->getAvailableSampleRates())
            sampleRateCombo.addItem (juce::String (sr, 0), srId++);
        sampleRateCombo.setText (juce::String (device->getCurrentSampleRate(), 0),
                                 juce::dontSendNotification);

        int bufId = 1;
        for (const int size : device->getAvailableBufferSizes())
            bufferSizeCombo.addItem (juce::String (size), bufId++);
        bufferSizeCombo.setText (juce::String (device->getCurrentBufferSizeSamples()),
                                 juce::dontSendNotification);
    }
}

void DeviceSettingsPanel::resized()
{
    auto area = getLocalBounds().reduced (px (20));
    const int rowH = px (32);
    const int labelW = px (120);
    const int comboW = px (280);

    auto row = [&] (juce::Label& label, juce::ComboBox& combo)
    {
        auto r = area.removeFromTop (rowH);
        label.setBounds (r.removeFromLeft (labelW));
        r.removeFromLeft (px (6));
        combo.setBounds (r.removeFromLeft (comboW).reduced (0, px (3)));
        area.removeFromTop (px (6));
    };
    row (deviceTypeLabel, deviceTypeCombo);
    row (deviceLabel, deviceCombo);
    row (sampleRateLabel, sampleRateCombo);
    row (bufferSizeLabel, bufferSizeCombo);

    area.removeFromTop (px (8));
    auto buttons = area.removeFromTop (rowH);
    buttons.removeFromLeft (labelW + px (6));
    controlPanelButton.setBounds (buttons.removeFromLeft (px (140)).reduced (0, px (2)));
    buttons.removeFromLeft (px (8));
    resetDeviceButton.setBounds (buttons.removeFromLeft (px (140)).reduced (0, px (2)));

    area.removeFromTop (px (10));
    errorLabel.setBounds (area.removeFromTop (rowH));
}

//==============================================================================
// XoaPatchTab
//==============================================================================

XoaPatchTab::XoaPatchTab (AppContext& ctx, bool isInputTab)
    : context (ctx),
      isInput (isInputTab),
      unpatchAllButton (800),
      matrix (ctx.store, isInputTab,
              isInputTab ? nullptr : &ctx.engine.getTestSignalGenerator())
{
    scrollingButton.setButtonText (LOC ("audioPatch.mode.scrolling"));
    patchingButton.setButtonText (LOC ("audioPatch.mode.patching"));
    testingButton.setButtonText (LOC ("audioPatch.mode.testing"));
    unpatchAllButton.setButtonText (LOC ("audioPatch.unpatchAll"));

    scrollingButton.onClick = [this] { setMode (XoaPatchMatrix::Mode::Scrolling); };
    patchingButton.onClick  = [this] { setMode (XoaPatchMatrix::Mode::Patching); };
    testingButton.onClick   = [this] { setMode (XoaPatchMatrix::Mode::Testing); };
    unpatchAllButton.onLongPress = [this] { matrix.clearAllPatches(); };

    addAndMakeVisible (scrollingButton);
    addAndMakeVisible (patchingButton);
    addAndMakeVisible (unpatchAllButton);
    addAndMakeVisible (matrix);

    if (isInput)
    {
        // Signal-presence tint: per-hardware-input peaks off the engine,
        // animated by the 20 Hz header repaint.
        matrix.setHardwareInputPeakProvider (
            [&engine = context.engine] (int hw) { return engine.getHwInputPeakLevel (hw); });
        startTimerHz (20);
    }
    else
    {
        addAndMakeVisible (testingButton);

        // Inline test controls (Testing mode only). Combo ids are 1-based on
        // the enum ordinals (1 = Off .. 5 = DiracPulse); SpeakerId stays on
        // the SpeakersDecoderTab, where the announced index means a speaker.
        signalTypeCombo.addItem (LOC ("enum.testSignal.off"),   1);
        signalTypeCombo.addItem (LOC ("enum.testSignal.pink"),  2);
        signalTypeCombo.addItem (LOC ("enum.testSignal.tone"),  3);
        signalTypeCombo.addItem (LOC ("enum.testSignal.sweep"), 4);
        signalTypeCombo.addItem (LOC ("enum.testSignal.dirac"), 5);
        signalTypeCombo.setSelectedId (2, juce::dontSendNotification);
        signalTypeCombo.onChange = [this] { applyTestSettings(); };
        addChildComponent (signalTypeCombo);

        holdButton.setButtonText (LOC ("audioPatch.test.hold"));
        holdButton.setClickingTogglesState (true);
        holdButton.onClick = [this]
        {
            auto& gen = context.engine.getTestSignalGenerator();
            gen.setHoldEnabled (holdButton.getToggleState());
            if (! holdButton.getToggleState())
                stopTestAudio();
        };
        addChildComponent (holdButton);

        // Non-linear mappings (§7.2 B): perceptual level curve to a -92 dB
        // floor, log frequency across 20 Hz - 20 kHz.
        auto& gen = context.engine.getTestSignalGenerator();
        levelSlider.setValue (dbToSliderValue (gen.getLevelDb()));
        levelSlider.onValueChanged = [this] (float v)
        {
            const float dB = sliderValueToDb (v);
            context.engine.getTestSignalGenerator().setLevel (dB);
            levelValueLabel.setText (juce::String (dB, 1) + " " + LOC ("units.db"),
                                     juce::dontSendNotification);
        };
        addChildComponent (levelSlider);
        addChildComponent (levelValueLabel);

        frequencySlider.setValue (frequencyToSliderValue (gen.getFrequency()));
        frequencySlider.onValueChanged = [this] (float v)
        {
            const float hz = sliderValueToFrequency (v);
            context.engine.getTestSignalGenerator().setFrequency (hz);
            frequencyValueLabel.setText (juce::String (juce::roundToInt (hz)) + " " + LOC ("units.hz"),
                                         juce::dontSendNotification);
        };
        addChildComponent (frequencySlider);
        addChildComponent (frequencyValueLabel);
    }

    setMode (XoaPatchMatrix::Mode::Scrolling);
}

XoaPatchTab::~XoaPatchTab()
{
    stopTimer();
    stopTestAudio();
}

void XoaPatchTab::timerCallback()
{
    matrix.repaintHeaderBand();
}

void XoaPatchTab::setMode (XoaPatchMatrix::Mode mode)
{
    if (mode == XoaPatchMatrix::Mode::Testing && isInput)
        return;   // testing is output-only by convention

    matrix.setMode (mode);   // the matrix stops its own test tone when leaving Testing

    scrollingButton.setToggleState (mode == XoaPatchMatrix::Mode::Scrolling, juce::dontSendNotification);
    patchingButton.setToggleState (mode == XoaPatchMatrix::Mode::Patching, juce::dontSendNotification);
    testingButton.setToggleState (mode == XoaPatchMatrix::Mode::Testing, juce::dontSendNotification);
    updateTestControlsVisibility();

    if (mode != XoaPatchMatrix::Mode::Scrolling)
        matrix.grabKeyboardFocus();
}

void XoaPatchTab::updateTestControlsVisibility()
{
    const bool testing = ! isInput && matrix.getMode() == XoaPatchMatrix::Mode::Testing;
    signalTypeCombo.setVisible (testing);
    holdButton.setVisible (testing);
    levelSlider.setVisible (testing);
    levelValueLabel.setVisible (testing);

    const bool tone = testing
                       && signalTypeCombo.getSelectedId() == 3;   // Tone
    frequencySlider.setVisible (tone);
    frequencyValueLabel.setVisible (tone);
}

void XoaPatchTab::applyTestSettings()
{
    auto& gen = context.engine.getTestSignalGenerator();
    const int selected = signalTypeCombo.getSelectedId();
    gen.setSignalType ((xoa::TestSignalGenerator::SignalType) juce::jmax (0, selected - 1));
    if (selected <= 1)
        stopTestAudio();
    updateTestControlsVisibility();
}

void XoaPatchTab::stopTestAudio()
{
    context.engine.getTestSignalGenerator().setOutputChannel (-1);
}

void XoaPatchTab::resetMode()
{
    stopTestAudio();
    setMode (XoaPatchMatrix::Mode::Scrolling);
}

float XoaPatchTab::sliderValueToDb (float v)
{
    // dB = 20·log10(floor + (1 − floor)·v²), floor = 10^(-92/20) — perceptual
    // taper that still reaches a true floor (§7.2 B).
    const float floorLin = std::pow (10.0f, -92.0f / 20.0f);
    const float lin = floorLin + (1.0f - floorLin) * v * v;
    return juce::jlimit (-92.0f, 0.0f, 20.0f * std::log10 (lin));
}

float XoaPatchTab::dbToSliderValue (float dB)
{
    const float floorLin = std::pow (10.0f, -92.0f / 20.0f);
    const float lin = std::pow (10.0f, juce::jlimit (-92.0f, 0.0f, dB) / 20.0f);
    return std::sqrt (juce::jmax (0.0f, (lin - floorLin) / (1.0f - floorLin)));
}

float XoaPatchTab::sliderValueToFrequency (float v)
{
    return 20.0f * std::pow (10.0f, 3.0f * juce::jlimit (0.0f, 1.0f, v));   // 20 Hz - 20 kHz log
}

float XoaPatchTab::frequencyToSliderValue (float hz)
{
    return std::log10 (juce::jlimit (20.0f, 20000.0f, hz) / 20.0f) / 3.0f;
}

void XoaPatchTab::paint (juce::Graphics& g)
{
    g.fillAll (ColorScheme::get().background);
}

void XoaPatchTab::resized()
{
    auto area = getLocalBounds().reduced (px (8));

    auto bar = area.removeFromTop (px (30));
    const int buttonW = px (100);
    scrollingButton.setBounds (bar.removeFromLeft (buttonW).reduced (px (2), 0));
    patchingButton.setBounds (bar.removeFromLeft (buttonW).reduced (px (2), 0));
    if (! isInput)
        testingButton.setBounds (bar.removeFromLeft (buttonW).reduced (px (2), 0));
    unpatchAllButton.setBounds (bar.removeFromRight (px (120)));

    if (! isInput)
    {
        auto testBar = area.removeFromTop (px (30));
        signalTypeCombo.setBounds (testBar.removeFromLeft (px (130)).reduced (px (2), px (3)));
        holdButton.setBounds (testBar.removeFromLeft (px (70)).reduced (px (2), px (3)));
        levelSlider.setBounds (testBar.removeFromLeft (px (160)).reduced (px (2), px (6)));
        levelValueLabel.setBounds (testBar.removeFromLeft (px (70)));
        frequencySlider.setBounds (testBar.removeFromLeft (px (160)).reduced (px (2), px (6)));
        frequencyValueLabel.setBounds (testBar.removeFromLeft (px (70)));
    }

    area.removeFromTop (px (4));
    matrix.setBounds (area);
}

//==============================================================================
// AudioInterfaceContent
//==============================================================================

AudioInterfaceContent::AudioInterfaceContent (AppContext& ctx)
    : context (ctx), infoBar (ctx.engine)
{
    addAndMakeVisible (infoBar);

    const auto tabBg = ColorScheme::get().backgroundAlt;
    devicePanel = new DeviceSettingsPanel (ctx.engine);
    inputTab    = new XoaPatchTab (ctx, true);
    outputTab   = new XoaPatchTab (ctx, false);
    tabs.addTab (LOC ("audioPatch.tabs.device"),      tabBg, devicePanel, true);
    tabs.addTab (LOC ("audioPatch.tabs.inputPatch"),  tabBg, inputTab, true);
    tabs.addTab (LOC ("audioPatch.tabs.outputPatch"), tabBg, outputTab, true);
    tabs.onTabChanged = [this] (int)
    {
        // Leaving a tab stops its tone and drops back to Scrolling.
        if (outputTab != nullptr) outputTab->resetMode();
        if (inputTab != nullptr)  inputTab->resetMode();
    };
    addAndMakeVisible (tabs);
}

void AudioInterfaceContent::windowClosing()
{
    if (outputTab != nullptr) outputTab->resetMode();
    if (inputTab != nullptr)  inputTab->resetMode();
}

void AudioInterfaceContent::paint (juce::Graphics& g)
{
    g.fillAll (ColorScheme::get().background);
}

void AudioInterfaceContent::resized()
{
    auto area = getLocalBounds();
    infoBar.setBounds (area.removeFromTop (px (28)));
    tabs.setBounds (area);
}

//==============================================================================
// AudioInterfaceWindow
//==============================================================================

AudioInterfaceWindow::AudioInterfaceWindow (AppContext& ctx)
    : juce::DocumentWindow (LOC ("audioPatch.window.title"),
                            ColorScheme::get().background,
                            juce::DocumentWindow::allButtons)
{
    setUsingNativeTitleBar (true);
    content = new AudioInterfaceContent (ctx);
    setContentOwned (content, false);
    setResizable (true, true);
    setResizeLimits (720, 480, 4096, 4096);
    centreWithSize (px (1100), px (720));
}

void AudioInterfaceWindow::closeButtonPressed()
{
    // A test tone must never outlive the window that started it (§7.2 B).
    if (content != nullptr)
        content->windowClosing();
    setVisible (false);
}

} // namespace xoa::ui
