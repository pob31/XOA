#pragma once

#include "spatcore/io/TestSignalGenerator.h"

//==============================================================================
// XOA - output test-signal generator (WP7, FR-21): the shared spatcore
// implementation, aliased into namespace xoa in the shared-EQ shim style.
//
// The two XOA-isms the local fork used to carry both moved upstream or became
// opt-in (D38, spatcore PR #8):
//
//   1. SpeakerId mode (declicked pink burst stepping across every output,
//      getCurrentSpeakerIndex() for the UI) is now a shared SignalType,
//      appended last so persisted / combo-mapped ordinals held.
//   2. The DETERMINISTIC seed is opt-in upstream: the shared prepare() seeds
//      from the wall clock unless setDeterministicSeed() was called.
//      AudioEngine seeds kTestSignalSeed at construction so a render stays
//      fully reproducible - the offline tests assert bit-equality across two
//      fresh generators.
//==============================================================================

namespace xoa
{

using TestSignalGenerator = spatcore::io::TestSignalGenerator;

/** The fixed pink-noise RNG seed (WFS-DIY seeded from the clock; XOA pins it
    for reproducibility). Applied via setDeterministicSeed() at engine
    construction and in the offline tests. */
inline constexpr juce::int64 kTestSignalSeed = 0x0A0A;

} // namespace xoa
