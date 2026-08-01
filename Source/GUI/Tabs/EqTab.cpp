/*
  ==============================================================================

    XOA — tenth-order Ambisonics spatial audio processor.
    EqTab implementation — see EqTab.h.

    This file is part of XOA, released under the GNU General Public License
    v3.0. See LICENSE for details.

  ==============================================================================
*/

#include "EqTab.h"

#include "Audio/AudioEngine.h"
#include "Localization/LocalizationManager.h"
#include "Parameters/XoaParameterDefaults.h"

namespace ids = xoa::ids;
namespace d   = xoa::defaults;

namespace xoa::ui
{

EqTab::EqTab (AppContext& ctx) : TabPage (ctx, Surface::eq)
{
    // --- Rail (same speaker list as the Speakers & Decoder tab) ----------
    rail.getCount   = [this] { return context.store.getNumSpeakers(); };
    rail.getRowText = [this] (int row)
    {
        juce::String t;
        t << (row + 1) << "  " << context.store.getStringParameter (ids::speakerName, row);
        if ((bool) context.store.getParameter (ids::speakerEqEnabled, row)) t << "  [EQ]";
        return t;
    };
    rail.onSelect = [this] (int row) { selectSpeaker (row); };
    addAndMakeVisible (rail);

    // --- Top row: enable + flatten ---------------------------------------
    addAndMakeVisible (eqEnabledButton);
    bindings.bindToggle (eqEnabledButton, ids::speakerEqEnabled, BindingSet::kCurrentChannel);

    flattenButton.setButtonText (LOC ("eq.flatten"));
    flattenButton.onLongPress = [this]
    {
        for (int b = 0; b < xoa::kNumEqBands; ++b)
            resetBand (b);
    };
    addAndMakeVisible (flattenButton);

    // --- Band columns -----------------------------------------------------
    for (int b = 0; b < xoa::kNumEqBands; ++b)
    {
        const auto i = (size_t) b;
        const auto colour = spatcore::ui::EQDisplayComponent::getBandColour (b);

        bandLabel[i].setText (LOC ("eq.band") + " " + juce::String (b + 1), juce::dontSendNotification);
        bandLabel[i].setJustificationType (juce::Justification::centredLeft);
        addAndMakeVisible (bandLabel[i]);   // colour applied by applyLabelColours()

        bandToggle[i].setBandColour (colour);
        bandToggle[i].colourProvider = [] { return xoa::speakerEqToggleColours(); };
        bandToggle[i].onClick = [this, b]
        {
            if (bandToggle[(size_t) b].getToggleState())
            {
                const int restored = lastShape[(size_t) b] > 0 ? lastShape[(size_t) b] : 3;  // peak
                writeBand (b, ids::eqShape, restored);
            }
            else
            {
                const int cur = (int) context.store.getEqBandParameter (currentSpeaker, b, ids::eqShape);
                if (cur > 0)
                    lastShape[(size_t) b] = cur;
                writeBand (b, ids::eqShape, 0);
            }
            if (display)
                display->setSelectedBand (b);
        };
        addAndMakeVisible (bandToggle[i]);

        bandReset[i].setButtonText (LOC ("common.reset"));
        bandReset[i].onLongPress = [this, b] { resetBand (b); };
        addAndMakeVisible (bandReset[i]);

        addAndMakeVisible (eqShape[i]);
        bindings.bindEqBandCombo (eqShape[i], ids::eqShape, b);
        eqShape[i].onChange = [this, b] { if (display) display->setSelectedBand (b); };

        eqFreq[i].setTrackColours (ColorScheme::get().sliderTrackBg, colour);
        addAndMakeVisible (eqFreq[i]);
        for (auto* dial : { &eqGain[i], &eqQ[i], &eqSlope[i] })
        {
            dial->setTrackColours (ColorScheme::get().sliderTrackBg, colour);
            addAndMakeVisible (*dial);
        }
        for (auto* v : { &eqFreqValue[i], &eqGainValue[i], &eqQValue[i], &eqSlopeValue[i] })
        {
            v->setJustificationType (juce::Justification::centred);
            v->setFont (juce::FontOptions (10.0f * XoaLookAndFeel::uiScale));
            addAndMakeVisible (*v);
        }
        auto header = [this] (juce::Label& l, const char* key)
        {
            l.setText (LOC (key), juce::dontSendNotification);
            l.setJustificationType (juce::Justification::centred);
            addAndMakeVisible (l);   // colour applied by applyLabelColours()
        };
        header (gainHdr[i],  "param.eqGain");
        header (qHdr[i],     "param.eqQ");
        header (slopeHdr[i], "param.eqSlope");

        bindings.bindKitEqBand  (eqFreq[i],  ids::eqFrequency, b, &eqFreqValue[i]);
        bindings.bindEqBandDial (eqGain[i],  ids::eqGain,      b, &eqGainValue[i]);
        bindings.bindEqBandDial (eqQ[i],     ids::eqQ,         b, &eqQValue[i]);
        bindings.bindEqBandDial (eqSlope[i], ids::eqSlope,     b, &eqSlopeValue[i]);
    }

    applyLabelColours();
    ColorScheme::Manager::getInstance().addListener (this);

    selectSpeaker (0);
    verifyRegistryCoverage();
}

EqTab::~EqTab()
{
    ColorScheme::Manager::getInstance().removeListener (this);
}

void EqTab::colorSchemeChanged()
{
    applyLabelColours();
    repaint();
}

void EqTab::applyLabelColours()
{
    const auto secondary = ColorScheme::get().textSecondary;
    for (int b = 0; b < xoa::kNumEqBands; ++b)
    {
        const auto i = (size_t) b;
        bandLabel[i].setColour (juce::Label::textColourId,
                                spatcore::ui::EQDisplayComponent::getBandColour (b));   // band rainbow: theme-invariant
        for (auto* l : { &gainHdr[i], &qHdr[i], &slopeHdr[i] })
            l->setColour (juce::Label::textColourId, secondary);
    }
}

void EqTab::selectSpeaker (int index)
{
    currentSpeaker = juce::jlimit (0, juce::jmax (0, context.store.getNumSpeakers() - 1), index);
    bindings.setChannel (currentSpeaker);
    rail.selectRow (currentSpeaker);
    rebuildDisplay();
    syncBandToggles();
}

void EqTab::rebuildDisplay()
{
    const int keepSelected = display ? display->getSelectedBand() : -1;
    display.reset();

    // The store's live eq subtree for this speaker (parent of the band nodes).
    auto eqTree = context.store.getSpeakerEqBand (currentSpeaker, 0).getParent();
    if (! eqTree.isValid())
        return;

    display = std::make_unique<spatcore::ui::EQDisplayComponent> (eqTree, xoa::kNumEqBands,
                                                                  xoa::makeSpeakerEQConfig());
    // XOA persists every edit itself, through the store's scoped-undo / band
    // seam in writeBand(). Without this the shared component would ALSO write
    // the tree directly (its default), double-writing and bypassing that seam.
    display->ownerPersistsValue = true;
    display->onParameterChanged = [this] (int band, const juce::Identifier& id, const juce::var& v)
    {
        writeBand (band, id, v);
    };
    display->setSampleRate (context.engine.getSampleRate());
    display->setEQEnabled ((bool) context.store.getParameter (ids::speakerEqEnabled, currentSpeaker));
    display->setSelectedBand (keepSelected);
    addAndMakeVisible (*display);
    resized();
}

void EqTab::writeBand (int band, const juce::Identifier& id, const juce::var& value)
{
    // Same seam as BindingSet::writeEqScoped: speakers undo-domain, band write.
    XoaValueTreeState::ScopedDomain dom (context.store, XoaValueTreeState::speakersDomain);
    context.store.setEqBandParameter (currentSpeaker, band, id, value);
    if (id == ids::eqShape)
        syncBandToggles();
}

void EqTab::resetBand (int band)
{
    writeBand (band, ids::eqShape,     (int) d::eqShapeDefault);
    writeBand (band, ids::eqFrequency, d::kEqBandDefaultHz[band]);
    writeBand (band, ids::eqGain,      d::eqGainDefault);
    writeBand (band, ids::eqQ,         d::eqQDefault);
    writeBand (band, ids::eqSlope,     d::eqSlopeDefault);
}

void EqTab::syncBandToggles()
{
    for (int b = 0; b < xoa::kNumEqBands; ++b)
    {
        const int shape = (int) context.store.getEqBandParameter (currentSpeaker, b, ids::eqShape);
        bandToggle[(size_t) b].setToggleState (shape != 0, juce::dontSendNotification);
        if (shape > 0)
            lastShape[(size_t) b] = shape;
    }
}

void EqTab::refresh()
{
    TabPage::refresh();
    rail.refresh();

    if (currentSpeaker >= context.store.getNumSpeakers())
        selectSpeaker (context.store.getNumSpeakers() - 1);

    if (display)
    {
        display->setSampleRate (context.engine.getSampleRate());
        display->setEQEnabled ((bool) context.store.getParameter (ids::speakerEqEnabled, currentSpeaker));
    }
    syncBandToggles();
}

void EqTab::resized()
{
    const float sc = XoaLookAndFeel::uiScale;
    auto px = [sc] (int v) { return juce::roundToInt ((float) v * sc); };
    auto area = getLocalBounds().reduced (px (10));

    rail.setBounds (area.removeFromLeft (px (180)));
    area.removeFromLeft (px (10));

    // Top row: enable (left) + flatten (right) — WFS-DIY arrangement.
    auto top = area.removeFromTop (px (32));
    eqEnabledButton.setBounds (top.removeFromLeft (px (110)));
    flattenButton.setBounds (top.removeFromRight (px (110)));
    area.removeFromTop (px (8));

    // Response graph: min 200, target ~40% of what's left (WFS-DIY sizing).
    if (display)
    {
        const int displayH = juce::jmax (px (200), area.getHeight() * 2 / 5);
        display->setBounds (area.removeFromTop (displayH));
        area.removeFromTop (px (8));
    }

    // Band columns.
    const int bandW = area.getWidth() / xoa::kNumEqBands;
    const int labelH  = px (18);
    const int rowH    = px (30);
    const int valueH  = px (15);
    const int toggleW = px (22);

    for (int b = 0; b < xoa::kNumEqBands; ++b)
    {
        const auto i = (size_t) b;
        auto col = area.removeFromLeft (bandW).reduced (px (5), 0);

        bandLabel[i].setBounds (col.removeFromTop (labelH));

        auto shapeRow = col.removeFromTop (rowH);
        bandToggle[i].setBounds (shapeRow.removeFromLeft (toggleW).withSizeKeepingCentre (toggleW, toggleW));
        shapeRow.removeFromLeft (px (4));
        bandReset[i].setBounds (shapeRow.removeFromRight (px (54)));
        shapeRow.removeFromRight (px (4));
        eqShape[i].setBounds (shapeRow);
        col.removeFromTop (px (5));

        eqFreq[i].setBounds (col.removeFromTop (px (28)));
        eqFreqValue[i].setBounds (col.removeFromTop (valueH));
        col.removeFromTop (px (5));

        // Gain | Q | Slope dials side by side, each header + dial + value.
        const int dialSize = juce::jmin (col.getWidth() / 3 - px (6),
                                         juce::jmax (px (40), px (56)));
        auto dialRow = col.removeFromTop (labelH + dialSize + valueH);
        const int cellW = dialRow.getWidth() / 3;

        auto dialCell = [&] (juce::Label& hdr, XoaBasicDial& dial, juce::Label& val)
        {
            auto cell = dialRow.removeFromLeft (cellW);
            hdr.setBounds (cell.removeFromTop (labelH));
            val.setBounds (cell.removeFromBottom (valueH));
            dial.setBounds (cell.withSizeKeepingCentre (dialSize, juce::jmin (dialSize, cell.getHeight())));
        };
        dialCell (gainHdr[i],  eqGain[i],  eqGainValue[i]);
        dialCell (qHdr[i],     eqQ[i],     eqQValue[i]);
        dialCell (slopeHdr[i], eqSlope[i], eqSlopeValue[i]);
    }
}

} // namespace xoa::ui
