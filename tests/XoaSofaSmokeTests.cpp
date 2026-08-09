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

#include "DSP/AmbiBinauralDecoder.h"
#include "DSP/AmbiBinauralFilterBank.h"
#include "DSP/AmbiBinauralRenderer.h"
#include "DSP/AmbiBinauralRtTypes.h"
#include "XoaConstants.h"
#include "XoaTestFramework.h"

#include <atomic>
#include <cmath>

namespace
{
#ifdef XOA_TEST_SOFA_FIXTURE
/** End-to-end on the REAL bundled set: load -> design -> cook -> render, the
    whole chain the monitor runs at startup with no project SOFA chosen. The
    synthetic-fixture tests pin the math; this one proves the real file
    survives every stage. */
void testBundledSofaRendersBinaurally()
{
    constexpr double fs = 48000.0;
    constexpr int block = 128;

    const auto loaded = spatcore::binaural::sofa::loadSofaFile (
        juce::File (XOA_TEST_SOFA_FIXTURE), fs);
    if (loaded.database == nullptr)
        return;   // the load failure is already reported by the caller

    const auto design = xoa::binaural::designShFilters (*loaded.database);
    CHECK (design.isValid());
    CHECK (design.sampleRate == fs);
    if (! design.isValid())
        return;

    // A real HRTF bank must carry energy on the W channel of both ears.
    for (int ear = 0; ear < 2; ++ear)
    {
        double energy = 0.0;
        for (int n = 0; n < design.firLength; ++n)
        {
            const double v = design.fir (ear, 0)[n];
            energy += v * v;
            CHECK (std::isfinite (v));
        }
        CHECK (energy > 0.0);
    }

    xoa::AmbiBinauralFilterBank bank;
    CHECK (bank.cook (design, block));
    bank.publish();

    spatcore::rt::RtSnapshot<xoa::rt::MonitorRtParams> params;
    xoa::rt::MonitorRtParams p;
    p.enabled = true;
    p.monitorGainLinear = 1.0f;
    p.epoch = 1;
    params.publish (p);

    std::atomic<spatcore::binaural::HeadOrientationSource*> source { nullptr };
    xoa::AmbiBinauralRenderer renderer;
    renderer.prepare (fs, block, &bank, &params, &source);

    // Encode a frontal plane wave into the bus and render a few blocks.
    juce::AudioBuffer<float> bus (xoa::kNumSHChannels, block);
    bus.clear();
    for (int i = 0; i < block; ++i)
        bus.setSample (0, i, 0.25f * std::sin (0.05f * (float) i));

    double energy = 0.0;
    for (int b = 0; b < 4; ++b)
    {
        renderer.render (bus, block, 1.0f);
        CHECK (renderer.isActive());
        for (int i = 0; i < block; ++i)
        {
            const float l = renderer.getOutputLeft()[i];
            const float r = renderer.getOutputRight()[i];
            CHECK (std::isfinite (l) && std::isfinite (r));
            CHECK (std::abs (l) < 8.0f && std::abs (r) < 8.0f);   // no runaway
            energy += (double) l * l + (double) r * r;
        }
    }
    CHECK (energy > 0.0);
}
#endif
} // namespace

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

    testBundledSofaRendersBinaurally();
#endif
}
