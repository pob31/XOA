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
#include "DSP/AmbiDecoderDesigner.h"
#include "DSP/AmbiRtTypes.h"
#include "Parameters/XoaParameterIDs.h"
#include "Parameters/XoaValueTreeState.h"

#include <chrono>
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

//==============================================================================
// WP7 C4 — async decoder rebuild.
//==============================================================================

// requestAsyncRebuild returns immediately and defers the publish; the result
// arrives (and matches a fresh synchronous design) only after the worker runs.
void testAsyncRebuildNonBlocking()
{
    xoa::XoaValueTreeState store;
    xoa::AudioEngine engine (store);

    const auto before = engine.getDecoderBuilder().acquire();
    store.setParameter (xoa::ids::decoderWeighting, 0);   // maxRe -> basic

    engine.requestAsyncRebuild();
    // Deferred: nothing published yet, and the store stays writable during the
    // background design (a non-decoder write, so the reference below is unaffected).
    CHECK (engine.getDecoderBuilder().acquire().epoch == before.epoch);
    store.setParameter (xoa::ids::rotationYaw, 30.0);   // proves non-blocking

    engine.finishPendingAsyncRebuild();
    const auto after = engine.getDecoderBuilder().acquire();
    CHECK (after.epoch > before.epoch);
    CHECK (! engine.isDecoderRebuildInFlight());

    // The published matrix equals a fresh synchronous design of the same store.
    xoa::DecoderMatrixBuilder ref;
    ref.rebuild (store);
    const auto& a = engine.getDecoderBuilder().masterMatrix();
    const auto& b = ref.masterMatrix();
    CHECK (a.numSpeakers == b.numSpeakers && a.order == b.order);
    double worst = 0.0;
    for (size_t i = 0; i < a.d.size() && i < b.d.size(); ++i)
        worst = std::max (worst, std::abs (a.d[i] - b.d[i]));
    CHECK (worst < 1e-12);
}

// Two requests before the worker drains: exactly one publish, matching the
// LATEST store state (the stale generation is discarded).
void testAsyncRebuildStaleDiscard()
{
    xoa::XoaValueTreeState store;
    xoa::AudioEngine engine (store);
    const auto e0 = engine.getDecoderBuilder().acquire().epoch;

    store.setParameter (xoa::ids::decoderType, 0);   // SAD
    engine.requestAsyncRebuild();
    store.setParameter (xoa::ids::decoderType, 1);   // mode-match (the intended final state)
    engine.requestAsyncRebuild();

    engine.finishPendingAsyncRebuild();
    // A second drain must be a no-op (idempotent).
    engine.finishPendingAsyncRebuild();

    const auto after = engine.getDecoderBuilder().acquire();
    CHECK (after.epoch == e0 + 1);   // exactly one publish despite two requests

    xoa::DecoderMatrixBuilder ref;
    ref.rebuild (store);   // store == the latest (mode-match) state
    CHECK (std::abs (engine.getDecoderBuilder().masterMatrix().at (0, 0)
                     - ref.masterMatrix().at (0, 0)) < 1e-12);
}

// A synchronous flush during an in-flight async rebuild wins; the late worker
// result is discarded (its generation no longer matches).
void testAsyncRebuildFlushWins()
{
    xoa::XoaValueTreeState store;
    xoa::AudioEngine engine (store);
    const auto e0 = engine.getDecoderBuilder().acquire().epoch;

    store.setParameter (xoa::ids::decoderType, 0);
    engine.requestAsyncRebuild();      // gen N in flight
    store.setParameter (xoa::ids::decoderType, 1);
    engine.flushDecoderRebuild();      // gen N+1, synchronous publish now
    const auto afterFlush = engine.getDecoderBuilder().acquire().epoch;
    CHECK (afterFlush == e0 + 1);
    CHECK (! engine.isDecoderRebuildInFlight());

    // Let the stale worker finish; its result must be dropped (no extra publish).
    engine.finishPendingAsyncRebuild();
    CHECK (engine.getDecoderBuilder().acquire().epoch == afterFlush);
}

// PRD sec.7: a full-order AllRAD design on a 250-speaker rig completes in <= 2 s.
// Timed on the pure design() (what the worker runs); NDEBUG-gated so Debug CI
// stays green, but always printed.
void testDecoderRebuildTimingBudget()
{
    xoa::decoder::SpeakerLayout layout;
    layout.count = 250;
    const double golden = juce::MathConstants<double>::pi * (3.0 - std::sqrt (5.0));
    for (int i = 0; i < layout.count; ++i)
    {
        const double z = 1.0 - 2.0 * (i + 0.5) / layout.count;   // Fibonacci sphere
        const double r = std::sqrt (std::max (0.0, 1.0 - z * z));
        const double phi = golden * i;
        layout.positions[i] = { 3.0 * r * std::cos (phi), 3.0 * r * std::sin (phi), 3.0 * z };
    }

    xoa::decoder::DesignOptions o;
    o.type = xoa::decoder::Type::allRad;

    const auto t0 = std::chrono::steady_clock::now();
    const auto r = xoa::decoder::design (layout, o);
    const double ms = std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now() - t0).count();

    std::printf ("  [timing] AllRAD design, 250 speakers / order %d: %.0f ms\n", r.designOrder, ms);
    CHECK (r.matrix.numSpeakers == 250);
    CHECK (! r.allRadFellBack);   // a full sphere encloses the listener
   #ifdef NDEBUG
    CHECK (ms < 2000.0);          // the PRD budget (Release only)
   #endif
}

//==============================================================================
// WP7 C7 — per-speaker compensation wiring (D17 listener split).
//==============================================================================

// Publish-before-enable: the comp POD is valid at construction, and the default
// config (mode 0, no trim, all audible) is neutral.
void testCompInitialPublish()
{
    xoa::XoaValueTreeState store;
    xoa::AudioEngine engine (store);

    const auto p = engine.speakerCompSource().acquire();
    CHECK (p.epoch > 0);
    CHECK (p.numSpeakers == store.getNumSpeakers());
    for (int s = 0; s < p.numSpeakers; ++s)
    {
        CHECK (p.delayMs[s] == 0.0f);
        CHECK (std::abs (p.gainLinear[s] - 1.0f) < 1.0e-6f);
    }
}

// D17: a trim edit republishes comp immediately and does NOT arm a decoder rebuild.
void testCompTrimNoDecoderRebuild()
{
    xoa::XoaValueTreeState store;
    xoa::AudioEngine engine (store);
    const auto e0 = engine.speakerCompSource().acquire().epoch;
    CHECK (! engine.isDecoderRebuildPending());

    store.setParameter (xoa::ids::speakerGain, -6.0, 1);

    const auto p = engine.speakerCompSource().acquire();
    CHECK (p.epoch > e0);                                                        // comp republished
    CHECK (std::abs (p.gainLinear[1] - (float) std::pow (10.0, -6.0 / 20.0)) < 1.0e-5f);
    CHECK (! engine.isDecoderRebuildPending());                                  // decoder untouched
}

// Mute folds to gain 0, still no decoder rebuild.
void testCompMuteNoDecoderRebuild()
{
    xoa::XoaValueTreeState store;
    xoa::AudioEngine engine (store);

    store.setParameter (xoa::ids::speakerMute, true, 0);
    CHECK (engine.speakerCompSource().acquire().gainLinear[0] == 0.0f);
    CHECK (! engine.isDecoderRebuildPending());
}

// EQ edits refresh the RT biquads only — the EQ route touches NEITHER the
// decoder rebuild NOR the comp POD (which distinguishes it from the trim and
// position routes; the biquad update itself is covered by C7b's EQ test).
void testCompEqNoDecoderRebuild()
{
    xoa::XoaValueTreeState store;
    xoa::AudioEngine engine (store);
    const auto e0 = engine.speakerCompSource().acquire().epoch;

    store.setParameter (xoa::ids::speakerEqEnabled, true, 0);
    store.setEqBandParameter (0, 0, xoa::ids::eqShape, 3);

    CHECK (! engine.isDecoderRebuildPending());                       // not the decoder route
    CHECK (engine.speakerCompSource().acquire().epoch == e0);         // not the comp route
}

// Positions drive BOTH: comp republishes AND the decoder rebuild arms.
void testCompPositionTriggersBoth()
{
    xoa::XoaValueTreeState store;
    xoa::AudioEngine engine (store);
    const auto e0 = engine.speakerCompSource().acquire().epoch;

    store.setParameter (xoa::ids::speakerPositionX, 2.5, 0);

    CHECK (engine.speakerCompSource().acquire().epoch > e0);   // comp updated
    CHECK (engine.isDecoderRebuildPending());                  // decoder armed
}

// distanceCompMode (a Config param) republishes comp with distance-aligned
// delays, and does not rebuild the decoder.
void testCompDistanceModeListener()
{
    xoa::XoaValueTreeState store;
    xoa::AudioEngine engine (store);

    store.setNumSpeakers (3);
    for (int s = 0; s < 3; ++s)
    {
        store.setParameter (xoa::ids::speakerPositionX, (double) (s + 1), s);   // radii 1,2,3
        store.setParameter (xoa::ids::speakerPositionY, 0.0, s);
        store.setParameter (xoa::ids::speakerPositionZ, 0.0, s);
    }
    engine.flushDecoderRebuild();   // clear the rebuild armed by the edits above

    const auto e0 = engine.speakerCompSource().acquire().epoch;
    store.setParameter (xoa::ids::distanceCompMode, 1);   // align to the farthest (r=3)

    const auto p = engine.speakerCompSource().acquire();
    CHECK (p.epoch > e0);
    const double expected0 = (3.0 - 1.0) / xoa::kSpeedOfSound * 1000.0;
    CHECK (std::abs (p.delayMs[0] - expected0) < 1.0e-3);
    CHECK (p.delayMs[2] == 0.0f);                 // farthest -> no added delay
    CHECK (! engine.isDecoderRebuildPending());   // config-only, no rebuild
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
    testAsyncRebuildNonBlocking();
    testAsyncRebuildStaleDiscard();
    testAsyncRebuildFlushWins();
    testDecoderRebuildTimingBudget();
    testCompInitialPublish();
    testCompTrimNoDecoderRebuild();
    testCompMuteNoDecoderRebuild();
    testCompEqNoDecoderRebuild();
    testCompPositionTriggersBoth();
    testCompDistanceModeListener();
}
