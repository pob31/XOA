/*
    XoaParameterTests.cpp - WP2 tests: schema tables, coordinates, the
    parameter store, project file I/O, and the WFS-DIY layout import.
    Mapped to the WP2 exit criteria:
      (a) save/load round-trip bit-stable ............ T8 (+T10)
      (b) merge-backfill gains new defaults .......... T9
      (c) constraints + coordinate conversions ....... T1, T2, T3, T5
      (d) WFS-DIY speaker section parses ............. T11
*/

#include "XoaTestFramework.h"

#include "spatcore/control/osc/OscTransportTypes.h"
#include "spatcore/control/state/XmlPersistence.h"

#include "Helpers/XoaCoordinates.h"
#include "Parameters/XoaConstraints.h"
#include "Parameters/XoaFileManager.h"
#include "Parameters/XoaParameterDefaults.h"
#include "Parameters/XoaParameterIDs.h"
#include "Parameters/XoaValueTreeState.h"
#include "XoaConstants.h"

#include <cmath>
#include <cstring>
#include <limits>

using xoa::XoaFileManager;
using xoa::XoaValueTreeState;
using spatcore::control::state::XmlPersistence;
namespace ids = xoa::ids;

static bool bitEqualDouble (double a, double b) noexcept
{
    return std::memcmp (&a, &b, sizeof (double)) == 0;
}

static bool approx (double a, double b, double tolerance = 1.0e-12) noexcept
{
    return std::abs (a - b) <= tolerance;
}

//==============================================================================
// T1 — bounds-table integrity
//==============================================================================
static void testBoundsTableIntegrity()
{
    for (const auto& entry : xoa::constraints::allBounds())
    {
        const auto& b = entry.second;
        CHECK (b.min <= b.defaultValue && b.defaultValue <= b.max);
        CHECK (xoa::constraints::findBounds (entry.first) == &b);
    }

    // eqShape range is pinned to spatcore::dsp::OutputEQBiquadFilter's 8 shapes
    CHECK (xoa::constraints::findBounds (ids::eqShape)->max == 7.0);

    // strings and bools are unbounded by design
    CHECK (xoa::constraints::findBounds (ids::showName) == nullptr);
    CHECK (xoa::constraints::findBounds (ids::inputMute) == nullptr);

    // Completeness: every numeric parameter that MUST be range-checked has to
    // resolve through findBounds, or it would silently escape both validation
    // gates. Adding a numeric parameter without a bounds entry fails here.
    const juce::Identifier mustBeBounded[] = {
        ids::masterGain, ids::oscReceivePort, ids::oscSendPort,
        ids::inputCount, ids::speakerCount,
        ids::inputGain, ids::inputPositionX, ids::inputPositionY, ids::inputPositionZ,
        ids::inputCoordinateMode, ids::inputSpread,
        ids::speakerGain, ids::speakerDelay,
        ids::speakerPositionX, ids::speakerPositionY, ids::speakerPositionZ,
        ids::speakerCoordinateMode,
        ids::eqShape, ids::eqFrequency, ids::eqGain, ids::eqQ, ids::eqSlope,
        ids::decoderType, ids::decoderWeighting, ids::decoderCrossoverFrequency,
        ids::decoderNormalization
    };
    for (const auto& id : mustBeBounded)
        CHECK (xoa::constraints::findBounds (id) != nullptr);
}

//==============================================================================
// T2 — the two validation gates
//==============================================================================
static void testValidationGates()
{
    using xoa::constraints::clampToBounds;
    using xoa::constraints::validateLoadedProperty;

    // Gate 1: live clamp
    const juce::var inRange (5.0);
    const auto accepted = clampToBounds (ids::masterGain, inRange);
    CHECK (accepted.equals (inRange) && accepted.isDouble());

    const auto clampedInt = clampToBounds (ids::oscReceivePort, juce::var (99999));
    CHECK (static_cast<int> (clampedInt) == 65535 && clampedInt.isInt());

    const auto clampedLow = clampToBounds (ids::masterGain, juce::var (-100.0));
    CHECK (static_cast<double> (clampedLow) == -60.0);

    const auto fromNaN = clampToBounds (ids::masterGain,
                                        juce::var (std::numeric_limits<double>::quiet_NaN()));
    CHECK (static_cast<double> (fromNaN) == xoa::defaults::masterGainDefault);

    CHECK (clampToBounds (ids::showName, juce::var ("hello")).equals (juce::var ("hello")));

    // Gate 2: load-merge validator (string vars, parse-then-check)
    const auto validString = validateLoadedProperty (ids::inputGain, juce::var ("-6.5"));
    CHECK (validString.has_value() && validString->equals (juce::var ("-6.5")));   // unchanged

    CHECK (! validateLoadedProperty (ids::inputGain, juce::var ("garbage")).has_value());
    CHECK (! validateLoadedProperty (ids::inputGain, juce::var ("nan")).has_value());
    CHECK (! validateLoadedProperty (ids::inputGain, juce::var ("")).has_value());
    CHECK (! validateLoadedProperty (ids::inputGain, juce::var ("6.5abc")).has_value());   // trailing garbage
    CHECK (! validateLoadedProperty (ids::inputGain, juce::var ("inf")).has_value());

    const auto clampedLoad = validateLoadedProperty (ids::inputGain, juce::var ("999"));
    CHECK (clampedLoad.has_value() && static_cast<double> (*clampedLoad) == 12.0);

    const auto unbounded = validateLoadedProperty (ids::showName, juce::var ("anything"));
    CHECK (unbounded.has_value() && unbounded->equals (juce::var ("anything")));
}

//==============================================================================
// T3 — coordinate conversions (Ambisonics conventions)
//==============================================================================
static void testCoordinates()
{
    using namespace xoa::coords;

    // Cardinal directions: az from +X, CCW-positive (+Y = left), el from Z-up
    {
        const auto front = cartesianToSpherical ({ 1.0, 0.0, 0.0 });
        CHECK (approx (front.radius, 1.0) && approx (front.azimuthDeg, 0.0)
               && approx (front.elevationDeg, 0.0));

        const auto left = cartesianToSpherical ({ 0.0, 1.0, 0.0 });
        CHECK (approx (left.azimuthDeg, 90.0));

        const auto back = cartesianToSpherical ({ -1.0, 0.0, 0.0 });
        CHECK (approx (back.azimuthDeg, 180.0));

        const auto right = cartesianToSpherical ({ 0.0, -1.0, 0.0 });
        CHECK (approx (right.azimuthDeg, -90.0));

        const auto zenith = cartesianToSpherical ({ 0.0, 0.0, 1.0 });
        CHECK (approx (zenith.elevationDeg, 90.0) && approx (zenith.azimuthDeg, 0.0));

        const auto nadir = cartesianToSpherical ({ 0.0, 0.0, -3.0 });
        CHECK (approx (nadir.elevationDeg, -90.0));
    }

    // sphericalToCartesian clamps out-of-range elevation to [-90, 90]
    {
        const auto over = sphericalToCartesian ({ 1.0, 0.0, 120.0 });
        const auto at90 = sphericalToCartesian ({ 1.0, 0.0, 90.0 });
        CHECK (approx (over.x, at90.x) && approx (over.z, at90.z));
    }

    // Azimuth normalization edges
    CHECK (approx (normalizeAzimuthDegrees (180.0), 180.0));
    CHECK (approx (normalizeAzimuthDegrees (-180.0), 180.0));
    CHECK (approx (normalizeAzimuthDegrees (540.0), 180.0));
    CHECK (approx (normalizeAzimuthDegrees (-540.0), 180.0));
    CHECK (approx (normalizeAzimuthDegrees (270.0), -90.0));
    CHECK (approx (normalizeAzimuthDegrees (0.0), 0.0));

    // Round trips
    const Cartesian points[] = { { 1.0, 2.0, 3.0 }, { -4.0, 0.5, -2.0 },
                                 { 0.1, -0.2, 0.3 }, { 5.0, 0.0, 0.0 }, { 0.0, 0.0, -3.0 } };
    for (const auto& p : points)
    {
        const auto viaSpherical = sphericalToCartesian (cartesianToSpherical (p));
        CHECK (approx (viaSpherical.x, p.x) && approx (viaSpherical.y, p.y)
               && approx (viaSpherical.z, p.z));

        const auto viaCylindrical = cylindricalToCartesian (cartesianToCylindrical (p));
        CHECK (approx (viaCylindrical.x, p.x) && approx (viaCylindrical.y, p.y)
               && approx (viaCylindrical.z, p.z));
    }

    // r == 0: direction undefined -> az = el = 0
    const auto origin = cartesianToSpherical ({ 0.0, 0.0, 0.0 });
    CHECK (origin.radius == 0.0 && origin.azimuthDeg == 0.0 && origin.elevationDeg == 0.0);
}

//==============================================================================
// T4 — fresh-store default schema
//==============================================================================
static void testDefaultSchema()
{
    XoaValueTreeState s;

    CHECK (s.getState().getType() == ids::root);
    CHECK (static_cast<int> (s.getState().getProperty (ids::schemaVersion))
           == xoa::defaults::kSchemaVersion);

    CHECK (s.getConfigSection().isValid());
    CHECK (s.getInputsSection().isValid());
    CHECK (s.getSpeakersSection().isValid());
    CHECK (s.getDecoderSection().isValid());
    CHECK (s.getMonitoringSection().isValid());

    CHECK (s.getNumInputs() == xoa::kDefaultInputs);
    CHECK (s.getNumSpeakers() == xoa::kDefaultSpeakers);
    CHECK (static_cast<int> (s.getInputTree (0).getProperty (ids::idProp)) == 1);
    CHECK (static_cast<int> (s.getSpeakerTree (xoa::kDefaultSpeakers - 1)
                                 .getProperty (ids::idProp)) == xoa::kDefaultSpeakers);

    // The M1 ring: speaker 0 at (2, 0, 0); speaker 6 (az 90) at (0, 2, 0)
    const auto pos0 = s.getSpeakerTree (0).getChildWithName (ids::position);
    CHECK (approx (static_cast<double> (pos0.getProperty (ids::speakerPositionX)), 2.0));
    CHECK (approx (static_cast<double> (pos0.getProperty (ids::speakerPositionY)), 0.0));

    const auto pos6 = s.getSpeakerTree (6).getChildWithName (ids::position);
    CHECK (approx (static_cast<double> (pos6.getProperty (ids::speakerPositionX)), 0.0));
    CHECK (approx (static_cast<double> (pos6.getProperty (ids::speakerPositionY)), 2.0));

    // 6 EQ bands per speaker, all OFF, per-band default frequencies
    CHECK (s.getSpeakerEqBand (0, xoa::kNumEqBands - 1).isValid());
    CHECK (! s.getSpeakerEqBand (0, xoa::kNumEqBands).isValid());
    CHECK (static_cast<int> (s.getEqBandParameter (0, 0, ids::eqShape)) == 0);
    CHECK (static_cast<double> (s.getEqBandParameter (0, 2, ids::eqFrequency))
           == xoa::defaults::kEqBandDefaultHz[2]);

    CHECK (s.getStringParameter (ids::inputName, 4) == "Input 5");
    CHECK (s.getStringParameter (ids::showName) == xoa::defaults::showNameDefault);
}

//==============================================================================
// T4b — WP6 Config additions: scene rotation + playback parameters
//==============================================================================
static void testWp6ConfigParameters()
{
    XoaValueTreeState s;

    // Defaults present in a fresh project
    CHECK (s.getFloatParameter (ids::rotationYaw) == 0.0f);
    CHECK (s.getFloatParameter (ids::rotationPitch) == 0.0f);
    CHECK (s.getFloatParameter (ids::rotationRoll) == 0.0f);
    CHECK (s.getStringParameter (ids::playbackFilePath).isEmpty());
    CHECK (! static_cast<bool> (s.getParameter (ids::playbackLoop)));
    CHECK (s.getIntParameter (ids::playbackContentOrder) == 0);   // auto
    CHECK (s.getIntParameter (ids::playbackConvention) == 0);     // SN3D

    // Gate-1 live clamps from the bounds table
    s.setParameter (ids::rotationYaw, 500.0);
    CHECK (s.getFloatParameter (ids::rotationYaw) == 180.0f);
    s.setParameter (ids::rotationPitch, -123.0);
    CHECK (s.getFloatParameter (ids::rotationPitch) == -90.0f);
    s.setParameter (ids::playbackContentOrder, 99.0);
    CHECK (s.getIntParameter (ids::playbackContentOrder) == xoa::kAmbisonicOrder);

    // In-range writes land verbatim; strings/bools stay unbounded
    s.setParameter (ids::rotationRoll, -45.0);
    CHECK (s.getFloatParameter (ids::rotationRoll) == -45.0f);
    s.setParameter (ids::playbackFilePath, "d:/scenes/dome.wav");
    CHECK (s.getStringParameter (ids::playbackFilePath) == "d:/scenes/dome.wav");
    s.setParameter (ids::playbackLoop, true);
    CHECK (static_cast<bool> (s.getParameter (ids::playbackLoop)));
}

//==============================================================================
// T4c — WP6 Config additions: persistence + Gate-2 load validation + backfill
// (the paths T4b's in-memory clamps do not reach)
//==============================================================================
static void testWp6ConfigPersistence()
{
    ScopedTempDir tmp;

    // Non-default, in-range values load verbatim; an out-of-range angle is
    // clamped by Gate-2 (validateLoadedProperty), not rejected to default.
    const auto cfgFile = tmp.dir.getChildFile ("wp6_config.xml");
    cfgFile.replaceWithText (
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<XOAConfig schemaVersion=\"1\">\n"
        "  <Config rotationYaw=\"45.0\" rotationPitch=\"9999.0\" rotationRoll=\"-30.0\""
        " playbackFilePath=\"d:/scenes/dome.wav\" playbackLoop=\"1\""
        " playbackContentOrder=\"3\" playbackConvention=\"2\"/>\n"
        "</XOAConfig>\n");

    XoaValueTreeState s;
    XoaFileManager fm (s);
    CHECK (fm.importConfig (cfgFile));

    CHECK (s.getFloatParameter (ids::rotationYaw) == 45.0f);        // in-range verbatim
    CHECK (s.getFloatParameter (ids::rotationPitch) == 90.0f);      // 9999 -> clamped to +90
    CHECK (s.getFloatParameter (ids::rotationRoll) == -30.0f);
    CHECK (s.getStringParameter (ids::playbackFilePath) == "d:/scenes/dome.wav");
    CHECK (static_cast<bool> (s.getParameter (ids::playbackLoop)));
    CHECK (s.getIntParameter (ids::playbackContentOrder) == 3);
    CHECK (s.getIntParameter (ids::playbackConvention) == 2);

    // A legacy config that predates these params must not lose them: the merge
    // leaves each at its default rather than dropping it from the tree.
    const auto legacyFile = tmp.dir.getChildFile ("legacy_config.xml");
    legacyFile.replaceWithText (
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<XOAConfig schemaVersion=\"1\">\n"
        "  <Config showName=\"Legacy\"/>\n"
        "</XOAConfig>\n");

    XoaValueTreeState s2;
    XoaFileManager fm2 (s2);
    CHECK (fm2.importConfig (legacyFile));

    CHECK (s2.getStringParameter (ids::showName) == "Legacy");      // file value applied
    CHECK (s2.getConfigSection().hasProperty (ids::rotationYaw));   // param survived the merge
    CHECK (s2.getFloatParameter (ids::rotationYaw) == 0.0f);        // at its default
    CHECK (s2.getStringParameter (ids::playbackFilePath).isEmpty());
    CHECK (s2.getIntParameter (ids::playbackContentOrder) == 0);    // auto
    CHECK (s2.getIntParameter (ids::playbackConvention) == 0);      // SN3D
}

//==============================================================================
// T5 — typed get/set, addressing, live clamp, EQ helpers
//==============================================================================
static void testGetSet()
{
    XoaValueTreeState s;

    s.setParameter (ids::inputGain, -6.0, 2);
    CHECK (s.getFloatParameter (ids::inputGain, 2) == -6.0f);
    CHECK (s.getFloatParameter (ids::inputGain, 3) == 0.0f);   // neighbor untouched

    s.setParameter (ids::decoderCrossoverFrequency, 500.0);
    CHECK (s.getFloatParameter (ids::decoderCrossoverFrequency) == 500.0f);

    s.setParameter (ids::showName, "My Show");
    CHECK (s.getStringParameter (ids::showName) == "My Show");

    // Unknown parameter: getter returns the type default, setter no-ops
    const juce::Identifier bogus ("inputNonexistentParam");
    CHECK (s.getFloatParameter (bogus, 0) == 0.0f);
    s.setParameter (bogus, 1.0, 0);   // must not crash or land anywhere
    CHECK (! s.getInputTree (0).getChildWithName (ids::channel).hasProperty (bogus));

    // Out-of-range channel: silent no-op
    s.setParameter (ids::inputGain, -6.0, 99);
    CHECK (s.getFloatParameter (ids::inputGain, 99) == 0.0f);

    // Live clamp through the write interceptor
    s.setParameter (ids::inputGain, 100.0, 0);
    CHECK (s.getFloatParameter (ids::inputGain, 0) == 12.0f);

    // EQ band helpers (two indices, outside the (id, channel) seam)
    s.setEqBandParameter (5, 3, ids::eqGain, -4.5);
    CHECK (static_cast<double> (s.getEqBandParameter (5, 3, ids::eqGain)) == -4.5);
    CHECK (static_cast<double> (s.getEqBandParameter (5, 2, ids::eqGain)) == 0.0);
    s.setEqBandParameter (99, 0, ids::eqGain, -1.0);   // bad speaker: no-op
    CHECK (s.getEqBandParameter (99, 0, ids::eqGain).isVoid());
}

//==============================================================================
// T6 — undo domains, structural undo, MCP suppression
//==============================================================================
static void testUndoDomains()
{
    XoaValueTreeState s;

    // Edits on two domains stay in separate histories
    {
        XoaValueTreeState::ScopedDomain domain (s, XoaValueTreeState::inputsDomain);
        s.beginUndoTransaction ("edit input gain");
        s.setParameter (ids::inputGain, -6.0, 0);
    }
    {
        XoaValueTreeState::ScopedDomain domain (s, XoaValueTreeState::speakersDomain);
        s.beginUndoTransaction ("edit speaker trim");
        s.setParameter (ids::speakerGain, -3.0, 0);
    }

    s.setActiveDomain (XoaValueTreeState::inputsDomain);
    CHECK (s.canUndo());
    CHECK (s.undo());
    CHECK (s.getFloatParameter (ids::inputGain, 0) == 0.0f);      // reverted
    CHECK (s.getFloatParameter (ids::speakerGain, 0) == -3.0f);   // other domain untouched

    // Structural undo restores removed channels WITH their edited values
    {
        XoaValueTreeState::ScopedDomain domain (s, XoaValueTreeState::inputsDomain);
        s.beginUndoTransaction ("edit channel 8");
        s.setParameter (ids::inputGain, -12.0, 7);
    }
    s.setNumInputs (4);
    CHECK (s.getNumInputs() == 4);
    s.setActiveDomain (XoaValueTreeState::inputsDomain);
    CHECK (s.undo());   // undoes "Set input count"
    CHECK (s.getNumInputs() == 8);
    CHECK (s.getFloatParameter (ids::inputGain, 7) == -12.0f);

    // MCP-origin writes bypass the JUCE undo stack
    s.clearAllUndoHistories();
    {
        spatcore::control::osc::OriginTagScope mcp (spatcore::control::osc::OriginTag::MCP);
        s.setParameter (ids::inputGain, -9.0, 1);
    }
    CHECK (s.getFloatParameter (ids::inputGain, 1) == -9.0f);     // applied...
    s.setActiveDomain (XoaValueTreeState::inputsDomain);
    CHECK (! s.canUndo());                                        // ...but not undoable

    // A structural write under MCP origin routes through setNumInputs ->
    // beginUndoTransaction while getActiveUndoManager() is nullptr: the base
    // guard must skip beginNewTransaction rather than deref null.
    s.clearAllUndoHistories();
    {
        spatcore::control::osc::OriginTagScope mcp (spatcore::control::osc::OriginTag::MCP);
        s.setParameter (ids::inputCount, 5);   // must not crash
    }
    CHECK (s.getNumInputs() == 5);
    s.setActiveDomain (XoaValueTreeState::inputsDomain);
    CHECK (! s.canUndo());
}

//==============================================================================
// T7 — parameter-listener registry
//==============================================================================
static void testListenerRegistry()
{
    XoaValueTreeState s;

    int channel2Hits = 0;
    juce::var channel2Value;
    s.addParameterListener (ids::inputGain,
                            [&] (const juce::var& v) { ++channel2Hits; channel2Value = v; }, 2);

    int allChannelHits = 0;
    s.addParameterListener (ids::inputGain, [&] (const juce::var&) { ++allChannelHits; }, -1);

    s.setParameter (ids::inputGain, -1.0, 2);
    s.setParameter (ids::inputGain, -2.0, 3);
    s.setParameter (ids::speakerGain, -4.0, 2);   // different id: neither fires

    CHECK (channel2Hits == 1);
    CHECK (static_cast<double> (channel2Value) == -1.0);
    CHECK (allChannelHits == 2);

    s.removeParameterListeners (ids::inputGain, 2);
    s.setParameter (ids::inputGain, -5.0, 2);
    CHECK (channel2Hits == 1);        // removed
    CHECK (allChannelHits == 3);      // -1 registration unaffected
}

//==============================================================================
// T8 — save/load round trip, bit-stable (exit criterion a)
//==============================================================================
static void testRoundTripBitStable()
{
    ScopedTempDir tmp;
    const auto folderA = tmp.dir.getChildFile ("ProjA");
    const auto folderB = tmp.dir.getChildFile ("ProjB");

    XoaValueTreeState s1;
    XoaFileManager fm1 (s1);
    CHECK (fm1.createProject (folderA, "Bit Show"));

    // Awkward doubles that expose any float promotion or lossy text round trip
    const double awkward[] = { 0.1, 1.0 / 3.0, -0.05 };
    s1.setParameter (ids::inputGain, awkward[0], 0);
    s1.setParameter (ids::inputSpread, awkward[1], 1);
    s1.setParameter (ids::masterGain, awkward[2]);
    s1.setParameter (ids::speakerPositionX, awkward[1], 2);
    CHECK (fm1.saveProject());

    XoaValueTreeState s2;
    XoaFileManager fm2 (s2);
    CHECK (fm2.loadProject (folderA));

    CHECK (bitEqualDouble (static_cast<double> (s2.getParameter (ids::inputGain, 0)), awkward[0]));
    CHECK (bitEqualDouble (static_cast<double> (s2.getParameter (ids::inputSpread, 1)), awkward[1]));
    CHECK (bitEqualDouble (static_cast<double> (s2.getParameter (ids::masterGain)), awkward[2]));
    CHECK (bitEqualDouble (static_cast<double> (s2.getParameter (ids::speakerPositionX, 2)), awkward[1]));

    // Save the loaded state to a second folder: payload must be byte-identical
    // (headers stripped — the Created timestamp line legitimately varies)
    CHECK (fm2.createProject (folderB, juce::String()));
    for (const char* name : { "config.xml", "inputs.xml", "speakers.xml", "decoder.xml" })
    {
        const auto textA = stripXmlHeader (folderA.getChildFile (name).loadFileAsString());
        const auto textB = stripXmlHeader (folderB.getChildFile (name).loadFileAsString());
        CHECK (textA.isNotEmpty());
        CHECK (textA == textB);
    }
}

//==============================================================================
// T9 — merge-backfill (exit criterion b)
//==============================================================================
static void testMergeBackfill()
{
    ScopedTempDir tmp;

    // Reduced-schema file: 4 channels, missing properties and a whole
    // subsection, one out-of-range value, one NaN
    const auto reducedFile = tmp.dir.getChildFile ("reduced_inputs.xml");
    reducedFile.replaceWithText (
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<XOAInputs schemaVersion=\"1\">\n"
        "  <Inputs inputCount=\"4\">\n"
        "    <Input id=\"1\"><Channel inputGain=\"-20.0\"/></Input>\n"
        "    <Input id=\"2\"><Channel inputGain=\"999.0\"/></Input>\n"
        "    <Input id=\"3\"><Channel inputGain=\"nan\"/></Input>\n"
        "    <Input id=\"4\"/>\n"
        "  </Inputs>\n"
        "</XOAInputs>\n");

    XoaValueTreeState s;
    XoaFileManager fm (s);
    CHECK (fm.importInputs (reducedFile));

    CHECK (s.getNumInputs() == 4);                                     // count reconciled
    CHECK (s.getFloatParameter (ids::inputGain, 0) == -20.0f);         // file value applied
    CHECK (s.getFloatParameter (ids::inputGain, 1) == 12.0f);          // out-of-range clamped
    CHECK (s.getFloatParameter (ids::inputGain, 2) == 0.0f);           // NaN rejected -> default
    CHECK (s.getStringParameter (ids::inputName, 0) == "Input 1");     // missing prop backfilled

    // Whole missing subsection backfilled from defaults
    CHECK (s.getInputTree (3).getChildWithName (ids::encoder).hasProperty (ids::inputSpread));
    CHECK (s.getFloatParameter (ids::inputSpread, 3) == 0.0f);

    // Larger-count file: channels beyond the live count get FULL default
    // schema (the count pre-pass at work), file values land by id match
    const auto largerFile = tmp.dir.getChildFile ("larger_inputs.xml");
    largerFile.replaceWithText (
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<XOAInputs schemaVersion=\"1\">\n"
        "  <Inputs inputCount=\"16\">\n"
        "    <Input id=\"9\"><Channel inputGain=\"-5.0\"/></Input>\n"
        "  </Inputs>\n"
        "</XOAInputs>\n");

    CHECK (fm.importInputs (largerFile));
    CHECK (s.getNumInputs() == 16);
    CHECK (s.getFloatParameter (ids::inputGain, 8) == -5.0f);          // id 9 -> index 8
    CHECK (s.getInputTree (12).getChildWithName (ids::encoder).hasProperty (ids::inputSpread));
    CHECK (s.getStringParameter (ids::inputName, 12) == "Input 13");

    // File OMITS the count attribute but lists N children: the pre-pass must
    // size from the actual child count, so every child gets a full-default
    // target (not a raw sparse append)
    {
        XoaValueTreeState s2;
        XoaFileManager fm2 (s2);
        const auto noCountFile = tmp.dir.getChildFile ("nocount_inputs.xml");
        noCountFile.replaceWithText (
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
            "<XOAInputs schemaVersion=\"1\">\n"
            "  <Inputs>\n"
            "    <Input id=\"1\"><Channel inputGain=\"-1.0\"/></Input>\n"
            "    <Input id=\"2\"><Channel inputGain=\"-2.0\"/></Input>\n"
            "    <Input id=\"3\"><Channel inputGain=\"-3.0\"/></Input>\n"
            "  </Inputs>\n"
            "</XOAInputs>\n");
        CHECK (fm2.importInputs (noCountFile));
        CHECK (s2.getNumInputs() == 3);
        CHECK (s2.getFloatParameter (ids::inputGain, 2) == -3.0f);
        CHECK (s2.getStringParameter (ids::inputName, 2) == "Input 3");        // backfilled
        CHECK (s2.getInputTree (2).getChildWithName (ids::encoder).isValid()); // full schema
    }

    // Non-contiguous ids: the reconcile pass renumbers to ordinal+1 and keeps
    // the count property consistent with the child list
    {
        XoaValueTreeState s2;
        XoaFileManager fm2 (s2);
        const auto sparseFile = tmp.dir.getChildFile ("sparse_speakers.xml");
        sparseFile.replaceWithText (
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
            "<XOASpeakers schemaVersion=\"1\">\n"
            "  <Speakers speakerCount=\"2\">\n"
            "    <Speaker id=\"1\"><Channel speakerName=\"A\"/></Speaker>\n"
            "    <Speaker id=\"2\"><Channel speakerName=\"B\"/></Speaker>\n"
            "    <Speaker id=\"99\"><Channel speakerName=\"Stray\"/></Speaker>\n"
            "  </Speakers>\n"
            "</XOASpeakers>\n");
        CHECK (fm2.importSpeakers (sparseFile));
        // A stray id beyond the declared count is inherently ambiguous; the
        // reconcile does not promise an exact channel count for such input,
        // only the section INVARIANTS: contiguous ids (id == ordinal+1) and a
        // count property consistent with the child list, within the ceiling.
        const int n = s2.getNumSpeakers();
        CHECK (n >= 3 && n <= xoa::kMaxSpeakers);
        CHECK (static_cast<int> (s2.getSpeakersSection().getProperty (ids::speakerCount)) == n);
        for (int i = 0; i < n; ++i)
            CHECK (static_cast<int> (s2.getSpeakerTree (i).getProperty (ids::idProp)) == i + 1);
        // The two explicitly-valued speakers survived the merge
        CHECK (s2.getStringParameter (ids::speakerName, 0) == "A");
        CHECK (s2.getStringParameter (ids::speakerName, 1) == "B");
    }

    // More children than the ceiling: clamped to kMaxSpeakers
    {
        XoaValueTreeState s2;
        XoaFileManager fm2 (s2);
        juce::String big ("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<XOASpeakers schemaVersion=\"1\">\n<Speakers>\n");
        for (int i = 1; i <= xoa::kMaxSpeakers + 1; ++i)
            big << "<Speaker id=\"" << i << "\"><Channel speakerName=\"S" << i << "\"/></Speaker>\n";
        big << "</Speakers>\n</XOASpeakers>\n";
        const auto overFile = tmp.dir.getChildFile ("over_speakers.xml");
        overFile.replaceWithText (big);
        CHECK (fm2.importSpeakers (overFile));
        CHECK (s2.getNumSpeakers() == xoa::kMaxSpeakers);
        CHECK (static_cast<int> (s2.getSpeakersSection().getProperty (ids::speakerCount))
               == xoa::kMaxSpeakers);
    }

    // Newer schema version is refused
    const auto newerFile = tmp.dir.getChildFile ("newer_inputs.xml");
    newerFile.replaceWithText (
        "<?xml version=\"1.0\"?><XOAInputs schemaVersion=\"99\"><Inputs inputCount=\"2\"/></XOAInputs>");
    CHECK (! fm.importInputs (newerFile));
    CHECK (fm.getLastError().contains ("newer"));
    CHECK (s.getNumInputs() == 16);   // untouched
}

//==============================================================================
// T10 — backups and manifest
//==============================================================================
static void testBackupsAndManifest()
{
    ScopedTempDir tmp;
    const auto folder = tmp.dir.getChildFile ("Proj");

    XoaValueTreeState s;
    XoaFileManager fm (s);
    CHECK (fm.createProject (folder, "Backup Show"));

    s.setParameter (ids::masterGain, -1.0);
    CHECK (fm.saveConfig());   // backs up the createProject-era file
    s.setParameter (ids::masterGain, -2.0);
    CHECK (fm.saveConfig());   // backs up the -1.0 file

    const auto backups = XmlPersistence::listBackups (fm.backupFolder(), "config");
    CHECK (backups.size() >= 1);   // saves within one second may collide on name

    // Newest backup = the state before the last save
    CHECK (fm.loadConfigBackup (0));
    CHECK (static_cast<double> (s.getParameter (ids::masterGain)) == -1.0);

    // Manifest round trip
    const auto manifest = juce::XmlDocument::parse (fm.manifestFile());
    CHECK (manifest != nullptr);
    if (manifest != nullptr)
    {
        CHECK (manifest->getTagName() == "XOAProject");
        CHECK (manifest->getStringAttribute ("projectName") == "Proj");
        CHECK (manifest->getIntAttribute ("schemaVersion") == xoa::defaults::kSchemaVersion);
    }

    // loadProject accepts the manifest file as the entry point
    XoaValueTreeState s3;
    XoaFileManager fm3 (s3);
    CHECK (fm3.loadProject (fm.manifestFile()));
    CHECK (s3.getStringParameter (ids::showName) == "Backup Show");
}

//==============================================================================
// T11 — WFS-DIY speaker-layout import (exit criterion d)
//==============================================================================
static void testWfsImport()
{
    const auto fixture = juce::File (XOA_TESTS_DATA_DIR).getChildFile ("wfs_outputs_fixture.xml");
    CHECK (fixture.existsAsFile());

    XoaValueTreeState s;
    XoaFileManager fm (s);

    const auto result = fm.importWfsSpeakerLayout (fixture);   // default rotate90 remap
    CHECK (result.ok);
    CHECK (result.speakersImported == 4);
    CHECK (s.getNumSpeakers() == 4);

    // Name/gain/delay landed
    CHECK (s.getStringParameter (ids::speakerName, 0) == "Front Left");
    CHECK (static_cast<double> (s.getParameter (ids::speakerGain, 0)) == -3.0);
    CHECK (static_cast<double> (s.getParameter (ids::speakerDelay, 0)) == 1.5);

    // Positions remapped: rotate90 maps WFS (x,y,z) -> XOA (y,-x,z).
    // Front Left WFS (-2,3,0.5) -> XOA (3,2,0.5).
    CHECK (static_cast<double> (s.getParameter (ids::speakerPositionX, 0)) == 3.0);
    CHECK (static_cast<double> (s.getParameter (ids::speakerPositionY, 0)) == 2.0);
    CHECK (static_cast<double> (s.getParameter (ids::speakerPositionZ, 0)) == 0.5);
    // Sub WFS (0,-3.5,-0.25) -> XOA (-3.5,0,-0.25)  (z preserved)
    CHECK (static_cast<double> (s.getParameter (ids::speakerPositionX, 3)) == -3.5);
    CHECK (static_cast<double> (s.getParameter (ids::speakerPositionY, 3)) == 0.0);
    CHECK (static_cast<double> (s.getParameter (ids::speakerPositionZ, 3)) == -0.25);

    // Out-of-range attenuation (999 dB on Ceiling) clamped by the live gate
    CHECK (s.getFloatParameter (ids::speakerGain, 2) == 12.0f);

    // Coordinate mode is a 1:1 display preference (Ceiling carries mode 1) and
    // is NOT a warning — WFS positions are always cartesian.
    CHECK (static_cast<int> (s.getParameter (ids::speakerCoordinateMode, 2)) == 1);
    CHECK (result.warnings.isEmpty());

    // EQ imported name-for-name: Front Left band shapes {0,2,3,3,5,0}, and the
    // WFS shelf slope 0.6999... now lands in range (post eqSlope bounds fix).
    CHECK (static_cast<int> (s.getEqBandParameter (0, 1, ids::eqShape)) == 2);   // LowShelf
    CHECK (static_cast<int> (s.getEqBandParameter (0, 2, ids::eqShape)) == 3);   // Peak
    CHECK (static_cast<int> (s.getEqBandParameter (0, 4, ids::eqShape)) == 5);   // HighShelf
    CHECK (std::abs (static_cast<double> (s.getEqBandParameter (0, 0, ids::eqSlope)) - 0.699999988079071) < 1e-9);

    // WFS DSP knobs did not leak into the XOA schema
    CHECK (! s.getSpeakerTree (0).getChildWithName (ids::position)
                .hasProperty (juce::Identifier ("outputOrientation")));

    // verbatim remap: positions copied without the frame rotation
    {
        XoaValueTreeState sv;
        XoaFileManager fmv (sv);
        const auto rv = fmv.importWfsSpeakerLayout (fixture, XoaFileManager::WfsAxisRemap::verbatim);
        CHECK (rv.ok);
        CHECK (static_cast<double> (sv.getParameter (ids::speakerPositionX, 0)) == -2.0);
        CHECK (static_cast<double> (sv.getParameter (ids::speakerPositionY, 0)) == 3.0);
    }

    // Error paths are clean
    const auto missing = fm.importWfsSpeakerLayout (
        juce::File (XOA_TESTS_DATA_DIR).getChildFile ("does_not_exist.xml"));
    CHECK (! missing.ok && missing.error.isNotEmpty());

    ScopedTempDir tmp;
    const auto wrongRoot = tmp.dir.getChildFile ("wrong.xml");
    wrongRoot.replaceWithText ("<?xml version=\"1.0\"?><NotWfs/>");
    const auto rejected = fm.importWfsSpeakerLayout (wrongRoot);
    CHECK (! rejected.ok && rejected.error.contains ("OutputConfig"));

    // OutputConfig with no <Outputs> element
    const auto noOutputs = tmp.dir.getChildFile ("no_outputs.xml");
    noOutputs.replaceWithText ("<?xml version=\"1.0\"?><OutputConfig version=\"1.0\"/>");
    const auto noOut = fm.importWfsSpeakerLayout (noOutputs);
    CHECK (! noOut.ok && noOut.error.contains ("Outputs"));

    // <Outputs> present but empty
    const auto emptyOutputs = tmp.dir.getChildFile ("empty_outputs.xml");
    emptyOutputs.replaceWithText (
        "<?xml version=\"1.0\"?><OutputConfig version=\"1.0\"><Outputs count=\"0\"/></OutputConfig>");
    const auto emptyOut = fm.importWfsSpeakerLayout (emptyOutputs);
    CHECK (! emptyOut.ok && emptyOut.error.isNotEmpty());
}

//==============================================================================
// T12 — section-file load error branches (wrong root, missing section,
// missing file) for a first-class XOA section
//==============================================================================
static void testSectionLoadErrors()
{
    ScopedTempDir tmp;

    XoaValueTreeState s;
    XoaFileManager fm (s);

    // Wrong root element
    const auto wrongRoot = tmp.dir.getChildFile ("wrong_config.xml");
    wrongRoot.replaceWithText ("<?xml version=\"1.0\"?><XOAInputs schemaVersion=\"1\"><Inputs/></XOAInputs>");
    CHECK (! fm.importConfig (wrongRoot));
    CHECK (fm.getLastError().contains ("root"));

    // Correct root, missing the inner <Config> section node
    const auto noSection = tmp.dir.getChildFile ("empty_config.xml");
    noSection.replaceWithText ("<?xml version=\"1.0\"?><XOAConfig schemaVersion=\"1\"/>");
    CHECK (! fm.importConfig (noSection));
    CHECK (fm.getLastError().contains ("section"));

    // Missing file
    CHECK (! fm.importConfig (tmp.dir.getChildFile ("does_not_exist.xml")));
    CHECK (fm.getLastError().isNotEmpty());

    // A well-formed decoder file still loads after those failures (state intact)
    const auto folder = tmp.dir.getChildFile ("Proj");
    XoaValueTreeState s2;
    XoaFileManager fm2 (s2);
    CHECK (fm2.createProject (folder, "OK"));
    s2.setParameter (ids::decoderCrossoverFrequency, 800.0);
    CHECK (fm2.saveDecoder());
    XoaValueTreeState s3;
    XoaFileManager fm3 (s3);
    fm3.setProjectFolder (folder);
    CHECK (fm3.loadDecoder());
    CHECK (s3.getFloatParameter (ids::decoderCrossoverFrequency) == 800.0f);
}

//==============================================================================

void runXoaParameterTests()
{
    testBoundsTableIntegrity();
    testValidationGates();
    testCoordinates();
    testDefaultSchema();
    testWp6ConfigParameters();
    testWp6ConfigPersistence();
    testGetSet();
    testUndoDomains();
    testListenerRegistry();
    testRoundTripBitStable();
    testMergeBackfill();
    testBackupsAndManifest();
    testWfsImport();
    testSectionLoadErrors();
}
