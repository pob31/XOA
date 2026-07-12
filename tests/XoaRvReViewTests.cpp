/*
    XoaRvReViewTests.cpp - WP10 C8.

    Gates the FR-18 rV/rE view: the analysis the panel renders is byte-for-byte the
    analysis the CSV export writes (runAnalysis's samples serialize identically to a
    fresh analysis::analyzeGrid), over a ring and a dome fixture; plus a coverage
    sanity on the ring equator and a decode-matrix JSON export round-trip.
*/

#include <juce_core/juce_core.h>

#include <cmath>

#include "XoaTestFramework.h"

#include "GUI/Analysis/RvReAnalysisCore.h"
#include "GUI/Layout/SpeakerLayoutGenerators.h"
#include "DSP/AmbiDecoderDesigner.h"
#include "DSP/AmbiRvReAnalysis.h"

namespace
{
xoa::decoder::SpeakerLayout toDecoderLayout (const std::vector<xoa::ui::layout::SpeakerPos>& pts)
{
    xoa::decoder::SpeakerLayout layout;
    layout.count = juce::jmin ((int) pts.size(), xoa::kMaxSpeakers);
    for (int i = 0; i < layout.count; ++i)
        layout.positions[i] = { pts[(size_t) i].x, pts[(size_t) i].y, pts[(size_t) i].z };
    return layout;
}
}

void runXoaRvReViewTests()
{
    namespace L = xoa::ui::layout;
    namespace dec = xoa::decoder;

    // Fixtures: a 24-speaker ring and a hemispherical dome.
    const auto ring = toDecoderLayout (L::ring (24, 2.0, 0.0, 0.0));
    const auto dome = toDecoderLayout (L::dome (3, 12, 2.0, 75.0, true));

    for (const auto* fixture : { &ring, &dome })
    {
        dec::DesignOptions opts;
        opts.type = dec::Type::sad;
        opts.weighting = dec::Weighting::maxRe;
        const auto dr = dec::design (*fixture, opts);

        // Plot ≡ export: runAnalysis's samples serialize identically to a fresh grid.
        const juce::String directCsv = xoa::analysis::toCsv (
            xoa::analysis::analyzeGrid (dr.matrix, *fixture));
        const auto result = xoa::ui::runAnalysis (dr.matrix, *fixture, 1);
        CHECK (xoa::analysis::toCsv (result.samples) == directCsv);
        CHECK (result.generation == 1);
        CHECK (result.samples.size() == 72u * 37u);
    }

    // Sanity: the analysis produces valid, finite localisation data on the ring's
    // equator (its coverage plane). This checks that the pipeline runs and yields
    // well-formed samples, not decoder quality (that is WP5/WP13's job — a 24-ring
    // is rank-deficient for a full-sphere decode, so ||rV|| is not pinned to 1).
    {
        dec::DesignOptions opts;
        opts.type = dec::Type::sad;
        opts.weighting = dec::Weighting::basic;
        opts.normalization = dec::NormalizationMode::amplitude;
        const auto dr = dec::design (ring, opts);
        const auto result = xoa::ui::runAnalysis (dr.matrix, ring, 2);

        int checked = 0;
        for (const auto& s : result.samples)
        {
            if (std::abs (s.elevationDeg) < 1.0)   // equator row (the ring plane)
            {
                CHECK (s.rvValid);
                CHECK (std::isfinite (s.rvMagnitude) && s.rvMagnitude >= 0.0);
                CHECK (std::isfinite (s.energy) && s.energy >= 0.0);
                ++checked;
            }
        }
        CHECK (checked > 0);
    }

    // Decode-matrix export round-trips (FR-18 matrix export).
    {
        dec::DesignOptions opts; opts.type = dec::Type::sad;
        const auto dr = dec::design (ring, opts);
        const juce::String json = xoa::analysis::decoderMatrixToJsonString (dr.matrix);
        dec::DecoderMatrix back;
        CHECK (xoa::analysis::decoderMatrixFromJson (json, back));
        CHECK (back.numSpeakers == dr.matrix.numSpeakers);
        CHECK (back.order == dr.matrix.order);
        CHECK (back.d.size() == dr.matrix.d.size());
        // The export recovers the matrix to high precision (juce::JSON's double
        // parse is not guaranteed bit-exact, so this is a tolerance, not equality).
        double maxErr = 0.0;
        for (size_t i = 0; i < dr.matrix.d.size(); ++i)
            maxErr = juce::jmax (maxErr, std::abs (back.d[i] - dr.matrix.d[i]));
        CHECK (maxErr < 1.0e-9);
    }
}
