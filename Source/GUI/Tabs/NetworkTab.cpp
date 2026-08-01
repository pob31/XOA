/*
  ==============================================================================

    XOA — tenth-order Ambisonics spatial audio processor.
    NetworkTab implementation — see NetworkTab.h.

    This file is part of XOA, released under the GNU General Public License
    v3.0. See LICENSE for details.

  ==============================================================================
*/

#include "NetworkTab.h"

#include "Network/OSCManager.h"
#include "Localization/LocalizationManager.h"

namespace ids = xoa::ids;

namespace xoa::ui
{

NetworkTab::NetworkTab (AppContext& ctx) : TabPage (ctx, Surface::network)
{
    addAndMakeVisible (receiveGroup);
    addAndMakeVisible (sendGroup);
    addAndMakeVisible (feedbackGroup);
    receiveGroup.setText (LOC ("network.receive"));
    sendGroup.setText (LOC ("network.send"));
    feedbackGroup.setText (LOC ("network.feedback"));

    // Receive
    addAndMakeVisible (oscEnabledButton);
    bindings.bindToggle (oscEnabledButton, ids::oscEnabled);
    addField (receivePortEditor, "param.oscReceivePort");
    bindings.bindText (receivePortEditor, ids::oscReceivePort);
    addAndMakeVisible (tcpEnabledButton);
    bindings.bindToggle (tcpEnabledButton, ids::oscTcpEnabled);
    addField (tcpPortEditor, "param.oscTcpPort");
    bindings.bindText (tcpPortEditor, ids::oscTcpPort);
    addAndMakeVisible (acceptAnyHostButton);
    bindings.bindToggle (acceptAnyHostButton, ids::oscAcceptAnyHost);

    // Send target (single, D25)
    addField (sendAddressEditor, "param.oscSendAddress");
    bindings.bindText (sendAddressEditor, ids::oscSendAddress);
    addField (sendPortEditor, "param.oscSendPort");
    bindings.bindText (sendPortEditor, ids::oscSendPort);

    // Feedback / metering
    addAndMakeVisible (feedbackButton);
    bindings.bindToggle (feedbackButton, ids::oscFeedbackEnabled);
    addAndMakeVisible (meterButton);
    bindings.bindToggle (meterButton, ids::oscMeterEnabled);

    rxStatusLabel.setJustificationType (juce::Justification::centredLeft);
    addAndMakeVisible (rxStatusLabel);

    verifyRegistryCoverage();
}

void NetworkTab::addField (juce::Component& editor, const char* labelKey)
{
    auto* l = fieldLabels.add (new juce::Label());
    l->setText (LOC (labelKey), juce::dontSendNotification);
    l->setJustificationType (juce::Justification::centredRight);
    addAndMakeVisible (l);
    addAndMakeVisible (editor);
}

void NetworkTab::refresh()
{
    TabPage::refresh();
    juce::String s;
    s << (context.oscManager.isReceiving()
              ? LOC ("network.rxOn") + " :" + juce::String (context.oscManager.getUdpPort())
              : LOC ("network.rxOff"));
    rxStatusLabel.setText (s, juce::dontSendNotification);
}

void NetworkTab::resized()
{
    const float sc = XoaLookAndFeel::uiScale;
    auto px = [sc] (int v) { return juce::roundToInt ((float) v * sc); };
    auto area = getLocalBounds().reduced (px (12));

    const int rowH = px (32);
    const int rowGap = px (6);
    const int labelW = px (130);
    const int fieldW = px (220);
    const int buttonW = px (220);
    int fieldLabelIdx = 0;

    // Group frame sized FROM its row count — a fixed frame smaller than its
    // content is how the old layout crushed its last rows into slivers.
    const int groupPadTop = px (22), groupPadBottom = px (12), groupPadX = px (12);
    auto groupHeight = [&] (int rows)
    {
        return groupPadTop + groupPadBottom + rows * rowH + (rows - 1) * rowGap;
    };

    auto labelledRow = [&] (juce::Rectangle<int>& col, juce::Component& editor)
    {
        auto r = col.removeFromTop (rowH);
        if (fieldLabelIdx < fieldLabels.size())
            fieldLabels[fieldLabelIdx++]->setBounds (r.removeFromLeft (labelW));
        r.removeFromLeft (px (6));
        editor.setBounds (r.removeFromLeft (fieldW).reduced (0, px (2)));
        col.removeFromTop (rowGap);
    };
    auto plainRow = [&] (juce::Rectangle<int>& col, juce::Component& c)
    {
        c.setBounds (col.removeFromTop (rowH).removeFromLeft (buttonW).reduced (0, px (2)));
        col.removeFromTop (rowGap);
    };

    // Receive group — 5 rows
    {
        auto g = area.removeFromTop (groupHeight (5));
        receiveGroup.setBounds (g);
        auto inner = g.reduced (groupPadX, 0).withTrimmedTop (groupPadTop).withTrimmedBottom (groupPadBottom);
        plainRow (inner, oscEnabledButton);
        labelledRow (inner, receivePortEditor);
        plainRow (inner, tcpEnabledButton);
        labelledRow (inner, tcpPortEditor);
        plainRow (inner, acceptAnyHostButton);
    }
    area.removeFromTop (px (10));

    // Send group — 2 rows
    {
        auto g = area.removeFromTop (groupHeight (2));
        sendGroup.setBounds (g);
        auto inner = g.reduced (groupPadX, 0).withTrimmedTop (groupPadTop).withTrimmedBottom (groupPadBottom);
        labelledRow (inner, sendAddressEditor);
        labelledRow (inner, sendPortEditor);
    }
    area.removeFromTop (px (10));

    // Feedback group — 2 rows
    {
        auto g = area.removeFromTop (groupHeight (2));
        feedbackGroup.setBounds (g);
        auto inner = g.reduced (groupPadX, 0).withTrimmedTop (groupPadTop).withTrimmedBottom (groupPadBottom);
        plainRow (inner, feedbackButton);
        plainRow (inner, meterButton);
    }
    area.removeFromTop (px (10));

    rxStatusLabel.setBounds (area.removeFromTop (rowH));
}

} // namespace xoa::ui
