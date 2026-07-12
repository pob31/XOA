/*
  ==============================================================================

    XOA — tenth-order Ambisonics spatial audio processor.
    HeaderBar implementation — see HeaderBar.h.

    This file is part of XOA, released under the GNU General Public License
    v3.0. See LICENSE for details.

  ==============================================================================
*/

#include "HeaderBar.h"

#include "Audio/AudioEngine.h"
#include "Network/OSCManager.h"
#include "Localization/LocalizationManager.h"
#include "XoaConstants.h"

namespace ids = xoa::ids;

namespace xoa::ui
{

HeaderBar::HeaderBar (AppContext& ctx)
    : context (ctx), bindings (ctx.store)
{
    // --- Transport --------------------------------------------------------
    addAndMakeVisible (openButton);
    addAndMakeVisible (playButton);
    addAndMakeVisible (stopButton);
    addAndMakeVisible (loopButton);
    addAndMakeVisible (sourceCombo);
    addAndMakeVisible (fileLabel);
    addAndMakeVisible (positionSlider);

    openButton.setButtonText (LOC ("common.browse"));
    openButton.onClick = [this] { openFileDialog(); };
    playButton.onClick = [this]
    {
        sourceCombo.setSelectedId (1, juce::dontSendNotification);
        context.engine.setInputSource (xoa::AudioEngine::InputSource::file);
        context.engine.getFilePlayer().play();
    };
    stopButton.onClick = [this] { context.engine.getFilePlayer().stop(); };

    // Loop is a store parameter (OSC-writable); refresh() pushes it to the player.
    bindings.bindToggle (loopButton, ids::playbackLoop);

    sourceCombo.addItem ("File", 1);
    sourceCombo.addItem ("Test scene", 2);
    sourceCombo.setSelectedId (1, juce::dontSendNotification);
    sourceCombo.onChange = [this]
    {
        context.engine.setInputSource (sourceCombo.getSelectedId() == 2
                                           ? xoa::AudioEngine::InputSource::testScene
                                           : xoa::AudioEngine::InputSource::file);
    };

    fileLabel.setText (LOC ("header.noFile"), juce::dontSendNotification);
    positionSlider.setRange (0.0, 1.0, 0.01);
    positionSlider.onValueChange = [this]
    {
        if (positionSlider.isMouseButtonDown())
            context.engine.getFilePlayer().seekSeconds (positionSlider.getValue());
    };

    // --- Rotation dials (FR-10) ------------------------------------------
    auto setupDial = [this] (XoaBasicDial& dial, juce::Label& label,
                             const juce::Identifier& id, const char* labelKey)
    {
        addAndMakeVisible (dial);
        label.setText (LOC (labelKey), juce::dontSendNotification);
        label.setJustificationType (juce::Justification::centred);
        addAndMakeVisible (label);
        bindings.bindDial (dial, id);
    };
    setupDial (yawDial,   yawLabel,   ids::rotationYaw,   "param.rotationYaw");
    setupDial (pitchDial, pitchLabel, ids::rotationPitch, "param.rotationPitch");
    setupDial (rollDial,  rollLabel,  ids::rotationRoll,  "param.rotationRoll");

    // --- Master ----------------------------------------------------------
    masterLabel.setText (LOC ("param.masterGain"), juce::dontSendNotification);
    masterLabel.setJustificationType (juce::Justification::centredRight);
    addAndMakeVisible (masterLabel);
    masterSlider.setSliderStyle (juce::Slider::LinearHorizontal);
    masterSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 64, 20);
    addAndMakeVisible (masterSlider);
    bindings.bindSlider (masterSlider, ids::masterGain);

    // --- Status ----------------------------------------------------------
    statusLabel.setJustificationType (juce::Justification::centredLeft);
    addAndMakeVisible (statusLabel);
}

HeaderBar::~HeaderBar() = default;

void HeaderBar::openFileDialog()
{
    juce::String patterns = "*.wav;*.flac";
   #if JUCE_MAC
    patterns += ";*.caf";
   #endif

    fileChooser = std::make_unique<juce::FileChooser> (LOC ("header.openTitle"),
                                                       juce::File(), patterns);
    fileChooser->launchAsync (juce::FileBrowserComponent::openMode
                                  | juce::FileBrowserComponent::canSelectFiles,
                              [this] (const juce::FileChooser& fc)
    {
        const auto file = fc.getResult();
        if (file == juce::File())
            return;

        const auto r = context.engine.openFile (file);
        if (r.ok)
        {
            juce::String text;
            text << file.getFileName() << "  (" << r.numChannels << " ch, "
                 << juce::String (r.fileSampleRate / 1000.0, 1) << " kHz, order "
                 << r.detectedOrder << ")";
            if (! r.warnings.isEmpty())
                text << "  ·  " << r.warnings.joinIntoString ("; ");
            fileLabel.setText (text, juce::dontSendNotification);

            positionSlider.setRange (0.0, juce::jmax (0.001, context.engine.getFilePlayer().getLengthSeconds()), 0.01);
            sourceCombo.setSelectedId (1, juce::dontSendNotification);
        }
        else
        {
            fileLabel.setText ("Error: " + r.error, juce::dontSendNotification);
        }
    });
}

void HeaderBar::refresh()
{
    // Push the loop parameter (UI- or OSC-driven) to the player.
    context.engine.getFilePlayer().setLooping ((bool) context.store.getParameter (ids::playbackLoop));

    if (! positionSlider.isMouseButtonDown())
        positionSlider.setValue (context.engine.getFilePlayer().getPositionSeconds(),
                                 juce::dontSendNotification);

    const double sr    = context.engine.getSampleRate();
    const int    block = context.engine.getBlockSize();

    juce::String s;
    s << "order " << xoa::kAmbisonicOrder << " · " << xoa::kNumSHChannels << " SH";
    if (sr > 0.0)
        s << "  ·  " << juce::String (sr / 1000.0, 1) << " kHz / " << block << " smp"
          << "  ·  " << juce::String (context.engine.getMeasuredLatencyMs(), 1) << " ms"
          << "  ·  CPU " << juce::String (context.engine.getCpuLoad() * 100.0, 1) << " %";
    else
        s << "  ·  " << LOC ("header.deviceStopped");
    if (context.engine.isDecoderRebuildInFlight())
        s << "  ·  " << LOC ("header.rebuilding");
    s << "  ·  OSC " << (context.oscManager.isReceiving()
                             ? "rx :" + juce::String (context.oscManager.getUdpPort())
                             : LOC ("common.off"));

    statusLabel.setText (s, juce::dontSendNotification);
}

void HeaderBar::resized()
{
    const float sc = XoaLookAndFeel::uiScale;
    auto px = [sc] (int v) { return juce::roundToInt ((float) v * sc); };

    auto area = getLocalBounds().reduced (px (8), px (4));

    // Row 1: transport
    auto r1 = area.removeFromTop (px (28));
    openButton  .setBounds (r1.removeFromLeft (px (80)));  r1.removeFromLeft (px (4));
    playButton  .setBounds (r1.removeFromLeft (px (60)));  r1.removeFromLeft (px (4));
    stopButton  .setBounds (r1.removeFromLeft (px (60)));  r1.removeFromLeft (px (8));
    loopButton  .setBounds (r1.removeFromLeft (px (64)));  r1.removeFromLeft (px (8));
    sourceCombo .setBounds (r1.removeFromLeft (px (120))); r1.removeFromLeft (px (8));
    fileLabel   .setBounds (r1);
    area.removeFromTop (px (4));
    positionSlider.setBounds (area.removeFromTop (px (18)));
    area.removeFromTop (px (6));

    // Row 2: rotation dials (left) | master (right) | status (bottom)
    auto statusRow = area.removeFromBottom (px (18));
    statusLabel.setBounds (statusRow);
    area.removeFromBottom (px (4));

    auto masterArea = area.removeFromRight (px (300));
    masterLabel.setBounds (masterArea.removeFromLeft (px (72)));
    masterArea.removeFromLeft (px (4));
    masterSlider.setBounds (masterArea.withSizeKeepingCentre (masterArea.getWidth(), px (24)));

    const int dialW = juce::jmax (px (60), area.getWidth() / 3);
    auto place = [] (juce::Rectangle<int> cell, juce::Label& lab, XoaBasicDial& dial)
    {
        lab.setBounds (cell.removeFromTop (juce::roundToInt (cell.getHeight() * 0.22f)));
        dial.setBounds (cell.reduced (juce::jmax (2, cell.getWidth() / 8), 0));
    };
    place (area.removeFromLeft (dialW), yawLabel,   yawDial);
    place (area.removeFromLeft (dialW), pitchLabel, pitchDial);
    place (area,                        rollLabel,  rollDial);
}

} // namespace xoa::ui
