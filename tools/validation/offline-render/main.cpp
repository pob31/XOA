/*
    XOA offline-render harness.

    Renders the deterministic scenarios in scenarios.h through the REAL RT chain
    (DecoderMatrixBuilder -> RtSnapshot -> AmbiBusAlgorithm, fed by the shared
    TestSceneGenerator), then SHA-256s the raw float32 output. A committed,
    per-machine baseline JSON turns any unintended change in the DSP into a
    failed --check. This is the bit-exact gate the WP6 chain and everything
    after it lean on.

    Baselines are per-machine (float results are compiler/CPU dependent) and are
    only meaningful under the pinned Release flags (spatcore_apply_compile_flags);
    see baselines/README.md. CI builds this target and runs the ctest smoke
    (--scenario all --blocks 40, no --check).

    CLI:
      --scenario <static3|rotate|order-adapt|scene10|shoebox-allrad|dual-band|comp|
                  comp-offcenter|encode-static|encode-move|all>
      [--blocks N] [--block 512] [--sr 48000] [--speakers 24]
      [--wav out.wav] [--raw out.f32]
      [--check baselines/<machine>.json] [--update] [--bench] [--warmup 16]

    exit codes: 0 ok, 1 baseline mismatch/missing, 2 usage, 3 silent-output
                sanity failure, 4 file write error.
*/

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_core/juce_core.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

#include "sha256.h"
#include "scenarios.h"

#include "XoaConstants.h"
#include "Audio/SpeakerCompParams.h"
#include "Audio/SpeakerCompProcessor.h"
#include "Audio/TestSceneGenerator.h"
#include "DSP/AmbiBusAlgorithm.h"
#include "DSP/AmbiDecoderDesigner.h"
#include "DSP/AmbiEncoder.h"
#include "DSP/AmbiNFCFilter.h"
#include "DSP/AmbiRtTypes.h"
#include "DSP/AmbiSphericalHarmonics.h"
#include "DSP/DecoderMatrixBuilder.h"
#include "Helpers/XoaCoordinates.h"

#include "spatcore/rt/RtSnapshot.h"

namespace
{

namespace dec = xoa::decoder;
namespace coords = xoa::coords;

using ChannelData = std::vector<std::vector<float>>;

struct Config
{
    double sr = 48000.0;
    int block = 512;
    int blocks = 300;
    int speakers = 24;
    bool bench = false;
    int warmup = 16;
};

//==============================================================================
dec::SpeakerLayout makeRing (int count, double radius)
{
    dec::SpeakerLayout layout;
    layout.count = juce::jmin (count, xoa::kMaxSpeakers);
    for (int s = 0; s < layout.count; ++s)
    {
        const double az = 360.0 * (double) s / (double) layout.count;
        layout.positions[s] = coords::sphericalToCartesian (
            { radius, coords::normalizeAzimuthDegrees (az), 0.0 });
    }
    return layout;
}

// A schematic irregular "shoebox" room rig (~31 speakers): an ear-level wall
// ring, an offset upper ring, a ceiling ring, a low ring ABOVE the floor, and a
// zenith - but NO floor, so the nadir gap forces AllRAD to insert an imaginary
// speaker. Radii vary 3-5 m (the "shoebox"); each speaker carries a small
// deterministic integer-hash jitter so the convex hull is unique. Constants are
// the source of truth for C3b's Python shoebox30() mirror.
dec::SpeakerLayout makeShoebox()
{
    struct Ring { int n; double elDeg; double baseAzDeg; double radius; };
    static const Ring rings[] = {
        { 8,   0.0,  0.0, 4.0 },   // ear-level wall ring
        { 8,  32.0, 22.5, 4.4 },   // upper wall ring (offset azimuths)
        { 6,  60.0,  0.0, 3.6 },   // ceiling ring
        { 8, -22.0, 15.0, 4.8 },   // low ring (above the floor -> nadir gap)
    };
    auto jitter = [] (int i, double scaleDeg)
    {
        const juce::uint32 h = (juce::uint32) i * 2654435761u;   // Knuth multiplicative hash
        return ((double) (h % 10000u) / 10000.0 - 0.5) * 2.0 * scaleDeg;
    };

    dec::SpeakerLayout layout;
    int idx = 0;
    for (const auto& r : rings)
        for (int k = 0; k < r.n; ++k, ++idx)
        {
            const double az  = r.baseAzDeg + 360.0 * (double) k / (double) r.n + jitter (idx, 2.0);
            const double el  = r.elDeg  + jitter (idx + 100, 1.5);
            const double rad = r.radius + jitter (idx + 200, 0.4);
            layout.positions[idx] = coords::sphericalToCartesian (
                { rad, coords::normalizeAzimuthDegrees (az), el });
        }
    layout.positions[idx++] = coords::sphericalToCartesian ({ 3.0, 0.0, 90.0 });   // zenith
    layout.count = idx;   // 8 + 8 + 6 + 8 + 1 = 31
    return layout;
}

// Per-speaker comp POD for the shoebox: distance mode 2 (delay aligns every
// speaker to the farthest; attenuate-only gain law) plus a -6 dB trim on
// speaker 0 - mirrors composeSpeakerCompParams (which the unit tests pin).
// The optional listener offset (D18) re-references distances to (lx,ly,lz);
// at the default origin the output is bit-identical to the pre-D18 mirror, so
// the `comp` baseline is unchanged and `comp-offcenter` differs only by it.
xoa::SpeakerCompRtParams makeShoeboxComp (const dec::SpeakerLayout& layout,
                                          double lx = 0.0, double ly = 0.0, double lz = 0.0)
{
    xoa::SpeakerCompRtParams p;
    p.numSpeakers = layout.count;
    p.epoch = 1u;

    std::vector<double> radius ((size_t) layout.count, 0.0);
    double rMax = 0.0;
    for (int s = 0; s < layout.count; ++s)
    {
        const auto& c = layout.positions[s];
        const double dx = c.x - lx, dy = c.y - ly, dz = c.z - lz;
        radius[(size_t) s] = std::sqrt (dx * dx + dy * dy + dz * dz);
        rMax = std::max (rMax, radius[(size_t) s]);
    }
    for (int s = 0; s < layout.count; ++s)
    {
        p.delayMs[s] = (float) ((rMax - radius[(size_t) s]) / xoa::kSpeedOfSound * 1000.0);
        double gainDb = juce::jlimit (-24.0, 0.0, 20.0 * std::log10 (radius[(size_t) s] / rMax));
        if (s == 0) gainDb += -6.0;
        p.gainLinear[s] = (float) std::pow (10.0, gainDb / 20.0);
    }
    return p;
}

// One +6 dB peak EQ band on speaker 0, to exercise the comp biquad path.
xoa::SpeakerEqParams makeShoeboxEq (int numSpeakers)
{
    xoa::SpeakerEqParams eq;
    eq.numSpeakers = numSpeakers;
    if (numSpeakers > 0)
    {
        eq.enabled[0] = true;
        eq.bands[0][0].shape  = 3;          // peak
        eq.bands[0][0].freq   = 1000.0f;
        eq.bands[0][0].gainDb = 6.0f;
        eq.bands[0][0].q      = 2.0f;
    }
    return eq;
}

// The rig (layout + decoder options + optional comp) each scenario runs on.
struct RigSpec
{
    dec::SpeakerLayout       layout;
    dec::DesignOptions       options;
    bool                     comp = false;
    bool                     encoder = false;   // WP8 mono-encoder scenarios
    xoa::SpeakerCompRtParams compParams;
    xoa::SpeakerEqParams     eqParams;
};

RigSpec rigFor (scenario::Id id, const Config& cfg)
{
    RigSpec rig;
    switch (id)
    {
        case scenario::Id::ShoeboxAllRad:
            rig.layout = makeShoebox();
            rig.options.type = dec::Type::allRad;
            break;
        case scenario::Id::DualBand:
            rig.layout = makeRing (cfg.speakers, 2.0);
            rig.options.dualBand = true;
            rig.options.crossoverHz = 400.0;
            break;
        case scenario::Id::Comp:
            rig.layout = makeShoebox();      // default SAD/max-rE decode; comp is the focus
            rig.comp = true;
            rig.compParams = makeShoeboxComp (rig.layout);
            rig.eqParams = makeShoeboxEq (rig.layout.count);
            break;
        case scenario::Id::CompOffCenter:    // identical rig; D18 listener at (1, 0.5, 0)
            rig.layout = makeShoebox();
            rig.comp = true;
            rig.compParams = makeShoeboxComp (rig.layout, 1.0, 0.5, 0.0);
            rig.eqParams = makeShoeboxEq (rig.layout.count);
            break;
        case scenario::Id::EncodeStatic:
        case scenario::Id::EncodeMove:
            rig.layout = makeRing (cfg.speakers, 2.0);   // r_ref = 2 m ring
            rig.encoder = true;
            break;
        default:                             // static3 / rotate / order-adapt / scene10
            rig.layout = makeRing (cfg.speakers, 2.0);
            break;
    }
    return rig;
}

// Render one scenario through the real chain. On --bench, reports wall time
// (excluding the first cfg.warmup blocks).
ChannelData renderScenario (scenario::Id id, const Config& cfg)
{
    const RigSpec rig = rigFor (id, cfg);
    const int numOut = rig.layout.count;

    xoa::DecoderMatrixBuilder builder;
    builder.rebuild (rig.layout, rig.options);
    builder.publish();

    spatcore::rt::RtSnapshot<xoa::rt::RotationRtState> rotSnap;
    spatcore::rt::RtSnapshot<xoa::rt::BusRtParams>      busSnap;

    // WP8 encoder seams (encoder scenarios only): app-owned live matrices +
    // the scalar side-band, composed per tick below exactly as the message
    // thread would. r_ref is the 2 m ring radius.
    constexpr double rRef = 2.0;
    std::vector<float>  encMatrix;
    std::vector<double> nfcPages;
    spatcore::rt::RtSnapshot<xoa::rt::EncoderRtParams> encSnap;
    juce::AudioBuffer<float> stems;

    xoa::AmbiBusAlgorithm algo;
    if (rig.encoder)
    {
        encMatrix.assign ((size_t) xoa::kMaxInputs * xoa::kNumSHChannels, 0.0f);
        nfcPages.assign  ((size_t) xoa::kMaxInputs * xoa::nfc::kCoeffsPerSource, 0.0);
        stems.setSize (xoa::kMaxInputs, cfg.block, false, false, true);
        algo.prepare (xoa::kNumSHChannels, numOut, cfg.sr, cfg.block,
                      &builder, &rotSnap, &busSnap, true,
                      encMatrix.data(), nfcPages.data(), &encSnap);
    }
    else
    {
        algo.prepare (xoa::kNumSHChannels, numOut, cfg.sr, cfg.block,
                      &builder, &rotSnap, &busSnap, true);
    }

    // Optional per-speaker compensation stage (comp scenario only).
    spatcore::rt::RtSnapshot<xoa::SpeakerCompRtParams> compSnap;
    xoa::SpeakerCompProcessor speakerComp;
    if (rig.comp)
    {
        compSnap.publish (rig.compParams);
        speakerComp.prepare (cfg.sr, cfg.block, numOut, &compSnap);
        speakerComp.setEqParameters (rig.eqParams);
    }

    const int sOrder = scenario::sceneOrder (id);
    const int sceneCh = xoa::sh::numChannels (sOrder);

    juce::AudioBuffer<float> input (sceneCh, cfg.block);
    juce::AudioBuffer<float> output (numOut, cfg.block);

    ChannelData out ((size_t) numOut);
    for (auto& c : out)
        c.reserve ((size_t) cfg.blocks * (size_t) cfg.block);

    float* inPtrs[xoa::kNumSHChannels];

    using Clock = std::chrono::steady_clock;
    Clock::time_point benchStart;
    double benchMs = 0.0;

    juce::ScopedNoDenormals noDenormals;

    for (int b = 0; b < cfg.blocks; ++b)
    {
        const juce::int64 blockStart = (juce::int64) b * (juce::int64) cfg.block;
        const int tick = (int) std::floor ((double) blockStart * 50.0 / cfg.sr);

        double yaw, pitch, roll;
        scenario::rotation (id, tick, yaw, pitch, roll);
        rotSnap.publish (xoa::rt::makeRotationState (yaw, pitch, roll, (juce::uint32) (tick + 1)));

        int numStems = 0;
        if (rig.encoder)
        {
            // Silent HOA (the stems carry the signal), then compose the encode
            // rows / NFC pages for this tick exactly as the calc engine would.
            busSnap.publish (xoa::rt::makeBusParams (0, 0, 0, 0, 0.0, (juce::uint32) (tick + 1)));
            input.clear();

            numStems = scenario::stemCount (id);
            juce::uint64 mask = 0;
            for (int src = 0; src < numStems; ++src)
            {
                const auto spec = scenario::sourceSpec (id, src, tick);
                xoa::enc::SourceParams sp;
                sp.x = spec.x; sp.y = spec.y; sp.z = spec.z;
                sp.gainDb = spec.gainDb; sp.spreadDeg = spec.spreadDeg;
                xoa::enc::composeRow (sp, rRef,
                                      encMatrix.data() + (size_t) src * xoa::kNumSHChannels);
                if (spec.nfc)
                {
                    const double rSrc = std::sqrt (spec.x * spec.x + spec.y * spec.y + spec.z * spec.z);
                    xoa::nfc::designSourceSections (rSrc, rRef, cfg.sr,
                        nfcPages.data() + (size_t) src * xoa::nfc::kCoeffsPerSource);
                    mask |= (juce::uint64) 1 << src;
                }
            }
            xoa::rt::EncoderRtParams ep;
            ep.numSources = numStems; ep.nfcMask = mask;
            ep.referenceRadius = (float) rRef; ep.epoch = (juce::uint32) (tick + 1);
            encSnap.publish (ep);

            for (int src = 0; src < numStems; ++src)
            {
                float* d = stems.getWritePointer (src);
                for (int j = 0; j < cfg.block; ++j)
                    d[j] = scenario::stemSample (id, src, blockStart + j, cfg.sr);
            }
        }
        else
        {
            const int cOrder = scenario::contentOrder (id, tick);
            const int numFile = xoa::sh::numChannels (cOrder);
            busSnap.publish (xoa::rt::makeBusParams (cOrder, sOrder, 0, numFile, 0.0,
                                                     (juce::uint32) (tick + 1)));

            for (int c = 0; c < sceneCh; ++c)
                inPtrs[c] = input.getWritePointer (c);
            xoa::scene::renderScene (sOrder, blockStart, cfg.block, cfg.sr, inPtrs);
        }

        if (cfg.bench && b == cfg.warmup)
            benchStart = Clock::now();

        output.clear();
        juce::AudioSourceChannelInfo info (&output, 0, cfg.block);
        if (rig.encoder)
            algo.processBlock (info, input, 0, numOut, &stems, numStems);
        else
            algo.processBlock (info, input, sceneCh, numOut);

        if (rig.comp)
            speakerComp.processBlock (output, numOut, cfg.block);

        for (int c = 0; c < numOut; ++c)
        {
            const float* o = output.getReadPointer (c);
            out[(size_t) c].insert (out[(size_t) c].end(), o, o + cfg.block);
        }
    }

    if (cfg.bench && cfg.blocks > cfg.warmup)
    {
        benchMs = std::chrono::duration<double, std::milli> (Clock::now() - benchStart).count();
        const int benched = cfg.blocks - cfg.warmup;
        const double audioMs = (double) benched * (double) cfg.block / cfg.sr * 1000.0;
        std::printf ("  bench %-12s %d blocks  %.2f ms wall  %.1fx realtime\n",
                     scenario::name (id), benched, benchMs,
                     benchMs > 0.0 ? audioMs / benchMs : 0.0);
    }

    return out;
}

//==============================================================================
// SHA-256 of the raw float32 PCM: all channels, channel-major, little-endian
// (matches sha256sum of the --raw dump).
std::string hashChannels (const ChannelData& chans)
{
    orh::Sha256 sha;
    for (const auto& c : chans)
        sha.update (c.data(), c.size() * sizeof (float));
    return sha.finalHex();
}

bool anyNonzero (const ChannelData& chans)
{
    for (const auto& c : chans)
        for (float v : c)
            if (v != 0.0f)
                return true;
    return false;
}

bool writeRaw (const juce::File& f, const ChannelData& chans)
{
    f.deleteFile();
    juce::FileOutputStream os (f);
    if (! os.openedOk())
        return false;
    for (const auto& c : chans)
        os.write (c.data(), c.size() * sizeof (float));
    return true;
}

bool writeWav (const juce::File& f, const ChannelData& chans, double sr)
{
    if (chans.empty())
        return false;

    f.deleteFile();
    auto fileStream = std::make_unique<juce::FileOutputStream> (f);
    if (! fileStream->openedOk())
        return false;

    std::unique_ptr<juce::OutputStream> os (std::move (fileStream));
    juce::WavAudioFormat fmt;
    const auto options = juce::AudioFormatWriterOptions {}
                             .withSampleRate (sr)
                             .withNumChannels ((int) chans.size())
                             .withBitsPerSample (32)
                             .withSampleFormat (juce::AudioFormatWriterOptions::SampleFormat::floatingPoint);
    auto writer = fmt.createWriterFor (os, options);
    if (writer == nullptr)
        return false;

    std::vector<const float*> ptrs;
    for (const auto& c : chans)
        ptrs.push_back (c.data());
    return writer->writeFromFloatArrays (ptrs.data(), (int) chans.size(), (int) chans[0].size());
}

juce::File taggedFile (const juce::File& base, const std::string& tag)
{
    return base.getSiblingFile (base.getFileNameWithoutExtension()
                                + "." + juce::String (tag) + base.getFileExtension());
}

void usage()
{
    std::fprintf (stderr,
        "usage: xoa-offline-render --scenario <static3|rotate|order-adapt|scene10|shoebox-allrad|dual-band|comp|comp-offcenter|all>\n"
        "                          [--blocks N] [--block 512] [--sr 48000] [--speakers 24]\n"
        "                          [--wav out.wav] [--raw out.f32]\n"
        "                          [--check baselines/<machine>.json] [--update]\n"
        "                          [--bench] [--warmup 16]\n"
        "\n"
        "Baselines are per-machine and only meaningful in Release (see baselines/README.md):\n"
        "  xoa-offline-render --scenario all --update                 # create/refresh\n"
        "  xoa-offline-render --scenario all --check baselines/<m>.json\n"
        "\n"
        "exit codes: 0 ok, 1 baseline mismatch/missing, 2 usage,\n"
        "            3 silent-output sanity failure, 4 file write error\n");
}

} // namespace

//==============================================================================
int main (int argc, char* argv[])
{
    if (! orh::Sha256::selfTest())
    {
        std::fprintf (stderr, "FATAL: SHA-256 self-test failed\n");
        return 2;
    }
    if (juce::ByteOrder::isBigEndian())
    {
        std::fprintf (stderr, "FATAL: hashes are defined on little-endian float dumps\n");
        return 2;
    }

    Config cfg;
    std::string scenarioArg = "all", wavArg, rawArg, checkArg;
    bool update = false;

    for (int i = 1; i < argc; ++i)
    {
        const std::string a = argv[i];
        auto next = [&] () -> std::string
        {
            if (i + 1 >= argc)
            {
                std::fprintf (stderr, "error: %s needs a value\n", a.c_str());
                usage();
                std::exit (2);
            }
            return argv[++i];
        };

        if      (a == "--scenario") scenarioArg = next();
        else if (a == "--blocks")   cfg.blocks = std::atoi (next().c_str());
        else if (a == "--block")    cfg.block = std::atoi (next().c_str());
        else if (a == "--sr")       cfg.sr = std::atof (next().c_str());
        else if (a == "--speakers") cfg.speakers = std::atoi (next().c_str());
        else if (a == "--wav")      wavArg = next();
        else if (a == "--raw")      rawArg = next();
        else if (a == "--check")    checkArg = next();
        else if (a == "--update")   update = true;
        else if (a == "--bench")    cfg.bench = true;
        else if (a == "--warmup")   cfg.warmup = std::atoi (next().c_str());
        else if (a == "--help" || a == "-h") { usage(); return 0; }
        else { std::fprintf (stderr, "error: unknown argument %s\n", a.c_str()); usage(); return 2; }
    }

    if (cfg.blocks <= 0 || cfg.block <= 0 || cfg.sr <= 0.0 || cfg.speakers <= 0)
    {
        std::fprintf (stderr, "error: --blocks/--block/--sr/--speakers must be positive\n");
        return 2;
    }

    std::vector<scenario::Id> toRun;
    if (scenarioArg == "all")
    {
        toRun = scenario::allScenarios();
    }
    else
    {
        scenario::Id id;
        if (! scenario::fromName (scenarioArg, id))
        {
            std::fprintf (stderr, "error: unknown scenario '%s'\n", scenarioArg.c_str());
            usage();
            return 2;
        }
        toRun.push_back (id);
    }

    const bool multiCombo = toRun.size() > 1;

    // Render + hash every scenario.
    std::map<std::string, std::string> results;   // "cpu/<scenario>" -> hex
    for (auto id : toRun)
    {
        const auto chans = renderScenario (id, cfg);

        if (! anyNonzero (chans))
        {
            std::fprintf (stderr, "error: scenario %s produced all-silent output\n",
                          scenario::name (id));
            return 3;
        }

        const std::string key = std::string ("cpu/") + scenario::name (id);
        const std::string hex = hashChannels (chans);
        results[key] = hex;
        std::printf ("%-20s %s\n", key.c_str(), hex.c_str());

        if (! wavArg.empty())
        {
            auto f = juce::File::getCurrentWorkingDirectory().getChildFile (juce::String (wavArg));
            if (multiCombo) f = taggedFile (f, scenario::name (id));
            if (! writeWav (f, chans, cfg.sr))
            {
                std::fprintf (stderr, "error: could not write %s\n", f.getFullPathName().toRawUTF8());
                return 4;
            }
        }
        if (! rawArg.empty())
        {
            auto f = juce::File::getCurrentWorkingDirectory().getChildFile (juce::String (rawArg));
            if (multiCombo) f = taggedFile (f, scenario::name (id));
            if (! writeRaw (f, chans))
            {
                std::fprintf (stderr, "error: could not write %s\n", f.getFullPathName().toRawUTF8());
                return 4;
            }
        }
    }

    if (checkArg.empty())
        return 0;

    //==========================================================================
    // Baseline check / update.
    //==========================================================================
    auto baselineFile = juce::File::getCurrentWorkingDirectory().getChildFile (juce::String (checkArg));

    if (update)
    {
        // Merge: keep entries for combos not rendered this run.
        std::map<std::string, std::string> merged;
        if (baselineFile.existsAsFile())
        {
            const auto parsed = juce::JSON::parse (baselineFile.loadFileAsString());
            if (auto* obj = parsed.getDynamicObject())
                for (const auto& prop : obj->getProperties())
                    merged[prop.name.toString().toStdString()] = prop.value.toString().toStdString();
        }
        for (const auto& r : results)
            merged[r.first] = r.second;

        juce::String json = "{\n";
        size_t i = 0;
        for (const auto& e : merged)
        {
            json << "  \"" << juce::String (e.first) << "\": \"" << juce::String (e.second) << "\"";
            if (++i < merged.size()) json << ",";
            json << "\n";
        }
        json << "}\n";

        baselineFile.getParentDirectory().createDirectory();
        if (! baselineFile.replaceWithText (json))
        {
            std::fprintf (stderr, "error: could not write %s\n", baselineFile.getFullPathName().toRawUTF8());
            return 4;
        }
        std::printf ("wrote %s (%d entries)\n", baselineFile.getFullPathName().toRawUTF8(),
                     (int) merged.size());
        return 0;
    }

    if (! baselineFile.existsAsFile())
    {
        std::fprintf (stderr, "error: baseline %s missing - run with --update to create it\n",
                      baselineFile.getFullPathName().toRawUTF8());
        return 1;
    }

    std::map<std::string, std::string> expected;
    {
        const auto parsed = juce::JSON::parse (baselineFile.loadFileAsString());
        if (auto* obj = parsed.getDynamicObject())
            for (const auto& prop : obj->getProperties())
                expected[prop.name.toString().toStdString()] = prop.value.toString().toStdString();
    }

    std::vector<std::string> problems;
    for (const auto& r : results)
    {
        auto it = expected.find (r.first);
        if (it == expected.end())
            problems.push_back ("MISSING   " + r.first + " (not in baseline - run --update if intentional)");
        else if (it->second != r.second)
            problems.push_back ("MISMATCH  " + r.first + "\n    expected " + it->second
                                + "\n    actual   " + r.second);
    }

    if (! problems.empty())
    {
        std::printf ("xoa-offline-render baseline check FAILED:\n");
        for (const auto& p : problems)
            std::printf ("  %s\n", p.c_str());
        return 1;
    }

    std::printf ("xoa-offline-render baseline check OK (%d combos match %s)\n",
                 (int) results.size(), baselineFile.getFileName().toRawUTF8());
    return 0;
}
