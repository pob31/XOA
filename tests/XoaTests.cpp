/*
    XoaTests.cpp - entry point for the xoa-tests console app.
    Dependency-free CHECK pattern (see XoaTestFramework.h); exit 0 = pass.

    Headless JUCE note: the parameter store's UndoManager is a
    ChangeBroadcaster whose async updates assert without a MessageManager in
    Debug builds, so main() creates the instance up front (no run loop is
    needed — the creating thread counts as the message thread) and tears it
    down cleanly before returning.

    Coverage:
      1. XoaConstants        order/channel-count invariants
      2. spatcore smoke      include+link proof across the consumed libs
      3. WP2 suite           schema tables, coordinates, parameter store,
                             project file I/O, WFS-DIY layout import
                             (XoaParameterTests.cpp)
      4. WP3 suite           SH evaluation, conventions (N3D/SN3D, FuMa),
                             order weights, FR-7 order adaptation
                             (XoaShTests.cpp)
      5. WP4 suite           SO(3) rotation (Ivanic-Ruedenberg + 1998 erratum),
                             mirror planes (XoaRotationTests.cpp)
      6. WP5 suite           linear algebra (SVD/pinv), decoder design
                             (SAD + mode-matching), rV/rE, matrix builder,
                             WFS import completion (XoaDecoderTests.cpp)
      7. WP6 suite           file-I/O spike: 121/128-ch WAV + 8-ch FLAC
                             round-trips, garbage rejection
                             (XoaFilePlayerTests.cpp); RT snapshot composers
                             (rotation state, gather table) (XoaBusTests.cpp);
                             synthetic test-scene generator: determinism,
                             block-partition independence, encoding
                             (XoaSceneTests.cpp)
*/

#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>

#include "spatcore/rt/RtSnapshot.h"
#include "spatcore/dsp/NumericGuards.h"

#include "XoaConstants.h"
#include "XoaTestFramework.h"

#include <limits>

void runXoaParameterTests();
void runXoaShTests();
void runXoaRotationTests();
void runXoaDecoderTests();
void runXoaFilePlayerTests();
void runXoaBusTests();
void runXoaSceneTests();
void runXoaEngineTests();
void runXoaVbapTests();
void runXoaCompTests();
void runXoaTestSignalTests();
void runXoaNfcTests();
void runXoaCalcEngineTests();
void runXoaEncoderTests();
void runXoaOscRouterTests();
void runXoaOscManagerTests();
void runXoaLocalizationTests();
void runXoaUiDescriptorTests();
void runXoaLayoutGeneratorTests();

//==============================================================================
static void testXoaConstants()
{
    CHECK (xoa::kAmbisonicOrder == 10);
    CHECK (xoa::kNumSHChannels == (xoa::kAmbisonicOrder + 1) * (xoa::kAmbisonicOrder + 1));
    CHECK (xoa::kNumSHChannels == 121);
    CHECK (xoa::kDefaultInputs <= xoa::kMaxInputs);
    CHECK (xoa::kDefaultSpeakers <= xoa::kMaxSpeakers);
}

//==============================================================================
static void testSpatcoreSmoke()
{
    using spatcore::dsp::WFSHelpers::safeClamp;

    // Normal jlimit behavior with finite, ordered bounds
    CHECK (safeClamp (0.0f, 1.0f, 0.5f) == 0.5f);
    CHECK (safeClamp (0.0f, 1.0f, 2.0f) == 1.0f);
    CHECK (safeClamp (0.0f, 1.0f, -3.0f) == 0.0f);

    // Tolerant paths: non-finite or inverted bounds pass the value through
    const float nan = std::numeric_limits<float>::quiet_NaN();
    CHECK (safeClamp (nan, 1.0f, 42.0f) == 42.0f);
    CHECK (safeClamp (0.0f, nan, 42.0f) == 42.0f);
    CHECK (safeClamp (1.0f, 0.0f, 7.0f) == 7.0f);

    // RtSnapshot publish/acquire roundtrip; instantiating it is the
    // compile-proof of the trivially-copyable static_assert
    struct Pod
    {
        float yaw;
        int order;
    };

    spatcore::rt::RtSnapshot<Pod> snapshot;
    snapshot.publish ({ 1.5f, xoa::kAmbisonicOrder });

    const Pod out = snapshot.acquire();
    CHECK (out.yaw == 1.5f);
    CHECK (out.order == 10);
}

//==============================================================================
int main()
{
    juce::MessageManager::getInstance();

    try
    {
        testXoaConstants();
        testSpatcoreSmoke();
        runXoaParameterTests();
        runXoaShTests();
        runXoaRotationTests();
        runXoaDecoderTests();
        runXoaFilePlayerTests();
        runXoaBusTests();
        runXoaSceneTests();
        runXoaEngineTests();
        runXoaVbapTests();
        runXoaCompTests();
        runXoaTestSignalTests();
        runXoaNfcTests();
        runXoaCalcEngineTests();
        runXoaEncoderTests();
        runXoaOscRouterTests();
        runXoaOscManagerTests();
        runXoaLocalizationTests();
        runXoaUiDescriptorTests();
        runXoaLayoutGeneratorTests();
    }
    catch (const std::exception& e)
    {
        std::fprintf (stderr, "FAIL: unexpected exception: %s\n", e.what());
        ++failures;
    }

    juce::DeletedAtShutdown::deleteAll();
    juce::MessageManager::deleteInstance();

    if (failures == 0)
    {
        std::printf ("xoa-tests: all tests passed\n");
        return 0;
    }

    std::fprintf (stderr, "xoa-tests: %d check(s) FAILED\n", failures);
    return 1;
}
