/*
  ==============================================================================

    XOA — tenth-order Ambisonics spatial audio processor.
    XoaCameraHeadTrackerSource — webcam head tracking through the
    wfs_headtrack plugin DLL (WP15, D53).

    Adapted from WFS-DIY's Source/DSP/CameraHeadTrackerSource.h. The plugin
    itself is app-agnostic (the versioned C ABI in
    spatcore/binaural/plugin/HeadTrackPluginApi.h), so the SAME
    wfs_headtrack.{dll,so,dylib} serves both apps unchanged — keep the name.
    Only two things differ from the WFS-DIY original: XOA has no logger, so
    failures land in a message-thread-readable status string (surfaced by the
    Monitoring tab) plus DBG; and the camera index arrives from the caller
    (the store's binauralCameraIndex) rather than an app settings file.

    The plugin reports RAW attitude from its inference thread. That thread is
    the ONE producer this source publishes from (RtSnapshot contract), and the
    publish path owns the two refinements the plugin deliberately doesn't:

     - Zero calibration: setZero() (message thread) flags a request; the next
       pose stores R_zeroInv and every report is pre-multiplied by it (matrix
       composition via spatcore::binaural::headframe — never per-angle
       subtraction, rotations don't commute).
     - 1-Euro filtering per angle (radian-scaled: minCutoff 1.5 Hz, beta 3.0),
       yaw unwrap-aware, so landmark jitter dies at rest while fast head turns
       pass essentially unfiltered.

    Staleness: getOrientation() reports valid=false when no pose arrived for
    300 ms (camera unplugged, plugin died) — the monitor then falls back to
    the manual orientation parameters. "Face briefly lost" arrives as
    faceDetected=0 packets and is handled the same way, but distinguishes a
    live tracker from a dead one.

    This file is part of XOA, released under the GNU General Public License
    v3.0. See LICENSE for details.

  ==============================================================================
*/

#pragma once

#include <juce_core/juce_core.h>

#include "spatcore/binaural/HeadOrientationSource.h"
#include "spatcore/binaural/HeadFrame.h"
#include "spatcore/binaural/plugin/HeadTrackPluginApi.h"
#include "spatcore/gpu/PlatformDynLib.h"
#include "spatcore/dsp/OneEuroFilter.h"

#include <atomic>
#include <cstring>
#include <functional>

namespace xoa
{

class XoaCameraHeadTrackerSource : public spatcore::binaural::SnapshotHeadOrientationSource
{
public:
    /** `cameraIndexProvider` is read on the message thread at start(); it
        lets the store own the setting without this class knowing the store. */
    explicit XoaCameraHeadTrackerSource (std::function<int()> cameraIndexProvider = {})
        : cameraIndex (std::move (cameraIndexProvider))
    {
    }

    ~XoaCameraHeadTrackerSource() override
    {
        stop();
        // The library handle is deliberately never closed (vtable-free C ABI,
        // but the plugin owns running threads' code) — same policy as the GPU
        // plugin loader.
    }

    juce::String getSourceId() const override    { return "camera:0"; }
    juce::String getDisplayName() const override { return "Webcam"; }

    bool isConnected() const override
    {
        return handle != nullptr
            && juce::Time::getMillisecondCounter() - lastPoseMs.load (std::memory_order_acquire) < kStaleMs;
    }

    spatcore::binaural::HeadOrientation getOrientation() const noexcept override
    {
        auto o = SnapshotHeadOrientationSource::getOrientation();
        if (juce::Time::getMillisecondCounter() - lastPoseMs.load (std::memory_order_acquire) > kStaleMs)
            o.valid = false;   // covers plugin death — no thread would ever publish invalid
        return o;
    }

    void setZero() override
    {
        zeroRequested.store (true, std::memory_order_release);
    }

    /** Last load/start failure or success message. Message thread only —
        XOA has no logger, so the UI is the reporting channel. */
    juce::String getStatusMessage() const { return status; }

    /** Message thread. Loads the plugin (first time), opens the camera and
        starts the pose stream. Returns false with getStatusMessage() set. */
    bool start()
    {
        if (handle != nullptr)
            return true;   // already running

        if (! loadPlugin())
            return false;

        WfsHeadtrackConfig config {};
        config.structSize = sizeof (config);
        config.cameraIndex = cameraIndex ? cameraIndex() : 0;

        handle = api.create (&config);
        if (handle == nullptr)
            return report ("Head tracker: create failed — " + juce::String (api.lastError (nullptr)));

        resetPublishState();

        if (api.start (handle, &poseTrampoline, this) == 0)
        {
            const auto message = "Head tracker: could not start webcam tracking — "
                               + juce::String (api.lastError (handle))
                               + " (camera index " + juce::String (config.cameraIndex)
                               + "; change it in the Monitoring tab)";
            api.destroy (handle);
            handle = nullptr;
            return report (message);
        }

        status = "Head tracker: webcam tracking started (" + juce::String (api.name())
               + ", camera index " + juce::String (config.cameraIndex) + ")";
        DBG (status);
        return true;
    }

    /** Message thread. Stops the stream and releases the camera. After this
        returns no callback is in flight (the plugin joins its worker). */
    void stop()
    {
        if (handle == nullptr)
            return;

        api.stop (handle);
        api.destroy (handle);
        handle = nullptr;

        // Publish one last invalid orientation so the monitor doesn't hold
        // the final pose while manual orientation takes over.
        publishOrientation ({});
        lastPoseMs.store (0, std::memory_order_release);
    }

private:
    //==========================================================================
    // Plugin loading (once per process; PlatformDynLib policy: exe dir first,
    // then the loader's default search path; handles never closed)

    struct Api
    {
        int32_t     (*abiVersion) (void) = nullptr;
        const char* (*name) (void) = nullptr;
        void*       (*create) (const WfsHeadtrackConfig*) = nullptr;
        int32_t     (*start) (void*, WfsHeadtrackPoseCb, void*) = nullptr;
        void        (*stop) (void*) = nullptr;
        void        (*destroy) (void*) = nullptr;
        const char* (*lastError) (void*) = nullptr;

        bool complete() const
        {
            return abiVersion && name && create && start && stop && destroy && lastError;
        }
    };

    bool report (const juce::String& message)
    {
        status = message;
        DBG (message);
        return false;
    }

    bool loadPlugin()
    {
        if (api.complete())
            return true;

        namespace dyn = spatcore::gpu::wfsdyn;
        const std::string so = dyn::pluginName ("headtrack");

        dyn::LibHandle lib = nullptr;
        if (const std::string beside = dyn::exeDir(); ! beside.empty())
            lib = dyn::openLib ((beside + "/" + so).c_str());
        if (lib == nullptr)
            lib = dyn::openLib (so.c_str());

        if (lib == nullptr)
            return report ("Head tracker: " + juce::String (so)
                           + " not found next to the application — webcam tracking"
                             " unavailable (build it with tools/headtrack/"
                             "build-headtrack-plugin)");

        Api fresh;
        fresh.abiVersion = (int32_t (*)(void))                             dyn::sym (lib, "wfs_headtrack_abi_version");
        fresh.name       = (const char* (*)(void))                         dyn::sym (lib, "wfs_headtrack_name");
        fresh.create     = (void* (*)(const WfsHeadtrackConfig*))          dyn::sym (lib, "wfs_headtrack_create");
        fresh.start      = (int32_t (*)(void*, WfsHeadtrackPoseCb, void*)) dyn::sym (lib, "wfs_headtrack_start");
        fresh.stop       = (void (*)(void*))                               dyn::sym (lib, "wfs_headtrack_stop");
        fresh.destroy    = (void (*)(void*))                               dyn::sym (lib, "wfs_headtrack_destroy");
        fresh.lastError  = (const char* (*)(void*))                        dyn::sym (lib, "wfs_headtrack_last_error");

        if (! fresh.complete())
            return report ("Head tracker: " + juce::String (so)
                           + " is missing exports — rebuild the plugin");

        // ABI check FIRST (before any other plugin call is trusted).
        if (const auto abi = fresh.abiVersion(); abi != WFS_HEADTRACK_ABI)
            return report ("Head tracker: " + juce::String (so) + " ABI " + juce::String (abi)
                           + " != expected " + juce::String (WFS_HEADTRACK_ABI)
                           + " — rebuild the plugin against this app version");

        api = fresh;
        return true;
    }

    //==========================================================================
    // Pose path (plugin inference thread — the one producer)

    static void poseTrampoline (const WfsHeadtrackPose* pose, void* user)
    {
        static_cast<XoaCameraHeadTrackerSource*> (user)->handlePose (*pose);
    }

    void handlePose (const WfsHeadtrackPose& pose)
    {
        lastPoseMs.store (juce::Time::getMillisecondCounter(), std::memory_order_release);

        if (pose.structSize < sizeof (WfsHeadtrackPose) || pose.faceDetected == 0)
        {
            publishOrientation ({});   // alive but nothing tracked -> manual fallback
            return;
        }

        namespace hf = spatcore::binaural::headframe;

        float rRaw[9];
        hf::yawPitchRollToMatrix (pose.yawRad, pose.pitchRad, pose.rollRad, rRaw);

        if (zeroRequested.exchange (false, std::memory_order_acq_rel))
        {
            hf::transpose (rRaw, rZeroInv);
            hasZero = true;
            filterYaw.reset();
            filterPitch.reset();
            filterRoll.reset();
            prevUnwrappedYaw = 0.0f;
            hasPrevYaw = false;
        }

        float corrected[9];
        if (hasZero)
            hf::multiply (rZeroInv, rRaw, corrected);
        else
            std::memcpy (corrected, rRaw, sizeof (corrected));

        float yaw, pitch, roll;
        hf::matrixToYawPitchRoll (corrected, yaw, pitch, roll);

        // Unwrap yaw against the previous sample so the 1-Euro filter never
        // sees the +/-pi seam as a full-circle jump.
        if (hasPrevYaw)
        {
            constexpr float twoPi = juce::MathConstants<float>::twoPi;
            float delta = yaw - std::fmod (prevUnwrappedYaw, twoPi);
            if (delta >  juce::MathConstants<float>::pi) delta -= twoPi;
            if (delta < -juce::MathConstants<float>::pi) delta += twoPi;
            yaw = prevUnwrappedYaw + delta;
        }
        prevUnwrappedYaw = yaw;
        hasPrevYaw = true;

        const double now = juce::Time::getMillisecondCounterHiRes() * 0.001;

        spatcore::binaural::HeadOrientation out;
        out.yawRad   = wrapPi (filterYaw.filter (yaw, now, kMinCutoffHz, kBeta, kDerivCutoffHz));
        out.pitchRad = filterPitch.filter (pitch, now, kMinCutoffHz, kBeta, kDerivCutoffHz);
        out.rollRad  = filterRoll.filter  (roll,  now, kMinCutoffHz, kBeta, kDerivCutoffHz);
        out.valid    = true;
        publishOrientation (out);
    }

    void resetPublishState()
    {
        hasZero = false;
        hasPrevYaw = false;
        prevUnwrappedYaw = 0.0f;
        filterYaw.reset();
        filterPitch.reset();
        filterRoll.reset();
        zeroRequested.store (false, std::memory_order_release);
        lastPoseMs.store (0, std::memory_order_release);
        publishOrientation ({});
    }

    static float wrapPi (float a) noexcept
    {
        constexpr float pi = juce::MathConstants<float>::pi;
        constexpr float twoPi = juce::MathConstants<float>::twoPi;
        while (a >  pi) a -= twoPi;
        while (a < -pi) a += twoPi;
        return a;
    }

    //==========================================================================

    // 1-Euro tuning, radian-scaled (beta multiplies rad/s — head turns reach
    // a few rad/s, so 3.0 fully opens the cutoff during real motion while
    // 1.5 Hz at rest kills sub-degree landmark jitter).
    static constexpr float kMinCutoffHz   = 1.5f;
    static constexpr float kBeta          = 3.0f;
    static constexpr float kDerivCutoffHz = 1.0f;
    static constexpr juce::uint32 kStaleMs = 300;

    std::function<int()> cameraIndex;
    juce::String status;

    Api api;
    void* handle = nullptr;

    std::atomic<bool> zeroRequested { false };
    std::atomic<juce::uint32> lastPoseMs { 0 };

    // Producer-thread-only state (the plugin guarantees one callback thread
    // per start/stop pair, and start/stop bracket its lifetime).
    float rZeroInv[9] = {};
    bool hasZero = false;
    float prevUnwrappedYaw = 0.0f;
    bool hasPrevYaw = false;
    spatcore::dsp::OneEuroFilter filterYaw, filterPitch, filterRoll;
};

} // namespace xoa
