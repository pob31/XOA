/*
  ==============================================================================

    XOA — tenth-order Ambisonics spatial audio processor.
    SpeakersDecoderTab — the rig + decoder surface (WP10 C7): a speaker rail and
    detail editor (name/gain/delay/mute/solo/position + coordinate mode), the
    per-speaker 6-band EQ, the decoder group (type/weighting/normalization,
    dual-band + crossover, layout suggestion, rebuild), per-speaker distance
    compensation + listener position (D18), the output test-signal generator, and
    the speaker-layout preset panel.

    This file is part of XOA, released under the GNU General Public License
    v3.0. See LICENSE for details.

  ==============================================================================
*/

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <array>

#include "TabPage.h"
#include "../ChannelRail.h"
#include "../Widgets/XoaBasicDial.h"
#include "../Layout/SpeakerLayoutPanel.h"
#include "../../XoaConstants.h"

namespace xoa::ui
{

class SpeakersDecoderTab : public TabPage
{
public:
    explicit SpeakersDecoderTab (AppContext& ctx);

    void resized() override;
    void refresh() override;

private:
    void selectSpeaker (int index);
    void updateSuggestion();
    juce::Label& addLabel (const char* labelKey, juce::Justification just = juce::Justification::centredRight);

    ChannelRail rail;

    // Speaker detail
    juce::Slider     speakerCountSlider { juce::Slider::IncDecButtons, juce::Slider::TextBoxLeft };
    juce::TextEditor nameEditor;
    juce::Slider     gainSlider, delaySlider;
    juce::ToggleButton muteButton { "M" }, soloButton { "S" };
    juce::Slider     posXSlider, posYSlider, posZSlider;
    juce::ComboBox   coordModeCombo;
    juce::Label      posReadout;

    // EQ (6 bands)
    juce::ToggleButton eqEnabledButton { "EQ" };
    std::array<juce::ComboBox, xoa::kNumEqBands> eqShape;
    std::array<juce::Slider,   xoa::kNumEqBands> eqFreq, eqGain, eqQ, eqSlope;

    // Decoder
    juce::ComboBox   decoderTypeCombo, weightingCombo, normalizationCombo;
    juce::ToggleButton dualBandButton { "Dual-band" };
    juce::Slider     crossoverSlider;
    juce::Label      suggestionLabel, decoderStatusLabel;
    juce::TextButton rebuildButton;

    // Compensation + listener (D18)
    juce::ComboBox distanceModeCombo;
    juce::Slider   listenerXSlider, listenerYSlider, listenerZSlider;

    // Test signal (engine-backed)
    juce::ComboBox testTypeCombo;
    juce::Slider   testLevelSlider, testFreqSlider, testChannelSlider;
    juce::Label    testInfoLabel;

    // Layout preset panel
    SpeakerLayoutPanel layoutPanel { context.store };

    juce::GroupComponent speakerGroup, eqGroup, decoderGroup, compGroup, testGroup, layoutGroup;
    juce::OwnedArray<juce::Label> labels;

    int currentSpeaker = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SpeakersDecoderTab)
};

} // namespace xoa::ui
