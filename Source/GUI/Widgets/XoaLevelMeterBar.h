/*
  ==============================================================================

    XOA — tenth-order Ambisonics spatial audio processor.
    XoaLevelMeterBar — vertical peak+RMS meter bar with peak-hold and clip
    indicator. Self-contained: driven by setLevel(peakDb, rmsDb) from a timer.

    Extracted from WFS-DIY (Source/gui/LevelMeterWindow.h, class LevelMeterBar);
    both projects are GPLv3. The WFS window/content classes (which bind to the
    WFS engine + solo state) are not ported — only this display widget is.
    This file is part of XOA, released under the GNU General Public License
    v3.0. See LICENSE for details.

  ==============================================================================
*/

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

/**
 * XoaLevelMeterBar
 * A vertical meter bar showing peak and RMS levels with peak hold.
 */
class XoaLevelMeterBar : public juce::Component
{
public:
    XoaLevelMeterBar()
    {
        peakHoldTime = juce::Time::currentTimeMillis();
    }

    void setLevel(float peakDb, float rmsDb)
    {
        currentPeakDb = peakDb;
        currentRmsDb = rmsDb;

        // Update peak hold
        if (peakDb > peakHoldDb || (juce::Time::currentTimeMillis() - peakHoldTime) > 1500)
        {
            peakHoldDb = peakDb;
            peakHoldTime = juce::Time::currentTimeMillis();
        }

        repaint();
    }

    void setSoloHighlight(bool highlighted)
    {
        isSoloHighlighted = highlighted;
        repaint();
    }

    /** Enable contribution mode - shows calculated level from soloed input */
    void setContributionMode(bool enabled)
    {
        isContributionMode = enabled;
        if (!enabled)
            contributionDb = -200.0f;
        repaint();
    }

    /** Set the contribution level (input level + attenuation) */
    void setContributionLevel(float db)
    {
        contributionDb = db;
        repaint();
    }

    /** Compact mode: drop the contribution/solo frame and the border insets so a
        very narrow meter stays readable (the bar reclaims the frame's width).
        Driven by the output layout once meters get squeezed below a legible width. */
    void setCompact(bool c)
    {
        if (compact != c) { compact = c; repaint(); }
    }

    void paint(juce::Graphics& g) override
    {
        const int inset = compact ? 0 : 2;
        auto bounds = getLocalBounds().reduced(inset);

        // Background - darker purple tint when in contribution mode
        if (isContributionMode)
            g.setColour(juce::Colour(0xFF1A1A2E));  // Dark purple-ish background
        else
            g.setColour(juce::Colour(0xFF303030));  // Dark grey - visible against black background
        g.fillRoundedRectangle(bounds.toFloat(), 3.0f);

        // Calculate meter height (0 dB at top, -60 dB at bottom)
        auto meterBounds = bounds.reduced(inset);
        float meterHeight = static_cast<float>(meterBounds.getHeight());

        if (isContributionMode)
        {
            // Contribution mode: show calculated level with cyan/magenta color scheme
            float contribNormalized = juce::jlimit(0.0f, 1.0f, (contributionDb + 60.0f) / 60.0f);
            float contribHeight = contribNormalized * meterHeight;
            if (contribHeight > 1.0f)
            {
                auto contribRect = meterBounds.removeFromBottom(static_cast<int>(contribHeight));
                g.setColour(getContributionColor(contributionDb));
                g.fillRoundedRectangle(contribRect.toFloat(), 2.0f);
            }

            // Contribution level line at top of bar
            if (contribNormalized > 0.01f)
            {
                int contribY = bounds.getY() + 2 + static_cast<int>((1.0f - contribNormalized) * meterHeight);
                g.setColour(juce::Colours::white);
                g.fillRect(bounds.getX() + 2, contribY, bounds.getWidth() - 4, 2);
            }

            // Contribution mode border - cyan (dropped in compact mode so the
            // bar reclaims the frame inset when meters are very narrow)
            if (! compact)
            {
                g.setColour(juce::Colour(0xFF00BFFF));  // Deep sky blue
                g.drawRoundedRectangle(getLocalBounds().toFloat(), 3.0f, 2.0f);
            }
        }
        else
        {
            // Normal mode: RMS level (wider bar)
            float rmsNormalized = juce::jlimit(0.0f, 1.0f, (currentRmsDb + 60.0f) / 60.0f);
            float rmsHeight = rmsNormalized * meterHeight;
            if (rmsHeight > 1.0f)
            {
                auto rmsRect = meterBounds.removeFromBottom(static_cast<int>(rmsHeight));
                g.setColour(getLevelColor(currentRmsDb).withAlpha(0.7f));
                g.fillRoundedRectangle(rmsRect.toFloat(), 2.0f);
            }

            // Peak level (thin line)
            float peakNormalized = juce::jlimit(0.0f, 1.0f, (currentPeakDb + 60.0f) / 60.0f);
            int peakY = meterBounds.getY() + static_cast<int>((1.0f - peakNormalized) * meterHeight);
            if (peakNormalized > 0.01f)
            {
                g.setColour(getLevelColor(currentPeakDb));
                g.fillRect(bounds.getX() + 2, peakY, bounds.getWidth() - 4, 3);
            }

            // Peak hold line
            float holdNormalized = juce::jlimit(0.0f, 1.0f, (peakHoldDb + 60.0f) / 60.0f);
            int holdY = bounds.getY() + 2 + static_cast<int>((1.0f - holdNormalized) * meterHeight);
            if (holdNormalized > 0.01f)
            {
                g.setColour(juce::Colours::white);
                g.fillRect(bounds.getX() + 2, holdY, bounds.getWidth() - 4, 2);
            }

            // Solo highlight border (dropped in compact mode)
            if (isSoloHighlighted && ! compact)
            {
                g.setColour(juce::Colours::yellow);
                g.drawRoundedRectangle(getLocalBounds().toFloat(), 3.0f, 2.0f);
            }

            // Clip indicator
            if (currentPeakDb > -0.5f)
            {
                g.setColour(juce::Colours::red);
                g.fillRoundedRectangle(bounds.toFloat().removeFromTop(6), 2.0f);
            }
        }
    }

private:
    static juce::Colour getLevelColor(float db)
    {
        if (db < -12.0f)
            return juce::Colours::green;
        else if (db < -6.0f)
            return juce::Colours::yellow;
        else
            return juce::Colours::red;
    }

    /** Contribution mode color: cyan → blue → magenta gradient */
    static juce::Colour getContributionColor(float db)
    {
        // Map -60 to 0 dB to a cyan→magenta gradient
        float normalized = juce::jlimit(0.0f, 1.0f, (db + 60.0f) / 60.0f);

        // Cyan (0xFF00FFFF) at low levels → Magenta (0xFFFF00FF) at high levels
        // Through blue (0xFF0080FF) in the middle
        if (normalized < 0.5f)
        {
            // Cyan to Blue: increase red channel slightly, keep full blue
            float t = normalized * 2.0f;
            return juce::Colour::fromFloatRGBA(t * 0.5f, 1.0f - t * 0.5f, 1.0f, 0.85f);
        }
        else
        {
            // Blue to Magenta: increase red, decrease green
            float t = (normalized - 0.5f) * 2.0f;
            return juce::Colour::fromFloatRGBA(0.5f + t * 0.5f, 0.5f - t * 0.5f, 1.0f, 0.85f);
        }
    }

    float currentPeakDb = -200.0f;
    float currentRmsDb = -200.0f;
    float peakHoldDb = -200.0f;
    int64_t peakHoldTime = 0;
    bool isSoloHighlighted = false;
    bool compact = false;  // drop frame + insets when squeezed (set by output layout)

    // Contribution mode state
    bool isContributionMode = false;
    float contributionDb = -200.0f;
};
