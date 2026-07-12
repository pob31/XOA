/*
  ==============================================================================

    XOA — tenth-order Ambisonics spatial audio processor.
    A scrollable per-speaker trim / mute / solo table for the WP7 shell UI.
    Deliberately throwaway plain-JUCE (the real editor lands in WP10); it exists
    so a developer can drive the FR-15 compensation for the M2 listening check.

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
/** One scrollable row per speaker: trim (dB), mute, solo. The controls write
    the store (which the AudioEngine's D17 listener turns into an immediate comp
    republish); they are seeded from the store on (re)build but not kept in
    live two-way sync — enough for the throwaway shell. */
class SpeakerListComponent : public juce::Component
{
public:
    explicit SpeakerListComponent (xoa::XoaValueTreeState& s) : store (s) {}

    /** (Re)create rows for the current speaker count. Call after a count change
        (WFS import / project load / initial build). */
    void rebuildRows()
    {
        rows.clear();
        const int n = store.getNumSpeakers();
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
    static constexpr int kRowH = 26;

    struct Row : public juce::Component
    {
        Row (xoa::XoaValueTreeState& s, int idx) : store (s), index (idx)
        {
            namespace ids = xoa::ids;

            name.setText ("Spk " + juce::String (idx + 1), juce::dontSendNotification);
            name.setJustificationType (juce::Justification::centredLeft);
            addAndMakeVisible (name);

            gain.setSliderStyle (juce::Slider::LinearHorizontal);
            gain.setTextBoxStyle (juce::Slider::TextBoxRight, false, 52, 18);
            gain.setRange (-24.0, 12.0, 0.1);
            gain.setTextValueSuffix (" dB");
            gain.setValue (store.getFloatParameter (ids::speakerGain, idx), juce::dontSendNotification);
            gain.onValueChange = [this]
            {
                store.setParameter (xoa::ids::speakerGain, gain.getValue(), index);
            };
            addAndMakeVisible (gain);

            mute.setButtonText ("M");
            mute.setToggleState (static_cast<bool> (store.getParameter (ids::speakerMute, idx)),
                                 juce::dontSendNotification);
            mute.onClick = [this]
            {
                store.setParameter (xoa::ids::speakerMute, mute.getToggleState(), index);
            };
            addAndMakeVisible (mute);

            solo.setButtonText ("S");
            solo.setToggleState (static_cast<bool> (store.getParameter (ids::speakerSolo, idx)),
                                 juce::dontSendNotification);
            solo.onClick = [this]
            {
                store.setParameter (xoa::ids::speakerSolo, solo.getToggleState(), index);
            };
            addAndMakeVisible (solo);
        }

        void resized() override
        {
            auto r = getLocalBounds().reduced (2, 2);
            name.setBounds (r.removeFromLeft (48));
            solo.setBounds (r.removeFromRight (30));
            mute.setBounds (r.removeFromRight (30));
            gain.setBounds (r);
        }

        xoa::XoaValueTreeState& store;
        int index;
        juce::Label        name;
        juce::Slider       gain;
        juce::ToggleButton mute, solo;
    };

    xoa::XoaValueTreeState& store;
    std::vector<std::unique_ptr<Row>> rows;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SpeakerListComponent)
};
