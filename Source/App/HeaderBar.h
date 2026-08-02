/*
  ==============================================================================

    XOA — tenth-order Ambisonics spatial audio processor.
    HeaderBar — the persistent top strip (WP10 C5, decision D27): the HOA
    source latch (none / test scene — program material arrives from external
    players via the device inputs, D48), the three rotation dials (FR-10),
    master gain, and a live status readout (sample rate / latency / CPU /
    decoder-rebuild / OSC). These are performance controls that must never
    sit behind a tab switch.

    This file is part of XOA, released under the GNU General Public License
    v3.0. See LICENSE for details.

  ==============================================================================
*/

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <memory>

#include "../GUI/Tabs/TabPage.h"          // AppContext
#include "../GUI/Widgets/XoaBasicDial.h"
#include "../GUI/Widgets/XoaStandardSlider.h"

namespace xoa::ui
{

class HeaderBar : public juce::Component
{
public:
    explicit HeaderBar (AppContext& ctx);
    ~HeaderBar() override;

    void resized() override;

    /** App timer tick: the live status line. */
    void refresh();

private:
    AppContext& context;
    BindingSet  bindings;

    // HOA source (D48): a test-scene latch; no transport, no file.
    juce::TextButton testSceneButton;

    // Rotation (FR-10)
    XoaBasicDial yawDial, pitchDial, rollDial;
    juce::Label  yawLabel, pitchLabel, rollLabel;

    // Master
    XoaStandardSlider masterSlider;
    juce::TextEditor  masterEditor;
    juce::Label       masterLabel;

    // Live status
    juce::Label statusLabel;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (HeaderBar)
};

} // namespace xoa::ui
