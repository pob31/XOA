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
#include "../Binding/InputNudger.h"
#include "../Widgets/XoaBasicDial.h"
#include "../Widgets/XoaStandardSlider.h"
#include "../Widgets/XoaBidirectionalSlider.h"

namespace xoa::ui
{

class InputsTab : public TabPage,
                  private InputSelectionModel::Listener
{
public:
    explicit InputsTab (AppContext& ctx);
    ~InputsTab() override;

    void resized() override;
    void refresh() override;
    bool keyPressed (const juce::KeyPress& key) override;
    void mouseDown (const juce::MouseEvent& e) override;

private:
    void currentInputChanged (int newIndex) override;
    void selectInput (int index);
    juce::Label& addRow (juce::Component& control, const char* labelKey);

    ChannelRail rail;

    // Top strip
    juce::TextButton monoInputsButton;   // latching (WFS toggle style)
    juce::Label      stemFeedLabel, inputCountLabel;
    juce::ComboBox   stemFeedCombo;
    juce::Slider     inputCountSlider { juce::Slider::IncDecButtons, juce::Slider::TextBoxLeft };

    // Detail editor
    juce::TextEditor nameEditor;
    XoaStandardSlider gainSlider;
    juce::TextEditor  gainEditor;
    juce::TextButton  muteButton { "Mute" };
    XoaBidirectionalSlider posXSlider, posYSlider, posZSlider;
    juce::TextEditor  posXEditor, posYEditor, posZEditor;
    juce::ComboBox   coordModeCombo;
    juce::Label      posReadout;
    XoaBasicDial     maxSpeedDial, trackingSmoothDial, spreadDial;
    juce::Label      maxSpeedValue, trackingSmoothValue, spreadValue;
    juce::TextButton nfcButton { "NFC" };

    juce::OwnedArray<juce::Label> rowLabels;

    InputNudger nudger { context.store };
    int currentInput = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (InputsTab)
};

} // namespace xoa::ui
