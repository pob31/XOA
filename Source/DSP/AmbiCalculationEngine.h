#pragma once

#include <juce_events/juce_events.h>

#include <vector>

#include "spatcore/dsp/InputSpeedLimiter.h"
#include "spatcore/dsp/TrackingPositionFilter.h"
#include "spatcore/rt/RtSnapshot.h"

#include "XoaConstants.h"
#include "DSP/AmbiEncoder.h"
#include "DSP/AmbiRtTypes.h"
#include "Parameters/XoaValueTreeState.h"

//==============================================================================
// XOA - the control-side mono-encoder calculation engine (WP8, FR-5/FR-6).
//
// The Ambisonics analogue of WFS-DIY's WFSCalculationEngine: a message-thread
// juce::ValueTree::Listener that, on a 50 Hz tick, turns source geometry into
// the live [kMaxInputs x 121] encode matrix and the [kMaxInputs x 150] NFC
// coefficient pages, and publishes the small EncoderRtParams side-band.
//
// The two flat arrays are the live-matrix seam (XOA-PLAN 2.2a): app-owned,
// allocated once, rewritten in place by this engine (single writer), handed as
// const float* to AmbiBusAlgorithm::prepare and read live by the RT thread
// (benign staleness - a torn NFC page only mixes zeros from adjacent radii; the
// poles, fixed by r_ref, are rewritten only on the rare rig/SR change). The
// scalar side-band (source count, NFC mask, r_ref) travels through RtSnapshot.
//
// Position conditioning matches the WFS chain: source positions are eased by
// the InputSpeedLimiter each tick (per-input inputMaxSpeed); incoming tracking
// positions are 1-Euro filtered in submitTrackedPosition (the seam WP9's OSC
// receivers call) before being written back to the store.
//
// tick() is public so tests and the offline harness drive it deterministically;
// the timer merely calls it at 50 Hz in the running app.
//==============================================================================

namespace xoa
{

class AmbiCalculationEngine : private juce::ValueTree::Listener,
                              private juce::Timer
{
public:
    explicit AmbiCalculationEngine (XoaValueTreeState& store);
    ~AmbiCalculationEngine() override;

    //==========================================================================
    // RT-facing seams (stable pointers for AmbiBusAlgorithm::prepare).
    //==========================================================================
    const float* encodeMatrix() const noexcept { return liveMatrix.data(); }   // [kMaxInputs*121]
    const float* nfcCoeffs()    const noexcept { return nfcPages.data(); }      // [kMaxInputs*150]
    const spatcore::rt::RtSnapshot<rt::EncoderRtParams>& encoderSource() const noexcept
    {
        return snapshot;
    }

    //==========================================================================
    // Control (message thread).
    //==========================================================================
    /** Recompute every dirty row / NFC page, ease the speed limiter, and
        republish the side-band if it changed. Timer calls it at 50 Hz; tests
        and the harness call it directly. */
    void tick();

    /** Mark every source row + NFC page dirty (startup, or an all-source change
        like r_ref / sample rate). */
    void forceRecompute();

    /** Active device sample rate (NFC filters are designed at runtime, FR-4). */
    void setSampleRate (double sampleRate);
    double getSampleRate() const noexcept { return currentSampleRate; }

    /** Rig mean radius (m) - the NFC pole reference and the distance-gain
        reference. Set by the owner from the Speakers layout. */
    void setReferenceRadius (double radiusMeters);
    double getReferenceRadius() const noexcept { return referenceRadius; }

    /** Feed one tracking position (message thread): 1-Euro filter it, then write
        the smoothed result back to the store. WP9's OSC receivers call this;
        WP8 tests call it directly. */
    void submitTrackedPosition (int inputIndex, int trackingId,
                                float x, float y, float z, float quality = 1.0f);

    void startTicking() { startTimerHz (50); }
    void stopTicking()  { stopTimer(); }

private:
    //==========================================================================
    void timerCallback() override { tick(); }

    void valueTreePropertyChanged (juce::ValueTree& node, const juce::Identifier& property) override;
    void valueTreeChildAdded   (juce::ValueTree&, juce::ValueTree&) override        { structureChanged(); }
    void valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree&, int) override   { structureChanged(); }
    void valueTreeChildOrderChanged (juce::ValueTree&, int, int) override           { structureChanged(); }
    void valueTreeParentChanged (juce::ValueTree&) override {}

    void registerListeners();
    void unregisterListeners();
    void structureChanged();
    int  inputIndexForNode (const juce::ValueTree& node) const;

    enc::SourceParams readSource (int i) const;   // gain/spread/mute + speed-limited position
    void publishParams();                          // rebuild EncoderRtParams; publish if changed

    XoaValueTreeState& store;
    juce::ValueTree inputsSection;                 // persistent handle (listener lifetime)

    std::vector<float> liveMatrix;                 // kMaxInputs * 121, row-major [src*121 + acn]
    std::vector<float> nfcPages;                   // kMaxInputs * 150, packed per source
    spatcore::rt::RtSnapshot<rt::EncoderRtParams> snapshot;

    spatcore::dsp::InputSpeedLimiter speedLimiter;
    TrackingPositionFilter trackingFilter;         // global-namespace class (forward-decl compat)

    std::vector<char> rowDirty;                    // kMaxInputs (char: no vector<bool> bitref)
    std::vector<char> nfcDirty;                    // kMaxInputs

    double currentSampleRate = 48000.0;
    double referenceRadius   = defaults::kDefaultRigRadius;   // 2.0 m
    bool listenersRegistered = false;
    juce::uint32 epoch = 0;

    // last-published side-band, for change detection.
    int lastNumSources = -1;
    juce::uint64 lastNfcMask = 0;
    float lastReferenceRadius = -1.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AmbiCalculationEngine)
};

} // namespace xoa
