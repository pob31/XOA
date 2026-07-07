/*
    XoaTests.cpp - minimal, dependency-free unit tests for XOA
    (no gtest; plain CHECK macro + exit code). Built as the `xoa-tests`
    console app and registered with ctest; run it -> exit 0 = pass.

    Golden reference data, when tests start needing it (WP3+), lives as
    committed JSON in tests/data/, produced by the generator scripts in
    tools/reference/ (see tools/reference/README.md for the convention).

    Coverage (WP1 seed - scaffolding proof, not a suite):
      1. XoaConstants        order/channel-count invariants
      2. spatcore smoke      include+link proof across the consumed libs:
                             dsp::WFSHelpers::safeClamp behavior (the same
                             symbol the app exercises) and an
                             rt::RtSnapshot<T> publish/acquire roundtrip
*/

#include <juce_core/juce_core.h>

#include "spatcore/rt/RtSnapshot.h"
#include "spatcore/dsp/NumericGuards.h"

#include "XoaConstants.h"

#include <cstdio>
#include <limits>

static int failures = 0;

#define CHECK(expr)                                                          \
    do                                                                       \
    {                                                                        \
        if (!(expr))                                                         \
        {                                                                    \
            std::fprintf (stderr, "FAIL %s:%d: %s\n",                        \
                          __FILE__, __LINE__, #expr);                        \
            ++failures;                                                      \
        }                                                                    \
    } while (false)

//==============================================================================
static void testXoaConstants()
{
    CHECK (xoa::kAmbisonicOrder == 10);
    CHECK (xoa::kNumSHChannels == (xoa::kAmbisonicOrder + 1) * (xoa::kAmbisonicOrder + 1));
    CHECK (xoa::kNumSHChannels == 121);
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
    try
    {
        testXoaConstants();
        testSpatcoreSmoke();
    }
    catch (const std::exception& e)
    {
        std::fprintf (stderr, "FAIL: unexpected exception: %s\n", e.what());
        ++failures;
    }

    if (failures == 0)
    {
        std::printf ("xoa-tests: all tests passed\n");
        return 0;
    }

    std::fprintf (stderr, "xoa-tests: %d check(s) FAILED\n", failures);
    return 1;
}
