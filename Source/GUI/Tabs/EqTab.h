/*
  ==============================================================================

    XOA — tenth-order Ambisonics spatial audio processor.
    EqTab — dedicated per-speaker EQ surface, mirroring WFS-DIY's Output-EQ
    screen: speaker rail, EQ enable + flatten row, the interactive response
    graph (the shared spatcore::ui::EQDisplayComponent), and one colour-coded
    column per band with toggle / shape / reset, frequency slider and
    gain-Q-slope dials.

    This file is part of XOA, released under the GNU General Public License
    v3.0. See LICENSE for details.

  ==============================================================================
*/

#pragma once

#include <array>
#include <memory>

#include "spatcore/ui/EQBandToggle.h"
#include "spatcore/ui/EQDisplayComponent.h"

#include "TabPage.h"
#include "../ChannelRail.h"
#include "../EqDisplayStyle.h"
#include "../Widgets/LongPressButton.h"
#include "../Widgets/XoaBasicDial.h"
#include "../Widgets/XoaStandardSlider.h"
#include "XoaConstants.h"

namespace xoa::ui
{

class EqTab : public TabPage,
              private ColorScheme::Manager::Listener
{
public:
    explicit EqTab (AppContext& ctx);
    ~EqTab() override;

    void refresh() override;
    void resized() override;

private:
    /** Band/parameter labels take their colour from the palette, so re-apply it
        when the theme changes (a per-component setColour outranks the LNF). */
    void colorSchemeChanged() override;
    void applyLabelColours();

    void selectSpeaker (int index);
    void rebuildDisplay();
    void writeBand (int band, const juce::Identifier& id, const juce::var& value);
    void resetBand (int band);
    void syncBandToggles();

    ChannelRail rail;
    int currentSpeaker = 0;

    juce::TextButton eqEnabledButton { "EQ" };            // latching (speakerEqEnabled)
    LongPressButton  flattenButton;                       // long-press: reset every band

    std::unique_ptr<spatcore::ui::EQDisplayComponent> display;

    std::array<juce::Label,        xoa::kNumEqBands> bandLabel;
    std::array<spatcore::ui::EQBandToggle, xoa::kNumEqBands> bandToggle;
    std::array<LongPressButton,    xoa::kNumEqBands> bandReset;
    std::array<juce::ComboBox,     xoa::kNumEqBands> eqShape;
    std::array<XoaStandardSlider,  xoa::kNumEqBands> eqFreq;
    std::array<XoaBasicDial,       xoa::kNumEqBands> eqGain, eqQ, eqSlope;
    std::array<juce::Label,        xoa::kNumEqBands> eqFreqValue, eqGainValue, eqQValue, eqSlopeValue;
    std::array<juce::Label,        xoa::kNumEqBands> gainHdr, qHdr, slopeHdr;
    std::array<int,                xoa::kNumEqBands> lastShape {};   // restore on toggle-on

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EqTab)
};

} // namespace xoa::ui
