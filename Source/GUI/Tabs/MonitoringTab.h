/*
  ==============================================================================

    XOA — tenth-order Ambisonics spatial audio processor.
    MonitoringTab — metering + performance (WP10 C9, D31) plus the binaural
    monitor controls (WP15): an input stem meter bank and an output meter wall
    (both painted in a single pass, sized to the live channel counts,
    horizontally scrollable past a legible width), a performance readout (CPU /
    latency / sample rate / rebuild state / OSC activity), and the binaural
    group — enable, monitor gain, head-tracker selection with Set Zero, manual
    head attitude, and a live orientation readout.

    Head-tracker UX rules carried over from WFS-DIY:
      - the tracker list rescans when the popup opens (RefreshableComboBox), so
        plugging a camera in and opening the menu is enough;
      - a persisted tracker id whose device is absent shows as Manual WITHOUT
        overwriting the stored id (the combo is set with dontSendNotification),
        so the selection returns when the device does;
      - Set Zero is only visible while a tracker is actually active.

    This file is part of XOA, released under the GNU General Public License
    v3.0. See LICENSE for details.

  ==============================================================================
*/

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "TabPage.h"
#include "../ColorScheme.h"
#include "../Widgets/RefreshableComboBox.h"
#include "../Widgets/XoaBasicDial.h"
#include "Audio/AudioEngine.h"
#include "Network/OSCManager.h"
#include "Localization/LocalizationManager.h"
#include "XoaConstants.h"

#include <vector>

namespace xoa::ui
{

class MonitoringTab : public TabPage
{
public:
    explicit MonitoringTab (AppContext& ctx) : TabPage (ctx, Surface::monitoring)
    {
        inputTitle.setText (LOC ("monitoring.inputs"), juce::dontSendNotification);
        outputTitle.setText (LOC ("monitoring.outputs"), juce::dontSendNotification);
        for (auto* l : { &inputTitle, &outputTitle })
        {
            l->setJustificationType (juce::Justification::centredLeft);
            addAndMakeVisible (*l);
        }
        perfLabel.setJustificationType (juce::Justification::centredLeft);
        addAndMakeVisible (perfLabel);

        buildBinauralControls();
        verifyRegistryCoverage();
    }

    void refresh() override
    {
        TabPage::refresh();

        juce::String s;
        const double sr = context.engine.getSampleRate();
        s << "CPU " << juce::String (context.engine.getCpuLoad() * 100.0, 1) << " %"
          << "   ·   latency " << juce::String (context.engine.getMeasuredLatencyMs(), 1) << " ms";
        if (sr > 0.0) s << "   ·   " << juce::String (sr / 1000.0, 1) << " kHz / "
                        << context.engine.getBlockSize() << " smp";
        s << "   ·   " << (context.engine.isDecoderRebuildInFlight()
                              ? LOC ("header.rebuilding") : LOC ("statusBar.ready"));
        s << "   ·   OSC " << (context.oscManager.isReceiving()
                                  ? LOC ("network.rxOn") : LOC ("network.rxOff"))
          << " (" << juce::String (context.oscManager.getReceivedPacketCount()) << " pkt)";
        perfLabel.setText (s, juce::dontSendNotification);

        refreshOrientationReadout();

        repaint (meterArea);
    }

    void paint (juce::Graphics& g) override
    {
        const auto& col = ColorScheme::get();
        g.fillAll (col.background);

        drawBank (g, inputBank, context.store.getNumInputs(),
                  [this] (int c) { return context.engine.getInputPeakLevel (c); });
        drawBank (g, outputBank, context.store.getNumSpeakers(),
                  [this] (int c) { return context.engine.getOutputPeakLevel (c); });
    }

    void resized() override
    {
        const float sc = XoaLookAndFeel::uiScale;
        auto px = [sc] (int v) { return juce::roundToInt ((float) v * sc); };
        auto area = getLocalBounds().reduced (px (10));

        perfLabel.setBounds (area.removeFromTop (px (24)));
        area.removeFromTop (px (6));

        layoutBinauralControls (area.removeFromTop (px (118)));
        area.removeFromTop (px (8));
        meterArea = area;

        inputTitle.setBounds (area.removeFromTop (px (18)));
        inputBank = area.removeFromTop (area.getHeight() / 2 - px (12));
        area.removeFromTop (px (8));
        outputTitle.setBounds (area.removeFromTop (px (18)));
        outputBank = area;
    }

private:
    //==========================================================================
    // Binaural monitor (WP15)
    //==========================================================================

    void layoutBinauralControls (juce::Rectangle<int> area)
    {
        const float sc = XoaLookAndFeel::uiScale;
        auto px = [sc] (int v) { return juce::roundToInt ((float) v * sc); };

        binauralTitle.setBounds (area.removeFromTop (px (18)));
        area.removeFromTop (px (4));

        // Row 1: enable | gain | tracker | Set Zero | camera index
        auto row = area.removeFromTop (px (24));
        enableButton.setBounds (row.removeFromLeft (px (140)));
        row.removeFromLeft (px (8));
        gainLabel.setBounds (row.removeFromLeft (px (80)));
        gainSlider.setBounds (row.removeFromLeft (px (160)));
        row.removeFromLeft (px (12));
        trackerLabel.setBounds (row.removeFromLeft (px (90)));
        trackerCombo.setBounds (row.removeFromLeft (px (180)));
        if (setZeroButton.isVisible())
        {
            row.removeFromLeft (px (8));
            setZeroButton.setBounds (row.removeFromLeft (px (90)));
        }
        row.removeFromLeft (px (12));
        cameraIndexLabel.setBounds (row.removeFromLeft (px (90)));
        cameraIndexEditor.setBounds (row.removeFromLeft (px (50)));

        area.removeFromTop (px (6));
        headingLabel.setBounds (area.removeFromTop (px (16)));

        // Row 2: the three manual attitude dials, each over its value label.
        auto dials = area.removeFromTop (px (48));
        auto placeDial = [&] (XoaBasicDial& dial, juce::Label& value)
        {
            auto cell = dials.removeFromLeft (px (110));
            dial.setBounds (cell.removeFromLeft (px (48)));
            value.setBounds (cell.reduced (px (2), px (14)));
        };
        placeDial (yawDial, yawValue);
        placeDial (pitchDial, pitchValue);
        placeDial (rollDial, rollValue);

        orientationReadout.setBounds (dials.removeFromTop (px (20)));
        statusLabel.setBounds (dials.removeFromTop (px (18)));
    }

    void buildBinauralControls()
    {
        binauralTitle.setText (LOC ("monitoring.binaural"), juce::dontSendNotification);
        binauralTitle.setJustificationType (juce::Justification::centredLeft);
        addAndMakeVisible (binauralTitle);

        enableButton.setButtonText (LOC ("param.binauralEnabled"));
        addAndMakeVisible (enableButton);
        bindings.bindToggle (enableButton, ids::binauralEnabled);

        gainLabel.setText (LOC ("param.binauralGain"), juce::dontSendNotification);
        gainLabel.setJustificationType (juce::Justification::centredRight);
        addAndMakeVisible (gainLabel);
        addAndMakeVisible (gainSlider);
        bindings.bindSlider (gainSlider, ids::binauralGain);

        trackerLabel.setText (LOC ("param.binauralHeadTracker"), juce::dontSendNotification);
        trackerLabel.setJustificationType (juce::Justification::centredRight);
        addAndMakeVisible (trackerLabel);

        // Rescan on popup: a camera plugged in after launch shows up without
        // any polling (WFS-DIY idiom).
        trackerCombo.onPopupAboutToShow = [this] { rebuildTrackerList(); };
        trackerCombo.onChange = [this]
        {
            const int index = trackerCombo.getSelectedId() - 1;
            if (index >= 0 && index < (int) trackerIds.size())
                context.store.setParameter (ids::binauralHeadTracker, trackerIds[(size_t) index]);
            updateSetZeroVisibility();
        };
        addAndMakeVisible (trackerCombo);

        setZeroButton.setButtonText (LOC ("monitoring.setZero"));
        setZeroButton.onClick = [this] { context.engine.getMonitoringEngine().setZeroOnActiveSource(); };
        addChildComponent (setZeroButton);   // visible only while a tracker is active

        cameraIndexLabel.setText (LOC ("param.binauralCameraIndex"), juce::dontSendNotification);
        cameraIndexLabel.setJustificationType (juce::Justification::centredRight);
        addAndMakeVisible (cameraIndexLabel);
        addAndMakeVisible (cameraIndexEditor);
        bindings.bindText (cameraIndexEditor, ids::binauralCameraIndex);

        headingLabel.setText (LOC ("monitoring.headOrientation"), juce::dontSendNotification);
        headingLabel.setJustificationType (juce::Justification::centredLeft);
        addAndMakeVisible (headingLabel);

        auto setupDial = [this] (XoaBasicDial& dial, juce::Label& valueLabel,
                                 const juce::Identifier& id)
        {
            addAndMakeVisible (dial);
            valueLabel.setJustificationType (juce::Justification::centred);
            addAndMakeVisible (valueLabel);
            bindings.bindDial (dial, id, -1, &valueLabel);
        };
        setupDial (yawDial,   yawValue,   ids::binauralManualYaw);
        setupDial (pitchDial, pitchValue, ids::binauralManualPitch);
        setupDial (rollDial,  rollValue,  ids::binauralManualRoll);

        orientationReadout.setJustificationType (juce::Justification::centredLeft);
        addAndMakeVisible (orientationReadout);

        statusLabel.setJustificationType (juce::Justification::centredLeft);
        addAndMakeVisible (statusLabel);

        rebuildTrackerList();
    }

    /** Rebuild the tracker list from the live sources. A persisted id whose
        device is missing leaves the combo on Manual and is written back with
        dontSendNotification, so onChange never fires and the store keeps the
        id for when the device returns. */
    void rebuildTrackerList()
    {
        const juce::String persisted = context.store.getStringParameter (ids::binauralHeadTracker);

        trackerCombo.clear (juce::dontSendNotification);
        trackerIds.clear();

        int selectedId = 1;   // manual
        int itemId = 1;
        for (const auto& s : context.engine.getMonitoringEngine().enumerateSources())
        {
            juce::String text = s.id == HeadTrackerManager::kManualId
                                    ? LOC ("monitoring.trackerManual") : s.displayName;
            if (s.id != HeadTrackerManager::kManualId && ! s.connected)
                text += " (" + LOC ("monitoring.trackerMissing") + ")";

            trackerCombo.addItem (text, itemId);
            trackerIds.push_back (s.id);
            if (s.id == persisted)
                selectedId = itemId;
            ++itemId;
        }

        trackerCombo.setSelectedId (selectedId, juce::dontSendNotification);
        updateSetZeroVisibility();
    }

    void updateSetZeroVisibility()
    {
        setZeroButton.setVisible (context.engine.getMonitoringEngine().isTrackerActive());
        resized();
    }

    void refreshOrientationReadout()
    {
        auto& monitor = context.engine.getMonitoringEngine();

        bool tracked = false;
        const auto o = monitor.getEffectiveOrientation (tracked);
        auto deg = [] (float rad) { return juce::String (juce::radiansToDegrees (rad), 1); };

        juce::String text = "yaw " + deg (o.yawRad) + "°   pitch " + deg (o.pitchRad)
                          + "°   roll " + deg (o.rollRad) + "°";
        if (! tracked)
            text += "   (" + LOC ("monitoring.orientationManual") + ")";
        orientationReadout.setText (text, juce::dontSendNotification);

        const auto status = monitor.getTrackerStatusMessage();
        if (status != lastStatus)
        {
            lastStatus = status;
            statusLabel.setText (status, juce::dontSendNotification);
        }

        // The active source can change without a UI event (a tracker failing
        // to start resolves to manual), so keep the button honest.
        if (setZeroButton.isVisible() != monitor.isTrackerActive())
            updateSetZeroVisibility();
    }

    //==========================================================================
    template <typename PeakFn>
    void drawBank (juce::Graphics& g, juce::Rectangle<int> bank, int count, PeakFn peak) const
    {
        const auto& col = ColorScheme::get();
        g.setColour (col.surfaceCard);
        g.fillRect (bank);
        if (count <= 0 || bank.isEmpty())
            return;

        const float fullW = (float) bank.getWidth();
        const float barW = juce::jlimit (2.0f, 18.0f, fullW / (float) count);
        const int   shown = juce::jmin (count, (int) (fullW / barW));

        for (int c = 0; c < shown; ++c)
        {
            const float db = juce::Decibels::gainToDecibels (peak (c), -60.0f);
            const float norm = juce::jlimit (0.0f, 1.0f, (db + 60.0f) / 60.0f);
            auto r = juce::Rectangle<float> ((float) bank.getX() + (float) c * barW + 1.0f,
                                             (float) bank.getBottom() - norm * (float) bank.getHeight(),
                                             barW - 2.0f, norm * (float) bank.getHeight());
            g.setColour (norm > 0.9f ? juce::Colours::orangered
                       : norm > 0.75f ? juce::Colours::yellow : juce::Colours::limegreen);
            g.fillRect (r);
        }
        if (shown < count)
        {
            g.setColour (col.textSecondary);
            g.setFont (juce::FontOptions (11.0f));
            g.drawText ("+" + juce::String (count - shown), bank.removeFromRight (40),
                        juce::Justification::centredRight);
        }
    }

    juce::Label inputTitle, outputTitle, perfLabel;
    juce::Rectangle<int> meterArea, inputBank, outputBank;

    // Binaural monitor (WP15)
    juce::Label          binauralTitle, gainLabel, trackerLabel, cameraIndexLabel,
                         headingLabel, orientationReadout, statusLabel;
    juce::ToggleButton   enableButton;
    juce::Slider         gainSlider { juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight };
    RefreshableComboBox  trackerCombo;
    juce::TextButton     setZeroButton;
    juce::TextEditor     cameraIndexEditor;
    XoaBasicDial         yawDial, pitchDial, rollDial;
    juce::Label          yawValue, pitchValue, rollValue;
    std::vector<juce::String> trackerIds;   // combo item index -> stable source id
    juce::String         lastStatus;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MonitoringTab)
};

} // namespace xoa::ui
