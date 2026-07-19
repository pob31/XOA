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
    // Arrow-key / PageUp-PageDown nudging of the current input (InputNudger).
    setWantsKeyboardFocus (true);

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
    inputCountSlider.setWantsKeyboardFocus (false);   // arrows must reach the nudger
    bindings.bindSlider (inputCountSlider, ids::inputCount);

    // --- Detail editor (per current input) -------------------------------
    addAndMakeVisible (nameEditor);
    addRow (nameEditor, "param.inputName");
    bindings.bindText (nameEditor, ids::inputName, BindingSet::kCurrentChannel);

    gainSlider.setTrackColours (ColorScheme::get().sliderTrackBg, ColorScheme::accents::level);
    addAndMakeVisible (gainSlider);
    addRow (gainSlider, "param.inputGain");
    bindings.bindKitSlider (gainSlider, ids::inputGain, BindingSet::kCurrentChannel);
    styleValueEditor (gainEditor);
    addAndMakeVisible (gainEditor);
    bindings.bindText (gainEditor, ids::inputGain, BindingSet::kCurrentChannel);

    muteButton.setColour (juce::TextButton::buttonOnColourId, ColorScheme::accents::mute);
    addAndMakeVisible (muteButton);
    addRow (muteButton, "param.inputMute");
    bindings.bindToggle (muteButton, ids::inputMute, BindingSet::kCurrentChannel);

    for (auto* s : { &posXSlider, &posYSlider, &posZSlider })
    {
        s->setTrackColours (ColorScheme::get().sliderTrackBg, ColorScheme::accents::spatial);
        addAndMakeVisible (*s);
    }
    for (auto* e : { &posXEditor, &posYEditor, &posZEditor })
    {
        styleValueEditor (*e);
        addAndMakeVisible (*e);
    }
    addRow (posXSlider, "param.inputPositionX");
    addRow (posYSlider, "param.inputPositionY");
    addRow (posZSlider, "param.inputPositionZ");
    bindings.bindKitSlider (posXSlider, ids::inputPositionX, BindingSet::kCurrentChannel);
    bindings.bindKitSlider (posYSlider, ids::inputPositionY, BindingSet::kCurrentChannel);
    bindings.bindKitSlider (posZSlider, ids::inputPositionZ, BindingSet::kCurrentChannel);
    bindings.bindText (posXEditor, ids::inputPositionX, BindingSet::kCurrentChannel);
    bindings.bindText (posYEditor, ids::inputPositionY, BindingSet::kCurrentChannel);
    bindings.bindText (posZEditor, ids::inputPositionZ, BindingSet::kCurrentChannel);

    addAndMakeVisible (coordModeCombo);
    addRow (coordModeCombo, "param.inputCoordinateMode");
    bindings.bindCombo (coordModeCombo, ids::inputCoordinateMode, BindingSet::kCurrentChannel);

    posReadout.setJustificationType (juce::Justification::centredLeft);
    addAndMakeVisible (posReadout);

    // Conditioning dials (WFS uses dials for speed/smoothing/spread), each with
    // a live value label underneath.
    auto setupDial = [this] (XoaBasicDial& dial, juce::Label& valueLabel,
                             const juce::Identifier& id, const char* labelKey)
    {
        addAndMakeVisible (dial);
        addRow (dial, labelKey);
        valueLabel.setJustificationType (juce::Justification::centred);
        addAndMakeVisible (valueLabel);
        bindings.bindDial (dial, id, BindingSet::kCurrentChannel, &valueLabel);
    };
    setupDial (maxSpeedDial,       maxSpeedValue,       ids::inputMaxSpeed,       "param.inputMaxSpeed");
    setupDial (trackingSmoothDial, trackingSmoothValue, ids::inputTrackingSmooth, "param.inputTrackingSmooth");
    setupDial (spreadDial,         spreadValue,         ids::inputSpread,         "param.inputSpread");

    addAndMakeVisible (nfcButton);
    addRow (nfcButton, "param.inputNfcEnabled");
    bindings.bindToggle (nfcButton, ids::inputNfcEnabled, BindingSet::kCurrentChannel);

    selectInput (0);
    context.inputSelection.addListener (this);
    verifyRegistryCoverage();
}

InputsTab::~InputsTab()
{
    context.inputSelection.removeListener (this);
}

void InputsTab::currentInputChanged (int newIndex)
{
    if (newIndex != currentInput)
        selectInput (newIndex);
}

bool InputsTab::keyPressed (const juce::KeyPress& key)
{
    // A focused TextEditor keeps its caret/paging keys.
    if (dynamic_cast<juce::TextEditor*> (juce::Component::getCurrentlyFocusedComponent()) != nullptr)
        return false;
    return nudger.handleKey (key, currentInput);
}

void InputsTab::mouseDown (const juce::MouseEvent&)
{
    grabKeyboardFocus();   // clicks on the tab background re-arm key nudging
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
    context.inputSelection.set (currentInput);
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
    // Latching buttons get a fixed width; full-row toggles read as bars.
    auto buttonRow = [&] (juce::Component& c)
    {
        auto r = area.removeFromTop (rowH);
        if (idx < rowLabels.size())
            rowLabels[idx++]->setBounds (r.removeFromLeft (labelW));
        r.removeFromLeft (px (6));
        c.setBounds (r.removeFromLeft (px (100)).reduced (0, px (2)));
        area.removeFromTop (px (4));
    };
    // Kit-slider row: reserve space on the right for the exact-value editor.
    auto sliderRow = [&] (juce::Component& slider, juce::TextEditor& editor)
    {
        auto r = area.removeFromTop (rowH);
        if (idx < rowLabels.size())
            rowLabels[idx++]->setBounds (r.removeFromLeft (labelW));
        r.removeFromLeft (px (6));
        editor.setBounds (r.removeFromRight (px (64)).reduced (0, px (3)));
        r.removeFromRight (px (4));
        slider.setBounds (r.reduced (0, px (2)));
        area.removeFromTop (px (4));
    };
    row (nameEditor);
    sliderRow (gainSlider, gainEditor);
    buttonRow (muteButton);
    sliderRow (posXSlider, posXEditor);
    sliderRow (posYSlider, posYEditor);
    sliderRow (posZSlider, posZEditor);
    row (coordModeCombo);
    posReadout.setBounds (area.removeFromTop (rowH));
    area.removeFromTop (px (4));

    // Conditioning dials: three columns of name / dial / value (WFS-style).
    {
        auto block = area.removeFromTop (px (110));
        const int colW = juce::jmax (px (90), juce::jmin (px (130), block.getWidth() / 3));
        XoaBasicDial* dials[3]  = { &maxSpeedDial, &trackingSmoothDial, &spreadDial };
        juce::Label*  values[3] = { &maxSpeedValue, &trackingSmoothValue, &spreadValue };
        for (int i = 0; i < 3; ++i)
        {
            auto cell = block.removeFromLeft (colW);
            if (idx < rowLabels.size())
            {
                rowLabels[idx]->setJustificationType (juce::Justification::centred);
                rowLabels[idx++]->setBounds (cell.removeFromTop (px (18)));
            }
            values[i]->setBounds (cell.removeFromBottom (px (18)));
            dials[i]->setBounds (cell.reduced (juce::jmax (2, cell.getWidth() / 8), 0));
        }
        area.removeFromTop (px (4));
    }
    buttonRow (nfcButton);
}

} // namespace xoa::ui
