/*
  ==============================================================================

    XOA — tenth-order Ambisonics spatial audio processor.
    HeaderBar — the persistent top strip (WP10 C5, decision D27): file transport,
    the three rotation dials (FR-10), master gain, and a live status readout
    (sample rate / latency / CPU / decoder-rebuild / OSC). These are performance
    controls that must never sit behind a tab switch.

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

    /** App timer tick: transport position, status line, loop -> FilePlayer. */
    void refresh();

private:
    void openFileDialog();

    AppContext& context;
    BindingSet  bindings;

    // Transport
    juce::TextButton  openButton  { "Open…" };
    juce::TextButton  playButton  { "Play" };
    juce::TextButton  stopButton  { "Stop" };
    juce::TextButton  loopButton  { "Loop" };   // latching (WFS toggle style)
    juce::ComboBox    sourceCombo;
    juce::Label       fileLabel;
    XoaStandardSlider positionSlider;
    bool              positionDragging = false;

    // Rotation (FR-10)
    XoaBasicDial yawDial, pitchDial, rollDial;
    juce::Label  yawLabel, pitchLabel, rollLabel;

    // Master
    XoaStandardSlider masterSlider;
    juce::TextEditor  masterEditor;
    juce::Label       masterLabel;

    // Live status
    juce::Label statusLabel;

    std::unique_ptr<juce::FileChooser> fileChooser;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (HeaderBar)
};

} // namespace xoa::ui
