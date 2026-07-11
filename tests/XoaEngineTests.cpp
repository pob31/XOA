/*
    XoaEngineTests.cpp - WP6 C6b: the AudioEngine message-thread wiring.

    These run headless (no openAudioDevice, so no hardware is touched): they
    exercise the three controllers - RotationPublisher, BusParamsPublisher, and
    the decoder rebuild - by driving the store and inspecting the published
    snapshots. The RT chain those snapshots feed is already covered by the
    B2-B11 bus tests; here we verify the store -> snapshot plumbing.
*/

#include "XoaTestFramework.h"

#include "XoaConstants.h"
#include "Audio/AudioEngine.h"
#include "DSP/AmbiRtTypes.h"
#include "Parameters/XoaParameterIDs.h"
#include "Parameters/XoaValueTreeState.h"

#include <cmath>

namespace
{

void testInitialPublishBeforeEnable()
{
    xoa::XoaValueTreeState store;
    xoa::AudioEngine engine (store);

    // Every seam has a valid, non-sentinel snapshot the moment the engine is
    // constructed (publish-before-enable).
    CHECK (engine.rotationSource().acquire().epoch > 0);
    const auto bus = engine.busParamsSource().acquire();
    CHECK (bus.epoch > 0);
    const auto dec = engine.getDecoderBuilder().acquire();
    CHECK (dec.matrix != nullptr);
    CHECK (dec.numSpeakers == xoa::kDefaultSpeakers);   // the default 24-ring
    CHECK (dec.designOrder == 3);
}

void testRotationPublisher()
{
    xoa::XoaValueTreeState store;
    xoa::AudioEngine engine (store);

    store.setParameter (xoa::ids::rotationYaw, 30.0);
    store.setParameter (xoa::ids::rotationPitch, 20.0);
    store.setParameter (xoa::ids::rotationRoll, 10.0);

    const auto published = engine.rotationSource().acquire();
    const auto expected = xoa::rt::makeRotationState (30.0, 20.0, 10.0, published.epoch);

    int mismatches = 0;
    for (int i = 0; i < xoa::rot::kNumRotationCoeffs; ++i)
        if (published.coeffs[i] != expected.coeffs[i])
            ++mismatches;
    CHECK (mismatches == 0);
}

void testBusParamsPublisher()
{
    xoa::XoaValueTreeState store;
    xoa::AudioEngine engine (store);

    // Master gain cooks to linear on publish.
    store.setParameter (xoa::ids::masterGain, -6.0205999133);
    CHECK (std::abs (engine.busParamsSource().acquire().masterGainLinear - 0.5f) < 1.0e-5f);

    // Test-scene mode: order-10 SN3D identity gather, all 121 input channels.
    engine.setInputSource (xoa::AudioEngine::InputSource::testScene);
    const auto scene = engine.busParamsSource().acquire();
    CHECK (scene.numInputChannels == xoa::kNumSHChannels);
    CHECK (scene.srcChannel[0] == 0);
    CHECK (scene.srcChannel[120] == 120);
    CHECK (scene.gain[120] == 1.0f);
    CHECK (scene.contentOrder == xoa::kAmbisonicOrder);
}

void testDecoderRebuildOnParamChange()
{
    xoa::XoaValueTreeState store;
    xoa::AudioEngine engine (store);

    const auto before = engine.getDecoderBuilder().acquire();

    // Changing a decoder parameter marks the rebuild dirty; flush runs it now.
    store.setParameter (xoa::ids::decoderWeighting, 0);   // maxRe -> basic
    engine.flushDecoderRebuild();

    const auto after = engine.getDecoderBuilder().acquire();
    CHECK (after.epoch > before.epoch);          // a fresh matrix was published
    CHECK (after.matrix != before.matrix);       // double-buffer flip
    CHECK (after.numSpeakers == before.numSpeakers);

    // Moving a speaker also triggers a rebuild.
    const auto epochAfterWeighting = after.epoch;
    store.setParameter (xoa::ids::speakerPositionX, 1.5, 0);
    engine.flushDecoderRebuild();
    CHECK (engine.getDecoderBuilder().acquire().epoch > epochAfterWeighting);
}

// The production trigger path (not a direct flush): a decoder-param change must
// go through the Speakers/Decoder subtree listener -> markDecoderDirty, arming
// the debounce timer and deferring the rebuild. (A juce::Timer cannot fire
// without a pumped message loop, which a console test lacks, so we assert the
// timer is armed and the rebuild is deferred, then complete it - the timer and
// flush share the same rebuildDecoderNow.)
void testDecoderRebuildViaListenerAndTimer()
{
    xoa::XoaValueTreeState store;
    xoa::AudioEngine engine (store);

    const auto before = engine.getDecoderBuilder().acquire();
    CHECK (! engine.isDecoderRebuildPending());

    // A decoder-param change must arm the debounce (proves the subtree listener
    // is registered) but NOT rebuild synchronously.
    store.setParameter (xoa::ids::decoderWeighting, 0);   // maxRe -> basic
    CHECK (engine.isDecoderRebuildPending());
    CHECK (engine.getDecoderBuilder().acquire().epoch == before.epoch);

    // A speaker move flows through the same listener path.
    store.setParameter (xoa::ids::speakerPositionX, 1.5, 0);
    CHECK (engine.isDecoderRebuildPending());

    // Completing the pending rebuild (what timerCallback does) publishes and
    // disarms.
    engine.flushDecoderRebuild();
    CHECK (! engine.isDecoderRebuildPending());
    CHECK (engine.getDecoderBuilder().acquire().epoch > before.epoch);
}

void testDecoderStatusCallback()
{
    xoa::XoaValueTreeState store;
    xoa::AudioEngine engine (store);

    int calls = 0;
    int lastOrder = -1;
    engine.onDecoderRebuilt = [&] (const xoa::decoder::DesignResult& r)
    {
        ++calls;
        lastOrder = r.designOrder;
    };

    store.setParameter (xoa::ids::decoderType, 1);   // SAD -> mode-match
    engine.flushDecoderRebuild();
    CHECK (calls >= 1);
    CHECK (lastOrder == 3);   // 24 speakers clamp to order 3
}

} // namespace

//==============================================================================
void runXoaEngineTests()
{
    testInitialPublishBeforeEnable();
    testRotationPublisher();
    testBusParamsPublisher();
    testDecoderRebuildOnParamChange();
    testDecoderRebuildViaListenerAndTimer();
    testDecoderStatusCallback();
}
