/*
  ==============================================================================

    XOA — tenth-order Ambisonics spatial audio processor.
    InputsTab implementation — see InputsTab.h.

    This file is part of XOA, released under the GNU General Public License
    v3.0. See LICENSE for details.

  ==============================================================================
*/

#include "InputsTab.h"

#include "Audio/AudioEngine.h"
#include "Helpers/XoaCoordinates.h"
#include "Localization/LocalizationManager.h"

namespace ids = xoa::ids;

namespace xoa::ui
{

InputsTab::InputsTab (AppContext& ctx) : TabPage (ctx, Surface::inputs)
{
    // --- Rail ------------------------------------------------------------
    rail.getCount   = [this] { return context.store.getNumInputs(); };
    rail.getRowText = [this] (int row)
    {
        juce::String t;
        t << (row + 1) << "  " << context.store.getStringParameter (ids::inputName, row);
        if ((bool) context.store.getParameter (ids::inputMute, row))
            t << "  [M]";
        return t;
    };
    rail.onSelect = [this] (int row) { selectInput (row); };
    addAndMakeVisible (rail);

    // --- Top strip -------------------------------------------------------
    monoInputsButton.setButtonText (LOC ("param.monoInputsEnabled"));
    addAndMakeVisible (monoInputsButton);
    bindings.bindToggle (monoInputsButton, ids::monoInputsEnabled);

    stemFeedLabel.setText (LOC ("inputs.stems"), juce::dontSendNotification);
    stemFeedLabel.setJustificationType (juce::Justification::centredRight);
    addAndMakeVisible (stemFeedLabel);
    stemFeedCombo.addItem (LOC ("inputs.stemsDevice"), 1);
    stemFeedCombo.addItem (LOC ("inputs.stemsTest"), 2);
    stemFeedCombo.setSelectedId (1, juce::dontSendNotification);
    stemFeedCombo.onChange = [this]
    {
        context.engine.setStemFeed (stemFeedCombo.getSelectedId() == 2
                                        ? xoa::AudioEngine::StemFeed::test
                                        : xoa::AudioEngine::StemFeed::device);
    };
    addAndMakeVisible (stemFeedCombo);

    inputCountLabel.setText (LOC ("param.inputCount"), juce::dontSendNotification);
    inputCountLabel.setJustificationType (juce::Justification::centredRight);
    addAndMakeVisible (inputCountLabel);
    addAndMakeVisible (inputCountSlider);
    bindings.bindSlider (inputCountSlider, ids::inputCount);

    // --- Detail editor (per current input) -------------------------------
    addAndMakeVisible (nameEditor);
    addRow (nameEditor, "param.inputName");
    bindings.bindText (nameEditor, ids::inputName, BindingSet::kCurrentChannel);

    gainSlider.setSliderStyle (juce::Slider::LinearHorizontal);
    gainSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 64, 20);
    addAndMakeVisible (gainSlider);
    addRow (gainSlider, "param.inputGain");
    bindings.bindSlider (gainSlider, ids::inputGain, BindingSet::kCurrentChannel);

    addAndMakeVisible (muteButton);
    addRow (muteButton, "param.inputMute");
    bindings.bindToggle (muteButton, ids::inputMute, BindingSet::kCurrentChannel);

    for (auto* s : { &posXSlider, &posYSlider, &posZSlider })
    {
        s->setSliderStyle (juce::Slider::LinearHorizontal);
        s->setTextBoxStyle (juce::Slider::TextBoxRight, false, 64, 20);
        addAndMakeVisible (*s);
    }
    addRow (posXSlider, "param.inputPositionX");
    addRow (posYSlider, "param.inputPositionY");
    addRow (posZSlider, "param.inputPositionZ");
    bindings.bindSlider (posXSlider, ids::inputPositionX, BindingSet::kCurrentChannel);
    bindings.bindSlider (posYSlider, ids::inputPositionY, BindingSet::kCurrentChannel);
    bindings.bindSlider (posZSlider, ids::inputPositionZ, BindingSet::kCurrentChannel);

    addAndMakeVisible (coordModeCombo);
    addRow (coordModeCombo, "param.inputCoordinateMode");
    bindings.bindCombo (coordModeCombo, ids::inputCoordinateMode, BindingSet::kCurrentChannel);

    posReadout.setJustificationType (juce::Justification::centredLeft);
    addAndMakeVisible (posReadout);

    maxSpeedSlider.setSliderStyle (juce::Slider::LinearHorizontal);
    maxSpeedSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 64, 20);
    addAndMakeVisible (maxSpeedSlider);
    addRow (maxSpeedSlider, "param.inputMaxSpeed");
    bindings.bindSlider (maxSpeedSlider, ids::inputMaxSpeed, BindingSet::kCurrentChannel);

    trackingSmoothSlider.setSliderStyle (juce::Slider::LinearHorizontal);
    trackingSmoothSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 64, 20);
    addAndMakeVisible (trackingSmoothSlider);
    addRow (trackingSmoothSlider, "param.inputTrackingSmooth");
    bindings.bindSlider (trackingSmoothSlider, ids::inputTrackingSmooth, BindingSet::kCurrentChannel);

    addAndMakeVisible (spreadDial);
    addRow (spreadDial, "param.inputSpread");
    bindings.bindDial (spreadDial, ids::inputSpread, BindingSet::kCurrentChannel);

    addAndMakeVisible (nfcButton);
    addRow (nfcButton, "param.inputNfcEnabled");
    bindings.bindToggle (nfcButton, ids::inputNfcEnabled, BindingSet::kCurrentChannel);

    selectInput (0);
    verifyRegistryCoverage();
}

juce::Label& InputsTab::addRow (juce::Component& /*control*/, const char* labelKey)
{
    auto* l = rowLabels.add (new juce::Label());
    l->setText (LOC (labelKey), juce::dontSendNotification);
    l->setJustificationType (juce::Justification::centredRight);
    addAndMakeVisible (l);
    return *l;
}

void InputsTab::selectInput (int index)
{
    currentInput = juce::jlimit (0, juce::jmax (0, context.store.getNumInputs() - 1), index);
    bindings.setChannel (currentInput);
    rail.selectRow (currentInput);
}

void InputsTab::refresh()
{
    TabPage::refresh();
    rail.refresh();

    // Keep the selection valid if the count shrank.
    if (currentInput >= context.store.getNumInputs())
        selectInput (context.store.getNumInputs() - 1);

    // Position readout in the selected coordinate system.
    const xoa::coords::Cartesian c {
        context.store.getFloatParameter (ids::inputPositionX, currentInput),
        context.store.getFloatParameter (ids::inputPositionY, currentInput),
        context.store.getFloatParameter (ids::inputPositionZ, currentInput) };
    const auto mode = (xoa::coords::Mode) context.store.getIntParameter (ids::inputCoordinateMode, currentInput);
    posReadout.setText (xoa::coords::formatForDisplay (c, mode), juce::dontSendNotification);
}

void InputsTab::resized()
{
    const float sc = XoaLookAndFeel::uiScale;
    auto px = [sc] (int v) { return juce::roundToInt ((float) v * sc); };
    auto area = getLocalBounds().reduced (px (10));

    // Top strip
    auto top = area.removeFromTop (px (28));
    monoInputsButton.setBounds (top.removeFromLeft (px (130))); top.removeFromLeft (px (10));
    stemFeedLabel.setBounds (top.removeFromLeft (px (48)));    top.removeFromLeft (px (4));
    stemFeedCombo.setBounds (top.removeFromLeft (px (130)));   top.removeFromLeft (px (16));
    inputCountLabel.setBounds (top.removeFromLeft (px (60)));  top.removeFromLeft (px (4));
    inputCountSlider.setBounds (top.removeFromLeft (px (110)));
    area.removeFromTop (px (8));

    // Rail (left) + detail (right)
    rail.setBounds (area.removeFromLeft (px (200)));
    area.removeFromLeft (px (12));

    const int rowH = px (26);
    const int labelW = px (120);
    int idx = 0;
    auto row = [&] (juce::Component& c)
    {
        auto r = area.removeFromTop (rowH);
        if (idx < rowLabels.size())
            rowLabels[idx++]->setBounds (r.removeFromLeft (labelW));
        r.removeFromLeft (px (6));
        c.setBounds (r.reduced (0, px (2)));
        area.removeFromTop (px (4));
    };
    row (nameEditor);
    row (gainSlider);
    row (muteButton);
    row (posXSlider);
    row (posYSlider);
    row (posZSlider);
    row (coordModeCombo);
    posReadout.setBounds (area.removeFromTop (rowH));
    area.removeFromTop (px (4));
    row (maxSpeedSlider);
    row (trackingSmoothSlider);
    {
        auto r = area.removeFromTop (px (90));
        if (idx < rowLabels.size())
            rowLabels[idx++]->setBounds (r.removeFromLeft (labelW).removeFromTop (rowH));
        spreadDial.setBounds (r.removeFromLeft (px (90)));
        area.removeFromTop (px (4));
    }
    row (nfcButton);
}

} // namespace xoa::ui
