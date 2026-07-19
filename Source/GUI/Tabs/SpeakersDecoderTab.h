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
#include "../Widgets/XoaStandardSlider.h"
#include "../Widgets/XoaBidirectionalSlider.h"
#include "../Layout/SpeakerLayoutPanel.h"
#include "../Analysis/RvReMapComponent.h"
#include "../../XoaConstants.h"

#include <cstdint>

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
    XoaStandardSlider gainSlider, delaySlider;
    juce::TextEditor  gainEditor, delayEditor;
    juce::TextButton  muteButton { "M" }, soloButton { "S" };   // latching (WFS style)
    XoaBidirectionalSlider posXSlider, posYSlider, posZSlider;
    juce::TextEditor  posXEditor, posYEditor, posZEditor;
    juce::ComboBox   coordModeCombo;
    juce::Label      posReadout;

    // EQ (6 bands): freq kit slider + gain/Q/slope dials, each with a value
    // readout (the WFS OutputsTab band-column pattern).
    juce::TextButton eqEnabledButton { "EQ" };   // latching
    std::array<juce::ComboBox, xoa::kNumEqBands> eqShape;
    std::array<XoaStandardSlider, xoa::kNumEqBands> eqFreq;
    std::array<XoaBasicDial, xoa::kNumEqBands> eqGain, eqQ, eqSlope;
    std::array<juce::Label, xoa::kNumEqBands> eqFreqValue, eqGainValue, eqQValue, eqSlopeValue;

    // Decoder
    juce::ComboBox   decoderTypeCombo, weightingCombo, normalizationCombo;
    juce::TextButton dualBandButton { "Dual-band" };   // latching
    XoaStandardSlider crossoverSlider;
    juce::TextEditor  crossoverEditor;
    juce::Label      suggestionLabel, decoderStatusLabel;
    juce::TextButton rebuildButton;

    // Compensation + listener (D18)
    juce::ComboBox distanceModeCombo;
    XoaBidirectionalSlider listenerXSlider, listenerYSlider, listenerZSlider;
    juce::TextEditor listenerXEditor, listenerYEditor, listenerZEditor;

    // Test signal (engine-backed)
    juce::ComboBox testTypeCombo;
    XoaStandardSlider testLevelSlider, testFreqSlider;
    juce::Slider   testChannelSlider;
    juce::Label    testLevelValue, testFreqValue;
    juce::Label    testInfoLabel;

    // Bottom-right area: layout preset panel / rV-rE analysis, switched by a toggle.
    SpeakerLayoutPanel layoutPanel { context.store };
    RvReMapComponent   analysisPanel;
    juce::TextButton   layoutViewButton, analysisViewButton;
    void setBottomView (bool showAnalysis);
    std::uint64_t lastAnalysisGen = 0;

    juce::GroupComponent speakerGroup, eqGroup, decoderGroup, compGroup, testGroup, layoutGroup;
    juce::OwnedArray<juce::Label> labels;

    int currentSpeaker = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SpeakersDecoderTab)
};

} // namespace xoa::ui
