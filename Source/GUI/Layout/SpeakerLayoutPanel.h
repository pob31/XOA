/*
  ==============================================================================

    XOA — tenth-order Ambisonics spatial audio processor.
    SpeakerLayoutPanel — the speaker-layout preset generator (WP10 C7): pick a
    preset (ring / line / dome / ceiling grid), set its parameters, preview the
    result top-down, and Apply it to the Speakers section in one undo transaction
    (count + positions + names). The engine's rebuild debounce coalesces the writes.

    This file is part of XOA, released under the GNU General Public License
    v3.0. See LICENSE for details.

  ==============================================================================
*/

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <vector>

#include "../../Parameters/XoaValueTreeState.h"
#include "../Widgets/RigProjection.h"
#include "SpeakerLayoutGenerators.h"

namespace xoa::ui
{

class SpeakerLayoutPanel : public juce::Component
{
public:
    explicit SpeakerLayoutPanel (XoaValueTreeState& storeToApply);

    std::function<void()> onApplied;   // notify the tab to refresh after Apply

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    enum class Preset { ring = 0, line, dome, grid };

    std::vector<layout::SpeakerPos> generate() const;
    void updateFieldVisibility();
    void apply();

    XoaValueTreeState& store;

    juce::Label    presetLabel;
    juce::ComboBox presetCombo;

    juce::Label  countLabel, radiusLabel, heightLabel, ringsLabel, spacingLabel;
    juce::Slider countSlider  { juce::Slider::IncDecButtons, juce::Slider::TextBoxLeft };
    juce::Slider radiusSlider, heightSlider;
    juce::Slider ringsSlider  { juce::Slider::IncDecButtons, juce::Slider::TextBoxLeft };
    juce::Slider spacingSlider;

    juce::TextButton applyButton;

    juce::Rectangle<int> previewArea;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SpeakerLayoutPanel)
};

} // namespace xoa::ui
