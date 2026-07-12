#include "DSP/AmbiCalculationEngine.h"

#include <cmath>

#include "DSP/AmbiNFCFilter.h"
#include "Parameters/XoaParameterIDs.h"

namespace xoa
{

AmbiCalculationEngine::AmbiCalculationEngine (XoaValueTreeState& s)
    : store (s)
{
    liveMatrix.assign ((size_t) xoa::kMaxInputs * xoa::kNumSHChannels, 0.0f);
    nfcPages.assign  ((size_t) xoa::kMaxInputs * nfc::kCoeffsPerSource, 0.0);
    rowDirty.assign  ((size_t) xoa::kMaxInputs, 1);
    nfcDirty.assign  ((size_t) xoa::kMaxInputs, 1);
    wasMoving.assign ((size_t) xoa::kMaxInputs, 0);

    speedLimiter.resize (xoa::kMaxInputs);
    trackingFilter.resize (xoa::kMaxInputs);

    registerListeners();

    // Compose the initial rows/pages and publish the first snapshot before the
    // RT side can read one (publish-before-enable).
    tick();
}

AmbiCalculationEngine::~AmbiCalculationEngine()
{
    stopTimer();
    unregisterListeners();
}

//==============================================================================
void AmbiCalculationEngine::registerListeners()
{
    if (listenersRegistered)
        return;

    // Hold the section handle as a member: a ValueTree registers listeners by
    // instance address, so listening on a temporary would unregister at once.
    inputsSection = store.getInputsSection();
    inputsSection.addListener (this);

    // The master gate lives in Config; toggling it changes numSources with the
    // rows already maintained, so it can publish immediately.
    store.addParameterListener (ids::monoInputsEnabled, [this] (const juce::var&) { publishParams(); });

    listenersRegistered = true;
}

void AmbiCalculationEngine::unregisterListeners()
{
    if (! listenersRegistered)
        return;

    inputsSection.removeListener (this);
    store.removeParameterListeners (ids::monoInputsEnabled);
    listenersRegistered = false;
}

//==============================================================================
void AmbiCalculationEngine::forceRecompute()
{
    for (int i = 0; i < xoa::kMaxInputs; ++i)
    {
        rowDirty[(size_t) i] = 1;
        nfcDirty[(size_t) i] = 1;
    }
}

void AmbiCalculationEngine::structureChanged()
{
    forceRecompute();   // a count/order change may add rows that need composing
}

void AmbiCalculationEngine::setSampleRate (double sampleRate)
{
    if (sampleRate > 0.0 && sampleRate != currentSampleRate)
    {
        currentSampleRate = sampleRate;
        for (int i = 0; i < xoa::kMaxInputs; ++i)   // NFC coeffs are SR-dependent
            nfcDirty[(size_t) i] = 1;
    }
}

void AmbiCalculationEngine::setReferenceRadius (double radiusMeters)
{
    const double r = juce::jmax (1.0e-3, radiusMeters);
    if (r != referenceRadius)
    {
        referenceRadius = r;
        forceRecompute();   // distance gain (rows) and NFC poles both depend on r_ref
    }
}

//==============================================================================
int AmbiCalculationEngine::inputIndexForNode (const juce::ValueTree& node) const
{
    for (juce::ValueTree n = node; n.isValid(); n = n.getParent())
        if (n.getParent() == inputsSection)
            return inputsSection.indexOf (n);
    return -1;
}

void AmbiCalculationEngine::valueTreePropertyChanged (juce::ValueTree& node,
                                                      const juce::Identifier& property)
{
    const int i = inputIndexForNode (node);
    if (i < 0 || i >= xoa::kMaxInputs)
        return;

    rowDirty[(size_t) i] = 1;

    // Source radius feeds the NFC design; NFC enable changes the mask.
    if (property == ids::inputPositionX || property == ids::inputPositionY
        || property == ids::inputPositionZ || property == ids::inputNfcEnabled)
        nfcDirty[(size_t) i] = 1;
}

//==============================================================================
enc::SourceParams AmbiCalculationEngine::readSource (int i) const
{
    enc::SourceParams sp;
    float px = 0.0f, py = 0.0f, pz = 0.0f;
    speedLimiter.getPosition (i, px, py, pz);      // speed-limited position
    sp.x = px; sp.y = py; sp.z = pz;
    sp.gainDb    = store.getFloatParameter (ids::inputGain, i);
    sp.spreadDeg = store.getFloatParameter (ids::inputSpread, i);
    sp.mute      = static_cast<bool> (store.getParameter (ids::inputMute, i));
    return sp;
}

void AmbiCalculationEngine::tick()
{
    const int numInputs = juce::jmin (store.getNumInputs(), xoa::kMaxInputs);

    // 1. Sync targets + speed settings, then ease every input one 50 Hz frame.
    for (int i = 0; i < numInputs; ++i)
    {
        const float sx = (float) store.getFloatParameter (ids::inputPositionX, i);
        const float sy = (float) store.getFloatParameter (ids::inputPositionY, i);
        const float sz = (float) store.getFloatParameter (ids::inputPositionZ, i);
        const double maxSpeed = store.getFloatParameter (ids::inputMaxSpeed, i);
        speedLimiter.setSpeedLimit (i, maxSpeed > 0.0, (float) maxSpeed);
        speedLimiter.setTargetPosition (i, sx, sy, sz);
    }
    speedLimiter.process (0.02f);
    for (int i = 0; i < numInputs; ++i)
    {
        const bool moving = speedLimiter.isInputMoving (i);
        // Recompute while moving AND on the moving->stopped edge: the limiter
        // snaps to the exact target on the settle tick (isInputMoving already
        // false), so without this the row would freeze one step short.
        if (moving || wasMoving[(size_t) i])
            rowDirty[(size_t) i] = nfcDirty[(size_t) i] = 1;
        wasMoving[(size_t) i] = moving ? 1 : 0;
    }

    // 2. Recompute dirty rows and NFC pages (compose BEFORE publish so a count
    // grow never exposes an uncomposed row through numSources).
    for (int i = 0; i < numInputs; ++i)
    {
        if (rowDirty[(size_t) i])
        {
            enc::composeRow (readSource (i), referenceRadius,
                             liveMatrix.data() + (size_t) i * xoa::kNumSHChannels);
            rowDirty[(size_t) i] = 0;
        }
        if (nfcDirty[(size_t) i])
        {
            float px = 0.0f, py = 0.0f, pz = 0.0f;
            speedLimiter.getPosition (i, px, py, pz);
            const double rSrc = std::sqrt ((double) px * px + (double) py * py + (double) pz * pz);
            nfc::designSourceSections (rSrc, referenceRadius, currentSampleRate,
                                       nfcPages.data() + (size_t) i * nfc::kCoeffsPerSource);
            nfcDirty[(size_t) i] = 0;
        }
    }

    // 3. Publish the scalar side-band if it changed.
    publishParams();
}

//==============================================================================
void AmbiCalculationEngine::publishParams()
{
    const int numInputs = juce::jmin (store.getNumInputs(), xoa::kMaxInputs);
    const bool enabled = static_cast<bool> (store.getParameter (ids::monoInputsEnabled));
    const int numSources = enabled ? numInputs : 0;

    juce::uint64 mask = 0;
    for (int i = 0; i < numInputs; ++i)
        if (static_cast<bool> (store.getParameter (ids::inputNfcEnabled, i)))
            mask |= (juce::uint64) 1 << i;

    const float rRef = (float) referenceRadius;
    if (numSources == lastNumSources && mask == lastNfcMask && rRef == lastReferenceRadius)
        return;

    rt::EncoderRtParams p;
    p.numSources      = numSources;
    p.nfcMask         = mask;
    p.referenceRadius = rRef;
    p.epoch           = ++epoch;
    snapshot.publish (p);

    lastNumSources      = numSources;
    lastNfcMask         = mask;
    lastReferenceRadius = rRef;
}

//==============================================================================
void AmbiCalculationEngine::submitTrackedPosition (int inputIndex, int trackingId,
                                                   float x, float y, float z, float quality)
{
    if (inputIndex < 0 || inputIndex >= xoa::kMaxInputs)
        return;

    const float smooth = store.getFloatParameter (ids::inputTrackingSmooth, inputIndex);
    float fx = x, fy = y, fz = z;

    // 1-Euro filter in place; returns false and leaves fx/fy/fz raw on a
    // rejected jump spike -> hold the previous stored position (no write).
    if (trackingFilter.filterPosition (inputIndex, trackingId, fx, fy, fz,
                                       true, true, true, smooth, quality))
    {
        store.setParameterWithoutUndo (ids::inputPositionX, (double) fx, inputIndex);
        store.setParameterWithoutUndo (ids::inputPositionY, (double) fy, inputIndex);
        store.setParameterWithoutUndo (ids::inputPositionZ, (double) fz, inputIndex);
    }
}

} // namespace xoa
