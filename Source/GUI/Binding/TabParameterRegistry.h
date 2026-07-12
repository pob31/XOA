/*
  ==============================================================================

    XOA — tenth-order Ambisonics spatial audio processor.
    TabParameterRegistry — declares which UI surface (persistent header or one of
    the six tabs) hosts each parameter (WP10 C4). Grows one chunk at a time as
    tabs are built; at WP10 C9 the union of all surfaces covers every non-system
    descriptor, which is the "every v1 parameter reachable from the GUI" exit
    criterion, enforced by XoaUiDescriptorTests.

    Console-safe (juce_core + ids).

    This file is part of XOA, released under the GNU General Public License
    v3.0. See LICENSE for details.

  ==============================================================================
*/

#pragma once

#include <juce_core/juce_core.h>

#include <vector>

#include "Parameters/XoaParameterIDs.h"

namespace xoa::ui
{

enum class Surface
{
    header,          // persistent transport / rotation / master strip
    systemConfig,
    network,
    inputs,
    speakersDecoder,
    monitoring,
    map
};

struct RegistryRow
{
    Surface          surface;
    juce::Identifier id;
};

/** Every (surface, id) placement. Seeded with the header in C4; each later chunk
    appends its tab's rows. */
inline const std::vector<RegistryRow>& registryRows()
{
    namespace i = xoa::ids;
    static const std::vector<RegistryRow> rows = {
        // Persistent header strip (C5): transport + rotation + master.
        { Surface::header, i::rotationYaw },
        { Surface::header, i::rotationPitch },
        { Surface::header, i::rotationRoll },
        { Surface::header, i::masterGain },
        { Surface::header, i::playbackLoop },
        { Surface::header, i::playbackFilePath },

        // System Config tab (C5).
        { Surface::systemConfig, i::showName },
        { Surface::systemConfig, i::playbackContentOrder },
        { Surface::systemConfig, i::playbackConvention },
        { Surface::systemConfig, i::audioDeviceState },

        // Network tab (C5): the WP9 OSC transport schema (single send target).
        { Surface::network, i::oscEnabled },
        { Surface::network, i::oscReceivePort },
        { Surface::network, i::oscTcpEnabled },
        { Surface::network, i::oscTcpPort },
        { Surface::network, i::oscAcceptAnyHost },
        { Surface::network, i::oscSendAddress },
        { Surface::network, i::oscSendPort },
        { Surface::network, i::oscFeedbackEnabled },
        { Surface::network, i::oscMeterEnabled },

        // Inputs tab (C6): mono-encoder gate + per-input encoder.
        { Surface::inputs, i::monoInputsEnabled },
        { Surface::inputs, i::inputCount },
        { Surface::inputs, i::inputName },
        { Surface::inputs, i::inputGain },
        { Surface::inputs, i::inputMute },
        { Surface::inputs, i::inputPositionX },
        { Surface::inputs, i::inputPositionY },
        { Surface::inputs, i::inputPositionZ },
        { Surface::inputs, i::inputCoordinateMode },
        { Surface::inputs, i::inputMaxSpeed },
        { Surface::inputs, i::inputTrackingSmooth },
        { Surface::inputs, i::inputSpread },
        { Surface::inputs, i::inputNfcEnabled },
    };
    return rows;
}

/** Ids placed on a given surface. */
inline std::vector<juce::Identifier> idsForSurface (Surface s)
{
    std::vector<juce::Identifier> out;
    for (const auto& r : registryRows())
        if (r.surface == s)
            out.push_back (r.id);
    return out;
}

/** True if the id is placed on at least one surface. */
inline bool isRegistered (const juce::Identifier& id)
{
    for (const auto& r : registryRows())
        if (r.id == id)
            return true;
    return false;
}

} // namespace xoa::ui
