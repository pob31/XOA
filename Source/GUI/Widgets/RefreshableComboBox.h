/*
  ==============================================================================

    XOA — tenth-order Ambisonics spatial audio processor.
    RefreshableComboBox — a ComboBox that rescans its content just before the
    popup opens (ported from WFS-DIY's Source/gui/RefreshableComboBox.h).

    For lists whose content is discovered rather than declared — head trackers
    that come and go, SOFA files dropped into <project>/sofa — rebuilding on
    popup is what makes "plug it in, open the menu" work without a poll timer.

    This file is part of XOA, released under the GNU General Public License
    v3.0. See LICENSE for details.

  ==============================================================================
*/

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

namespace xoa::ui
{

class RefreshableComboBox : public juce::ComboBox
{
public:
    std::function<void()> onPopupAboutToShow;

    void showPopup() override
    {
        if (onPopupAboutToShow)
            onPopupAboutToShow();
        juce::ComboBox::showPopup();
    }
};

} // namespace xoa::ui
