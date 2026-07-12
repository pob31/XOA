/*
  ==============================================================================

    XOA — tenth-order Ambisonics spatial audio processor.
    RvReAnalysisCore — the pure rV/rE analysis result (WP10 C8): a thin bundle of
    the decoder matrix, the layout, the analyzed direction grid, and a generation
    stamp. runAnalysis() is a pure wrapper over analysis::analyzeGrid, so the map
    plot renders exactly the sample vector that analysis::toCsv serializes (the
    FR-18 export ≡ plot identity, gated by XoaRvReViewTests).

    Console-safe (no GUI). This file is part of XOA, released under the GNU General
    Public License v3.0. See LICENSE for details.

  ==============================================================================
*/

#pragma once

#include <cstdint>
#include <utility>
#include <vector>

#include "DSP/AmbiRvReAnalysis.h"
#include "DSP/AmbiDecoderDesigner.h"

namespace xoa::ui
{

struct AnalysisResult
{
    std::vector<analysis::DirectionSample> samples;
    decoder::DecoderMatrix                 matrix;   // the analyzed decode matrix (for export)
    decoder::SpeakerLayout                 layout;   // speaker directions (for markers)
    std::uint64_t                          generation = 0;
};

/** Analyze a decode matrix over the display grid. Pure — the returned samples are
    exactly analysis::analyzeGrid(matrix, layout, opts). */
inline AnalysisResult runAnalysis (decoder::DecoderMatrix matrix,
                                   decoder::SpeakerLayout layout,
                                   std::uint64_t generation,
                                   analysis::GridOptions opts = {})
{
    AnalysisResult r;
    r.samples    = analysis::analyzeGrid (matrix, layout, opts);
    r.matrix     = std::move (matrix);
    r.layout     = layout;
    r.generation = generation;
    return r;
}

} // namespace xoa::ui
