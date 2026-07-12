/*
  ==============================================================================

    XOA — tenth-order Ambisonics spatial audio processor.
    XoaHelpCards — the XOA help-content registry: (title, body) topics resolved
    through LocalizationManager, attached to HelpCard/HelpCardButton pairs by the
    tabs. Seeded here (WP10 C3); topics grow as tabs are built.

    This file is part of XOA, released under the GNU General Public License
    v3.0. See LICENSE for details.

  ==============================================================================
*/

#pragma once

#include <juce_core/juce_core.h>

#include "../Localization/LocalizationManager.h"

namespace xoa::help
{
    /** A help topic: a short title plus the localized body text shown on the card. */
    struct Topic
    {
        juce::String title;
        juce::String body;
    };

    inline Topic rotation()    { return { "Rotation",    LOC ("help.rotation") }; }
    inline Topic masterGain()  { return { "Master gain", LOC ("help.masterGain") }; }
    inline Topic decoderType() { return { "Decoder",     LOC ("help.decoderType") }; }
}
