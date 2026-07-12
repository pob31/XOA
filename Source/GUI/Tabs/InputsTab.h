/*
  ==============================================================================

    XOA — tenth-order Ambisonics spatial audio processor.
    InputsTab — the mono-encoder surface (WP10 C6): a channel rail plus a
    per-input detail editor (name, gain, mute, position + coordinate mode,
    speed/tracking conditioning, spread, NFC), the input-count control, the
    mono-encoder gate and the stem-feed source.

    This file is part of XOA, released under the GNU General Public License
    v3.0. See LICENSE for details.

  ==============================================================================
*/

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "TabPage.h"
#include "../ChannelRail.h"
#include "../Widgets/XoaBasicDial.h"

namespace xoa::ui
{

class InputsTab : public TabPage
{
public:
    explicit InputsTab (AppContext& ctx);

    void resized() override;
    void refresh() override;

private:
    void selectInput (int index);
    juce::Label& addRow (juce::Component& control, const char* labelKey);

    ChannelRail rail;

    // Top strip
    juce::ToggleButton monoInputsButton;
    juce::Label        stemFeedLabel, inputCountLabel;
    juce::ComboBox     stemFeedCombo;
    juce::Slider       inputCountSlider { juce::Slider::IncDecButtons, juce::Slider::TextBoxLeft };

    // Detail editor
    juce::TextEditor nameEditor;
    juce::Slider     gainSlider;
    juce::ToggleButton muteButton { "Mute" };
    juce::Slider     posXSlider, posYSlider, posZSlider;
    juce::ComboBox   coordModeCombo;
    juce::Label      posReadout;
    juce::Slider     maxSpeedSlider, trackingSmoothSlider;
    XoaBasicDial     spreadDial;
    juce::ToggleButton nfcButton { "NFC" };

    juce::OwnedArray<juce::Label> rowLabels;

    int currentInput = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (InputsTab)
};

} // namespace xoa::ui
