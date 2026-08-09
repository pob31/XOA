/*
  ==============================================================================

    XOA — tenth-order Ambisonics spatial audio processor.
    XoaMonitoringEngine — the message-thread controller for binaural
    monitoring (WP15). Stage 1 covers the head-orientation half; the SOFA
    load/design worker and the filter-bank publish join it in stage 3.

    Responsibilities:
     - owns the HeadTrackerManager and therefore every orientation source;
     - follows the store's binauralHeadTracker id: selecting a device starts
       it, and a device that fails to start (or is simply absent) resolves to
       manual WITHOUT clearing the persisted id, so the selection survives
       until the hardware comes back;
     - exposes the active source as a raw pointer for the audio thread (D53:
       the RT stage reads getOrientation() fresh every block, bypassing the
       damped control path) plus the manual fallback attitude for when no
       source is active or the tracker has gone stale.

    Threading: everything here is message thread except getActiveSource(),
    which the audio thread calls (atomic load). Sources outlive the audio
    callback because stopMonitoring() runs before the engine tears down.

    This file is part of XOA, released under the GNU General Public License
    v3.0. See LICENSE for details.

  ==============================================================================
*/

#pragma once

#include <juce_core/juce_core.h>

#include "spatcore/binaural/HeadOrientationSource.h"

#include "Audio/HeadTrackerManager.h"
#include "DSP/AmbiHeadMapping.h"
#include "Parameters/XoaValueTreeState.h"

namespace xoa
{

class XoaMonitoringEngine
{
public:
    explicit XoaMonitoringEngine (XoaValueTreeState& storeToUse)
        : store (storeToUse),
          trackers ([this] { return store.getIntParameter (ids::binauralCameraIndex); })
    {
        applyTrackerSelection();
    }

    ~XoaMonitoringEngine() { stopMonitoring(); }

    /** Stop tracker hardware. Must run before anything the sources publish
        into goes away. Message thread. */
    void stopMonitoring()
    {
        trackers.stopAll();
        appliedTrackerId = {};
    }

    /** Re-read binauralHeadTracker and start/stop hardware to match. Call on
        any change to that parameter (and once at construction). A failed
        start leaves the ACTIVE source null (= manual) while the store keeps
        the requested id. Message thread. */
    void applyTrackerSelection()
    {
        const juce::String wanted = store.getStringParameter (ids::binauralHeadTracker);
        if (wanted == appliedTrackerId)
            return;

        appliedTrackerId = wanted;
        trackers.setActiveSource (wanted.isEmpty() ? juce::String (HeadTrackerManager::kManualId)
                                                   : wanted);
    }

    /** Restart the active tracker so a camera-index edit takes effect. */
    void reapplyCameraIndex()
    {
        const juce::String current = appliedTrackerId;
        if (current.isEmpty() || current == HeadTrackerManager::kManualId)
            return;

        trackers.setActiveSource (HeadTrackerManager::kManualId);
        trackers.setActiveSource (current);
    }

    void setZeroOnActiveSource() { trackers.setZeroOnActiveSource(); }

    std::vector<HeadTrackerManager::SourceInfo> enumerateSources() const
    {
        return trackers.enumerateSources();
    }

    juce::String getTrackerStatusMessage() const { return trackers.getStatusMessage(); }

    /** True when a tracker (not manual) is currently the active source — the
        condition the Set Zero button's visibility keys on. */
    bool isTrackerActive() const noexcept { return trackers.getActiveSource() != nullptr; }

    /** Audio thread (and the UI readout): nullptr = manual orientation. */
    spatcore::binaural::HeadOrientationSource* getActiveSource() const noexcept
    {
        return trackers.getActiveSource();
    }

    /** The manual attitude from the store, in the spatcore convention.
        Message thread — the RT path gets this pre-cooked in a snapshot. */
    spatcore::binaural::HeadOrientation getManualOrientation() const
    {
        return binaural::manualOrientation (store.getFloatParameter (ids::binauralManualYaw),
                                            store.getFloatParameter (ids::binauralManualPitch),
                                            store.getFloatParameter (ids::binauralManualRoll));
    }

    /** What the monitor is actually steering by right now: the tracker when it
        has a valid pose, else the manual parameters. The `tracked` flag lets
        the UI show which one is live. */
    spatcore::binaural::HeadOrientation getEffectiveOrientation (bool& tracked) const
    {
        if (auto* source = getActiveSource())
        {
            const auto o = source->getOrientation();
            if (o.valid)
            {
                tracked = true;
                return o;
            }
        }
        tracked = false;
        return getManualOrientation();
    }

private:
    XoaValueTreeState& store;
    HeadTrackerManager trackers;
    juce::String appliedTrackerId;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (XoaMonitoringEngine)
};

} // namespace xoa
