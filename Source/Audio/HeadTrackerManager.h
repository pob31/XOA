/*
  ==============================================================================

    XOA — tenth-order Ambisonics spatial audio processor.
    HeadTrackerManager — owns the head-orientation sources for binaural
    monitoring and hands the active one to the audio thread (WP15, D53).

    Ported from WFS-DIY's Source/DSP/HeadTrackerManager.h, which has no
    app-specific dependencies; only the namespace and the camera source's
    type/construction differ.

    "Manual orientation" is the absence of an active source (id "manual"):
    the manual yaw/pitch/roll parameters already reach the RT stage through
    the monitor snapshot, so nothing needs producing for it. Trackers publish
    through SnapshotHeadOrientationSource from their own thread — the fast
    path that makes head motion feel immediate (D53: the audio thread reads
    getOrientation() fresh every block, bypassing the damped control path).

    Threading:
     - enumerate/select run on the message thread.
     - The audio thread reads the active source through a raw pointer set
       with an atomic (sources are only ever added, and destroyed after the
       device stops, so a stale read is at worst one block behind).

    This file is part of XOA, released under the GNU General Public License
    v3.0. See LICENSE for details.

  ==============================================================================
*/

#pragma once

#include <juce_core/juce_core.h>

#include "spatcore/binaural/HeadOrientationSource.h"

#include "XoaCameraHeadTrackerSource.h"

#include <atomic>
#include <functional>
#include <memory>
#include <vector>

namespace xoa
{

class HeadTrackerManager
{
public:
    struct SourceInfo
    {
        juce::String id;            // stable, persisted ("manual", "camera:0")
        juce::String displayName;
        bool connected = false;
    };

    /** The camera-index provider is forwarded to the webcam source; it reads
        the store's binauralCameraIndex without this class touching the store. */
    explicit HeadTrackerManager (std::function<int()> cameraIndexProvider = {})
    {
        // Webcam tracking via the wfs_headtrack plugin DLL. Always listed:
        // whether the plugin/camera actually works is only knowable by trying,
        // and a failed selection falls back to manual with a status message.
        auto camera = std::make_unique<XoaCameraHeadTrackerSource> (std::move (cameraIndexProvider));
        cameraSource = camera.get();
        sources.push_back (std::move (camera));
    }

    /** All selectable sources, "Manual orientation" first. Message thread. */
    std::vector<SourceInfo> enumerateSources() const
    {
        std::vector<SourceInfo> list;
        list.push_back ({ kManualId, "Manual orientation", true });
        for (const auto& s : sources)
            list.push_back ({ s->getSourceId(), s->getDisplayName(), s->isConnected() });
        return list;
    }

    /** Select by stable id. Unknown ids (device not present) fall back to
        manual — the persisted id is kept by the CALLER so the device can be
        re-selected when it reappears. Message thread. */
    void setActiveSource (const juce::String& sourceId)
    {
        spatcore::binaural::HeadOrientationSource* next = nullptr;
        for (const auto& s : sources)
            if (s->getSourceId() == sourceId)
                next = s.get();

        // Sources that own hardware only run while selected: start the camera
        // when it becomes active (failure -> manual fallback, reason in the
        // source's status message), release it when anything else takes over.
        if (next == cameraSource && cameraSource != nullptr)
        {
            if (! cameraSource->start())
                next = nullptr;
        }
        else if (cameraSource != nullptr)
        {
            cameraSource->stop();
        }

        activeSource.store (next, std::memory_order_release);
    }

    /** "Set zero while facing the stage" for whatever tracker is active.
        No-op in manual mode. Message thread. */
    void setZeroOnActiveSource()
    {
        if (auto* source = getActiveSource())
            source->setZero();
    }

    /** Audio thread: nullptr = manual orientation. */
    spatcore::binaural::HeadOrientationSource* getActiveSource() const noexcept
    {
        return activeSource.load (std::memory_order_acquire);
    }

    /** The slot itself, for the RT stage to load from once per block (D53).
        Stable for this object's lifetime. */
    const std::atomic<spatcore::binaural::HeadOrientationSource*>* getActiveSourceSlot() const noexcept
    {
        return &activeSource;
    }

    /** Last message from the webcam source (load/start failure or success).
        Message thread; empty when nothing has been attempted. */
    juce::String getStatusMessage() const
    {
        return cameraSource != nullptr ? cameraSource->getStatusMessage() : juce::String();
    }

    /** Stop any running tracker hardware. Called before teardown so no
        plugin callback is in flight while members die. Message thread. */
    void stopAll()
    {
        activeSource.store (nullptr, std::memory_order_release);
        if (cameraSource != nullptr)
            cameraSource->stop();
    }

    static constexpr const char* kManualId = "manual";

private:
    std::vector<std::unique_ptr<spatcore::binaural::HeadOrientationSource>> sources;
    std::atomic<spatcore::binaural::HeadOrientationSource*> activeSource { nullptr };
    XoaCameraHeadTrackerSource* cameraSource = nullptr;   // owned by `sources`

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (HeadTrackerManager)
};

} // namespace xoa
