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
      --scenario <static3|rotate|order-adapt|scene10|all>
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
#include "Audio/TestSceneGenerator.h"
#include "DSP/AmbiBusAlgorithm.h"
#include "DSP/AmbiDecoderDesigner.h"
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

// Render one scenario through the real chain. On --bench, reports wall time
// (excluding the first cfg.warmup blocks).
ChannelData renderScenario (scenario::Id id, const Config& cfg)
{
    const int numOut = cfg.speakers;

    xoa::DecoderMatrixBuilder builder;
    builder.rebuild (makeRing (cfg.speakers, 2.0), dec::DesignOptions {});   // SAD / max-rE
    builder.publish();

    spatcore::rt::RtSnapshot<xoa::rt::RotationRtState> rotSnap;
    spatcore::rt::RtSnapshot<xoa::rt::BusRtParams>      busSnap;

    xoa::AmbiBusAlgorithm algo;
    algo.prepare (xoa::kNumSHChannels, numOut, cfg.sr, cfg.block,
                  &builder, &rotSnap, &busSnap, true);

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

        const int cOrder = scenario::contentOrder (id, tick);
        const int numFile = xoa::sh::numChannels (cOrder);
        busSnap.publish (xoa::rt::makeBusParams (cOrder, sOrder, 0, numFile, 0.0,
                                                 (juce::uint32) (tick + 1)));

        for (int c = 0; c < sceneCh; ++c)
            inPtrs[c] = input.getWritePointer (c);
        xoa::scene::renderScene (sOrder, blockStart, cfg.block, cfg.sr, inPtrs);

        if (cfg.bench && b == cfg.warmup)
            benchStart = Clock::now();

        output.clear();
        juce::AudioSourceChannelInfo info (&output, 0, cfg.block);
        algo.processBlock (info, input, sceneCh, numOut);

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
        "usage: xoa-offline-render --scenario <static3|rotate|order-adapt|scene10|all>\n"
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
