/*
    XoaSofaSmokeTests.cpp — WP15 stage 0 gate: proves the spatcore-mysofa
    target links and spatcore's SOFA pipeline (SofaLoader -> HrirDatabase)
    works against XOA's bundled SADIE II KU100 set (assets/SOFA).

    Gated on XOA_TEST_SOFA_FIXTURE (defined by tests/CMakeLists.txt from the
    committed asset path) so a checkout without the asset still builds; the
    define is always present in-repo, this is belt-and-braces for forks.
*/

#include <juce_core/juce_core.h>

#include "spatcore/binaural/SofaLoader.h"

#include "XoaTestFramework.h"

void runXoaSofaSmokeTests()
{
#ifdef XOA_TEST_SOFA_FIXTURE
    const juce::File fixture (XOA_TEST_SOFA_FIXTURE);
    CHECK (fixture.existsAsFile());

    constexpr double fs = 48000.0;
    const auto result = spatcore::binaural::sofa::loadSofaFile (fixture, fs);

    if (result.database == nullptr)
    {
        std::fprintf (stderr, "FAIL: SOFA fixture load: %s\n",
                      result.status.toRawUTF8());
        ++failures;
        return;
    }

    const auto& db = *result.database;
    CHECK (db.sampleRate == fs);
    CHECK (db.hrirLength >= 64 && db.hrirLength <= 1024);

    // Baked grid shape and the time-alignment contract: for a hard-right
    // direction the right ear leads (relDelay 0) and the left ear carries a
    // plausible human ITD; the frontal direction is near-symmetric.
    using Db = spatcore::binaural::HrirDatabase;
    const int azRight = 90 / (int) Db::kAzStepDeg;      // az 90 deg = listener's right
    const int elMid   = (int) (Db::kNumEl / 2);          // el 0
    const float relL  = db.relDelaySec[(size_t) db.delayIndex (azRight, elMid, 0)];
    const float relR  = db.relDelaySec[(size_t) db.delayIndex (azRight, elMid, 1)];
    CHECK (relR == 0.0f);
    CHECK (relL > 0.0004f && relL < 0.001f);

    const float frontL = db.relDelaySec[(size_t) db.delayIndex (0, elMid, 0)];
    const float frontR = db.relDelaySec[(size_t) db.delayIndex (0, elMid, 1)];
    CHECK (std::abs (frontL - frontR) < 0.00015f);
#endif
}
