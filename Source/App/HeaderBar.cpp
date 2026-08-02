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
    // --- HOA source (D48) -------------------------------------------------
    // XOA is a processor: program material arrives from external players via
    // the device inputs. The only in-app source is the synthetic test scene,
    // the audible-without-hardware fallback.
    testSceneButton.setButtonText (LOC ("header.testScene"));
    testSceneButton.setClickingTogglesState (true);
    testSceneButton.onClick = [this]
    {
        context.engine.setInputSource (testSceneButton.getToggleState()
                                           ? xoa::AudioEngine::InputSource::testScene
                                           : xoa::AudioEngine::InputSource::none);
    };
    addAndMakeVisible (testSceneButton);

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
    masterSlider.setTrackColours (ColorScheme::get().sliderTrackBg, ColorScheme::accents::level);
    addAndMakeVisible (masterSlider);
    bindings.bindKitSlider (masterSlider, ids::masterGain);
    styleValueEditor (masterEditor);
    addAndMakeVisible (masterEditor);
    bindings.bindText (masterEditor, ids::masterGain);

    // --- Status ----------------------------------------------------------
    statusLabel.setJustificationType (juce::Justification::centredLeft);
    addAndMakeVisible (statusLabel);
}

HeaderBar::~HeaderBar() = default;

void HeaderBar::refresh()
{
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

    // Rotation dials (centre) | test-scene latch (left) | master (right) |
    // status (bottom). No transport row (D48).
    auto statusRow = area.removeFromBottom (px (18));
    statusLabel.setBounds (statusRow);
    area.removeFromBottom (px (4));

    testSceneButton.setBounds (area.removeFromLeft (px (110))
                                   .withSizeKeepingCentre (px (110), px (28)));
    area.removeFromLeft (px (8));

    auto masterArea = area.removeFromRight (px (300));
    masterLabel.setBounds (masterArea.removeFromLeft (px (72)));
    masterArea.removeFromLeft (px (4));
    masterEditor.setBounds (masterArea.removeFromRight (px (64))
                                      .withSizeKeepingCentre (px (64), px (20)));
    masterArea.removeFromRight (px (4));
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
