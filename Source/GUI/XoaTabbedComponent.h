/*
  ==============================================================================

    XOA — tenth-order Ambisonics spatial audio processor.
    XoaTabbedComponent — a TabbedComponent that reports tab changes (onTabChanged)
    and announces the newly-selected tab through the screen reader (accessibility).

    This file is part of XOA, released under the GNU General Public License
    v3.0. See LICENSE for details.

  ==============================================================================
*/

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "../Accessibility/TTSManager.h"

namespace xoa::ui
{

class XoaTabbedComponent : public juce::TabbedComponent
{
public:
    XoaTabbedComponent() : juce::TabbedComponent (juce::TabbedButtonBar::TabsAtTop) {}

    std::function<void (int)> onTabChanged;

    void currentTabChanged (int newCurrentTabIndex, const juce::String& newCurrentTabName) override
    {
        if (onTabChanged)
            onTabChanged (newCurrentTabIndex);

        TTSManager::getInstance().announceImmediate (newCurrentTabName);
    }

private:
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (XoaTabbedComponent)
};

} // namespace xoa::ui
