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
#include <juce_events/juce_events.h>

#include "spatcore/binaural/HeadOrientationSource.h"
#include "spatcore/rt/RtSnapshot.h"

#include "Audio/BinauralDesignWorker.h"
#include "Audio/HeadTrackerManager.h"
#include "DSP/AmbiBinauralFilterBank.h"
#include "DSP/AmbiBinauralRtTypes.h"
#include "DSP/AmbiHeadMapping.h"
#include "Parameters/XoaFileManager.h"
#include "Parameters/XoaValueTreeState.h"

namespace xoa
{

class XoaMonitoringEngine : private juce::AsyncUpdater
{
public:
    XoaMonitoringEngine (XoaValueTreeState& storeToUse,
                         spatcore::rt::RtSnapshot<rt::MonitorRtParams>& snapshotToPublish)
        : store (storeToUse),
          monitorSnapshot (snapshotToPublish),
          trackers ([this] { return store.getIntParameter (ids::binauralCameraIndex); })
    {
        applyTrackerSelection();
        publishParams();
    }

    ~XoaMonitoringEngine() override
    {
        designWorker.stop();      // join BEFORE cancelling async updates, so no
        cancelPendingUpdate();    // triggerAsyncUpdate can fire into a dead object
        stopMonitoring();
    }

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

    //==========================================================================
    // Filter bank + SOFA lifecycle
    //==========================================================================

    const AmbiBinauralFilterBank& getFilterBank() const noexcept { return filterBank; }

    const std::atomic<spatcore::binaural::HeadOrientationSource*>* getActiveSourceSlot() const noexcept
    {
        return trackers.getActiveSourceSlot();
    }

    /** Compose and publish MonitorRtParams. Call on any change to the enable
        gate, the gain, or the manual attitude. Message thread, ONE writer. */
    void publishParams()
    {
        rt::MonitorRtParams p;
        p.enabled = (bool) store.getParameter (ids::binauralEnabled);
        p.monitorGainLinear =
            juce::Decibels::decibelsToGain (store.getFloatParameter (ids::binauralGain), -60.0f);

        const auto manual = getManualOrientation();
        p.manualYawRad = manual.yawRad;
        p.manualPitchRad = manual.pitchRad;
        p.manualRollRad = manual.rollRad;
        p.epoch = ++paramsEpoch;
        monitorSnapshot.publish (p);
    }

    /** The device opened or changed shape. Re-cooks the existing design for
        the new block size, and redesigns from scratch when the sample rate
        moved (the HRIRs are resampled at load). Message thread. */
    void deviceChanged (double sampleRate, int blockSize)
    {
        const bool rateMoved = std::abs (sampleRate - deviceSampleRate) > 1.0e-6;
        deviceSampleRate = sampleRate;
        deviceBlockSize = blockSize;

        if (rateMoved || ! currentDesign.isValid())
        {
            requestDesign();
            return;
        }

        if (filterBank.cook (currentDesign, blockSize))
            filterBank.publish();
        else
            filterBank.publishEmpty();
    }

    /** Kick a background load+design of the selected HRTF set. Safe to call
        repeatedly: the worker is latest-wins and a repeat request for the
        SAME key is dropped, so a failed load does not retry forever. */
    void requestDesign (bool force = false)
    {
        if (deviceSampleRate <= 0.0 || deviceBlockSize <= 0)
            return;   // nothing to design for yet; prepareToPlay will call back

        const auto file = resolveSofaFile();
        const juce::String key = file.getFullPathName() + "|" + juce::String (deviceSampleRate)
                               + "|" + juce::String (store.getIntParameter (ids::binauralDecoderMode));
        if (! force && key == requestedKey)
            return;
        requestedKey = key;

        if (! file.existsAsFile())
        {
            statusMessage = "HRTF set not found: " + file.getFullPathName();
            currentDesign = {};
            filterBank.publishEmpty();
            return;
        }

        BinauralDesignWorker::Job job;
        job.sofaFile = file;
        job.sampleRate = deviceSampleRate;
        job.options.mode = static_cast<binaural::DecoderMode> (
            juce::jlimit (0, 1, store.getIntParameter (ids::binauralDecoderMode)));
        job.generation = ++designGeneration;
        designWorker.submit (job);
    }

    /** Where the selected HRTF set lives: a bare filename resolves against
        <project>/sofa, an empty setting means the bundled default staged
        next to the binary. */
    juce::File resolveSofaFile() const
    {
        const auto name = store.getStringParameter (ids::binauralSofaFile);
        if (name.isNotEmpty() && projectSofaFolder.isDirectory())
            return projectSofaFolder.getChildFile (name);
        if (name.isNotEmpty())
            return juce::File();
        return builtInSofaFile();
    }

    /** The bundled SADIE II KU100 set, staged in the app's resource dir (and
        read from the source tree in a dev build).

        The staged location is platform-specific and must match the POST_BUILD
        staging in CMakeLists.txt: Contents/Resources inside the bundle on
        macOS, beside the binary elsewhere. Data files may NOT live in
        Contents/MacOS on macOS - codesign treats that directory as code and
        `--verify --strict` rejects each unsigned file there, which fails
        notarization. LocalizationManager resolves its lang dir the same way. */
    static juce::File builtInSofaFile()
    {
        const auto exeDir = juce::File::getSpecialLocation (juce::File::currentExecutableFile)
                                .getParentDirectory();
       #if JUCE_MAC
        const auto resourceDir = juce::File::getSpecialLocation (juce::File::currentApplicationFile)
                                     .getChildFile ("Contents/Resources");
       #else
        const auto resourceDir = exeDir.getChildFile ("Resources");
       #endif
        const auto staged = resourceDir.getChildFile ("SOFA").getChildFile (kBuiltInSofaName);
        if (staged.existsAsFile())
            return staged;

        // Dev fallback: walk up to the repo root's assets/SOFA.
        for (auto dir = exeDir; dir.exists(); dir = dir.getParentDirectory())
        {
            const auto candidate = dir.getChildFile ("assets").getChildFile ("SOFA")
                                      .getChildFile (kBuiltInSofaName);
            if (candidate.existsAsFile())
                return candidate;
            if (dir.getParentDirectory() == dir)
                break;
        }
        return staged;   // non-existent: the caller reports it
    }

    /** Point HRTF-set resolution at a project folder. Message thread. */
    void setProjectSofaFolder (const juce::File& folder)
    {
        projectSofaFolder = folder;
        requestDesign();
    }

    /** Human-readable state of the HRTF set (load status / failure). */
    juce::String getSofaStatusMessage() const { return statusMessage; }

    bool isDesignInFlight() const noexcept { return designWorker.isBusy(); }

    /** Headless test seam: block until the worker is idle and adopt whatever
        it produced (no message loop involved). */
    void flushDesignForTesting()
    {
        designWorker.waitUntilIdle();
        adoptCompletedDesign();
    }

private:
    void handleAsyncUpdate() override { adoptCompletedDesign(); }

    /** Message thread: take the worker's result, cook it for the current
        block size and publish. Stale generations are discarded. */
    void adoptCompletedDesign()
    {
        BinauralDesignWorker::Result result;
        while (designWorker.takeCompleted (result))
        {
            if (result.generation != designGeneration)
                continue;   // superseded by a newer request

            statusMessage = result.status;

            if (result.loadFailed || ! result.design.isValid())
            {
                currentDesign = {};
                filterBank.publishEmpty();
                continue;
            }

            currentDesign = std::move (result.design);
            if (deviceBlockSize > 0 && filterBank.cook (currentDesign, deviceBlockSize))
                filterBank.publish();
            else
                filterBank.publishEmpty();
        }
    }

    static constexpr const char* kBuiltInSofaName = "D1_48K_24bit_256tap_FIR_SOFA.sofa";

    XoaValueTreeState& store;
    spatcore::rt::RtSnapshot<rt::MonitorRtParams>& monitorSnapshot;
    HeadTrackerManager trackers;
    juce::String appliedTrackerId;

    AmbiBinauralFilterBank filterBank;
    BinauralDesignWorker designWorker { [this] { triggerAsyncUpdate(); } };
    binaural::BinauralDesignResult currentDesign;

    juce::File projectSofaFolder;
    juce::String requestedKey;
    juce::String statusMessage;
    double deviceSampleRate = 0.0;
    int deviceBlockSize = 0;
    juce::uint64 designGeneration = 0;
    juce::uint32 paramsEpoch = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (XoaMonitoringEngine)
};

} // namespace xoa
