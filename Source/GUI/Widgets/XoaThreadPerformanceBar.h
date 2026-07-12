/*
  ==============================================================================

    XOA — tenth-order Ambisonics spatial audio processor.
    XoaThreadPerformanceBar — small horizontal CPU-usage bar with a tooltip.
    Driven by setPerformance(cpuPercent, microseconds).

    Extracted from WFS-DIY (Source/gui/LevelMeterWindow.h, class
    ThreadPerformanceBar); both projects are GPLv3.
    This file is part of XOA, released under the GNU General Public License
    v3.0. See LICENSE for details.

  ==============================================================================
*/

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

/**
 * XoaThreadPerformanceBar
 * A small horizontal bar showing CPU usage percentage.
 */
class XoaThreadPerformanceBar : public juce::Component,
                                public juce::SettableTooltipClient
{
public:
    void setPerformance(float cpuPercent, float microseconds)
    {
        currentCpuPercent = cpuPercent;
        currentMicroseconds = microseconds;
        setTooltip(juce::String::formatted("%.1f%% | %.0f us", cpuPercent, microseconds));
        repaint();
    }

    /** GPU pipeline strip variant: percent-of-budget fill with a caller-built
        tooltip (setPerformance's default tooltip is CPU-thread-shaped). */
    void setPercent(float percentOfBudget, const juce::String& tooltipText)
    {
        currentCpuPercent = percentOfBudget;
        currentMicroseconds = 0.0f;
        setTooltip(tooltipText);
        repaint();
    }

    void paint(juce::Graphics& g) override
    {
        auto bounds = getLocalBounds().reduced(1);

        // Background
        g.setColour(juce::Colour(0xFF303030));  // Dark grey - visible against black background
        g.fillRoundedRectangle(bounds.toFloat(), 2.0f);

        // CPU bar
        float normalized = juce::jlimit(0.0f, 1.0f, currentCpuPercent / 100.0f);
        int barWidth = static_cast<int>(normalized * bounds.getWidth());
        if (barWidth > 0)
        {
            auto barRect = bounds.removeFromLeft(barWidth);
            g.setColour(getCpuColor(currentCpuPercent));
            g.fillRoundedRectangle(barRect.toFloat(), 2.0f);
        }
    }

private:
    static juce::Colour getCpuColor(float percent)
    {
        if (percent < 50.0f)
            return juce::Colours::green;
        else if (percent < 80.0f)
            return juce::Colours::yellow;
        else
            return juce::Colours::red;
    }

    float currentCpuPercent = 0.0f;
    float currentMicroseconds = 0.0f;
};
