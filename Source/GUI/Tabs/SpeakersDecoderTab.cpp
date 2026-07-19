/*
  ==============================================================================

    XOA — tenth-order Ambisonics spatial audio processor.
    SpeakersDecoderTab implementation — see SpeakersDecoderTab.h.

    This file is part of XOA, released under the GNU General Public License
    v3.0. See LICENSE for details.

  ==============================================================================
*/

#include "SpeakersDecoderTab.h"

#include "Audio/AudioEngine.h"
#include "Audio/TestSignalGenerator.h"
#include "DSP/AmbiDecoderDesigner.h"
#include "DSP/DecoderMatrixBuilder.h"
#include "Helpers/XoaCoordinates.h"
#include "Localization/LocalizationManager.h"
#include "../Analysis/RvReAnalysisService.h"

namespace ids = xoa::ids;

namespace xoa::ui
{

SpeakersDecoderTab::SpeakersDecoderTab (AppContext& ctx) : TabPage (ctx, Surface::speakersDecoder)
{
    for (auto* grp : { &speakerGroup, &eqGroup, &decoderGroup, &compGroup, &testGroup, &layoutGroup })
        addAndMakeVisible (*grp);
    speakerGroup.setText (LOC ("speakers.speaker"));
    eqGroup.setText (LOC ("speakers.eq"));
    decoderGroup.setText (LOC ("speakers.decoder"));
    compGroup.setText (LOC ("speakers.comp"));
    testGroup.setText (LOC ("speakers.test"));
    layoutGroup.setText (LOC ("speakers.layout"));

    auto valueEditor = [this] (juce::TextEditor& e)
    {
        styleValueEditor (e);
        addAndMakeVisible (e);
    };

    // --- Rail ------------------------------------------------------------
    rail.getCount   = [this] { return context.store.getNumSpeakers(); };
    rail.getRowText = [this] (int row)
    {
        juce::String t;
        t << (row + 1) << "  " << context.store.getStringParameter (ids::speakerName, row);
        if ((bool) context.store.getParameter (ids::speakerMute, row)) t << "  [M]";
        if ((bool) context.store.getParameter (ids::speakerSolo, row)) t << "  [S]";
        return t;
    };
    rail.onSelect = [this] (int row) { selectSpeaker (row); };
    addAndMakeVisible (rail);

    // --- Speaker detail --------------------------------------------------
    addLabel ("param.speakerCount");
    addAndMakeVisible (speakerCountSlider);
    bindings.bindSlider (speakerCountSlider, ids::speakerCount);

    addLabel ("param.speakerName");
    addAndMakeVisible (nameEditor);
    bindings.bindText (nameEditor, ids::speakerName, BindingSet::kCurrentChannel);

    addLabel ("param.speakerGain");
    gainSlider.setTrackColours (ColorScheme::get().sliderTrackBg, ColorScheme::accents::level);
    addAndMakeVisible (gainSlider);
    bindings.bindKitSlider (gainSlider, ids::speakerGain, BindingSet::kCurrentChannel);
    valueEditor (gainEditor);
    bindings.bindText (gainEditor, ids::speakerGain, BindingSet::kCurrentChannel);

    addLabel ("param.speakerDelay");
    delaySlider.setTrackColours (ColorScheme::get().sliderTrackBg, ColorScheme::accents::time);
    addAndMakeVisible (delaySlider);
    bindings.bindKitSlider (delaySlider, ids::speakerDelay, BindingSet::kCurrentChannel);
    valueEditor (delayEditor);
    bindings.bindText (delayEditor, ids::speakerDelay, BindingSet::kCurrentChannel);

    muteButton.setColour (juce::TextButton::buttonOnColourId, ColorScheme::accents::mute);
    addAndMakeVisible (muteButton);
    bindings.bindToggle (muteButton, ids::speakerMute, BindingSet::kCurrentChannel);
    soloButton.setColour (juce::TextButton::buttonOnColourId, ColorScheme::accents::solo);
    soloButton.setColour (juce::TextButton::textColourOnId, juce::Colours::black);
    addAndMakeVisible (soloButton);
    bindings.bindToggle (soloButton, ids::speakerSolo, BindingSet::kCurrentChannel);

    addLabel ("param.speakerPositionX");
    addLabel ("param.speakerPositionY");
    addLabel ("param.speakerPositionZ");
    for (auto* s : { &posXSlider, &posYSlider, &posZSlider })
    {
        s->setTrackColours (ColorScheme::get().sliderTrackBg, ColorScheme::accents::spatial);
        addAndMakeVisible (*s);
    }
    bindings.bindKitSlider (posXSlider, ids::speakerPositionX, BindingSet::kCurrentChannel);
    bindings.bindKitSlider (posYSlider, ids::speakerPositionY, BindingSet::kCurrentChannel);
    bindings.bindKitSlider (posZSlider, ids::speakerPositionZ, BindingSet::kCurrentChannel);
    valueEditor (posXEditor);
    valueEditor (posYEditor);
    valueEditor (posZEditor);
    bindings.bindText (posXEditor, ids::speakerPositionX, BindingSet::kCurrentChannel);
    bindings.bindText (posYEditor, ids::speakerPositionY, BindingSet::kCurrentChannel);
    bindings.bindText (posZEditor, ids::speakerPositionZ, BindingSet::kCurrentChannel);

    addLabel ("param.speakerCoordinateMode");
    addAndMakeVisible (coordModeCombo);
    bindings.bindCombo (coordModeCombo, ids::speakerCoordinateMode, BindingSet::kCurrentChannel);
    posReadout.setJustificationType (juce::Justification::centredLeft);
    addAndMakeVisible (posReadout);

    // --- EQ (6 bands) ----------------------------------------------------
    addAndMakeVisible (eqEnabledButton);
    bindings.bindToggle (eqEnabledButton, ids::speakerEqEnabled, BindingSet::kCurrentChannel);
    for (int b = 0; b < xoa::kNumEqBands; ++b)
    {
        const auto i = (size_t) b;
        addAndMakeVisible (eqShape[i]);
        bindings.bindEqBandCombo (eqShape[i], ids::eqShape, b);

        eqFreq[i].setTrackColours (ColorScheme::get().sliderTrackBg, ColorScheme::accents::freq);
        addAndMakeVisible (eqFreq[i]);
        for (auto* d : { &eqGain[i], &eqQ[i], &eqSlope[i] })
            addAndMakeVisible (*d);
        for (auto* v : { &eqFreqValue[i], &eqGainValue[i], &eqQValue[i], &eqSlopeValue[i] })
        {
            v->setJustificationType (juce::Justification::centred);
            v->setFont (juce::FontOptions (10.0f * XoaLookAndFeel::uiScale));
            addAndMakeVisible (*v);
        }

        bindings.bindKitEqBand  (eqFreq[i],  ids::eqFrequency, b, &eqFreqValue[i]);
        bindings.bindEqBandDial (eqGain[i],  ids::eqGain,      b, &eqGainValue[i]);
        bindings.bindEqBandDial (eqQ[i],     ids::eqQ,         b, &eqQValue[i]);
        bindings.bindEqBandDial (eqSlope[i], ids::eqSlope,     b, &eqSlopeValue[i]);
    }

    // --- Decoder ---------------------------------------------------------
    addLabel ("param.decoderType");
    addAndMakeVisible (decoderTypeCombo);
    bindings.bindCombo (decoderTypeCombo, ids::decoderType);
    addLabel ("param.decoderWeighting");
    addAndMakeVisible (weightingCombo);
    bindings.bindCombo (weightingCombo, ids::decoderWeighting);
    addLabel ("param.decoderNormalization");
    addAndMakeVisible (normalizationCombo);
    bindings.bindCombo (normalizationCombo, ids::decoderNormalization);
    addAndMakeVisible (dualBandButton);
    bindings.bindToggle (dualBandButton, ids::decoderDualBandEnabled);
    addLabel ("param.decoderCrossoverFrequency");
    crossoverSlider.setTrackColours (ColorScheme::get().sliderTrackBg, ColorScheme::accents::freq);
    addAndMakeVisible (crossoverSlider);
    bindings.bindKitSlider (crossoverSlider, ids::decoderCrossoverFrequency);
    valueEditor (crossoverEditor);
    bindings.bindText (crossoverEditor, ids::decoderCrossoverFrequency);
    suggestionLabel.setJustificationType (juce::Justification::centredLeft);
    addAndMakeVisible (suggestionLabel);
    rebuildButton.setButtonText (LOC ("speakers.rebuild"));
    rebuildButton.onClick = [this] { context.engine.flushDecoderRebuild(); updateSuggestion(); };
    addAndMakeVisible (rebuildButton);
    decoderStatusLabel.setJustificationType (juce::Justification::centredLeft);
    addAndMakeVisible (decoderStatusLabel);

    // --- Compensation + listener (D18) -----------------------------------
    addLabel ("param.distanceCompMode");
    addAndMakeVisible (distanceModeCombo);
    bindings.bindCombo (distanceModeCombo, ids::distanceCompMode);
    addLabel ("param.listenerX");
    addLabel ("param.listenerY");
    addLabel ("param.listenerZ");
    for (auto* s : { &listenerXSlider, &listenerYSlider, &listenerZSlider })
    {
        s->setTrackColours (ColorScheme::get().sliderTrackBg, ColorScheme::accents::spatial);
        addAndMakeVisible (*s);
    }
    bindings.bindKitSlider (listenerXSlider, ids::listenerX);
    bindings.bindKitSlider (listenerYSlider, ids::listenerY);
    bindings.bindKitSlider (listenerZSlider, ids::listenerZ);
    valueEditor (listenerXEditor);
    valueEditor (listenerYEditor);
    valueEditor (listenerZEditor);
    bindings.bindText (listenerXEditor, ids::listenerX);
    bindings.bindText (listenerYEditor, ids::listenerY);
    bindings.bindText (listenerZEditor, ids::listenerZ);

    // --- Test signal (engine-backed) -------------------------------------
    addLabel ("speakers.testType");
    testTypeCombo.addItem (LOC ("enum.testSignal.off"),    1);
    testTypeCombo.addItem (LOC ("enum.testSignal.pink"),   2);
    testTypeCombo.addItem (LOC ("enum.testSignal.tone"),   3);
    testTypeCombo.addItem (LOC ("enum.testSignal.sweep"),  4);
    testTypeCombo.addItem (LOC ("enum.testSignal.dirac"),  5);
    testTypeCombo.addItem (LOC ("enum.testSignal.speakerId"), 6);
    addAndMakeVisible (testTypeCombo);
    bindings.bindEngineCombo (testTypeCombo,
        [this] { return (int) context.engine.getTestSignalGenerator().getSignalType(); },
        [this] (int v) { context.engine.getTestSignalGenerator().setSignalType (
                             (xoa::TestSignalGenerator::SignalType) v); });

    addLabel ("speakers.testLevel");
    testLevelSlider.setRange (-92.0f, 0.0f);
    testLevelSlider.setTrackColours (ColorScheme::get().sliderTrackBg, ColorScheme::accents::level);
    addAndMakeVisible (testLevelSlider);
    testLevelValue.setJustificationType (juce::Justification::centredRight);
    addAndMakeVisible (testLevelValue);
    bindings.bindEngineKitValue (testLevelSlider,
        [this] { return (double) context.engine.getTestSignalGenerator().getLevelDb(); },
        [this] (double v) { context.engine.getTestSignalGenerator().setLevel ((float) v); });

    addLabel ("speakers.testFreq");
    testFreqSlider.setRange (20.0f, 20000.0f);
    testFreqSlider.setSkewMidPoint (1000.0f);
    testFreqSlider.setTrackColours (ColorScheme::get().sliderTrackBg, ColorScheme::accents::freq);
    addAndMakeVisible (testFreqSlider);
    testFreqValue.setJustificationType (juce::Justification::centredRight);
    addAndMakeVisible (testFreqValue);
    bindings.bindEngineKitValue (testFreqSlider,
        [this] { return (double) context.engine.getTestSignalGenerator().getFrequency(); },
        [this] (double v) { context.engine.getTestSignalGenerator().setFrequency ((float) v); });

    addLabel ("speakers.testChannel");
    testChannelSlider.setSliderStyle (juce::Slider::IncDecButtons);
    testChannelSlider.setTextBoxStyle (juce::Slider::TextBoxLeft, false, 44, 18);
    testChannelSlider.setRange (0.0, (double) (xoa::kMaxSpeakers - 1), 1.0);
    addAndMakeVisible (testChannelSlider);
    bindings.bindEngineValue (testChannelSlider,
        [this] { return (double) context.engine.getTestSignalGenerator().getOutputChannel(); },
        [this] (double v) { context.engine.getTestSignalGenerator().setOutputChannel ((int) v); });
    testInfoLabel.setJustificationType (juce::Justification::centredLeft);
    addAndMakeVisible (testInfoLabel);

    // Seed the (non-persisted) generator so the sliders show sensible values.
    context.engine.getTestSignalGenerator().setLevel (-40.0f);
    context.engine.getTestSignalGenerator().setFrequency (1000.0f);

    // --- Layout panel / rV-rE analysis (switched) ------------------------
    addAndMakeVisible (layoutPanel);
    addChildComponent (analysisPanel);
    layoutPanel.onApplied = [this]
    {
        context.engine.flushDecoderRebuild();
        if (context.refreshAllTabs) context.refreshAllTabs();
    };
    layoutViewButton.setButtonText (LOC ("speakers.viewLayout"));
    analysisViewButton.setButtonText (LOC ("speakers.viewAnalysis"));
    layoutViewButton.setClickingTogglesState (true);
    analysisViewButton.setClickingTogglesState (true);
    layoutViewButton.setRadioGroupId (7010);
    analysisViewButton.setRadioGroupId (7010);
    layoutViewButton.setToggleState (true, juce::dontSendNotification);
    layoutViewButton.onClick  = [this] { setBottomView (false); };
    analysisViewButton.onClick = [this] { setBottomView (true); };
    addAndMakeVisible (layoutViewButton);
    addAndMakeVisible (analysisViewButton);

    selectSpeaker (0);
    updateSuggestion();
    verifyRegistryCoverage();
}

void SpeakersDecoderTab::setBottomView (bool showAnalysis)
{
    layoutPanel.setVisible (! showAnalysis);
    analysisPanel.setVisible (showAnalysis);
}

juce::Label& SpeakersDecoderTab::addLabel (const char* labelKey, juce::Justification just)
{
    auto* l = labels.add (new juce::Label());
    l->setText (LOC (labelKey), juce::dontSendNotification);
    l->setJustificationType (just);
    addAndMakeVisible (l);
    return *l;
}

void SpeakersDecoderTab::selectSpeaker (int index)
{
    currentSpeaker = juce::jlimit (0, juce::jmax (0, context.store.getNumSpeakers() - 1), index);
    bindings.setChannel (currentSpeaker);
    rail.selectRow (currentSpeaker);
}

void SpeakersDecoderTab::updateSuggestion()
{
    const auto layout = xoa::DecoderMatrixBuilder::layoutFromStore (context.store);
    const auto c = xoa::decoder::classify (layout);

    const char* klass = c.layoutClass == xoa::decoder::LayoutClass::ring   ? "ring"
                      : c.layoutClass == xoa::decoder::LayoutClass::dome   ? "dome"
                      : c.layoutClass == xoa::decoder::LayoutClass::sphere ? "sphere" : "irregular";
    const char* suggest = c.suggestedDecoderType == 0 ? "SAD"
                        : c.suggestedDecoderType == 1 ? "Mode-matching" : "AllRAD";

    double meanR = 0.0;
    for (int s = 0; s < layout.count; ++s)
    {
        const auto& p = layout.positions[s];
        meanR += std::sqrt (p.x * p.x + p.y * p.y + p.z * p.z);
    }
    if (layout.count > 0) meanR /= (double) layout.count;

    suggestionLabel.setText (juce::String (layout.count) + " spk · " + klass
                                 + " · suggest " + suggest + " · ~"
                                 + juce::String (juce::roundToInt (xoa::decoder::suggestedCrossoverHz (meanR)))
                                 + " Hz",
                             juce::dontSendNotification);
}

void SpeakersDecoderTab::refresh()
{
    TabPage::refresh();
    rail.refresh();

    if (currentSpeaker >= context.store.getNumSpeakers())
        selectSpeaker (context.store.getNumSpeakers() - 1);

    const xoa::coords::Cartesian c {
        context.store.getFloatParameter (ids::speakerPositionX, currentSpeaker),
        context.store.getFloatParameter (ids::speakerPositionY, currentSpeaker),
        context.store.getFloatParameter (ids::speakerPositionZ, currentSpeaker) };
    posReadout.setText (xoa::coords::formatForDisplay (
                            c, (xoa::coords::Mode) context.store.getIntParameter (ids::speakerCoordinateMode, currentSpeaker)),
                        juce::dontSendNotification);

    // Decoder status from the last build result.
    const auto& r = context.engine.getDecoderBuilder().lastDesignResult();
    juce::String s;
    s << "order " << r.designOrder;
    if (r.conditionWarning) s << " · κ=" << juce::String (r.conditionNumber, 1);
    if (context.engine.isDecoderRebuildInFlight()) s << " · " << LOC ("header.rebuilding");
    decoderStatusLabel.setText (s, juce::dontSendNotification);

    // Test-signal feedback.
    auto& gen = context.engine.getTestSignalGenerator();
    testLevelValue.setText (juce::String (gen.getLevelDb(), 1) + " dB", juce::dontSendNotification);
    testFreqValue.setText (juce::String (juce::roundToInt (gen.getFrequency())) + " Hz", juce::dontSendNotification);
    if (gen.getSignalType() == xoa::TestSignalGenerator::SignalType::SpeakerId)
    {
        const int spk = gen.getCurrentSpeakerIndex();
        testInfoLabel.setText (spk >= 0 ? "Spk " + juce::String (spk + 1) : "(gap)", juce::dontSendNotification);
    }
    else
    {
        testInfoLabel.setText (gen.isActive() ? "-> ch " + juce::String (gen.getOutputChannel()) : juce::String(),
                               juce::dontSendNotification);
    }

    updateSuggestion();

    // Pick up a freshly-published rV/rE analysis (computed off the message thread).
    if (context.analysis != nullptr)
    {
        auto latest = context.analysis->latest();
        if (latest && latest->generation != lastAnalysisGen)
        {
            lastAnalysisGen = latest->generation;
            analysisPanel.setResult (latest);
        }
    }
}

void SpeakersDecoderTab::resized()
{
    const float sc = XoaLookAndFeel::uiScale;
    auto px = [sc] (int v) { return juce::roundToInt ((float) v * sc); };
    auto area = getLocalBounds().reduced (px (8));

    rail.setBounds (area.removeFromLeft (px (180)));
    area.removeFromLeft (px (8));

    // Two columns: left = speaker detail + EQ; right = decoder + comp + test + layout.
    auto left = area.removeFromLeft (area.getWidth() / 2 - px (4));
    area.removeFromLeft (px (8));
    auto right = area;

    const int rowH = px (24);
    const int labelW = px (110);
    int li = 0;
    auto labelled = [&] (juce::Rectangle<int>& colr, juce::Component& c, int cw = 0)
    {
        auto r = colr.removeFromTop (rowH);
        if (li < labels.size()) labels[li++]->setBounds (r.removeFromLeft (labelW));
        r.removeFromLeft (px (4));
        c.setBounds ((cw > 0 ? r.removeFromLeft (cw) : r).reduced (0, px (2)));
        colr.removeFromTop (px (3));
    };
    // Kit-slider row: label | track | value (editor or readout) on the right.
    auto labelledSlider = [&] (juce::Rectangle<int>& colr, juce::Component& slider, juce::Component& value)
    {
        auto r = colr.removeFromTop (rowH);
        if (li < labels.size()) labels[li++]->setBounds (r.removeFromLeft (labelW));
        r.removeFromLeft (px (4));
        value.setBounds (r.removeFromRight (px (56)).reduced (0, px (3)));
        r.removeFromRight (px (4));
        slider.setBounds (r.reduced (0, px (2)));
        colr.removeFromTop (px (3));
    };

    // Left column
    {
        auto g = left.removeFromTop (px (330));
        speakerGroup.setBounds (g);
        auto in = g.reduced (px (10), px (18));
        labelled (in, speakerCountSlider, px (110));
        labelled (in, nameEditor, px (200));
        labelledSlider (in, gainSlider, gainEditor);
        labelledSlider (in, delaySlider, delayEditor);
        auto ms = in.removeFromTop (rowH);
        muteButton.setBounds (ms.removeFromLeft (px (60)));
        soloButton.setBounds (ms.removeFromLeft (px (60)));
        in.removeFromTop (px (3));
        labelledSlider (in, posXSlider, posXEditor);
        labelledSlider (in, posYSlider, posYEditor);
        labelledSlider (in, posZSlider, posZEditor);
        labelled (in, coordModeCombo, px (140));
        posReadout.setBounds (in.removeFromTop (rowH));
    }
    left.removeFromTop (px (6));
    {
        auto g = left;
        eqGroup.setBounds (g);
        auto in = g.reduced (px (10), px (18));
        eqEnabledButton.setBounds (in.removeFromTop (rowH).removeFromLeft (px (80)));
        in.removeFromTop (px (4));
        const int bandW = juce::jmax (px (48), in.getWidth() / xoa::kNumEqBands);
        const int valueH = px (13);
        for (int b = 0; b < xoa::kNumEqBands; ++b)
        {
            const auto i = (size_t) b;
            auto col = in.removeFromLeft (bandW).reduced (px (2), 0);
            eqShape[i].setBounds (col.removeFromTop (rowH));
            col.removeFromTop (px (2));
            const int cellH = juce::jmax (px (40), col.getHeight() / 4);
            auto cell = [&] (juce::Component& w, juce::Label& v)
            {
                auto c = col.removeFromTop (cellH);
                v.setBounds (c.removeFromBottom (valueH));
                w.setBounds (c);
            };
            cell (eqFreq[i],  eqFreqValue[i]);
            cell (eqGain[i],  eqGainValue[i]);
            cell (eqQ[i],     eqQValue[i]);
            cell (eqSlope[i], eqSlopeValue[i]);
        }
    }

    // Right column
    {
        auto g = right.removeFromTop (px (200));
        decoderGroup.setBounds (g);
        auto in = g.reduced (px (10), px (18));
        labelled (in, decoderTypeCombo, px (150));
        labelled (in, weightingCombo, px (150));
        labelled (in, normalizationCombo, px (150));
        auto db = in.removeFromTop (rowH);
        dualBandButton.setBounds (db.removeFromLeft (px (100)));
        rebuildButton.setBounds (db.removeFromRight (px (100)));
        in.removeFromTop (px (3));
        labelledSlider (in, crossoverSlider, crossoverEditor);
        suggestionLabel.setBounds (in.removeFromTop (rowH));
        decoderStatusLabel.setBounds (in.removeFromTop (rowH));
    }
    right.removeFromTop (px (6));
    {
        auto g = right.removeFromTop (px (150));
        compGroup.setBounds (g);
        auto in = g.reduced (px (10), px (18));
        labelled (in, distanceModeCombo, px (150));
        labelledSlider (in, listenerXSlider, listenerXEditor);
        labelledSlider (in, listenerYSlider, listenerYEditor);
        labelledSlider (in, listenerZSlider, listenerZEditor);
    }
    right.removeFromTop (px (6));
    {
        auto g = right.removeFromTop (px (150));
        testGroup.setBounds (g);
        auto in = g.reduced (px (10), px (18));
        labelled (in, testTypeCombo, px (140));
        labelledSlider (in, testLevelSlider, testLevelValue);
        labelledSlider (in, testFreqSlider, testFreqValue);
        labelled (in, testChannelSlider, px (110));
        testInfoLabel.setBounds (in.removeFromTop (rowH));
    }
    right.removeFromTop (px (6));
    {
        layoutGroup.setBounds (right);
        auto in = right.reduced (px (10), px (18));
        auto toggle = in.removeFromTop (px (24));
        layoutViewButton.setBounds (toggle.removeFromLeft (px (80)));
        toggle.removeFromLeft (px (4));
        analysisViewButton.setBounds (toggle.removeFromLeft (px (90)));
        in.removeFromTop (px (4));
        layoutPanel.setBounds (in);
        analysisPanel.setBounds (in);
    }
}

} // namespace xoa::ui
