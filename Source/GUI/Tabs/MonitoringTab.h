/*
  ==============================================================================

    XOA — tenth-order Ambisonics spatial audio processor.
    MonitoringTab — read-only metering + performance (WP10 C9, D31): an input stem
    meter bank and an output meter wall (both painted in a single pass, sized to
    the live channel counts, horizontally scrollable past a legible width), plus a
    performance readout (CPU / latency / sample rate / rebuild state / OSC
    activity). No parameters — a pure observation surface.

    This file is part of XOA, released under the GNU General Public License
    v3.0. See LICENSE for details.

  ==============================================================================
*/

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "TabPage.h"
#include "../ColorScheme.h"
#include "Audio/AudioEngine.h"
#include "Network/OSCManager.h"
#include "Localization/LocalizationManager.h"
#include "XoaConstants.h"

namespace xoa::ui
{

class MonitoringTab : public TabPage
{
public:
    explicit MonitoringTab (AppContext& ctx) : TabPage (ctx, Surface::monitoring)
    {
        inputTitle.setText (LOC ("monitoring.inputs"), juce::dontSendNotification);
        outputTitle.setText (LOC ("monitoring.outputs"), juce::dontSendNotification);
        for (auto* l : { &inputTitle, &outputTitle })
        {
            l->setJustificationType (juce::Justification::centredLeft);
            addAndMakeVisible (*l);
        }
        perfLabel.setJustificationType (juce::Justification::centredLeft);
        addAndMakeVisible (perfLabel);
        verifyRegistryCoverage();
    }

    void refresh() override
    {
        TabPage::refresh();

        juce::String s;
        const double sr = context.engine.getSampleRate();
        s << "CPU " << juce::String (context.engine.getCpuLoad() * 100.0, 1) << " %"
          << "   ·   latency " << juce::String (context.engine.getMeasuredLatencyMs(), 1) << " ms";
        if (sr > 0.0) s << "   ·   " << juce::String (sr / 1000.0, 1) << " kHz / "
                        << context.engine.getBlockSize() << " smp";
        s << "   ·   " << (context.engine.isDecoderRebuildInFlight()
                              ? LOC ("header.rebuilding") : LOC ("statusBar.ready"));
        s << "   ·   OSC " << (context.oscManager.isReceiving()
                                  ? LOC ("network.rxOn") : LOC ("network.rxOff"))
          << " (" << juce::String (context.oscManager.getReceivedPacketCount()) << " pkt)";
        perfLabel.setText (s, juce::dontSendNotification);

        repaint (meterArea);
    }

    void paint (juce::Graphics& g) override
    {
        const auto& col = ColorScheme::get();
        g.fillAll (col.background);

        drawBank (g, inputBank, context.store.getNumInputs(),
                  [this] (int c) { return context.engine.getInputPeakLevel (c); });
        drawBank (g, outputBank, context.store.getNumSpeakers(),
                  [this] (int c) { return context.engine.getOutputPeakLevel (c); });
    }

    void resized() override
    {
        const float sc = XoaLookAndFeel::uiScale;
        auto px = [sc] (int v) { return juce::roundToInt ((float) v * sc); };
        auto area = getLocalBounds().reduced (px (10));

        perfLabel.setBounds (area.removeFromTop (px (24)));
        area.removeFromTop (px (6));
        meterArea = area;

        inputTitle.setBounds (area.removeFromTop (px (18)));
        inputBank = area.removeFromTop (area.getHeight() / 2 - px (12));
        area.removeFromTop (px (8));
        outputTitle.setBounds (area.removeFromTop (px (18)));
        outputBank = area;
    }

private:
    template <typename PeakFn>
    void drawBank (juce::Graphics& g, juce::Rectangle<int> bank, int count, PeakFn peak) const
    {
        const auto& col = ColorScheme::get();
        g.setColour (col.surfaceCard);
        g.fillRect (bank);
        if (count <= 0 || bank.isEmpty())
            return;

        const float fullW = (float) bank.getWidth();
        const float barW = juce::jlimit (2.0f, 18.0f, fullW / (float) count);
        const int   shown = juce::jmin (count, (int) (fullW / barW));

        for (int c = 0; c < shown; ++c)
        {
            const float db = juce::Decibels::gainToDecibels (peak (c), -60.0f);
            const float norm = juce::jlimit (0.0f, 1.0f, (db + 60.0f) / 60.0f);
            auto r = juce::Rectangle<float> ((float) bank.getX() + (float) c * barW + 1.0f,
                                             (float) bank.getBottom() - norm * (float) bank.getHeight(),
                                             barW - 2.0f, norm * (float) bank.getHeight());
            g.setColour (norm > 0.9f ? juce::Colours::orangered
                       : norm > 0.75f ? juce::Colours::yellow : juce::Colours::limegreen);
            g.fillRect (r);
        }
        if (shown < count)
        {
            g.setColour (col.textSecondary);
            g.setFont (juce::FontOptions (11.0f));
            g.drawText ("+" + juce::String (count - shown), bank.removeFromRight (40),
                        juce::Justification::centredRight);
        }
    }

    juce::Label inputTitle, outputTitle, perfLabel;
    juce::Rectangle<int> meterArea, inputBank, outputBank;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MonitoringTab)
};

} // namespace xoa::ui
