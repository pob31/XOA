/*
  ==============================================================================

    XOA — tenth-order Ambisonics spatial audio processor.
    Minimal shell UI (WP6) — see MainComponent.h.

    This file is part of XOA, released under the GNU General Public License
    v3.0. See LICENSE for details.

  ==============================================================================
*/

#include "MainComponent.h"

#include "DSP/AmbiDecoderDesigner.h"
#include "DSP/DecoderMatrixBuilder.h"
#include "Parameters/XoaParameterIDs.h"

namespace ids = xoa::ids;

//==============================================================================
MainComponent::MainComponent()
{
    // --- Transport --------------------------------------------------------
    addAndMakeVisible (openButton);
    addAndMakeVisible (playButton);
    addAndMakeVisible (stopButton);
    addAndMakeVisible (loopButton);
    addAndMakeVisible (sceneButton);
    addAndMakeVisible (positionSlider);
    addAndMakeVisible (fileLabel);
    fileLabel.setText ("No file loaded — use the test scene or open an AmbiX file.",
                       juce::dontSendNotification);

    openButton.onClick = [this] { openFileDialog(); };
    playButton.onClick = [this]
    {
        sceneButton.setToggleState (false, juce::dontSendNotification);
        engine.setInputSource (xoa::AudioEngine::InputSource::file);
        engine.getFilePlayer().play();
    };
    stopButton.onClick = [this] { engine.getFilePlayer().stop(); };
    loopButton.onClick = [this] { engine.getFilePlayer().setLooping (loopButton.getToggleState()); };
    sceneButton.onClick = [this]
    {
        const bool scene = sceneButton.getToggleState();
        engine.setInputSource (scene ? xoa::AudioEngine::InputSource::testScene
                                     : xoa::AudioEngine::InputSource::file);
    };
    positionSlider.setRange (0.0, 1.0, 0.01);
    positionSlider.onValueChange = [this]
    {
        if (positionSlider.isMouseButtonDown())
            engine.getFilePlayer().seekSeconds (positionSlider.getValue());
    };

    // --- Rotation dials ---------------------------------------------------
    for (auto* l : { &yawLabel, &pitchLabel, &rollLabel })
    {
        l->setJustificationType (juce::Justification::centred);
        addAndMakeVisible (*l);
    }
    bindRotary (yawSlider,   ids::rotationYaw,   -180.0, 180.0, " deg");
    bindRotary (pitchSlider, ids::rotationPitch,  -90.0,  90.0, " deg");
    bindRotary (rollSlider,  ids::rotationRoll,  -180.0, 180.0, " deg");

    // --- Master + content -------------------------------------------------
    masterLabel.setJustificationType (juce::Justification::centredRight);
    addAndMakeVisible (masterLabel);
    masterSlider.setSliderStyle (juce::Slider::LinearHorizontal);
    masterSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 64, 20);
    masterSlider.setRange (-60.0, 12.0, 0.1);
    masterSlider.setTextValueSuffix (" dB");
    masterSlider.setValue (store.getFloatParameter (ids::masterGain), juce::dontSendNotification);
    masterSlider.onValueChange = [this] { store.setParameter (ids::masterGain, masterSlider.getValue()); };
    store.addParameterListener (ids::masterGain, [this] (const juce::var& v)
    {
        masterSlider.setValue ((double) v, juce::dontSendNotification);
    });
    addAndMakeVisible (masterSlider);

    contentOrderCombo.addItem ("Auto", 1);   // value 0
    for (int n = 1; n <= xoa::kAmbisonicOrder; ++n)
        contentOrderCombo.addItem ("Order " + juce::String (n), n + 1);
    bindCombo (contentOrderCombo, ids::playbackContentOrder);

    conventionCombo.addItem ("AmbiX (SN3D)", 1);
    conventionCombo.addItem ("N3D", 2);
    conventionCombo.addItem ("FuMa (≤3)", 3);
    bindCombo (conventionCombo, ids::playbackConvention);

    // --- Decoder ----------------------------------------------------------
    decoderTypeCombo.addItem ("Sampling (SAD)", 1);
    decoderTypeCombo.addItem ("Mode-matching", 2);
    decoderTypeCombo.addItem ("AllRAD (WP7)", 3);
    bindCombo (decoderTypeCombo, ids::decoderType);

    weightingCombo.addItem ("Basic", 1);
    weightingCombo.addItem ("Max-rE", 2);
    bindCombo (weightingCombo, ids::decoderWeighting);

    normalizationCombo.addItem ("Amplitude", 1);
    normalizationCombo.addItem ("Energy", 2);
    bindCombo (normalizationCombo, ids::decoderNormalization);

    suggestionLabel.setJustificationType (juce::Justification::centredLeft);
    addAndMakeVisible (suggestionLabel);
    addAndMakeVisible (importWfsButton);
    addAndMakeVisible (loadProjectButton);

    importWfsButton.onClick = [this]
    {
        fileChooser = std::make_unique<juce::FileChooser> ("Import a WFS-DIY outputs.xml",
                                                           juce::File(), "*.xml");
        fileChooser->launchAsync (juce::FileBrowserComponent::openMode
                                      | juce::FileBrowserComponent::canSelectFiles,
                                  [this] (const juce::FileChooser& fc)
        {
            const auto f = fc.getResult();
            if (f == juce::File()) return;
            fileManager.importWfsSpeakerLayout (f);
            engine.flushDecoderRebuild();
            updateSuggestionLabel();
            speakerList->rebuildRows();   // speaker count may have changed
            resized();
        });
    };
    loadProjectButton.onClick = [this]
    {
        fileChooser = std::make_unique<juce::FileChooser> ("Load an XOA project folder",
                                                           juce::File(), "*");
        fileChooser->launchAsync (juce::FileBrowserComponent::openMode
                                      | juce::FileBrowserComponent::canSelectDirectories,
                                  [this] (const juce::FileChooser& fc)
        {
            const auto f = fc.getResult();
            if (f == juce::File()) return;
            fileManager.loadProject (f);
            engine.flushDecoderRebuild();
            updateSuggestionLabel();
            speakerList->rebuildRows();   // speaker count may have changed
            resized();
        });
    };

    // --- Dual-band decode (FR-14) -----------------------------------------
    bindToggle (dualBandButton, ids::decoderDualBandEnabled);
    crossoverLabel.setJustificationType (juce::Justification::centredRight);
    addAndMakeVisible (crossoverLabel);
    crossoverSlider.setSliderStyle (juce::Slider::LinearHorizontal);
    crossoverSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 64, 20);
    crossoverSlider.setRange (80.0, 2000.0, 1.0);
    crossoverSlider.setSkewFactorFromMidPoint (400.0);
    crossoverSlider.setTextValueSuffix (" Hz");
    crossoverSlider.setValue (store.getFloatParameter (ids::decoderCrossoverFrequency),
                              juce::dontSendNotification);
    crossoverSlider.onValueChange = [this]
    {
        store.setParameter (ids::decoderCrossoverFrequency, crossoverSlider.getValue());
    };
    store.addParameterListener (ids::decoderCrossoverFrequency, [this] (const juce::var& v)
    {
        crossoverSlider.setValue ((double) v, juce::dontSendNotification);
    });
    addAndMakeVisible (crossoverSlider);

    // --- Per-speaker compensation (FR-15) ---------------------------------
    distanceModeLabel.setJustificationType (juce::Justification::centredRight);
    addAndMakeVisible (distanceModeLabel);
    distanceModeCombo.addItem ("Off", 1);            // value 0
    distanceModeCombo.addItem ("Delay", 2);          // value 1
    distanceModeCombo.addItem ("Delay + gain", 3);   // value 2
    bindCombo (distanceModeCombo, ids::distanceCompMode);

    speakerList = std::make_unique<SpeakerListComponent> (store);
    speakerViewport.setViewedComponent (speakerList.get(), false);
    speakerViewport.setScrollBarsShown (true, false);
    addAndMakeVisible (speakerViewport);
    speakerList->rebuildRows();

    // --- Output test signal (FR-21) ---------------------------------------
    testSignalLabel.setJustificationType (juce::Justification::centredRight);
    addAndMakeVisible (testSignalLabel);
    testSignalCombo.addItem ("Off", 1);
    testSignalCombo.addItem ("Pink noise", 2);
    testSignalCombo.addItem ("Tone", 3);
    testSignalCombo.addItem ("Sweep", 4);
    testSignalCombo.addItem ("Dirac", 5);
    testSignalCombo.addItem ("Speaker ID", 6);
    testSignalCombo.setSelectedId (1, juce::dontSendNotification);
    testSignalCombo.onChange = [this]
    {
        engine.getTestSignalGenerator().setSignalType (
            static_cast<xoa::TestSignalGenerator::SignalType> (testSignalCombo.getSelectedId() - 1));
    };
    addAndMakeVisible (testSignalCombo);

    testLevelSlider.setSliderStyle (juce::Slider::LinearHorizontal);
    testLevelSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 56, 18);
    testLevelSlider.setRange (-92.0, 0.0, 0.1);
    testLevelSlider.setTextValueSuffix (" dB");
    testLevelSlider.setValue (-40.0, juce::dontSendNotification);
    testLevelSlider.onValueChange = [this]
    {
        engine.getTestSignalGenerator().setLevel ((float) testLevelSlider.getValue());
    };
    addAndMakeVisible (testLevelSlider);

    testFreqSlider.setSliderStyle (juce::Slider::LinearHorizontal);
    testFreqSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 60, 18);
    testFreqSlider.setRange (20.0, 20000.0, 1.0);
    testFreqSlider.setSkewFactorFromMidPoint (1000.0);
    testFreqSlider.setTextValueSuffix (" Hz");
    testFreqSlider.setValue (1000.0, juce::dontSendNotification);
    testFreqSlider.onValueChange = [this]
    {
        engine.getTestSignalGenerator().setFrequency ((float) testFreqSlider.getValue());
    };
    addAndMakeVisible (testFreqSlider);

    testChannelSlider.setSliderStyle (juce::Slider::IncDecButtons);
    testChannelSlider.setTextBoxStyle (juce::Slider::TextBoxLeft, false, 44, 18);
    testChannelSlider.setRange (0.0, (double) (xoa::kMaxSpeakers - 1), 1.0);
    testChannelSlider.setValue (0.0, juce::dontSendNotification);
    testChannelSlider.onValueChange = [this]
    {
        engine.getTestSignalGenerator().setOutputChannel ((int) testChannelSlider.getValue());
    };
    addAndMakeVisible (testChannelSlider);

    testInfoLabel.setJustificationType (juce::Justification::centredLeft);
    addAndMakeVisible (testInfoLabel);

    // Seed the generator's non-schema state to match the controls above.
    engine.getTestSignalGenerator().setLevel (-40.0f);
    engine.getTestSignalGenerator().setFrequency (1000.0f);
    engine.getTestSignalGenerator().setOutputChannel (0);

    // --- Status + device --------------------------------------------------
    statusLabel.setJustificationType (juce::Justification::centredLeft);
    addAndMakeVisible (statusLabel);

    engine.onDecoderRebuilt = [this] (const xoa::decoder::DesignResult& r)
    {
        juce::String s;
        s << "decoder: order " << r.designOrder;
        if (r.conditionWarning)
            s << "  ·  κ=" << juce::String (r.conditionNumber, 1) << " (ill-conditioned)";
        if (! r.warnings.isEmpty())
            s << "  ·  " << r.warnings.joinIntoString ("; ");
        decoderStatus = s;
    };

    deviceSelector = std::make_unique<juce::AudioDeviceSelectorComponent> (
        engine.getDeviceManager(),
        0, xoa::kMaxInputs,         // mono-encoder stems (WP8): up to kMaxInputs
        0, xoa::kMaxSpeakers,       // up to 256 outputs (FR-20)
        false, false, false, false);
    addAndMakeVisible (*deviceSelector);

    updateSuggestionLabel();
    engine.openAudioDevice();

    setSize (1000, 940);
    startTimerHz (25);
}

MainComponent::~MainComponent()
{
    engine.closeAudioDevice();
}

//==============================================================================
void MainComponent::bindRotary (juce::Slider& s, const juce::Identifier& id,
                                double lo, double hi, const juce::String& suffix)
{
    s.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    s.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 60, 18);
    s.setRange (lo, hi, 0.1);
    s.setTextValueSuffix (suffix);
    s.setValue (store.getFloatParameter (id), juce::dontSendNotification);
    s.onValueChange = [this, &s, id] { store.setParameter (id, s.getValue()); };
    store.addParameterListener (id, [&s] (const juce::var& v)
    {
        s.setValue ((double) v, juce::dontSendNotification);
    });
    addAndMakeVisible (s);
}

void MainComponent::bindCombo (juce::ComboBox& c, const juce::Identifier& id)
{
    c.setSelectedId (store.getIntParameter (id) + 1, juce::dontSendNotification);
    c.onChange = [this, &c, id] { store.setParameter (id, c.getSelectedId() - 1); };
    store.addParameterListener (id, [&c] (const juce::var& v)
    {
        c.setSelectedId ((int) v + 1, juce::dontSendNotification);
    });
    addAndMakeVisible (c);
}

void MainComponent::bindToggle (juce::ToggleButton& b, const juce::Identifier& id)
{
    b.setToggleState (static_cast<bool> (store.getParameter (id)), juce::dontSendNotification);
    b.onClick = [this, &b, id] { store.setParameter (id, b.getToggleState()); };
    store.addParameterListener (id, [&b] (const juce::var& v)
    {
        b.setToggleState (static_cast<bool> (v), juce::dontSendNotification);
    });
    addAndMakeVisible (b);
}

//==============================================================================
void MainComponent::openFileDialog()
{
    juce::String patterns = "*.wav;*.flac";
   #if JUCE_MAC
    patterns += ";*.caf";
   #endif

    fileChooser = std::make_unique<juce::FileChooser> ("Open an Ambisonics file",
                                                       juce::File(), patterns);
    fileChooser->launchAsync (juce::FileBrowserComponent::openMode
                                  | juce::FileBrowserComponent::canSelectFiles,
                              [this] (const juce::FileChooser& fc)
    {
        const auto file = fc.getResult();
        if (file == juce::File())
            return;

        const auto r = engine.openFile (file);
        if (r.ok)
        {
            juce::String text;
            text << file.getFileName() << "  (" << r.numChannels << " ch, "
                 << juce::String (r.fileSampleRate / 1000.0, 1) << " kHz, order "
                 << r.detectedOrder << ")";
            if (! r.warnings.isEmpty())
                text << "  ·  " << r.warnings.joinIntoString ("; ");
            fileLabel.setText (text, juce::dontSendNotification);

            positionSlider.setRange (0.0, juce::jmax (0.001, engine.getFilePlayer().getLengthSeconds()), 0.01);
            sceneButton.setToggleState (false, juce::dontSendNotification);
        }
        else
        {
            fileLabel.setText ("Error: " + r.error, juce::dontSendNotification);
        }
    });
}

void MainComponent::updateSuggestionLabel()
{
    const auto layout = xoa::DecoderMatrixBuilder::layoutFromStore (store);
    const auto c = xoa::decoder::classify (layout);

    const char* klass = "irregular";
    switch (c.layoutClass)
    {
        case xoa::decoder::LayoutClass::ring:   klass = "ring";   break;
        case xoa::decoder::LayoutClass::dome:   klass = "dome";   break;
        case xoa::decoder::LayoutClass::sphere: klass = "sphere"; break;
        case xoa::decoder::LayoutClass::irregular: default: klass = "irregular"; break;
    }
    const char* suggest = c.suggestedDecoderType == 0 ? "SAD"
                        : c.suggestedDecoderType == 1 ? "Mode-matching" : "AllRAD";

    // Mean speaker radius -> suggested dual-band crossover (UI hint only, D16).
    double meanR = 0.0;
    for (int s = 0; s < layout.count; ++s)
    {
        const auto& p = layout.positions[s];
        meanR += std::sqrt (p.x * p.x + p.y * p.y + p.z * p.z);
    }
    if (layout.count > 0) meanR /= (double) layout.count;
    const double suggestedHz = xoa::decoder::suggestedCrossoverHz (meanR);

    suggestionLabel.setText (juce::String (layout.count) + " speakers · detected " + klass
                                 + " · suggested: " + suggest
                                 + " · crossover ~" + juce::String (juce::roundToInt (suggestedHz))
                                 + " Hz (override free)",
                             juce::dontSendNotification);
}

void MainComponent::refreshStatusLine()
{
    const double sr    = engine.getSampleRate();
    const int    block = engine.getBlockSize();

    juce::String s;
    s << "order " << xoa::kAmbisonicOrder << " · " << xoa::kNumSHChannels << " SH (ACN/SN3D)";
    if (sr > 0.0)
        s << "  ·  " << juce::String (sr / 1000.0, 1) << " kHz / " << block << " smp"
          << "  ·  latency " << juce::String (engine.getMeasuredLatencyMs(), 1) << " ms"
          << "  ·  CPU " << juce::String (engine.getCpuLoad() * 100.0, 1) << " %";
    else
        s << "  ·  audio device stopped";
    if (decoderStatus.isNotEmpty())
        s << "  ·  " << decoderStatus;
    if (engine.isDecoderRebuildInFlight())
        s << "  ·  rebuilding…";

    statusLabel.setText (s, juce::dontSendNotification);
}

//==============================================================================
void MainComponent::timerCallback()
{
    // Track playback position when the user isn't scrubbing.
    if (! positionSlider.isMouseButtonDown())
        positionSlider.setValue (engine.getFilePlayer().getPositionSeconds(),
                                 juce::dontSendNotification);

    // Test-signal feedback: which speaker is under test (SpeakerId), or the
    // target channel for the other modes.
    auto& gen = engine.getTestSignalGenerator();
    if (gen.getSignalType() == xoa::TestSignalGenerator::SignalType::SpeakerId)
    {
        const int spk = gen.getCurrentSpeakerIndex();
        testInfoLabel.setText (spk >= 0 ? "testing Spk " + juce::String (spk + 1) : "(gap)",
                               juce::dontSendNotification);
    }
    else if (gen.isActive())
    {
        testInfoLabel.setText ("-> ch " + juce::String (gen.getOutputChannel()),
                               juce::dontSendNotification);
    }
    else
    {
        testInfoLabel.setText ({}, juce::dontSendNotification);
    }

    refreshStatusLine();
    repaint (meterArea);
}

//==============================================================================
void MainComponent::paint (juce::Graphics& g)
{
    g.fillAll (getLookAndFeel().findColour (juce::ResizableWindow::backgroundColourId));

    // Output peak strip: first min(24, layout) channels, -60..0 dB.
    g.setColour (juce::Colours::black.withAlpha (0.25f));
    g.fillRect (meterArea);

    const int shown = juce::jmin (24, xoa::kDefaultSpeakers);
    if (shown <= 0 || meterArea.isEmpty())
        return;

    const float barW = (float) meterArea.getWidth() / (float) shown;
    for (int c = 0; c < shown; ++c)
    {
        const float peak = engine.getOutputPeakLevel (c);
        const float db = juce::Decibels::gainToDecibels (peak, -60.0f);
        const float norm = juce::jlimit (0.0f, 1.0f, (db + 60.0f) / 60.0f);

        auto bar = juce::Rectangle<float> (meterArea.getX() + (float) c * barW + 1.0f,
                                           (float) meterArea.getBottom() - norm * (float) meterArea.getHeight(),
                                           barW - 2.0f,
                                           norm * (float) meterArea.getHeight());
        g.setColour (norm > 0.9f ? juce::Colours::orangered : juce::Colours::limegreen);
        g.fillRect (bar);
    }
}

void MainComponent::resized()
{
    auto area = getLocalBounds().reduced (12);

    auto row = [&area] (int h) { return area.removeFromTop (h); };

    // Transport row.
    {
        auto r = row (30);
        openButton .setBounds (r.removeFromLeft (110)); r.removeFromLeft (6);
        playButton .setBounds (r.removeFromLeft (70));  r.removeFromLeft (6);
        stopButton .setBounds (r.removeFromLeft (70));  r.removeFromLeft (12);
        loopButton .setBounds (r.removeFromLeft (70));  r.removeFromLeft (12);
        sceneButton.setBounds (r.removeFromLeft (170));
    }
    area.removeFromTop (6);
    fileLabel.setBounds (row (22));
    positionSlider.setBounds (row (24));
    area.removeFromTop (10);

    // Rotation dials.
    {
        auto r = row (110);
        const int w = r.getWidth() / 3;
        auto place = [] (juce::Rectangle<int> cell, juce::Label& lab, juce::Slider& dial)
        {
            lab.setBounds (cell.removeFromTop (18));
            dial.setBounds (cell);
        };
        place (r.removeFromLeft (w), yawLabel,   yawSlider);
        place (r.removeFromLeft (w), pitchLabel, pitchSlider);
        place (r,                    rollLabel,  rollSlider);
    }
    area.removeFromTop (8);

    // Master + content interpretation.
    {
        auto r = row (26);
        masterLabel.setBounds (r.removeFromLeft (60)); r.removeFromLeft (6);
        masterSlider.setBounds (r.removeFromLeft (260)); r.removeFromLeft (12);
        contentOrderCombo.setBounds (r.removeFromLeft (120)); r.removeFromLeft (6);
        conventionCombo.setBounds (r.removeFromLeft (140));
    }
    area.removeFromTop (10);

    // Decoder row.
    {
        auto r = row (26);
        decoderTypeCombo  .setBounds (r.removeFromLeft (150)); r.removeFromLeft (6);
        weightingCombo    .setBounds (r.removeFromLeft (110)); r.removeFromLeft (6);
        normalizationCombo.setBounds (r.removeFromLeft (120)); r.removeFromLeft (12);
        importWfsButton   .setBounds (r.removeFromLeft (150)); r.removeFromLeft (6);
        loadProjectButton .setBounds (r.removeFromLeft (130));
    }
    suggestionLabel.setBounds (row (22));
    area.removeFromTop (8);

    // Dual-band row.
    {
        auto r = row (26);
        dualBandButton .setBounds (r.removeFromLeft (100)); r.removeFromLeft (12);
        crossoverLabel .setBounds (r.removeFromLeft (72));  r.removeFromLeft (6);
        crossoverSlider.setBounds (r.removeFromLeft (240));
    }
    area.removeFromTop (6);

    // Distance-comp mode row.
    {
        auto r = row (26);
        distanceModeLabel.setBounds (r.removeFromLeft (100)); r.removeFromLeft (6);
        distanceModeCombo.setBounds (r.removeFromLeft (140));
    }
    area.removeFromTop (6);

    // Test-signal row.
    {
        auto r = row (26);
        testSignalLabel  .setBounds (r.removeFromLeft (80));  r.removeFromLeft (6);
        testSignalCombo  .setBounds (r.removeFromLeft (110)); r.removeFromLeft (10);
        testLevelSlider  .setBounds (r.removeFromLeft (160)); r.removeFromLeft (8);
        testFreqSlider   .setBounds (r.removeFromLeft (170)); r.removeFromLeft (8);
        testChannelSlider.setBounds (r.removeFromLeft (110)); r.removeFromLeft (8);
        testInfoLabel    .setBounds (r);
    }
    area.removeFromTop (6);

    // Per-speaker trim/mute/solo table (scrollable).
    {
        speakerViewport.setBounds (row (130));
        if (speakerList != nullptr)
            speakerList->setSize (speakerViewport.getMaximumVisibleWidth(),
                                  juce::jmax (speakerViewport.getHeight(), speakerList->preferredHeight()));
    }
    area.removeFromTop (8);

    // Meter strip.
    meterArea = row (70);
    area.removeFromTop (8);

    // Status line.
    statusLabel.setBounds (area.removeFromBottom (24));
    area.removeFromBottom (6);

    // Device selector fills the rest.
    if (deviceSelector != nullptr)
        deviceSelector->setBounds (area);
}
