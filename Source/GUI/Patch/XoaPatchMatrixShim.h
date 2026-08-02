/*
  ==============================================================================

    XOA — tenth-order Ambisonics spatial audio processor.
    XoaPatchMatrixShim — XOA's face of the shared spatcore patch matrix
    (stage 2 of the audio-device handoff): a derived class plus the config
    factory that binds the app's ValueTree schema, colours, localisation and
    accessibility singletons into spatcore::ui::patch::PatchMatrixConfig.

    The basename is deliberately NOT PatchMatrixComponent: spatcore compiles
    its own PatchMatrixComponent.cpp, and identical object basenames in one
    build have bitten before (see the handoff §7.1 object-file trap note).

    This file is part of XOA, released under the GNU General Public License
    v3.0. See LICENSE for details.

  ==============================================================================
*/

#pragma once

#include "spatcore/ui/patch/PatchMatrixComponent.h"

#include "Audio/TestSignalGenerator.h"
#include "Parameters/XoaValueTreeState.h"

namespace xoa::ui
{

class XoaPatchMatrix : public spatcore::ui::patch::PatchMatrixComponent
{
public:
    /** isInputPatch selects the InputPatch tree (rows = FLATTENED stem
        channels, one per channel of every input's span — D43) or the
        OutputPatch tree (rows = speakers). The generator is only used by the
        output matrix's Testing mode. */
    XoaPatchMatrix (XoaValueTreeState& store,
                    bool isInputPatch,
                    xoa::TestSignalGenerator* testSignalGen = nullptr)
        : spatcore::ui::patch::PatchMatrixComponent (makeConfig (store, isInputPatch),
                                                     isInputPatch,
                                                     testSignalGen)
    {
    }

    /** The app half of the shared component: everything spatcore deliberately
        does not know. Providers are invoked at paint/layout time, never
        snapshotted, so theme/language/scale changes land on the next repaint. */
    static spatcore::ui::patch::PatchMatrixConfig makeConfig (XoaValueTreeState& store,
                                                              bool isInputPatch);

private:
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (XoaPatchMatrix)
};

} // namespace xoa::ui
