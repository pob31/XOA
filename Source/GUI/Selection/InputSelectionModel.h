/*
  ==============================================================================

    XOA — tenth-order Ambisonics spatial audio processor.
    InputSelectionModel — the shared "current input" selection (one 0-based
    index), owned by the AppShell and handed to every tab via AppContext so the
    Inputs tab rail and the Map tab (2-D pick / 3-D pick) stay in sync.
    Message-thread only; clamping against the live input count stays in the
    callers (they know the count at the moment of use).

    This file is part of XOA, released under the GNU General Public License
    v3.0. See LICENSE for details.

  ==============================================================================
*/

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace xoa::ui
{

class InputSelectionModel
{
public:
    struct Listener
    {
        virtual ~Listener() = default;
        virtual void currentInputChanged (int newIndex) = 0;
    };

    int get() const noexcept { return current; }

    void set (int newIndex)
    {
        if (newIndex == current)
            return;
        current = newIndex;
        listeners.call ([newIndex] (Listener& l) { l.currentInputChanged (newIndex); });
    }

    void addListener (Listener* l)    { listeners.add (l); }
    void removeListener (Listener* l) { listeners.remove (l); }

private:
    int current = 0;
    juce::ListenerList<Listener> listeners;
};

} // namespace xoa::ui
