/*
  ==============================================================================

    XOA — tenth-order Ambisonics spatial audio processor.
    A scrollable per-input encoder table (position / gain / spread / NFC) for the
    WP8 shell UI. Deliberately throwaway plain-JUCE (the real Inputs tab lands in
    WP10); it exists so a developer can drive the FR-5/FR-6 mono encoders for the
    M3 listening check. Controls write the store; the AmbiCalculationEngine's
    50 Hz listener turns edits into live encode-matrix updates.

    This file is part of XOA, released under the GNU General Public License
    v3.0. See LICENSE for details.

  ==============================================================================
*/

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <memory>
#include <vector>

#include "Parameters/XoaParameterIDs.h"
#include "Parameters/XoaValueTreeState.h"

//==============================================================================
/** One scrollable row per mono input: X/Y/Z position (m), gain (dB), spread
    (deg), mute, NFC. Seeded from the store on (re)build; not kept in live
    two-way sync — enough for the throwaway shell. */
class InputListComponent : public juce::Component
{
public:
    explicit InputListComponent (xoa::XoaValueTreeState& s) : store (s) {}

    void rebuildRows()
    {
        rows.clear();
        const int n = store.getNumInputs();
        for (int i = 0; i < n; ++i)
        {
            auto row = std::make_unique<Row> (store, i);
            addAndMakeVisible (*row);
            rows.push_back (std::move (row));
        }
        setSize (juce::jmax (1, getWidth()), n * kRowH);
        resized();
    }

    int preferredHeight() const { return (int) rows.size() * kRowH; }

    void resized() override
    {
        int y = 0;
        for (auto& r : rows)
        {
            r->setBounds (0, y, getWidth(), kRowH);
            y += kRowH;
        }
    }

private:
    static constexpr int kRowH = 28;

    struct Row : public juce::Component
    {
        Row (xoa::XoaValueTreeState& s, int idx) : store (s), index (idx)
        {
            namespace ids = xoa::ids;

            name.setText ("In " + juce::String (idx + 1), juce::dontSendNotification);
            name.setJustificationType (juce::Justification::centredLeft);
            addAndMakeVisible (name);

            auto setupPos = [this] (juce::Slider& sl, const juce::Identifier& id)
            {
                sl.setSliderStyle (juce::Slider::LinearBar);
                sl.setTextBoxStyle (juce::Slider::TextBoxLeft, false, 40, 18);
                sl.setRange (-100.0, 100.0, 0.01);
                sl.setValue (store.getFloatParameter (id, index), juce::dontSendNotification);
                sl.onValueChange = [this, &sl, id] { store.setParameter (id, sl.getValue(), index); };
                addAndMakeVisible (sl);
            };
            setupPos (posX, ids::inputPositionX);
            setupPos (posY, ids::inputPositionY);
            setupPos (posZ, ids::inputPositionZ);

            gain.setSliderStyle (juce::Slider::LinearBar);
            gain.setTextBoxStyle (juce::Slider::TextBoxLeft, false, 40, 18);
            gain.setRange (-60.0, 12.0, 0.1);
            gain.setTextValueSuffix (" dB");
            gain.setValue (store.getFloatParameter (ids::inputGain, index), juce::dontSendNotification);
            gain.onValueChange = [this] { store.setParameter (xoa::ids::inputGain, gain.getValue(), index); };
            addAndMakeVisible (gain);

            spread.setSliderStyle (juce::Slider::LinearBar);
            spread.setTextBoxStyle (juce::Slider::TextBoxLeft, false, 34, 18);
            spread.setRange (0.0, 180.0, 0.5);
            spread.setValue (store.getFloatParameter (ids::inputSpread, index), juce::dontSendNotification);
            spread.onValueChange = [this] { store.setParameter (xoa::ids::inputSpread, spread.getValue(), index); };
            addAndMakeVisible (spread);

            mute.setButtonText ("M");
            mute.setToggleState (static_cast<bool> (store.getParameter (ids::inputMute, index)),
                                 juce::dontSendNotification);
            mute.onClick = [this] { store.setParameter (xoa::ids::inputMute, mute.getToggleState(), index); };
            addAndMakeVisible (mute);

            nfc.setButtonText ("NFC");
            nfc.setToggleState (static_cast<bool> (store.getParameter (ids::inputNfcEnabled, index)),
                                juce::dontSendNotification);
            nfc.onClick = [this] { store.setParameter (xoa::ids::inputNfcEnabled, nfc.getToggleState(), index); };
            addAndMakeVisible (nfc);
        }

        void resized() override
        {
            auto r = getLocalBounds().reduced (2, 3);
            name.setBounds (r.removeFromLeft (40));
            nfc.setBounds (r.removeFromRight (44));
            mute.setBounds (r.removeFromRight (30));
            const int w = r.getWidth() / 5;
            posX.setBounds   (r.removeFromLeft (w));
            posY.setBounds   (r.removeFromLeft (w));
            posZ.setBounds   (r.removeFromLeft (w));
            gain.setBounds   (r.removeFromLeft (w));
            spread.setBounds (r);
        }

        xoa::XoaValueTreeState& store;
        int index;
        juce::Label        name;
        juce::Slider       posX, posY, posZ, gain, spread;
        juce::ToggleButton mute, nfc;
    };

    xoa::XoaValueTreeState& store;
    std::vector<std::unique_ptr<Row>> rows;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (InputListComponent)
};
