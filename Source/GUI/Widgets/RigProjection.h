/*
  ==============================================================================

    XOA — tenth-order Ambisonics spatial audio processor.
    RigProjection — 2-D projection of the 3-D rig for the layout preview (C7) and
    the Map tab (C9). Top-down plan view: +X (front) points up on screen, +Y (left)
    points left; a side view maps X across and Z up. Auto-fits to a world extent.

    This file is part of XOA, released under the GNU General Public License
    v3.0. See LICENSE for details.

  ==============================================================================
*/

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <algorithm>
#include <cmath>

namespace xoa::ui
{

struct RigProjection
{
    juce::Rectangle<float> bounds;   // target area on screen
    double extent = 5.0;             // half-range (metres) mapped to half the smaller side

    float scale() const
    {
        return 0.5f * juce::jmin (bounds.getWidth(), bounds.getHeight())
             / (float) juce::jmax (0.001, extent);
    }

    /** Top-down plan: +X front -> up, +Y left -> left. */
    juce::Point<float> planToScreen (double x, double y) const
    {
        const float s = scale();
        return { bounds.getCentreX() - (float) y * s, bounds.getCentreY() - (float) x * s };
    }

    /** Inverse of planToScreen (screen -> world XY). */
    juce::Point<double> screenToPlan (juce::Point<float> p) const
    {
        const float s = scale();
        return { (double) ((bounds.getCentreY() - p.y) / s),
                 (double) ((bounds.getCentreX() - p.x) / s) };
    }

    /** Side elevation: X across (front to the right), Z up. */
    juce::Point<float> sideToScreen (double x, double z) const
    {
        const float s = scale();
        return { bounds.getCentreX() + (float) x * s, bounds.getCentreY() - (float) z * s };
    }

    /** Fit `extent` to the largest |coordinate| across the points (+ margin). */
    template <typename PosArray>
    void fitTo (const PosArray& positions, int count, double margin = 1.2, double minExtent = 1.0)
    {
        double maxAbs = 0.0;
        for (int i = 0; i < count; ++i)
        {
            maxAbs = std::max (maxAbs, std::abs ((double) positions[i].x));
            maxAbs = std::max (maxAbs, std::abs ((double) positions[i].y));
            maxAbs = std::max (maxAbs, std::abs ((double) positions[i].z));
        }
        extent = std::max (minExtent, maxAbs * margin);
    }
};

} // namespace xoa::ui
