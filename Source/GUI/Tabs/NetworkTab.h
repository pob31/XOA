/*
  ==============================================================================

    XOA — tenth-order Ambisonics spatial audio processor.
    NetworkTab — the OSC control-plane surface (WP10 C5). Exposes the WP9 OSC
    transport schema (enable, ports, TCP, host allow-list, feedback/meter toggles,
    single send target — D25) plus a live receive-status readout. Multi-target
    output is deferred to WP12 (OSC map §11).

    This file is part of XOA, released under the GNU General Public License
    v3.0. See LICENSE for details.

  ==============================================================================
*/

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "TabPage.h"

namespace xoa::ui
{

class NetworkTab : public TabPage
{
public:
    explicit NetworkTab (AppContext& ctx);

    void resized() override;
    void refresh() override;

private:
    void addField (juce::Component& editor, const char* labelKey);

    juce::GroupComponent receiveGroup, sendGroup, feedbackGroup;

    // Latching text buttons (WFS toggle style)
    juce::TextButton oscEnabledButton { "OSC receive" };
    juce::TextEditor receivePortEditor;
    juce::TextButton tcpEnabledButton { "TCP receive" };
    juce::TextEditor tcpPortEditor;
    juce::TextButton acceptAnyHostButton { "Accept any host" };

    juce::TextEditor sendAddressEditor;
    juce::TextEditor sendPortEditor;

    juce::TextButton feedbackButton { "Parameter feedback" };
    juce::TextButton meterButton { "Meter stream" };

    juce::Label rxStatusLabel;

    juce::OwnedArray<juce::Label> fieldLabels;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (NetworkTab)
};

} // namespace xoa::ui
