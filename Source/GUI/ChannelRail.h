/*
  ==============================================================================

    XOA — tenth-order Ambisonics spatial audio processor.
    ChannelRail — a virtualized ListBox of channels (inputs or speakers) for the
    detail-editor tabs (WP10 C6/C7). Supplies count/label via callbacks; reports
    selection. Scales to 64 inputs / 256 speakers without per-row components.

    This file is part of XOA, released under the GNU General Public License
    v3.0. See LICENSE for details.

  ==============================================================================
*/

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

#include "ColorScheme.h"

namespace xoa::ui
{

class ChannelRail : public juce::Component,
                    private juce::ListBoxModel
{
public:
    std::function<int()>                getCount;
    std::function<juce::String (int)>   getRowText;   // 0-based row
    std::function<void (int)>           onSelect;     // 0-based row

    ChannelRail()
    {
        list.setModel (this);
        list.setRowHeight (22);
        addAndMakeVisible (list);
    }

    /** Rebuild the row list if the count changed; refresh row text otherwise. */
    void refresh()
    {
        const int n = getCount ? getCount() : 0;
        if (n != lastCount)
        {
            lastCount = n;
            list.updateContent();
            if (list.getSelectedRow() >= n)
                list.selectRow (juce::jmax (0, n - 1));
        }
        else
        {
            list.repaint();   // names / mute glyphs may have changed
        }
    }

    void selectRow (int row) { list.selectRow (row); }
    int  getSelectedRow() const { return list.getSelectedRow(); }

    void resized() override { list.setBounds (getLocalBounds()); }

private:
    int getNumRows() override { return getCount ? getCount() : 0; }

    void paintListBoxItem (int row, juce::Graphics& g, int width, int height, bool selected) override
    {
        const auto& col = ColorScheme::get();
        g.fillAll (selected ? col.listSelection : (row % 2 ? col.listRowAlt : col.listBackground));
        g.setColour (col.textPrimary);
        g.setFont (juce::FontOptions (14.0f));
        const juce::String text = getRowText ? getRowText (row) : juce::String (row + 1);
        g.drawText (text, 6, 0, width - 8, height, juce::Justification::centredLeft, true);
    }

    void selectedRowsChanged (int lastRowSelected) override
    {
        if (onSelect && lastRowSelected >= 0)
            onSelect (lastRowSelected);
    }

    juce::ListBox list;
    int lastCount = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ChannelRail)
};

} // namespace xoa::ui
