/*
  ==============================================================================

    XOA — tenth-order Ambisonics spatial audio processor.
    InputNudger — keyboard nudging of an input position, shared by the Inputs
    and Map tabs. Arrows move in the horizontal plane (frame is +X front,
    +Y LEFT, +Z up — so Left = +Y), PageUp/PageDown move Z. Step 0.1 m,
    Shift 0.01 m (the UI descriptor step), Ctrl 1 m. Writes go through the
    same path as a map drag: inputs undo domain + store.setParameter, tagged
    OriginTag::UI; a burst of keypresses (< 500 ms apart, same channel)
    coalesces into a single undo transaction like one drag gesture.

    This file is part of XOA, released under the GNU General Public License
    v3.0. See LICENSE for details.

  ==============================================================================
*/

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "spatcore/control/osc/OscTransportTypes.h"

#include "Parameters/XoaValueTreeState.h"
#include "Parameters/XoaConstraints.h"
#include "Localization/LocalizationManager.h"

namespace xoa::ui
{

class InputNudger
{
public:
    explicit InputNudger (XoaValueTreeState& s) : store (s) {}

    /** Returns true if the key was a nudge key and was applied to `channel`. */
    bool handleKey (const juce::KeyPress& key, int channel)
    {
        if (channel < 0 || channel >= store.getNumInputs())
            return false;

        const juce::Identifier* axis = nullptr;
        double sign = 1.0;
        const int code = key.getKeyCode();

        if      (code == juce::KeyPress::upKey)       { axis = &ids::inputPositionX; }
        else if (code == juce::KeyPress::downKey)     { axis = &ids::inputPositionX; sign = -1.0; }
        else if (code == juce::KeyPress::leftKey)     { axis = &ids::inputPositionY; }
        else if (code == juce::KeyPress::rightKey)    { axis = &ids::inputPositionY; sign = -1.0; }
        else if (code == juce::KeyPress::pageUpKey)   { axis = &ids::inputPositionZ; }
        else if (code == juce::KeyPress::pageDownKey) { axis = &ids::inputPositionZ; sign = -1.0; }
        else
            return false;

        double step = 0.1;
        if (key.getModifiers().isShiftDown())      step = 0.01;
        else if (key.getModifiers().isCtrlDown())  step = 1.0;

        const spatcore::control::osc::OriginTagScope origin { spatcore::control::osc::OriginTag::UI };
        XoaValueTreeState::ScopedDomain domain (store, XoaValueTreeState::inputsDomain);

        const auto now = juce::Time::getMillisecondCounter();
        if (now - lastNudgeMs > kBurstGapMs || channel != lastChannel)
            store.beginUndoTransaction (LOC ("inputs.nudge"));
        lastNudgeMs = now;
        lastChannel = channel;

        const double v = juce::jlimit (defaults::positionMin, defaults::positionMax,
                                       (double) store.getFloatParameter (*axis, channel) + sign * step);
        store.setParameter (*axis, v, channel);
        return true;
    }

private:
    static constexpr juce::uint32 kBurstGapMs = 500;

    XoaValueTreeState& store;
    juce::uint32 lastNudgeMs = 0;
    int lastChannel = -1;
};

} // namespace xoa::ui
