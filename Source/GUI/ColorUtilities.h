/*
  ==============================================================================

    XOA — tenth-order Ambisonics spatial audio processor.
    XoaColorUtilities — shared colour maths: HSL marker colours (per input) and
    contrasting text colour selection. Used by the Map view and channel markers.

    Ported from WFS-DIY (Source/gui/ColorUtilities.h, namespace WfsColorUtilities);
    both projects are GPLv3.
    This file is part of XOA, released under the GNU General Public License
    v3.0. See LICENSE for details.

  ==============================================================================
*/

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <cmath>

/**
 * Shared color utilities for consistent coloring across the application.
 * Used by the Map view for source markers.
 */
namespace XoaColorUtilities
{
    /**
     * Get marker color for inputs.
     * HSL-based, spread evenly around the hue wheel.
     *
     * @param id The marker ID (1-based)
     * @param totalMarkers Number of distinct hues to spread across
     * @return HSL-based color for the marker
     */
    inline juce::Colour getMarkerColor(int id, int totalMarkers = 32)
    {
        const int n = juce::jmax(1, totalMarkers);
        float hue = std::fmod((id * 360.0f / (float) n), 360.0f) / 360.0f;
        return juce::Colour::fromHSL(hue, 0.9f, 0.6f, 1.0f);
    }

    /**
     * Get color for an input marker (convenience wrapper).
     *
     * @param inputId Input ID (1-based)
     * @return HSL-based color for the input
     */
    inline juce::Colour getInputColor(int inputId)
    {
        return getMarkerColor(inputId);
    }

    /**
     * Get contrasting text color (black or white) for a background color.
     * Uses relative luminance to determine readability.
     *
     * @param backgroundColor The background color to contrast against
     * @return Black for light backgrounds, white for dark backgrounds
     */
    inline juce::Colour getContrastingTextColor(const juce::Colour& backgroundColor)
    {
        // Calculate relative luminance using sRGB formula
        // https://www.w3.org/TR/WCAG20/#relativeluminancedef
        float r = backgroundColor.getFloatRed();
        float g = backgroundColor.getFloatGreen();
        float b = backgroundColor.getFloatBlue();

        // Apply gamma correction
        r = (r <= 0.03928f) ? r / 12.92f : std::pow((r + 0.055f) / 1.055f, 2.4f);
        g = (g <= 0.03928f) ? g / 12.92f : std::pow((g + 0.055f) / 1.055f, 2.4f);
        b = (b <= 0.03928f) ? b / 12.92f : std::pow((b + 0.055f) / 1.055f, 2.4f);

        float luminance = 0.2126f * r + 0.7152f * g + 0.0722f * b;

        // Use black text for light backgrounds (luminance > 0.4)
        return luminance > 0.4f ? juce::Colours::black : juce::Colours::white;
    }
}
